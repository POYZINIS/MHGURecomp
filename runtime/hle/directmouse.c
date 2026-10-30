// Direct mouse ([Mouse] DirectMouse = On, the default): the mouse as a PC player expects it, fed into the game's
// own state instead of a simulated controller.
// - Menus: the pointer is free; the item under it is the menu's selection (the menu's cursor is set to it, with the
//   move sound and the refresh a move brings), a left click confirms it (A), the right button goes back (B).
// - Quests: mouse movement turns the camera itself - its yaw and pitch - at the start of the camera's update.
// - Towns (Hub, villages, Pub): the camera stays fixed as in the original; the pointer stays free, left click A.
// The window loop (runtime/host_input.c) owns the pointer, the buttons and the movement; this file is the game's side
// (tools/gen.py PATCHES).
//
// The game has no pointer support on the Switch (the GUI's "Mouse Reaction" flag and sMouse are never read, the 3DS
// touch panels get no input), so a menu's items are found from its own pieces:
// - its input: cCursor's update (the start menu, lists, the character select...) and the Yes/No choice of the
//   confirmation boxes; each names a menu input that runs this frame.
// - where its items are: cUIObject's three helpers that put a menu's highlight on its row / column / cell give the
//   GUI instances of the highlight and of the "next" marker one step beside it (the layouts' Null_cursor /
//   Null_cursor_next): their canvas positions give the grid. Some menus (the item pouch and 23 more) put the
//   highlight on its row themselves: uGUIResourceOwner's setY called from those places (INLINE_ROWS) gives the
//   highlight and row x step. The confirmation box has Yes and No left and right of its answer instance.
//   Menus without a highlight of their own (the title menu, the character select...) show the selection as an
//   animation of the item itself (SELECT among DEFAULT): their items are the visible sibling animation instances of
//   the GUI the menu sets sequences in (uGUIResourceOwner's setSequence, 0x746c74), the selected one the odd one out;
//   in screen order they are the cursor's items.
// Moving the pointer selects the item under it; a still pointer leaves the d-pad / keyboard alone.
//   MHGU_DIRECTMOUSE_LOG=1   log the menus' inputs, highlights and the pointer's selections
#include <math.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "directmouse.h"

// ---- the game's menus (main v1.4.0)
// cCursor (vtable 0x196cac8): +0x08 the last move (1 left, 2 right, 4 up, 8 down: column -/+, row -/+), +0x0d column
// (u8), +0x0e columns (u8), +0x10 row (u16), +0x12 rows (u16), +0x14 items (u16), +0x18 / +0x1c the column / row
// axis's buttons (CURSOR_MODES index). Column and row are the cursor's own terms: the start menu's 8 entries are its
// columns, its 2 pages its rows (an index is row x columns + column). Its update returns the button that moved it,
// which the menus refresh on.
#define CUR_VTABLE 0x0196cac8u
#define CUR_MOVE 0x08u
#define CUR_COL 0x0du
#define CUR_COLS 0x0eu
#define CUR_ROW 0x10u
#define CUR_ROWS 0x12u
#define CUR_ITEMS 0x14u
#define CUR_MODE 0x18u  // + 4 x axis (0 column, 1 row)
// Axis modes: 16 bytes each - the "previous" and "next" buttons in the game's pad bits (0x10 up, 0x40 down, 0x80
// left, 0x20 right; 0x80000 / 0x20000 left / right on the stick; 0x100 / 0x200 L / R), the move sound's kind (0:
// SND_PLAY(sound id)) and id
#define CURSOR_MODES 0x0182d0c0u
#define CURSOR_NMODES 12
#define PAD_UP 0x10u
#define PAD_LEFT (0x80u | 0x80000u)
// cBoxCursor (vtable 0x196cb9c; the item box's and equipment box's pages of cells): +0x08 the last move (1 back, 2
// on), +0x14 / +0x15 column / row, +0x16 / +0x17 columns / rows, +0x18 / +0x1a page / pages, +0x1c items. Its update
// (0x7530b8) returns the pad bit of the move (0x10 up, 0x40 down, 0x80 left, 0x20 right) and plays sound 4.
#define BOX_VTABLE 0x0196cb9cu
#define BOX_MOVE 0x08u
#define BOX_COL 0x14u
#define BOX_ROW 0x15u
#define BOX_COLS 0x16u
#define BOX_ROWS 0x17u
#define BOX_PAGE 0x18u
#define BOX_PAGES 0x1au
#define BOX_ITEMS 0x1cu
#define BOX_SOUND 4u
// The menu sounds: SND_PLAY(*SND_MANAGER, id) as cCursor's update plays a move; the Yes/No choice's is id 4
#define SND_MANAGER 0x01a96f5cu
#define SND_PLAY 0x006eeb78u
#define SND_YESNO 4u
// GUI instances: +0x10 the world matrix (row-major; +0x10 x scale, +0x24 y scale, +0x40 / +0x44 position on the
// canvas, y down), +0x68 the parent. uGUIResourceOwner (a cUIObject's +0x24): instances +0xf8 (count +0x298), then
// +0x294 for the ids past them.
#define INST_SX 0x10u
#define INST_SY 0x24u
#define INST_X 0x40u
#define INST_Y 0x44u
#define INST_PARENT 0x68u
#define INST_POSX 0x90u    // the position the setters set, in the parent's space (the placement helpers' steps)
#define INST_POSY 0x94u
#define INST_FLAGS 0x54u   // bit 0 visible
#define INST_ANIM 0xdcu    // animation instances: their child GUI; its +0x94 the current sequence (id at +0)
#define ANIM_VTABLE 0x0198f5b8u  // cGUIInstAnimation
#define UIO_OWNER 0x24u
#define OWN_INST 0xf8u
#define OWN_INST2 0x294u
#define OWN_NINST 0x298u
// uGUIResourceOwner's setY(owner, id, y in s0) at 0x745ac0, called with "row x step" for a highlight from these
// places (the return addresses): the value's row is the cursor's column (+0x0d)
static const uint32_t INLINE_ROWS[] = {
    0x77c8c8, 0x77e044, 0x789ca4, 0x795ddc, 0x7ac32c, 0x7aec28, 0x7b18e4, 0x7b190c, 0x7bb834, 0x7c571c, 0x7d012c, 0x80fc14,
    0x80fe70, 0x810130, 0x81fdf4, 0x8201e4, 0x820b44, 0x826aa0, 0x82f6f0, 0x832118, 0x838070, 0x8380b0, 0x838110, 0x83a298,
};
// cUIOWindowDialog (vtable 0x1978e54): the Yes/No choice byte at +0x6c, its GUI (+0x04 rGUI, path at +0x08)
// window_dialog.gui with instance 2 = Anm_answer: Yes left of its position, No right, the row below it
#define DIALOG_VTABLE 0x01978e54u
#define DIALOG_CHOICE 0x6cu
#define DIALOG_ANSWER 2u
#define RES_PATH 0x08u
// sGameControl * (a global): +0x4c the area type, 0 Hub, 1..6 villages, 7 Hunters' Pub, quests above
#define G_GAMECONTROL 0x01a8ca48u
#define GC_AREA 0x4cu

// ---- the quest camera: uQuestCamera (vtable 0x1930d0c) keeps its angles as 16-bit turns (0x10000 = 360 degrees):
// pitch at +0x9d8 (current), +0x9e0 (last frame), +0x9e8 (goal), yaw at 4 past each. Its update (0x21fec4) eases the
// current toward the goal the stick sets. Pitch stays between the stick's ends (measured: 0xd100 down .. 0x2800 up).
#define QCAM_VTABLE 0x01930d0cu
#define QCAM_ANGLES 0x9d8u  // 3 x {pitch, yaw}, 8 bytes apart
#define QCAM_PITCH_MIN (-0x2f00)
#define QCAM_PITCH_MAX 0x2800
#define CAM_TURN_PER_COUNT 16.0f       // at MouseSensitivity 1: 4096 counts a full turn
#define CAM_FIX 16                     // the movement waits as count x CAM_FIX
#define CAM_FRESH_NS 200000000ull      // the camera updated this recently: it runs

#define FRESH_FRAMES 4      // a menu input this recent is open
#define PLACE_FRAMES 120    // a highlight placed this recently still stands (some menus place it only on a move)
#define CLICK_NS 600000000ull  // a click waits this long for its item to become the selection

enum { IN_NONE, IN_CURSOR, IN_YESNO, IN_BOX };
enum { PL_ROW, PL_COL, PL_CELL, PL_YESNO };

#define NSEQ 16
static struct {  // the GUIs animation sequences were set in lately (game thread): where menus' items are looked for
    uint32_t owner;
    uint64_t frame;
    uint32_t inst[8];  // the last instances set (a menu's items are among them)
    int ninst;
} seq_owner[NSEQ];
static struct { uint32_t input, caller, owner, parent; } group_owner[4];  // where a menu input's items were: GUI, parent

static struct {  // the menu input that ran last (game thread)
    int kind;
    uint32_t obj;          // the cCursor / cBoxCursor, or the Yes/No choice's byte (0 Yes, 1 No)
    uint32_t caller;       // where it was called from (menus share a cursor between their lists: the shop's)
    uint64_t frame;
} input;
static struct { uint32_t obj, caller; uint64_t frame; } box_in;  // the box cursor that ran last (another input may
                                                                  // run after it: the smithy's equipment detail pages)

typedef struct {  // a highlight a menu input placed
    int kind;
    uint32_t input, caller;     // the input (input.obj, input.caller) it followed
    uint32_t cur, nx, ny;       // the highlight's instance and the next-column / next-row markers (0: none, `step`)
    int col, row;               // where it was put (column / row of what the menu shows)
    float step;                 // the row step of a highlight without a next marker
    uint64_t frame;
} Place;
#define NPLACES 4  // (menus with a second list beside the first: tabs, sub-menus)
static Place places[NPLACES];

typedef struct { float x0, y0, x1, y1; } Box;  // (on the canvas)
static Box inst_box(uint32_t inst);

static volatile float ptr_x, ptr_y;
static volatile int ptr_inside;
static volatile uint32_t ptr_seq;        // pointer moves
static volatile uint64_t click_ns;       // a click waiting for its item (0: none)
static volatile int confirm;             // a click reached its item: the window loop presses A
static uint32_t seen_seq;
static uint64_t move_frame;              // the frame the pointer last moved in
static bool pending;                     // it moved since it last selected an item or stood on the selection
static int miss_frames;                  // frames since, with a menu's items known and the pointer over none of them
static bool frame_known, frame_over;     // (this frame: a menu's items were known; the pointer was over one)
static uint64_t known_frame;             // the last frame a menu whose items are known ran
static volatile int32_t cam_dx, cam_dy;  // mouse movement for the camera, not applied yet (count x CAM_FIX)
static volatile uint64_t cam_ns;         // the quest camera's last update

static bool log_on(void) { return ENV("MHGU_DIRECTMOUSE_LOG") != NULL; }
// (the inspector, below)
static bool watching(void);
static void watch(void);
static const char *inst_name(uint32_t own, uint32_t inst);
static const char *gui_name(uint32_t own);
static int32_t anim_sequence(uint32_t inst);
static float f32(uint32_t a) { float f; uint32_t w = LD32(a); memcpy(&f, &w, 4); return f; }
static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }
// A GUI instance (vtables 0x196c158 .. 0x198f848 in main's rodata)
static bool inst_ok(uint32_t a) { return sane(a) && LD32(a) - 0x01960000u < 0x00040000u; }

// GUI instance `id` of a uGUIResourceOwner (as its getters look it up)
static uint32_t owner_instance(uint32_t own, uint32_t id) {
    if (!sane(own)) return 0;
    uint32_t n = LD32(own + OWN_NINST), arr = id < n ? LD32(own + OWN_INST) : LD32(own + OWN_INST2);
    if (!sane(arr)) return 0;
    uint32_t a = LD32(arr + 4u * (id < n ? id : id - n));
    return inst_ok(a) ? a : 0;
}

static uint32_t instance(uint32_t uio, uint32_t id) { return owner_instance(LD32(uio + UIO_OWNER), id); }

static void play_sound(Cpu *c, uint32_t id) {
    uint32_t mgr = LD32(SND_MANAGER);
    if (!sane(mgr)) return;
    c->r[0] = mgr, c->r[1] = id;
    guest_call(c, SND_PLAY);
}

// ---- the window loop's side
void dmouse_pointer(float x, float y, bool inside) {
    if (fabsf(x - ptr_x) >= 0.5f || fabsf(y - ptr_y) >= 0.5f || inside != ptr_inside)
        ptr_x = x, ptr_y = y, ptr_inside = inside, __atomic_add_fetch(&ptr_seq, 1, __ATOMIC_RELAXED);
}

void dmouse_click(void) { click_ns = plat_now_ns(); }

bool dmouse_take_confirm(void) { return __atomic_exchange_n(&confirm, 0, __ATOMIC_RELAXED) != 0; }

bool dmouse_menu_open(void) {
    return g_config.direct_mouse && input.kind != IN_NONE && g_present_count - input.frame <= FRESH_FRAMES;
}

bool dmouse_in_town(void) {
    if (!g_config.direct_mouse) return false;
    uint32_t gc = LD32(G_GAMECONTROL);
    return sane(gc) && LD8(gc + GC_AREA) <= 7;
}

bool dmouse_camera_active(void) { return g_config.direct_mouse && plat_now_ns() - cam_ns < CAM_FRESH_NS; }

void dmouse_camera_move(float dx, float dy) {
    __atomic_add_fetch(&cam_dx, (int32_t)lrintf(dx * CAM_FIX), __ATOMIC_RELAXED);
    __atomic_add_fetch(&cam_dy, (int32_t)lrintf(dy * CAM_FIX), __ATOMIC_RELAXED);
}

// ---- where a menu's items are
static Place *place_of(uint32_t in) {
    for (int i = 0; i < NPLACES; i++)
        if (places[i].input == in && places[i].caller == input.caller && g_present_count - places[i].frame <= PLACE_FRAMES)
            return &places[i];
    return NULL;
}

// The record for the current menu input's highlight: its own, else the oldest.
static Place *place_slot(void) {
    for (int i = 0; i < NPLACES; i++)
        if (places[i].input == input.obj && places[i].caller == input.caller) return &places[i];
    Place *p = &places[0];
    for (int i = 1; i < NPLACES; i++)
        if (places[i].frame < p->frame) p = &places[i];
    return p;
}

// The highlights menu inputs ran with lately (each the one standing for its input's selection when it last ran).
static struct { uint32_t cur, input, caller; uint64_t frame; } claims[8];

static void place_claim(const Place *p) {
    static const char *const kinds[] = {"row", "column", "cell", "yes/no"};
    int s = 0, last = -1;
    for (int i = 0; i < 8; i++)
        if (claims[i].input == p->input && claims[i].caller == p->caller && (last < 0 || claims[i].frame > claims[last].frame))
            last = i;
    for (int i = 0; i < 8; i++) {
        if (claims[i].cur == p->cur) { s = i; break; }
        if (claims[i].frame < claims[s].frame) s = i;
    }
    if (log_on() && (last < 0 || claims[last].cur != p->cur))
        LOG("[directmouse] highlight %s of %08x: instance %08x at %.0f,%.0f (scale %.0f,%.0f)\n", kinds[p->kind], p->input,
            p->cur, f32(p->cur + INST_X), f32(p->cur + INST_Y), f32(p->cur + INST_SX), f32(p->cur + INST_SY));
    claims[s].cur = p->cur, claims[s].input = p->input, claims[s].caller = p->caller, claims[s].frame = g_present_count;
}

// Whether highlight instance `cur` is another menu input's (a menu under a sub-menu keeps placing its own highlight:
// the start menu's bar beside its Palicoes list).
static bool place_claimed(uint32_t cur, uint32_t in) {
    for (int i = 0; i < 8; i++)
        if (claims[i].cur == cur && (claims[i].input != in || claims[i].caller != input.caller)) return true;
    return false;
}

// Whether a highlight stands where its menu input's selection is: its row / column one of the input's own (a long list
// counts rows on the screen, its cursor in the whole list: only a hint).
static bool place_fits(const Place *p) {
    uint32_t o = p->input;
    int c, r;
    if (LD32(o) == CUR_VTABLE) c = LD8(o + CUR_COL), r = LD16(o + CUR_ROW);
    else if (LD32(o) == BOX_VTABLE) c = LD8(o + BOX_COL), r = LD8(o + BOX_ROW);
    else return true;
    if (p->kind == PL_ROW) return p->row == c || p->row == r;
    if (p->kind == PL_COL) return p->col == c || p->col == r;
    return p->col == c && p->row == r;
}

static void place_set(Place p) {
    Place *s = place_slot();
    p.caller = input.caller;
    // two highlights placed for one menu in a frame: the one no other menu input has, else the one where the input's
    // selection is (a sub-menu's under its parent's), else the one that draws more (the combo list's bar and a mark on
    // it, every frame)
    if (s->input == p.input && s->caller == p.caller && s->frame == p.frame && s->kind == p.kind && s->cur != p.cur &&
        inst_ok(s->cur)) {
        bool ca = place_claimed(s->cur, p.input), cb = place_claimed(p.cur, p.input), fa = place_fits(s), fb = place_fits(&p);
        Box a = inst_box(s->cur), b = inst_box(p.cur);
        if (ca != cb ? cb : fa != fb ? fa : (a.x1 - a.x0) * (a.y1 - a.y0) >= (b.x1 - b.x0) * (b.y1 - b.y0)) return;
    }
    *s = p;  // (logged by place_claim as its input runs with it)
}

// The cell [o + k s, o + k s + |s|) holding v: cells start at o and repeat every s, either way.
static int cell_of(float v, float o, float s) { return s > 0 ? (int)floorf((v - o) / s) : (int)ceilf((v - o) / s); }

#define EDGE 3.0f  // a pointer this far past the selected item's edge still holds it (no flicker on the line between two)

// cell_of, holding cell `cur` while v is within EDGE of it.
static int cell_hold(float v, float o, float s, int cur) {
    int c = cell_of(v, o, s);
    float a = o + (float)cur * s, b = a + s;
    return c != cur && fminf(a, b) - EDGE <= v && v <= fmaxf(a, b) + EDGE ? cur : c;
}

// The column / row (the highlight's terms) under the pointer; false when it is beside the menu.
static bool hover(const Place *p, int *hc, int *hr) {
    if (!ptr_inside || !inst_ok(p->cur)) return false;
    float x = ptr_x, y = ptr_y;
    float cx = f32(p->cur + INST_X), cy = f32(p->cur + INST_Y);
    *hc = *hr = 0;
    if (p->kind == PL_YESNO) {  // the answers' row: Yes, then No, either side of the answer instance
        float dx = x - cx, dy = y - cy;
        if (dy < -4.0f || dy > 44.0f || fabsf(dx) < 4.0f || fabsf(dx) > 140.0f) return false;
        *hc = dx > 0;
        return true;
    }
    uint32_t par = LD32(p->cur + INST_PARENT);
    if (!inst_ok(par)) return false;
    // one step: the next marker's own position, as the helpers put the highlight at column / row x it in the parent's
    // space (the marker may hang anywhere: the food menu's is under the scaled highlight itself)
    uint32_t ry = p->kind == PL_CELL ? p->ny : p->nx;
    float sx = p->kind != PL_ROW && inst_ok(p->nx) ? f32(p->nx + INST_POSX) * f32(par + INST_SX) : 0.0f;
    float sy = p->kind == PL_COL ? 0.0f : !ry ? p->step : inst_ok(ry) ? f32(ry + INST_POSY) * f32(par + INST_SY) : 0.0f;
    float ox = cx - p->col * sx, oy = cy - p->row * sy;  // column / row 0
    // the highlight's own size across the steps (cursor.gui: a frame of half size = the instance's scale)
    float w = 2.0f * fabsf(f32(p->cur + INST_SX)), h = 2.0f * fabsf(f32(p->cur + INST_SY));
    if (p->kind == PL_ROW || p->kind == PL_CELL) {
        if (fabsf(sy) < 4.0f) return false;
        *hr = cell_hold(y, oy, sy, p->row);
    } else if (y < oy || y > oy + (h > 4.0f ? h : 32.0f)) return false;
    if (p->kind == PL_COL || p->kind == PL_CELL) {
        if (fabsf(sx) < 4.0f) return false;
        *hc = cell_hold(x, ox, sx, p->col);
    } else if (x < ox || x > ox + (w > 4.0f ? w : 320.0f)) return false;
    return true;
}

// ---- every GUI that runs and what of it shows: uGUIResourceOwner's move (0x744b88) runs once a frame for each.
// The inspector (the control file's "gui", runtime/host_input.c) logs them with the menu inputs that ran lately.
#define OWN_RES 0xf0u         // its rGUI (+0x08 the path, +0x64 the .gui file)
#define OWN_ROOT 0xf4u        // its root instance
#define RES_FILE 0x64u
#define GUIF_INSTANCES 0xbcu  // the file's instance table (0x24 bytes an entry: +0 id, +0x14 name), the next table after it
#define INST_ID 0x04u
#define INST_CHILD 0x60u
#define INST_NEXT 0x64u
#define NLIVE 256
static uint32_t live[2][NLIVE];  // the GUIs that ran: this frame, the last one (game thread)
static int nlive[2], live_cur;
static uint64_t live_frame;
static volatile int dump_request;

void dmouse_dump(void) { dump_request = 1; }

// The name the .gui file gives instance `inst` of GUI `own` (Null_cursor, Anm_answer...).
static const char *inst_name(uint32_t own, uint32_t inst) {
    uint32_t res = LD32(own + OWN_RES), file = sane(res) ? LD32(res + RES_FILE) : 0;
    if (!sane(file)) return "?";
    uint32_t tab = LD32(file + GUIF_INSTANCES), end = LD32(file + GUIF_INSTANCES + 4u), id = LD32(inst + INST_ID);
    if (!sane(tab) || end < tab || end - tab > 0x24u * 4096u) return "?";
    for (uint32_t e = tab; e + 0x24u <= end; e += 0x24u)
        if (LD32(e) == id) {
            uint32_t s = LD32(e + 0x14u);
            return s >= 0x08000000u ? (const char *)G2H(s) : "?";
        }
    return "?";
}

static const char *inst_kind(uint32_t inst) {
    static const struct { uint32_t vt; const char *name; } kinds[] = {
        {0x0198f470u, "root"}, {0x0198f4d8u, "null"}, {0x0198f548u, "scissor"}, {0x0198f5b8u, "anim"},
        {0x0198f62cu, "animvar"}, {0x0198f6a4u, "animctl"}, {0x0198f7acu, "message"}, {0x0198f848u, "text"},
        {0x0196c158u, "animparam"},
    };
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (LD32(inst) == kinds[i].vt) return kinds[i].name;
    return "?";
}

// A GUI's file name (its path's last part).
static const char *gui_name(uint32_t own) {
    uint32_t res = sane(own) ? LD32(own + OWN_RES) : 0;
    if (!sane(res)) return "?";
    const char *p = (const char *)G2H(res + RES_PATH), *s = strrchr(p, '\\');
    return s ? s + 1 : p;
}

// Whether a GUI shows: its root and one of its top instances visible.
static bool gui_shows(uint32_t own) {
    uint32_t root = LD32(own + OWN_ROOT);
    if (!inst_ok(root) || !(LD32(root + INST_FLAGS) & 1)) return false;
    for (uint32_t k = LD32(root + INST_CHILD), n = 0; inst_ok(k) && n < 512; k = LD32(k + INST_NEXT), n++)
        if (LD32(k + INST_FLAGS) & 1) return true;
    return false;
}

// Whether an instance shows: it and each instance above it visible (a menu that closes may hide only a place high up:
// the title menu's buttons stay flagged visible under the character select).
static bool inst_shows(uint32_t inst) {
    for (int d = 0; inst_ok(inst) && d < 32; inst = LD32(inst + INST_PARENT), d++)
        if (!(LD32(inst + INST_FLAGS) & 1)) return false;
    return true;
}

// ---- where an item is on the screen: an animation instance draws GUI objects (its frame's pieces, its texts,
// icons), a tree from its root (+0xdc): +0x68 first child, +0x6c next sibling, +0x54 flags (bit 0 visible), +0x40 /
// +0x44 the position on the canvas like an instance's (a text's middle, a piece's corner). The visible ones' positions
// span what it shows.
#define ANIMP_VTABLE 0x0196c158u  // cGUIInstAnimWithParam (the game's own)
#define OBJ_FIRST 0x68u
#define OBJ_NEXT 0x6cu
#define OBJ_NULL_VT 0x0198fb70u
// Pieces with a size (polygons, textures, texture sets: frames, bars, icons): +0xb4 / +0xb8 width / height (times the
// piece's scale), +0xa0 low 4 bits where the position is in it (0 its top left corner, 5 its middle: a numpad's places)
#define OBJ_W 0xb4u
#define OBJ_H 0xb8u
#define OBJ_ALIGN 0xa0u

static bool obj_sized(uint32_t vt) {
    return vt == 0x0198fc48u || vt == 0x0198fcc8u || vt == 0x0198fd48u || vt == 0x0196c1e0u || vt == 0x0196c274u;
}

static bool is_anim(uint32_t inst) { return LD32(inst) == ANIM_VTABLE || LD32(inst) == ANIMP_VTABLE; }

// (*drawn: something of it shows - a text alone spans no box)
static Box inst_box_drawn(uint32_t inst, bool *drawn) {
    float x = f32(inst + INST_X), y = f32(inst + INST_Y);
    Box b = {x, y, x, y};
    uint32_t root = is_anim(inst) ? LD32(inst + INST_ANIM) : 0;
    bool any = false;
    *drawn = false;
    if (!sane(root)) return b;
    uint32_t stack[64];
    int sp = 0, seen = 0;
    if (inst_ok(LD32(root + OBJ_FIRST))) stack[sp++] = LD32(root + OBJ_FIRST);
    while (sp && seen++ < 512) {
        uint32_t o = stack[--sp], next = LD32(o + OBJ_NEXT), first = LD32(o + OBJ_FIRST);
        if (inst_ok(next) && sp < 64) stack[sp++] = next;
        if (!(LD32(o + INST_FLAGS) & 1)) continue;
        if (inst_ok(first) && sp < 64) stack[sp++] = first;
        if (LD32(o) == OBJ_NULL_VT) continue;  // (a piece's anchor: draws nothing)
        float ox = f32(o + INST_X), oy = f32(o + INST_Y), w = 0.0f, h = 0.0f, fx = 0.0f, fy = 0.0f;
        if (!(fabsf(ox) < 4096.0f && fabsf(oy) < 4096.0f)) continue;
        if (obj_sized(LD32(o))) {
            w = f32(o + OBJ_W) * fabsf(f32(o + INST_SX)), h = f32(o + OBJ_H) * fabsf(f32(o + INST_SY));
            if (!(w > 0.0f && w < 2048.0f && h > 0.0f && h < 2048.0f)) w = h = 0.0f;
            int al = (int)(LD32(o + OBJ_ALIGN) & 0xfu);  // the position's place in it: 0 top left, else as on a numpad
            if (al) fx = (float)((al - 1) % 3) * 0.5f, fy = (float)(2 - (al - 1) / 3) * 0.5f;
        }
        float x0 = ox - fx * w, y0 = oy - fy * h;
        if (!any) b = (Box){x0, y0, x0 + w, y0 + h}, any = true;
        else b.x0 = fminf(b.x0, x0), b.y0 = fminf(b.y0, y0), b.x1 = fmaxf(b.x1, x0 + w), b.y1 = fmaxf(b.y1, y0 + h);
    }
    *drawn = any;
    return b;
}

static Box inst_box(uint32_t inst) {
    bool drawn;
    return inst_box_drawn(inst, &drawn);
}

// Whether an animation instance draws anything (the transfer menu keeps an empty bar between its two).
static bool inst_draws(uint32_t inst) {
    bool drawn = true;
    if (is_anim(inst)) inst_box_drawn(inst, &drawn);
    return drawn;
}

// ---- the parts of a GUI found by their names, kept for each GUI from when it first shows (game thread)
// - numbered siblings (Anm_list_S00..S07, Anm_itemSet01..08, Anm_panel01..03): a menu's items, in number order;
// - page arrows (Anm_arrow_L / _R).
#define NPARTS 48
#define NGROUPS 24
#define GROUP_N 64
#define NARROWS 4
typedef struct {
    uint32_t parent;
    int n;
    char prefix[28];
    uint16_t idx[GROUP_N];  // the GUI's instance indices (as the game's code names them), by number
} Numbered;
typedef struct {
    uint32_t own, res;
    uint64_t frame, built;
    bool again;  // (scanned once more after it has shown a while: page counters' arrows are made as the menu opens)
    int ngroups, narrows, nparents, nhilite;
    uint16_t hilite[16];  // animations named ..cursor.. (a list's highlight: as wide as its rows; menu_down has many)
    Numbered g[NGROUPS];
    uint16_t arrow[NARROWS];
    int8_t arrow_dir[NARROWS];  // -1 back (_L), +1 on (_R)
    uint16_t arrow_parent[NARROWS];  // places named ..arrow.. / ..page.. holding unnamed arrows (made as the menu opens)
} Parts;
static Parts parts[NPARTS];

// A page arrow's direction by its name: ..arrow.._L / _R (Arw, yaji); 0: none.
static int arrow_dir(const char *nm) {
    char low[64];
    size_t n = 0;
    for (; nm[n] && n < sizeof low - 1; n++) low[n] = (char)(nm[n] >= 'A' && nm[n] <= 'Z' ? nm[n] + 32 : nm[n]);
    low[n] = 0;
    if (!strstr(low, "arrow") && !strstr(low, "arw") && !strstr(low, "yaji")) return 0;
    if (n >= 2 && low[n - 2] == '_' && low[n - 1] == 'l') return -1;
    if (n >= 2 && low[n - 2] == '_' && low[n - 1] == 'r') return 1;
    if (n >= 4 && !strcmp(low + n - 4, "left")) return -1;
    if (n >= 5 && !strcmp(low + n - 5, "right")) return 1;
    return 0;
}

static Parts *parts_of(uint32_t own) {
    uint32_t res = LD32(own + OWN_RES);
    Parts *p = &parts[0];
    bool again = false;
    for (int i = 0; i < NPARTS; i++) {
        if (parts[i].own == own && parts[i].res == res) {
            parts[i].frame = g_present_count;
            if (parts[i].again || g_present_count - parts[i].built < 20) return &parts[i];
            p = &parts[i], again = true;
            break;
        }
        if (parts[i].frame < p->frame) p = &parts[i];
    }
    memset(p, 0, sizeof *p);
    p->own = own, p->res = res, p->frame = p->built = g_present_count, p->again = again;
    uint32_t n = LD32(own + OWN_NINST), arr = LD32(own + OWN_INST);
    if (!sane(arr) || n > 4096) return p;
    static int16_t num[NGROUPS][GROUP_N];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t inst = LD32(arr + 4u * i);
        if (!inst_ok(inst)) continue;
        const char *nm = inst_name(own, inst);
        int dir = arrow_dir(nm);
        if (dir && p->narrows < NARROWS) p->arrow[p->narrows] = (uint16_t)i, p->arrow_dir[p->narrows++] = (int8_t)dir;
        if (is_anim(inst) && p->nhilite < 16 && (strstr(nm, "ursor") || strstr(nm, "orsol"))) p->hilite[p->nhilite++] = (uint16_t)i;
        if (LD32(inst) == 0x0198f4d8u && p->nparents < NARROWS) {  // (cGUIInstNull)
            char low[64];
            size_t k = 0;
            for (; nm[k] && k < sizeof low - 1; k++) low[k] = (char)(nm[k] >= 'A' && nm[k] <= 'Z' ? nm[k] + 32 : nm[k]);
            low[k] = 0;
            int copies = 0;  // (or holding copies of an animation made as the menu opens, no names: a page counter's)
            for (uint32_t k = LD32(inst + INST_CHILD), m = 0; inst_ok(k) && m < 16; k = LD32(k + INST_NEXT), m++)
                copies += LD32(k) == ANIMP_VTABLE && !strcmp(inst_name(own, k), "?");
            if (((strstr(low, "arrow") || strstr(low, "page")) && !strstr(low, "count")) || copies >= 2)
                p->arrow_parent[p->nparents++] = (uint16_t)i;
        }
        size_t len = strlen(nm), d = len;
        while (d > 0 && nm[d - 1] >= '0' && nm[d - 1] <= '9') d--;
        if (d == len || d == 0 || len - d > 3 || d >= sizeof p->g[0].prefix) continue;
        uint32_t par = LD32(inst + INST_PARENT);
        int g = 0;
        while (g < p->ngroups && !(p->g[g].parent == par && !strncmp(p->g[g].prefix, nm, d) && !p->g[g].prefix[d])) g++;
        if (g == p->ngroups) {
            if (g == NGROUPS) continue;
            p->ngroups++, p->g[g].parent = par, p->g[g].n = 0;
            memcpy(p->g[g].prefix, nm, d), p->g[g].prefix[d] = 0;
        }
        Numbered *G = &p->g[g];
        if (G->n == GROUP_N) continue;
        int v = atoi(nm + d), at = G->n++;  // (insertion by number)
        while (at > 0 && num[g][at - 1] > v) num[g][at] = num[g][at - 1], G->idx[at] = G->idx[at - 1], at--;
        num[g][at] = (int16_t)v, G->idx[at] = (uint16_t)i;
    }
    for (int g = 0; g < p->ngroups; g++)  // (a run of numbers, each once)
        if (p->g[g].n < 2 || num[g][p->g[g].n - 1] - num[g][0] != p->g[g].n - 1) p->g[g].n = 0;
    return p;
}

// How a menu input's items are laid out: kind 0 a list along its columns (its rows pages), 1 along its rows, 2 a grid
// of columns x rows; n places a page, `at` the selection's place on the page, `shown` the items on this page. Only
// axes that move on the page count (a cursor that only flips tabs has no items to point at).
typedef struct { int kind, cols, rows, n, at, shown; } Layout;

static bool spatial_mode(uint32_t m) { return m == 1 || m == 2 || m == 11; }  // (moves on the page; 3..10 flip pages)

static int clamp_shown(int left, int n) { return left < 1 ? 1 : left < n ? left : n; }

// The axis that flips a cursor's pages (0 its columns, 1 its rows; -1 none): one L / R or the stick move (modes 3..10),
// else one the d-pad moves while the other axis, with places, doesn't move (the special permits: a page of 6 rows
// shown at a time, left / right the pages).
static int page_axis(uint32_t cur) {
    for (int a = 0; a < 2; a++) {
        uint32_t m = LD32(cur + CUR_MODE + 4u * a);
        if (m >= 3 && m <= 10 && (a ? LD16(cur + CUR_ROWS) : LD8(cur + CUR_COLS)) >= 2) return a;
    }
    for (int a = 0; a < 2; a++)
        if (spatial_mode(LD32(cur + CUR_MODE + 4u * a)) && !LD32(cur + CUR_MODE + 4u * (a ^ 1)) &&
            LD16(cur + CUR_ROWS) >= 2 && LD8(cur + CUR_COLS) >= 2)
            return a;
    return -1;
}

static bool layout_of(uint32_t in, Layout *L) {
    if (LD32(in) == BOX_VTABLE) {
        int cols = LD8(in + BOX_COLS), rows = LD8(in + BOX_ROWS), n = cols * rows;
        *L = (Layout){2, cols, rows, n, LD8(in + BOX_ROW) * cols + LD8(in + BOX_COL),
                      clamp_shown(LD16(in + BOX_ITEMS) - LD16(in + BOX_PAGE) * n, n)};
        return cols > 0 && rows > 0 && n > 1;
    }
    if (LD32(in) != CUR_VTABLE) return false;
    int cols = LD8(in + CUR_COLS), rows = LD16(in + CUR_ROWS), col = LD8(in + CUR_COL), row = LD16(in + CUR_ROW);
    int items = LD16(in + CUR_ITEMS);
    int pa = page_axis(in);
    bool sc = cols > 1 && spatial_mode(LD32(in + CUR_MODE)) && pa != 0;
    bool sr = rows > 1 && spatial_mode(LD32(in + CUR_MODE + 4u)) && pa != 1;
    if (sc && sr) *L = (Layout){2, cols, rows, cols * rows, row * cols + col, clamp_shown(items, cols * rows)};
    else if (sc) *L = (Layout){0, cols, rows, cols, col, clamp_shown(items - row * cols, cols)};
    else if (sr) *L = (Layout){1, cols, rows, rows, row, rows};
    else return false;
    return true;
}

// The steps between visible members across and down (0: none).
static void group_steps(uint32_t own, const Numbered *G, float *sx, float *sy) {
    *sx = *sy = 0.0f;
    float px = 0, py = 0;
    bool have = false;
    for (int k = 0; k < G->n; k++) {
        uint32_t m = owner_instance(own, G->idx[k]);
        if (!m || !(LD32(m + INST_FLAGS) & 1)) continue;
        float x = f32(m + INST_X), y = f32(m + INST_Y);
        if (have) {
            float dx = fabsf(x - px), dy = fabsf(y - py);
            if (dx > 4.0f && (!*sx || dx < *sx)) *sx = dx;
            if (dy > 4.0f && (!*sy || dy < *sy)) *sy = dy;
        }
        px = x, py = y, have = true;
    }
}

// How well group G of GUI `own` fits layout L: -1 not at all (fewer members than the page has places, fewer of them
// showing than the page has items, or more past its places); more when the selection's member stands out (a sequence
// of its own, or set apart from the others), when as many show as the page has items, when they draw (not anchors),
// when their name says list / item / panel.
static int group_fit(uint32_t own, const Numbered *G, const Layout *L) {
    if (G->n < L->n || L->at < 0 || L->at >= L->n) return -1;
    uint32_t sel = owner_instance(own, G->idx[L->at]);
    if (!sel || !(LD32(sel + INST_FLAGS) & 1) || (inst_ok(G->parent) && !inst_shows(G->parent))) return -1;
    if (LD32(sel) == 0x0198f4d8u) {  // (cGUIInstNull: only a place - an item if it holds something that shows)
        bool holds = false;
        for (uint32_t k = LD32(sel + INST_CHILD), n = 0; inst_ok(k) && n < 64 && !holds; k = LD32(k + INST_NEXT), n++)
            holds = LD32(k + INST_FLAGS) & 1;
        if (!holds) return -1;
    }
    int score = 0, same_seq = 0, same_x = 0, same_y = 0, vis = 0, past = 0;
    for (int k = 0; k < G->n; k++) {
        uint32_t m = owner_instance(own, G->idx[k]);
        if (!m || !(LD32(m + INST_FLAGS) & 1)) continue;
        if (k >= L->n) { past++; continue; }
        vis++;
        if (k == L->at) continue;
        same_seq += is_anim(m) && anim_sequence(m) == anim_sequence(sel);
        same_x += fabsf(f32(m + INST_X) - f32(sel + INST_X)) < 2.0f;
        same_y += fabsf(f32(m + INST_Y) - f32(sel + INST_Y)) < 2.0f;
    }
    if (past || vis < L->shown) return -1;
    float sx, sy;
    group_steps(own, G, &sx, &sy);
    if (!sx && !sy) return -1;  // (all in one place)
    if (vis > 1 && (!same_seq || (L->kind == 0 && sy && !same_x) || (L->kind == 0 && sx && !same_y))) score += 4;
    if (vis == L->shown) score += 3;
    if (is_anim(sel)) score += 2;
    else if (LD32(sel) == 0x0198f4d8u) score -= 3;  // (cGUIInstNull: a place, draws nothing)
    char low[28];
    for (size_t i = 0; i < sizeof low; i++) low[i] = (char)(G->prefix[i] >= 'A' && G->prefix[i] <= 'Z' ? G->prefix[i] + 32 : G->prefix[i]);
    static const char *const words[] = {"list", "item", "panel", "menu", "set", "slot", "cell", "btn", "button", "tab", "icon", "box"};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (strstr(low, words[i])) { score += 1; break; }
    return score;
}

// Group G without the members flagged visible that draw nothing: no items (the transfer menu's empty bar between its
// two).
static void group_items(uint32_t own, const Numbered *G, Numbered *E) {
    *E = *G;
    E->n = 0;
    for (int k = 0; k < G->n; k++) {
        uint32_t m = owner_instance(own, G->idx[k]);
        if (m && (LD32(m + INST_FLAGS) & 1) && !inst_draws(m)) continue;
        E->idx[E->n++] = G->idx[k];
    }
}

// The numbered group of a showing GUI that is menu input `in`'s items (as group_items has it): the best fit this frame
// (menus open in steps: what fits first may not be it), the earlier pick while it fits as well.
static struct { uint32_t input, caller, own, parent; } num_pick[4];
static bool numbered_of(uint32_t in, const Layout *L, uint32_t *own_out, Numbered *out) {
    int prev = live_cur ^ 1, best = -1, slot = -1, kept = -1;
    Numbered E, kg;
    for (int i = 0; i < 4; i++)
        if (num_pick[i].input == in && num_pick[i].caller == input.caller) slot = i;
    for (int j = 0; j < nlive[prev]; j++) {
        uint32_t own = live[prev][j];
        if (!gui_shows(own)) continue;
        Parts *p = parts_of(own);
        for (int g = 0; g < p->ngroups; g++) {
            if (p->g[g].n < L->n) continue;
            group_items(own, &p->g[g], &E);
            int f = group_fit(own, &E, L);
            if (f < 0) continue;
            if (slot >= 0 && own == num_pick[slot].own && E.parent == num_pick[slot].parent) kept = f, kg = E;
            if (f > best) best = f, *own_out = own, *out = E;
        }
    }
    if (best < 0) return false;
    if (kept >= 0 && kept >= best) { *own_out = num_pick[slot].own, *out = kg; return true; }
    static int next;
    if (slot < 0) slot = next, next = (next + 1) % 4;
    num_pick[slot].input = in, num_pick[slot].caller = input.caller, num_pick[slot].own = *own_out, num_pick[slot].parent = out->parent;
    if (log_on())
        LOG("[directmouse] items of %08x from %08x: %s.%s.. (%d of %d, fit %d)\n", in, input.caller, gui_name(*own_out),
            out->prefix, L->n, out->n, best);
    return true;
}

// Where the members of numbered group G answer to the pointer: what each draws, grown to half a step around its
// middle along the list (no gaps between neighbours), across a list the whole list's width (the selected item slides
// aside in the quest list and the shop: its own box would leave the pointer on a neighbour); a member that only marks
// a place, a step around it (across a list at least 120 px).
typedef struct { float sx, sy, hx, hy, lo, hi, off0, off1; bool list, down, across, offs; } NumGeo;

static void numbered_geo(uint32_t own, const Numbered *G, const Layout *L, NumGeo *g) {
    group_steps(own, G, &g->sx, &g->sy);
    g->hx = g->sx * 0.5f, g->hy = g->sy * 0.5f, g->lo = 1e30f, g->hi = -1e30f, g->off0 = g->off1 = 0.0f, g->offs = false;
    g->list = L->kind != 2;
    g->down = !g->list || g->sy > g->sx, g->across = !g->list || g->sx >= g->sy;  // (the list's direction: where it decides)
    if (!g->list) return;
    if (g->down) g->hx = 60.0f; else g->hy = 20.0f;
    for (int k = 0; k < G->n; k++) {
        uint32_t m = owner_instance(own, G->idx[k]);
        if (!m || !(LD32(m + INST_FLAGS) & 1)) continue;
        Box b = inst_box(m);
        if (b.x1 - b.x0 <= 8.0f || b.y1 - b.y0 <= 8.0f) continue;
        g->lo = fminf(g->lo, g->down ? b.x0 : b.y0), g->hi = fmaxf(g->hi, g->down ? b.x1 : b.y1);
    }
    if (g->lo < g->hi) return;
    // members that only mark places (their texts' corners: the combo list's, the kitchen's): the rows are where the
    // menu's highlight stands on the selected one - as wide as it, as far from each mark as it is from the selected's
    uint32_t sel = owner_instance(own, G->idx[L->at]);
    if (!sel) return;
    Parts *p = parts_of(own);
    float sa = g->down ? f32(sel + INST_X) : f32(sel + INST_Y), sl = g->down ? f32(sel + INST_Y) : f32(sel + INST_X);
    float step = g->down ? g->sy : g->sx;
    for (int i = 0; i < p->nhilite; i++) {
        uint32_t h = owner_instance(own, p->hilite[i]);
        if (!h || !inst_shows(h)) continue;
        Box b = inst_box(h);
        float lo = g->down ? b.x0 : b.y0, hi = g->down ? b.x1 : b.y1, l0 = g->down ? b.y0 : b.x0, l1 = g->down ? b.y1 : b.x1;
        if (hi - lo <= 40.0f || sa < lo - 60.0f || sa > hi) continue;  // (not this list's)
        g->lo = fminf(g->lo, lo), g->hi = fmaxf(g->hi, hi);
        if (step > 0.0f && l1 - l0 > 4.0f && fabsf((l0 + l1) * 0.5f - sl) < step)
            g->off0 = l0 - sl, g->off1 = l1 - sl, g->offs = true;
    }
}

static Box numbered_box(uint32_t own, const Numbered *G, int k, const NumGeo *g) {
    uint32_t m = owner_instance(own, G->idx[k]);
    Box b = inst_box(m);
    if (b.x1 - b.x0 > 8.0f && b.y1 - b.y0 > 8.0f) {
        float mx = (b.x0 + b.x1) * 0.5f, my = (b.y0 + b.y1) * 0.5f;
        if (g->down && g->sy) b.y0 = fminf(b.y0, my - g->hy), b.y1 = fmaxf(b.y1, my + g->hy);
        if (g->across && g->sx) b.x0 = fminf(b.x0, mx - g->hx), b.x1 = fmaxf(b.x1, mx + g->hx);
    } else {
        float cx = f32(m + INST_X), cy = f32(m + INST_Y);
        b = (Box){fminf(b.x0, cx - g->hx), fminf(b.y0, cy - g->hy), fmaxf(b.x1, cx + g->hx), fmaxf(b.y1, cy + g->hy)};
        if (g->offs) {  // (the row as the highlight shows the selected one, grown to half a step around its middle)
            float a0 = (g->down ? cy : cx) + g->off0, a1 = (g->down ? cy : cx) + g->off1, mid = (a0 + a1) * 0.5f;
            float h = g->down ? g->hy : g->hx;
            a0 = fminf(a0, mid - h), a1 = fmaxf(a1, mid + h);
            if (g->down) b.y0 = a0, b.y1 = a1; else b.x0 = a0, b.x1 = a1;
        }
    }
    if (g->list && g->lo < g->hi) {  // across a list: the whole list's width
        if (g->down) b.x0 = fminf(b.x0, g->lo), b.x1 = fmaxf(b.x1, g->hi);
        else b.y0 = fminf(b.y0, g->lo), b.y1 = fmaxf(b.y1, g->hi);
    }
    return b;
}

// The member of a numbered group under the pointer, -1: none; the nearest when their places overlap.
static int numbered_hover(uint32_t own, const Numbered *G, const Layout *L) {
    if (!ptr_inside) return -1;
    NumGeo g;
    numbered_geo(own, G, L, &g);
    int best = -1;
    float bd = 1e30f;
    for (int k = 0; k < G->n; k++) {
        uint32_t m = owner_instance(own, G->idx[k]);
        if (!m || !(LD32(m + INST_FLAGS) & 1)) continue;
        Box b = numbered_box(own, G, k, &g);
        b.x0 -= 4.0f, b.y0 -= 4.0f, b.x1 += 4.0f, b.y1 += 4.0f;
        if (ptr_x < b.x0 || ptr_x > b.x1 || ptr_y < b.y0 || ptr_y > b.y1) continue;
        if (k == L->at) return k;  // (the selection holds where it overlaps a neighbour: no flicker between two)
        float d = (g.down ? fabsf(ptr_y - (b.y0 + b.y1) * 0.5f) : 0.0f) + (g.across ? fabsf(ptr_x - (b.x0 + b.x1) * 0.5f) : 0.0f);
        if (d < bd) bd = d, best = k;
    }
    return best;
}

// ---- box grids (cBoxCursor) whose menu moves a highlight itself: the item box sets its Anm_cursor to column x 36,
// row x 36 every frame (setX / setY). The step is learned from a value off column / row 0 (kept for the GUI after it
// closes); before that the highlight's own size stands in.
static struct {
    uint32_t input, caller, inst, key;
    uint64_t frame, xframe;  // (its last setX / setY, its last setX: a grid's highlight moves across as well)
} hl;
static struct { uint32_t key; float sx, sy; } hl_steps[16];

static uint32_t hl_key(uint32_t own, uint32_t inst) {  // (the GUI file's path and the instance's id)
    uint32_t res = LD32(own + OWN_RES), h = 2166136261u ^ LD32(inst + INST_ID);
    for (const char *p = sane(res) ? (const char *)G2H(res + RES_PATH) : ""; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static int hl_slot(uint32_t key) {
    int s = (int)(key % 16u);
    if (hl_steps[s].key != key) hl_steps[s].key = key, hl_steps[s].sx = hl_steps[s].sy = 0.0f;
    return s;
}

// From the position setters (setX: axes 1, setY: 2, setXY: 3): a box menu's highlight, named so (..cursor..).
static void hl_note(uint32_t own, uint32_t inst, int axes, float vx, float vy) {
    if (!box_in.obj || g_present_count - box_in.frame > 1) return;
    char low[64];
    const char *nm = inst_name(own, inst);
    size_t n = 0;
    for (; nm[n] && n < sizeof low - 1; n++) low[n] = (char)(nm[n] >= 'A' && nm[n] <= 'Z' ? nm[n] + 32 : nm[n]);
    low[n] = 0;
    if (!strstr(low, "cursor") && !strstr(low, "corsol")) return;
    if (hl.input != box_in.obj || hl.caller != box_in.caller || hl.inst != inst) {
        // (another menu's highlight set in the same frame - the item box's side menu - doesn't take a grid's over)
        bool fresh = hl.input == box_in.obj && hl.caller == box_in.caller && g_present_count - hl.frame <= 1;
        if (fresh && (g_present_count - hl.xframe <= 1 || !(axes & 1))) return;
        hl.input = box_in.obj, hl.caller = box_in.caller, hl.inst = inst, hl.key = hl_key(own, inst);
        if (log_on()) LOG("[directmouse] highlight of box %08x: %s.%s\n", box_in.obj, gui_name(own), nm);
    }
    hl.frame = g_present_count;
    if (axes & 1) hl.xframe = g_present_count;
    int s = hl_slot(hl.key), col = LD8(box_in.obj + BOX_COL), row = LD8(box_in.obj + BOX_ROW);
    uint32_t par = LD32(inst + INST_PARENT);  // (the values are in the parent's space: the character creation's is x2)
    float kx = inst_ok(par) ? fabsf(f32(par + INST_SX)) : 1.0f, ky = inst_ok(par) ? fabsf(f32(par + INST_SY)) : 1.0f;
    if (!(kx > 0.1f && kx < 10.0f)) kx = 1.0f;
    if (!(ky > 0.1f && ky < 10.0f)) ky = 1.0f;
    if ((axes & 1) && col > 0 && vx * kx / col >= 4.0f) hl_steps[s].sx = vx * kx / col;
    if ((axes & 2) && row > 0 && vy * ky / row >= 4.0f) hl_steps[s].sy = vy * ky / row;
}

static bool hl_known(uint32_t cur) {
    return hl.input == cur && hl.caller == input.caller && g_present_count - hl.frame <= FRESH_FRAMES && inst_ok(hl.inst);
}

// The cell under the pointer (the highlight stands on the selected one), false: off the grid.
static bool hl_hover(uint32_t cur, const Layout *L, int *col, int *row) {
    if (!ptr_inside) return false;
    int s = hl_slot(hl.key);
    float sx = hl_steps[s].sx, sy = hl_steps[s].sy, ax = f32(hl.inst + INST_X), ay = f32(hl.inst + INST_Y);
    Box b = inst_box(hl.inst);
    float w = b.x1 - b.x0, h = b.y1 - b.y0;
    if (!sx) sx = sy ? sy : w;
    if (!sy) sy = sx ? sx : h;
    if (sx < 4.0f || sy < 4.0f) return false;
    // the middle of the highlight's cell: half a step from its place (the cell's corner), or the place itself when what
    // it draws is around it (judged from its box, which may pulse: only which of the two)
    float mx = ax + sx * 0.5f, my = ay + sy * 0.5f;
    if (w > 4.0f && fabsf((b.x0 + b.x1) * 0.5f - ax) < sx * 0.25f) mx = ax;
    if (h > 4.0f && fabsf((b.y0 + b.y1) * 0.5f - ay) < sy * 0.25f) my = ay;
    int c0 = LD8(cur + BOX_COL), r0 = LD8(cur + BOX_ROW);
    int c = cell_hold(ptr_x, mx - sx * 0.5f - (float)c0 * sx, sx, c0), r = cell_hold(ptr_y, my - sy * 0.5f - (float)r0 * sy, sy, r0);
    if (c < 0 || r < 0 || c >= L->cols || r >= L->rows) return false;
    *col = c, *row = r;
    return true;
}

// ---- page arrows: a click on one flips the page of the menu that runs (its cursor's page axis), as its buttons do
// Each belongs to the menu whose items are in its GUI (the item box's item sets and pouch side by side page apart).
#define INST_OWNER 0x6cu  // an instance's GUI
static Box arrows[8];
static int8_t arrows_dir[8];
static uint32_t arrows_own[8];
static int narrows;
static bool over_arrow;            // the pointer is on one (last frame)
static volatile int page_request;  // a click on one: -1 / +1, taken by its menu's cursor
static uint32_t page_own;          // (the GUI it is in)
static uint64_t page_frame;        // (when: one no menu takes soon is dropped)
static int fz_click;               // the fuzzer's click: an arrow's, or none

// The GUI menu input `in`'s items were found in (0: not known).
static uint32_t input_gui(uint32_t in) {
    const Place *p = place_of(in);
    if (p && inst_ok(p->cur)) return LD32(p->cur + INST_OWNER);
    for (int i = 0; i < 4; i++)
        if (num_pick[i].input == in && num_pick[i].caller == input.caller) return num_pick[i].own;
    if (hl.input == in && hl.caller == input.caller && inst_ok(hl.inst)) return LD32(hl.inst + INST_OWNER);
    for (int i = 0; i < 4; i++)
        if (group_owner[i].input == in && group_owner[i].caller == input.caller) return group_owner[i].owner;
    return 0;
}

// Whether a clicked arrow's page is menu input `in`'s: its items in the arrow's GUI; after a few frames without such
// a menu (arrows in a GUI of their own), any menu that pages.
static bool page_for(uint32_t in) {
    if (!page_request) return false;
    uint64_t age = g_present_count - page_frame;
    if (age > 8) { page_request = 0; return false; }
    return input_gui(in) == page_own || age >= 3;
}

static void find_arrows(void) {
    int prev = live_cur ^ 1;
    narrows = 0;
    for (int j = 0; j < nlive[prev] && narrows < 8; j++) {
        uint32_t own = live[prev][j];
        if (!gui_shows(own)) continue;
        Parts *p = parts_of(own);
        for (int a = 0; a < p->narrows && narrows < 8; a++) {
            uint32_t m = owner_instance(own, p->arrow[a]);
            uint32_t par = m ? LD32(m + INST_PARENT) : 0;
            if (!m || !(LD32(m + INST_FLAGS) & 1) || (inst_ok(par) && !inst_shows(par))) continue;
            Box b = inst_box(m);
            float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;  // (at least 40 x 40 around its middle)
            b.x0 = fminf(b.x0, cx - 20.0f), b.x1 = fmaxf(b.x1, cx + 20.0f), b.y0 = fminf(b.y0, cy - 20.0f), b.y1 = fmaxf(b.y1, cy + 20.0f);
            arrows[narrows] = b, arrows_own[narrows] = own, arrows_dir[narrows++] = p->arrow_dir[a];
        }
        for (int a = 0; a < p->nparents && narrows < 7; a++) {  // unnamed children: the leftmost back, the rightmost on
            uint32_t par = owner_instance(own, p->arrow_parent[a]), lo = 0, hi = 0;
            if (!par || !inst_shows(par)) continue;
            Box bl = {0}, bh = {0};
            for (uint32_t k = LD32(par + INST_CHILD), n = 0; inst_ok(k) && n < 16; k = LD32(k + INST_NEXT), n++) {
                if (!(LD32(k + INST_FLAGS) & 1) || strcmp(inst_name(own, k), "?")) continue;
                Box b = inst_box(k);
                float cx = (b.x0 + b.x1) * 0.5f;
                if (!lo || cx < (bl.x0 + bl.x1) * 0.5f) lo = k, bl = b;
                if (!hi || cx > (bh.x0 + bh.x1) * 0.5f) hi = k, bh = b;
            }
            if (!lo || lo == hi) continue;
            if (bl.x1 - bl.x0 > 48.0f || bl.y1 - bl.y0 > 64.0f || bh.x1 - bh.x0 > 48.0f || bh.y1 - bh.y0 > 64.0f) continue;  // (arrow-sized)
            Box *bs[2] = {&bl, &bh};
            for (int e = 0; e < 2; e++) {
                Box b = *bs[e];
                float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
                b.x0 = fminf(b.x0, cx - 20.0f), b.x1 = fmaxf(b.x1, cx + 20.0f), b.y0 = fminf(b.y0, cy - 20.0f), b.y1 = fmaxf(b.y1, cy + 20.0f);
                arrows[narrows] = b, arrows_own[narrows] = own, arrows_dir[narrows++] = (int8_t)(e ? 1 : -1);
            }
        }
    }
    over_arrow = false;
    for (int i = 0; i < narrows && ptr_inside; i++)
        if (ptr_x >= arrows[i].x0 && ptr_x <= arrows[i].x1 && ptr_y >= arrows[i].y0 && ptr_y <= arrows[i].y1) {
            over_arrow = true;
            if ((click_ns && plat_now_ns() - click_ns <= CLICK_NS) || fz_click) {
                click_ns = 0, fz_click = 0, page_request = arrows_dir[i], page_own = arrows_own[i], page_frame = g_present_count;
                if (log_on()) LOG("[directmouse] pointer %.0f,%.0f: page arrow %+d\n", ptr_x, ptr_y, arrows_dir[i]);
            }
        }
    fz_click = 0;
}

// ---- selecting
// The cursor axis (0 column, 1 row) moved by up / down (vertical) or left / right; -1: none.
static int cursor_axis(uint32_t cur, bool vertical) {
    for (int a = 0; a < 2; a++) {
        uint32_t m = LD32(cur + CUR_MODE + 4u * a);
        if (!m || m >= CURSOR_NMODES) continue;
        uint32_t prev = LD32(CURSOR_MODES + 16u * m);
        if (vertical ? prev & PAD_UP : prev & PAD_LEFT) return a;
    }
    return -1;
}

// Whether moving by dc columns and dr rows (the cursor's own) lands on an item.
static bool cursor_fits(uint32_t cur, int dc, int dr) {
    int col = LD8(cur + CUR_COL) + dc, cols = LD8(cur + CUR_COLS), row = LD16(cur + CUR_ROW) + dr;
    int rows = LD16(cur + CUR_ROWS), items = LD16(cur + CUR_ITEMS);
    return cols > 0 && col >= 0 && col < cols && row >= 0 && row < (rows > 0 ? rows : 1) && row * cols + col < items;
}

// The pointer's selecting and clicking, given what is under it (over; dc / dr: the steps from the selection to it in
// the menu's terms): true when the selection moves there now. A waiting click becomes A once its item is selected.
static struct { uint32_t in, caller; } fz_over;  // (the fuzzer: the menu input the pointer was over lately)
static void fz_picked(uint64_t frames);
static void snap_open(uint32_t obj, uint32_t caller);
static void snap_hit(uint64_t frame);
static void snap_confirmed(uint64_t frame, const char *how);

// The menu inputs that ran lately, each with the first frame of its current run (when its menu opened) and the first
// frame of it its items were known.
static struct { uint32_t obj, caller; uint64_t first, last, known; } runs[8];
static int run_of(uint32_t obj, uint32_t caller) {
    for (int i = 0; i < 8; i++)
        if (runs[i].obj == obj && runs[i].caller == caller) return i;
    return -1;
}
static uint64_t run_start(uint32_t in) {
    int r = run_of(in, input.caller);
    return r >= 0 ? runs[r].first : g_present_count;
}

// A pointer move selects the item under it once its menu's items are known (a menu opens in steps: the pointer may
// get there before the item does), for MOVE_FRAMES; a pointer still over none of the items of a menu that knows them
// for MISS_FRAMES stops selecting (a menu that changes under a pointer left beside it is left alone). While a menu
// opens (OPEN_FRAMES) its pieces may still move in: the pointer keeps selecting what comes under it.
#define MOVE_FRAMES 60
#define MISS_FRAMES 12
#define OPEN_FRAMES 10
static void note_moves(void) {
    uint32_t seq = ptr_seq;
    if (seq != seen_seq) seen_seq = seq, move_frame = g_present_count, pending = true, miss_frames = 0;
}

static void frame_moves(void) {  // (at a frame's start: what the last frame showed)
    if (pending && frame_known && !frame_over && ++miss_frames > MISS_FRAMES) pending = false;
    if (pending && g_present_count - move_frame > MOVE_FRAMES) pending = false;
    frame_known = frame_over = false;
    note_moves();
}

static bool want_select(bool over, int dc, int dr) {
    uint64_t frame = g_present_count;
    known_frame = frame, frame_known = true;
    note_moves();
    int r = run_of(input.obj, input.caller);
    bool opening = r >= 0 && frame - runs[r].first < OPEN_FRAMES;
    if (r >= 0 && !runs[r].known) {
        runs[r].known = frame;
        if (log_on())
            LOG("[snap] %s %08x from %08x: items known %llu frames after it opened\n", input.kind == IN_BOX ? "box cursor" :
                input.kind == IN_YESNO ? "yes/no" : "cursor", input.obj, input.caller, (unsigned long long)(frame - runs[r].first));
    }
    if (over) fz_over.in = input.obj, fz_over.caller = input.caller;
    uint64_t click = click_ns;
    if (click && plat_now_ns() - click > CLICK_NS) click_ns = click = 0;
    if (over_arrow) click = 0;  // (a page arrow's: find_arrows takes it)
    if (!over) return false;
    frame_over = true;
    if (click) {
        if (!dc && !dr) click_ns = 0, confirm = 1, snap_confirmed(frame, "on the item under the pointer");
        else pending = true, move_frame = frame;  // selected this frame: confirmed on the next
    }
    if (!dc && !dr) {  // (on the selection: as the pointer wants it)
        if (!opening) pending = false;
        snap_hit(frame);
        return false;
    }
    if (!pending) return false;
    if (!opening) pending = false;  // (until it moves again: a list that slides or scrolls under a still pointer stays put)
    fz_picked(frame - move_frame);
    snap_hit(frame);
    return true;
}

// A menu whose items aren't known: a click is A, on whatever is selected (unless another menu that runs knows its).
// A menu that has just opened gets BLIND_WAIT frames to show where its items are first (a quick click as it opens).
#define BLIND_WAIT 15
static void click_blind(void) {
    uint64_t frame = g_present_count;
    if (!click_ns || over_arrow || frame - known_frame <= FRESH_FRAMES) return;
    if (plat_now_ns() - click_ns > CLICK_NS) { click_ns = 0; return; }
    int r = run_of(input.obj, input.caller);
    if (r >= 0 && frame - runs[r].first < BLIND_WAIT) return;
    for (int i = 0; i < NPLACES; i++)
        if (frame - places[i].frame <= FRESH_FRAMES) return;
    click_ns = 0, confirm = 1;
    snap_confirmed(frame, "blind: A on the menu's own selection");
}

// For a menu input with a placed highlight: the column / row (the highlight's terms) to select now, or false.
static bool aim(int *dc, int *dr, int *to_col, int *to_row) {
    const Place *p = place_of(input.obj);
    if (!p) return false;
    int hc, hr;
    bool over = hover(p, &hc, &hr);
    *to_col = hc, *to_row = hr, *dc = over ? hc - p->col : 0, *dr = over ? hr - p->row : 0;
    if (over && input.kind == IN_CURSOR && (*dc || *dr)) {  // past the cursor's first / last item: beside the menu
        int d[2] = {0, 0}, ah = cursor_axis(input.obj, false), av = cursor_axis(input.obj, true);
        if (*dc && ah >= 0) d[ah] += *dc;
        if (*dr && av >= 0) d[av] += *dr;
        if ((*dc && ah < 0) || (*dr && av < 0) || !cursor_fits(input.obj, d[0], d[1])) over = false, *dc = *dr = 0;
    }
    return want_select(over, *dc, *dr);
}

// ---- menus that show the selection as an animation of the item itself (SELECT among DEFAULT)
#define GROUP_MAX 32
typedef struct {
    uint32_t inst[GROUP_MAX];  // in screen order: rows top to bottom, left to right in a row
    int n, sel;
    float px, py;              // the smallest step between items across / down (0: a single row / column)
    int by;                    // the cursor's reading of their order: 0 its column, 1 its row, 2 its whole index
} Group;
typedef struct { int col, cols, row; } CurPos;

static int32_t anim_sequence(uint32_t inst) {
    uint32_t g = LD32(inst + INST_ANIM);
    if (!sane(g)) return -2;
    uint32_t s = LD32(g + 0x94u);
    return sane(s) ? (int32_t)LD32(s) : -1;
}

static bool same_row(uint32_t a, uint32_t b) { return fabsf(f32(a + INST_Y) - f32(b + INST_Y)) < 8.0f; }

static bool same_place(uint32_t a, uint32_t b) {
    return fabsf(f32(a + INST_X) - f32(b + INST_X)) < 2.0f && fabsf(f32(a + INST_Y) - f32(b + INST_Y)) < 2.0f;
}

// The item group in GUI `own` for cursor position `cp`: visible sibling animation instances on the screen, one of them
// animated lately (`recent`), the one at the cursor's position the selection - as it shows: in a sequence no other
// item has (SELECT among DEFAULT), or under the menu's highlight (another instance standing where it stands), or
// simply as many items as the cursor has columns.
static bool find_group(uint32_t own, uint32_t only, CurPos cp, const uint32_t *recent, int nrecent, Group *out) {
    uint32_t n = LD32(own + OWN_NINST), arr = LD32(own + OWN_INST);
    if (!sane(arr) || n > 1024) return false;
    uint32_t done[64];
    int ndone = 0;
    bool found = false;
    for (uint32_t i = 0; i < n && ndone < 64; i++) {
        uint32_t a = LD32(arr + 4u * i);
        if (!sane(a) || !is_anim(a) || !(LD32(a + INST_FLAGS) & 1)) continue;
        uint32_t par = LD32(a + INST_PARENT);
        if (only && par != only) continue;
        bool seen = false;
        for (int k = 0; k < ndone; k++) seen |= done[k] == par;
        if (seen) continue;
        done[ndone++] = par;
        if (inst_ok(par) && !inst_shows(par)) continue;
        Group g = {.n = 0, .sel = -1};
        for (uint32_t j = i; j < n && g.n < GROUP_MAX; j++) {
            uint32_t b = LD32(arr + 4u * j);
            if (!sane(b) || !is_anim(b) || !(LD32(b + INST_FLAGS) & 1) || LD32(b + INST_PARENT) != par || !inst_draws(b))
                continue;  // (one that draws nothing is no item)
            int at = g.n++;  // insertion sort into screen order
            while (at > 0) {
                uint32_t q = g.inst[at - 1];
                bool before = same_row(b, q) ? f32(b + INST_X) < f32(q + INST_X) : f32(b + INST_Y) < f32(q + INST_Y);
                if (!before) break;
                g.inst[at] = q, at--;
            }
            g.inst[at] = b;
        }
        if (g.n < 2) continue;
        // where the cursor is among them: a column of one page, an index in a grid, a row of a single column
        g.by = cp.cols > 1 ? (g.n <= cp.cols ? 0 : 2) : 1;
        int at = g.by == 0 ? cp.col : g.by == 1 ? cp.row : cp.row * cp.cols + cp.col;
        if (at < 0 || at >= g.n) continue;
        bool own_seq = true, under = false;  // (a sequence of its own; another instance standing on it)
        for (int m = 0; m < g.n; m++)
            if (m != at && anim_sequence(g.inst[m]) == anim_sequence(g.inst[at])) own_seq = false;
        for (uint32_t j = 0; j < n && !own_seq && !under; j++) {
            uint32_t b = LD32(arr + 4u * j);
            if (!sane(b) || LD32(b + INST_PARENT) == par || !(LD32(b + INST_FLAGS) & 1) || !same_place(b, g.inst[at])) continue;
            int on = 0;  // (only on the selected one: decorations stand on every item)
            for (int m = 0; m < g.n; m++) on += same_place(b, g.inst[m]);
            under = on == 1;
        }
        if (!own_seq && !under && !(g.by == 0 && g.n == cp.cols)) continue;
        g.sel = at;
        bool animated = !nrecent;  // (the menu animated one of them lately)
        for (int k = 0; k < g.n && !animated; k++)
            for (int m = 0; m < nrecent && !animated; m++) animated = g.inst[k] == recent[m];
        if (!animated) continue;
        // laid out on the screen, each in a place of its own
        bool placed = true;
        g.px = g.py = 0.0f;
        for (int k = 0; k < g.n && placed; k++) {
            float x = f32(g.inst[k] + INST_X), y = f32(g.inst[k] + INST_Y);
            placed = x > -64.0f && x < g_ui_size[0] + 64.0f && y > -64.0f && y < g_ui_size[1] + 64.0f;
            if (!k) continue;
            float dx = fabsf(x - f32(g.inst[k - 1] + INST_X)), dy = fabsf(y - f32(g.inst[k - 1] + INST_Y));
            if (same_row(g.inst[k], g.inst[k - 1])) { if (dx <= 4.0f) placed = false; else if (!g.px || dx < g.px) g.px = dx; }
            else if (!g.py || dy < g.py) g.py = dy;
        }
        if (!placed) continue;
        *out = g;
        found = true;
        break;
    }
    return found;
}

// The item of a group under the pointer, -1: none. Nearest by its position, within half a step (a single column
// reaches across the screen's middle, a single row a little above and below).
static int group_hover(const Group *g) {
    if (!ptr_inside) return -1;
    int best = -1;
    float bd = 1e30f;
    for (int k = 0; k < g->n; k++) {
        float dx = fabsf(ptr_x - f32(g->inst[k] + INST_X)), dy = fabsf(ptr_y - f32(g->inst[k] + INST_Y));
        float lx = g->px ? g->px * 0.5f : 480.0f, ly = g->py ? g->py * 0.5f : 40.0f;
        if (dx > lx || dy > ly) continue;
        if (dx + dy < bd) bd = dx + dy, best = k;
    }
    return best;
}

// The item group of menu input `in`: in the GUI it was found in before, else in one of those animated since its menu
// opened (a menu sets its items' sequences as it opens), the latest first.
static bool group_of(uint32_t in, CurPos cp, Group *g) {
    static int next;
    for (int i = 0; i < 4; i++)
        if (group_owner[i].input == in && group_owner[i].caller == input.caller &&
            find_group(group_owner[i].owner, group_owner[i].parent, cp, NULL, 0, g))
            return true;
    uint64_t since = run_start(in) - 2, tried = UINT64_MAX;
    for (int n = 0; n < NSEQ; n++) {  // (newest first)
        int best = -1;
        for (int i = 0; i < NSEQ; i++)
            if (seq_owner[i].owner && seq_owner[i].frame >= since && seq_owner[i].frame < tried &&
                (best < 0 || seq_owner[i].frame > seq_owner[best].frame))
                best = i;
        if (best < 0) break;
        tried = seq_owner[best].frame;
        uint32_t own = seq_owner[best].owner;
        int nrecent = seq_owner[best].ninst < 8 ? seq_owner[best].ninst : 8;
        if (!find_group(own, 0, cp, seq_owner[best].inst, nrecent, g)) continue;
        group_owner[next].input = in, group_owner[next].caller = input.caller, group_owner[next].owner = own;
        group_owner[next].parent = LD32(g->inst[0] + INST_PARENT);
        next = (next + 1) % 4;
        return true;
    }
    return false;
}

static void ran(int kind, uint32_t obj, uint32_t caller) {
    uint64_t now = g_present_count;
    if (now - input.frame > FRESH_FRAMES && log_on()) LOG("[directmouse] menu opens\n");
    int r = run_of(obj, caller);
    if (r < 0) {
        r = 0;
        for (int i = 1; i < 8; i++)
            if (runs[i].last < runs[r].last) r = i;
    }
    bool starts = runs[r].obj != obj || runs[r].caller != caller || now - runs[r].last > FRESH_FRAMES;
    if (starts) {  // (this input starts a run: its menu opens)
        runs[r].obj = obj, runs[r].caller = caller, runs[r].first = now, runs[r].known = 0;
        for (int i = 0; i < 4; i++)  // (its GUI may be another one now)
            if (group_owner[i].input == obj && group_owner[i].caller == caller) group_owner[i].input = 0;
    }
    runs[r].last = now;
    input.kind = kind, input.obj = obj, input.caller = caller, input.frame = now;
    if (kind == IN_BOX) box_in.obj = obj, box_in.caller = caller, box_in.frame = now;
    if (starts) snap_open(obj, caller);
}

// Moves the cursor by d[0] columns and d[1] rows (its own) as a move of its own would: the last move's flag, its sound;
// returns the button such a move returns.
static uint32_t cursor_move(Cpu *c, uint32_t cur, const int d[2]) {
    if (d[0]) ST8(cur + CUR_COL, LD8(cur + CUR_COL) + d[0]);
    if (d[1]) ST16(cur + CUR_ROW, LD16(cur + CUR_ROW) + d[1]);
    int a = d[1] ? 1 : 0;  // the move it reads as: along the row axis if that moved, else the column axis
    uint32_t mode = LD32(cur + CUR_MODE + 4u * a), btn = 0;
    ST32(cur + CUR_MOVE, (a ? 4u : 1u) << (d[a] > 0));
    if (mode && mode < CURSOR_NMODES) {
        btn = LD32(CURSOR_MODES + 16u * mode + (d[a] > 0 ? 4u : 0u));
        if (LD32(CURSOR_MODES + 16u * mode + 8u) == 0) play_sound(c, LD32(CURSOR_MODES + 16u * mode + 12u));
    }
    return btn ? btn : 1u;
}

// ---- menus whose items are placed freely, with a highlight of their own by the selected one (an animation named
// ..cursor..): the world map's destinations, the character creation's grid. The items are the other animations of
// the highlight's GUI that draw (not inside another one, not a backdrop; one mostly inside another is part of it); the
// selected one is the item the highlight overlaps most, else the one in its row (the creation's Done: a bar measured
// short), in its column, or the nearest. Which item each of the cursor's places selects is learned as the cursor
// moves; the pointer's item is reached by setting the cursor to the place that selects it, or else to the places not
// seen yet, toward it first. A highlight that doesn't follow the cursor is another menu's: it is left alone.
#define FREE_MAX 24
typedef struct {
    uint32_t own, hl;  // (hl 0: none shows - which item is selected isn't known)
    int n, sel;
    uint32_t inst[FREE_MAX];
    Box box[FREE_MAX];
} Free;

// One for each menu input lately (the world map runs two cursors: its destinations' and another's, one highlight).
typedef struct {
    uint32_t input, caller, own, hl, target, bad[4];  // (bad: highlights that didn't follow the cursor)
    int n, nbad, start;
    bool follows;          // its highlight went to two items or more as the cursor moved: the cursor's own
    uint64_t run, moved, used;  // (the run of the input it is for; the frame the cursor was last set; last used)
    uint32_t item[64];     // the item each place selects (0: not seen)
} Walker;
static Walker walkers[4], *fw = &walkers[0];

// The walker of menu input `in` (from input.caller), or NULL; make: else the one used least lately, for it.
static Walker *walker_of(uint32_t in, uint32_t caller, bool make) {
    Walker *w = &walkers[0];
    for (int i = 0; i < 4; i++) {
        if (walkers[i].input == in && walkers[i].caller == caller) return &walkers[i];
        if (walkers[i].used < w->used) w = &walkers[i];
    }
    if (!make) return NULL;
    memset(w, 0, sizeof *w);
    w->input = in, w->caller = caller;
    return w;
}

// Whether highlight h follows another menu input's cursor (a walker used lately has seen it go to its items).
static bool walker_claimed(uint32_t h) {
    for (int i = 0; i < 4; i++)
        if (&walkers[i] != fw && walkers[i].hl == h && walkers[i].follows && g_present_count - walkers[i].used <= FRESH_FRAMES)
            return true;
    return false;
}

static bool free_collect(uint32_t own, uint32_t h, Free *F) {
    uint32_t n = LD32(own + OWN_NINST), arr = LD32(own + OWN_INST);
    if (!sane(arr) || n > 4096) return false;
    float big = 0.2f * (float)g_ui_size[0] * (float)g_ui_size[1], wsum = 0.0f, hsum = 0.0f;
    int nsized = 0;
    F->own = own, F->hl = h, F->n = 0, F->sel = -1;
    for (uint32_t i = 0; i < n && F->n < FREE_MAX; i++) {
        uint32_t a = LD32(arr + 4u * i);
        if (!inst_ok(a) || !is_anim(a) || a == h || !inst_shows(a)) continue;
        bool inner = false;  // (inside another animation, or holding the highlight)
        for (uint32_t q = LD32(a + INST_PARENT), d = 0; inst_ok(q) && d < 32 && !inner; q = LD32(q + INST_PARENT), d++)
            inner = is_anim(q);
        for (uint32_t q = h ? LD32(h + INST_PARENT) : 0, d = 0; inst_ok(q) && d < 32 && !inner; q = LD32(q + INST_PARENT), d++)
            inner = q == a;
        bool drawn;
        Box b = inst_box_drawn(a, &drawn);
        float w = b.x1 - b.x0, ht = b.y1 - b.y0;
        if (inner || !drawn || w * ht > big || ((w > 0.0f || ht > 0.0f) && (w < 12.0f || ht < 12.0f))) continue;
        if (w > 0.0f) nsized++, wsum += w, hsum += ht;
        F->inst[F->n] = a, F->box[F->n++] = b;
    }
    for (int k = 0; k < F->n && nsized; k++)  // texts alone: as big as the others are, around their middle
        if (F->box[k].x1 <= F->box[k].x0) {
            float cx = F->box[k].x0, cy = F->box[k].y0, hw = wsum / (float)nsized * 0.5f, hh = hsum / (float)nsized * 0.5f;
            F->box[k] = (Box){cx - hw, cy - hh, cx + hw, cy + hh};
        }
    for (int k = 0; k < F->n; k++) {  // (the hired hunter's figure on its stand: part of it)
        Box a = F->box[k];
        float aa = (a.x1 - a.x0) * (a.y1 - a.y0);
        bool part = false;
        for (int m = 0; m < F->n && !part; m++) {
            Box b = F->box[m];
            float ix = fminf(a.x1, b.x1) - fmaxf(a.x0, b.x0), iy = fminf(a.y1, b.y1) - fmaxf(a.y0, b.y0);
            part = m != k && ix > 0.0f && iy > 0.0f && ix * iy >= 0.7f * aa && aa < (b.x1 - b.x0) * (b.y1 - b.y0);
        }
        if (part) F->n--, F->inst[k] = F->inst[F->n], F->box[k] = F->box[F->n], k--;
    }
    if (F->n < 2) return false;
    if (!h) return true;
    Box hb = inst_box(h);
    float hx = (hb.x0 + hb.x1) * 0.5f, hy = (hb.y0 + hb.y1) * 0.5f, best = -1e30f;
    for (int k = 0; k < F->n; k++) {
        Box b = F->box[k];
        float ox = fminf(b.x1, hb.x1) - fmaxf(b.x0, hb.x0), oy = fminf(b.y1, hb.y1) - fmaxf(b.y0, hb.y0);
        float dx = fabsf((b.x0 + b.x1) * 0.5f - hx), dy = fabsf((b.y0 + b.y1) * 0.5f - hy);
        float s = ox > 0.0f && oy > 0.0f ? 1e7f + ox * oy : oy > 0.0f ? 1e6f - dx : ox > 0.0f ? 1e5f - dy : -(dx + dy);
        if (s > best) best = s, F->sel = k;
    }
    return true;
}

// The freely placed items of the menu input fw is for: by the highlight it used before, else by the first that shows
// (not one found not to follow it); with none showing, the items of the GUI used before.
static bool free_items(Free *F) {
    int prev = live_cur ^ 1;
    for (int pass = 0; pass < 2; pass++)
        for (int j = 0; j < nlive[prev]; j++) {
            uint32_t own = live[prev][j];
            if ((pass == 0 && own != fw->own) || !gui_shows(own)) continue;
            Parts *p = parts_of(own);
            for (int i = 0; i < p->nhilite; i++) {
                uint32_t h = owner_instance(own, p->hilite[i]);
                bool skip = !h || (pass == 0 && h != fw->hl) || !inst_shows(h) || walker_claimed(h);
                for (int b = 0; b < fw->nbad && !skip; b++) skip = fw->bad[b] == h;
                if (skip) continue;
                Box hb = inst_box(h);
                if (hb.x1 - hb.x0 >= 8.0f && hb.y1 - hb.y0 >= 8.0f && free_collect(own, h, F)) return true;
            }
        }
    for (int j = 0; j < nlive[prev] && fw->own; j++)
        if (live[prev][j] == fw->own && gui_shows(fw->own)) return free_collect(fw->own, 0, F);
    return false;
}

// For menu input `in` with layout L: true when its items are found so (want_select has the pointer over one or not);
// *to: the place (row x columns + column on the page) to set the cursor to now, -1: none.
static bool free_walk(uint32_t in, const Layout *L, int *to) {
    *to = -1;
    if (L->n < 2 || L->n > 64) return false;
    uint64_t now = g_present_count, run = run_start(in);
    fw = walker_of(in, input.caller, true);
    if (fw->n != L->n || fw->run != run) {  // (each time it opens)
        memset(fw, 0, sizeof *fw);
        fw->input = in, fw->caller = input.caller, fw->n = L->n, fw->run = run;
    }
    fw->used = now;
    Free F;
    if (!free_items(&F) || L->shown < F.n) return false;  // (fewer places than items: not this cursor's - the world
                                                           // map's other one)
    if (F.own != fw->own) {
        memset(fw->item, 0, sizeof fw->item), fw->own = F.own, fw->target = 0;
        if (log_on())
            LOG("[directmouse] items of %08x from %08x: %d placed freely in %s, highlight %s\n", in, input.caller, F.n,
                gui_name(F.own), F.hl ? inst_name(F.own, F.hl) : "-");
    }
    if (F.hl && F.hl != fw->hl) fw->hl = F.hl, fw->follows = false;
    uint32_t sel = F.sel >= 0 ? F.inst[F.sel] : 0;
    if (sel && now - fw->moved >= 2) fw->item[L->at] = sel;  // (the highlight has followed the last move)
    for (int p = 0; p < L->shown && !fw->follows; p++)
        fw->follows = fw->item[p] && fw->item[p] != fw->item[L->at];
    if (!sel) sel = fw->item[L->at];
    int k = -1;  // the item under the pointer: the smallest that holds it
    float ka = 1e30f;
    for (int i = 0; i < F.n && ptr_inside; i++) {
        Box b = F.box[i];
        float a = (b.x1 - b.x0) * (b.y1 - b.y0);
        if (ptr_x >= b.x0 - 4.0f && ptr_x <= b.x1 + 4.0f && ptr_y >= b.y0 - 4.0f && ptr_y <= b.y1 + 4.0f && a < ka) ka = a, k = i;
    }
    bool mapped = false;  // (a place seen selects it)
    for (int p = 0; p < L->shown && k >= 0 && !mapped; p++) mapped = fw->item[p] == F.inst[k];
    // on the selection: the item selected or - not known which - one no place seen selects (the places not seen select
    // the items not seen)
    bool on = k >= 0 && (sel ? F.inst[k] == sel : !mapped);
    if (want_select(k >= 0, on ? 0 : 1, 0)) fw->target = F.inst[k], fw->start = L->at;
    else if (k < 0 || F.inst[k] != fw->target) fw->target = 0;  // (the pointer left the item being reached)
    if (on) fw->target = 0;
    if (!fw->target || now - fw->moved < 2) return true;
    for (int p = 0; p < L->shown && *to < 0; p++)
        if (fw->item[p] == fw->target) *to = p;
    int s = -1, dir = 1;  // (the way toward it: from the selected item's place on the screen)
    for (int i = 0; i < F.n; i++)
        if (F.inst[i] == sel) s = i;
    if (s >= 0) {
        float dx = F.box[k].x0 + F.box[k].x1 - F.box[s].x0 - F.box[s].x1;
        float dy = F.box[k].y0 + F.box[k].y1 - F.box[s].y0 - F.box[s].y1;
        dir = (fabsf(dx) >= fabsf(dy) ? dx : dy) >= 0.0f ? 1 : -1;
    }
    for (int e = 0; e < 2 && *to < 0; e++)  // the nearest place not seen yet toward it, else the other way
        for (int p = L->at + (e ? -dir : dir); p >= 0 && p < L->shown && *to < 0; p += e ? -dir : dir)
            if (!fw->item[p]) *to = p;
    if (*to < 0) {  // every place seen, none selects it: not one of this menu's items - back where it was
        int distinct = 0;
        for (int p = 0; p < L->shown; p++) {
            bool again = false;
            for (int q = 0; q < p && !again; q++) again = fw->item[q] == fw->item[p];
            distinct += !again && fw->item[p];
        }
        if (distinct <= 1 && fw->hl && fw->nbad < 4) {  // (its highlight never followed the cursor: another menu's)
            if (log_on())
                LOG("[directmouse] %s.%s doesn't follow %08x: not its highlight\n", gui_name(fw->own), inst_name(fw->own, fw->hl), in);
            fw->bad[fw->nbad++] = fw->hl, fw->hl = 0;
            memset(fw->item, 0, sizeof fw->item);
        }
        fw->target = 0, *to = fw->start != L->at ? fw->start : -1;
    }
    if (*to >= 0) fw->moved = now;
    return true;
}

// The patched end of cCursor's update (tools/gen.py PATCHES, 0x74f228: r4 the cursor, r11 the button that moved it):
// returns what the update returns. The item under the pointer becomes the selection here, as a move of the cursor.
uint32_t dmouse_cursor_done(Cpu *c, uint32_t moved) {
    uint32_t cur = c->r[4];
    if (!g_config.direct_mouse || !sane(cur) || LD32(cur) != CUR_VTABLE) return moved;
    ran(IN_CURSOR, cur, LD32(c->r[13] + 0x2cu));  // (the update's return address, under its saved registers)
    if (moved) watch();
    int d[2] = {0, 0};
    int pa = page_axis(cur);
    if (!moved && pa >= 0 && page_for(cur)) {  // its page arrow clicked: the axis that flips this cursor's pages
        int count = pa ? LD16(cur + CUR_ROWS) : LD8(cur + CUR_COLS), at = pa ? LD16(cur + CUR_ROW) : LD8(cur + CUR_COL);
        d[pa] = page_request, page_request = 0;
        if (at + d[pa] < 0 || at + d[pa] >= count) d[pa] = d[pa] > 0 ? -at : count - 1 - at;  // (round, both ways)
        if (pa == 1 && !cursor_fits(cur, d[0], d[1])) {  // (onto a last page with fewer items: its last one)
            int cols = LD8(cur + CUR_COLS), last = LD16(cur + CUR_ITEMS) - 1 - (LD16(cur + CUR_ROW) + d[1]) * cols;
            if (last >= 0 && last < cols) d[0] = last - LD8(cur + CUR_COL);
        }
        if (!d[pa] || !cursor_fits(cur, d[0], d[1])) return moved;
        uint32_t btn = cursor_move(c, cur, d);
        if (log_on()) LOG("[directmouse] cursor %08x: page %+d\n", cur, d[pa]);
        return btn;
    }
    if (place_of(cur)) {
        place_claim(place_of(cur));
        int dc, dr, tc, tr;
        if (!aim(&dc, &dr, &tc, &tr) || moved) return moved;  // (the d-pad moved it this frame: that goes first)
        // the highlight's columns / rows are the cursor's left-right / up-down axes
        int ah = cursor_axis(cur, false), av = cursor_axis(cur, true);
        if (dc && ah >= 0) d[ah] += dc;
        if (dr && av >= 0) d[av] += dr;
    } else {
        int col = LD8(cur + CUR_COL), cols = LD8(cur + CUR_COLS), row = LD16(cur + CUR_ROW);
        CurPos cp = {col, cols, row};
        Group g;
        Layout L;
        uint32_t own;
        Numbered G;
        if (layout_of(cur, &L) && numbered_of(cur, &L, &own, &G)) {  // the menu's numbered items
            int k = numbered_hover(own, &G, &L);
            if (k >= 0) {
                if (L.kind == 0) d[0] = k - col;
                else if (L.kind == 1) d[1] = k - row;
                else d[0] = k % L.cols - col, d[1] = k / L.cols - row;
                if (!cursor_fits(cur, d[0], d[1])) k = -1, d[0] = d[1] = 0;  // (past its last item: beside the menu)
            }
            if (!want_select(k >= 0, k - L.at, 0) || moved) return moved;
            goto select;
        }
        if (!group_of(cur, cp, &g)) {  // else items shown selected by an animation; else placed freely
            int to;
            if (!layout_of(cur, &L) || !free_walk(cur, &L, &to)) { click_blind(); return moved; }
            if (to < 0 || moved) return moved;
            if (L.kind == 0) d[0] = to - col;
            else if (L.kind == 1) d[1] = to - row;
            else d[0] = to % L.cols - col, d[1] = to / L.cols - row;
            goto select;
        }
        static uint32_t logged;
        if (log_on() && logged != (g.inst[0] ^ (uint32_t)g.n)) {
            logged = g.inst[0] ^ (uint32_t)g.n;
            LOG("[directmouse] items of %08x: %d animations from %.0f,%.0f (steps %.0f, %.0f), selected %d\n", cur, g.n,
                f32(g.inst[0] + INST_X), f32(g.inst[0] + INST_Y), g.px, g.py, g.sel);
        }
        int k = group_hover(&g);
        int dk = k - g.sel, to = row * cols + col + dk;
        if (k >= 0) {
            if (g.by == 0) d[0] = dk;  // the items are its columns, its rows, or its whole index
            else if (g.by == 1) d[1] = dk;
            else if (cols > 0 && to >= 0) d[0] = to % cols - col, d[1] = to / cols - row;
            if (!cursor_fits(cur, d[0], d[1])) k = -1, d[0] = d[1] = 0;
        }
        if (!want_select(k >= 0, k - g.sel, 0) || moved) return moved;
    }
select:
    if ((!d[0] && !d[1]) || !cursor_fits(cur, d[0], d[1])) return moved;  // past the last item, or no such axis
    uint32_t btn = cursor_move(c, cur, d);
    if (log_on())
        LOG("[directmouse] pointer %.0f,%.0f: cursor %08x by column %+d, row %+d\n", ptr_x, ptr_y, cur, d[0], d[1]);
    return btn;
}

// The patched entry of the Yes/No choice (choice byte *, pad trigger bits, buttons to Yes, to No) (0x74fbb0): the
// answer under the pointer becomes the choice.
void dmouse_yesno(Cpu *c) {
    if (!g_config.direct_mouse || c->r[0] < 0x08000000u) return;
    uint32_t sel = c->r[0];
    ran(IN_YESNO, sel, c->r[14]);
    uint32_t dlg = sel - DIALOG_CHOICE, res = sane(dlg) && LD32(dlg) == DIALOG_VTABLE ? LD32(dlg + 4u) : 0;
    uint32_t ans = sane(res) && strstr((const char *)G2H(res + RES_PATH), "window_dialog") ? instance(dlg, DIALOG_ANSWER) : 0;
    if (ans) place_set((Place){PL_YESNO, sel, 0, ans, 0, 0, LD8(sel) != 0, 0, 0.0f, g_present_count});
    else { click_blind(); return; }  // (another box: a click is A on the current choice)
    int dc, dr, tc, tr;
    if (!aim(&dc, &dr, &tc, &tr)) return;
    uint32_t r1 = c->r[1], r2 = c->r[2], r3 = c->r[3];
    ST8(sel, tc);
    play_sound(c, SND_YESNO);
    c->r[0] = sel, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3;
    if (log_on()) LOG("[directmouse] pointer %.0f,%.0f: %s\n", ptr_x, ptr_y, tc ? "No" : "Yes");
}

// The patched entry of uGUIResourceOwner's setSequence(owner, id, sequence) (0x746c74): the GUIs animated lately
// (where to look for the items of a menu without a highlight of its own; a menu sets its items' sequences as it
// opens and as its selection moves).
void dmouse_seq(Cpu *c) {
    if (!g_config.direct_mouse) return;
    uint32_t own = c->r[0];
    if (watching() && owner_instance(own, c->r[1]))
        LOG("[gui] move: sequence %s.%s = %d from %08x\n", gui_name(own), inst_name(own, owner_instance(own, c->r[1])),
            (int)c->r[2], c->r[14]);
    int slot = 0;
    for (int i = 0; i < NSEQ; i++) {
        if (seq_owner[i].owner == own) { slot = i; break; }
        if (seq_owner[i].frame < seq_owner[slot].frame) slot = i;
    }
    if (seq_owner[slot].owner != own) seq_owner[slot].owner = own, seq_owner[slot].ninst = 0;
    seq_owner[slot].frame = g_present_count;
    uint32_t inst = owner_instance(own, c->r[1]);
    bool known = false;
    for (int i = 0; i < seq_owner[slot].ninst && i < 8; i++) known |= seq_owner[slot].inst[i] == inst;
    if (inst && !known) seq_owner[slot].inst[seq_owner[slot].ninst++ % 8] = inst;
}

// The patched entries of cUIObject's highlight placement: kind 0 (this, cursor id, next id, row) 0x77033c, kind 1
// (..., column) 0x770378, kind 2 (this, cursor id, next column id, next row id; column, row on the stack) 0x7703b4.
// It belongs to the menu input that ran just before it.
void dmouse_place(Cpu *c, int kind) {
    if (watching() && sane(c->r[0]) && instance(c->r[0], c->r[1]))
        LOG("[gui] move: highlight %d of %s.%s, %08x %08x, from %08x\n", kind, gui_name(LD32(c->r[0] + UIO_OWNER)),
            inst_name(LD32(c->r[0] + UIO_OWNER), instance(c->r[0], c->r[1])), c->r[3], LD32(c->r[13]), c->r[14]);
    if (!g_config.direct_mouse || !sane(c->r[0]) || input.kind == IN_NONE || g_present_count - input.frame > 1) return;
    uint32_t cur = instance(c->r[0], c->r[1]), nx = instance(c->r[0], c->r[2]);
    uint32_t ny = kind == PL_CELL ? instance(c->r[0], c->r[3]) : 0;
    if (!cur || !nx || (kind == PL_CELL && !ny)) return;
    int col = kind == PL_ROW ? 0 : kind == PL_COL ? (int)c->r[3] : (int)LD32(c->r[13]);
    int row = kind == PL_ROW ? (int)c->r[3] : kind == PL_COL ? 0 : (int)LD32(c->r[13] + 4u);
    place_set((Place){kind, input.obj, 0, cur, nx, ny, col, row, 0.0f, g_present_count});
}

// The patched entry of uGUIResourceOwner's setY(owner, id, y in s0) (0x745ac0): from one of INLINE_ROWS, a menu
// putting its highlight on row (cursor column) x step.
void dmouse_sety(Cpu *c) {
    if (g_config.direct_mouse && owner_instance(c->r[0], c->r[1])) hl_note(c->r[0], owner_instance(c->r[0], c->r[1]), 2, 0.0f, c->v.f[0]);
    if (watching() && owner_instance(c->r[0], c->r[1]))
        LOG("[gui] move: setY %s.%s = %.1f from %08x\n", gui_name(c->r[0]), inst_name(c->r[0], owner_instance(c->r[0], c->r[1])),
            c->v.f[0], c->r[14]);
    if (!g_config.direct_mouse || input.kind != IN_CURSOR || g_present_count - input.frame > 1) return;
    uint32_t lr = c->r[14];
    int lo = 0, hi = (int)(sizeof INLINE_ROWS / sizeof *INLINE_ROWS);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (INLINE_ROWS[mid] < lr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == (int)(sizeof INLINE_ROWS / sizeof *INLINE_ROWS) || INLINE_ROWS[lo] != lr) return;
    uint32_t cur = owner_instance(c->r[0], c->r[1]);
    if (!cur) return;
    int row = LD8(input.obj + CUR_COL);
    const Place *p = place_of(input.obj);
    // the step: the value over its row; on row 0 the one seen before, at first the highlight's height
    float step = row > 0 ? c->v.f[0] / (float)row
                 : p && p->cur == cur && p->step > 0.0f ? p->step : 2.0f * fabsf(f32(cur + INST_SY));
    place_set((Place){PL_ROW, input.obj, 0, cur, 0, 0, 0, row, step, g_present_count});
}

// A text instance's text, as far as it can be found: the first pointer in it to a string of letters.
static void inst_text(uint32_t inst, char *out, size_t n) {
    out[0] = 0;
    for (uint32_t k = 0x80u; k < 0x1a0u; k += 4u) {
        uint32_t p = LD32(inst + k);
        if (p < 0x08000000u || p >= 0xF0000000u) continue;
        const unsigned char *s = (const unsigned char *)G2H(p);
        size_t len = 0;
        while (len < n - 1 && (s[len] >= 0x20 && s[len] != 0x7f)) len++;
        if (len < 2 || s[len]) continue;
        snprintf(out, n, " text@%x \"%.*s\"", k, (int)len, (const char *)s);
        return;
    }
}

static void dump_tree(uint32_t own, uint32_t inst, int depth, int *budget) {
    for (int n = 0; inst_ok(inst) && *budget > 0 && n < 512; inst = LD32(inst + INST_NEXT), n++) {
        if (!(LD32(inst + INST_FLAGS) & 1)) continue;
        (*budget)--;
        char seq[64] = "", text[80] = "";
        if (is_anim(inst)) {
            Box b = inst_box(inst);
            snprintf(seq, sizeof seq, " seq %d box %.0f,%.0f-%.0f,%.0f", (int)anim_sequence(inst), b.x0, b.y0, b.x1, b.y1);
        }
        if (LD32(inst) == 0x0198f7acu || LD32(inst) == 0x0198f848u) inst_text(inst, text, 64);
        LOG("[gui] %*s%u %s (%s) %.0f,%.0f scale %.2f,%.2f%s%s\n", 2 * depth + 2, "", LD32(inst + INST_ID), inst_name(own, inst),
            inst_kind(inst), f32(inst + INST_X), f32(inst + INST_Y), f32(inst + INST_SX), f32(inst + INST_SY), seq, text);
        if (depth < 12) dump_tree(own, LD32(inst + INST_CHILD), depth + 1, budget);
    }
}

static void gui_dump(void) {
    int prev = live_cur ^ 1, budget = 900;
    LOG("[gui] ---- frame %llu: %d GUIs run, pointer %.0f,%.0f\n", (unsigned long long)g_present_count, nlive[prev], ptr_x, ptr_y);
    for (int i = 0; i < 8; i++) {
        uint32_t o = runs[i].obj;
        if (!o || g_present_count - runs[i].last > 8) continue;
        if (LD32(o) == CUR_VTABLE)
            LOG("[gui] cursor %08x from %08x: column %u/%u, row %u/%u, %u items, modes %u %u\n", o, runs[i].caller, LD8(o + CUR_COL),
                LD8(o + CUR_COLS), LD16(o + CUR_ROW), LD16(o + CUR_ROWS), LD16(o + CUR_ITEMS), LD32(o + CUR_MODE), LD32(o + CUR_MODE + 4u));
        else if (LD32(o) == BOX_VTABLE)
            LOG("[gui] box cursor %08x from %08x: column %u/%u, row %u/%u, page %u/%u, %u items, kind %u\n", o, runs[i].caller,
                LD8(o + 0x14u), LD8(o + 0x16u), LD8(o + 0x15u), LD8(o + 0x17u), LD16(o + 0x18u), LD16(o + 0x1au), LD16(o + 0x1cu),
                LD16(o + 0x1eu));
        else LOG("[gui] input %08x from %08x\n", o, runs[i].caller);
    }
    for (int i = 0; i < nlive[prev] && budget > 0; i++) {
        uint32_t own = live[prev][i], res = LD32(own + OWN_RES);
        if (!gui_shows(own)) continue;
        LOG("[gui] GUI %08x %s\n", own, sane(res) ? (const char *)G2H(res + RES_PATH) : "?");
        dump_tree(own, LD32(LD32(own + OWN_ROOT) + INST_CHILD), 0, &budget);
    }
}

// MHGU_DIRECTMOUSE_LOG: a dump of each new screen once it has settled (the GUIs that show and the menu inputs that
// run, the same for 20 frames, not dumped lately), and the GUI calls of the frames a cursor moves in.
static uint64_t watch_until;
static void watch(void) { if (log_on()) watch_until = g_present_count + 3; }
static bool watching(void) { return watch_until && g_present_count <= watch_until; }

static void auto_dump(void) {
    static uint32_t sig_last, done[64];
    static int stable, next;
    uint32_t sig = 0;
    for (int i = 0; i < nlive[live_cur ^ 1]; i++)
        if (gui_shows(live[live_cur ^ 1][i])) sig = sig * 31u + live[live_cur ^ 1][i];
    for (int i = 0; i < 8; i++)
        if (runs[i].obj && g_present_count - runs[i].last <= FRESH_FRAMES) sig ^= runs[i].obj * 2654435761u ^ runs[i].caller;
    if (sig != sig_last) { sig_last = sig, stable = 0; return; }
    if (++stable != 20) return;
    for (int i = 0; i < 64; i++)
        if (done[i] == sig) return;
    done[next] = sig, next = (next + 1) % 64;
    gui_dump();
}

// The patched entries of uGUIResourceOwner's setX (0x745a40, kind 0) and setXY (0x745c20, kind 2; setY is
// dmouse_sety): the inspector's record of how a menu shows a move.
void dmouse_set(Cpu *c, int kind) {
    if (!g_config.direct_mouse || !owner_instance(c->r[0], c->r[1])) return;
    uint32_t own = c->r[0], inst = owner_instance(own, c->r[1]);
    hl_note(own, inst, kind == 2 ? 3 : 1, c->v.f[0], kind == 2 ? c->v.f[1] : 0.0f);
    if (!watching()) return;
    if (kind == 2)
        LOG("[gui] move: setXY %s.%s = %.1f,%.1f from %08x\n", gui_name(own), inst_name(own, inst), c->v.f[0], c->v.f[1], c->r[14]);
    else LOG("[gui] move: setX %s.%s = %.1f from %08x\n", gui_name(own), inst_name(own, inst), c->v.f[0], c->r[14]);
}

// The patched entry of cBoxCursor's update (0x7530b8: the cursor, pad trigger bits; returns the button that moved
// it): true when it is done here (r0 its result). The item box's and the equipment box's pages of cells: +0x14 / +0x15
// column / row, +0x16 / +0x17 columns / rows, +0x18 / +0x1a page / pages, +0x1c items.
int dmouse_box(Cpu *c) {
    uint32_t cur = c->r[0];
    if (!g_config.direct_mouse || !sane(cur) || LD32(cur) != BOX_VTABLE) return 0;
    ran(IN_BOX, cur, c->r[14]);
    if (c->r[1] & 0xf00f0u) { watch(); return 0; }  // (the d-pad moves it; the inspector records how the menu shows it)
    int pages = LD16(cur + BOX_PAGES);
    if (pages > 1 && page_for(cur)) {  // a page arrow clicked: the next / previous page (round, as its own moves go)
        int dir = page_request, page = (LD16(cur + BOX_PAGE) + dir + pages) % pages, n = LD8(cur + BOX_COLS) * LD8(cur + BOX_ROWS);
        page_request = 0;
        ST16(cur + BOX_PAGE, page);
        int left = LD16(cur + BOX_ITEMS) - page * n, at = LD8(cur + BOX_ROW) * LD8(cur + BOX_COLS) + LD8(cur + BOX_COL);
        if (n > 0 && at >= left && left > 0) ST8(cur + BOX_COL, (left - 1) % LD8(cur + BOX_COLS)), ST8(cur + BOX_ROW, (left - 1) / LD8(cur + BOX_COLS));
        ST32(cur + BOX_MOVE, dir > 0 ? 2u : 1u);
        play_sound(c, BOX_SOUND);
        c->r[0] = dir > 0 ? 0x20u : 0x80u;
        if (log_on()) LOG("[directmouse] box cursor %08x: page %d\n", cur, page);
        return 1;
    }
    Layout L;
    uint32_t own;
    Numbered G;
    const Place *pl = place_of(cur);
    int k = -1, hc, hr;
    if (!layout_of(cur, &L)) { click_blind(); return 0; }
    if (pl && pl->kind != PL_YESNO) {  // the menu's highlight helper: its rows / columns are the page's
        place_claim(pl);
        if (hover(pl, &hc, &hr)) {
            int col = pl->kind == PL_ROW ? LD8(cur + BOX_COL) : hc, row = pl->kind == PL_COL ? LD8(cur + BOX_ROW) : hr;
            if (col >= 0 && col < L.cols && row >= 0 && row < L.rows) k = row * L.cols + col;
        }
    } else if (numbered_of(cur, &L, &own, &G)) k = numbered_hover(own, &G, &L);
    else if (hl_known(cur)) { if (hl_hover(cur, &L, &hc, &hr)) k = hr * L.cols + hc; }
    else {  // items placed freely
        if (!free_walk(cur, &L, &k)) { click_blind(); return 0; }
        if (k < 0 || k == L.at) return 0;
        goto set;
    }
    int page = LD16(cur + BOX_PAGE);
    if (k >= 0 && page * L.n + k >= LD16(cur + BOX_ITEMS)) k = -1;  // (past the last item: beside the menu)
    if (!want_select(k >= 0, k - L.at, 0)) return 0;
set:;
    int col = k % L.cols, row = k / L.cols;
    int dc = col - LD8(cur + BOX_COL), dr = row - LD8(cur + BOX_ROW);
    ST8(cur + BOX_COL, col), ST8(cur + BOX_ROW, row);
    ST32(cur + BOX_MOVE, (dr ? dr : dc) > 0 ? 2u : 1u);
    play_sound(c, BOX_SOUND);
    c->r[0] = dr ? (dr > 0 ? 0x40u : 0x10u) : (dc > 0 ? 0x20u : 0x80u);  // (what a move of its own returns)
    if (log_on()) LOG("[directmouse] pointer %.0f,%.0f: box cursor %08x to column %d, row %d\n", ptr_x, ptr_y, cur, col, row);
    return 1;
}

// ---- the fuzzer (MHGU_DIRECTMOUSE_LOG; the control file's "fuzz [n]", F6): the pointer to n random places over the
// screen (most near the items found so far), each checked 5 frames later against what the game shows then: the item
// the menu shows selected - its highlight, or the item itself - must be under the pointer. Then each page arrow that
// shows is clicked: it must flip the page of the menu whose items are in its GUI. Hands off the mouse.
static struct {
    int left, wait, total, over, ok, miss;
    bool pending;
    float x, y;
    uint32_t rng;
    Box region;  // (where the items are: the selections seen, grown)
    int lat_n, lat_max;
    uint64_t lat_sum;  // (frames from a pointer move to the selection it makes)
    int arrow, na, astep, aok, aother, anone;  // the arrow being clicked (-1: none), how many, its step; the results
    Box abox[8];
    int8_t adir[8];
    uint32_t aown[8];
    struct { uint32_t obj, caller; int page; } before[8];  // (the pages of the menu inputs before a click)
} fz = {.arrow = -1};

void dmouse_fuzz(int n) {
    if (!log_on()) return;
    fz.left = fz.total = n > 0 ? n : 100, fz.over = fz.ok = fz.miss = 0, fz.pending = false, fz.wait = 0;
    fz.lat_n = fz.lat_max = 0, fz.lat_sum = 0, fz.arrow = -1;
    fz.region = (Box){0, 0, 0, 0}, fz.rng ^= (uint32_t)plat_now_ns();
    LOG("[fuzz] %d points\n", fz.total);
}

static void fz_picked(uint64_t frames) {
    if (!fz.left && !fz.pending) return;
    fz.lat_n++, fz.lat_sum += frames;
    if ((int)frames > fz.lat_max) fz.lat_max = (int)frames;
}

// The page a menu input shows (-1: none): a box's, a cursor's along its page axis.
static int input_page(uint32_t in) {
    if (!sane(in)) return -1;
    if (LD32(in) == BOX_VTABLE) return LD16(in + BOX_PAGES) > 1 ? LD16(in + BOX_PAGE) : -1;
    if (LD32(in) != CUR_VTABLE) return -1;
    int a = page_axis(in);
    return a < 0 ? -1 : a ? LD16(in + CUR_ROW) : LD8(in + CUR_COL);
}

// One step of the arrow phase: the pointer onto the arrow, 2 frames later the pages of the menu inputs that run and
// a click, 8 frames later which page it flipped.
static void fuzz_arrow(void) {
    if (fz.arrow >= fz.na) {
        LOG("[fuzz] arrows done: %d clicked: %d flipped their menu's page, %d another's or more, %d none\n", fz.na, fz.aok,
            fz.aother, fz.anone);
        fz.arrow = -1;
        return;
    }
    Box b = fz.abox[fz.arrow];
    float x = (b.x0 + b.x1) * 0.5f, y = (b.y0 + b.y1) * 0.5f;
    int dir = fz.adir[fz.arrow];
    uint32_t own = fz.aown[fz.arrow];
    if (fz.astep == 0) {
        ptr_x = x, ptr_y = y, ptr_inside = 1;
        __atomic_add_fetch(&ptr_seq, 1, __ATOMIC_RELAXED);
        fz.astep = 1, fz.wait = 2;
        return;
    }
    if (fz.astep == 1) {
        for (int i = 0; i < 8; i++) {
            bool on = runs[i].obj && g_present_count - runs[i].last <= FRESH_FRAMES;
            fz.before[i].obj = runs[i].obj, fz.before[i].caller = runs[i].caller, fz.before[i].page = on ? input_page(runs[i].obj) : -1;
        }
        fz_click = 1, fz.astep = 2, fz.wait = 8;
        return;
    }
    int flipped = 0, mine = 0;
    for (int i = 0; i < 8; i++) {
        int was = fz.before[i].page;
        int now = runs[i].obj == fz.before[i].obj && runs[i].caller == fz.before[i].caller ? input_page(runs[i].obj) : -1;
        if (was < 0 || now < 0 || now == was) continue;
        uint32_t keep = input.caller;
        input.caller = runs[i].caller;  // (input_gui reads it)
        uint32_t gui = input_gui(runs[i].obj);
        input.caller = keep;
        bool way = dir > 0 ? now > was || now == 0 : now < was || was == 0;  // (round from the last page to the first)
        bool ok = way;  // (a details page's cursor has no items of its own: its GUI may be another)
        flipped++, mine += ok;
        LOG("[fuzz] arrow %+d at %.0f,%.0f (%s): %s %08x from %08x page %d -> %d%s\n", dir, x, y, gui_name(own),
            LD32(runs[i].obj) == BOX_VTABLE ? "box cursor" : "cursor", runs[i].obj, runs[i].caller, was, now,
            !way ? " - the wrong way" : gui == own ? "" : gui ? " - its items are in another GUI" : " (its items aren't known)");
    }
    if (!flipped) LOG("[fuzz] arrow %+d at %.0f,%.0f (%s): no page flipped\n", dir, x, y, gui_name(own)), fz.anone++;
    else if (flipped == 1 && mine == 1) fz.aok++;
    else fz.aother++;
    fz.arrow++, fz.astep = 0;
}

// ---- the snap test (the control file's "snap x y [click]"): the pointer goes to (x, y) in the first frame the next
// menu input runs - a hand that moves as the menu opens, and clicks - and the frames until the item there is the
// selection (and until the click confirms) are logged. Several queue up, one for each menu that opens next.
static struct { float x, y; bool click; } snapq[16];
static volatile uint32_t snap_head, snap_tail;  // (the window thread queues, the game thread takes)
static struct { bool on, clicked; uint64_t frame; } snap;

void dmouse_snap(float x, float y, bool click) {
    if (!log_on() || snap_tail - snap_head >= 16) return;
    uint32_t t = snap_tail;
    snapq[t % 16].x = x, snapq[t % 16].y = y, snapq[t % 16].click = click;
    __atomic_store_n(&snap_tail, t + 1, __ATOMIC_RELEASE);
    LOG("[snap] queued: the pointer to %.0f,%.0f%s when a menu opens\n", x, y, click ? ", a click" : "");
}

static void snap_open(uint32_t obj, uint32_t caller) {
    uint32_t h = snap_head;
    if (snap.on || h == __atomic_load_n(&snap_tail, __ATOMIC_ACQUIRE)) return;
    float x = snapq[h % 16].x, y = snapq[h % 16].y;
    bool click = snapq[h % 16].click;
    snap_head = h + 1;
    snap.on = true, snap.frame = g_present_count, snap.clicked = click;
    ptr_x = x, ptr_y = y, ptr_inside = 1;
    __atomic_add_fetch(&ptr_seq, 1, __ATOMIC_RELAXED);
    if (click) click_ns = plat_now_ns();
    LOG("[snap] %08x from %08x opens in frame %llu: the pointer to %.0f,%.0f%s\n", obj, caller,
        (unsigned long long)snap.frame, x, y, click ? ", a click" : "");
}

static void snap_confirmed(uint64_t frame, const char *how) {
    if (!snap.clicked) return;
    snap.clicked = false;
    LOG("[snap] the click confirmed %llu frames after the menu opened (%s)\n", (unsigned long long)(frame - snap.frame), how);
}

static void snap_hit(uint64_t frame) {
    if (!snap.on) return;
    snap.on = false;
    int r = run_of(input.obj, input.caller);
    LOG("[snap] %08x from %08x: the item under the pointer is the selection %llu frames after the menu opened (its "
        "items known after %lld)\n", input.obj, input.caller, (unsigned long long)(frame - snap.frame),
        r >= 0 && runs[r].known ? (long long)(runs[r].known - runs[r].first) : -1LL);
}

static void snap_step(void) {  // (at a frame's start)
    if (snap.clicked && !click_ns) snap.clicked = false, LOG("[snap] the click went to no item (dropped)\n");
    if (!snap.on || g_present_count - snap.frame <= 120) return;
    snap.on = false;
    LOG("[snap] nothing selected under the pointer in 120 frames; the menu inputs that run:\n");
    for (int i = 0; i < 8; i++)
        if (runs[i].obj && g_present_count - runs[i].last <= FRESH_FRAMES)
            LOG("[snap]   %08x from %08x: items %s\n", runs[i].obj, runs[i].caller, runs[i].known ? "known" : "not known");
}

static float fz_rand(void) { fz.rng = fz.rng * 1664525u + 1013904223u; return (float)(fz.rng >> 8) / 16777216.0f; }

static Box grow(Box b, float d) { return (Box){b.x0 - d, b.y0 - d, b.x1 + d, b.y1 + d}; }
static bool inside(Box b, float x, float y) { return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1; }

// Where menu input `in` (called from `caller`) shows its selection now, as the hooks find its items; *how: which way.
static bool shown_selection(uint32_t in, uint32_t caller, Box *out, const char **how) {
    uint32_t keep = input.caller;
    input.caller = caller;  // (place_of / numbered_of / hl_known read it)
    bool found = false;
    const Place *p = place_of(in);
    Layout L;
    uint32_t own;
    Numbered G;
    if (p && p->kind == PL_YESNO && inst_ok(p->cur)) {
        float cx = f32(p->cur + INST_X), cy = f32(p->cur + INST_Y);
        *out = LD8(in) ? (Box){cx + 4, cy - 4, cx + 140, cy + 44} : (Box){cx - 140, cy - 4, cx - 4, cy + 44};
        *how = "yes/no", found = true;
    } else if (p && inst_ok(p->cur)) {
        Box b = inst_box(p->cur);
        if (b.x1 - b.x0 < 8.0f || b.y1 - b.y0 < 8.0f) {
            float x = f32(p->cur + INST_X), y = f32(p->cur + INST_Y);
            b = (Box){x, y, x + 2.0f * fabsf(f32(p->cur + INST_SX)), y + 2.0f * fabsf(f32(p->cur + INST_SY))};
        }
        *out = grow(b, EDGE + 4.0f), *how = "highlight", found = true;
    } else if (layout_of(in, &L) && numbered_of(in, &L, &own, &G)) {
        NumGeo g;
        numbered_geo(own, &G, &L, &g);
        found = owner_instance(own, G.idx[L.at]) != 0;
        if (found) *out = grow(numbered_box(own, &G, L.at, &g), 4.0f), *how = "numbered item";
    } else if (LD32(in) == BOX_VTABLE && hl_known(in)) {
        int s = hl_slot(hl.key);
        float x = f32(hl.inst + INST_X), y = f32(hl.inst + INST_Y), sx = hl_steps[s].sx, sy = hl_steps[s].sy;
        Box b = inst_box(hl.inst);
        if (sx >= 4.0f && sy >= 4.0f) b = (Box){x, y, x + sx, y + sy};
        *out = grow(b, EDGE + 1.0f), *how = "box highlight", found = true;
    } else if (LD32(in) == CUR_VTABLE) {
        Group g;
        CurPos cp = {LD8(in + CUR_COL), LD8(in + CUR_COLS), LD16(in + CUR_ROW)};
        if (group_of(in, cp, &g) && g.sel >= 0) {
            float x = f32(g.inst[g.sel] + INST_X), y = f32(g.inst[g.sel] + INST_Y);
            float hx = g.px ? g.px * 0.5f : 480.0f, hy = g.py ? g.py * 0.5f : 40.0f;
            *out = (Box){x - hx, y - hy, x + hx, y + hy}, *how = "animated item", found = true;
        }
    }
    Free F;
    Walker *w = found ? NULL : walker_of(in, caller, false);
    if (w) {
        Walker *keep_fw = fw;
        fw = w;  // (free_items reads it)
        if (free_items(&F) && F.sel >= 0) *out = grow(F.box[F.sel], 4.0f), *how = "item placed freely", found = true;
        fw = keep_fw;
    }
    input.caller = keep;
    return found;
}

static void fuzz_step(void) {
    if (fz.wait > 0) { fz.wait--; return; }
    if (fz.arrow >= 0) { fuzz_arrow(); return; }
    if (!fz.left && !fz.pending) return;
    static int walking;  // (a menu with its items placed freely still learning its places: up to 60 frames more)
    bool walks = false;
    for (int i = 0; i < 4; i++) walks |= walkers[i].target && g_present_count - walkers[i].used <= FRESH_FRAMES;
    if (fz.pending && walks && walking++ < 60) return;
    walking = 0;
    if (fz.pending) {
        fz.pending = false;
        Box b;
        const char *how = "?";
        if (fz_over.in && shown_selection(fz_over.in, fz_over.caller, &b, &how)) {
            fz.over++;
            fz.region = fz.region.x1 > fz.region.x0 ? (Box){fminf(fz.region.x0, b.x0), fminf(fz.region.y0, b.y0),
                                                           fmaxf(fz.region.x1, b.x1), fmaxf(fz.region.y1, b.y1)} : b;
            if (inside(b, fz.x, fz.y)) fz.ok++;
            else {
                fz.miss++;
                LOG("[fuzz] off: pointer %.0f,%.0f, %08x from %08x shows its %s at %.0f,%.0f-%.0f,%.0f\n", fz.x, fz.y, fz_over.in,
                    fz_over.caller, how, b.x0, b.y0, b.x1, b.y1);
            }
        }
        if (!fz.left) {
            LOG("[fuzz] done: %d points, %d over a menu's items: %d right, %d off; a move selected in %.1f frames on "
                "average, %d at most\n", fz.total, fz.over, fz.ok, fz.miss, fz.lat_n ? (double)fz.lat_sum / fz.lat_n : 0.0,
                fz.lat_max);
            fz.na = 0;  // then the page arrows that show
            for (int i = 0; i < narrows && fz.na < 8; i++)
                fz.abox[fz.na] = arrows[i], fz.adir[fz.na] = arrows_dir[i], fz.aown[fz.na++] = arrows_own[i];
            fz.arrow = 0, fz.astep = fz.aok = fz.aother = fz.anone = 0;
            LOG("[fuzz] %d page arrows show\n", fz.na);
            return;
        }
    }
    fz.left--;
    Box b;
    const char *how;
    if (fz.region.x1 <= fz.region.x0 && g_present_count - input.frame <= FRESH_FRAMES && shown_selection(input.obj, input.caller, &b, &how))
        fz.region = b;  // (at first: where the open menu shows its selection)
    Box r = grow(fz.region, 48.0f);
    bool near = fz.region.x1 > fz.region.x0 && fz_rand() < 0.8f;
    fz.x = near ? r.x0 + fz_rand() * (r.x1 - r.x0) : 8.0f + fz_rand() * ((float)g_ui_size[0] - 16.0f);
    fz.y = near ? r.y0 + fz_rand() * (r.y1 - r.y0) : 8.0f + fz_rand() * ((float)g_ui_size[1] - 16.0f);
    ptr_x = fz.x, ptr_y = fz.y, ptr_inside = 1;
    __atomic_add_fetch(&ptr_seq, 1, __ATOMIC_RELAXED);
    fz_over.in = 0, fz.pending = true, fz.wait = 4;
}

// The patched entry of uGUIResourceOwner's move (0x744b88, r0 the GUI).
void dmouse_gui(Cpu *c) {
    if (!g_config.direct_mouse) return;
    uint32_t own = c->r[0];
    int n = nlive[live_cur];
    // a new frame: the first GUI again (or the list full, or frames gone by since)
    if (n && (own == live[live_cur][0] || n == NLIVE || g_present_count - live_frame > 8)) {
        live_cur ^= 1, nlive[live_cur] = n = 0, live_frame = g_present_count;
        find_arrows();
        fuzz_step();
        frame_moves();
        snap_step();
        if (dump_request) dump_request = 0, gui_dump();
        else if (log_on()) auto_dump();
    }
    if (n < NLIVE) live[live_cur][nlive[live_cur]++] = own;
}

// The patched entry of uQuestCamera's update (0x21fec4, r0 the camera): the mouse's movement since the last update
// turns its current, last and goal angles alike, so the camera follows it at once.
void dmouse_camera(Cpu *c) {
    uint32_t cam = c->r[0];
    if (!g_config.direct_mouse || !sane(cam) || LD32(cam) != QCAM_VTABLE) return;
    cam_ns = plat_now_ns();
    int32_t dx = __atomic_exchange_n(&cam_dx, 0, __ATOMIC_RELAXED), dy = __atomic_exchange_n(&cam_dy, 0, __ATOMIC_RELAXED);
    if (!dx && !dy) return;
    float k = CAM_TURN_PER_COUNT * g_config.mouse_sensitivity / CAM_FIX;
    int yaw = (int)lrintf(-(float)dx * k), pitch = (int)lrintf(-(float)dy * k);  // (right turns right, up looks up)
    for (uint32_t a = cam + QCAM_ANGLES; a < cam + QCAM_ANGLES + 24u; a += 8u) {
        int p = (int16_t)LD32(a) + pitch;
        p = p < QCAM_PITCH_MIN ? QCAM_PITCH_MIN : p > QCAM_PITCH_MAX ? QCAM_PITCH_MAX : p;
        ST32(a, (uint32_t)p & 0xffffu);
        ST32(a + 4u, (LD32(a + 4u) + (uint32_t)yaw) & 0xffffu);
    }
}
