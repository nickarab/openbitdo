#!/usr/bin/env python3
"""Gera o ícone do openbitdo (desenho próprio, sem arte de terceiros)."""
from PIL import Image, ImageDraw

SIZE = 256
img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

BG = (26, 28, 33, 255)
BODY = (235, 236, 240, 255)
ACCENT = (32, 200, 120, 255)
DARK = (40, 42, 48, 255)

d.rounded_rectangle([0, 0, SIZE - 1, SIZE - 1], radius=48, fill=BG)

# corpo do controle
d.rounded_rectangle([28, 96, 228, 168], radius=34, fill=BODY)
d.ellipse([28, 120, 108, 200], fill=BODY)
d.ellipse([148, 120, 228, 200], fill=BODY)

# d-pad
d.rounded_rectangle([66, 118, 82, 166], radius=4, fill=DARK)
d.rounded_rectangle([54, 134, 94, 150], radius=4, fill=DARK)

# botões
for cx, cy, color in ((150, 132, ACCENT), (168, 150, ACCENT), (186, 132, ACCENT), (168, 114, ACCENT)):
    d.ellipse([cx - 7, cy - 7, cx + 7, cy + 7], fill=color)

# sticks
d.ellipse([40, 150, 76, 186], fill=DARK)
d.ellipse([180, 150, 216, 186], fill=DARK)

# "P1/P2" nas costas
d.rounded_rectangle([96, 62, 116, 76], radius=4, fill=ACCENT)
d.rounded_rectangle([140, 62, 160, 76], radius=4, fill=ACCENT)

img.save("openbitdo.png")
print("openbitdo.png", img.size)
