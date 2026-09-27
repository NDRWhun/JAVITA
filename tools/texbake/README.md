# texbake

Pre-compresses the game's textures on a PC so the Vita never has to.

The port compresses every texture to DXT the first time it sees it and keeps the
result in `ux0:data/JAVITA/texcache_dxt`. That first sight is the slow one. This
tool produces those same files on a PC, so you can copy them to the card and skip
the wait entirely.

## Use it

1. Double-click `TEXBAKE.bat`.
2. Press Enter to let it find the game, or drag the folder that holds `assets0.pk3`
   into the window first.
3. Wait. It prints a progress line and finishes in a few minutes.
4. Copy the `texcache_dxt` folder it made into `ux0:data/JAVITA/` on the Vita.
   If the card is plugged in, the tool offers to copy for you.

The first build needs Visual Studio with "Desktop development with C++". After
that, `texbake.exe` sits next to the batch file and runs on its own.

## Settings that have to match the game

The cache records the `r_picmip` it was built with, and the game rejects entries
that disagree. The tool defaults to `1`, which is what the port ships. If you
changed `r_picmip` on the Vita, pass the same number:

    texbake.exe "...\GameData\base" --picmip 2

Changing the setting re-bakes everything, so the tool notices and does it for you.

## Options

| Option | What it does |
|---|---|
| `-o <folder>` | Where to write `texcache_dxt`. Default `texcache_out`. |
| `--picmip <n>` | The `r_picmip` the Vita runs. Default 1. |
| `--fast` | Quicker encode, slightly worse blocks. Default is the better one. |
| `--force` | Re-bake entries that already exist. |
| `--threads <n>` | Worker threads. Default is one per core. |
| `--copy <path>` | Copy to that card root when finished, no questions. |
| `--no-copy` | Never offer to copy. |
| `--pack <dir>` | Only build `pack.bin` from the entries already in `<dir>`, no baking. |

## pack.bin

Every run ends by writing `pack.bin` next to the entries: the same files, packed
behind one index, so the game opens one file per session instead of one per
texture. The loose files stay and are still read when an entry is not in the
pack, so a cache without a `pack.bin` keeps working as before.

To pack a cache you already have on the card, without baking anything:

    texbake.exe --pack F:\data\JAVITA\texcache_dxt

Entries the game bakes on the device after that are not in the pack until it is
rebuilt the same way.

## What it does and does not cover

It bakes every texture the game can reach by name, and reads the shader files to
learn which ones are loaded without mipmaps or without picmip, because those
details are recorded in each entry.

Some textures are never cached, by the game's own rules, and the tool skips them
to match: anything a shader marks `notc`, which includes most skyboxes and the
main menu background, and anything whose size is not a power of two.

A handful of images that the menus load may end up with the wrong flags, because
that is decided in code rather than in a shader file. Those get re-compressed once
on the device and are then correct forever. Nothing breaks either way.

The encoder is the game's own, so the result is what the Vita would have produced,
except that the tool uses the higher-quality setting a PC can afford.
