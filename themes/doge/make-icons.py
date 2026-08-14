#!/usr/bin/env python3
"""Regenerate the Doge theme artwork.

`doge-source.png` is the master: a transparent-background Shiba illustration in
thug-life shades, supplied by the theme author. Every icon, the boot splash and
the preview strip are composed from it, so replace that file rather than editing
the generated PNGs individually.

Needs ImageMagick on PATH. Run from this directory:  python3 make-icons.py
"""

import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "doge-source.png"

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
FONT = "Bookman-Demi"  # rounded and friendly; no Comic Sans on this box

ICON_W, ICON_H = 160, 140  # sized for the T-Embed's 320x170 panel
FACE_H = 98                # leaves room for the caption underneath


def run(args):
    subprocess.run([str(a) for a in args], check=True)


def optimise(path: Path):
    """Palette-reduce. The art is flat-shaded, so 128 colours is indistinguishable
    by eye and cuts each icon to a couple of KB."""
    run(["convert", path, "-strip", "-depth", "8", "-colors", "128", f"PNG8:{path}"])


def main():
    if not SOURCE.exists():
        raise SystemExit(f"missing master artwork: {SOURCE}")

    for key, caption in ICONS.items():
        out = HERE / f"{key}.png"
        run([
            "convert", "-size", f"{ICON_W}x{ICON_H}", "xc:none",
            "(", SOURCE, "-resize", f"x{FACE_H}", ")",
            "-gravity", "north", "-geometry", "+0+2", "-composite",
            "-font", FONT, "-pointsize", 19, "-fill", GOLD,
            "-gravity", "south", "-annotate", "+0+8", caption,
            out,
        ])
        optimise(out)

    boot = HERE / "boot.png"
    run([
        "convert", "-size", "300x170", f"xc:{BG}",
        "(", SOURCE, "-resize", "x150", ")",
        "-gravity", "west", "-geometry", "+14+0", "-composite",
        "-font", FONT, "-pointsize", 27, "-fill", GOLD,
        "-gravity", "east", "-annotate", "+20-34", "such wow",
        "-font", FONT, "-pointsize", 23, "-fill", CREAM,
        "-gravity", "east", "-annotate", "+20+0", "much scan",
        "-font", FONT, "-pointsize", 23, "-fill", BRONZE,
        "-gravity", "east", "-annotate", "+20+34", "very flock",
        boot,
    ])
    optimise(boot)

    keys = list(ICONS)
    strips = []
    for i, row in enumerate((keys[:8], keys[8:])):
        strip = HERE / f"_row{i}.png"
        run(["montage", *[HERE / f"{k}.png" for k in row],
             "-tile", f"{len(row)}x1", "-geometry", "+5+5", "-background", BG, strip])
        strips.append(strip)
    preview = HERE / "theme-preview.png"
    run(["montage", *strips, "-tile", "1x2", "-geometry", "+0+0",
         "-background", BG, preview])
    for s in strips:
        s.unlink()
    optimise(preview)
    print(f"regenerated {len(ICONS)} icons, boot.png and theme-preview.png")


if __name__ == "__main__":
    main()
