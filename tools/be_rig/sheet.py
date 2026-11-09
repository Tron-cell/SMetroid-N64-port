import sys, glob
from PIL import Image
fs = sorted(glob.glob('snaps/*.ppm'))[: int(sys.argv[2]) if len(sys.argv) > 2 else 40]
cols = 8; W, H = 160, 120
sheet = Image.new('RGB', (cols * W, ((len(fs) + cols - 1) // cols) * (H + 12)), 'white')
from PIL import ImageDraw
d = ImageDraw.Draw(sheet)
for i, f in enumerate(fs):
    im = Image.open(f).resize((W, H))
    x, y = (i % cols) * W, (i // cols) * (H + 12)
    sheet.paste(im, (x, y + 12)); d.text((x + 2, y), f[-10:-4], fill='black')
sheet.save(sys.argv[1])
