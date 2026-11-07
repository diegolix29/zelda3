// Menu Start in italiano. I nomi italiani sono piu' lunghi di quelli inglesi
// e con le lettere del gioco (8 pixel l'una, 8 per riga) non ci stanno: si
// scrivono con un carattere stretto, alto come quello del gioco, e le
// intestazioni dei riquadri con un carattere piccolo come "ITEM". Le parole si
// disegnano in caselle del BG3 che il gioco usa solo per il testo dei dialoghi
// (0x180 in poi): mentre il menu e' aperto non serve, e il prossimo dialogo
// lo riscrive.
#include <string.h>
#include "hud_it.h"
#include "hud.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "config.h"
#include "util.h"
#include "snes/ppu.h"
#include "screens_it.h"

enum {
  kTileBase = 0x180,       // prima casella usata (testo dei dialoghi)
  kTileItemName = kTileBase,         // 16 caselle: nome dell'oggetto
  kTileLabels = kTileBase + 16,      // intestazioni, una dopo l'altra (31 caselle)
  kTileAbilities = kTileBase + 48,   // 6 abilita' da 5 caselle
  kColorFg = 2, kColorBg = 3,        // come le lettere del gioco
  kTileAttr = 0x2400,                // palette 1 e priorita', come le scritte del menu
};

bool HudIt_Active(void) {
  return g_config.language && StringEqualsNoCase(g_config.language, "it");
}

// Caratteri: ogni lettera e' una riga di testo per riga di pixel, '#' acceso.
// Stretto (nomi): alto 6, largo 4 (I e T 3, M, V, W e Y 5). Piccolo
// (intestazioni): alto 5, largo 3 (I 1, N 4, M e W 5), come "ITEM" del gioco.
typedef struct Glyph {
  char c;
  const char *rows[6];
} Glyph;

static const Glyph kNarrow[] = {
  { 'A', { ".##.", "#..#", "#..#", "####", "#..#", "#..#" } },
  { 'B', { "###.", "#..#", "###.", "#..#", "#..#", "###." } },
  { 'C', { ".##.", "#..#", "#...", "#...", "#..#", ".##." } },
  { 'D', { "###.", "#..#", "#..#", "#..#", "#..#", "###." } },
  { 'E', { "####", "#...", "###.", "#...", "#...", "####" } },
  { 'F', { "####", "#...", "###.", "#...", "#...", "#..." } },
  { 'G', { ".##.", "#...", "#.##", "#..#", "#..#", ".###" } },
  { 'H', { "#..#", "#..#", "####", "#..#", "#..#", "#..#" } },
  { 'I', { "###", ".#.", ".#.", ".#.", ".#.", "###" } },
  { 'J', { "...#", "...#", "...#", "...#", "#..#", ".##." } },
  { 'K', { "#..#", "#.#.", "##..", "#.#.", "#..#", "#..#" } },
  { 'L', { "#...", "#...", "#...", "#...", "#...", "####" } },
  { 'M', { "#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#" } },
  { 'N', { "#..#", "##.#", "#.##", "#..#", "#..#", "#..#" } },
  { 'O', { ".##.", "#..#", "#..#", "#..#", "#..#", ".##." } },
  { 'P', { "###.", "#..#", "#..#", "###.", "#...", "#..." } },
  { 'Q', { ".##.", "#..#", "#..#", "#..#", "#.#.", ".#.#" } },
  { 'R', { "###.", "#..#", "#..#", "###.", "#.#.", "#..#" } },
  { 'S', { ".###", "#...", ".##.", "...#", "...#", "###." } },
  { 'T', { "###", ".#.", ".#.", ".#.", ".#.", ".#." } },
  { 'U', { "#..#", "#..#", "#..#", "#..#", "#..#", ".##." } },
  { 'V', { "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#.." } },
  { 'W', { "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#" } },
  { 'X', { "#..#", "#..#", ".##.", ".##.", "#..#", "#..#" } },
  { 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#.." } },
  { 'Z', { "####", "...#", "..#.", ".#..", "#...", "####" } },
  { '\'', { "#", "#", ".", ".", ".", "." } },
  { '.', { ".", ".", ".", ".", ".", "#" } },
  { '1', { ".#.", "##.", ".#.", ".#.", ".#.", "###" } },
  { '2', { ".##.", "#..#", "..#.", ".#..", "#...", "####" } },
  { '3', { "###.", "...#", ".##.", "...#", "...#", "###." } },
  { ' ', { "..", "..", "..", "..", "..", ".." } },
};

static const Glyph kSmall[] = {
  { 'A', { ".#.", "#.#", "###", "#.#", "#.#" } },
  { 'B', { "##.", "#.#", "##.", "#.#", "##." } },
  { 'C', { ".##", "#..", "#..", "#..", ".##" } },
  { 'D', { "##.", "#.#", "#.#", "#.#", "##." } },
  { 'E', { "###", "#..", "##.", "#..", "###" } },
  { 'F', { "###", "#..", "##.", "#..", "#.." } },
  { 'G', { ".##", "#..", "#.#", "#.#", ".##" } },
  { 'H', { "#.#", "#.#", "###", "#.#", "#.#" } },
  { 'I', { "#", "#", "#", "#", "#" } },
  { 'J', { "..#", "..#", "..#", "#.#", ".#." } },
  { 'K', { "#.#", "#.#", "##.", "#.#", "#.#" } },
  { 'L', { "#..", "#..", "#..", "#..", "###" } },
  { 'M', { "#...#", "##.##", "#.#.#", "#...#", "#...#" } },
  { 'N', { "#..#", "##.#", "#.##", "#..#", "#..#" } },
  { 'O', { ".#.", "#.#", "#.#", "#.#", ".#." } },
  { 'P', { "##.", "#.#", "##.", "#..", "#.." } },
  { 'Q', { ".#.", "#.#", "#.#", "##.", ".##" } },
  { 'R', { "##.", "#.#", "##.", "#.#", "#.#" } },
  { 'S', { ".##", "#..", ".#.", "..#", "##." } },
  { 'T', { "###", ".#.", ".#.", ".#.", ".#." } },
  { 'U', { "#.#", "#.#", "#.#", "#.#", "###" } },
  { 'V', { "#.#", "#.#", "#.#", "#.#", ".#." } },
  { 'W', { "#...#", "#...#", "#.#.#", "##.##", "#...#" } },
  { 'X', { "#.#", "#.#", ".#.", "#.#", "#.#" } },
  { 'Y', { "#.#", "#.#", ".#.", ".#.", ".#." } },
  { 'Z', { "###", "..#", ".#.", "#..", "###" } },
  { '.', { ".", ".", ".", ".", "#" } },
  { '\'', { "#", "#", ".", ".", "." } },
  { ' ', { "..", "..", "..", "..", ".." } },
};

static const Glyph *FindGlyph(const Glyph *font, int n, char c) {
  for (int i = 0; i < n; i++)
    if (font[i].c == c)
      return &font[i];
  return NULL;
}

const char *const *HudIt_SmallGlyph(char c) {
  const Glyph *g = FindGlyph(kSmall, (int)countof(kSmall), c);
  return g ? g->rows : NULL;
}

// Larghezza in pixel del testo (1 pixel fra una lettera e l'altra)
static int TextWidth(const Glyph *font, int n, const char *s) {
  int w = 0;
  for (; *s; s++) {
    const Glyph *g = FindGlyph(font, n, *s);
    if (g)
      w += (int)strlen(g->rows[0]) + (s[1] ? 1 : 0);
  }
  return w;
}

// Disegna il testo nelle caselle a partire da tile (larghe ntiles, una riga di
// caselle), x pixel dal bordo sinistro e y dall'alto; le caselle vanno
// cancellate prima con ClearTiles
static void DrawText(int tile, int ntiles, const Glyph *font, int n, int height,
                     const char *s, int x, int y) {
  uint16 *vram = g_zenv.vram;
  for (; *s; s++) {
    const Glyph *g = FindGlyph(font, n, *s);
    if (!g)
      continue;
    int w = (int)strlen(g->rows[0]);
    for (int r = 0; r < height; r++) {
      for (int c = 0; c < w; c++) {
        int px = x + c, py = y + r;
        if (g->rows[r][c] != '#' || px < 0 || px >= ntiles * 8 || py < 0 || py >= 8)
          continue;
        uint16 *word = &vram[(0x7000 + (tile + px / 8) * 8 + py) & 0x7fff];
        int bit = 7 - (px & 7);
        *word = (*word & ~(0x101 << bit)) | (kColorFg & 1) << bit | (kColorFg >> 1 & 1) << (bit + 8);
      }
    }
    x += w + 1;
  }
}

// Apostrofo largo: il gioco non ce l'ha, si disegna nella prima casella del
// nome (libera quando il nome usa le lettere larghe), tratto da 1 pixel
// come le lettere del gioco
static const Glyph kWideApostrophe = { '\'', { "...##", "....#" } };

// Vero se s sta in n lettere larghe e ha solo lettere che ci sono
static bool WideFits(const char *s, int len, int n) {
  if (len > n)
    return false;
  for (int i = 0; i < len; i++)
    if (s[i] != ' ' && s[i] != '\'' && (s[i] < 'A' || s[i] > 'Z'))
      return false;
  return true;
}

// Lettere larghe del gioco (8 pixel, come nei nomi inglesi): A-Z dalla
// casella 0x150, lo spazio e' 0xf5. Scrive in dst le caselle di s allineato
// a destra (right) o a sinistra in n caselle; falso se non ci sta o ha
// lettere che il gioco non ha
static bool DrawWide(uint16 *dst, int n, const char *s, uint16 attr, bool right) {
  int len = (int)strlen(s);
  if (!WideFits(s, len, n))
    return false;
  int x = right ? n - len : 0;
  for (int i = 0; i < n; i++) {
    int c = i - x;
    char ch = c >= 0 && c < len ? s[c] : ' ';
    dst[i] = attr | (ch == '\'' ? kTileItemName : ch != ' ' ? 0x150 + ch - 'A' : 0xf5);
  }
  return true;
}

static void ClearTiles(int tile, int ntiles) {
  uint16 fill = (kColorBg & 1 ? 0xff : 0) | (kColorBg & 2 ? 0xff00 : 0);
  for (int i = 0; i < ntiles * 8; i++)
    g_zenv.vram[(0x7000 + tile * 8 + i) & 0x7fff] = fill;
}

// Intestazioni: testo, caselle occupate
static const struct { const char *text; uint8 tiles; } kLabels[] = {
  { "STRUMENTO", 5 },
  { "AZIONE", 3 },
  { "EQUIPAGGIAMENTO", 8 },
  { "LABIRINTO", 5 },
  { "AMULETI", 5 },
  { "CRISTALLI", 5 },
};

void HudIt_DrawLabel(uint16 *dst, int label) {
  int tile = kTileLabels;
  for (int i = 0; i < label; i++)
    tile += kLabels[i].tiles;
  int n = kLabels[label].tiles;
  ClearTiles(tile, n);
  // il testo comincia dal primo pixel e sta fra la riga 1 e la 5, come "ITEM"
  DrawText(tile, n, kSmall, (int)countof(kSmall), 5, kLabels[label].text, 0, 1);
  for (int i = 0; i < n; i++)
    dst[i] = kTileAttr | (tile + i);
}

// Nome italiano dell'oggetto, con '\n' dove va a capo (righe al massimo di
// 64 pixel); stessa scelta di Hud_DrawSelectedYButtonItem
static const char *ItemName(int item, int btn_index) {
  static const char *const kBottles[9] = {
    "FUNGO", "AMPOLLA", "POZIONE\nVITA", "POZIONE\nMAGICA", "POZIONE\nCURATUTTO",
    "FATA", "APE", "APE\nDORATA", "POZIONE\nD'ORO",
  };
  // items 1, 5, 13, 16 and 20 are named by the switch below
  static const char *const kItems[20] = {
    "", "BOOMERANG", "ARPIONE", "BOMBA", "",
    "BACCHETTA DEL\nFUOCO", "BACCHETTA DEL\nGHIACCIO", "FIAMMA", "ETERE", "SISMA",
    "LANTERNA", "MARTELLO\nMAGICO", "", "RETINO PER\nFARFALLE", "LIBRO DI\nMUDORA",
    "", "BASTONE DI\nSOMARIA", "BASTONE DI\nBYRNA", "MANTELLO\nMAGICO", "",
  };
  // inventario esteso: quattro bottiglie, la 16 e' la pala; in quello
  // originale la 16 e' la bottiglia scelta
  if (item >= kHudItem_Bottle1 && item <= kHudItem_Bottle4) {
    int bottle = link_bottle_info[item - kHudItem_Bottle1];
    return bottle >= 1 && bottle <= 9 ? kBottles[bottle - 1] : "";
  }
  if (item == kHudItem_BottleOld && !Hud_ExtendedInventory()) {
    int i = link_item_bottle_index;
    int bottle = i >= 1 && i <= 4 ? link_bottle_info[i - 1] : 0;
    return bottle >= 1 && bottle <= 9 ? kBottles[bottle - 1] : "";
  }
  switch (item) {
  case 0: return btn_index ? "NON\nASSEGNATO" : "";
  case 1: return link_item_bow == 2 ? "ARCO E FRECCE" : link_item_bow == 4 ? "ARCO E FRECCE\nD'ARGENTO" : "ARCO";
  case 5: return link_item_mushroom != 1 ? "POLVERE\nMAGICA" : "FUNGO";
  case kHudItem_Flute: return link_item_flute != 1 ? "OCARINA" : "PALA";
  case kHudItem_Shovel: return "PALA";
  case 20: return link_item_mirror != 1 ? "SPECCHIO\nMAGICO" : "";
  }
  return item >= 1 && item <= 20 ? kItems[item - 1] : "";
}

void HudIt_DrawItemName(uint16 *dst, int item, int btn_index) {
  const char *name = ItemName(item, btn_index);
  const char *nl = strchr(name, '\n');
  char first[24];
  const char *second = name;
  first[0] = 0;
  if (nl) {
    int n = (int)(nl - name) < (int)sizeof(first) - 1 ? (int)(nl - name) : (int)sizeof(first) - 1;
    memcpy(first, name, n);
    first[n] = 0;
    second = nl + 1;
  }
  // Due righe di 8 caselle: come nel gioco, un nome corto sta sulla seconda
  // riga a sinistra; con due righe la prima e' a sinistra e la seconda a destra.
  // Se ogni riga ci sta in 8 lettere si usano le lettere larghe del gioco
  // (FIAMMA, POZIONE VITA), altrimenti il carattere stretto. Un nome con l'a
  // capo scritto lo tiene (BASTONE DI / BYRNA resta stretto come SOMARIA);
  // uno senza prova ogni a capo fra due parole, scegliendo le righe piu'
  // pari (ARCO E / FRECCE)
  // BOOMERANG ha 9 lettere, ma il gioco lo ha gia' disegnato largo in 8
  // caselle (0x170-0x177, come nel nome inglese)
  if (!strcmp(name, "BOOMERANG")) {
    for (int i = 0; i < 8; i++) {
      dst[i] = kTileAttr | 0xf5;
      dst[32 + i] = kTileAttr | (0x170 + i);
    }
    return;
  }
  char wide1[24], wide2[24];
  bool wide =WideFits(first, (int)strlen(first), 8) && WideFits(second, (int)strlen(second), 8);
  bool wide_two = nl != NULL;
  if (wide) {
    strcpy(wide1, first);
    strcpy(wide2, second);
  } else if (!nl) {
    char flat[48];
    int len = (int)strlen(name) < (int)sizeof(flat) - 1 ? (int)strlen(name) : (int)sizeof(flat) - 1;
    memcpy(flat, name, len);
    flat[len] = 0;
    for (int i = 0; i < len; i++)
      if (flat[i] == '\n')
        flat[i] = ' ';
    if (WideFits(flat, len, 8)) {
      wide1[0] = 0;
      strcpy(wide2, flat);
      wide = true, wide_two = false;
    } else {
      int best = -1, best_max = 99;
      for (int i = 1; i < len - 1; i++) {
        int m = i > len - 1 - i ? i : len - 1 - i;
        if (flat[i] == ' ' && i < (int)sizeof(wide1) && len - 1 - i < (int)sizeof(wide2) &&
            WideFits(flat, i, 8) && WideFits(flat + i + 1, len - 1 - i, 8) && m < best_max)
          best = i, best_max = m;
      }
      if (best >= 0) {
        memcpy(wide1, flat, best);
        wide1[best] = 0;
        strcpy(wide2, flat + best + 1);
        wide = true, wide_two = true;
      }
    }
  }
  uint16 row1[8], row2[8];
  if (wide && DrawWide(row1, 8, wide1, kTileAttr, false) && DrawWide(row2, 8, wide2, kTileAttr, wide_two)) {
    if (strchr(name, '\'')) {
      ClearTiles(kTileItemName, 1);
      DrawText(kTileItemName, 1, &kWideApostrophe, 1, 2, "'", 0, 1);
    }
    memcpy(dst, row1, sizeof(row1));
    memcpy(dst + 32, row2, sizeof(row2));
    return;
  }
  int n = (int)countof(kNarrow);
  ClearTiles(kTileItemName, 16);
  DrawText(kTileItemName, 8, kNarrow, n, 6, first, 0, 1);
  int x2 = nl ? 64 - TextWidth(kNarrow, n, second) : 0;
  DrawText(kTileItemName + 8, 8, kNarrow, n, 6, second, x2 < 0 ? 0 : x2, 1);
  for (int i = 0; i < 8; i++) {
    dst[i] = kTileAttr | (kTileItemName + i);
    dst[32 + i] = kTileAttr | (kTileItemName + 8 + i);
  }
}

void HudIt_DrawAbility(uint16 *dst, int ability, int level, bool wide) {
  static const char *const kAbilities[6] = { "ALZA", "LEGGI", "PARLA", "TIRA", "CORRI", "NUOTA" };
  char buf[8];
  const char *s = kAbilities[ability];
  int lv = level < 1 ? 1 : level > 3 ? 3 : level;
  // Lettere larghe solo se le colonne sono staccate (riquadro a 6 colonne):
  // le parole di 5 lettere riempiono le 5 caselle. ALZA.N usa la casella ".N"
  // di LIFT.N (0x127-0x129)
  if (wide) {
    DrawWide(dst, 5, s, 0x2c00, false);
    if (ability == 0)
      dst[4] = 0x2c00 | (0x127 + lv - 1);
    return;
  }
  // con le colonne attaccate restano strette: le parole di 5 lettere
  // riempiono le 5 caselle e si attaccano a quelle accanto
  if (ability == 0) {
    // ALZA.1, ALZA.2, ALZA.3 come LIFT.1-3: il livello dei guanti
    memcpy(buf, "ALZA.1", 7);
    buf[5] = '0' + lv;
    s = buf;
  }
  // nel mezzo delle 5 caselle: le parole di 5 lettere restano staccate
  int tile = kTileAbilities + ability * 5, n = (int)countof(kNarrow);
  int x = (40 - TextWidth(kNarrow, n, s)) / 2;
  ClearTiles(tile, 5);
  DrawText(tile, 5, kNarrow, n, 6, s, x < 0 ? 0 : x, 1);
  for (int i = 0; i < 5; i++)
    dst[i] = 0x2c00 | (tile + i);  // palette 3, come le abilita' del gioco
}

// ---------------------------------------------------------------------------
// Scritte disegnate nella grafica del gioco: "VITA" al posto di "LIFE"
// nell'HUD e "MAPPA" al posto di "MAP" nella mappa dei labirinti. Le caselle
// si cambiano solo mentre la PPU disegna (HudIt_BeforeRender) e poi si
// rimettono (HudIt_AfterRender): la VRAM del gioco non cambia.

// "LIFE" nelle caselle 0xAB-0xAC (2 bit per pixel, colore 2 lettere, 3 ombra)
static const uint16 kLifeTiles[16] = {
  0x6622, 0x6622, 0x6622, 0x6622, 0x6622, 0x7e0a, 0x7e7e, 0x0000,
  0xff11, 0xff77, 0xff11, 0xff77, 0xcc44, 0xcf41, 0xcfcf, 0x0000,
};
// "VITA" disegnata da zero come "LIFE": lettere alte 6 con un tratto di 1
// pixel, ombra a destra e sotto
static const char *const kVitaRows[6] = {
  ".#.#.#..###..#..",
  ".#.#.#...#..#.#.",
  ".#.#.#...#..#.#.",
  ".#.#.#...#..###.",
  ".#.#.#...#..#.#.",
  "..#..#...#..#.#.",
};

static bool VitaPixel(int x, int y) {
  return x >= 0 && x < 16 && y >= 0 && y < 6 && kVitaRows[y][x] == '#';
}

static void MakeVitaTiles(uint16 out[16]) {
  memset(out, 0, 16 * sizeof(uint16));
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 16; x++) {
      int p = VitaPixel(x, y) ? 2 :
              (VitaPixel(x - 1, y) || VitaPixel(x, y - 1) || VitaPixel(x - 1, y - 1)) ? 3 : 0;
      uint16 *w = &out[(x >> 3) * 8 + y];
      int b = 7 - (x & 7);
      *w |= (p & 1) << b | (p >> 1) << (b + 8);
    }
}

// Cartiglio "MAP" della mappa dei labirinti: 6 x 2 caselle (0x102-0x107 e
// 0x112-0x117), lettere larghe 7 con 3 colonne fra l'una e l'altra (M 10-16,
// A 20-26, P 30-36). "MAPPA" usa le colonne di quelle lettere, con fra le
// lettere 2 colonne invece di 3 (64 pixel, quanto l'interno della cornice)
static const uint8 kMappaCols[64] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,  // coda e M
  17, 19, 20, 21, 22, 23, 24, 25, 26,                       // A
  27, 29, 30, 31, 32, 33, 34, 35, 36,                       // P
  27, 29, 30, 31, 32, 33, 34, 35, 36,                       // P
  17, 19, 20, 21, 22, 23, 24, 25, 26,                       // A
  37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,               // coda
};

// "LIVELLO" disegnata da zero come "LEVEL" (caselle 0x132-0x14a): lettere
// piene larghe 2 pixel, alte 10, con 1 pixel di contorno tutto intorno
// (colore 2 pieno, 3 contorno) e una colonna di contorno fra le lettere
static const char *const kLivelloGlyphs[7][10] = {
  { "##..", "##..", "##..", "##..", "##..", "##..", "##..", "##..", "####", "####" },
  { "##", "##", "##", "##", "##", "##", "##", "##", "##", "##" },
  { "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", ".####.", ".####.", "..##..", "..##.." },
  { "#####", "#####", "##...", "##...", "####.", "####.", "##...", "##...", "#####", "#####" },
  { "##..", "##..", "##..", "##..", "##..", "##..", "##..", "##..", "####", "####" },
  { "##..", "##..", "##..", "##..", "##..", "##..", "##..", "##..", "####", "####" },
  { ".####.", "######", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "######", ".####." },
};

// 2 caselle di VITA, 16 di MAPPA e 16 di LIVELLO, 3 x 2 del piano, 4 x 2
// mezze caselle delle lettere e delle etichette dei piani nella mappa
enum { kItMaxSaved = 96 };
static uint16 g_it_saved_chr[kItMaxSaved][8], g_it_saved_chr_adr[kItMaxSaved];
static uint16 g_it_saved_map[kItMaxSaved], g_it_saved_map_adr[kItMaxSaved];
static int g_it_saved_chr_n, g_it_saved_map_n;

static void SaveChr(Ppu *ppu, int adr) {
  if (g_it_saved_chr_n >= kItMaxSaved)
    return;
  g_it_saved_chr_adr[g_it_saved_chr_n] = adr;
  memcpy(g_it_saved_chr[g_it_saved_chr_n++], &ppu->vram[adr], 8 * sizeof(uint16));
}

static void SetMap(Ppu *ppu, int adr, uint16 v) {
  if (g_it_saved_map_n >= kItMaxSaved)
    return;
  g_it_saved_map_adr[g_it_saved_map_n] = adr;
  g_it_saved_map[g_it_saved_map_n++] = ppu->vram[adr];
  ppu->vram[adr] = v;
}

// Voce della mappa dello strato 3 alla casella a
static uint16 *Bg3Map(Ppu *ppu, int a) {
  return &ppu->vram[(ppu->bgLayer[2].tilemapAdr + a) & 0x7fff];
}

// Pixel (x, y) di una voce della mappa dello strato 3 (con i ribaltamenti)
static int Bg3Pixel(Ppu *ppu, uint16 entry, int x, int y) {
  if (entry & 0x4000) x = 7 - x;
  if (entry & 0x8000) y = 7 - y;
  uint16 w = ppu->vram[(ppu->bgLayer[2].tileAdr + (entry & 0x3ff) * 8 + y) & 0x7fff];
  int b = 7 - x;
  return (w >> b & 1) | (w >> (b + 8) & 1) << 1;
}

// Prima casella della mappa dello strato 3 con le caselle top[0..n-1] e
// sotto bottom[0..n-1], o -1 (map entries compared under mask)
static int FindBg3Words(Ppu *ppu, const uint16 *top, const uint16 *bottom, int n, uint16 mask) {
  for (int a = 32; a < 32 * 31; a++) {
    if (a % 32 < 1 || a % 32 + n > 31)
      continue;
    int i = 0;
    while (i < n && (*Bg3Map(ppu, a + i) & mask) == top[i] && (*Bg3Map(ppu, a + 32 + i) & mask) == bottom[i])
      i++;
    if (i == n)
      return a;
  }
  return -1;
}

// Mette le prime cols colonne di caselle dell'immagine img (16 pixel di
// altezza, valori 0-3) in cols x 2 caselle dello strato 3 a partire dalla
// casella a, con l'attributo attr, usando caselle che nessuna voce della
// mappa mostra (rimesse da HudIt_AfterRender)
static void PutBg3Tiles(Ppu *ppu, int a, uint16 attr, uint8 img[16][64], int cols) {
  int n_tiles = cols * 2;
  static uint8 used[512];
  memset(used, 0, sizeof(used));
  for (int i = 0; i < 32 * 32; i++)
    if ((*Bg3Map(ppu, i) & 0x3ff) < 512)
      used[*Bg3Map(ppu, i) & 0x3ff] = 1;
  int tileadr = ppu->bgLayer[2].tileAdr, tiles[16], n = 0;
  // solo caselle dello strato 3 (prima della fine della VRAM)
  for (int t = 1; t < 512 && n < n_tiles; t++)
    if (!used[t] && tileadr + t * 8 + 8 <= 0x8000)
      tiles[n++] = t;
  if (n < n_tiles)
    return;
  for (int k = 0; k < n_tiles; k++) {
    int r = k / cols, c = k % cols, adr = tileadr + tiles[k] * 8;
    SaveChr(ppu, adr);
    for (int y = 0; y < 8; y++) {
      uint16 w = 0;
      for (int x = 0; x < 8; x++) {
        int p = img[r * 8 + y][c * 8 + x], b = 7 - x;
        w |= (p & 1) << b | (p >> 1) << (b + 8);
      }
      ppu->vram[adr + y] = w;
    }
  }
  for (int k = 0; k < n_tiles; k++)
    SetMap(ppu, (ppu->bgLayer[2].tilemapAdr + a + (k / cols) * 32 + k % cols) & 0x7fff, attr | tiles[k]);
}

static void BannerToMappa(Ppu *ppu) {
  static const uint16 kTop[6] = { 0x102, 0x103, 0x104, 0x105, 0x106, 0x107 };
  static const uint16 kBottom[6] = { 0x112, 0x113, 0x114, 0x115, 0x116, 0x117 };
  int a = FindBg3Words(ppu, kTop, kBottom, 6, 0x3ff);
  if (a < 0)
    return;
  uint8 img[16][64];
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 64; x++) {
      int sx = kMappaCols[x];
      img[y][x] = Bg3Pixel(ppu, *Bg3Map(ppu, a + (y >> 3) * 32 + (sx >> 3)), sx & 7, y & 7);
    }
  // una casella piu' a sinistra: 8 caselle dove ce n'erano 6
  PutBg3Tiles(ppu, a - 1, *Bg3Map(ppu, a) & ~0x3ff, img, 8);
}

static void LevelToLivello(Ppu *ppu) {
  static const uint16 kTop[4] = { 0x132, 0x133, 0x138, 0x13a };
  static const uint16 kBottom[4] = { 0x142, 0x143, 0x149, 0x14a };
  int a = FindBg3Words(ppu, kTop, kBottom, 4, 0x3ff);
  if (a < 0)
    return;
  // "LIVELLO" (righe 5-14 come "LEVEL"), 4 pixel e il numero (la casella
  // dopo quella vuota), centrati nelle 8 x 2 caselle da una a sinistra
  uint8 fill[16][64];
  memset(fill, 0, sizeof(fill));
  int x = 1;
  for (int i = 0; i < 7; i++) {
    int w = (int)strlen(kLivelloGlyphs[i][0]);
    for (int r = 0; r < 10; r++)
      for (int c = 0; c < w; c++)
        if (kLivelloGlyphs[i][r][c] == '#')
          fill[5 + r][x + c] = 1;
    x += w + 1;
  }
  int text_w = x;  // con il contorno ai due lati
  int left = (64 - (text_w + 4 + 8)) / 2;
  uint8 img[16][64];
  memset(img, 0, sizeof(img));
  for (int y = 0; y < 16; y++)
    for (int c = 0; c < text_w; c++) {
      bool near = false;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int yy = y + dy, xx = c + dx;
          near |= yy >= 0 && yy < 16 && xx >= 0 && xx < 64 && fill[yy][xx];
        }
      img[y][left + c] = fill[y][c] ? 2 : near ? 3 : 0;
    }
  int num_x = left + text_w + 4;
  for (int y = 0; y < 16; y++)
    for (int c = 0; c < 8; c++)
      img[y][num_x + c] = Bg3Pixel(ppu, *Bg3Map(ppu, a + 5 + (y >> 3) * 32), c, y & 7);
  PutBg3Tiles(ppu, a - 1, *Bg3Map(ppu, a) & ~0x3ff, img, 8);
}

// Indicatore del piano sotto i cuori (Hud_FloorIndicator): "1F" e "B1"
// diventano come sulla GBA "PT" (piano terra), "1oP" e "PS1". Le cifre sono
// quelle del gioco; P, T, S e o sono disegnate come le lettere B e F delle
// stesse caselle (aste di 2 pixel, colore 2, contorno 3 sui quattro lati).
// Con tre caratteri l'indicatore si allarga di una casella a sinistra.
static const uint16 kFloorGfx0[10] = { 0x2508, 0x2509, 0x2509, 0x250a, 0x250b, 0x250c, 0x250d, 0x251d, 0xe51c, 0x250e };
static const uint16 kFloorGfx1[10] = { 0x2518, 0x2519, 0xa509, 0x251a, 0x251b, 0x251c, 0x2518, 0xa51d, 0xe50c, 0xa50e };

// Lettere piene alte 14 righe (righe 1-14 delle caselle), larghe fino a 8
typedef struct FloorGlyph {
  char c;
  const char *rows[14];
} FloorGlyph;

static const FloorGlyph kFloorGlyphs[] = {
  { 'P', { ".#####", ".######", ".##..##", ".##..##", ".##..##", ".##..##", ".######", ".#####",
           ".##", ".##", ".##", ".##", ".##", ".##" } },
  { 'T', { ".######", ".######", "...##", "...##", "...##", "...##", "...##", "...##",
           "...##", "...##", "...##", "...##", "...##", "...##" } },
  { 'S', { "..####", ".######", ".##..##", ".##", ".##", ".##", ".#####", "..#####",
           ".....##", ".....##", ".....##", ".##..##", ".######", "..####" } },
  // o: anello sottile in alto, largo 6 con il contorno
  { 'o', { ".##", "#..#", "#..#", ".##", "", "", "", "", "", "", "", "", "", "" } },
};

static void PutFloorGlyph(uint8 fill[16][64], char c, int x0) {
  for (size_t i = 0; i < countof(kFloorGlyphs); i++)
    if (kFloorGlyphs[i].c == c)
      for (int r = 0; r < 14; r++)
        for (int x = 0; kFloorGlyphs[i].rows[r][x]; x++)
          if (kFloorGlyphs[i].rows[r][x] == '#')
            fill[1 + r][x0 + x] = 1;
}

static void FloorIndicatorToItalian(Ppu *ppu) {
  // Niente controllo del timer: quando arriva a 0 le caselle restano ancora
  // nella VRAM (Hud_RemoveSuperBombIndicator non chiede di aggiornarle) e
  // si vedeva l'inglese. Basta riconoscere le caselle del piano attuale
  int f = (int8)dung_cur_floor, n = f >= 0 ? f : -f;
  if (n > 9)
    return;
  uint16 top[2], bottom[2];
  if (f >= 0) {
    top[0] = kFloorGfx0[f], top[1] = 0x250f, bottom[0] = kFloorGfx1[f], bottom[1] = 0x251f;
  } else {
    top[0] = 0x251e, top[1] = kFloorGfx0[n - 1], bottom[0] = 0x2520, bottom[1] = kFloorGfx1[n - 1];
  }
  int a = FindBg3Words(ppu, top, bottom, 2, 0xffff);
  if (a < 0)
    return;
  static uint8 fill[16][64], img[16][64];
  memset(fill, 0, sizeof(fill));
  memset(img, 0, sizeof(img));
  int cols, digit_x = -1;
  if (f == 0) {
    cols = 2;
    PutFloorGlyph(fill, 'P', 0);
    PutFloorGlyph(fill, 'T', 8);
  } else if (f > 0) {
    cols = 3, digit_x = 1;     // 1 + cifra 8 + o 6 + P 8 + 1
    PutFloorGlyph(fill, 'o', 9);
    PutFloorGlyph(fill, 'P', 14);
  } else {
    cols = 3, digit_x = 16;
    PutFloorGlyph(fill, 'P', 0);
    PutFloorGlyph(fill, 'S', 8);
  }
  int w = cols * 8;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < w; x++) {
      bool near = (y > 0 && fill[y - 1][x]) || (y < 15 && fill[y + 1][x]) ||
                  (x > 0 && fill[y][x - 1]) || (x < w - 1 && fill[y][x + 1]);
      img[y][x] = fill[y][x] ? 2 : near ? 3 : 0;
    }
  if (digit_x >= 0) {
    uint16 dt = kFloorGfx0[n - 1], db = kFloorGfx1[n - 1];
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 8; x++) {
        int p = Bg3Pixel(ppu, y < 8 ? dt : db, x, y & 7);
        if (p)
          img[y][digit_x + x] = p;
      }
  }
  PutBg3Tiles(ppu, cols == 3 ? a - 1 : a, *Bg3Map(ppu, a) & ~0x3ff & ~0xc000, img, cols);
}

// Piani della mappa dei labirinti come sulla GBA: a sinistra gli sprite di
// DungeonMap_DrawFloorNumberObjects (due caselle 8x8 a x 0x30 e 0x38), a
// destra le voci dello sfondo di DungeonMap_DrawFloorNumbersByRoom. Le
// caselle 0x1c-0x25 della seconda tavola degli sprite (B, F e le cifre)
// sono le stesse dello sfondo 0x31c-0x325. P, T, S e o si disegnano nello
// stesso stile (colore 6, alte 7, aste di 3 o 2 pixel) in quattro caselle
// che in quel fotogramma nessuno usa; le etichette di tre caratteri si
// allargano di una casella a sinistra (lo sprite in piu' e' una voce OAM
// nascosta). A destra la casella a sinistra e' sotto la cornice: le
// etichette di tre caratteri stanno nelle stesse due caselle con lettere e
// cifre strette (kMapNarrow). OAM e caselle si rimettono dopo il disegno.
static const char *const kMapGlyphs[4][7] = {
  { ".666666.", ".666..66", ".666..66", ".666666.", ".666....", ".666....", ".666...." },  // P
  { ".6666666", "...666..", "...666..", "...666..", "...666..", "...666..", "...666.." },  // T
  { "..66666.", ".66...66", ".66.....", "..66666.", "......66", ".66...66", "..66666." },  // S
  { "...666..", "..66.66.", "..66.66.", "...666..", "", "", "" },                           // o
};
enum { kMapP, kMapT, kMapS, kMapOrd, kMapGlyph = 0x100 };

// Strette per le etichette di destra di tre caratteri (16 pixel)
typedef struct MapNarrowGlyph {
  char c;
  const char *rows[7];
} MapNarrowGlyph;
static const MapNarrowGlyph kMapNarrow[] = {
  { 'P', { "###.", "##.#", "##.#", "###.", "##..", "##..", "##.." } },
  { 'S', { ".###", "##..", "##..", ".##.", "..##", "..##", "###." } },
  { 'o', { ".##.", "#..#", "#..#", ".##.", "", "", "" } },
  { '1', { ".##.", "###.", ".##.", ".##.", ".##.", ".##.", "####" } },
  { '2', { ".###.", "##.##", "...##", "..##.", ".##..", "##...", "#####" } },
  { '3', { "####.", "...##", "...##", ".###.", "...##", "...##", "####." } },
  { '4', { "...##", "..###", ".#.##", "#..##", "#####", "...##", "...##" } },
  { '5', { "#####", "##...", "####.", "...##", "...##", "##.##", ".###." } },
  { '6', { ".###.", "##...", "####.", "##.##", "##.##", "##.##", ".###." } },
  { '7', { "#####", "...##", "..##.", "..##.", ".##..", ".##..", ".##.." } },
  { '8', { ".###.", "##.##", "##.##", ".###.", "##.##", "##.##", ".###." } },
};

// Scrive una casella 8x8 a 4 bit con i pixel img[y][x0..x0+7] (colori)
static void WriteObjTile4(Ppu *ppu, int adr, uint8 img[7][16], int x0) {
  memset(&ppu->vram[adr], 0, 16 * sizeof(uint16));
  for (int y = 0; y < 7; y++)
    for (int x = 0; x < 8; x++) {
      int c = img[y][x0 + x], b = 7 - x;
      ppu->vram[adr + y] |= (c & 1) << b | (c >> 1 & 1) << (b + 8);
      ppu->vram[adr + 8 + y] |= (c >> 2 & 1) << b | (c >> 3 & 1) << (b + 8);
    }
}

// Etichetta stretta di tre caratteri in 16 x 7 pixel (colore 6), da x = 1
static void MakeNarrowLabel(const char *text, uint8 img[7][16]) {
  memset(img, 0, 7 * 16);
  int x = 1;
  for (int i = 0; text[i]; i++)
    for (size_t g = 0; g < countof(kMapNarrow); g++)
      if (kMapNarrow[g].c == text[i]) {
        int w = (int)strlen(kMapNarrow[g].rows[0]);
        for (int y = 0; y < 7; y++)
          for (int c = 0; kMapNarrow[g].rows[y][c]; c++)
            if (kMapNarrow[g].rows[y][c] == '#' && x + c < 16)
              img[y][x + c] = 6;
        x += w + 1;
        break;
      }
}

static uint16 g_map_saved_oam[0x110];
static bool g_map_oam_changed;

static int ObjTileVram(Ppu *ppu, int tile9) {
  int base = (tile9 & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
  return (base + (tile9 & 0xff) * 16) & 0x7fff;
}

static int OamHigh(Ppu *ppu, int i) {
  return ppu->oam[0x100 + (i >> 3)] >> ((i & 7) * 2) & 3;
}

// Etichetta di un piano dalle due caselle (0x1c = B, 0x1d = F, 0x1e-0x25 =
// cifre 1-8): n caratteri in out (kMapGlyph | lettera, o la casella della cifra)
static int MapFloorLabel(int left, int right, int out[3]) {
  if (left == 0x1c && right >= 0x1e && right <= 0x25) {
    out[0] = kMapGlyph | kMapP, out[1] = kMapGlyph | kMapS, out[2] = right;  // PS1
    return 3;
  }
  if (right == 0x1d && left == 0x1e) {
    out[0] = kMapGlyph | kMapP, out[1] = kMapGlyph | kMapT;  // PT
    return 2;
  }
  if (right == 0x1d && left > 0x1e && left <= 0x25) {
    out[0] = left - 1, out[1] = kMapGlyph | kMapOrd, out[2] = kMapGlyph | kMapP;  // 1oP
    return 3;
  }
  return 0;
}

static bool IsMapLabelSprite(uint16 q) {
  return (q & 0x1ff) >= 0x11c && (q & 0x1ff) <= 0x125;
}

static void FloorLabelsToItalian(Ppu *ppu) {
  if (ppu->objXValid || ppu->objYValid)
    return;
  int bg_tile_adr = ppu->bgLayer[0].tileAdr;
  if (ppu->bgLayer[1].tileAdr != bg_tile_adr)
    return;
  // Etichette di sinistra: coppie di sprite 8x8 della seconda tavola
  int pairs[16][2], np = 0;
  for (int i = 0; i < 128 && np < 16; i++) {
    uint16 p = ppu->oam[2 * i], q = ppu->oam[2 * i + 1];
    if ((p & 0xff) != 0x30 || OamHigh(ppu, i) || !IsMapLabelSprite(q))
      continue;
    for (int j = 0; j < 128; j++) {
      uint16 p2 = ppu->oam[2 * j], q2 = ppu->oam[2 * j + 1];
      if (j != i && (p2 & 0xff) == 0x38 && (p2 >> 8) == (p >> 8) && !OamHigh(ppu, j) &&
          (q2 & ~0x1ff) == (q & ~0x1ff) && IsMapLabelSprite(q2)) {
        int out[3];
        if (MapFloorLabel(q & 0xff, q2 & 0xff, out))
          pairs[np][0] = i, pairs[np++][1] = j;
        break;
      }
    }
  }
  // Etichette di destra: due voci vicine dello sfondo (B + cifra o cifra + F)
  int bg_pos[8][2], nb = 0;
  for (int l = 0; l < 2; l++) {
    BgLayer *bg = &ppu->bgLayer[l];
    int words = 1024 << (bg->tilemapWider + bg->tilemapHigher);
    for (int a = 1; a + 1 < words && nb < 8; a++) {
      int t0 = ppu->vram[(bg->tilemapAdr + a) & 0x7fff] & 0x3ff;
      int t1 = ppu->vram[(bg->tilemapAdr + a + 1) & 0x7fff] & 0x3ff;
      int out[3];
      if (t0 >= 0x31c && t0 <= 0x325 && t1 >= 0x31c && t1 <= 0x325 && (a & 31) != 0 &&
          MapFloorLabel(t0 - 0x300, t1 - 0x300, out))
        bg_pos[nb][0] = l, bg_pos[nb++][1] = a;
    }
  }
  if (np == 0 && nb == 0)
    return;
  // Quattro caselle della seconda tavola degli sprite che nessuno usa,
  // raggiungibili anche dallo sfondo (caselle degli strati 1 e 2 sotto 1024)
  static uint8 used[0x8000 / 16];
  memset(used, 0, sizeof(used));
  static const uint8 kSizes[8][2] = { {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32} };
  for (int i = 0; i < 128; i++) {
    uint16 p = ppu->oam[2 * i], q = ppu->oam[2 * i + 1];
    if ((p >> 8) == 0xf0)
      continue;
    int n = kSizes[ppu->objSize & 7][OamHigh(ppu, i) >> 1] / 8;
    for (int r = 0; r < n; r++)
      for (int c = 0; c < n; c++) {
        int t = (q & 0x100) | ((q + r * 16) & 0xf0) | ((q + c) & 0xf);
        used[ObjTileVram(ppu, t) >> 4] = 1;
      }
  }
  for (int l = 0; l < 3; l++) {
    BgLayer *bg = &ppu->bgLayer[l];
    int words = 1024 << (bg->tilemapWider + bg->tilemapHigher);
    int size = l == 2 ? 8 : 16;  // modo 1: strato 3 a 2 bit
    for (int a = 0; a < words; a++) {
      int t = ppu->vram[(bg->tilemapAdr + a) & 0x7fff] & 0x3ff;
      int adr = (bg->tileAdr + t * size) & 0x7fff;
      used[adr >> 4] = 1;
      used[((adr + size - 1) & 0x7fff) >> 4] = 1;
    }
  }
  // 4 lettere e 2 caselle per ogni etichetta stretta di destra
  int glyph_obj[4 + 2 * 8], ng = 0, need = 4 + 2 * nb;
  for (int k = 0x26; k < 0x100 && ng < need; k++) {
    int adr = ObjTileVram(ppu, 0x100 | k);
    int bt = ((adr - bg_tile_adr) & 0x7fff) >> 4;
    if (!used[adr >> 4] && bt < 1024 && !((adr - bg_tile_adr) & 15))
      glyph_obj[ng++] = k;
  }
  if (ng < need)
    return;
  for (int g = 0; g < need; g++) {
    int adr = ObjTileVram(ppu, 0x100 | glyph_obj[g]);
    SaveChr(ppu, adr);
    SaveChr(ppu, adr + 8);
  }
  for (int g = 0; g < 4; g++) {
    uint8 img[7][16];
    memset(img, 0, sizeof(img));
    for (int y = 0; y < 7; y++)
      for (int x = 0; kMapGlyphs[g][y][x]; x++)
        if (kMapGlyphs[g][y][x] != '.')
          img[y][x] = kMapGlyphs[g][y][x] - '0';
    WriteObjTile4(ppu, ObjTileVram(ppu, 0x100 | glyph_obj[g]), img, 0);
  }
  // sfondo: PT con le lettere larghe, le altre strette nelle stesse due voci
  for (int i = 0; i < nb; i++) {
    BgLayer *bg = &ppu->bgLayer[bg_pos[i][0]];
    int a = bg_pos[i][1];
    uint16 e0 = ppu->vram[(bg->tilemapAdr + a) & 0x7fff];
    uint16 e1 = ppu->vram[(bg->tilemapAdr + a + 1) & 0x7fff];
    int out[3], n = MapFloorLabel((e0 & 0x3ff) - 0x300, (e1 & 0x3ff) - 0x300, out);
    int tiles[2];
    if (n == 2) {
      for (int c = 0; c < 2; c++)
        tiles[c] = glyph_obj[out[c] & 0xff];
    } else {
      char text[4];
      for (int c = 0; c < 3; c++)
        text[c] = out[c] & kMapGlyph ? "PTSo"[out[c] & 0xff] : '1' + out[c] - 0x1e;
      text[3] = 0;
      uint8 img[7][16];
      MakeNarrowLabel(text, img);
      for (int c = 0; c < 2; c++) {
        tiles[c] = glyph_obj[4 + 2 * i + c];
        WriteObjTile4(ppu, ObjTileVram(ppu, 0x100 | tiles[c]), img, c * 8);
      }
    }
    for (int c = 0; c < 2; c++) {
      int t = ((ObjTileVram(ppu, 0x100 | tiles[c]) - bg_tile_adr) & 0x7fff) >> 4;
      SetMap(ppu, (bg->tilemapAdr + a + c) & 0x7fff, (e0 & 0xfc00) | t);
    }
  }
  // sprite: la terza casella e' una voce OAM nascosta, la prima libera: il
  // riquadro del piano evidenziato e' fatto di sprite che vengono dopo
  if (np == 0)
    return;
  memcpy(g_map_saved_oam, ppu->oam, sizeof(g_map_saved_oam));
  g_map_oam_changed = true;
  int spare = 0;
  for (int k = 0; k < np; k++) {
    int i = pairs[k][0], j = pairs[k][1];
    uint16 q = ppu->oam[2 * i + 1];
    int out[3], n = MapFloorLabel(q & 0xff, ppu->oam[2 * j + 1] & 0xff, out);
    int idx[3] = { i, j, -1 };
    if (n == 3) {
      while (spare < 128 && ((ppu->oam[2 * spare] >> 8) != 0xf0 || ppu->objScale[spare] ||
                            ppu->objIce[spare] || ppu->objUnderWater[spare] ||
                            ppu->objAsBg[spare] || ppu->objCastShadow[spare]))
        spare++;
      if (spare >= 128)
        continue;
      idx[2] = spare++;
    }
    int y = ppu->oam[2 * i] >> 8, x0 = n == 3 ? 0x2c : 0x30;
    for (int c = 0; c < n; c++) {
      int t = out[c] & kMapGlyph ? glyph_obj[out[c] & 0xff] : out[c];
      int s = idx[c];
      ppu->oam[2 * s] = (x0 + c * 8) | y << 8;
      ppu->oam[2 * s + 1] = (q & ~0xff) | t;
      ppu->oam[0x100 + (s >> 3)] &= ~(3 << ((s & 7) * 2));
    }
  }
}

void HudIt_BeforeRender(Ppu *ppu) {
  g_it_saved_chr_n = g_it_saved_map_n = 0;
  g_map_oam_changed = false;
  ScreensIt_BeforeRender(ppu);
  if (!HudIt_Active() || ppu->mode != 1)
    return;
  int tileadr = ppu->bgLayer[2].tileAdr;
  int life = (tileadr + 0xab * 8) & 0x7fff;
  if (life + 16 <= 0x8000 && !memcmp(&ppu->vram[life], kLifeTiles, sizeof(kLifeTiles))) {
    SaveChr(ppu, life);
    SaveChr(ppu, life + 8);
    MakeVitaTiles(&ppu->vram[life]);
  }
  if (main_module_index == 14 && submodule_index == 3) {
    BannerToMappa(ppu);
    LevelToLivello(ppu);
    FloorLabelsToItalian(ppu);
  }
  FloorIndicatorToItalian(ppu);
}

void HudIt_AfterRender(Ppu *ppu) {
  if (g_map_oam_changed) {
    memcpy(ppu->oam, g_map_saved_oam, sizeof(g_map_saved_oam));
    g_map_oam_changed = false;
  }
  ScreensIt_AfterRender(ppu);
  while (g_it_saved_map_n > 0) {
    g_it_saved_map_n--;
    ppu->vram[g_it_saved_map_adr[g_it_saved_map_n]] = g_it_saved_map[g_it_saved_map_n];
  }
  while (g_it_saved_chr_n > 0) {
    g_it_saved_chr_n--;
    memcpy(&ppu->vram[g_it_saved_chr_adr[g_it_saved_chr_n]], g_it_saved_chr[g_it_saved_chr_n], 8 * sizeof(uint16));
  }
}
