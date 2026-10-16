// Second batch of Win32 API implementations: virtual filesystem, time, system info, window/display queries,
// crypto, winsock placeholders, and defined failures for Bink / DirectX until those phases land.
#include "emu.h"
static uint32_t packet;      // XInput packet number (file scope so it can be snapshotted)
#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint32_t h_DirectSoundCreate8(Ctx *c, uint32_t *a); uint32_t h_DirectSoundEnumerateA(Ctx *c, uint32_t *a);
#define H(n) static uint32_t h_##n(Ctx *c, uint32_t *a)
#define UNUSED (void)c; (void)a
#define INVALID_HANDLE 0xffffffffu
#define GENERIC_WRITE_ 0x40000000u

static Handle *hp(Ctx *c, uint32_t h) { if (h < 0x104 || (h & 3) || (h - 0x100) / 4 >= (uint32_t)c->nhandles) return NULL; return &c->handles[(h - 0x100) / 4]; }
static void set_err(Ctx *c, uint32_t v) { wr32(c, cur_thread(c)->teb + 0x34, v); }

// ---- constant stubs ---------------------------------------------------------------------------------------------
static uint32_t t_0(Ctx *c, uint32_t *a) { UNUSED; return 0; }
static uint32_t t_1(Ctx *c, uint32_t *a) { UNUSED; return 1; }
static uint32_t t_m1(Ctx *c, uint32_t *a) { UNUSED; return 0xffffffffu; }
static uint32_t t_handle(Ctx *c, uint32_t *a) { UNUSED; return 0x30001; }
static uint32_t t_err2(Ctx *c, uint32_t *a) { UNUSED; return 2; }                // ERROR_FILE_NOT_FOUND
static uint32_t t_fail(Ctx *c, uint32_t *a) { UNUSED; return 0x80004005u; }      // E_FAIL
static uint32_t t_nodev(Ctx *c, uint32_t *a) { UNUSED; return 1167; }            // ERROR_DEVICE_NOT_CONNECTED

// ---- virtual filesystem: C:\BLUR\... -> game dir; writes go to g_savedir -----------------------------------------------
// per-user data (Documents etc.) lives entirely under the save directory
static int map_path_user(const char *rel, char *host, int rw_write) {
    char sv[1024]; snprintf(sv, sizeof sv, "%s/%s", g_savedir, rel);
    if (rw_write) for (char *q = sv + strlen(g_savedir) + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(sv, 0755); *q = '/'; }
    snprintf(host, 1024, "%s", sv); return 1;
}
// The game asks for "ui\\movies\\atvi.bik" while the directory is "UI/movies": Windows and macOS file systems ignore case, the browser file systems
// (WORKERFS, MEMFS) do not. If the exact path does not exist, match every component after the game directory case-insensitively.
static void fix_case(char *host) {
    struct stat st; if (stat(host, &st) == 0) return;
    size_t root = strlen(g_gamedir); if (strncmp(host, g_gamedir, root) || host[root] != '/') return;
    char cur[1024]; snprintf(cur, sizeof cur, "%s", g_gamedir); char *p = host + root + 1;
    while (*p) {
        char *e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p); char comp[256]; if (n >= sizeof comp) return; memcpy(comp, p, n); comp[n] = 0;
        char next[1024]; snprintf(next, sizeof next, "%s/%s", cur, comp);
        if (stat(next, &st) != 0) {
            DIR *d = opendir(cur); if (!d) return; struct dirent *de; int found = 0;
            while ((de = readdir(d))) if (!strcasecmp(de->d_name, comp)) { snprintf(next, sizeof next, "%s/%s", cur, de->d_name); found = 1; break; }
            closedir(d); if (!found) return;
        }
        snprintf(cur, sizeof cur, "%s", next); if (!e) break; p = e + 1;
    }
    snprintf(host, 1024, "%s", cur);
}
static int map_path(const char *gp, char *host, int rw_write) {
    char p[512]; snprintf(p, sizeof p, "%s", gp);
    for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
    char *rel = p;
    { size_t L = strlen(p); while (L > 1 && p[L - 1] == '/') p[--L] = 0;                                    // directory probes with a trailing slash
      if (!strcasecmp(p, "C:/Users") || !strcasecmp(p, "C:/Users/player")) { char tmp[1024]; snprintf(tmp, sizeof tmp, "%s/userprofile", g_savedir); mkdir(g_savedir, 0755); mkdir(tmp, 0755); snprintf(host, 1024, "%s", tmp); return 1; } }
    if (!strncasecmp(rel, "C:/BLUR/", 8)) rel += 8;
    else if (!strncasecmp(rel, "C:/Users/player/", 16)) { char tmp[512]; snprintf(tmp, sizeof tmp, "userprofile/%s", rel + 16); snprintf(p, sizeof p, "%s", tmp); rel = p; *host = 0; return map_path_user(rel, host, rw_write); }
    else if (rel[0] && rel[1] == ':' && rel[2] != '/' && (rel[0] == 'c' || rel[0] == 'C')) rel += 2;      // drive-relative: relative to the current directory (C:\BLUR)
    else if (rel[0] && rel[1] == ':') return 0;                         // other drives don't exist
    else if (rel[0] == '/' && rel[1] == '/') return 0;                  // \\.\ device paths
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    if (strstr(rel, "..")) return 0;
    char sv[1024]; snprintf(sv, sizeof sv, "%s/%s", g_savedir, rel);
    if (rw_write) {
        for (char *q = sv + strlen(g_savedir) + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(sv, 0755); *q = '/'; }
        snprintf(host, 1024, "%s", sv); return 1;
    }
    struct stat st;
    if (stat(sv, &st) == 0) { snprintf(host, 1024, "%s", sv); return 1; }
    snprintf(host, 1024, "%s/%s", g_gamedir, rel); fix_case(host); return 1;
}
static uint32_t open_file(Ctx *c, const char *gp, uint32_t access, uint32_t disp) {
    if (getenv("BLUR_BOOTRAM") && strstr(gp, "gamedata.pak")) { G[0x1226f94] = 1; fprintf(stderr, "[poke] BootRamPakEnable=1\n"); }     // experiment: take the boot_rampak path
    char host[1024]; int wr = (access & GENERIC_WRITE_) || disp == 1 || disp == 2 || disp == 5;
    if (!map_path(gp, host, wr)) { set_err(c, 3); return INVALID_HANDLE; }
    struct stat st; int exists = stat(host, &st) == 0;
    const char *mode;
    if (disp == 3) mode = (access & GENERIC_WRITE_) ? "r+b" : "rb";            // OPEN_EXISTING
    else if (disp == 1) { if (exists) { set_err(c, 80); return INVALID_HANDLE; } mode = "w+b"; }
    else if (disp == 2 || disp == 5) mode = "w+b";
    else mode = exists ? "r+b" : "w+b";                                        // OPEN_ALWAYS
    if (disp == 3 && !exists && (access & GENERIC_WRITE_)) { set_err(c, 2); return INVALID_HANDLE; }
    if (!wr && !exists) { set_err(c, 2); if (c->verbose) fprintf(stderr, "[file] open %s -> not found\n", gp); return INVALID_HANDLE; }
    FILE *f = fopen(host, mode);
    if (!f) { set_err(c, 5); return INVALID_HANDLE; }
    uint32_t h = new_handle(c, H_FILE); hp(c, h)->fp = f; set_err(c, 0);
    if (c->verbose) fprintf(stderr, "[file] open %s -> %s (h=%x)\n", gp, host, h);
    return h;
}
static uint32_t do_mkdir(Ctx *c, const char *gp) {
    char host[1024]; if (!map_path(gp, host, 1)) { set_err(c, 3); return 0; }
    struct stat st; if (stat(host, &st) == 0) { set_err(c, 183); return 0; }                                  // ERROR_ALREADY_EXISTS
    if (mkdir(host, 0755) != 0) { set_err(c, 3); return 0; } set_err(c, 0); if (c->verbose) fprintf(stderr, "[file] CreateDirectory %s -> %s\n", gp, host); return 1;
}
H(CreateDirectoryA) { char n[512]; rd_cstr(c, a[0], n, sizeof n); return do_mkdir(c, n); }
H(CreateDirectoryW) { char n[512]; rd_wstr(c, a[0], n, sizeof n); return do_mkdir(c, n); }
H(CreateFileA) { char n[512]; rd_cstr(c, a[0], n, sizeof n); return open_file(c, n, a[1], a[4]); }
H(CreateFileW) { char n[512]; rd_wstr(c, a[0], n, sizeof n); return open_file(c, n, a[1], a[4]); }
H(ReadFile) {
    Handle *o = hp(c, a[0]); if (!o || o->type != H_FILE) { set_err(c, 6); return 0; }
    if (c->verbose) fprintf(stderr, "[file] ReadFile h=%x n=%u pread=%08x overlapped=%08x off=%08x%08x event=%08x\n", a[0], a[2], a[3], a[4], a[4] ? rd32(c, a[4] + 12) : 0, a[4] ? rd32(c, a[4] + 8) : 0, a[4] ? rd32(c, a[4] + 16) : 0);
    if (a[4]) fseeko(o->fp, ((off_t)rd32(c, a[4] + 12) << 32) | rd32(c, a[4] + 8), SEEK_SET);       // OVERLAPPED: explicit offset
    uint8_t *buf = malloc(a[2] ? a[2] : 1); size_t n = fread(buf, 1, a[2], o->fp);
    if (c->verbose && getenv("BLUR_READLOG")) { fprintf(stderr, "[read] h=%x off=%llx want=%u got=%zu first:", a[0], a[4] ? (((unsigned long long)rd32(c, a[4] + 12) << 32) | rd32(c, a[4] + 8)) : 0ull, a[2], n); for (size_t i = 0; i < 16 && i < n; i++) fprintf(stderr, " %02x", buf[i]); fprintf(stderr, "\n"); }
    wrbuf(c, a[1], buf, n); free(buf); if (a[3]) wr32(c, a[3], (uint32_t)n);
    if (a[4]) {                                                         // complete immediately: bytes transferred, status 0, signal the event
        wr32(c, a[4], 0); wr32(c, a[4] + 4, (uint32_t)n);
        Handle *ev = hp(c, rd32(c, a[4] + 16) & ~1u); if (ev && ev->type == H_EVENT) ev->signaled = 1;
    }
    return 1;
}
H(WriteFile) {
    if (a[0] == 4 || a[0] == 5) { uint8_t *b = malloc(a[2] + 1); rdbuf(c, a[1], b, a[2]); fwrite(b, 1, a[2], stderr); free(b); if (a[3]) wr32(c, a[3], a[2]); return 1; }
    Handle *o = hp(c, a[0]); if (!o || o->type != H_FILE) { set_err(c, 6); return 0; }
    uint8_t *b = malloc(a[2] + 1); rdbuf(c, a[1], b, a[2]); size_t n = fwrite(b, 1, a[2], o->fp); free(b);
    if (a[3]) wr32(c, a[3], (uint32_t)n); return 1;
}
H(SetFilePointer) {
    Handle *o = hp(c, a[0]); if (!o || o->type != H_FILE) return INVALID_HANDLE;
    long off = (int32_t)a[1]; fseek(o->fp, off, a[3] == 0 ? SEEK_SET : a[3] == 1 ? SEEK_CUR : SEEK_END);
    return (uint32_t)ftell(o->fp);
}
H(GetFileSize) {
    Handle *o = hp(c, a[0]); if (!o || o->type != H_FILE) return INVALID_HANDLE;
    long cur = ftell(o->fp); fseek(o->fp, 0, SEEK_END); long sz = ftell(o->fp); fseek(o->fp, cur, SEEK_SET);
    if (a[1]) wr32(c, a[1], 0); return (uint32_t)sz;
}
H(GetFileAttributesA) {
    char n[512], host[1024]; rd_cstr(c, a[0], n, sizeof n);
    if (!map_path(n, host, 0)) { if (c->verbose) fprintf(stderr, "[file] attr %s -> REJECTED by path mapper\n", n); return INVALID_HANDLE; }
    struct stat st; int bad = stat(host, &st);
    if (c->verbose) fprintf(stderr, "[file] attr %s -> %s\n", n, bad ? "MISSING" : S_ISDIR(st.st_mode) ? "dir" : "file");
    if (bad) { set_err(c, 2); return INVALID_HANDLE; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
H(FindFirstFileA) {
    char n[512], host[1024]; rd_cstr(c, a[0], n, sizeof n);
    char dirpart[512]; snprintf(dirpart, sizeof dirpart, "%s", n);
    char *sl = strrchr(dirpart, '\\'); char pat[260];
    if (sl) { snprintf(pat, sizeof pat, "%s", sl + 1); *sl = 0; } else { snprintf(pat, sizeof pat, "%s", dirpart); dirpart[0] = 0; }
    if (!map_path(dirpart[0] ? dirpart : ".", host, 0)) { set_err(c, 3); return INVALID_HANDLE; }
    DIR *d = opendir(host); if (!d) { set_err(c, 3); return INVALID_HANDLE; }
    uint32_t h = new_handle(c, H_FIND); Handle *o = hp(c, h); o->fp = d; snprintf(o->pat, sizeof o->pat, "%s", pat);
    // fall through to "next" for the first entry
    uint32_t args[2] = {h, a[1]}; extern uint32_t hle2_find_next(Ctx *, uint32_t *);
    if (!hle2_find_next(c, args)) { closedir(d); o->fp = NULL; set_err(c, 2); return INVALID_HANDLE; }
    return h;
}
uint32_t hle2_find_next(Ctx *c, uint32_t *a) {
    Handle *o = hp(c, a[0]); if (!o || o->type != H_FIND || !o->fp) return 0;
    struct dirent *e;
    while ((e = readdir(o->fp))) {
        if (fnmatch(strcmp(o->pat, "*.*") ? o->pat : "*", e->d_name, FNM_CASEFOLD)) continue;
        uint8_t z[320] = {0}; wrbuf(c, a[1], z, sizeof z);
        wr32(c, a[1], e->d_type == DT_DIR ? 0x10 : 0x80);
        wrbuf(c, a[1] + 44, e->d_name, strlen(e->d_name) + 1); return 1;
    }
    set_err(c, 18); return 0;
}
H(FindNextFileA) { return hle2_find_next(c, a); }
H(FindClose) { Handle *o = hp(c, a[0]); if (o && o->type == H_FIND && o->fp) { closedir(o->fp); o->fp = NULL; } return 1; }

// ---- time / system info ---------------------------------------------------------------------------------------------------
static void put_systemtime(Ctx *c, uint32_t p, int local) {
    time_t t = time(NULL); struct tm tm; if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    uint16_t v[8] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, 0};
    wrbuf(c, p, v, sizeof v);
}
H(GetSystemTime) { put_systemtime(c, a[0], 0); return 0; }
H(GetLocalTime) { put_systemtime(c, a[0], 1); return 0; }
H(GetTimeZoneInformation) { uint8_t z[172] = {0}; wrbuf(c, a[0], z, sizeof z); return 0; }
H(GlobalMemoryStatusEx) {
    uint8_t z[64] = {0}; wrbuf(c, a[0], z, sizeof z); wr32(c, a[0], 64); wr32(c, a[0] + 4, 30);
    wr32(c, a[0] + 8, 0x80000000u); wr32(c, a[0] + 12, 1); wr32(c, a[0] + 16, 0x60000000u); wr32(c, a[0] + 24, 0xc0000000u); wr32(c, a[0] + 28, 1);
    wr32(c, a[0] + 32, 0x90000000u); wr32(c, a[0] + 40, 0x7ffe0000u); wr32(c, a[0] + 48, 0x70000000u); return 1;
}
H(GlobalMemoryStatus) {
    wr32(c, a[0], 32); wr32(c, a[0] + 4, 30); wr32(c, a[0] + 8, 0x7fffffffu); wr32(c, a[0] + 12, 0x60000000u);
    wr32(c, a[0] + 16, 0x7fffffffu); wr32(c, a[0] + 20, 0x70000000u); wr32(c, a[0] + 24, 0x7ffe0000u); wr32(c, a[0] + 28, 0x70000000u); return 0;
}
H(GetCurrentDirectoryA) { const char *p = "C:\\BLUR"; if (a[0] > strlen(p)) wrbuf(c, a[1], p, strlen(p) + 1); return (uint32_t)strlen(p); }
H(GetFullPathNameA) {
    char n[512], out[600]; rd_cstr(c, a[0], n, sizeof n);
    if (n[0] && n[1] == ':') snprintf(out, sizeof out, "%s", n); else snprintf(out, sizeof out, "C:\\BLUR\\%s", n);
    if (a[1] > strlen(out)) { wrbuf(c, a[2], out, strlen(out) + 1); if (a[3]) { char *s = strrchr(out, '\\'); wr32(c, a[3], a[2] + (uint32_t)(s - out) + 1); } }
    return (uint32_t)strlen(out);
}
H(GetTempPathA) { const char *p = "C:\\TEMP\\"; wrbuf(c, a[1], p, strlen(p) + 1); return (uint32_t)strlen(p); }
H(GetWindowsDirectoryA) { const char *p = "C:\\WINDOWS"; wrbuf(c, a[0], p, strlen(p) + 1); return (uint32_t)strlen(p); }
H(GetSystemDirectoryA) { const char *p = "C:\\WINDOWS\\system32"; wrbuf(c, a[0], p, strlen(p) + 1); return (uint32_t)strlen(p); }
H(GetComputerNameA) { const char *p = "BLUR-PC"; wrbuf(c, a[0], p, strlen(p) + 1); wr32(c, a[1], (uint32_t)strlen(p)); return 1; }
H(GetUserNameA) { const char *p = "player"; wrbuf(c, a[0], p, strlen(p) + 1); wr32(c, a[1], (uint32_t)strlen(p) + 1); return 1; }
H(SHGetFolderPathA) { const char *p = "C:\\Users\\player\\Documents"; wrbuf(c, a[4], p, strlen(p) + 1); return 0; }
H(SHGetSpecialFolderPathA) { const char *p = "C:\\Users\\player\\Documents"; wrbuf(c, a[1], p, strlen(p) + 1); return 1; }
H(GetEnvironmentStrings) { uint32_t p = guest_alloc(c, 8); wrbuf(c, p, "A=1\0\0", 6); return p; }
H(lstrlenW) { uint32_t n = 0; while (rd16(c, a[0] + 2 * n)) n++; return n; }
static int cmpstr(Ctx *c, uint32_t x, uint32_t y, int ci) {
    for (uint32_t i = 0;; i++) { int p = rd8(c, x + i), q = rd8(c, y + i); if (ci) { p = tolower(p); q = tolower(q); } if (p != q) return p < q ? -1 : 1; if (!p) return 0; }
}
H(lstrcmpA) { return (uint32_t)cmpstr(c, a[0], a[1], 0); }
H(lstrcmpiA) { return (uint32_t)cmpstr(c, a[0], a[1], 1); }
H(CompareStringA) { int r = cmpstr(c, a[2], a[4], (a[1] & 1) != 0); return r < 0 ? 1 : r == 0 ? 2 : 3; }
H(CompareStringW) {
    for (uint32_t i = 0;; i++) { int p = rd16(c, a[2] + 2 * i), q = rd16(c, a[4] + 2 * i); if (a[1] & 1) { p = tolower(p); q = tolower(q); } if (p != q) return p < q ? 1 : 3; if (!p) return 2; }
}
H(LCMapStringA) { uint32_t n = a[3] == 0xffffffffu ? 0 : a[3]; if (a[3] == 0xffffffffu) { while (rd8(c, a[2] + n)) n++; n++; } if (a[5]) for (uint32_t i = 0; i < n && i < a[5]; i++) wr8(c, a[4] + i, rd8(c, a[2] + i)); return n; }
H(GetStringTypeA) {
    for (uint32_t i = 0; i < a[3]; i++) { uint8_t ch = rd8(c, a[2] + i); uint16_t t = 0; if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8; if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (isalpha(ch)) t |= 0x100; wr16(c, a[4] + 2 * i, t); }
    return 1;
}

// ---- user32 / display ----------------------------------------------------------------------------------------------------------------
H(GetClientRect) { wr32(c, a[1], 0); wr32(c, a[1] + 4, 0); wr32(c, a[1] + 8, 1280); wr32(c, a[1] + 12, 720); return 1; }
H(GetWindowRect) { return h_GetClientRect(c, a); }
H(GetCursorPos) { wr32(c, a[0], 640); wr32(c, a[0] + 4, 360); return 1; }
H(GetSystemMetrics) { switch (a[0]) { case 0: return 1920; case 1: return 1080; case 16: return 1920; case 17: return 1080; case 4: return 23; case 5: case 6: return 1; } return 0; }
H(EnumDisplaySettingsA) {
    if (a[1] != 0 && a[1] != 0xffffffffu && a[1] != 0xfffffffeu) return 0;            // exactly one mode: 1920x1080@60, 32bpp
    uint8_t z[156] = {0}; wrbuf(c, a[2], z, sizeof z); wr16(c, a[2] + 36, 156);
    wr32(c, a[2] + 104, 32); wr32(c, a[2] + 108, 1920); wr32(c, a[2] + 112, 1080); wr32(c, a[2] + 120, 60);   // DEVMODEA: bpp/width/height/frequency
    wr32(c, a[2] + 40, 0x5c0000); return 1;
}
H(EnumDisplayDevicesA) {
    if (a[1] != 0) return 0;                                                       // exactly one adapter
    uint8_t z[424] = {0}; wrbuf(c, a[2], z, sizeof z); wr32(c, a[2], 424);
    wrbuf(c, a[2] + 4, "\\\\.\\DISPLAY1", 13); wrbuf(c, a[2] + 36, "Emulated Direct3D 9 GPU", 24);
    wr32(c, a[2] + 164, 1 | 4);                                                    // ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
    wrbuf(c, a[2] + 168, "PCI\\VEN_10DE&DEV_0000", 22); return 1;
}
H(MessageBoxA) {
    char t[512], cap[128]; rd_cstr(c, a[1], t, sizeof t); rd_cstr(c, a[2], cap, sizeof cap);
    fprintf(stderr, "\n[MessageBox] %s: %s\n", cap, t); return 1;
}
H(GetKeyState_) { UNUSED; return 0; }

// ---- CryptoAPI hashing: accumulate the data, digest on demand (MD5, SHA-1, SHA-256) --------------------------------------------------
#define MAXHASH 4096
static struct { int used; uint32_t alg; uint8_t *data; size_t len, cap; } hashes[MAXHASH];
H(CryptCreateHash) {                                   // prov, algid, key, flags, phHash
    for (int i = 1; i < MAXHASH; i++) if (!hashes[i].used) { hashes[i].used = 1; hashes[i].alg = a[1]; hashes[i].data = NULL; hashes[i].len = hashes[i].cap = 0; wr32(c, a[4], 0x8000 + (uint32_t)i); if (c->verbose) fprintf(stderr, "[crypt] CreateHash alg=%x -> %x\n", a[1], 0x8000 + i); return 1; }
    return 0;
}
H(CryptHashData) {                                     // hash, data, len, flags
    uint32_t i = a[0] - 0x8000; if (i == 0 || i >= MAXHASH || !hashes[i].used) return 0;
    if (hashes[i].len + a[2] > hashes[i].cap) { hashes[i].cap = (hashes[i].len + a[2]) * 2 + 64; hashes[i].data = realloc(hashes[i].data, hashes[i].cap); }
    rdbuf(c, a[1], hashes[i].data + hashes[i].len, a[2]); hashes[i].len += a[2]; return 1;
}
H(CryptGetHashParam) {                                 // hash, param, pbData, pdwDataLen, flags
    uint32_t i = a[0] - 0x8000; if (i == 0 || i >= MAXHASH || !hashes[i].used) return 0;
    uint8_t d[32]; uint32_t n = 0; static const uint8_t z = 0;
    switch (hashes[i].alg) { case 0x8003: n = 16; md5_digest(hashes[i].data ? hashes[i].data : &z, hashes[i].len, d); break; case 0x8004: n = 20; sha1_digest(hashes[i].data ? hashes[i].data : &z, hashes[i].len, d); break;
                             case 0x800c: n = 32; sha256_digest(hashes[i].data ? hashes[i].data : &z, hashes[i].len, d); break; default: fprintf(stderr, "[crypt] unsupported hash alg %x\n", hashes[i].alg); return 0; }
    if (a[1] == 4) { if (a[2]) wr32(c, a[2], n); wr32(c, a[3], 4); return 1; }          // HP_HASHSIZE
    if (a[1] == 1) { if (a[2]) wr32(c, a[2], hashes[i].alg); wr32(c, a[3], 4); return 1; }   // HP_ALGID
    if (a[1] != 2) return 0;                                                            // HP_HASHVAL
    if (!a[2]) { wr32(c, a[3], n); return 1; }
    if (rd32(c, a[3]) < n) { wr32(c, a[3], n); return 0; }
    wrbuf(c, a[2], d, n); wr32(c, a[3], n); if (c->verbose) { fprintf(stderr, "[crypt] hash of %zu bytes =", hashes[i].len); for (uint32_t k = 0; k < n; k++) fprintf(stderr, "%02x", d[k]); fprintf(stderr, "\n"); } return 1;
}
H(CryptDestroyHash) { uint32_t i = a[0] - 0x8000; if (i && i < MAXHASH && hashes[i].used) { free(hashes[i].data); hashes[i].data = NULL; hashes[i].used = 0; } (void)c; return 1; }

// ---- advapi32 crypto: enough for seeding and integrity-check call sequences ----------------------------------------------------
H(CryptAcquireContextA) { if (a[0]) wr32(c, a[0], 0x7001); return 1; }
H(CryptGenRandom) { for (uint32_t i = 0; i < a[1]; i++) wr8(c, a[2] + i, (uint8_t)(rand() >> 7)); return 1; }

// ---- winsock placeholders (real implementation arrives in the networking phase) -----------------------------------------------------
H(WSAStartup) { uint8_t z[400] = {0}; wrbuf(c, a[1], z, sizeof z); wr16(c, a[1], 0x0202); wr16(c, a[1] + 2, 0x0202); return 0; }
H(htons) { UNUSED; return ((a[0] & 0xff) << 8) | ((a[0] >> 8) & 0xff); }
H(htonl) { UNUSED; uint32_t v = a[0]; return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24); }
H(WSAGetLastError) { UNUSED; return 10093; }                                                      // WSANOTINITIALISED
H(gethostname) { const char *p = "BLUR-PC"; wrbuf(c, a[0], p, strlen(p) + 1); return 0; }

// ---- fake Bink: BINK struct = Width, Height, Frames, FrameNum, LastFrameNum, FrameRate, FrameRateDiv, ReadError, ... NumRects ----
#define BINK_W 1280
#define BINK_H 720
#define BINK_FRAMES 3
H(BinkOpen) {
    char n[256], host[1024]; rd_cstr(c, a[0], n, sizeof n);
    if (c->verbose) { fprintf(stderr, "[bink] BinkOpen arg0=%08x flags=%08x bytes:", a[0], a[1]); for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", rd8(c, a[0] + i)); fprintf(stderr, "\n"); }
    if (!getenv("BINK_FAKE")) return 0;                       // default: movies fail to open (set BINK_FAKE=1 to use the fake player)
    if (!map_path(n, host, 0)) return 0; struct stat st; if (stat(host, &st)) { if (c->verbose) fprintf(stderr, "[bink] open %s -> missing\n", n); return 0; }
    uint32_t b = guest_alloc(c, 1024); uint8_t z[1024] = {0}; wrbuf(c, b, z, sizeof z);
    wr32(c, b, BINK_W); wr32(c, b + 4, BINK_H); wr32(c, b + 8, BINK_FRAMES); wr32(c, b + 20, 30); wr32(c, b + 24, 1); wr32(c, b + 32, a[1]);
    wr32(c, b + 56 + 8 * 16, 1);                       // NumRects slot (after FrameRects[8]) is set when GetRects is called; keep non-zero default
    if (c->verbose) fprintf(stderr, "[bink] open %s -> %08x (fake %dx%d, %d frames)\n", n, b, BINK_W, BINK_H, BINK_FRAMES);
    return b;
}
H(BinkDoFrame) { if (!rd32(c, a[0] + 12)) wr32(c, a[0] + 12, 1); wr32(c, a[0] + 16, rd32(c, a[0] + 12)); return 0; }
H(BinkNextFrame) { uint32_t f = rd32(c, a[0] + 12), n = rd32(c, a[0] + 8); if (f < n) wr32(c, a[0] + 12, f + 1); return 0; }
H(BinkGetFrameBuffersInfo) {   // BINKFRAMEBUFFERS: TotalFrames, Y w/h, cR/cB w/h, FrameNum, then 2 plane sets of {Y,cR,cB,A} x {Buffer, Pitch}
    uint32_t w = BINK_W, h = BINK_H, cw = w / 2, ch = h / 2;
    int nset = getenv("BINK_SETS") ? atoi(getenv("BINK_SETS")) : 2; int nplanes = (rd32(c, a[0] + 32) & 0x100000) ? 4 : 3;   // BINKALPHA => alpha plane present
    wr32(c, a[1], nset); wr32(c, a[1] + 4, w); wr32(c, a[1] + 8, h); wr32(c, a[1] + 12, cw); wr32(c, a[1] + 16, ch); wr32(c, a[1] + 20, 0);
    for (int set = 0; set < nset; set++) {
        uint32_t sz[4] = {w * h, cw * ch, cw * ch, w * h}, pitch[4] = {w, cw, cw, w}; uint8_t fill[4] = {16, 128, 128, 255};
        for (int pl = 0; pl < nplanes; pl++) {                            // no alpha plane: its Buffer/Pitch stay 0
            uint32_t buf = guest_alloc(c, sz[pl] + 16); uint8_t *tmp = malloc(sz[pl]); memset(tmp, fill[pl], sz[pl]); wrbuf(c, buf, tmp, sz[pl]); free(tmp);
            wr32(c, a[1] + 24 + 32 * set + 8 * pl, buf); wr32(c, a[1] + 24 + 32 * set + 8 * pl + 4, pitch[pl]);
        }
    }
    return 0;
}

// ---- real Bink (FFmpeg) glue: BINK_FAKE=1 keeps the old fake player ----------------------------------------------------------------------
#include "bink_hle.h"
static Ctx *bk_c;
static uint32_t bkh_alloc(uint32_t n) { return guest_alloc(bk_c, n); }
static int bkh_map(const char *n, char *host) { return map_path(n, host, 0) && access(host, R_OK) == 0; }
static uint64_t bkh_now(void) { return real_us(); }
static void bk_init_host(Ctx *c) { static int done; bk_c = c; if (done) return; done = 1; BkHost h = { bkh_alloc, bkh_map, bkh_now }; bk_set_host(&h); }
#define BKR(name, fake) static uint32_t r_##name(Ctx *c, uint32_t *a) { if (getenv("BINK_FAKE")) return fake(c, a); bk_init_host(c); return bk_##name(a); }
extern uint8_t g_keys[256]; void input_pump(Ctx *c);
// XInput pad 0 is always "connected" and driven from the keyboard (the game's menus wait for a Start/A press): Enter = Start+A, Space = A, Esc/Backspace = B, arrows = dpad, WASD = left stick.
H(XInputGetState) {
    if (a[0] != 0) return 1167;                                       // ERROR_DEVICE_NOT_CONNECTED
    input_pump(c);
    uint16_t b = 0; int16_t lx = 0, ly = 0;
    if (g_keys[0xc8]) b |= 0x1; if (g_keys[0xd0]) b |= 0x2; if (g_keys[0xcb]) b |= 0x4; if (g_keys[0xcd]) b |= 0x8;
    if (g_keys[0x1c]) b |= 0x10 | 0x1000; if (g_keys[0x39]) b |= 0x1000; if (g_keys[0x01] || g_keys[0x0e]) b |= 0x2000; if (g_keys[0x0f]) b |= 0x20;
    if (g_keys[0x1e]) lx = -32767; if (g_keys[0x20]) lx = 32767; if (g_keys[0x1f]) ly = -32767; if (g_keys[0x11]) ly = 32767;
    uint8_t buf[16] = {0}; uint32_t pk = ++packet; uint8_t lt = g_keys[0x2a] ? 255 : 0, rt = 0; int16_t rx = 0, ry = 0;
    if (g_pad.active) { b |= g_pad.btn; if (g_pad.lt > lt) lt = g_pad.lt; rt = g_pad.rt; if (g_pad.lx || g_pad.ly) { lx = g_pad.lx; ly = g_pad.ly; } rx = g_pad.rx; ry = g_pad.ry; }
    memcpy(buf, &pk, 4); memcpy(buf + 4, &b, 2); buf[6] = lt; buf[7] = rt; memcpy(buf + 8, &lx, 2); memcpy(buf + 10, &ly, 2); memcpy(buf + 12, &rx, 2); memcpy(buf + 14, &ry, 2);
    wrbuf(c, a[1], buf, 16); return 0;
}
H(Direct3DCreate9) { UNUSED; return com_create_direct3d9(c); }
H(DirectInput8Create) { uint32_t o = com_create_directinput8(c); wr32(c, a[3], o); return 0; }

#define E(n, na) { #n, na, 0, h_##n }
extern uint32_t g_api_ret;     // Bink audio entry points: log (BLUR_AUDIOLOG) and answer; see README 'Bink movie audio'
static uint32_t bl_BinkSetSoundSystem(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkSetSoundSystem(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 1; }
static uint32_t bl_BinkOpenDirectSound(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkOpenDirectSound(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 1; }
static uint32_t bl_BinkOpenWaveOut(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkOpenWaveOut(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 1; }
static uint32_t bl_BinkSetSoundTrack(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkSetSoundTrack(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 1; }
static uint32_t bl_BinkSetVolume(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkSetVolume(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 1; }
static uint32_t bl_BinkGetTrackID(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkGetTrackID(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 0; }
static uint32_t bl_BinkOpenTrack(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkOpenTrack(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 0; }
static uint32_t bl_BinkPause(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkPause(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 0; }
static uint32_t bl_BinkClose(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkClose(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 0; }
static uint32_t bl_BinkGetTrackData(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkGetTrackData(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 0; }
static uint32_t bl_BinkCloseTrack(Ctx *c, uint32_t *a) { static int k; static int on = -1; if (on < 0) on = getenv("BLUR_AUDIOLOG") != NULL; if (on && k++ < 60) fprintf(stderr, "[bink] BinkCloseTrack(%08x,%08x,%08x) caller=%08x\n", a[0], a[1], a[2], g_api_ret); (void)c; return 0; }
static uint32_t t_0_(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }
BKR(BinkOpen, h_BinkOpen) BKR(BinkClose, bl_BinkClose) BKR(BinkGetFrameBuffersInfo, h_BinkGetFrameBuffersInfo) BKR(BinkRegisterFrameBuffers, t_0_)
BKR(BinkDoFrame, h_BinkDoFrame) BKR(BinkNextFrame, h_BinkNextFrame) BKR(BinkWait, t_0_) BKR(BinkShouldSkip, t_0_) BKR(BinkPause, bl_BinkPause)
BKR(BinkGetRects, t_1) BKR(BinkSetVolume, bl_BinkSetVolume) BKR(BinkOpenTrack, bl_BinkOpenTrack) BKR(BinkGetTrackData, bl_BinkGetTrackData)
BKR(BinkGetTrackID, bl_BinkGetTrackID) BKR(BinkCloseTrack, bl_BinkCloseTrack)
static const HleDef real[] = {
    E(CreateDirectoryA,2), E(CreateDirectoryW,2), E(CreateFileA,7), E(CreateFileW,7), E(ReadFile,5), E(WriteFile,5), E(SetFilePointer,4), E(GetFileSize,2), E(GetFileAttributesA,1),
    E(FindFirstFileA,2), E(FindNextFileA,2), E(FindClose,1),
    E(GetSystemTime,1), E(GetLocalTime,1), E(GetTimeZoneInformation,1), E(GlobalMemoryStatusEx,1), E(GlobalMemoryStatus,1),
    E(GetCurrentDirectoryA,2), E(GetFullPathNameA,4), E(GetTempPathA,2), E(GetWindowsDirectoryA,2), E(GetSystemDirectoryA,2),
    E(GetComputerNameA,2), E(GetUserNameA,2), E(SHGetFolderPathA,5), E(SHGetSpecialFolderPathA,4), E(GetEnvironmentStrings,0),
    E(lstrlenW,1), E(lstrcmpA,2), E(lstrcmpiA,2), E(CompareStringA,6), E(CompareStringW,6), E(LCMapStringA,6), E(GetStringTypeA,5),
    E(GetClientRect,2), E(GetWindowRect,2), E(GetCursorPos,1), E(GetSystemMetrics,1), E(EnumDisplaySettingsA,3), E(EnumDisplayDevicesA,4), E(MessageBoxA,4),
    E(CryptAcquireContextA,5), E(CryptGenRandom,3), E(CryptCreateHash,5), E(CryptHashData,4), E(CryptGetHashParam,5), E(CryptDestroyHash,1),
    { "WSAStartup", 2, 0, h_WSAStartup }, { "WSACleanup", 0, 0, t_0 }, { "WSAGetLastError", 0, 0, h_WSAGetLastError }, { "WSASetLastError", 1, 0, t_0 },
    { "htons", 1, 0, h_htons }, { "ntohs", 1, 0, h_htons }, { "htonl", 1, 0, h_htonl }, { "ntohl", 1, 0, h_htonl }, { "gethostname", 2, 0, h_gethostname },
    { "gethostbyname", 1, 0, t_0 }, { "socket", 3, 0, t_m1 }, { "closesocket", 1, 0, t_0 }, { "accept", 3, 0, t_m1 }, { "bind", 3, 0, t_m1 },
    { "connect", 3, 0, t_m1 }, { "getpeername", 3, 0, t_m1 }, { "getsockname", 3, 0, t_m1 }, { "getsockopt", 5, 0, t_m1 }, { "inet_addr", 1, 0, t_m1 },
    { "inet_ntoa", 1, 0, t_0 }, { "ioctlsocket", 3, 0, t_m1 }, { "listen", 2, 0, t_m1 }, { "recv", 4, 0, t_m1 }, { "recvfrom", 6, 0, t_m1 },
    { "select", 5, 0, t_m1 }, { "send", 4, 0, t_m1 }, { "sendto", 6, 0, t_m1 }, { "setsockopt", 5, 0, t_m1 }, { "shutdown", 2, 0, t_m1 },
    { "WSAEventSelect", 3, 0, t_m1 }, { "WSAEnumNetworkEvents", 3, 0, t_m1 }, { "WSACloseEvent", 1, 0, t_1 }, { "__WSAFDIsSet", 2, 0, t_0 },
    // Bink video: fake player (opens real files, reports a short black movie so the game's movie states complete)
    { "_BinkPause@8", 2, 0, r_BinkPause }, { "_BinkSetVolume@12", 3, 0, r_BinkSetVolume }, { "_BinkSetIOSize@4", 1, 0, t_0 }, { "_BinkGetTrackID@8", 2, 0, r_BinkGetTrackID },
    { "_BinkClose@4", 1, 0, r_BinkClose }, { "_BinkCloseTrack@4", 1, 0, r_BinkCloseTrack }, { "_BinkRegisterFrameBuffers@8", 2, 0, r_BinkRegisterFrameBuffers }, { "_BinkGetFrameBuffersInfo@8", 2, 0, r_BinkGetFrameBuffersInfo },
    { "_BinkGetTrackData@8", 2, 0, r_BinkGetTrackData }, { "_BinkOpenWaveOut@4", 1, 0, bl_BinkOpenWaveOut }, { "_BinkOpen@8", 2, 0, r_BinkOpen }, { "_BinkSetSoundSystem@8", 2, 0, bl_BinkSetSoundSystem },
    { "_BinkOpenDirectSound@4", 1, 0, bl_BinkOpenDirectSound }, { "_BinkSetSoundTrack@8", 2, 0, bl_BinkSetSoundTrack }, { "_BinkNextFrame@4", 1, 0, r_BinkNextFrame }, { "_BinkShouldSkip@4", 1, 0, r_BinkShouldSkip },
    { "_BinkDoFrame@4", 1, 0, r_BinkDoFrame }, { "_BinkWait@4", 1, 0, r_BinkWait }, { "_BinkGetRects@8", 2, 0, r_BinkGetRects }, { "_BinkOpenTrack@8", 2, 0, r_BinkOpenTrack },
    // DirectX family: defined failures until the graphics/audio/input phases
    { "Direct3DCreate9", 1, 0, h_Direct3DCreate9 }, { "DirectInput8Create", 5, 0, h_DirectInput8Create }, { "XInputGetState", 2, 0, h_XInputGetState }, { "XInputSetState", 2, 0, t_nodev },
    { "DirectSoundCreate", 3, 0, h_DirectSoundCreate8 }, { "DirectSoundCreate8", 3, 0, h_DirectSoundCreate8 }, { "DirectSoundEnumerateA", 2, 0, h_DirectSoundEnumerateA }, { "DirectSoundEnumerateW", 2, 0, t_fail },
    // CryptCreateHash etc. and the rest are in the trivial table below
};
static const HleDef trivial[] = {
#include "trivial.inc"
};
const HleDef *hle2_lookup(const char *name) {
    { const HleDef *d = wsock_lookup(name); if (d) return d; }          // virtual-LAN winsock (wsock_hle.c); only active when BLUR_NET_ROOM / BLUR_NET_ID is set, otherwise the placeholder rows below are used
    { const HleDef *d = win_lookup(name); if (d) return d; }
    { const HleDef *d = d3dx_lookup(name); if (d) return d; }
    for (size_t i = 0; i < sizeof real / sizeof real[0]; i++) if (!strcmp(real[i].name, name)) return &real[i];
    for (size_t i = 0; i < sizeof trivial / sizeof trivial[0]; i++) if (!strcmp(trivial[i].name, name)) return &trivial[i];
    return NULL;
}

// ---- snapshot (see snap_ctx.c): crypto hash contexts hold host pointers and must not exist at the snapshot point ----
int hle2_snap_busy(void) { int n = 0; for (int i = 0; i < MAXHASH; i++) n += hashes[i].used; return n; }
void hle2_snap(void (*cb)(void *, void *, size_t), void *u) { cb(u, &packet, sizeof packet); }
