"""Generate the original StirHex application and document icons.

Only Python's standard library is used. Artwork is rendered at 4x resolution and
box-filtered before each PNG-compressed ICO entry is written.
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path


SIZES = (16, 20, 24, 32, 48, 64, 128, 256)
SUPERSAMPLE = 4
RGBA = tuple[int, int, int, int]


class Canvas:
    def __init__(self, size: int) -> None:
        self.size = size
        self.pixels = bytearray(size * size * 4)

    def blend(self, x: int, y: int, color: RGBA) -> None:
        if x < 0 or y < 0 or x >= self.size or y >= self.size:
            return
        i = (y * self.size + x) * 4
        sr, sg, sb, sa = color
        da = self.pixels[i + 3]
        out_a = sa + (da * (255 - sa) + 127) // 255
        if out_a == 0:
            return
        for channel, source in enumerate((sr, sg, sb)):
            dest = self.pixels[i + channel]
            numerator = source * sa * 255 + dest * da * (255 - sa)
            self.pixels[i + channel] = (numerator + out_a * 127) // (out_a * 255)
        self.pixels[i + 3] = out_a

    def rounded_rect(self, left: float, top: float, right: float, bottom: float,
                     radius: float, color: RGBA) -> None:
        for y in range(max(0, int(top)), min(self.size, math.ceil(bottom))):
            for x in range(max(0, int(left)), min(self.size, math.ceil(right))):
                cx = min(max(x + 0.5, left + radius), right - radius)
                cy = min(max(y + 0.5, top + radius), bottom - radius)
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= radius ** 2:
                    self.blend(x, y, color)

    def polygon(self, points: list[tuple[float, float]], color: RGBA) -> None:
        min_x = max(0, int(min(p[0] for p in points)))
        max_x = min(self.size, math.ceil(max(p[0] for p in points)))
        min_y = max(0, int(min(p[1] for p in points)))
        max_y = min(self.size, math.ceil(max(p[1] for p in points)))
        for y in range(min_y, max_y):
            for x in range(min_x, max_x):
                px, py = x + 0.5, y + 0.5
                inside = False
                previous = points[-1]
                for current in points:
                    x1, y1 = previous
                    x2, y2 = current
                    if (y1 > py) != (y2 > py):
                        cross_x = (x2 - x1) * (py - y1) / (y2 - y1) + x1
                        if px < cross_x:
                            inside = not inside
                    previous = current
                if inside:
                    self.blend(x, y, color)

    def line(self, start: tuple[float, float], end: tuple[float, float],
             width: float, color: RGBA) -> None:
        x1, y1 = start
        x2, y2 = end
        radius = width / 2
        min_x = max(0, int(min(x1, x2) - radius - 1))
        max_x = min(self.size, math.ceil(max(x1, x2) + radius + 1))
        min_y = max(0, int(min(y1, y2) - radius - 1))
        max_y = min(self.size, math.ceil(max(y1, y2) + radius + 1))
        dx, dy = x2 - x1, y2 - y1
        length_sq = dx * dx + dy * dy
        for y in range(min_y, max_y):
            for x in range(min_x, max_x):
                px, py = x + 0.5, y + 0.5
                t = 0.0 if length_sq == 0 else ((px - x1) * dx + (py - y1) * dy) / length_sq
                t = min(1.0, max(0.0, t))
                nearest_x, nearest_y = x1 + t * dx, y1 + t * dy
                if (px - nearest_x) ** 2 + (py - nearest_y) ** 2 <= radius * radius:
                    self.blend(x, y, color)


def draw_grid(canvas: Canvas, box: tuple[float, float, float, float], alpha: int) -> None:
    left, top, right, bottom = box
    width = max(1.0, canvas.size * 0.012)
    color = (73, 151, 190, alpha)
    for index in range(1, 4):
        x = left + (right - left) * index / 4
        y = top + (bottom - top) * index / 4
        canvas.line((x, top), (x, bottom), width, color)
        canvas.line((left, y), (right, y), width, color)


def draw_spiral(canvas: Canvas, center: tuple[float, float], radius: float,
                compact: bool = False) -> None:
    turns = 1.55 if compact else 2.15
    samples = 48 if compact else 96
    points: list[tuple[float, float]] = []
    for index in range(samples + 1):
        ratio = index / samples
        theta = ratio * turns * math.tau - math.pi / 2
        current_radius = radius * (0.10 + 0.90 * ratio)
        points.append((center[0] + math.cos(theta) * current_radius,
                       center[1] + math.sin(theta) * current_radius))
    line_width = max(1.3 * SUPERSAMPLE, radius * (0.16 if compact else 0.13))
    shadow = (0, 20, 45, 135)
    cyan = (64, 226, 244, 255)
    for first, second in zip(points, points[1:]):
        canvas.line((first[0] + SUPERSAMPLE * 0.6, first[1] + SUPERSAMPLE * 0.8),
                    (second[0] + SUPERSAMPLE * 0.6, second[1] + SUPERSAMPLE * 0.8),
                    line_width * 1.25, shadow)
    for first, second in zip(points, points[1:]):
        canvas.line(first, second, line_width, cyan)


def render_app(size: int) -> bytes:
    scale = SUPERSAMPLE
    large = size * scale
    canvas = Canvas(large)
    margin = large * 0.055
    radius = large * 0.18
    canvas.rounded_rect(margin, margin, large - margin, large - margin, radius,
                        (5, 30, 63, 255))
    canvas.rounded_rect(margin * 1.35, margin * 1.35, large - margin * 1.35,
                        large - margin * 1.35, radius * 0.82, (8, 48, 88, 255))
    grid_box = (large * 0.16, large * 0.16, large * 0.84, large * 0.84)
    draw_grid(canvas, grid_box, 150)
    draw_spiral(canvas, (large * 0.50, large * 0.51), large * 0.31, size <= 24)
    return downsample(canvas, size)


def render_document(size: int) -> bytes:
    scale = SUPERSAMPLE
    large = size * scale
    canvas = Canvas(large)
    paper = [(large * 0.15, large * 0.05), (large * 0.66, large * 0.05),
             (large * 0.88, large * 0.27), (large * 0.88, large * 0.95),
             (large * 0.15, large * 0.95)]
    canvas.polygon([(x + scale, y + scale) for x, y in paper], (0, 13, 28, 105))
    canvas.polygon(paper, (235, 247, 252, 255))
    outline = (12, 57, 91, 255)
    for first, second in zip(paper, paper[1:] + paper[:1]):
        canvas.line(first, second, max(scale, large * 0.025), outline)
    fold = [(large * 0.66, large * 0.05), (large * 0.66, large * 0.27),
            (large * 0.88, large * 0.27)]
    canvas.polygon(fold, (173, 220, 235, 255))
    canvas.line(fold[0], fold[1], max(scale, large * 0.018), outline)
    canvas.line(fold[1], fold[2], max(scale, large * 0.018), outline)

    badge = (large * 0.22, large * 0.35, large * 0.81, large * 0.84)
    canvas.rounded_rect(*badge, large * 0.075, (7, 43, 79, 255))
    draw_grid(canvas, (large * 0.28, large * 0.41, large * 0.75, large * 0.78), 145)
    draw_spiral(canvas, (large * 0.515, large * 0.60), large * 0.205, size <= 24)
    return downsample(canvas, size)


def downsample(canvas: Canvas, size: int) -> bytes:
    factor = canvas.size // size
    output = bytearray(size * size * 4)
    sample_count = factor * factor
    for y in range(size):
        for x in range(size):
            alpha_sum = 0
            premultiplied = [0, 0, 0]
            for sy in range(y * factor, (y + 1) * factor):
                for sx in range(x * factor, (x + 1) * factor):
                    i = (sy * canvas.size + sx) * 4
                    alpha = canvas.pixels[i + 3]
                    alpha_sum += alpha
                    for channel in range(3):
                        premultiplied[channel] += canvas.pixels[i + channel] * alpha
            out = (y * size + x) * 4
            if alpha_sum:
                for channel in range(3):
                    output[out + channel] = (premultiplied[channel] + alpha_sum // 2) // alpha_sum
            output[out + 3] = (alpha_sum + sample_count // 2) // sample_count
    return bytes(output)


def png_bytes(size: int, pixels: bytes) -> bytes:
    signature = b"\x89PNG\r\n\x1a\n"

    def chunk(kind: bytes, payload: bytes) -> bytes:
        data = kind + payload
        return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data))

    rows = b"".join(b"\x00" + pixels[y * size * 4:(y + 1) * size * 4]
                    for y in range(size))
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return signature + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def ico_bytes(renderer) -> bytes:
    images = [(size, png_bytes(size, renderer(size))) for size in SIZES]
    offset = 6 + len(images) * 16
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    payload = bytearray()
    for size, image in images:
        encoded_size = 0 if size == 256 else size
        directory.extend(struct.pack("<BBBBHHII", encoded_size, encoded_size, 0, 0,
                                     1, 32, len(image), offset))
        payload.extend(image)
        offset += len(image)
    return bytes(directory + payload)


def main() -> None:
    default_output = Path(__file__).resolve().parents[1] / "StirHex" / "res"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=default_output)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    outputs = (("ico_128.ico", render_app), ("ico_129.ico", render_document))
    for name, renderer in outputs:
        destination = args.output_dir / name
        destination.write_bytes(ico_bytes(renderer))
        print(f"generated {destination} ({destination.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
