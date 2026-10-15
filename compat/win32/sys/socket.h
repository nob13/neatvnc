#pragma once

#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>

#include "../win32.h"

#define MSG_NOSIGNAL 0

struct msghdr {
	void* msg_name;
	int msg_namelen;
	struct iovec* msg_iov;
	size_t msg_iovlen;
	void* msg_control;
	size_t msg_controllen;
	int msg_flags;
};

static inline ssize_t sendmsg(int fd, const struct msghdr* msg, int flags)
{
	WSABUF bufs[IOV_MAX];
	size_t n = msg->msg_iovlen < IOV_MAX ? msg->msg_iovlen : IOV_MAX;
	for (size_t i = 0; i < n; ++i) {
		bufs[i].buf = msg->msg_iov[i].iov_base;
		bufs[i].len = msg->msg_iov[i].iov_len;
	}

	DWORD sent = 0;
	if (WSASend(fd, bufs, n, &sent, flags, NULL, NULL) != 0) {
		errno = WSAGetLastError() == WSAEWOULDBLOCK ? EAGAIN : EPIPE;
		return -1;
	}
	return sent;
}
