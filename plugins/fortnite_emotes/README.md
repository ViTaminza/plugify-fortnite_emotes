# fortnite_emotes — Fortnite emotes & dances (Plugify port)

Port of the Metamod:Source plugin cs2-fortnite_emotes (a C++ port of the SwiftlyS2
plugin FortniteEmotesS2 by Cruze). `!emote` / `!dance` (and `!setemote` / `!setdance` for admins)
plus the emote menu play an animation on an invisible rig prop that a visible clone of the player
follows, with a third-person camera, hidden weapons/gloves for the duration and an optional sound
loop.

| | |
|---|---|
| original | cs2-fortnite_emotes (~3450 LOC of C++ across 7 files) |
| config | `configs/fortnite_emotes/settings.ini` and `configs/fortnite_emotes/emotes.ini` — the production files (80 entries: 37 emotes, 43 dances) |
| phrases | `configs/fortnite_emotes/translations/fortnite_emotes.phrases.txt` |
| commands | chat `!emote`, `!emotes`, `!dance`, `!dances`, `!setemote(s)`, `!setdance(s)`, the chat triggers of `emotes.ini`, `!et`/`!dtrigger` (trigger list); console `mm_emote`, `mm_dance`, `mm_setemote`, `mm_setdance`, `css_et*`, `css_dtrigger*` |
| convar | `css_fortnite_emotes_enable` (master switch, stops the running emotes when changed) |
| integrations | `vip` (optional): `vip` / `vip:<feature>` access entries, `VIPCooldownFeature`; `admin_system` (optional): admin permission entries (`emotes.admin` for `!setemote`) |
| workshop addons | **3328582199** (models `characters/kolka/fortnite_dance.vmdl`, `soundevents/fortnite_emotes.vsndevts`). Add it to `csgo/cfg/plugify/multi_addon_manager.cfg` (`s2_extra_addons`); the plugin checks `mm_extra_addons` 1 s after load and warns when it is missing. |
| tests | `test/fe_core_tests.cpp` (build/tests/fortnite_emotes_tests): the production settings/emotes/phrases, the cancel-button parsing, the trigger map, the precache list, the D.3 name lookup |

## What is ported

* every config key of `settings.ini` and `emotes.ini` (allowed period, freezetime stop, hide
  players, freeze, forced visibility, glove fix, camera moveable/blocking, cooldowns incl. the VIP
  feature, cancel buttons, sound module with its recipient mode and loop, chat triggers, max emote
  seconds, debug logs, access lists, command names, per-emote permission/trigger/sound/duration);
* the lifecycle of the original: preconditions (period, alive, scope, ground, ducking, cooldown),
  the invisible rig + visible clone (`prop_dynamic` with `model`/`targetname`, `SetAnimation`,
  `FollowEntity`), the loop switch and the stop timers with their generation guards, the
  `OnAnimationDone` stop, `MaxEmoteSeconds` as the safety net, the cancel buttons, the velocity
  check, the camera (behind/above, occlusion trace, lerp, `m_hViewEntity` on the
  camera-services component), the weapon snapshot/drop/give-back (with the ammo array), the attack
  block, the econ-glove hiding with the bodygroup rebuild, the hide-players transmit filter, the
  sound (emit/loop/stop), the menu with its two-step deferred start, the chat triggers, the admin
  target selectors (`#slot`, `@me`, `@aim`, `@all`, `@t`, `@ct`, `@alive`, `@dead`, `@bots`,
  `@human`, names) and the round-end stop timer based on `mp_round_restart_delay`;
* access lists: `""` = everyone, `vip` / `vip:<feature>` through the **vip** plugin, anything else
  through **admin_system**'s `HasPermission`; both are optional (a missing plugin means "no
  access", exactly like the original without its cores).

## Deliberate differences

1. **OnAnimationDone** uses `s2sdk::HookEntityOutput("*", "OnAnimationDone", …, Post)` instead of a
   funchook detour of `CEntityIOOutput::FireOutputInternal`; the original's `SigFireOutputInternal`
   gamedata is therefore unused (the key is read and ignored with a log line).
2. **Camera occlusion** uses `s2sdk::TraceLine` (mask + ignored pawn) instead of the gamedata
   resolved `UTIL_TraceShape`/`GetGameTraceManager`; the `SigTraceShape`/`SigGetGameTraceManager`
   keys are ignored the same way. The original's mask constant is kept.
3. **CheckTransmit** uses `s2sdk::OnServerCheckTransmit` + `s2sdk::ClearTransmitInfoEntity` instead
   of a `ISource2GameEntities::CheckTransmit` SourceHook.
4. **Precache** uses the s2sdk game-session manifest + `s2sdk::Precache` instead of
   `IEntityResourceManifest::AddResource` on a manually hooked `OnBuildGameSessionManifest`.
5. **The weapon classname table is gone**: `s2sdk::GetEntityClassname` returns exactly the name
   `GiveNamedItem` takes, so the item-definition-index mapping of the original (mp5sd, knifegg, …)
   is unnecessary.
6. **The vip integration consumes the generated bindings directly** (`<vip/vip.hpp>`,
   four methods, each guarded by its `__vip_*` slot) instead of `vip_api.hpp` — the wrapper's
   `std::function` aliases collide with the bindings' prototypes (the known
   `vip_api_compat.hpp` problem of the other vip modules), and this plugin uses so little of the
   interface that the shim would be dead weight.
7. **The chat spelling "."** (`!emote` / `/emote` / `.emote` in the original) cannot be dispatched:
   s2sdk strips only the trigger characters of its settings ("!" and "/"); the commands are
   registered with "!" and work with "!" and "/" (the console names are unchanged).
8. **The frame loop** is Plugify's `OnPluginUpdate` instead of a utils timer with 0 delay.
9. **The ConVar change callback** only records the change; the frame loop stops the emotes (never
   call back into a subsystem that is calling you — see pisexcompat's rules).

## Not ported

1. **`StopDamageWhenInEmote`** — s2sdk has no `TakeDamage` hook and the port does not detour engine
   functions, so the "dancing players take no damage" rule of the original is missing. The
   production config has the key at `0`; when it is `1` the plugin logs a warning at startup.
   (Damage therefore *interrupts* an emote through `player_hurt`, as the original did when the
   setting was off.)
2. The mesh-group-mask copy stays as the original wrote it (schema offsets through
   `CBodyComponent::m_pSceneNode` → `CSkeletonInstance::m_modelState` → `CModelMeshGroupMask`), but
   it is skipped silently when this build's offsets do not resolve — the clone then simply uses the
   model's default bodygroups.

## Needs a check on a real server

* the whole emote lifecycle on a real server: rig/clone props, `SetAnimation`, `FollowEntity`,
  the camera view entity, the glove hiding and the weapon give-back are all engine paths that no
  offline test can cover. Walk the checklist once: start/stop an
  emote, double-tap cancel, shoot-cancel, death/spawn/round-end stops, `!setemote @aim`,
  glove/weapon restore, camera occlusion at a wall;
* the VIP access path: with the config's `EmoteDanceCommandPerm "vip:emotes"` only VIP groups with
  the `emotes` feature can use `!emote` / `!dance`; give a test account the feature and check the
  menu, the disabled items and the `emotes_cooldown` override;
* camera walls: the camera is swept from the pawn's eye as five parallel `s2sdk::TraceLine`s
  (centre and 12 units left/right/up/down) and stops 14 units before the first wall, with no
  minimum distance — against a wall it stays at the head. Both first teleports use it, and every
  frame the camera sits on the swept ray (only the distance is smoothed). `CameraBlockObject` no
  longer turns the check off, it only picks the smoothing speed. `s2sdk::TraceHull` is not used:
  in s2sdk 2.18.2 it passes `start`/`end` to the engine's VScript TraceHull, which wants
  `startpos`/`endpos`, and every hull trace fails;
* performance: a dancing player costs five `s2sdk::TraceLine` per frame while the view turns (the
  result is cached while the pawn's origin and yaw stay the same, up to 2 s), and s2sdk's trace
  wrappers go through the engine's VScript VM (table in, table out). Watch the server frame time
  with several dancers at once.
