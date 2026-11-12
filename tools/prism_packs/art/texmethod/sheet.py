"""Labelled side-by-side sheets of equal-size crops.

  sheet.py OUT.png COLS label=path [label=path ...]   (crops shown 1:1; '-' leaves a cell empty)
"""
import sys

from PIL import Image, ImageDraw, ImageFont


def make(out, cols, items, scale=1.0):
    ims = []
    for lab, p in items:
        ims.append((lab, None if p == "-" else Image.open(p).convert("RGB")))
    w, h = next(im.size for _, im in ims if im is not None)
    w, h = int(w * scale), int(h * scale)
    rows = (len(ims) + cols - 1) // cols
    bar = 22
    sh = Image.new("RGB", (cols * w + (cols - 1) * 4, rows * (h + bar) + (rows - 1) * 4), (24, 24, 24))
    d = ImageDraw.Draw(sh)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 14)
    except OSError:
        font = ImageFont.load_default()
    for i, (lab, im) in enumerate(ims):
        x, y = (i % cols) * (w + 4), (i // cols) * (h + bar + 4)
        d.text((x + 4, y + 3), lab, fill=(255, 230, 120), font=font)
        if im is not None:
            if scale != 1.0:
                im = im.resize((w, h), Image.LANCZOS)
            sh.paste(im, (x, y + bar))
    sh.save(out)


if __name__ == "__main__":
    out, cols = sys.argv[1], int(sys.argv[2])
    items = [a.split("=", 1) for a in sys.argv[3:]]
    make(out, cols, items)
