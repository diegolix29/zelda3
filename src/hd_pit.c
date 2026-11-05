// Stanza di sotto nelle buche dei labirinti (FxPitDepth)
//
// Al posto della trama scura delle buche si vede, appena sfocata e in ombra,
// la stanza in cui si cade (dung_hdr_travel_destinations[0]). Cadendo Link
// tiene la stessa posizione dentro la stanza, quindi ogni punto della buca
// corrisponde allo stesso punto (x & 0x1ff, y & 0x1ff) della stanza di sotto.
//
// La stanza di sotto si costruisce una volta entrando nella stanza, come fa il
// mondo continuo con le aree vicine (hd_world.c RenderArea): copia di RAM e
// VRAM, caricamento con le funzioni del gioco, disegno in un'immagine 512x512,
// poi tutto torna com'era. A ogni fotogramma si sostituiscono solo i pixel
// "vuoto" delle caselle con attributo 0x20 (buca); il bordo disegnato resta.
// Effetto profondita': la stanza di sotto e' piu' lontana, quindi rimpicciolita
// verso il centro dello schermo (scorre piu' lenta della telecamera) e in
// ombra sotto i bordi della buca.
//
// Transizioni: ogni stanza tiene la sua mappa delle buche e la sua immagine
// (le ultime due stanze), e ogni pixel usa la stanza in cui sta nel mondo
// (512x512 per stanza). Cosi' nello scorrimento fra due stanze e nella
// dissolvenza della caduta le buche non tornano alla trama originale.

#include "hd_pit.h"
#include "features.h"
#include "hd_fx.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "dungeon.h"
#include "load_gfx.h"
#include "snes/ppu.h"
#include "sprite.h"
#include "sprite_main.h"
#include "hd_world.h"
#include <stdlib.h>
#include <string.h>

bool g_fx_pit;

enum {
  kRoomPx = 512,
  kAttrPit = 0x20,
  kEntries = 2,
};

// sfocatura della stanza di sotto, raggio in pixel (anche con la virgola:
// 1,5 = pesi 0,5 1 1 1 0,5); 0 = nitida. L'utente l'ha provata a 2 (due
// passate), 1, 0 e 1,5 e ha scelto nitida: la stanza si deve riconoscere
static const float kBlurRadius = 0.0f;
static const float kDepth = 0.12f;     // quanto la stanza di sotto e' rimpicciolita
static const float kShadeTop = 18.0f;  // ombra del bordo in alto (pixel)
static const float kShadeSide = 10.0f; // ombra dei bordi ai lati e in basso

typedef struct PitRoom {
  int room;            // stanza con le buche (-1 = posto libero)
  int dest;            // stanza di sotto (-1 = nessuna: buche normali)
  uint32 last_use;
  uint8 pit[64 * 64];  // 1 = casella buca (attributo 0x20 di BG2 o BG1)
  uint8 hole[2][128];  // colori del "vuoto" (palette*16 + indice) per BG1/BG2
  uint8 show[64 * 64]; // 1 = casella di una buca grande: ci si vede la stanza sotto
  bool holes_known;
  bool black;          // il gioco disegna le buche nere: stanza di sotto nascosta
  uint32 *img;         // stanza di sotto: 0x00RRGGBB, gia' sfocata e scurita
} PitRoom;

static PitRoom g_rooms[kEntries] = { { -1, -1 }, { -1, -1 } };
static uint32 g_use;
static uint32 *g_tmp;
static uint8 g_saved_ram[0x20000];
// stanza le cui caselle sono in dung_bg2/dung_bg1 (HdPit_RoomLoaded), -1 = non si sa
static int g_loaded_room = -1;
static bool g_building;
static uint16 g_saved_vram[0x8000];

static inline uint32 Pack(int r, int g, int b) {
  return (uint32)r << 16 | (uint32)g << 8 | (uint32)b;
}

// Disegna gli strati della stanza caricata in questo momento (dung_bg2 e
// dung_bg1, 64x64 caselle 8x8) con la grafica della VRAM e i colori pal
static void DrawLoadedRoom(uint32 *dst, const uint16 *pal, int bg2_prop) {
  Ppu *ppu = g_zenv.ppu;
  uint16 chr_base = ppu->bgLayer[1].tileAdr;
  uint32 back = HdFx_Bgr15ToRgb(pal[0]);
  // BG1 sopra BG2: opaco quando sta sullo schermo principale, a meta' quando
  // il gioco lo somma in trasparenza (come kSpiralTab1 in dungeon.c)
  bool bg1_half = bg2_prop == 1 || bg2_prop == 2 || bg2_prop >= 4;
  for (int layer = 0; layer < 2; layer++) {
    const uint16 *map = layer == 0 ? dung_bg2 : dung_bg1;
    for (int ty = 0; ty < 64; ty++) {
      for (int tx = 0; tx < 64; tx++) {
        uint16 e = map[ty * 64 + tx];
        int pl = e >> 10 & 7;
        bool hf = (e & 0x4000) != 0, vf = (e & 0x8000) != 0;
        const uint16 *t = &ppu->vram[(chr_base + (e & 0x3ff) * 16) & 0x7fff];
        for (int row = 0; row < 8; row++) {
          int rr = vf ? 7 - row : row;
          uint16 p01 = t[rr], p23 = t[rr + 8];
          uint32 *d = dst + (ty * 8 + row) * kRoomPx + tx * 8;
          for (int col = 0; col < 8; col++) {
            int v = HdFx_Pixel4bpp(p01, p23, hf ? col : 7 - col);
            if (layer == 0) {
              d[col] = v ? HdFx_Bgr15ToRgb(pal[pl * 16 + v]) : back;
            } else if (v) {
              uint32 c = HdFx_Bgr15ToRgb(pal[pl * 16 + v]);
              if (bg1_half) {
                uint32 o = d[col];
                c = Pack((((c >> 16) & 0xff) + ((o >> 16) & 0xff)) >> 1,
                         (((c >> 8) & 0xff) + ((o >> 8) & 0xff)) >> 1,
                         ((c & 0xff) + (o & 0xff)) >> 1);
              }
              d[col] = c;
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Oggetti fermi della stanza di sotto (sprite che non si muovono da soli):
// statue spostabili, interruttori di cristallo e a pressione, respingenti.
// Lampade, torce, vasi, scrigni e statue decorative sono gia' caselle dello
// sfondo. I nemici no: fermi in una posa sembrerebbero strani.

extern uint8 g_oam_real_f0[128];

static bool IsStillObject(uint8 type) {
  return type == 0x1c || type == 0x1e || type == 0x21 || type == 0x93;
}

static void DrawStillObject(int k) {
  switch (sprite_type[k]) {
  case 0x1c: MovableStatue_Draw(k); break;
  case 0x1e: SpriteDraw_SingleLarge(k); break;
  case 0x21: PushSwitch_Draw(k); break;
  case 0x93: Bumper_Draw(k); break;
  }
}

// Pezzi OAM scritti dal disegno di uno sprite, nell'immagine della stanza:
// cam = angolo della "telecamera" usata per il disegno, in coordinate della
// stanza. Dall'ultimo al primo: quelli prima stanno davanti (come la PPU)
static void PaintOam(uint32 *dst, const uint16 *pal, int cam_x, int cam_y) {
  Ppu *ppu = g_zenv.ppu;
  for (int i = 127; i >= 0; i--) {
    const OamEnt *o = &oam_buf[i];
    if (o->y == 0xf0)
      continue;
    uint8 ext = bytewise_extended_oam[i];
    int size = ext & 2 ? 16 : 8;
    int sx = o->x | (ext & 1) << 8;
    if (sx >= 256)
      sx -= 512;
    // il pezzo con y = n comincia sulla riga n + 1, come il fondale
    int x0 = cam_x + sx, y0 = cam_y + o->y + 1;
    int adr = (o->flags & 1) ? ppu->objTileAdr2 : ppu->objTileAdr1;
    int pbase = 0x80 + 16 * (o->flags >> 1 & 7);
    for (int r = 0; r < size; r++) {
      int y = y0 + r;
      if (y < 0 || y >= kRoomPx)
        continue;
      int rr = (o->flags & 0x80) ? size - 1 - r : r;
      for (int c = 0; c < size; c++) {
        int x = x0 + c;
        if (x < 0 || x >= kRoomPx)
          continue;
        int cc = (o->flags & 0x40) ? size - 1 - c : c;
        int tile = (((o->charnum >> 4) + (rr >> 3)) << 4) | (((o->charnum & 0xf) + (cc >> 3)) & 0xf);
        const uint16 *t = &ppu->vram[(adr + tile * 16 + (rr & 7)) & 0x7fff];
        int v = HdFx_Pixel4bpp(t[0], t[8], 7 - (cc & 7));
        if (v)
          dst[y * kRoomPx + x] = HdFx_Bgr15ToRgb(pal[pbase + v]);
      }
    }
  }
}

// Nella stanza appena caricata (dentro la copia di BuildImage): carica gli
// sprite come entrando, prepara e disegna solo gli oggetti fermi
static void DrawStillObjects(uint32 *dst, const uint16 *pal, int room) {
  static uint8 real_f0[128];
  memcpy(real_f0, g_oam_real_f0, sizeof(real_f0));  // fuori dalla RAM del gioco
  for (int k = 0; k < 16; k++)
    sprite_state[k] = 0;
  dungeon_room_index2 = room;
  Dungeon_LoadSprites();
  int base_x = (room & 15) * kRoomPx, base_y = (room >> 4) * kRoomPx;
  for (int k = 0; k < 16; k++) {
    if (sprite_state[k] != 8 || !IsStillObject(sprite_type[k]))
      continue;
    SpriteModule_Initialize(k);
    Sprite_Get16BitCoords(k);
    // telecamera attorno all'oggetto: le coordinate OAM restano piccole
    BG2HOFS_copy2 = cur_sprite_x - 128, BG2VOFS_copy2 = cur_sprite_y - 112;
    for (int i = 0; i < 128; i++)
      oam_buf[i].y = 0xf0;
    oam_cur_ptr = 0x800, oam_ext_cur_ptr = 0xa20;
    DrawStillObject(k);
    PaintOam(dst, pal, BG2HOFS_copy2 - base_x, BG2VOFS_copy2 - base_y);
  }
  memcpy(g_oam_real_f0, real_f0, sizeof(real_f0));
}

// Media mobile in orizzontale e in verticale, una volta: pixel entro il
// raggio con peso 1, quello appena oltre con la parte decimale del raggio
// (raggio 1,5: 0,5 1 1 1 0,5 su 4). La stanza resta riconoscibile
static void BoxBlur(uint32 *img, uint32 *tmp) {
  int ri = (int)kBlurRadius;
  int wf = (int)((kBlurRadius - ri) * 256 + 0.5f);  // peso dei pixel di bordo, su 256
  int reach = ri + (wf > 0), total = (2 * ri + 1) * 256 + 2 * wf;
  if (reach == 0)
    return;
  for (int pass = 0; pass < 2; pass++) {
    // passo 0: righe da img a tmp; passo 1: colonne da tmp a img
    const uint32 *src = pass ? tmp : img;
    uint32 *dst = pass ? img : tmp;
    int step = pass ? kRoomPx : 1;
    for (int y = 0; y < kRoomPx; y++) {
      for (int x = 0; x < kRoomPx; x++) {
        int pos = pass ? y : x;
        const uint32 *base = src + (pass ? x : y * kRoomPx);
        int r = 0, g = 0, b = 0;
        for (int k = -reach; k <= reach; k++) {
          int p = pos + k < 0 ? 0 : pos + k >= kRoomPx ? kRoomPx - 1 : pos + k;
          int w = (k < -ri || k > ri) ? wf : 256;
          uint32 c = base[p * step];
          r += (c >> 16 & 0xff) * w, g += (c >> 8 & 0xff) * w, b += (c & 0xff) * w;
        }
        dst[y * kRoomPx + x] = Pack(r / total, g / total, b / total);
      }
    }
  }
}

// Lontananza: piu' scura e un po' piu' fredda (aria in ombra)
static void Darken(uint32 *img) {
  for (int i = 0; i < kRoomPx * kRoomPx; i++) {
    uint32 c = img[i];
    int r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    int l = (r * 77 + g * 150 + b * 29) >> 8;
    // un quarto verso il grigio, poi circa due terzi della luce
    r = (r * 3 + l) >> 2, g = (g * 3 + l) >> 2, b = (b * 3 + l) >> 2;
    img[i] = Pack(r * 62 / 100, g * 64 / 100, b * 72 / 100 + 4);
  }
}

static bool BuildImage(uint32 *img, int dest) {
  Ppu *ppu = g_zenv.ppu;
  static uint16 pal[256];
  memcpy(g_saved_ram, g_ram, sizeof(g_saved_ram));
  memcpy(g_saved_vram, ppu->vram, sizeof(g_saved_vram));
  // palette 0 e 1 (HUD, usate anche da qualche casella) restano quelle di ora
  memcpy(pal, main_palette_buffer, sizeof(pal));

  // come entrando nella stanza (vedi anche ztest ZTEST_ROOMDUMP)
  dungeon_room_index = dest;
  g_building = true;
  Dungeon_LoadRoom();
  g_building = false;
  int bg2_prop = dung_hdr_bg2_properties;
  InitializeTilesets();
  overworld_palette_aux_or_main = 0;
  Palette_Load_DungeonSet();
  memcpy(pal + 0x20, aux_palette_buffer + 0x20, (0x80 - 0x20) * sizeof(uint16));
  DrawLoadedRoom(img, pal, bg2_prop);
  // palette sprite 5 e 6 della stanza di sotto (le altre sono comuni)
  Palette_Load_Sp5L();
  Palette_Load_Sp6L();
  memcpy(pal + 0xd1, aux_palette_buffer + 0xd1, 7 * sizeof(uint16));
  memcpy(pal + 0xe1, aux_palette_buffer + 0xe1, 7 * sizeof(uint16));
  DrawStillObjects(img, pal, dest);

  memcpy(g_ram, g_saved_ram, sizeof(g_saved_ram));
  memcpy(ppu->vram, g_saved_vram, sizeof(g_saved_vram));

  BoxBlur(img, g_tmp);
  Darken(img);
  return true;
}

// Stanza di sotto delle buche della stanza attuale, -1 se le buche non
// portano a un'altra stanza (o fanno solo danno)
static int PitDestination(void) {
  int dest = dung_hdr_travel_destinations[0];
  if (dest == 0 || dest == (dungeon_room_index & 0xff) || Dungeon_IsPitThatHurtsPlayer())
    return -1;
  return dest;
}

void HdPit_RoomLoaded(void) {
  if (!g_building)
    g_loaded_room = dungeon_room_index;
}

// Mappa delle buche della stanza attuale; falso se non ce ne sono. A gioco
// fermo dalla tabella degli attributi del gioco (esatta, anche con le buche
// che si aprono). Nei passaggi il gioco la ricostruisce un pezzo per
// fotogramma (Dungeon_LoadAttribute_Selectable) e per un po' contiene ancora
// la stanza di prima: allora la si ricava dalle caselle, come fa il gioco
// (Dungeon_LoadBasicAttribute_full), quando sono gia' della stanza nuova
static bool ReadPitMap(uint8 *pit, bool *ok) {
  bool any = false, stable = submodule_index == 0;
  *ok = stable || g_loaded_room == dungeon_room_index;
  if (!*ok)
    return false;
  for (int i = 0; i < 64 * 64; i++) {
    if (stable)
      pit[i] = dung_bg2_attr_table[i] == kAttrPit || dung_bg1_attr_table[i] == kAttrPit;
    else
      pit[i] = attributes_for_tile[dung_bg2[i] & 0x3ff] == kAttrPit ||
               attributes_for_tile[dung_bg1[i] & 0x3ff] == kAttrPit;
    any |= pit[i];
  }
  return any;
}

static inline bool IsPitCell(const PitRoom *pr, int cx, int cy) {
  return cx >= 0 && cy >= 0 && cx < 64 && cy < 64 && pr->pit[cy * 64 + cx];
}

// Casella interna della buca: anche le 8 vicine sono buca (niente bordo)
static bool IsInnerPitCell(const PitRoom *pr, int cx, int cy) {
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
      if (!IsPitCell(pr, cx + dx, cy + dy))
        return false;
  return true;
}

// Colore BGR555 quasi nero (al massimo 2 su 31 per canale)
static bool IsBlack15(uint16 c) {
  return (c & 31) <= 2 && (c >> 5 & 31) <= 2 && (c >> 10 & 31) <= 2;
}

// Colori del "vuoto" della stanza e buche in cui si vede la stanza di sotto.
// Solo le buche grandi, con almeno una casella interna (3x3 caselle, 24
// pixel): i fori piccoli sono passaggi (es. gli angoli della stanza 0x77 di
// Hera, 2x2 su BG1) e restano come nell'originale; con l'ombra dei bordi
// diventavano quadrati neri. I colori vengono dalle caselle interne di tutta
// la stanza (non solo quelle sullo schermo: niente cambi quando una buca entra
// dal bordo). Se sono tutti neri il gioco disegna il buio (es. Palazzo della
// Palude Nebbiosa, stanza 0x97, anche se sotto c'e' una stanza): allora la
// stanza di sotto non si mostra
static void ComputeHoles(PitRoom *pr) {
  Ppu *ppu = g_zenv.ppu;
  memset(pr->hole, 0, sizeof(pr->hole));
  memset(pr->show, 0, sizeof(pr->show));
  // buche come gruppi di caselle vicine (4 direzioni); grande = con una
  // casella interna
  static uint16 queue[64 * 64];
  static uint8 seen[64 * 64];
  memset(seen, 0, sizeof(seen));
  for (int start = 0; start < 64 * 64; start++) {
    if (!pr->pit[start] || seen[start])
      continue;
    int head = 0, tail = 0;
    bool big = false;
    queue[tail++] = start, seen[start] = 1;
    while (head < tail) {
      int i = queue[head++], cx = i & 63, cy = i >> 6;
      big |= IsInnerPitCell(pr, cx, cy);
      static const int8 kD[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
      for (int d = 0; d < 4; d++) {
        int x = cx + kD[d][0], y = cy + kD[d][1];
        if (x < 0 || y < 0 || x >= 64 || y >= 64)
          continue;
        int j = y * 64 + x;
        if (pr->pit[j] && !seen[j])
          seen[j] = 1, queue[tail++] = j;
      }
    }
    if (big)
      for (int q = 0; q < tail; q++)
        pr->show[queue[q]] = 1;
  }
  bool any = false;
  for (int n = 0; n < 2; n++) {
    const uint16 *map = n ? dung_bg2 : dung_bg1;
    uint16 chr = ppu->bgLayer[n].tileAdr;
    for (int cy = 0; cy < 64; cy++) {
      for (int cx = 0; cx < 64; cx++) {
        if (!IsInnerPitCell(pr, cx, cy))
          continue;
        uint16 e = map[cy * 64 + cx];
        const uint16 *t = &ppu->vram[(chr + (e & 0x3ff) * 16) & 0x7fff];
        for (int row = 0; row < 8; row++)
          for (int bit = 0; bit < 8; bit++) {
            int v = HdFx_Pixel4bpp(t[row], t[row + 8], bit);
            pr->hole[n][v ? (e >> 10 & 7) * 16 + v : 0] = 1, any = true;
          }
      }
    }
  }
  bool black = true;
  for (int n = 0; n < 2; n++)
    for (int c = 0; c < 128; c++)
      if (pr->hole[n][c] && !IsBlack15(main_palette_buffer[c]))
        black = false;
  // Correzioni minori GBA: nel Palazzo della Palude Nebbiosa (0x0E) il GBA
  // disegna il pavimento anche nelle buche nere (stanze 0x97 ecc.): qui si
  // vede la stanza di sotto come nelle altre buche; senza la correzione restano nere
  if (black && cur_palace_index_x2 == 0x0e && (enhanced_features0 & kFeatures0_GbaMinorFixes))
    black = false;
  pr->black = !any || black;
  pr->holes_known = true;
}

static PitRoom *FindRoom(int room) {
  for (int i = 0; i < kEntries; i++)
    if (g_rooms[i].room == room)
      return &g_rooms[i];
  return NULL;
}

static void ForgetAll(void) {
  for (int i = 0; i < kEntries; i++)
    g_rooms[i].room = -1, g_rooms[i].dest = -1;
}

static void SkyViewBeforeRender(void);

static bool SceneOn(void) {
  // labirinto (anche scorrimenti, scale e la dissolvenza della caduta) e
  // dialoghi; mai la mappa del mondo in Mode 7
  return g_fx_pit && player_is_indoors && g_zenv.ppu->mode == 1 &&
         (main_module_index == 7 || main_module_index == 14);
}

void HdPit_BeforeRender(void) {
  if (!g_fx_pit || !player_is_indoors) {
    ForgetAll();
    return;
  }
  // anche durante i passaggi (caduta, scale, scorrimenti): la stanza nuova
  // e' pronta prima che si veda. Se la si legge a meta' (attributi o
  // intestazione non ancora aggiornati) si rifa' al fotogramma dopo; i pixel
  // della stanza vecchia usano la sua voce, che qui non si tocca
  if (!SceneOn() || main_module_index != 7)
    return;
  SkyViewBeforeRender();
  int room = dungeon_room_index, dest = PitDestination();
  PitRoom *pr = FindRoom(room);
  // stanza nuova non ancora caricata: si aspetta (niente voce a meta')
  if (!pr && submodule_index != 0 && g_loaded_room != room)
    return;
  if (!pr) {
    pr = &g_rooms[0];
    for (int i = 1; i < kEntries; i++)
      if (g_rooms[i].last_use < pr->last_use)
        pr = &g_rooms[i];
    pr->room = room, pr->dest = -1, pr->holes_known = false;
  }
  pr->last_use = ++g_use;
  // le buche possono cambiare (pavimenti che crollano): mappa a ogni fotogramma
  bool ok;
  if (!ReadPitMap(pr->pit, &ok))
    dest = -1;
  if (!ok)
    return;
  // colori del vuoto a gioco fermo (nelle dissolvenze le palette cambiano)
  if (dest >= 0 && (submodule_index == 0 || !pr->holes_known))
    ComputeHoles(pr);
  if (pr->black)
    dest = -1;  // buche nere: niente stanza di sotto (e niente immagine)
  if (dest == pr->dest)
    return;
  pr->dest = -1;
  if (dest < 0)
    return;
  if (!pr->img)
    pr->img = malloc(kRoomPx * kRoomPx * sizeof(uint32));
  if (!g_tmp)
    g_tmp = malloc(kRoomPx * kRoomPx * sizeof(uint32));
  if (pr->img && g_tmp && BuildImage(pr->img, dest))
    pr->dest = dest;
}

// ---------------------------------------------------------------------------
// Viste dalle stanze sopra il mondo (con "Stanza sotto le buche")
//
// Le due stanze di Agahnim sono terrazze in cima alle torri: dove il gioco
// disegna il vuoto (fuori dalla stanza e nelle finestre fra le colonne) si
// vede il mondo sotto, appena sfocato e piu' lento della telecamera (e'
// lontano). Il punto del mondo e' quello scelto dall'utente con due
// salvataggi: il Santuario per il primo scontro, la cima del Monte della
// Morte oscuro per il secondo. Il vuoto sono i pixel col colore 15 della
// palette 6 (casella piena 0x0C9) e col colore 7 della palette 7 (parte vuota
// delle caselle diagonali delle pareti): il contorno nero delle pareti e' un
// altro colore (1) e resta.
typedef struct SkyView {
  uint16 room;
  uint8 world;      // 0 = mondo chiaro, 0x40 = mondo oscuro
  int x0, y0, w, h; // pezzo di mondo che si puo' vedere
  int cx, cy;       // punto del mondo al centro della vista (stanza al centro)
  float zoom;       // il mondo rimpicciolito: zoom volte piu' piccolo (< 1: ingrandito)
  uint32 normal;    // colore del vuoto fuori dai lampi (0xRRGGBB)
} SkyView;
// Primo Agahnim: la striscia fra il Monte della Morte (finisce a y 1035) e il
// castello (fossato da y 1470): ne' l'uno ne' l'altro si devono vedere, quindi
// rimpicciolita di 1,75 in modo che 240 righe di schermo coprano 420 pixel di
// mondo. Secondo Agahnim: solo il terreno scuro sopra il ponte (dal terrazzo
// il ponte non si vedrebbe), la zona scelta dall'utente; quel terreno e' gia'
// la montagna vista da lontano, quindi e' ingrandito di 2 (zoom 0,5) per
// riempire lo schermo
static const SkyView kSkyViews[] = {
  { 0x20, 0x00, 1402, 1040, 1280, 420, 2042, 1250, 1.75f, 0x5858f8 },  // il Santuario
  { 0x0d, 0x40, 2740, 48, 235, 136, 2857, 116, 0.5f, 0x000000 },        // cima del Monte della Morte oscuro
};
static const float kViewParallax = 0.25f;  // quanto la vista segue la telecamera
enum { kViewBlur = 2 };                    // sfocatura (raggio in pixel)
enum { kVoidColorA = 6 * 16 + 15, kVoidColorB = 7 * 16 + 7 };
static int g_view_room = -1, g_view_cx, g_view_cy;  // centro scelto, nell'immagine
static int g_view_w, g_view_h;                      // misure dell'immagine
static uint32 g_view_normal;                        // colore normale del vuoto
static uint32 *g_view_img;

static void BlurView(uint32 *img, uint32 *tmp, int w, int h) {
  const int r = kViewBlur, n = 2 * r + 1;
  for (int pass = 0; pass < 2; pass++) {
    const uint32 *src = pass ? tmp : img;
    uint32 *dst = pass ? img : tmp;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
          int qx = pass ? x : x + k, qy = pass ? y + k : y;
          qx = qx < 0 ? 0 : qx >= w ? w - 1 : qx;
          qy = qy < 0 ? 0 : qy >= h ? h - 1 : qy;
          uint32 c = src[qy * w + qx];
          sr += c >> 16 & 0xff, sg += c >> 8 & 0xff, sb += c & 0xff;
        }
        dst[y * w + x] = Pack(sr / n, sg / n, sb / n);
      }
  }
}

static const SkyView *FindSkyView(int room) {
  for (int i = 0; i < (int)(sizeof(kSkyViews) / sizeof(kSkyViews[0])); i++)
    if (kSkyViews[i].room == room)
      return &kSkyViews[i];
  return NULL;
}

// Prepara l'immagine entrando in una delle stanze (una volta): il pezzo di
// mondo disegnato e poi rimpicciolito (ogni pixel e' la media dei pixel di
// mondo che copre)
static void SkyViewBeforeRender(void) {
  int room = dungeon_room_index;
  const SkyView *v = FindSkyView(room);
  // nei passaggi solo a stanza nuova gia' caricata (come le buche)
  if (!v || room == g_view_room || (submodule_index != 0 && g_loaded_room != room))
    return;
  int w = (int)(v->w / v->zoom), h = (int)(v->h / v->zoom);
  uint32 *img = malloc((size_t)w * h * sizeof(uint32));
  uint32 *tmp = malloc((size_t)w * h * sizeof(uint32));
  uint32 *big = malloc((size_t)v->w * v->h * sizeof(uint32));
  if (img && tmp && big && HdWorld_DrawWorldRect(v->world, v->x0, v->y0, v->w, v->h, big)) {
    for (int y = 0; y < h; y++) {
      int sy0 = (int)(y * v->zoom), sy1 = IntMax(sy0 + 1, IntMin(v->h, (int)((y + 1) * v->zoom)));
      for (int x = 0; x < w; x++) {
        int sx0 = (int)(x * v->zoom), sx1 = IntMax(sx0 + 1, IntMin(v->w, (int)((x + 1) * v->zoom)));
        int sr = 0, sg = 0, sb = 0, n = 0;
        for (int sy = sy0; sy < sy1; sy++)
          for (int sx = sx0; sx < sx1; sx++, n++) {
            uint32 c = big[sy * v->w + sx];
            sr += c >> 16 & 0xff, sg += c >> 8 & 0xff, sb += c & 0xff;
          }
        img[y * w + x] = Pack(sr / n, sg / n, sb / n);
      }
    }
    BlurView(img, tmp, w, h);
    free(g_view_img);
    g_view_img = img, img = NULL;
    g_view_room = room, g_view_w = w, g_view_h = h, g_view_normal = v->normal;
    g_view_cx = (int)((v->cx - v->x0) / v->zoom), g_view_cy = (int)((v->cy - v->y0) / v->zoom);
  }
  free(img);
  free(big);
  free(tmp);
}

static int BgColorAt(int n, int x, int y, int ext, int *wx, int *wy);
static inline int PitLayer(uint32 p);

static void SkyViewApply(uint8 *pixels, int pitch, int width, int height) {
  if (g_view_room < 0 || !SceneOn())
    return;
  Ppu *ppu = g_zenv.ppu;
  int bright = ppu->brightness > 15 ? 15 : ppu->brightness;
  if (!bright)
    return;
  int ext = ppu->extraLeftRight;
  // centro della vista nel mondo e spostamento lento rispetto al centro stanza
  int vx = BG2HOFS_copy + ppu->bgBiasX + width / 2 - ext, vy = BG2VOFS_copy + ppu->bgBiasY + 1 + height / 2;
  int rcx = ((g_view_room & 15) << 9) + 256, rcy = ((g_view_room >> 4 & 15) << 9) + 256;
  int ox = g_view_cx - width / 2 + (int)((vx - rcx) * kViewParallax);
  int oy = g_view_cy - height / 2 + (int)((vy - rcy) * kViewParallax);
  // la vista resta dentro l'immagine (in cima al mondo non c'e' niente sopra)
  // Lampi (e dissolvenze): il gioco cambia il colore del vuoto nella
  // palette, nella seconda stanza dal nero al rosso per qualche fotogramma.
  // La differenza dal colore normale si aggiunge alla vista, che si accende
  // insieme al cielo come faceva il vuoto
  int delta[2][3];
  for (int i = 0; i < 2; i++) {
    uint32 cur = HdFx_Bgr15ToRgb(ppu->cgram[i ? kVoidColorB : kVoidColorA]);
    for (int ch = 0; ch < 3; ch++)
      delta[i][ch] = (int)(cur >> (16 - 8 * ch) & 0xff) - (int)(g_view_normal >> (16 - 8 * ch) & 0xff);
  }
  ox = ox > g_view_w - width ? g_view_w - width : ox, ox = ox < 0 ? 0 : ox;
  oy = oy > g_view_h - height ? g_view_h - height : oy, oy = oy < 0 ? 0 : oy;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      int n = PitLayer(row[x]);
      if (n > 1)
        continue;
      int wx, wy;
      int c = BgColorAt(n, x, y, ext, &wx, &wy) & 0x7f;
      if (c != kVoidColorA && c != kVoidColorB)
        continue;
      if ((wy >> 9 & 15) * 16 + (wx >> 9 & 15) != g_view_room)
        continue;
      int ix = x + ox, iy = y + oy;
      ix = ix < 0 ? 0 : ix >= g_view_w ? g_view_w - 1 : ix;
      iy = iy < 0 ? 0 : iy >= g_view_h ? g_view_h - 1 : iy;
      uint32 s = g_view_img[iy * g_view_w + ix];
      const int *d = delta[c == kVoidColorB];
      int r = (int)(s >> 16 & 0xff) + d[0], g = (int)(s >> 8 & 0xff) + d[1], b = (int)(s & 0xff) + d[2];
      r = r < 0 ? 0 : r > 255 ? 255 : r;
      g = g < 0 ? 0 : g > 255 ? 255 : g;
      b = b < 0 ? 0 : b > 255 ? 255 : b;
      // strato 3 (fondale, come il cielo della montagna): lo shader Four
      // Swords non lo dipinge
      row[x] = 3u << 24 | Pack(r * bright / 15, g * bright / 15, b * bright / 15);
    }
  }
}

// Punto del mondo dello strato n sotto il pixel (x, y) dello schermo
static inline void WorldAt(int n, int x, int y, int ext, int *wx, int *wy) {
  Ppu *ppu = g_zenv.ppu;
  int hofs = n ? BG2HOFS_copy : BG1HOFS_copy, vofs = n ? BG2VOFS_copy : BG1VOFS_copy;
  // la riga 0 dello schermo e' la riga 1 del fondale (come HdWorld_Composite)
  *wx = (x - ext + hofs + ppu->bgBiasX) & 0xffff, *wy = (y + 1 + vofs + ppu->bgBiasY) & 0xffff;
}

// Punto del fondale (strato n) sotto il pixel (x, y) dello schermo, colore
// (palette*16 + indice, 0 se trasparente) come lo disegna la PPU
static int BgColorAt(int n, int x, int y, int ext, int *wx, int *wy) {
  Ppu *ppu = g_zenv.ppu;
  const BgLayer *bg = &ppu->bgLayer[n];
  int hofs = n ? BG2HOFS_copy : BG1HOFS_copy, vofs = n ? BG2VOFS_copy : BG1VOFS_copy;
  // la riga 0 dello schermo e' la riga 1 del fondale (come HdWorld_Composite)
  int px = (x - ext + hofs + ppu->bgBiasX) & 0xffff, py = (y + 1 + vofs + ppu->bgBiasY) & 0xffff;
  *wx = px, *wy = py;
  px &= 0x3ff, py &= 0x3ff;
  int tx = px >> 3 & 63, ty = py >> 3 & 63;
  uint16 adr = HdFx_VramMapAdr(bg, tx, ty);
  uint16 e = ppu->vram[adr & 0x7fff];
  int row = (e & 0x8000) ? 7 - (py & 7) : (py & 7);
  int bit = (e & 0x4000) ? (px & 7) : 7 - (px & 7);
  const uint16 *t = &ppu->vram[(bg->tileAdr + (e & 0x3ff) * 16) & 0x7fff];
  int v = HdFx_Pixel4bpp(t[row], t[row + 8], bit);
  return v ? (e >> 10 & 7) * 16 + v : 0;
}

// Strato del pixel per le buche: l'interno delle buche e' spesso trasparente
// e la PPU mostra il colore di fondo (5): vale come pavimento (BG2), colore 0
static inline int PitLayer(uint32 p) {
  int n = p >> 24 & 0xf;
  return n == 5 ? 1 : n;
}

// Stanza del punto del mondo (512x512 pixel per stanza) con le sue buche
static PitRoom *RoomAt(int wx, int wy) {
  int room = (wy >> 9) * 16 + (wx >> 9);
  for (int i = 0; i < kEntries; i++)
    if (g_rooms[i].room == room)
      return g_rooms[i].dest >= 0 && g_rooms[i].holes_known && !g_rooms[i].black ? &g_rooms[i] : NULL;
  return NULL;
}

// Distanza in pixel (fino a lim) dal bordo della buca nella direzione (dx, dy)
static int EdgeDist(const PitRoom *pr, int lx, int ly, int dx, int dy, int lim) {
  int cx = lx >> 3, cy = ly >> 3;
  // pixel fino al bordo della casella in quella direzione
  int d = dx > 0 ? 7 - (lx & 7) : dx < 0 ? (lx & 7) : dy > 0 ? 7 - (ly & 7) : (ly & 7);
  while (d < lim) {
    cx += dx, cy += dy;
    if (!IsPitCell(pr, cx, cy))
      return d;
    d += 8;
  }
  return lim;
}

static inline float Clamp01(float v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

// Correzioni minori GBA: nel Palazzo della Palude Nebbiosa la casella vuota
// 0x1EC di BG1 (sotto le buche e sotto la grata del pavimento) lascia vedere
// il fondale nero: errore dello SNES. Il GBA ci mette uno strato con un
// intreccio 8x8 col colore 4/10 della palette (0x2545 nella Palude, uguale al
// GBA) che scorre a meta' velocita' della telecamera (misurato con mGBA: per
// 16-20 pixel di scorrimento l'intreccio si sposta della meta'), cosi' sembra
// seguire Link. Casella presa dalla cattura del GBA (stanza 0x97); la fase e'
// quella della cattura. Si guarda la casella di BG1 nella mappa in VRAM (giusto
// anche nei passaggi e nelle dissolvenze). Con "Stanza sotto le buche" al
// posto dell'intreccio si vede la stanza di sotto in tutto il vuoto (buchi
// della grata compresi, piu' scura); nelle buche la disegna poi HdPit_Apply
// con l'ombra dei bordi
static const uint8 kGbaMireFloor[8] = { 0xd9, 0xe6, 0x67, 0x9b, 0xb9, 0x76, 0x6e, 0x9d };
enum { kMireVoidTile = 0x1ec };
static const float kMireBelowShade = 0.45f;
// Scia come lo schermo del GBA: l'intreccio si mescola con le sue ultime
// kMireTrail posizioni (una per fotogramma del gioco). Grata e intreccio hanno
// buchi di 8 pixel e scorrono a velocita' diverse: senza scia il vuoto passa
// da verde a nero ogni 2-3 fotogrammi (uguale al GBA, misurato: 17-50% di nero,
// ma li' lo schermo lo attenua). Da fermo e' identico al GBA
enum { kMireTrail = 4 };
static int g_mire_hx[kMireTrail], g_mire_hy[kMireTrail], g_mire_frame = -1;

static void GbaMireFloor(uint8 *pixels, int pitch, int width, int height) {
  Ppu *ppu = g_zenv.ppu;
  if (!(enhanced_features0 & kFeatures0_GbaMinorFixes) || !player_is_indoors ||
      cur_palace_index_x2 != 0x0e || ppu->mode != 1 || ppu->forcedBlank ||
      (main_module_index != 7 && main_module_index != 14))
    return;
  int bright = ppu->brightness > 15 ? 15 : ppu->brightness;
  if (!bright)
    return;
  uint32 c = HdFx_Bgr15ToRgb(ppu->cgram[0x4a]);
  int r = (c >> 16 & 0xff) * bright / 15, g = (c >> 8 & 0xff) * bright / 15, b = (c & 0xff) * bright / 15;
  int ext = ppu->extraLeftRight;
  const BgLayer *bg = &ppu->bgLayer[0];
  // strato a meta' velocita' della telecamera
  int half_x = (BG2HOFS_copy + ppu->bgBiasX) >> 1, half_y = (BG2VOFS_copy + ppu->bgBiasY) >> 1;
  // posizioni degli ultimi fotogrammi (all'inizio o dopo un salto: tutte uguali)
  if (g_mire_frame < 0 || abs(half_x - g_mire_hx[0]) > 16 || abs(half_y - g_mire_hy[0]) > 16) {
    for (int i = 0; i < kMireTrail; i++)
      g_mire_hx[i] = half_x, g_mire_hy[i] = half_y;
  } else if (g_mire_frame != frame_counter) {
    memmove(g_mire_hx + 1, g_mire_hx, (kMireTrail - 1) * sizeof(int));
    memmove(g_mire_hy + 1, g_mire_hy, (kMireTrail - 1) * sizeof(int));
    g_mire_hx[0] = half_x, g_mire_hy[0] = half_y;
  }
  g_mire_frame = frame_counter;
  // colore per 0..kMireTrail volte "verde" nelle posizioni della scia
  uint32 shade[kMireTrail + 1];
  for (int i = 0; i <= kMireTrail; i++)
    shade[i] = Pack(r * i / kMireTrail, g * i / kMireTrail, b * i / kMireTrail);
  // stanza di sotto: stessa profondita' di HdPit_Apply
  bool below = SceneOn();
  int cxw = BG2HOFS_copy + ppu->bgBiasX + 128;
  int cyw = BG2VOFS_copy + ppu->bgBiasY + 1 + height / 2;
  float k = kMireBelowShade * bright / 15.0f;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      if ((row[x] >> 24 & 0xf) != 5)
        continue;
      int wx, wy;
      WorldAt(0, x, y, ext, &wx, &wy);
      int px = wx & 0x3ff, py = wy & 0x3ff, tx = px >> 3 & 63, ty = py >> 3 & 63;
      uint16 adr = HdFx_VramMapAdr(bg, tx, ty);
      if ((ppu->vram[adr & 0x7fff] & 0x3ff) != kMireVoidTile)
        continue;
      if (below) {
        int bx, by;
        WorldAt(1, x, y, ext, &bx, &by);
        PitRoom *pr = RoomAt(bx, by);
        if (pr) {
          int lx = bx & 0x1ff, ly = by & 0x1ff;
          int sx = lx - (int)((bx - cxw) * kDepth), sy = ly - (int)((by - cyw) * kDepth);
          sx = sx < 0 ? 0 : sx >= kRoomPx ? kRoomPx - 1 : sx;
          sy = sy < 0 ? 0 : sy >= kRoomPx ? kRoomPx - 1 : sy;
          uint32 s = pr->img[sy * kRoomPx + sx];
          row[x] = (row[x] & 0xff000000) |
                   Pack((int)((s >> 16 & 0xff) * k), (int)((s >> 8 & 0xff) * k), (int)((s & 0xff) * k));
          continue;
        }
      }
      int n = 0;
      for (int i = 0; i < kMireTrail; i++) {
        int qx = x - ext + g_mire_hx[i], qy = y + 1 + g_mire_hy[i];
        n += kGbaMireFloor[qy & 7] >> (7 - (qx & 7)) & 1;
      }
      if (n)
        row[x] = (row[x] & 0xff000000) | shade[n];
    }
  }
}

void HdPit_Apply(uint8 *pixels, int pitch, int width, int height) {
  GbaMireFloor(pixels, pitch, width, height);
  SkyViewApply(pixels, pitch, width, height);
  if (!SceneOn() || (g_rooms[0].dest < 0 && g_rooms[1].dest < 0))
    return;
  Ppu *ppu = g_zenv.ppu;
  int ext = ppu->extraLeftRight;
  // dissolvenze e luminosita' dello schermo come i pixel veri
  int bright = ppu->brightness > 15 ? 15 : ppu->brightness;
  int cxw = BG2HOFS_copy + ppu->bgBiasX + 128;
  int cyw = BG2VOFS_copy + ppu->bgBiasY + 1 + height / 2;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      int n = PitLayer(row[x]);
      if (n > 1)
        continue;
      int wx, wy;
      WorldAt(n, x, y, ext, &wx, &wy);
      PitRoom *pr = RoomAt(wx, wy);
      int lx = wx & 0x1ff, ly = wy & 0x1ff;
      if (!pr || !pr->show[(ly >> 3) * 64 + (lx >> 3)])
        continue;
      if (!pr->hole[n][BgColorAt(n, x, y, ext, &wx, &wy) & 0x7f])
        continue;
      // profondita': il punto della stanza di sotto si avvicina al centro
      int dx = wx - cxw, dy = wy - cyw;
      int sx = lx - (int)(dx * kDepth), sy = ly - (int)(dy * kDepth);
      sx = sx < 0 ? 0 : sx >= kRoomPx ? kRoomPx - 1 : sx;
      sy = sy < 0 ? 0 : sy >= kRoomPx ? kRoomPx - 1 : sy;
      uint32 s = pr->img[sy * kRoomPx + sx];
      // ombra dei bordi: forte sotto il bordo in alto, leggera ai lati
      float top = Clamp01(EdgeDist(pr, lx, ly, 0, -1, 32) / kShadeTop);
      float side = Clamp01(EdgeDist(pr, lx, ly, -1, 0, 16) / kShadeSide) *
                   Clamp01(EdgeDist(pr, lx, ly, 1, 0, 16) / kShadeSide) *
                   Clamp01((EdgeDist(pr, lx, ly, 0, 1, 16) + 4) / kShadeSide);
      float k = (0.30f + 0.70f * top) * (0.60f + 0.40f * side) * bright / 15.0f;
      int r = (int)((s >> 16 & 0xff) * k), g = (int)((s >> 8 & 0xff) * k), b = (int)((s & 0xff) * k);
      row[x] = (row[x] & 0xff000000) | Pack(r, g, b);
    }
  }
}
