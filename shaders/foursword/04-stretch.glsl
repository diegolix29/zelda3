// PASS 4/4 - Allarga allo schermo l'immagine gia' dipinta.
//
// La fase 3 dipinge a un ingrandimento intero dell'immagine del gioco
// (scale_type = integer: 4x su uno schermo di 1080 righe). Se dipingesse
// direttamente a 4,5x, i pixel del gioco cadrebbero sui pixel dello schermo
// in modo diverso a ogni passo di scorrimento dispari e pennellate e ombre
// sfarfallerebbero camminando. Qui si allarga soltanto, con filtro lineare.

#include "fsa_common.inc"

void main()
{
    FragColor = vec4(COMPAT_TEXTURE(Source, vTexCoord).rgb, 1.0);
}
#endif
