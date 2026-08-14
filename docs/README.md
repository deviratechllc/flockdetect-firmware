# docs/ — the published landing page

Served by GitHub Pages at **https://deviratechllc.github.io/flockdetect-firmware/**
(Settings → Pages → deploy from branch `main`, folder `/docs`). This file isn't part
of the site; Pages serves `index.html`.

## What's here and why

| | |
|---|---|
| `index.html` | the page |
| `manifest-v3.json` | ESP Web Tools manifest for the current build |
| `Bruce-lilygo-t-embed-cc1101-v3.bin` | merged image — flashed by the browser, so it **must** be hosted here |
| `FlockDetect-app-only-firmware-v3.bin` | app-only image |
| `esp-web-tools/` | esp-web-tools 10.4.0, self-hosted (it lazy-loads hashed sibling chunks, so the whole `dist/web/` is mirrored) |
| `img/`, `FlockDetect-doge-theme.zip` | screenshots, theme preview, theme pack |
| `SHA256SUMS.txt` | checksums for the three downloads above |
| `.nojekyll` | skip Jekyll processing |

**Only the current version's binaries live here.** Older builds stay on
[Releases](https://github.com/deviratechllc/flockdetect-firmware/releases), which the
page links to. Git history is permanent, so every binary committed here costs ~8.5 MB
forever — when cutting a new version, replace the v-N binaries rather than adding
alongside them.

## Why the binaries can't just live on Releases

GitHub release assets are served **without** an `access-control-allow-origin` header.
ESP Web Tools pulls the image with `fetch()`, so a manifest pointing at a release asset
is blocked by CORS and browser flashing breaks. Plain download links to Releases are
fine — the browser navigates rather than fetching, so CORS never applies. That's why
the page flashes from here but links elsewhere for older versions.

## Relationship to `~/fd-share`

The VM also serves a copy at `devira-bruce.exe.xyz` via
`python3 -m http.server 8000 --directory ~/fd-share`. The two are **separate copies and
will drift** — `fd-share` additionally holds the v1/v2 binaries and their manifests,
and its page still offers a "Flash v2 instead" button.

To refresh the VM copy from what's published:

```sh
cp -r ~/firmware/docs/. ~/fd-share/
```

## Publishing a new version

1. Build, then copy the new merged and app-only images in, deleting the previous ones.
2. Update `manifest-vN.json` and the version references in `index.html`.
3. Regenerate checksums: `sha256sum *.bin *.zip > SHA256SUMS.txt`
4. Commit and push — Pages redeploys automatically, usually within a minute.
