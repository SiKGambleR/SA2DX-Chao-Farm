# SA2DX-Chao-Farm
-Adds in the 3 DX Chao Gardens & the Chao Farm - A way to play SA2 as a Chao Only Experience-

There has been minimal testing done and I am looking for people to test any and all scenarios possible. 

Version 0.4.20 - for Sonic Adventure 2 with the SA Mod Manager.

## What This Mod Adds:

### SADX Chao Gardens:
Station Square, Mystic Ruins and Egg Carrier Chao Gardens from Sonic
Adventure DX - rebuilt for SA2. Each garden is reached through its own gate
in a new area in the Chao Lobby, and keep each's own independent
Chao Population and Item Pool. 

-Known issue: Water on Egg Carrier doesn't work.

### The Chao Farm:
Ever wanted to be able to play the Chao World as its own game?  The Chao Farm 
was born from this thought.  A brand-new stage that is accessible from the main
menu before starting the story giving all playable characters Chao World access.

On the main menu the DOWNLOAD CONTENT button has been replaced with a CHAO FARM
button; Selecting it takes you straight to the character select - 
no story progress needed, it works on a fresh save, and every character is unlocked.

Inside the Chao Farm every character gets:
- 24 respawning rings
- All 21 animals that respawn
- 4 Chaos Drives (!x each color, respawning)
- A Chao Key Crate
- A Goal Ring for Sonic/Shadow/Tails/Eggman & 3 Master Emeralds for Knuckles/Rouge

Each character plays the Chao Farm with a song from one of their own missions,
and the Chao Farm has its own loading screen & stage-select node (the Chao Key
Icon, Image Credit: https://sonic.fandom.com/wiki/Chao_Key).
## Installation

1. Install the SA Mod Manager if you have not already:
   https://github.com/X-Hax/SA-Mod-Manager
2. Copy the folder "SA2DX Chao Farm 0.4.20" into your game's "mods" folder
   or use the "+ Add Mod" button in SA Mod Manager.
3. Enable "SA2DX Chao Farm" in SA Mod Manager then "Save and Play".

## Compatibility notes

- Built and tested against the Steam 2012 PC port with SA Mod Manager
- Mods that heavily modify the main menu, stage select screen, or the
  Chao World lobby may conflict.
- The mod never writes extra Chao into your game save. The added gardens
  store their Chao and items in a small sidecar file next to your save
  (SA2SixGardens-<save>.dat); deleting it resets the three added gardens
  to empty without touching your save.
- Character unlocks inside the Farm are display/selection overrides only.

## Credits

- SiK GambleR - design, direction, testing, graphical assets
- Built with AI assistance (GitHub Copilot) for code, reverse engineering
  and asset tooling
- SA Mod Manager & SA Tools 
  (https://github.com/X-Hax/SA-Mod-Manager & https://github.com/X-Hax/sa_tools)
- Font used on loading screen: https://www.dafont.com/chaolphabet.font
- Sonic Adventure 2, Sonic Adventure DX and all related assets are the
  property of SEGA. The texture archives in this mod are modified copies
  of the game's original files and require a legally owned copy of both games.

## Version history

- 0.4.20 - Independent per-garden item pools; Chao and items can be
  carried between gardens, the lobby and Neutral without loss or
  duplication, stability tweaks; public release.
- 0.4.0 - First Chao Farm release: new stage, main-menu button with direct
  warp, fresh-save support, custom loading screen, stage-select art,
  per-character music, small tweaks; not released to public.
- 0.3.1 - First stable release shown to the public but removed due to 
  issues with, ironically, stability
- 0.2.5 - First thought stable build: the three SADX gardens with textures
  and lobby corridor entrances, never released to public.
- 0.1.0 - A Mess.
