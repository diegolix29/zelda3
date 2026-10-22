// PASS 3/3 - Composizione finale, alla risoluzione dello schermo.
//
// Cosa si vede in Four Swords Adventures (studiato su un video del primo
// livello): la pixel art resta nitida, con i suoi contorni; sopra c'e' una
// trama di pennellate diagonali "sporche" (fsa_brush.png, piastrella continua
// inclinata), ferma rispetto al mondo; alberi e dirupi
// proiettano ombre morbide in basso a destra; all'aperto passano ombre di
// nuvole; i colori sono saturi; le zone molto chiare hanno un leggero alone.
// Personaggi e HUD non ricevono la trama.

#pragma parameter FSA_PIXEL_SHARP "Nitidezza pixel"              2.50 1.00 8.00 0.10
#pragma parameter FSA_HATCH       "Forza della trama"            1.00 0.00 2.00 0.05
#pragma parameter FSA_BRUSH_SCALE "Dimensione pennellate"        0.25 0.05 1.00 0.01
#pragma parameter FSA_BRUSH_SKIP  "Macchie saltate (0-1)"        0.70 0.00 1.00 0.02
#pragma parameter FSA_SHADOW      "Ombre morbide"                0.45 0.00 1.00 0.05
#pragma parameter FSA_CLOUDS      "Ombre di nuvole (esterni)"    0.30 0.00 0.60 0.01
#pragma parameter FSA_CLOUD_AMOUNT "Quante nuvole (0-1)"         1.00 0.00 1.00 0.05
#pragma parameter FSA_CLOUD_WIND  "Velocita' nuvole (px/s)"     6.00 0.00 30.0 0.50
#pragma parameter FSA_BLOOM       "Alone sulle zone chiare"      0.30 0.00 1.00 0.02
#pragma parameter FSA_SATURATION  "Saturazione"                  0.96 0.50 2.00 0.02
#pragma parameter FSA_CONTRAST    "Contrasto a S"                0.12 0.00 0.80 0.02
#pragma parameter FSA_KEEP_SPRITES "Sprite senza trama"          1.00 0.00 1.00 0.05
#pragma parameter FSA_KEEP_HUD    "HUD senza effetto"            1.00 0.00 1.00 0.05
#pragma parameter FSA_SKIP_MENUS  "Spegni l'effetto nei menu"    1.00 0.00 1.00 1.00

#include "fsa_common.inc"

uniform sampler2D OrigTexture;
uniform sampler2D FSABrush;
uniform COMPAT_PRECISION vec2 OrigTextureSize;
uniform COMPAT_PRECISION vec2 ScrollOffset;
// Modulo di gioco + 1 (0 = uniform non valorizzato, cioe' eseguibile vecchio).
uniform COMPAT_PRECISION float GameModule;
// 1 = all'aperto con il sole (niente pioggia, nebbia o nuvole basse)
uniform COMPAT_PRECISION float Sunny;

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float FSA_PIXEL_SHARP;
uniform COMPAT_PRECISION float FSA_HATCH;
uniform COMPAT_PRECISION float FSA_BRUSH_SCALE;
uniform COMPAT_PRECISION float FSA_BRUSH_SKIP;
uniform COMPAT_PRECISION float FSA_SHADOW;
uniform COMPAT_PRECISION float FSA_CLOUDS;
uniform COMPAT_PRECISION float FSA_CLOUD_AMOUNT;
uniform COMPAT_PRECISION float FSA_CLOUD_WIND;
uniform COMPAT_PRECISION float FSA_BLOOM;
uniform COMPAT_PRECISION float FSA_SATURATION;
uniform COMPAT_PRECISION float FSA_CONTRAST;
uniform COMPAT_PRECISION float FSA_KEEP_SPRITES;
uniform COMPAT_PRECISION float FSA_KEEP_HUD;
uniform COMPAT_PRECISION float FSA_SKIP_MENUS;
#else
#define FSA_PIXEL_SHARP  2.50
#define FSA_HATCH        1.00
#define FSA_BRUSH_SCALE  0.25
#define FSA_BRUSH_SKIP   0.70
#define FSA_SHADOW       0.45
#define FSA_CLOUDS       0.30
#define FSA_CLOUD_AMOUNT 1.00
#define FSA_CLOUD_WIND   6.00
#define FSA_BLOOM        0.30
#define FSA_SATURATION   0.96
#define FSA_CONTRAST     0.12
#define FSA_KEEP_SPRITES 1.00
#define FSA_KEEP_HUD     1.00
#define FSA_SKIP_MENUS   1.00
#endif

float hash21(vec2 p)
{
    p = fract(p * vec2(233.34, 851.73));
    p += dot(p, p + 23.45);
    return fract(p.x * p.y);
}

float vnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p)
{
    return vnoise(p) * 0.55
         + vnoise(p * 2.03 + vec2(11.3, -7.1)) * 0.30
         + vnoise(p * 4.11 + vec2(-5.7, 19.4)) * 0.15;
}

// --- Maschera dei layer ---------------------------------------------------
// Il PPU di zelda3 scrive nel canale alpha l'indice del layer di ogni pixel:
//   0 = BG1   1 = BG2   2 = BG3 (HUD)   3 = fondale   5 = sfondo
//   4 e 6 = OBJ (sprite, palette 0-3 e 4-7)   7 = acqua (fondale)
// Con un eseguibile non modificato l'alpha e' 0: niente viene protetto.
float layer_at(vec2 uv)
{
    return floor(COMPAT_TEXTURE(OrigTexture, uv).a * 255.0 + 0.5);
}

float sprite_of(float layer)
{
    return step(3.5, layer) * step(layer, 4.5) + step(5.5, layer) * step(layer, 6.5);
}

// HUD (2) e fondali (3) restano come li disegna il gioco. Il 3 lo scrive il
// gioco solo sul cielo dietro la Piramide e sulle nuvole del Monte della
// Morte (lo sfondo, 5, e' spesso anche il prato fra le caselle: si dipinge).
float hud_of(float layer)
{
    return step(1.5, layer) * step(layer, 3.5);
}

// 7 = acqua (marcata dal gioco): niente pennellate
float water_of(float layer)
{
    return step(6.5, layer) * step(layer, 7.5);
}

float water_mask(vec2 uv, vec2 sz)
{
    vec2 p = uv * sz - 0.5;
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    vec2 base = (i + 0.5) / sz;
    vec2 tx = vec2(1.0 / sz.x, 0.0);
    vec2 ty = vec2(0.0, 1.0 / sz.y);
    return mix(mix(water_of(layer_at(base)), water_of(layer_at(base + tx)), f.x),
               mix(water_of(layer_at(base + ty)), water_of(layer_at(base + tx + ty)), f.x), f.y);
}

// Il confronto va fatto sui singoli texel e interpolato dopo: interpolando
// l'indice di layer verrebbero fuori layer intermedi che non esistono.
vec2 protect_mask(vec2 uv, vec2 sz)
{
    vec2 p = uv * sz - 0.5;
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    vec2 base = (i + 0.5) / sz;
    vec2 tx = vec2(1.0 / sz.x, 0.0);
    vec2 ty = vec2(0.0, 1.0 / sz.y);
    float l00 = layer_at(base), l10 = layer_at(base + tx);
    float l01 = layer_at(base + ty), l11 = layer_at(base + tx + ty);
    vec2 m00 = vec2(sprite_of(l00), hud_of(l00));
    vec2 m10 = vec2(sprite_of(l10), hud_of(l10));
    vec2 m01 = vec2(sprite_of(l01), hud_of(l01));
    vec2 m11 = vec2(sprite_of(l11), hud_of(l11));
    return mix(mix(m00, m10, f.x), mix(m01, m11, f.x), f.y);  // x = sprite, y = HUD
}

// Pixel nitidi con il bordo appena ammorbidito (niente scalini tremolanti)
vec3 sample_sharp(vec2 uv, vec2 sz, float sharp)
{
    vec2 p = uv * sz - 0.5;
    vec2 i = floor(p);
    vec2 f = p - i;
    f = clamp((f - 0.5) * sharp + 0.5, 0.0, 1.0);
    f = f * f * (3.0 - 2.0 * f);
    return COMPAT_TEXTURE(OrigTexture, (i + 0.5 + f) / sz).rgb;
}

// Menu (intro, selezione file, inventario, scelta del punto di partenza)
// e morte di Link (modulo 18, lo schermo rosso): escono intatti.
// GameModule arriva incrementato di 1.
float menu_amount()
{
    float known = step(0.5, GameModule);
    float m = GameModule;
    float menu = step(m, 6.5)
               + step(14.5, m) * step(m, 15.5)
               + step(18.5, m) * step(m, 19.5)
               + step(27.5, m) * step(m, 28.5);
    return known * clamp(menu, 0.0, 1.0) * FSA_SKIP_MENUS;
}

// All'aperto (modulo 9 = mondo esterno)
float outdoors()
{
    return step(9.5, GameModule) * step(GameModule, 10.5);
}

// --- Pennellate -------------------------------------------------------------
// fsa_brush.png e' una piastrella continua (ricavata dalla trama disegnata
// dall'utente da Four Swords: la banda diagonale raddrizzata, con i bordi
// raccordati): ripetuta lungo la stessa inclinazione, una ripetizione prosegue
// nella successiva. I vuoti sono quelli della trama stessa.
#define BRUSH_PX vec2(225.0, 133.0)   // dimensioni di fsa_brush.png
#define BRUSH_ANGLE 50.44             // inclinazione della banda, in gradi

float brush_marks(vec2 world)
{
    float ang = radians(BRUSH_ANGLE);
    vec2 d = vec2(cos(ang), sin(ang));
    vec2 n = vec2(-d.y, d.x);
    vec2 bw = vec2(dot(world, d), dot(world, n));   // pixel SNES lungo/attraverso la banda
    vec2 band = bw / FSA_BRUSH_SCALE;
    float m = COMPAT_TEXTURE(FSABrush, band / BRUSH_PX).r;
    // Ogni tanto una macchia salta: buchi irregolari grandi piu' o meno come
    // una macchia, allungati come le pennellate, fermi rispetto al mondo.
    float hole = vnoise(bw * vec2(1.0 / 15.0, 1.0 / 7.0) + vec2(3.7, 11.2));
    float skip = smoothstep(FSA_BRUSH_SKIP - 0.05, FSA_BRUSH_SKIP + 0.05, 1.0 - hole);
    return m * (1.0 - skip);
}

// --- Ombre di nuvole ----------------------------------------------------------
// Sagome da cartone animato, come in Four Swords: ogni nuvola e' un gruppo di
// cerchi uniti, larga circa 250-310 pixel SNES, di tre tipi scelti a caso
// (cumulo lungo, cumulo alto a cupola, nuvola bassa a due strati) e a volte
// girata da destra a sinistra. Ogni nuvola
// sta tutta dentro la sua casella, con posizione e misura a caso, e le righe
// di caselle sono sfalsate a caso: due nuvole non si toccano mai.
#define CLOUD_CELL vec2(345.0, 250.0)
#define CLOUD_HALF vec2(112.0, 82.0)   // mezzo ingombro di una nuvola in scala 1
#define CLOUD_GAP 18.0

vec2 hash22(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}

// Distanza dal bordo di un lobo (cerchio di centro c e raggio r), con un po'
// di variazione a caso di posizione e misura
float cloud_lobe(vec2 p, vec2 seed, float k, vec2 c, float r, vec2 jitter)
{
    vec2 h = hash22(seed + vec2(k * 3.17, k * 1.31));
    c += (h - 0.5) * jitter;
    r *= mix(0.88, 1.12, fract(h.x * 5.3));
    return length(p - c) - r;
}

float cloud_shape(vec2 p, vec2 seed)
{
    // p: posizione rispetto al centro della nuvola, in pixel SNES (scala 1)
    vec2 kind = hash22(seed + vec2(17.9, 63.1));
    if (kind.y < 0.5)
        p.x = -p.x;
    float d = 1e9;
    if (kind.x < 0.34) {
        // cumulo lungo: una fila di lobi, piu' grandi in mezzo
        for (int k = 0; k < 7; k++) {
            float fk = float(k) / 6.0;
            float r = mix(36.0, 52.0, 1.0 - abs(fk - 0.5) * 2.0);
            d = min(d, cloud_lobe(p, seed, float(k), vec2(mix(-58.0, 58.0, fk), 0.0), r, vec2(22.0, 44.0)));
        }
    } else if (kind.x < 0.67) {
        // cumulo alto a cupola: un lobo grande in mezzo, due sopra e i lati
        // che scendono, con la base quasi piatta
        d = min(d, cloud_lobe(p, seed, 0.0, vec2(0.0, -8.0), 58.0, vec2(14.0, 10.0)));
        d = min(d, cloud_lobe(p, seed, 1.0, vec2(-30.0, -40.0), 36.0, vec2(14.0, 10.0)));
        d = min(d, cloud_lobe(p, seed, 2.0, vec2(26.0, -42.0), 34.0, vec2(14.0, 10.0)));
        d = min(d, cloud_lobe(p, seed, 3.0, vec2(-56.0, 16.0), 40.0, vec2(14.0, 10.0)));
        d = min(d, cloud_lobe(p, seed, 4.0, vec2(56.0, 14.0), 40.0, vec2(14.0, 10.0)));
        d = min(d, cloud_lobe(p, seed, 5.0, vec2(-88.0, 32.0), 26.0, vec2(12.0, 8.0)));
        d = min(d, cloud_lobe(p, seed, 6.0, vec2(88.0, 30.0), 28.0, vec2(12.0, 8.0)));
        d = max(d, p.y - 52.0);
    } else {
        // nuvola bassa a due strati: sotto tanti lobi piccoli, sopra pochi
        // lobi spostati da una parte
        for (int k = 0; k < 6; k++) {
            float fk = float(k) / 5.0;
            d = min(d, cloud_lobe(p, seed, float(k), vec2(mix(-84.0, 84.0, fk), 22.0), 30.0, vec2(18.0, 12.0)));
        }
        for (int k = 0; k < 3; k++) {
            float fk = float(k) / 2.0;
            d = min(d, cloud_lobe(p, seed, float(k) + 7.0, vec2(mix(-62.0, 18.0, fk), -12.0), 36.0, vec2(18.0, 14.0)));
        }
    }
    return 1.0 - smoothstep(-2.5, 2.5, d);
}

float cloud_in_cell(vec2 w, vec2 cell)
{
    vec2 h = hash22(cell + vec2(41.7, 7.3));
    if (h.x > FSA_CLOUD_AMOUNT)
        return 0.0;
    float sc = mix(1.2, 1.4, h.y);
    vec2 o = hash22(cell);
    float shift = hash22(vec2(cell.y + 3.3, 9.1)).x * CLOUD_CELL.x;
    vec2 room = max(CLOUD_CELL - 2.0 * CLOUD_HALF * sc - CLOUD_GAP, vec2(0.0));
    vec2 center = cell * CLOUD_CELL + vec2(shift, 0.0) + CLOUD_GAP * 0.5 + CLOUD_HALF * sc + o * room;
    return cloud_shape((w - center) / sc, cell);
}

float cloud_shadow(vec2 world, float t)
{
    vec2 w = world - FSA_CLOUD_WIND * t * vec2(0.92, 0.38);
    float row = floor(w.y / CLOUD_CELL.y);
    float acc = 0.0;
    for (int j = -1; j <= 1; j++) {
        float r = row + float(j);
        float shift = hash22(vec2(r + 3.3, 9.1)).x * CLOUD_CELL.x;
        float col = floor((w.x - shift) / CLOUD_CELL.x);
        for (int i = -1; i <= 1; i++)
            acc = max(acc, cloud_in_cell(w, vec2(col + float(i), r)));
    }
    return acc;
}

void main()
{
    vec2 uv = vTexCoord;
    vec2 src = max(OrigTextureSize, vec2(1.0));
    vec2 px = uv * src;                 // pixel SNES sullo schermo
    vec2 world = px + ScrollOffset;     // pixel SNES nel mondo

    vec2 prot = protect_mask(uv, src);
    float sprite = prot.x * FSA_KEEP_SPRITES;
    float hud = prot.y * FSA_KEEP_HUD;
    float scene = 1.0 - clamp(sprite + hud, 0.0, 1.0);   // fondale
    float menu = menu_amount();

    vec3 orig = sample_sharp(uv, src, FSA_PIXEL_SHARP);
    vec3 color = orig;
    float luma = dot(color, vec3(0.299, 0.587, 0.114));

    // --- Ombre morbide ------------------------------------------------
    // La luce viene dall'alto a sinistra: l'ombra in un punto viene dalle zone
    // scure che stanno un po' piu' in alto a sinistra. Dove il pixel e' gia'
    // scuro di suo (la chioma stessa) l'ombra non si aggiunge.
    vec2 shadow_off = vec2(3.0, 4.0) / src;
    float occ = COMPAT_TEXTURE(Source, uv - shadow_off).r;
    float self_dark = COMPAT_TEXTURE(Source, uv).r;
    float shadow = clamp(occ * 1.7 - self_dark * 0.9, 0.0, 1.0);
    color *= 1.0 - FSA_SHADOW * 0.55 * shadow * (1.0 - hud);

    // --- Ombre di nuvole (solo all'aperto) -------------------------------
    float t = float(FrameCount) / 60.0;
    float cloud = Sunny > 0.5 ? cloud_shadow(world, t) : 0.0;
    color *= 1.0 - FSA_CLOUDS * cloud * (1.0 - hud);

    // --- Pennellate di Four Swords -------------------------------------------
    float marks = brush_marks(world) * (1.0 - water_mask(uv, src));
    // Piu' forti sui toni medi, quasi assenti sui neri e sui bianchi
    float tone = smoothstep(0.03, 0.16, luma) * (1.0 - smoothstep(0.80, 1.00, luma));
    color *= 1.0 - FSA_HATCH * 0.17 * marks * tone * scene;
    color *= 1.0 + 0.04 * scene;  // il fondale di Four Swords e' un filo piu' chiaro

    // --- Alone sulle zone chiare ------------------------------------------
    vec3 glow = COMPAT_TEXTURE(Source, uv).g * vec3(1.0, 0.99, 0.95);
    color += glow * FSA_BLOOM * 0.5 * scene;

    // --- Colore ---------------------------------------------------------
    color = clamp(color, 0.0, 1.0);
    float l = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3(l), color, FSA_SATURATION);
    color = clamp(color, 0.0, 1.0);
    color = mix(color, color * color * (3.0 - 2.0 * color), FSA_CONTRAST);

    // HUD e menu escono intatti
    color = mix(color, orig, max(hud, menu));
    FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}

#endif
