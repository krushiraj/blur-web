// Minimal COM runtime for emulated DirectX interfaces. An object is [vtable ptr][refcount][user data...] in guest memory;
// every vtable slot is a stub whose handler receives `this` as a[0] (all COM methods are stdcall).
#include "emu.h"

uint32_t com_ok(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t com_addref(Ctx *c, uint32_t *a) { uint32_t n = rd32(c, a[0] + 4) + 1; wr32(c, a[0] + 4, n); return n; }
static uint32_t com_release(Ctx *c, uint32_t *a) { uint32_t n = rd32(c, a[0] + 4); if (n) n--; wr32(c, a[0] + 4, n); return n; }
static uint32_t com_qi(Ctx *c, uint32_t *a) {            // every object answers every interface with itself
    if (a[2]) wr32(c, a[2], a[0]);
    wr32(c, a[0] + 4, rd32(c, a[0] + 4) + 1); return 0;
}

uint32_t com_make(Ctx *c, const char *iface, const ComMethod *m, int n, uint32_t extra) {
    uint32_t vt = guest_alloc(c, 4 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        char nm[128]; snprintf(nm, sizeof nm, "%s::%s", iface, m[i].name);
        hle_fn fn = m[i].fn; int na = m[i].nargs;
        if (fn != com_ok && fn) { /* explicit IUnknown override (audio.c) */ }
        else if (!strcmp(m[i].name, "QueryInterface")) { fn = com_qi; na = 3; }
        else if (!strcmp(m[i].name, "AddRef")) { fn = com_addref; na = 1; }
        else if (!strcmp(m[i].name, "Release")) { fn = com_release; na = 1; }
        wr32(c, vt + 4 * i, make_stub(c, "com", nm, na, fn));
    }
    uint32_t obj = guest_alloc(c, 8 + extra); wr32(c, obj, vt); wr32(c, obj + 4, 1);
    return obj;
}

// ---- DirectInput8 -----------------------------------------------------------------------------------------------------------
#define OK(n, na) { n, na, com_ok }
#define IUNK OK("QueryInterface", 3), OK("AddRef", 1), OK("Release", 1)

uint8_t g_keys[256];
PadState g_pad; MouseState g_mouse;                       // DirectInput scancode state (0x80 = down), fed by browser input
static const struct { int vk, dik; } vkmap[] = {{13,0x1c},{27,0x01},{32,0x39},{9,0x0f},{8,0x0e},{37,0xcb},{38,0xc8},{39,0xcd},{40,0xd0},{160,0x2a},{162,0x1d},{164,0x38},
    {'A',0x1e},{'B',0x30},{'C',0x2e},{'D',0x20},{'E',0x12},{'F',0x21},{'G',0x22},{'H',0x23},{'I',0x17},{'J',0x24},{'K',0x25},{'L',0x26},{'M',0x32},{'N',0x31},{'O',0x18},{'P',0x19},{'Q',0x10},{'R',0x13},{'S',0x1f},{'T',0x14},{'U',0x16},{'V',0x2f},{'W',0x11},{'X',0x2d},{'Y',0x15},{'Z',0x2c},
    {'1',2},{'2',3},{'3',4},{'4',5},{'5',6},{'6',7},{'7',8},{'8',9},{'9',10},{'0',11}};
void input_pump(Ctx *c) {                  // drain browser input into g_keys; BLUR_AUTOPRESS=1 taps Enter/Space periodically (testing)
    (void)c; NetInput e; while (net_poll_input(&e)) {
        if (e.type == 6) { g_pad.active = e.d; g_pad.btn = (uint16_t)e.a; g_pad.lt = (uint8_t)(e.a >> 16); g_pad.rt = (uint8_t)(e.a >> 24); g_pad.lx = (int16_t)e.b; g_pad.ly = (int16_t)(e.b >> 16); g_pad.rx = (int16_t)e.c; g_pad.ry = (int16_t)(e.c >> 16); }     // Gamepad API -> XInput state
        else if (e.type == 7) { g_mouse.dx += e.a; g_mouse.dy += e.b; }                                    // pointer-lock mouse deltas
        else if (e.type == 4) { if (e.a >= 0 && e.a < 8) g_mouse.btn[e.a] = e.b ? 0x80 : 0; }
        else if (e.type == 5) g_mouse.dz += e.a;
        if (e.type == 1 || e.type == 2) for (size_t i = 0; i < sizeof vkmap / sizeof vkmap[0]; i++) if (vkmap[i].vk == e.a) g_keys[vkmap[i].dik & 0xff] = e.type == 1 ? 0x80 : 0; }
    { static int init, n; static struct { uint8_t dik; double at; } ev[256]; static double t0;     // BLUR_KEYS="Enter@30,Down@35": scripted key taps (seconds after the first poll)
      if (!init) { init = 1; const char *k = getenv("BLUR_KEYS"); t0 = (double)real_us() / 1e6;
        static const struct { const char *name; uint8_t dik; } nm[] = {{"Enter", 0x1c}, {"Space", 0x39}, {"Up", 0xc8}, {"Down", 0xd0}, {"Left", 0xcb}, {"Right", 0xcd}, {"Escape", 0x01}, {"Tab", 0x0f}, {"Backspace", 0x0e}, {"W", 0x11}, {"A", 0x1e}, {"S", 0x1f}, {"D", 0x20}};
        while (k && *k && n < 256) { char name[32] = {0}; double at = 0; int used = 0; if (sscanf(k, "%31[^@]@%lf%n", name, &at, &used) < 2) break; for (size_t i = 0; i < sizeof nm / sizeof nm[0]; i++) if (!strcmp(nm[i].name, name)) { ev[n].dik = nm[i].dik; ev[n].at = at; n++; } k += used; if (*k == ',') k++; } }
      double now = (double)real_us() / 1e6 - t0; for (int i = 0; i < n; i++) { uint8_t d = (now >= ev[i].at && now < ev[i].at + 0.15) ? 0x80 : 0; if (d || now >= ev[i].at + 0.15) { if (d) g_keys[ev[i].dik] = 0x80; else if (now < ev[i].at + 0.5) g_keys[ev[i].dik] = 0; } } }
    if (getenv("BLUR_AUTOPRESS")) { static uint64_t n; n++; uint8_t d = ((n / 400) % 4 == 1) ? 0x80 : 0; g_keys[0x1c] = d; g_keys[0x39] = d; }
}
static uint32_t dev_getstate(Ctx *c, uint32_t *a) { input_pump(c);
    if (a[1] == 16 || a[1] == 20) {                                  // DIMOUSESTATE / DIMOUSESTATE2: lX, lY, lZ, buttons
        uint8_t m[20] = {0}; int32_t d[3] = { g_mouse.dx, g_mouse.dy, g_mouse.dz }; memcpy(m, d, 12); memcpy(m + 12, g_mouse.btn, a[1] - 12 > 8 ? 8 : a[1] - 12);
        g_mouse.dx = g_mouse.dy = g_mouse.dz = 0; wrbuf(c, a[2], m, a[1]); return 0; }
    for (uint32_t i = 0; i < a[1]; i++) wr8(c, a[2] + i, a[1] == 256 ? g_keys[i] : 0); return 0; }
static uint32_t dev_getdata(Ctx *c, uint32_t *a) { if (a[3]) wr32(c, a[3], 0); return 0; }                                    // no buffered events
static uint32_t dev_enumobjects(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }

static const ComMethod dev_methods[] = {
    IUNK, OK("GetCapabilities", 2), { "EnumObjects", 4, dev_enumobjects }, OK("GetProperty", 3), OK("SetProperty", 3), OK("Acquire", 1), OK("Unacquire", 1),
    { "GetDeviceState", 3, dev_getstate }, { "GetDeviceData", 5, dev_getdata }, OK("SetDataFormat", 2), OK("SetEventNotification", 2), OK("SetCooperativeLevel", 3),
    OK("GetObjectInfo", 4), OK("GetDeviceInfo", 2), OK("RunControlPanel", 3), OK("Initialize", 4), OK("CreateEffect", 4), OK("EnumEffects", 4), OK("GetEffectInfo", 3),
    OK("GetForceFeedbackState", 2), OK("SendForceFeedbackCommand", 2), OK("EnumCreatedEffectObjects", 3), OK("Escape", 2), OK("Poll", 1), OK("SendDeviceData", 4),
    OK("EnumEffectsInFile", 5), OK("WriteEffectToFile", 4), OK("BuildActionMap", 4), OK("SetActionMap", 4), OK("GetImageInfo", 2),
};
static uint32_t di_createdevice(Ctx *c, uint32_t *a) {
    uint32_t o = com_make(c, "IDirectInputDevice8A", dev_methods, sizeof dev_methods / sizeof dev_methods[0], 0);
    if (c->verbose) fprintf(stderr, "[dinput] CreateDevice -> %08x\n", o);
    wr32(c, a[2], o); return 0;
}
static const ComMethod di_methods[] = {
    IUNK, { "CreateDevice", 4, di_createdevice }, OK("EnumDevices", 5), OK("GetDeviceStatus", 2), OK("RunControlPanel", 3), OK("Initialize", 3),
    OK("FindDevice", 4), OK("EnumDevicesBySemantics", 6), OK("ConfigureDevices", 5),
};
uint32_t com_create_directinput8(Ctx *c) { return com_make(c, "IDirectInput8A", di_methods, sizeof di_methods / sizeof di_methods[0], 0); }

// ---- snapshot (see snap_ctx.c) ----------------------------------------------------------------------------------
void com_snap(void (*cb)(void *, void *, size_t), void *u) { cb(u, g_keys, sizeof g_keys); }
