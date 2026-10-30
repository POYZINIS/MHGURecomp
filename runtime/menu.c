// The settings menu (menu.h). The ` key (below Esc) opens and closes it; while it is open the game gets no keyboard,
// mouse or controller input (it keeps running, as with Forge's menu). Pages: Controls (rebinding: runtime/bindings.c),
// Mouse, Display, Graphics, Gameplay, Multiplayer (mhgurecomp.ini: runtime/config.c). Settings that can change while
// the game runs do so at once; the others say "Restart to apply". Everything is saved to mhgurecomp.ini a moment
// after it changes.
//
// Clay (third_party/clay) lays the menu out every frame: immediate mode, the widgets below are functions that declare
// elements and read the pointer against the last frame's layout. Its render commands become quads for
// runtime/nvn/vulkan/menu_vk.c: rounded rectangles with borders and soft shadows drawn as distance fields, and text
// from a glyph atlas baked here with stb_truetype (Dear ImGui's copy) from a system font: Segoe UI on Windows, Noto
// Sans / DejaVu Sans elsewhere; MHGU_MENU_FONT / MHGU_MENU_FONT_BOLD pick others.
//
// Input: the mouse (hover, click, wheel, dragging sliders; right click clears a key), the keyboard (arrows move and
// change, Enter activates, Tab switches pages, Esc goes back) and the controller (d-pad or left stick, A, B, X
// clears, L / R switch pages). Host main thread only; the render thread takes the last finished frame.
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#include "imgui/imstb_rectpack.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imgui/imstb_truetype.h"

#include "bindings.h"
#include "config.h"
#include "forge.h"
#include "hle/input.h"
#include "menu.h"
#include "rt.h"
#include "textinput.h"

#ifndef MHGU_NULL_BACKEND
void vk_set_msaa(uint32_t n);          // runtime/nvn/vulkan/device.c
void vk_request_present_mode(void);    // (VSync: the swapchain made again)
#endif

// ================================================================== look
// Sizes are for a 720-line window; S scales them to the window's height.
static float S = 1.0f;
static float px(float v) { return roundf(v * S); }
static uint16_t pxu(float v) { return (uint16_t)fmaxf(0.0f, roundf(v * S)); }

static const Clay_Color C_NONE = {0, 0, 0, 0};
static const Clay_Color C_BACKDROP = {4, 6, 10, 150};
static const Clay_Color C_PANEL = {19, 21, 27, 250};
static const Clay_Color C_PANEL_LINE = {255, 255, 255, 16};
static const Clay_Color C_SIDE = {14, 16, 21, 255};
static const Clay_Color C_CARD = {26, 29, 36, 255};
static const Clay_Color C_CARD_LINE = {255, 255, 255, 9};
static const Clay_Color C_HOVER = {255, 255, 255, 9};
static const Clay_Color C_HOVER2 = {255, 255, 255, 20};
static const Clay_Color C_WELL = {12, 14, 18, 255};
static const Clay_Color C_TEXT = {236, 238, 242, 255};
static const Clay_Color C_TEXT2 = {200, 205, 214, 255};
static const Clay_Color C_MUTED = {150, 157, 169, 255};
static const Clay_Color C_FAINT = {104, 111, 123, 255};
static const Clay_Color C_ACCENT = {242, 169, 59, 255};
static const Clay_Color C_ACCENT_HI = {255, 192, 96, 255};
static const Clay_Color C_ACCENT_SOFT = {242, 169, 59, 28};
static const Clay_Color C_ACCENT_INK = {32, 20, 4, 255};
static const Clay_Color C_FOCUS = {242, 169, 59, 210};
static const Clay_Color C_FOCUS_ROW = {242, 169, 59, 20};
static const Clay_Color C_TRACK = {58, 63, 74, 255};
static const Clay_Color C_KNOB = {246, 247, 249, 255};
static const Clay_Color C_CHIP = {33, 37, 45, 255};
static const Clay_Color C_CHIP_HOT = {42, 47, 57, 255};
static const Clay_Color C_CHIP_LINE = {255, 255, 255, 22};
static const Clay_Color C_BADGE = {40, 44, 53, 255};
static const Clay_Color C_GOOD = {88, 199, 122, 255};
static const Clay_Color C_SHADOW = {0, 0, 0, 170};

static Clay_Color mixc(Clay_Color a, Clay_Color b, float t) {
    return (Clay_Color){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
static Clay_Color alpha(Clay_Color c, float k) { return (Clay_Color){c.r, c.g, c.b, c.a * k}; }

// ================================================================== fonts: stb_truetype, baked into one atlas
enum { F_REG, F_SEMI, NFONTS };
static struct { uint8_t *data; stbtt_fontinfo info; } fonts[NFONTS];

// The characters baked: ASCII, Latin-1, dashes / quotes / bullet / ellipsis, single angle quotes, arrows, minus.
static const struct { int first, count; } ranges[] = {{32, 95}, {160, 96}, {0x2013, 20}, {0x2039, 2}, {0x2190, 4}, {0x2212, 1}};
#define NRANGES ((int)(sizeof ranges / sizeof *ranges))
#define NGLYPHS (95 + 96 + 20 + 2 + 4 + 1)
static int glyph_ids[NFONTS][NGLYPHS];  // the fonts' own glyph numbers (kerning)

typedef struct {
    int font, size;
    float scale, ascent, descent, line;  // pixels
    stbtt_packedchar ch[NGLYPHS];
} Face;
#define MAX_FACES 48
static Face faces[MAX_FACES];
static int nfaces;
static uint8_t *atlas;
static int atlas_w, atlas_h;
static stbtt_pack_context pack;
static uint64_t atlas_gen;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;  // the shown frame and the atlas (render thread)

static uint8_t *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    return d;
}

static bool load_font(int i, const char *env, const char *const *names) {
    char path[600];
    const char *dirs[] = {getenv("WINDIR"), "/usr/share/fonts/truetype/noto", "/usr/share/fonts/noto", "/usr/share/fonts/google-noto",
                          "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF", "/usr/share/fonts/dejavu",
                          "/usr/share/fonts/truetype/liberation", "/usr/share/fonts/liberation-sans"};
    for (int k = -1; k < (int)(sizeof dirs / sizeof *dirs) * 8; k++) {
        if (k < 0) {
            if (!getenv(env)) continue;
            snprintf(path, sizeof path, "%s", getenv(env));
        } else {
            const char *dir = dirs[k / 8], *name = names[k % 8];
            if (!dir || !name) continue;
            snprintf(path, sizeof path, "%s%s%s", dir, k / 8 == 0 ? "\\Fonts\\" : "/", name);
        }
        uint8_t *d = read_file(path);
        if (d && stbtt_InitFont(&fonts[i].info, d, stbtt_GetFontOffsetForIndex(d, 0))) {
            fonts[i].data = d;
            LOG("[menu] font %s\n", path);
            for (int r = 0, g = 0; r < NRANGES; r++)
                for (int c = 0; c < ranges[r].count; c++) glyph_ids[i][g++] = stbtt_FindGlyphIndex(&fonts[i].info, ranges[r].first + c);
            return true;
        }
        free(d);
    }
    return false;
}

static void fonts_init(void) {
    static const char *const reg[8] = {"segoeui.ttf", "NotoSans-Regular.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf", "arial.ttf"};
    static const char *const semi[8] = {"seguisb.ttf", "segoeuib.ttf", "NotoSans-SemiBold.ttf", "NotoSans-Bold.ttf", "DejaVuSans-Bold.ttf",
                                        "LiberationSans-Bold.ttf", "arialbd.ttf"};
    if (!load_font(F_REG, "MHGU_MENU_FONT", reg)) LOG("[menu] no font found (MHGU_MENU_FONT=<.ttf>): the menu has no text\n");
    if (!load_font(F_SEMI, "MHGU_MENU_FONT_BOLD", semi) && fonts[F_REG].data) {
        fonts[F_SEMI] = fonts[F_REG];
        memcpy(glyph_ids[F_SEMI], glyph_ids[F_REG], sizeof glyph_ids[F_REG]);
    }
}

static void atlas_reset(int w, int h) {
    if (atlas) stbtt_PackEnd(&pack);
    atlas = realloc(atlas, (size_t)w * (size_t)h);
    memset(atlas, 0, (size_t)w * (size_t)h);
    atlas_w = w, atlas_h = h;
    stbtt_PackBegin(&pack, atlas, w, h, 0, 1, NULL);
    stbtt_PackSetOversampling(&pack, 1, 1);  // (drawn at exactly this size, on whole pixels)
    atlas_gen++;
}

static bool bake(Face *f) {
    if (!fonts[f->font].data) return true;
    stbtt_pack_range r[NRANGES];
    for (int i = 0, base = 0; i < NRANGES; base += ranges[i].count, i++)
        r[i] = (stbtt_pack_range){.font_size = STBTT_POINT_SIZE((float)f->size), .first_unicode_codepoint_in_range = ranges[i].first,
                                  .num_chars = ranges[i].count, .chardata_for_range = f->ch + base};
    return stbtt_PackFontRanges(&pack, fonts[f->font].data, 0, r, NRANGES) != 0;
}

static Face *face(int font, int size) {
    if (font < 0 || font >= NFONTS) font = F_REG;
    for (int i = 0; i < nfaces; i++)
        if (faces[i].font == font && faces[i].size == size) return &faces[i];
    if (nfaces == MAX_FACES) return &faces[0];
    pthread_mutex_lock(&frame_lock);
    Face *f = &faces[nfaces++];
    memset(f, 0, sizeof *f);
    f->font = font, f->size = size;
    if (fonts[font].data) {
        int a, d, g;
        stbtt_GetFontVMetrics(&fonts[font].info, &a, &d, &g);
        f->scale = stbtt_ScaleForMappingEmToPixels(&fonts[font].info, (float)size);
        f->ascent = a * f->scale, f->descent = d * f->scale, f->line = (a - d + g) * f->scale;
    } else {
        f->line = (float)size;
    }
    if (!atlas) atlas_reset(1024, 1024);
    while (!bake(f)) {  // full: a larger atlas, every face baked again
        if (atlas_w >= 4096 && atlas_h >= 4096) { LOG("[menu] glyph atlas full\n"); break; }
        if (atlas_w <= atlas_h) atlas_reset(atlas_w * 2, atlas_h);
        else atlas_reset(atlas_w, atlas_h * 2);
        for (int i = 0; i < nfaces - 1; i++) bake(&faces[i]);
    }
    atlas_gen++;
    pthread_mutex_unlock(&frame_lock);
    return f;
}

static int glyph(int cp) {
    for (int i = 0, base = 0; i < NRANGES; base += ranges[i].count, i++)
        if (cp >= ranges[i].first && cp < ranges[i].first + ranges[i].count) return base + cp - ranges[i].first;
    return '?' - 32;
}

static int utf8_next(const char **ps, const char *end) {
    const unsigned char *p = (const unsigned char *)*ps;
    int c = *p++, n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n) c &= 0x3F >> n;
    while (n-- > 0 && (const char *)p < end && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
    *ps = (const char *)p;
    return c;
}

static float kern(const Face *f, int a, int b) {
    return fonts[f->font].data ? stbtt_GetGlyphKernAdvance(&fonts[f->font].info, glyph_ids[f->font][a], glyph_ids[f->font][b]) * f->scale : 0;
}

static float text_width(const Face *f, const char *s, int len, float spacing) {
    const char *end = s + len;
    float w = 0;
    for (int prev = -1; s < end;) {
        int g = glyph(utf8_next(&s, end));
        if (prev >= 0) w += kern(f, prev, g);
        w += f->ch[g].xadvance + spacing;
        prev = g;
    }
    return w;
}

static Clay_Dimensions measure(Clay_StringSlice t, Clay_TextElementConfig *c, void *user) {
    (void)user;
    const Face *f = face(c->fontId, c->fontSize);
    return (Clay_Dimensions){text_width(f, t.chars, t.length, c->letterSpacing), c->lineHeight ? c->lineHeight : roundf(f->line)};
}

// ================================================================== strings of a frame
static char sbuf[1 << 16];
static int spos;

static Clay_String str(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int room = (int)sizeof sbuf - spos, n = room > 1 ? vsnprintf(sbuf + spos, (size_t)room, fmt, ap) : 0;
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= room) n = room > 0 ? room - 1 : 0;
    Clay_String s = {.isStaticallyAllocated = false, .length = n, .chars = sbuf + spos};
    spos += n + 1;
    return s;
}
// (a string that stays: a literal or static table; Clay caches its size by address)
static Clay_String lit(const char *s) { return (Clay_String){.isStaticallyAllocated = true, .length = (int32_t)strlen(s), .chars = s}; }

static Clay_TextElementConfig *tc(int font, float size, Clay_Color c) {
    return CLAY_TEXT_CONFIG({.fontId = (uint16_t)font, .fontSize = pxu(size), .textColor = c});
}
static Clay_TextElementConfig *tcn(int font, float size, Clay_Color c) {  // one line, never wrapped
    return CLAY_TEXT_CONFIG({.fontId = (uint16_t)font, .fontSize = pxu(size), .textColor = c, .wrapMode = CLAY_TEXT_WRAP_NONE});
}

// ================================================================== state
enum { PAGE_CONTROLS, PAGE_MOUSE, PAGE_DISPLAY, PAGE_GRAPHICS, PAGE_GAMEPLAY, PAGE_NETWORK, NPAGES };
static const char *const page_names[NPAGES] = {"Controls", "Mouse", "Display", "Graphics", "Gameplay", "Multiplayer"};
static const char *const page_desc[NPAGES] = {
    "The keys, mouse buttons and controller buttons for each of the Switch controller's buttons.",
    "How the mouse turns the camera and works in menus.",
    "Window, resolution and the picture's shape. Most of these apply after a restart.",
    "Frame rate and edge smoothing. These apply at once.",
    "Monster HP and where quests can be played.",
    "Local play with other hunters over the network.",
};

static bool inited, want_open;
static float fade;              // 0 closed .. 1 open (the open / close animation)
static int page, ctl_tab;
static uint64_t now_ns, last_ns, last_build_ns;
static float dt;
static bool dirty_frame;        // input since the last frame
static SDL_Gamepad *cur_pad;
static Config boot;             // the settings the game started with (restart-only ones compare to these)
static ConfigSizes boot_sizes;

// input gathered from events (ev) and the frame's copy (in)
typedef struct {
    float mx, my;
    bool down, pressed, released, rpressed;
    float wheel;
    int dy, dx, page;
    bool activate, back, clear, moved;
} Input;
static Input ev, in;
static bool kb_nav;             // the keyboard / controller moves the focus (its highlight shows), else the mouse

static uint32_t press_id;       // the widget the left button went down on
static uint32_t focus_id, focus_row, focus_row_next, row_id;  // row_id: the row being declared (its focusables)
typedef struct { uint32_t id, row; uint8_t col; bool dx; } Focus;  // dx: the widget takes left / right itself
static Focus order[512], prev_order[512];
static int norder, nprev, scroll_from, scroll_to;  // focusable widgets this frame / last; those in the scroll area
static int row_cols;                                // focusables in the row so far (their column)
static bool focus_moved, scroll_reset;

// rebinding: waiting for a key (or a controller button) for this action's slot
static struct { int act, slot; bool pad, armed; uint64_t start_ns; } cap = {-1, 0, false, false, 0};
static bool pad_hold;           // a controller button went to a binding: ignored until released

// text editing (Port, Join)
static uint32_t edit_id;
static char edit_buf[256];
static bool edit_digits;
static int edit_done;           // 1 commit, 2 cancel (keys), handled by the field

// status line
static char status_text[200];
static uint64_t status_ns, saved_ns;
static bool status_good;

// saving: settings changed lately, written together a moment after the last change
static const char *dirty[32];
static int ndirty;
static bool dirty_binds;
static uint64_t dirty_ns, reset_armed_ns;

static void status(bool good, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_text, sizeof status_text, fmt, ap);
    va_end(ap);
    status_good = good, status_ns = now_ns;
}

static void save_later(const char *key) {
    for (int i = 0; i < ndirty; i++) if (!strcmp(dirty[i], key)) { dirty_ns = now_ns; return; }
    if (ndirty < 32) dirty[ndirty++] = key;
    dirty_ns = now_ns;
}
static void binds_changed(void) { dirty_binds = true, dirty_ns = now_ns; }

static void save_flush(bool force) {
    if (!ndirty && !dirty_binds) return;
    if (!force && now_ns - dirty_ns < 400000000ull) return;
    config_edit_begin();
    for (int i = 0; i < ndirty; i++) config_edit_setting(dirty[i]);
    if (dirty_binds) bindings_put();
    config_edit_end();
    LOG("[menu] saved %d setting(s)%s to mhgurecomp.ini\n", ndirty, dirty_binds ? " and the controls" : "");
    ndirty = 0, dirty_binds = false, saved_ns = now_ns;
}

static bool restart_pending(void) {
    const Config *c = &g_config, *b = &boot;
    return c->display_mode != b->display_mode || memcmp(&g_config_sizes, &boot_sizes, sizeof boot_sizes) ||
           c->aspect_ratio != b->aspect_ratio || c->ui_anchor != b->ui_anchor || c->background_input != b->background_input ||
           c->bonus_content != b->bonus_content || c->ldn_port != b->ldn_port;
}

// per-element animations (hover fades, toggle knobs): eased toward their target
static struct { uint32_t id; float v; } anims[512];
static float anim(uint32_t id, float target, float rate) {
    unsigned h = id % 512;
    for (int i = 0; i < 512; i++, h = (h + 1) % 512) {
        if (anims[h].id == id) break;
        if (!anims[h].id) { anims[h].id = id, anims[h].v = target; break; }
    }
    if (anims[h].id != id) return target;
    anims[h].v += (target - anims[h].v) * (1.0f - expf(-dt * rate));
    if (fabsf(target - anims[h].v) < 0.002f) anims[h].v = target;
    else dirty_frame = true;  // (still moving: keep drawing)
    return anims[h].v;
}

// ================================================================== interaction
static bool over(Clay_ElementId id) { return !kb_nav && Clay_PointerOver(id); }
static void interact(Clay_ElementId id) {  // (deeper elements come later and win)
    if (in.pressed && Clay_PointerOver(id)) press_id = id.id;
}
static bool clicked(Clay_ElementId id) {
    if (!(in.released && press_id == id.id && Clay_PointerOver(id))) return false;
    focus_id = id.id;
    return true;
}
// A widget the keyboard / controller can focus: up / down go from row to row (to the widget in the same column), left /
// right change the widget's value or, for those without one (keys, buttons, fields), go to the next in the row.
static bool focusable_in(Clay_ElementId id, bool takes_dx) {
    if (norder < 512) order[norder++] = (Focus){id.id, row_id, (uint8_t)row_cols, takes_dx};
    row_cols++;
    bool f = kb_nav && focus_id == id.id;
    if (f) focus_row_next = row_id;
    return f;
}
static bool focusable(Clay_ElementId id) { return focusable_in(id, true); }
static int focus_index(uint32_t id) {
    for (int k = 0; k < nprev; k++) if (prev_order[k].id == id) return k;
    return -1;
}
static Clay_BorderElementConfig ring(bool on, Clay_Color off_color, uint16_t off_width) {
    return on ? (Clay_BorderElementConfig){C_FOCUS, {pxu(2), pxu(2), pxu(2), pxu(2), 0}}
              : (Clay_BorderElementConfig){off_color, {off_width, off_width, off_width, off_width, 0}};
}

static void nav_begin(void) {
    if (in.moved || in.pressed) kb_nav = false;
    bool nav = in.dy || in.dx || in.activate || in.clear;
    if (nav && !kb_nav) {  // the first key press only shows where the focus is
        kb_nav = true;
        bool known = focus_index(focus_id) >= 0;
        if (!known && nprev) focus_id = prev_order[scroll_from < nprev ? scroll_from : 0].id;
        focus_moved = true;
        in.dx = 0, in.activate = in.clear = false;
        if (!known) in.dy = 0;
    }
    int i = focus_index(focus_id);
    if (in.dy && i >= 0) {
        int dir = in.dy > 0 ? 1 : -1, col = prev_order[i].col;
        for (int step = 0, j = i; step < (in.dy > 0 ? in.dy : -in.dy); step++) {
            int k = j;
            while (k >= 0 && k < nprev && prev_order[k].row == prev_order[j].row) k += dir;  // (out of this row)
            if (k < 0 || k >= nprev) break;
            int first = k;
            while (first > 0 && prev_order[first - 1].row == prev_order[k].row) first--;
            j = first;  // (the next row's widget in this column, else its last)
            for (int m = first; m < nprev && prev_order[m].row == prev_order[first].row; m++) if (prev_order[m].col <= col) j = m;
            focus_id = prev_order[j].id, focus_moved = true;
        }
    }
    if (in.dx && i >= 0 && !prev_order[i].dx) {
        int k = i + (in.dx > 0 ? 1 : -1);
        if (k >= 0 && k < nprev && prev_order[k].row == prev_order[i].row) focus_id = prev_order[k].id, focus_moved = true;
        in.dx = 0;
    }
    norder = 0, focus_row_next = 0;
}

static void nav_end(void) {
    memcpy(prev_order, order, (size_t)norder * sizeof *order);
    nprev = norder;
    focus_row = focus_row_next;
    if (!focus_moved || !kb_nav) return;
    focus_moved = false;
    bool in_scroll = false;
    for (int i = scroll_from; i < scroll_to; i++) in_scroll |= order[i].id == focus_id;
    Clay_ElementId sid = CLAY_ID("Scroll");
    Clay_ElementData e = Clay_GetElementData((Clay_ElementId){.id = focus_row ? focus_row : focus_id}), v = Clay_GetElementData(sid);
    Clay_ScrollContainerData s = Clay_GetScrollContainerData(sid);
    if (!in_scroll || !e.found || !v.found || !s.found || !s.scrollPosition) return;
    float m = px(16), top = v.boundingBox.y + m, bottom = v.boundingBox.y + v.boundingBox.height - m;
    if (e.boundingBox.y < top) s.scrollPosition->y += top - e.boundingBox.y;
    else if (e.boundingBox.y + e.boundingBox.height > bottom) s.scrollPosition->y -= e.boundingBox.y + e.boundingBox.height - bottom;
    if (s.scrollPosition->y > 0) s.scrollPosition->y = 0;
}

// ================================================================== rebinding
static void cap_start(int act, int slot, bool pad) {
    cap.act = act, cap.slot = slot, cap.pad = pad, cap.armed = !pad, cap.start_ns = now_ns;
}
static void cap_cancel(void) { cap.act = -1; }

static const char *act_title(int act);

static void cap_key(int key) {  // a key, mouse button or wheel turn while waiting (keyboard / mouse bindings)
    char label[48];
    bind_key_label(key, label, sizeof label);
    if (bind_key_reserved(key)) {
        status(false, "%s is one of the port's own keys: choose another (Esc cancels)", label);
        return;
    }
    int act = cap.act, from;
    bind_set_key(act, cap.slot, key, &from);
    cap.act = -1;
    binds_changed();
    if (from >= 0) status(true, "%s now presses %s (it was %s's)", label, act_title(act), act_title(from));
    else status(true, "%s now presses %s", label, act_title(act));
}

static void cap_pad(void) {  // waiting for a controller button: polled
    if (cap.act < 0 || !cap.pad) return;
    int code = bind_pad_held(cur_pad);
    if (now_ns - cap.start_ns > 8000000000ull) { cap_cancel(); status(false, "No button pressed: nothing changed"); return; }
    if (!cap.armed) { cap.armed = code < 0; return; }  // (the press that started it let go first)
    if (code < 0) return;
    g_bind.pad[cap.act] = (int8_t)code;
    char label[48];
    bind_pad_label(code, cur_pad, label, sizeof label);
    status(true, "%s now presses %s", label, act_title(cap.act));
    cap.act = -1, pad_hold = true;
    binds_changed();
}

// ================================================================== widgets
// A row: a title (with a Switch-style badge for controls), a description, and the control on the right.
static void row_open(uint32_t key, const char *badge, const char *title, const char *desc, bool restart) {
    Clay_ElementId id = CLAY_IDI("row", key);
    row_id = id.id, row_cols = 0;
    bool foc = kb_nav && focus_row == id.id;
    float h = anim(id.id, over(id) ? 1.0f : 0.0f, 22.0f);
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .id = id,
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(px(54))}, .padding = {pxu(14), pxu(12), pxu(10), pxu(10)},
                   .childGap = pxu(16), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = foc ? C_FOCUS_ROW : alpha(C_HOVER, h), .cornerRadius = CLAY_CORNER_RADIUS(px(10))});
    if (badge) {
        int chars = 0;  // (one character: a round badge, as the controller's face buttons; else a rounded one)
        for (const char *c = badge; *c; c++) chars += (*c & 0xC0) != 0x80;
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(px(30)), CLAY_SIZING_FIXED(px(30))}, .padding = {pxu(6), pxu(6), 0, 0},
                         .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = C_BADGE, .cornerRadius = CLAY_CORNER_RADIUS(px(chars > 1 ? 8 : 15))}) {
            CLAY_TEXT(lit(badge), tcn(F_SEMI, 13, C_TEXT));
        }
    }
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = pxu(3), .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        CLAY({.layout = {.childGap = pxu(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            CLAY_TEXT(str("%s", title), tcn(F_SEMI, 15, C_TEXT));
            if (restart)
                CLAY({.layout = {.padding = {pxu(7), pxu(7), pxu(2), pxu(3)}}, .backgroundColor = C_ACCENT_SOFT,
                      .cornerRadius = CLAY_CORNER_RADIUS(px(6))}) {
                    CLAY_TEXT(CLAY_STRING("Restart to apply"), tcn(F_SEMI, 11, C_ACCENT));
                }
        }
        if (desc && *desc) CLAY_TEXT(lit(desc), tc(F_REG, 13, C_MUTED));
    }
    Clay__OpenElement();  // (the control side)
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){.layout = {.childGap = pxu(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}});
}
static void row_close(void) {
    Clay__CloseElement();
    Clay__CloseElement();
    row_id = 0;
}
#define ROW(key, badge, title, desc, restart) for (int row_ = (row_open(key, badge, title, desc, restart), 0); !row_; row_ = 1, row_close())

// A group: a small heading over a card of rows.
static void group_open(const char *title) {
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = pxu(8), .layoutDirection = CLAY_TOP_TO_BOTTOM}});
    if (title)
        CLAY({.layout = {.padding = {pxu(6), 0, 0, 0}}}) {
            CLAY_TEXT(str("%s", title), CLAY_TEXT_CONFIG({.fontId = F_SEMI, .fontSize = pxu(11.5f), .textColor = C_FAINT,
                                                    .letterSpacing = pxu(1), .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = CLAY_PADDING_ALL(pxu(5)), .childGap = pxu(2),
                   .layoutDirection = CLAY_TOP_TO_BOTTOM},
        .backgroundColor = C_CARD, .cornerRadius = CLAY_CORNER_RADIUS(px(14)),
        .border = {C_CARD_LINE, {1, 1, 1, 1, 0}}});
}
static void group_close(void) {
    Clay__CloseElement();
    Clay__CloseElement();
}
#define GROUP(title) for (int group_ = (group_open(title), 0); !group_; group_ = 1, group_close())

static void note(const char *text) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {pxu(12), pxu(16), pxu(11), pxu(11)},
                     .childGap = pxu(12)},
          .backgroundColor = C_ACCENT_SOFT, .cornerRadius = CLAY_CORNER_RADIUS(px(12))}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(px(3)), CLAY_SIZING_GROW(0)}}, .backgroundColor = C_ACCENT,
              .cornerRadius = CLAY_CORNER_RADIUS(px(2))}) {}
        CLAY_TEXT(lit(text), tc(F_REG, 13, C_TEXT2));
    }
}

static bool w_toggle(uint32_t key, int *v) {
    Clay_ElementId id = CLAY_IDI("tog", key);
    interact(id);
    bool foc = focusable(id), changed = false;
    if (clicked(id) || (foc && in.activate)) *v = !*v, changed = true;
    if (foc && in.dx && (in.dx > 0) != (*v != 0)) *v = in.dx > 0, changed = true;
    float t = anim(id.id, *v ? 1.0f : 0.0f, 18.0f);
    Clay_Color track = mixc(C_TRACK, C_ACCENT, t);
    if (over(id)) track = mixc(track, C_KNOB, 0.08f);
    CLAY({.id = id,
          .layout = {.sizing = {CLAY_SIZING_FIXED(px(46)), CLAY_SIZING_FIXED(px(26))},
                     .padding = {(uint16_t)(pxu(3) + roundf(t * px(20))), 0, 0, 0}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = track, .cornerRadius = CLAY_CORNER_RADIUS(px(13)), .border = ring(foc, C_NONE, 0)}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(px(20)), CLAY_SIZING_FIXED(px(20))}}, .backgroundColor = C_KNOB,
              .cornerRadius = CLAY_CORNER_RADIUS(px(10))}) {}
    }
    return changed;
}

static bool w_segment(uint32_t key, const char *const *opts, int n, int *sel) {
    Clay_ElementId id = CLAY_IDI("seg", key);
    bool foc = focusable(id), changed = false;
    for (int i = 0; i < n; i++) {
        Clay_ElementId o = CLAY_IDI("sego", key * 16 + i);
        interact(o);
        if (clicked(o)) {
            focus_id = id.id;
            if (*sel != i) *sel = i, changed = true;
        }
    }
    if (foc && in.dx && *sel + in.dx >= 0 && *sel + in.dx < n) *sel += in.dx, changed = true;
    if (foc && in.activate) *sel = (*sel + 1) % n, changed = true;
    CLAY({.id = id, .layout = {.padding = CLAY_PADDING_ALL(pxu(3)), .childGap = pxu(2)}, .backgroundColor = C_WELL,
          .cornerRadius = CLAY_CORNER_RADIUS(px(10)), .border = ring(foc, C_CHIP_LINE, 1)}) {
        for (int i = 0; i < n; i++) {
            Clay_ElementId o = CLAY_IDI("sego", key * 16 + i);
            bool on = *sel == i;
            float h = anim(o.id, over(o) ? 1.0f : 0.0f, 22.0f);
            CLAY({.id = o, .layout = {.padding = {pxu(12), pxu(12), pxu(6), pxu(7)}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = on ? C_ACCENT : alpha(C_HOVER2, h), .cornerRadius = CLAY_CORNER_RADIUS(px(8))}) {
                CLAY_TEXT(lit(opts[i]), tcn(on ? F_SEMI : F_REG, 13.5f, on ? C_ACCENT_INK : C_TEXT2));
            }
        }
    }
    return changed;
}

static float slider_t(float v, float lo, float hi, bool lg) {
    float t = lg ? logf(v / lo) / logf(hi / lo) : (v - lo) / (hi - lo);
    return t < 0 ? 0 : t > 1 ? 1 : t;
}
static float tidy(float v, float step, bool lg) {  // log sliders: two decimals below 1, then coarser
    if (lg) step = v < 1.0f ? 0.01f : v < 4.0f ? 0.05f : 0.1f;
    return roundf(v / step) * step;
}

static bool w_slider(uint32_t key, float *v, float lo, float hi, float step, bool lg, const char *fmt) {
    Clay_ElementId id = CLAY_IDI("sld", key);
    interact(id);
    bool foc = focusable(id), drag = press_id == id.id && (in.down || in.released), changed = false;  // (a click sets it too)
    float w = px(196), k = px(18), nv = *v;
    if (drag) {
        Clay_ElementData d = Clay_GetElementData(id);
        if (d.found) {
            float t = (in.mx - d.boundingBox.x - k / 2) / (d.boundingBox.width - k);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            nv = tidy(lg ? lo * powf(hi / lo, t) : lo + t * (hi - lo), step, lg);
        }
    }
    if (foc && in.dx) {
        nv = lg ? tidy(*v * powf(1.06f, (float)in.dx), step, lg) : *v + (float)in.dx * step;
        if (nv == *v) nv = *v + (float)in.dx * (lg ? 0.01f : step);
    }
    nv = nv < lo ? lo : nv > hi ? hi : nv;
    if (nv != *v) *v = nv, changed = true;
    float t = slider_t(*v, lo, hi, lg), hot = anim(id.id, over(id) || drag ? 1.0f : 0.0f, 22.0f);
    CLAY({.layout = {.childGap = pxu(12), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(px(26))}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .cornerRadius = CLAY_CORNER_RADIUS(px(13)), .border = ring(foc, C_NONE, 0)}) {
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(roundf(t * (w - k))), CLAY_SIZING_FIXED(px(6))}},
                  .backgroundColor = mixc(C_ACCENT, C_ACCENT_HI, hot), .cornerRadius = {px(3), 0, px(3), 0}}) {}
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(k), CLAY_SIZING_FIXED(k)}}, .backgroundColor = C_KNOB,
                  .cornerRadius = CLAY_CORNER_RADIUS(k / 2), .border = {alpha(C_ACCENT, hot), {pxu(2), pxu(2), pxu(2), pxu(2), 0}}}) {}
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(px(6))}}, .backgroundColor = C_TRACK,
                  .cornerRadius = {0, px(3), 0, px(3)}}) {}
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(px(64)), CLAY_SIZING_FIT(0)}, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}}) {
            CLAY_TEXT(str(fmt, *v), tcn(F_SEMI, 14, C_TEXT));
        }
    }
    return changed;
}

static bool w_cycle(uint32_t key, const char *const *opts, int n, int *sel) {
    Clay_ElementId id = CLAY_IDI("cyc", key), l = CLAY_IDI("cycl", key), r = CLAY_IDI("cycr", key);
    interact(l), interact(r);
    bool foc = focusable(id), changed = false;
    int want = *sel;
    if (clicked(l)) want--, focus_id = id.id;
    if (clicked(r)) want++, focus_id = id.id;
    if (foc && in.dx) want += in.dx;
    if (foc && in.activate) want = (want + 1) % n;
    if (want >= 0 && want < n && want != *sel) *sel = want, changed = true;
    CLAY({.id = id, .layout = {.padding = CLAY_PADDING_ALL(pxu(3)), .childGap = pxu(2), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_WELL, .cornerRadius = CLAY_CORNER_RADIUS(px(10)), .border = ring(foc, C_CHIP_LINE, 1)}) {
        for (int side = 0; side < 3; side++) {
            if (side == 1) {
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(px(132)), CLAY_SIZING_FIT(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
                    CLAY_TEXT(lit(opts[*sel]), tcn(F_SEMI, 14, C_TEXT));
                }
                continue;
            }
            Clay_ElementId b = side ? r : l;
            bool can = side ? *sel < n - 1 : *sel > 0;
            float h = anim(b.id, can && over(b) ? 1.0f : 0.0f, 22.0f);
            CLAY({.id = b, .layout = {.sizing = {CLAY_SIZING_FIXED(px(28)), CLAY_SIZING_FIXED(px(28))},
                                      .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = alpha(C_HOVER2, h), .cornerRadius = CLAY_CORNER_RADIUS(px(8))}) {
                CLAY_TEXT(side ? CLAY_STRING("\xe2\x80\xba") : CLAY_STRING("\xe2\x80\xb9"), tcn(F_SEMI, 20, can ? C_TEXT : C_FAINT));
            }
        }
    }
    return changed;
}

static bool w_button(uint32_t key, const char *label, bool primary) {
    Clay_ElementId id = CLAY_IDI("btn", key);
    interact(id);
    bool foc = focusable_in(id, false), hit = clicked(id) || (foc && in.activate);
    float h = anim(id.id, over(id) ? 1.0f : 0.0f, 22.0f);
    Clay_Color bg = primary ? mixc(C_ACCENT, C_ACCENT_HI, h) : mixc(C_CHIP, C_CHIP_HOT, h);
    CLAY({.id = id, .layout = {.padding = {pxu(18), pxu(18), pxu(8), pxu(9)}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(px(10)), .border = ring(foc, primary ? C_NONE : C_CHIP_LINE, primary ? 0 : 1)}) {
        CLAY_TEXT(lit(label), tcn(F_SEMI, 14, primary ? C_ACCENT_INK : C_TEXT));
    }
    return hit;
}

static void w_keychip(int act, int slot) {
    Clay_ElementId id = CLAY_IDI("key", act * 4 + slot);
    interact(id);
    bool foc = focusable_in(id, false), waiting = cap.act == act && cap.slot == slot && !cap.pad;
    if (!waiting && (clicked(id) || (foc && in.activate))) cap_start(act, slot, false), waiting = true;
    else if (!waiting && ((in.rpressed && Clay_PointerOver(id)) || (foc && in.clear)) && g_bind.key[act][slot]) {
        g_bind.key[act][slot] = 0;
        binds_changed();
        status(true, "%s: key cleared", act_title(act));
    }
    char label[48];
    bind_key_label(g_bind.key[act][slot], label, sizeof label);
    float h = anim(id.id, over(id) ? 1.0f : 0.0f, 22.0f), pulse = 0.6f + 0.4f * sinf((float)(now_ns % 4000000000ull) * 6e-9f);
    if (waiting) dirty_frame = true;
    CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIXED(px(144)), CLAY_SIZING_FIXED(px(34))},
                               .padding = {pxu(8), pxu(8), 0, 0}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = waiting ? C_ACCENT_SOFT : mixc(C_CHIP, C_CHIP_HOT, h), .cornerRadius = CLAY_CORNER_RADIUS(px(9)),
          .border = waiting ? (Clay_BorderElementConfig){alpha(C_ACCENT, pulse), {pxu(2), pxu(2), pxu(2), pxu(2), 0}} : ring(foc, C_CHIP_LINE, 1)}) {
        if (waiting) CLAY_TEXT(CLAY_STRING("Press a key\xe2\x80\xa6"), tcn(F_SEMI, 13, C_ACCENT));
        else if (label[0]) CLAY_TEXT(str("%s", label), tcn(F_SEMI, 13.5f, C_TEXT));
        else CLAY_TEXT(CLAY_STRING("\xe2\x80\x94"), tcn(F_REG, 13.5f, C_FAINT));
    }
}

static void w_padchip(int btn) {
    Clay_ElementId id = CLAY_IDI("pad", btn);
    interact(id);
    bool foc = focusable_in(id, false), waiting = cap.act == btn && cap.pad;
    if (!waiting && (clicked(id) || (foc && in.activate))) cap_start(btn, 0, true), waiting = true;
    else if (!waiting && ((in.rpressed && Clay_PointerOver(id)) || (foc && in.clear)) && g_bind.pad[btn] >= 0) {
        g_bind.pad[btn] = -1;
        binds_changed();
        status(true, "%s: back to its default button", act_title(btn));
    }
    char label[48];
    bind_pad_label(bind_pad_code(btn, cur_pad), cur_pad, label, sizeof label);
    float h = anim(id.id, over(id) ? 1.0f : 0.0f, 22.0f), pulse = 0.6f + 0.4f * sinf((float)(now_ns % 4000000000ull) * 6e-9f);
    if (waiting) dirty_frame = true;
    CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIXED(px(196)), CLAY_SIZING_FIXED(px(34))},
                               .padding = {pxu(8), pxu(8), 0, 0}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = waiting ? C_ACCENT_SOFT : mixc(C_CHIP, C_CHIP_HOT, h), .cornerRadius = CLAY_CORNER_RADIUS(px(9)),
          .border = waiting ? (Clay_BorderElementConfig){alpha(C_ACCENT, pulse), {pxu(2), pxu(2), pxu(2), pxu(2), 0}} : ring(foc, C_CHIP_LINE, 1)}) {
        if (waiting) CLAY_TEXT(cur_pad ? CLAY_STRING("Press a button\xe2\x80\xa6") : CLAY_STRING("No controller"), tcn(F_SEMI, 13, C_ACCENT));
        else CLAY_TEXT(str("%s", label), tcn(F_SEMI, 13.5f, g_bind.pad[btn] >= 0 ? C_ACCENT : C_TEXT));
    }
}

// A text field (Port, Join): click or Enter to edit, Enter or a click elsewhere keeps it, Esc drops the change.
static bool w_text(uint32_t key, char *buf, size_t size, float width, bool digits, const char *empty) {
    Clay_ElementId id = CLAY_IDI("txt", key);
    interact(id);
    bool foc = focusable_in(id, false), editing = edit_id == id.id, commit = false;
    if (editing && (edit_done || (in.pressed && !Clay_PointerOver(id)))) {
        if (edit_done != 2) snprintf(buf, size, "%s", edit_buf), commit = true;
        edit_id = 0, edit_done = 0, editing = false;
    } else if (!editing && (clicked(id) || (foc && in.activate))) {
        edit_id = id.id, editing = true, edit_digits = digits, edit_done = 0;
        snprintf(edit_buf, sizeof edit_buf, "%s", buf);
    }
    const char *shown = editing ? edit_buf : buf;
    bool blink = editing && (now_ns / 530000000ull) % 2 == 0;
    if (editing) dirty_frame = true;
    CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIXED(width), CLAY_SIZING_FIXED(px(34))}, .padding = {pxu(12), pxu(12), 0, 0},
                               .childGap = pxu(1), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_WELL, .cornerRadius = CLAY_CORNER_RADIUS(px(9)),
          .border = editing ? (Clay_BorderElementConfig){C_ACCENT, {pxu(2), pxu(2), pxu(2), pxu(2), 0}} : ring(foc, C_CHIP_LINE, 1),
          .clip = {.horizontal = true}}) {
        if (shown[0]) CLAY_TEXT(str("%s", shown), tcn(F_REG, 14, C_TEXT));
        else if (!editing) CLAY_TEXT(lit(empty), tcn(F_REG, 14, C_FAINT));
        if (editing)
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(px(2)), CLAY_SIZING_FIXED(px(17))}}, .backgroundColor = blink ? C_ACCENT : C_NONE}) {}
    }
    return commit;
}

// ================================================================== pages
static const struct { const char *badge, *name, *hint; } act_info[ACT_COUNT] = {
    [BTN_A] = {"A", "A", "Confirm \xc2\xb7 talk \xc2\xb7 gather"},
    [BTN_B] = {"B", "B", "Back \xc2\xb7 evade"},
    [BTN_X] = {"X", "X", "Attack \xc2\xb7 draw the weapon"},
    [BTN_Y] = {"Y", "Y", "Use an item \xc2\xb7 sheathe"},
    [BTN_LSTICK] = {"LS", "Left stick click", ""},
    [BTN_RSTICK] = {"RS", "Right stick click", ""},
    [BTN_L] = {"L", "L", "Camera behind the hunter"},
    [BTN_R] = {"R", "R", "Dash \xc2\xb7 weapon action"},
    [BTN_ZL] = {"ZL", "ZL", ""},
    [BTN_ZR] = {"ZR", "ZR", ""},
    [BTN_PLUS] = {"+", "Plus", "Start menu"},
    [BTN_MINUS] = {"\xe2\x88\x92", "Minus", ""},
    [BTN_LEFT] = {"\xe2\x86\x90", "D-pad left", ""},
    [BTN_UP] = {"\xe2\x86\x91", "D-pad up", ""},
    [BTN_RIGHT] = {"\xe2\x86\x92", "D-pad right", ""},
    [BTN_DOWN] = {"\xe2\x86\x93", "D-pad down", ""},
    [ACT_MOVE_UP] = {"\xe2\x86\x91", "Move forward", "Left stick up"},
    [ACT_MOVE_DOWN] = {"\xe2\x86\x93", "Move back", "Left stick down"},
    [ACT_MOVE_LEFT] = {"\xe2\x86\x90", "Move left", "Left stick left"},
    [ACT_MOVE_RIGHT] = {"\xe2\x86\x92", "Move right", "Left stick right"},
    [ACT_WALK] = {"\xc2\xbd", "Walk", "Held: the left stick at half tilt"},
    [ACT_CAM_UP] = {"\xe2\x86\x91", "Camera up", "Right stick up"},
    [ACT_CAM_DOWN] = {"\xe2\x86\x93", "Camera down", "Right stick down"},
    [ACT_CAM_LEFT] = {"\xe2\x86\x90", "Camera left", "Right stick left"},
    [ACT_CAM_RIGHT] = {"\xe2\x86\x92", "Camera right", "Right stick right"},
};
static const char *act_title(int act) {  // (four buffers in turn: a status line names two)
    static char bufs[4][64];
    static int k;
    char *buf = bufs[k++ % 4];
    if (act < 16 && strlen(act_info[act].name) <= 2) snprintf(buf, 64, "%s button", act_info[act].name);
    else snprintf(buf, 64, "%s", act_info[act].name);
    return buf;
}

static bool dmi_running(void);

static void page_controls(void) {
    if (ctl_tab == 0) {
        note("Click a box, then press the key or mouse button for it. Right-click a box (or press Delete) to clear it; "
             "Esc cancels. A key that already does something else moves here.");
        static const struct { const char *title; int n, acts[10]; } groups[] = {
            {"MOVEMENT", 5, {ACT_MOVE_UP, ACT_MOVE_DOWN, ACT_MOVE_LEFT, ACT_MOVE_RIGHT, ACT_WALK}},
            {"BUTTONS", 10, {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS}},
            {"D-PAD", 4, {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT}},
            {"STICK BUTTONS", 2, {BTN_LSTICK, BTN_RSTICK}},
            {"CAMERA KEYS  \xc2\xb7  FOR PLAYING WITHOUT A MOUSE", 4, {ACT_CAM_UP, ACT_CAM_DOWN, ACT_CAM_LEFT, ACT_CAM_RIGHT}},
        };
        for (unsigned g = 0; g < sizeof groups / sizeof *groups; g++)
            GROUP(groups[g].title) {
                for (int i = 0; i < groups[g].n; i++) {
                    int a = groups[g].acts[i];
                    ROW(100 + a, act_info[a].badge, act_title(a), act_info[a].hint, false) {
                        w_keychip(a, 0);
                        w_keychip(a, 1);
                    }
                }
            }
        if (g_config.direct_mouse && dmi_running())
            note("Direct mouse is on: in menus and towns a left click selects and confirms and a right click goes back, "
                 "whatever the mouse buttons are set to here.");
        return;
    }
    GROUP("LAYOUT") {
        static const char *const faces_[] = {"Position", "Label"};
        ROW(1, NULL, "Face buttons",
            "Position: the Switch layout the game is made for (the right button is A). Label: the letter printed on each "
            "button (on an Xbox controller A is the bottom one).", false) {
            if (w_segment(1, faces_, 2, &g_config.face_buttons)) save_later("FaceButtons");
        }
        ROW(2, NULL, "Controller in the background", "The controller keeps working while another window is in front.",
            g_config.background_input != boot.background_input) {
            if (w_toggle(2, &g_config.background_input)) save_later("BackgroundInput");
        }
    }
    const char *name = cur_pad ? SDL_GetGamepadName(cur_pad) : NULL;
    note(name ? "Click a box, then press the controller button for it. Right-click a box (or press X on the controller) "
                "to give it back its default. Orange: changed from the default."
              : "No controller is connected. Connect one to change its buttons.");
    static const int order_[16] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
                                   BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_LSTICK, BTN_RSTICK};
    GROUP(name ? str("BUTTONS  \xc2\xb7  %s", name).chars : "BUTTONS") {
        for (int i = 0; i < 16; i++) {
            int b = order_[i];
            ROW(200 + b, act_info[b].badge, act_title(b), act_info[b].hint, false) w_padchip(b);
        }
    }
}

// The DirectMouseInput mod is running (a Forge plugin in mods/DirectMouseInput: the direct mouse is its).
static bool dmi_running(void) {
    const char *t[32];
    int n = forge_plugin_titles(t, 32);
    for (int i = 0; i < n; i++)
        if (strstr(t[i], "DirectMouseInput")) return true;
    return false;
}

static void page_mouse(void) {
    bool dmi = dmi_running();
    GROUP(NULL) {
        ROW(1, NULL, "Direct mouse",
            "On: in menus the pointer selects what is under it, a left click confirms and a right click goes back; in "
            "quests the mouse turns the camera itself. Off: the mouse acts as the right stick.", false) {
            if (w_toggle(1, &g_config.direct_mouse)) save_later("DirectMouse");
        }
        ROW(2, NULL, "Mouse sensitivity", "How far the camera turns for a mouse movement. 1 is the default.", false) {
            if (w_slider(2, &g_config.mouse_sensitivity, 0.1f, 10.0f, 0.01f, true, "%.2f\xc3\x97")) save_later("MouseSensitivity");
        }
    }
    if (!dmi)
        note("Direct mouse comes with the DirectMouseInput mod, which isn't installed: put its folder in mods/. "
             "Until then the mouse acts as the right stick.");
    else if (g_config.direct_mouse)
        note("In quests the window takes the mouse for the camera; Esc lets it go and a click takes it again.");
    if (!dmi || !g_config.direct_mouse)
        note("Click in the game window to capture the mouse for the camera; Esc lets it go. The game's own camera "
             "options (speed, inverted axes) apply on top.");
}

// Resolutions offered (0 x 0: the desktop's / Auto), the file's own added when it is none of these.
static int res_opts[16][2], nres, rres_opts[16][2], nrres;
static char res_lab[16][24], rres_lab[16][24];
static const char *res_ptr[16], *rres_ptr[16];
static void size_list(int (*o)[2], int *n, char (*lab)[24], const char **ptr, const int (*base)[2], int nbase, int w, int h,
                      const char *zero) {
    *n = 0;
    bool have = false;
    for (int i = 0; i < nbase; i++) {
        o[*n][0] = base[i][0], o[*n][1] = base[i][1];
        have |= base[i][0] == w && base[i][1] == h;
        (*n)++;
    }
    if (!have) o[*n][0] = w, o[*n][1] = h, (*n)++;
    for (int i = 0; i < *n; i++) {
        if (o[i][0]) snprintf(lab[i], 24, "%d \xc3\x97 %d", o[i][0], o[i][1]);
        else snprintf(lab[i], 24, "%s", zero);
        ptr[i] = lab[i];
    }
}
static int size_index(int (*o)[2], int n, int w, int h) {
    for (int i = 0; i < n; i++) if (o[i][0] == w && o[i][1] == h) return i;
    return 0;
}

static void page_display(void) {
    static const char *const modes[] = {"Windowed", "Fullscreen", "Borderless"};
    static const char *const shapes[] = {"Auto", "16:9", "16:10", "21:9", "32:9"};
    static const float shape_v[] = {0, 16.0f / 9.0f, 1.6f, 21.0f / 9.0f, 32.0f / 9.0f};
    static const char *const huds[] = {"Center", "21:9", "32:9", "Edges"};
    ConfigSizes *z = &g_config_sizes;
    GROUP("SCREEN") {
        ROW(1, NULL, "Display mode", "Borderless covers the screen with a window; Alt+Enter or F11 switches to Windowed.",
            g_config.display_mode != boot.display_mode) {
            if (w_segment(1, modes, 3, &g_config.display_mode)) save_later("Mode");
        }
        ROW(2, NULL, "Resolution", "The window's size, or the monitor mode in Fullscreen.",
            z->width != boot_sizes.width || z->height != boot_sizes.height) {
            int sel = size_index(res_opts, nres, z->width, z->height);
            if (w_cycle(2, res_ptr, nres, &sel)) z->width = res_opts[sel][0], z->height = res_opts[sel][1], save_later("Resolution");
        }
        ROW(3, NULL, "3D resolution", "Lower is faster, higher is sharper. Auto: as large as the screen allows.",
            z->render_w != boot_sizes.render_w || z->render_h != boot_sizes.render_h) {
            int sel = size_index(rres_opts, nrres, z->render_w, z->render_h);
            if (w_cycle(3, rres_ptr, nrres, &sel))
                z->render_w = rres_opts[sel][0], z->render_h = rres_opts[sel][1], save_later("RenderResolution");
        }
    }
    GROUP("PICTURE") {
        ROW(4, NULL, "Aspect ratio", "Auto: the window's shape. Wider than 16:9 shows more to the sides.",
            g_config.aspect_ratio != boot.aspect_ratio) {
            int sel = 0;
            for (int i = 1; i < 5; i++) if (fabsf(g_config.aspect_ratio - shape_v[i]) < 0.005f) sel = i;
            if (w_segment(4, shapes, 5, &sel)) g_config.aspect_ratio = shape_v[sel], save_later("AspectRatio");
        }
        ROW(5, NULL, "HUD position", "On a picture wider than 16:9: in the middle, or spread toward the sides.",
            g_config.ui_anchor != boot.ui_anchor) {
            int sel = g_config.ui_anchor >= UI_CENTER && g_config.ui_anchor <= UI_EDGES ? g_config.ui_anchor - 1 : 0;
            if (w_segment(5, huds, 4, &sel)) g_config.ui_anchor = sel + 1, save_later("UIAnchor");
        }
        ROW(6, NULL, "Field of view", "1 is the game's own; more shows more around the hunter.", false) {
            if (w_slider(6, &g_config.fov, 0.5f, 2.0f, 0.01f, true, "%.2f\xc3\x97")) g_fov_scale = g_config.fov, save_later("FOV");
        }
    }
}

static void page_graphics(void) {
    static const char *const msaa[] = {"Off", "2x", "4x", "8x"};
    GROUP(NULL) {
        ROW(1, NULL, "Frame rate cap", "30 is the Switch's rate. Above your monitor's refresh rate, turn VSync off.", false) {
            float f = (float)g_config.fps;
            if (w_slider(1, &f, 30.0f, 240.0f, 1.0f, false, "%.0f fps")) g_config.fps = (int)f, save_later("FPS");
        }
        ROW(2, NULL, "VSync", "No tearing, but no more frames than the monitor shows.", false) {
            if (w_toggle(2, &g_config.vsync)) {
                save_later("VSync");
#ifndef MHGU_NULL_BACKEND
                vk_request_present_mode();
#endif
            }
        }
    }
    GROUP("EDGE SMOOTHING") {
        ROW(3, NULL, "MSAA", "Smooths the edges of 3D shapes. Each step costs GPU time, a lot on older cards.", false) {
            int sel = g_config.msaa >= 8 ? 3 : g_config.msaa >= 4 ? 2 : g_config.msaa >= 2 ? 1 : 0;
            if (w_segment(3, msaa, 4, &sel)) {
                g_config.msaa = 1 << sel;
                save_later("MSAA");
#ifndef MHGU_NULL_BACKEND
                vk_set_msaa((uint32_t)g_config.msaa);
#endif
            }
        }
        ROW(4, NULL, "FXAA", "The game's own smoothing filter over the finished picture; slightly soft.", false) {
            if (w_toggle(4, &g_config.fxaa)) save_later("FXAA");
        }
    }
}

static void page_gameplay(void) {
    static const char *const keys[4] = {"1Player", "2Players", "3Players", "4Players"};
    static const char *const names[4] = {"1 hunter", "2 hunters", "3 hunters", "4 hunters"};
    note("Monster HP factors multiply each quest's own large-monster HP (1 = the original game); only HP changes. They "
         "apply from the next quest. In a hub, everyone uses the host's.");
    GROUP("LARGE MONSTER HP IN HUB QUESTS") {
        for (int i = 0; i < 4; i++)
            ROW(10 + i, NULL, names[i], NULL, false) {
                if (w_slider(10 + i, &g_config.hp.scale[i], HP_SCALE_MIN, HP_SCALE_MAX, 0.01f, true, "%.2f\xc3\x97")) save_later(keys[i]);
            }
    }
    GROUP("VILLAGE QUESTS") {
        ROW(1, NULL, "Village quests at the Hub",
            "At the Hub's and the Hunters' Pub's quest counter, L or R switches to the Village quests, to play them together.", false) {
            if (w_toggle(1, &g_config.village_in_hub)) save_later("VillageQuestsInHub");
        }
        ROW(2, NULL, "Their HP in the Hub", "Village quests are made for one hunter: their monsters' HP is multiplied by this first.", false) {
            if (w_slider(2, &g_config.hp.village, HP_SCALE_MIN, HP_SCALE_MAX, 0.01f, true, "%.2f\xc3\x97")) save_later("HubHP");
        }
    }
    GROUP("DOWNLOADS") {
        ROW(3, NULL, "Bonus content",
            "Copies the free downloads (event quests, item packs) from a donor save in the bonus folder into a save that "
            "has none.", g_config.bonus_content != boot.bonus_content) {
            if (w_toggle(3, &g_config.bonus_content)) save_later("BonusContent");
        }
    }
}

static void page_network(void) {
    GROUP(NULL) {
        ROW(1, NULL, "Sync small monsters",
            "Small monsters move the same for everyone in a zone, like large ones. Everyone in the hunt should use the same.", false) {
            if (w_toggle(1, &g_config.sync_small_monsters)) save_later("SyncSmallMonsters");
        }
        ROW(2, NULL, "Port", "The port others reach your hub on (TCP and UDP). Forward it in your router to host over the internet.",
            g_config.ldn_port != boot.ldn_port) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", g_config.ldn_port);
            if (w_text(2, buf, sizeof buf, px(110), true, "39500")) {
                int p = atoi(buf);
                if (p > 0 && p < 65536) g_config.ldn_port = p, save_later("Port");
                else status(false, "A port is a number from 1 to 65535");
            }
        }
        ROW(3, NULL, "Join", "Hosts outside your LAN: an address or name, with :port if it isn't the Port above. Commas between several.",
            false) {
            if (w_text(3, g_config.ldn_join, sizeof g_config.ldn_join, px(260), false, "203.0.113.7:39500")) save_later("Join");
        }
    }
}

// ================================================================== the frame
static void hint_line(const char *key, const char *what) {
    CLAY({.layout = {.padding = {pxu(8), 0, 0, 0}, .childGap = pxu(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(px(22)), CLAY_SIZING_FIT(0)}, .padding = {pxu(6), pxu(6), pxu(2), pxu(3)},
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
              .backgroundColor = C_BADGE, .cornerRadius = CLAY_CORNER_RADIUS(px(5))}) {
            CLAY_TEXT(lit(key), tcn(F_SEMI, 11.5f, C_TEXT2));
        }
        CLAY_TEXT(lit(what), tcn(F_REG, 12, C_FAINT));
    }
}

static void sidebar(void) {
    CLAY({.id = CLAY_ID("Side"),
          .layout = {.sizing = {CLAY_SIZING_FIXED(px(210)), CLAY_SIZING_GROW(0)}, .padding = {pxu(14), pxu(14), pxu(24), pxu(18)},
                     .childGap = pxu(3), .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = C_SIDE, .cornerRadius = {.topLeft = px(18), .bottomLeft = px(18)}}) {
        CLAY({.layout = {.padding = {pxu(10), pxu(10), 0, pxu(22)}, .childGap = pxu(2), .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            CLAY_TEXT(CLAY_STRING("MHGU Recomp"), tcn(F_SEMI, 18, C_TEXT));
            CLAY_TEXT(CLAY_STRING("Settings"), tcn(F_REG, 13, C_MUTED));
        }
        for (int i = 0; i < NPAGES; i++) {
            Clay_ElementId id = CLAY_IDI("nav", i);
            interact(id);
            if (clicked(id) && page != i) page = i, focus_id = 0, kb_nav = false, scroll_reset = true;
            bool sel = page == i;
            float h = anim(id.id, over(id) ? 1.0f : 0.0f, 22.0f);
            CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(px(38))}, .padding = {pxu(10), pxu(10), 0, 0},
                                       .childGap = pxu(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = sel ? C_ACCENT_SOFT : alpha(C_HOVER2, h * 0.7f), .cornerRadius = CLAY_CORNER_RADIUS(px(10))}) {
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(px(3)), CLAY_SIZING_FIXED(px(16))}}, .backgroundColor = sel ? C_ACCENT : C_NONE,
                      .cornerRadius = CLAY_CORNER_RADIUS(px(2))}) {}
                CLAY_TEXT(lit(page_names[i]), tcn(sel ? F_SEMI : F_REG, 14.5f, sel ? C_TEXT : C_MUTED));
            }
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        hint_line("`", "Close");
        hint_line("Tab", "Next page");
        hint_line("Esc", "Back");
    }
}

static void header(void) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {pxu(30), pxu(30), pxu(26), pxu(16)},
                     .childGap = pxu(16), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = pxu(5), .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            CLAY_TEXT(lit(page_names[page]), tcn(F_SEMI, 25, C_TEXT));
            CLAY_TEXT(lit(page_desc[page]), tc(F_REG, 13.5f, C_MUTED));
        }
        if (page == PAGE_CONTROLS) {
            static const char *const tabs[] = {"Keyboard & mouse", "Controller"};
            row_id = 1, row_cols = 0;  // (a row of its own for the focus)
            if (w_segment(900, tabs, 2, &ctl_tab)) cap_cancel(), scroll_reset = true;
            row_id = 0;
        }
    }
}

static void footer(void) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {pxu(30), pxu(20), pxu(12), pxu(14)},
                     .childGap = pxu(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .border = {C_PANEL_LINE, {.top = 1}}}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = pxu(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            bool recent = status_ns && now_ns - status_ns < 4500000000ull, saved = saved_ns && now_ns - saved_ns < 2500000000ull;
            Clay_Color dot = recent ? (status_good ? C_GOOD : C_ACCENT) : restart_pending() ? C_ACCENT : saved ? C_GOOD : C_FAINT;
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(px(7)), CLAY_SIZING_FIXED(px(7))}}, .backgroundColor = dot,
                  .cornerRadius = CLAY_CORNER_RADIUS(px(3.5f))}) {}
            if (recent) CLAY_TEXT(str("%s", status_text), tc(F_REG, 13, C_TEXT2));
            else if (restart_pending()) CLAY_TEXT(CLAY_STRING("Some changes apply after a restart of the game"), tc(F_REG, 13, C_ACCENT));
            else if (saved) CLAY_TEXT(CLAY_STRING("Saved to mhgurecomp.ini"), tc(F_REG, 13, C_TEXT2));
            else CLAY_TEXT(CLAY_STRING("Changes are saved as you make them"), tc(F_REG, 13, C_FAINT));
        }
        row_id = 2, row_cols = 0;  // (the buttons: a row of their own for the focus)
        if (page == PAGE_CONTROLS) {
            bool armed = reset_armed_ns && now_ns - reset_armed_ns < 3000000000ull;
            if (w_button(950, armed ? "Click again to reset" : "Reset to defaults", false)) {
                if (!armed) {
                    reset_armed_ns = now_ns;
                } else {
                    reset_armed_ns = 0;
                    Bindings d;
                    bindings_defaults(&d);
                    if (ctl_tab == 0) memcpy(g_bind.key, d.key, sizeof d.key);
                    else memcpy(g_bind.pad, d.pad, sizeof d.pad);
                    binds_changed();
                    status(true, ctl_tab == 0 ? "Keyboard and mouse controls are back to the defaults"
                                              : "Controller buttons are back to the defaults");
                }
            }
        }
        if (w_button(951, "Done", true)) want_open = false;
        row_id = 0;
    }
}

// ---- render commands -> quads
typedef struct { MenuPrim *p; uint32_t n, cap; MenuBatch *b; uint32_t nb, capb; } List;
static List lists[2];
static int back;            // the list being built (the other one is shown)
static List *out;
static int clip[4], clip_stack[8][4], nclip;
static float slide;          // the open / close animation's offset (pixels, down)
static bool shown_valid;
static MenuFrame shown;

static MenuPrim *prim(void) {
    if (out->n == out->cap) out->p = realloc(out->p, (out->cap = out->cap ? out->cap * 2 : 4096) * sizeof *out->p);
    MenuBatch *b = out->nb ? &out->b[out->nb - 1] : NULL;
    if (!b || memcmp(b->clip, clip, sizeof clip)) {
        if (out->nb == out->capb) out->b = realloc(out->b, (out->capb = out->capb ? out->capb * 2 : 64) * sizeof *out->b);
        b = &out->b[out->nb++];
        b->first = out->n, b->count = 0;
        memcpy(b->clip, clip, sizeof clip);
    }
    b->count++;
    MenuPrim *q = &out->p[out->n++];
    memset(q, 0, sizeof *q);
    return q;
}

static void color(float o[4], Clay_Color c) {
    o[0] = c.r / 255.0f, o[1] = c.g / 255.0f, o[2] = c.b / 255.0f, o[3] = c.a / 255.0f * fade;
}

static void put_rect(Clay_BoundingBox b, Clay_CornerRadius r, Clay_Color fill, Clay_Color line, float width, float dy) {
    MenuPrim *q = prim();
    float lim = fminf(b.width, b.height) * 0.5f;
    q->rect[0] = b.x, q->rect[1] = b.y + dy, q->rect[2] = b.x + b.width, q->rect[3] = b.y + b.height + dy;
    q->radius[0] = fminf(r.topLeft, lim), q->radius[1] = fminf(r.topRight, lim);
    q->radius[2] = fminf(r.bottomRight, lim), q->radius[3] = fminf(r.bottomLeft, lim);
    color(q->color, fill);
    color(q->border_color, line);
    q->border = width;
    q->mode = MENU_PRIM_RECT;
}

static void put_shadow(Clay_BoundingBox b, float radius, float blur, float dy) {
    MenuPrim *q = prim();
    q->rect[0] = b.x, q->rect[1] = b.y + dy, q->rect[2] = b.x + b.width, q->rect[3] = b.y + b.height + dy;
    for (int i = 0; i < 4; i++) q->radius[i] = radius;
    color(q->color, C_SHADOW);
    q->soft = blur;
    q->mode = MENU_PRIM_SHADOW;
}

static void put_text(Clay_BoundingBox b, const Clay_TextRenderData *t) {
    const Face *f = face(t->fontId, t->fontSize);
    float lh = t->lineHeight ? t->lineHeight : roundf(f->line), x = roundf(b.x);
    float y = roundf(b.y + slide + (lh - (f->ascent - f->descent)) * 0.5f + f->ascent);
    const char *s = t->stringContents.chars, *end = s + t->stringContents.length;
    for (int prev = -1; s < end;) {
        int g = glyph(utf8_next(&s, end));
        if (prev >= 0) x += kern(f, prev, g);
        prev = g;
        const stbtt_packedchar *c = &f->ch[g];
        if (c->x1 > c->x0 && c->y1 > c->y0) {
            MenuPrim *q = prim();
            float x0 = floorf(x + c->xoff + 0.5f), y0 = floorf(y + c->yoff + 0.5f);
            q->rect[0] = x0, q->rect[1] = y0, q->rect[2] = x0 + (c->x1 - c->x0), q->rect[3] = y0 + (c->y1 - c->y0);
            q->uv[0] = c->x0, q->uv[1] = c->y0, q->uv[2] = c->x1, q->uv[3] = c->y1;
            color(q->color, t->textColor);
            q->mode = MENU_PRIM_TEXT;
            q->gamma = 0.8f;  // (light text on dark: a little more weight)
        }
        x += c->xadvance + t->letterSpacing;
    }
}

static void emit(Clay_RenderCommandArray cmds, int w, int h) {
    out = &lists[back];
    out->n = out->nb = 0;
    clip[0] = clip[1] = 0, clip[2] = w, clip[3] = h, nclip = 0;
    uint32_t root = CLAY_ID("Root").id, panel = CLAY_ID("Panel").id;
    for (int i = 0; i < cmds.length; i++) {
        Clay_RenderCommand *c = Clay_RenderCommandArray_Get(&cmds, i);
        Clay_BoundingBox b = c->boundingBox;
        switch (c->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            const Clay_RectangleRenderData *r = &c->renderData.rectangle;
            if (c->id == panel) {  // (a soft shadow under the panel, two layers)
                put_shadow(b, px(18), px(36), slide + px(14));
                put_shadow(b, px(18), px(8), slide + px(2));
            }
            put_rect(b, r->cornerRadius, r->backgroundColor, C_NONE, 0, c->id == root ? 0 : slide);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            const Clay_BorderRenderData *r = &c->renderData.border;
            const Clay_BorderWidth *bw = &r->width;
            if (bw->left == bw->right && bw->left == bw->top && bw->left == bw->bottom) {
                put_rect(b, r->cornerRadius, C_NONE, r->color, bw->left, slide);
                break;
            }
            Clay_CornerRadius none = {0};
            if (bw->left) put_rect((Clay_BoundingBox){b.x, b.y, bw->left, b.height}, none, r->color, C_NONE, 0, slide);
            if (bw->right) put_rect((Clay_BoundingBox){b.x + b.width - bw->right, b.y, bw->right, b.height}, none, r->color, C_NONE, 0, slide);
            if (bw->top) put_rect((Clay_BoundingBox){b.x, b.y, b.width, bw->top}, none, r->color, C_NONE, 0, slide);
            if (bw->bottom) put_rect((Clay_BoundingBox){b.x, b.y + b.height - bw->bottom, b.width, bw->bottom}, none, r->color, C_NONE, 0, slide);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT:
            put_text(b, &c->renderData.text);
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            if (nclip < 8) memcpy(clip_stack[nclip++], clip, sizeof clip);
            clip[0] = (int)fmaxf((float)clip[0], floorf(b.x)), clip[1] = (int)fmaxf((float)clip[1], floorf(b.y + slide));
            clip[2] = (int)fminf((float)clip[2], ceilf(b.x + b.width)), clip[3] = (int)fminf((float)clip[3], ceilf(b.y + b.height + slide));
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            if (nclip) memcpy(clip, clip_stack[--nclip], sizeof clip);
            break;
        default:
            break;
        }
    }
    // the scroll area's bar, while its content is taller than it
    Clay_ElementId sid = CLAY_ID("Scroll");
    Clay_ScrollContainerData s = Clay_GetScrollContainerData(sid);
    Clay_ElementData v = Clay_GetElementData(sid);
    float vh = s.scrollContainerDimensions.height, ch = s.contentDimensions.height;
    if (s.found && v.found && s.scrollPosition && ch > vh + 1) {
        float th = fmaxf(px(36), vh * vh / ch), y = v.boundingBox.y + (-s.scrollPosition->y) / (ch - vh) * (vh - th);
        put_rect((Clay_BoundingBox){v.boundingBox.x + v.boundingBox.width - px(9), y + px(2), px(4), th - px(4)},
                 CLAY_CORNER_RADIUS(px(2)), (Clay_Color){255, 255, 255, 40}, C_NONE, 0, slide);
    }
}

static void publish(bool empty) {
    pthread_mutex_lock(&frame_lock);
    if (empty) {
        shown_valid = false;
    } else {
        List *l = &lists[back];
        shown.prims = l->p, shown.nprims = l->n, shown.batches = l->b, shown.nbatches = l->nb;
        shown_valid = l->n > 0;
        back ^= 1;
    }
    pthread_mutex_unlock(&frame_lock);
}

const MenuFrame *menu_frame_take(void) {
    pthread_mutex_lock(&frame_lock);
    if (!shown_valid) { pthread_mutex_unlock(&frame_lock); return NULL; }
    shown.atlas = atlas, shown.atlas_w = (uint32_t)atlas_w, shown.atlas_h = (uint32_t)atlas_h, shown.atlas_gen = atlas_gen;
    return &shown;
}
void menu_frame_done(void) { pthread_mutex_unlock(&frame_lock); }

static void clay_error(Clay_ErrorData e) {
    static int n;
    if (n++ < 20) LOG("[menu] Clay: %.*s\n", e.errorText.length, e.errorText.chars);
}

static void init(void) {
    inited = true;
    fonts_init();
    uint32_t size = Clay_MinMemorySize();
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(size, malloc(size)), (Clay_Dimensions){1280, 720},
                    (Clay_ErrorHandler){clay_error, NULL});
    Clay_SetMeasureTextFunction(measure, NULL);
    boot = g_config, boot_sizes = g_config_sizes;
    static const int res[][2] = {{0, 0}, {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
    static const int rres[][2] = {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
    size_list(res_opts, &nres, res_lab, res_ptr, res, 9, g_config_sizes.width, g_config_sizes.height, "Desktop");
    size_list(rres_opts, &nrres, rres_lab, rres_ptr, rres, 7, g_config_sizes.render_w, g_config_sizes.render_h, "Auto");
}

static void build(int w, int h) {
    float s = h / 720.0f;
    s = s < 0.6f ? 0.6f : s > 4.0f ? 4.0f : s;
    if (fabsf(s - S) > 0.001f) {  // (another size: every face baked again for it)
        S = s;
        pthread_mutex_lock(&frame_lock);
        nfaces = 0;
        atlas_reset(1024, 1024);
        pthread_mutex_unlock(&frame_lock);
    }
    spos = 0;
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)w, (float)h});
    Clay_SetPointerState((Clay_Vector2){in.mx, in.my}, in.down);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){0, in.wheel * px(6)}, dt);
    if (scroll_reset) {
        Clay_ScrollContainerData sc = Clay_GetScrollContainerData(CLAY_ID("Scroll"));
        if (sc.found && sc.scrollPosition) sc.scrollPosition->x = sc.scrollPosition->y = 0;
        scroll_reset = false;
    }
    nav_begin();
    Clay_BeginLayout();
    Clay_ElementId root = CLAY_ID("Root"), panel = CLAY_ID("Panel");
    interact(root);
    float pw = fminf(px(1040), (float)w - px(32)), ph = fminf(px(650), (float)h - px(32));
    CLAY({.id = root, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_BACKDROP}) {
        CLAY({.id = panel, .layout = {.sizing = {CLAY_SIZING_FIXED(pw), CLAY_SIZING_FIXED(ph)}}, .backgroundColor = C_PANEL,
              .cornerRadius = CLAY_CORNER_RADIUS(px(18)), .border = {C_PANEL_LINE, {1, 1, 1, 1, 0}}}) {
            interact(panel);
            sidebar();
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                header();
                scroll_from = norder;
                CLAY({.id = CLAY_ID("Scroll"),
                      .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = {pxu(30), pxu(30), pxu(2), pxu(24)},
                                 .childGap = pxu(22), .layoutDirection = CLAY_TOP_TO_BOTTOM},
                      .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
                    switch (page) {
                    case PAGE_CONTROLS: page_controls(); break;
                    case PAGE_MOUSE: page_mouse(); break;
                    case PAGE_DISPLAY: page_display(); break;
                    case PAGE_GRAPHICS: page_graphics(); break;
                    case PAGE_GAMEPLAY: page_gameplay(); break;
                    case PAGE_NETWORK: page_network(); break;
                    }
                }
                scroll_to = norder;
                footer();
            }
        }
    }
    Clay_RenderCommandArray cmds = Clay_EndLayout();
    if (clicked(root)) want_open = false;  // (a click beside the panel closes the menu)
    if (in.released) press_id = 0;
    nav_end();
    emit(cmds, w, h);
    publish(false);
}

// ================================================================== input and the loop
static void set_open(bool open) {
    if (open == want_open) return;
    want_open = open;
}

static void opened_or_closed(bool open) {  // (once per change, from menu_update)
    SDL_Window *win = ui_window();
    if (open) {
        if (!inited) init();
        SDL_ShowCursor();
        kb_nav = false, press_id = 0, edit_id = 0;
        memset(&ev, 0, sizeof ev);
        float x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        float d = win ? SDL_GetWindowPixelDensity(win) : 1.0f;
        ev.mx = x * d, ev.my = y * d;
        LOG("[menu] open\n");
    } else {
        cap_cancel();
        edit_id = 0;
        save_flush(true);
        if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) SDL_HideCursor();
        LOG("[menu] closed\n");
    }
}

bool menu_is_open(void) { return want_open; }
bool menu_wants_text(void) { return want_open && edit_id; }

bool menu_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_GRAVE && !textinput_active() &&
        !(want_open && edit_id)) {
        cap_cancel();
        set_open(!want_open);
        dirty_frame = true;
        return true;
    }
    if (!want_open) return false;
    SDL_Window *win = ui_window();
    float d = win ? SDL_GetWindowPixelDensity(win) : 1.0f;
    dirty_frame = true;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode sc = e->key.scancode;
        bool port_key = sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12 ||
                        ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && (e->key.mod & SDL_KMOD_ALT));
        if (port_key && cap.act < 0) return false;  // (full screen, screenshots: the port's own keys still work)
        if (cap.act >= 0) {
            if (!e->key.repeat && sc == SDL_SCANCODE_ESCAPE) cap_cancel(), status(false, "Nothing changed");
            else if (!e->key.repeat && !cap.pad) cap_key(sc);
            return true;
        }
        if (edit_id) {
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_TAB) edit_done = 1;
            else if (sc == SDL_SCANCODE_ESCAPE) edit_done = 2;
            else if (sc == SDL_SCANCODE_BACKSPACE) {
                size_t n = strlen(edit_buf);
                while (n && (edit_buf[n - 1] & 0xC0) == 0x80) n--;  // (a whole UTF-8 character)
                if (n) edit_buf[n - 1] = 0;
            }
            return true;
        }
        bool shift = e->key.mod & SDL_KMOD_SHIFT;
        switch (sc) {
        case SDL_SCANCODE_ESCAPE: if (!e->key.repeat) ev.back = true; break;
        case SDL_SCANCODE_UP: ev.dy--; break;
        case SDL_SCANCODE_DOWN: ev.dy++; break;
        case SDL_SCANCODE_LEFT: ev.dx--; break;
        case SDL_SCANCODE_RIGHT: ev.dx++; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if (!e->key.repeat) ev.activate = true; break;
        case SDL_SCANCODE_TAB: ev.page += shift ? -1 : 1; break;
        case SDL_SCANCODE_PAGEUP: ev.page--; break;
        case SDL_SCANCODE_PAGEDOWN: ev.page++; break;
        case SDL_SCANCODE_DELETE: case SDL_SCANCODE_BACKSPACE: ev.clear = true; break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_TEXT_INPUT:
        if (edit_id)
            for (const char *t = e->text.text; *t; t++) {
                size_t n = strlen(edit_buf);
                if ((edit_digits && (*t < '0' || *t > '9')) || n + 1 >= (edit_digits ? 6 : sizeof edit_buf)) continue;
                edit_buf[n] = *t, edit_buf[n + 1] = 0;
            }
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        ev.mx = e->motion.x * d, ev.my = e->motion.y * d;
        if (fabsf(e->motion.xrel) + fabsf(e->motion.yrel) > 1.5f) ev.moved = true;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (cap.act >= 0 && !cap.pad) {
            if (e->button.button >= 1 && e->button.button <= 5) cap_key(KEY_MOUSE + e->button.button);
            return true;
        }
        if (cap.act >= 0) { cap_cancel(); status(false, "Nothing changed"); return true; }  // (a click stops waiting for the controller)
        ev.mx = e->button.x * d, ev.my = e->button.y * d;
        if (e->button.button == SDL_BUTTON_LEFT) ev.pressed = ev.down = true;
        if (e->button.button == SDL_BUTTON_RIGHT) ev.rpressed = true;
        ev.moved = true;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) ev.released = true, ev.down = false;
        return true;
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        if (cap.act >= 0 && !cap.pad) {
            if (y != 0) cap_key(y > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN);
        } else {
            ev.wheel += y;
        }
        return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        cap_cancel();
        ev.down = false;
        return false;
    default:
        return false;
    }
}

// The controller moves through the menu: d-pad / left stick, A, B, X clears, L / R pages (held directions repeat).
static void pad_nav(void) {
    enum { UP, DOWN, LEFT, RIGHT, A, B, X, L, R, N };
    static bool was[N];
    static uint64_t since[N], last[N];
    bool held[N] = {0};
    if (pad_hold && bind_pad_held(cur_pad) < 0) pad_hold = false;
    if (cur_pad && !pad_hold && !(cap.act >= 0 && cap.pad)) {
        float lx = SDL_GetGamepadAxis(cur_pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f, ly = SDL_GetGamepadAxis(cur_pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        static const int btn[N] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_L, BTN_R};
        for (int i = 0; i < N; i++) held[i] = bind_pad_down(cur_pad, bind_pad_code(btn[i], cur_pad));
        held[UP] |= ly < -0.55f, held[DOWN] |= ly > 0.55f, held[LEFT] |= lx < -0.55f, held[RIGHT] |= lx > 0.55f;
    }
    for (int i = 0; i < N; i++) {
        bool fire = held[i] && !was[i];
        if (fire) since[i] = last[i] = now_ns;
        else if (held[i] && i <= RIGHT && now_ns - since[i] > 380000000ull && now_ns - last[i] > 75000000ull) fire = true, last[i] = now_ns;
        was[i] = held[i];
        if (!fire) continue;
        dirty_frame = true;
        switch (i) {
        case UP: ev.dy--; break;
        case DOWN: ev.dy++; break;
        case LEFT: ev.dx--; break;
        case RIGHT: ev.dx++; break;
        case A: ev.activate = true; break;
        case B: ev.back = true; break;
        case X: ev.clear = true; break;
        case L: ev.page--; break;
        case R: ev.page++; break;
        }
    }
}

void menu_update(SDL_Gamepad *pad) {
    static bool was_open;
    if (want_open != was_open) opened_or_closed(was_open = want_open);
    if (!inited) return;
    now_ns = SDL_GetTicksNS();
    dt = last_ns ? (float)(now_ns - last_ns) * 1e-9f : 1.0f / 60;
    dt = dt > 0.1f ? 0.1f : dt;
    last_ns = now_ns;
    cur_pad = pad;
    save_flush(false);
    float target = want_open ? 1.0f : 0.0f;
    if (fade != target) {
        fade += (target > fade ? 1.0f : -1.0f) * dt / 0.14f;
        fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
        dirty_frame = true;
    }
    if (!want_open && fade <= 0.0f) {
        if (shown_valid) publish(true);
        return;
    }
    if (want_open) {
        cap_pad();
        pad_nav();
    }
    // a frame when something changed, else at a slow pace (a status line times out, a field blinks)
    if (!dirty_frame && now_ns - last_build_ns < 100000000ull) return;
    if (now_ns - last_build_ns < 4000000ull) return;  // (at most 250 a second)
    int w = 0, h = 0;
    if (!ui_window() || !SDL_GetWindowSizeInPixels(ui_window(), &w, &h) || w <= 0 || h <= 0) return;
    in = ev;
    ev.pressed = ev.released = ev.rpressed = ev.moved = false;
    ev.wheel = 0, ev.dy = ev.dx = ev.page = 0;
    ev.activate = ev.back = ev.clear = false;
    if (!want_open) memset(&in, 0, sizeof in), in.mx = ev.mx, in.my = ev.my;  // (closing: nothing reacts)
    if (in.back) {
        if (cap.act >= 0) cap_cancel();
        else if (edit_id) edit_done = 2;
        else want_open = false;
    }
    if (in.page) {
        page = ((page + in.page) % NPAGES + NPAGES) % NPAGES;
        focus_id = 0, nprev = 0, cap_cancel(), edit_id = 0, scroll_reset = true;
    }
    float e = 1.0f - fade;
    slide = px(14) * e * e;
    dirty_frame = false;
    last_build_ns = now_ns;
    build(w, h);
}
