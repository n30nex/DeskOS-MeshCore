#!/usr/bin/env python3
"""Render the deterministic DeskOS boot preview used by the README."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


SIZE = 480
DURATION_MS = 3200
FRAME_MS = 100
CHARCOAL = (23, 25, 26)
SURFACE = (32, 38, 43)
CYAN = (32, 217, 237)
COBALT = (30, 90, 239)
LIME = (132, 255, 46)
TEXT = (244, 247, 251)
MUTED = (166, 176, 183)
TRACK = (48, 58, 66)
STARS = ((36, 92), (92, 314), (402, 104), (438, 286),
         (54, 392), (416, 374), (126, 46), (354, 336))


def font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    try:
        return ImageFont.truetype("DejaVuSans.ttf", size)
    except OSError:
        return ImageFont.load_default()


def ramp(value: int, start: int, end: int) -> float:
    if value <= start:
        return 0.0
    if value >= end:
        return 1.0
    return (value - start) / (end - start)


def status(elapsed: int) -> str:
    if elapsed < 600:
        return "Starting DeskOS"
    if elapsed < 1500:
        return "Drawing the mesh"
    if elapsed < 2800:
        return "Opening your desk"
    return "Ready"


def centered_text(draw: ImageDraw.ImageDraw, text: str, y: int,
                  text_font: ImageFont.ImageFont, fill: tuple[int, ...]) -> None:
    box = draw.textbbox((0, 0), text, font=text_font)
    draw.text(((SIZE - (box[2] - box[0])) // 2, y), text,
              font=text_font, fill=fill)


def render_frame(elapsed: int) -> Image.Image:
    image = Image.new("RGB", (SIZE, SIZE), CHARCOAL)
    draw = ImageDraw.Draw(image, "RGBA")
    pulse = 0.5 + 0.5 * math.sin(elapsed / 260.0)
    for index, (x, y) in enumerate(STARS):
        alpha = int(35 + 65 * ((pulse + index * 0.13) % 1.0))
        radius = 2 if index % 3 == 0 else 1
        color = CYAN if index % 2 == 0 else COBALT
        draw.ellipse((x - radius, y - radius, x + radius, y + radius),
                     fill=(*color, alpha))

    reveal = ramp(elapsed, 40, 520)
    alpha = int(255 * reveal)
    draw.rounded_rectangle((124, 38, 356, 262), radius=22,
                           fill=(*CYAN, alpha))
    draw.rounded_rectangle((136, 50, 344, 216), radius=14,
                           fill=(*SURFACE, alpha))
    draw.ellipse((229, 228, 251, 250), fill=(*LIME, alpha))

    nodes = ((186, 159), (286, 159), (236, 87))
    links = ((nodes[0], nodes[2]), (nodes[2], nodes[1]), (nodes[0], nodes[1]))
    link_count = 0 if elapsed < 760 else 1 if elapsed < 980 else 2 if elapsed < 1200 else 3
    for start, end in links[:link_count]:
        draw.line((start, end), fill=(*COBALT, alpha), width=7)
    node_count = 0 if elapsed < 420 else 1 if elapsed < 650 else 2 if elapsed < 880 else 3
    node_alpha = int(170 + 85 * pulse)
    for x, y in nodes[:node_count]:
        draw.ellipse((x - 13, y - 13, x + 13, y + 13),
                     fill=(*LIME, node_alpha))

    if link_count:
        base = ((elapsed - 650) // 18) % 100
        for index, size in enumerate((7, 10, 13)):
            travel = (base + index * 31) % 100
            x = 306 + travel * 52 // 100
            y = 108 - travel * 52 // 100
            draw.rounded_rectangle((x, y, x + size, y + size), radius=2,
                                   fill=(*CYAN, 120 + travel * 135 // 100))

    title_alpha = int(255 * ramp(elapsed, 1050, 1600))
    centered_text(draw, "DeskOS", 280, font(34), (*TEXT, title_alpha))
    centered_text(draw, "Touch the mesh", 324, font(17), (*CYAN, title_alpha))

    progress = min(elapsed, 2800) / 2800
    draw.rounded_rectangle((64, 390, 416, 396), radius=3, fill=TRACK)
    draw.rounded_rectangle((64, 390, 68 + int(344 * progress), 396),
                           radius=3, fill=CYAN)
    centered_text(draw, status(elapsed), 416, font(15), MUTED)
    return image


def render(out_dir: Path) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    frames = [render_frame(ms) for ms in range(0, DURATION_MS, FRAME_MS)]
    gif = out_dir / "deskos-boot.gif"
    frames[0].save(gif, save_all=True, append_images=frames[1:],
                   duration=FRAME_MS, loop=0, optimize=True, disposal=2)
    still = out_dir / "deskos-boot-ready.png"
    render_frame(2950).save(still, optimize=True)
    return gif, still


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    gif, still = render(args.out_dir)
    print(f"{gif}\n{still}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
