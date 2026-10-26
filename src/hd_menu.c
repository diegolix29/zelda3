// Menu opzioni in gioco (tasto Esc o clic della levetta destra), rimappatura
// dei tasti, salvataggi rapidi e salvataggi automatici ai cambi di area.
#include <SDL.h>
#include "hd_menu.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "zelda_rtl.h"
#include "variables.h"
#include "config.h"
#include "features.h"
#include "util.h"
#include "snes/ppu.h"
#include "hd_light.h"
#include "hd_fx.h"
#include "hd_water.h"
#include "hd_hud.h"
#include "hd_3d.h"
#include "hd_art.h"

void ZeldaApuLock();
void ZeldaApuUnlock();

enum {
  kPage_Main,
  kPage_Graphics,
  kPage_Game,
  kPage_Audio,
  kPage_Controls,
  kPage_Saves,
};

enum {
  kNumQuickSlots = 20,
  kMaxItems = 40,
  kRowHeight = 16,
};

typedef struct MenuItem {
  char label[64];
  char value[64];
  int id;
} MenuItem;

// Testi del menu: scritti in italiano, in inglese con le altre lingue
static const char *Tr(const char *it);
static bool IsItalian();
static void UpdateDungeonName(void);

static bool g_menu_open;
static int g_page;
static int g_cursor[6];
static int g_scroll;
static int g_capture = -1;        // indice del comando in attesa di un tasto
static bool g_capture_pad;
static bool g_slot_action_save;
static char g_message[96];
static int g_message_timer;
static bool g_last_input_pad;  // ultimo tasto premuto sul pad (non sulla tastiera)
static bool g_controls_dirty, g_msu_dirty, g_lang_dirty;
static bool g_msu_on;
static uint8 g_msu_mode;  // modalita' dell'ini (deluxe, opuz...) da riscrivere riaccendendo la musica
static uint8 g_last_stick;

static MenuItem g_items[kMaxItems];
static int g_num_items;

// ---------------------------------------------------------------------------
// Opzioni del gioco controllate da un bit di features0

typedef struct FeatureOption {
  const char *label;
  const char *ini_key;
  uint32 bit;
} FeatureOption;

static const FeatureOption kFeatureOptions[] = {
  { "Transizioni veloci", "FastTransitions", kFeatures0_FastTransitions },
  { "Mondo continuo all'esterno", "ContinuousWorld", kFeatures0_ContinuousWorld },
  { "HUD moderno", "ModernHud", kFeatures0_ModernHud },
  { "Oggetti X, L, R nell'HUD", "ShowXLRItemsInHud", kFeatures0_ShowXLRItemsInHud },
  { "Nomi dei labirinti", "DungeonNames", kFeatures0_DungeonNames },
  { "Mappa migliorata", "ImprovedWorldMap", kFeatures0_ImprovedWorldMap },
  { "Mappa dei labirinti migliorata", "ImprovedDungeonMap", kFeatures0_ImprovedDungeonMap },
  { "Mappa progressiva", "ProgressiveMap", kFeatures0_ProgressiveMap },
  { "Cambio oggetti con L e R", "ItemSwitchLR", kFeatures0_SwitchLR },
  { "L/R solo sui primi 4 oggetti", "ItemSwitchLRLimit", kFeatures0_SwitchLRLimit },
  { "Inventario esteso", "ExtendedInventory", kFeatures0_ExtendedInventory },
  { "Spada e oggetti interazione ambiente", "SwordAndItemsInteraction", kFeatures0_SwordAndItems },
  { "Oggetti riempiono le risorse", "ItemsRefillResources", kFeatures0_ItemsRefillResources },
  { "Pozione d'oro", "GoldPotion", kFeatures0_GoldPotion },
  { "Interazione bruciatura e congelamento", "BurnAndFreezeInteraction", kFeatures0_BurnAndFreeze },
  { "Sterza durante lo scatto", "TurnWhileDashing", kFeatures0_TurnWhileDashing },
  { "Fino a 10 bombe insieme", "MoreActiveBombs", kFeatures0_MoreActiveBombs },
  { "Massimi in giallo nell'HUD", "ShowMaxItemsInYellow", kFeatures0_ShowMaxItemsInYellow },
  { "Borsa pi\xc3\xb9 grande", "CarryMoreRupees", kFeatures0_CarryMoreRupees },
  { "Disattiva bip vita bassa", "DisableLowHealthBeep", kFeatures0_DisableLowHealthBeep },
  { "Salva stato cuori e magia", "SaveHeartsAndMagic", kFeatures0_SaveHeartsAndMagic },
  { "Annulla il volo con X", "CancelBirdTravel", kFeatures0_CancelBirdTravel },
  { "Punto di volo 9", "BirdTravelPoint9", kFeatures0_BirdTravel9 },
  { "Specchio verso il Mondo delle Tenebre", "MirrorToDarkworld", kFeatures0_MirrorToDarkworld },
  { "Correzioni di bug", "MiscBugFixes", kFeatures0_MiscBugFixes },
  { "Correzioni maggiori", "GameChangingBugFixes", kFeatures0_GameChangingBugFixes },
};

static const char *const kControlNames[12] = {
  "Su", "Gi\xc3\xb9", "Sinistra", "Destra", "Select", "Start", "A", "B", "X", "Y", "L", "R",
};

static const char *const kSmoothCameraNames[4] = { "Spenta", "Leggera", "Media", "Forte" };
static const char *const kView3dNames[kHd3d_NumLevels] = { "Spenta", "20\xc2\xb0", "35\xc2\xb0", "50\xc2\xb0", "65\xc2\xb0", "75\xc2\xb0" };
static const char *const kView3dCurveNames[kHd3d_NumCurves] = { "Spenta", "Leggera", "Media", "Forte" };
static const char *const kView3dDistNames[kHd3d_NumDists] = { "Schermo", "2x", "3x" };
static const char *const kTextSpeedNames[3] = { "Originale", "Veloce con A o B", "Istantanea" };

// ---------------------------------------------------------------------------
// Testo: si usa il font dei dialoghi del gioco (con le lettere accentate
// italiane se e' caricata la lingua italiana).

static const uint8 *g_font_data, *g_font_width;
static int g_font_count;

static void Font_Load() {
  MemBlk data = FindIndexInMemblk(g_zenv.dialogue_font_blk, 0);
  MemBlk width = FindIndexInMemblk(g_zenv.dialogue_font_blk, 1);
  g_font_data = data.ptr;
  g_font_width = width.ptr;
  g_font_count = (int)width.size;
}

// Restituisce l'indice del glifo per un carattere unicode, o -1
static int Font_GlyphFor(uint32 c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  bool has_it = g_font_count >= 105;
  switch (c) {
  case '!': return 62;
  case '?': return 63;
  case '-': return 64;
  case '.': return 65;
  case ',': return 66;
  case '>': return 68;
  case '(': return 69;
  case ')': return 70;
  case '"': return 76;
  case '\'': return 81;
  case ' ': return 89;
  case '<': return 90;
  case 0x2191: return 77;  // freccia su
  case 0x2193: return 78;  // freccia giu'
  case 0xe0: return has_it ? 99 : 26;    // à
  case 0xe8: return has_it ? 100 : 30;   // è
  case 0xe9: return has_it ? 101 : 30;   // é
  case 0xec: return has_it ? 102 : 34;   // ì
  case 0xf2: return has_it ? 103 : 40;   // ò
  case 0xf9: return has_it ? 104 : 46;   // ù
  }
  return -1;
}

// Glifi di riserva per i simboli che il font del gioco non ha (5x8, 1 bit)
static const uint8 *Font_Fallback(uint32 c) {
  static const uint8 kPlus[8] = { 0, 0x04, 0x04, 0x1f, 0x04, 0x04, 0, 0 };
  static const uint8 kColon[8] = { 0, 0x06, 0x06, 0, 0x06, 0x06, 0, 0 };
  static const uint8 kSlash[8] = { 0x10, 0x10, 0x08, 0x04, 0x04, 0x02, 0x01, 0x01 };
  static const uint8 kPercent[8] = { 0x13, 0x13, 0x08, 0x04, 0x04, 0x02, 0x19, 0x19 };
  static const uint8 kUnderscore[8] = { 0, 0, 0, 0, 0, 0, 0, 0x1f };
  static const uint8 kEquals[8] = { 0, 0, 0x1f, 0, 0x1f, 0, 0, 0 };
  static const uint8 kBox[8] = { 0x1f, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1f };
  switch (c) {
  case '+': return kPlus;
  case ':': return kColon;
  case '/': case '\\': return kSlash;
  case '%': return kPercent;
  case '_': return kUnderscore;
  case '=': return kEquals;
  }
  return kBox;
}

static uint32 Utf8Next(const char **ps) {
  const uint8 *s = (const uint8 *)*ps;
  uint32 c = *s++;
  if (c >= 0xc0 && c < 0xe0 && (*s & 0xc0) == 0x80)
    c = (c & 0x1f) << 6 | (*s++ & 0x3f);
  else if (c >= 0xe0 && c < 0xf0 && (s[0] & 0xc0) == 0x80 && (s[1] & 0xc0) == 0x80)
    c = (c & 0xf) << 12 | (s[0] & 0x3f) << 6 | (s[1] & 0x3f), s += 2;
  *ps = (const char *)s;
  return c;
}

typedef struct Canvas {
  uint8 *pixels;
  int pitch, width, height, scale;
} Canvas;

static void Canvas_Pixel(Canvas *cv, int x, int y, uint32 color) {
  if ((unsigned)x >= (unsigned)cv->width || (unsigned)y >= (unsigned)cv->height)
    return;
  int s = cv->scale;
  for (int yy = 0; yy < s; yy++) {
    uint32 *row = (uint32 *)(cv->pixels + (y * s + yy) * cv->pitch);
    for (int xx = 0; xx < s; xx++)
      row[x * s + xx] = color | 0x02000000;  // alpha 2 = layer HUD: lo shader non lo dipinge
  }
}

static void Canvas_Rect(Canvas *cv, int x, int y, int w, int h, uint32 color) {
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      Canvas_Pixel(cv, xx, yy, color);
}

// Disegna un carattere e restituisce la sua larghezza
static int DrawChar(Canvas *cv, int x, int y, uint32 c, uint32 color) {
  int g = Font_GlyphFor(c);
  if (g >= 0 && g < g_font_count && g_font_data) {
    int w = g_font_width[g];
    int tile = (g & 0x70) * 2 + (g & 0xf);
    for (int half = 0; half < 2; half++) {
      const uint8 *t = g_font_data + (tile + half * 16) * 16;
      for (int row = 0; row < 8; row++) {
        uint8 d0 = t[row * 2], d1 = t[row * 2 + 1];
        for (int col = 0; col < 8; col++) {
          int bit = 7 - col;
          int v = (d0 >> bit & 1) | (d1 >> bit & 1) << 1;
          if (v == 1)
            Canvas_Pixel(cv, x + col, y + half * 8 + row, 0x101018);
          else if (v >= 2)
            Canvas_Pixel(cv, x + col, y + half * 8 + row, color);
        }
      }
    }
    return w;
  }
  const uint8 *f = Font_Fallback(c);
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 5; col++)
      if (f[row] >> (4 - col) & 1)
        Canvas_Pixel(cv, x + col, y + 5 + row, color);
  return 7;
}

static int TextWidth(const char *s) {
  int w = 0;
  while (*s) {
    uint32 c = Utf8Next(&s);
    int g = Font_GlyphFor(c);
    w += (g >= 0 && g < g_font_count && g_font_data) ? g_font_width[g] : 7;
  }
  return w;
}

// Forma breve delle voci piu' lunghe, per quando il pannello e' stretto (4:3)
static const char *ShortLabel(const char *s) {
  static const char *const kShort[][2] = {
    { "Specchio verso il Mondo delle Tenebre", "Specchio verso M. delle Tenebre" },
    { "Spada e oggetti interazione ambiente", "Spada e oggetti: ambiente" },
    { "Interazione bruciatura e congelamento", "Bruciatura e congelamento" },
  };
  for (int i = 0; i < countof(kShort); i++)
    if (strcmp(s, kShort[i][0]) == 0)
      return kShort[i][1];
  return s;
}

static int DrawText(Canvas *cv, int x, int y, const char *s, uint32 color) {
  while (*s)
    x += DrawChar(cv, x, y, Utf8Next(&s), color);
  return x;
}

// ---------------------------------------------------------------------------
// Salvataggi

static void QuickSlotName(int i, char *buf, size_t size) {
  snprintf(buf, size, "saves/save%d.sav", i);
}

static bool FileTime(const char *name, time_t *t) {
  struct stat st;
  if (stat(name, &st) != 0)
    return false;
  *t = st.st_mtime;
  return true;
}

static void FormatFileTime(const char *name, char *buf, size_t size) {
  time_t t;
  if (!FileTime(name, &t)) {
    snprintf(buf, size, "%s", Tr("vuoto"));
    return;
  }
  struct tm *tm = localtime(&t);
  if (IsItalian())
    snprintf(buf, size, "%02d.%02d  ore %02d.%02d", tm->tm_mday, tm->tm_mon + 1, tm->tm_hour, tm->tm_min);
  else
    snprintf(buf, size, "%02d/%02d  %02d:%02d", tm->tm_mday, tm->tm_mon + 1, tm->tm_hour, tm->tm_min);
}

static void ShowMessage(const char *msg) {
  snprintf(g_message, sizeof(g_message), "%s", Tr(msg));
  g_message_timer = 150;
}

void HdMenu_ShowMessage(const char *msg) {
  ShowMessage(msg);
}

void HdMenu_AfterFrame(bool is_replay) {
  UpdateDungeonName();
  if (g_message_timer)
    g_message_timer--;
}

static void QuickSlotAction(int slot) {
  char name[64];
  time_t t;
  QuickSlotName(slot, name, sizeof(name));
  if (!g_slot_action_save && !FileTime(name, &t)) {
    ShowMessage("Lo slot e' vuoto");
    return;
  }
  ZeldaApuLock();
  SaveLoadSlot(g_slot_action_save ? kSaveLoad_Save : kSaveLoad_Load, slot);
  ZeldaApuUnlock();
  char msg[64];
  snprintf(msg, sizeof(msg), Tr(g_slot_action_save ? "Salvato nello slot %d" : "Slot %d caricato"), slot + 1);
  ShowMessage(msg);
  if (!g_slot_action_save) {
    HdMenu_Close();
  }
}

// ---------------------------------------------------------------------------
// Impostazioni

void HdMenu_ApplyConfig() {
  g_smooth_camera = g_config.smooth_camera;
  g_text_speed = g_config.text_speed;
  g_msu_on = g_config.enable_msu != 0;
  if (g_msu_on)
    g_msu_mode = g_config.enable_msu;
  g_dynamic_light = g_config.dynamic_light;
  g_fx_heat = g_config.fx_heat, g_fx_window_rays = g_config.fx_window_rays;
  g_fx_item_glow = g_config.fx_item_glow;
  g_fx_rain = g_config.fx_rain;
  g_fx_menu_blur = g_config.fx_menu_blur, g_fx_sprite_shadows = g_config.fx_sprite_shadows;
  g_fx_soft_iris = g_config.fx_soft_iris, g_fx_button_hints = g_config.fx_button_hints;
  g_fx_whirlpool = g_config.fx_whirlpool;
  g_fx_fog = g_config.fx_fog;
  g_fx_water = g_config.fx_water;
}

static bool IsItalian() {
  return g_config.language && StringEqualsNoCase(g_config.language, "it");
}

static const char *Tr(const char *it) {
  static const char *const kTr[][2] = {
    { "Transizioni veloci", "Fast transitions" },
    { "Mondo continuo all'esterno", "Continuous overworld" },
    { "HUD moderno", "Modern HUD" },
    { "Oggetti X, L, R nell'HUD", "X, L, R items in the HUD" },
    { "Richiede Cambio oggetti con L e R", "Requires Switch items with L and R" },
    { "Richiede la PPU ottimizzata (NewRenderer)", "Requires the optimized PPU (NewRenderer)" },
    { "Nomi dei labirinti", "Dungeon names" },
    { "Cambio oggetti con L e R", "Switch items with L and R" },
    { "Spada e oggetti interazione ambiente", "Sword and items interaction" },
    { "Oggetti riempiono le risorse", "Items refill resources" },
    { "Interazione bruciatura e congelamento", "Burn and freeze interaction" },
    { "Inventario esteso", "Extended inventory" },
    { "Mappa migliorata", "Improved map" },
    { "Mappa dei labirinti migliorata", "Improved dungeon map" },
    { "Mappa progressiva", "Progressive map" },
    { "Sterza durante lo scatto", "Turn while dashing" },
    { "Fino a 10 bombe insieme", "Up to 10 bombs at once" },
    { "Massimi in giallo nell'HUD", "Maximums in yellow in the HUD" },
    { "Disattiva bip vita bassa", "Disable low health beep" },
    { "Salva stato cuori e magia", "Save hearts and magic" },
    { "Annulla il volo con X", "Cancel bird travel with X" },
    { "Punto di volo 9", "Bird travel point 9" },
    { "Specchio verso il Mondo delle Tenebre", "Mirror to the Dark World" },
    { "Correzioni di bug", "Bug fixes" },
    { "Correzioni maggiori", "Game changing bug fixes" },
    { "L/R solo sui primi 4 oggetti", "Limit L/R to the first 4 items" },
    { "Borsa pi\xc3\xb9 grande", "Larger wallet" },
    { "Su", "Up" },
    { "Gi\xc3\xb9", "Down" },
    { "Sinistra", "Left" },
    { "Destra", "Right" },
    { "Spenta", "Off" },
    { "Leggera", "Light" },
    { "Media", "Medium" },
    { "Forte", "Strong" },
    { "Originale", "Original" },
    { "Veloce con A o B", "Fast with A or B" },
    { "Istantanea", "Instant" },
    { "Grafica", "Graphics" },
    { "Gioco", "Game" },
    { "Controlli", "Controls" },
    { "Salvataggi", "Saves" },
    { "Opzioni", "Options" },
    { "Riprendi", "Resume" },
    { "Esci dal gioco", "Quit game" },
    { "Telecamera fluida", "Smooth camera" },
    { "Vista 3D (inclinazione)", "3D view (tilt)" },
    { "Curvatura del mondo 3D", "3D world curve" },
    { "Distanza di disegno 3D", "3D draw distance" },
    { "Muri 3D al chiuso", "3D indoor walls" },
    { "Schermo", "Screen" },
    { "Schermo intero", "Fullscreen" },
    { "Mappa del mondo in HD", "HD world map" },
    { "Mostra FPS", "Show FPS" },
    { "Testo nitido nei dialoghi", "Sharp dialogue text" },
    { "Fluidit\xc3\xa0 oltre i 60 Hz", "Smoothness above 60 Hz" },
    { "Illuminazione dinamica", "Dynamic lighting" },
    { "Effetto calore e fiamme", "Heat and flames effect" },
    { "Raggi dalle finestre", "Light rays from windows" },
    { "Bagliore degli oggetti ricevuti", "Glow of received items" },
    { "Pioggia e lampi migliorati", "Improved rain and lightning" },
    { "Nebbia migliorata", "Improved fog" },
    { "Acqua e cascate realistiche", "Realistic water and waterfalls" },
    { "Sfocatura dietro menu e dialoghi", "Blur behind menus and dialogues" },
    { "Ombre di personaggi e nemici", "Shadows of characters and enemies" },
    { "Cerchio di transizione morbido", "Soft transition circle" },
    { "Suggerimenti dei tasti", "Button hints" },
    { "Distingui vortici da Zora", "Tell whirlpools from Zoras" },
    { "Aspetto dei disegni ufficiali", "Official artwork look" },
    { "Link biondo", "Blond Link" },
    { "Immersione con le pinne", "Dive with the flippers" },
    { "Frecce piantate nei muri", "Arrows stuck in walls" },
    { "Giravolta Tornado", "Hurricane Spin" },
    { "Solo 16:9 e 16:10", "Only 16:9 and 16:10" },
    { "Link personalizzato", "Custom Link" },
    { "Non con un Link personalizzato", "Not with a custom Link" },
    { "Richiede lo schermo 16:9 o 16:10", "Requires a 16:9 or 16:10 screen" },
    { "Velocit\xc3\xa0 del testo", "Text speed" },
    { "Lingua dei dialoghi", "Dialogue language" },
    { "Musica orchestrale MSU", "MSU orchestral music" },
    { "(al riavvio)", "(on restart)" },
    { "Menu opzioni", "Options menu" },
    { "Ripristina tasti predefiniti", "Restore default keys" },
    { "Azione sugli slot", "Slot action" },
    { "Salva", "Save" },
    { "Carica", "Load" },
    { "vuoto", "empty" },
    { "S\xc3\xac", "Yes" },
    { "Lo slot e' vuoto", "The slot is empty" },
    { "Salvato nello slot %d", "Saved to slot %d" },
    { "Slot %d caricato", "Slot %d loaded" },
    { "Tasti predefiniti ripristinati", "Default keys restored" },
    { "Scegli", "Select" },
    { "Chiudi", "Close" },
    { "Riordina", "Sort" },
    { "Segnalino", "Marker" },
    { "Centra su Link", "Center on Link" },
    { "Veloce", "Fast" },
    { "Riduci", "Zoom out" },
    { "Ingrandisci", "Zoom in" },
    { "Cambia piano", "Change floor" },
    { "Croce", "D-pad" },
    { "Frecce", "Arrows" },
    { "Fogne del castello", "Sewers" },
    { "Castello di Hyrule", "Hyrule Castle" },
    { "Palazzo dell'Est", "Eastern Palace" },
    { "Castello di Sabbia", "Desert Palace" },
    { "Sala superiore del castello", "Agahnim's Tower" },
    { "Dedalo d'Acqua", "Swamp Palace" },
    { "Palazzo Cupo", "Palace of Darkness" },
    { "Malapalude", "Misery Mire" },
    { "Bosco d'Ossa", "Skull Woods" },
    { "Forte Ghiaccio", "Ice Palace" },
    { "Torre sul Monte", "Tower of Hera" },
    { "Tana dei Ladroni", "Thieves' Town" },
    { "Rocca Tartaruga", "Turtle Rock" },
    { "Torre di Ganon", "Ganon's Tower" },
    { "Premi il nuovo tasto o pulsante  (Esc o Back annulla)", "Press the new key or button  (Esc or Back cancels)" },
    { "A conferma   B indietro   Sinistra e Destra cambiano", "A confirm   B back   Left and Right change" },
    { "Segnalino tolto", "Marker removed" },
    { "Troppi segnalini, togline qualcuno", "Too many markers, remove some" },
    { "Segnalino aggiunto", "Marker added" },
    { "A segnalino   Y centra su Link   tieni B per scorrere veloce", "A marker   Y center on Link   hold B to scroll fast" },
  };
  if (IsItalian())
    return it;
  for (int i = 0; i < countof(kTr); i++)
    if (!strcmp(it, kTr[i][0]))
      return kTr[i][1];
  return it;
}

static void SetFeature(uint32 bit, bool on) {
  g_config.features0 = on ? (g_config.features0 | bit) : (g_config.features0 & ~bit);
  g_wanted_zelda_features = g_config.features0;
}

static void AddIni(IniValue *v, int *n, const char *section, const char *key, const char *fmt, int value) {
  v[*n].section = section;
  v[*n].key = key;
  snprintf(v[*n].value, sizeof(v[*n].value), fmt, value);
  (*n)++;
}

static void SaveSettings() {
  IniValue v[64];
  int n = 0;
  AddIni(v, &n, "[Graphics]", "SmoothCamera", "%d", g_config.smooth_camera);
  AddIni(v, &n, "[Graphics]", "View3D", "%d", g_config.view3d);
  AddIni(v, &n, "[Graphics]", "View3DCurve", "%d", g_config.view3d_curve);
  AddIni(v, &n, "[Graphics]", "View3DDrawDistance", "%d", g_config.view3d_dist);
  AddIni(v, &n, "[Graphics]", "View3DWalls", "%d", g_config.view3d_walls);
  AddIni(v, &n, "[Graphics]", "EnhancedMode7", "%d", g_config.enhanced_mode7);
  AddIni(v, &n, "[Graphics]", "HdText", "%d", g_config.hd_text);
  AddIni(v, &n, "[Graphics]", "HighRefreshInterpolation", "%d", g_config.high_refresh);
  AddIni(v, &n, "[Graphics]", "DynamicLighting", "%d", g_config.dynamic_light);
  AddIni(v, &n, "[Graphics]", "FxHeatAndFlames", "%d", g_config.fx_heat);
  AddIni(v, &n, "[Graphics]", "FxWindowRays", "%d", g_config.fx_window_rays);
  AddIni(v, &n, "[Graphics]", "FxItemGlow", "%d", g_config.fx_item_glow);
  AddIni(v, &n, "[Graphics]", "FxRain", "%d", g_config.fx_rain);
  AddIni(v, &n, "[Graphics]", "FxForestFog", "%d", g_config.fx_fog);
  AddIni(v, &n, "[Graphics]", "FxWater", "%d", g_config.fx_water);
  AddIni(v, &n, "[Graphics]", "FxMenuBlur", "%d", g_config.fx_menu_blur);
  AddIni(v, &n, "[Graphics]", "FxSpriteShadows", "%d", g_config.fx_sprite_shadows);
  AddIni(v, &n, "[Graphics]", "FxSoftIris", "%d", g_config.fx_soft_iris);
  AddIni(v, &n, "[Graphics]", "FxButtonHints", "%d", g_config.fx_button_hints);
  AddIni(v, &n, "[Graphics]", "FxWhirlpoolGlow", "%d", g_config.fx_whirlpool);
  AddIni(v, &n, "[Graphics]", "OfficialArtwork", "%d", g_config.official_artwork);
  AddIni(v, &n, "[Graphics]", "BlondLink", "%d", g_config.blond_link);
  AddIni(v, &n, "[Features]", "FlipperDive", "%d", g_config.flipper_dive);
  AddIni(v, &n, "[Features]", "StuckArrows", "%d", g_config.stuck_arrows);
  AddIni(v, &n, "[Features]", "HurricaneSpin", "%d", g_config.hurricane_spin);
  // La lingua si scrive solo se cambiata dal menu (altrimenti resta quella
  // dell'ini, anche de, fr...)
  if (g_lang_dirty) {
    v[n].section = "[General]", v[n].key = "Language";
    snprintf(v[n].value, sizeof(v[n].value), "%s", g_config.language ? g_config.language : "us"), n++;
  }
  for (int i = 0; i < countof(kFeatureOptions); i++)
    AddIni(v, &n, "[Features]", kFeatureOptions[i].ini_key, "%d", (g_config.features0 & kFeatureOptions[i].bit) != 0);
  AddIni(v, &n, "[Features]", "TextSpeed", "%d", g_config.text_speed);
  if (g_msu_dirty) {
    v[n].section = "[Sound]", v[n].key = "EnableMSU";
    const char *mode = "true";
    if ((g_msu_mode & (kMsuEnabled_MsuDeluxe | kMsuEnabled_Opuz)) == (kMsuEnabled_MsuDeluxe | kMsuEnabled_Opuz))
      mode = "deluxe-opuz";
    else if (g_msu_mode & kMsuEnabled_MsuDeluxe)
      mode = "deluxe";
    else if (g_msu_mode & kMsuEnabled_Opuz)
      mode = "opuz";
    snprintf(v[n].value, sizeof(v[n].value), "%s", g_msu_on ? mode : "false"), n++;
  }
  if (g_controls_dirty) {
    char buf[160], name[48];
    buf[0] = 0;
    for (int i = 0; i < 12; i++) {
      Config_KeyName(Config_GetKeyForCmd(kKeys_Controls + i), name, sizeof(name));
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", i ? ", " : "", strcmp(name, "-") ? name : "");
    }
    v[n].section = "[KeyMap]", v[n].key = "Controls";
    snprintf(v[n].value, sizeof(v[n].value), "%s", buf), n++;
    Config_KeyName(Config_GetKeyForCmd(kKeys_HdMenu), name, sizeof(name));
    v[n].section = "[KeyMap]", v[n].key = "HdMenu";
    snprintf(v[n].value, sizeof(v[n].value), "%s", name), n++;
    buf[0] = 0;
    for (int i = 0; i < 12; i++) {
      int b = Config_GetPadButtonForCmd(kKeys_Controls + i);
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", i ? ", " : "", b >= 0 ? Config_PadButtonName(b) : "");
    }
    v[n].section = "[GamepadMap]", v[n].key = "Controls";
    snprintf(v[n].value, sizeof(v[n].value), "%s", buf), n++;
    int b = Config_GetPadButtonForCmd(kKeys_HdMenu);
    v[n].section = "[GamepadMap]", v[n].key = "HdMenu";
    snprintf(v[n].value, sizeof(v[n].value), "%s", b >= 0 ? Config_PadButtonName(b) : ""), n++;
  }
  if (!Config_WriteIniValues(v, n))
    fprintf(stderr, "Impossibile salvare le impostazioni in %s\n", Config_GetFilename());
}

// ---------------------------------------------------------------------------
// Costruzione delle pagine

static MenuItem *AddItem(int id, const char *label, const char *value) {
  MenuItem *it = &g_items[g_num_items++];
  it->id = id;
  snprintf(it->label, sizeof(it->label), "%s", Tr(label));
  snprintf(it->value, sizeof(it->value), "%s", value ? Tr(value) : "");
  return it;
}

static const char *YesNo(bool b) {
  return b ? "S\xc3\xac" : "No";
}

static const char *PageTitle() {
  switch (g_page) {
  case kPage_Graphics: return "Grafica";
  case kPage_Game: return "Gioco";
  case kPage_Audio: return "Audio";
  case kPage_Controls: return "Controlli";
  case kPage_Saves: return "Salvataggi";
  }
  return "Opzioni";
}

static void BuildItems() {
  char buf[64], buf2[64];
  g_num_items = 0;
  switch (g_page) {
  case kPage_Main:
    AddItem(0, "Riprendi", NULL);
    AddItem(1, "Grafica", ">");
    AddItem(2, "Gioco", ">");
    AddItem(3, "Audio", ">");
    AddItem(4, "Controlli", ">");
    AddItem(5, "Salvataggi", ">");
    AddItem(6, "Esci dal gioco", NULL);
    break;
  case kPage_Graphics:
    AddItem(0, "Telecamera fluida", kSmoothCameraNames[g_config.smooth_camera & 3]);
    AddItem(1, "Schermo intero", YesNo(Host_IsFullscreen()));
    snprintf(buf, sizeof(buf), "%s%s%s", Tr(YesNo(g_config.enhanced_mode7)), Host_IsOpenGL() ? "" : " ", Host_IsOpenGL() ? "" : Tr("(al riavvio)"));
    AddItem(2, "Mappa del mondo in HD", buf);
    AddItem(3, "Mostra FPS", YesNo(Host_GetDisplayPerf()));
    AddItem(4, "Testo nitido nei dialoghi", YesNo(g_config.hd_text));
    AddItem(5, "Fluidit\xc3\xa0 oltre i 60 Hz", YesNo(g_config.high_refresh));
    AddItem(6, "Illuminazione dinamica", YesNo(g_config.dynamic_light));
    AddItem(7, "Effetto calore e fiamme", YesNo(g_config.fx_heat));
    AddItem(8, "Raggi dalle finestre", YesNo(g_config.fx_window_rays));
    AddItem(9, "Bagliore degli oggetti ricevuti", YesNo(g_config.fx_item_glow));
    AddItem(11, "Pioggia e lampi migliorati", YesNo(g_config.fx_rain));
    AddItem(10, "Nebbia migliorata", YesNo(g_config.fx_fog));
    AddItem(12, "Sfocatura dietro menu e dialoghi", YesNo(g_config.fx_menu_blur));
    AddItem(13, "Ombre di personaggi e nemici", YesNo(g_config.fx_sprite_shadows));
    AddItem(14, "Cerchio di transizione morbido", YesNo(g_config.fx_soft_iris));
    AddItem(15, "Suggerimenti dei tasti", g_zenv.ppu->extraLeftRight > 0 ? YesNo(g_config.fx_button_hints) : "Solo 16:9 e 16:10");
    AddItem(16, "Acqua e cascate realistiche", YesNo(g_config.fx_water));
    AddItem(17, "Distingui vortici da Zora", YesNo(g_config.fx_whirlpool));
    AddItem(18, "Aspetto dei disegni ufficiali", YesNo(g_config.official_artwork));
    AddItem(19, "Link biondo", g_config.link_graphics ? "Link personalizzato" : YesNo(g_config.blond_link));
    AddItem(18, "Vista 3D (inclinazione)", kView3dNames[g_view3d % kHd3d_NumLevels]);
    AddItem(19, "Curvatura del mondo 3D", kView3dCurveNames[g_view3d_curve % kHd3d_NumCurves]);
    AddItem(20, "Distanza di disegno 3D", kView3dDistNames[g_view3d_dist % kHd3d_NumDists]);
    AddItem(21, "Muri 3D al chiuso", YesNo(g_view3d_walls != 0));
    break;
  case kPage_Game:
    AddItem(100, "Velocit\xc3\xa0 del testo", kTextSpeedNames[g_config.text_speed % 3]);
    AddItem(101, "Lingua dei dialoghi", IsItalian() ? "Italiano" : "English");
    for (int i = 0; i < countof(kFeatureOptions); i++)
      AddItem(i, kFeatureOptions[i].label, YesNo((g_config.features0 & kFeatureOptions[i].bit) != 0));
    AddItem(102, "Immersione con le pinne", YesNo(g_config.flipper_dive));
    AddItem(103, "Frecce piantate nei muri", YesNo(g_config.stuck_arrows));
    AddItem(104, "Giravolta Tornado", YesNo(g_config.hurricane_spin));
    break;
  case kPage_Audio:
    snprintf(buf, sizeof(buf), "%d%%", Host_GetVolume());
    AddItem(0, "Volume", buf);
    snprintf(buf, sizeof(buf), "%s %s", Tr(YesNo(g_msu_on)), Tr("(al riavvio)"));
    AddItem(1, "Musica orchestrale MSU", buf);
    break;
  case kPage_Controls:
    for (int i = 0; i < 12; i++) {
      Config_KeyName(Config_GetKeyForCmd(kKeys_Controls + i), buf2, sizeof(buf2));
      snprintf(buf, sizeof(buf), "%s   -   %s", buf2, Config_PadButtonName(Config_GetPadButtonForCmd(kKeys_Controls + i)));
      AddItem(i, kControlNames[i], buf);
    }
    Config_KeyName(Config_GetKeyForCmd(kKeys_HdMenu), buf2, sizeof(buf2));
    snprintf(buf, sizeof(buf), "%s   -   %s", buf2, Config_PadButtonName(Config_GetPadButtonForCmd(kKeys_HdMenu)));
    AddItem(12, "Menu opzioni", buf);
    AddItem(13, "Ripristina tasti predefiniti", NULL);
    break;
  case kPage_Saves:
    AddItem(103, "Azione sugli slot", g_slot_action_save ? "Salva" : "Carica");
    for (int i = 0; i < kNumQuickSlots; i++) {
      char name[64];
      QuickSlotName(i, name, sizeof(name));
      FormatFileTime(name, buf2, sizeof(buf2));
      snprintf(buf, sizeof(buf), "Slot %d", i + 1);
      AddItem(i, buf, buf2);
    }
    break;
  }
}

// dir: -1/+1 per sinistra/destra, 0 per conferma
static void Activate(int id, int dir) {
  switch (g_page) {
  case kPage_Main:
    if (dir != 0)
      return;
    if (id == 0) HdMenu_Close();
    else if (id == 6) { HdMenu_Close(); Host_Quit(); }
    else { g_page = id; g_scroll = 0; }
    break;
  case kPage_Graphics:
    if (!g_config.new_renderer && (id == 0 || id == 5 || id == 6 || (id >= 7 && id <= 17))) {
      // come nel launcher: servono gli strati e lo spostamento della vista
      ShowMessage("Richiede la PPU ottimizzata (NewRenderer)");
    } else if (id == 0) {
      int v = (g_config.smooth_camera + (dir < 0 ? 3 : 1)) & 3;
      g_config.smooth_camera = v;
      g_smooth_camera = v;
    } else if (id == 1) {
      Host_ToggleFullscreen();
    } else if (id == 2 && !g_config.new_renderer) {
      ShowMessage("Richiede la PPU ottimizzata (NewRenderer)");
    } else if (id == 2) {
      g_config.enhanced_mode7 = !g_config.enhanced_mode7;
      if (Host_IsOpenGL())
        Host_SetPpuRenderFlags(Host_GetPpuRenderFlags() ^ kPpuRenderFlags_4x4Mode7);
    } else if (id == 3) {
      Host_SetDisplayPerf(!Host_GetDisplayPerf());
    } else if (id == 4) {
      g_config.hd_text = !g_config.hd_text;
    } else if (id == 5) {
      g_config.high_refresh = !g_config.high_refresh;
    } else if (id == 6) {
      g_config.dynamic_light = !g_config.dynamic_light;
      g_dynamic_light = g_config.dynamic_light;
    } else if (id == 18) {
      // Agahnim, re e medaglione dal prossimo caricamento
      g_config.official_artwork = !g_config.official_artwork;
    } else if (id == 19 && g_config.link_graphics) {
      ShowMessage("Non con un Link personalizzato");
    } else if (id == 19) {
      // Link e la sua palette subito
      g_config.blond_link = !g_config.blond_link;
      HdArt_Apply();
    } else if (id == 20) {
        Hd3d_SetLevel(g_view3d + (dir < 0 ? kHd3d_NumLevels - 1 : 1), "options menu");
    } else if (id == 21) {
        Hd3d_SetCurve(g_view3d_curve + (dir < 0 ? kHd3d_NumCurves - 1 : 1), "options menu");
    } else if (id == 22) {
        Hd3d_SetDrawDistance(g_view3d_dist + (dir < 0 ? kHd3d_NumDists - 1 : 1), "options menu");
    } else if (id == 15 && g_zenv.ppu->extraLeftRight == 0) {
      ShowMessage("Richiede lo schermo 16:9 o 16:10");
    } else if (id >= 7 && id <= 17) {
      bool *opt[11] = { &g_config.fx_heat, &g_config.fx_window_rays, &g_config.fx_item_glow,
                       &g_config.fx_fog, &g_config.fx_rain, &g_config.fx_menu_blur,
                       &g_config.fx_sprite_shadows, &g_config.fx_soft_iris, &g_config.fx_button_hints,
                       &g_config.fx_water, &g_config.fx_whirlpool };
      *opt[id - 7] = !*opt[id - 7];
      HdMenu_ApplyConfig();
    }
    break;
  case kPage_Game:
    if (id == 100) {
      g_config.text_speed = (g_config.text_speed + (dir < 0 ? 2 : 1)) % 3;
      g_text_speed = g_config.text_speed;
    } else if (id == 101) {
      g_config.language = IsItalian() ? "us" : "it";
      g_lang_dirty = true;
      ZeldaSetLanguage(g_config.language);
      Font_Load();
    } else if (id == 102) {
      g_config.flipper_dive = !g_config.flipper_dive;
    } else if (id == 103) {
      g_config.stuck_arrows = !g_config.stuck_arrows;
    } else if (id == 104) {
      g_config.hurricane_spin = !g_config.hurricane_spin;
    } else {
      uint32 bit = kFeatureOptions[id].bit;
      if ((bit == kFeatures0_ContinuousWorld || bit == kFeatures0_FastTransitions) && !g_config.new_renderer) {
        ShowMessage("Richiede la PPU ottimizzata (NewRenderer)");
        break;
      }
      // oggetti X, L, R nell'HUD solo con il cambio oggetti con L e R
      if ((bit == kFeatures0_ShowXLRItemsInHud || bit == kFeatures0_SwitchLRLimit) && !(g_config.features0 & kFeatures0_SwitchLR)) {
        ShowMessage("Richiede Cambio oggetti con L e R");
        break;
      }
      SetFeature(bit, !(g_config.features0 & bit));
      if (bit == kFeatures0_SwitchLR && !(g_config.features0 & bit))
        SetFeature(kFeatures0_ShowXLRItemsInHud, false), SetFeature(kFeatures0_SwitchLRLimit, false);
    }
    break;
  case kPage_Audio:
    if (id == 0)
      Host_AdjustVolume(dir < 0 ? -1 : 1);
    else if (id == 1)
      g_msu_on = !g_msu_on, g_msu_dirty = true;
    break;
  case kPage_Controls:
    if (dir != 0)
      return;
    if (id == 13) {
      Config_ResetControlsToDefault();
      g_controls_dirty = true;
      ShowMessage("Tasti predefiniti ripristinati");
    } else {
      g_capture = id;
    }
    break;
  case kPage_Saves:
    if (id == 103) {
      g_slot_action_save = !g_slot_action_save;
    } else if (dir == 0) {
      QuickSlotAction(id);
    }
    break;
  }
}

// ---------------------------------------------------------------------------
// Input

enum { kAct_None, kAct_Up, kAct_Down, kAct_Left, kAct_Right, kAct_Ok, kAct_Back, kAct_Close };

static void DoAction(int act) {
  BuildItems();
  int *cur = &g_cursor[g_page];
  if (*cur >= g_num_items) *cur = g_num_items - 1;
  if (*cur < 0) *cur = 0;
  switch (act) {
  case kAct_Up: *cur = (*cur + g_num_items - 1) % g_num_items; break;
  case kAct_Down: *cur = (*cur + 1) % g_num_items; break;
  case kAct_Left: Activate(g_items[*cur].id, -1); break;
  case kAct_Right: Activate(g_items[*cur].id, 1); break;
  case kAct_Ok: Activate(g_items[*cur].id, 0); break;
  case kAct_Back:
    if (g_page == kPage_Main) HdMenu_Close();
    else g_page = kPage_Main, g_scroll = 0;
    break;
  case kAct_Close: HdMenu_Close(); break;
  }
}

bool HdMenu_IsOpen() {
  return g_menu_open;
}

void HdMenu_Open() {
  if (g_menu_open)
    return;
  g_menu_open = true;
  g_page = kPage_Main;
  g_scroll = 0;
  g_capture = -1;
  Font_Load();
  Host_ClearInputs();
}

void HdMenu_Close() {
  if (!g_menu_open)
    return;
  g_menu_open = false;
  g_capture = -1;
  SaveSettings();
  g_controls_dirty = g_msu_dirty = false;
  Host_ClearInputs();
}

bool HdMenu_HandleKey(int code, int mod, bool pressed) {
  int cmd = FindCmdForSdlKey(code, mod);
  if (pressed)
    g_last_input_pad = false;
  if (!g_menu_open) {
    if (cmd == kKeys_HdMenu && pressed) {
      HdMenu_Open();
      return true;
    }
    return false;
  }
  if (!pressed)
    return true;
  if (g_capture >= 0) {
    if (code == SDLK_ESCAPE) {
      g_capture = -1;
    } else if (code != SDLK_LSHIFT && code != SDLK_RSHIFT && code != SDLK_LCTRL &&
               code != SDLK_RCTRL && code != SDLK_LALT && code != SDLK_RALT &&
               code != SDLK_COMMA) {  // la virgola separa i tasti nella riga Controls dell'ini
      Config_SetKeyForCmd(g_capture == 12 ? kKeys_HdMenu : kKeys_Controls + g_capture, code, mod);
      g_controls_dirty = true;
      g_capture = -1;
    }
    return true;
  }
  int act = kAct_None;
  switch (code) {
  case SDLK_UP: act = kAct_Up; break;
  case SDLK_DOWN: act = kAct_Down; break;
  case SDLK_LEFT: act = kAct_Left; break;
  case SDLK_RIGHT: act = kAct_Right; break;
  case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: act = kAct_Ok; break;
  case SDLK_ESCAPE: case SDLK_BACKSPACE: act = kAct_Back; break;
  default:
    if (cmd >= kKeys_Controls && cmd <= kKeys_Controls_Last) {
      static const uint8 kMap[12] = { kAct_Up, kAct_Down, kAct_Left, kAct_Right, kAct_None, kAct_Ok,
                                      kAct_Ok, kAct_Back, kAct_None, kAct_None, kAct_None, kAct_None };
      act = kMap[cmd - kKeys_Controls];
    } else if (cmd == kKeys_HdMenu) {
      act = kAct_Close;
    }
  }
  DoAction(act);
  return true;
}

bool HdMenu_HandlePadButton(int button, bool pressed) {
  if (pressed)
    g_last_input_pad = true;
  if (!g_menu_open)
    return false;
  if (!pressed)
    return true;
  if (g_capture >= 0) {
    if (button == kGamepadBtn_Back) {
      g_capture = -1;
    } else {
      Config_SetPadButtonForCmd(g_capture == 12 ? kKeys_HdMenu : kKeys_Controls + g_capture, button);
      g_controls_dirty = true;
      g_capture = -1;
    }
    return true;
  }
  int act = kAct_None;
  switch (button) {
  case kGamepadBtn_DpadUp: act = kAct_Up; break;
  case kGamepadBtn_DpadDown: act = kAct_Down; break;
  case kGamepadBtn_DpadLeft: act = kAct_Left; break;
  case kGamepadBtn_DpadRight: act = kAct_Right; break;
  case kGamepadBtn_A: case kGamepadBtn_Start: act = kAct_Ok; break;
  case kGamepadBtn_B: case kGamepadBtn_Back: act = kAct_Back; break;
  default:
    if (Config_GetPadButtonForCmd(kKeys_HdMenu) == button)
      act = kAct_Close;
  }
  DoAction(act);
  return true;
}

void HdMenu_HandleStick(uint8 dir_bits) {
  if (dir_bits)
    g_last_input_pad = true;
  if (!g_menu_open || g_capture >= 0) {
    g_last_stick = dir_bits;
    return;
  }
  uint8 newly = dir_bits & ~g_last_stick;
  g_last_stick = dir_bits;
  if (newly & 0x10) DoAction(kAct_Up);
  else if (newly & 0x20) DoAction(kAct_Down);
  else if (newly & 0x40) DoAction(kAct_Left);
  else if (newly & 0x80) DoAction(kAct_Right);
}

// ---------------------------------------------------------------------------
// Disegno

static void DrawMessage(Canvas *cv) {
  if (!g_message_timer || !g_message[0])
    return;
  int w = TextWidth(g_message) + 16;
  int x = (cv->width - w) / 2, y = cv->height - 28;
  Canvas_Rect(cv, x, y, w, 20, 0x182848);
  DrawText(cv, x + 8, y + 1, g_message, 0xf8f8f8);
}

// Pixel mescolato con quello sotto (a = opacita')
static void BlendPixel(Canvas *cv, int x, int y, uint32 c, float a) {
  if ((unsigned)x >= (unsigned)cv->width || (unsigned)y >= (unsigned)cv->height)
    return;
  for (int yy = 0; yy < cv->scale; yy++) {
    uint32 *row = (uint32 *)(cv->pixels + (y * cv->scale + yy) * cv->pitch) + x * cv->scale;
    for (int xx = 0; xx < cv->scale; xx++) {
      uint32 o = row[xx];
      int r = (int)((o >> 16 & 0xff) * (1 - a) + (c >> 16 & 0xff) * a);
      int g = (int)((o >> 8 & 0xff) * (1 - a) + (c >> 8 & 0xff) * a);
      int b = (int)((o & 0xff) * (1 - a) + (c & 0xff) * a);
      row[xx] = 0x02000000 | r << 16 | g << 8 | b;
    }
  }
}

// ---------------------------------------------------------------------------
// Suggerimenti dei tasti in inventario e mappe: mostrano il tasto davvero
// assegnato, della tastiera o del pad secondo l'ultimo dispositivo usato.

enum { kBtn_Dpad = -1, kBtn_Select = 4, kBtn_Start = 5, kBtn_A = 6, kBtn_B = 7, kBtn_X = 8, kBtn_Y = 9, kBtn_L = 10, kBtn_R = 11 };

static void ButtonName(int btn, char *buf, size_t size) {
  if (btn == kBtn_Dpad) {
    if (g_last_input_pad) {
      snprintf(buf, size, "%s", Tr("Croce"));
      return;
    }
    char k[4][48];
    for (int i = 0; i < 4; i++)
      Config_KeyName(Config_GetKeyForCmd(kKeys_Controls + i), k[i], sizeof(k[i]));
    if (!strcmp(k[0], "Up") && !strcmp(k[1], "Down") && !strcmp(k[2], "Left") && !strcmp(k[3], "Right"))
      snprintf(buf, size, "%s", Tr("Frecce"));
    else
      snprintf(buf, size, "%s %s %s %s", k[0], k[2], k[1], k[3]);
    return;
  }
  if (g_last_input_pad) {
    snprintf(buf, size, "%s", Config_PadButtonName(Config_GetPadButtonForCmd(kKeys_Controls + btn)));
    return;
  }
  char k[48];
  Config_KeyName(Config_GetKeyForCmd(kKeys_Controls + btn), k, sizeof(k));
  static const char *const kNames[][2] = {
    { "Return", "Invio" }, { "Right Shift", "Maiusc dx" }, { "Left Shift", "Maiusc sx" },
    { "Space", "Spazio" }, { "Backspace", "Canc" }, { "Right Ctrl", "Ctrl dx" }, { "Left Ctrl", "Ctrl sx" },
  };
  for (int i = 0; i < countof(kNames) && IsItalian(); i++)
    if (!strcmp(k, kNames[i][0])) {
      snprintf(buf, size, "%s", kNames[i][1]);
      return;
    }
  if (k[0] && !k[1] && k[0] >= 'a' && k[0] <= 'z')
    k[0] -= 32;  // lettere in maiuscolo, come sui tasti
  snprintf(buf, size, "%s", k);
}

typedef struct Hint {
  int btn;
  const char *label;
} Hint;

// Riquadro scuro semitrasparente con gli angoli smussati
static void DrawPanel(Canvas *cv, int x, int y, int w, int h, uint32 color, float a) {
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++) {
      bool corner = (yy == 0 || yy == h - 1) && (xx == 0 || xx == w - 1);
      if (!corner)
        BlendPixel(cv, x + xx, y + yy, color, a);
    }
}

static void DrawHints(Canvas *cv, const Hint *h, int n, int bottom) {
  char names[8][48];
  int total = 0;
  for (int i = 0; i < n; i++) {
    ButtonName(h[i].btn, names[i], sizeof(names[i]));
    total += TextWidth(names[i]) + 8 + 4 + TextWidth(Tr(h[i].label));
  }
  // spazio fra i suggerimenti: piu' stretto se la riga non entra nello schermo
  // (16:10 o 4:3 con nomi dei tasti lunghi)
  int gap = 14;
  if (n > 1 && total + (n - 1) * gap + 16 > cv->width)
    gap = IntMax(4, (cv->width - 16 - total) / (n - 1));
  total += (n - 1) * gap;
  int x = (cv->width - total) / 2, y = cv->height - bottom;
  DrawPanel(cv, x - 8, y - 2, total + 16, 20, 0x101020, 0.70f);
  for (int i = 0; i < n; i++) {
    if (i)
      x += gap;
    int kw = TextWidth(names[i]) + 8;
    DrawPanel(cv, x, y + 1, kw, 14, 0xe0e0e8, 1.0f);
    DrawPanel(cv, x + 1, y + 2, kw - 2, 12, 0x383848, 1.0f);
    DrawText(cv, x + 4, y - 1, names[i], 0xf8f8f8);
    x += kw + 4;
    x = DrawText(cv, x, y - 1, Tr(h[i].label), 0xf8e070);
  }
}

// Larghezza del riquadro piu' largo di una colonna di suggerimenti, con il
// margine interno pad
static int HintColumnWidth(const Hint *h, int n, int pad) {
  int w = 0;
  for (int i = 0; i < n; i++) {
    char name[48];
    ButtonName(h[i].btn, name, sizeof(name));
    int kw = TextWidth(name) + 8, lw = TextWidth(Tr(h[i].label));
    w = IntMax(w, IntMax(kw, lw) + pad);
  }
  return w;
}

// Suggerimenti in colonna ai lati dello schermo: per ognuno il tasto e sotto
// la sua azione, centrati nella fascia laterale
static void DrawHintColumn(Canvas *cv, const Hint *h, int n, int cx, int pad) {
  int y = cv->height - 12 - n * 40;
  for (int i = 0; i < n; i++, y += 40) {
    char name[48];
    ButtonName(h[i].btn, name, sizeof(name));
    int kw = TextWidth(name) + 8, lw = TextWidth(Tr(h[i].label));
    int pw = (kw > lw ? kw : lw) + pad;
    DrawPanel(cv, cx - pw / 2, y - 3, pw, 37, 0x101020, 0.70f);
    int kx = cx - kw / 2;
    DrawPanel(cv, kx, y, kw, 14, 0xe0e0e8, 1.0f);
    DrawPanel(cv, kx + 1, y + 1, kw - 2, 12, 0x383848, 1.0f);
    DrawText(cv, kx + 4, y - 2, name, 0xf8f8f8);
    DrawText(cv, cx - lw / 2, y + 15, Tr(h[i].label), 0xf8e070);
  }
}

// Contorno nero di 2 pixel attorno alle lettere dei tasti sul bordo sinistro
// dei riquadri dell'inventario (Y/X/L/R e la A rossa). Le lettere sono caselle
// dello strato 3: si cercano nella sua mappa (colonna 0) e se ne ricava la
// posizione sullo schermo dallo scorrimento, cosi' il contorno segue il menu
// anche mentre scende e risale.
static void DrawInventoryLetterOutlines(Canvas *cv) {
  if (main_module_index != 14 || submodule_index != 1)
    return;
  Ppu *ppu = g_zenv.ppu;
  BgLayer *bg = &ppu->bgLayer[2];
  int ext = (cv->width - 256) / 2;
  int rows = bg->tilemapHigher ? 64 : 32;
  for (int ty = 0; ty + 1 < rows; ty++) {
    int base = bg->tilemapAdr + (ty & 31) * 32 + ((ty & 32) ? (bg->tilemapWider ? 0x800 : 0x400) : 0);
    int base2 = bg->tilemapAdr + ((ty + 1) & 31) * 32 + (((ty + 1) & 32) ? (bg->tilemapWider ? 0x800 : 0x400) : 0);
    uint16 top = ppu->vram[base & 0x7fff], bot = ppu->vram[base2 & 0x7fff];
    int t = top & 0x3ff, b = bot & 0x3ff;
    bool letter = (t == 0xf0 && (b == 0xf1 || b == 0xf0 || b == 0xf2)) ||
                  (t == 0x0e && b == 0x0f) || (t == 0x0c && b == 0x0d);
    if (!letter)
      continue;
    // forma della lettera (8x16), dalle due caselle a 2 bit per pixel
    uint8 glyph[16][8];
    for (int half = 0; half < 2; half++) {
      uint16 e = half ? bot : top;
      bool hf = (e & 0x4000) != 0, vf = (e & 0x8000) != 0;
      for (int r = 0; r < 8; r++) {
        uint16 w = ppu->vram[(bg->tileAdr + (e & 0x3ff) * 8 + (vf ? 7 - r : r)) & 0x7fff];
        for (int c = 0; c < 8; c++) {
          int bit = hf ? c : 7 - c;
          glyph[half * 8 + r][c] = ((w >> bit) & 1) | ((w >> (8 + bit)) & 1);
        }
      }
    }
    int wrap = rows * 8;
    int sy = ((ty * 8 - bg->vScroll) % wrap + wrap) % wrap;
    if (sy >= 256)
      sy -= wrap;
    int sx = ext - (bg->hScroll & 0xff);
    for (int y = -2; y < 18; y++)
      for (int x = -2; x < 10; x++) {
        if (y >= 0 && y < 16 && x >= 0 && x < 8 && glyph[y][x])
          continue;
        bool near = false;
        for (int dy = -2; dy <= 2 && !near; dy++)
          for (int dx = -2; dx <= 2 && !near; dx++) {
            if (abs(dx) + abs(dy) > 3)
              continue;  // angoli arrotondati
            int gy = y + dy, gx = x + dx;
            near = gy >= 0 && gy < 16 && gx >= 0 && gx < 8 && glyph[gy][gx];
          }
        if (near)
          Canvas_Pixel(cv, sx + x, sy + y, 0x000000);
      }
  }
}

// I suggerimenti stanno nelle fasce del widescreen (16:9, 16:10): in 4:3 non
// c'e' posto e coprirebbero il menu o uscirebbero dallo schermo
bool HdMenu_ButtonHintsOn(void) {
  return g_config.fx_button_hints && g_zenv.ppu->extraLeftRight > 0;
}

static void DrawButtonHints(Canvas *cv) {
  if (!HdMenu_ButtonHintsOn() || main_module_index != 14)
    return;
  // il tasto della mappa e' X, oppure Select se a X e' assegnato un oggetto
  int map_btn = hud_cur_item_x != 0 ? kBtn_Select : kBtn_X;
  int sub = submodule_index;
  if (sub == 1) {  // inventario: ai lati, due e due, per non coprire i riquadri
    Hint left[2] = { { kBtn_Dpad, "Scegli" }, { kBtn_Start, "Chiudi" } };
    Hint right[2];
    int nr = 0;
    if (sram_progress_indicator)
      right[nr++] = (Hint){ kBtn_Select, "Salva" };
    if (enhanced_features0 & kFeatures0_SwitchLR)
      right[nr++] = (Hint){ kBtn_Y, "Riordina" };
    // Le colonne stanno nelle fasce laterali: in 16:9 (71 px) con il margine
    // interno pieno, in 16:10 (51 px) con un margine piu' stretto. In 4:3 non
    // c'e' spazio: niente suggerimenti (sotto coprirebbero i riquadri)
    int side = (cv->width - 256) / 2, pad = 12;
    int need = IntMax(HintColumnWidth(left, 2, pad), HintColumnWidth(right, nr, pad));
    if (need > side - 2)
      pad = 4, need = IntMax(HintColumnWidth(left, 2, pad), HintColumnWidth(right, nr, pad));
    if (need <= side - 2) {
      DrawHintColumn(cv, left, 2, side / 2, pad);
      if (nr)
        DrawHintColumn(cv, right, nr, cv->width - side / 2, pad);
    }
  } else if (sub == 7 && overworld_map_state == 5) {  // mappa del mondo (non mentre si schiarisce)
    if ((overworld_map_flags & 1) && (enhanced_features0 & kFeatures0_ImprovedWorldMap)) {
      Hint h[4] = { { kBtn_A, "Segnalino" }, { kBtn_Y, "Centra su Link" }, { kBtn_B, "Veloce" }, { kBtn_L, "Riduci" } };
      DrawHints(cv, h, 4, 22);
    } else if (overworld_map_flags & 1) {
      Hint h[1] = { { kBtn_L, "Riduci" } };
      DrawHints(cv, h, 1, 22);
    } else {
      Hint h[2] = { { kBtn_L, "Ingrandisci" }, { map_btn, "Chiudi" } };
      DrawHints(cv, h, 2, 22);
    }
  } else if (sub == 3 && overworld_map_state >= 2 && overworld_map_state <= 4) {  // mappa del labirinto
    Hint h[2] = { { kBtn_Dpad, "Cambia piano" }, { map_btn, "Chiudi" } };
    DrawHints(cv, h, 2, 22);  // in basso al centro (il nome del labirinto sta in alto)
  }
}

// Nome del labirinto mostrato sulla mappa del dungeon, in alto al centro (fa
// parte della mappa migliorata, decisione dell'utente)
static void DrawDungeonMapStatus(Canvas *cv) {
  static const char *const kDungeonNames[14] = {
    "Fogne del castello", "Castello di Hyrule", "Palazzo dell'Est", "Castello di Sabbia",
    "Sala superiore del castello", "Dedalo d'Acqua", "Palazzo Cupo", "Malapalude",
    "Bosco d'Ossa", "Forte Ghiaccio", "Torre sul Monte", "Tana dei Ladroni",
    "Rocca Tartaruga", "Torre di Ganon",
  };
  if (!(enhanced_features0 & kFeatures0_ImprovedWorldMap))
    return;
  if (main_module_index != 14 || submodule_index != 3 || overworld_map_state < 2 || overworld_map_state > 4)
    return;
  int dung = cur_palace_index_x2 >> 1;
  if (dung < 0 || dung >= 14)
    return;
  const char *name = Tr(kDungeonNames[dung]);
  int w = TextWidth(name);
  int x = (cv->width - w) / 2;
  Canvas_Rect(cv, x - 8, 4, w + 16, 18, 0x101830);
  DrawText(cv, x, 4, name, 0xf8f8f8);
}

// Nome del dungeon al centro dello schermo appena Link ci entra dal mondo
// esterno, come su GBA; sparisce appena Link si muove. Carattere ricavato
// dalla versione GBA: maiuscole alte 10, minuscole 8, contorno nero.
static const char *const kGbaFont[] = {
  "A.##.|#..#|#..#|#..#|####|#..#|#..#|#..#|#..#|#..#",
  "B###.|#..#|#..#|#..#|###.|#..#|#..#|#..#|#..#|###.",
  "C.##.|#..#|#...|#...|#...|#...|#...|#...|#..#|.##.",
  "D###.|#..#|#..#|#..#|#..#|#..#|#..#|#..#|#..#|###.",
  "E####|#...|#...|#...|###.|#...|#...|#...|#...|####",
  "F####|#...|#...|#...|###.|#...|#...|#...|#...|#...",
  "G.##.|#..#|#...|#...|#...|#.##|#..#|#..#|#..#|.###",
  "H#..#|#..#|#..#|#..#|####|#..#|#..#|#..#|#..#|#..#",
  "I###|.#.|.#.|.#.|.#.|.#.|.#.|.#.|.#.|###",
  "J..##|...#|...#|...#|...#|...#|...#|#..#|#..#|.##.",
  "K#..#|#..#|#.#.|#.#.|##..|##..|#.#.|#.#.|#..#|#..#",
  "L#...|#...|#...|#...|#...|#...|#...|#...|#...|####",
  "M#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#|#...#|#...#|#...#",
  "N#..#|##.#|##.#|##.#|#.##|#.##|#.##|#..#|#..#|#..#",
  "O.##.|#..#|#..#|#..#|#..#|#..#|#..#|#..#|#..#|.##.",
  "P###.|#..#|#..#|#..#|#..#|###.|#...|#...|#...|#...",
  "Q.##.|#..#|#..#|#..#|#..#|#..#|#..#|#.##|#..#|.##.",
  "R###.|#..#|#..#|#..#|###.|#.#.|#..#|#..#|#..#|#..#",
  "S.##.|#..#|#...|#...|.##.|...#|...#|...#|#..#|.##.",
  "T#####|..#..|..#..|..#..|..#..|..#..|..#..|..#..|..#..|..#..",
  "U#..#|#..#|#..#|#..#|#..#|#..#|#..#|#..#|#..#|.##.",
  "V#...#|#...#|#...#|#...#|.#.#.|.#.#.|.#.#.|.#.#.|..#..|..#..",
  "W#...#|#...#|#...#|#...#|#.#.#|#.#.#|#.#.#|#.#.#|##.##|#...#",
  "X#..#|#..#|#..#|.##.|.##.|.##.|.##.|#..#|#..#|#..#",
  "Y#...#|#...#|.#.#.|.#.#.|..#..|..#..|..#..|..#..|..#..|..#..",
  "Z####|...#|...#|..#.|..#.|.#..|.#..|#...|#...|####",
  "a....|....|###.|...#|..##|.#.#|#..#|#..#|#.##|.#.#",
  "b#...|#...|#.#.|##.#|#..#|#..#|#..#|#..#|##.#|#.#.",
  "c....|....|.##.|#..#|#...|#...|#...|#...|#..#|.##.",
  "d...#|...#|.#.#|#.##|#..#|#..#|#..#|#..#|#.##|.#.#",
  "e....|....|.##.|#..#|#..#|####|#...|#...|#..#|.##.",
  "f.##|.#.|###|.#.|.#.|.#.|.#.|.#.|.#.|.#.",
  "g....|....|.###|#..#|#..#|#..#|#..#|#..#|.###|...#|...#|###.",
  "h#...|#...|#.#.|##.#|#..#|#..#|#..#|#..#|#..#|#..#",
  "i#|.|#|#|#|#|#|#|#|#",
  "j.#|..|.#|.#|.#|.#|.#|.#|.#|.#|.#|#.",
  "k#...|#...|#..#|#..#|#.#.|##..|##..|#.#.|#..#|#..#",
  "l#|#|#|#|#|#|#|#|#|#",
  "m.....|.....|##.#.|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#",
  "n....|....|#.#.|##.#|#..#|#..#|#..#|#..#|#..#|#..#",
  "o....|....|.##.|#..#|#..#|#..#|#..#|#..#|#..#|.##.",
  "p....|....|#.#.|##.#|#..#|#..#|#..#|#..#|##.#|#.#.|#...|#...",
  "q....|....|.#.#|#.##|#..#|#..#|#..#|#..#|#.##|.#.#|...#|...#",
  "r....|....|#.##|##..|#...|#...|#...|#...|#...|#...",
  "s....|....|.##.|#..#|##..|.##.|..##|...#|#..#|.##.",
  "t.#..|.#..|####|.#..|.#..|.#..|.#..|.#..|.#..|..##",
  "u....|....|#..#|#..#|#..#|#..#|#..#|#..#|#.##|.#.#",
  "v.....|.....|#...#|#...#|#...#|.#.#.|.#.#.|.#.#.|..#..|..#..",
  "w.....|.....|#...#|#...#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|.#.#.",
  "x....|....|#..#|#..#|.##.|.##.|.##.|.##.|#..#|#..#",
  "y....|....|#..#|#..#|#..#|#..#|#..#|#..#|.###|...#|...#|###.",
  "z....|....|####|...#|...#|..#.|.#..|#...|#...|####",
  "0.##.|#..#|#..#|#..#|#..#|#..#|#..#|#..#|#..#|.##.",
  "1.#.|##.|.#.|.#.|.#.|.#.|.#.|.#.|.#.|###",
  "2.##.|#..#|#..#|...#|..#.|.#..|#...|#...|#...|####",
  "3.##.|#..#|#..#|...#|.##.|...#|...#|#..#|#..#|.##.",
  "4..#.|.##.|.##.|#.#.|#.#.|#.#.|####|..#.|..#.|..#.",
  "5####|#...|#...|###.|...#|...#|...#|#..#|#..#|.##.",
  "6.##.|#..#|#...|#...|###.|#..#|#..#|#..#|#..#|.##.",
  "7####|...#|...#|..#.|..#.|..#.|.#..|.#..|.#..|.#..",
  "8.##.|#..#|#..#|#..#|.##.|#..#|#..#|#..#|#..#|.##.",
  "9.##.|#..#|#..#|#..#|#..#|.###|...#|...#|#..#|.##.",
  "'##|##|.#|#.",
  " ....",
};

static const char *GbaGlyph(char c) {
  for (int i = 0; i < (int)(sizeof(kGbaFont) / sizeof(kGbaFont[0])); i++)
    if (kGbaFont[i][0] == c)
      return kGbaFont[i] + 1;
  return NULL;
}

static int GbaGlyphWidth(const char *g) {
  int w = 0;
  while (g[w] && g[w] != '|')
    w++;
  return w;
}

// pass 0 = contorno nero, pass 1 = lettere bianche
static void GbaText(Canvas *cv, int x, int y, const char *s, int pass) {
  for (; *s; s++) {
    const char *g = GbaGlyph(*s);
    if (!g)
      continue;
    int w = GbaGlyphWidth(g);
    for (int r = 0, c = 0; *g; g++) {
      if (*g == '|') {
        r++, c = 0;
        continue;
      }
      if (*g == '#') {
        if (pass == 0) {
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
              Canvas_Pixel(cv, x + c + dx, y + r + dy, 0x000000);
        } else {
          Canvas_Pixel(cv, x + c, y + r, 0xf8f8f8);
        }
      }
      c++;
    }
    x += w + 2;
  }
}

// Nomi dei dungeon per indice (cur_palace_index_x2 / 2), con il livello dei
// dungeon del Mondo delle Tenebre (0 = senza livello). Come su GBA (tabella dei
// messaggi 0x814e498) fogne, Castello di Hyrule e torre di Agahnim non ne hanno.
typedef struct DungeonEntry {
  uint8 level;
  const char *name;
} DungeonEntry;

static const DungeonEntry kDungeonEntries[14] = {
  { 0, NULL },                       // fogne
  { 0, NULL },                       // Castello di Hyrule
  { 0, "Palazzo dell'Est" },
  { 0, "Castello di Sabbia" },
  { 0, NULL },                       // torre di Agahnim
  { 2, "Dedalo d'Acqua" },
  { 1, "Palazzo Cupo" },
  { 6, "Malapalude" },
  { 3, "Bosco d'Ossa" },
  { 5, "Forte Ghiaccio" },
  { 0, "Torre sul Monte" },
  { 4, "Tana dei Ladroni" },
  { 7, "Rocca Tartaruga" },
  { 8, "Torre di Ganon" },
};

static const DungeonEntry *g_dungeon_name;
static bool g_dungeon_name_armed;
static uint16 g_dungeon_name_x, g_dungeon_name_y;
// Suono di un cartello quando compare il nome: le lettere (12) per tre
// fotogrammi di fila e, 26 fotogrammi dopo, il colpo di fine testo (0x24)
static int g_dungeon_name_tick;
static bool g_dungeon_name_held;  // tasto tenuto mentre la scritta aspettava (HdMenu_FilterInput)

// Da Module_PreDungeon: come su GBA (0x806f2c4) il nome dipende solo dal
// dungeon caricato, quindi compare da ogni porta, ripartendo dopo la morte e
// caricando la partita; non dalle cadute, che non passano di li'
void HdMenu_ShowDungeonName(void) {
  g_dungeon_name = NULL;
  g_dungeon_name_armed = false;
  g_dungeon_name_tick = 0;
  g_dungeon_name_held = false;
  uint16 i = cur_palace_index_x2 >> 1;
  if (cur_palace_index_x2 != 0xff && i < countof(kDungeonEntries) && kDungeonEntries[i].name)
    g_dungeon_name = &kDungeonEntries[i];
}

static int GbaTextWidth(const char *s) {
  int w = -2;
  for (; *s; s++) {
    const char *g = GbaGlyph(*s);
    if (g)
      w += GbaGlyphWidth(g) + 2;
  }
  return w;
}

// Riga di testo centrata, con il contorno
static void GbaTextCentered(Canvas *cv, int y, const char *s) {
  int x = (cv->width - GbaTextWidth(s)) / 2;
  GbaText(cv, x, y, s, 0);
  GbaText(cv, x, y, s, 1);
}

// Dopo ogni frame: la scritta resta finche' Link non si muove dal punto in cui
// e' comparso nel dungeon (per esempio su un nastro) o non si preme un tasto
// (HdMenu_FilterInput)
static void UpdateDungeonName(void) {
  if (!(enhanced_features0 & kFeatures0_DungeonNames))
    g_dungeon_name = NULL;
  if (!g_dungeon_name)
    return;
  int mod = main_module_index;
  // Entrata e caricamento: si aspetta il dungeon
  if (mod == 15 || mod == 6)
    return;
  if (mod != 7) {
    g_dungeon_name = NULL;
  } else if (!g_dungeon_name_armed) {
    g_dungeon_name_armed = true;
    g_dungeon_name_x = link_x_coord;
    g_dungeon_name_y = link_y_coord;
  } else if (link_x_coord != g_dungeon_name_x || link_y_coord != g_dungeon_name_y) {
    g_dungeon_name = NULL;
  }
  if (g_dungeon_name && g_dungeon_name_armed && submodule_index == 0 && (INIDISP_copy & 0x8f) == 0x0f) {
    if (g_dungeon_name_tick < 3)
      sound_effect_2 = 12;
    else if (g_dungeon_name_tick == 28)
      sound_effect_2 = 0x24;
    g_dungeon_name_tick++;
  }
}

bool HdMenu_FreezeEnemies(void) {
  return g_dungeon_name && g_dungeon_name_armed && main_module_index == 7;
}

// Con la scritta sullo schermo e' fermo anche Link: il primo tasto (passo,
// spada, oggetto) toglie solo la scritta e non arriva al gioco finche' non si
// lasciano tutti i tasti; dal tasto dopo Link si muove. Chi entra tenendo
// premuta la freccia si ferma appena dentro (l'input si ignora anche mentre lo
// schermo si schiarisce) e vede la scritta: quella freccia conta gia' come primo
// tasto, quindi lasciata e ripremuta toglie la scritta e muove subito Link.
// Il tasto della mappa (X) intanto non conta: non apre la mappa e non toglie
// la scritta; la corsa (A) e tutti gli altri si'.
enum { kInputMapButton = 1 << 9 };  // X (kKbdRemap in main.c)
// su, giu', sinistra, destra, A (corsa) e Y (oggetto)
enum { kInputPassThrough = 0xf0 | 1 << 8 | 1 << 1 };
static bool g_dungeon_name_swallow;
static int g_dungeon_name_prev_inputs;

int HdMenu_FilterInput(int inputs) {
  int all = inputs;
  // con la scritta (o mentre la si aspetta) il tasto della mappa non esiste
  if (g_dungeon_name && main_module_index == 7)
    inputs &= ~kInputMapButton;
  int pressed = inputs & ~g_dungeon_name_prev_inputs;
  g_dungeon_name_prev_inputs = inputs;
  if (g_dungeon_name_swallow) {
    if (inputs)
      return 0;
    g_dungeon_name_swallow = false;
  }
  // sottomodulo 15: lo schermo si schiarisce entrando (la freccia tenuta
  // spostava Link proprio nell'ultimo fotogramma)
  if (!g_dungeon_name || main_module_index != 7 || (submodule_index != 0 && submodule_index != 15))
    return all;
  bool visible = g_dungeon_name_armed && submodule_index == 0 && (INIDISP_copy & 0x8f) == 0x0f;
  if (!visible || (inputs & ~pressed)) {
    // si aspetta la scritta, o si tiene un tasto premuto da prima
    if (inputs)
      g_dungeon_name_held = true;
    return 0;
  }
  if (!pressed)
    return 0;
  g_dungeon_name = NULL;
  if (g_dungeon_name_held) {
    g_dungeon_name_held = false;
    return inputs;
  }
  // come su GBA una direzione o la corsa tolgono la scritta e Link si muove
  // subito; l'oggetto pure (vicino alla porta il gioco non lo usa); la spada
  // e gli altri tasti la tolgono soltanto, la mappa e' gia' esclusa sopra
  if (pressed & kInputPassThrough)
    return inputs & kInputPassThrough;
  g_dungeon_name_swallow = true;
  return 0;
}

static void DrawDungeonEntryName(Canvas *cv) {
  if (!HdMenu_FreezeEnemies() || submodule_index != 0 || (INIDISP_copy & 0x8f) != 0x0f)
    return;  // ancora in dissolvenza o in un passaggio
  // Come su GBA: il nome poco sopra il centro, il livello una riga sopra
  int y = cv->height / 2 - 1;
  GbaTextCentered(cv, y, Tr(g_dungeon_name->name));
  if (g_dungeon_name->level) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %d", IsItalian() ? "Livello" : "Level", g_dungeon_name->level);
    GbaTextCentered(cv, y - 16, buf);
  }
}

void HdMenu_Draw(uint8 *pixels, int pitch, int width, int height, int scale) {
  Canvas cv = { pixels, pitch, width / scale, height / scale, scale };
  if (!g_menu_open) {
    if (!g_font_data)
      Font_Load();
    HdHud_Draw(pixels, pitch, width, height, scale);
    DrawDungeonMapStatus(&cv);
    DrawDungeonEntryName(&cv);
    DrawInventoryLetterOutlines(&cv);
    DrawButtonHints(&cv);
    DrawMessage(&cv);
    return;
  }
  if (!g_font_data)
    Font_Load();
  // Scurisce il gioco sotto il menu
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++)
      row[x] = (row[x] >> 2 & 0x3f3f3f) | (row[x] & 0xff000000);
  }
  BuildItems();
  int *cur = &g_cursor[g_page];
  if (*cur >= g_num_items) *cur = g_num_items - 1;

  int panel_w = IntMin(cv.width - 16, 360);
  int panel_x = (cv.width - panel_w) / 2;
  int top = 8;
  int visible = (cv.height - top - 44) / kRowHeight;
  if (*cur < g_scroll) g_scroll = *cur;
  if (*cur >= g_scroll + visible) g_scroll = *cur - visible + 1;

  // Titolo
  Canvas_Rect(&cv, panel_x, top, panel_w, 20, 0x30285c);
  const char *title = Tr(PageTitle());
  DrawText(&cv, panel_x + (panel_w - TextWidth(title)) / 2, top + 1, title, 0xf8d878);

  int y = top + 24;
  for (int i = g_scroll; i < g_num_items && i < g_scroll + visible; i++, y += kRowHeight) {
    MenuItem *it = &g_items[i];
    bool sel = (i == *cur);
    if (sel)
      Canvas_Rect(&cv, panel_x, y - 1, panel_w, kRowHeight, 0x2050a0);
    uint32 color = sel ? 0xf8f8f8 : 0xc8c8d0;
    // Se la scritta non ci sta accanto al valore si usa la forma breve
    const char *label = it->label;
    int room = panel_w - 20 - (it->value[0] ? TextWidth(it->value) + 8 : 0);
    if (TextWidth(label) > room)
      label = ShortLabel(label);
    DrawText(&cv, panel_x + 10, y - 2, label, color);
    if (it->value[0]) {
      int vw = TextWidth(it->value);
      DrawText(&cv, panel_x + panel_w - 10 - vw, y - 2, it->value, sel ? 0xf8e070 : 0x98c0f0);
    }
  }
  // Frecce di scorrimento
  if (g_scroll > 0)
    DrawText(&cv, panel_x + panel_w - 14, top + 2, "\xe2\x86\x91", 0xf8f8f8);
  if (g_scroll + visible < g_num_items)
    DrawText(&cv, panel_x + panel_w - 14, cv.height - 38, "\xe2\x86\x93", 0xf8f8f8);

  // Riga di aiuto
  const char *help = Tr(g_capture >= 0 ?
      "Premi il nuovo tasto o pulsante  (Esc o Back annulla)" :
      "A conferma   B indietro   Sinistra e Destra cambiano");
  int hw = TextWidth(help);
  if (g_capture >= 0)
    Canvas_Rect(&cv, (cv.width - hw) / 2 - 8, cv.height - 22, hw + 16, 20, 0x803020);
  DrawText(&cv, (cv.width - hw) / 2, cv.height - 21, help, 0xf8f8f8);
  DrawMessage(&cv);
}

// ---------------------------------------------------------------------------
// Testo nitido

bool HdText_GetRect(int frame_w, int frame_h, int *x, int *y, int *w, int *h) {
  if (!g_config.hd_text || main_module_index != 14 || submodule_index != 2)
    return false;
  int off = (text_msgbox_topleft - 0x6000) & 0x3ff;
  int col = off & 31, row = off >> 5;
  Ppu *ppu = g_zenv.ppu;
  int ext = (frame_w - 256) / 2;
  int x0 = ext + col * 8 - (ppu->bgLayer[2].hScroll & 0xff);
  int y0 = row * 8 - (ppu->bgLayer[2].vScroll & 0xff);
  int x1 = x0 + 24 * 8, y1 = y0 + 8 * 8;
  x0 = IntMax(x0, 0), y0 = IntMax(y0, 0);
  x1 = IntMin(x1, frame_w), y1 = IntMin(y1, frame_h);
  if (x1 - x0 < 16 || y1 - y0 < 16)
    return false;
  *x = x0, *y = y0, *w = x1 - x0, *h = y1 - y0;
  return true;
}

// Scale2x (EPX) su un buffer w*h
static void Scale2x(const uint32 *src, int w, int h, uint32 *dst) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint32 E = src[y * w + x];
      uint32 B = y > 0 ? src[(y - 1) * w + x] : E;
      uint32 H = y < h - 1 ? src[(y + 1) * w + x] : E;
      uint32 D = x > 0 ? src[y * w + x - 1] : E;
      uint32 F = x < w - 1 ? src[y * w + x + 1] : E;
      uint32 *d0 = dst + (y * 2) * (w * 2) + x * 2, *d1 = d0 + w * 2;
      if (B != H && D != F) {
        d0[0] = D == B ? D : E;
        d0[1] = B == F ? F : E;
        d1[0] = D == H ? D : E;
        d1[1] = H == F ? F : E;
      } else {
        d0[0] = d0[1] = d1[0] = d1[1] = E;
      }
    }
  }
}

void HdText_Scale4x(const uint8 *src, int src_pitch, int x, int y, int w, int h, uint32 *dst) {
  static uint32 *tmp0, *tmp1;
  static int cap;
  if (w * h > cap) {
    cap = w * h;
    free(tmp0), free(tmp1);
    tmp0 = malloc(cap * 4);
    tmp1 = malloc(cap * 4 * 4);
  }
  // solo i pixel dell'interfaccia (strato 2: riquadro e testo) sono opachi:
  // i messaggi senza riquadro (la telepatia di Zelda) lasciano vedere il
  // fondale passato dallo shader, prima nel rettangolo restava quello originale
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      uint32 c = ((const uint32 *)(src + (y + yy) * src_pitch))[x + xx];
      tmp0[yy * w + xx] = (c >> 24) == 2 ? c | 0xff000000 : 0;
    }
  }
  Scale2x(tmp0, w, h, tmp1);
  Scale2x(tmp1, w * 2, h * 2, dst);
}
