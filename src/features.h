// This file declares extensions to the base game
#ifndef ZELDA3_FEATURES_H_
#define ZELDA3_FEATURES_H_

#include "types.h"

// Special RAM locations that are unused but I use for compat things.
enum {
  kRam_APUI00 = 0x648,
  kRam_CrystalRotateCounter = 0x649,
  kRam_BugsFixed = 0x64a,
  kRam_Features0 = 0x64c,
};

enum {
  // Poly rendered uses correct speed
  kBugFix_PolyRenderer = 1,
  kBugFix_AncillaOverwrites = 1,
  kBugFix_Latest = 1,
};

// Enum values for kRam_Features0
enum {
  kFeatures0_ExtendScreen64 = 1,
  kFeatures0_SwitchLR = 2,
  kFeatures0_TurnWhileDashing = 4,
  kFeatures0_MirrorToDarkworld = 8,
  // Spade e oggetti come su GBA, che interagiscono con l'ambiente (prima
  // CollectItemsWithSword): ogni spada raccoglie gli oggetti, dalla Spada
  // Suprema rompe i cartelli, la Spada d'Oro rompe i vasi; le frecce rompono
  // i vasi (quelle d'argento tutti quelli sul percorso), il martello rompe
  // cespugli e rocce piccole chiare; la Lanterna ferisce i nemici davanti a
  // Link; il primo oggetto magico ricevuto riempie la barra della magia;
  // con le pinne B fa immergere Link (player.c, PlayerHandler_04_Swimming);
  // la super bomba si gonfia e si sgonfia (hd_fx.c, HdFx_SuperBombBeforeRender)
  kFeatures0_SwordAndItems = 16,
  // Punto di volo 9 dell'uccello: in cima al Monte della Morte a est, sulla
  // piattaforma dei pioli della Roccia della Tartaruga (prima era
  // BreakPotsWithSword, ora dentro SwordAndItems)
  kFeatures0_BirdTravel9 = 32,
  kFeatures0_DisableLowHealthBeep = 64,
  kFeatures0_SkipIntroOnKeypress = 128,
  kFeatures0_ShowMaxItemsInYellow = 256,
  kFeatures0_MoreActiveBombs = 512,

  // This is set for visual fixes that don't affect game behavior but will affect ram compare.
  kFeatures0_WidescreenVisualFixes = 1024,

  kFeatures0_CarryMoreRupees = 2048,

  kFeatures0_MiscBugFixes = 4096,

  kFeatures0_CancelBirdTravel = 8192,

  kFeatures0_GameChangingBugFixes = 16384,

  kFeatures0_SwitchLRLimit = 32768,

  kFeatures0_DimFlashes = 65536,

  // Migliorie "HD"
  // Passaggio tra aree e stanze rapido e fluido (36 frame): le fasi di
  // caricamento si fanno in un frame, Link si disegna mentre cammina a
  // velocita' costante e la telecamera parte e frena dolcemente (hd_trans.c)
  kFeatures0_FastTransitions = 1 << 17,
  // Mostra nell'HUD gli oggetti assegnati a X/L/R
  kFeatures0_ShowXLRItemsInHud = 1 << 18,
  // 1 << 19 era la transizione continua tra aree esterne (tolta: la fa meglio
  // il mondo continuo)
  // HUD moderno: cuori e magia a sinistra, tasti Y/A/B a destra, scritta
  // dell'azione del tasto A
  kFeatures0_ModernHud = 1 << 20,
  // Mondo continuo all'esterno: oltre il bordo dell'area si vedono le aree
  // vicine e la vista segue Link fino al bordo e durante il passaggio
  kFeatures0_ContinuousWorld = 1 << 21,
  // Nome del dungeon al centro dello schermo entrando dal mondo esterno, come su GBA
  kFeatures0_DungeonNames = 1 << 22,
  // Interazione bruciatura e congelamento: Lanterna e Bacchetta del Fuoco
  // incendiano cespugli ed erba alta e il fuoco si propaga, il Medaglione della
  // Fiamma brucia tutto quello che si vede; la Bacchetta del Ghiaccio congela
  // quelli sulla sua linea, il Medaglione dell'Etere tutti quelli che si vedono
  // (hd_burn.c)
  kFeatures0_BurnAndFreeze = 1 << 23,
  // Inventario esteso a 6 colonne come su GBA: quattro bottiglie separate,
  // pala e ocarina separate (hud.c)
  kFeatures0_ExtendedInventory = 1 << 24,
  // Mappa del mondo migliorata: fino a 24 segnalini con A, Y ricentra su
  // Link, B tenuto scorre veloce, mirino al centro (messaging.c)
  kFeatures0_ImprovedWorldMap = 1 << 25,
  // Mappa del dungeon migliorata: cambio di piano piu' rapido (messaging.c)
  kFeatures0_ImprovedDungeonMap = 1 << 26,
  // Mappa progressiva: le aree mai visitate restano coperte dalle nuvole
  // (messaging.c, saves/nebbia_fileN.dat)
  kFeatures0_ProgressiveMap = 1 << 27,
  // Salva stato cuori e magia: "Salva ed esci" ricorda i cuori di adesso invece
  // di rimettere quelli di ripartenza (messaging.c, Death_Func15); nella scelta
  // del salvataggio i cuori non salvati sono neri (select_file.c)
  kFeatures0_SaveHeartsAndMagic = 1 << 28,
};

#define enhanced_features0 (*(uint32*)(g_ram+0x64c))
#define msu_curr_sample (*(uint32*)(g_ram+0x650))
#define msu_volume (*(uint8*)(g_ram+0x654))
#define msu_track (*(uint8*)(g_ram+0x655))
#define hud_inventory_order ((uint8*)(g_ram + 0x225)) // 4x6 bytes
#define hud_cur_item_x (*(uint8*)(g_ram+0x656))
#define hud_cur_item_l (*(uint8*)(g_ram+0x657))
#define hud_cur_item_r (*(uint8*)(g_ram+0x658))



extern uint32 g_wanted_zelda_features;


#endif  // ZELDA3_FEATURES_H_
