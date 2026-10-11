# fortnite_emotes (Plugify)

Fortnite emotes & dances (a port of the SwiftlyS2 plugin FortniteEmotesS2 via the Metamod:Source port cs2-fortnite_emotes): '!emote' / '!dance' (plus '!setemote' / '!setdance' for admins) and the emote menu play an animation on an invisible rig prop that a visible clone of the player follows, with a third-person camera, hidden weapons/gloves for the duration and an optional sound loop. Access lists support 'vip' / 'vip:<feature>' through the vip plugin and admin permissions through admin_system; the emote list is configs/fortnite_emotes/emotes.ini, settings are configs/fortnite_emotes/settings.ini (the live server uses workshop addon 3328582199 for the models and sounds) and the texts come from translations/fortnite_emotes.phrases.txt. Chat triggers (single words from the config) and the console commands mm_emote/mm_dance/mm_setemote/mm_setdance work as in the original. No exported methods.

Plugify port (fork) of **FortniteEmotesNDances** by Cruze (CounterStrikeSharp (C#)).
Original: https://github.com/Cruze03/FortniteEmotesNDances
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system (optional), vip (optional), hudmenu (optional)

## Installation
Download `fortnite_emotes-2.0.4.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/fortnite_emotes`.

`plugins/admin_system`, `plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

