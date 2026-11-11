# SPDX-License-Identifier: GPL-3.0-only
"""Renders the source DLL's built-in image (PHASE2 §4.6.4) and writes placeholder_builtin.inc.

Run on the Mac with the app venv (PyObjC: Quartz + CoreText), from the repository root:
    mac_app/.venv/bin/python native/audio/vcam-win/tools/make_builtin.py [--png out.png]
The image is 1920x1080: Beamer's dark ground, the Beamer mark (Beamer.svg drawn as paths), the title
"Beamer Camera" and the line "Waiting for Beamer" in Hanken Grotesk (OFL, win_app/assets). The DLL decodes
the PNG with WIC, scales it to the app's size and draws a moving dot at kDotY (the dot is not in the PNG).
"""
import argparse
import os
import sys

import Quartz
import CoreText
from Foundation import NSURL

W, H = 1920, 1080
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
FONT = os.path.join(ROOT, "win_app", "assets", "HankenGrotesk-Variable.ttf")
OUT_INC = os.path.join(HERE, "..", "placeholder_builtin.inc")

GROUND = "#0e0f0d"   # tokens.PALETTE dark
INK = "#efeee6"
INK_2 = "#b3b2a7"


def rgb(hex_):
    return tuple(int(hex_[i:i + 2], 16) / 255.0 for i in (1, 3, 5))


def fill(ctx, hex_):
    r, g, b = rgb(hex_)
    Quartz.CGContextSetRGBFillColor(ctx, r, g, b, 1.0)


def rrect(ctx, x, y, w, h, r, hex_, scale, ox, oy):
    # SVG coordinates (y down, 64x64 viewBox) -> context (y up)
    rect = Quartz.CGRectMake(ox + x * scale, oy + (64 - y - h) * scale, w * scale, h * scale)
    path = Quartz.CGPathCreateWithRoundedRect(rect, r * scale, r * scale, None)
    Quartz.CGContextAddPath(ctx, path)
    fill(ctx, hex_)
    Quartz.CGContextFillPath(ctx)


def draw_mark(ctx, ox, oy, size):
    s = size / 64.0
    rrect(ctx, 1, 1, 62, 62, 14, "#161714", s, ox, oy)
    rrect(ctx, 8, 13, 20, 36, 2, "#3d3e37", s, ox, oy)
    rrect(ctx, 36, 13, 20, 36, 2, "#6e6f66", s, ox, oy)
    rrect(ctx, 29.5, 9, 5, 44, 1.2, "#5fd4f4", s, ox, oy)
    pts = [(42, 17), (42, 40.8), (36.4, 35.2), (32.2, 45), (28.7, 43.6), (32.9, 33.8), (25.2, 33.8)]

    def poly():
        Quartz.CGContextBeginPath(ctx)
        for i, (x, y) in enumerate(pts):
            px, py = ox + x * s, oy + (64 - y) * s
            (Quartz.CGContextMoveToPoint if i == 0 else Quartz.CGContextAddLineToPoint)(ctx, px, py)
        Quartz.CGContextClosePath(ctx)

    poly()
    r, g, b = rgb("#0e0f0d")
    Quartz.CGContextSetRGBStrokeColor(ctx, r, g, b, 1.0)
    Quartz.CGContextSetLineWidth(ctx, 5 * s)
    Quartz.CGContextSetLineJoin(ctx, Quartz.kCGLineJoinRound)
    Quartz.CGContextStrokePath(ctx)
    poly()
    fill(ctx, "#efeee6")
    Quartz.CGContextFillPath(ctx)


def font(size, weight):
    url = NSURL.fileURLWithPath_(FONT)
    descs = CoreText.CTFontManagerCreateFontDescriptorsFromURL(url)
    if not descs:
        sys.exit("font not found: " + FONT)
    f = CoreText.CTFontCreateWithFontDescriptor(descs[0], size, None)
    # variable font: weight axis 'wght' (tag 0x77676874)
    var = {0x77676874: weight}
    return CoreText.CTFontCreateCopyWithAttributes(
        f, size, None, CoreText.CTFontDescriptorCreateWithAttributes({CoreText.kCTFontVariationAttribute: var}))


def text(ctx, s, size, weight, hex_, cy):
    r, g, b = rgb(hex_)
    color = Quartz.CGColorCreateGenericRGB(r, g, b, 1.0)
    attrs = {CoreText.kCTFontAttributeName: font(size, weight), CoreText.kCTForegroundColorAttributeName: color}
    line = CoreText.CTLineCreateWithAttributedString(CoreText.CFAttributedStringCreate(None, s, attrs))
    Quartz.CGContextSetTextPosition(ctx, 0, 0)  # image bounds are relative to the current text position
    bounds = CoreText.CTLineGetImageBounds(line, ctx)
    width = CoreText.CTLineGetTypographicBounds(line, None, None, None)[0]
    Quartz.CGContextSetTextPosition(ctx, (W - width) / 2, H - cy - bounds.size.height / 2 - bounds.origin.y)
    CoreText.CTLineDraw(line, ctx)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", help="also write the PNG here (for review)")
    a = ap.parse_args()
    cs = Quartz.CGColorSpaceCreateWithName(Quartz.kCGColorSpaceSRGB)
    ctx = Quartz.CGBitmapContextCreate(None, W, H, 8, W * 4, cs, Quartz.kCGImageAlphaNoneSkipLast)
    Quartz.CGContextSetShouldAntialias(ctx, True)
    Quartz.CGContextSetShouldSmoothFonts(ctx, False)
    fill(ctx, GROUND)
    Quartz.CGContextFillRect(ctx, Quartz.CGRectMake(0, 0, W, H))
    mark = 168
    # layout (y from the top): mark centred at 400, title at 600, detail at 664; the DLL's dot runs at 800 (kDotY)
    draw_mark(ctx, (W - mark) / 2, H - 400 - mark / 2, mark)
    text(ctx, "Beamer Camera", 54, 600, INK, 600)
    text(ctx, "Waiting for Beamer", 34, 400, INK_2, 666)
    img = Quartz.CGBitmapContextCreateImage(ctx)
    data = Quartz.CFDataCreateMutable(None, 0)
    dest = Quartz.CGImageDestinationCreateWithData(data, "public.png", 1, None)
    Quartz.CGImageDestinationAddImage(dest, img, None)
    Quartz.CGImageDestinationFinalize(dest)
    png = bytes(data)
    if a.png:
        with open(a.png, "wb") as f:
            f.write(png)
    lines = ["// SPDX-License-Identifier: GPL-3.0-only",
             "// Generated by tools/make_builtin.py: the built-in image (PNG, 1920x1080, sRGB). Do not edit.",
             "// Text in Hanken Grotesk (SIL Open Font License 1.1, win_app/assets/HankenGrotesk-OFL.txt).",
             "constexpr unsigned kBuiltinPngSize = %d;" % len(png),
             "alignas(16) static const unsigned char kBuiltinPng[kBuiltinPngSize] = {"]
    for i in range(0, len(png), 24):
        lines.append("  " + ",".join("0x%02x" % b for b in png[i:i + 24]) + ",")
    lines.append("};")
    with open(OUT_INC, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s (%d PNG bytes)" % (os.path.normpath(OUT_INC), len(png)))


if __name__ == "__main__":
    main()
