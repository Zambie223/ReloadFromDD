"""Draws the Reload From DD icon: a purple isometric depot cube, a curved reload arrow, and a cartridge.

Writes Resources/Icon128.png (SML's Mods menu, fixed path and size) and Branding/Icon512.png (for the SMR logo, not shipped).
Pure drawing with Pillow, no external art. Run: py -3 Tools/make_icon.py
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

SS = 4  # supersampling factor
BASE = 512
N = BASE * SS

# Purple, after the Dimensional Depot's in-game colour scheme.
CUBE_TOP = (190, 146, 255)
CUBE_LEFT = (142, 88, 232)
CUBE_RIGHT = (98, 54, 180)
EDGE = (38, 18, 70)
WHITE = (248, 248, 248)
BRASS = (232, 186, 86)
BRASS_DARK = (176, 128, 44)
TIP = (200, 204, 212)
BG_TOP = (46, 52, 62)
BG_BOTTOM = (22, 25, 30)
BORDER = (70, 78, 92)


def s(v):
    return int(round(v * SS))


def rounded_mask(size, radius):
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return mask


def background():
    grad = Image.new("RGB", (N, N))
    d = ImageDraw.Draw(grad)
    for y in range(N):
        t = y / (N - 1)
        d.line([(0, y), (N, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)))
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    img.paste(grad, (0, 0), rounded_mask(N, s(96)))
    ImageDraw.Draw(img).rounded_rectangle((s(6), s(6), N - s(6), N - s(6)), radius=s(90), outline=BORDER, width=s(6))
    return img


def cube(draw, cx, cy, r):
    """Isometric cube; (cx, cy) is the centre of the hexagon outline, r its circumradius."""
    dx = r * math.cos(math.radians(30))
    top = (cx, cy - r)
    upper_left = (cx - dx, cy - r / 2)
    upper_right = (cx + dx, cy - r / 2)
    centre = (cx, cy)
    lower_left = (cx - dx, cy + r / 2)
    lower_right = (cx + dx, cy + r / 2)
    bottom = (cx, cy + r)
    sc = lambda pts: [(s(x), s(y)) for x, y in pts]
    draw.polygon(sc([top, upper_right, centre, upper_left]), fill=CUBE_TOP)
    draw.polygon(sc([upper_left, centre, bottom, lower_left]), fill=CUBE_LEFT)
    draw.polygon(sc([centre, upper_right, lower_right, bottom]), fill=CUBE_RIGHT)
    width = s(7)
    draw.line(sc([top, upper_right, lower_right, bottom, lower_left, upper_left, top]), fill=EDGE, width=width, joint="curve")
    draw.line(sc([upper_left, centre, upper_right]), fill=EDGE, width=width, joint="curve")
    draw.line(sc([centre, bottom]), fill=EDGE, width=width)


def reload_arrow(layer, cx, cy, radius, start_deg, end_deg, width, head, color):
    """Clockwise arc from start_deg to end_deg (degrees, 0 = east, y down) with an arrowhead at the end."""
    d = ImageDraw.Draw(layer)
    box = (s(cx - radius), s(cy - radius), s(cx + radius), s(cy + radius))
    d.arc(box, start=start_deg, end=end_deg, fill=color, width=s(width))
    a = math.radians(end_deg)
    ex, ey = cx + radius * math.cos(a), cy + radius * math.sin(a)
    # Tangent direction for a clockwise arc in screen coordinates.
    tx, ty = -math.sin(a), math.cos(a)
    nx, ny = math.cos(a), math.sin(a)
    tip = (ex + tx * head, ey + ty * head)
    left = (ex + nx * head * 0.75, ey + ny * head * 0.75)
    right = (ex - nx * head * 0.75, ey - ny * head * 0.75)
    d.polygon([(s(x), s(y)) for x, y in (tip, left, right)], fill=color)


def cartridge(layer, cx, top, height, width):
    """Upright rifle cartridge: rounded bullet tip on a brass case with a rim."""
    d = ImageDraw.Draw(layer)
    tip_h = height * 0.34
    case_top = top + tip_h
    case_bottom = top + height
    left, right = cx - width / 2, cx + width / 2
    # Bullet tip: a half ellipse over a short straight section.
    d.ellipse((s(left), s(top), s(right), s(top + tip_h * 2)), fill=TIP)
    d.rectangle((s(left), s(top + tip_h), s(right), s(case_top + 4)), fill=TIP)
    # Case and rim.
    d.rounded_rectangle((s(left - 3), s(case_top), s(right + 3), s(case_bottom - 12)), radius=s(5), fill=BRASS)
    d.rectangle((s(left + 6), s(case_top + 10), s(left + 14), s(case_bottom - 24)), fill=(250, 220, 140))
    d.rounded_rectangle((s(left - 8), s(case_bottom - 16), s(right + 8), s(case_bottom)), radius=s(4), fill=BRASS_DARK)
    d.line([(s(left - 3), s(case_top)), (s(right + 3), s(case_top))], fill=BRASS_DARK, width=s(5))


def main():
    img = background()

    cube_layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    cube(ImageDraw.Draw(cube_layer), 200, 286, 140)
    shadow = cube_layer.split()[3].filter(ImageFilter.GaussianBlur(s(10)))
    img.paste((0, 0, 0, 110), (s(6), s(14)), shadow)
    img.alpha_composite(cube_layer)

    overlay = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    # Reload arrow sweeping over the top from the cube towards the cartridge.
    reload_arrow(overlay, 300, 250, 168, 200, 330, 30, 50, WHITE)
    cartridge(overlay, 382, 236, 214, 58)
    overlay_shadow = overlay.split()[3].filter(ImageFilter.GaussianBlur(s(6)))
    img.paste((0, 0, 0, 120), (s(4), s(8)), overlay_shadow)
    img.alpha_composite(overlay)

    mod_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # Resources/ ships inside the mod (PluginSettings.ini), so only the 128 px icon goes there.
    resources_dir = os.path.join(mod_dir, "Resources")
    branding_dir = os.path.join(mod_dir, "Branding")
    os.makedirs(resources_dir, exist_ok=True)
    os.makedirs(branding_dir, exist_ok=True)
    big = img.resize((BASE, BASE), Image.LANCZOS)
    big.save(os.path.join(branding_dir, "Icon512.png"))
    big.resize((128, 128), Image.LANCZOS).save(os.path.join(resources_dir, "Icon128.png"))
    print("wrote", resources_dir, "and", branding_dir)


main()
