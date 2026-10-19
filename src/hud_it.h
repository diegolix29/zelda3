#ifndef ZELDA3_HUD_IT_H_
#define ZELDA3_HUD_IT_H_

#include "types.h"

// Menu Start in italiano (lingua dei dialoghi "it"): le scritte si disegnano
// con caratteri piu' stretti di quelli del gioco in caselle del BG3 libere
// mentre il menu e' aperto (quelle del testo dei dialoghi)
bool HudIt_Active(void);

// Intestazioni dei riquadri
enum {
  kHudItLabel_Item,       // "STRUMENTO" (ITEM)
  kHudItLabel_Do,         // "AZIONE" (DO)
  kHudItLabel_Equipment,  // "EQUIPAGGIAMENTO" (EQUIPMENT)
  kHudItLabel_Dungeon,    // "LABIRINTO" (DUNGEON ITEM)
  kHudItLabel_Pendants,   // "AMULETI" (PENDANTS)
  kHudItLabel_Crystals,   // "CRISTALLI" (CRYSTALS)
};
// Scrive l'intestazione a partire da dst (n caselle)
void HudIt_DrawLabel(uint16 *dst, int label);
int HudIt_LabelTiles(int label);

// Abilita' del riquadro AZIONE (0 ALZA, 1 LEGGI, 2 PARLA, 3 TIRA, 4 CORRI,
// 5 NUOTA) nella riga di 5 caselle dst; level e' il livello di ALZA (1-3)
void HudIt_DrawAbility(uint16 *dst, int ability, int level);

// Nome dell'oggetto selezionato nel riquadro 8x2 (dst = prima casella in alto
// a sinistra, righe da 32 caselle)
void HudIt_DrawItemName(uint16 *dst, int item, int btn_index);

#endif  // ZELDA3_HUD_IT_H_
