# Zelda3 HD

A fork of [snesrev/zelda3](https://github.com/snesrev/zelda3) with a set of "HD" improvements inspired by *Link's Awakening* (Switch) and *Four Swords Adventures*, plus Italian support.
Everything is optional and can be turned on or off from the new in-game options menu (**Esc** or right stick click) or from the [launcher](https://github.com/ghost19880/Zelda-3-Launcher). The HD effects, the smooth camera, smoothness above 60 Hz, dynamic lighting, the continuous world and fast transitions need the optimized PPU (`NewRenderer = 1`): without it they stay off.

> 🇮🇹 *Versione italiana più sotto.*

## What's new

**Gameplay and comfort**
- Quick and smooth transitions between areas and rooms: no loading pauses, Link keeps walking and the camera starts and stops gently.
- **Continuous world** (optional, outdoors): beyond the edge of an area you see the neighbouring areas, the camera follows Link past the edges and area changes are instant. Cut bushes and grass, lifted rocks, shovel holes and defeated enemies stay as you left them while you remain outdoors. Death Mountain and the lowlands below it are not shown together (the edge between them works like the edge of the world), and the Lost Woods fog covers the whole screen.
  - The whole world is active: every area of the current world moves at the same time, also far away and off screen, and all its enemies are present from the start (only the areas near the view are drawn).
  - Every enemy lives in its own "screen", as on the SNES (192 pixels to the sides, 176 above and below): it notices Link only inside it, and without Link it never strays farther than that from its starting point. Enemies that lose Link off screen go back to their place; when Link is more than 3 screens away, enemies he hurt or alerted are back at full health and defeated enemies come back.
  - Beyond the edge: enemies chasing Link follow him into the next area, flying enemies and projectiles cross the borders, fire spreads from one area to another, falling rocks keep falling past the edge and arrows stuck in walls stay where they hit.
- Smooth camera that follows Link with a slight delay (4 levels), with interpolation on displays above 60 Hz.
- In-game options menu with key and gamepad remapping; settings are written back to `zelda3.ini` keeping its comments.
- Quick save slots in the options menu.
- **Improved world map** (optional): free scrolling, crosshair, up to 24 personal markers saved per save file, Y recenters on Link, hold B to scroll faster.
- **Progressive map** (optional): the world map starts covered by clouds and is revealed one area at a time where Link goes; the explored areas are stored in the save file.
- **Improved dungeon map** (optional): faster floor switching and the dungeon name at the top.
- Items assigned to X, L and R shown in the HUD; faster dialogue text while holding A or B.
- **Controller rumble** (optional, off, light, medium or strong; inspired by *Ocarina of Time*): bombs and explosions (weaker when far from Link), damage according to how many hearts you lose, death, Pegasus Boots crashes, screen shakes, electric shocks, blocked hits on the shield, charged Spin Attack, landing after a jump or a fall, the Hookshot grabbing, hammer blows, lifting heavy rocks, and choosing, copying or erasing a file.
- The player name can be typed on the keyboard too (letters, digits, Backspace), besides choosing the letters with the controller as in the original.
- **Extended inventory** (optional): 6 columns like on GBA, with four separate bottles and the shovel and flute in separate slots.
- **Sword and items interact with the world** (optional, like the GBA version): every sword collects items, stronger swords cut signs and break pots, arrows break pots, the hammer breaks bushes and light rocks, the Magic Powder breaks bushes and pots, the Lantern hurts enemies, and the first magic item you get fills your magic.
- **Fire and ice** (optional): the Lantern and the Fire Rod set bushes and tall grass on fire, and the fire spreads; Bombos burns everything on screen. The Ice Rod and Ether freeze bushes and grass (frozen grass is slippery ice), and fire and ice cancel each other; fire slowly melts frozen water. Arrows of Link and of the enemies stuck in walls stay there instead of vanishing.
- **Dungeon names** (optional): entering a dungeon from its main door shows its name, as on GBA (with "Level N" in the Dark World).
- Optional: up to 10 bombs at once and an extra flute travel point (9) on top of Death Mountain. More room for Link's weapons and items on screen.
- **Gold Medicine** (optional, from *BS Zelda: Ancient Stone Tablets*): a fourth cauldron in the witch's shop, 50 rupees, one per day (computer clock, back at midnight). Like the blue medicine it refills life and magic, and for 10 minutes the Spin Attack charges twice as fast.
- The options menu, the button hints and the messages are in Italian or English depending on the language; in Italian the Start menu (item names, actions, headers) is translated too.
- **Modern HUD** (optional): hearts and a thin magic bar top left, Y/A/B buttons top right like on the controller with the item (and its ammo), the sword and the action of the A button (Talk, Read, Open, Lift, Grab, Pull, Throw, Dash, Swim), rupees and dungeon keys bottom left, and top center the timer (dropped super bomb, shovel game) as mm:ss.cc and the dungeon floor when it changes, on a transparent band. Like the original HUD it stays during dialogues and the Start menu pushes it down as it slides in.

**Graphics**
- Sharp dialogue text drawn on top of any shader.
- Dynamic lighting: warm glow around torches, braziers, lava and fire; dark rooms lit by the Lantern and by the torches you light.
- **Realistic water and waterfalls** (optional, like *Four Swords Adventures*): depth, foam along the shores and around rocks, a moving surface and currents, flowing waterfalls with foam and mist at their base, also in the Dark World and in dungeons (deep dungeon water stays see-through, as in the original). Mirror and warp ripples and the whirlpool mosaic cover the HD effects too.
- **Heat and flames** (optional): embers rising from torches, braziers, fire and lava, and the air shimmering above lava.
- Light rays and dust from windows, glow behind items Link holds up, denser rain with soft lightning, Lost Woods and Skull Woods fog made of soft drifting clouds in the original three shades (off by default, like the rain).
- Widescreen also in special areas (Zora's Domain...) and while scrolling between dungeon rooms.
- Blurred game behind the inventory, dialogues and menus, soft shadows under characters and enemies, a soft circle transition, on-screen button hints in the inventory (once the menu has finished sliding) and on the maps, and an autosave icon.
- **Four Swords Adventures look** (`shaders/foursword`): crisp pixels, a diagonal brush texture anchored to the world (not on water), soft drop shadows, glow on bright areas and cartoon cloud shadows of three different shapes drifting over sunny outdoor areas. HUD and characters stay untouched.

<img width="500" height="281" alt="3" src="https://github.com/user-attachments/assets/5ca23c38-fb94-439a-98fe-52fc64f99db5" />
<img width="500" height="281" alt="4" src="https://github.com/user-attachments/assets/42f82c58-6f48-4f21-b400-a87536fc1a17" />
<img width="500" height="281" alt="5" src="https://github.com/user-attachments/assets/fee65366-8e6a-4192-9418-14593d34f422" />

**Language**
- Italian: the dialogue font gains accented letters; the translation itself lives in a separate repository, [zelda3-italiano](https://github.com/ghost19880/zelda3-italiano), and is downloaded by the launcher.

## Getting started

The easiest way is the [Zelda 3 Launcher (HD fork)](https://github.com/ghost19880/Zelda-3-Launcher/releases): download it, point it to your US ROM and it downloads and builds everything (and, if you choose Italian, the translation). The first `zelda3.ini` it creates has the improvements turned off: you choose which ones to use in **Settings**.

To build by hand follow the steps of the original project below; `build_msvc.bat` is also included for Visual Studio Build Tools. The HD options are documented in `zelda3.ini`.

## Versione italiana

Fork di [snesrev/zelda3](https://github.com/snesrev/zelda3) con una serie di migliorie "HD" ispirate a *Link's Awakening* per Switch e a *Four Swords Adventures*, più il supporto all'italiano. Ogni miglioria si attiva o disattiva dal nuovo menu opzioni nel gioco (**Esc** o clic della levetta destra) oppure dal [launcher](https://github.com/ghost19880/Zelda-3-Launcher). Effetti HD, telecamera fluida, fluidità oltre i 60 Hz, illuminazione dinamica, mondo continuo e transizioni rapide richiedono la PPU ottimizzata (`NewRenderer = 1`): senza restano spenti.

- **Mondo continuo** (facoltativo, all'aperto): oltre il bordo di un'area si vedono le aree vicine, la telecamera segue Link oltre i bordi e il cambio d'area è istantaneo. Cespugli ed erba tagliati, sassi sollevati, buche e nemici sconfitti restano come li hai lasciati finché resti all'aperto. La Montagna della Morte e la pianura sotto di lei non si vedono insieme (il confine fra le due fa da fine del mondo) e la nebbia del Bosco Perduto copre tutto lo schermo. Tutto il mondo è attivo: ogni area del mondo in cui si trova Link si muove insieme alle altre, anche lontano e fuori dallo schermo, con tutti i suoi nemici presenti fin dall'inizio (si disegnano solo le aree vicine alla vista). Ogni nemico vive nella sua "schermata", come sullo SNES (192 pixel ai lati, 176 sopra e sotto): nota Link solo lì dentro e senza Link non si allontana di più dal suo punto di partenza. Chi perde Link fuori dallo schermo torna al suo posto; con Link a più di 3 schermate, i nemici feriti o messi in allarme tornano con la vita piena e quelli sconfitti ricompaiono. Oltre il bordo: chi insegue Link lo segue nell'area dopo, nemici volanti e colpi passano i confini, il fuoco si propaga da un'area all'altra, i sassi che cadono continuano oltre il bordo e le frecce piantate nei muri restano.
- **HUD moderno** (facoltativo): cuori e magia a sinistra, tasti Y/A/B a destra come sul controller con oggetto, spada e la scritta dell'azione del tasto A (Parla, Leggi, Apri, Solleva…), rupie e chiavi in basso a sinistra, e in alto al centro, su una fascia trasparente, il contatore (super bomba posata, gioco della pala) in mm:ss.cc e il piano del labirinto quando cambia. Come l'HUD originale resta nei dialoghi e il menu Start lo spinge giù mentre scende.
- **Spada e oggetti interagiscono con l'ambiente** (facoltativo, come su GBA): ogni spada raccoglie gli oggetti, le spade più forti tagliano i cartelli e rompono i vasi, le frecce rompono i vasi, il martello rompe cespugli e rocce chiare, la Polvere Magica rompe cespugli e vasi, la Lanterna ferisce i nemici e il primo oggetto magico riempie la magia.
- **Fuoco e ghiaccio** (facoltativo): Lanterna e Bacchetta del Fuoco incendiano cespugli ed erba alta e il fuoco si propaga; il Medaglione della Fiamma brucia tutto lo schermo. La Bacchetta del Ghiaccio e il Medaglione dell'Etere congelano cespugli ed erba (l'erba congelata è ghiaccio che fa scivolare); fuoco e ghiaccio si annullano e il fuoco scioglie piano l'acqua ghiacciata. Le frecce di Link e dei nemici piantate nei muri restano lì invece di sparire.
- **Acqua e cascate realistiche** (facoltativo, come in *Four Swords Adventures*): profondità, schiuma lungo le rive e attorno ai sassi, superficie che si muove e correnti, cascate che scorrono con schiuma e nebbia alla base, anche nel Mondo Oscuro e nei labirinti (l'acqua profonda dei labirinti resta trasparente come nell'originale). Le onde dello specchio e dei teletrasporti e il mosaico del mulinello coprono anche gli effetti HD.
- **Inventario esteso** (facoltativo): 6 colonne come su GBA, con quattro bottiglie separate e pala e flauto in posti separati.
- **Mappa migliorata** (facoltativa): fino a 24 segnalini, Y torna su Link, B tenuto premuto scorre più veloce; **mappa dei labirinti migliorata** (facoltativa) con cambio di piano più rapido e il nome del labirinto in alto; **mappa progressiva** (facoltativa): la mappa del mondo parte coperta dalle nuvole e si scopre un'area alla volta dove passa Link; le aree esplorate si salvano nella partita.
- **Nomi dei labirinti** (facoltativo): entrando dalla porta principale compare il nome del labirinto, come su GBA (con "Livello N" nel Mondo delle Tenebre).
- Facoltativi: fino a 10 bombe insieme e un punto di volo in più (9) in cima alla Montagna della Morte. Più posto sullo schermo per armi e oggetti di Link.
- **Pozione d'oro** (facoltativa, da *BS Zelda: Le Antiche Tavole di Pietra*): un quarto calderone nella bottega della strega, 50 rupie, una al giorno (orologio del computer, torna a mezzanotte). Come la pozione blu ridà energia e Magia, e per 10 minuti l'attacco rotante si carica al doppio della velocità.
- **Gioco:** transizioni rapide e fluide tra aree e stanze (Link non si ferma), telecamera fluida, menu opzioni con rimappatura di tastiera e pad, slot di salvataggio rapido, mappa del mondo interattiva con segnalini, oggetti sui tasti X, L e R (si vedono nell'HUD), testo più veloce, **vibrazione del controller** (facoltativa: leggera, media o forte, ispirata a *Ocarina of Time*: bombe ed esplosioni, più deboli se lontane, danni secondo i cuori persi, morte, schianto con gli Stivali di Pegaso, terremoti, scosse, colpi parati dallo scudo, Attacco Rotante carico, atterraggi, rampino, martello, pietre pesanti e scelta, copia o cancellazione dei file), nome del giocatore da scrivere anche con la tastiera (lettere, cifre, Backspace) oltre che col controller come nell'originale.
- **Grafica:** testo nitido nei dialoghi, illuminazione dinamica, effetto calore e fiamme (braci e aria che tremola sopra la lava), raggi dalle finestre, bagliore degli oggetti ricevuti, pioggia e lampi migliorati, nebbia del Bosco Perduto e del Bosco d'Ossa fatta di nuvole morbide che scorrono, con le tre tonalità dell'originale (pioggia e nebbia spente di base), schermo largo anche nelle aree speciali e nello scorrimento tra le stanze, sfocatura dietro menu e dialoghi, ombre di personaggi e nemici, cerchio di transizione morbido, suggerimenti dei tasti, icona del salvataggio automatico, e lo shader in stile **Four Swords Adventures** (pennellate, ombre morbide, ombre di nuvole di tre forme diverse).
- **Italiano:** font con le lettere accentate e menu Start tradotto (nomi degli oggetti, azioni, intestazioni); la traduzione è nel repository [zelda3-italiano](https://github.com/ghost19880/zelda3-italiano) e la scarica il launcher.

Per cominciare scarica il [launcher](https://github.com/ghost19880/Zelda-3-Launcher/releases), indica la ROM americana e premi *Scarica*: al resto pensa lui. Il primo `zelda3.ini` che crea ha le migliorie spente: scegli tu quali usare dalle **Impostazioni**.

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
