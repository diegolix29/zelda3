// Menu opzioni in gioco (tasto Esc o clic della levetta destra), rimappatura
// dei tasti e salvataggi rapidi.
#include <SDL.h>
#include "hd_menu.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "zelda_rtl.h"
#include "variables.h"
#include "overworld.h"
#include "config.h"
#include "features.h"
#include "load_gfx.h"
#include "util.h"
#include "snes/ppu.h"
#include "hd_light.h"
#include "hd_fx.h"
#include "hd_water.h"
#include "hd_pit.h"
#include "hd_hud.h"
#include "hd_art.h"
#include "hud_it.h"
#include "assets.h"

void ZeldaApuLock();
void ZeldaApuUnlock();

enum {
  kPage_Main,
  kPage_Graphics,
  kPage_Game,
  kPage_Audio,
  kPage_Controls,
  kPage_Saves,
  kPage_Items,  // seconda meta' delle opzioni del gioco (pagina Items del launcher)
  kNumPages,
};

enum {
  kNumQuickSlots = 20,
  kMaxItems = 64,
  // voci di intestazione (sezioni come nel launcher): non si selezionano
  kHeaderId = -1,
  // voci della pagina Grafica mostrate nella pagina Gioco, e di Gioco in Audio
  kGfxInGame = 200,
  kGameInAudio = 300,
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
static int g_cursor[kNumPages];
static int g_scroll;
static int g_capture = -1;        // indice del comando in attesa di un tasto
static bool g_slot_action_save;
static char g_message[96];
static int g_message_timer;
static bool g_menu_modern; // menu col font del testo moderno
static int g_open_anim;    // fotogrammi dall'apertura (discesa del riquadro)
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
  { "Mondo continuo", "ContinuousWorld", kFeatures0_ContinuousWorld },
  { "HUD moderno", "ModernHud", kFeatures0_ModernHud },
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
  { "Sterzata durante la corsa", "TurnWhileDashing", kFeatures0_TurnWhileDashing },
  { "Pi\xc3\xb9 bombe attive", "MoreActiveBombs", kFeatures0_MoreActiveBombs },
  { "Evidenzia risorse al massimo", "ShowMaxItemsInYellow", kFeatures0_ShowMaxItemsInYellow },
  { "Borsa pi\xc3\xb9 grande", "CarryMoreRupees", kFeatures0_CarryMoreRupees },
  { "Niente bip con pochi cuori", "DisableLowHealthBeep", kFeatures0_DisableLowHealthBeep },
  { "Salva stato cuori e magia", "SaveHeartsAndMagic", kFeatures0_SaveHeartsAndMagic },
  { "Annulla viaggio uccello", "CancelBirdTravel", kFeatures0_CancelBirdTravel },
  { "Punto di volo 9", "BirdTravelPoint9", kFeatures0_BirdTravel9 },
  { "Specchio verso il Mondo delle Tenebre", "MirrorToDarkworld", kFeatures0_MirrorToDarkworld },
  { "Correzioni minori varie", "MiscBugFixes", kFeatures0_MiscBugFixes },
  { "Correzioni maggiori", "GameChangingBugFixes", kFeatures0_GameChangingBugFixes },
  { "Correzioni varie GBA", "GbaMinorFixes", kFeatures0_GbaMinorFixes },
};

static const char *const kControlNames[12] = {
  "Su", "Gi\xc3\xb9", "Sinistra", "Destra", "Select", "Start", "A", "B", "X", "Y", "L", "R",
};

static const char *const kLevelNames[4] = { "Spenta", "Leggera", "Media", "Forte" };
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
  // con il testo moderno lo stesso font dei dialoghi (ombra in basso a destra)
  const uint16 *bits;
  int mw = g_menu_modern && g >= 0 ? HdText_ModernGlyph(g, &bits) : 0;
  if (mw) {
    for (int pass = 0; pass < 2; pass++)
      for (int r = 0; r < 9; r++)
        for (int b = 0; b < mw; b++)
          if (bits[r] >> (mw - 1 - b) & 1)
            Canvas_Pixel(cv, x + b + (pass == 0), y + 4 + r + (pass == 0), pass ? color : 0x101018);
    return mw + 1;
  }
  if (g >= 0 && g < g_font_count && g_font_data) {
    int w = g_font_width[g];
    for (int row = 0; row < 16; row++) {
      for (int col = 0; col < 8; col++) {
        int v = HdFx_GameGlyphPixel(g_font_data, g, row, col);
        if (v == 1)
          Canvas_Pixel(cv, x + col, y + row, 0x101018);
        else if (v >= 2)
          Canvas_Pixel(cv, x + col, y + row, color);
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
    const uint16 *bits;
    int mw = g_menu_modern && g >= 0 ? HdText_ModernGlyph(g, &bits) : 0;
    w += mw ? mw + 1 : (g >= 0 && g < g_font_count && g_font_data) ? g_font_width[g] : 7;
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

void HdMenu_AfterFrame(void) {
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
  g_text_speed = g_config.text_speed;
  g_msu_on = g_config.enable_msu != 0;
  if (g_msu_on)
    g_msu_mode = g_config.enable_msu;
  g_dynamic_light = g_config.dynamic_light;
  g_fx_heat = g_config.fx_heat, g_fx_window_rays = g_config.fx_window_rays;
  g_fx_item_glow = g_config.fx_item_glow;
  g_fx_rain = g_config.fx_rain;
  g_fx_menu_blur = g_config.fx_menu_blur, g_fx_sprite_shadows = g_config.fx_sprite_shadows;
  g_fx_soft_iris = g_config.fx_soft_iris;
  g_fx_whirlpool = g_config.fx_whirlpool;
  g_fx_pit = g_config.fx_pit;
  g_fx_fog = g_config.fx_fog;
  g_fx_water = g_config.fx_water;
}

static bool IsItalian() {
  return g_config.language && StringEqualsNoCase(g_config.language, "it");
}

static const char *Tr(const char *it) {
  static const char *const kTr[][2] = {
    { "Transizioni veloci", "Fast transitions" },
    { "Mondo continuo", "Continuous world" },
    { "HUD moderno", "Modern HUD" },
    { "Richiede Cambio oggetti con L e R", "Requires Switch items with L and R" },
    { "Richiede la PPU ottimizzata (NewRenderer)", "Requires the optimized PPU (NewRenderer)" },
    { "Nomi dei labirinti", "Dungeon names" },
    { "Tasto X per gli oggetti", "X button for items" },
    { "Cambio oggetti con L e R", "Switch items with L and R" },
    { "Tasti L e R per gli oggetti", "L and R buttons for items" },
    { "Spada e oggetti interazione ambiente", "Sword and items interaction" },
    { "Oggetti riempiono le risorse", "Items refill resources" },
    { "Interazione bruciatura e congelamento", "Burn and freeze interaction" },
    { "Inventario esteso", "Extended inventory" },
    { "Mappa migliorata", "Improved map" },
    { "Mappa dei labirinti migliorata", "Improved dungeon map" },
    { "Mappa progressiva", "Progressive map" },
    { "Sterzata durante la corsa", "Allow turning while dashing" },
    { "Pi\xc3\xb9 bombe attive", "More active bombs" },
    { "Evidenzia risorse al massimo", "Indicate max resources" },
    { "Niente bip con pochi cuori", "Disable low heart beep" },
    { "Salva stato cuori e magia", "Save hearts and magic" },
    { "Annulla viaggio uccello", "Cancel bird" },
    { "Punto di volo 9", "Bird travel point 9" },
    { "Specchio verso il Mondo delle Tenebre", "Mirror to the Dark World" },
    { "Correzioni minori varie", "Miscellaneous minor fixes" },
    { "Correzioni maggiori", "Game changing bug fixes" },
    { "Correzioni varie GBA", "Various GBA fixes" },
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
    { "Oggetti", "Items" },
    { "Controlli", "Controls" },
    { "Salvataggi", "Saves" },
    { "Opzioni", "Options" },
    { "Riprendi", "Resume" },
    { "Esci dal gioco", "Quit game" },
    { "Schermo intero", "Fullscreen" },
    { "Mode7 migliorato", "Enhanced Mode7" },
    { "Mostra FPS", "Show FPS" },
    { "Testo nitido nei dialoghi", "Sharp dialogue text" },
    { "Testo moderno", "Modern text" },
    { "Fluidit\xc3\xa0 oltre i 60 Hz", "Smoothness above 60 Hz" },
    { "Illuminazione dinamica", "Dynamic lighting" },
    { "Effetto calore e fiamme", "Heat and flames effect" },
    { "Raggi dalle finestre", "Light rays from windows" },
    { "Bagliore degli oggetti ricevuti", "Glow of received items" },
    { "Pioggia e lampi migliorati", "Improved rain and lightning" },
    { "Meno flash VC", "Fewer flashes (VC)" },
    { "Meno flash GBA", "Fewer flashes (GBA)" },
    { "Nebbia migliorata", "Improved fog" },
    { "Acqua e cascate realistiche", "Realistic water and waterfalls" },
    { "Sfocatura dietro menu e dialoghi", "Blur behind menus and dialogues" },
    { "Ombre di personaggi e nemici", "Shadows of characters and enemies" },
    { "Cerchio di transizione morbido", "Soft transition circle" },
    { "Suggerimenti dei tasti", "Button hints" },
    { "Distingui vortici da Zora", "Tell whirlpools from Zoras" },
    { "Stanza sotto le buche", "Room below in pits" },
    { "Link biondo", "Blond Link" },
    { "Immersione con le pinne", "Dive with the flippers" },
    { "Frecce piantate nei muri", "Arrows stuck in walls" },
    { "Giravolta Tornado", "Hurricane Spin" },
    { "Rocce incantate", "Enchanted rocks" },
    { "Danni dei nemici come su GBA", "Enemy damage as on GBA" },
    { "Solo schermo largo", "Only widescreen" },
    { "Link personalizzato", "Custom Link" },
    { "Non con un Link personalizzato", "Not with a custom Link" },
    { "Richiede lo schermo largo", "Requires a widescreen aspect ratio" },
    { "Non con meno flash", "Not with fewer flashes" },
    { "Velocit\xc3\xa0 dei dialoghi", "Dialogue speed" },
    { "Generale", "General" },
    { "Immagine", "Image" },
    { "Aspetto HD", "HD look" },
    { "Effetti", "Effects" },
    { "Mondo e transizioni", "World and transitions" },
    { "Dialoghi", "Dialogue" },
    { "Mappe", "Maps" },
    { "Interfaccia", "Interface" },
    { "Salvataggi e avvio", "Saves and start" },
    { "Selezione degli oggetti", "Item selection" },
    { "Movimento e viaggi", "Movement and travel" },
    { "Interazione con l'ambiente", "World interaction" },
    { "Oggetti e combattimento", "Items and combat" },
    { "Correzioni", "Bug fixes" },
    { "Lingua dei dialoghi", "Dialogue language" },
    { "Musica orchestrale MSU", "MSU orchestral music" },
    { "(al riavvio)", "(on restart)" },
    { "Menu opzioni", "Options menu" },
    { "Ripristina tasti predefiniti", "Restore default keys" },
    { "Vibrazione del controller", "Controller rumble" },
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
    { "Conferma", "Confirm" },
    { "Indietro", "Back" },
    { "Scegli e cambia", "Select and change" },
    { "Chiudi", "Close" },
    { "Segnalino", "Marker" },
    { "Centra su Link", "Center on Link" },
    { "Veloce", "Fast" },
    { "Riduci", "Zoom out" },
    { "Ingrandisci", "Zoom in" },
    { "Cambia piano", "Change floor" },
    { "Croce", "D-pad" },
    { "Frecce", "Arrows" },
    { "Prigione del castello", "Sewers" },
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
  IniValue v[96];
  int n = 0;
  // Senza la PPU ottimizzata ApplyConfigRules spegne solo in memoria le
  // opzioni che la richiedono (e il menu non le lascia accendere): nell'ini
  // restano le scelte dell'utente, che altrimenti si cancellavano chiudendo il menu
  bool ppu = g_config.new_renderer;
  if (ppu) {
    AddIni(v, &n, "[Graphics]", "EnhancedMode7", "%d", g_config.enhanced_mode7);
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
    AddIni(v, &n, "[Graphics]", "FxPitDepth", "%d", g_config.fx_pit);
  }
  AddIni(v, &n, "[Graphics]", "HdText", "%d", g_config.hd_text);
  AddIni(v, &n, "[Graphics]", "ModernText", "%d", g_config.modern_text);
  AddIni(v, &n, "[Graphics]", "OfficialArtwork", "%d", g_config.official_artwork);
  AddIni(v, &n, "[Graphics]", "BlondLink", "%d", g_config.blond_link);
  AddIni(v, &n, "[Graphics]", "DimFlashes", "%d", (g_config.features0 & kFeatures0_DimFlashes) != 0);
  AddIni(v, &n, "[Graphics]", "DimFlashesGba", "%d", (g_config.features0 & kFeatures0_DimFlashesGba) != 0);
  AddIni(v, &n, "[Features]", "GbaEnemyDamage", "%d", g_config.gba_damage);
  AddIni(v, &n, "[Features]", "FlipperDive", "%d", g_config.flipper_dive);
  AddIni(v, &n, "[Features]", "StuckArrows", "%d", g_config.stuck_arrows);
  AddIni(v, &n, "[Features]", "HurricaneSpin", "%d", g_config.hurricane_spin);
  AddIni(v, &n, "[Features]", "EnchantedRocks", "%d", g_config.enchanted_rocks);
  AddIni(v, &n, "[Features]", "ItemOnX", "%d", g_config.item_on_x);
  AddIni(v, &n, "[Features]", "ItemOnLR", "%d", g_config.item_on_lr);
  // La lingua si scrive solo se cambiata dal menu (altrimenti resta quella
  // dell'ini, anche de, fr...)
  if (g_lang_dirty) {
    v[n].section = "[General]", v[n].key = "Language";
    snprintf(v[n].value, sizeof(v[n].value), "%s", g_config.language ? g_config.language : "us"), n++;
  }
  for (int i = 0; i < countof(kFeatureOptions); i++) {
    if (!ppu && (kFeatureOptions[i].bit & (kFeatures0_ContinuousWorld | kFeatures0_FastTransitions)))
      continue;
    AddIni(v, &n, "[Features]", kFeatureOptions[i].ini_key, "%d", (g_config.features0 & kFeatureOptions[i].bit) != 0);
  }
  AddIni(v, &n, "[Features]", "TextSpeed", "%d", g_config.text_speed);
  AddIni(v, &n, "[Features]", "Rumble", "%d", g_config.rumble);
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
  case kPage_Items: return "Oggetti";
  }
  return "Opzioni";
}

// Voci che ora non si possono cambiare: non si mostrano (stesse condizioni
// dei messaggi di Activate, che restano per sicurezza)
static bool GfxAvailable(int id) {
  if (!g_config.new_renderer && (id == 2 || id == 5 || id == 6 || (id >= 7 && id <= 17) || id == 23))
    return false;  // servono gli strati e lo spostamento della vista
  if (id == 19 && g_config.link_graphics)
    return false;  // Link biondo: non con un Link personalizzato
  if (id == 11 && (g_config.features0 & (kFeatures0_DimFlashes | kFeatures0_DimFlashesGba)))
    return false;  // pioggia e lampi: non con meno flash
  if (id == 15 && g_zenv.ppu->extraLeftRight == 0)
    return false;  // suggerimenti dei tasti: solo schermo largo
  return true;
}

static bool FeatureAvailable(uint32 bit) {
  if ((bit == kFeatures0_ContinuousWorld || bit == kFeatures0_FastTransitions) && !g_config.new_renderer)
    return false;
  if (bit == kFeatures0_SwitchLRLimit && !(g_config.features0 & kFeatures0_SwitchLR))
    return false;
  return true;
}

// Toglie le intestazioni rimaste senza voci
static void PruneHeaders(void) {
  int n = 0;
  for (int i = 0; i < g_num_items; i++) {
    if (g_items[i].id == kHeaderId && (i + 1 == g_num_items || g_items[i + 1].id == kHeaderId))
      continue;
    g_items[n++] = g_items[i];
  }
  g_num_items = n;
}

static void AddHeader(const char *label) {
  AddItem(kHeaderId, label, NULL);
}

// Opzione di features0 (kFeatureOptions) con id = indice + off
static void AddFeatureAt(uint32 bit, int off) {
  if (!FeatureAvailable(bit))
    return;
  for (int i = 0; i < (int)countof(kFeatureOptions); i++)
    if (kFeatureOptions[i].bit == bit) {
      AddItem(i + off, kFeatureOptions[i].label, YesNo((g_config.features0 & bit) != 0));
      return;
    }
}

static void AddFeature(uint32 bit) {
  AddFeatureAt(bit, 0);
}

// Voce della pagina Grafica (id come in Activate), con id + off
static void AddGfx(int id, int off) {
  char buf[64];
  if (!GfxAvailable(id))
    return;
  const char *label = NULL, *value = NULL;
  switch (id) {
  case 1: label = "Schermo intero", value = YesNo(Host_IsFullscreen()); break;
  case 2:
    snprintf(buf, sizeof(buf), "%s%s%s", Tr(YesNo(g_config.enhanced_mode7)), Host_IsOpenGL() ? "" : " ", Host_IsOpenGL() ? "" : Tr("(al riavvio)"));
    label = "Mode7 migliorato", value = buf;
    break;
  case 3: label = "Mostra FPS", value = YesNo(Host_GetDisplayPerf()); break;
  case 4: label = "Testo nitido nei dialoghi", value = YesNo(g_config.hd_text); break;
  case 22: label = "Testo moderno", value = YesNo(g_config.modern_text); break;
  case 5: label = "Fluidit\xc3\xa0 oltre i 60 Hz", value = YesNo(g_config.high_refresh); break;
  case 6: label = "Illuminazione dinamica", value = YesNo(g_config.dynamic_light); break;
  case 7: label = "Effetto calore e fiamme", value = YesNo(g_config.fx_heat); break;
  case 8: label = "Raggi dalle finestre", value = YesNo(g_config.fx_window_rays); break;
  case 9: label = "Bagliore degli oggetti ricevuti", value = YesNo(g_config.fx_item_glow); break;
  case 11: label = "Pioggia e lampi migliorati", value = YesNo(g_config.fx_rain); break;
  case 10: label = "Nebbia migliorata", value = YesNo(g_config.fx_fog); break;
  case 12: label = "Sfocatura dietro menu e dialoghi", value = YesNo(g_config.fx_menu_blur); break;
  case 13: label = "Ombre di personaggi e nemici", value = YesNo(g_config.fx_sprite_shadows); break;
  case 14: label = "Cerchio di transizione morbido", value = YesNo(g_config.fx_soft_iris); break;
  case 15: label = "Suggerimenti dei tasti", value = g_zenv.ppu->extraLeftRight > 0 ? YesNo(g_config.fx_button_hints) : "Solo schermo largo"; break;
  case 16: label = "Acqua e cascate realistiche", value = YesNo(g_config.fx_water); break;
  case 17: label = "Distingui vortici da Zora", value = YesNo(g_config.fx_whirlpool); break;
  case 23: label = "Stanza sotto le buche", value = YesNo(g_config.fx_pit); break;
  case 19: label = "Link biondo", value = g_config.link_graphics ? "Link personalizzato" : YesNo(g_config.blond_link); break;
  case 20: label = "Meno flash VC", value = YesNo((g_config.features0 & kFeatures0_DimFlashes) != 0); break;
  case 21: label = "Meno flash GBA", value = YesNo((g_config.features0 & kFeatures0_DimFlashesGba) != 0); break;
  default: return;
  }
  AddItem(id + off, label, value);
}

static void BuildItemsRaw(void);

static void BuildItems() {
  BuildItemsRaw();
  PruneHeaders();
}

static void BuildItemsRaw(void) {
  char buf[64], buf2[64];
  g_num_items = 0;
  switch (g_page) {
  case kPage_Main:
    AddItem(0, "Riprendi", NULL);
    AddItem(1, "Grafica", ">");
    AddItem(kPage_Game, "Gioco", ">");
    AddItem(kPage_Items, "Oggetti", ">");
    AddItem(kPage_Audio, "Audio", ">");
    AddItem(kPage_Controls, "Controlli", ">");
    AddItem(kPage_Saves, "Salvataggi", ">");
    AddItem(99, "Esci dal gioco", NULL);
    break;
  case kPage_Graphics:
    // stesso ordine e sezioni della pagina Grafica del launcher
    AddHeader("Generale");
    AddGfx(1, 0);
    AddGfx(3, 0);
    AddHeader("Immagine");
    AddGfx(2, 0);
    AddGfx(20, 0);
    AddGfx(21, 0);
    AddHeader("Aspetto HD");
    AddGfx(5, 0);
    AddGfx(6, 0);
    AddHeader("Effetti");
    {
      static const uint8 kEffects[] = { 7, 8, 9, 11, 10, 16, 17, 12, 13, 19 };
      for (int i = 0; i < (int)countof(kEffects); i++)
        AddGfx(kEffects[i], 0);
    }
    break;
  case kPage_Game:
    // stesso ordine e sezioni delle pagine Gameplay e Oggetti del launcher
    AddHeader("Mondo e transizioni");
    AddFeature(kFeatures0_FastTransitions);
    AddFeature(kFeatures0_ContinuousWorld);
    AddGfx(14, kGfxInGame);
    AddHeader("Dialoghi");
    AddItem(100, "Velocit\xc3\xa0 dei dialoghi", kTextSpeedNames[g_config.text_speed % 3]);
    AddGfx(4, kGfxInGame);
    AddGfx(22, kGfxInGame);
    AddItem(101, "Lingua dei dialoghi", IsItalian() ? "Italiano" : "English");
    AddHeader("Mappe");
    AddFeature(kFeatures0_ImprovedWorldMap);
    AddFeature(kFeatures0_ImprovedDungeonMap);
    AddFeature(kFeatures0_ProgressiveMap);
    AddGfx(23, kGfxInGame);
    AddHeader("Interfaccia");
    AddFeature(kFeatures0_ModernHud);
    AddFeature(kFeatures0_DungeonNames);
    AddGfx(15, kGfxInGame);
    AddFeature(kFeatures0_ShowMaxItemsInYellow);
    AddHeader("Salvataggi e avvio");
    AddFeature(kFeatures0_SaveHeartsAndMagic);
    break;
  case kPage_Items:
    // come la pagina Items del launcher (stessi id: li gestisce kPage_Game)
    AddHeader("Selezione degli oggetti");
    AddItem(105, "Tasto X per gli oggetti", YesNo(g_config.item_on_x));
    AddFeature(kFeatures0_SwitchLR);
    AddItem(106, "Tasti L e R per gli oggetti", YesNo(g_config.item_on_lr));
    AddFeature(kFeatures0_SwitchLRLimit);
    AddFeature(kFeatures0_ExtendedInventory);
    AddHeader("Movimento e viaggi");
    AddFeature(kFeatures0_TurnWhileDashing);
    AddItem(102, "Immersione con le pinne", YesNo(g_config.flipper_dive));
    AddFeature(kFeatures0_MirrorToDarkworld);
    AddFeature(kFeatures0_CancelBirdTravel);
    AddFeature(kFeatures0_BirdTravel9);
    AddHeader("Interazione con l'ambiente");
    AddFeature(kFeatures0_SwordAndItems);
    AddFeature(kFeatures0_BurnAndFreeze);
    AddItem(103, "Frecce piantate nei muri", YesNo(g_config.stuck_arrows));
    AddItem(107, "Rocce incantate", YesNo(g_config.enchanted_rocks));
    AddHeader("Oggetti e combattimento");
    AddFeature(kFeatures0_ItemsRefillResources);
    AddFeature(kFeatures0_GoldPotion);
    AddItem(108, "Danni dei nemici come su GBA", YesNo(g_config.gba_damage));
    AddItem(104, "Giravolta Tornado", YesNo(g_config.hurricane_spin));
    AddFeature(kFeatures0_MoreActiveBombs);
    AddFeature(kFeatures0_CarryMoreRupees);
    AddHeader("Correzioni");
    AddFeature(kFeatures0_MiscBugFixes);
    AddFeature(kFeatures0_GameChangingBugFixes);
    AddFeature(kFeatures0_GbaMinorFixes);
    break;
  case kPage_Audio:
    snprintf(buf, sizeof(buf), "%d%%", Host_GetVolume());
    AddItem(0, "Volume", buf);
    snprintf(buf, sizeof(buf), "%s %s", Tr(YesNo(g_msu_on)), Tr("(al riavvio)"));
    AddItem(1, "Musica orchestrale MSU", buf);
    // come nel launcher (Audio)
    AddFeatureAt(kFeatures0_DisableLowHealthBeep, kGameInAudio);
    break;
  case kPage_Controls:
    for (int i = 0; i < 12; i++) {
      Config_KeyName(Config_GetKeyForCmd(kKeys_Controls + i), buf2, sizeof(buf2));
      // testo tagliato se troppo lungo: va bene (gcc -Werror su Linux vuole il risultato usato)
      if (snprintf(buf, sizeof(buf), "%s   -   %s", buf2, Config_PadButtonName(Config_GetPadButtonForCmd(kKeys_Controls + i))) < 0)
        buf[0] = 0;
      AddItem(i, kControlNames[i], buf);
    }
    Config_KeyName(Config_GetKeyForCmd(kKeys_HdMenu), buf2, sizeof(buf2));
    if (snprintf(buf, sizeof(buf), "%s   -   %s", buf2, Config_PadButtonName(Config_GetPadButtonForCmd(kKeys_HdMenu))) < 0)
      buf[0] = 0;
    AddItem(12, "Menu opzioni", buf);
    AddItem(14, "Vibrazione del controller", kLevelNames[g_config.rumble & 3]);
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
  if (id == kHeaderId)
    return;
  // nelle pagine delle opzioni A non cambia niente (solo Sinistra e Destra,
  // come dice la riga di aiuto): A serve a entrare nelle pagine e per le azioni
  if (dir == 0 && (g_page == kPage_Graphics || g_page == kPage_Game || g_page == kPage_Items || g_page == kPage_Audio))
    return;
  int page = g_page;
  if (page == kPage_Game && id >= kGfxInGame && id < kGameInAudio)
    page = kPage_Graphics, id -= kGfxInGame;
  else if (page == kPage_Audio && id >= kGameInAudio)
    page = kPage_Game, id -= kGameInAudio;
  else if (page == kPage_Items)
    page = kPage_Game;
  switch (page) {
  case kPage_Main:
    if (dir != 0)
      return;
    if (id == 0) HdMenu_Close();
    else if (id == 99) { HdMenu_Close(); Host_Quit(); }
    else { g_page = id; g_scroll = 0; }
    break;
  case kPage_Graphics:
    if (!g_config.new_renderer && (id == 5 || id == 6 || (id >= 7 && id <= 17) || id == 23)) {
      // come nel launcher: servono gli strati e lo spostamento della vista
      ShowMessage("Richiede la PPU ottimizzata (NewRenderer)");
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
    } else if (id == 4 || id == 22) {
      // testo nitido e testo moderno: accenderne uno spegne l'altro
      bool *opt = id == 4 ? &g_config.hd_text : &g_config.modern_text;
      *opt = !*opt;
      if (*opt)
        *(id == 4 ? &g_config.modern_text : &g_config.hd_text) = false;
    } else if (id == 5) {
      g_config.high_refresh = !g_config.high_refresh;
    } else if (id == 6) {
      g_config.dynamic_light = !g_config.dynamic_light;
      g_dynamic_light = g_config.dynamic_light;
    } else if (id == 23) {
      g_config.fx_pit = !g_config.fx_pit;
      HdMenu_ApplyConfig();
    } else if (id == 19 && g_config.link_graphics) {
      ShowMessage("Non con un Link personalizzato");
    } else if (id == 19) {
      // Link e la sua palette subito
      g_config.blond_link = !g_config.blond_link;
      HdArt_Apply();
    } else if (id == 20 || id == 21) {
      // meno flash VC o GBA: accenderne uno spegne l'altro
      uint32 bit = id == 20 ? kFeatures0_DimFlashes : kFeatures0_DimFlashesGba;
      bool on = !(g_config.features0 & bit);
      SetFeature(kFeatures0_DimFlashes | kFeatures0_DimFlashesGba, false);
      SetFeature(bit, on);
      Palette_ApplyDimFlashFloor((g_config.features0 & kFeatures0_DimFlashes) != 0);
      // con meno flash niente pioggia e lampi migliorati
      if (on) {
        g_config.fx_rain = false;
        HdMenu_ApplyConfig();
      }
    } else if (id == 11 && (g_config.features0 & (kFeatures0_DimFlashes | kFeatures0_DimFlashesGba))) {
      ShowMessage("Non con meno flash");
    } else if (id == 15 && g_zenv.ppu->extraLeftRight == 0) {
      ShowMessage("Richiede lo schermo largo");
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
    } else if (id == 107) {
      // spenta: la roccia accesa si spegne subito (GbaRock_Frame); accesa:
      // la prossima roccia nasce entrando in un'area
      g_config.enchanted_rocks = !g_config.enchanted_rocks;
    } else if (id == 108) {
      // subito: solo le voci GBA della tabella dei danni (overworld.c)
      g_config.gba_damage = !g_config.gba_damage;
      Overworld_ApplyGbaDamage(g_config.gba_damage);
    } else if (id == 105 || id == 106) {
      bool *opt = id == 105 ? &g_config.item_on_x : &g_config.item_on_lr;
      *opt = !*opt;
      // tasti L e R per gli oggetti oppure cambio oggetti con L e R
      if (id == 106 && g_config.item_on_lr)
        SetFeature(kFeatures0_SwitchLR | kFeatures0_SwitchLRLimit, false);
    } else {
      uint32 bit = kFeatureOptions[id].bit;
      if ((bit == kFeatures0_ContinuousWorld || bit == kFeatures0_FastTransitions) && !g_config.new_renderer) {
        ShowMessage("Richiede la PPU ottimizzata (NewRenderer)");
        break;
      }
      // L/R solo sui primi 4 oggetti solo con il cambio con L e R
      if (bit == kFeatures0_SwitchLRLimit && !(g_config.features0 & kFeatures0_SwitchLR)) {
        ShowMessage("Richiede Cambio oggetti con L e R");
        break;
      }
      SetFeature(bit, !(g_config.features0 & bit));
      if (bit == kFeatures0_SwitchLR && !(g_config.features0 & bit))
        SetFeature(kFeatures0_SwitchLRLimit, false);
      if (bit == kFeatures0_SwitchLR && (g_config.features0 & bit))
        g_config.item_on_lr = false;
    }
    break;
  case kPage_Audio:
    if (id == 0)
      Host_AdjustVolume(dir < 0 ? -1 : 1);
    else if (id == 1)
      g_msu_on = !g_msu_on, g_msu_dirty = true;
    break;
  case kPage_Controls:
    if (id == 14) {
      if (dir == 0)  // scelta: solo Sinistra e Destra
        return;
      // spenta, leggera, media, forte; una vibrazione di prova con la nuova forza
      g_config.rumble = (g_config.rumble + (dir < 0 ? 3 : 1)) & 3;
      Host_Rumble(1.0f, 1.0f, 300);
      return;
    }
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
      if (dir != 0)  // scelta: solo Sinistra e Destra
        g_slot_action_save = !g_slot_action_save;
    } else if (dir == 0) {
      QuickSlotAction(id);
    }
    break;
  }
}

// ---------------------------------------------------------------------------
// Input

// Cursore dentro l'elenco e non su un'intestazione (si va avanti nel verso dir)
static void FixCursor(int *cur, int dir) {
  if (*cur >= g_num_items) *cur = g_num_items - 1;
  if (*cur < 0) *cur = 0;
  for (int n = 0; n < g_num_items && g_items[*cur].id == kHeaderId; n++)
    *cur = (*cur + dir + g_num_items) % g_num_items;
}

enum { kAct_None, kAct_Up, kAct_Down, kAct_Left, kAct_Right, kAct_Ok, kAct_Back, kAct_Close };

static void DoAction(int act) {
  BuildItems();
  int *cur = &g_cursor[g_page];
  FixCursor(cur, 1);
  switch (act) {
  case kAct_Up:
    *cur = (*cur + g_num_items - 1) % g_num_items;
    FixCursor(cur, -1);
    break;
  case kAct_Down:
    *cur = (*cur + 1) % g_num_items;
    FixCursor(cur, 1);
    break;
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
  g_open_anim = 0;
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
    else if (snprintf(buf, size, "%s %s %s %s", k[0], k[2], k[1], k[3]) < 0)
      buf[0] = 0;
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

// I suggerimenti stanno nelle fasce del widescreen (16:9, 16:10, 3:2, 18:9): in 4:3 non
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
  // inventario: ai lati, due e due, per non coprire i riquadri. Solo col menu
  // fermo in basso (Hud_BringMenuDown finito, 0xff18): mentre scende o risale
  // gli passerebbe sopra; spariscono appena si preme START per chiuderlo (5)
  if (sub == 1 && (uint16)BG3VOFS_copy2 == 0xff18 && overworld_map_state != 5) {
    Hint left[2] = { { kBtn_Dpad, "Scegli" }, { kBtn_Start, "Chiudi" } };
    Hint right[2];
    int nr = 0;
    // SELECT salva solo con un oggetto su X (Hud_NormalMenu)
    if (sram_progress_indicator && hud_cur_item_x != 0)
      right[nr++] = (Hint){ kBtn_Select, "Salva" };
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

// Labirinti per indice (cur_palace_index_x2 / 2): nome (sulla mappa del
// labirinto e, se banner, all'ingresso) e livello dei labirinti del Mondo
// delle Tenebre (0 = senza livello). Come su GBA (tabella dei messaggi
// 0x814e498) prigione del castello, Castello di Hyrule e torre di Agahnim
// non hanno la scritta all'ingresso.
typedef struct DungeonEntry {
  const char *name;
  uint8 level;
  bool banner;
} DungeonEntry;

static const DungeonEntry kDungeons[14] = {
  { "Prigione del castello", 0, false },
  { "Castello di Hyrule", 0, false },
  { "Palazzo dell'Est", 0, true },
  { "Castello di Sabbia", 0, true },
  { "Sala superiore del castello", 0, false },  // torre di Agahnim
  { "Dedalo d'Acqua", 2, true },
  { "Palazzo Cupo", 1, true },
  { "Malapalude", 6, true },
  { "Bosco d'Ossa", 3, true },
  { "Forte Ghiaccio", 5, true },
  { "Torre sul Monte", 0, true },
  { "Tana dei Ladroni", 4, true },
  { "Rocca Tartaruga", 7, true },
  { "Torre di Ganon", 8, true },
};

// Nome del labirinto mostrato sulla mappa del dungeon, in alto al centro
// (parte della mappa dei labirinti migliorata, ImprovedDungeonMap)
static void DrawDungeonMapStatus(Canvas *cv) {
  if (!(enhanced_features0 & kFeatures0_ImprovedDungeonMap))
    return;
  if (main_module_index != 14 || submodule_index != 3 || overworld_map_state < 2 || overworld_map_state > 4)
    return;
  int dung = cur_palace_index_x2 >> 1;
  if (dung < 0 || dung >= 14)
    return;
  const char *name = Tr(kDungeons[dung].name);
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
  if (cur_palace_index_x2 != 0xff && i < countof(kDungeons) && kDungeons[i].banner)
    g_dungeon_name = &kDungeons[i];
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
// Il tasto della mappa intanto non conta: non apre la mappa e non toglie
// la scritta; la corsa (A) e tutti gli altri si'.
// Tasto della mappa (bit di kKbdRemap in main.c): X, oppure Select con un
// oggetto su X (DidPressButtonForMap in messaging.c)
static int MapButtonMask(void) {
  return hud_cur_item_x ? 1 << 2 : 1 << 9;
}
// su, giu', sinistra, destra, A (corsa) e Y (oggetto), X con un oggetto su X
static int PassThroughMask(void) {
  return 0xf0 | 1 << 8 | 1 << 1 | (hud_cur_item_x ? 1 << 9 : 0);
}
static bool g_dungeon_name_swallow;
static int g_dungeon_name_prev_inputs;

int HdMenu_FilterInput(int inputs) {
  int all = inputs;
  // con la scritta (o mentre la si aspetta) il tasto della mappa non esiste
  if (g_dungeon_name && main_module_index == 7)
    inputs &= ~MapButtonMask();
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
  if (pressed & PassThroughMask())
    return inputs & PassThroughMask();
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

// ---------------------------------------------------------------------------
// Riquadro del menu con le caselle dei riquadri dell'inventario (Hud_DrawBox:
// angolo 0xFB, lati 0xFC e 0xF9, fondo 0xF5) e la palette dell'HUD, prese
// dalla ROM (la VRAM puo' avere altro: mappa, titolo)

enum { kOpenFrames = 10, kFramePal = 2, kFillPal = 1 };
static uint8 g_hud_gfx[3 * 2048];  // caselle 2bpp 0-383 (pacchetti 0x6a, 0x6b, 0x69)
static bool g_hud_gfx_ok;

static void HudGfx_Load(void) {
  if (g_hud_gfx_ok)
    return;
  static uint8 tmp[0x4000];
  static const uint8 kPacks[3] = { 0x6a, 0x6b, 0x69 };
  for (int i = 0; i < 3; i++) {
    Decomp_spr(tmp, kPacks[i]);
    memcpy(g_hud_gfx + i * 2048, tmp, 2048);
  }
  g_hud_gfx_ok = true;
}

static uint32 HudColor(int pal, int v) {
  return HdFx_Bgr15ToRgb(kHudPalData[pal * 4 + v]);
}

// Casella t (con i bit di ribaltamento 0x4000/0x8000) in x, y
static void DrawHudTile(Canvas *cv, int x, int y, int t, int pal) {
  const uint8 *d = g_hud_gfx + (t & 0x1ff) * 16;
  for (int r = 0; r < 8; r++) {
    int rr = (t & 0x8000) ? 7 - r : r;
    for (int c = 0; c < 8; c++) {
      int b = (t & 0x4000) ? c : 7 - c;
      int v = (d[rr * 2] >> b & 1) | (d[rr * 2 + 1] >> b & 1) << 1;
      if (v)
        Canvas_Pixel(cv, x + c, y + r, HudColor(pal, v));
    }
  }
}

// Colore del fondo del riquadro (per coprire il bordo dietro il titolo)
static uint32 HudFrameFill(void) {
  HudGfx_Load();
  const uint8 *d = g_hud_gfx + 0xf5 * 16;
  int v = (d[0] >> 7 & 1) | (d[1] >> 7 & 1) << 1;
  return v ? HudColor(kFillPal, v) : 0x000000;
}

static void DrawHudFrame(Canvas *cv, int x, int y, int cols, int rows) {
  HudGfx_Load();
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++) {
      int px = x + c * 8, py = y + r * 8;
      bool l = c == 0, rt = c == cols - 1, tp = r == 0, bt = r == rows - 1;
      int t;
      if ((l || rt) && (tp || bt))
        t = 0xfb | (rt ? 0x4000 : 0) | (bt ? 0x8000 : 0);
      else if (l || rt)
        t = 0xfc | (rt ? 0x4000 : 0);
      else if (tp || bt)
        t = 0xf9 | (bt ? 0x8000 : 0);
      else {
        DrawHudTile(cv, px, py, 0xf5, kFillPal);
        continue;
      }
      // sotto il bordo il fondo, come nel gioco (BG3 con caselle trasparenti)
      DrawHudTile(cv, px, py, 0xf5, kFillPal);
      DrawHudTile(cv, px, py, t, kFramePal);
    }
}

// Voci con piu' di due valori: si scorrono con Sinistra e Destra
static bool IsChoiceItem(int id) {
  switch (g_page) {
  case kPage_Game: return id == 100;             // velocita' dei dialoghi
  case kPage_Audio: return id == 0;              // volume
  case kPage_Controls: return id == 14;          // vibrazione
  }
  return false;
}

// Intestazioni col carattere piccolo dei riquadri dell'inventario
// ("STRUMENTO"), in maiuscolo, con l'ombra: righe basse kHeaderH
enum { kHeaderH = 10 };

static int SmallTextWidth(const char *s) {
  int w = 0;
  for (; *s; s++) {
    char c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
    const char *const *g = HudIt_SmallGlyph(c);
    w += (g ? (int)strlen(g[0]) : 3) + 1;
  }
  return w;
}

static int DrawSmallText(Canvas *cv, int x, int y, const char *s, uint32 color) {
  for (; *s; s++) {
    char c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
    const char *const *g = HudIt_SmallGlyph(c);
    if (!g) {
      x += 4;
      continue;
    }
    int w = (int)strlen(g[0]);
    for (int pass = 0; pass < 2; pass++)
      for (int r = 0; r < 5; r++)
        for (int b = 0; b < w; b++)
          if (g[r][b] == '#')
            Canvas_Pixel(cv, x + b + (pass == 0), y + r + (pass == 0), pass ? color : 0x101018);
    x += w + 1;
  }
  return x;
}

static int RowH(int i) {
  return g_items[i].id == kHeaderId ? kHeaderH : kRowHeight;
}

void HdMenu_Draw(uint8 *pixels, int pitch, int width, int height, int scale) {
  Canvas cv = { pixels, pitch, width / scale, height / scale, scale };
  HdText_Draw(pixels, pitch, width, height, scale);
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
  g_menu_modern = g_config.modern_text;
  // Scurisce il gioco sotto il menu (meno se c'e' gia' la sfocatura)
  bool blur = g_config.fx_menu_blur;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++)
      row[x] = blur ? (row[x] >> 1 & 0x7f7f7f) | (row[x] & 0xff000000)
                    : (row[x] >> 2 & 0x3f3f3f) | (row[x] & 0xff000000);
  }
  BuildItems();
  int *cur = &g_cursor[g_page];
  FixCursor(cur, 1);

  // riquadro di caselle da 8 pixel come i riquadri dell'inventario
  int cols = IntMin(cv.width - 16, 360) / 8, rows = (cv.height - 8 - 30) / 8;
  int panel_w = cols * 8, panel_h = rows * 8;
  int panel_x = (cv.width - panel_w) / 2;
  // all'apertura scende dall'alto come il menu degli oggetti
  float t = g_open_anim >= kOpenFrames ? 1.0f : (float)g_open_anim / kOpenFrames;
  g_open_anim++;
  int top = 8 - (int)((1 - t) * (1 - t) * (panel_h + 8));
  DrawHudFrame(&cv, panel_x, top, cols, rows);
  // spazio per le righe (alte kRowHeight, le intestazioni kHeaderH)
  int avail = panel_h - 16 - 6;
  // l'intestazione sopra la voce scelta resta visibile
  int first = *cur > 0 && g_items[*cur - 1].id == kHeaderId ? *cur - 1 : *cur;
  if (first < g_scroll) g_scroll = first;
  for (;;) {
    int h = 0;
    for (int i = g_scroll; i <= *cur; i++)
      h += RowH(i);
    if (h <= avail || g_scroll >= *cur)
      break;
    g_scroll++;
  }

  // Titolo nel bordo in alto, su un pezzo di fondo del riquadro
  const char *title = Tr(PageTitle());
  int tw = TextWidth(title);
  Canvas_Rect(&cv, panel_x + (panel_w - tw) / 2 - 6, top, tw + 12, 8, HudFrameFill());
  DrawText(&cv, panel_x + (panel_w - tw) / 2, top - 6, title, 0xf8d878);

  int y = top + 14, used = 0;
  bool more = false;
  for (int i = g_scroll; i < g_num_items; i++) {
    MenuItem *it = &g_items[i];
    int rh = RowH(i);
    // un'intestazione in fondo senza la sua prima voce sotto non si disegna
    if (used + rh > avail || (it->id == kHeaderId && i + 1 < g_num_items && used + rh + RowH(i + 1) > avail)) {
      more = true;
      break;
    }
    used += rh;
    if (it->id == kHeaderId) {
      // intestazione di sezione: carattere piccolo dell'inventario, colore
      // del titolo, e una riga
      int ex = DrawSmallText(&cv, panel_x + 10, y + 1, it->label, 0xf8d878);
      int lx = ex + 5;
      if (lx < panel_x + panel_w - 12)
        Canvas_Rect(&cv, lx, y + 3, panel_x + panel_w - 12 - lx, 1, 0x605890);
      y += rh;
      continue;
    }
    bool sel = (i == *cur);
    uint32 color = sel ? 0xf8f8f8 : 0xa8a8b8;
    // cursore: la freccia delle scelte nei dialoghi, che ondeggia
    if (sel)
      DrawText(&cv, panel_x + 10 + ((g_open_anim >> 4) & 1), y - 2, ">", 0xf8f8f8);
    // Colonna dei valori sempre nello stesso punto: a destra resta lo spazio
    // delle frecce < > dei valori a scelta (piu' di due), che si disegnano
    // ai lati senza spostare il testo. Le pagine (>) stanno nella colonna
    // della freccia
    const char *value = it->value;
    int arrow_x = panel_x + panel_w - 14 - TextWidth(">");
    int value_r = arrow_x - 4;
    bool page_link = strcmp(value, ">") == 0;
    int vw = value[0] && !page_link ? TextWidth(value) : 0;
    // Se la scritta non ci sta accanto al valore si usa la forma breve
    const char *label = it->label;
    int room = value_r - (panel_x + 22) - (vw ? vw + 8 + TextWidth("< ") : 0);
    if (TextWidth(label) > room)
      label = ShortLabel(label);
    DrawText(&cv, panel_x + 22, y - 2, label, color);
    uint32 vcolor = sel ? 0xf8e070 : 0x98c0f0;
    if (page_link) {
      DrawText(&cv, arrow_x, y - 2, ">", vcolor);
    } else if (vw) {
      DrawText(&cv, value_r - vw, y - 2, value, vcolor);
      if (sel && IsChoiceItem(it->id)) {
        DrawText(&cv, value_r - vw - 4 - TextWidth("<"), y - 2, "<", vcolor);
        DrawText(&cv, arrow_x, y - 2, ">", vcolor);
      }
    }
    y += rh;
  }
  // Frecce di scorrimento nel bordo in alto e in basso a destra, come il
  // titolo (non coprono la colonna dei valori)
  int sw = TextWidth("\xe2\x86\x93"), sx = panel_x + panel_w - 24 - sw;
  if (g_scroll > 0) {
    Canvas_Rect(&cv, sx - 2, top, sw + 4, 8, HudFrameFill());
    DrawText(&cv, sx, top - 6, "\xe2\x86\x91", 0xf8f8f8);
  }
  if (more) {
    Canvas_Rect(&cv, sx - 2, top + panel_h - 8, sw + 4, 8, HudFrameFill());
    DrawText(&cv, sx, top + panel_h - 14, "\xe2\x86\x93", 0xf8f8f8);
  }

  // Riga di aiuto: i tasti assegnati, disegnati come i suggerimenti dei tasti
  if (g_capture >= 0) {
    const char *help = Tr("Premi il nuovo tasto o pulsante  (Esc o Back annulla)");
    int hw = TextWidth(help);
    Canvas_Rect(&cv, (cv.width - hw) / 2 - 8, cv.height - 22, hw + 16, 20, 0x803020);
    DrawText(&cv, (cv.width - hw) / 2, cv.height - 21, help, 0xf8f8f8);
  } else if (g_page == kPage_Main) {
    Hint h[3] = { { kBtn_Dpad, "Scegli" }, { kBtn_A, "Conferma" }, { kBtn_B, "Indietro" } };
    DrawHints(&cv, h, 3, 21);
  } else {
    // in 4:3 la forma breve: altrimenti la riga esce dallo schermo
    Hint h[3] = { { kBtn_Dpad, cv.width > 300 ? "Scegli e cambia" : "Scegli" }, { kBtn_A, "Conferma" }, { kBtn_B, "Indietro" } };
    DrawHints(&cv, h, 3, 21);
  }
  DrawMessage(&cv);
}

// ---------------------------------------------------------------------------
// Testo nitido

bool HdText_GetRect(int frame_w, int frame_h, int *x, int *y, int *w, int *h) {
  // con il testo moderno il riquadro originale non c'e'
  if (!g_config.hd_text || main_module_index != 14 || submodule_index != 2 || HdText_ModernActive())
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
