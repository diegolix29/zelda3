// HUD moderno: cuori e magia a sinistra, tasti Y/A/B a destra come sul
// controller, rupie e chiavi in basso a sinistra, scritta dell'azione del
// tasto A (Parla, Leggi, Apri...). Ridisegna i pezzi dell'HUD originale.
#ifndef ZELDA3_HD_HUD_H_
#define ZELDA3_HD_HUD_H_

#include "types.h"

typedef struct Ppu Ppu;

// Prima del disegno della PPU: nasconde l'HUD originale quando serve
void HdHud_BeforeRender(Ppu *ppu);
// Disegna l'HUD moderno nel frame (alla risoluzione SNES moltiplicata per scale)
void HdHud_Draw(uint8 *pixels, int pitch, int width, int height, int scale);
// Chiamata dai personaggi che parlano quando Link e' nella posizione giusta
void HdHud_CanTalk(void);

// Testo moderno dei dialoghi (opzione ModernText). Dal gioco: messaggio
// nuovo o finito (la comparsa delle lettere riparte da capo)
void HdText_NewMessage(void);
// Prima che una scelta ricarichi il messaggio: le righe scritte restano sotto
void HdText_KeepLines(void);
// Comparsa delle lettere: un passo per fotogramma del gioco; Revealing =
// lettere ancora in arrivo (il gioco aspetta), RevealAll = mostrale tutte
void HdText_Tick(void);
bool HdText_Revealing(void);
void HdText_RevealAll(void);
// Prima e dopo il disegno della PPU: il riquadro originale non si disegna
void HdText_BeforeRender(Ppu *ppu);
void HdText_AfterRender(Ppu *ppu);
// true mentre un dialogo e' mostrato con il testo moderno
bool HdText_ModernActive(void);
// Disegna il dialogo nel frame (come HdHud_Draw)
void HdText_Draw(uint8 *pixels, int pitch, int width, int height, int scale);

#endif  // ZELDA3_HD_HUD_H_
