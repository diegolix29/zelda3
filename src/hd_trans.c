// Transizioni veloci: passaggio fra aree esterne e fra stanze che sembra
// continuo (vedi hd_trans.h). Si tocca solo il disegno.
//
// Link: nell'originale resta fermo mentre il gioco carica e mentre la
// telecamera scorre, poi scatta di 4-8 pixel per frame, salta sulla soglia
// della porta e finisce a 2 pixel per frame. Qui si disegna in un punto che
// avanza a velocita' costante verso la posizione in cui il giocatore
// riprende il controllo, calcolata dalle tabelle del gioco (Dungeon_ /
// Overworld_TransitionPlan): a fine transizione coincide con quella vera.
//
// Telecamera: la vista parte piano, accelera e frena arrivando esattamente
// dove e quando arriva quella del gioco. Non puo' arrivare dopo: finito lo
// scorrimento il gioco riusa subito la VRAM rimasta indietro.
//
// Durata: ogni transizione dura kHdTransFrames frame. Lo scorrimento del
// gioco fa 1 o 2 passi per frame (HdTrans_ScrollSteps) in base ai frame che
// restano; la vista segue il tempo e non i passi, cosi' non si vede
// l'alternanza.
#include <math.h>
#include <stdlib.h>
#include "hd_trans.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "hd_fx.h"

int g_trans_link_dx, g_trans_link_dy;

enum {
  kKind_None,
  kKind_Dungeon,
  kKind_Outdoors,
};

static struct {
  int kind, axis, dir;
  float link;         // coordinata di Link disegnato lungo l'asse
  float link_off;     // dopo la transizione: distanza che resta da recuperare
  int last_cam;       // telecamera del gioco nel frame precedente
  int elapsed;        // frame della transizione gia' fatti
  bool scrolling;     // lo scorrimento e' cominciato
  int scroll_t;       // frame di scorrimento gia' fatti
  int steps_total;    // passi di scorrimento di tutta la transizione
  int cam_start, cam_end;
  float b0x, b0y;     // spostamento della telecamera fluida a inizio transizione
  int bt;
} s;

static float SmoothStep(float u) {
  u = u < 0 ? 0 : u > 1 ? 1 : u;
  return u * u * (3 - 2 * u);
}

static int Round(float v) {
  return (int)floorf(v + 0.5f);
}

// Frame di scorrimento che restano (compreso il prossimo) dopo "done" frame
// della transizione: quanti ne servono per arrivare a kHdTransFrames, ma
// almeno uno ogni max_steps passi e al massimo uno per passo
static int ScrollFramesLeft(const HdTransPlan *p, int done) {
  if (p->steps_left <= 0)
    return 0;
  int f = kHdTransFrames - done - p->frames_before - p->frames_after;
  int lo = (p->steps_left + p->max_steps - 1) / p->max_steps;
  return f < lo ? lo : f > p->steps_left ? p->steps_left : f;
}

// I passi doppi si spargono in modo uniforme lungo lo scorrimento
int HdTrans_ScrollSteps(const HdTransPlan *p) {
  if (s.kind == kKind_None)
    return 2;
  int f = ScrollFramesLeft(p, s.elapsed);
  if (f <= 0)
    return 1;
  if (!s.scrolling)
    s.steps_total = p->steps_left;
  int done = s.steps_total - p->steps_left, t = s.scrolling ? s.scroll_t : 0;
  int total = done + p->steps_left;
  int want = (2 * total * (t + 1) + (t + f)) / (2 * (t + f)) - done;  // arrotondato
  int lo = p->steps_left - p->max_steps * (f - 1);  // altrimenti non si arriva in tempo
  if (want < lo) want = lo;
  if (want < 1) want = 1;
  return want > p->max_steps ? p->max_steps : want;
}

static int CurrentKind(HdTransPlan *plan) {
  if (!(enhanced_features0 & kFeatures0_FastTransitions))
    return kKind_None;
  if (HdFx_RoomScroll())
    return Dungeon_TransitionPlan(plan) ? kKind_Dungeon : kKind_None;
  if (main_module_index == 9)
    return Overworld_TransitionPlan(plan) ? kKind_Outdoors : kKind_None;
  return kKind_None;
}

bool HdTrans_Update(int prev_dx, int prev_dy, int *bias_x, int *bias_y) {
  HdTransPlan plan;
  int kind = CurrentKind(&plan);
  *bias_x = *bias_y = 0;
  g_trans_link_dx = g_trans_link_dy = 0;

  if (kind == kKind_None) {
    if (s.kind != kKind_None) {
      // transizione finita: Link disegnato raggiunge quello vero
      int link = s.axis ? link_y_coord : link_x_coord;
      s.link_off = s.link - link;
      s.kind = kKind_None;
    }
    if (s.link_off != 0) {
      if (main_module_index != 7 && main_module_index != 9) {
        s.link_off = 0;  // altro modulo (menu, mappa, caricamento): niente scie
      } else {
        s.link_off *= 0.7f;
        if (fabsf(s.link_off) < 0.5f)
          s.link_off = 0;
      }
      int o = Round(s.link_off);
      *(s.axis ? &g_trans_link_dy : &g_trans_link_dx) = o;
    }
    return false;
  }

  int link = plan.axis ? link_y_coord : link_x_coord;
  int cam = plan.axis ? BG2VOFS_copy2 : BG2HOFS_copy2;
  if (s.kind != kind || s.axis != plan.axis || s.dir != plan.dir) {
    // comincia una transizione (se la precedente si stava ancora ricucendo,
    // si parte da dove Link e' disegnato)
    float off = s.axis == plan.axis ? s.link_off : 0;
    s.kind = kind, s.axis = plan.axis, s.dir = plan.dir;
    s.link = link + off;
    s.link_off = 0;
    s.last_cam = cam;
    s.elapsed = 0;
    s.scrolling = false;
    s.b0x = (float)prev_dx, s.b0y = (float)prev_dy;
    s.bt = 0;
  }

  s.elapsed++;
  int scroll_left = ScrollFramesLeft(&plan, s.elapsed);
  int frames = plan.frames_before + scroll_left + plan.frames_after;

  // Link: velocita' costante fino alla posizione finale
  float rem = (plan.link_final - s.link) * s.dir;
  if (rem > 0) {
    float v = frames > 0 ? rem / (frames + 1) : rem;
    if (v > 3.0f) v = 3.0f;
    if (v < 0.5f) v = 0.5f;
    if (v > rem) v = rem;
    s.link += v * s.dir;
  } else if (frames == 0) {
    s.link = (float)link;
  }
  *(s.axis ? &g_trans_link_dy : &g_trans_link_dx) = Round(s.link) - link;

  // Telecamera
  int bias = 0;
  if (!s.scrolling && cam != s.last_cam && abs(cam - s.last_cam) <= 32) {
    s.scrolling = true;
    s.cam_start = s.last_cam;
    s.cam_end = plan.cam_end;
    s.scroll_t = 0;
  }
  // la vista segue la curva nel tempo dello scorrimento e arriva insieme
  // alla telecamera del gioco
  int d = s.cam_end - s.cam_start;
  if (s.scrolling && d != 0 && (s.cam_end - cam) * d > 0) {
    s.scroll_t++;
    float u = (float)s.scroll_t / (s.scroll_t + scroll_left);
    bias = Round(s.cam_start + d * SmoothStep(u)) - cam;
  }
  s.last_cam = cam;
  *(s.axis ? bias_y : bias_x) = bias;

  // la telecamera fluida torna a zero in 10 frame invece che di colpo
  float k = s.bt < 10 ? 1.0f - (s.bt + 1) / 10.0f : 0;
  s.bt++;
  *bias_x += Round(s.b0x * k);
  *bias_y += Round(s.b0y * k);
  return true;
}
