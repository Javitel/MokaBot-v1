#!/usr/bin/env python3
# Writes the boot logo into src/ui/ui_img_boot_logo.c (Logo_map and its descriptor)
# as an LVGL 2-bit indexed image: four grey levels, drawn directly by LVGL
# without an image decoder (LV_USE_PNG off saves ~16 KB of flash). The logo
# (a light-on-transparent SVG, e.g. a white wordmark) is rendered WIDTH px
# wide on black; the boot screen is black around it.
#
# Needs rsvg-convert (librsvg).
# Usage: python3 tools/make-boot-logo.py logo.svg [src/ui/ui_img_boot_logo.c]

import re
import struct
import subprocess
import sys
import zlib

WIDTH = 440
LEVELS = 4                     # 2 bits per pixel
PALETTE = [0x00, 0x55, 0xAA, 0xFF]


def render(svg_path):
    """Renders the SVG WIDTH px wide on black; returns (w, h, grey rows)."""
    svg = open(svg_path, encoding="utf-8").read()
    view_box = [float(v) for v in re.search(r'viewBox="([^"]+)"', svg).group(1).replace(",", " ").split()]
    height = round(WIDTH * view_box[3] / view_box[2])
    inner = svg[svg.index(">", svg.index("<svg")) + 1:svg.rindex("</svg>")]
    framed = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
        '<rect width="%d" height="%d" fill="#000"/>'
        '<svg width="%d" height="%d" viewBox="%s">%s</svg></svg>'
        % (WIDTH, height, WIDTH, height, WIDTH, height, WIDTH, height,
           " ".join(str(v) for v in view_box), inner))
    png = subprocess.run(["rsvg-convert", "-w", str(WIDTH), "-h", str(height)],
                         input=framed.encode(), capture_output=True, check=True).stdout
    return decode_png(png)


def decode_png(png):
    """Minimal decoder for rsvg-convert's output: 8-bit RGB/RGBA, not interlaced."""
    pos, idat, header = 8, b"", None
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        chunk = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    width, height, depth, color_type, _, _, interlace = header
    if depth != 8 or color_type not in (2, 6) or interlace:
        raise ValueError("unsupported PNG (depth %d, colour type %d)" % (depth, color_type))
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            up_left = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                line[x] = (line[x] + predictor) & 0xFF
        rows.append([sum(line[x * channels:x * channels + 3]) // 3 for x in range(width)])
        previous = line
    return width, height, rows


def to_indexed_2bit(width, rows):
    """LVGL LV_IMG_CF_INDEXED_2BIT: 4 palette colours (B, G, R, A), then rows
    of 2-bit indexes, first pixel in the high bits, each row padded to a byte."""
    data = bytearray()
    for grey in PALETTE:
        data += bytes((grey, grey, grey, 0xFF))
    for row in rows:
        line = bytearray((width * 2 + 7) // 8)
        for x, grey in enumerate(row):
            index = min(LEVELS - 1, (grey * (LEVELS - 1) + 127) // 255)
            line[x // 4] |= index << (6 - 2 * (x % 4))
        data += line
    return data


def main(svg_path, source="src/ui/ui_img_boot_logo.c"):
    width, height, rows = render(svg_path)
    data = to_indexed_2bit(width, rows)
    text = open(source, encoding="utf-8").read()
    head = "uint8_t Logo_map[] = {"
    start = text.index(head) + len(head)
    end = text.index("};", start)
    body = ",\n".join("    " + ", ".join("0x%02x" % v for v in data[k:k + 16]) for k in range(0, len(data), 16))
    text = text[:start] + "\n" + body + "\n" + text[end:]
    for field, value in (("header.cf", "LV_IMG_CF_INDEXED_2BIT"), ("header.w", str(width)),
                         ("header.h", str(height)), ("data_size", str(len(data)))):
        text, count = re.subn(r"(\.%s = )[^,]+," % re.escape(field), r"\g<1>%s," % value, text, count=1)
        if count != 1:
            raise ValueError("descriptor field %s not found" % field)
    open(source, "w", encoding="utf-8").write(text)
    print("Logo_map: %dx%d, 2-bit indexed, %d bytes" % (width, height, len(data)))


if __name__ == "__main__":
    main(*sys.argv[1:])
