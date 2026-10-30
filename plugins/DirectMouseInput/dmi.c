// DirectMouseInput (DMI) as a Forge plugin, mods/DirectMouseInput: the trial build, before the direct mouse moves
// here from the port (runtime/hle/directmouse.c). It hooks every game function the port's direct mouse hooks
// (tools/gen.py PATCHES) with detours that only count their calls, and once a frame walks each GUI that ran and
// measures its visible animation instances, as the menu pointer's search does. Every 600 frames the log gets the
// calls per frame and the time the walk took: whether DMI can run as a plugin, in the port's interpreter.
#include "forge_port.h"

// ---- the game (main v1.4.0; addresses as runtime/hle/directmouse.c has them, main loaded at 0x200000)
#define MAIN_AT 0x200000u
static uintptr_t main_text;
static void *game(uint32_t a) { return (void *)(main_text + (a - MAIN_AT)); }

#define LD32(a) (*(const volatile uint32_t *)(uintptr_t)(a))
static float f32(uint32_t a) { return *(const volatile float *)(uintptr_t)a; }
static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }
static bool inst_ok(uint32_t a) { return sane(a) && LD32(a) - 0x01960000u < 0x00040000u; }  // (a GUI instance)
static bool is_anim(uint32_t a) { return LD32(a) == 0x0198f5b8u || LD32(a) == 0x0196c158u; }

// ---- the hooks: every one passes its core and first VFP arguments on unchanged
enum { H_CURSOR, H_YESNO, H_ROW, H_COL, H_CELL, H_SETX, H_SETY, H_SETXY, H_SEQ, H_GUI, H_BOX, H_CAMERA, NHOOKS };
static const struct { uint32_t at; const char *name; } sites[NHOOKS] = {
    {0x74ee98, "cursor update"}, {0x74fbb0, "yes/no"}, {0x77033c, "highlight row"}, {0x770378, "highlight column"},
    {0x7703b4, "highlight cell"}, {0x745a40, "setX"}, {0x745ac0, "setY"}, {0x745c20, "setXY"},
    {0x746c74, "setSequence"}, {0x744b88, "GUI move"}, {0x7530b8, "box cursor update"}, {0x21fec4, "quest camera"},
};
static uint32_t calls[NHOOKS];

typedef uint64_t GAME_FN Fn4(uint32_t, uint32_t, uint32_t, uint32_t, float, float);
typedef uint64_t GAME_FN Fn6(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, float, float);
static void *orig[NHOOKS];

#define DETOUR4(i)                                                                                          \
    static uint64_t GAME_FN d##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, float s0, float s1) {      \
        calls[i]++;                                                                                         \
        return ((Fn4 *)orig[i])(a, b, c, d, s0, s1);                                                        \
    }
DETOUR4(0) DETOUR4(1) DETOUR4(2) DETOUR4(3) DETOUR4(5) DETOUR4(6) DETOUR4(7) DETOUR4(8) DETOUR4(10) DETOUR4(11)

// the highlight cell (column and row on the stack)
static uint64_t GAME_FN d4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, float s0, float s1) {
    calls[H_CELL]++;
    return ((Fn6 *)orig[H_CELL])(a, b, c, d, e, f, s0, s1);
}

// a GUI's move, once a frame each: the GUIs that ran this frame
#define NLIVE 256
static uint32_t live[NLIVE];
static int nlive;
static uint64_t GAME_FN d9(uint32_t a, uint32_t b, uint32_t c, uint32_t d, float s0, float s1) {
    calls[H_GUI]++;
    if (nlive < NLIVE) live[nlive++] = a;
    return ((Fn4 *)orig[H_GUI])(a, b, c, d, s0, s1);
}

static void *const detours[NHOOKS] = {d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11};

// ---- the walk: each GUI that ran and shows, each visible animation instance of it, the visible pieces it draws
static uint32_t insts, anims, objs;
static float span;  // (what the boxes add up to: keeps the work from being optimised away)

static void inst_box(uint32_t inst) {
    uint32_t root = LD32(inst + 0xdc), stack[64];
    int sp = 0, seen = 0;
    if (!sane(root)) return;
    if (inst_ok(LD32(root + 0x68))) stack[sp++] = LD32(root + 0x68);
    float x0 = 1e30f, x1 = -1e30f;
    while (sp && seen++ < 512) {
        uint32_t o = stack[--sp], next = LD32(o + 0x6c), first = LD32(o + 0x68);
        if (inst_ok(next) && sp < 64) stack[sp++] = next;
        if (!(LD32(o + 0x54) & 1)) continue;
        if (inst_ok(first) && sp < 64) stack[sp++] = first;
        objs++;
        float x = f32(o + 0x40), w = f32(o + 0xb4) * f32(o + 0x10);
        if (x < x0) x0 = x;
        if (x + w > x1) x1 = x + w;
    }
    if (x1 > x0) span += x1 - x0;
}

static void walk(void) {
    for (int j = 0; j < nlive; j++) {
        uint32_t own = live[j], root = LD32(own + 0xf4), n = LD32(own + 0x298), arr = LD32(own + 0xf8);
        if (!inst_ok(root) || !(LD32(root + 0x54) & 1) || !sane(arr) || n > 4096) continue;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = LD32(arr + 4 * i);
            insts++;
            if (!inst_ok(a) || !is_anim(a) || !(LD32(a + 0x54) & 1)) continue;
            anims++;
            inst_box(a);
        }
    }
    nlive = 0;
}

// ---- Forge's entry points
void forge_onLoad(ForgeInitParam *param) {
    param->major = 1, param->minor = 0, param->patch = 0;
    const char *name = "DirectMouseInput (trial)";
    for (int i = 0; name[i] && i < 63; i++) param->name[i] = name[i], param->name[i + 1] = 0;
}

void forge_onInit(void) {
    main_text = forge_mem_getMainTextAddr();
    for (int i = 0; i < NHOOKS; i++) {
        ForgeHook h = forge_hook_create(game(sites[i].at), detours[i], &orig[i]);
        if (!h.original) forge_log(FORGE_LOG_WARN, "DMI trial: %s at %08x not hooked", sites[i].name, sites[i].at);
    }
    forge_log(FORGE_LOG_INFO, "DMI trial: %d hooks, main text at %08x", NHOOKS, (unsigned)main_text);
}

void forge_onUpdate(float dt) {
    static uint32_t frames;
    static uint64_t ns, worst;
    (void)dt;
    uint64_t t0 = mhgu_time_ns ? mhgu_time_ns() : 0;
    walk();
    uint64_t t = mhgu_time_ns ? mhgu_time_ns() - t0 : 0;
    ns += t;
    if (t > worst) worst = t;
    if (++frames < 600) return;
    forge_log(FORGE_LOG_INFO, "DMI trial, a frame on average over %u: walk %u.%03u ms (worst %u.%03u ms), %u GUI "
              "instances, %u visible animations, %u pieces", frames, (unsigned)(ns / frames / 1000000),
              (unsigned)(ns / frames / 1000 % 1000), (unsigned)(worst / 1000000), (unsigned)(worst / 1000 % 1000),
              insts / frames, anims / frames, objs / frames);
    for (int i = 0; i < NHOOKS; i++)
        if (calls[i]) forge_log(FORGE_LOG_INFO, "DMI trial:   %-20s %u.%02u calls a frame", sites[i].name,
                                calls[i] / frames, calls[i] * 100 / frames % 100);
    for (int i = 0; i < NHOOKS; i++) calls[i] = 0;
    frames = 0, ns = worst = 0, insts = anims = objs = 0;
    if (span < 0) forge_log(FORGE_LOG_DEBUG, "%d", (int)span);
}
