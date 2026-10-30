// Menu opzioni in gioco, salvataggi rapidi e altre migliorie "HD".
#ifndef ZELDA3_HD_MENU_H_
#define ZELDA3_HD_MENU_H_

#include "types.h"


// Funzioni fornite da main.c
bool Host_IsFullscreen();
void Host_ToggleFullscreen();
bool Host_GetDisplayPerf();
void Host_SetDisplayPerf(bool on);
int Host_GetVolume();
// Vibrazione dei controller aperti: forza dei due motori (0-1, pesante e
// leggero) e durata; si scala con l'opzione Rumble (spenta = niente). Una
// chiamata nuova prende il posto di quella in corso
void Host_Rumble(float low, float high, int ms);
void Host_AdjustVolume(int step);
bool Host_IsOpenGL();
// Disegno con uno shader GLSL (per esempio Four Swords)
bool Host_HasShader();
uint32 Host_GetPpuRenderFlags();
void Host_SetPpuRenderFlags(uint32 flags);
void Host_ClearInputs();
void Host_Quit();

void HdMenu_ApplyConfig();
bool HdMenu_IsOpen();
void HdMenu_Open();
void HdMenu_Close();
// Restituiscono true se l'evento e' stato usato dal menu
bool HdMenu_HandleKey(int code, int mod, bool pressed);
bool HdMenu_HandlePadButton(int button, bool pressed);
void HdMenu_HandleStick(uint8 dir_bits);
// Disegna il menu sopra l'immagine del gioco (pixel 0x00RRGGBB)
void HdMenu_Draw(uint8 *pixels, int pitch, int width, int height, int scale);

// Mostra un breve messaggio in basso sullo schermo
void HdMenu_ShowMessage(const char *msg);
// Suggerimenti dei tasti accesi e disegnabili (solo con le fasce del 16:9 o 16:10)
bool HdMenu_ButtonHintsOn(void);
// Link entra da una porta del mondo esterno: se e' la porta principale di un
// dungeon ne mostra il nome al centro
void HdMenu_ShowDungeonName(void);
// Scritta del dungeon visibile: i nemici restano fermi finche' Link non si muove
bool HdMenu_FreezeEnemies(void);
// Input del fotogramma con la scritta del dungeon: tutto fermo, il primo tasto
// toglie solo la scritta
int HdMenu_FilterInput(int inputs);

// Da chiamare dopo ogni frame di gioco (con l'audio bloccato)
void HdMenu_AfterFrame(void);

// Testo nitido: rettangolo (in pixel del frame) del riquadro dei dialoghi da
// ridisegnare sopra lo shader, e ingrandimento 4x per pixel art.
bool HdText_GetRect(int frame_w, int frame_h, int *x, int *y, int *w, int *h);
void HdText_Scale4x(const uint8 *src, int src_pitch, int x, int y, int w, int h, uint32 *dst);

#endif  // ZELDA3_HD_MENU_H_
