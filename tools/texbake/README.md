# texbake

Pre-compresses the game's textures on a PC so the Vita never has to.

The port compresses every texture to DXT the first time it sees it and keeps the
result in `ux0:data/JAVITA/texcache_dxt`. That first sight is the slow one. This
tool produces the same cache on a PC, as one file, so you can copy it to the card
and skip the wait entirely.

## Use it

1. Double-click `TEXBAKE.bat`.
2. Press Enter to let it find the game, or drag the folder that holds `assets0.pk3`
   into the window first.
3. Wait. It prints a progress line and finishes in a few minutes.
4. Copy `texcache_out\texcache_dxt\pack.bin` to `ux0:data/JAVITA/texcache_dxt/` on the Vita.
   That one file is the whole cache. If the card is plugged in, the tool offers to copy it for you.

The first build needs Visual Studio with "Desktop development with C++". After
that, `texbake.exe` sits next to the batch file and runs on its own.

## What the device adds

Anything the game compresses on the Vita that was not in `pack.bin` is appended to
`pack.delta` in the same folder. The game reads it on the next start, so nothing is
compressed twice. Now and then, with the card plugged in, fold it into the pack:

    texbake.exe --pack F:\data\JAVITA\texcache_dxt

That rewrites `pack.bin` from `pack.delta`, the old `pack.bin` and any loose `.bin`
entries an older build left behind, in that order of precedence, and deletes
`pack.delta` once the new pack is in place. Nothing writes loose files any more;
the ones already on a card still work and are folded in the same way.

## Settings that have to match the game

The cache records the `r_picmip` it was built with, and the game rejects entries
that disagree. The tool defaults to `1`, which is what the port ships. If you
changed `r_picmip` on the Vita, pass the same number:

    texbake.exe "...\GameData\base" --picmip 2

Changing the setting re-bakes everything, so the tool notices and does it for you.

## Options

| Option | What it does |
|---|---|
| `-o <folder>` | Where to write `texcache_dxt\pack.bin`. Default `texcache_out`. |
| `--picmip <n>` | The `r_picmip` the Vita runs. Default 1. |
| `--fast` | Quicker encode, slightly worse blocks. Default is the better one. |
| `--force` | Re-encode entries that are already in `pack.bin`. |
| `--threads <n>` | Worker threads. Default is one per core. |
| `--copy <path>` | Copy `pack.bin` to that card root when finished, no questions. |
| `--no-copy` | Never offer to copy. |
| `--pack <dir>` | Fold `pack.delta`, the old `pack.bin` and any loose entries in `<dir>` into a fresh `pack.bin`, no baking. |

A second run into the same output folder only encodes what its `pack.bin` is
missing and carries the rest over.

## What it does and does not cover

It bakes every texture the game can reach by name, and reads the shader files to
learn which ones are loaded without mipmaps or without picmip, because those
details are recorded in each entry.

Some textures are never cached, by the game's own rules, and the tool skips them
to match: anything a shader marks `notc`, which includes most skyboxes and the
main menu background, and anything whose size is not a power of two.

A handful of images that the menus load may end up with the wrong flags, because
that is decided in code rather than in a shader file. Those get re-compressed once
on the device, land in `pack.delta`, and are then correct forever. Nothing breaks
either way.

The encoder is the game's own, so the result is what the Vita would have produced,
except that the tool uses the higher-quality setting a PC can afford.
