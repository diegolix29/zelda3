// Palazzo 6 come GBA (IcePalaceGba): nella versione GBA l'enigma dei blocchi
// del Palazzo del Ghiaccio e' diverso. Nella stanza 0x9E i blocchi non si
// spingono nella buca: ci sono pioli e binari diversi e la buca e' coperta da
// un grosso blocco da sollevare. Nella stanza 0xBE, sotto, i tre blocchi ci
// sono gia', con tre pioli da martello, due vasi in fondo e la porta a sud
// sempre aperta (niente interruttore sotto il vaso).
// Si cambiano solo i dati che il gioco legge al caricamento della stanza.
#include <string.h>
#include "hd_icegba.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "assets.h"

#define IceGbaOn() ((enhanced_features0 & kFeatures0_IcePalaceGba) != 0)

typedef struct IceObj { uint8 x, y, size; uint16 id; } IceObj;  // id >= 0xf80: tipo 3
typedef struct IceRoom {
  uint16 room;
  const IceObj *del, *add;  // oggetti dello strato 1 da togliere e da aggiungere
  int ndel, nadd;
  uint8 layout[1024];
  int door_offs;
  bool built;
} IceRoom;

static const IceObj kDel9E[] = {
  { 44, 39, 2, 0x93 }, { 44, 53, 3, 0x93 }, { 42, 43, 6, 0x5d }, { 42, 50, 6, 0x5d },
  { 42, 46, 2, 0x92 }, { 52, 46, 2, 0x92 }, { 44, 46, 0, 0x5e }, { 50, 48, 0, 0x5e },
  { 46, 46, 0, 0xa4 },
};
static const IceObj kAdd9E[] = {
  { 44, 40, 2, 0x93 }, { 42, 44, 4, 0x5d }, { 50, 44, 4, 0x88 }, { 46, 47, 0, 0xa4 },
  { 44, 47, 3, 0x92 }, { 46, 51, 2, 0xb8 }, { 52, 51, 3, 0xb9 },
  { 46, 47, 0, 0xfac },  // grosso blocco sulla buca
};
static const IceObj kDelBE[] = {
  { 46, 50, 0, 0xfaf },  // vaso con l'interruttore
};
static const IceObj kAddBE[] = {
  { 37, 51, 2, 0x5d }, { 43, 44, 4, 0x88 }, { 51, 51, 2, 0x5d }, { 51, 44, 4, 0x88 },
  { 45, 46, 0, 0xf96 }, { 47, 46, 0, 0xf96 }, { 49, 46, 0, 0xf96 },  // pioli da martello
  { 38, 56, 0, 0xfaf }, { 56, 56, 0, 0xfaf },  // vasi
};

static IceRoom g_ice_rooms[2] = {
  { 0x9e, kDel9E, kAdd9E, countof(kDel9E), countof(kAdd9E) },
  { 0xbe, kDelBE, kAddBE, countof(kDelBE), countof(kAddBE) },
};

// Segreti: (46,47) la buca sotto il grosso blocco; (38,56) e (56,56) magia
// grande nei vasi. Posizione = (x + y * 64) * 2
#define ICE_POS(x, y) (uint8)(((x) + (y) * 64) * 2), (uint8)(((x) + (y) * 64) * 2 >> 8)
static const uint8 kSecrets9E[] = { ICE_POS(46, 47), 0x80, 0xff, 0xff };
static const uint8 kSecretsBE[] = { ICE_POS(38, 56), 0x0d, ICE_POS(56, 56), 0x0d, 0xff, 0xff };

// Blocchi spostabili: i due della stanza 0x9E passano nella 0xBE e se ne
// aggiunge un terzo nel posto 99 (che il gioco non usa)
static const MovableBlockData kBlockFrom[2] = { { 0x9e, 0x1858 }, { 0x9e, 0x5764 } };
static const MovableBlockData kBlockTo[3] = { { 0xbe, 0x165a }, { 0xbe, 0x165e }, { 0xbe, 0x1662 } };

static IceRoom *FindRoom(int room) {
  if (!IceGbaOn())
    return NULL;
  for (int i = 0; i < 2; i++)
    if (g_ice_rooms[i].room == room)
      return &g_ice_rooms[i];
  return NULL;
}

static void EncodeObj(uint8 *d, const IceObj *o) {
  if (o->id >= 0xf80) {
    int n = o->id - 0xf80;
    d[0] = o->x << 2 | (n & 3);
    d[1] = o->y << 2 | (n >> 2 & 3);
    d[2] = 0xf8 | n >> 4;
  } else {
    d[0] = o->x << 2 | o->size >> 2;
    d[1] = o->y << 2 | (o->size & 3);
    d[2] = (uint8)o->id;
  }
}

// Lunghezza di uno strato (con la fine FFFF), porte comprese
static int LayerLen(const uint8 *p) {
  int i = 0;
  for (;;) {
    uint16 d = WORD(p[i]);
    if (d == 0xffff)
      return i + 2;
    if (d == 0xfff0)
      break;
    i += 3;
  }
  for (i += 2; WORD(p[i]) != 0xffff; i += 2) {}
  return i + 2;
}

static void BuildRoom(IceRoom *r) {
  const uint8 *src = kDungeonRoom + kDungeonRoomOffs[r->room];
  uint8 *dst = r->layout;
  int si = 2, di = 2;
  memcpy(dst, src, 2);  // pavimenti e disposizione
  for (; WORD(src[si]) != 0xffff; si += 3) {
    uint8 enc[3];
    bool skip = false;
    for (int j = 0; j < r->ndel && !skip; j++) {
      EncodeObj(enc, &r->del[j]);
      skip = !memcmp(enc, src + si, 3);
    }
    if (!skip)
      memcpy(dst + di, src + si, 3), di += 3;
  }
  for (int j = 0; j < r->nadd; j++)
    EncodeObj(dst + di, &r->add[j]), di += 3;
  int delta = di - si;
  int rest = LayerLen(src + si);
  rest += LayerLen(src + si + rest);
  rest += LayerLen(src + si + rest);
  assert(di + rest <= (int)sizeof(r->layout));
  memcpy(dst + di, src + si, rest);
  r->door_offs = kDungeonRoomDoorOffs[r->room] - kDungeonRoomOffs[r->room] + delta;
  // 0xBE: la porta a sud (la prima) non si chiude piu' con l'interruttore
  if (r->room == 0xbe && dst[r->door_offs] == 0x81)
    dst[r->door_offs + 1] = 0;
  r->built = true;
}

const uint8 *HdIce_RoomLayout(int room, const uint8 *p) {
  IceRoom *r = FindRoom(room);
  if (!r)
    return p;
  if (!r->built)
    BuildRoom(r);
  return r->layout;
}

const uint16 *HdIce_DoorInfo(int room, const uint16 *p) {
  IceRoom *r = FindRoom(room);
  if (!r)
    return p;
  if (!r->built)
    BuildRoom(r);
  return (const uint16 *)(r->layout + r->door_offs);
}

const uint8 *HdIce_Header(int room, const uint8 *p) {
  static uint8 hdr[14];
  if (room != 0xbe || !IceGbaOn())
    return p;
  memcpy(hdr, p, 14);
  hdr[5] = 0;  // niente "l'interruttore tiene aperta la porta"
  return hdr;
}

const uint8 *HdIce_Sprites(int room, const uint8 *p) {
  static uint8 spr[64];
  if (room != 0xbe || !IceGbaOn())
    return p;
  int n = 1;
  while (p[n] != 0xff && n + 4 < (int)sizeof(spr))
    n += 3;
  memcpy(spr, p, n + 1);
  // lo Stalfos Cavaliere una casella piu' in basso
  for (int i = 1; i < n; i += 3)
    if (spr[i] == 0x16 && spr[i + 1] == 0x18 && spr[i + 2] == 0x91)
      spr[i] = 0x17;
  return spr;
}

const uint8 *HdIce_Secrets(int room, const uint8 *p) {
  if (!IceGbaOn())
    return p;
  return room == 0x9e ? kSecrets9E : room == 0xbe ? kSecretsBE : p;
}

void HdIce_ApplyMovableBlocks(void) {
  bool on = IceGbaOn();
  int n = kMovableBlockDataInit_SIZE / 4;
  const MovableBlockData *init = (const MovableBlockData *)kMovableBlockDataInit;
  for (int i = 0; i < n && i < 99; i++) {
    for (int j = 0; j < 2; j++) {
      if (init[i].room == kBlockFrom[j].room && init[i].tilemap == kBlockFrom[j].tilemap)
        movable_block_datas[i] = on ? kBlockTo[j] : kBlockFrom[j];
    }
  }
  // il posto 99 di solito contiene una copia di scarto dei dati delle torce
  if (on)
    movable_block_datas[99] = kBlockTo[2];
  else
    memcpy(&movable_block_datas[99], kTorchDataInit, 4);
}

// 100 se il posto 99 contiene il terzo blocco (anche se poi e' caduto in una
// buca e ha cambiato stanza), 99 se contiene ancora lo scarto delle torce
int HdIce_NumMovableBlocks(void) {
  return IceGbaOn() && memcmp(&movable_block_datas[99], kTorchDataInit, 4) ? 100 : 99;
}
