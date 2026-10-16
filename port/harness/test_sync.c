// Host-side unit tests for the Win32 synchronisation / thread / time / file HLE (hle.c, hle2.c), run against the real handlers with a fake
// Ctx (4 guest threads with their own TEB) over real Unicorn-mapped guest memory. A blocking handler sets c->block: call() reports it as "BLOCKED"
// (the dispatcher would switch threads and re-run the call). The guest clock is fake: advance(ms).
// build: make test_sync     run: ./test_sync [tmpdir]
#include "emu.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define main blur_main_unused
#include "main.c"
#undef main

static Ctx C_; static int nfail, npass; static const char *cur_test = "";
#define BLOCKED 0xB10C4EDu
#define T_(c) do { if (!(c)) { nfail++; printf("  FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #c); } else npass++; } while (0)
#define EQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { nfail++; printf("  FAIL [%s] line %d: %s == %s  (got 0x%llx, want 0x%llx)\n", cur_test, __LINE__, #a, #b, (unsigned long long)_a, (unsigned long long)_b); } else npass++; } while (0)

// fake guest clock
static uint64_t g_fake_us = 1000000000ull;
uint64_t now_us(void) { return g_fake_us; }
static void advance(uint32_t ms) { g_fake_us += (uint64_t)ms * 1000; }

static const HleDef *lookup(const char *n) { const HleDef *d = hle_lookup(n); if (!d) d = hle2_lookup(n); return d; }
static void tcur(int i) { C_.cur = i; }
static uint32_t scratch = 0x1100000;        // guest scratch area (bump)
static uint32_t S(uint32_t n) { uint32_t p = scratch; scratch += (n + 15) & ~15u; return p; }
static uint32_t call_v(const char *fn, const uint32_t *args) {
    const HleDef *d = lookup(fn); if (!d) { printf("  no handler %s\n", fn); nfail++; return 0; }
    uint32_t a[16] = {0}; memcpy(a, args, 7 * 4); C_.block = 0; C_.yield_req = 0; uint32_t r = d->fn(&C_, a);
    if (C_.block) { C_.block = 0; return BLOCKED; } return r;
}
#define CALL(fn, ...) call_v(fn, (uint32_t[]){0, ##__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0} + 1)
static uint32_t lasterr(void) { return rd32(&C_, C_.threads[C_.cur].teb + 0x34); }

static uint32_t mkevent(int manual, int init) { return CALL("CreateEventA", 0, manual, init, 0); }
static void test_events(void) {
    cur_test = "events"; tcur(0);
    uint32_t e = mkevent(0, 0);
    EQ(CALL("WaitForSingleObject", e, 0), 0x102);                       // poll an unsignalled event: WAIT_TIMEOUT
    CALL("SetEvent", e); EQ(CALL("WaitForSingleObject", e, 0), 0);      // signalled -> satisfied, auto-reset consumes it
    EQ(CALL("WaitForSingleObject", e, 0), 0x102);
    CALL("SetEvent", e); CALL("SetEvent", e);                           // SetEvent does not count
    EQ(CALL("WaitForSingleObject", e, 0), 0); EQ(CALL("WaitForSingleObject", e, 0), 0x102);
    // two waiters, one SetEvent -> exactly one wakes
    tcur(1); EQ(CALL("WaitForSingleObject", e, 0xffffffff), BLOCKED);
    tcur(2); EQ(CALL("WaitForSingleObject", e, 0xffffffff), BLOCKED);
    tcur(0); CALL("SetEvent", e);
    tcur(1); EQ(CALL("WaitForSingleObject", e, 0xffffffff), 0);
    tcur(2); EQ(CALL("WaitForSingleObject", e, 0xffffffff), BLOCKED);
    // manual-reset: stays signalled for everybody until ResetEvent
    uint32_t m = mkevent(1, 0); tcur(1); EQ(CALL("WaitForSingleObject", m, 0xffffffff), BLOCKED); tcur(2); EQ(CALL("WaitForSingleObject", m, 0xffffffff), BLOCKED);
    tcur(0); CALL("SetEvent", m); tcur(1); EQ(CALL("WaitForSingleObject", m, 0xffffffff), 0); tcur(2); EQ(CALL("WaitForSingleObject", m, 0xffffffff), 0);
    EQ(CALL("WaitForSingleObject", m, 0), 0); CALL("ResetEvent", m); EQ(CALL("WaitForSingleObject", m, 0), 0x102);
    // initial state signalled
    uint32_t s = mkevent(0, 1); EQ(CALL("WaitForSingleObject", s, 0), 0); EQ(CALL("WaitForSingleObject", s, 0), 0x102);
    // timeouts use the guest clock; timeout 0 never blocks
    uint32_t t = mkevent(0, 0); tcur(3); EQ(CALL("WaitForSingleObject", t, 50), BLOCKED); advance(49); EQ(CALL("WaitForSingleObject", t, 50), BLOCKED);
    advance(2); EQ(CALL("WaitForSingleObject", t, 50), 0x102);
    EQ(CALL("WaitForSingleObject", t, 50), BLOCKED);                    // the next wait starts a fresh deadline
    // invalid handles
    tcur(0); EQ(CALL("WaitForSingleObject", 0x7777, 0), 0xffffffff); EQ(lasterr(), 6);
    EQ(CALL("SetEvent", 0x7777), 0); EQ(lasterr(), 6);
    // named events share the object; GetLastError = ERROR_ALREADY_EXISTS
    uint32_t nm = S(16); strcpy((char *)G + nm, "Global\\Test"); uint32_t e1 = CALL("CreateEventA", 0, 1, 0, nm); EQ(lasterr(), 0);
    uint32_t e2 = CALL("CreateEventA", 0, 0, 1, nm); EQ(e2, e1); EQ(lasterr(), 183); EQ(CALL("OpenEventA", 0x1f0003, 0, nm), e1);
    // setting an event handle that is a semaphore / mutex is an error
    uint32_t sem = CALL("CreateSemaphoreA", 0, 1, 2, 0); EQ(CALL("SetEvent", sem), 0);
}
static void test_semaphores(void) {
    cur_test = "semaphores"; tcur(0);
    uint32_t sem = CALL("CreateSemaphoreA", 0, 2, 3, 0), prev = S(4);
    EQ(CALL("WaitForSingleObject", sem, 0), 0); EQ(CALL("WaitForSingleObject", sem, 0), 0); EQ(CALL("WaitForSingleObject", sem, 0), 0x102);
    EQ(CALL("ReleaseSemaphore", sem, 1, prev), 1); EQ(rd32(&C_, prev), 0);                  // previous count
    EQ(CALL("ReleaseSemaphore", sem, 3, prev), 0); EQ(lasterr(), 298);                      // would exceed max (1 + 3 > 3): ERROR_TOO_MANY_POSTS, count unchanged
    EQ(CALL("ReleaseSemaphore", sem, 0, prev), 0);                                          // zero release count is invalid
    EQ(CALL("ReleaseSemaphore", sem, 2, prev), 1); EQ(rd32(&C_, prev), 1);
    EQ(CALL("WaitForSingleObject", sem, 0), 0); EQ(CALL("WaitForSingleObject", sem, 0), 0); EQ(CALL("WaitForSingleObject", sem, 0), 0); EQ(CALL("WaitForSingleObject", sem, 0), 0x102);
    // each waiter consumes ONE count
    tcur(1); EQ(CALL("WaitForSingleObject", sem, 0xffffffff), BLOCKED); tcur(2); EQ(CALL("WaitForSingleObject", sem, 0xffffffff), BLOCKED);
    tcur(0); CALL("ReleaseSemaphore", sem, 1, 0);
    tcur(1); EQ(CALL("WaitForSingleObject", sem, 0xffffffff), 0); tcur(2); EQ(CALL("WaitForSingleObject", sem, 0xffffffff), BLOCKED);
    EQ(CALL("ReleaseSemaphore", 0x7777, 1, 0), 0);
}
static void test_mutexes(void) {
    cur_test = "mutexes"; tcur(0);
    uint32_t m = CALL("CreateMutexA", 0, 0, 0);
    EQ(CALL("WaitForSingleObject", m, 0), 0); EQ(CALL("WaitForSingleObject", m, 0), 0);                // recursive for the owner
    tcur(1); EQ(CALL("WaitForSingleObject", m, 0), 0x102); EQ(CALL("ReleaseMutex", m), 0); EQ(lasterr(), 288);   // non-owner release fails: ERROR_NOT_OWNER
    EQ(CALL("WaitForSingleObject", m, 0xffffffff), BLOCKED);
    tcur(0); EQ(CALL("ReleaseMutex", m), 1); tcur(1); EQ(CALL("WaitForSingleObject", m, 0xffffffff), BLOCKED);   // still held once
    tcur(0); EQ(CALL("ReleaseMutex", m), 1); EQ(CALL("ReleaseMutex", m), 0);                                   // third release: not owned any more
    tcur(1); EQ(CALL("WaitForSingleObject", m, 0xffffffff), 0);
    // abandoned: owner thread exits while holding it
    Thread *t1 = &C_.threads[1]; hle_thread_exit(&C_, t1); t1->done = 1;
    tcur(2); EQ(CALL("WaitForSingleObject", m, 0), 0x80);                                             // WAIT_ABANDONED_0 once ...
    EQ(CALL("ReleaseMutex", m), 1); EQ(CALL("WaitForSingleObject", m, 0), 0);                          // ... then normal
    CALL("ReleaseMutex", m); t1->done = 0;
    uint32_t own = CALL("CreateMutexA", 0, 1, 0); tcur(3); C_.threads[3].wake_at = 0; EQ(CALL("WaitForSingleObject", own, 0), 0x102);   // initial owner = creator (t2)
}
static void test_wait_multiple(void) {
    cur_test = "WaitForMultipleObjects"; tcur(0);
    uint32_t e0 = mkevent(0, 0), e1 = mkevent(0, 0), e2 = mkevent(1, 0), arr = S(16); wr32(&C_, arr, e0); wr32(&C_, arr + 4, e1); wr32(&C_, arr + 8, e2);
    EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 0), 0x102);
    CALL("SetEvent", e1); EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 0), 1); EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 0), 0x102);    // returns the index, consumes only that one
    CALL("SetEvent", e0); CALL("SetEvent", e1); EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 0), 0); EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 0), 1);   // lowest index first
    // wait-all: nothing is consumed until everything is signalled
    CALL("SetEvent", e0); CALL("SetEvent", e2); EQ(CALL("WaitForMultipleObjects", 3, arr, 1, 0), 0x102);
    EQ(CALL("WaitForSingleObject", e0, 0), 0); CALL("SetEvent", e0);                                  // e0 survived the failed wait-all
    CALL("SetEvent", e1); EQ(CALL("WaitForMultipleObjects", 3, arr, 1, 0), 0);                        // all three ready -> WAIT_OBJECT_0
    EQ(CALL("WaitForSingleObject", e0, 0), 0x102); EQ(CALL("WaitForSingleObject", e1, 0), 0x102); EQ(CALL("WaitForSingleObject", e2, 0), 0);   // auto events consumed, manual not
    EQ(CALL("WaitForMultipleObjects", 0, arr, 0, 0), 0xffffffff);                                     // n == 0 invalid
    wr32(&C_, arr + 4, 0x7777); EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 0), 0xffffffff);
    // blocking with timeout
    wr32(&C_, arr + 4, e1); CALL("ResetEvent", e2); tcur(1); EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 100), BLOCKED); advance(101); EQ(CALL("WaitForMultipleObjects", 3, arr, 0, 100), 0x102);
}
static void test_signal_and_wait(void) {
    cur_test = "SignalObjectAndWait"; tcur(0);
    uint32_t a = mkevent(0, 0), b = mkevent(0, 0);
    EQ(CALL("SignalObjectAndWait", a, b, 0xffffffff, 0), BLOCKED);                // signalled A, still waiting for B
    tcur(1); EQ(CALL("WaitForSingleObject", a, 0), 0);                           // another thread sees A
    tcur(0); CALL("SetEvent", b); EQ(CALL("SignalObjectAndWait", a, b, 0xffffffff, 0), 0);   // resumed call: B satisfied, A is NOT signalled a second time
    tcur(1); EQ(CALL("WaitForSingleObject", a, 0), 0x102);
    tcur(0); CALL("SetEvent", b); EQ(CALL("SignalObjectAndWait", a, b, 0, 0), 0); tcur(1); EQ(CALL("WaitForSingleObject", a, 0), 0);   // fresh call signals A again
    tcur(0); EQ(CALL("SignalObjectAndWait", a, 0x7777, 0, 0), 0xffffffff);
}
static void test_critical_sections(void) {
    cur_test = "critical sections"; tcur(0);
    uint32_t cs = S(32); CALL("InitializeCriticalSection", cs);
    EQ(rd32(&C_, cs + 4), 0xffffffff); EQ(rd32(&C_, cs + 8), 0); EQ(rd32(&C_, cs + 12), 0);          // LockCount -1, RecursionCount 0, OwningThread 0
    EQ(CALL("EnterCriticalSection", cs), 0); EQ(rd32(&C_, cs + 8), 1); EQ(rd32(&C_, cs + 12), C_.threads[0].tid); EQ(rd32(&C_, cs + 4), 0);   // LockCount 0 = held once
    EQ(CALL("EnterCriticalSection", cs), 0); EQ(rd32(&C_, cs + 8), 2); EQ(rd32(&C_, cs + 4), 1);
    tcur(1); EQ(CALL("TryEnterCriticalSection", cs), 0); EQ(CALL("EnterCriticalSection", cs), BLOCKED);
    CALL("LeaveCriticalSection", cs); EQ(rd32(&C_, cs + 8), 2);                                   // leave by a non-owner is ignored
    tcur(0); CALL("LeaveCriticalSection", cs); tcur(1); EQ(CALL("EnterCriticalSection", cs), BLOCKED);
    tcur(0); CALL("LeaveCriticalSection", cs); EQ(rd32(&C_, cs + 12), 0); EQ(rd32(&C_, cs + 4), 0xffffffff);
    tcur(1); EQ(CALL("EnterCriticalSection", cs), 0); EQ(rd32(&C_, cs + 12), C_.threads[1].tid); CALL("LeaveCriticalSection", cs);
    EQ(CALL("TryEnterCriticalSection", cs), 1); CALL("LeaveCriticalSection", cs);
    EQ(CALL("InitializeCriticalSectionAndSpinCount", S(32), 4000), 1);
}
static void test_threads(void) {
    cur_test = "threads"; tcur(0);
    // a fake thread record (CreateThread itself needs Unicorn contexts: covered by the live run)
    int idx = 5; Thread *t = &C_.threads[idx]; memset(t, 0, sizeof *t); t->used = 1; t->tid = 0x2000; t->teb = 0x1000000 + idx * 0x2000; C_.nthreads = 6;
    uint32_t h = new_handle(&C_, H_THREAD); C_.handles[(h - 0x100) / 4].thread = idx; t->handle = h; t->suspended = 1;
    EQ(CALL("SuspendThread", h), 1); EQ(t->suspended, 2); EQ(CALL("ResumeThread", h), 2); EQ(t->suspended, 1); EQ(CALL("ResumeThread", h), 1); EQ(t->suspended, 0); EQ(CALL("ResumeThread", h), 0);
    uint32_t code = S(4); EQ(CALL("GetExitCodeThread", h, code), 1); EQ(rd32(&C_, code), 259);
    tcur(1); EQ(CALL("WaitForSingleObject", h, 0), 0x102); EQ(CALL("WaitForSingleObject", h, 50), BLOCKED);
    t->done = 1; t->exit_code = 7; C_.handles[(h - 0x100) / 4].signaled = 1;                       // what ExitThread / thread return do
    EQ(CALL("WaitForSingleObject", h, 0), 0); EQ(CALL("WaitForSingleObject", h, 0), 0);            // thread handles stay signalled
    EQ(CALL("GetExitCodeThread", h, code), 1); EQ(rd32(&C_, code), 7);
    EQ(CALL("SetThreadPriority", h, 2), 1); EQ(CALL("GetThreadPriority", 0xfffffffe), 0);
    { uint32_t dup = S(4); tcur(2); EQ(CALL("DuplicateHandle", 0xffffffff, 0xfffffffe, 0xffffffff, dup, 0, 0, 2), 1); uint32_t th = rd32(&C_, dup); T_(th != 0xfffffffe);   // a real handle bound to THIS thread
      tcur(3); EQ(CALL("SuspendThread", th), 0); EQ(C_.threads[2].suspended, 1); EQ(CALL("ResumeThread", th), 1); EQ(CALL("ResumeThread", 0xfffffffe), 0);   // pseudo handle = calling thread (t3)
      EQ(CALL("GetExitCodeThread", th, S(4)), 1); tcur(0); }
    EQ(CALL("GetCurrentThread"), 0xfffffffe); tcur(2); EQ(CALL("GetCurrentThreadId"), C_.threads[2].tid);
    // TLS: distinct indices, per-thread values
    uint32_t i0 = CALL("TlsAlloc"), i1 = CALL("TlsAlloc"); T_(i0 != i1);
    tcur(0); CALL("TlsSetValue", i0, 0x1111); tcur(1); CALL("TlsSetValue", i0, 0x2222); tcur(0); EQ(CALL("TlsGetValue", i0), 0x1111); tcur(1); EQ(CALL("TlsGetValue", i0), 0x2222); EQ(CALL("TlsGetValue", i1), 0);
    // Sleep(0) / SwitchToThread yield once, Sleep(n) waits on the guest clock
    tcur(0); EQ(CALL("Sleep", 0), BLOCKED); EQ(CALL("Sleep", 0), 0);
    EQ(CALL("SwitchToThread"), BLOCKED); EQ(CALL("SwitchToThread"), 1);
    EQ(CALL("Sleep", 10), BLOCKED); advance(9); EQ(CALL("Sleep", 10), BLOCKED); advance(2); EQ(CALL("Sleep", 10), 0);
}
static void test_time_and_process(void) {
    cur_test = "time/process"; tcur(0);
    uint32_t q0 = S(8), q1 = S(8), f = S(8), ft = S(8);
    EQ(CALL("QueryPerformanceFrequency", f), 1); EQ(rd32(&C_, f), 10000000); EQ(rd32(&C_, f + 4), 0);
    CALL("QueryPerformanceCounter", q0); uint32_t tick0 = CALL("GetTickCount"); advance(1000); CALL("QueryPerformanceCounter", q1); uint32_t tick1 = CALL("GetTickCount");
    uint64_t d = (((uint64_t)rd32(&C_, q1 + 4) << 32) | rd32(&C_, q1)) - (((uint64_t)rd32(&C_, q0 + 4) << 32) | rd32(&C_, q0));
    EQ(d, 10000000); EQ(tick1 - tick0, 1000);                                                     // QPC at 10 MHz and GetTickCount (ms) tick consistently
    advance(5); CALL("QueryPerformanceCounter", q0); T_((((uint64_t)rd32(&C_, q0 + 4) << 32) | rd32(&C_, q0)) > (((uint64_t)rd32(&C_, q1 + 4) << 32) | rd32(&C_, q1)));   // monotonic
    CALL("GetSystemTimeAsFileTime", ft); uint64_t fts = ((uint64_t)rd32(&C_, ft + 4) << 32) | rd32(&C_, ft); int64_t unix_s = (int64_t)(fts / 10000000ull) - 11644473600ll;
    T_(llabs(unix_s - (int64_t)time(NULL)) < 10);                                                  // FILETIME is the real current date (not 1970)
    uint32_t si = S(48); CALL("GetSystemInfo", si); EQ(rd32(&C_, si + 20), 8); EQ(rd32(&C_, si + 16), 0xff); EQ(rd32(&C_, si + 4), 0x1000);
    uint32_t pm = S(4), sm = S(4); EQ(CALL("GetProcessAffinityMask", 0xffffffff, pm, sm), 1); EQ(rd32(&C_, pm), 0xff); EQ(rd32(&C_, sm), 0xff);
    EQ(CALL("GetCurrentProcess"), 0xffffffff);
    uint32_t e = mkevent(0, 0), dup = S(4); EQ(CALL("DuplicateHandle", 0xffffffff, e, 0xffffffff, dup, 0, 0, 2), 1); EQ(rd32(&C_, dup), e);
    EQ(CALL("DuplicateHandle", 0xffffffff, 0x7777, 0xffffffff, dup, 0, 0, 2), 0);
    uint32_t pc = S(4); EQ(CALL("GetExitCodeProcess", 0xffffffff, pc), 1); EQ(rd32(&C_, pc), 259);
}
static void test_files(const char *dir) {
    cur_test = "files"; tcur(0);
    char path[600]; snprintf(path, sizeof path, "%s/sync_test.bin", dir); FILE *f = fopen(path, "wb"); for (int i = 0; i < 1000; i++) fputc(i & 255, f); fclose(f);
    snprintf(g_gamedir, sizeof g_gamedir, "%s", dir); snprintf(g_savedir, sizeof g_savedir, "%s/save", dir); mkdir(g_savedir, 0755);
    uint32_t name = S(64); strcpy((char *)G + name, "C:\\BLUR\\sync_test.bin");
    uint32_t h = CALL("CreateFileA", name, 0x80000000u, 1, 0, 3, 0x80, 0); T_(h != 0xffffffff);
    uint32_t buf = S(2048), nread = S(4), hi = S(4);
    EQ(CALL("GetFileSize", h, hi), 1000); EQ(rd32(&C_, hi), 0);
    EQ(CALL("ReadFile", h, buf, 100, nread, 0), 1); EQ(rd32(&C_, nread), 100); EQ(G[buf + 99], 99);
    EQ(CALL("SetFilePointer", h, 990, 0, 0), 990); EQ(CALL("ReadFile", h, buf, 100, nread, 0), 1); EQ(rd32(&C_, nread), 10);        // partial read at EOF
    EQ(CALL("ReadFile", h, buf, 100, nread, 0), 1); EQ(rd32(&C_, nread), 0);                                                         // at EOF: success with 0 bytes
    EQ(CALL("SetFilePointer", h, (uint32_t)-10, 0, 2), 990);                                                                          // FILE_END with negative distance
    EQ(CALL("SetFilePointer", h, 5, 0, 1), 995);                                                                                      // FILE_CURRENT
    wr32(&C_, hi, 0); EQ(CALL("SetFilePointer", h, 7, hi, 0), 7); EQ(rd32(&C_, hi), 0);                                               // high word pointer given: 64-bit distance
    wr32(&C_, hi, 1); EQ(CALL("SetFilePointer", h, 0, hi, 0), 0); EQ(rd32(&C_, hi), 1);                                               // 4 GiB: low 0, high 1 written back
    EQ(CALL("SetFilePointer", h, 0, 0, 0), 0);
    // OVERLAPPED read: explicit offset, event signalled, InternalHigh = bytes
    uint32_t ov = S(32), ev = mkevent(1, 0); wr32(&C_, ov + 8, 500); wr32(&C_, ov + 12, 0); wr32(&C_, ov + 16, ev);
    EQ(CALL("ReadFile", h, buf, 16, 0, ov), 1); EQ(G[buf], (500 & 255)); EQ(rd32(&C_, ov + 4), 16); EQ(CALL("WaitForSingleObject", ev, 0), 0);
    EQ(CALL("GetFileType", h), 1); EQ(CALL("FlushFileBuffers", h), 1); EQ(CALL("GetFileType", 0x13), 2); EQ(CALL("GetDriveTypeA", name), 3);
    EQ(CALL("CloseHandle", h), 1);
    snprintf(path, sizeof path, "%s/save/w.bin", dir); strcpy((char *)G + name, "C:\\BLUR\\w.bin");                                    // writes go to the save dir
    uint32_t w = CALL("CreateFileA", name, 0x40000000u, 0, 0, 2, 0x80, 0); T_(w != 0xffffffff); strcpy((char *)G + buf, "hello"); uint32_t nw = S(4);
    EQ(CALL("WriteFile", w, buf, 5, nw, 0), 1); EQ(rd32(&C_, nw), 5); CALL("FlushFileBuffers", w); CALL("CloseHandle", w);
    struct stat st; T_(stat(path, &st) == 0 && st.st_size == 5);
    strcpy((char *)G + name, "C:\\BLUR\\does_not_exist.bin"); EQ(CALL("CreateFileA", name, 0x80000000u, 1, 0, 3, 0x80, 0), 0xffffffff); EQ(lasterr(), 2);
    EQ(CALL("GetFileAttributesA", name), 0xffffffff);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/blur-sync_test"; mkdir(dir, 0755);
    G = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0); g_recomp = 1;
    Ctx *c = &C_; if (uc_open(UC_ARCH_X86, UC_MODE_32, &c->uc) != UC_ERR_OK) return 2;
    if (uc_mem_map(c->uc, 0x1000000, 0x400000, UC_PROT_ALL) != UC_ERR_OK || uc_mem_map(c->uc, HEAP_BASE, 0x1000000, UC_PROT_ALL) != UC_ERR_OK) return 3;
    c->heap_next = HEAP_BASE; c->nhandles = 0; c->nthreads = 5;
    for (int i = 0; i < 5; i++) { Thread *t = &c->threads[i]; t->used = 1; t->tid = 0x1004 + 4 * (uint32_t)i; t->teb = 0x1000000 + (uint32_t)i * 0x2000; wr32(c, t->teb + 0x2c, t->teb + 0x1000); wr32(c, t->teb + 0x24, t->tid); }
    c->tls_next = 0;
    test_events(); test_semaphores(); test_mutexes(); test_wait_multiple(); test_signal_and_wait(); test_critical_sections(); test_threads(); test_time_and_process(); test_files(dir);
    printf("sync HLE tests: %d checks passed, %d failed\n", npass, nfail); return nfail ? 1 : 0;
}
