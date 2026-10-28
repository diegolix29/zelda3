#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <SDL.h>
#ifdef _WIN32
#include "platform/win32/volume_control.h"
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "snes/ppu.h"

#include "types.h"
#include "variables.h"

#include "zelda_rtl.h"
#include "zelda_cpu_infra.h"

#include "config.h"
#include "assets.h"
#include "load_gfx.h"
#include "util.h"
#include "audio.h"
#include "hd_menu.h"
#include "hd_3d.h"
#include "hd_art.h"

static bool g_run_without_emu = 0;

// Forwards
static bool LoadRom(const char *filename);
static void LoadLinkGraphics();
static void RenderNumber(uint8 *dst, size_t pitch, int n, bool big);
static void HandleInput(int keyCode, int modCode, bool pressed);
static void HandleCommand(uint32 j, bool pressed);
static int RemapSdlButton(int button);
static void HandleGamepadInput(int button, bool pressed);
static void HandleGamepadAxisInput(int gamepad_id, int axis, int value);
static void OpenOneGamepad(int i);
static void GamepadSetHints(void);
static void GamepadInit(void);
static void GamepadJoystickAdded(int i);
static void CloseOneGamepad(SDL_JoystickID id);
static void HandleVolumeAdjustment(int volume_adjustment);
static void LoadAssets();
static void SwitchDirectory();

enum {
  kDefaultFullscreen = 0,
  kMaxWindowScale = 10,
  kDefaultFreq = 44100,
  kDefaultChannels = 2,
  kDefaultSamples = 2048,
};

static const char kWindowTitle[] = "The Legend of Zelda: A Link to the Past";
static uint32 g_win_flags = SDL_WINDOW_RESIZABLE;
static SDL_Window *g_window;

static uint8 g_paused, g_turbo, g_replay_turbo = true, g_cursor = true;
static uint8 g_current_window_scale;
static uint8 g_gamepad_buttons;
static int g_input1_state;
static bool g_display_perf;
static int g_curr_fps;
static int g_ppu_render_flags = 0;
static int g_snes_width, g_snes_height;
static int g_sdl_audio_mixer_volume = SDL_MIX_MAXVOLUME;
static struct RendererFuncs g_renderer_funcs;
static uint32 g_gamepad_modifiers;
static uint16 g_gamepad_last_cmd[kGamepadBtn_Count];
static bool g_running = true;
static bool g_pause_dimmed, g_pause_redraw;
static int g_display_refresh = 60;

// Frequenza dello schermo su cui si trova la finestra
static void UpdateDisplayRefresh() {
  SDL_DisplayMode mode;
  int display = SDL_GetWindowDisplayIndex(g_window);
  if (SDL_GetCurrentDisplayMode(display < 0 ? 0 : display, &mode) == 0 && mode.refresh_rate > 0)
    g_display_refresh = mode.refresh_rate;
}

static bool HighRefreshActive() {
  return g_config.high_refresh && g_display_refresh > 62;
}

void NORETURN Die(const char *error) {
#if defined(NDEBUG) && defined(_WIN32)
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
#endif
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

void ChangeWindowScale(int scale_step) {
  if ((SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) != 0)
    return;
  int screen = SDL_GetWindowDisplayIndex(g_window);
  if (screen < 0) screen = 0;
  int max_scale = kMaxWindowScale;
  SDL_Rect bounds;
  int bt = -1, bl, bb, br;
  // note this takes into effect Windows display scaling, i.e., resolution is divided by scale factor
  if (SDL_GetDisplayUsableBounds(screen, &bounds) == 0) {
    // this call may take a while before it is reported by Windows (or not at all in my testing)
    if (SDL_GetWindowBordersSize(g_window, &bt, &bl, &bb, &br) != 0) {
      // guess based on Windows 10/11 defaults
      bl = br = bb = 1;
      bt = 31;
    }
    // Allow a scale level slightly above the max that fits on screen
    int mw = (bounds.w - bl - br + g_snes_width / 4) / g_snes_width;
    int mh = (bounds.h - bt - bb + g_snes_height / 4) / g_snes_height;
    max_scale = IntMin(mw, mh);
  }
  int new_scale = IntMax(IntMin(g_current_window_scale + scale_step, max_scale), 1);
  g_current_window_scale = new_scale;
  int w = new_scale * g_snes_width;
  int h = new_scale * g_snes_height;

  //SDL_RenderSetLogicalSize(g_renderer, w, h);
  SDL_SetWindowSize(g_window, w, h);
  if (bt >= 0) {
    // Center the window on top of the mouse
    int mx, my;
    SDL_GetGlobalMouseState(&mx, &my);
    int wx = IntMax(IntMin(mx - w / 2, bounds.x + bounds.w - bl - br - w), bounds.x + bl);
    int wy = IntMax(IntMin(my - h / 2, bounds.y + bounds.h - bt - bb - h), bounds.y + bt);
    SDL_SetWindowPosition(g_window, wx, wy);
  } else {
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

#define RESIZE_BORDER 20
static SDL_HitTestResult HitTestCallback(SDL_Window *win, const SDL_Point *pt, void *data) {
  uint32 flags = SDL_GetWindowFlags(win);
  if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 || (flags & SDL_WINDOW_FULLSCREEN) != 0)
    return SDL_HITTEST_NORMAL;

  if ((SDL_GetModState() & KMOD_CTRL) != 0)
    return SDL_HITTEST_DRAGGABLE;

  int w, h;
  SDL_GetWindowSize(win, &w, &h);

  if (pt->y < RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPLEFT :
           (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
  } else if (pt->y >= h - RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMLEFT :
           (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
  } else {
    if (pt->x < RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_LEFT;
    } else if (pt->x >= w - RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_RIGHT;
    }
  }
  return SDL_HITTEST_NORMAL;
}

static void DrawPpuFrameWithPerf() {
  int render_scale = PpuGetCurrentRenderScale(g_zenv.ppu, g_ppu_render_flags);
  uint8 *pixel_buffer = 0;
  int pitch = 0;

  g_renderer_funcs.BeginDraw(g_snes_width * render_scale,
                             g_snes_height * render_scale,
                             &pixel_buffer, &pitch);
  if (g_display_perf || g_config.display_perf_title) {
    static float history[64], average;
    static int history_pos;
    uint64 before = SDL_GetPerformanceCounter();
    ZeldaDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
    uint64 after = SDL_GetPerformanceCounter();
    float v = (double)SDL_GetPerformanceFrequency() / (after - before);
    average += v - history[history_pos];
    history[history_pos] = v;
    history_pos = (history_pos + 1) & 63;
    g_curr_fps = average * (1.0f / 64);
  } else {
    ZeldaDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
  }
  if (g_display_perf)
    RenderNumber(pixel_buffer + pitch * render_scale, pitch, g_curr_fps, render_scale == 4);
  HdMenu_Draw(pixel_buffer, pitch, g_snes_width * render_scale, g_snes_height * render_scale, render_scale);
  // Pausa scurita: funziona con qualunque metodo di output
  if (g_paused && g_pause_dimmed) {
    for (int y = 0; y < g_snes_height * render_scale; y++) {
      uint32 *row = (uint32 *)(pixel_buffer + y * pitch);
      for (int x = 0; x < g_snes_width * render_scale; x++)
        row[x] = ((row[x] >> 1 & 0x7f7f7f) - (row[x] >> 3 & 0x1f1f1f)) | (row[x] & 0xff000000);
    }
  }
  g_renderer_funcs.EndDraw();
}

static SDL_mutex *g_audio_mutex;
static uint8 *g_audiobuffer, *g_audiobuffer_cur, *g_audiobuffer_end;
static int g_frames_per_block;
static uint8 g_audio_channels;

static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  if (SDL_LockMutex(g_audio_mutex)) Die("Mutex lock failed!");
  while (len != 0) {
    if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
      ZeldaRenderAudio((int16*)g_audiobuffer, g_frames_per_block, g_audio_channels);
      g_audiobuffer_cur = g_audiobuffer;
      g_audiobuffer_end = g_audiobuffer + g_frames_per_block * g_audio_channels * sizeof(int16);
    }
    int n = IntMin(len, g_audiobuffer_end - g_audiobuffer_cur);
    if (g_sdl_audio_mixer_volume == SDL_MIX_MAXVOLUME) {
      memcpy(stream, g_audiobuffer_cur, n);
    } else {
      SDL_memset(stream, 0, n);
      SDL_MixAudioFormat(stream, g_audiobuffer_cur, AUDIO_S16, n, g_sdl_audio_mixer_volume);
    }
    g_audiobuffer_cur += n;
    stream += n;
    len -= n;
  }

  ZeldaDiscardUnusedAudioFrames();
  SDL_UnlockMutex(g_audio_mutex);
}

// State for sdl renderer
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static SDL_Rect g_sdl_renderer_rect;
static uint8 *g_sdl_pixels;
static int g_sdl_pitch;
static SDL_Texture *g_sdl_overlay;
static int g_sdl_overlay_w, g_sdl_overlay_h;
static uint32 *g_sdl_overlay_pixels;
static int g_sdl_overlay_cap;

static bool SdlRenderer_Init(SDL_Window *window) {

  if (g_config.shader)
    fprintf(stderr, "Warning: Shaders are supported only with the OpenGL backend\n");

  SDL_Renderer *renderer = SDL_CreateRenderer(g_window, -1,
                                              g_config.output_method == kOutputMethod_SDLSoftware ? SDL_RENDERER_SOFTWARE :
                                              SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer == NULL) {
    printf("Failed to create renderer: %s\n", SDL_GetError());
    return false;
  }
  SDL_RendererInfo renderer_info;
  SDL_GetRendererInfo(renderer, &renderer_info);
  if (kDebugFlag) {
    printf("Supported texture formats:");
    for (int i = 0; i < renderer_info.num_texture_formats; i++)
      printf(" %s", SDL_GetPixelFormatName(renderer_info.texture_formats[i]));
    printf("\n");
  }
  g_renderer = renderer;
  if (!g_config.ignore_aspect_ratio)
    SDL_RenderSetLogicalSize(renderer, g_snes_width, g_snes_height);
  if (g_config.linear_filtering)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

  int tex_mult = (g_ppu_render_flags & kPpuRenderFlags_4x4Mode7) ? 4 : 1;
  g_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                g_snes_width * tex_mult, g_snes_height * tex_mult);
  if (g_texture == NULL) {
    printf("Failed to create texture: %s\n", SDL_GetError());
    return false;
  }
  return true;
}

static void SdlRenderer_Destroy() {
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
}

static void SdlRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  g_sdl_renderer_rect.w = width;
  g_sdl_renderer_rect.h = height;
  if (SDL_LockTexture(g_texture, &g_sdl_renderer_rect, (void **)pixels, pitch) != 0) {
    printf("Failed to lock texture: %s\n", SDL_GetError());
    return;
  }
  g_sdl_pixels = *pixels;
  g_sdl_pitch = *pitch;
}

static void SdlRenderer_EndDraw() {
  // Testo nitido: si prepara il riquadro dei dialoghi ingrandito 4x
  int rx, ry, rw, rh;
  bool hd_text = HdText_GetRect(g_sdl_renderer_rect.w, g_sdl_renderer_rect.h, &rx, &ry, &rw, &rh);
  if (hd_text) {
    if (rw * rh * 16 > g_sdl_overlay_cap) {
      g_sdl_overlay_cap = rw * rh * 16;
      free(g_sdl_overlay_pixels);
      g_sdl_overlay_pixels = malloc(g_sdl_overlay_cap * 4);
    }
    HdText_Scale4x(g_sdl_pixels, g_sdl_pitch, rx, ry, rw, rh, g_sdl_overlay_pixels);
  }

//  uint64 before = SDL_GetPerformanceCounter();
  SDL_UnlockTexture(g_texture);
//  uint64 after = SDL_GetPerformanceCounter();
//  float v = (double)(after - before) / SDL_GetPerformanceFrequency();
//  printf("%f ms\n", v * 1000);
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, &g_sdl_renderer_rect, NULL);
  if (hd_text) {
    if (!g_sdl_overlay || g_sdl_overlay_w != rw * 4 || g_sdl_overlay_h != rh * 4) {
      if (g_sdl_overlay)
        SDL_DestroyTexture(g_sdl_overlay);
      g_sdl_overlay_w = rw * 4, g_sdl_overlay_h = rh * 4;
      g_sdl_overlay = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        g_sdl_overlay_w, g_sdl_overlay_h);
    }
    if (g_sdl_overlay) {
      SDL_UpdateTexture(g_sdl_overlay, NULL, g_sdl_overlay_pixels, g_sdl_overlay_w * 4);
      SDL_SetTextureBlendMode(g_sdl_overlay, SDL_BLENDMODE_BLEND);
      SDL_Rect dst = { rx, ry, rw, rh };
      if (g_config.ignore_aspect_ratio) {
        int ow, oh;
        SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
        dst.x = rx * ow / g_sdl_renderer_rect.w, dst.w = rw * ow / g_sdl_renderer_rect.w;
        dst.y = ry * oh / g_sdl_renderer_rect.h, dst.h = rh * oh / g_sdl_renderer_rect.h;
      }
      SDL_RenderCopy(g_renderer, g_sdl_overlay, NULL, &dst);
    }
  }
  SDL_RenderPresent(g_renderer); // vsyncs to 60 FPS?
}

static const struct RendererFuncs kSdlRendererFuncs  = {
  &SdlRenderer_Init,
  &SdlRenderer_Destroy,
  &SdlRenderer_BeginDraw,
  &SdlRenderer_EndDraw,
};

void OpenGLRenderer_Create(struct RendererFuncs *funcs, bool use_opengl_es);

#undef main
int main(int argc, char** argv) {
  argc--, argv++;
  const char *config_file = NULL;
  if (argc >= 2 && strcmp(argv[0], "--config") == 0) {
    config_file = argv[1];
    argc -= 2, argv += 2;
  } else {
    SwitchDirectory();
  }
  ParseConfigFile(config_file);
  LoadAssets();
  LoadLinkGraphics();
  HdArt_Apply();

  ZeldaInitialize();
  g_zenv.ppu->extraLeftRight = UintMin(g_config.extended_aspect_ratio, kPpuExtraLeftRight);
  // le fasce si limitano a kPpuExtraLeftRight anche per la finestra (18:9 con extend_y)
  g_snes_width = (g_zenv.ppu->extraLeftRight * 2 + 256);
  g_snes_height = (g_config.extend_y ? 240 : 224);


  // Delay actually setting those features in ram until any snapshots finish playing.
  g_wanted_zelda_features = g_config.features0;
  HdMenu_ApplyConfig();
  Hd3d_Init();

  g_ppu_render_flags = g_config.new_renderer * kPpuRenderFlags_NewRenderer |
                       g_config.enhanced_mode7 * kPpuRenderFlags_4x4Mode7 |
                       g_config.extend_y * kPpuRenderFlags_Height240 |
                       g_config.no_sprite_limits * kPpuRenderFlags_NoSpriteLimits;
  ZeldaEnableMsu(g_config.enable_msu);
  ZeldaSetLanguage(g_config.language);

  if (g_config.fullscreen == 1)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
  else if (g_config.fullscreen == 2)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN;

  // Window scale (1=100%, 2=200%, 3=300%, etc.)
  g_current_window_scale = (g_config.window_scale == 0) ? 2 : IntMin(g_config.window_scale, kMaxWindowScale);

  // audio_freq: Use common sampling rates (see user config file. values higher than 48000 are not supported.)
  if (g_config.audio_freq < 11025 || g_config.audio_freq > 48000)
    g_config.audio_freq = kDefaultFreq;

  // Currently, the SPC/DSP implementation only supports up to stereo.
  if (g_config.audio_channels < 1 || g_config.audio_channels > 2)
    g_config.audio_channels = kDefaultChannels;

  // audio_samples: power of 2
  if (g_config.audio_samples <= 0 || ((g_config.audio_samples & (g_config.audio_samples - 1)) != 0))
    g_config.audio_samples = kDefaultSamples;

  // set up SDL
  GamepadSetHints();
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    printf("Failed to init SDL: %s\n", SDL_GetError());
    return 1;
  }

  bool custom_size  = g_config.window_width != 0 && g_config.window_height != 0;
  int window_width  = custom_size ? g_config.window_width  : g_current_window_scale * g_snes_width;
  int window_height = custom_size ? g_config.window_height : g_current_window_scale * g_snes_height;

  if (g_config.output_method == kOutputMethod_OpenGL ||
      g_config.output_method == kOutputMethod_OpenGL_ES) {
    g_win_flags |= SDL_WINDOW_OPENGL;
    OpenGLRenderer_Create(&g_renderer_funcs, (g_config.output_method == kOutputMethod_OpenGL_ES));
  } else {
    g_renderer_funcs = kSdlRendererFuncs;
  }

  SDL_Window* window = SDL_CreateWindow(kWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_width, window_height, g_win_flags);
  if(window == NULL) {
    printf("Failed to create window: %s\n", SDL_GetError());
    return 1;
  }
  g_window = window;
  SDL_SetWindowHitTest(window, HitTestCallback, NULL);
  UpdateDisplayRefresh();

  if (!g_renderer_funcs.Initialize(window))
    return 1;

  SDL_AudioDeviceID device = 0;
  SDL_AudioSpec want = { 0 }, have;
  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("No mutex");

  if (g_config.enable_audio) {
    want.freq = g_config.audio_freq;
    want.format = AUDIO_S16;
    want.channels = g_config.audio_channels;
    want.samples = g_config.audio_samples;
    want.callback = &AudioCallback;
    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (device == 0) {
      printf("Failed to open audio device: %s\n", SDL_GetError());
      return 1;
    }
    g_audio_channels = have.channels;
    g_frames_per_block = (534 * have.freq) / 32000;
    g_audiobuffer = malloc(g_frames_per_block * have.channels * sizeof(int16));
  }

  if (argc >= 1 && !g_run_without_emu)
    LoadRom(argv[0]);

#if defined(_WIN32)
  _mkdir("saves");
#else
  mkdir("saves", 0755);
#endif

  ZeldaReadSram();

  GamepadInit();
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    OpenOneGamepad(i);

  SDL_Event event;
  uint32 lastTick = SDL_GetTicks();
  uint32 curTick = 0;
  uint32 frameCtr = 0;
  bool audiopaused = true;

  if (g_config.autosave)
    HandleCommand(kKeys_Load + 0, true);

  while(g_running) {
    while(SDL_PollEvent(&event)) {
      switch(event.type) {
      case SDL_CONTROLLERDEVICEADDED:
        OpenOneGamepad(event.cdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        CloseOneGamepad(event.cdevice.which);
        break;
      case SDL_JOYDEVICEADDED:
        GamepadJoystickAdded(event.jdevice.which);
        break;
      case SDL_CONTROLLERAXISMOTION:
        HandleGamepadAxisInput(event.caxis.which, event.caxis.axis, event.caxis.value);
        HdMenu_HandleStick(g_gamepad_buttons);
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        int b = RemapSdlButton(event.cbutton.button);
        if (b >= 0)
          HandleGamepadInput(b, event.type == SDL_CONTROLLERBUTTONDOWN);
        break;
      }
      case SDL_MOUSEWHEEL:
        if (SDL_GetModState() & KMOD_CTRL && event.wheel.y != 0)
          ChangeWindowScale(event.wheel.y > 0 ? 1 : -1);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT && event.button.state == SDL_PRESSED && event.button.clicks == 2) {
          if ((g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 && (g_win_flags & SDL_WINDOW_FULLSCREEN) == 0 && SDL_GetModState() & KMOD_SHIFT) {
            g_win_flags ^= SDL_WINDOW_BORDERLESS;
            SDL_SetWindowBordered(g_window, (g_win_flags & SDL_WINDOW_BORDERLESS) == 0);
          }
        }
        break;
      case SDL_KEYDOWN:
        if (!HdMenu_HandleKey(event.key.keysym.sym, event.key.keysym.mod, true))
          HandleInput(event.key.keysym.sym, event.key.keysym.mod, true);
        break;
      case SDL_KEYUP:
        if (!HdMenu_HandleKey(event.key.keysym.sym, event.key.keysym.mod, false))
          HandleInput(event.key.keysym.sym, event.key.keysym.mod, false);
        break;
      case SDL_QUIT:
        g_running = false;
        break;
      }
    }

    bool menu_open = HdMenu_IsOpen();
    if ((g_paused || menu_open) != audiopaused) {
      audiopaused = g_paused || menu_open;
      if (device)
        SDL_PauseAudioDevice(device, audiopaused);
    }

    // Con il menu opzioni aperto il gioco e' fermo: si ridisegna solo
    // l'ultimo frame con il menu sopra (anche in pausa, altrimenti il menu
    // sarebbe aperto ma invisibile)
    if (menu_open) {
      DrawPpuFrameWithPerf();
      SDL_Delay(16);
      continue;
    }

    if (g_paused) {
      if (g_pause_redraw) {
        g_pause_redraw = false;
        DrawPpuFrameWithPerf();
      }
      SDL_Delay(16);
      continue;
    }

    // Schermi oltre i 60 Hz: il gioco avanza a 60 frame al secondo in base al
    // tempo reale e ogni aggiornamento dello schermo (sincronizzato col
    // vsync) disegna lo scorrimento interpolato fra gli ultimi due frame.
    if ((frameCtr & 127) == 0)
      UpdateDisplayRefresh();
    if (HighRefreshActive() && !g_turbo) {
      static uint64 last_counter;
      static double acc;
      uint64 now = SDL_GetPerformanceCounter();
      double freq = (double)SDL_GetPerformanceFrequency();
      if (last_counter == 0)
        last_counter = now;
      acc += (now - last_counter) / freq;
      last_counter = now;
      const double kFramePeriod = 1.0 / 60.0;
      if (acc > kFramePeriod * 4)
        acc = kFramePeriod * 4;  // dopo uno scatto non si recupera all'infinito
      int frames = 0;
      while (acc >= kFramePeriod && frames < 3) {
        int hr_inputs = g_input1_state;
        if (g_input1_state & 0xf0)
          g_gamepad_buttons = 0;
        hr_inputs |= g_gamepad_buttons;
        hr_inputs = Hd3d_RotateInput(hr_inputs);
        SDL_LockMutex(g_audio_mutex);
        bool hr_replay = ZeldaRunFrame(hr_inputs);
        HdMenu_AfterFrame(hr_replay);
        SDL_UnlockMutex(g_audio_mutex);
        frameCtr++;
        acc -= kFramePeriod;
        frames++;
      }
      g_interp_alpha = (float)(acc / kFramePeriod);
      if (g_interp_alpha > 1.0f) g_interp_alpha = 1.0f;
      DrawPpuFrameWithPerf();
      g_interp_alpha = 1.0f;
      // Se il vsync non e' attivo si evita di occupare la CPU al 100%
      double spent = (SDL_GetPerformanceCounter() - now) / freq;
      double interval = 1.0 / g_display_refresh;
      if (spent < interval * 0.5)
        SDL_Delay((uint32)((interval * 0.5 - spent) * 1000.0));
      continue;
    }

    // Clear gamepad inputs when joypad directional inputs to avoid wonkiness
    int inputs = g_input1_state;
    if (g_input1_state & 0xf0)
      g_gamepad_buttons = 0;
    inputs |= g_gamepad_buttons;
    inputs = Hd3d_RotateInput(inputs);   // third person 3D camera: walk relative to it

    SDL_LockMutex(g_audio_mutex);
    bool is_replay = ZeldaRunFrame(inputs);
    HdMenu_AfterFrame(is_replay);
    SDL_UnlockMutex(g_audio_mutex);

    frameCtr++;

    if ((g_turbo ^ (is_replay & g_replay_turbo)) && (frameCtr & (g_turbo ? 0xf : 0x7f)) != 0) {
      continue;
    }

    DrawPpuFrameWithPerf();

    if (g_config.display_perf_title) {
      char title[60];
      snprintf(title, sizeof(title), "%s | FPS: %d", kWindowTitle, g_curr_fps);
      SDL_SetWindowTitle(g_window, title);
    }

    // if vsync isn't working, delay manually
    curTick = SDL_GetTicks();

    if (!g_config.disable_frame_delay) {
      static const uint8 delays[3] = { 17, 17, 16 }; // 60 fps
      lastTick += delays[frameCtr % 3];

      if (lastTick > curTick) {
        uint32 delta = lastTick - curTick;
        if (delta > 500) {
          lastTick = curTick - 500;
          delta = 500;
        }
//        printf("Sleeping %d\n", delta);
        SDL_Delay(delta);
      } else if (curTick - lastTick > 500) {
        lastTick = curTick;
      }
    }
  }
  // Finestra chiusa con il menu opzioni aperto: le impostazioni si salvano lo stesso
  HdMenu_Close();
  if (g_config.autosave)
    HandleCommand(kKeys_Save + 0, true);

  // clean sdl
  if (g_config.enable_audio) {
    SDL_PauseAudioDevice(device, 1);
    SDL_CloseAudioDevice(device);
  }

  SDL_DestroyMutex(g_audio_mutex);
  free(g_audiobuffer);

  g_renderer_funcs.Destroy();

  SDL_DestroyWindow(window);
  SDL_Quit();
  //SaveConfigFile();
  return 0;
}

static void RenderDigit(uint8 *dst, size_t pitch, int digit, uint32 color, bool big) {
  static const uint8 kFont[] = {
    0x1c, 0x36, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x36, 0x1c,
    0x18, 0x1c, 0x1e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e,
    0x3e, 0x63, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x63, 0x7f,
    0x3e, 0x63, 0x60, 0x60, 0x3c, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x30, 0x38, 0x3c, 0x36, 0x33, 0x7f, 0x30, 0x30, 0x30, 0x78,
    0x7f, 0x03, 0x03, 0x03, 0x3f, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x1c, 0x06, 0x03, 0x03, 0x3f, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x7f, 0x63, 0x60, 0x60, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x0c,
    0x3e, 0x63, 0x63, 0x63, 0x3e, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x3e, 0x63, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x60, 0x30, 0x1e,
  };
  const uint8 *p = kFont + digit * 10;
  if (!big) {
    for (int y = 0; y < 10; y++, dst += pitch) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1)
          ((uint32 *)dst)[x] = color;
      }
    }
  } else {
    for (int y = 0; y < 10; y++, dst += pitch * 2) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1) {
          ((uint32 *)dst)[x * 2 + 1] = ((uint32 *)dst)[x * 2] = color;
          ((uint32 *)(dst+pitch))[x * 2 + 1] = ((uint32 *)(dst + pitch))[x * 2] = color;
        }
      }
    }
  }
}

static void RenderNumber(uint8 *dst, size_t pitch, int n, bool big) {
  char buf[32], *s;
  int i;
  sprintf(buf, "%d", n);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + ((pitch + i + 4) << big), pitch, *s - '0', 0x404040, big);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + (i << big), pitch, *s - '0', 0xffffff, big);
}

static void HandleCommand_Locked(uint32 j, bool pressed);

static void HandleCommand(uint32 j, bool pressed) {
  if (j <= kKeys_Controls_Last) {
    static const uint8 kKbdRemap[] = { 0, 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };
    if (pressed)
      g_input1_state |= 1 << kKbdRemap[j];
    else
      g_input1_state &= ~(1 << kKbdRemap[j]);
    return;
  }

  if (j == kKeys_Turbo) {
    g_turbo = pressed;
    return;
  }

  // Everything that might access audio state
  // (like SaveLoad and Reset) must have the lock.
  SDL_LockMutex(g_audio_mutex);
  HandleCommand_Locked(j, pressed);
  SDL_UnlockMutex(g_audio_mutex);
}

void ZeldaApuLock() {
  SDL_LockMutex(g_audio_mutex);
}

void ZeldaApuUnlock() {
  SDL_UnlockMutex(g_audio_mutex);
}


static void HandleCommand_Locked(uint32 j, bool pressed) {
  if (!pressed)
    return;
  if (j <= kKeys_Load_Last) {
    SaveLoadSlot(kSaveLoad_Load, j - kKeys_Load);
  } else if (j <= kKeys_Save_Last) {
    SaveLoadSlot(kSaveLoad_Save, j - kKeys_Save);
  } else if (j <= kKeys_Replay_Last) {
    SaveLoadSlot(kSaveLoad_Replay, j - kKeys_Replay);
  } else if (j <= kKeys_LoadRef_Last) {
    SaveLoadSlot(kSaveLoad_Load, 256 + j - kKeys_LoadRef);
  } else if (j <= kKeys_ReplayRef_Last) {
    SaveLoadSlot(kSaveLoad_Replay, 256 + j - kKeys_ReplayRef);
  } else {
    switch (j) {
    case kKeys_CheatLife: PatchCommand('w'); break;
    case kKeys_CheatEquipment: PatchCommand('W'); break;
    case kKeys_CheatKeys: PatchCommand('o'); break;
    case kKeys_CheatWalkThroughWalls: PatchCommand('E'); break;
    case kKeys_ClearKeyLog: PatchCommand('k'); break;
    case kKeys_StopReplay: PatchCommand('l'); break;
    case kKeys_Fullscreen:
      g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
      SDL_SetWindowFullscreen(g_window, g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP);
      g_cursor = !g_cursor;
      SDL_ShowCursor(g_cursor);
      break;
    case kKeys_Reset:
      ZeldaReset(true);
      break;
    case kKeys_Pause: g_paused = !g_paused; g_pause_dimmed = false; break;
    case kKeys_PauseDimmed:
      // Il frame scurito viene ridisegnato dal ciclo principale, cosi'
      // funziona anche con OpenGL (prima si usava solo il renderer SDL).
      g_paused = !g_paused;
      g_pause_dimmed = g_paused;
      g_pause_redraw = g_paused;
      break;
    case kKeys_ReplayTurbo: g_replay_turbo = !g_replay_turbo; break;
    case kKeys_WindowBigger: ChangeWindowScale(1); break;
    case kKeys_WindowSmaller: ChangeWindowScale(-1); break;
    case kKeys_DisplayPerf: g_display_perf ^= 1; break;
    case kKeys_ToggleRenderer: g_ppu_render_flags ^= kPpuRenderFlags_NewRenderer; break;
    case kKeys_VolumeUp:
    case kKeys_VolumeDown: HandleVolumeAdjustment(j == kKeys_VolumeUp ? 1 : -1); break;
    case kKeys_HdMenu: HdMenu_Open(); break;
    case kKeys_View3D: Hd3d_CycleLevel(); break;
    case kKeys_View3DCurve: Hd3d_CycleCurve(); break;
    case kKeys_View3DDrawDistance: Hd3d_CycleDrawDistance(); break;
    case kKeys_View3DDump: Hd3d_RequestDump(); break;
    case kKeys_View3DWalls: Hd3d_ToggleWalls(); break;
    case kKeys_View3DRoomDump: Hd3d_RequestRoomDump(); break;
    case kKeys_View3DCamera: Hd3d_ToggleCamera(); break;
    case kKeys_View3DRecenter: Hd3d_Recenter(); break;
    default: assert(0);
    }
  }
}

static void HandleInput(int keyCode, int keyMod, bool pressed) {
  int j = FindCmdForSdlKey(keyCode, keyMod);
  if (j != 0)
    HandleCommand(j, pressed);
  // Un tasto di gioco lasciato mentre si tiene Ctrl, Alt o Maiusc (per
  // esempio dopo Ctrl+E del no clip) si legge come "Ctrl+tasto" e non
  // corrisponde piu' al tasto premuto: restava premuto e Link continuava a
  // camminare da solo. Al rilascio il tasto senza modificatori si lascia sempre.
  if (!pressed && (keyMod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT))) {
    int k = FindCmdForSdlKey(keyCode, 0);
    if (k != 0 && k != j && k <= kKeys_Controls_Last)
      HandleCommand(k, false);
  }
}

// ---------------------------------------------------------------------------
// Controller support (Xbox, PlayStation, Switch, generic pads).
// All pads go through the SDL GameController API, which presents every pad
// with an Xbox-style layout: A = bottom face button (Cross on PlayStation),
// B = right, X = left, Y = top. So the [GamepadMap] in zelda3.ini works the
// same for every brand. Detection is written to zelda3_controllers.log.
// ---------------------------------------------------------------------------
#define kMaxOpenPads 8
static SDL_GameController *g_open_pads[kMaxOpenPads];
static SDL_JoystickID g_open_pad_ids[kMaxOpenPads];
static FILE *g_pad_log;

static void PadLog(const char *fmt, ...) {
  char buf[512];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  printf("[pad] %s\n", buf);
  if (!g_pad_log) {
    g_pad_log = fopen("zelda3_controllers.log", "w");
    if (!g_pad_log)
      return;
  }
  fprintf(g_pad_log, "[%8.2fs] %s\n", SDL_GetTicks() / 1000.0, buf);
  fflush(g_pad_log);
}

// Called before SDL_Init: these hints let SDL talk to PlayStation and Switch
// pads directly (USB and Bluetooth). They need SDL 2.0.9+ (DualSense: 2.0.14+)
// and are simply ignored by older SDL versions. Set with "default" priority,
// so environment variables can still override them.
static void GamepadSetHints(void) {
  SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI", "1", SDL_HINT_DEFAULT);
  SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS4", "1", SDL_HINT_DEFAULT);
  SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS5", "1", SDL_HINT_DEFAULT);
  SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_SWITCH", "1", SDL_HINT_DEFAULT);
  // Nintendo pads: map face buttons by position (like Xbox), not by label.
  SDL_SetHintWithPriority("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0", SDL_HINT_DEFAULT);
}

// Called after SDL_Init: log versions and load an optional gamecontrollerdb.txt
// (community mapping database, adds mappings for many generic pads).
static void GamepadInit(void) {
  SDL_version compiled, linked;
  SDL_VERSION(&compiled);
  SDL_GetVersion(&linked);
  char cwd[512] = "?";
#if defined(_WIN32)
  if (!_getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
#else
  if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
#endif
  PadLog("SDL compiled %d.%d.%d, SDL2.dll loaded %d.%d.%d", compiled.major, compiled.minor, compiled.patch,
         linked.major, linked.minor, linked.patch);
  PadLog("Log folder: %s", cwd);
  if (SDL_VERSIONNUM(linked.major, linked.minor, linked.patch) < SDL_VERSIONNUM(2, 0, 14))
    PadLog("WARNING: this SDL2.dll is too old for DualSense and direct PS4/Switch support (needs 2.0.14+)");

  const char *db_name = "gamecontrollerdb.txt";
  int n = SDL_GameControllerAddMappingsFromFile(db_name);
  if (n < 0) {
    char *base = SDL_GetBasePath();
    if (base) {
      char path[1024];
      snprintf(path, sizeof(path), "%s%s", base, db_name);
      SDL_free(base);
      n = SDL_GameControllerAddMappingsFromFile(path);
      if (n >= 0) db_name = "gamecontrollerdb.txt (next to exe)";
    }
  }
  if (n >= 0)
    PadLog("Loaded %d extra mappings from %s", n, db_name);
  else
    PadLog("No gamecontrollerdb.txt found (optional, only built-in mappings used)");
  PadLog("%d joystick device(s) found at startup", SDL_NumJoysticks());
}

static const char *GamepadTypeName(SDL_GameController *c) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
  SDL_version v;
  SDL_GetVersion(&v);
  if (SDL_VERSIONNUM(v.major, v.minor, v.patch) >= SDL_VERSIONNUM(2, 0, 12)) {
    switch (SDL_GameControllerGetType(c)) {
    case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_CONTROLLER_TYPE_PS3: return "PS3";
    case SDL_CONTROLLER_TYPE_PS4: return "PS4 (DualShock 4)";
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_TYPE_PS5: return "PS5 (DualSense)";
#endif
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    default: return "generic";
    }
  }
#endif
  (void)c;
  return "unknown (old SDL)";
}

static void OpenOneGamepad(int i) {
  char guid[64];
  SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
  const char *name = SDL_JoystickNameForIndex(i);
  if (!SDL_IsGameController(i)) {
    PadLog("Device %d \"%s\" GUID %s: NO MAPPING, ignored (try adding gamecontrollerdb.txt)",
           i, name ? name : "?", guid);
    return;
  }
  SDL_GameController *controller = SDL_GameControllerOpen(i);
  if (!controller) {
    PadLog("Device %d \"%s\" GUID %s: could not open: %s", i, name ? name : "?", guid, SDL_GetError());
    return;
  }
  SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
  int slot = -1;
  for (int k = 0; k < kMaxOpenPads; k++) {
    if (g_open_pads[k] && g_open_pad_ids[k] == id) {
      // Already open (SDL also sends "added" events for pads present at startup).
      SDL_GameControllerClose(controller);
      return;
    }
    if (!g_open_pads[k] && slot < 0)
      slot = k;
  }
  if (slot >= 0) {
    g_open_pads[slot] = controller;
    g_open_pad_ids[slot] = id;
  }
  PadLog("Connected: \"%s\" type=%s GUID %s id=%d", SDL_GameControllerName(controller),
         GamepadTypeName(controller), guid, (int)id);
  char *mapping = SDL_GameControllerMapping(controller);
  if (mapping) {
    PadLog("  mapping: %s", mapping);
    SDL_free(mapping);
  }
}

// Joystick added: only log devices that have no controller mapping, since
// mapped ones are handled by SDL_CONTROLLERDEVICEADDED / OpenOneGamepad.
static void GamepadJoystickAdded(int i) {
  if (!SDL_IsGameController(i))
    OpenOneGamepad(i);
}

static void CloseOneGamepad(SDL_JoystickID id) {
  for (int k = 0; k < kMaxOpenPads; k++) {
    if (g_open_pads[k] && g_open_pad_ids[k] == id) {
      PadLog("Disconnected: id=%d", (int)id);
      SDL_GameControllerClose(g_open_pads[k]);
      g_open_pads[k] = NULL;
    }
  }
  // Release any buttons still held so nothing gets stuck after unplugging.
  for (int b = 0; b < 32; b++)
    if (g_gamepad_modifiers & (1u << b))
      HandleGamepadInput(b, false);
  g_gamepad_buttons = 0;
}

static int RemapSdlButton(int button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return kGamepadBtn_A;
  case SDL_CONTROLLER_BUTTON_B: return kGamepadBtn_B;
  case SDL_CONTROLLER_BUTTON_X: return kGamepadBtn_X;
  case SDL_CONTROLLER_BUTTON_Y: return kGamepadBtn_Y;
  case SDL_CONTROLLER_BUTTON_BACK: return kGamepadBtn_Back;
  case SDL_CONTROLLER_BUTTON_GUIDE: return kGamepadBtn_Guide;
  case SDL_CONTROLLER_BUTTON_START: return kGamepadBtn_Start;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return kGamepadBtn_L3;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return kGamepadBtn_R3;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return kGamepadBtn_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return kGamepadBtn_R1;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return kGamepadBtn_DpadUp;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return kGamepadBtn_DpadDown;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return kGamepadBtn_DpadLeft;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return kGamepadBtn_DpadRight;
  default: return -1;
  }
}

static void HandleGamepadInput(int button, bool pressed) {
  if (!!(g_gamepad_modifiers & (1 << button)) == pressed)
    return;
  g_gamepad_modifiers ^= 1 << button;
  if (HdMenu_HandlePadButton(button, pressed)) {
    g_gamepad_last_cmd[button] = 0;
    return;
  }
  if (pressed)
    g_gamepad_last_cmd[button] = FindCmdForGamepadButton(button, g_gamepad_modifiers);
  if (g_gamepad_last_cmd[button] != 0)
    HandleCommand(g_gamepad_last_cmd[button], pressed);
}

static void HandleVolumeAdjustment(int volume_adjustment) {
#if SYSTEM_VOLUME_MIXER_AVAILABLE
  int current_volume = GetApplicationVolume();
  int new_volume = IntMin(IntMax(0, current_volume + volume_adjustment * 5), 100);
  SetApplicationVolume(new_volume);
  printf("[System Volume]=%i\n", new_volume);
#else
  g_sdl_audio_mixer_volume = IntMin(IntMax(0, g_sdl_audio_mixer_volume + volume_adjustment * (SDL_MIX_MAXVOLUME >> 4)), SDL_MIX_MAXVOLUME);
  printf("[SDL mixer volume]=%i\n", g_sdl_audio_mixer_volume);
#endif
}

// Approximates atan2(y, x) normalized to the [0,4) range
// with a maximum error of 0.1620 degrees
// normalized_atan(x) ~ (b x + x^2) / (1 + 2 b x + x^2)
static float ApproximateAtan2(float y, float x) {
  uint32 sign_mask = 0x80000000;
  float b = 0.596227f;
  // Extract the sign bits
  uint32 ux_s = sign_mask & *(uint32 *)&x;
  uint32 uy_s = sign_mask & *(uint32 *)&y;
  // Determine the quadrant offset
  float q = (float)((~ux_s & uy_s) >> 29 | ux_s >> 30);
  // Calculate the arctangent in the first quadrant
  float bxy_a = b * x * y;
  if (bxy_a < 0.0f) bxy_a = -bxy_a;  // avoid fabs
  float num = bxy_a + y * y;
  float atan_1q = num / (x * x + bxy_a + num + 0.000001f);
  // Translate it to the proper quadrant
  uint32_t uatan_2q = (ux_s ^ uy_s) | *(uint32 *)&atan_1q;
  return q + *(float *)&uatan_2q;
}

static void HandleGamepadAxisInput(int gamepad_id, int axis, int value) {
  static int last_gamepad_id, last_x, last_y;
  if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_LEFTY) {
    // ignore other gamepads unless they have a big input
    if (last_gamepad_id != gamepad_id) {
      if (value > -16000 && value < 16000)
        return;
      last_gamepad_id = gamepad_id;
      last_x = last_y = 0;
    }
    *(axis == SDL_CONTROLLER_AXIS_LEFTX ? &last_x : &last_y) = value;
    int buttons = 0;
    if (last_x * last_x + last_y * last_y >= 10000 * 10000) {
      // in the non deadzone part, divide the circle into eight 45 degree
      // segments rotated by 22.5 degrees that control which direction to move.
      // todo: do this without floats?
      static const uint8 kSegmentToButtons[8] = {
        1 << 4,           // 0 = up
        1 << 4 | 1 << 7,  // 1 = up, right
        1 << 7,           // 2 = right
        1 << 7 | 1 << 5,  // 3 = right, down
        1 << 5,           // 4 = down
        1 << 5 | 1 << 6,  // 5 = down, left
        1 << 6,           // 6 = left
        1 << 6 | 1 << 4,  // 7 = left, up
      };
      uint8 angle = (uint8)(int)(ApproximateAtan2(last_y, last_x) * 64.0f + 0.5f);
      buttons = kSegmentToButtons[(uint8)(angle + 16 + 64) >> 5];
    }
    g_gamepad_buttons = buttons;
  } else if (axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY) {
    Hd3d_RightStick(axis == SDL_CONTROLLER_AXIS_RIGHTY, value);   // third person 3D camera
  } else if ((axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
    if (value < 12000 || value >= 16000)  // hysteresis
      HandleGamepadInput(axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? kGamepadBtn_L2 : kGamepadBtn_R2, value >= 12000);
  }
}

static bool LoadRom(const char *filename) {
  size_t length = 0;
  uint8 *file = ReadWholeFile(filename, &length);
  if(!file) Die("Failed to read file");
  bool result = EmuInitialize(file, length);
  free(file);
  return result;
}

static bool ParseLinkGraphics(uint8 *file, size_t length) {
  if (length < 27 || memcmp(file, "ZSPR", 4) != 0)
    return false;
  uint32 pixel_offs = DWORD(file[9]);
  uint32 pixel_length = WORD(file[13]);
  uint32 palette_offs = DWORD(file[15]);
  uint32 palette_length = WORD(file[19]);
  if ((uint64)pixel_offs + pixel_length > length ||
      (uint64)palette_offs + palette_length > length ||
      pixel_length != 0x7000)
    return false;
  if (kPalette_ArmorAndGloves_SIZE != 150 || kLinkGraphics_SIZE != 0x7000)
    Die("ParseLinkGraphics: Invalid asset sizes");
  memcpy(kLinkGraphics, file + pixel_offs, 0x7000);
  if (palette_length >= 120)
    memcpy(kPalette_ArmorAndGloves, file + palette_offs, 120);
  if (palette_length >= 124)
    memcpy(kGlovesColor, file + palette_offs + 120, 4);
  return true;
}

static void LoadLinkGraphics() {
  if (g_config.link_graphics) {
    fprintf(stderr, "Loading Link Graphics: %s\n", g_config.link_graphics);
    size_t length = 0;
    uint8 *file = ReadWholeFile(g_config.link_graphics, &length);
    if (file == NULL || !ParseLinkGraphics(file, length))
      Die("Unable to load file");
    free(file);
  }
}


const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];

static void LoadAssets() {
  size_t length = 0;
  uint8 *data = ReadWholeFile("zelda3_assets.dat", &length);
  if (!data) {
    size_t bps_length, bps_src_length;
    uint8 *bps, *bps_src;
    bps = ReadWholeFile("zelda3_assets.bps", &bps_length);
    if (!bps)
      Die("Failed to read zelda3_assets.dat. Please see the README for information about how you get this file.");
    bps_src = ReadWholeFile("zelda3.sfc", &bps_src_length);
    if (!bps_src)
      Die("Missing file: zelda3.sfc");
    data = ApplyBps(bps_src, bps_src_length, bps, bps_length, &length);
    if (!data)
      Die("Unable to apply zelda3_assets.bps. Please make sure you got the right version of 'zelda3.sfc'");
  }

  static const char kAssetsSig[] = { kAssets_Sig };

  if (length < 16 + 32 + 32 + 8 + kNumberOfAssets * 4 ||
      memcmp(data, kAssetsSig, 48) != 0 ||
      *(uint32*)(data + 80) != kNumberOfAssets)
    Die("Invalid assets file");

  uint32 offset = 88 + kNumberOfAssets * 4 + *(uint32 *)(data + 84);

  for (size_t i = 0; i < kNumberOfAssets; i++) {
    uint32 size = *(uint32 *)(data + 88 + i * 4);
    offset = (offset + 3) & ~3;
    if ((uint64)offset + size > length)
      Die("Assets file corruption");
    g_asset_sizes[i] = size;
    g_asset_ptrs[i] = data + offset;
    offset += size;
  }

  if (g_config.features0 & kFeatures0_DimFlashes) { // patch dungeon floor palettes
    kPalette_DungBgMain[0x484] = 0x70;
    kPalette_DungBgMain[0x485] = 0x95;
    kPalette_DungBgMain[0x486] = 0x57;
  }
}

// Go some steps up and find zelda3.ini
static void SwitchDirectory() {
  char buf[4096];
  if (!getcwd(buf, sizeof(buf) - 32))
    return;
  size_t pos = strlen(buf);

  for (int step = 0; pos != 0 && step < 3; step++) {
    memcpy(buf + pos, "/zelda3.ini", 12);
    FILE *f = fopen(buf, "rb");
    if (f) {
      fclose(f);
      buf[pos] = 0;
      if (step != 0) {
        printf("Found zelda3.ini in %s\n", buf);
        int err = chdir(buf);
        (void)err;
      }
      return;
    }
    pos--;
    while (pos != 0 && buf[pos] != '/' && buf[pos] != '\\')
      pos--;
  }
}

MemBlk FindInAssetArray(int asset, int idx) {
  return FindIndexInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, idx);
}

// ---------------------------------------------------------------------------
// Funzioni usate dal menu opzioni (hd_menu.c)

bool Host_IsFullscreen() {
  return (g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

void Host_ToggleFullscreen() {
  HandleCommand_Locked(kKeys_Fullscreen, true);
}

bool Host_GetDisplayPerf() {
  return g_display_perf;
}

void Host_SetDisplayPerf(bool on) {
  g_display_perf = on;
}

int Host_GetVolume() {
#if SYSTEM_VOLUME_MIXER_AVAILABLE
  return GetApplicationVolume();
#else
  return g_sdl_audio_mixer_volume * 100 / SDL_MIX_MAXVOLUME;
#endif
}

void Host_AdjustVolume(int step) {
  HandleVolumeAdjustment(step);
}

bool Host_IsOpenGL() {
  return g_config.output_method == kOutputMethod_OpenGL || g_config.output_method == kOutputMethod_OpenGL_ES;
}

bool OpenGLRenderer_HasShader(void);
bool Host_HasShader() {
  return Host_IsOpenGL() && OpenGLRenderer_HasShader();
}

uint32 Host_GetPpuRenderFlags() {
  return g_ppu_render_flags;
}

void Host_SetPpuRenderFlags(uint32 flags) {
  g_ppu_render_flags = flags;
}

void Host_ClearInputs() {
  g_input1_state = 0;
  g_gamepad_buttons = 0;
}

void Host_Quit() {
  g_running = false;
}
