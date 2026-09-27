# texbake

Pre-compresses the game's textures on a PC so the Vita never has to.

The port compresses every texture to DXT the first time it sees it and keeps the
result in `ux0:data/JAVITA/texcache_dxt`. That first sight is the slow one. This
tool produces the same cache on a PC, as one file, so you can copy it to the card
and skip the wait entirely.

## Use it

1. Plug the Vita's card in if you have it, then double-click `TEXBAKE.bat`.
2. Read the status block: where it found the game, whether the card is there and
   what is on it, and whether you have a local `pack.bin` from a previous run.
   If it could not find the game, drag the folder that holds `assets0.pk3` into
   the window and press Enter.
3. Press Enter. With the card in, that builds the cache, merges in whatever the
   Vita compressed on its own, copies `pack.bin` over and checks the copy. Without
   the card, it just builds, and tells you where `pack.bin` is and where it goes:
   `ux0:data/JAVITA/texcache_dxt/pack.bin`. That one file is the whole cache.
4. Wait. It prints a progress line and finishes in a few minutes.

The first build needs Visual Studio with "Desktop development with C++". After
that, `texbake.exe` sits next to the batch file and runs on its own.

## What the device adds

Anything the game compresses on the Vita that was not in `pack.bin` is appended to
`pack.delta` in the same folder. The game reads it on the next start, so nothing is
compressed twice. Every build with the card plugged in folds it in: the merged pack
takes `pack.delta` first, then the fresh build, then the card's old `pack.bin`, then
any loose `.bin` entries in the folder, and `pack.delta` is deleted only
after the copy on the card has been read back and verified. To fold without baking,
pick option 3 in the menu, or:

    texbake.exe --pack <card>:\data\JAVITA\texcache_dxt

The game writes only `pack.delta`; loose `.bin` files from older caches are still read and folded in the same way.

## Settings and the game

Every entry records the `r_picmip` it was built at. The game uses an entry built at
the same or a lower `r_picmip` and skips the levels it would have halved away, so a
pack built at the default `1` also serves a Vita running `r_picmip 2`. An entry built
at a higher `r_picmip` than the game runs is rejected and re-compressed on the
device. To build for a coarser setting only, pass the number:

    texbake.exe "...\GameData\base" --picmip 2

A changed `--picmip` or `--fast` re-bakes the whole pack.

## Looking inside

Entries are keyed by the texture's name as the game files it: lowercase, forward
slashes, no extension. `--list` prints every entry of a `pack.bin` or `pack.delta`
with its key, size, mip count, picmip and flags:

    texbake.exe --list texcache_out\texcache_dxt\pack.bin

A hash-keyed `pack.bin` (version 1) or hash-keyed `pack.delta` records are read too:
`--list` shows those entries as `#<hash>`, and a fold names them from the game's
files, leaving out any hash no known name matches.

## Options

| Option | What it does |
|---|---|
| `-o <folder>` | Where to write `texcache_dxt\pack.bin`. Default `texcache_out`. |
| `--picmip <n>` | The `r_picmip` the Vita runs. Default 1. |
| `--fast` | Quicker encode, slightly worse blocks. Default is the better one. |
| `--force` | Re-encode entries that are already in `pack.bin`. |
| `--threads <n>` | Worker threads. Default is one per core. |
| `--copy <path>` | Put `pack.bin` on the card at that root when finished, no questions; the card's `pack.delta` is merged in first. |
| `--no-copy` | Never offer to copy. |
| `--pack <dir>` | Fold `pack.delta`, the old `pack.bin` and any loose entries in `<dir>` into a fresh `pack.bin`, no baking. |
| `--list <file>` | Print every entry of a `pack.bin` or `pack.delta`: key, size, mips, picmip, flags. |

A second run into the same output folder only encodes what its `pack.bin` is
missing and carries the rest over.

## What it does and does not cover

It bakes every texture the game can reach by name, and reads the shader files to
learn which ones are loaded without mipmaps or without picmip, because those
details are recorded in each entry.

Some textures are never cached, by the game's own rules, and the tool skips them
to match: anything a shader marks `notc`, which includes most skyboxes and the
main menu background, and anything whose size is not a power of two.

Images the modules register without mipmaps under a name built at runtime cannot be predicted; the device compresses those once into `pack.delta`.

The encoder is the game's own, so the result is what the Vita would have produced,
except that the tool uses the higher-quality setting a PC can afford.
