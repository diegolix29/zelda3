#include "config.h"
#include "types.h"
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "features.h"
#include "util.h"
#ifdef _WIN32
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned long flags);
#endif

enum {
  kKeyMod_ScanCode = 0x200,
  kKeyMod_Alt = 0x400,
  kKeyMod_Shift = 0x800,
  kKeyMod_Ctrl = 0x1000,
};

Config g_config;

#define REMAP_SDL_KEYCODE(key) ((key) & SDLK_SCANCODE_MASK ? kKeyMod_ScanCode : 0) | (key) & (kKeyMod_ScanCode - 1)
#define _(x) REMAP_SDL_KEYCODE(x)
#define S(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Shift
#define A(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Alt
#define C(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Ctrl
#define N 0
static const uint16 kDefaultKbdControls[kKeys_Total] = {
  0,
  // Controls
  _(SDLK_UP), _(SDLK_DOWN), _(SDLK_LEFT), _(SDLK_RIGHT), _(SDLK_RSHIFT), _(SDLK_RETURN), _(SDLK_x), _(SDLK_z), _(SDLK_s), _(SDLK_a), _(SDLK_c), _(SDLK_v),
  // LoadState
  _(SDLK_F1), _(SDLK_F2), _(SDLK_F3), _(SDLK_F4), _(SDLK_F5), _(SDLK_F6), _(SDLK_F7), _(SDLK_F8), _(SDLK_F9), _(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // SaveState
  S(SDLK_F1), S(SDLK_F2), S(SDLK_F3), S(SDLK_F4), S(SDLK_F5), S(SDLK_F6), S(SDLK_F7), S(SDLK_F8), S(SDLK_F9), S(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // Replay State
  C(SDLK_F1), C(SDLK_F2), C(SDLK_F3), C(SDLK_F4), C(SDLK_F5), C(SDLK_F6), C(SDLK_F7), C(SDLK_F8), C(SDLK_F9), C(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // Load Ref State
  N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N,
  // Replay Ref State
  N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N,
  // CheatLife, CheatKeys, CheatEquipment, CheatWalkThroughWalls
  _(SDLK_w), _(SDLK_o), S(SDLK_w), C(SDLK_e),
  // ClearKeyLog, StopReplay, Fullscreen, Reset, Pause, PauseDimmed, Turbo, ReplayTurbo, WindowBigger, WindowSmaller, DisplayPerf, ToggleRenderer
  _(SDLK_k), _(SDLK_l), A(SDLK_RETURN), C(SDLK_r), S(SDLK_p), _(SDLK_p), _(SDLK_TAB), _(SDLK_t), N, N, _(SDLK_f), _(SDLK_r),
  // VolumeUp, VolumeDown, HdMenu
  N, N, _(SDLK_ESCAPE),
};
#undef _
#undef A
#undef C
#undef S
#undef N

typedef struct KeyNameId {
  const char *name;
  uint16 id, size;
} KeyNameId;

#define M(n) {#n, kKeys_##n, kKeys_##n##_Last - kKeys_##n + 1}
#define S(n) {#n, kKeys_##n, 1}
static const KeyNameId kKeyNameId[] = {
  {"Null", kKeys_Null, 65535},
  M(Controls), M(Load), M(Save), M(Replay), M(LoadRef), M(ReplayRef),
  S(CheatLife), S(CheatKeys), S(CheatEquipment), S(CheatWalkThroughWalls),
  S(ClearKeyLog), S(StopReplay), S(Fullscreen), S(Reset),
  S(Pause), S(PauseDimmed), S(Turbo), S(ReplayTurbo), S(WindowBigger), S(WindowSmaller), S(VolumeUp), S(VolumeDown), S(DisplayPerf), S(ToggleRenderer),
  S(HdMenu),
};
#undef S
#undef M
typedef struct KeyMapHashEnt {
  uint16 key, cmd, next;
} KeyMapHashEnt;

static uint16 keymap_hash_first[255];
static KeyMapHashEnt *keymap_hash;
static int keymap_hash_size;
static bool has_keynameid[countof(kKeyNameId)];

static bool KeyMapHash_Add(uint16 key, uint16 cmd) {
  if ((keymap_hash_size & 0xff) == 0) {
    if (keymap_hash_size > 10000)
      Die("Too many keys");
    keymap_hash = realloc(keymap_hash, sizeof(KeyMapHashEnt) * (keymap_hash_size + 256));
  }
  int i = keymap_hash_size++;
  KeyMapHashEnt *ent = &keymap_hash[i];
  ent->key = key;
  ent->cmd = cmd;
  ent->next = 0;
  int j = (uint32)key % 255;

  uint16 *cur = &keymap_hash_first[j];
  while (*cur) {
    KeyMapHashEnt *ent = &keymap_hash[*cur - 1];
    if (ent->key == key)
      return false;
    cur = &ent->next;
  }
  *cur = i + 1;
  return true;
}

static int KeyMapHash_Find(uint16 key) {
  int i = keymap_hash_first[key % 255];
  while (i) {
    KeyMapHashEnt *ent = &keymap_hash[i - 1];
    if (ent->key == key)
      return ent->cmd;
    i = ent->next;
  }
  return 0;
}

int FindCmdForSdlKey(SDL_Keycode code, SDL_Keymod mod) {
  if (code & ~(SDLK_SCANCODE_MASK | 0x1ff))
    return 0;
  int key = 0;
  if (code != SDLK_LALT && code != SDLK_RALT)
    key |=  mod & KMOD_ALT ? kKeyMod_Alt : 0;
  if (code != SDLK_LCTRL && code != SDLK_RCTRL)
    key |= mod & KMOD_CTRL ? kKeyMod_Ctrl : 0;
  if (code != SDLK_LSHIFT && code != SDLK_RSHIFT)
    key |= mod & KMOD_SHIFT ? kKeyMod_Shift : 0;
  key |= REMAP_SDL_KEYCODE(code);
  return KeyMapHash_Find(key);
}

static void ParseKeyArray(char *value, int cmd, int size) {
  char *s;
  int i = 0;
  for (; i < size && (s = NextDelim(&value, ',')) != NULL; i++, cmd += (cmd != 0)) {
    if (*s == 0)
      continue;
    int key_with_mod = 0;
    for (;;) {
      if (StringStartsWithNoCase(s, "Shift+")) {
        key_with_mod |= kKeyMod_Shift, s += 6;
      } else if (StringStartsWithNoCase(s, "Ctrl+")) {
        key_with_mod |= kKeyMod_Ctrl, s += 5;
      } else if (StringStartsWithNoCase(s, "Alt+")) {
        key_with_mod |= kKeyMod_Alt, s += 4;
      } else {
        break;
      }
    }
    SDL_Keycode key = SDL_GetKeyFromName(s);
    if (key == SDLK_UNKNOWN) {
      fprintf(stderr, "Unknown key: '%s'\n", s);
      continue;
    }
    if (!KeyMapHash_Add(key_with_mod | REMAP_SDL_KEYCODE(key), cmd))
      fprintf(stderr, "Duplicate key: '%s'\n", s);
  }
}

typedef struct GamepadMapEnt {
  uint32 modifiers;
  uint16 cmd, next;
} GamepadMapEnt;

static uint16 joymap_first[kGamepadBtn_Count];
static GamepadMapEnt *joymap_ents;
static int joymap_size;
static bool has_joypad_controls;
static bool has_joypad_hdmenu;
static const char *g_config_filename = "zelda3.ini";

static int CountBits32(uint32 n) {
  int count = 0;
  for (; n != 0; count++)
    n &= (n - 1);
  return count;
}

static void GamepadMap_Add(int button, uint32 modifiers, uint16 cmd) {
  if ((joymap_size & 0xff) == 0) {
    if (joymap_size > 1000)
      Die("Too many joypad keys");
    joymap_ents = realloc(joymap_ents, sizeof(GamepadMapEnt) * (joymap_size + 64));
    if (!joymap_ents) Die("realloc failure");
  }
  uint16 *p = &joymap_first[button];
  // Insert it as early as possible but before after any entry with more modifiers.
  int cb = CountBits32(modifiers);
  while (*p && cb < CountBits32(joymap_ents[*p - 1].modifiers))
    p = &joymap_ents[*p - 1].next;
  int i = joymap_size++;
  GamepadMapEnt *ent = &joymap_ents[i];
  ent->modifiers = modifiers;
  ent->cmd = cmd;
  ent->next = *p;
  *p = i + 1;
}

int FindCmdForGamepadButton(int button, uint32 modifiers) {
  GamepadMapEnt *ent;
  for(int e = joymap_first[button]; e != 0; e = ent->next) {
    ent = &joymap_ents[e - 1];
    if ((modifiers & ent->modifiers) == ent->modifiers)
      return ent->cmd;
  }
  return 0;
}

static int ParseGamepadButtonName(const char **value) {
  const char *s = *value;
  // Longest substring first
  static const char *const kGamepadKeyNames[] = {
    "Back", "Guide", "Start", "L3", "R3",
    "L1", "R1", "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "L2", "R2",
    "Lb", "Rb", "A", "B", "X", "Y"
  };
  static const uint8 kGamepadKeyIds[] = {
    kGamepadBtn_Back, kGamepadBtn_Guide, kGamepadBtn_Start, kGamepadBtn_L3, kGamepadBtn_R3,
    kGamepadBtn_L1, kGamepadBtn_R1, kGamepadBtn_DpadUp, kGamepadBtn_DpadDown, kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight, kGamepadBtn_L2, kGamepadBtn_R2,
    kGamepadBtn_L1, kGamepadBtn_R1, kGamepadBtn_A, kGamepadBtn_B, kGamepadBtn_X, kGamepadBtn_Y,
  };
  for (size_t i = 0; i != countof(kGamepadKeyNames); i++) {
    const char *r = StringStartsWithNoCase(s, kGamepadKeyNames[i]);
    if (r) {
      *value = r;
      return kGamepadKeyIds[i];
    }
  }
  return kGamepadBtn_Invalid;
}

static const uint8 kDefaultGamepadCmds[] = {
  kGamepadBtn_DpadUp, kGamepadBtn_DpadDown, kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight, kGamepadBtn_Back, kGamepadBtn_Start,
  kGamepadBtn_B, kGamepadBtn_A, kGamepadBtn_Y, kGamepadBtn_X, kGamepadBtn_L1, kGamepadBtn_R1,
};

static void ParseGamepadArray(char *value, int cmd, int size) {
  char *s;
  int i = 0;
  for (; i < size && (s = NextDelim(&value, ',')) != NULL; i++, cmd += (cmd != 0)) {
    if (*s == 0)
      continue;
    uint32 modifiers = 0;
    const char *ss = s;
    for (;;) {
      int button = ParseGamepadButtonName(&ss);
      if (button == kGamepadBtn_Invalid) BAD: {
        fprintf(stderr, "Unknown gamepad button: '%s'\n", s);
        break;
      }
      while (*ss == ' ' || *ss == '\t') ss++;
      if (*ss == '+') {
        ss++;
        modifiers |= 1 << button;
      } else if (*ss == 0) {
        GamepadMap_Add(button, modifiers, cmd);
        break;
      } else
        goto BAD;
    }
  }
}

static void RegisterDefaultKeys() {
  for (int i = 1; i < countof(kKeyNameId); i++) {
    if (!has_keynameid[i]) {
      int size = kKeyNameId[i].size, k = kKeyNameId[i].id;
      for (int j = 0; j < size; j++, k++)
        KeyMapHash_Add(kDefaultKbdControls[k], k);
    }
  }
  if (!has_joypad_controls) {
    for (int i = 0; i < countof(kDefaultGamepadCmds); i++)
      GamepadMap_Add(kDefaultGamepadCmds[i], 0, kKeys_Controls + i);
  }
  // Il menu opzioni si apre col clic della levetta destra
  if (!has_joypad_hdmenu)
    GamepadMap_Add(kGamepadBtn_R3, 0, kKeys_HdMenu);
}

static int GetIniSection(const char *s) {
  if (StringEqualsNoCase(s, "[KeyMap]"))
    return 0;
  if (StringEqualsNoCase(s, "[Graphics]"))
    return 1;
  if (StringEqualsNoCase(s, "[Sound]"))
    return 2;
  if (StringEqualsNoCase(s, "[General]"))
    return 3;
  if (StringEqualsNoCase(s, "[Features]"))
    return 4;
  if (StringEqualsNoCase(s, "[GamepadMap]"))
    return 5;
  return -1;
}

bool ParseBool(const char *value, bool *result) {
  bool rv = false;
  switch (*value++ | 32) {
  case '0': if (*value == 0) break; return false;
  case 'f': if (StringEqualsNoCase(value, "alse")) break; return false;
  case 'n': if (StringEqualsNoCase(value, "o")) break; return false;
  case 'o':
    rv = (*value | 32) == 'n';
    if (StringEqualsNoCase(value, rv ? "n" : "ff")) break;
    return false;
  case '1': rv = true; if (*value == 0) break; return false;
  case 'y': rv = true; if (StringEqualsNoCase(value, "es")) break; return false;
  case 't': rv = true; if (StringEqualsNoCase(value, "rue")) break; return false;
  default: return false;
  }
  if (result) {
    *result = rv;
    return true;
  }
  return rv;
}

static bool ParseBoolBit(const char *value, uint32 *data, uint32 mask) {
  bool tmp;
  if (!ParseBool(value, &tmp))
    return false;
  *data = *data & ~mask | (tmp ? mask : 0);
  return true;
}

static bool g_fx_heat_key_seen;  // l'ini ha gia' FxHeatAndFlames
// chiavi divise da opzioni piu' vecchie: se mancano prendono il valore di quella
static bool g_blond_link_seen, g_gba_damage_seen, g_flipper_dive_seen, g_stuck_arrows_seen, g_hurricane_spin_seen;
static bool g_item_on_x_seen, g_item_on_lr_seen;

static bool HandleIniConfig(int section, const char *key, char *value) {
  if (section == 0) {
    for (int i = 0; i < countof(kKeyNameId); i++) {
      if (StringEqualsNoCase(key, kKeyNameId[i].name)) {
        has_keynameid[i] = true;
        ParseKeyArray(value, kKeyNameId[i].id, kKeyNameId[i].size);
        return true;
      }
    }
  } else if (section == 5) {
    for (int i = 0; i < countof(kKeyNameId); i++) {
      if (StringEqualsNoCase(key, kKeyNameId[i].name)) {
        if (i == 1)
          has_joypad_controls = true;
        if (kKeyNameId[i].id == kKeys_HdMenu)
          has_joypad_hdmenu = true;
        ParseGamepadArray(value, kKeyNameId[i].id, kKeyNameId[i].size);
        return true;
      }
    }
  } else if (section == 1) {
    if (StringEqualsNoCase(key, "WindowSize")) {
      char *s;
      if (StringEqualsNoCase(value, "Auto")){
        g_config.window_width  = 0;
        g_config.window_height = 0;
        return true;
      }
      while ((s = NextDelim(&value, 'x')) != NULL) {
        if(g_config.window_width == 0) {
          g_config.window_width = atoi(s);
        } else {
          g_config.window_height = atoi(s);
          return true;
        }
      }
    } else if (StringEqualsNoCase(key, "EnhancedMode7")) {
      return ParseBool(value, &g_config.enhanced_mode7);
    } else if (StringEqualsNoCase(key, "NewRenderer")) {
      return ParseBool(value, &g_config.new_renderer);
    } else if (StringEqualsNoCase(key, "IgnoreAspectRatio")) {
      return ParseBool(value, &g_config.ignore_aspect_ratio);
    } else if (StringEqualsNoCase(key, "Fullscreen")) {
      g_config.fullscreen = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "WindowScale")) {
      g_config.window_scale = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "OutputMethod")) {
      g_config.output_method = StringEqualsNoCase(value, "SDL-Software") ? kOutputMethod_SDLSoftware :
                               StringEqualsNoCase(value, "OpenGL") ? kOutputMethod_OpenGL : 
                               StringEqualsNoCase(value, "OpenGL ES") ? kOutputMethod_OpenGL_ES :
                                                                        kOutputMethod_SDL;
      return true;
    } else if (StringEqualsNoCase(key, "LinearFiltering")) {
      return ParseBool(value, &g_config.linear_filtering);
    } else if (StringEqualsNoCase(key, "NoSpriteLimits")) {
      return ParseBool(value, &g_config.no_sprite_limits);
    } else if (StringEqualsNoCase(key, "LinkGraphics")) {
      g_config.link_graphics = value;
      return true;
    } else if (StringEqualsNoCase(key, "Shader")) {
      g_config.shader = *value ? value : NULL;
      return true;
    } else if (StringEqualsNoCase(key, "DimFlashes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DimFlashes);
    } else if (StringEqualsNoCase(key, "DimFlashesGba")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DimFlashesGba);
    } else if (StringEqualsNoCase(key, "FxHeatAndFlames")) {
      g_fx_heat_key_seen = true;
      return ParseBool(value, &g_config.fx_heat);
    } else if (StringEqualsNoCase(key, "FxEmbers") || StringEqualsNoCase(key, "FxHeatHaze")) {
      // vecchie opzioni separate, ora dentro FxHeatAndFlames (che vince se c'e')
      bool on;
      if (!ParseBool(value, &on))
        return false;
      if (!g_fx_heat_key_seen)
        g_config.fx_heat = on;
      return true;
    } else if (StringEqualsNoCase(key, "FxWindowRays")) {
      return ParseBool(value, &g_config.fx_window_rays);
    } else if (StringEqualsNoCase(key, "FxItemGlow")) {
      return ParseBool(value, &g_config.fx_item_glow);
    } else if (StringEqualsNoCase(key, "FxRain")) {
      return ParseBool(value, &g_config.fx_rain);
    } else if (StringEqualsNoCase(key, "FxForestFog")) {
      return ParseBool(value, &g_config.fx_fog);
    } else if (StringEqualsNoCase(key, "FxWater")) {
      return ParseBool(value, &g_config.fx_water);
    } else if (StringEqualsNoCase(key, "FxMenuBlur")) {
      return ParseBool(value, &g_config.fx_menu_blur);
    } else if (StringEqualsNoCase(key, "FxSpriteShadows")) {
      return ParseBool(value, &g_config.fx_sprite_shadows);
    } else if (StringEqualsNoCase(key, "FxSoftIris")) {
      return ParseBool(value, &g_config.fx_soft_iris);
    } else if (StringEqualsNoCase(key, "FxButtonHints")) {
      return ParseBool(value, &g_config.fx_button_hints);
    } else if (StringEqualsNoCase(key, "FxWhirlpoolGlow")) {
      return ParseBool(value, &g_config.fx_whirlpool);
    } else if (StringEqualsNoCase(key, "OfficialArtwork")) {
      return ParseBool(value, &g_config.official_artwork);
    } else if (StringEqualsNoCase(key, "BlondLink")) {
      g_blond_link_seen = true;
      return ParseBool(value, &g_config.blond_link);
    } else if (StringEqualsNoCase(key, "DynamicLighting")) {
      return ParseBool(value, &g_config.dynamic_light);
    } else if (StringEqualsNoCase(key, "HighRefreshInterpolation")) {
      return ParseBool(value, &g_config.high_refresh);
    } else if (StringEqualsNoCase(key, "HdText")) {
      return ParseBool(value, &g_config.hd_text);
    } else if (StringEqualsNoCase(key, "SmoothCamera")) {
      g_config.smooth_camera = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    }
  } else if (section == 2) {
    if (StringEqualsNoCase(key, "EnableAudio")) {
      return ParseBool(value, &g_config.enable_audio);
    } else if (StringEqualsNoCase(key, "AudioFreq")) {
      g_config.audio_freq = (uint16)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AudioChannels")) {
      g_config.audio_channels = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AudioSamples")) {
      g_config.audio_samples = (uint16)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "EnableMSU")) {
        if (StringEqualsNoCase(value, "opuz"))
        g_config.enable_msu = kMsuEnabled_Opuz;
      else if (StringEqualsNoCase(value, "deluxe"))
        g_config.enable_msu = kMsuEnabled_MsuDeluxe;
      else if (StringEqualsNoCase(value, "deluxe-opuz"))
        g_config.enable_msu = kMsuEnabled_MsuDeluxe | kMsuEnabled_Opuz;
      else 
        return ParseBool(value, (bool*)&g_config.enable_msu);
      return true;
    } else if (StringEqualsNoCase(key, "MSUPath")) {
      g_config.msu_path = value;
      return true;
    } else if (StringEqualsNoCase(key, "MSUVolume")) {
      g_config.msuvolume = atoi(value);
      return true;
    } else if (StringEqualsNoCase(key, "ResumeMSU")) {
      return ParseBool(value, &g_config.resume_msu);
    }
  } else if (section == 3) {
    if (StringEqualsNoCase(key, "Autosave")) {
      g_config.autosave = (bool)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "ExtendedAspectRatio")) {
      const char* s;
      int h = 224, num = 0, den = 1;
      bool nospr = false, novis = false;
      while ((s = NextDelim(&value, ',')) != NULL) {
        if (strcmp(s, "extend_y") == 0)
          h = 240, g_config.extend_y = true;
        else if (strcmp(s, "16:9") == 0)
          num = 16, den = 9;
        else if (strcmp(s, "16:10") == 0)
          num = 16, den = 10;
        else if (strcmp(s, "18:9") == 0)
          num = 18, den = 9;
        else if (strcmp(s, "4:3") == 0)
          num = 0;
        else if (strcmp(s, "unchanged_sprites") == 0)
          nospr = true;
        else if (strcmp(s, "no_visual_fixes") == 0)
          novis = true;
        else
          return false;
      }
      // la larghezza si calcola dopo aver letto tutte le parole: con extend_y in
      // qualunque posizione il rapporto resta quello scelto (16:9 = 426x240)
      g_config.extended_aspect_ratio = num ? (h * num / den - 256) / 2 : 0;
      if (g_config.extended_aspect_ratio && !nospr)
        g_config.features0 |= kFeatures0_ExtendScreen64;
      if (g_config.extended_aspect_ratio && !novis)
        g_config.features0 |= kFeatures0_WidescreenVisualFixes;
      return true;
    } else if (StringEqualsNoCase(key, "DisplayPerfInTitle")) {
      return ParseBool(value, &g_config.display_perf_title);
    } else if (StringEqualsNoCase(key, "DisableFrameDelay")) {
      return ParseBool(value, &g_config.disable_frame_delay);
    } else if (StringEqualsNoCase(key, "Language")) {
      g_config.language = value;
      return true;
    } else if (StringEqualsNoCase(key, "AutosaveOnAreaChange") || StringEqualsNoCase(key, "AutosaveSlots")) {
      return true;  // vecchio salvataggio automatico ai cambi di area, tolto
    }
  } else if (section == 4) {
    if (StringEqualsNoCase(key, "ItemSwitchLR")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SwitchLR);
    } else if (StringEqualsNoCase(key, "ItemSwitchLRLimit")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SwitchLRLimit);
    } else if (StringEqualsNoCase(key, "TurnWhileDashing")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_TurnWhileDashing);
    } else if (StringEqualsNoCase(key, "MirrorToDarkworld")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MirrorToDarkworld);
    } else if (StringEqualsNoCase(key, "SwordAndItemsInteraction")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SwordAndItems);
    } else if (StringEqualsNoCase(key, "GbaEnemyDamage")) {
      g_gba_damage_seen = true;
      return ParseBool(value, &g_config.gba_damage);
    } else if (StringEqualsNoCase(key, "FlipperDive")) {
      g_flipper_dive_seen = true;
      return ParseBool(value, &g_config.flipper_dive);
    } else if (StringEqualsNoCase(key, "ItemOnX")) {
      g_item_on_x_seen = true;
      return ParseBool(value, &g_config.item_on_x);
    } else if (StringEqualsNoCase(key, "ItemOnLR")) {
      g_item_on_lr_seen = true;
      return ParseBool(value, &g_config.item_on_lr);
    } else if (StringEqualsNoCase(key, "HurricaneSpin")) {
      g_hurricane_spin_seen = true;
      return ParseBool(value, &g_config.hurricane_spin);
    } else if (StringEqualsNoCase(key, "StuckArrows")) {
      g_stuck_arrows_seen = true;
      return ParseBool(value, &g_config.stuck_arrows);
    } else if (StringEqualsNoCase(key, "CollectItemsWithSword") || StringEqualsNoCase(key, "BreakPotsWithSword")) {
      return true;  // vecchie opzioni, ora dentro SwordAndItemsInteraction
    } else if (StringEqualsNoCase(key, "GoldPotion")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_GoldPotion);
    } else if (StringEqualsNoCase(key, "ItemsRefillResources")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ItemsRefillResources);
    } else if (StringEqualsNoCase(key, "IcePalaceGba")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_IcePalaceGba);
    } else if (StringEqualsNoCase(key, "SaveHeartsAndMagic")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SaveHeartsAndMagic);
    } else if (StringEqualsNoCase(key, "DisableLowHealthBeep")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DisableLowHealthBeep);
    } else if (StringEqualsNoCase(key, "SkipIntroOnKeypress")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SkipIntroOnKeypress);
    } else if (StringEqualsNoCase(key, "ShowMaxItemsInYellow")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ShowMaxItemsInYellow);
    } else if (StringEqualsNoCase(key, "MoreActiveBombs")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MoreActiveBombs);
    } else if (StringEqualsNoCase(key, "CarryMoreRupees")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CarryMoreRupees);
    } else if (StringEqualsNoCase(key, "MiscBugFixes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MiscBugFixes);
    } else if (StringEqualsNoCase(key, "GameChangingBugFixes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_GameChangingBugFixes);
    } else if (StringEqualsNoCase(key, "CancelBirdTravel")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CancelBirdTravel);
    } else if (StringEqualsNoCase(key, "BirdTravelPoint9")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_BirdTravel9);
    } else if (StringEqualsNoCase(key, "FastTransitions")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_FastTransitions);
    } else if (StringEqualsNoCase(key, "ContinuousWorld")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ContinuousWorld);
    } else if (StringEqualsNoCase(key, "ModernHud")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ModernHud);
    } else if (StringEqualsNoCase(key, "DungeonNames")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DungeonNames);
    } else if (StringEqualsNoCase(key, "ExtendedInventory")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ExtendedInventory);
    } else if (StringEqualsNoCase(key, "ImprovedWorldMap")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ImprovedWorldMap);
    } else if (StringEqualsNoCase(key, "ImprovedDungeonMap")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ImprovedDungeonMap);
    } else if (StringEqualsNoCase(key, "ProgressiveMap")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ProgressiveMap);
    } else if (StringEqualsNoCase(key, "BurnAndFreezeInteraction")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_BurnAndFreeze);
    } else if (StringEqualsNoCase(key, "ShowXLRItemsInHud")) {
      return true;  // tolta: l'HUD mostra sempre i tasti con gli oggetti
    } else if (StringEqualsNoCase(key, "TextSpeed")) {
      g_config.text_speed = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    }
  }
  return false;
}

static bool ParseOneConfigFile(const char *filename, int depth) {
  char *filedata = (char*)ReadWholeFile(filename, NULL), *p;
  if (!filedata)
    return false;
  
  int section = -2;
  g_config.memory_buffer = filedata;

  for (int lineno = 1; (p = NextLineStripComments(&filedata)) != NULL; lineno++) {
    if (*p == 0)
      continue; // empty line
    if (*p == '[') {
      section = GetIniSection(p);
      if (section < 0)
        fprintf(stderr, "%s:%d: Invalid .ini section %s\n", filename, lineno, p);
    } else if (*p == '!' && SkipPrefix(p + 1, "include ")) {
      char *tt = p + 8;
      char *new_filename = ReplaceFilenameWithNewPath(filename, NextPossiblyQuotedString(&tt));
      if (depth > 10 || !ParseOneConfigFile(new_filename, depth + 1))
        fprintf(stderr, "Warning: Unable to read %s\n", new_filename);
      free(new_filename);
    } else if (section == -2) {
      fprintf(stderr, "%s:%d: Expecting [section]\n", filename, lineno);
    } else {
      char *v = SplitKeyValue(p);
      if (v == NULL) {
        fprintf(stderr, "%s:%d: Expecting 'key=value'\n", filename, lineno);
        continue;
      }
      if (section >= 0 && !HandleIniConfig(section, p, v))
        fprintf(stderr, "%s:%d: Can't parse '%s'\n", filename, lineno, p);
    }
  }
  return true;
}

// Opzioni che funzionano solo insieme a un'altra (come nel launcher):
// - L/R solo sui primi 4 oggetti solo con il cambio con L e R;
// - cambio oggetti con L e R e tasti L e R per gli oggetti non insieme;
// - mappa del mondo in HD e schermo largo solo con la PPU ottimizzata (il
//   vecchio renderer disegna 256 pixel e lascia neri i lati);
// - anche gli effetti HD, la telecamera fluida, la fluidita' oltre i 60 Hz, il
//   mondo continuo e le transizioni rapide: lavorano sull'immagine sapendo da
//   quale strato viene ogni pixel (HUD, sprite, acqua) e spostano la vista,
//   cose che il vecchio renderer non fa;
// - con l'MSU la frequenza e' quella dei suoi file: 48000 per gli opuz,
//   44100 per i pcm.
static void ApplyConfigRules(void) {
  // opzioni divise: un ini di prima le accende come l'opzione da cui vengono
  if (!g_blond_link_seen)
    g_config.blond_link = g_config.official_artwork;
  if (!g_gba_damage_seen)
    g_config.gba_damage = (g_config.features0 & kFeatures0_SwordAndItems) != 0;
  if (!g_flipper_dive_seen)
    g_config.flipper_dive = (g_config.features0 & kFeatures0_SwordAndItems) != 0;
  if (!g_hurricane_spin_seen)
    g_config.hurricane_spin = (g_config.features0 & kFeatures0_SwordAndItems) != 0;
  if (!g_stuck_arrows_seen)
    g_config.stuck_arrows = (g_config.features0 & kFeatures0_BurnAndFreeze) != 0;
  // un ini di prima: ItemSwitchLR accendeva anche gli oggetti su X, L e R;
  // L e R restano al cambio rapido
  if (!g_item_on_x_seen)
    g_config.item_on_x = (g_config.features0 & kFeatures0_SwitchLR) != 0;
  // cambio oggetti con L e R oppure oggetti su L e R, non tutti e due
  if (g_config.item_on_lr)
    g_config.features0 &= ~kFeatures0_SwitchLR;
  if (!(g_config.features0 & kFeatures0_SwitchLR))
    g_config.features0 &= ~kFeatures0_SwitchLRLimit;
  // meno flash: GBA o Virtual Console, non tutti e due
  if (g_config.features0 & kFeatures0_DimFlashesGba)
    g_config.features0 &= ~kFeatures0_DimFlashes;
  if (!g_config.new_renderer) {
    g_config.enhanced_mode7 = false;
    g_config.extended_aspect_ratio = 0;
    g_config.features0 &= ~(kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes |
                            kFeatures0_ContinuousWorld | kFeatures0_FastTransitions);
    g_config.smooth_camera = 0;
    g_config.high_refresh = g_config.dynamic_light = false;
    g_config.fx_heat = g_config.fx_window_rays = g_config.fx_item_glow = g_config.fx_rain = false;
    g_config.fx_menu_blur = g_config.fx_sprite_shadows = g_config.fx_soft_iris = false;
    g_config.fx_button_hints = g_config.fx_water = g_config.fx_whirlpool = g_config.fx_fog = false;
  }
  if (g_config.enable_msu)
    g_config.audio_freq = (g_config.enable_msu & kMsuEnabled_Opuz) ? 48000 : 44100;
}

void ParseConfigFile(const char *filename) {
  g_config.msuvolume = 100;  // default msu volume, 100%

  if (filename != NULL || !ParseOneConfigFile("zelda3.user.ini", 0)) {
    if (filename == NULL)
      filename = "zelda3.ini";
    if (!ParseOneConfigFile(filename, 0))
      fprintf(stderr, "Warning: Unable to read config file %s\n", filename);
    g_config_filename = filename;
  } else {
    g_config_filename = "zelda3.user.ini";
  }
  ApplyConfigRules();
  RegisterDefaultKeys();
}

// ---------------------------------------------------------------------------
// Supporto al menu opzioni: lettura/modifica dei tasti e scrittura dell'ini.

const char *Config_GetFilename() {
  return g_config_filename;
}

#define UNREMAP_SDL_KEYCODE(k) (((k) & (kKeyMod_ScanCode - 1)) | ((k) & kKeyMod_ScanCode ? SDLK_SCANCODE_MASK : 0))

int Config_GetKeyForCmd(int cmd) {
  // La prima voce attiva per quel comando
  for (int i = 0; i < keymap_hash_size; i++)
    if (keymap_hash[i].cmd == cmd && KeyMapHash_Find(keymap_hash[i].key) == cmd)
      return keymap_hash[i].key;
  return 0;
}

void Config_KeyName(int key, char *buf, size_t size) {
  if (key == 0) {
    snprintf(buf, size, "-");
    return;
  }
  snprintf(buf, size, "%s%s%s%s",
           key & kKeyMod_Ctrl ? "Ctrl+" : "",
           key & kKeyMod_Alt ? "Alt+" : "",
           key & kKeyMod_Shift ? "Shift+" : "",
           SDL_GetKeyName(UNREMAP_SDL_KEYCODE(key)));
}

// Ricostruisce la tabella dei tasti togliendo le voci del comando drop_cmd
// e quelle che usano il tasto drop_key.
static void KeyMapHash_Rebuild(int drop_cmd, int drop_key) {
  int n = keymap_hash_size;
  KeyMapHashEnt *old = malloc(sizeof(KeyMapHashEnt) * (n + 1));
  memcpy(old, keymap_hash, sizeof(KeyMapHashEnt) * n);
  memset(keymap_hash_first, 0, sizeof(keymap_hash_first));
  keymap_hash_size = 0;
  for (int i = 0; i < n; i++) {
    if (old[i].cmd == drop_cmd || old[i].key == drop_key)
      continue;
    KeyMapHash_Add(old[i].key, old[i].cmd);
  }
  free(old);
}

void Config_SetKeyForCmd(int cmd, SDL_Keycode code, SDL_Keymod mod) {
  int key = 0;
  if (code != SDLK_LALT && code != SDLK_RALT)
    key |= mod & KMOD_ALT ? kKeyMod_Alt : 0;
  if (code != SDLK_LCTRL && code != SDLK_RCTRL)
    key |= mod & KMOD_CTRL ? kKeyMod_Ctrl : 0;
  if (code != SDLK_LSHIFT && code != SDLK_RSHIFT)
    key |= mod & KMOD_SHIFT ? kKeyMod_Shift : 0;
  key |= REMAP_SDL_KEYCODE(code);
  // Il tasto viene tolto da qualunque altro comando lo usasse
  KeyMapHash_Rebuild(cmd, key);
  KeyMapHash_Add(key, cmd);
}

int Config_GetPadButtonForCmd(int cmd) {
  for (int b = 0; b < kGamepadBtn_Count; b++) {
    for (int e = joymap_first[b]; e != 0; e = joymap_ents[e - 1].next) {
      if (joymap_ents[e - 1].cmd == cmd && joymap_ents[e - 1].modifiers == 0)
        return b;
    }
  }
  return -1;
}

const char *Config_PadButtonName(int button) {
  static const char *const kNames[kGamepadBtn_Count] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3",
    "L1", "R1", "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "L2", "R2",
  };
  return (button >= 0 && button < kGamepadBtn_Count) ? kNames[button] : "-";
}

static void GamepadMap_Rebuild(int drop_cmd, int drop_button) {
  typedef struct { int button; uint32 modifiers; uint16 cmd; } Ent;
  Ent *list = malloc(sizeof(Ent) * (joymap_size + 1));
  int n = 0;
  for (int b = 0; b < kGamepadBtn_Count; b++) {
    for (int e = joymap_first[b]; e != 0; e = joymap_ents[e - 1].next) {
      GamepadMapEnt *ent = &joymap_ents[e - 1];
      if (ent->cmd == drop_cmd || (b == drop_button && ent->modifiers == 0))
        continue;
      list[n].button = b, list[n].modifiers = ent->modifiers, list[n].cmd = ent->cmd, n++;
    }
  }
  memset(joymap_first, 0, sizeof(joymap_first));
  joymap_size = 0;
  for (int i = 0; i < n; i++)
    GamepadMap_Add(list[i].button, list[i].modifiers, list[i].cmd);
  free(list);
}

void Config_SetPadButtonForCmd(int cmd, int button) {
  GamepadMap_Rebuild(cmd, button);
  GamepadMap_Add(button, 0, cmd);
}

void Config_ResetControlsToDefault() {
  for (int i = 0; i < 12; i++) {
    int key = kDefaultKbdControls[kKeys_Controls + i];
    KeyMapHash_Rebuild(kKeys_Controls + i, key);
    KeyMapHash_Add(key, kKeys_Controls + i);
    Config_SetPadButtonForCmd(kKeys_Controls + i, kDefaultGamepadCmds[i]);
  }
  // anche il tasto del menu opzioni (Esc e R3): altrimenti, se era stato
  // spostato su un tasto dei comandi, il menu non si aprirebbe piu'
  int menu_key = kDefaultKbdControls[kKeys_HdMenu];
  KeyMapHash_Rebuild(kKeys_HdMenu, menu_key);
  KeyMapHash_Add(menu_key, kKeys_HdMenu);
  Config_SetPadButtonForCmd(kKeys_HdMenu, kGamepadBtn_R3);
}

static bool LineIsKey(const char *line, const char *key) {
  while (*line == ' ' || *line == '\t') line++;
  const char *r = StringStartsWithNoCase(line, key);
  if (!r)
    return false;
  while (*r == ' ' || *r == '\t') r++;
  return *r == '=';
}

static bool LineIsSection(const char *line, char *out, size_t size) {
  while (*line == ' ' || *line == '\t') line++;
  if (*line != '[')
    return false;
  size_t i = 0;
  while (line[i] && line[i] != ']' && i + 2 < size)
    out[i] = line[i], i++;
  out[i] = ']', out[i + 1] = 0;
  return true;
}

typedef struct OutBuf {
  char *data;
  size_t len, cap;
} OutBuf;

static void OutBuf_Add(OutBuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->cap + n) * 2;
    b->data = realloc(b->data, b->cap);
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
}

static void OutBuf_AddStr(OutBuf *b, const char *s) {
  OutBuf_Add(b, s, strlen(s));
}

static void OutBuf_AddValue(OutBuf *b, const IniValue *v, const char *nl) {
  OutBuf_AddStr(b, v->key);
  OutBuf_AddStr(b, " = ");
  OutBuf_AddStr(b, v->value);
  OutBuf_AddStr(b, nl);
}

// Scrive le chiavi della sezione non ancora presenti nel file
static void FlushSection(OutBuf *b, const IniValue *values, bool *done, int count, const char *section, const char *nl) {
  for (int k = 0; k < count; k++) {
    if (!done[k] && StringEqualsNoCase(values[k].section, section)) {
      OutBuf_AddValue(b, &values[k], nl);
      done[k] = true;
    }
  }
}

// Aggiorna i valori indicati nel file ini caricato, lasciando intatti
// commenti e righe non toccate. Le chiavi mancanti vengono aggiunte in fondo
// alla loro sezione, le sezioni mancanti in fondo al file.
bool Config_WriteIniValues(const IniValue *values, int count) {
  size_t len = 0;
  char *data = (char *)ReadWholeFile(g_config_filename, &len);
  if (!data)
    return false;
  bool *done = calloc(count, sizeof(bool));
  OutBuf b = { malloc(len + 4096), 0, len + 4096 };
  char section[64] = "";
  const char *nl = strstr(data, "\r\n") ? "\r\n" : "\n";

  char *p = data, *end = data + len;
  while (p < end) {
    char *eol = memchr(p, '\n', end - p);
    char *next = eol ? eol + 1 : end;
    size_t linelen = next - p;
    char line[1024];
    size_t cl = linelen < sizeof(line) - 1 ? linelen : sizeof(line) - 1;
    memcpy(line, p, cl);
    line[cl] = 0;
    char newsec[64];
    if (LineIsSection(line, newsec, sizeof(newsec))) {
      if (section[0])
        FlushSection(&b, values, done, count, section, nl);
      strcpy(section, newsec);
      OutBuf_Add(&b, p, linelen);
    } else {
      int found = -1;
      if (line[0] != '#' && line[0] != ';') {
        for (int k = 0; k < count; k++) {
          if (StringEqualsNoCase(values[k].section, section) && LineIsKey(line, values[k].key)) {
            found = k;
            break;
          }
        }
      }
      if (found >= 0) {
        OutBuf_AddValue(&b, &values[found], nl);
        done[found] = true;
      } else {
        OutBuf_Add(&b, p, linelen);
      }
    }
    p = next;
  }
  if (b.len && b.data[b.len - 1] != '\n')
    OutBuf_AddStr(&b, nl);
  if (section[0])
    FlushSection(&b, values, done, count, section, nl);
  // Sezioni non presenti nel file
  for (int k = 0; k < count; k++) {
    if (done[k])
      continue;
    OutBuf_AddStr(&b, nl);
    OutBuf_AddStr(&b, values[k].section);
    OutBuf_AddStr(&b, nl);
    FlushSection(&b, values, done, count, values[k].section, nl);
  }
  // Si scrive prima un file temporaneo e lo si mette al posto dell'ini solo
  // se la scrittura e' riuscita: l'ini dell'utente non resta mai a meta'
  bool ok = false;
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_config_filename);
  FILE *f = fopen(tmp, "wb");
  if (f) {
    ok = fwrite(b.data, 1, b.len, f) == b.len;
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, g_config_filename, 1 /* MOVEFILE_REPLACE_EXISTING */);
#else
    ok = ok && rename(tmp, g_config_filename) == 0;
#endif
    if (!ok)
      remove(tmp);
  }
  free(b.data);
  free(done);
  free(data);
  return ok;
}
