#pragma once

#include <stdlib.h>
#include <process.h>

#include "win32.h"

typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void pthread_attr_t;
typedef void pthread_mutexattr_t;
typedef void pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1

struct pthread__start {
	void* (*fn)(void*);
	void* arg;
};

static inline unsigned __stdcall pthread__trampoline(void* ud)
{
	struct pthread__start start = *(struct pthread__start*)ud;
	free(ud);
	start.fn(start.arg);
	return 0;
}

static inline int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
		void* (*fn)(void*), void* arg)
{
	(void)attr;
	struct pthread__start* start = malloc(sizeof(*start));
	if (!start)
		return -1;
	start->fn = fn;
	start->arg = arg;
	uintptr_t handle = _beginthreadex(NULL, 0, pthread__trampoline, start,
			0, NULL);
	if (!handle) {
		free(start);
		return -1;
	}
	*thread = (HANDLE)handle;
	return 0;
}

static inline int pthread_join(pthread_t thread, void** retval)
{
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	if (retval)
		*retval = NULL;
	return 0;
}

static inline int pthread_setcancelstate(int state, int* oldstate)
{
	(void)state;
	if (oldstate)
		*oldstate = PTHREAD_CANCEL_ENABLE;
	return 0;
}

static inline int pthread_mutex_init(pthread_mutex_t* mutex,
		const pthread_mutexattr_t* attr)
{
	(void)attr;
	InitializeSRWLock(mutex);
	return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
	(void)mutex;
	return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t* mutex)
{
	AcquireSRWLockExclusive(mutex);
	return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t* mutex)
{
	ReleaseSRWLockExclusive(mutex);
	return 0;
}

static inline int pthread_cond_init(pthread_cond_t* cond,
		const pthread_condattr_t* attr)
{
	(void)attr;
	InitializeConditionVariable(cond);
	return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t* cond)
{
	(void)cond;
	return 0;
}

static inline int pthread_cond_wait(pthread_cond_t* cond,
		pthread_mutex_t* mutex)
{
	SleepConditionVariableSRW(cond, mutex, INFINITE, 0);
	return 0;
}

static inline int pthread_cond_signal(pthread_cond_t* cond)
{
	WakeConditionVariable(cond);
	return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t* cond)
{
	WakeAllConditionVariable(cond);
	return 0;
}
