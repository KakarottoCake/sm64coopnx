# Legal Notice

sm64coopNX (a fork of sm64coopdx) is a fan-made, non-commercial project built on top of a
community reverse-engineering (decompilation) of Super Mario 64. It is not affiliated with,
endorsed by, or sponsored by Nintendo.

## No Nintendo assets are distributed

This repository contains no Nintendo copyrighted assets (ROM data, textures, models, audio,
etc.). Building the game requires you to supply your own legally-owned Super Mario 64 ROM,
from which the build process (`extract_assets.py`) extracts the assets needed locally on your
own machine. Nothing derived from Nintendo's ROM is committed to this repository or
distributed with it.

## No formal license

Because this codebase is derived from a decompilation of Nintendo's copyrighted game, it is
not released under a standard open-source license (MIT, GPL, etc.) - the underlying game code
is not ours to license. This matches how other Super Mario 64 decompilation-based projects
(sm64ex, sm64-port, sm64coopdx, and others) are typically distributed: source-available for
educational, preservation, and interoperability purposes, without granting the redistribution
or commercial-use rights a formal open-source license would imply.

All trademarks and copyrights for Super Mario 64 and related properties belong to Nintendo.

## Third-party components

A small number of bundled third-party tools/libraries retain their own separate licenses,
found alongside them:

- `src/pc/utils/miniz/LICENSE`
- `tools/ido5.3_compiler/LICENSE.md`
- `tools/n64graphics_ci_dir/LICENSE`
- `tools/sm64tools.LICENSE`
- `lib/lua/` (Lua, MIT license - see the copyright notice in `lib/lua/include/lua.h`)
