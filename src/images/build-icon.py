"""Regenerates the application icons from the vector source.

    python src/images/build-icon.py

Outputs, all overwritten in place:
    Win32DiskImager.ico   app + CLI icon (referenced from the .rc files)
    setup.ico             installer icon (referenced from setup.iss)
    Win32DiskImager.png   window icon, pulled in through gui_icons.qrc

The project used to ship a single 32x32 icon, so Windows had nothing to
show at 64/128/256 and it looked lost on a high-DPI display. icon-source.svg
is a vector tracing of that original artwork: every size below is rasterised
from it at its target resolution, so nothing is ever upscaled and future
sizes cost nothing.

Requires: pip install pillow cairosvg
"""
import io
from pathlib import Path

import cairosvg
from PIL import Image

HERE = Path(__file__).parent
SOURCE = HERE / "icon-source.svg"

SIZES = [16, 24, 32, 48, 64, 128, 256]

# Window icon: Qt scales this one down itself, so ship it large enough that
# scaling stays a downscale on any display.
WINDOW_PNG_SIZE = 256


def render(size: int) -> Image.Image:
    png = cairosvg.svg2png(url=str(SOURCE), output_width=size,
                           output_height=size, background_color=None)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def main() -> None:
    frames = {size: render(size) for size in SIZES}
    largest = frames[SIZES[-1]]

    # Pillow would otherwise re-derive every entry by resampling the base
    # image; append_images keeps each frame as rendered from the vector.
    for name in ("Win32DiskImager.ico", "setup.ico"):
        largest.save(HERE / name, format="ICO",
                     sizes=[(s, s) for s in SIZES],
                     append_images=[frames[s] for s in SIZES[:-1]])
        print(f"{name}: {(HERE / name).stat().st_size} bytes, sizes {SIZES}")

    render(WINDOW_PNG_SIZE).save(HERE / "Win32DiskImager.png")
    print(f"Win32DiskImager.png: {WINDOW_PNG_SIZE}x{WINDOW_PNG_SIZE}")


if __name__ == "__main__":
    main()
