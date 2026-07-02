![sm64coopdx Logo](textures/segment2/custom_coopdx_logo.rgba32.png)

sm64coopdx is an online multiplayer project for the Super Mario 64 PC port that synchronizes all entities and every level for multiple players. The project was started by the Coop Deluxe Team. The purpose is to actively maintain and improve, but also continue sm64ex-coop, created by djoslin0. More features, customization, and power to the Lua API allow modders and players to enjoy Super Mario 64 more than ever!

Feel free to report bugs or contribute to the project.

## Initial Goal (Accomplished)
Create a mod for the PC port where multiple people can play together online.

Unlike previous multiplayer projects, this one synchronizes enemies and events. This allows players to interact with the same world at the same time.

Interestingly enough though, the goal of the project has slowly evolved over time from simply just making a Super Mario 64 multiplayer mod to constantly maintaining and improving the project (notably the Lua API.)

## Documentation

sm64coopdx is moddable via Lua, similar to Roblox and Garry's Mod's Lua APIs. To get started, click [here](docs/lua/lua.md) to see the Lua documentation. If you want to contribute to the repo, you can view the C documentation [here](docs/c/c.md).

## Wiki
The wiki is made using GitHub's wiki feature, you can go to the wiki tab or click [here](https://github.com/coop-deluxe/sm64coopdx/wiki).

## Nintendo Switch Port (`nx-port` branch)
This branch adds a homebrew Nintendo Switch port (devkitA64/libnx) with native LDN local wireless multiplayer, alongside the existing PC build.

A few ROM-extracted asset files (`actors/bowser_key/*.rgba16.png`, `sound/sequences_compressed.bin`) that had accidentally been committed upstream were removed from this branch, since they're derived from Nintendo's original ROM data. To restore them (required for a full build), place your own legally-owned Super Mario 64 ROM as `baserom.us.z64` in the repo root and run the project's own extraction tool:

```
python3 extract_assets.py us
```

This regenerates all asset files listed in `assets.json` from your ROM, including the two removed here - the build system already treats extracted assets as untracked/gitignored, so this only needs to be run once per checkout.

Switch build: see `Makefile.nx` (`make -f Makefile.nx`), requires devkitA64/libnx.

## Community
We have an official Discord server open to the public [here](https://discord.gg/TJVKHS4).
