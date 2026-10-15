#pragma once

#include_next <time.h>

#include "win32.h"

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

typedef int clockid_t;

static inline int clock_gettime(clockid_t clock, struct timespec* ts)
{
	if (clock == CLOCK_REALTIME)
		return timespec_get(ts, TIME_UTC) ? 0 : -1;

	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	ts->tv_sec = count.QuadPart / freq.QuadPart;
	ts->tv_nsec = (long)((count.QuadPart % freq.QuadPart) * 1000000000 /
			freq.QuadPart);
	return 0;
}
