// Window classes, windows, and a message queue. Handlers that deliver messages call the game's WndProc through the
// dispatcher's CallReq mechanism (the WndProc runs as ordinary guest code on the calling thread).
#include "emu.h"

#define MAXWND 64
#define MAXMSG 256
typedef struct { char cls[64]; uint32_t proc, atom; } WClass;
typedef struct { int used; uint32_t proc, userdata, style, exstyle; char title[64]; } Wnd;
typedef struct { uint32_t hwnd, msg, wp, lp; } Msg;
static WClass classes[32]; static int nclasses;
static Wnd wnds[MAXWND];
static Msg q[MAXMSG]; static int qh, qt;
static uint32_t fg_hwnd, focus_hwnd;

static Wnd *W(uint32_t h) { if (h < 0x10001 || ((h - 0x10001) & 3)) return NULL; uint32_t i = (h - 0x10001) / 4; return i < MAXWND && wnds[i].used ? &wnds[i] : NULL; }
void win_post(Ctx *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { (void)c; if (((qt + 1) % MAXMSG) != qh) { q[qt] = (Msg){hwnd, msg, wp, lp}; qt = (qt + 1) % MAXMSG; } }

uint32_t cont_stub_addr(Ctx *c);
#ifdef __EMSCRIPTEN__
static uint32_t cont_handler(Ctx *c, uint32_t *unused) { (void)c; (void)unused; return 0; }      // the recompiled runtime handles this stub itself (rt_wasm.c)
#else
static uint32_t cont_handler(Ctx *c, uint32_t *unused) {          // WndProc returned here; restore the caller of the original API
    (void)unused; Thread *t = cur_thread(c); Cont k = t->cont[--t->ncont]; uint32_t eax; uc_reg_read(c->uc, UC_X86_REG_EAX, &eax);
    if (!k.use_result) eax = k.value; uc_reg_write(c->uc, UC_X86_REG_EAX, &eax); uc_reg_write(c->uc, UC_X86_REG_EIP, &k.real_ret); c->api_progress++; return 0;
}
#endif
static uint32_t cont_addr_s;
uint32_t cont_stub_addr(Ctx *c) { uint32_t a = cont_addr_s; if (!a) { a = cont_addr_s = make_stub(c, "host", "__continuation", 0, cont_handler); c->stubs[(a - STUB_BASE) / 16].raw = 1; } return a; }

static void call_wndproc(Ctx *c, uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, int cont, uint32_t cont_value) {
    c->req = (CallReq){ 1, proc, 4, {hwnd, msg, wp, lp}, cont, cont ? 0 : 1, cont_value };
}

#define H(n) static uint32_t h_##n(Ctx *c, uint32_t *a)
static uint32_t find_class(Ctx *c, uint32_t namep, char *out) {
    if (namep < 0x10000) { for (int i = 0; i < nclasses; i++) if (classes[i].atom == namep) { snprintf(out, 64, "%s", classes[i].cls); return (uint32_t)i + 1; } return 0; }
    rd_cstr(c, namep, out, 64); for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].cls, out)) return (uint32_t)i + 1; return 0;
}
static uint32_t register_class(Ctx *c, uint32_t proc, uint32_t namep) {
    WClass *k = &classes[nclasses]; rd_cstr(c, namep, k->cls, sizeof k->cls); k->proc = proc; k->atom = 0xc000 + (uint32_t)nclasses; nclasses++;
    if (c->verbose) fprintf(stderr, "[win] RegisterClass %s wndproc=%08x\n", k->cls, proc); return classes[nclasses - 1].atom;
}
H(RegisterClassA) { return register_class(c, rd32(c, a[0] + 4), rd32(c, a[0] + 36)); }
H(RegisterClassExA) { return register_class(c, rd32(c, a[0] + 8), rd32(c, a[0] + 40)); }
H(CreateWindowExA) {     // exstyle, class, title, style, x, y, w, h, parent, menu, hinst, param
    char cn[64]; uint32_t ci = find_class(c, a[1], cn); int idx = -1;
    for (int i = 0; i < MAXWND; i++) if (!wnds[i].used) { idx = i; break; }
    if (idx < 0) return 0;
    Wnd *w = &wnds[idx]; memset(w, 0, sizeof *w); w->used = 1; w->proc = ci ? classes[ci - 1].proc : 0; w->style = a[3]; w->exstyle = a[0];
    if (a[2] >= 0x10000) rd_cstr(c, a[2], w->title, sizeof w->title);
    uint32_t hwnd = 0x10001 + 4 * (uint32_t)idx;
    if (c->verbose) fprintf(stderr, "[win] CreateWindow class=%s title=\"%s\" %ux%u -> %08x wndproc=%08x\n", cn, w->title, a[6], a[7], hwnd, w->proc);
    if (!fg_hwnd) fg_hwnd = focus_hwnd = hwnd;
    if (a[3] & 0x10000000) {                                                  // WS_VISIBLE: Windows shows + activates the window during creation
        win_post(c, hwnd, 0x0018, 1, 0); win_post(c, hwnd, 0x001C, 1, 0x1004); win_post(c, hwnd, 0x0006, 1, 0); win_post(c, hwnd, 0x0007, 0, 0);
        win_post(c, hwnd, 0x0005, 0, 1280 | (720 << 16)); }
    if (!w->proc) return hwnd;
    uint32_t cs = guest_alloc(c, 48); uint32_t v[12] = {a[11], a[10], a[9], a[8], a[7], a[6], a[5], a[4], a[3], a[2], a[1], a[0]}; wrbuf(c, cs, v, 48);
    call_wndproc(c, w->proc, hwnd, 0x0001 /*WM_CREATE*/, 0, cs, 1, hwnd);       // returns hwnd to the caller afterwards
    return hwnd;
}
H(CreateWindowExW) { return h_CreateWindowExA(c, a); }
H(DefWindowProcA) { (void)c; return a[1] == 0x0081 ? 1 : 0; }                                            // WM_NCCREATE -> TRUE
H(DestroyWindow) { Wnd *w = W(a[0]); if (w) w->used = 0; return 1; }
H(ShowWindow) {
    Wnd *w = W(a[0]); if (w && a[1]) { win_post(c, a[0], 0x0018 /*WM_SHOWWINDOW*/, 1, 0); win_post(c, a[0], 0x001C /*WM_ACTIVATEAPP*/, 1, 0x1004);
        win_post(c, a[0], 0x0006 /*WM_ACTIVATE*/, 1, 0); win_post(c, a[0], 0x0007 /*WM_SETFOCUS*/, 0, 0); win_post(c, a[0], 0x0005 /*WM_SIZE*/, 0, 1280 | (720 << 16)); }
    return 1;
}
H(SetFocus) { uint32_t old = focus_hwnd; focus_hwnd = a[0]; return old; }
H(SetForegroundWindow) { fg_hwnd = a[0]; return 1; }
H(GetForegroundWindow) { (void)a; (void)c; return fg_hwnd; }
H(GetFocus) { (void)a; (void)c; return focus_hwnd; }
H(IsWindow) { (void)c; return W(a[0]) != NULL; }
H(GetWindowLongA) { Wnd *w = W(a[0]); if (!w) return 0; int32_t i = (int32_t)a[1]; return i == -4 ? w->proc : i == -21 ? w->userdata : i == -16 ? w->style : i == -20 ? w->exstyle : 0; }
H(SetWindowLongA) {
    Wnd *w = W(a[0]); if (!w) return 0; int32_t i = (int32_t)a[1]; uint32_t old = 0; (void)c;
    if (i == -4) { old = w->proc; w->proc = a[2]; } else if (i == -21) { old = w->userdata; w->userdata = a[2]; } else if (i == -16) { old = w->style; w->style = a[2]; } else if (i == -20) { old = w->exstyle; w->exstyle = a[2]; }
    return old;
}
static void fill_msg(Ctx *c, uint32_t p, Msg *m) { wr32(c, p, m->hwnd); wr32(c, p + 4, m->msg); wr32(c, p + 8, m->wp); wr32(c, p + 12, m->lp); wr32(c, p + 16, 0); wr32(c, p + 20, 640); wr32(c, p + 24, 360); }
H(PeekMessageA) {        // msg, hwnd, min, max, remove
    if (qh == qt) return 0;
    fill_msg(c, a[0], &q[qh]); if (a[4] & 1) qh = (qh + 1) % MAXMSG; return 1;
}
H(GetMessageA) { if (qh == qt) { c->block = 1; return 0; } fill_msg(c, a[0], &q[qh]); Msg m = q[qh]; qh = (qh + 1) % MAXMSG; return m.msg != 0x0012; }
H(DispatchMessageA) {
    uint32_t hwnd = rd32(c, a[0]), msg = rd32(c, a[0] + 4), wp = rd32(c, a[0] + 8), lp = rd32(c, a[0] + 12); Wnd *w = W(hwnd);
    if (!w || !w->proc) return 0; call_wndproc(c, w->proc, hwnd, msg, wp, lp, 0, 0); return 0;
}
H(SendMessageA) { Wnd *w = W(a[0]); if (!w || !w->proc) return 0; call_wndproc(c, w->proc, a[0], a[1], a[2], a[3], 0, 0); return 0; }
H(PostQuitMessage) { win_post(c, 0, 0x0012, a[0], 0); return 0; }
H(TranslateMessage) { (void)c; (void)a; return 0; }
H(GetWindowThreadProcessId) { (void)c; if (a[1]) wr32(c, a[1], 0x1000); return 0x1004; }

#define E(n, na) { #n, na, 0, h_##n }
static const HleDef tbl[] = { E(RegisterClassA,1), E(RegisterClassExA,1), E(CreateWindowExA,12), E(CreateWindowExW,12), E(DefWindowProcA,4), E(DestroyWindow,1), E(ShowWindow,2),
    E(SetFocus,1), E(SetForegroundWindow,1), E(GetForegroundWindow,0), E(GetFocus,0), E(IsWindow,1), E(GetWindowLongA,2), E(SetWindowLongA,3), E(PeekMessageA,5),
    E(GetMessageA,4), E(DispatchMessageA,1), E(SendMessageA,4), E(PostQuitMessage,1), E(TranslateMessage,1), E(GetWindowThreadProcessId,2) };
const HleDef *win_lookup(const char *name) { for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) if (!strcmp(tbl[i].name, name)) return &tbl[i]; return NULL; }

// ---- snapshot (see snap_ctx.c) ----------------------------------------------------------------------------------
void win_snap(void (*cb)(void *, void *, size_t), void *u) {
    cb(u, classes, sizeof classes); cb(u, &nclasses, sizeof nclasses); cb(u, wnds, sizeof wnds); cb(u, q, sizeof q); cb(u, &qh, sizeof qh); cb(u, &qt, sizeof qt);
    cb(u, &fg_hwnd, 4); cb(u, &focus_hwnd, 4); cb(u, &cont_addr_s, 4);
}
