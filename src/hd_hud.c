// HUD moderno (opzione ModernHud).
//
// L'HUD originale e' una piccola mappa di caselle dello strato 3 che il gioco
// carica in memoria video: durante il gioco le caselle che coincidono ancora
// con l'HUD vengono svuotate (quelle che un dialogo ci ha scritto sopra
// restano), e i pezzi vengono ridisegnati qui con la loro grafica originale
// (cuori, rupie, chiavi, cifre, icone degli oggetti) in una nuova disposizione.
// I pixel sono marcati come HUD (alpha 2): lo shader non li tocca.
#include <math.h>
#include <string.h>
#include "hd_hud.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "config.h"
#include "hud.h"
#include "snes/ppu.h"
#include "player.h"

const uint16 *Hud_GetItemIconTiles(int item);
const uint16 *Hud_GetSwordIconTiles(void);


enum {
  kAction_None, kAction_Talk, kAction_Read, kAction_Open, kAction_Lift,
  kAction_Grab, kAction_Pull, kAction_Throw, kAction_Dash, kAction_Swim,
};

static bool g_hud_was_active;
static uint8 g_talk_frame = 0xff;  // ultimo frame in cui si poteva parlare

static bool ModernHudOn(void) {
  return (enhanced_features0 & kFeatures0_ModernHud) != 0;
}

// Durante il gioco e nei dialoghi (non nei menu, sulla mappa o nei titoli)
static bool HudContext(bool *in_dialog) {
  int mod = main_module_index;
  *in_dialog = false;
  // 15 e 16: il cerchio che si chiude e si riapre entrando e uscendo
  if (mod == 7 || mod == 9 || mod == 15 || mod == 16)
    return true;
  if (mod == 14 && submodule_index == 2 && (saved_module_for_menu == 7 || saved_module_for_menu == 9)) {
    *in_dialog = true;
    return true;
  }
  return false;
}

// Inventario e menu di salvataggio aperti durante il gioco: l'HUD originale
// scorrerebbe insieme ai riquadri, quindi resta nascosto anche li'
static bool InGameMenu(void) {
  return main_module_index == 14 && (submodule_index == 1 || submodule_index == 11) &&
         (saved_module_for_menu == 7 || saved_module_for_menu == 9);
}

void HdHud_BeforeRender(Ppu *ppu) {
  bool dlg;
  bool active = ModernHudOn() && (HudContext(&dlg) || InGameMenu());
  uint16 *v = &ppu->vram[word_7E0219 & 0x7fff];
  if (active) {
    for (int i = 0; i < 165 && (word_7E0219 & 0x7fff) + i < 0x8000; i++)
      if (hud_tile_indices_buffer[i] != 0x207f && v[i] == hud_tile_indices_buffer[i])
        v[i] = 0x207f;
  } else if (g_hud_was_active && !ModernHudOn()) {
    flag_update_hud_in_nmi = 1;  // opzione appena spenta: il gioco ricarica l'HUD originale
  }
  g_hud_was_active = active;
}

void HdHud_CanTalk(void) {
  g_talk_frame = frame_counter;
}

// ---------------------------------------------------------------------------
// Azione del tasto A, calcolata senza cambiare lo stato del gioco

static int ComputeAction(void) {
  if (main_module_index != 7 && main_module_index != 9)
    return kAction_None;
  if (link_player_handler_state == kPlayerState_Swimming)
    return link_item_flippers ? kAction_Swim : kAction_None;
  if (link_state_bits & 0x80)
    return kAction_Throw;  // tiene qualcosa sopra la testa
  if (link_grabbing_wall)
    return kAction_Pull;
  if (link_item_in_hand || (link_position_mode & 0x1f) || link_incapacitated_timer)
    return kAction_None;
  if (g_talk_frame == frame_counter || (uint8)(frame_counter - g_talk_frame) == 1)
    return kAction_Talk;
  if (link_need_for_pullforrupees_sprite && !link_direction_facing)
    return kAction_Pull;
  if (link_is_near_moveable_statue)
    return kAction_Grab;
  if (flag_is_sprite_to_pick_up || flag_is_ancilla_to_pick_up)
    return (link_ability_flags & 0x40) ? kAction_Lift : kAction_None;

  // Stessa scelta che fa il gioco alla pressione di A, su una copia della RAM
  static uint8 saved[0x20000];
  memcpy(saved, g_ram, sizeof(saved));
  uint8 action = Link_HandleLiftables();
  memcpy(g_ram, saved, sizeof(saved));
  static const uint8 kAbility[] = { 0xE0, 0x40, 4, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0 };
  if (action > 7 || !(kAbility[action] & link_ability_flags))
    return kAction_None;
  switch (action) {
  case 0: return kAction_Read;   // preghiera davanti alla tavoletta
  case 1: return kAction_Lift;
  case 2: return link_item_boots ? kAction_Dash : kAction_None;
  case 3: return kAction_Grab;
  case 4: return kAction_Read;
  case 5: return kAction_Open;
  }
  return kAction_None;
}

// Azione mostrata: calcolata una volta per frame di gioco, cambia solo dopo
// che la nuova azione e' rimasta uguale per alcuni frame (niente lampeggi
// quando il controllo alterna per un attimo fra due risultati)
static int StableAction(void) {
  static uint8 last_frame = 0xff;
  static int shown = kAction_None, candidate = kAction_None, count;
  if (last_frame == frame_counter)
    return shown;
  last_frame = frame_counter;
  // Solo con Link nel normale controllo: durante transizioni fra schermate,
  // caricamenti e scene la scritta resta quella di prima
  if (submodule_index != 0) {
    candidate = shown, count = 0;
    return shown;
  }
  int a = ComputeAction();
  if (a == shown) {
    candidate = a, count = 0;
  } else if (a == candidate) {
    if (++count >= 6)
      shown = a, count = 0;
  } else {
    candidate = a, count = 1;
  }
  return shown;
}

static const char *ActionLabel(int a) {
  static const char *const kIt[] = { "", "PARLA", "LEGGI", "APRI", "SOLLEVA", "AFFERRA", "TIRA", "LANCIA", "CORRI", "NUOTA" };
  static const char *const kEn[] = { "", "TALK", "READ", "OPEN", "LIFT", "GRAB", "PULL", "THROW", "DASH", "SWIM" };
  bool it = g_config.language && (g_config.language[0] == 'i' || g_config.language[0] == 'I');
  return (it ? kIt : kEn)[a];
}

// ---------------------------------------------------------------------------
// Disegno

typedef struct Canvas {
  uint8 *pixels;
  int pitch, width, height, scale;
  int oy;  // spostamento verticale (HUD che scivola fuori dallo schermo)
} Canvas;

static void Px(Canvas *cv, int x, int y, uint32 color) {
  y += cv->oy;
  if ((unsigned)x >= (unsigned)cv->width || (unsigned)y >= (unsigned)cv->height)
    return;
  int s = cv->scale;
  for (int yy = 0; yy < s; yy++) {
    uint32 *row = (uint32 *)(cv->pixels + (y * s + yy) * cv->pitch);
    for (int xx = 0; xx < s; xx++)
      row[x * s + xx] = (color & 0xffffff) | 0x02000000;  // alpha 2 = HUD
  }
}

static uint32 Bgr15ToRgb(uint16 c) {
  uint32 r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
  return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

// Una casella 8x8 dello strato 3 (2 bit per pixel) come la disegnerebbe la PPU
static void DrawTile(Canvas *cv, int x, int y, uint16 entry) {
  Ppu *ppu = g_zenv.ppu;
  int tile = entry & 0x3ff, pal = entry >> 10 & 7;
  bool hf = entry & 0x4000, vf = entry & 0x8000;
  uint16 base = ppu->bgLayer[2].tileAdr + tile * 8;
  for (int r = 0; r < 8; r++) {
    uint16 w = ppu->vram[(base + (vf ? 7 - r : r)) & 0x7fff];
    for (int c = 0; c < 8; c++) {
      int bit = hf ? c : 7 - c;
      int v = (w >> bit & 1) | (w >> (8 + bit) & 1) << 1;
      if (v)
        Px(cv, x + c, y + r, Bgr15ToRgb(ppu->cgram[pal * 4 + v]));
    }
  }
}

// Una casella 8x8 decodificata nei colori (0 = trasparente)
static void DecodeTile(uint16 entry, uint32 *out, int stride) {
  Ppu *ppu = g_zenv.ppu;
  int tile = entry & 0x3ff, pal = entry >> 10 & 7;
  bool hf = entry & 0x4000, vf = entry & 0x8000;
  uint16 base = ppu->bgLayer[2].tileAdr + tile * 8;
  for (int r = 0; r < 8; r++) {
    uint16 w = ppu->vram[(base + (vf ? 7 - r : r)) & 0x7fff];
    for (int c = 0; c < 8; c++) {
      int bit = hf ? c : 7 - c;
      int v = (w >> bit & 1) | (w >> (8 + bit) & 1) << 1;
      out[r * stride + c] = v ? (Bgr15ToRgb(ppu->cgram[pal * 4 + v]) | 0xff000000) : 0;
    }
  }
}

// Icona 16x16 di un oggetto (caselle: su-sx, su-dx, giu'-sx, giu'-dx). Il nero
// e' lo sfondo del riquadro dell'HUD: diventa tutto trasparente, anche quello
// chiuso dentro l'icona (per esempio fra l'arco e la corda o nel vetro della
// lanterna), e solo all'esterno dell'icona si aggiunge un contorno nero di 1
// pixel che la tiene leggibile: i buchi interni restano trasparenti.
static void DrawIcon(Canvas *cv, int x, int y, const uint16 *t) {
  uint32 img[16 * 16];
  DecodeTile(t[0], img, 16);
  DecodeTile(t[1], img + 8, 16);
  DecodeTile(t[2], img + 8 * 16, 16);
  DecodeTile(t[3], img + 8 * 16 + 8, 16);
  for (int i = 0; i < 256; i++) {
    uint32 c = img[i];
    if (((c >> 16 & 0xff) + (c >> 8 & 0xff) + (c & 0xff)) < 40)
      img[i] = 0;
  }
  // esterno: pixel trasparenti raggiungibili dal bordo (griglia 18x18 con cornice)
  // (ogni pixel entra nella pila una sola volta: viene segnato quando ci entra)
  uint8 outside[18 * 18] = { 0 };
  int stack[18 * 18], n = 0;
  outside[0] = 1, stack[n++] = 0;
  while (n) {
    int i = stack[--n];
    int nb[4] = { i >= 18 ? i - 18 : -1, i < 17 * 18 ? i + 18 : -1,
                  i % 18 > 0 ? i - 1 : -1, i % 18 < 17 ? i + 1 : -1 };
    for (int k = 0; k < 4; k++) {
      int j = nb[k];
      if (j < 0 || outside[j])
        continue;
      int r = j / 18 - 1, c = j % 18 - 1;
      if (r >= 0 && r < 16 && c >= 0 && c < 16 && img[r * 16 + c])
        continue;
      outside[j] = 1, stack[n++] = j;
    }
  }
  for (int r = -1; r <= 16; r++)
    for (int c = -1; c <= 16; c++) {
      if (!outside[(r + 1) * 18 + (c + 1)])
        continue;
      bool near = false;
      for (int k = 0; k < 4 && !near; k++) {
        int rr = r + (k == 0) - (k == 1), cc = c + (k == 2) - (k == 3);
        near = rr >= 0 && rr < 16 && cc >= 0 && cc < 16 && img[rr * 16 + cc];
      }
      if (near)
        Px(cv, x + c, y + r, 0x101010);
    }
  for (int i = 0; i < 256; i++)
    if (img[i])
      Px(cv, x + i % 16, y + i / 16, img[i]);
}

// Font 3x5 per le scritte e i numeri piccoli (righe dall'alto, 3 bit)
static const uint8 *Glyph(char c) {
  static const uint8 kLetters[26][5] = {
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4},
    {3,4,5,5,3}, {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7},
    {5,7,7,5,5}, {6,5,5,5,5}, {2,5,5,5,2}, {6,5,6,4,4}, {2,5,5,6,3}, {6,5,6,5,5},
    {3,4,2,1,6}, {7,2,2,2,2}, {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5},
    {5,5,2,2,2}, {7,1,2,4,7},
  };
  static const uint8 kDigits[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {6,1,2,4,7}, {6,1,2,1,6}, {5,5,7,1,1},
    {7,4,6,1,6}, {3,4,7,5,7}, {7,1,2,2,2}, {7,5,7,5,7}, {7,5,7,1,6},
  };
  static const uint8 kSlash[5] = { 1, 1, 2, 4, 4 };
  if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
  if (c >= '0' && c <= '9') return kDigits[c - '0'];
  if (c == '/') return kSlash;
  return NULL;
}

static int SmallTextWidth(const char *s) {
  int n = (int)strlen(s);
  return n ? n * 4 - 1 : 0;
}

// Testo chiaro con contorno scuro (alto 5 pixel piu' il contorno)
static void DrawSmallText(Canvas *cv, int x, int y, const char *s, uint32 color) {
  for (int pass = 0; pass < 2; pass++) {
    int cx = x;
    for (const char *p = s; *p; p++, cx += 4) {
      const uint8 *g = Glyph(*p);
      if (!g)
        continue;
      for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++) {
          if (!(g[r] >> (2 - c) & 1))
            continue;
          if (pass == 0) {
            for (int dy = -1; dy <= 1; dy++)
              for (int dx = -1; dx <= 1; dx++)
                Px(cv, cx + c + dx, y + r + dy, 0x101010);
          } else {
            Px(cv, cx + c, y + r, color);
          }
        }
    }
  }
}

// "1/2" della barra della magia: caratteri 4x6, alti quanto l'interno della
// barra (il contorno scuro cade sul bordo esterno)
enum { kMagicTextW = 3 * 4 + 2 };

static void DrawMagicText(Canvas *cv, int x, int y) {
  static const uint8 kGlyphs[3][6] = {
    { 2, 6, 2, 2, 2, 7 },     // 1
    { 1, 1, 2, 4, 8, 8 },     // /
    { 6, 9, 1, 2, 4, 15 },    // 2
  };
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < 3; i++)
      for (int r = 0; r < 6; r++)
        for (int c = 0; c < 4; c++) {
          if (!(kGlyphs[i][r] >> (3 - c) & 1))
            continue;
          int px = x + i * 5 + c, py = y + r;
          if (pass == 0) {
            for (int dy = -1; dy <= 1; dy++)
              for (int dx = -1; dx <= 1; dx++)
                Px(cv, px + dx, py + dy, 0x101010);
          } else {
            Px(cv, px, py, 0xf8f8f8);
          }
        }
}

#include "hd_hud_buttons.h"

// I tre tasti, disposti come li ha disegnati l'utente (pulsanti.png)
static void DrawButtons(Canvas *cv, int x, int y) {
  for (int r = 0; r < HD_BUTTONS_H; r++)
    for (int c = 0; c < HD_BUTTONS_W; c++) {
      uint32 v = kHdButtons[r * HD_BUTTONS_W + c];
      if (v >> 24)
        Px(cv, x + c, y + r, v);
    }
}

// Primo pixel visibile (riga e colonna) di una casella: serve ad allineare i
// tasti esattamente al cuore piu' in alto a sinistra
static void TileFirstPixel(uint16 entry, int *row, int *col) {
  uint32 img[64];
  DecodeTile(entry, img, 8);
  *row = 8, *col = 8;
  for (int i = 0; i < 64; i++)
    if (img[i]) {
      if (i / 8 < *row) *row = i / 8;
      if (i % 8 < *col) *col = i % 8;
    }
  if (*row == 8) *row = 0, *col = 0;
}

static void DrawNumberTiles(Canvas *cv, int x, int y, int value, int digits) {
  char buf[8];
  int n = 0;
  do {
    buf[n++] = value % 10;
    value /= 10;
  } while (value && n < 7);
  while (n < digits)
    buf[n++] = 0;
  for (int i = n - 1; i >= 0; i--, x += 8)
    DrawTile(cv, x, y, 0x2490 + buf[i]);
}

void HdHud_Draw(uint8 *pixels, int pitch, int width, int height, int scale) {
  bool in_dialog;
  if (!ModernHudOn() || !HudContext(&in_dialog))
    return;
  Ppu *ppu = g_zenv.ppu;
  if (!(ppu->screenEnabled[0] & 4) || ppu->mode == 7)
    return;  // il gioco stesso non mostra l'HUD (scene, mappa...)
  // Nei dialoghi l'HUD scivola fuori dallo schermo (in alto i cuori e i
  // tasti, in basso rupie e chiavi) e rientra quando il dialogo finisce
  static float slide;
  static uint8 last_frame;
  if (last_frame != frame_counter) {
    last_frame = frame_counter;
    float target = in_dialog ? 1.0f : 0.0f;
    slide += (target > slide ? 1.0f : -1.0f) / 10.0f;
    if (slide < 0) slide = 0;
    if (slide > 1) slide = 1;
  }
  float ease = slide * slide * (3 - 2 * slide);
  if (ease >= 1.0f)
    return;
  Canvas cv = { pixels, pitch, width / scale, height / scale, scale, -(int)(ease * 48 + 0.5f) };
  Canvas cvb = cv;
  cvb.oy = (int)(ease * 32 + 0.5f);
  int W = cv.width, H = cv.height;

  // --- Cuori (caselle originali 8x8, 10 per riga) ------------------------
  const int x0 = 8, y0 = 8;
  int hearts = link_health_capacity >> 3;
  int hp = (link_health_current + 3) & ~3;
  // Cuori che si riempiono (fata, pozione, cuore preso): come nell'HUD
  // originale (Hud_AnimateHeartRefill), il cuore appena aggiunto lampeggia
  // con i tile 0x24A3, 0x24A4, 0x24A3 prima di restare pieno; fino al primo
  // lampo mostra ancora com'era prima
  int anim_heart = is_doing_heart_animation ? ((link_health_current & ~7) - 1) >> 3 : -1;
  for (int i = 0; i < hearts; i++) {
    int left = hp - i * 8;
    uint16 t = left >= 8 ? 0x24A0 : left >= 4 ? 0x24A1 : 0x24A2;
    if (i == anim_heart) {
      static const uint16 kAnimHeartPartial[4] = { 0x24A3, 0x24A4, 0x24A3, 0x24A0 };
      int sub = animate_heart_refill_countdown_subpos;
      if (sub)
        t = kAnimHeartPartial[sub - 1];
      else
        t = left - 8 >= 8 ? 0x24A0 : left - 8 >= 4 ? 0x24A1 : 0x24A2;
    }
    DrawTile(&cv, x0 + (i % 10) * 8, y0 + (i / 10) * 8, t);
  }
  int rows = hearts > 10 ? 2 : 1;
  // margine vero dei cuori: primo pixel visibile della casella
  int heart_row, heart_col;
  TileFirstPixel(0x24A0, &heart_row, &heart_col);
  int top = y0 + heart_row, margin = x0 + heart_col;

  // --- Magia: barra larga quanto 10 cuori, alta 8, con 1/2 o 1/4 al centro
  {
    int bx = x0, by = y0 + rows * 8 + 2, bw = 79, bh = 8;
    int iw = bw - 4, fill = (link_magic_power * iw + 0x7f) / 0x80;
    if (fill > iw) fill = iw;
    for (int y = 0; y < bh; y++)
      for (int x = 0; x < bw; x++) {
        uint32 col;
        bool outer = y == 0 || y == bh - 1 || x == 0 || x == bw - 1;
        bool inner = y == 1 || y == bh - 2 || x == 1 || x == bw - 2;
        if (outer)
          col = 0xd8d8d8;
        else if (inner)
          col = 0x585858;
        else if (x - 2 < fill)
          col = y == bh - 3 ? 0x00a800 : 0x00d000;
        else
          col = 0x100c08;
        Px(&cv, bx + x, by + y, col);
      }
    // come il gioco originale: "1/2" anche con il consumo a 1/4
    if (link_magic_consumption >= 1)
      DrawMagicText(&cv, bx + (bw - kMagicTextW) / 2, by + 1);
  }

  // --- Tasti: il punto piu' alto a filo con i cuori, il piu' a destra con
  // lo stesso margine dei cuori a sinistra
  const int S = HD_BUTTON_SIZE;
  int gx = W - margin - HD_BUTTONS_W, gy = top;
  DrawButtons(&cv, gx, gy);
  int yx = gx + HD_BUTTON_Y_X, yy = gy + HD_BUTTON_Y_Y;
  int ax = gx + HD_BUTTON_A_X, ay = gy + HD_BUTTON_A_Y;
  int bx = gx + HD_BUTTON_B_X, by = gy + HD_BUTTON_B_Y;

  // Y: oggetto selezionato, con il numero se e' un consumabile
  if (hud_cur_item) {
    DrawIcon(&cv, yx + 1, yy + 1, Hud_GetItemIconTiles(hud_cur_item));
    int count = -1;
    if (hud_cur_item == 1 && link_item_bow)
      count = link_num_arrows;
    else if (hud_cur_item == kHudItem_Bombs)
      count = link_item_bombs;
    if (count >= 0) {
      char buf[4];
      int n = count > 99 ? 99 : count;
      buf[0] = '0' + n / 10, buf[1] = '0' + n % 10, buf[2] = 0;
      const char *t = n < 10 ? buf + 1 : buf;
      DrawSmallText(&cv, yx + S - SmallTextWidth(t), yy + S - 5, t, 0xf8f8f8);
    }
  }
  // B: spada
  if (link_sword_type && link_sword_type != 0xff)
    DrawIcon(&cv, bx + 1, by + 1, Hud_GetSwordIconTiles());  // icona del menu, con il livello
  // A: scritta dell'azione dentro il tasto rosso
  if (!in_dialog) {
    const char *label = ActionLabel(StableAction());
    if (*label)
      DrawSmallText(&cv, ax + (S - SmallTextWidth(label)) / 2, ay + (S - 5) / 2, label, 0xe8e8e8);
  }

  // --- Rupie in basso a sinistra, chiavi del labirinto sopra ---------------
  {
    int ry = H - margin - 8;
    DrawTile(&cvb, x0, ry, 0x3ca8);
    DrawNumberTiles(&cvb, x0 + 10, ry, link_rupees_actual, 3);
    if (link_num_keys != 0xff) {
      DrawTile(&cvb, x0, ry - 10, 0x2871);
      DrawNumberTiles(&cvb, x0 + 10, ry - 10, link_num_keys, 1);
    }
  }
}
