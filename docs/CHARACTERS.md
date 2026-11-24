# Characters: the format, PS3 saves, the library and previews (M12)

The characters players make in 3D Dot Game Heroes' editor, as Cubic.Heroes reads, keeps, shows, exports and
installs them. Code: `tools/edt_format.py`, `tools/ps3_save.py`, `tools/secret_store.py`, `tools/char_preview.py`,
`tools/char_library.py`; tests: `tests/test_characters.py` (made-up data only), `tests/scripts/edit_load_editor.drive`
and `edit_load_select.drive` (the game loading a character, with a local save as their seed).

## The character's data (DATA.EDT)

A character is an edit-data save folder `BLJM60180T<20 hex digits>` (the US release uses the Japanese release's
edit-data prefix) with `PARAM.SFO`, `ICON0.PNG` and `DATA.EDT`. `DATA.EDT` is the character itself. Its layout was
read from players' own files, every byte accounted for (`tools/edt_format.py`):

| Field | |
|---|---|
| `"EDTFILE1.9"`, u16 0 | magic and version |
| u32 length + UTF-8 | the name (the game writes full-width letters) |
| u32 | gender (0 so far) |
| u32 length + UTF-8 | the class (`ＨＥＲＯ`, `ＰＲＩＮＣＥ`, `ＳＣＨＯＬＡＲ`, ...) |
| 14 x f32 | settings (kept as they are) |
| u32 = 6 | poses: Stand, Walk 1, Walk 2, Hurray, Attack 1, Attack 2 |
| per pose: u32 n, n x 20-byte voxels, 6 x f32 | a voxel: u8 flag, u8 x, u8 y, u8 z (0..15; y up; the character faces -z; x to its left), f32 r, g, b (0..1), f32 palette slot |

Big-endian; nothing compressed or encrypted. The importer refuses anything that does not parse exactly: a voxel
outside the 16 x 16 x 16 grid, two voxels in one place, a wrong pose count, bytes left over.

`PARAM.SFO` is not part of the character: a PS3's copy carries the creator's **account ID** and console
parameters. `ICON0.PNG` is mostly the game's own artwork (the editor's EDIT card and the logo). Neither is ever
exported or published; installing a character writes fresh ones (no account or console identifiers; the icon is the
character's own preview).

## Characters saved on a PS3: what is encrypted, and the key

A character copied off a PS3 (to a USB stick, onto a website) keeps `DATA.EDT` encrypted by the console's save
system: it is a "protected file" listed in the save's `PARAM.PFD` (version 3). Read as it is, the game finds no
name, no class and no voxels: an invisible character (issue #8; the same report as RPCS3's issue #11336).
Decrypted, it is byte for byte the file the game, an emulator or Cubic.Heroes writes. There is no other encoding.

**Recognising** (never by `PARAM.PFD` alone: a decrypted `DATA.EDT` may still sit beside its original `PARAM.PFD`):

- *plain*: `DATA.EDT` parses as a character. Imported as it is, with no key.
- *encrypted*: `DATA.EDT` does not parse, and `PARAM.PFD` is a PS3 PFD (`PFDB`, version 3 or 4) listing `DATA.EDT`
  with a recorded size that rounds up to the file's length, and the data looks random (above 7.5 bits a byte).
- *damaged*: anything else, with the reason.

**Decrypting needs two things**:

1. **The game's secure file ID** (16 bytes): what the game gives the save system for the file. It is in the
   player's own decrypted program (every installation keeps `data/disc/PS3_GAME/USRDIR/EBOOT.elf`). The importer
   finds it there and proves it with the HMAC-SHA1 that `PARAM.PFD` keeps for the file (keyed with the ID). That is
   an exact match, never a guess, and it needs no console key. The result is remembered in the library's own state.
2. **The PS3's syscon manager key** (16 bytes, AES-128): a fixed key of the console's system software.
   `PARAM.PFD` keeps each protected file's own AES key encrypted with it (AES-128-CBC; the IV is built from the
   secure file ID). It is **not** in the game, the disc, the disc key or the keys file for the game's program.
   Cubic.Heroes never includes it. The player supplies it: 32 hexadecimal digits.

The file is then decrypted 16 bytes at a time: AES-decrypt the block with the file's key, XOR with the block's index
(a 128-bit big-endian counter) AES-encrypted with the same key.

**Validating a supplied key** happens before anything is written:

1. The file key it yields must decrypt the first block of `DATA.EDT` to the header `EDTFILE1.9`. That is 80 known
   bits, so a wrong key passes with probability 2^-80. Only that one block is decrypted for this test.
2. The whole decrypted file must then parse as a character, at the length `PARAM.PFD` records, and read back
   exactly.

Only then is the character added to the library, as **plain** data. From then on it works without the key, and
its export and any download made from it need no key either. A missing or wrong key gives a message ("saved on a PS3
and still encrypted: importing it needs your PS3 save key"; "the PS3 save key does not decrypt this character")
and writes nothing. The original files are only ever read.

**Keeping the key** (`tools/secret_store.py`), only when the player asks and only after it has been validated:

- Windows: Credential Manager, a generic credential `Cubic.Heroes/ps3-save-key` for the current user, protected by
  DPAPI.
- Linux: the Secret Service (GNOME Keyring, KWallet...) through libsecret's `secret-tool`.
- Otherwise: for the current session only. It is never written in a plain file.

The key never appears in a log, a message, the diagnostics report, a package or either repository.

## The library and the portable file

`<install>/data/characters`, kept with the installation across updates:

- `entries/<id>/`: `character.edt` (plain), `character.json`, `preview/`.
- `originals/<id>/`: the imported files exactly as they were. These stay local and are never exported.
- `index.json`: every entry's summary, so the page lists 1,000 characters without reading 1,000 files; it is made
  again from the entries whenever it does not match them.
- `cache/<sha16>/`: previews of the characters made in the game that are not in the library (the Installed tab).

The folders are made by the first import: showing the Characters page with nothing imported writes nothing. An
import never stops at one bad file: each unreadable archive or file is reported by name and the others are
imported; each character is added whole or not at all, and a cancelled import keeps the characters already added.

The `<id>` is the first 16 hex digits of the character data's SHA-256. Importing the same data again is a
*duplicate* and is never imported twice. The same model under another name or with other settings is imported and
flagged *similar*. Installing writes a new edit-data folder into the game's saves and never replaces one; the
game's saves (`BLUS30490*`) are never touched. Uninstalling removes only the folder the library itself installed,
and only while it still holds exactly what was installed (`DATA.EDT`, `PARAM.SFO`, `ICON0.PNG`, `PARAM.PFD`); a
character made in the game, or one the player changed since, is never removed. The library entry stays.

**The launcher's Characters page** has two tabs, each with its own search, filters, order, scroll position and
selected card: *My Library* (the entries; a card shows IN GAME when the character is in the game's saves) and
*Installed* (every character the game can use now: the library's, and those made in the game, which can be added
to the library). LT/RT (or `[` `]`) switch the tabs; LB/RB stay the launcher's pages. A third tab, *Character
Store*, is prepared but hidden: it shows nothing and touches no network until the store exists and its security
requirements are met.

**`.chchar`** is a ZIP of `character.json`, `character.edt`, `preview/thumb.png` and `preview/views.png`. It holds the
character and what describes it: name, class, label, creator, description, tags, source and provenance, hashes,
compatibility. Reading one checks the data's SHA-256 and makes the previews again. Archives are read in memory
within limits: no absolute or `..` paths, a cap on members, sizes and expansion ratio (archive bombs).

## Previews (`tools/char_preview.py`)

The same renderer serves the library, imports, exports, the Hall of Heroes recovery and the store's workflow. It
draws from the character's own voxels and colours, with no game art and no retouching:

- `thumb.png` (288 x 288): the Stand pose, slightly from the front-left and above (16.3 degrees), framed tightly.
- `views.png` (front | side | back, 3 x 320 x 400): one fixed zoom for every character (the editor's 16-voxel
  grid always fits), so characters compare at their real sizes.
- `thumb.rle`, `views.rle`: the same pictures run-length coded for the launcher ("RLE1", width and height, then runs
  of a count and a colour), which draws them without an image library.

The views are orthographic, with fixed lighting and the launcher's blue background; each voxel is drawn as a cube
with a darker edge, at twice the size and then reduced (edges smoothed, voxels still crisp). The renderer's version
is recorded with each preview: previews made by an older version, or of other data, are made again. They are deterministic: the camera angles have exact rational sines and cosines, the drawing order is
total and the code is pure Python, so the same character always gives the same pixels. A character that cannot be
drawn gets a placeholder, and its entry says so.
