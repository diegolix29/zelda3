# Zelda3 HD

A fork of [snesrev/zelda3](https://github.com/snesrev/zelda3) with a set of "HD" improvements inspired by *Link's Awakening* (Switch) and *Four Swords Adventures*, plus Italian support.
Everything is optional and can be turned on or off from the new in-game options menu (**Esc** or right stick click) or from the [launcher](https://github.com/ghost19880/Zelda-3-Launcher).

> 🇮🇹 *Versione italiana più sotto.*

## What's new

**Gameplay and comfort**
- Fast and continuous transitions between areas: Link keeps walking while the camera scrolls, no stop-and-go.
- **Continuous world** (optional, outdoors): beyond the edge of an area you see the neighbouring areas, the camera follows Link past the edges and area changes are instant. Cut bushes and grass, lifted rocks, shovel holes and defeated enemies stay as you left them while you remain outdoors, and the enemies you leave behind stay visible, frozen, beyond the edge. Death Mountain fades into mist toward the lowlands, and the Lost Woods fog covers the whole screen.
- Smooth camera that follows Link with a slight delay (4 levels), with interpolation on displays above 60 Hz.
- In-game options menu with key and gamepad remapping; settings are written back to `zelda3.ini` keeping its comments.
- Silent automatic saves on every area change and dungeon/house entrance (rotating slots), plus quick save slots.
- Interactive world map: free scrolling, crosshair, personal markers saved per save file, recenter on Link.
- Dungeon map shows the dungeon name; faster floor switching.
- Items assigned to X, L and R shown in the HUD; faster dialogue text while holding A or B.
- Inventory with the bottles in their own column on the right.
- The options menu, the button hints and the messages are in Italian or English depending on the language.
- **Modern HUD** (optional): hearts and a thin magic bar top left, Y/A/B buttons top right like on the controller with the item (and its ammo), the sword and the action of the A button (Talk, Read, Open, Lift, Grab, Pull, Throw, Dash, Swim), rupees and dungeon keys bottom left.

**Graphics**
- Sharp dialogue text drawn on top of any shader.
- Dynamic lighting: warm glow around torches, braziers, lava and fire; dark rooms lit by the Lantern and by the torches you light.
- Embers rising from flames, light rays and dust from windows, glow behind items Link holds up, heat haze over lava, denser rain with soft lightning.
- Blurred game behind the inventory, dialogues and menus, soft shadows under characters and enemies, a soft circle transition, on-screen button hints in the inventory and on the maps, a HUD that slides away during dialogues and an autosave icon.
- **Four Swords Adventures look** (`shaders/foursword`): crisp pixels, a diagonal brush texture anchored to the world (not on water), soft drop shadows, glow on bright areas and cartoon cloud shadows drifting over sunny outdoor areas. HUD and characters stay untouched.

<img width="500" height="281" alt="1" src="https://github.com/user-attachments/assets/57f92bd8-ecea-4552-8463-49ebad60474d" />
<img width="500" height="281" alt="2" src="https://github.com/user-attachments/assets/648afd26-692d-482a-9c59-6dac186eea85" />
<img width="500" height="281" alt="3" src="https://github.com/user-attachments/assets/0c60e144-1db2-4f56-be3f-a57565a06f60" />

**Language**
- Italian: the dialogue font gains accented letters; the translation itself lives in a separate repository, [zelda3-italiano](https://github.com/ghost19880/zelda3-italiano), and is downloaded by the launcher.

## Getting started

The easiest way is the [Zelda 3 Launcher (HD fork)](https://github.com/ghost19880/Zelda-3-Launcher/releases): download it, point it to your US ROM and it downloads, builds and configures everything (including the shader and, if you choose Italian, the translation).

To build by hand follow the steps of the original project below; `build_msvc.bat` is also included for Visual Studio Build Tools. The HD options are documented in `zelda3.ini`.

## Versione italiana

Fork di [snesrev/zelda3](https://github.com/snesrev/zelda3) con una serie di migliorie "HD" ispirate a *Link's Awakening* per Switch e a *Four Swords Adventures*, più il supporto all'italiano. Ogni miglioria si attiva o disattiva dal nuovo menu opzioni nel gioco (**Esc** o clic della levetta destra) oppure dal [launcher](https://github.com/ghost19880/Zelda-3-Launcher).

- **Mondo continuo** (facoltativo, all'aperto): oltre il bordo di un'area si vedono le aree vicine, la telecamera segue Link oltre i bordi e il cambio d'area è istantaneo. Cespugli ed erba tagliati, sassi sollevati, buche e nemici sconfitti restano come li hai lasciati finché resti all'aperto, e i nemici che ti lasci alle spalle restano visibili, fermi, oltre il bordo. La Montagna della Morte sfuma nella nebbia verso la pianura e la nebbia del Bosco Perduto copre tutto lo schermo.
- **HUD moderno** (facoltativo): cuori e magia a sinistra, tasti Y/A/B a destra come sul controller con oggetto, spada e la scritta dell'azione del tasto A (Parla, Leggi, Apri, Solleva…), rupie e chiavi in basso a sinistra.
- **Gioco:** transizioni veloci e continue tra le aree, telecamera fluida, menu opzioni con rimappatura di tastiera e pad, salvataggi automatici e rapidi, mappa del mondo interattiva con segnalini, nome del labirinto sulla mappa, oggetti X/L/R nell'HUD, testo più veloce.
- **Grafica:** testo nitido nei dialoghi, illuminazione dinamica, braci, raggi dalle finestre, bagliore degli oggetti ricevuti, calore sopra la lava, pioggia e lampi migliorati, sfocatura dietro menu e dialoghi, ombre di personaggi e nemici, cerchio di transizione morbido, suggerimenti dei tasti, HUD che scivola via nei dialoghi, icona del salvataggio automatico, e lo shader in stile **Four Swords Adventures** (pennellate, ombre morbide, nuvole).
- **Italiano:** font con le lettere accentate; la traduzione è nel repository [zelda3-italiano](https://github.com/ghost19880/zelda3-italiano) e la scarica il launcher.

Per cominciare scarica il [launcher](https://github.com/ghost19880/Zelda-3-Launcher/releases), indica la ROM americana e premi *Scarica*: al resto pensa lui.

---

*The original README of snesrev/zelda3 follows.*

# Zelda3

A reimplementation of Zelda 3.

Our discord server is: https://discord.gg/AJJbJAzNNJ

## About

This is a reverse engineered clone of Zelda 3 - A Link to the Past.

It's around 70-80kLOC of C code, and reimplements all parts of the original game. The game is playable from start to end.

You need a copy of the ROM to extract game resources (levels, images). Then once that's done, the ROM is no longer needed.

It uses the PPU and DSP implementation from [LakeSnes](https://github.com/elzo-d/LakeSnes), but with lots of speed optimizations.
Additionally, it can be configured to also run the original machine code side by side. Then the RAM state is compared after each frame, to verify that the C implementation is correct.

I got much assistance from spannerism's Zelda 3 JP disassembly and the other ones that documented loads of function names and variables.

## Additional features

A bunch of features have been added that are not supported by the original game. Some of them are: basic widescreen, shaders, 240 vertical res, hi-res mode7 map, MSU-1 audio, item switching, and over 30 bugfixes.

## Building

You must self-build for now. Steps for 64-bit Windows:<br>
(0) Download [Python](https://www.python.org/ftp/python/3.11.4/python-3.11.4-amd64.exe) and install with "Add to PATH" checked<br>
(1) Click the green button "Code > Download ZIP" on the github page and extract the ZIP<br>
(2) Place your USA rom named zelda3.sfc in that folder<br>
(3) Download [TCC](https://github.com/FitzRoyX/tinycc/releases/download/tcc_20230519/tcc_20230519.zip) and [SDL2](https://github.com/libsdl-org/SDL/releases/download/release-2.28.2/SDL2-devel-2.28.2-VC.zip) and extract each ZIP into the "third-party" subfolder<br>
(4) Double-click "get_python_libs.bat" in the main dir.<br>
(5) Double-click "extract_assets.bat" in the main dir. This will create zelda3_assets.dat.<br>
(6) Double-click "run_with_tcc.bat" in the main dir. This will create zelda3.exe and run it.<br>
(7) Configure with zelda3.ini in a text editor like notepad++<br>

For other platforms and compilers, see: https://github.com/snesrev/zelda3/blob/main/BUILDING.md

## Usage and controls

The game supports snapshots. The joypad input history is also saved in the snapshot. It's thus possible to replay a playthrough in turbo mode to verify that the game behaves correctly.

The game is run with `./zelda3` and takes an optional path to the ROM-file, which will verify for each frame that the C code matches the original behavior.

| Button | Key         |
| ------ | ----------- |
| Up     | Up arrow    |
| Down   | Down arrow  |
| Left   | Left arrow  |
| Right  | Right arrow |
| Start  | Enter       |
| Select | Right shift |
| A      | X           |
| B      | Z           |
| X      | S           |
| Y      | A           |
| L      | C           |
| R      | V           |

The keys can be reconfigured in zelda3.ini

Additionally, the following commands are available:

| Key | Action                |
| --- | --------------------- |
| Tab | Turbo mode |
| W   | Fill health/magic     |
| Shift+W   | Fill rupees/bombs/arrows     |
| Ctrl+E | Reset            |
| P   | Pause (with dim)                |
| Shift+P   | Pause (without dim)                |
| Ctrl+Up   | Increase window size                |
| Ctrl+Down   | Decrease window size                |
| T   | Toggle replay turbo mode  |
| O   | Set dungeon key to 1  |
| K   | Clear all input history from the joypad log  |
| L   | Stop replaying a shapshot  |
| R   | Toggle between fast and slow renderer |
| F   | Display renderer performance |
| F1-F10 | Load snapshot      |
| Alt+Enter | Toggle Fullscreen     |
| Shift+F1-F10 | Save snapshot |
| Ctrl+F1-F10 | Replay the snapshot |
| 1-9 | Load a dungeons playthrough snapshot |
| Ctrl+1-9 | Run a dungeons playthrough in turbo mode |


## License

This project is licensed under the MIT license. See 'LICENSE.txt' for details.
