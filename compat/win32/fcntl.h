#pragma once

#include_next <fcntl.h>

#include <stdarg.h>

#include "win32.h"

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x10000

static inline int fcntl(int fd, int cmd, ...)
{
	va_list args;
	va_start(args, cmd);
	int flags = cmd == F_SETFL ? va_arg(args, int) : 0;
	va_end(args);

	if (cmd == F_GETFL)
		return 0;

	u_long nonblock = !!(flags & O_NONBLOCK);
	return ioctlsocket(fd, FIONBIO, &nonblock) == 0 ? 0 : -1;
}
