// Controls (bindings.h): every Switch button and stick direction takes up to two keys or mouse buttons, and every
// Switch button one controller input. The defaults are the port's original layout (README.md, Controls): WASD moves,
// left click X, right click A, Space B... The controller's face buttons follow [Controls] FaceButtons unless a button
// is set in [Controller].
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "bindings.h"
#include "config.h"
#include "hle/input.h"
#include "rt.h"

Bindings g_bind;

static const char *const act_names[ACT_COUNT] = {
    "A", "B", "X", "Y", "LStick", "RStick", "L", "R", "ZL", "ZR", "Plus", "Minus", "DpadLeft", "DpadUp", "DpadRight",
    "DpadDown", "MoveUp", "MoveDown", "MoveLeft", "MoveRight", "Walk", "CameraUp", "CameraDown", "CameraLeft",
    "CameraRight"};

void bindings_defaults(Bindings *b) {
    static const struct { int act; uint16_t k0, k1; } d[] = {
        {BTN_A, KEY_MOUSE + SDL_BUTTON_RIGHT}, {BTN_B, SDL_SCANCODE_SPACE}, {BTN_X, KEY_MOUSE + SDL_BUTTON_LEFT},
        {BTN_Y, SDL_SCANCODE_R}, {BTN_L, SDL_SCANCODE_Q}, {BTN_R, SDL_SCANCODE_E}, {BTN_ZL, SDL_SCANCODE_TAB},
        {BTN_ZR, SDL_SCANCODE_T}, {BTN_PLUS, SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER},
        {BTN_MINUS, SDL_SCANCODE_BACKSPACE}, {BTN_LEFT, SDL_SCANCODE_LEFT}, {BTN_UP, SDL_SCANCODE_UP},
        {BTN_RIGHT, SDL_SCANCODE_RIGHT}, {BTN_DOWN, SDL_SCANCODE_DOWN}, {BTN_LSTICK, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL},
        {BTN_RSTICK, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT}, {ACT_MOVE_UP, SDL_SCANCODE_W}, {ACT_MOVE_DOWN, SDL_SCANCODE_S},
        {ACT_MOVE_LEFT, SDL_SCANCODE_A}, {ACT_MOVE_RIGHT, SDL_SCANCODE_D}, {ACT_WALK, SDL_SCANCODE_LSHIFT},
    };
    memset(b, 0, sizeof *b);
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++) b->key[d[i].act][0] = d[i].k0, b->key[d[i].act][1] = d[i].k1;
    memset(b->pad, -1, sizeof b->pad);
}

const char *bind_action_name(int act) { return act >= 0 && act < ACT_COUNT ? act_names[act] : "?"; }

// ---- keys by name. The ini has SDL's scancode names (the key's place, as on a US keyboard: the same key on any
// layout), "Key<number>" for the few whose names an ini line can't hold (; # , =), and the mouse's own names.
static const char *const mouse_names[5] = {"Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};

static void key_ini_name(int key, char *out, size_t n) {
    if (!key) { out[0] = 0; return; }
    if (key > KEY_MOUSE && key <= KEY_MOUSE + 5) { snprintf(out, n, "%s", mouse_names[key - KEY_MOUSE - 1]); return; }
    if (key == KEY_WHEEL_UP || key == KEY_WHEEL_DOWN) { snprintf(out, n, "Wheel %s", key == KEY_WHEEL_UP ? "Up" : "Down"); return; }
    const char *s = SDL_GetScancodeName((SDL_Scancode)key);
    if (!s[0] || strpbrk(s, ";#,=")) snprintf(out, n, "Key%d", key);
    else snprintf(out, n, "%s", s);
}

static int key_from_name(const char *s) {  // 0 for none, -1 when not a key
    if (!*s || !strcasecmp(s, "none")) return 0;
    for (int i = 0; i < 5; i++) if (!strcasecmp(s, mouse_names[i])) return KEY_MOUSE + 1 + i;
    if (!strcasecmp(s, "Wheel Up")) return KEY_WHEEL_UP;
    if (!strcasecmp(s, "Wheel Down")) return KEY_WHEEL_DOWN;
    if (!strcasecmp(s, "Enter")) return SDL_SCANCODE_RETURN;
    int k;
    char end;
    if (!strncasecmp(s, "Key", 3) && sscanf(s + 3, "%d%c", &k, &end) == 1) return k > 0 && k < SDL_SCANCODE_COUNT ? k : -1;
    SDL_Scancode sc = SDL_GetScancodeFromName(s);
    return sc != SDL_SCANCODE_UNKNOWN ? (int)sc : -1;
}

void bind_key_label(int key, char *out, size_t n) {
    static const char *const mouse[5] = {"Left click", "Middle click", "Right click", "Mouse 4", "Mouse 5"};
    out[0] = 0;
    if (!key) return;
    if (key > KEY_MOUSE && key <= KEY_MOUSE + 5) { snprintf(out, n, "%s", mouse[key - KEY_MOUSE - 1]); return; }
    if (key == KEY_WHEEL_UP || key == KEY_WHEEL_DOWN) { snprintf(out, n, "Wheel %s", key == KEY_WHEEL_UP ? "up" : "down"); return; }
    switch (key) {  // (SDL's names where they read oddly)
    case SDL_SCANCODE_RETURN: snprintf(out, n, "Enter"); return;
    case SDL_SCANCODE_KP_ENTER: snprintf(out, n, "Num Enter"); return;
    case SDL_SCANCODE_GRAVE: snprintf(out, n, "`"); return;
    }
    // letters, digits and signs as this keyboard layout labels the key (AZERTY's A is where QWERTY's Q is)
    const char *s = SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)key, SDL_KMOD_NONE, false));
    if (!s || !s[0]) s = SDL_GetScancodeName((SDL_Scancode)key);
    if (s && s[0]) snprintf(out, n, "%s", s);
    else snprintf(out, n, "Key %d", key);
}

bool bind_key_reserved(int key) {
    return key == SDL_SCANCODE_GRAVE || key == SDL_SCANCODE_ESCAPE || (key >= SDL_SCANCODE_F6 && key <= SDL_SCANCODE_F12);
}

void bind_set_key(int act, int slot, int key, int *from) {
    *from = -1;
    if (key)
        for (int a = 0; a < ACT_COUNT; a++)
            for (int s = 0; s < KEY_SLOTS; s++)
                if (g_bind.key[a][s] == key && !(a == act && s == slot)) {
                    g_bind.key[a][s] = 0;
                    if (a != act) *from = a;
                }
    g_bind.key[act][slot] = (uint16_t)key;
}

// ---- the controller. Inputs by their place (SDL's names: south is the bottom face button); triggers past a third.
static const SDL_GamepadButton faces[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                           SDL_GAMEPAD_BUTTON_NORTH};

int bind_pad_code(int btn, SDL_Gamepad *pad) {
    if (btn < 0 || btn >= 16) return -1;
    if (g_bind.pad[btn] >= 0) return g_bind.pad[btn];
    switch (btn) {
    case BTN_L: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case BTN_R: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case BTN_ZL: return PAD_LT;
    case BTN_ZR: return PAD_RT;
    case BTN_PLUS: return SDL_GAMEPAD_BUTTON_START;
    case BTN_MINUS: return SDL_GAMEPAD_BUTTON_BACK;
    case BTN_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case BTN_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case BTN_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case BTN_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    case BTN_LSTICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case BTN_RSTICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    }
    // face buttons. FaceButtons Position: the Switch layout (the right one is A); Label: the letter printed on the
    // button (PlayStation symbols: an Xbox controller's letters at that place)
    static const int switch_at[4] = {BTN_B, BTN_A, BTN_Y, BTN_X}, xbox_at[4] = {BTN_A, BTN_B, BTN_X, BTN_Y};
    if (g_config.face_buttons == FACE_LABEL) {
        for (int i = 0; pad && i < 4; i++) {
            SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabel(pad, faces[i]);
            int b = l == SDL_GAMEPAD_BUTTON_LABEL_A ? BTN_A : l == SDL_GAMEPAD_BUTTON_LABEL_B ? BTN_B
                  : l == SDL_GAMEPAD_BUTTON_LABEL_X ? BTN_X : l == SDL_GAMEPAD_BUTTON_LABEL_Y ? BTN_Y : -1;
            if (b == btn) return faces[i];
        }
        for (int i = 0; i < 4; i++) if (xbox_at[i] == btn) return faces[i];
    }
    for (int i = 0; i < 4; i++) if (switch_at[i] == btn) return faces[i];
    return -1;
}

bool bind_pad_down(SDL_Gamepad *pad, int code) {
    if (!pad || code < 0) return false;
    if (code == PAD_LT || code == PAD_RT)
        return SDL_GetGamepadAxis(pad, code == PAD_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
    return code < SDL_GAMEPAD_BUTTON_COUNT && SDL_GetGamepadButton(pad, (SDL_GamepadButton)code);
}

int bind_pad_held(SDL_Gamepad *pad) {
    if (!pad) return -1;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) return b;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) return PAD_LT;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) return PAD_RT;
    return -1;
}

void bind_pad_label(int code, SDL_Gamepad *pad, char *out, size_t n) {
    SDL_GamepadType t = pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    bool ps = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    bool nin = t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    const char *s = NULL;
    switch (code) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH: {
        static const char *const place[] = {"Bottom", "Right", "Left", "Top"};
        static const char *const labels[] = {"", "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
        SDL_GamepadButtonLabel l = pad ? SDL_GetGamepadButtonLabel(pad, (SDL_GamepadButton)code) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
        if (l > 0 && l < (int)(sizeof labels / sizeof *labels)) snprintf(out, n, "%s (%s)", place[code], labels[l]);
        else snprintf(out, n, "%s button", place[code]);
        return;
    }
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: s = ps ? "L1" : nin ? "L" : "LB"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: s = ps ? "R1" : nin ? "R" : "RB"; break;
    case PAD_LT: s = ps ? "L2" : nin ? "ZL" : "LT"; break;
    case PAD_RT: s = ps ? "R2" : nin ? "ZR" : "RT"; break;
    case SDL_GAMEPAD_BUTTON_BACK: s = ps ? "Share" : nin ? "Minus" : "View"; break;
    case SDL_GAMEPAD_BUTTON_START: s = ps ? "Options" : nin ? "Plus" : "Menu"; break;
    case SDL_GAMEPAD_BUTTON_GUIDE: s = "Home"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: s = ps ? "L3" : "Left stick click"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: s = ps ? "R3" : "Right stick click"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: s = "D-pad up"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: s = "D-pad down"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: s = "D-pad left"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: s = "D-pad right"; break;
    case SDL_GAMEPAD_BUTTON_MISC1: s = ps ? "Mute" : nin ? "Capture" : "Share"; break;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: s = "Touchpad"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: s = "Paddle R1"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: s = "Paddle L1"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: s = "Paddle R2"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: s = "Paddle L2"; break;
    }
    if (s) snprintf(out, n, "%s", s);
    else if (code >= 0) snprintf(out, n, "Button %d", code);
    else snprintf(out, n, "None");
}

// The ini's names: SDL's ("south", "leftshoulder", "dpup"...; "lefttrigger" / "righttrigger"), "Default".
static const char *pad_ini_name(int code) {
    if (code < 0) return "Default";
    if (code == PAD_LT || code == PAD_RT)
        return SDL_GetGamepadStringForAxis(code == PAD_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const char *s = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
    return s ? s : "Default";
}

// ---- the ini
bool bindings_parse(bool keyboard, const char *key, const char *val, int line) {
    int act = -1;
    for (int i = 0; i < ACT_COUNT; i++) if (!strcasecmp(key, act_names[i])) act = i;
    if (act < 0 || (!keyboard && act >= 16)) return false;
    if (!keyboard) {
        int code = -2;
        if (!*val || !strcasecmp(val, "default")) code = -1;
        else if (!strcasecmp(val, "lefttrigger")) code = PAD_LT;
        else if (!strcasecmp(val, "righttrigger")) code = PAD_RT;
        else if (SDL_GetGamepadButtonFromString(val) != SDL_GAMEPAD_BUTTON_INVALID) code = SDL_GetGamepadButtonFromString(val);
        if (code == -2) LOG("[config] line %d: \"%s\" is not a controller button; %s keeps its default\n", line, val, key);
        else g_bind.pad[act] = (int8_t)code;
        return true;
    }
    uint16_t keys[KEY_SLOTS] = {0};
    int n = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", val);
    for (char *tok = buf, *next; tok; tok = next) {
        next = strchr(tok, ',');
        if (next) *next++ = 0;
        while (*tok == ' ' || *tok == '\t') tok++;
        for (char *e = tok + strlen(tok); e > tok && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (!*tok) continue;
        int k = key_from_name(tok);
        if (k < 0) { LOG("[config] line %d: \"%s\" is not a key (%s)\n", line, tok, key); continue; }
        if (n < KEY_SLOTS) keys[n++] = (uint16_t)k;
        else LOG("[config] line %d: %s takes at most %d keys (\"%s\" ignored)\n", line, key, KEY_SLOTS, tok);
    }
    memcpy(g_bind.key[act], keys, sizeof keys);
    return true;
}

void bindings_put(void) {
    for (int act = 0; act < ACT_COUNT; act++) {
        char a[48], b[48], v[100];
        key_ini_name(g_bind.key[act][0], a, sizeof a);
        key_ini_name(g_bind.key[act][1], b, sizeof b);
        snprintf(v, sizeof v, "%s%s%s", a, a[0] && b[0] ? ", " : "", b);
        config_edit_put("Keyboard", act_names[act], v);
    }
    for (int btn = 0; btn < 16; btn++) config_edit_put("Controller", act_names[btn], pad_ini_name(g_bind.pad[btn]));
}
