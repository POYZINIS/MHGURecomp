# Keyboard and mouse: direct pointer in menus, direct camera in quests (research notes)

Goal (2026-10-29): mouse support the way a PC player expects it, behind an ini setting ([Mouse] MenuMouse):
- menus: the pointer's XY picks the item under it (the game's own selection follows, no simulated d-pad), left
  click confirms, right click is Back (B);
- quests: mouse movement turns the game's camera directly (its yaw / pitch), not through a simulated right stick;
- towns (Hub, villages, Pub): the camera stays fixed as in the original; left click A, right click B.
Simple button presses mapped 1:1 to controller buttons are fine.

## Engine facts found (main v1.4.0, runtime addresses: main at 0x200000)

Type info: MT Framework DTI, `f_9aedcc(dti, name, parent, size)` registers each class; an object's vtable has the
class's getDTI at +0x14 (vtable = slot - 0x14). 3123 classes recovered by scanning the DTI constructor calls.

### Menus
- cCursor (vtable 0x196cac8, 0x24 bytes): +0x0d column (u8), +0x0e columns (u8), +0x10 row (u16), +0x12 rows (u16),
  +0x14 items (u16), +0x18 / +0x1c the column / row axis's button mode (index into the table at 0x182d0c0: 16 bytes
  each, "previous" and "next" pad bits: 1 up/down 0x10/0x40, 2-3 left/right 0x80/0x20, 4-5 L/R 0x100/0x200).
  update(cursor, pad trigger bits) at 0x74ee98 moves one step, sets +0x08 (1 left, 2 right, 4 up, 8 down) and
  returns the button that moved it (menus refresh on that). getIndex = slot 14 (row x columns + column), setIndex =
  slot 15. 416 call sites of update. Writing the column directly moves the highlight (tested on the character
  select).
- Yes/No choice of the confirmation boxes: 0x74fbb0(choice byte *, pad trigger, button to 0, button to 1);
  cUIOWindowDialog (vtable 0x1978e54) has the byte at +0x6c, its Yes / No touch panels at +0x60 / +0x64.
- cUIObject (+0x04 rGUI, +0x24 uGUIResourceOwner). Highlight placement helpers: 0x77033c(this, cursor id, next id,
  row) (y = row x next.y), 0x770378 (x), 0x7703b4(this, cursor, next x, next y; col, row on the stack); 87 call
  sites. 24 menus inline it instead: setY(owner, id, column x step) (sites in runtime/hle/menumouse.c).
- uGUIResourceOwner: instances +0xf8 (count +0x298), then +0x294. Getter 0x745a04 (&inst->PosX), setX 0x745a40,
  setY 0x745ac0, setXY 0x745c20.
- GUI instances: +0x10 world matrix (row-major; +0x10 sx, +0x24 sy, +0x40/+0x44 canvas position, y down),
  +0x50 id, +0x54 flags (bit 0 visible), +0x5c property table, +0x64 next sibling? / +0x68 parent (instances) -
  pick() walks +0x68 first child / +0x6c next sibling, +0x6c owner, +0x70 rGUI, +0x90/+0x94 PosX/PosY (local).
  GUI objects: +0x74 definition (name at +0x10, DTI at +0x14). Instance order = the order of instance names in the
  .gui file (window_dialog: 1 Null_cursor, 2 Anm_answer, 7 Anm_text ...).
- The engine's pointer picking: cGUIInstance vtable +0x48 = hit test (the animation instances' 0xc39cd8 recurses
  into their child GUI, 3D ray for billboards), pick(instance, a, b, s0) at 0xc47ab0 returns the id of the
  instance hit or -1. Nothing in the menus calls it on the Switch (the 3DS touch path is gone: no touch imports,
  sMouse at 0x23267b0 constructed but never read, the "Mouse Reaction" GUI flag never read).
- 3DS touch heritage still in the code: cUIOPanelBase (vtable 0x19740f0; +0x54 state, 1 = touched (slot +0x90),
  +0x58 decided (slot +0x8c), +0x28 its instance id), dozens of cUIOCPanel* (the old lower-screen panels),
  cUIOPageCountWithTouchInput. Panel rows: the character select's slots, the item pouch rows, dialog buttons.

### Camera
- uQuestCamera (vtable 0x1930d0c, 0xb00 bytes) in quests, uVillageCamera (0x1930e10) in towns, sCamera
  (0x1995058). sGameControl * at 0x1a8ca48: +0x4c area type (0 Hub, 1-6 villages, 7 Pub, quests above).

- uQuestCamera angles (found by diffing the object while turning the stick; integer 16-bit turns, 0x10000 = 360):
  pitch +0x9d8 current / +0x9e0 last frame / +0x9e8 goal, yaw 4 past each. The stick sets the goal and the update
  eases the current toward it; the eye position (+0x40, copies at +0x70, +0x560...) is rebuilt from them every frame
  (a poke of the eye is overwritten at once). Stick left = yaw up, stick up = pitch up (looks up). Pitch limits at
  the stick's ends: 0xd100 .. 0x2800. Only the camera modes' code (0x21c000-0x22a6xx) writes them.
- Sound of a menu move: 0x6eeb78(*0x1a96f5c, id); the cursor mode table's third word 0 = this kind, fourth the id.
- Menus that show the selection as an animation of the item (title menu, character select): their items are
  visible sibling cGUIInstAnimation instances (title: 5 under Null_StartMenu at 640, 185 + 84 k; character select:
  3 at 104, 160 + 116 k). An animation instance's current sequence: *(*(inst + 0xdc) + 0x94). The selection shows
  as a sequence of its own (title) or as a highlight instance standing on the item (character select: the slot
  with save data is the one with a sequence of its own, not the selection). setSequence(owner, id, seq) at
  0x746c74 tells which GUIs a menu animates (it sets its items' sequences as it opens and as the selection moves).

## Design (2026-10-29, runtime/hle/directmouse.c, [Mouse] DirectMouse)
- Quests: mouse movement goes straight into uQuestCamera's angles at the start of its update (0x21fec4): yaw and
  pitch of all three pairs move together (no easing, no stick), 16 turn units per count x MouseSensitivity, pitch
  clamped to the stick's range. The mouse is captured when the camera starts running with the window focused.
- Menus: the pointer selects the item under it by setting the menu's own selection (cCursor's column / row at the
  end of its update, 0x74f228, with the move flag, the returned button the menus refresh on and the move sound; the
  Yes/No byte with its sound). Item geometry: the highlight placement helpers, the 24 inline setY rows, the dialog's
  answer instance, else the animated item group. A click confirms once its item is the selection (A for 4 frames);
  a menu whose items aren't known gets A on its current selection. Right click = B.
- Towns: the camera is fixed (as the game has it); the pointer stays free, left click = A (talks to / uses what is
  in range: the game names it top left, "(A) Village Moofah"), right click = B.
Verified in the lab (scripted mouse events, screenshots): title menu, character select, the load Yes/No, the start
menu in town and in a quest, the item pouch, petting a Moofah by click, the quest camera (35 degree right turn and
look up from 400 / 240 counts). Not yet looked at: cBoxCursor grids (item box, equipment), shops, smithy, quest
board and NPC menus.

## Every menu (later on 2026-10-29): GUI structure, numbered items, boxes, arrows, fuzzer
- Every GUI that runs: uGUIResourceOwner's move (vtable slot 9, 0x744b88) once a frame each (hooked: the live list).
  Owner +0xf0 rGUI (+0x08 path, +0x64 the .gui file: +0xbc instance table, 0x24 bytes an entry: +0 id, +0x14 name),
  +0xf4 root instance. Instances: +0x04 file id, +0x60 first child, +0x64 next sibling, +0x68 parent, +0x6c owner.
  The game's code names instances by array index (owner +0xf8[i]), not by file id. Copies made at run time (page
  counters' arrows) have ids past the file's table: no name.
- An animation instance draws GUI objects from its root (+0xdc): +0x68 first child, +0x6c next sibling, +0x54 bit 0
  visible, +0x40/+0x44 canvas position; polygons / textures / texture sets: +0xb4/+0xb8 size (x the object's scale),
  +0xa0 low nibble the position's place (0 top left, 5 middle: numpad). Their union is what an item shows (its box).
- Items found by name: numbered siblings (Anm_list_S00..07, Anm_itemSet01..08, Anm_panel01..03), matched to the
  cursor's page (count, visible count = items on the page, the selected one standing out, drawing vs anchors),
  re-chosen every frame (menus open in steps). Menus key by cursor + caller (the shop shares one cursor).
- cBoxCursor (vtable 0x196cb9c, update 0x7530b8, hooked at entry with early return): +0x14/+0x15 column/row, +0x16
  /+0x17 columns/rows, +0x18/+0x1a page/pages, +0x1c items; returns the move's pad bit. The item box moves Anm_cursor
  with setX/setY = column/row x 36 every frame: steps learned from those (the highlight pulses: use its place).
- Page arrows: names ..arrow.._L/_R, or unnamed copies under a node (leftmost back, rightmost on, arrow-sized);
  a click flips the page of the menu whose items are in the arrow's GUI (cCursor: its page axis, modes 3-10;
  box: its page).
- Fuzzer (F6 / control "fuzz n", MHGU_DIRECTMOUSE_LOG): random pointer places, each checked 4 frames later against
  where the game then shows the selection. Start menu 59/59, item pouch 62/62, combine 79/79 after the fixes.

## Town menus fuzzed (2026-10-29, late)
User's run in Bherna: shop top 35/40, quest counter 82/87 and 60/61, kitchen 18/70, smithy list 24/42 and 15/35,
smithy equipment box 0 hovered, smithy upgrade list 87/87, item sets / item box 75/75. Causes:
- The placement helpers put the highlight at column / row x the next marker's own position (+0x90 / +0x94, read
  through the getter 0x745a04), in the highlight's parent's space. The kitchen's (shop\food) Null_cursor_next hangs
  under the highlight Anm_cursor, which is scaled 169 x 17: on the canvas it stands 612 px below, not 36. Steps are
  taken from the marker's own position x the parent's scale now.
- The selected item of a list slides aside (quest list Anm_list_S: 32 px left; shop and smithy Anm_panel: 32 px left).
  Its own box left a pointer near the edge on the neighbour, which then slid back under it: the two flipped every
  frame. A list's rows now span the whole list's width, and a pointer selection holds until the pointer moves again.
- One cCursor serves several screens of a shop (callers differ): an animated group found for one caller was taken
  for the next screen's list. Numbered groups go first now, the animated group after, both kept per caller.
- The smithy's equipment box (i_box) runs with a second input after it every frame (the equipment detail's page
  cursor, modes 7 0): the highlight learner listened to the last input only. It follows the last box cursor now.
- Still open: the equipment detail's arrows (menu_equipDetail Null_arrow) have no menu with items in their GUI: a click
  goes to whichever paging input runs first after 3 frames, which may be the box behind it.
After the fixes (new build, by script): title menu 34/34, character select 32/32 (now its numbered save.Anm_button_L),
start menu 11/11, item pouch 39/39.

## Snappy from the first frame (2026-10-29, night)
User report: some menus can't be hovered when they open, then respond after some fidgeting. Causes found with the
snap test (below):
- The arming rules threw moves away: a menu that opened forgot every earlier move, and a pointer over no item for one
  frame stopped selecting. A hand that moved while a menu slid in and stopped on an item did nothing until it moved
  again. Now a move stays pending until it selects (or the pointer stands on the selection), for 60 frames, unless the
  pointer stands over none of the items of a menu that knows them for 12 frames; in a menu's first 10 frames a
  selection doesn't use it up (pieces still moving in). After a selection the pointer must move again.
- A closing menu may hide only a place high above its items: the title's menu_down.Anm_button_S stay flagged visible
  under the character select. Checking the item's parent only, the mod took them for the character select's 3 items in
  its first frame and kept them (the right group scores no higher once the selection leaves the slot with data).
  Visibility is now checked up the whole parent chain.
- Groups of Nulls that hold nothing that shows (menu_bar.Null_01/02) are no items.
- Rows of text anchors (combo list, kitchen: Anm_mesM00..07, a point each, at the row's top left) answered only
  +-60 px around the anchor and half a row too high: they now take the menu's highlight (an animation named ..cursor..
  in the GUI): its width, and its offset from the selected row's anchor.
- The combo list places two highlights every frame (the 190 x 17 bar and an unscaled mark at the same place): the
  record flipped between them and with the mark the row step came out 2 px (no hover at all). The one that draws more
  is kept.
- A click in a menu's first 15 frames no longer goes blind (A on the menu's own selection) before its items are found.
- Not handled: the world map (village icons placed freely, a left / right cursor of 4; no numbered items): no hover.
Tools: the control file's "snap x y [click]" queues a pointer move (and click) for the first frame the next menu
input runs and logs "[snap]" (frames until its items are known, until the item there is the selection, until the click
confirms); every menu run logs "[snap] ... items known N frames after it opened". The fuzz (F6) ends with each page
arrow that shows clicked, checked against which menu input's page flipped, and reports the frames from a move to its
selection. scratchpad menutest.sh drives title -> character select -> Yes/No -> start menu -> pouch -> combine -> combo
list -> world map with a snap into each and a fuzz.
Results (macro): title, character select (click confirmed 1 frame after opening), Yes/No, start menu, pouch, combine,
combo list: the pointed item selected 0-1 frames after the menu opened; fuzz 22/22, 18/18, 22/22, 19/19, a move
selected in 1 frame; 8 of 8 arrows flipped their own menu's page.

## Test setup
Lab: a copy of the save in the scratchpad (MHGU_SAVE_DIR), MHGU_HOME with its own ini (MenuMouse = On, windowed
1280x720), MHGU_CONTROL for `mouse x y [click|rclick]`, buttons, `shot`, `peek`, `fntrace`. The window starts
minimised when launched from the agent: restore it (ShowWindow 4) or nothing is drawn. memsave of a range with
uncommitted pages writes nothing: dump small ranges.

## First attempt (wrong design, kept for the record)
Moved the highlight by adding d-pad presses to cCursor's update toward the item under the pointer (geometry from
the placement helpers); the camera stayed the port's mouse-as-right-stick. Worked on the start menu, the item pouch
and the Yes/No box in scripted tests, but: it simulates the controller; menus without placement geometry got no
hover at all (what the user hit first); two HUD cursors update every frame in town, so "a cursor ran" is no test
for "a menu is open"; and a right-drag camera in town goes against the original design.
