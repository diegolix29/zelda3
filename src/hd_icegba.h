// Palazzo 6 come GBA (IcePalaceGba): l'enigma delle stanze 0x9E e 0xBE del
// Palazzo del Ghiaccio come nella versione GBA
#ifndef ZELDA3_HD_ICEGBA_H_
#define ZELDA3_HD_ICEGBA_H_

#include "types.h"

// Ognuna riceve i dati originali della stanza e ritorna quelli da usare
const uint8 *HdIce_RoomLayout(int room, const uint8 *p);
const uint16 *HdIce_DoorInfo(int room, const uint16 *p);
const uint8 *HdIce_Header(int room, const uint8 *p);
const uint8 *HdIce_Sprites(int room, const uint8 *p);
const uint8 *HdIce_Secrets(int room, const uint8 *p);
// Blocchi spostabili: da chiamare dopo aver copiato la tabella iniziale (e
// quando cambia l'opzione); quanti posti della tabella guardare
void HdIce_ApplyMovableBlocks(void);
int HdIce_NumMovableBlocks(void);

#endif  // ZELDA3_HD_ICEGBA_H_
