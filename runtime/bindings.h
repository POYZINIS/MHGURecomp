// Controls (runtime/bindings.c): the keys, mouse buttons and controller buttons that press the Switch controller's
// buttons and push its sticks. mhgurecomp.ini [Keyboard] / [Controller]; the settings menu (runtime/menu.c) changes
// them, runtime/host_input.c reads them.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Actions: the Switch buttons (hle/input.h BTN_*: 0..15), then the sticks pushed by keys.
enum {
    ACT_MOVE_UP = 16, ACT_MOVE_DOWN, ACT_MOVE_LEFT, ACT_MOVE_RIGHT, ACT_WALK,  // left stick (Walk held: half tilt)
    ACT_CAM_UP, ACT_CAM_DOWN, ACT_CAM_LEFT, ACT_CAM_RIGHT,                     // right stick
    ACT_COUNT
};
// A keyboard / mouse input: an SDL scancode (1..511), a mouse button, a turn of the wheel. 0: none.
#define KEY_MOUSE 0x200  // + SDL_BUTTON_LEFT (1) .. SDL_BUTTON_X2 (5)
#define KEY_WHEEL_UP 0x210
#define KEY_WHEEL_DOWN 0x211
#define KEY_SLOTS 2      // keys per action
// A controller input: an SDL_GamepadButton or a trigger. -1: the default ([Controls] FaceButtons' layout).
#define PAD_LT 0x40
#define PAD_RT 0x41

typedef struct {
    uint16_t key[ACT_COUNT][KEY_SLOTS];
    int8_t pad[16];
} Bindings;
extern Bindings g_bind;

void bindings_defaults(Bindings *b);
// config.c: a line of the [Keyboard] (keyboard true) or [Controller] section; false: not a key these sections have.
bool bindings_parse(bool keyboard, const char *key, const char *val, int line);
void bindings_put(void);  // both sections into the settings file being edited (config_edit_begin .. end)

const char *bind_action_name(int act);              // as the ini names it: "A", "DpadUp", "MoveUp"...
void bind_key_label(int key, char *out, size_t n);  // for people: the key's label on this keyboard layout
// key to act's slot. A key bound to another action moves here: *from is that action, else -1.
void bind_set_key(int act, int slot, int key, int *from);
bool bind_key_reserved(int key);  // ` (the settings menu), Esc (lets the mouse go), F6-F12 (the port's own keys)

struct SDL_Gamepad;
int bind_pad_code(int btn, struct SDL_Gamepad *pad);  // the controller input that presses Switch button btn
bool bind_pad_down(struct SDL_Gamepad *pad, int code);
void bind_pad_label(int code, struct SDL_Gamepad *pad, char *out, size_t n);  // for people: "Bottom (A)", "LB"...
int bind_pad_held(struct SDL_Gamepad *pad);  // a controller input held now, else -1 (rebinding)
