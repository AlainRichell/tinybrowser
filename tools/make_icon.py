"""Generates tinybrowser.ico: pixel-art donut in the Windows 95 VGA palette.

Dev tool, not part of the build: run it only when the icon needs to change.
    .venv/Scripts/python.exe tools/make_icon.py
Writes tools/preview.png (8x, checkerboard) next to the .ico so the drawing can be eyeballed.
"""

from __future__ import annotations

import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)

# The 16 VGA colours Windows 95 icons were drawn with, in that order.
PALETTE = [
    (0x00, 0x00, 0x00),  # 0 black   (outline)
    (0x80, 0x00, 0x00),  # 1 maroon
    (0x00, 0x80, 0x00),  # 2 green
    (0x80, 0x80, 0x00),  # 3 olive
    (0x00, 0x00, 0x80),  # 4 navy
    (0x80, 0x00, 0x80),  # 5 purple
    (0x00, 0x80, 0x80),  # 6 teal    (accent)
    (0xC0, 0xC0, 0xC0),  # 7 silver  (dough body)
    (0x80, 0x80, 0x80),  # 8 gray    (shadow)
    (0xFF, 0x00, 0x00),  # 9 red
    (0x00, 0xFF, 0x00),  # 10 lime
    (0xFF, 0xFF, 0x00),  # 11 yellow
    (0x00, 0x00, 0xFF),  # 12 blue
    (0xFF, 0x00, 0xFF),  # 13 fuchsia
    (0x00, 0xFF, 0xFF),  # 14 aqua
    (0xFF, 0xFF, 0xFF),  # 15 white   (highlight)
]
BLACK, TEAL, SILVER, GRAY, WHITE = 0, 6, 7, 8, 15
TRANSPARENT = -1


def ring(size: int, variant: str) -> list[list[int]]:
    """A donut on a size x size grid, one palette index (or TRANSPARENT) per pixel."""
    outer = size * 0.4375
    inner = size * 0.1875
    bevel = size * 0.16
    centre = size / 2.0
    grid = [[TRANSPARENT] * size for _ in range(size)]

    for y in range(size):
        for x in range(size):
            dx = x + 0.5 - centre
            dy = y + 0.5 - centre
            distance = math.hypot(dx, dy)
            if distance > outer or distance < inner:
                continue

            diagonal = dx + dy  # negative = upper left, positive = lower right
            if variant == "bevel":
                # Classic Win95 raised edge, no outline: reads as a donut at 16 px.
                if distance > outer - 0.6 or distance < inner + 0.6:
                    colour = WHITE if diagonal < 0 else GRAY
                elif diagonal < -bevel:
                    colour = WHITE
                elif diagonal > bevel:
                    colour = GRAY
                else:
                    colour = SILVER
            elif variant == "outline":
                # Black outline with a teal quadrant, closer to a browser badge.
                if distance > outer - 0.7:
                    colour = BLACK
                elif distance < inner + 0.7:
                    colour = BLACK
                elif dx > 0 and dy < 0:
                    colour = TEAL
                elif diagonal < -bevel:
                    colour = WHITE
                elif diagonal > bevel:
                    colour = GRAY
                else:
                    colour = SILVER
            else:  # "quadrants"
                if distance > outer - 0.7 or distance < inner + 0.7:
                    colour = BLACK
                elif dx >= 0 and dy < 0:
                    colour = TEAL
                elif dx >= 0:
                    colour = GRAY
                elif dy < 0:
                    colour = WHITE
                else:
                    colour = SILVER
            if variant in ("stripes", "stripes_alt"):
                # Ring body split into three vertical bands, flat colours, black outline:
                # the classic "shield" look, and the clearest thing at 16 px.
                if distance > outer - 0.7 or distance < inner + 0.7:
                    colour = BLACK
                else:
                    band = min(2, int(x * 3 / size))
                    order = (TEAL, WHITE, GRAY) if variant == "stripes" else (GRAY, WHITE, TEAL)
                    colour = order[band]
            grid[y][x] = colour
    return grid


def ico_bytes(images: list[list[list[int]]]) -> bytes:
    """4-bit indexed ICO with an AND mask: 16, 32 and 48 px together stay under 3 KB."""
    header = struct.pack("<HHH", 0, 1, len(images))
    directory = b""
    payload = b""
    offset = len(header) + 16 * len(images)

    for grid in images:
        size = len(grid)
        stride = ((size * 4 + 31) // 32) * 4
        xor_rows = []
        for y in range(size - 1, -1, -1):  # bottom-up
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
        info = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 4, 0, len(xor_bitmap) + len(mask_bitmap),
                           0, 0, 16, 0)
        palette = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in PALETTE)
        image = info + palette + xor_bitmap + mask_bitmap

        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 16, 0, 1, 4, len(image), offset)
        offset += len(image)
        payload += image

    return header + directory + payload


def png_bytes(pixels: list[list[tuple[int, int, int, int]]], width: int, height: int) -> bytes:
    raw = b"".join(b"\x00" + b"".join(struct.pack("<BBBB", *px) for px in row) for row in pixels)
    chunks = []

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    chunks.append(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
    chunks.append(chunk(b"IDAT", zlib.compress(raw, 9)))
    return (b"\x89PNG\r\n\x1a\n" + b"".join(chunks) + chunk(b"IEND", b""))


def preview(variants: list[str], scale: int = 6) -> None:
    """Contact sheet: rows = variants, columns = 16/32/48 px, on a checkerboard."""
    sizes = [16, 32, 48]
    cell = max(sizes) * scale + 8
    width = cell * len(sizes)
    height = cell * len(variants)
    canvas = [[(0, 0, 0, 0)] * width for _ in range(height)]

    for row in range(height):
        for column in range(width):
            shade = 0x9A if ((column // 8) + (row // 8)) % 2 else 0xC8
            canvas[row][column] = (shade, shade, shade, 255)

    for index, variant in enumerate(variants):
        for column, size in enumerate(sizes):
            grid = ring(size, variant)
            origin_x = column * cell + 4 + (max(sizes) - size) * scale // 2
            origin_y = index * cell + 4 + (max(sizes) - size) * scale // 2
            for y in range(size):
                for x in range(size):
                    colour = grid[y][x]
                    if colour == TRANSPARENT:
                        continue
                    r, g, b = PALETTE[colour]
                    for dy in range(scale):
                        for dx in range(scale):
                            canvas[origin_y + y * scale + dy][origin_x + x * scale + dx] = (r, g, b, 255)

    with open(os.path.join(HERE, "preview.png"), "wb") as handle:
        handle.write(png_bytes(canvas, width, height))


def header(variant: str, size: int = 48, scale: int = 8) -> None:
    """One big copy of a variant on a transparent background, for the README."""
    grid = ring(size, variant)
    canvas = [[(0, 0, 0, 0)] * (size * scale) for _ in range(size * scale)]
    for y in range(size):
        for x in range(size):
            colour = grid[y][x]
            if colour == TRANSPARENT:
                continue
            r, g, b = PALETTE[colour]
            for dy in range(scale):
                for dx in range(scale):
                    canvas[y * scale + dy][x * scale + dx] = (r, g, b, 255)
    with open(os.path.join(HERE, "icono.png"), "wb") as handle:
        handle.write(png_bytes(canvas, size * scale, size * scale))


def main() -> None:
    chosen = "bevel"  # the variant installed in the app
    images = [ring(size, chosen) for size in (16, 32, 48)]
    target = os.path.join(PROJECT, "tinybrowser.ico")
    with open(target, "wb") as handle:
        handle.write(ico_bytes(images))
    # The sheet carries every variant: losing sight of one is what makes a design feel lost.
    preview(["bevel", "outline", "quadrants", "stripes", "stripes_alt"])
    header(chosen)
    print("icono:", target, os.path.getsize(target), "bytes")
    print("preview:", os.path.join(HERE, "preview.png"))


if __name__ == "__main__":
    main()
