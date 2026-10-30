// Direct mouse ([Mouse] DirectMouse, runtime/hle/directmouse.c): the window loop (runtime/host_input.c) gives the
// pointer, the clicks and the mouse's movement; the game's menus and quest camera follow them directly.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The pointer in the game's 2D canvas (1280x720 at 16:9: g_ui_size); inside false: off the window. UI thread.
void dmouse_pointer(float x, float y, bool inside);
// A left click in a menu: confirms the item under the pointer once it is the selection. UI thread.
void dmouse_click(void);
// true once per click that reached its item: the window loop presses A. UI thread.
bool dmouse_take_confirm(void);
// true while a menu runs (the game updated a menu cursor in the last few frames): the pointer is free.
bool dmouse_menu_open(void);
// true in a town (the Hub, a village, the Hunters' Pub; not a quest): the pointer is free there, the camera fixed.
bool dmouse_in_town(void);
// true while the quest camera runs: mouse movement turns it (dmouse_camera_move) instead of mouse look.
bool dmouse_camera_active(void);
// Mouse movement (counts, y down) for the quest camera. UI thread.
void dmouse_camera_move(float dx, float dy);
// The control file's "gui": log every GUI that shows (instances: id, name, kind, position) and the menu inputs that
// ran lately, on the game's next frame.
void dmouse_dump(void);
// The fuzzer (MHGU_DIRECTMOUSE_LOG; the control file's "fuzz [n]", F6): n random pointer places over the open menu,
// each checked against the selection the game then shows, then each page arrow that shows clicked; "[fuzz]" lines.
void dmouse_fuzz(int n);
// The snap test (the control file's "snap x y [click]"): the pointer to (x, y) in the first frame the next menu runs,
// clicked there; "[snap]" lines: the frames until its items are known, the item there is the selection, the click
// confirms.
void dmouse_snap(float x, float y, bool click);
