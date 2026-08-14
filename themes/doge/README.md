# Doge

![Doge theme icons](./theme-preview.png)

Shiba Inu meme theme — the dog in thug-life shades, three-quarter pose. Doge gold
on near-black, and the labels are in doge-speak: `much wifi`, `such files`,
`where am`, `very long`.

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
`doge-source.png` is the master artwork everything is composed from, and
`theme-preview.png` is the strip at the top of this file — neither is referenced by
the theme itself.

## Regenerating

`doge-source.png` is the source of truth — a transparent-background Shiba
illustration. Swap that file and rerun:

```sh
cd themes/doge && python3 make-icons.py
```

That rewrites every icon, the boot splash and the preview strip. It needs ImageMagick
on `PATH` and nothing else — no Python packages.

If you replace the master, keep a **real alpha channel**. A subject flattened onto
white looks like a sticker stuck on the menu, because the theme background is
`#120C04` rather than white, and there is no transparency left to recover.

Icons are palette-reduced to 128 colours on the way out; the art is flat-shaded, so
that's invisible by eye and takes each icon to a few KB.

Captions use `Bookman-Demi`. Comic Sans would be more on-brand for the meme — drop the
TTF anywhere on the box and change `FONT` in `make-icons.py`.

## Licensing

The master artwork was supplied by the theme author. If you fork this theme and swap
the art, be careful with stock-photo previews and marketplace product images: those
are someone's copyrighted work, and this repository is public and AGPL-3.0, which
redistributes whatever is committed to it. The underlying Kabosu photograph that most
doge images derive from is likewise copyrighted, however widely it has been memed.

"Doge" refers to the 2013 meme; this theme is a homage and carries no affiliation with
anyone.
