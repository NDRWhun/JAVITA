# JAVITA 0.9 - ALPHA

**Star Wars Jedi Knight: Jedi Academy — single-player, on the PS Vita**

First public alpha. The game is completable, but this is an alpha: expect rough edges and
please open an issue if you hit one.

## Install

You need your own legally-owned copy of Jedi Academy (eg. from Steam).

1. Install `JAVITA.vpk` with VitaShell.
2. Copy `assets0.pk3`, `assets1.pk3`, `assets2.pk3`, `assets3.pk3` from your PC copy's
   `GameData/base/` to `ux0:data/JAVITA/base/`.
3. Launch from the LiveArea.

No `libshacccg.suprx` needed — the shaders are compiled ahead of time and built into the binary.

**The first visit to each level is slow.** Textures are re-compressed to DXT once and cached to
`ux0:data/JAVITA/texcache_dxt/`. Later loads of that level read the cache and are much quicker.

## What's in it

- **Native sceGxm renderer.** The renderer talks to the Vita's graphics API directly — there is no
  OpenGL translation layer in the build.
- **Threaded.** Rendering, sound mixing and character skinning run off the main thread.
- **Static world geometry in GPU buffers**, so level surfaces don't get re-uploaded every frame.
- **Static prop batching**, so the many copies of one tree or rock a map places share a draw
  instead of taking one each.
- **DXT texture cache** on the memory card, which is what makes the second visit to a level fast.
- **Vita controls** — dual-stick, rear touch panel with a combo layer, and an on-screen keyboard
  console on **Start + Select**.
- **Aim assist**, on by default. It gently steers your view toward a nearby enemy and slows your
  look speed so the sticks overshoot less. `g_aimAssist 0` turns it off.

## Experimental

Off by default. Turn them on from the console or `openjk_sp.cfg`, and expect artefacts.

| Setting | What it does |
|---------|--------------|
| `cg_shadows 2` | Stencil shadow volumes. Sabers and blaster bolts move the shadow. |
| `cg_shadows 3` | Projected shadows. Experimental |
| `r_shadowAlpha` | How dark a stencil shadow lands (`0.22` default). |
| `r_shadowExtrude` | How far a shadow reaches past the ground (`96`). Raise it to catch walls — too far and shadows show through geometry. |
| `cg_shadowCasterRange` | Characters past this distance keep a blob instead (`1024`). |

## Known issues

- Stencil shadows land on the ground and only reach walls as far as `r_shadowExtrude` allows.
  There is no distance fade, so a distant character's shadow pops out rather than fading.
- Frame rate varies by map. Open, prop-heavy areas run below 60.
- Loading a level for the first time can stall noticeably while textures are cached.

