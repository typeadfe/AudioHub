"""Render the AudioHub geometric SVG mark as an antialiased PNG."""

from math import cos, radians, sin
from pathlib import Path

from PIL import Image, ImageDraw


SIZE = 1024
SCALE = 4
BLUE = "#2563EB"
TEAL = "#22B8AC"


def box(*coords):
    return tuple(round(value * SCALE) for value in coords)


def capsule(draw, x1, x2, cy, radius, rotation, pivot_x):
    angle = radians(rotation)
    cx = pivot_x

    def rotated(x):
        dx = x - cx
        return ((cx + dx * cos(angle)) * SCALE, (cy + dx * sin(angle)) * SCALE)

    start = rotated(x1 + radius)
    end = rotated(x2 - radius)
    width = round(2 * radius * SCALE)
    draw.line([start, end], fill=TEAL, width=width)
    for x, y in (start, end):
        r = radius * SCALE
        draw.ellipse((round(x-r), round(y-r), round(x+r), round(y+r)), fill=TEAL)


canvas = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
ring = Image.new("L", canvas.size, 0)
ring_draw = ImageDraw.Draw(ring)
ring_draw.ellipse(box(132, 235, 892, 995), fill=255)
ring_draw.ellipse(box(234, 337, 790, 893), fill=0)
ring_draw.rectangle(box(0, 615, 1024, 1024), fill=0)
canvas.paste(BLUE, (0, 0, *canvas.size), ring)

draw = ImageDraw.Draw(canvas)
draw.rounded_rectangle(box(132, 574, 292, 876), radius=74*SCALE, fill=BLUE)
draw.rounded_rectangle(box(732, 574, 892, 876), radius=74*SCALE, fill=BLUE)
draw.rounded_rectangle(box(478, 456, 546, 661), radius=34*SCALE, fill=TEAL)
capsule(draw, 370, 524, 685, 33, -43, 447)
capsule(draw, 500, 654, 685, 33, 43, 577)

output = Path(__file__).with_name("audiohub-icon.png")
master = canvas.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
master.save(output)
print(output)

root = Path(__file__).resolve().parent.parent
android_res = root / "android-source" / "app" / "src" / "main" / "res"
foreground_path = android_res / "drawable-nodpi" / "ic_launcher_foreground.png"
foreground_path.parent.mkdir(parents=True, exist_ok=True)
foreground = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
mark = master.resize((800, 800), Image.Resampling.LANCZOS)
foreground.alpha_composite(mark, (112, 112))
foreground.save(foreground_path)
print(foreground_path)

windows_icon_path = root / "windows-player" / "src" / "audiohub.ico"
windows_icon = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
icon_draw = ImageDraw.Draw(windows_icon)
icon_draw.rounded_rectangle((12, 12, 1012, 1012), radius=216, fill="#F6F9FC")
windows_icon.alpha_composite(master)
windows_icon.save(
    windows_icon_path,
    format="ICO",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
)
print(windows_icon_path)
