# A contact sheet of a burst of captures (captura.exe ... burst): every frame small, and the bar of each one at full
# size, to look for what flickers or breaks while something moves.
#   uv run --no-project --with pillow python tools/hoja.py <folder> <prefix> [top px]
import sys
from pathlib import Path
from PIL import Image, ImageDraw

folder, prefix = Path(sys.argv[1]), sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 130
frames = sorted(folder.glob(prefix + '-*.png'))
imgs = [Image.open(f).convert('RGB') for f in frames]
w, h = imgs[0].size
cols, scale = 4, 0.25
tw, th = int(w * scale), int(h * scale)
rows = (len(imgs) + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, rows * th), 'white')
for i, im in enumerate(imgs):
    sheet.paste(im.resize((tw, th)), ((i % cols) * tw, (i // cols) * th))
    ImageDraw.Draw(sheet).text(((i % cols) * tw + 4, (i // cols) * th + 4), str(i), fill='red')
sheet.save(folder / (prefix + '-hoja.png'))
strips = Image.new('RGB', (w, top * len(imgs)), 'white')
for i, im in enumerate(imgs):
    strips.paste(im.crop((0, 0, w, top)), (0, i * top))
strips.save(folder / (prefix + '-barras.png'))
print(folder / (prefix + '-hoja.png'))
