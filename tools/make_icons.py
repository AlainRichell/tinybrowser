"""Builds tinybrowser.ico and tiny-file.ico from the two designed PNG renders.

Windows never smooths an icon by itself: it picks the closest size the .ico offers and scales
that one, so the sharpness lives entirely in which sizes are present. The ladder here covers the
taskbar and menu (16, 24), the DPI steps above 100 % that a 16 px slot asks for (20, 32, 40),
Explorer's medium icon (48), the 150/200 % large views (64, 96), the extra-large view (128) and
the jumbo file-type view (256).

Two things decide the quality of each reduction:

- The source is resampled in premultiplied alpha. Reducing an RGBA image whose transparent
  pixels still carry colour drags that colour into the edge, which shows up as a dark rim around
  the drawing on a white background.
- 32 bpp BMP entries up to 48 px (sizes the shell reads without decoding a PNG) and PNG entries
  from 64 px up, where a BMP would cost 16-256 KB each.

The .to icon is a portrait sheet: it is fitted by height into the square frame and centred, the
way Windows' own document icons sit inside their slot, instead of being stretched.

Run: python tools/make_icons.py [outdir]
"""

from __future__ import annotations

import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent

BROWSER_SOURCE = ROOT / "TinyBrowserIcon.png"
OBJECT_SOURCE = ROOT / "TinyObjectIcon.png"
BROWSER_ICO = ROOT / "tinybrowser.ico"
OBJECT_ICO = ROOT / "tiny-file.ico"

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
# Up to this size the shell gets a raw 32 bpp DIB, which it reads without a PNG decoder.
BMP_MAX = 48
# Sharpening applied after a small reduction: the gloss and the rim of the ring merge into a grey
# blob under 32 px, and a light unsharp pass keeps the silhouette readable without ringing it.
SHARPEN = {16: 55, 20: 50, 24: 45, 32: 30}


def reduce_to(image: Image.Image, width: int, height: int, sharpen: int = 0) -> Image.Image:
    """Lanczos reduction in premultiplied alpha, so transparent pixels cannot bleed."""
    reduced = image.convert("RGBa").resize((width, height), Image.LANCZOS).convert("RGBA")
    if sharpen:
        alpha = reduced.getchannel("A")
        colour = reduced.convert("RGB")
        colour = colour.filter(ImageFilter.UnsharpMask(radius=1, percent=sharpen, threshold=1))
        reduced = colour.convert("RGBA")
        reduced.putalpha(alpha)
    return reduced


def square_frames(source: Image.Image) -> dict[int, Image.Image]:
    return {size: reduce_to(source, size, size, SHARPEN.get(size, 0)) for size in SIZES}


def portrait_frames(source: Image.Image) -> dict[int, Image.Image]:
    """The sheet keeps its own aspect ratio inside the square, centred, never stretched."""
    frames = {}
    for size in SIZES:
        width = max(1, round(size * source.width / source.height))
        drawing = reduce_to(source, width, size, SHARPEN.get(size, 0) if width <= size else 0)
        canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        canvas.alpha_composite(drawing, ((size - width) // 2, 0))
        frames[size] = canvas
    return frames


def bmp_entry(image: Image.Image) -> bytes:
    """32 bpp DIB plus the AND mask the format still requires (unused with an alpha channel)."""
    width, height = image.size
    red, green, blue, alpha = image.split()
    bgra = Image.merge("RGBA", (blue, green, red, alpha))
    # Colour rows are bottom-up BGRA.
    xor = bgra.tobytes()
    row = b"".join(xor[y * width * 4 : (y + 1) * width * 4] for y in range(height - 1, -1, -1))
    bits = alpha.point(lambda value: 255 if value == 0 else 0).convert("1").tobytes()
    packed = (width + 7) // 8
    mask_stride = ((width + 31) // 32) * 4
    mask = b"".join(
        bits[y * packed : (y + 1) * packed].ljust(mask_stride, b"\0") for y in range(height - 1, -1, -1)
    )
    header = struct.pack(
        "<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0, len(row) + len(mask), 0, 0, 0, 0
    )
    return header + row + mask


def png_entry(image: Image.Image) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_ico(path: Path, frames: dict[int, Image.Image]) -> int:
    entries = []
    payloads = []
    offset = 6 + 16 * len(frames)
    for size in sorted(frames):
        data = png_entry(frames[size]) if size > BMP_MAX else bmp_entry(frames[size])
        entries.append(
            struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        )
        payloads.append(data)
        offset += len(data)
    blob = struct.pack("<HHH", 0, 1, len(frames)) + b"".join(entries) + b"".join(payloads)
    path.write_bytes(blob)
    return len(blob)


def contact_sheet(frames: dict[int, Image.Image], path: Path) -> None:
    """Every size, upscaled with whole-pixel blocks, over a light and a dark background."""
    cell = 288
    columns = 5
    rows = (len(frames) + columns - 1) // columns
    band = rows * cell
    sheet = Image.new("RGBA", (columns * cell, band * 2), (246, 246, 246, 255))
    sheet.paste(Image.new("RGBA", (columns * cell, band), (28, 28, 28, 255)), (0, band))
    for index, size in enumerate(sorted(frames)):
        zoom = max(1, min(cell // (size * 2), 8))
        grown = frames[size].resize((size * zoom, size * zoom), Image.NEAREST)
        x = (index % columns) * cell + (cell - grown.width) // 2
        y = (index // columns) * cell + (cell - grown.height) // 2
        sheet.alpha_composite(grown, (x, y))
        sheet.alpha_composite(grown, (x, y + band))
    sheet.convert("RGB").save(path)


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "tools" / "icon-previews"
    out.mkdir(parents=True, exist_ok=True)

    browser = square_frames(Image.open(BROWSER_SOURCE).convert("RGBA"))
    obj = portrait_frames(Image.open(OBJECT_SOURCE).convert("RGBA"))

    print(f"{BROWSER_ICO.name}: {write_ico(BROWSER_ICO, browser)} bytes")
    print(f"{OBJECT_ICO.name}: {write_ico(OBJECT_ICO, obj)} bytes")
    contact_sheet(browser, out / "preview-browser.png")
    contact_sheet(obj, out / "preview-object.png")
    print(f"previews: {out / 'preview-browser.png'} {out / 'preview-object.png'}")


if __name__ == "__main__":
    main()
