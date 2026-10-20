"""Generates the .to file icon: a portrait sheet with the tiny donut in the lower right corner.

Dev tool, not part of the build: run it only when the icon needs to change.
    python tools/make_file_icon.py
The drawing is defined on a 32 px grid and the bigger sizes are nearest-neighbour blow-ups of it, so
the icon keeps its pixel look instead of turning into a thin-stroked vector drawing. Writes one .ico
per variant under tools/file-icons/, a 256 px PNG of each, and a contact sheet (16/32/48 px, on a
light and a dark background) at the sizes Explorer really shows.

The chosen one (v4-hoja-donut-grande) is copied to the project root as tiny-file.ico, which
tinybrowser.rc embeds as icon id 2 and the .to association points at. v5 to v8 are the same sheets
with ruled lines: kept as variants, not embedded.
"""

from __future__ import annotations

import os
import struct
import zlib

from make_icon import PALETTE, TRANSPARENT, ring

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "file-icons")

BLACK, MAROON, GREEN, OLIVE, NAVY, PURPLE, TEAL, SILVER = 0, 1, 2, 3, 4, 5, 6, 7
GRAY, RED, LIME, YELLOW, BLUE, FUCHSIA, AQUA, WHITE = 8, 9, 10, 11, 12, 13, 14, 15

# The grid everything is drawn on, and the sizes the .ico carries.
BASE = 32
ICO_SIZES = (16, 32, 48, 256)


def sheet_geometry(size: int) -> dict:
    """A portrait page: real sheets are taller than wide, and on a square icon that difference is what
    makes the drawing read as paper instead of as a card."""
    margin = max(1, round(size * 0.06))
    top, bottom = margin, size - 1 - margin
    height = bottom - top + 1
    width = max(6, round(height * 0.80))
    left = (size - width) // 2
    return {"left": left, "top": top, "right": left + width - 1, "bottom": bottom,
            "fold": max(3, round(width * 0.32))}


def fill_ring(diameter: int) -> list[list[int]]:
    """The donut with no white in it: on white paper a white highlight disappears and the ring reads
    as an open C."""
    return [[SILVER if cell == WHITE else cell for cell in row]
            for row in ring(diameter, "bevel")]


def sheet(size: int, paper: int, edge: int, fold_colour: int, badge_ratio: float,
          badge_style: str, rules: bool) -> list[list[int]]:
    grid = [[TRANSPARENT] * size for _ in range(size)]
    box = sheet_geometry(size)
    left, top, right, bottom, fold = (box["left"], box["top"], box["right"], box["bottom"],
                                      box["fold"])

    def cut(x: int, y: int) -> bool:
        return (right - x) + (y - top) < fold

    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            if not cut(x, y):
                grid[y][x] = paper

    # The fold: the triangle that turns the clipped corner into a folded page. Its right angle sits
    # inside the sheet and its hypotenuse is the clip itself, so the silhouette stays a page. Both
    # inner edges get their line, or the triangle reads as a grey patch instead of a fold.
    for y in range(top, top + fold + 1):
        for x in range(right - fold, right + 1):
            if not cut(x, y):
                grid[y][x] = fold_colour
    for y in range(top, top + fold + 1):
        for x in range(right - fold, right + 1):
            if grid[y][x] != fold_colour:
                continue
            if x == right - fold or y == top + fold:
                grid[y][x] = edge

    # The badge is sized against the sheet, not against the icon: on a portrait page the room the
    # corner offers is the sheet's width.
    diameter = max(6, round((right - left + 1) * badge_ratio))
    donut = fill_ring(diameter) if badge_style == "fill" else ring(diameter, badge_style)
    origin_x = right - diameter + 1
    origin_y = bottom - diameter + 1

    centre_x = origin_x + diameter / 2.0
    centre_y = origin_y + diameter / 2.0
    clearance = diameter / 2.0 + 2.0  # the ruled lines keep this much air around the badge

    # Ruled lines on the sheet's face, in the same light grey as the fold: one pixel on the 32 grid,
    # so they blow up into the same blocks as everything else. They keep air on every side -- the
    # page's border and the badge's ring -- and a line that would run behind the badge is cut there
    # instead of reappearing on the far side of it.
    if rules:
        step = max(3, round(size / 8))
        y = top + fold + 2  # a line never runs right under the fold's edge
        while y <= bottom - 2:
            for x in range(left + 2, right - 1):
                if grid[y][x] != paper:
                    continue
                dx = x + 0.5 - centre_x
                dy = y + 0.5 - centre_y
                if dx * dx + dy * dy <= clearance * clearance:
                    continue
                grid[y][x] = fold_colour
            y += step

    for y in range(diameter):
        for x in range(diameter):
            colour = donut[y][x]
            if colour == TRANSPARENT:
                continue
            ty, tx = origin_y + y, origin_x + x
            if 0 <= ty < size and 0 <= tx < size:
                grid[ty][tx] = colour

    # A dark ring around the badge, or its edge melts into the page.
    inside = {(origin_x + x, origin_y + y) for y in range(diameter) for x in range(diameter)
              if donut[y][x] != TRANSPARENT}
    marks = set()
    for x, y in inside:
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if (nx, ny) in inside or not (0 <= nx < size and 0 <= ny < size):
                continue
            if grid[ny][nx] not in (TRANSPARENT, BLACK):
                marks.add((nx, ny))
    for x, y in marks:
        grid[y][x] = BLACK

    # And the page's own border, drawn last so it closes behind the badge.
    marks = set()
    for y in range(size):
        for x in range(size):
            if grid[y][x] in (TRANSPARENT, edge):
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < size and 0 <= ny < size) or grid[ny][nx] == TRANSPARENT:
                    marks.add((x, y))
                    break
    for x, y in marks:
        grid[y][x] = edge
    return grid


def scale_to(grid: list[list[int]], target: int) -> list[list[int]]:
    """Nearest neighbour: the pixel blocks of the base grid are the point, so nothing is smoothed."""
    source = len(grid)
    return [[grid[min(source - 1, y * source // target)][min(source - 1, x * source // target)]
             for x in range(target)] for y in range(target)]


# name: (paper, edge, fold colour, badge ratio, badge style, ruled lines)
VARIANTS = {
    "v1-hoja-donut-pleno": (WHITE, BLACK, SILVER, 0.58, "fill", False),
    "v2-hoja-donut-medio": (WHITE, BLACK, SILVER, 0.48, "fill", False),
    "v3-hoja-donut-rayas": (WHITE, BLACK, SILVER, 0.58, "stripes", False),
    "v4-hoja-donut-grande": (WHITE, BLACK, SILVER, 0.68, "fill", False),
    "v5-lineas-donut-pleno": (WHITE, BLACK, SILVER, 0.58, "fill", True),
    "v6-lineas-donut-medio": (WHITE, BLACK, SILVER, 0.48, "fill", True),
    "v7-lineas-donut-rayas": (WHITE, BLACK, SILVER, 0.58, "stripes", True),
    "v8-lineas-donut-grande": (WHITE, BLACK, SILVER, 0.68, "fill", True),
}


def render(name: str, size: int) -> list[list[int]]:
    """16 and 32 are drawn on their own grid, where a one pixel line is the right weight; the bigger
    copies are blow-ups of the 32 grid, so the pixel blocks stay visible instead of the drawing
    turning into a smooth vector shape."""
    paper, edge, fold_colour, badge_ratio, badge_style, rules = VARIANTS[name]
    if size in (16, BASE):
        return sheet(size, paper, edge, fold_colour, badge_ratio, badge_style, rules)
    return scale_to(sheet(BASE, paper, edge, fold_colour, badge_ratio, badge_style, rules), size)


def ico(images: list[tuple[int, list[list[int]]]]) -> bytes:
    """4-bit indexed entries for the small sizes plus a PNG one for 256, which is what Explorer
    expects there."""
    header = struct.pack("<HHH", 0, 1, len(images))
    directory = b""
    payload = b""
    offset = len(header) + 16 * len(images)

    for size, grid in images:
        if size >= 256:
            body = png(grid)
            directory += struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(body), offset)
            offset += len(body)
            payload += body
            continue

        stride = ((size * 4 + 31) // 32) * 4
        xor_rows = []
        for y in range(size - 1, -1, -1):
            row = bytearray(stride)
            for x in range(size):
                colour = grid[y][x]
                if colour == TRANSPARENT:
                    continue
                if x % 2 == 0:
                    row[x // 2] |= (colour & 0x0F) << 4
                else:
                    row[x // 2] |= colour & 0x0F
            xor_rows.append(bytes(row))

        mask_stride = ((size + 31) // 32) * 4
        mask_rows = []
        for y in range(size - 1, -1, -1):
            row = bytearray(mask_stride)
            for x in range(size):
                if grid[y][x] == TRANSPARENT:
                    row[x // 8] |= 0x80 >> (x % 8)
            mask_rows.append(bytes(row))

        xor_bitmap = b"".join(xor_rows)
        mask_bitmap = b"".join(mask_rows)
        info = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 4, 0,
                           len(xor_bitmap) + len(mask_bitmap), 0, 0, 16, 0)
        palette = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in PALETTE)
        image = info + palette + xor_bitmap + mask_bitmap
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 16, 0, 1, 4, len(image),
                                 offset)
        offset += len(image)
        payload += image

    return header + directory + payload


def png(grid: list[list[int]]) -> bytes:
    height = len(grid)
    width = len(grid[0])
    raw = b""
    for row in grid:
        raw += b"\x00"
        for colour in row:
            if colour == TRANSPARENT:
                raw += struct.pack("<BBBB", 0, 0, 0, 0)
            else:
                r, g, b = PALETTE[colour]
                raw += struct.pack("<BBBB", r, g, b, 255)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data)))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def contact(names: list[str], scale: int = 6) -> None:
    """Rows = variants, columns = 16/32/48 px; the top half on a light background and the bottom on
    the app's own dark one, because an icon can read on one and vanish on the other."""
    sizes = (16, 32, 48)
    cell = max(sizes) * scale + 10
    label = 60
    width = cell * len(sizes) + label
    row_height = cell
    height = row_height * len(names) * 2 + 10
    canvas = [[(255, 255, 255, 255)] * width for _ in range(height)]

    for row in range(height):
        light = row < height // 2
        for column in range(width):
            canvas[row][column] = (0xF5, 0xF5, 0xF5, 255) if light else (0x0F, 0x14, 0x1B, 255)

    for band in (0, 1):
        for index, name in enumerate(names):
            origin_y = band * (row_height * len(names)) + index * row_height + 5
            for column, size in enumerate(sizes):
                grid = render(name, size)
                origin_x = label + column * cell + 5 + (max(sizes) - size) * scale // 2
                for y in range(size):
                    for x in range(size):
                        colour = grid[y][x]
                        if colour == TRANSPARENT:
                            continue
                        r, g, b = PALETTE[colour]
                        for dy in range(scale):
                            for dx in range(scale):
                                canvas[origin_y + y * scale + dy][origin_x + x * scale + dx] = (
                                    r, g, b, 255)

    write_png(os.path.join(OUT, "contacto.png"), canvas)


def write_png(path: str, canvas: list[list[tuple[int, int, int, int]]]) -> None:
    height = len(canvas)
    width = len(canvas[0])
    raw = b"".join(b"\x00" + b"".join(struct.pack("<BBBB", *px) for px in row) for row in canvas)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data)))

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for name in VARIANTS:
        images = [(size, render(name, size)) for size in ICO_SIZES]
        target = os.path.join(OUT, name + ".ico")
        with open(target, "wb") as handle:
            handle.write(ico(images))
        write_png(os.path.join(OUT, name + "-256.png"),
                  [[PALETTE[colour] + (255,) if colour != TRANSPARENT else (0, 0, 0, 0)
                    for colour in row] for row in render(name, 256)])
        print(f"{name}.ico  {os.path.getsize(target)} bytes")
    contact(list(VARIANTS))
    print("contacto:", os.path.join(OUT, "contacto.png"))


if __name__ == "__main__":
    main()
