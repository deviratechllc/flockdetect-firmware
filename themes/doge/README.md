# Doge

![Doge theme icons](./theme-preview.png)

Shiba Inu meme theme in pixel art — the dog in thug-life shades, three-quarter
pose with the muzzle low and left. Doge gold on near-black, and the labels are in
doge-speak: `much wifi`, `such files`, `where am`, `very long`.

## Install

Copy this whole folder to your SD card or LittleFS, then on the device:
**Config → UI Theme → (choose FS) → `doge.json`**

Image paths resolve relative to the `.json`, so keep the folder together.

## Palette

| | Hex | Stored as | Where it shows |
|---|---|---|---|
| Primary | `#F2B705` | `f5a0` (RGB565) | titles, selection, borders |
| Secondary | `#8C6A28` | `8b45` (RGB565) | unselected submenu items |
| Background | `#120C04` | `1060` (RGB565) | everything behind |
| LED | `#F2B705` | `F2B705` (24-bit) | breathing, 70% brightness |

The LED value is plain 24-bit RGB — Bruce reads UI colors as RGB565 and the LED color
as ordinary hex out of the same file.

`label` is `0` because each icon already carries its own caption; leaving it at `1`
would draw the menu name a second time underneath.

## Contents

`doge.json`, sixteen 160x140 menu icons, and `boot.png` (300x170) as the boot splash.
`doge-pixel.png` is the rendered dog the icons are composed from, and
`theme-preview.png` is the strip at the top of this file — neither is referenced by
the theme itself.

## Regenerating

**`make-icons.py` holds the real source**: a hand-authored 32x28 pixel grid, plus the
caption for each menu key. Edit the grid, not the PNGs.

```sh
cd themes/doge && python3 make-icons.py
```

That rewrites every icon, the boot splash and the preview strip. It needs ImageMagick
on `PATH` and nothing else — no Python packages.

Two things it is careful about, worth preserving if you change it:

- **Nearest-neighbour scaling only** (`-filter point`). Any smooth filter turns pixel
  art into mush, and ImageMagick's default is smooth.
- The 1x master is written as **PAM, not PPM** — PPM has no alpha channel, and the
  icons need transparency around the dog.
- Scale factors are **whole numbers** (3x for icons, 4x for the splash). A fractional
  resize drops pixel rows unevenly and the grid stops looking deliberate.

The preview strip above squeezes sixteen icons into 1360px, so it undersells them —
on a 320x170 panel each icon draws at 160x140 and the ears and lens glints are crisp.

Captions use `Courier-Bold`; its blockiness suits the pixel art. Comic Sans would be
more on-brand for the meme, and dropping the TTF in and changing `FONT` is all it takes.

## Licensing

The artwork is a hand-authored pixel grid — nothing traced, photographed or imported —
so there is no third-party image licensing attached to this theme. If you replace the
art, be careful with stock-photo previews and meme images found online: most are
someone's copyrighted work, and this repository is public and AGPL-3.0, which
redistributes whatever is committed to it.

"Doge" refers to the 2013 meme; this theme is a homage and carries no affiliation with
anyone.
