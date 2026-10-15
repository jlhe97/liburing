#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <assert.h>

#include "liburing.h"
#include "test.h"
#include "helpers.h"

#include "mock_file.h"

static struct io_uring mgr_ring;
static __u64 mock_features;
static int mgr_fd;

static bool has_feature(int feature)
{
	return mock_features >= feature;
}

static int setup_mgr(void)
{
	struct io_uring_mock_probe mp;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	ret = mgr_fd = open("/dev/io_uring_mock", O_RDWR);
	if (mgr_fd < 0) {
		if (errno == EACCES)
			printf("No permission to open /dev/io_uring_mock, skipping\n");
		else
			printf("no io_uring mock files, skip\n");
		return T_EXIT_SKIP;
	}

	ret = io_uring_queue_init(8, &mgr_ring, 0);
	if (ret) {
		fprintf(stderr, "mgr ring setup failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	memset(&mp, 0, sizeof(mp));
	sqe = io_uring_get_sqe(&mgr_ring);
	io_uring_prep_uring_cmd(sqe, IORING_MOCK_MGR_CMD_PROBE, mgr_fd);
	sqe->addr  = (__u64)(unsigned long)&mp;
	sqe->len = sizeof(mp);

	ret = t_submit_and_wait_single(&mgr_ring, &cqe);
	if (ret || cqe->res) {
		fprintf(stderr, "probe cmd failed %i %i\n", ret, cqe->res);
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(&mgr_ring, cqe);
	mock_features = mp.features;
	return 0;
}

static int create_mock_file(struct io_uring_mock_create *mc)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(&mgr_ring);
	io_uring_prep_uring_cmd(sqe, IORING_MOCK_MGR_CMD_CREATE, mgr_fd);
	sqe->addr  = (__u64)(unsigned long)mc;
	sqe->len = sizeof(*mc);

	ret = t_submit_and_wait_single(&mgr_ring, &cqe);
	if (ret || cqe->res) {
		fprintf(stderr, "file create cmd failed %i %i\n", ret, cqe->res);
		return T_EXIT_FAIL;
	}
	io_uring_cqe_seen(&mgr_ring, cqe);
	return 0;
}

static int poll_wake(int mock_fd, unsigned int nr_wakeups,
		     unsigned int nr_wakeups_on_poll)
{
	struct io_uring_mock_poll_wake arg = {
		.events = POLLIN,
		.nr_wakeups = nr_wakeups,
		.nr_wakeups_on_poll = nr_wakeups_on_poll,
	};
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(&mgr_ring);
	io_uring_prep_uring_cmd(sqe, IORING_MOCK_CMD_POLL_WAKE, mock_fd);
	sqe->addr = (__u64)(unsigned long)&arg;
	sqe->len = sizeof(arg);

	ret = t_submit_and_wait_single(&mgr_ring, &cqe);
	if (ret || cqe->res) {
		fprintf(stderr, "poll wake command failed %i %i\n", ret,
			cqe->res);
		return T_EXIT_FAIL;
	}
	io_uring_cqe_seen(&mgr_ring, cqe);
	return T_EXIT_PASS;
}

static int t_copy_regvec(struct io_uring *ring, int mock_fd,
			 struct iovec *iov, unsigned iov_len, char *buf,
			 bool from_iov)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_uring_cmd(sqe, IORING_MOCK_CMD_COPY_REGBUF, mock_fd);
	sqe->addr3 = (__u64)(unsigned long)buf;
	sqe->addr = (__u64)(unsigned long)iov;
	sqe->len = iov_len;
	if (from_iov)
		sqe->file_index = IORING_MOCK_COPY_FROM;
	sqe->buf_index = from_iov ? 0 : 1;
	sqe->user_data = 43;
	sqe->uring_cmd_flags |= IORING_URING_CMD_FIXED;

	ret = t_submit_and_wait_single(ring, &cqe);
	if (ret)
		t_error(1, ret, "submit/wait failed");

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static int t_copy_verify_regvec(struct io_uring *ring, int mock_fd,
				struct iovec *iov, unsigned iov_len, char *buf,
				bool from_iov)
{
	struct iovec iov2;
	int ret;

	ret = t_copy_regvec(ring, mock_fd, iov, iov_len, buf, from_iov);
	if (ret < 0 || ret != t_iovec_data_length(iov, iov_len))
		return ret < 0 ? ret : -1;

	iov2.iov_base = buf;
	iov2.iov_len = -1U;

	ret = t_compare_data_iovec(iov, iov_len, &iov2, 1);
	if (ret) {
		fprintf(stderr, "iovec1 data mismatch %i\n", ret);
		return -1;
	}
	return 0;
}

static int test_regvec_cmd(struct io_uring *ring, int mock_fd)
{
	struct iovec buf_iovec[2];
	struct iovec iov[8];
	size_t size = 4096 * 32;
	char *buf_src, *buf_dst;
	int i, ret;

	buf_src = aligned_alloc(4096, size);
	buf_dst = aligned_alloc(4096, size);
	if (!buf_src || !buf_dst)
		t_error(0, -ENOMEM, "can't allocate buffers");

	for (i = 0; i < size; i++)
		buf_src[i] = 'a' + (i % 26);

	buf_iovec[0].iov_base = buf_src;
	buf_iovec[0].iov_len = size;
	buf_iovec[1].iov_base = buf_dst;
	buf_iovec[1].iov_len = size;
	ret = t_register_buffers(ring, buf_iovec, 2);
	if (ret) {
		free(buf_src);
		free(buf_dst);
		return ret == T_SETUP_SKIP ? 0 : T_EXIT_FAIL;
	}

	memset(buf_dst, 0, size);
	iov[0].iov_len = size;
	iov[0].iov_base = buf_src;
	ret = t_copy_verify_regvec(ring, mock_fd, iov, 1, buf_dst, true);
	if (ret < 0) {
		fprintf(stderr, "t_copy_verify_regvec iovec1 failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	memset(buf_dst, 0, size);
	iov[0].iov_len = size;
	iov[0].iov_base = buf_dst;
	ret = t_copy_verify_regvec(ring, mock_fd, iov, 1, buf_src, false);
	if (ret < 0) {
		fprintf(stderr, "t_copy_verify_regvec iovec1 reverse failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	memset(buf_dst, 0, size);
	iov[0].iov_base = buf_src;
	iov[0].iov_len = 5;
	iov[1].iov_base = buf_src + 5;
	iov[1].iov_len = 11;
	iov[2].iov_base = buf_src + (4096 - 127);
	iov[2].iov_len = 127;
	iov[3].iov_base = buf_src + (4096 - 127);
	iov[3].iov_len = 127 + 4096 + 13;
	iov[4].iov_base = buf_src + 4 * 4096;
	iov[4].iov_len = 4096 + 73;
	iov[5].iov_base = buf_src + 7 * 4096 + 127;
	iov[5].iov_len = 4096 * 11 + 132;
	assert(t_iovec_data_length(iov, 6) <= size);
	ret = t_copy_verify_regvec(ring, mock_fd, iov, 6, buf_dst, true);
	if (ret < 0) {
		fprintf(stderr, "t_copy_verify_regvec iovec6 failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	memset(buf_dst, 0, size);
	iov[0].iov_base = buf_dst;
	iov[0].iov_len = 5;
	iov[1].iov_base = buf_dst + 5;
	iov[1].iov_len = 11;
	iov[2].iov_base = buf_dst + (4096 - 127);
	iov[2].iov_len = 127;
	iov[3].iov_base = buf_dst + 4 * 4096;
	iov[3].iov_len = 4096 + 73;
	iov[4].iov_base = buf_dst + 7 * 4096 + 127;
	iov[4].iov_len = 4096 * 11 + 132;
	assert(t_iovec_data_length(iov, 5) <= size);
	ret = t_copy_verify_regvec(ring, mock_fd, iov, 5, buf_src, false);
	if (ret < 0) {
		fprintf(stderr, "t_copy_verify_regvec iovec6 reverse failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	free(buf_src);
	free(buf_dst);
	return 0;
}

static int test_cmds(void)
{
	struct io_uring_mock_create mc;
	struct io_uring ring;
	int ret, mock_fd;

	memset(&mc, 0, sizeof(mc));
	if (create_mock_file(&mc))
		return T_EXIT_FAIL;
	mock_fd = mc.out_fd;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	if (has_feature(IORING_MOCK_FEAT_CMD_COPY)) {
		ret = test_regvec_cmd(&ring, mock_fd);
		if (ret) {
			fprintf(stderr, "test_regvec_cmd() failed\n");
			return T_EXIT_FAIL;
		}
	} else {
		printf("skip test_regvec_cmd()\n");
	}

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_reads(struct io_uring *ring, int mock_fd, void *buffer)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int io_len = 4096;
	int nr_reqs = 16;
	int i, ret;

	for (i = 0; i < nr_reqs; i++) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_read(sqe, mock_fd, buffer, io_len, 0);
		sqe->user_data = i;
	}

	ret = io_uring_submit(ring);
	if (ret != nr_reqs) {
		fprintf(stderr, "submit got %d, wanted %d\n", ret, nr_reqs);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < nr_reqs; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return T_EXIT_FAIL;
		}
		if (cqe->res != io_len) {
			fprintf(stderr, "unexpected cqe res %i, data %i\n",
				cqe->res, (int)cqe->user_data);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
	}
	return 0;
}

static int test_rw(void)
{
	void *buffer;
	struct io_uring ring;
	int ret, i;

	if (!has_feature(IORING_MOCK_FEAT_RW_ZERO)) {
		printf("no mock read-write support, skip\n");
		return T_EXIT_SKIP;
	}

	buffer = malloc(4096);
	if (!buffer) {
		fprintf(stderr, "can't allocate buffers\n");
		return T_EXIT_FAIL;
	}

	ret = io_uring_queue_init(32, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	for (i = 0; i < 8; i++) {
		struct io_uring_mock_create mc;
		bool nowait = i & 1;
		bool async = i & 2;
		bool poll = i & 4;
		int mock_fd;

		memset(&mc, 0, sizeof(mc));
		if (poll) {
			if (!has_feature(IORING_MOCK_FEAT_POLL))
				continue;
			mc.flags |= IORING_MOCK_CREATE_F_POLL;
		}
		if (nowait) {
			if (!has_feature(IORING_MOCK_FEAT_RW_NOWAIT))
				continue;
			mc.flags |= IORING_MOCK_CREATE_F_SUPPORT_NOWAIT;
		}
		if (async) {
			if (!has_feature(IORING_MOCK_FEAT_RW_ASYNC))
				continue;
			mc.rw_delay_ns = 1000 * 1000 * 50;
		}
		mc.file_size = 10 * (1UL << 20);
		if (create_mock_file(&mc))
			return T_EXIT_FAIL;
		mock_fd = mc.out_fd;

		ret = test_reads(&ring, mock_fd, buffer);
		if (ret) {
			fprintf(stderr, "rw failed %i/%i/%i\n",
				nowait, async, poll);
			return T_EXIT_FAIL;
		}

		close(mock_fd);
	}

	free(buffer);
	io_uring_queue_exit(&ring);
	return 0;
}

/* Keep in sync with IO_POLL_REF_BIAS in the kernel's io_uring/poll.c. */
#define POLL_REF_BIAS 128

static int test_poll_retry(void)
{
	struct __kernel_timespec ts = { .tv_sec = 1 };
	struct io_uring_mock_create mc = {
		.flags = IORING_MOCK_CREATE_F_POLL,
	};
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int mock_fd, ret, i;

	if (!has_feature(IORING_MOCK_FEAT_POLL_WAKE)) {
		printf("no mock poll wake support, skip\n");
		return T_EXIT_PASS;
	}

	if (create_mock_file(&mc))
		return T_EXIT_FAIL;
	mock_fd = mc.out_fd;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					       IORING_SETUP_DEFER_TASKRUN);
	if (ret) {
		fprintf(stderr, "poll ring setup failed: %d\n", ret);
		close(mock_fd);
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_multishot(sqe, mock_fd, POLLIN);
	sqe->user_data = 1;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "poll submit failed: %d\n", ret);
		goto err;
	}
	if (io_uring_cq_ready(&ring)) {
		fprintf(stderr, "poll completed before task work was run\n");
		goto err;
	}

	/*
	 * Queue enough wakeups to force io_poll_get_ownership_slowpath().
	 * The wakeup from the next ->poll() invocation then sets the retry
	 * flag while io_poll_check_events() is preparing to drop ownership.
	 */
	ret = poll_wake(mock_fd, POLL_REF_BIAS, 1);
	if (ret)
		goto err;

	ret = io_uring_submit_and_wait_timeout(&ring, &cqe, 2, &ts, NULL);
	if (ret) {
		fprintf(stderr, "waiting for retry CQE failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_peek_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "missing poll CQE %d: %d\n", i, ret);
			goto err;
		}
		if (cqe->user_data != 1 || !(cqe->res & POLLIN) ||
		    !(cqe->flags & IORING_CQE_F_MORE)) {
			fprintf(stderr, "bad poll CQE %d: data=%llu res=%d flags=%x\n",
				i, (unsigned long long)cqe->user_data, cqe->res,
				cqe->flags);
			goto err;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	close(mock_fd);
	return T_EXIT_PASS;
err:
	io_uring_queue_exit(&ring);
	close(mock_fd);
	return T_EXIT_FAIL;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = setup_mgr();
	if (ret)
		return ret;

	ret = test_cmds();
	if (ret)
		return T_EXIT_FAIL;

	ret = test_rw();
	if (ret) {
		fprintf(stderr, "test_rw failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	ret = test_poll_retry();
	if (ret) {
		fprintf(stderr, "test_poll_retry failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&mgr_ring);
	close(mgr_fd);
	return 0;
}
