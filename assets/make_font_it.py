# Crea font_it.png partendo da font.png (inglese) e aggiungendo le vocali
# accentate italiane nei posti 99-104 dell'alfabeto (vedi LangIT in
# text_compression.py).
from PIL import Image

# '#' = contorno, '+' = riempimento, '.' = sfondo, ' ' = lascia invariato
GRAVE = ['.#+#..', '.##+#.', '..###.']
ACUTE = ['..#+#.', '.#+##.', '.###..']
GRAVE_I = ['#+#.', '##+#', '.###', '....']

# (indice nuovo, indice lettera base, accento, larghezza)
NEW_GLYPHS = [
  (99,  26, GRAVE, None),    # à  (a = 26)
  (100, 30, GRAVE, None),    # è  (e = 30)
  (101, 30, ACUTE, None),    # é
  (102, 34, GRAVE_I, 4),     # ì  (i = 34)
  (103, 40, GRAVE, None),    # ò  (o = 40)
  (104, 46, GRAVE, None),    # ù  (u = 46)
]

PIX = {'#': 97, '+': 98, '.': 96}

def origin(c):
  x, y = c % 16, (c >> 4) * 2
  return x * 9, y * 8 + (y >> 1)

def main():
  im = Image.open('font.png')
  d = im.load()
  for new, base, accent, width in NEW_GLYPHS:
    nx, ny = origin(new)
    bx, by = origin(base)
    # copia glifo (16 righe) e riga marcatore larghezza
    for yy in range(17):
      for xx in range(9):
        d[nx + xx, ny + yy] = d[bx + xx, by + yy]
    if base == 34:
      # cancella il puntino della i (righe 2-4 del glifo)
      for yy in range(1, 5):
        for xx in range(8):
          d[nx + xx, ny + yy] = 96
    for yy, row in enumerate(accent):
      for xx, ch in enumerate(row):
        if ch != ' ':
          d[nx + xx, ny + 1 + yy] = PIX[ch]
    if width:
      for xx in range(9):
        if d[nx + xx, ny] == 255:
          d[nx + xx, ny] = 0
      d[nx + width - 1, ny] = 255
  im.save('font_it.png')
  print('font_it.png creato')

if __name__ == '__main__':
  main()
