"""Remove the red keypad hints from a simulator skin bitmap, for the
touchscreen skins in uisimulator/android/bitmaps.

Usage: clean-skin.py in.bmp out.bmp cx cy inner_radius
e.g.   clean-skin.py ../bitmaps/UI-ipodvideo.bmp bitmaps/UI-ipodvideo.bmp 174 439 45
       (then reduce out.bmp to 8 bits, like the originals)
Red pixels (plus a small margin for anti-aliasing) are refilled from pixels
at the same radius around the wheel centre (cx, cy), a few degrees away,
skipping other masked pixels and white labels. The centre button
(inner_radius) is filled with its own median colour.
"""
import sys, math
import numpy as np
from PIL import Image
from scipy import ndimage

src, dst = sys.argv[1], sys.argv[2]
cx, cy, inner = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
img = np.asarray(Image.open(src).convert("RGB")).astype(np.int32)
h, w, _ = img.shape
r, g, b = img[..., 0], img[..., 1], img[..., 2]

red = (r - np.maximum(g, b)) > 30
mask = ndimage.binary_dilation(red, iterations=2)
yy, xx = np.mgrid[0:h, 0:w]
on_button = np.hypot(xx - cx, yy - cy) < inner
bright = img.sum(axis=2) > 3 * 225          # white labels: never copy these
usable = ~mask & ~bright

out = img.copy()
# the centre button is flat: fill it with its own colour
button_colour = np.median(img[on_button & ~mask], axis=0)
ys, xs = np.nonzero(mask)
for y, x in zip(ys, xs):
    if on_button[y, x]:
        out[y, x] = button_colour
        continue
    dx, dy = x - cx, y - cy
    rad = math.hypot(dx, dy)
    ang = math.atan2(dy, dx)
    samples = []
    # walk outwards in angle both ways, a few samples each side
    for sign in (1, -1):
        found = 0
        for step in range(1, 400):
            a = ang + sign * step / max(rad, 1.0)   # ~1px per step
            sx, sy = int(round(cx + rad * math.cos(a))), int(round(cy + rad * math.sin(a)))
            if 0 <= sx < w and 0 <= sy < h and usable[sy, sx]:
                samples.append(img[sy, sx])
                found += 1
                if found == 3:
                    break
    if samples:
        out[y, x] = np.mean(samples, axis=0)

Image.fromarray(out.astype(np.uint8)).save(dst)
print(f"replaced {len(ys)} pixels")
