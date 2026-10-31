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
#include "hd_world.h"
#include "hd_fx.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "config.h"
#include "hud.h"
#include "assets.h"
#include "snes/ppu.h"
#include "player.h"
#include "messaging.h"
#include "util.h"

const uint16 *Hud_GetItemIconTiles(int item);
const uint16 *Hud_GetSwordIconTiles(void);
bool Hud_HaveItem(int item);


enum {
  kAction_None, kAction_Talk, kAction_Read, kAction_Open, kAction_Lift,
  kAction_Grab, kAction_Pull, kAction_Throw, kAction_Dash, kAction_Swim,
};

static bool g_hud_was_active;
static uint8 g_talk_frame = 0xff;  // ultimo frame in cui si poteva parlare

static bool ModernHudOn(void) {
  return (enhanced_features0 & kFeatures0_ModernHud) != 0;
}

// Moduli di gioco: labirinti (7), esterno (9) e aree speciali (11: Dominio
// di Zora, sotto il ponte, Piedistallo...)
static bool IsPlayModule(int mod) {
  return mod == 7 || mod == 9 || mod == 11;
}

// Durante il gioco e nei dialoghi (non nei menu, sulla mappa o nei titoli)
static bool HudContext(bool *in_dialog) {
  int mod = main_module_index;
  *in_dialog = false;
  // il gioco in vista (HdFx_SceneModule: cerchio, cadute, pozioni, mappe che
  // si scuriscono, vittorie, Salva ed esci...)
  if (IsPlayModule(HdFx_SceneModule())) {
    // dialogo (2) e menu di salvataggio aperto con SELECT (11)
    *in_dialog = mod == 14 && (submodule_index == 2 || submodule_index == 11);
    return true;
  }
  // Morte di Link (18) e fata che lo fa rialzare: l'HUD sparisce solo con la
  // scritta GAME OVER e il menu per continuare (7-9)
  if (mod == 18 && (submodule_index < 7 || submodule_index > 9))
    return true;
  return false;
}

// Inventario e menu di salvataggio aperti durante il gioco: l'HUD originale
// scorrerebbe insieme ai riquadri, quindi resta nascosto anche li' (quello
// moderno si vede nel menu di salvataggio e scende con l'inventario)
static bool InGameMenu(void) {
  return main_module_index == 14 && (submodule_index == 1 || submodule_index == 11) &&
         IsPlayModule(saved_module_for_menu);
}

// Menu START (inventario, 14/1) aperto durante il gioco
static bool InItemMenu(void) {
  return main_module_index == 14 && submodule_index == 1 && IsPlayModule(saved_module_for_menu);
}

// Pixel di cui il menu START e' sceso (0-232, Hud_BringMenuDown e
// Hud_CloseMenu). Nel gioco l'HUD sta nello stesso strato del menu (BG3) e
// scende con lui fino a uscire dallo schermo: l'HUD moderno fa lo stesso, con
// tasti X e L/R o senza (sta tutto sotto la riga 8, a 232 pixel e' fuori)
static int ItemMenuPush(void) {
  int d = -(int16)BG3VOFS_copy2;
  return d > 0 ? d : 0;
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
  if (g_hd_neighbor_tick)
    return;  // un abitante di un'area vicina (hd_world.c)
  g_talk_frame = frame_counter;
}

// ---------------------------------------------------------------------------
// Azione del tasto A, calcolata senza cambiare lo stato del gioco

static int ComputeAction(void) {
  if (!IsPlayModule(main_module_index))
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

// Luminosita' dello schermo (0-15): l'HUD si scurisce con le dissolvenze del
// gioco (scale, mappa) invece di restare acceso sullo schermo nero
static int g_hud_bri = 15;

static void Px(Canvas *cv, int x, int y, uint32 color) {
  y += cv->oy;
  if ((unsigned)x >= (unsigned)cv->width || (unsigned)y >= (unsigned)cv->height)
    return;
  if (g_hud_bri < 15)
    color = HdFx_DimRgb(color, g_hud_bri);
  int s = cv->scale;
  for (int yy = 0; yy < s; yy++) {
    uint32 *row = (uint32 *)(cv->pixels + (y * s + yy) * cv->pitch);
    for (int xx = 0; xx < s; xx++)
      row[x * s + xx] = (color & 0xffffff) | 0x02000000;  // alpha 2 = HUD
  }
}

// Una casella 8x8 dello strato 3 (2 bit per pixel) come la disegnerebbe la PPU
static void DrawTile(Canvas *cv, int x, int y, uint16 entry) {
  Ppu *ppu = g_zenv.ppu;
  int tile = entry & 0x3ff, pal = entry >> 10 & 7;
  bool hf = entry & 0x4000, vf = entry & 0x8000;
  // cifra al massimo (0x3490 + cifra): il giallo della versione GBA
  bool max_digit = pal == 5 && (uint16)(tile - 0x90) < 10;
  uint16 base = ppu->bgLayer[2].tileAdr + tile * 8;
  for (int r = 0; r < 8; r++) {
    uint16 w = ppu->vram[(base + (vf ? 7 - r : r)) & 0x7fff];
    for (int c = 0; c < 8; c++) {
      int bit = hf ? c : 7 - c;
      int v = (w >> bit & 1) | (w >> (8 + bit) & 1) << 1;
      if (v)
        Px(cv, x + c, y + r, (max_digit && v == 2) ? 0xffde52 : HdFx_Bgr15ToRgb(ppu->cgram[pal * 4 + v]));
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
      out[r * stride + c] = v ? (HdFx_Bgr15ToRgb(ppu->cgram[pal * 4 + v]) | 0xff000000) : 0;
    }
  }
}

// Icona 16x16 di un oggetto (caselle: su-sx, su-dx, giu'-sx, giu'-dx). Il nero
// e' lo sfondo del riquadro dell'HUD: diventa tutto trasparente, anche quello
// chiuso dentro l'icona (per esempio fra l'arco e la corda o nel vetro della
// lanterna). Attorno a ogni pixel dell'icona, fuori e dentro, si aggiunge un
// contorno nero di 1 pixel che la tiene leggibile: senza, nei buchi piccoli
// (il collo del sacchetto delle bombe) il colore del tasto sembrava una
// macchia dentro l'oggetto. Nei buchi grandi resta il colore del tasto al centro.
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
  for (int r = -1; r <= 16; r++)
    for (int c = -1; c <= 16; c++) {
      if (r >= 0 && r < 16 && c >= 0 && c < 16 && img[r * 16 + c])
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

// Un'immagine ARGB di hd_hud_buttons.h (tasti o dorsali)
static void DrawImage(Canvas *cv, int x, int y, const uint32 *img, int w, int h) {
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++) {
      uint32 v = img[r * w + c];
      if (v >> 24)
        Px(cv, x + c, y + r, v);
    }
}

// Nome del tasto (A, B, X, Y) al centro del tasto, 4x5 come la L e la R dei
// dorsali, in un tono piu' scuro del tasto; gli oggetti lo coprono.
// (bx, by) = angolo del tasto dentro l'immagine disegnata in (x, y)
static void DrawButtonLetter(Canvas *cv, int x, int y, const uint32 *img, int w, int bx, int by, char ch) {
  static const uint8 kGlyph[4][5] = {
    { 0x6, 0x9, 0xf, 0x9, 0x9 },  // A
    { 0xe, 0x9, 0xe, 0x9, 0xe },  // B
    { 0x9, 0x9, 0x6, 0x9, 0x9 },  // X
    { 0x9, 0x9, 0x6, 0x6, 0x6 },  // Y
  };
  const uint8 *g = kGlyph[ch == 'A' ? 0 : ch == 'B' ? 1 : ch == 'X' ? 2 : 3];
  int lx = bx + (HD_BUTTON_SIZE - 4) / 2, ly = by + (HD_BUTTON_SIZE - 5) / 2;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 4; c++) {
      uint32 v = img[(ly + r) * w + lx + c];
      if (!(g[r] & 8 >> c) || !(v >> 24))
        continue;
      uint32 dark = ((v >> 16 & 0xff) * 7 / 10) << 16 | ((v >> 8 & 0xff) * 7 / 10) << 8 | (v & 0xff) * 7 / 10;
      Px(cv, x + lx + c, y + ly + r, dark);
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

// "Massimi in giallo nell'HUD": come nell'HUD normale (hud.c), i valori al
// massimo usano la palette gialla
static bool ShowMaxInYellow(int value, int max) {
  return (enhanced_features0 & kFeatures0_ShowMaxItemsInYellow) && value == max;
}

// Il giallo delle cifre al massimo come nella versione GBA (anche per i
// numeri piccoli di bombe e frecce nei tasti)
static uint32 YellowDigitColor(void) {
  return 0xffde52;
}

// Oggetto dentro un tasto 18x18, con il numero se e' un consumabile; gli
// oggetti di X, L e R solo se Link li ha ancora (come Hud_DrawXLRItems)
static void DrawButtonItem(Canvas *cv, int x, int y, int item) {
  const int S = HD_BUTTON_SIZE;
  if (!item || !Hud_HaveItem(item))
    return;
  DrawIcon(cv, x + 1, y + 1, Hud_GetItemIconTiles(item));
  int count = -1, max = -1;
  if (item == 1 && link_item_bow)
    count = link_num_arrows, max = kMaxArrowsForLevel[link_arrow_upgrades];
  else if (item == kHudItem_Bombs)
    count = link_item_bombs, max = kMaxBombsForLevel[link_bomb_upgrades];
  if (count >= 0) {
    char buf[4];
    int n = count > 99 ? 99 : count;
    buf[0] = '0' + n / 10, buf[1] = '0' + n % 10, buf[2] = 0;
    const char *t = n < 10 ? buf + 1 : buf;
    DrawSmallText(cv, x + S - SmallTextWidth(t), y + S - 5, t, ShowMaxInYellow(count, max) ? YellowDigitColor() : 0xf8f8f8);
  }
}

static void DrawNumberTiles(Canvas *cv, int x, int y, int value, int digits, bool yellow) {
  char buf[8];
  int n = 0;
  do {
    buf[n++] = value % 10;
    value /= 10;
  } while (value && n < 7);
  while (n < digits)
    buf[n++] = 0;
  for (int i = n - 1; i >= 0; i--, x += 8)
    DrawTile(cv, x, y, (yellow ? 0x3490 : 0x2490) + buf[i]);
}

// ---------------------------------------------------------------------------
// Contatore del tempo (super bomba posata, gioco della pala) e piano del
// labirinto: nel gioco sono le due caselle a destra nell'HUD originale
// (Hud_SuperBombIndicator, Hud_FloorIndicator), che l'HUD moderno svuota.
// Qui diventano "mm:ss.cc" o "1F"/"B1" in caratteri chiari su una fascia scura
// trasparente con i lati tondi e una piccola Triforza per parte, disegnati da
// zero.

// Cifre 5x7 a tratto doppio (righe dall'alto, 5 bit), poi B e F dei piani
static const uint8 kTimerDigits[10][7] = {
  {0x0e, 0x1b, 0x1b, 0x1b, 0x1b, 0x1b, 0x0e}, {0x06, 0x0e, 0x1e, 0x06, 0x06, 0x06, 0x06},
  {0x0e, 0x1b, 0x03, 0x06, 0x0c, 0x18, 0x1f}, {0x1e, 0x03, 0x03, 0x0e, 0x03, 0x03, 0x1e},
  {0x03, 0x07, 0x0b, 0x13, 0x1f, 0x03, 0x03}, {0x1f, 0x18, 0x1e, 0x03, 0x03, 0x1b, 0x0e},
  {0x0e, 0x18, 0x1e, 0x1b, 0x1b, 0x1b, 0x0e}, {0x1f, 0x03, 0x06, 0x06, 0x0c, 0x0c, 0x0c},
  {0x0e, 0x1b, 0x1b, 0x0e, 0x1b, 0x1b, 0x0e}, {0x0e, 0x1b, 0x1b, 0x0f, 0x03, 0x03, 0x0e},
};
static const uint8 kBandB[7] = { 0x1e, 0x1b, 0x1b, 0x1e, 0x1b, 0x1b, 0x1e };
static const uint8 kBandF[7] = { 0x1f, 0x18, 0x18, 0x1e, 0x18, 0x18, 0x18 };
// due punti e punto: 2 colonne
static const uint8 kTimerColon[7] = { 0, 3, 3, 0, 3, 3, 0 };
static const uint8 kTimerDot[7] = { 0, 0, 0, 0, 0, 3, 3 };
// Triforza 7x4 ai due lati
static const uint8 kBandTriforce[4] = { 0x08, 0x1c, 0x22, 0x77 };
enum { kTimerH = 7, kTimerBandH = 13, kBandTriW = 7, kBandTriH = 4, kTimerPad = 13 };

// Mescola un colore al pixel che c'e' gia' (a = 0..256)
static void PxBlend(Canvas *cv, int x, int y, uint32 color, int a) {
  int yy = y + cv->oy;
  if ((unsigned)x >= (unsigned)cv->width || (unsigned)yy >= (unsigned)cv->height)
    return;
  uint32 old = ((uint32 *)(cv->pixels + yy * cv->scale * cv->pitch))[x * cv->scale];
  if (g_hud_bri < 15)
    a = a * g_hud_bri / 15;
  int r = ((old >> 16 & 255) * (256 - a) + (color >> 16 & 255) * a) >> 8;
  int g = ((old >> 8 & 255) * (256 - a) + (color >> 8 & 255) * a) >> 8;
  int b = ((old & 255) * (256 - a) + (color & 255) * a) >> 8;
  int bri = g_hud_bri;
  g_hud_bri = 15;  // la luminosita' e' gia' nel peso
  Px(cv, x, y, r << 16 | g << 8 | b);
  g_hud_bri = bri;
}

// Carattere largo 5 (cifre e lettere) o 2 (due punti e punto)
static const uint8 *BandGlyph(char c, int *w) {
  *w = 5;
  if (c >= '0' && c <= '9') return kTimerDigits[c - '0'];
  if (c == 'B') return kBandB;
  if (c == 'F') return kBandF;
  *w = 2;
  return c == ':' ? kTimerColon : kTimerDot;
}

// Tempo rimasto in fotogrammi: il gioco conta secondi da 62 fotogrammi
// (super_bomb_indicator_unk2) e i fotogrammi del secondo (unk1); finisce
// quando i secondi arrivano a 0. -1 = nessun contatore
static int TimerFrames(void) {
  if (player_is_indoors || sign8(super_bomb_indicator_unk2))
    return -1;
  if (super_bomb_indicator_unk2 == 0)
    return 0;
  return (super_bomb_indicator_unk2 - 1) * 62 + super_bomb_indicator_unk1;
}

// "mm:ss.cc" in tempo vero (60 fotogrammi al secondo): la super bomba
// posata dura ~5,2 s
static void TimerText(int frames, char t[9]) {
  int cs = frames * 100 / 60, mm = cs / 6000;
  if (mm > 99) mm = 99;
  t[0] = '0' + mm / 10, t[1] = '0' + mm % 10, t[2] = ':', t[3] = '0' + cs / 1000 % 6;
  t[4] = '0' + cs / 100 % 10, t[5] = '.', t[6] = '0' + cs / 10 % 10, t[7] = '0' + cs % 10, t[8] = 0;
}

// Piano mostrato dopo un cambio di piano (Hud_FloorIndicator, circa 3 s):
// piano 0 = "1F", -1 = "B1". false = niente
static bool FloorText(char t[4]) {
  if (!hud_floor_changed_timer || !player_is_indoors)
    return false;
  int8 f = (int8)dung_cur_floor;
  int n = f >= 0 ? f + 1 : -f;
  if (n > 9)
    return false;
  if (f >= 0)
    t[0] = '0' + n, t[1] = 'F';
  else
    t[0] = 'B', t[1] = '0' + n;
  t[2] = 0;
  return true;
}

// Un punto della fascia: pieno (a < 0) o mescolato al gioco sotto
static void BandPx(Canvas *cv, int x, int y, uint32 col, int a) {
  if (a < 0)
    Px(cv, x, y, col);
  else
    PxBlend(cv, x, y, col, a);
}

// Fascia centrata in cx con il testo t
static void DrawBand(Canvas *cv, int cx, int y, const char *t) {
  int tw = 0, gw;
  for (int i = 0; t[i]; i++)
    BandGlyph(t[i], &gw), tw += gw == 5 ? 7 : 3;
  tw--;  // niente spazio dopo l'ultimo carattere
  int w = tw + 2 * kTimerPad, h = kTimerBandH, x = cx - w / 2;
  // fascia: lati a semicerchio, interno scuro trasparente, bordo chiaro sottile
  float rad = h * 0.5f;
  for (int yy = 0; yy < h; yy++) {
    float dy = yy + 0.5f - rad;
    int in = (int)(rad - sqrtf(rad * rad - dy * dy) + 0.5f);
    for (int xx = in; xx < w - in; xx++) {
      bool edge = yy == 0 || yy == h - 1 || xx == in || xx == w - 1 - in;
      if (edge)
        BandPx(cv, x + xx, y + yy, 0x9cac98, 110);
      else
        BandPx(cv, x + xx, y + yy, 0x101a14, 170);
    }
  }
  // Triforze ai lati, bianche come i caratteri e con la stessa ombra
  int ty = (h - kBandTriH) / 2;
  for (int pass = 0; pass < 2; pass++)
    for (int r = 0; r < kBandTriH; r++)
      for (int c = 0; c < kBandTriW; c++)
        if (kBandTriforce[r] >> (kBandTriW - 1 - c) & 1) {
          int d = pass == 0;
          uint32 col = pass == 0 ? 0x000000 : r == 0 ? 0xffffff : 0xeef2e4;
          int a = pass == 0 ? 150 : -1;
          BandPx(cv, x + 4 + c + d, y + ty + r + d, col, a);
          BandPx(cv, x + w - 4 - kBandTriW + c + d, y + ty + r + d, col, a);
        }
  // testo: ombra scura e poi caratteri chiari
  int py = (h - kTimerH) / 2;
  for (int pass = 0; pass < 2; pass++) {
    int px = kTimerPad;
    for (int i = 0; t[i]; i++) {
      const uint8 *g = BandGlyph(t[i], &gw);
      bool digit = gw == 5;
      for (int r = 0; r < kTimerH; r++)
        for (int c = 0; c < gw; c++) {
          if (!(g[r] >> (gw - 1 - c) & 1))
            continue;
          if (pass == 0)
            BandPx(cv, x + px + c + 1, y + py + r + 1, 0x000000, 150);
          else
            BandPx(cv, x + px + c, y + py + r, r <= 1 ? 0xffffff : 0xeef2e4, -1);
        }
      px += gw + (digit ? 2 : 1);
    }
  }
}

void HdHud_Draw(uint8 *pixels, int pitch, int width, int height, int scale) {
  bool in_dialog = false, in_menu = InItemMenu();
  if (!ModernHudOn() || (!in_menu && !HudContext(&in_dialog)))
    return;
  Ppu *ppu = g_zenv.ppu;
  if (!(ppu->screenEnabled[0] & 4) || ppu->mode == 7)
    return;  // il gioco stesso non mostra l'HUD (scene, mappa...)
  g_hud_bri = ppu->forcedBlank ? 0 : ppu->brightness;
  if (g_hud_bri == 0)
    return;
  // Come l'HUD originale resta anche nei dialoghi (prima scivolava fuori) e
  // il menu START lo spinge giu' mentre scende
  int push = in_menu ? ItemMenuPush() : 0;
  Canvas cv = { pixels, pitch, width / scale, height / scale, scale, push };
  int W = cv.width, H = cv.height;
  if (push >= H)
    return;

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
  // I tre tasti Y, A e B, piu' quelli a cui si possono assegnare oggetti: con
  // "Tasto X per gli oggetti" i tasti sono quattro, a rombo con X blu; con
  // "Tasti L e R per gli oggetti" sopra ci sono i dorsali L (sopra Y) e R
  // (sopra A) come su un controller SNES
  const int S = HD_BUTTON_SIZE;
  int yx, yy, ax, ay, bx, by;
  bool show_x = Hud_ItemOnX(), show_lr = Hud_ItemOnLR();
  int gy = top;
  if (show_lr) {
    int lx = W - margin - HD_BUTTONS4_W, rx = W - margin - HD_SHOULDER_W;
    DrawImage(&cv, lx, top, kHdShoulderL, HD_SHOULDER_W, HD_SHOULDER_H);
    DrawImage(&cv, rx, top, kHdShoulderR, HD_SHOULDER_W, HD_SHOULDER_H);
    DrawButtonItem(&cv, lx + HD_SHOULDER_L_ITEM_X, top + HD_SHOULDER_ITEM_Y, hud_cur_item_l);
    DrawButtonItem(&cv, rx + HD_SHOULDER_R_ITEM_X, top + HD_SHOULDER_ITEM_Y, hud_cur_item_r);
    gy = top + HD_SHOULDER_H + 2;
  }
  if (show_x) {
    int gx = W - margin - HD_BUTTONS4_W;
    DrawImage(&cv, gx, gy, kHdButtons4, HD_BUTTONS4_W, HD_BUTTONS4_H);
    DrawButtonLetter(&cv, gx, gy, kHdButtons4, HD_BUTTONS4_W, HD_BUTTON4_X_X, HD_BUTTON4_X_Y, 'X');
    DrawButtonLetter(&cv, gx, gy, kHdButtons4, HD_BUTTONS4_W, HD_BUTTON4_Y_X, HD_BUTTON4_Y_Y, 'Y');
    DrawButtonLetter(&cv, gx, gy, kHdButtons4, HD_BUTTONS4_W, HD_BUTTON4_A_X, HD_BUTTON4_A_Y, 'A');
    DrawButtonLetter(&cv, gx, gy, kHdButtons4, HD_BUTTONS4_W, HD_BUTTON4_B_X, HD_BUTTON4_B_Y, 'B');
    yx = gx + HD_BUTTON4_Y_X, yy = gy + HD_BUTTON4_Y_Y;
    ax = gx + HD_BUTTON4_A_X, ay = gy + HD_BUTTON4_A_Y;
    bx = gx + HD_BUTTON4_B_X, by = gy + HD_BUTTON4_B_Y;
    DrawButtonItem(&cv, gx + HD_BUTTON4_X_X, gy + HD_BUTTON4_X_Y, hud_cur_item_x);
  } else {
    int gx = W - margin - HD_BUTTONS_W;
    DrawImage(&cv, gx, gy, kHdButtons, HD_BUTTONS_W, HD_BUTTONS_H);
    DrawButtonLetter(&cv, gx, gy, kHdButtons, HD_BUTTONS_W, HD_BUTTON_Y_X, HD_BUTTON_Y_Y, 'Y');
    DrawButtonLetter(&cv, gx, gy, kHdButtons, HD_BUTTONS_W, HD_BUTTON_A_X, HD_BUTTON_A_Y, 'A');
    DrawButtonLetter(&cv, gx, gy, kHdButtons, HD_BUTTONS_W, HD_BUTTON_B_X, HD_BUTTON_B_Y, 'B');
    yx = gx + HD_BUTTON_Y_X, yy = gy + HD_BUTTON_Y_Y;
    ax = gx + HD_BUTTON_A_X, ay = gy + HD_BUTTON_A_Y;
    bx = gx + HD_BUTTON_B_X, by = gy + HD_BUTTON_B_Y;
  }

  // Y: oggetto selezionato
  if (hud_cur_item)
    DrawButtonItem(&cv, yx, yy, hud_cur_item);
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
    DrawTile(&cv, x0, ry, 0x3ca8);
    int max_rupees = (enhanced_features0 & kFeatures0_CarryMoreRupees) ? 9999 : 999;  // come MaxRupees (hud.c)
    DrawNumberTiles(&cv, x0 + 10, ry, link_rupees_actual, 3, ShowMaxInYellow(link_rupees_goal, max_rupees));
    if (link_num_keys != 0xff) {
      DrawTile(&cv, x0, ry - 10, 0x2871);
      DrawNumberTiles(&cv, x0 + 10, ry - 10, link_num_keys, 1, false);
    }
  }

  // --- Contatore del tempo o piano del labirinto, in alto al centro a filo
  // con i cuori
  char band[9];
  int frames = TimerFrames();
  if (frames >= 0)
    TimerText(frames, band), DrawBand(&cv, W / 2, top, band);
  else if (FloorText(band))
    DrawBand(&cv, W / 2, top, band);
}

// ---------------------------------------------------------------------------
// Testo moderno dei dialoghi (opzione ModernText): al posto del riquadro
// originale una fascia scura trasparente come quella del contatore, con
// caratteri chiari e dritti dello stesso stile (hd_text_font.h, disegnati da
// zero con tools/zelda3-script/hud_art/testo.js). Il gioco scrive il messaggio
// come sempre (scorrimento, scelte): qui a ogni fotogramma si ricostruiscono
// dalla RAM le tre righe gia' scritte e si ridisegnano col carattere nuovo,
// andando a capo dove va a capo il gioco. La telepatia (messaggio senza
// riquadro) resta senza fascia. I tasti A B X Y sono cerchi colorati con la
// lettera ritagliata, la faccia di Link e' la testa del suo sprite; solo i cuori (mai usati
// nei dialoghi italiani) si disegnano dal font del gioco.

#include "hd_text_font.h"

enum {
  kMtLines = 3, kMtSlots = 64, kMtSpace = 89,
  kMtPitch = 12, kMtPadX = 10, kMtPadY = 6, kMtRadius = 5,
  kMtBoxW = 24, kMtBoxH = 8,  // riquadro originale in caselle
};
static uint8 g_mt_chars[kMtLines][kMtSlots], g_mt_len[kMtLines];
static bool g_mt_border = true;
// Comparsa morbida: ogni carattere ha il suo numero d'ordine (g_mt_seq) e
// compare quando g_mt_reveal lo raggiunge, sfumando in kMtFadeFrames
// fotogrammi; g_mt_reveal avanza di kMtRevealStep caratteri per fotogramma
// senza superare quelli gia' scritti dal gioco (g_mt_written)
// (velocita' presa dal video di riferimento, 11:12-11:18: circa 75 lettere al
// secondo, ognuna si accende in 3 fotogrammi)
static const float kMtRevealStep = 1.25f, kMtFadeFrames = 3;
static int g_mt_seq[kMtLines][kMtSlots], g_mt_written;
// posizione di ogni carattere nella riga, fissata quando il gioco lo scrive:
// una scelta riscrive solo il cursore e gli spazi davanti, le parole restano
static uint8 g_mt_x[kMtLines][kMtSlots];
static float g_mt_reveal;
// caselle del riquadro originale nascoste mentre disegna la PPU; quelle
// lasciate (icona dell'oggetto da scegliere) restano visibili anche nella fascia
static uint16 g_mt_saved[kMtBoxH][kMtBoxW];
static uint32 g_mt_kept[kMtBoxH];
static bool g_mt_hidden;

// Il carattere nuovo esiste per l'alfabeto inglese e per l'italiano
static bool ModernTextAlphabet(void) {
  const char *l = g_config.language;
  return !l || !*l || StringEqualsNoCase(l, "us") || StringEqualsNoCase(l, "it") ||
         StringEqualsNoCase(l, "redux");
}

// dialoghi (14/2) e menu Continua / Salva ed esci aperto con SELECT (14/11)
bool HdText_ModernActive(void) {
  return g_config.modern_text && main_module_index == 14 &&
         (submodule_index == 2 || submodule_index == 11) && ModernTextAlphabet();
}

static void MtClearLine(int line) {
  memset(g_mt_chars[line], kMtSpace, kMtSlots);
  memset(g_mt_seq[line], 0, sizeof(g_mt_seq[line]));
  memset(g_mt_x[line], 0, kMtSlots);
  g_mt_len[line] = 0;
}

static int MtAdvance(int c);

static void MtChar(int line, int slot, int c) {
  if ((unsigned)line >= kMtLines || (unsigned)slot >= kMtSlots)
    return;
  // come VWF_RenderSingle: subito dopo il carattere scritto prima
  g_mt_x[line][slot] = slot ? IntMin(g_mt_x[line][slot - 1] + MtAdvance(g_mt_chars[line][slot - 1]), 255) : 0;
  g_mt_chars[line][slot] = c;
  g_mt_seq[line][slot] = g_mt_written++;
  if (slot >= g_mt_len[line])
    g_mt_len[line] = slot + 1;
}

static void MtLinesUp(void) {
  for (int i = 0; i < kMtLines - 1; i++) {
    memcpy(g_mt_chars[i], g_mt_chars[i + 1], kMtSlots);
    memcpy(g_mt_seq[i], g_mt_seq[i + 1], sizeof(g_mt_seq[i]));
    memcpy(g_mt_x[i], g_mt_x[i + 1], kMtSlots);
    g_mt_len[i] = g_mt_len[i + 1];
  }
  MtClearLine(kMtLines - 1);
}

// Righe rimaste sotto una scelta: il gioco carica un messaggio con le sole
// righe della scelta e lo scrive sopra la domanda, che resta. Stanno nella RAM
// libera (finiscono nei salvataggi rapidi): 3 righe di kMtSlots caratteri, le
// lunghezze e le posizioni dei caratteri
#define mt_base_chars ((uint8 *)(g_ram + 0x1F600))
#define mt_base_len ((uint8 *)(g_ram + 0x1F6C0))
#define mt_base_x ((uint8 *)(g_ram + 0x1F700))

// Le tre righe ricostruite da cio' che il gioco ha gia' scritto (dalla RAM:
// valgono anche dopo un salvataggio rapido o una ricarica dei testi). Le scelte
// riscrivono il messaggio nello stesso ordine: le lettere non ricompaiono
static void MtRebuild(void) {
  for (int i = 0; i < kMtLines; i++) {
    MtClearLine(i);
    g_mt_len[i] = IntMin(mt_base_len[i], kMtSlots);
    memcpy(g_mt_chars[i], mt_base_chars + i * kMtSlots, g_mt_len[i]);
    memcpy(g_mt_x[i], mt_base_x + i * kMtSlots, g_mt_len[i]);
    for (int j = 0; j < g_mt_len[i]; j++)
      g_mt_seq[i][j] = -1;  // gia' comparse
  }
  g_mt_written = 0;
  Text_ReplayMessage(MtChar, MtLinesUp);
  // le lettere gia' uscite in alto con lo scorrimento non contano: dopo un
  // salvataggio rapido a meta' messaggio la comparsa parte dalla prima visibile
  int first = g_mt_written;
  for (int i = 0; i < kMtLines; i++)
    for (int j = 0; j < g_mt_len[i]; j++)
      if (g_mt_seq[i][j] >= 0)
        first = IntMin(first, g_mt_seq[i][j]);
  g_mt_reveal = fmaxf(g_mt_reveal, (float)first);
}

void HdText_NewMessage(void) {
  g_mt_reveal = 0;
  memset(mt_base_len, 0, kMtLines);
}

void HdText_KeepLines(void) {
  MtRebuild();
  for (int i = 0; i < kMtLines; i++) {
    memcpy(mt_base_chars + i * kMtSlots, g_mt_chars[i], kMtSlots);
    memcpy(mt_base_x + i * kMtSlots, g_mt_x[i], kMtSlots);
    mt_base_len[i] = g_mt_len[i];
  }
  // la scelta riscrive le sue righe da capo: niente comparsa per quelle
  g_mt_reveal = 1e6f;
}

// Una volta per fotogramma del gioco mentre scrive il messaggio
void HdText_Tick(void) {
  MtRebuild();
  g_mt_reveal = fminf(g_mt_reveal + kMtRevealStep, (float)g_mt_written + kMtRevealStep * kMtFadeFrames);
  // menu di SELECT: compare tutto insieme come l'originale, senza far
  // aspettare la scelta
  if (submodule_index == 11)
    HdText_RevealAll();
}

bool HdText_Revealing(void) {
  if (!HdText_ModernActive())
    return false;
  MtRebuild();
  return g_mt_reveal < g_mt_written + kMtRevealStep * kMtFadeFrames;
}

void HdText_RevealAll(void) {
  g_mt_reveal = g_mt_written + kMtRevealStep * kMtFadeFrames;
}

static bool IsDialogBoxTile(int t) {
  // riempimento, cornice (0xf3 angoli, 0xf4 lati sopra e sotto, 0xc8 lati) e
  // le 126 caselle del testo
  return t == 0x7f || t == 0xf3 || t == 0xf4 || t == 0xc8 || (t >= 0x180 && t < 0x180 + 126);
}

void HdText_BeforeRender(Ppu *ppu) {
  g_mt_hidden = false;
  if (!HdText_ModernActive())
    return;
  // con il riquadro l'angolo in alto a sinistra e' la casella 0xf3; la
  // telepatia (comando Window) non lo disegna
  g_mt_border = (ppu->vram[text_msgbox_topleft & 0x7fff] & 0x3ff) == 0xf3;
  // una casella vuota dello strato 3 da mettere al posto del riquadro
  int blank = -1;
  for (int t = 0; t < 1024 && blank < 0; t++) {
    const uint16 *p = &ppu->vram[(ppu->bgLayer[2].tileAdr + t * 8) & 0x7fff];
    if (!(p[0] | p[1] | p[2] | p[3] | p[4] | p[5] | p[6] | p[7]))
      blank = t;
  }
  if (blank < 0)
    return;
  for (int r = 0; r < kMtBoxH; r++) {
    g_mt_kept[r] = 0;
    for (int c = 0; c < kMtBoxW; c++) {
      uint16 *v = &ppu->vram[(text_msgbox_topleft + r * 32 + c) & 0x7fff];
      g_mt_saved[r][c] = *v;
      if (IsDialogBoxTile(*v & 0x3ff))
        *v = (*v & ~0x3ff) | blank;
      else if (r > 0 && r < kMtBoxH - 1 && c > 0 && c < kMtBoxW - 1)
        g_mt_kept[r] |= 1u << c;
    }
  }
  g_mt_hidden = true;
}

void HdText_AfterRender(Ppu *ppu) {
  if (!g_mt_hidden)
    return;
  for (int r = 0; r < kMtBoxH; r++)
    for (int c = 0; c < kMtBoxW; c++)
      ppu->vram[(text_msgbox_topleft + r * 32 + c) & 0x7fff] = g_mt_saved[r][c];
  g_mt_hidden = false;
}

// Larghezza dei simboli presi dal font del gioco (gia' con lo spazio dopo)
static int GameGlyphWidth(int c) {
  MemBlk widths = FindIndexInMemblk(g_zenv.dialogue_font_blk, 1);
  return (unsigned)c < widths.size ? IntMin(widths.ptr[c], 8) : 0;
}

static int MtAdvance(int c) {
  if (c == 74)
    return 8;  // le due meta' della faccia di Link sono attaccate
  if ((unsigned)c < kModernTextGlyphs && kModernTextWidth[c])
    return kModernTextWidth[c] + 1;
  return GameGlyphWidth(c);
}

// Simbolo del font del gioco (16 righe, 2 bit per pixel) con i colori della
// palette del testo
static void DrawGameGlyph(Canvas *cv, int x, int y, int c, int clip0, int clip1, int alpha) {
  const uint8 *font = FindIndexInMemblk(g_zenv.dialogue_font_blk, 0).ptr;
  int w = GameGlyphWidth(c), pal = text_tilemap_cur >> 10 & 7;
  for (int r = 0; r < 16; r++) {
    if (y + r < clip0 || y + r >= clip1)
      continue;
    for (int i = 0; i < w; i++) {
      int v = HdFx_GameGlyphPixel(font, c, r, i);
      if (v)
        BandPx(cv, x + i, y + r, HdFx_Bgr15ToRgb(g_zenv.ppu->cgram[pal * 4 + v]), alpha >= 256 ? -1 : alpha);
    }
  }
}

// Una riga del messaggio: ombra scura e poi caratteri chiari, solo le righe
// di pixel fra clip0 e clip1 (scorrimento)
static void DrawModernLine(Canvas *cv, int x, int y, int line, int clip0, int clip1) {
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < g_mt_len[line]; i++) {
      int c = g_mt_chars[line][i], px = x + g_mt_x[line][i];
      int w = (unsigned)c < kModernTextGlyphs ? kModernTextWidth[c] : 0;
      // comparsa morbida: 0 = ancora nascosto, 256 = pieno
      float t = (g_mt_reveal - g_mt_seq[line][i]) / (kMtRevealStep * kMtFadeFrames);
      int alpha = g_mt_seq[line][i] < 0 || t >= 1 ? 256 : t <= 0 ? 0 : (int)(t * 256);
      if (!w) {
        // simbolo del gioco, centrato in altezza sulla riga
        if (pass == 1 && alpha)
          DrawGameGlyph(cv, px, y - 4, c, clip0, clip1, alpha);
        continue;
      }
      if (!alpha)
        continue;
      if (c == 74 || c == 75) {
        // faccia di Link: la testa dello sprite di Link vista di fronte
        // (caselle 2-3 e 18-19 di kLinkGraphics, anche con Link biondo o
        // personalizzato) con la sua palette; sporge sopra la riga
        // come il simbolo originale, anche sul margine della fascia
        if (pass == 0)
          continue;
        const uint16 *pal = &g_zenv.ppu->cgram[0xf0];  // palette di Link (sprite 7)
        for (int r = 0; r < 16; r++) {
          int yy = y - 6 + r;
          if (yy < clip0 - 2 || yy >= clip1)
            continue;
          const uint8 *t = kLinkGraphics + (2 + (c - 74) + (r >> 3) * 16) * 32 + (r & 7) * 2;
          for (int b = 0; b < 8; b++) {
            int v = (t[0] >> (7 - b) & 1) | (t[1] >> (7 - b) & 1) << 1 |
                    (t[16] >> (7 - b) & 1) << 2 | (t[17] >> (7 - b) & 1) << 3;
            if (v)
              BandPx(cv, px + b, yy, HdFx_Bgr15ToRgb(pal[v]), alpha >= 256 ? -1 : alpha);
          }
        }
        continue;
      }
      const uint16 *g = kModernTextFont[c];
      // tasti A B X Y: colori SNES come i tasti dell'HUD (tasti.js)
      static const uint32 kButtonColor[4] = { 0xff4545, 0xfae037, 0x2a6ad8, 0x46bf40 };
      uint32 ink = c >= 91 && c <= 94 ? kButtonColor[c - 91] : 0;
      for (int r = 0; r < kModernTextRows; r++) {
        int yy = y + r + (pass == 0);
        if (yy < clip0 || yy >= clip1)
          continue;
        for (int b = 0; b < w; b++) {
          if (!(g[r] >> (w - 1 - b) & 1))
            continue;
          if (pass == 0)
            BandPx(cv, px + b + 1, yy, 0x000000, 150 * alpha >> 8);
          else
            BandPx(cv, px + b, yy, ink ? ink : r <= 1 ? 0xffffff : 0xeef2e4, alpha >= 256 ? -1 : alpha);
        }
      }
    }
  }
}

void HdText_Draw(uint8 *pixels, int pitch, int width, int height, int scale) {
  // messaging_module 0: il dialogo e' appena cominciato e il messaggio non e'
  // ancora caricato (nel buffer c'e' ancora quello di prima)
  if (!HdText_ModernActive() || messaging_module == 0)
    return;
  Canvas cv = { pixels, pitch, width / scale, height / scale, scale };
  MtRebuild();
  int bri = g_hud_bri;
  g_hud_bri = 15;
  // riquadro originale (come HdText_GetRect): la fascia sta al centro in
  // orizzontale e a filo del suo bordo esterno (sotto o sopra, come il gioco)
  Ppu *ppu = g_zenv.ppu;
  int off = (text_msgbox_topleft - 0x6000) & 0x3ff;
  int bx = (cv.width - 256) / 2 + (off & 31) * 8 - (ppu->bgLayer[2].hScroll & 0xff);
  int by = (off >> 5) * 8 - (ppu->bgLayer[2].vScroll & 0xff);
  // grandezza fissa per tutti i messaggi, come il riquadro originale: larga
  // quanto la riga piu' lunga del gioco (ricalcolata se cambia la lingua), alta
  // tre righe; il testo a sinistra, con le righe usate al centro in altezza
  static const uint8 *width_for;
  static int max_w;
  if (width_for != g_zenv.dialogue_blk.ptr)
    width_for = g_zenv.dialogue_blk.ptr, max_w = Text_MaxLineWidth(MtAdvance);
  // righe con del testo: quelle vuote in cima (cartelli che cominciano dalla
  // seconda riga) non contano, cosi' il testo resta centrato come nel gioco;
  // nelle scelte le righe della domanda restano dove sono
  int lines, first;
  Text_MeasureMessage(MtAdvance, &lines, &first);
  if (mt_base_len[0] | mt_base_len[1] | mt_base_len[2])
    first = 0;
  int w = IntMax(max_w - 1, 40) + 2 * kMtPadX, h = kMtLines * kMtPitch - 3 + 2 * kMtPadY;
  int x = bx + kMtBoxW * 4 - w / 2;
  int y = (off >> 5) >= 16 ? by + kMtBoxH * 8 - 2 - h : by + 2;
  // senza fascia (telepatia) la prima riga sta dove comincia il testo originale
  if (!g_mt_border)
    x = bx + 9 - kMtPadX, y = by + 9 - kMtPadY;
  if (g_mt_border) {
    // rettangolo con gli angoli tondi, scuro e trasparente come la fascia del
    // contatore (senza bordo)
    int in[66] = { 0 };  // rientro di ogni riga (angoli), con una riga vuota sopra e sotto
    for (int yy = 0; yy < h; yy++) {
      int d = yy < kMtRadius ? kMtRadius - yy : yy >= h - kMtRadius ? yy - (h - 1 - kMtRadius) : 0;
      float dy = d - 0.5f;
      in[yy + 1] = d ? (int)(kMtRadius - sqrtf(kMtRadius * kMtRadius - dy * dy) + 0.5f) : 0;
    }
    in[0] = in[h + 1] = w;
    for (int yy = 0; yy < h; yy++) {
      int a = in[yy + 1];
      for (int xx = a; xx < w - a; xx++) {
        int sx = x + xx, sy = y + yy;
        // icona dell'oggetto da scegliere: resta com'e'
        int cc = (sx - bx) >> 3, cr = (sy - by) >> 3;
        if (sx >= bx && sy >= by && cr < kMtBoxH && cc < kMtBoxW && (g_mt_kept[cr] >> cc & 1))
          continue;
        BandPx(&cv, sx, sy, 0x101a14, 170);
      }
    }
  }
  // righe: durante lo scorrimento salgono insieme al testo del gioco
  int ty = y + (g_mt_border ? (h - ((lines - first) * kMtPitch - 3)) / 2 - first * kMtPitch : kMtPadY) - (byte_7E1CDF & 0xf) * kMtPitch / 16;
  int clip0 = g_mt_border ? y + 2 : y, clip1 = g_mt_border ? y + h - 2 : y + h;
  for (int i = 0; i < kMtLines; i++)
    DrawModernLine(&cv, x + kMtPadX, ty + i * kMtPitch, i, clip0, clip1);
  // triangolo bianco in basso al centro, sul bordo: il messaggio aspetta un
  // tasto (compare quando il testo e' tutto visibile, e va su e giu')
  if (Text_WaitingForKey() && !HdText_Revealing()) {
    static const uint8 kTri[4] = { 0x7f, 0x3e, 0x1c, 0x08 };
    int tx = x + w / 2 - 3, tyy = y + h - 3 + (frame_counter >> 4 & 1);
    for (int pass = 0; pass < 2; pass++)
      for (int r = 0; r < 4; r++)
        for (int c = 0; c < 7; c++)
          if (kTri[r] >> (6 - c) & 1) {
            if (pass == 0)
              BandPx(&cv, tx + c + 1, tyy + r + 1, 0x000000, 150);
            else
              BandPx(&cv, tx + c, tyy + r, r == 0 ? 0xffffff : 0xeef2e4, -1);
          }
  }
  g_hud_bri = bri;
}
