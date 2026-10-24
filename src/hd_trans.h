// Transizioni veloci (opzione FastTransitions): passaggio fra le aree esterne
// (senza mondo continuo) e fra le stanze dei dungeon che sembra continuo.
// La logica del gioco resta quella originale; cambia solo come si disegna:
// Link avanza a velocita' costante dal punto in cui comincia il passaggio a
// quello in cui il giocatore riprende il controllo, e la telecamera parte e
// frena dolcemente.
#ifndef ZELDA3_HD_TRANS_H_
#define ZELDA3_HD_TRANS_H_

#include "types.h"

// Ogni transizione dura questi frame, dal cambio di schermata all'ultimo
// frame prima che il giocatore riprenda il controllo: i passi di
// scorrimento si distribuiscono (1 o 2 per frame) per arrivarci
enum { kHdTransFrames = 36 };

// Previsione della transizione in corso
typedef struct HdTransPlan {
  int axis;           // 0 = orizzontale, 1 = verticale
  int dir;            // +1 o -1 lungo l'asse
  int link_final;     // coordinata di Link (lungo l'asse) a fine transizione
  int cam_end;        // dove si ferma la telecamera del gioco (lungo l'asse)
  int steps_left;     // passi di scorrimento che mancano
  int max_steps;      // passi al massimo in un frame
  int frames_before;  // frame che mancano all'inizio dello scorrimento
  int frames_after;   // frame dopo lo scorrimento (soglia, uscita dalla porta)
} HdTransPlan;

// Vere se il gioco e' in quella transizione (dungeon.c, overworld.c)
bool Dungeon_TransitionPlan(HdTransPlan *p);
bool Overworld_TransitionPlan(HdTransPlan *p);

// Passi di scorrimento da fare in questo frame (durante la logica)
int HdTrans_ScrollSteps(const HdTransPlan *p);

// Spostamento con cui disegnare Link rispetto alla sua posizione vera
extern int g_trans_link_dx, g_trans_link_dy;

// Una volta per frame di gioco. prev_dx/prev_dy: spostamento della vista nel
// frame precedente (telecamera fluida). Vera se la vista va spostata di
// (*bias_x, *bias_y) rispetto alla telecamera del gioco al posto della
// telecamera fluida.
bool HdTrans_Update(int prev_dx, int prev_dy, int *bias_x, int *bias_y);

#endif  // ZELDA3_HD_TRANS_H_
