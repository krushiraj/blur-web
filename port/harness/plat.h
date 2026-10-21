// Host-platform shims so the harness builds on macOS, Linux and Windows (mingw): counting semaphore with timeout, pc/sp from a signal context.
#pragma once
#include <stdint.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
typedef dispatch_semaphore_t plat_sem;
static inline void plat_sem_init(plat_sem *s) { *s = dispatch_semaphore_create(0); }
static inline void plat_sem_post(plat_sem *s) { dispatch_semaphore_signal(*s); }
static inline int plat_sem_wait_us(plat_sem *s, int64_t us) { return (int)dispatch_semaphore_wait(*s, dispatch_time(DISPATCH_TIME_NOW, us * 1000)); }   // 0 = signalled
#elif defined(_WIN32)
#include <windows.h>
typedef HANDLE plat_sem;
static inline void plat_sem_init(plat_sem *s) { *s = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL); }
static inline void plat_sem_post(plat_sem *s) { ReleaseSemaphore(*s, 1, NULL); }
static inline int plat_sem_wait_us(plat_sem *s, int64_t us) { DWORD ms = (DWORD)((us + 999) / 1000); return WaitForSingleObject(*s, ms) == WAIT_OBJECT_0 ? 0 : 1; }
#else
#include <semaphore.h>
#include <time.h>
#include <errno.h>
typedef sem_t plat_sem;
static inline void plat_sem_init(plat_sem *s) { sem_init(s, 0, 0); }
static inline void plat_sem_post(plat_sem *s) { sem_post(s); }
static inline int plat_sem_wait_us(plat_sem *s, int64_t us) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += us / 1000000; t.tv_nsec += (us % 1000000) * 1000; if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    int r; do { r = sem_timedwait(s, &t); } while (r < 0 && errno == EINTR); return r == 0 ? 0 : 1; }
#endif
