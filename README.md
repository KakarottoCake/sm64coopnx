![sm64coopNX Logo](textures/segment2/custom_coopdx_logo.rgba32.png)

sm64coopNX is a fork of sm64coopdx, an online multiplayer project for the Super Mario 64 PC port that synchronizes all entities and every level for multiple players, adding a native Nintendo Switch homebrew port on top. The original project was started by the Coop Deluxe Team. The purpose is to actively maintain and improve, but also continue sm64ex-coop, created by djoslin0. More features, customization, and power to the Lua API allow modders and players to enjoy Super Mario 64 more than ever!

Feel free to report bugs or contribute to the project.

## Nintendo Switch Port
sm64coopNX adds a homebrew Nintendo Switch port (devkitA64/libnx) with native LDN local wireless multiplayer, alongside the existing PC build.

A few ROM-extracted asset files (`actors/bowser_key/*.rgba16.png`, `sound/sequences_compressed.bin`) that had accidentally been committed upstream were removed from this fork, since they're derived from Nintendo's original ROM data. To restore them (required for a full build), place your own legally-owned Super Mario 64 ROM as `baserom.us.z64` in the repo root and run the project's own extraction tool:

```
python3 extract_assets.py us
```

This regenerates all asset files listed in `assets.json` from your ROM, including the two removed here - the build system already treats extracted assets as untracked/gitignored, so this only needs to be run once per checkout.

Switch build: see `Makefile.nx` (`make -f Makefile.nx`), requires devkitA64/libnx.
