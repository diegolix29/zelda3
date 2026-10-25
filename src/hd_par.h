// Lavori in parallelo per gli effetti HD: un compito si divide in parti
// (strisce dell'immagine) che girano insieme su piu' core
#ifndef ZELDA3_HD_PAR_H_
#define ZELDA3_HD_PAR_H_

enum { kHdParMaxParts = 8 };

// fn(ctx, parte, inizio, fine) per i pezzi di [0, n); ritorna quando sono
// tutti finiti. "parte" e' il thread (0 .. kHdParMaxParts-1): una parte puo'
// fare piu' pezzi, uno dopo l'altro. Solo dal thread del gioco. Con un solo
// core gira tutto qui
typedef void HdParFunc(void *ctx, int part, int begin, int end);
// pezzi piccoli presi uno alla volta: va bene quando il lavoro non e' uniforme
void HdPar_For(HdParFunc *fn, void *ctx, int n);
// un pezzo per parte: per i lavori che costano per pezzo (margini)
void HdPar_ForParts(HdParFunc *fn, void *ctx, int n);

#endif  // ZELDA3_HD_PAR_H_
