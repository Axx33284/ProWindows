"""Generate res/app.ico for ProWindows.

Draws the app mark - a tiled window arrangement (one master pane on the left,
two stacked panes on the right) on a dark plate with its top-left and
bottom-right corners cut, in the settings window's own palette: gunmetal
and hazard orange, after DOOM Eternal's menus. Rendered at several sizes and
packed into a PNG-compressed .ico. Uses only the standard library.

    python res/gen_icon.py
"""

import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 48, 64, 128, 256]
SS = 4  # supersampling factor for smooth edges

# theme.h: Panel, Border, Accent, PanelAlt, and a darker step of PanelAlt.
BG        = (0x16, 0x18, 0x1B)   # plate
EDGE      = (0x3A, 0x3E, 0x44)   # plate border
PANE_MAIN = (0xF5, 0x92, 0x1E)   # focused / master pane: the accent
PANE_ALT  = (0x4A, 0x50, 0x58)   # secondary panes
PANE_DIM  = (0x33, 0x38, 0x3E)


def chamfer_coverage(px, py, x, y, w, h, cut):
    """1 if the supersample point is inside the cut-corner rect, else 0."""
    if px < x or py < y or px >= x + w or py >= y + h:
        return False
    if (px - x) + (py - y) < cut:                    # top-left corner cut
        return False
    if (x + w - px) + (y + h - py) < cut:            # bottom-right corner cut
        return False
    return True


def render(size):
    """Return RGBA bytes for one square icon of the given size."""
    n = size * SS
    # accumulate colour + alpha per output pixel
    acc = [[0, 0, 0, 0] for _ in range(size * size)]

    unit = n / 32.0                      # design grid is 32x32
    tile_x = tile_y = 1.0 * unit
    tile_w = tile_h = 30.0 * unit
    tile_cut = 7.0 * unit
    edge = max(1.0, 1.1 * unit)          # the border ring, one pixel at 16 px

    gap = 2.0 * unit
    inner_x = tile_x + 4.0 * unit
    inner_y = tile_y + 4.0 * unit
    inner_w = tile_w - 8.0 * unit
    inner_h = tile_h - 8.0 * unit
    pane_cut = 2.2 * unit

    master_w = inner_w * 0.52
    right_x = inner_x + master_w + gap
    right_w = inner_w - master_w - gap
    right_h = (inner_h - gap) / 2.0

    panes = [
        (inner_x, inner_y, master_w, inner_h, PANE_MAIN, pane_cut),
        (right_x, inner_y, right_w, right_h, PANE_ALT, 0.0),
        (right_x, inner_y + right_h + gap, right_w, right_h, PANE_DIM, 0.0),
    ]

    for sy in range(n):
        py = sy + 0.5
        oy = sy // SS
        for sx in range(n):
            px = sx + 0.5
            if not chamfer_coverage(px, py, tile_x, tile_y, tile_w, tile_h, tile_cut):
                continue
            # the border: the plate minus a slightly smaller plate
            color = EDGE
            if chamfer_coverage(px, py, tile_x + edge, tile_y + edge,
                                tile_w - 2 * edge, tile_h - 2 * edge, tile_cut - edge):
                color = BG
                for rx, ry, rw, rh, c, cut in panes:
                    if chamfer_coverage(px, py, rx, ry, rw, rh, cut):
                        color = c
                        break
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

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
    with open(path, "wb") as fh:
        fh.write(out)
    print("wrote %s (%d bytes)" % (path, len(out)))


if __name__ == "__main__":
    main()
