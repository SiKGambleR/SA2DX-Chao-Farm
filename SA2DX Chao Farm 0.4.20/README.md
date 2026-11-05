# SA2DX Chao Farm

Version 0.4.20 - for Sonic Adventure 2 (2012 PC port) with the SA2 Mod Loader.

## What this mod adds

### SADX Chao Gardens
Station Square, Mystic Ruins and Egg Carrier Chao Gardens from Sonic
Adventure DX, rebuilt for SA2. Each garden is reached through its own gate
in a new corridor off the Chao World lobby, and keeps its own independent
Chao population and ground items. Chao and items can be carried between
gardens and the lobby - carrying one out removes it from the garden and
takes it with you.

### The Chao Farm
A brand-new stage with its own button on the main menu (it replaces the
unused DOWNLOAD CONTENT entry). Selecting it takes you straight to the
character select - no story progress needed, it works on a fresh save, and
every character is unlocked for it.

Inside the Farm every character gets:
- 24 respawning rings
- all 21 animals
- 4 Chaos Drives
- a Chao crate with a key that exits to the Chao World lobby
- a goal ring
- Knuckles and Rouge also get a 3-piece emerald hunt with working radar

Each character plays the Farm with a song from one of their own missions,
and the Farm has its own loading screen, stage-select node (the Chao key
icon) and name.

## Installation

1. Install the SA2 Mod Loader / SA Mod Manager if you have not already:
   https://github.com/X-Hax/SA-Mod-Manager
2. Copy the folder "SA2DX Chao Farm 0.4.20" into your game's "mods" folder
   (next to sonic2app.exe).
3. Enable "SA2DX Chao Farm" in the mod manager and save.

Do not run this together with a development build of the same mod - they
patch the same game code.

## Entering the Chao Farm

- Main menu > CHAO FARM, or
- Stage Select > the Chao-key node below Chao World.

The Chao key in the crate takes you to the Chao World lobby, so you can
carry rings, animals and drives straight to your Chao.

## Compatibility notes

- Built and tested against the Steam 2012 PC port with SA2 Mod Loader
  (API version 16).
- Mods that heavily modify the main menu, stage select screen, or the
  Chao World lobby may conflict.
- The mod never writes extra Chao into your game save. The added gardens
  store their Chao and items in a small sidecar file next to your save
  (SA2SixGardens-<save>.dat); deleting it resets the three added gardens
  to empty without touching your save.
- Character unlocks inside the Farm are display/selection overrides only.

## Credits

- SiK GambleR - design, direction, testing
- Built with AI assistance (GitHub Copilot) for code, reverse engineering
  and asset tooling
- SA2 Mod Loader / SA Mod Manager by the X-Hax team
- SA Tools by the X-Hax team (level model conversion)
- Sonic Adventure 2, Sonic Adventure DX and all related assets are the
  property of SEGA. The texture archives in this mod are modified copies
  of the game's own files and require a legally owned copy of the game.

## Version history

- 0.4.20 - independent per-garden item pools (the three added gardens now
  start empty and keep their own ground items); Chao and items can be
  carried between gardens, the lobby and Neutral without loss or
  duplication.
- 0.4 - first Chao Farm release: new stage, main-menu button with direct
  warp, fresh-save support, custom loading screen, stage-select art,
  per-character music, "Welcome To The Farm" objective text.
- 0.2.5 - first stable release: the three SADX gardens with textures and
  lobby corridor entrances.
