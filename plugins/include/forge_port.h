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

// ---- the port's own (runtime/forge.c "the port's own calls"); weak: NULL where the plugin runs without the port
uint64_t mhgu_time_ns(void) __attribute__((weak));  // a steady clock, nanoseconds
