#!/usr/bin/env python3
"""Regenerate the Doge theme artwork.

The pixel grid below is the source of truth -- every icon, the boot splash and
the preview strip are derived from it, so edit the grid rather than the PNGs.

Original artwork: hand-authored pixel grid, nothing traced or imported, so the
theme carries no third-party image licensing.

Needs ImageMagick on PATH. Run from this directory:  python3 make-icons.py
"""

import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent

# --- palette ---------------------------------------------------------------
PAL = {
    ".": (0x00, 0x00, 0x00, 0),  # transparent
    "D": (0xC9, 0x7F, 0x2E, 255),  # outline / dark fur
    "F": (0xE3, 0xA2, 0x4A, 255),  # mid fur
    "L": (0xF5, 0xC0, 0x69, 255),  # light fur
    "C": (0xF7, 0xDC, 0xAC, 255),  # cream muzzle
    "K": (0x17, 0x10, 0x0A, 255),  # black - shades, nose, mouth
    "W": (0xFF, 0xFF, 0xFF, 255),  # lens glint
}

# --- the dog ---------------------------------------------------------------
GRID = [
    "............................",
    "....DD................DD....",
    "...DFFD..............DFFD...",
    "...DFLFD............DFLFD...",
    "...DFLLFD..........DFLLFD...",
    "....DFLLFDDDDDDDDDDFLLFD....",
    ".....DFLLLLLLLLLLLLLLFD.....",
    "....DFLLLLLLLLLLLLLLLLFD....",
    "...DFLLLLLLLLLLLLLLLLLLFD...",
    "...DFLLLLLLLLLLLLLLLLLLFD...",
    "...DFLLLLLLLLLLLLLLLLLLFD...",
    "...DKKKKKKKKKKKKKKKKKKKKD...",
    "...DKKWWKKKKKKKKWWKKKKKKD...",
    "...DKKWWKKKKKKKKWWKKKKKKD...",
    "...DKKKKKKKKKKKKKKKKKKKKD...",
    "...DFCCCCCCCCCCCCCCCCCCFD...",
    "...DFCCCCCCCCCCCCCCCCCCFD...",
    "....DCCCCCCCKKKKCCCCCCCD....",
    "....DCCCCCCCKKKKCCCCCCCD....",
    ".....DCCCCCCCKKCCCCCCCD.....",
    ".....DCCCCKKCKKCKKCCCCD.....",
    "......DCCCCKKKKKKKCCCCD.....",
    "......DDCCCCCCCCCCCDD.......",
    ".......DDDDDDDDDDDDD........",
    "............................",
]

# menu key -> doge-speak caption
ICONS = {
    "wifi": "much wifi", "ble": "such blue", "ethernet": "such cable",
    "rf": "very radio", "rfid": "so badge", "fm": "many hertz",
    "ir": "much beam", "files": "such files", "gps": "where am",
    "nrf": "very 2.4", "interpreter": "much script", "clock": "such time",
    "others": "many wow", "connect": "so link", "config": "much tweak",
    "lora": "very long",
}

GOLD, CREAM, BRONZE, BG = "#F2B705", "#F7DCAC", "#C68A3E", "#120C04"
FONT = "Courier-Bold"  # blocky monospace suits pixel art; no Comic Sans here


def write_master() -> Path:
    """1x PAM master. PAM rather than PPM because we need the alpha channel."""
    grid = [r.ljust(max(map(len, GRID)), ".") for r in GRID]
    w, h = len(grid[0]), len(grid)
    px = bytearray()
    for row in grid:
        for ch in row:
            px += bytes(PAL[ch])
    hdr = (
        f"P7\nWIDTH {w}\nHEIGHT {h}\nDEPTH 4\nMAXVAL 255\n"
        f"TUPLTYPE RGB_ALPHA\nENDHDR\n"
    ).encode()
    p = HERE / "doge-pixel.pam"
    p.write_bytes(hdr + bytes(px))
    return p


def scaled(master: Path, factor: int, out: str) -> Path:
    """Nearest-neighbour only -- any smooth filter would blur the pixels."""
    p = HERE / out
    subprocess.run(
        ["convert", str(master), "-filter", "point", "-resize", f"{factor*100}%", str(p)],
        check=True,
    )
    return p


def optimise(path: Path) -> None:
    subprocess.run(
        ["convert", str(path), "-strip", "-depth", "8", "-colors", "32", f"PNG8:{path}"],
        check=True,
    )


def main() -> None:
    master = write_master()
    face = scaled(master, 4, "doge-pixel.png")  # 112x100, the icon-sized dog

    for key, caption in ICONS.items():
        out = HERE / f"{key}.png"
        subprocess.run([
            "convert", "-size", "160x140", "xc:none",
            str(face), "-gravity", "north", "-geometry", "+0+4", "-composite",
            "-font", FONT, "-pointsize", "17", "-fill", GOLD,
            "-gravity", "south", "-annotate", "+0+10", caption,
            str(out),
        ], check=True)
        optimise(out)

    boot_face = scaled(master, 5, "doge-boot-face.png")  # 140x125
    boot = HERE / "boot.png"
    subprocess.run([
        "convert", "-size", "300x170", f"xc:{BG}",
        str(boot_face), "-gravity", "west", "-geometry", "+10+0", "-composite",
        "-font", FONT, "-pointsize", "26", "-fill", GOLD,
        "-gravity", "east", "-annotate", "+16-34", "such wow",
        "-font", FONT, "-pointsize", "22", "-fill", CREAM,
        "-gravity", "east", "-annotate", "+16+0", "much scan",
        "-font", FONT, "-pointsize", "22", "-fill", BRONZE,
        "-gravity", "east", "-annotate", "+16+34", "very flock",
        str(boot),
    ], check=True)
    optimise(boot)
    (HERE / "doge-boot-face.png").unlink()

    keys = list(ICONS)
    rows = [keys[:8], keys[8:]]
    strips = []
    for i, row in enumerate(rows):
        strip = HERE / f"_row{i}.png"
        subprocess.run(
            ["montage", *[str(HERE / f"{k}.png") for k in row],
             "-tile", f"{len(row)}x1", "-geometry", "+5+5", "-background", BG, str(strip)],
            check=True,
        )
        strips.append(strip)
    preview = HERE / "theme-preview.png"
    subprocess.run(
        ["montage", *map(str, strips), "-tile", "1x2", "-geometry", "+0+0",
         "-background", BG, str(preview)],
        check=True,
    )
    for s in strips:
        s.unlink()
    optimise(preview)
    master.unlink()
    print(f"regenerated {len(ICONS)} icons, boot.png and theme-preview.png")


if __name__ == "__main__":
    main()
