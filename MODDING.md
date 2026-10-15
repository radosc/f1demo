# Modding Fallout Demo HD

Everything the game uses lives in `data\` as ordinary files: PNG images, JSON, WAV, text message
files and compiled scripts. You can edit `data\` directly, but the better way is a **mod**: a
folder in `mods\` with the same layout as `data\`. A file in a mod replaces the file with the same
path in `data\` (or in an earlier mod). Message files (`.msg`) are the exception: their lines are
merged, so a mod's `.msg` only needs the messages it changes. Nothing is packed and nothing needs
rebuilding, so restart the game to see a change.

## Mods

```
fo1hd\
  data\                      base game data (converted from FALLDEMO.DAT)
  mods\
    mods.txt                 optional load order
    my-mod\
      mod.json               optional: { "name": "My Mod", "description": "..." }
      text\english\dialog\rock.msg
      art\critters\hd\hmjmpsaa.png
```

* Without `mods.txt`, every folder in `mods\` is loaded in alphabetical order.
* With `mods.txt`, only the listed folders are loaded, one per line, from first to last. A later mod
  wins over an earlier one. Lines starting with `-` disable a mod, and `#` starts a comment.
* `bin\fo1hd.log` lists the enabled mods at start-up (`Mod enabled: ...`).

`mods\example-mod` shows a small working mod. It is disabled in `mods\mods.txt`; remove the `-`
there to try it.

## Data layout

| Path | Contents |
|---|---|
| `manifest.json` | data version; optional `"startMap"` (map started by NEW GAME, default `junkdemo`) |
| `color.pal` | the 256-colour palette (palette-indexed art uses it; index 0 is transparent) |
| `art\<category>\<name>.png + .json` | sprites (categories: critters, items, inven, scenery, walls, tiles, misc, intrface, skilldex, backgrnd, heads) |
| `art\<category>\<category>.lst` | list mapping art numbers (FIDs) to names, one per line |
| `art\<category>\hd\<name>.png` | high-resolution version of a sprite sheet |
| `proto\<type>\NNNNN.json` | object prototypes (items, critters, scenery, walls, tiles, misc) |
| `maps\<name>.json` | maps: tiles, objects, spatial triggers, entrance, map script |
| `scripts\*.int`, `scripts\scripts.lst` | compiled scripts and the script number list |
| `text\english\game\*.msg` | engine texts (proto names, skills, combat, interface...) |
| `text\english\dialog\<script>.msg` | dialogue and floating text of each script |
| `sound\sfx\*.wav`, `sound\music\*.wav` | sound effects and music (any PCM WAV) |
| `premade\<id>.json + .bio` | characters offered in the character selection |
| `data\vault13.gam` | global variables and their start values |
| `data\ai.txt` | combat AI packets (aggression, run-away limits, taunt ranges) |
| `rules\critical.json` | critical hit tables (damage multipliers, effects, messages) |
| `rules\perks.json` | perks: ranks, requirements, bonuses, names and descriptions |
| `fonts\*` | bitmap fonts (PNG atlas + JSON metrics) |
| `movies\*.mve` | movies: `iplogo` and `intro` play at start-up, `intro` and `credits` from the main menu (Interplay MVE, 8-bit video, optional PCM or DPCM sound) |

## Sprites

A sprite is a sheet image plus metadata, for example `art\scenery\barrel.png` and `barrel.json`:

```json
{ "fps": 10, "actionFrame": 0, "framesPerDirection": 1, "directions": 1,
  "shift": [[0,0],[0,0],[0,0],[0,0],[0,0],[0,0]],
  "sheetSize": [44, 35],
  "frames": [[[4, 4, 36, 27, 2, 0]]] }
```

* `frames[direction][frame]` = `[x, y, width, height, ox, oy]`: the frame's rectangle in the sheet, and
  the offset a moving animation adds per frame (walking critters advance by these).
* `shift[direction]` = where the image sits relative to the object's hex.
* `directions` is 1 for things that look the same from every side, or 6 for critters
  (NE, E, SE, SW, W, NW).
* `actionFrame` is the frame where an attack hits or a hand reaches an object.

The sheet PNG can use the Fallout palette (8-bit, palette from `color.pal`; such art keeps the
original's colour cycling for indices 229-254) or plain RGBA. To add new art, add the files and a line
to the category's `.lst`. A mod replaces the whole list, so copy it and append the new line at the end.
The line number (counting from 0) is the art number that prototypes and maps use in their `fid` values.

### HD sprites

`art\<category>\hd\<name>.png` is used instead of the base sheet whenever the screen is drawn at 2x or
more. It must be the same sheet scaled by a whole number (2x, 3x, 4x, 8x...), with the same layout as
the base sheet. The scale is detected from the size. RGBA and palette images both work.

To start a hand-made HD sprite from the converter's output, run
`FalloutDemoHD.exe --hd-preview data\art\critters\hmjmpsaa.png out\hmjmpsaa`, which writes a smooth and
a pixel-style 4x version. The converter's style for the whole game can be chosen with
`--convert ... --hd-style smooth|pixel --scale 2|4`.

## Prototypes

`proto\items\00040.json` (the Stimpak):

```json
{
  "pid": "0x00000028",
  "name": "Stimpak",
  "messageId": 4000,
  "fid": "0x0000000F",
  "flags": ["flat"],
  "extFlags": ["canUseOn", "canLook", "canPickUp"],
  "itemType": "drug", "material": "metal", "size": 0, "weight": 1, "cost": 100,
  "inventoryFid": "0x0700001D",
  "drug": { "stats": [35, -1, -1], "amount": [12, 0, 0],
            "duration1": 0, "amount1": [0, 0, 0], "duration2": 0, "amount2": [0, 0, 0],
            "addictionChance": 0, "withdrawalEffect": 0, "withdrawalOnset": 0 }
}
```

* `pid` = type in the top byte (0 item, 1 critter, 2 scenery, 3 wall, 4 tile, 5 misc), number in the
  low bytes. The file name is the number.
* `fid` = art: `0xRTAAWNNN`, where `R` is a rotation (0 in prototypes), `T` the art type, `AA` the
  animation, `W` the weapon code and `NNN` the line in the category's `.lst`.
* `name` is only a comment. The displayed name and description are messages `messageId` and
  `messageId + 1` in `text\english\game\pro_item.msg` (`pro_crit.msg`, `pro_scen.msg`... for other types).
* Drugs: `stats` are stat numbers (35 = current HP, 36 = poison, 37 = radiation, 0-6 = SPECIAL...).
  `amount` applies at once, `amount1` after `duration1` minutes and `amount2` after `duration2` minutes.
* Weapons have `weapon { animCode, minDamage, maxDamage, damageType, range1, range2, minStrength,
  ap1, ap2, criticalFail, burstRounds, caliber, ammoPid, maxAmmo, soundCode }`. Critters have base
  stats, skills, `aiPacket`, `team`, `experience` (points for killing one; the demo's critters give
  none) and `killType`, which picks the critical hit table and the kill counter line: 0 man, 1 woman,
  2 child, 3 super mutant, 4 ghoul, 5 brahmin, 6 radscorpion, 7 rat, 8 floater, 9 centaur, 10 robot,
  11 dog, 12 mantis, 13 deathclaw, 14 plant. The demo's prototypes have no kill type, so the converter
  derives it from the critter's art.

## Maps

`maps\junkdemo.json` holds `entrance` (tile, elevation, rotation), `script` (map script), `floor` and
`roof` arrays per elevation (100x100 squares, values are lines of `art\tiles\tiles.lst`, 1 = empty),
`spatials` (scripted trigger areas), and `objects`. A critter from the demo map:

```json
{ "pid": "0x01000002", "tile": 25908, "elevation": 0, "rotation": 2,
  "script": "rock",
  "critter": { "hp": 33, "ap": 7, "aiPacket": 1, "team": 2 },
  "inventory": [ { "count": 1, "object": { "pid": "0x00000009", "weapon": { "ammo": 30, "ammoPid": "0x0000001D" } } } ] }
```

Hexes are numbered `row * 200 + column` (200x200 per elevation). To make NEW GAME start on another
map, put `"startMap": "<name>"` in `manifest.json` (a mod can ship its own `manifest.json`).
The in-game debug overlay (**Ctrl+D**) shows the hex number under the mouse.

## Map editor

`MapEditor.cmd` opens the map editor, which is the same as
`bin\FalloutDemoHD.exe --editor [map] [--mod <folder>] [--save-as <name>]`. Without a map it opens
`junkdemo`, and a map that does not exist yet starts as an empty stretch of desert. **Ctrl+S** saves to
`mods\<folder>\maps\<name>.json` (the default folder is `my-maps`) and never changes `data\`. When
`mods.txt` exists, the folder is added to it so that the game loads the mod. **F5** saves and starts the
game on the map in a second window. **F1** lists every key.

* **1 Objects**: pick an item, critter, scenery, wall or misc prototype in the palette on the right and
  click the map to place it. Without a brush (Esc), click an object to select it and drag it to move it.
  **Delete** removes it, **R** turns it and **F2** sets its script. Right click copies the object under
  the mouse into the brush.
* **2 Floor** and **3 Roof**: paint tiles. **[** and **]** change the brush size, right click picks the
  tile under the mouse, and the `(empty)` tile erases.
* **4 Entrance**: where the player arrives. A map script can override it with `override_map_start`, and
  the demo's Scrapheap script does.
* **PgUp/PgDn** switch elevations, **Ctrl+Z/Ctrl+Y** undo and redo, **Ctrl+F** filters the palette, and
  the mouse wheel zooms (or scrolls the palette).

The palette lists only prototypes whose art exists. The demo's data contains just the art its own map
uses, so adding art (see Sprites) widens the choice. Inventories, critter teams and map scripts are not
edited in the editor; change them in the saved JSON.

## Scripts and dialogue

Scripts are Fallout 1 compiled bytecode (`.int`). The engine runs the demo's scripts unchanged and
implements the full Fallout 1 opcode set. That covers the 1997 demo's `say*` dialogue opcodes and the
release game's `gsay_*` / `giq_option` opcodes, so scripts compiled with a Fallout script compiler
(`sslc`) work. `scripts\scripts.lst` maps script numbers to files; `# local_vars=N` after an entry sets
the number of local variables.

Dialogue text is in `text\english\dialog\<script>.msg`, one message per line:

```
{102}{}{Crypts should not be here, should they boys? [The other Fools shake their heads.]}
```

The format is `{number}{sound file}{text}`. Changing the text needs no recompiling. In a mod, list
only the lines you change; the other lines still come from `data\` (see `mods\example-mod`).
Global variables and their start values are in `data\vault13.gam`.

## Characters

`premade\<id>.json` defines a character. It holds `name`, `bio` (a text file next to it), optional
`face` (interface art shown in the selection screen), `stats` (SPECIAL plus derived values),
`skills` (extra points), `tagSkills`, `traits` and `bodyType`. Every premade with a name appears in the
character selection, except the templates `blank` (used by CREATE CHARACTER), `player` and `demo`.
Add `"selectable": false` to hide one.

## Rules

* `rules\critical.json`: critical hit effects per kill type (see Prototypes) and hit location, each with a damage
  multiplier (in halves), effect flags, an optional stat check with extra effects, and the effect-level
  roll thresholds. Hit location to-hit penalties are here too.
* `rules\perks.json`: the perks offered at level ups (every 3rd level, every 4th with the Skilled trait):
  how many ranks each has, the level, SPECIAL and skill values it needs, the derived stat it raises, and
  its name and description. The perk window opens in the character screen when a perk is waiting. The
  demo's critters give no experience and its quests give 550 points, so its characters never reach
  level 3. Perks matter for mods that hand out more experience.
* `data\ai.txt`: AI packets. They set when critters run away (`min_hp`, `hurt_too_much`), the minimum hit
  chance before attacking, how often they use secondary attacks and aimed shots, and taunt message ranges
  in `text\english\game\combatai.msg`.

## Sounds and music

Drop a PCM WAV into `sound\sfx\` or `sound\music\` with the name the game asks for (for example
weapon sounds follow the original `W<effect><code><mode><material>XX1` scheme, and button clicks are
`butin1`/`butout1`). Missing sounds are skipped. The menu music is `sound\music\06vault.wav`.
