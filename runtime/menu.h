// The settings menu (runtime/menu.c): the ` key (below Esc) opens it over the game. Controls (keys, mouse buttons,
// controller buttons) and mhgurecomp.ini's settings, saved as they change.
#pragma once
#include <stdbool.h>
#include <stdint.h>

union SDL_Event;
struct SDL_Gamepad;

// Host main thread.
bool menu_event(const union SDL_Event *e);  // true: the menu took it (` toggles it; open, keyboard and mouse are its)
void menu_update(struct SDL_Gamepad *pad);  // every loop pass: the controller, the layout, saving
bool menu_is_open(void);
bool menu_wants_text(void);  // a text field is being edited (SDL's text input on)

// What the render thread draws (runtime/nvn/vulkan/menu_vk.c): quads in window pixels, each a rounded rectangle with
// an optional border, a soft shadow, or a glyph from the atlas. 6 vec4s: the shader's layout (std430).
enum { MENU_PRIM_RECT, MENU_PRIM_TEXT, MENU_PRIM_SHADOW };
typedef struct {
    float rect[4];          // x0, y0, x1, y1
    float radius[4];        // corners: top left, top right, bottom right, bottom left
    float color[4];         // RGBA 0..1 (fill; text: the glyph's colour)
    float border_color[4];
    float border, soft, mode, gamma;  // border width; shadow blur; MENU_PRIM_*; text: coverage exponent
    float uv[4];            // text: the glyph's place in the atlas (pixels)
} MenuPrim;
typedef struct { uint32_t first, count; int32_t clip[4]; } MenuBatch;  // clip: x0, y0, x1, y1
typedef struct {
    const MenuPrim *prims;
    uint32_t nprims;
    const MenuBatch *batches;
    uint32_t nbatches;
    const uint8_t *atlas;  // 8-bit coverage, atlas_w x atlas_h
    uint32_t atlas_w, atlas_h;
    uint64_t atlas_gen;    // changes whenever the atlas does
} MenuFrame;
// Render thread: the last finished frame, NULL when the menu shows nothing. Held until menu_frame_done.
const MenuFrame *menu_frame_take(void);
void menu_frame_done(void);
