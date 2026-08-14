# Themes

Themes shipped with this fork. Each subdirectory is a self-contained theme: one
`.json` file plus the images it references.

| Theme | |
|---|---|
| [`doge/`](./doge) | Shiba Inu meme — doge gold on near-black, breathing gold LED |

This is separate from [`sd_files/themes/`](../sd_files/themes), which holds upstream
Bruce's theme documentation, the Theme Builder page and the example packs. The format
is identical — anything here works with stock Bruce, and anything from upstream works
here.

## Installing

Copy the theme's folder to your SD card or to LittleFS (via the WebUI), then on the
device: **Config → UI Theme → (choose FS) → select the `.json`**.

Image paths inside a theme `.json` resolve relative to the `.json` itself, so keep each
theme's folder intact rather than flattening the files together.

## Format notes

Worth knowing if you author one, since the upstream README doesn't spell these out:

- **UI colors are RGB565** (`priColor`, `secColor`, `bgColor`) written as a bare hex
  string with no `0x`. **LED color is plain 24-bit RGB hex.** They are different
  encodings in the same file — the LED value is not RGB565.
- **Every image key is optional.** The loader checks each file exists and silently skips
  the ones that don't (`log_w("THEME: file not found")`), so a colors-only theme is
  valid. A theme with no images at all still works.
- `label: 1` draws Bruce's own text label under each menu icon — use icons ~50px
  shorter in that case. Themes whose icons already contain their own captions should
  set `label: 0` to avoid drawing the name twice.
- `ledEffect`: `0` solid, `1` breathe, `2` color cycle, `3` color wheel, `4` chase,
  `5` chase tail, `6` rainbow chase, `7` rainbow breathe, `8` disco, `9` fire.
- Recommended icon height is 140px on the T-Embed (320x170), 105px on Cardputer and
  StickCPlus (240x135), 180px on Core and CYD (320x240). Bigger images just draw slower.

To convert a hex color to RGB565:

```python
r, g, b = 0xF2, 0xB7, 0x05
print(f"{((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3):04x}")   # f5a0
```

Upstream's [Theme Builder](https://bruce.computer/build_theme.html) will also generate
a ready-to-unzip pack for you.
