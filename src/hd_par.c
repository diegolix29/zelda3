// Lavori in parallelo per gli effetti HD. I thread di lavoro nascono al
// primo uso e aspettano su un semaforo; il compito e' diviso in pezzi piu'
// piccoli delle parti, che ogni thread (anche quello del gioco) prende uno
// alla volta finche' ce ne sono: chi trova righe senza acqua ne prende di
// piu'. Se SDL non riesce a creare i thread si lavora da soli
#include <stdint.h>
#include <SDL.h>
#include "hd_par.h"

enum { kPiecesPerPart = 4 };

static SDL_sem *g_go[kHdParMaxParts], *g_done;
static int g_workers = -1;  // -1 = non ancora pronti
static HdParFunc *g_fn;
static void *g_ctx;
static int g_n, g_pieces;
static SDL_atomic_t g_next;

static void RunPieces(int part) {
  for (;;) {
    int p = SDL_AtomicAdd(&g_next, 1);
    if (p >= g_pieces)
      break;
    g_fn(g_ctx, part, (int)((int64_t)g_n * p / g_pieces), (int)((int64_t)g_n * (p + 1) / g_pieces));
  }
}

static int Worker(void *arg) {
  int i = (int)(intptr_t)arg;
  for (;;) {
    SDL_SemWait(g_go[i]);
    RunPieces(i + 1);
    SDL_SemPost(g_done);
  }
  return 0;
}

static void Init(void) {
  // lascia un core al gioco e uno al resto (audio, scheda video)
  int want = SDL_GetCPUCount() - 2;
  if (want > kHdParMaxParts - 1)
    want = kHdParMaxParts - 1;
  g_workers = 0;
  if (want <= 0 || !(g_done = SDL_CreateSemaphore(0)))
    return;
  for (int i = 0; i < want; i++) {
    if (!(g_go[i] = SDL_CreateSemaphore(0)))
      break;
    SDL_Thread *t = SDL_CreateThread(Worker, "hd_par", (void *)(intptr_t)i);
    if (!t)
      break;
    SDL_DetachThread(t);
    g_workers++;
  }
}

static void Run(HdParFunc *fn, void *ctx, int n, int per_part) {
  if (g_workers < 0)
    Init();
  int parts = g_workers + 1;
  if (g_workers == 0 || n < 2 * parts) {
    fn(ctx, 0, 0, n);
    return;
  }
  g_fn = fn, g_ctx = ctx, g_n = n;
  g_pieces = parts * per_part;
  if (g_pieces > n)
    g_pieces = n;
  SDL_AtomicSet(&g_next, 0);
  for (int i = 0; i < g_workers; i++)
    SDL_SemPost(g_go[i]);
  RunPieces(0);
  for (int i = 0; i < g_workers; i++)
    SDL_SemWait(g_done);
}

void HdPar_For(HdParFunc *fn, void *ctx, int n) {
  Run(fn, ctx, n, kPiecesPerPart);
}

void HdPar_ForParts(HdParFunc *fn, void *ctx, int n) {
  Run(fn, ctx, n, 1);
}
