# Controlla che ogni riga dei dialoghi stia nel riquadro del gioco.
# Uso: python check_dialogue_width.py dialogue_it.txt [limite_px]
import re, sys
from PIL import Image

LINE_BREAKS = ('[1]', '[2]', '[3]', '[Scroll]')
TOKEN = re.compile(r'\[[^\]]*\]|.', re.S)

def load_widths(png='font.png'):
  im = Image.open(png); d = im.load()
  widths = {}
  for c in range(128):
    x, y = c % 16, (c >> 4) * 2
    bx, by = x * 9, y * 8 + (y >> 1)
    for i in range(8):
      if d[bx + i, by] == 255:
        widths[c] = i + 1
        break
  return widths

def make_measure(lang):
  import text_compression as tc
  info = tc.kLanguages[lang]
  png = 'font.png' if lang == 'us' else 'font_%s.png' % lang
  W = load_widths(png)
  a2i = {e: i for i, e in enumerate(info.alphabet)}
  def width(tok):
    if tok == '[Name]':
      return 6 * 8  # caso peggiore: nome di 6 lettere larghe
    if tok.startswith('[Number'):
      return 8
    if tok in a2i:
      return W.get(a2i[tok], 8)
    if tok.startswith('['):
      return 0
    raise KeyError(tok)
  return width

def lines_of(text):
  cur, out = [], []
  for tok in TOKEN.findall(text):
    if tok in LINE_BREAKS:
      out.append(cur); cur = []
    elif tok.startswith('[Waitkey') or tok.startswith('[Choose') or tok.startswith('[Selchg'):
      continue
    else:
      cur.append(tok)
  out.append(cur)
  return out

def main():
  fname = sys.argv[1]
  base = fname.replace('\\', '/').rsplit('/', 1)[-1]
  lang = 'us' if base == 'dialogue.txt' else base[len('dialogue_'):-4].split('_part')[0]
  width = make_measure(lang)
  limit = int(sys.argv[2]) if len(sys.argv) > 2 else None
  worst, bad = 0, 0
  for raw in open(fname, encoding='utf8').read().splitlines():
    num, text = raw.split(': ', 1)
    for toks in lines_of(text):
      try:
        w = sum(width(t) for t in toks)
      except KeyError as e:
        print(f'{num}: carattere non supportato {e}'); bad += 1; continue
      worst = max(worst, w)
      if limit and w > limit:
        print(f'{num}: {w}px  "{"".join(toks)}"'); bad += 1
  print(f'Riga piu larga: {worst}px, righe fuori limite: {bad}')

if __name__ == '__main__':
  main()
