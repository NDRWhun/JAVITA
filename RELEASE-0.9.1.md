# JAVITA 0.9.1 - ALPHA

Bug-fix release. Same install as 0.9 — if you already have it, just install the new VPK
over the top; your settings and texture cache are kept.

## Fixed

- **Crash after the LucasArts logo.** The game could die on the transition from the boot
  logo to the main menu, either when the logo finished or when you pressed a button to
  skip it. The menu's background video was being decoded on the wrong thread, which could
  corrupt memory while the game was still loading.
- **Broken map geometry.** Props placed with a size other than their default — rocks,
  crates, scenery — were drawn at the wrong size and in the wrong place, so you could walk
  straight through them. Most visible on Tatooine and Yavin.
- **White flash between screens.** A brief white frame could appear while the game moved
  from one screen to another.

## Known issues

- The boot screen is black until the menu appears.
- Stencil shadows (`cg_shadows 2`) still only land on the ground and have no distance
  fade. Blob shadows remain the default.
- Frame rate varies by map; open, prop-heavy areas run below 60.
- The first visit to each level is slow while textures are cached.

## Install

You need your own legally-owned copy of Jedi Academy.

1. Install `JAVITA.vpk` with VitaShell.
2. Copy `assets0.pk3`, `assets1.pk3`, `assets2.pk3`, `assets3.pk3` from your PC copy's
   `GameData/base/` to `ux0:data/JAVITA/base/`.
3. Launch from the LiveArea.

Full settings list is in the [README](README.md).
