// Trucchi scelti nel launcher (sezione [Cheats] di zelda3.ini, tutti spenti
// in partenza). Quelli a tasto (passa-muri, uccidi Link, riempi, chiave)
// funzionano solo se accesi; gli altri valgono sempre mentre si gioca.
#ifndef ZELDA3_HD_CHEATS_H_
#define ZELDA3_HD_CHEATS_H_

#include "types.h"

// prima e dopo ogni fotogramma del gioco (ZeldaRunFrame): le risorse con il
// trucco "infinite" che sono scese durante il fotogramma tornano come prima
void Cheats_BeforeFrame(void);
void Cheats_AfterFrame(void);

// tasto "Uccidi Link": la morte arriva al prossimo controllo di Link
void Cheats_RequestKill(void);
// Link_ControlHandler: true una volta sola se Link va ucciso adesso
bool Cheats_TakeKill(void);

// Link non perde vita (danni dei nemici, buche)
bool Cheats_InfiniteLife(void);
// un colpo qualsiasi uccide il nemico
bool Cheats_OneHitKill(void);

#endif  // ZELDA3_HD_CHEATS_H_
