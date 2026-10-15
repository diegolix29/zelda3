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
  kMaxAutosaves = 9,
  kMaxItems = 40,
  kRowHeight = 16,
};

typedef struct MenuItem {
  char label[64];
  char value[64];
  int id;
} MenuItem;

static bool g_menu_open;
static int g_page;
static int g_cursor[6];
static int g_scroll;
static int g_capture = -1;        // indice del comando in attesa di un tasto
static bool g_capture_pad;
static bool g_slot_action_save;
static char g_message[96];
static int g_message_timer;
static bool g_controls_dirty, g_msu_dirty;
static bool g_msu_on;
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
  { "Transizione continua tra aree", "ContinuousTransitions", kFeatures0_ContinuousTransitions },
  { "Oggetti X, L, R nell'HUD", "ShowXLRItemsInHud", kFeatures0_ShowXLRItemsInHud },
  { "Cambio oggetti con L e R", "ItemSwitchLR", kFeatures0_SwitchLR },
  { "Raccogli oggetti con la spada", "CollectItemsWithSword", kFeatures0_CollectItemsWithSword },
  { "Rompi vasi con la spada", "BreakPotsWithSword", kFeatures0_BreakPotsWithSword },
  { "Sterza durante lo scatto", "TurnWhileDashing", kFeatures0_TurnWhileDashing },
  { "Fino a 4 bombe insieme", "MoreActiveBombs", kFeatures0_MoreActiveBombs },
  { "Massimi in giallo nell'HUD", "ShowMaxItemsInYellow", kFeatures0_ShowMaxItemsInYellow },
  { "Disattiva bip vita bassa", "DisableLowHealthBeep", kFeatures0_DisableLowHealthBeep },
  { "Annulla il volo con X", "CancelBirdTravel", kFeatures0_CancelBirdTravel },
  { "Specchio verso le Tenebre", "MirrorToDarkworld", kFeatures0_MirrorToDarkworld },
  { "Correzioni di bug", "MiscBugFixes", kFeatures0_MiscBugFixes },
};

static const char *const kControlNames[12] = {
  "Su", "Gi\xc3\xb9", "Sinistra", "Destra", "Select", "Start", "A", "B", "X", "Y", "L", "R",
};

static const char *const kSmoothCameraNames[4] = { "Spenta", "Leggera", "Media", "Forte" };
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

static void AutosaveName(int i, char *buf, size_t size) {
  snprintf(buf, size, "saves/auto%d.sav", i);
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
    snprintf(buf, size, "vuoto");
    return;
  }
  struct tm *tm = localtime(&t);
  snprintf(buf, size, "%02d.%02d  ore %02d.%02d", tm->tm_mday, tm->tm_mon + 1, tm->tm_hour, tm->tm_min);
}

static int AutosaveCount() {
  int n = g_config.autosave_slots;
  return n < 1 ? 3 : n > kMaxAutosaves ? kMaxAutosaves : n;
}

// Restituisce lo slot automatico piu' recente (newest=true) o da sovrascrivere
static int FindAutosave(bool newest) {
  int best = -1;
  time_t best_t = 0;
  char name[64];
  for (int i = 0; i < AutosaveCount(); i++) {
    time_t t;
    AutosaveName(i, name, sizeof(name));
    if (!FileTime(name, &t)) {
      if (!newest)
        return i;  // slot libero
      continue;
    }
    if (best < 0 || (newest ? t > best_t : t < best_t))
      best = i, best_t = t;
  }
  return best < 0 ? 0 : best;
}

static void ShowMessage(const char *msg) {
  snprintf(g_message, sizeof(g_message), "%s", msg);
  g_message_timer = 150;
}

void HdMenu_ShowMessage(const char *msg) {
  ShowMessage(msg);
}

static int g_autosave_last_key = -1;

void HdMenu_AfterFrame(bool is_replay) {
  if (g_message_timer)
    g_message_timer--;
  if (is_replay || !g_config.autosave_areas)
    return;
  int mod = main_module_index;
  if ((mod != 7 && mod != 9) || submodule_index != 0 || link_health_current == 0)
    return;
  // Chiave del "luogo": area esterna, oppure dungeon/casa
  int key = (mod == 9) ? 0x1000 | overworld_screen_index : 0x2000 | cur_palace_index_x2;
  if (key == g_autosave_last_key)
    return;
  bool first = (g_autosave_last_key < 0);
  g_autosave_last_key = key;
  if (first)
    return;  // non salvare appena caricato
  char name[64];
  AutosaveName(FindAutosave(false), name, sizeof(name));
  // Salvataggio silenzioso: un messaggio a ogni cambio di area disturberebbe
  SaveLoadNamed(kSaveLoad_Save, name);
}

static void LoadLatestAutosave() {
  char name[64];
  time_t t;
  AutosaveName(FindAutosave(true), name, sizeof(name));
  if (!FileTime(name, &t)) {
    ShowMessage("Nessun salvataggio automatico");
    return;
  }
  ZeldaApuLock();
  bool ok = SaveLoadNamed(kSaveLoad_Load, name);
  ZeldaApuUnlock();
  g_autosave_last_key = -1;
  ShowMessage(ok ? "Salvataggio automatico caricato" : "Caricamento non riuscito");
  if (ok)
    HdMenu_Close();
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
  snprintf(msg, sizeof(msg), g_slot_action_save ? "Salvato nello slot %d" : "Slot %d caricato", slot + 1);
  ShowMessage(msg);
  if (!g_slot_action_save) {
    g_autosave_last_key = -1;
    HdMenu_Close();
  }
}

// ---------------------------------------------------------------------------
// Impostazioni

void HdMenu_ApplyConfig() {
  g_smooth_camera = g_config.smooth_camera;
  g_text_speed = g_config.text_speed;
  g_msu_on = g_config.enable_msu != 0;
  g_dynamic_light = g_config.dynamic_light;
  g_fx_embers = g_config.fx_embers, g_fx_window_rays = g_config.fx_window_rays;
  g_fx_item_glow = g_config.fx_item_glow;
  g_fx_heat_haze = g_config.fx_heat_haze, g_fx_rain = g_config.fx_rain;
}

static bool IsItalian() {
  return g_config.language && StringEqualsNoCase(g_config.language, "it");
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
  IniValue v[48];
  int n = 0;
  AddIni(v, &n, "[Graphics]", "SmoothCamera", "%d", g_config.smooth_camera);
  AddIni(v, &n, "[Graphics]", "EnhancedMode7", "%d", g_config.enhanced_mode7);
  AddIni(v, &n, "[Graphics]", "HdText", "%d", g_config.hd_text);
  AddIni(v, &n, "[Graphics]", "HighRefreshInterpolation", "%d", g_config.high_refresh);
  AddIni(v, &n, "[Graphics]", "DynamicLighting", "%d", g_config.dynamic_light);
  AddIni(v, &n, "[Graphics]", "FxEmbers", "%d", g_config.fx_embers);
  AddIni(v, &n, "[Graphics]", "FxWindowRays", "%d", g_config.fx_window_rays);
  AddIni(v, &n, "[Graphics]", "FxItemGlow", "%d", g_config.fx_item_glow);
  AddIni(v, &n, "[Graphics]", "FxHeatHaze", "%d", g_config.fx_heat_haze);
  AddIni(v, &n, "[Graphics]", "FxRain", "%d", g_config.fx_rain);
  AddIni(v, &n, "[General]", "AutosaveOnAreaChange", "%d", g_config.autosave_areas);
  AddIni(v, &n, "[General]", "AutosaveSlots", "%d", AutosaveCount());
  v[n].section = "[General]", v[n].key = "Language";
  snprintf(v[n].value, sizeof(v[n].value), "%s", IsItalian() ? "it" : "us"), n++;
  for (int i = 0; i < countof(kFeatureOptions); i++)
    AddIni(v, &n, "[Features]", kFeatureOptions[i].ini_key, "%d", (g_config.features0 & kFeatureOptions[i].bit) != 0);
  AddIni(v, &n, "[Features]", "TextSpeed", "%d", g_config.text_speed);
  if (g_msu_dirty) {
    v[n].section = "[Sound]", v[n].key = "EnableMSU";
    snprintf(v[n].value, sizeof(v[n].value), "%s", g_msu_on ? "true" : "false"), n++;
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
  snprintf(it->label, sizeof(it->label), "%s", label);
  snprintf(it->value, sizeof(it->value), "%s", value ? value : "");
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
    snprintf(buf, sizeof(buf), "%s%s", YesNo(g_config.enhanced_mode7), Host_IsOpenGL() ? "" : " (al riavvio)");
    AddItem(2, "Mappa del mondo in HD", buf);
    AddItem(3, "Mostra FPS", YesNo(Host_GetDisplayPerf()));
    AddItem(4, "Testo nitido nei dialoghi", YesNo(g_config.hd_text));
    AddItem(5, "Fluidit\xc3\xa0 oltre i 60 Hz", YesNo(g_config.high_refresh));
    AddItem(6, "Illuminazione dinamica", YesNo(g_config.dynamic_light));
    AddItem(7, "Braci dalle fiamme", YesNo(g_config.fx_embers));
    AddItem(8, "Raggi dalle finestre", YesNo(g_config.fx_window_rays));
    AddItem(9, "Bagliore degli oggetti ricevuti", YesNo(g_config.fx_item_glow));
    AddItem(10, "Calore sopra la lava", YesNo(g_config.fx_heat_haze));
    AddItem(11, "Pioggia e lampi migliorati", YesNo(g_config.fx_rain));
    break;
  case kPage_Game:
    AddItem(100, "Velocit\xc3\xa0 del testo", kTextSpeedNames[g_config.text_speed % 3]);
    AddItem(101, "Lingua dei dialoghi", IsItalian() ? "Italiano" : "English");
    for (int i = 0; i < countof(kFeatureOptions); i++)
      AddItem(i, kFeatureOptions[i].label, YesNo((g_config.features0 & kFeatureOptions[i].bit) != 0));
    break;
  case kPage_Audio:
    snprintf(buf, sizeof(buf), "%d%%", Host_GetVolume());
    AddItem(0, "Volume", buf);
    snprintf(buf, sizeof(buf), "%s (al riavvio)", YesNo(g_msu_on));
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
    AddItem(100, "Salva a ogni cambio di area", YesNo(g_config.autosave_areas));
    snprintf(buf, sizeof(buf), "%d", AutosaveCount());
    AddItem(101, "Salvataggi automatici da tenere", buf);
    AddItem(102, "Carica l'ultimo automatico", NULL);
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
    if (id == 0) {
      int v = (g_config.smooth_camera + (dir < 0 ? 3 : 1)) & 3;
      g_config.smooth_camera = v;
      g_smooth_camera = v;
    } else if (id == 1) {
      Host_ToggleFullscreen();
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
    } else if (id >= 7 && id <= 11) {
      bool *opt[5] = { &g_config.fx_embers, &g_config.fx_window_rays, &g_config.fx_item_glow,
                       &g_config.fx_heat_haze, &g_config.fx_rain };
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
      ZeldaSetLanguage(g_config.language);
      Font_Load();
    } else {
      uint32 bit = kFeatureOptions[id].bit;
      SetFeature(bit, !(g_config.features0 & bit));
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
    if (id == 100) {
      g_config.autosave_areas = !g_config.autosave_areas;
    } else if (id == 101) {
      int n = AutosaveCount() + (dir < 0 ? -1 : 1);
      g_config.autosave_slots = n < 1 ? kMaxAutosaves : n > kMaxAutosaves ? 1 : n;
    } else if (id == 102) {
      if (dir == 0) LoadLatestAutosave();
    } else if (id == 103) {
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
               code != SDLK_RCTRL && code != SDLK_LALT && code != SDLK_RALT) {
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

// Nome del labirinto mostrato sulla mappa del dungeon
static void DrawDungeonMapStatus(Canvas *cv) {
  static const char *const kDungeonNames[14] = {
    "Fogne del castello", "Castello di Hyrule", "Palazzo dell'Est", "Castello di Sabbia",
    "Sala superiore del castello", "Dedalo d'Acqua", "Palazzo Cupo", "Malapalude",
    "Bosco d'Ossa", "Forte Ghiaccio", "Torre di Hera", "Villaggio dei Ladroni",
    "Rocca Tartaruga", "Torre di Ganon",
  };
  if (main_module_index != 14 || submodule_index != 3 || overworld_map_state < 2 || overworld_map_state > 4)
    return;
  int dung = cur_palace_index_x2 >> 1;
  if (dung < 0 || dung >= 14)
    return;
  const char *name = kDungeonNames[dung];
  int w = TextWidth(name);
  int x = (cv->width - w) / 2;
  Canvas_Rect(cv, x - 8, cv->height - 20, w + 16, 18, 0x101830);
  DrawText(cv, x, cv->height - 20, name, 0xf8f8f8);
}

void HdMenu_Draw(uint8 *pixels, int pitch, int width, int height, int scale) {
  Canvas cv = { pixels, pitch, width / scale, height / scale, scale };
  if (!g_menu_open) {
    if (!g_font_data)
      Font_Load();
    DrawDungeonMapStatus(&cv);
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
  const char *title = PageTitle();
  DrawText(&cv, panel_x + (panel_w - TextWidth(title)) / 2, top + 1, title, 0xf8d878);

  int y = top + 24;
  for (int i = g_scroll; i < g_num_items && i < g_scroll + visible; i++, y += kRowHeight) {
    MenuItem *it = &g_items[i];
    bool sel = (i == *cur);
    if (sel)
      Canvas_Rect(&cv, panel_x, y - 1, panel_w, kRowHeight, 0x2050a0);
    uint32 color = sel ? 0xf8f8f8 : 0xc8c8d0;
    DrawText(&cv, panel_x + 10, y - 2, it->label, color);
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
  const char *help = g_capture >= 0 ?
      "Premi il nuovo tasto o pulsante  (Esc o Back annulla)" :
      "A conferma   B indietro   Sinistra e Destra cambiano";
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
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++)
      tmp0[yy * w + xx] = ((const uint32 *)(src + (y + yy) * src_pitch))[x + xx] | 0xff000000;
  Scale2x(tmp0, w, h, tmp1);
  Scale2x(tmp1, w * 2, h * 2, dst);
}
