![sm64coopNX Logo](textures/segment2/custom_coopdx_logo.rgba32.png)

sm64coopNX is a fork of sm64coopdx, an online multiplayer project for the Super Mario 64 PC port that synchronizes all entities and every level for multiple players, adding a native Nintendo Switch homebrew port on top. The original project was started by the Coop Deluxe Team. The purpose is to actively maintain and improve, but also continue sm64ex-coop, created by djoslin0. More features, customization, and power to the Lua API allow modders and players to enjoy Super Mario 64 more than ever!

Feel free to report bugs or contribute to the project.

## Nintendo Switch Port
sm64coopNX adds a homebrew Nintendo Switch port (devkitA64/libnx) with native LDN local wireless multiplayer, alongside the existing PC build.

**Status: playable, still being polished.**
- Offline/solo play is stable.
- LDN local wireless multiplayer works: two consoles connect over local wireless and stay in sync during gameplay (game packets travel as UDP over the LDN network). There's still a brief hitch at the start of a session while a joining player drops in. Tested with 2 consoles; more than 2 is untested.
- The in-game player name defaults to the Switch profile that launched the app.
- Direct connect (regular IP-based online multiplayer) on Switch is untested.

Switch build: see `Makefile.nx` (`make -f Makefile.nx`), requires devkitA64/libnx. Like upstream sm64coopdx, this fork builds without you needing to supply a ROM.

## Legal
This is a fan-made, non-commercial project not affiliated with Nintendo, built on the community sm64 decompilation just like upstream sm64coopdx. See [NOTICE.md](NOTICE.md) for details.
