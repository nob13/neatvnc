#pragma once

#include <stddef.h>

#define IOV_MAX 1024

struct iovec {
	void* iov_base;
	size_t iov_len;
};
