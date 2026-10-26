// Mondo continuo all'esterno (opzione ContinuousWorld): oltre il bordo
// dell'area si vedono le aree vicine e la telecamera segue Link fino al bordo.
#ifndef ZELDA3_HD_WORLD_H_
#define ZELDA3_HD_WORLD_H_

#include "types.h"

// Attivo in questo frame (all'esterno, nel Mondo della Luce o delle Tenebre)
bool HdWorld_Active(void);
// Una volta per frame di gioco: posizione della telecamera "libera", cioe'
// quella che il gioco avrebbe se non si fermasse ai bordi dell'area
void HdWorld_UpdateCamera(int *cam_x, int *cam_y);
// Ai quattro bordi della mappa del mondo la vista si ferma come nel gioco:
// riporta dentro la posizione (x, y) della telecamera se mostrerebbe il vuoto
void HdWorld_ClampView(int *cam_x, int *cam_y);
// Dopo il disegno della PPU: il fondale fuori dall'area corrente (e durante
// il passaggio fra aree, tutto il fondale) viene preso dalle immagini delle aree
void HdWorld_Composite(uint8 *pixels, int pitch, int width, int height);
// Vero se il punto del mondo e' dentro l'area corrente
bool HdWorld_InsideCurrentArea(int wx, int wy);
// Vero se in questo frame tutto il fondale viene dalle immagini delle aree
// (passaggio fra aree): gli strati del gioco li' sono solo da coprire
bool HdWorld_BackgroundFromImages(void);

// Mondo persistente (cespugli, sassi, buche, nemici sconfitti per area)
void HdWorld_ResetStates(void);
// Dopo il caricamento di un salvataggio: chi seguiva Link resta sconfitto nella sua area
void HdWorld_AfterLoad(void);
// Dopo aver decompresso l'area corrente: riscrive le caselle ricordate
void HdWorld_EnterAreaTiles(bool memorize);
// Dopo che il gioco ha azzerato la sua lista: la riempie di nuovo
void HdWorld_RememberTiles(void);
// Dopo aver letto i nemici dell'area: quelli sconfitti non ricompaiono
void HdWorld_RestoreKilled(void);
// Ogni casella cambiata dell'area corrente (anche quelle che il gioco non
// memorizza o per cui la sua lista non ha posto)
void HdWorld_RememberExtraTile(uint16 pos, uint16 value);
// La freccia k, ferma in un muro, resta li' finche' il mondo non torna com'era.
// Vero se da ora la disegna hd_world.c (l'oggetto in volo si puo' togliere)
bool HdWorld_KeepArrow(int k);
bool HdWorld_KeepEnemyArrow(int k, const uint8 *chr, const uint8 *flags, const int8 *dx, const int8 *dy);

// Aree vicine dal vivo: una volta per frame di gioco, dopo la telecamera, i
// nemici delle aree vicine visibili fanno un passo
void HdWorld_TickNeighbors(void);
// Vero mentre si muovono i nemici di un'area vicina (nella sua copia dello
// stato del gioco): niente effetti su quello che vale per tutto il gioco
extern bool g_hd_neighbor_tick;

// Mondo unico: la casella (map16) nel punto del mondo (in pixel) fuori
// dall'area corrente, presa dalla mappa di tutto il mondo; -1 nell'area
// corrente o se non serve (dentro case e labirinti, mondo unico spento)
int HdWorld_WorldTile(int px, int py);
// Vero se il terreno fuori dall'area corrente viene dalla mappa del mondo
bool HdWorld_SeamlessOn(void);
// Vero se il punto e' fuori dall'area corrente (solo col mondo unico)
bool HdWorld_OutsideArea(int px, int py);
// Al cambio d'area, prima che il gioco carichi la nuova
void HdWorld_LeaveArea(void);
// Angolo della vista nel mondo all'ultimo frame (zelda_rtl.c), se valido
extern int g_view_wx, g_view_wy;
extern bool g_view_valid;

// Nemici che seguono Link: al passaggio in un'altra area, prima che il gioco
// legga i nemici della nuova, chi lo insegue resta con lui
void HdWorld_BeforeNewSprites(void);
// Mondo vivo, nemici fuori dalla vista (Sprite_PrepOamCoordOrDoubleRet): un
// nemico dell'area resta fermo invece di sparire; chi seguiva Link torna a casa
bool HdWorld_KeepOffscreen(int k);
void HdWorld_SpriteBegin(int k);
void HdWorld_SpriteEnd(int k, bool offscreen);
bool HdWorld_LeashWall(int k, uint16 x, uint16 y);
bool HdWorld_HoldReload(uint16 blk);
bool HdWorld_ProbeMoreSlots(int k);
int HdWorld_FreeSlot(int k, int last);
bool HdWorld_KeepDropNormal(int k);
void HdWorld_SpriteSpawned(int k);
void HdWorld_CarriedLost(int k);
// Vero se il bordo dell'area fa da muro per questo nemico (Sprite_CheckTileProperty)
bool HdWorld_ConfinedToArea(int k);
// Ogni frame di ogni sprite (Sprite_ExecuteSingle) e ogni volta che guarda il
// terreno: chi non lo guarda da un po' vola e passa da un'area all'altra
void HdWorld_SpriteFrame(int k);
void HdWorld_SpriteTouchedTiles(int k);
// Mondo unico: area (con il bit del mondo) che contiene il punto, -1 se non
// si sa; e cambio di una casella di un'area diversa da quella corrente (resta
// nel mondo persistente come le altre caselle cambiate)
int HdWorld_AreaAt(int px, int py);
bool HdWorld_SetWorldTile(int px, int py, uint16 value);
// Mondo unico: i sassi che cadono dall'alto (Montagna della Morte) restano
// nelle aree in cui cadono: vero se il sasso k e' entrato in un'altra area
// (nel gioco usciva dallo schermo proprio al bordo dell'area)
bool HdWorld_BoulderGone(int k);
// Vero se il nemico k sta seguendo Link fuori dalla sua area: il suo posto
// non va dato a un nemico nuovo
bool HdWorld_Carried(int k);
// Prima del disegno della PPU: i nemici arrivati da un'area con grafica
// diversa si disegnano a parte, con la grafica della loro area
void HdWorld_BeforeRender(int width, int height);

// Specchio e caselle di teletrasporto: il fondale ondeggia riga per riga e si
// schiarisce fino al bianco. La PPU disegna senza onda e con i colori veri
// (BeforeRender, Line dopo l'HDMA di ogni riga, AfterRender); Mask dopo la
// composizione, Post dopo tutte le aggiunte HD: onda e bianco su tutto
void HdWorld_WarpBeforeRender(void);
void HdWorld_WarpLine(int line);
void HdWorld_WarpAfterRender(void);
void HdWorld_WarpMask(uint8 *pixels, int pitch, int width, int height);
void HdWorld_WarpPost(uint8 *pixels, int pitch, int width, int height);
// Per lo shader: quanto e' bianco lo schermo (0-1) e l'onda di ogni riga (240)
float HdWorld_WarpWhite(void);
const float *HdWorld_WarpRowDx(void);
// Colore (BGR a 15 bit) dello strato BG1 sommato a tutto (nebbia, pioggia)
// nel pixel sx, sy dello schermo; -1 dove e' trasparente
int HdWorld_OverlayColorAt(int sx, int sy);

// Vista 3D (hd_3d.c): origine della vista nel mondo e terreno di un punto
void HdWorld_ViewOrigin(int width, int *x, int *y);
bool HdWorld_SampleGround(int wx, int wy, uint32 *out);
// Vista 3D: strato del tempo atmosferico (pioggia, nebbia) tenuto piatto
bool HdWorld_OverlayPresent(void);
void HdWorld_SuppressOverlay(bool on);
void HdWorld_OverlayFlat(uint8 *pixels, int pitch, int width, int height, bool uniform);

#endif  // ZELDA3_HD_WORLD_H_
