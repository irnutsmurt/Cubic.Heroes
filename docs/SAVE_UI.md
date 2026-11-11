# The save-data screen

The PS3's system dialogs for save data (the list a title shows with cellSaveDataListLoad2 and
cellSaveDataListSave2, the "New Save Data" row, the overwrite question) are the host's in this port. Since
2026-11-09 they were a working placeholder: a text panel of upper-case rows over the game's picture
(ps3recomp libs/system/cellSaveData.c, drawn by libs/video/rsx_vulkan_backend.c). This is its finished
form, as the owner asked on 2026-11-10, after a reference picture of the PS3's own screen.

## What it looks like

- The whole window, a dark background (a vertical gradient), a heading at the top left ("Load Save",
  "Save Game"; for the character editor's data "Load Character", "Save Character") and the date and
  time at the top right, a thin line under them.
- The saves as large rows, one above the other: the save's own **ICON0.PNG** on the left (from the
  player's save data, its aspect kept, a neutral tile when a save has none), then its title (the
  save's SUB_TITLE: "SAVE DATA", "EDIT DATA"), the date it was written and its size ("281 KB": every
  file, in kilobytes of 1000 bytes, rounded up, as the PS3 shows it), and its DETAIL lines ("NAME :
  ...", "TIME : ...", "LOCATION : ..."). Proportional text in mixed case: the full-width forms the game
  writes are read as their ASCII, a label's colon is spaced, nothing is upper-cased. Text too long for
  the row ends in an ellipsis.
- The selected row: a lighter panel, a light edge and a soft glow. Thin separators between rows.
- More saves than fit: the list scrolls with the selection, and a thin scroll bar shows where it is.
- The title's new-save row: "New Save Data", with the title's own new-save icon when it gives one,
  else a tile with a plus.
- The overwrite question: the list dimmed, a box with the question and the save it would replace.
- Along the bottom, right-aligned: the confirm and back prompts ("Enter", "Back"; "Yes", "No" for the
  question) in the player's prompt style.
- No list (nothing to load): "There is no saved data." (today the dialog is not opened then: the
  title's own flow handles an empty list as before).

Sizes are set for 1280x720 and scaled with the window's height (a narrow window by its width); the
picture is composed at the window's own size, so text and icons stay sharp in a small window and in
fullscreen.

## Prompts

`PS3_UI_PROMPTS` = keyboard, playstation, xbox or switch fixes the style; unset, it follows the
device the player last used (the keyboard; an SDL pad by its type: PlayStation, Switch, otherwise
Xbox's). It is this screen's part of docs/PC_EXPERIENCE.md section 6 (the PROMPTS setting).
- Keyboard: Enter (also Z) confirms; Esc (also Backspace, and X) goes back. Esc no longer quits the
  game while the screen is open.
- PlayStation: Cross and Circle, drawn from shapes (no PlayStation images are shipped, by decision).
- Xbox: A and B. Switch: the pad's own labels for the same buttons (confirm B, back A).

## How it is built

- **The dialog** (cellSaveData.c) is unchanged in what it does: the entries funcList kept, the
  title's sort and focus, the new-save row where the title puts it, Cross/Start to choose, Circle to
  cancel (CELL_CANCEL), the overwrite question before a save replaces another. It now also posts the
  screen's model (ps3emu/osd.h `ps3_osd_saves_state`): per row the title, date, size, DETAIL lines and
  the ICON0.PNG path; the heading, the labels, the prompt style, the selection and the question. The
  text panel is still posted beside it, and drawn when the screen cannot be (no font registered).
- **The picture** (ps3recomp libs/video/osd_saves.c) is composed on the CPU when the model, the window's
  size or the clock's minute changes, and copied over the game's picture each frame while the screen
  is open (rsx_vulkan_backend.c vk_osd_saves_draw). Text with stb_truetype, PNGs with stb_image (both
  already in ps3recomp, compiled here as private copies).
- **The font and prompt images** are the port's own, redistributable, in port/assets/ui, embedded in
  the executable at build time (port/cmake/embed_files.cmake, port/ui_assets.cpp, in the
  dotheroes_native library so the player's install links them too) and registered at start:
  - DejaVu Sans 2.37 (LICENSE-DejaVuSans.txt: Bitstream Vera's licence, DejaVu's changes public
    domain);
  - four of Kenney's input prompts, unmodified (LICENSE-Kenney.txt: CC0): Xbox's and the Switch's
    confirm and back buttons. The keyboard's prompts are key caps drawn with the key's name (Kenney's
    keyboard images are unreadable at a small window's size); PlayStation's are drawn too.
  Nothing comes from PS3 firmware or the game's files; the icons are the player's own save data.

## The dialog presents itself

The overlays are drawn in the window's presents, and the window presents a frame when the game flips one.
While one of these dialogs is open the game flips nothing -- every dialog of the save_picker drive in a
window (CONTINUE's load twice, a cancel, the system menu's SAVE) logged no present in its time [M]; that
the game waits for the dialog on the thread that renders is the likely reason, not traced. So nothing was
drawn: the placeholder panel neither, since the native release (R7) has no FIFO walker presenting on its
own. After 4 s
the dialog gave up ("not shown (nothing draws it)") and took the title's first entry -- a load of that
save, or a SAVE over it whichever save had been loaded. A player who chose within 4 s chose blind.

Now the dialog watches the window's presents (ps3emu/osd.h `g_ps3_osd_presents`) and, when none comes
for 45 ms, presents the last frame again with the overlays itself (`g_ps3_osd_present_again`, the
backend's `vk_present_again`: under the present mutex, after any present in flight completes). The log
says so once per dialog ("the title presents nothing meanwhile; the dialog presents itself"). Without a
window (a headless run) there is nothing to present and the 4 s fallback stays.

The harness's picker drives ran headless (no `WINDOW=1`), so they never saw a list drawn; they passed
because their presses came within the 4 s, and the SAVE in save_picker went to the first entry. They run in
a window now (build/r8/ui20_run.sh).

## Results (rel20, 2026-11-10)

In the game, in a window, on the drives' copies of the saves (build/r8/ui20_run.sh; the pictures in
build/r8/ui20*/screens, kept locally: they show the saves' names):
- **save_picker, PlayStation prompts**: CONTINUE's list (the two saves), the second chosen and loaded; the
  system menu's SAVE: the list with "New Save Data" (the game's own new-save icon) and the two saves, the
  second chosen, the overwrite question, YES -- **the SAVE wrote the loaded save** (25F6), not the first
  entry; back to the title, CONTINUE, a cancel; CONTINUE again, the first row (now the save just written:
  the list is newest first, as the title asks) loaded. Every dialog presented itself. The drive passed.
- the same **fullscreen** (1920x1080, the desktop's size) with the Switch's prompts, and with the
  automatic style (the drive's pad file is no device: the keyboard's key caps): the same choices and
  writes, passed.
- **save_picker_edit, Xbox prompts**: the first save loaded; "Change characters?" YES; the character
  editor's list ("Load Character", three edits), the third chosen and loaded; passed.
- The pictures against the reference: the heading, the date and time, the rows with their icons, title,
  date and size (281 KB as the reference shows), NAME / TIME / LOCATION in mixed case, the selected row,
  the separators, the prompts at the bottom: the reference's layout. What differs is ours by choice: the
  font (DejaVu Sans, not the PS3's), the colours of the panels and the highlight, and the scroll bar and
  the overwrite box, which the reference does not show.
- Off-line (tools/save_ui_preview.c): 800x450, 1280x720, 1920x1080 and 3840x2160; a long list scrolled
  (12 rows, the sixth selected); long names; the empty message.

## Tests

- Screenshots of each state against the reference picture: the load list, the save list with the
  new-save row, the overwrite question, the character editor's list, a long list scrolled, a long
  name, the empty message; at 1280x720, a small window and fullscreen; each prompt style. In the game,
  `PS3_SAVES_SCREEN_DUMP=<dir>` writes each composed picture (a drive's `shot` reads the game's own
  frame, not the window); off-line, tools/save_ui_preview.c composes any state at any size from save
  directories.
- The whole flow on copies of the owner's saves (build/checkpoints/picker-two and the play saves'
  copies, never the originals): load the second save, play, save over it (the question, Yes), save
  as new, cancel at each step; the character editor's load and save; then the files compared (only
  the chosen save written).
- The validation scenes with the screen (save_picker, save_picker_edit) and without it (the harness's
  default, PS3_SAVEDATA_PICKER=0), unchanged.
