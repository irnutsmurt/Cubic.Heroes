// The host thread's id, cheap enough for every call on a hot path (M11.4): Windows' GetCurrentThreadId (read from
// the thread's own block); elsewhere the kernel's thread id, asked once per thread (a system call) and kept.
#pragma once
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

static inline uint32_t dotheroes_host_tid(void)
{
#ifdef _WIN32
    return (uint32_t)GetCurrentThreadId();
#else
    static thread_local uint32_t tid = 0;
    if (!tid) {
#if defined(__linux__)
        tid = (uint32_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
        uint64_t t = 0;
        pthread_threadid_np(nullptr, &t);
        tid = (uint32_t)t;
#endif
    }
    return tid;
#endif
}
