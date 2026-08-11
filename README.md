# JAVITA

**Star Wars Jedi Knight: Jedi Academy — single-player, on the PS Vita**

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](LICENSE)

A port of Jedi Academy's single-player to the PS Vita, built on
[OpenJK](https://github.com/JACoders/OpenJK) with a renderer backend written directly against
the Vita's GXM graphics API — no OpenGL translation layer in between.

Please report bugs if you find any -> Issues

## Setup (for players)

You need your own legally-owned copy of Jedi Academy (eg.: from Steam)

- Install `JAVITA.vpk` (from [Releases](../../releases)) with VitaShell.
- Copy your JKA `base/` PK3s — `assets0.pk3`, `assets1.pk3`, `assets2.pk3`, `assets3.pk3` — to
  `ux0:data/JAVITA/base/`.
- Launch from the LiveArea. Settings live in `ux0:data/JAVITA/base/openjk_sp.cfg`.

No `libshacccg.suprx` needed — the GXM shaders are compiled ahead of time and built into the
binary, so there is no runtime shader compiler to install.

**First run is slow.** Textures are re-compressed to DXT once and cached under
`ux0:data/JAVITA/texcache_dxt/`; the first time you load a given area it will stall while that
happens. Later loads read the cache and are much faster.

## Controls

### Sticks

| Input | Action |
|-------|--------|
| Left stick | Move (forward/back/strafe) — also the menu cursor |
| Right stick | Look / turn |
| Front touchscreen | In menus acting like a pointer moving the cursor |

In menus, Cross selects/clicks and Circle goes back/cancels. That face-button remap is in `cl_keys.cpp`. Navigate menus with the left stick plus those two buttons.

### Base layer (physical buttons)

| Button | Action |
|--------|--------|
| R | Attack / saber swing (`+attack`) |
| L | Alt-attack / saber special (`+altattack`) |
| ✕ Cross | Jump (`+moveup`) |
| ◻ Square | Crouch (`+movedown`) |
| ○ Circle | Use / activate (`+use`) |
| △ Triangle | Use selected force power (`+useforce`) |
| D-pad Up / Down | Next / previous weapon |
| D-pad Left / Right | Select previous / next force power |
| Select | Mission objectives — datapad (`datapad`) |
| Start | In-game menu (`togglemenu`) |

### Rear touch panel

The rear panel is split into four corner zones. A cross-shaped dead band down the middle ignores the fingers that grip the console. Set `vita_rearTouch 0` to disable all of it.

| Rear zone | Action |
|-----------|--------|
| Top-left (HOLD) | Combo modifier — see below |
| Top-right | Binocular zoom (`zoom`) |
| Bottom-left | Secondary force fire (`+useforce`) |
| Bottom-right | Run / walk (`+speed`) |

### Combo layer — hold rear top-left, then press:

| Combo | Action |
|-------|--------|
| + △ Triangle | Force Speed |
| + ○ Circle | Force Heal |
| + ✕ Cross | Force Push (`force_throw`) |
| + ◻ Square | Force Pull |
| + R | Cycle saber stance (`saberAttackCycle`) |
| + D-pad Up / Down | Inventory next / previous |
| + D-pad Left | Use inventory item (`invuse`) |
| + D-pad Right | Quick-select lightsaber (`weapon 1`) |

The combo layer only fires instant commands. The modifier role is latched per button at the moment it is pressed, so releasing the rear modifier mid-press can't strand a held action. The combo layer is inactive while a menu is open.

### Console

Open with **Start + Select** — the on-screen keyboard pops up. Type a command, press **Enter** to run it. Close with **Circle** or **Start + Select** again.

## Build (for developers)

Needs [VitaSDK](https://vitasdk.org) and [vdpm](https://github.com/vitasdk/vdpm) on `PATH`, plus
cmake and ninja. **On Windows, run from Git Bash.** SDL2 comes in as a git submodule — a
[fork](https://github.com/NDRWhun/SDL/tree/jk2vita) with the Vita patches already committed — and
is built in-tree by CMake, so nothing is installed over the copy VitaSDK ships.

```bash
git clone --recursive https://github.com/NDRWhun/JAVITA && cd JAVITA
bash tools/build.sh        # vdpm deps + port -> build/JAVITA.vpk
```

`bash tools/build.sh --skip-deps` rebuilds just the port once the deps are installed. Cloned
without `--recursive`? The script runs `git submodule update --init` for you.

`tools/env.sh` puts VitaSDK on `PATH` and defaults `VITASDK` to `/usr/local/vitasdk`. Override it
by exporting `VITASDK` first, or by dropping host-specific settings in `tools/env.local.sh`
(gitignored).

### Renderer notes

The GXM backend lives in [`src/code/rd-gxm/`](src/code/rd-gxm) and is reached from the stock
OpenJK renderer through `tr_gxm_bridge.cpp`. Its shaders are Cg sources under
`src/code/rd-gxm/shaders/`, compiled offline by `build_shaders.py` into `gxm_shaders.h` — if you
change a `.cg` you need to re-run that script (it needs Sony's shader compiler, which is not
redistributable, so the generated header is committed).

## Credits

- [OpenJK](https://github.com/JACoders/OpenJK) (JACoders) — the open-source JK2/JKA engine this builds on.
- Raven Software / LucasArts — the original *Jedi Knight: Jedi Academy*.
- [Rinnegatamante](https://github.com/Rinnegatamante) — vitaGL, which this port used before the
  native GXM backend replaced it, and vitaQuakeIII as the reference id Tech 3 Vita port.
- [Northfear](https://github.com/Northfear/SDL) — the SDL2 Vita fork this builds against.

## License

GPLv2 (see [LICENSE](LICENSE)), matching OpenJK. Source under `src/` keeps its original
OpenJK / id Software copyright headers.

Unofficial, non-commercial fan port — not affiliated with or endorsed by Disney, Lucasfilm,
LucasArts, Activision, or Raven. *Star Wars*, *Jedi Knight*, and *Jedi Academy* are trademarks of
their owners; you must own a legal copy to play.
