#pragma once
#include "types.h"
#include <SDL_keycode.h>

enum {
  kKeys_Null,
  kKeys_Controls,
  kKeys_Controls_Last = kKeys_Controls + 11,
  kKeys_Load,
  kKeys_Load_Last = kKeys_Load + 19,
  kKeys_Save,
  kKeys_Save_Last = kKeys_Save + 19,
  kKeys_Replay,
  kKeys_Replay_Last = kKeys_Replay + 19,
  kKeys_LoadRef,
  kKeys_LoadRef_Last = kKeys_LoadRef + 19,
  kKeys_ReplayRef,
  kKeys_ReplayRef_Last = kKeys_ReplayRef + 19,
  kKeys_CheatLife,
  kKeys_CheatKeys,
  kKeys_CheatEquipment,
  kKeys_CheatWalkThroughWalls,
  kKeys_ClearKeyLog,
  kKeys_StopReplay,
  kKeys_Fullscreen,
  kKeys_Reset,
  kKeys_Pause,
  kKeys_PauseDimmed,
  kKeys_Turbo,
  kKeys_ReplayTurbo,
  kKeys_WindowBigger,
  kKeys_WindowSmaller,
  kKeys_DisplayPerf,
  kKeys_ToggleRenderer,
  kKeys_VolumeUp,
  kKeys_VolumeDown,
  kKeys_HdMenu,
  kKeys_Total,
};

enum {
  kOutputMethod_SDL,
  kOutputMethod_SDLSoftware,
  kOutputMethod_OpenGL,
  kOutputMethod_OpenGL_ES,
};

typedef struct Config {
  int window_width;
  int window_height;
  bool enhanced_mode7;
  bool new_renderer;
  bool ignore_aspect_ratio;
  uint8 fullscreen;
  uint8 window_scale;
  bool enable_audio;
  bool linear_filtering;
  uint8 output_method;
  uint16 audio_freq;
  uint8 audio_channels;
  uint16 audio_samples;
  bool autosave;
  uint8 extended_aspect_ratio;
  bool extend_y;
  bool no_sprite_limits;
  bool display_perf_title;
  uint8 enable_msu;
  bool resume_msu;
  bool disable_frame_delay;
  uint8 msuvolume;
  uint32 features0;
  // Migliorie HD
  uint8 smooth_camera;      // 0 = spenta, 1-3 = intensita'
  bool hd_text;
  bool modern_text;         // dialoghi con la fascia trasparente e il carattere del contatore
  bool high_refresh;
  bool dynamic_light;
  // Effetti visivi aggiuntivi
  // fx_heat: braci da torce, bracieri, fuoco e lava, calore sopra la lava e
  // animazioni del fondale al chiuso a meta' velocita'
  bool fx_heat, fx_window_rays, fx_item_glow, fx_rain;
  bool fx_menu_blur, fx_sprite_shadows, fx_soft_iris, fx_button_hints, fx_water, fx_whirlpool, fx_fog;
  // aspetto dei disegni ufficiali (hd_art.c): Agahnim rosso, re in rosso,
  // medaglione con la stella della versione giapponese; Link biondo a parte
  bool official_artwork, blond_link;
  // divise da SwordAndItemsInteraction: danni dei nemici come su GBA (si
  // applicano a inizio partita) e immersione con le pinne; divisa da
  // BurnAndFreezeInteraction: frecce che restano piantate nei muri
  bool gba_damage, flipper_dive, stuck_arrows;
  // Giravolta Tornado come su GBA, sbloccata dal Retino per Farfalle
  bool hurricane_spin;
  // Oggetti assegnabili al tasto X e ai tasti L e R nel menu degli oggetti
  // (divise da ItemSwitchLR, che ora e' solo il cambio rapido con L e R)
  bool item_on_x, item_on_lr;
  uint8 text_speed;         // 0 = originale, 1 = veloce, 2 = istantaneo
  uint8 rumble;             // vibrazione del controller: 0 spenta, 1 leggera, 2 media, 3 forte

  const char *link_graphics;
  char *memory_buffer;
  const char *shader;
  const char *msu_path;
  const char *language;
} Config;

enum {
  kMsuEnabled_Msu = 1,
  kMsuEnabled_MsuDeluxe = 2,
  kMsuEnabled_Opuz = 4,
};
enum {
  kGamepadBtn_Invalid = -1,
  kGamepadBtn_A,
  kGamepadBtn_B,
  kGamepadBtn_X,
  kGamepadBtn_Y,
  kGamepadBtn_Back,
  kGamepadBtn_Guide,
  kGamepadBtn_Start,
  kGamepadBtn_L3,
  kGamepadBtn_R3,
  kGamepadBtn_L1,
  kGamepadBtn_R1,
  kGamepadBtn_DpadUp,
  kGamepadBtn_DpadDown,
  kGamepadBtn_DpadLeft,
  kGamepadBtn_DpadRight,
  kGamepadBtn_L2,
  kGamepadBtn_R2,
  kGamepadBtn_Count,
};

extern Config g_config;

void ParseConfigFile(const char *filename);
int FindCmdForSdlKey(SDL_Keycode code, SDL_Keymod mod);
int FindCmdForGamepadButton(int button, uint32 modifiers);

// Funzioni per il menu opzioni (hd_menu.c)
const char *Config_GetFilename();
int Config_GetKeyForCmd(int cmd);
void Config_KeyName(int key, char *buf, size_t size);
void Config_SetKeyForCmd(int cmd, SDL_Keycode code, SDL_Keymod mod);
int Config_GetPadButtonForCmd(int cmd);
const char *Config_PadButtonName(int button);
void Config_SetPadButtonForCmd(int cmd, int button);
void Config_ResetControlsToDefault();

typedef struct IniValue {
  const char *section;  // es. "[Graphics]"
  const char *key;
  char value[160];
} IniValue;
bool Config_WriteIniValues(const IniValue *values, int count);
