// Vibrazione del controller: i momenti del gioco che fanno vibrare
// (opzione [Features] Rumble). Forze e durate ispirate a Ocarina of Time,
// piu' brevi e leggere per il ritmo di A Link to the Past.
#ifndef ZELDA3_HD_RUMBLE_H_
#define ZELDA3_HD_RUMBLE_H_

#include "types.h"

enum {
  kRumble_Explosion,   // bombe, super bomba, esplosioni dei nemici e dei boss
  kRumble_DashCrash,   // schianto con gli Stivali di Pegaso
  kRumble_HurtSmall,   // danno sotto un cuore
  kRumble_HurtMedium,  // da uno a due cuori
  kRumble_HurtBig,     // due cuori o piu'
  kRumble_Death,
  kRumble_Shake,       // lo schermo trema (boss, Sisma, crolli)
  kRumble_Shock,       // scossa elettrica
  kRumble_Shield,      // colpo parato dallo scudo
  kRumble_SpinReady,   // Attacco Rotante carico
  kRumble_LedgeLand,   // atterraggio dopo un salto da un dislivello
  kRumble_FallLand,    // atterraggio al piano di sotto
  kRumble_Hookshot,    // il rampino si aggancia
  kRumble_Hammer,      // colpo di martello: solo un tocco
  kRumble_HeavyLift,   // sollevare una pietra pesante
  // scelta del file (come OoT): valgono anche fuori dal gioco
  kRumble_MenuConfirm, // file scelto, copiato, nome registrato
  kRumble_MenuErase,   // file cancellato
  kRumble_MenuError,   // scelta non possibile (suono d'errore)
  kRumble_Count,
};

// Un momento del gioco
void HdRumble(int kind);
// Un momento in un punto del mondo: piena entro 3 caselle da Link, poi cala
// fino a un terzo (il pan dei suoni del gioco guarda solo i 256 pixel della
// sua schermata: col 16:9 e la telecamera fluida anche una bomba ai piedi di
// Link risultava "di lato")
void HdRumble_At(int kind, uint16 x, uint16 y);
// Suono in partenza verso il chip audio (nmi.c): le esplosioni
void HdRumble_Sound(uint8 sfx1);
// A ogni fotogramma del gioco: momenti che si vedono dallo stato
void HdRumble_Tick(void);

#endif  // ZELDA3_HD_RUMBLE_H_
