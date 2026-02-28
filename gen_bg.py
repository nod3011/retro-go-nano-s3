import math
from PIL import Image, ImageDraw

def rotate(c, cx, cy, th):
    rad = math.radians(th)
    x = c[0] - cx
    y = c[1] - cy
    nx = x * math.cos(rad) - y * math.sin(rad)
    ny = x * math.sin(rad) + y * math.cos(rad)
    return (nx + cx, ny + cy)

def rrect(draw, center, w, h, angle, rad, color):
    cx, cy = center
    poly = []
    for x, y in [(-1, -1), (1, -1), (1, 1), (-1, 1)]:
        for a in range(0, 91, 15):
            angle_deg = (0 if x>0 and y>0 else 90 if x<0 and y>0 else 180 if x<0 and y<0 else 270) + a
            px = cx + (w//2 - rad) * x + rad * math.cos(math.radians(angle_deg))
            py = cy + (h//2 - rad) * y + rad * math.sin(math.radians(angle_deg))
            poly.append(rotate((px,py), cx, cy, angle))
    draw.polygon(poly, fill=color)

def create_ws(filename):
    bg_color = (48, 142, 230)
    fg_color = (25, 114, 187)
    img = Image.new('RGB', (320, 240), color=bg_color)
    draw = ImageDraw.Draw(img)

    cx, cy = 160, 120
    # Tilt the device (WS)
    angle = -35

    # Move device down
    dx, dy = cx, cy + 90

    # Body
    rrect(draw, (dx, dy), 500, 250, angle, 40, fg_color)

    # Screen bezel
    sw, sh = 200, 150
    rrect(draw, (dx-50, dy-10), sw, sh, angle, 10, bg_color)

    # Power button cutout
    rrect(draw, (dx+100, dy-130), 40, 40, angle, 20, bg_color)

    # Sound/Start buttons (bottom screen edge)
    rrect(draw, (dx-50, dy+90), 30, 15, angle, 5, bg_color)
    rrect(draw, (dx-10, dy+90), 30, 15, angle, 5, bg_color)

    # X Y A B Action Buttons (Right side)
    bx, by = dx + 100, dy + 10
    draw.ellipse([rotate((bx-15, by-15), dx, dy, angle), rotate((bx+15, by+15), dx, dy, angle)], fill=bg_color)
    bx, by = dx + 150, dy - 20
    draw.ellipse([rotate((bx-15, by-15), dx, dy, angle), rotate((bx+15, by+15), dx, dy, angle)], fill=bg_color)
    bx, by = dx + 140, dy + 80
    draw.ellipse([rotate((bx-15, by-15), dx, dy, angle), rotate((bx+15, by+15), dx, dy, angle)], fill=bg_color)
    bx, by = dx + 180, dy + 60
    draw.ellipse([rotate((bx-15, by-15), dx, dy, angle), rotate((bx+15, by+15), dx, dy, angle)], fill=bg_color)

    # D-pad (Y-pad on WS left)
    px, py = dx - 200, dy - 30
    draw.ellipse([rotate((px-30, py-30), dx, dy, angle), rotate((px+30, py+30), dx, dy, angle)], fill=bg_color)
    px, py = dx - 200, dy + 60
    rrect(draw, (px, py-25), 25, 25, angle, 5, bg_color)
    rrect(draw, (px, py+25), 25, 25, angle, 5, bg_color)
    rrect(draw, (px-25, py), 25, 25, angle, 5, bg_color)
    rrect(draw, (px+25, py), 25, 25, angle, 5, bg_color)

    img = img.quantize(colors=16)
    img.save(filename, format='PNG')

def create_ngp(filename):
    bg_color = (68, 178, 116)
    fg_color = (48, 148, 92)
    img = Image.new('RGB', (320, 240), color=bg_color)
    draw = ImageDraw.Draw(img)

    cx, cy = 160, 120
    angle = -20
    dx, dy = cx + 80, cy + 120

    # Body
    rrect(draw, (dx, dy), 420, 300, angle, 50, fg_color)

    # Screen bezel
    rrect(draw, (dx-20, dy-20), 200, 180, angle, 20, bg_color)

    # Option button
    draw.ellipse([rotate((dx+120, dy-120), dx, dy, angle), rotate((dx+140, dy-100), dx, dy, angle)], fill=bg_color)

    # Joystick base
    draw.ellipse([rotate((dx-180, dy), dx, dy, angle), rotate((dx-90, dy+90), dx, dy, angle)], fill=bg_color)
    draw.ellipse([rotate((dx-160, dy+20), dx, dy, angle), rotate((dx-110, dy+70), dx, dy, angle)], fill=fg_color)

    # Buttons
    bx, by = dx + 120, dy + 50
    draw.ellipse([rotate((bx-20, by-20), dx, dy, angle), rotate((bx+20, by+20), dx, dy, angle)], fill=bg_color)
    bx, by = dx + 180, dy - 10
    draw.ellipse([rotate((bx-20, by-20), dx, dy, angle), rotate((bx+20, by+20), dx, dy, angle)], fill=bg_color)

    # power switch
    rrect(draw, (dx-180, dy-100), 50, 20, angle, 10, bg_color)

    img = img.quantize(colors=16)
    img.save(filename, format='PNG')

create_ws('themes/default/background_ws.png')
create_ws('themes/classic/background_ws.png')
create_ngp('themes/default/background_ngp.png')
create_ngp('themes/classic/background_ngp.png')
