/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "liburing.h"
#include "helpers.h"

#define NR_BUFS 32
#define RQ_ENTRIES 64

struct test_ctx {
	struct io_uring ring;
	struct io_uring_zcrx_rq rq;
	struct io_uring_zcrx_area_reg area;
	struct io_uring_zcrx_ifq_reg reg;
	void *rq_mem;
	void *area_mem;
	int fds[2];
	bool ring_ready;
};

static int setup(struct test_ctx *ctx, long page_size)
{
	struct io_uring_query_zcrx query = {};
	struct io_uring_query_hdr hdr = {
		.size = sizeof(query),
		.query_data = uring_ptr_to_u64(&query),
		.query_op = IO_URING_QUERY_ZCRX,
	};
	struct io_uring_region_desc region;
	size_t rq_size;
	int ret, one = 1;

	ret = io_uring_register(-1, IORING_REGISTER_QUERY, &hdr, 0);
	if (ret < 0 || hdr.result < 0 ||
	    !(query.register_flags & ZCRX_REG_NODEV))
		return T_EXIT_SKIP;

	ret = t_create_ring(8, &ctx->ring, IORING_SETUP_DEFER_TASKRUN |
			    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQE32);
	if (ret != T_SETUP_OK)
		return ret == T_SETUP_SKIP ? T_EXIT_SKIP : T_EXIT_FAIL;
	ctx->ring_ready = true;

	rq_size = (query.rq_hdr_size + query.rq_hdr_alignment - 1) &
		  ~(query.rq_hdr_alignment - 1);
	rq_size += RQ_ENTRIES * sizeof(struct io_uring_zcrx_rqe);
	rq_size = (rq_size + page_size - 1) & ~(page_size - 1);
	ctx->rq_mem = t_aligned_alloc(page_size, rq_size);
	ctx->area_mem = t_aligned_alloc(page_size, NR_BUFS * page_size);
	if (!ctx->rq_mem || !ctx->area_mem)
		return T_EXIT_FAIL;
	memset(ctx->rq_mem, 0, rq_size);
	ctx->area = (struct io_uring_zcrx_area_reg) {
		.addr = uring_ptr_to_u64(ctx->area_mem),
		.len = NR_BUFS * page_size,
	};
	region = (struct io_uring_region_desc) {
		.user_addr = uring_ptr_to_u64(ctx->rq_mem),
		.size = rq_size,
		.flags = IORING_MEM_REGION_TYPE_USER,
	};
	ctx->reg = (struct io_uring_zcrx_ifq_reg) {
		.flags = ZCRX_REG_NODEV,
		.rq_entries = RQ_ENTRIES,
		.area_ptr = uring_ptr_to_u64(&ctx->area),
		.region_ptr = uring_ptr_to_u64(&region),
		.rx_buf_len = page_size,
	};
	ret = io_uring_register_ifq(&ctx->ring, &ctx->reg);
	if (ret) {
		fprintf(stderr, "register NODEV: %s\n", strerror(-ret));
		return ret == -EPERM ? T_EXIT_SKIP : T_EXIT_FAIL;
	}
	ctx->rq.khead = ctx->rq_mem + ctx->reg.offsets.head;
	ctx->rq.ktail = ctx->rq_mem + ctx->reg.offsets.tail;
	ctx->rq.rqes = ctx->rq_mem + ctx->reg.offsets.rqes;
	ctx->rq.ring_entries = ctx->reg.rq_entries;

	ret = t_create_socket_pair(ctx->fds, true);
	if (ret || setsockopt(ctx->fds[1], IPPROTO_TCP, TCP_NODELAY,
			     &one, sizeof(one)))
		return T_EXIT_FAIL;
	return T_EXIT_PASS;
}

static int recv_byte(struct test_ctx *ctx, unsigned char value, bool send_byte,
		     struct io_uring_zcrx_rqe *buffer)
{
	struct __kernel_timespec timeout = { .tv_sec = 2 };
	struct io_uring_zcrx_cqe *zcqe;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	uint64_t offset;
	int ret;

	sqe = io_uring_get_sqe(&ctx->ring);
	io_uring_prep_rw(IORING_OP_RECV_ZC, sqe, ctx->fds[0], NULL, 1, 0);
	sqe->zcrx_ifq_idx = ctx->reg.zcrx_id;
	sqe->ioprio |= IORING_RECV_MULTISHOT;
	ret = io_uring_submit(&ctx->ring);
	if (ret != 1)
		return ret < 0 ? ret : -EINVAL;
	if (send_byte && send(ctx->fds[1], &value, 1, MSG_NOSIGNAL) != 1)
		return -errno;
	ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &timeout);
	if (ret)
		return ret;
	if (cqe->res < 0) {
		ret = cqe->res;
		io_uring_cqe_seen(&ctx->ring, cqe);
		return ret;
	}
	if (cqe->res != 1 || !(cqe->flags & IORING_CQE_F_MORE))
		return -EINVAL;
	zcqe = (void *)(cqe + 1);
	offset = zcqe->off & ~IORING_ZCRX_AREA_MASK;
	if (offset >= ctx->area.len || ((unsigned char *)ctx->area_mem)[offset] != value)
		return -EIO;
	buffer->off = offset | ctx->area.rq_area_token;
	buffer->len = cqe->res;
	buffer->__pad = 0;
	io_uring_cqe_seen(&ctx->ring, cqe);
	ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &timeout);
	if (ret)
		return ret;
	ret = cqe->res || (cqe->flags & IORING_CQE_F_MORE) ? -EINVAL : 0;
	io_uring_cqe_seen(&ctx->ring, cqe);
	return ret;
}

static void return_buffer(struct test_ctx *ctx, struct io_uring_zcrx_rqe buffer)
{
	struct io_uring_zcrx_rq *rq = &ctx->rq;

	rq->rqes[rq->rq_tail & (rq->ring_entries - 1)] = buffer;
	io_uring_smp_store_release(rq->ktail, ++rq->rq_tail);
}

static int test_reclaim(struct test_ctx *ctx)
{
	struct io_uring_zcrx_rqe held[2], buffer;
	unsigned int head, i;
	int ret;

	for (i = 0; i < NR_BUFS; i++) {
		ret = recv_byte(ctx, i, true, &buffer);
		if (ret) {
			fprintf(stderr, "fill buffer %u: %s\n", i, strerror(-ret));
			return T_EXIT_FAIL;
		}
		if (i < 2)
			held[i] = buffer;
	}
	ret = recv_byte(ctx, NR_BUFS, true, &buffer);
	if (ret != -ENOMEM) {
		fprintf(stderr, "held buffers must exhaust the pool, got %d\n", ret);
		return T_EXIT_FAIL;
	}
	puts("Unreturned buffers correctly exhaust the fallback pool");

	/* Keep the RQ below the threshold that would otherwise trigger a flush. */
	return_buffer(ctx, held[0]);
	return_buffer(ctx, held[1]);
	ret = recv_byte(ctx, NR_BUFS, false, &buffer);
	if (ret) {
		fprintf(stderr, "queued fallback returns were not reused: %s (%d)\n",
			strerror(-ret), ret);
		return T_EXIT_FAIL;
	}
	if (buffer.off != held[0].off && buffer.off != held[1].off) {
		fprintf(stderr, "reused a buffer whose user reference is still held\n");
		return T_EXIT_FAIL;
	}
	head = io_uring_smp_load_acquire(ctx->rq.khead);
	if (!head) {
		fprintf(stderr, "reused a buffer without consuming its return\n");
		return T_EXIT_FAIL;
	}
	puts("Queued fallback returns reclaimed without an explicit RQ flush");
	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	struct test_ctx ctx = { .fds = { -1, -1 } };
	long page_size;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return T_EXIT_FAIL;
	alarm(20);
	ret = setup(&ctx, page_size);
	if (!ret)
		ret = test_reclaim(&ctx);
	if (ctx.ring_ready)
		io_uring_queue_exit(&ctx.ring);
	if (ctx.fds[0] >= 0)
		close(ctx.fds[0]);
	if (ctx.fds[1] >= 0)
		close(ctx.fds[1]);
	free(ctx.rq_mem);
	free(ctx.area_mem);
	return ret;
}
