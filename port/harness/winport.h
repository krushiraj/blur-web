// Windows (mingw-w64 / llvm-mingw) shims for the POSIX bits the harness uses. Force-included (-include winport.h) when cross-compiling; see ../PORT_NOTES.md.
#pragma once
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#define mkdir(p, m) _mkdir(p)                                    // mingw's mkdir takes no mode
#define setenv(n, v, o) _putenv_s(n, v)
#define localtime_r(t, tm) (localtime_s(tm, t) ? NULL : (tm))
#define gmtime_r(t, tm) (gmtime_s(tm, t) ? NULL : (tm))
static inline void *memmem(const void *h, size_t hl, const void *n, size_t nl) {
    const unsigned char *p = h; if (!nl) return (void *)h; for (; hl >= nl; p++, hl--) if (*p == *(const unsigned char *)n && !memcmp(p, n, nl)) return (void *)p; return NULL; }
#define popen _popen
#define pclose _pclose
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#define FNM_CASEFOLD 1
// minimal fnmatch: '*' '?' only, case-insensitive when FNM_CASEFOLD (the only use: FindFirstFile patterns)
static inline int fnmatch(const char *p, const char *s, int fl) {
    for (; *p; p++, s++) {
        if (*p == '*') { while (p[1] == '*') p++; if (!p[1]) return 0; for (; *s; s++) if (!fnmatch(p + 1, s, fl)) return 0; return 1; }
        if (!*s) return 1;
        if (*p == '?') continue;
        char a = *p, b = *s; if (fl & FNM_CASEFOLD) { if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; }
        if (a != b) return 1;
    }
    return *s != 0;
}
#endif
