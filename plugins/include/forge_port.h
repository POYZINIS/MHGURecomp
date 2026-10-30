// Forge's plugin API as this port implements it (runtime/forge.c), for the plugins in plugins/ (built by `make
// plugins`): C declarations of the calls they use, and the port's own calls (mhgu_*, not on a console). A plugin is
// AArch32 ARM code, softfp as Forge's: floats in core registers between plugin functions. The game's own functions
// take floats in VFP registers: a detour or a pointer to one of them is GAME_FN.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FORGE_EXPORT __attribute__((visibility("default"), used))
#define GAME_FN      __attribute__((pcs("aapcs-vfp")))

// forge_onLoad(ForgeInitParam *) fills it: the Forge version the plugin needs and its name
typedef struct {
    uint16_t major, minor, patch;
    char name[64];
} ForgeInitParam;

// A hook (24 bytes, as Forge's): original is what calls the function as it was
typedef struct {
    void *target, *detour, *original, *context, *context_ptr;
    bool has_context;
} ForgeHook;

enum { FORGE_LOG_DEBUG, FORGE_LOG_INFO, FORGE_LOG_WARN, FORGE_LOG_ERROR };

// the plugin's entry points, called by the port: forge_onLoad once (fills param), forge_onInit once the game's
// singletons exist, forge_onUpdate(dt) after each frame's update
FORGE_EXPORT void forge_onLoad(ForgeInitParam *param);
FORGE_EXPORT void forge_onInit(void);
FORGE_EXPORT void forge_onUpdate(float dt);

ForgeHook forge_hook_create(void *target, void *detour, void **original);
uintptr_t forge_mem_getMainTextAddr(void);
void forge_log(int level, const char *fmt, ...);
bool forge_input_isKeyPressed(uint32_t key);  // a key pressed this frame (SDL's scancodes: F6 is 63)

// ---- the port's own (runtime/forge.c "the port's own calls"); weak: NULL where the plugin runs without the port
uint64_t mhgu_time_ns(void) __attribute__((weak));  // a steady clock, nanoseconds
uint32_t mhgu_frame(void) __attribute__((weak));    // frames presented so far
void mhgu_log(const char *fmt, ...) __attribute__((weak));  // a line of the port's log, as it writes its own

// The mouse, for a plugin that takes it over (runtime/host_input.c). Each frame the plugin says what it is for:
// MHGU_MOUSE_POINTER a free pointer whose left / right clicks are the plugin's (not the buttons bound to them),
// MHGU_MOUSE_CAPTURE captured (the window takes it; Escape lets it go, a click takes it again), its movement the
// plugin's, MHGU_MOUSE_PORT the port's own (mouse look as the right stick, the bound buttons). Never said: the port's.
enum { MHGU_MOUSE_PORT, MHGU_MOUSE_POINTER, MHGU_MOUSE_CAPTURE };
enum { MHGU_MOUSE_LEFT = 1, MHGU_MOUSE_RIGHT = 2 };
typedef struct {
    float x, y, w, h;  // the pointer on the game's 2D canvas (menus' coordinates, y down), the canvas' size
    float dx, dy;      // movement since the last call (mouse counts)
    uint32_t moves;    // pointer moves so far (a change: it moved)
    uint8_t inside, held, clicks;  // over the window; MHGU_MOUSE_* held, pressed since the last call
    int8_t wheel;      // wheel notches since the last call (up: +)
} MhguMouse;
void mhgu_mouse_get(MhguMouse *out) __attribute__((weak));
void mhgu_mouse_mode(int mode) __attribute__((weak));
// Switch buttons pressed for that many frames from the next (bit 0 A, 1 B, 2 X, 3 Y, 4 / 5 the stick clicks, 6 L,
// 7 R, 8 ZL, 9 ZR, 10 Plus, 11 Minus, 12-15 d-pad left, up, right, down)
void mhgu_pad_press(uint32_t buttons, uint32_t frames) __attribute__((weak));
// a setting as mhgurecomp.ini has it now (section and key in any case; the value without its comment): false if missing
bool mhgu_ini_get(const char *section, const char *key, char *out, uint32_t size) __attribute__((weak));
bool mhgu_env(const char *name, char *out, uint32_t size) __attribute__((weak));  // an environment variable
// the control file's (MHGU_CONTROL) next line for the plugins - a command the port doesn't know - after seq: its
// seq, 0 when there is none
uint32_t mhgu_control_next(uint32_t seq, char *out, uint32_t size) __attribute__((weak));
