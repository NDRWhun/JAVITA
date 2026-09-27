# Third-party components

| Component | License | Where | Role |
|-----------|---------|-------|------|
| [OpenJK](https://github.com/JACoders/OpenJK) | GPLv2 | `src/` (vendored subset, commit 2ba5021) | the engine/game this port builds on |
| [SDL2](https://github.com/Northfear/SDL) | zlib | `third_party/SDL-vitagl` (fork, statically linked) | video/input/audio backend |
| math-neon | via vdpm | linked | NEON math routines |
| minizip, libjpeg-turbo, libpng, zlib | zlib/IJG/zlib/zlib | `src/lib/minizip` + vdpm | asset loading |
| [gsl-lite](https://github.com/gsl-lite/gsl-lite) | MIT | `src/lib/gsl-lite` (headers) | used by vendored OpenJK code |
| mp3code | GPLv2 (part of the JK2/JKA source release) | `src/code/mp3code`, `src/codemp/mp3code` | MP3 decoding |
| [puff](https://github.com/madler/zlib/tree/master/contrib/puff) (Mark Adler) | zlib | `tools/texbake/zipread.h` | inflate for the PC texture baker |
| OpenAL / EAX headers | LGPL / Creative | `src/code/client/OpenAL`, `src/code/client/eax` and the `src/codemp` twins | inherited from OpenJK, not compiled on the Vita |

The renderer backend under `src/rd-gxm/` is written for this port and is GPLv2 with the
rest of the tree. Its shaders are Cg sources compiled offline into `src/rd-gxm/shaders/gxm_shaders.h`;
Sony's shader compiler is not redistributable and is not included here.

Licenses of vdpm-installed libraries are documented in their upstream repositories.

Game assets are not distributed; a legally-owned copy of Jedi Academy is required.
