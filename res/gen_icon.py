"""Generate res/app.ico for ProWindows.

Draws the app mark - a tiled window arrangement (one master pane on the left,
two stacked panes on the right) - on a plate cut the way the settings window
cuts its buttons: pure black with its top-right corner taken off at 45 degrees
and a light hairline round the edge. The master pane is brushed silver / metal;
the other two are white and grey, after the options screens of Resident Evil
Requiem. Rendered at several sizes and packed into a PNG-compressed .ico. Uses
only the standard library.

    python res/gen_icon.py
"""

import math
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 48, 64, 128, 256]
SS = 4  # supersampling factor for smooth edges

# theme.h: Bg, MetalEdge, Text, and a grey between them.
PLATE     = (0x00, 0x00, 0x00)  # the screen: pure black
EDGE      = (0x96, 0x96, 0x96)  # MetalEdge, light hairline
PANE_MAIN = (0x96, 0x96, 0x96)  # the master pane: brushed silver (MetalEdge)
PANE_ALT  = (0xE8, 0xE8, 0xE8)  # top right: white
PANE_DIM  = (0x82, 0x82, 0x82)  # bottom right: grey

LO, HI = 1.0, 31.0               # the plate, on a 32-unit grid
CUT = 8.0                        # the top-right corner taken off


def colour_at(px, py, unit):
    """The colour of the sample point, or None where the icon is clear.

    Everything is described on a 32 x 32 grid and scaled by `unit`.
    """
    x, y = px / unit, py / unit
    if not (LO <= x <= HI and LO <= y <= HI):
        return None
    # The cut: a line from (HI - CUT, LO) to (HI, LO + CUT).
    over = (x - (HI - CUT)) - (y - LO)
    if over > 0:
        return None

    # The hairline round the edge: one device pixel at every size (a grid
    # unit is SS / unit pixels wide), never thinner than 0.7 of a unit.
    edge = max(SS / unit, 0.7)
    d_cut = -over / math.sqrt(2.0)
    d = min(x - LO, HI - x, y - LO, HI - y, d_cut)
    if d < edge:
        return EDGE

    # The tile mark in the middle.
    lo, hi, gap = 8.0, 24.0, 1.7
    mid = lo + (hi - lo) * 0.55
    ymid = (lo + hi) / 2.0
    if lo <= x <= hi and lo <= y <= hi:
        if x <= mid - gap / 2.0:
            return PANE_MAIN
        if x >= mid + gap / 2.0:
            if y <= ymid - gap / 2.0:
                return PANE_ALT
            if y >= ymid + gap / 2.0:
                return PANE_DIM
    return PLATE


def render(size):
    """Return RGBA bytes for one square icon of the given size."""
    n = size * SS
    unit = n / 32.0
    acc = [[0, 0, 0, 0] for _ in range(size * size)]

    for sy in range(n):
        py = sy + 0.5
        oy = sy // SS
        for sx in range(n):
            color = colour_at(sx + 0.5, py, unit)
            if color is None:
                continue
            cell = acc[oy * size + (sx // SS)]
            cell[0] += color[0]
            cell[1] += color[1]
            cell[2] += color[2]
            cell[3] += 255

    total = SS * SS
    out = bytearray(size * size * 4)
    for i, (r, g, b, a) in enumerate(acc):
        alpha = a // total
        if alpha == 0:
            continue
        # un-premultiply: colours were only summed for covered samples
        covered = a // 255
        out[i * 4 + 0] = r // covered
        out[i * 4 + 1] = g // covered
        out[i * 4 + 2] = b // covered
        out[i * 4 + 3] = alpha
    return bytes(out)


def png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def make_png(size, rgba):
    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)                      # filter type: none
        raw += rgba[y * stride:(y + 1) * stride]
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" +
            png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            png_chunk(b"IEND", b""))


def main():
    images = []
    for size in SIZES:
        print("rendering %dx%d..." % (size, size))
        images.append((size, make_png(size, render(size))))

    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, blob in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for _, blob in images:
        out += blob

    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "app.ico"), "wb") as fh:
        fh.write(out)
    print("wrote app.ico (%d bytes)" % len(out))

    # The two sizes worth looking at, for the screenshot folder.
    shots = os.path.join(here, "..", "tests", "shots")
    if os.path.isdir(shots):
        for size, blob in images:
            if size in (32, 256):
                with open(os.path.join(shots, "icon-%d.png" % size), "wb") as fh:
                    fh.write(blob)


if __name__ == "__main__":
    main()
