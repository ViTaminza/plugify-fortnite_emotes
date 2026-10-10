// fortnite_emotes — shared declarations of the port (the original's fe.h, adapted to
// Plugify/s2sdk/pisexcompat).
//
// The original is the Metamod:Source plugin cs2-fortnite_emotes (a C++ port of the SwiftlyS2
// plugin FortniteEmotesS2): chat commands and a menu play an emote/dance animation on an invisible
// rig prop cloned onto the player's body, with a third-person camera, hidden weapons and a sound
// loop.
//
// Porting rules applied everywhere in this plugin:
//   * an entity is identified by its **s2sdk handle** (CEntityHandle::ToInt); pisexcompat and the
//     s2sdk "*Index" helpers hand out 1-based indexes, Handle() converts once at the boundary;
//   * schema fields are read/written through s2sdk (by class and field name; component fields
//     through the component pointer, which is what s2sdk's chain handling expects);
//   * no engine function is detoured: OnAnimationDone is s2sdk's entity-output hook, CheckTransmit
//     is s2sdk's OnServerCheckTransmit, the precache list is s2sdk's game-session manifest, the
//     aim trace is s2sdk's TraceCollideable/TraceLine;
//   * the original's gamedata (FireOutputInternal / UTIL_TraceShape / GetGameTraceManager) is
//     therefore not used at all; the corresponding settings keys are ignored with a log line;
//   * HookOnTakeDamagePre (StopDamageWhenInEmote) has no s2sdk equivalent and is NOT ported; the
//     setting defaults to 0 in the production config (see the README section of this plugin).

#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include <pisex/pisex.hpp>

#define FE_MAX_PLAYERS 64

// Monotonic seconds (the original's FE_Now(); tier0's Plat_FloatTime is not available).
double FE_Now();

// ---------------------------------------------------------------------------------------------
// Globals — defined in plugin.cpp
// ---------------------------------------------------------------------------------------------

extern bool g_bEnabled;  // css_fortnite_emotes_enable
extern bool g_bRoundEnd; // set on round_end, cleared on round_prestart
// False when the s2sdk entry points the emote lifecycle needs are not all present (checked once at
// plugin start): playing an emote is then refused instead of crashing in the middle of it.
extern bool g_bEmoteBindings;
void CheckEmoteBindings();

// Logging helpers; DebugLog prints only when Settings.DebugLogs is on.
void LogInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogWarning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void DebugLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Plugin-owned timers: the timer registry of pisexcompat keeps the callback; wrapping every timer
// in a slot lets OnPluginEnd take down everything that is still live before the code is unmapped.
pisex::CTimer* CreatePluginTimer(float flInterval, std::function<float()> fn);
void RemovePluginTimers();

// ---------------------------------------------------------------------------------------------
// Config, translations, lookup — config.cpp
// Files: configs/fortnite_emotes/{settings.ini,emotes.ini}, translations/fortnite_emotes.phrases.txt
// ---------------------------------------------------------------------------------------------

struct Settings
{
	int EmoteAllowedPeriod = 0;
	bool StopEmoteAfterFreezetimeEnd = false;
	bool StopDamageWhenInEmote = false; // not ported, see the file header
	int EmoteHidePlayers = 0;
	bool EmoteFreezePlayer = true;
	bool ForcePlayerVisibility = false;
	bool EmoteGlovesFix = true;
	bool CameraMoveable = true;
	bool CameraBlockObject = true;
	int EmoteCooldown = 20;
	int EmoteVIPCooldown = 15;
	uint64_t CancelButtons = 0; // parsed from EmoteCancelButtons ("w,a,s,d,jump,crouch,leftclick")
	std::vector<std::string> SoundEventFiles; // precached
	std::vector<std::string> EmoteCommand, DanceCommand, AdminSetEmoteCommand, AdminSetDanceCommand;
	// Access lists (see HasAccess): "" = everyone, "vip" = any VIP, "vip:<feature>" = VIP feature
	// bool, anything else = admin permission (admin_system HasPermission).
	std::vector<std::string> EmoteDanceCommandPerm, AdminSetEmoteDanceCommandPerm, VIPPerm;
	std::string VIPCooldownFeature = "emotes_cooldown"; // VIP_INT feature: per-group cooldown seconds
	bool SoundModuleEnabled = true;
	int SoundRecipients = 0; // 0 = only the dancer, 1 = everybody
	bool ChatTriggersEnabled = true;
	bool DebugLogs = false;
	float MaxEmoteSeconds = 60.0f; // safety stop when OnAnimationDone never comes
};

struct Emote
{
	std::string Name;    // unique name and translation key
	bool IsEmote = true; // false = dance
	std::string Model;
	std::string AnimIntro;     // AnimationName
	std::string AnimLoop;      // DefaultAnimationName
	float AnimLoopAfter = -1;  // SetToDefaultAnimationDuration
	float Duration = -1;       // AnimationDuration
	std::string Sound;
	float SoundVolume = 0.7f;
	float SoundLoopAfter = -1; // LoopSoundAfterSeconds
	std::vector<std::string> Triggers;
	std::vector<std::string> Permission;
};

extern Settings g_Settings;
extern std::vector<Emote> g_Emotes;                         // config order
extern std::unordered_map<std::string, int> g_ChatTriggers; // exact, case-sensitive; first wins
extern std::vector<std::string> g_TriggerOrder;             // first-seen order (trigger list)

bool LoadConfig();                          // settings + emotes + phrases
std::vector<std::string> GetPrecacheList(); // distinct emote models, camera model, soundevent files

std::string Phrase(const char* key); // server language, fallback "en", then the key
std::string FormatPhrase(const char* key, std::initializer_list<std::string> args); // {0}, {1}, ...
std::string Prefix();
void Print(int iSlot, const std::string& msg); // converts the phrase colour tags
std::string EmoteDisplayName(const Emote& emote);
int GetEmoteByName(const std::string& input, bool isEmote, bool partial); // index or -1

// ---------------------------------------------------------------------------------------------
// Per-player state and emote lifecycle — emote.cpp
// ---------------------------------------------------------------------------------------------

struct GloveSnapshot
{
	bool Valid = false;
	uint32_t PawnHandle = 0; // full s2sdk handle (index + serial) of the pawn the gloves belong to
	uint16_t DefIndex = 0;
	bool Initialized = false;
	uint64_t ItemID = 0;
};

struct WeaponEntry
{
	std::string Classname;
	int Clip = 0;
	int Reserve = 0;
};

struct PlayerState
{
	bool IsDancing = false;
	int EmoteIndex = -1;
	// Generation of the current emote: every start bumps it; timers/NextFrame callbacks capture it
	// and do nothing when it no longer matches.
	uint32_t Generation = 0;
	int Rig = 0;    // s2sdk handle of the invisible animated prop
	int Clone = 0;  // visible body, FollowEntity -> rig
	int Camera = 0; // view entity prop
	// True while the camera prop's handle is the pawn's view entity (set by SetCam's NextFrame after
	// the schema write, cleared by ResetCam). Only a slot that ever showed a third-person camera may
	// be restored to first person: a forced stop on a slot that never danced must not touch the
	// camera-services schema at all (map-start crash, see ResetCam).
	bool CameraSet = false;
	std::string RigName;
	uint8_t PlayerAlpha = 255;
	// MOVETYPE_WALK, the original's default: the stop compares it against the pawn's current
	// move type and writes it back, so 0 (MOVETYPE_NONE = frozen) must never be used here.
	int PreviousMoveType = 2;
	double CooldownUntil = 0; // FE_Now() based
	bool StopOnAnimationDone = false;
	double StartedAt = 0;
	std::string CurrentSound;
	// s2sdk entity HANDLES hidden from this viewer (hooks.cpp). Handles, not indexes: an index is
	// recycled, and hiding the wrong entity from a client would be worse than not hiding.
	std::vector<int> BlockedTransmit;
	GloveSnapshot Gloves;             // weapons_gloves.cpp
	bool HasWeaponSnapshot = false;   // weapons_gloves.cpp
	std::vector<WeaponEntry> Weapons;
	std::vector<uint16_t> Ammo;
	uint64_t PrevButtons = 0;
};

extern PlayerState g_States[FE_MAX_PLAYERS];

// Entity indexes (0 = none) — the shape the original's hl2sdk helpers had.
int GetController(int iSlot);
int GetAlivePawn(int iSlot);
bool IsValidHuman(int iSlot);

// PlayEmote: error receives " {prefix} {message}" on failure; bForced = admin !setemote.
bool PlayEmote(int iSlot, int iEmoteIndex, std::string& error, bool bForced);
void StopEmote(int iSlot, bool bForce = false);
void StopAllEmotes();
void ResetPlayer(int iSlot);
bool IsDancing(int iSlot);
bool IsReadyForDancing(int iSlot);
void OnFrame();
void OnRigAnimationDone(int iRigHandle); // called by hooks.cpp for OnAnimationDone

// Shared entity helpers (emote.cpp): alpha via m_clrRender, the original's PrepareProp, prop
// creation with "model"/"targetname". All take/return s2sdk handles.
void SetEntityAlpha(int iHandle, uint8_t alpha);
void PrepareProp(int iHandle, int solidType);
int CreateProp(const char* szModel, const plg::vec3& origin, const plg::vec3& angles, const char* szName);

// ---------------------------------------------------------------------------------------------
// Camera — camera.cpp
// ---------------------------------------------------------------------------------------------

bool InitCameraTrace();  // resolves what it needs; false = occlusion disabled
// The pawn's camera-services component (the field name differs between builds): used as the
// "fully spawned" check of the original's PlayEmote/IsReadyForDancing.
bool HasCameraServices(int iPawnHandle);
int SetCam(int iSlot);   // creates the camera prop and makes it the view entity
void ResetCam(int iSlot);
void UpdateCamera(int iSlot);
void UpdateRigFollow(int iSlot);
plg::vec3 CalculatePositionInFront(int iPawnHandle, float offsetXy, float offsetZ);
// SPEC §B.4 "@aim": trace from the pawn's eyes along its view angles; returns the hit entity index
// (0 = none, never the pawn itself).
int TraceFromEyes(int iPawnHandle, float flDistance);

// ---------------------------------------------------------------------------------------------
// Weapons, attack block, gloves — weapons_gloves.cpp
// ---------------------------------------------------------------------------------------------

void ClearPlayerWeapons(int iSlot);
void GivePlayerWeaponsBack(int iSlot); // always consumes the snapshot
void BlockPlayerAttack(int iSlot);
void ResetPlayerAttackTicks(int iSlot);
void HideEmoteGloves(int iSlot);
void RestoreEmoteGloves(int iSlot);
void DropGloveSnapshot(int iSlot);

// ---------------------------------------------------------------------------------------------
// Engine hooks, transmit hiding, sound, precache — hooks.cpp
// ---------------------------------------------------------------------------------------------

bool InstallHooks();
void RemoveHooks();
void ApplyHidePlayers(int iViewerSlot);
void ClearHidePlayers(int iViewerSlot);
void EmitEmoteSound(int iSlot);
void StopEmoteSound(int iSlot);

// ---------------------------------------------------------------------------------------------
// Commands, chat triggers, menu, access — commands.cpp
// ---------------------------------------------------------------------------------------------

void RegisterCommands();
void ShowEmoteMenu(int iSlot, bool bDance);
void CloseEmoteMenus();
bool HasAccess(int iSlot, const std::vector<std::string>& perms);
int GetCooldownSeconds(int iSlot);

// ---------------------------------------------------------------------------------------------
// Small entity/schema helpers shared by the implementation files (fe_entity.cpp).
// All of them take s2sdk handles and are no-ops when the field/binding is missing.
// ---------------------------------------------------------------------------------------------

// HANDLE validity of the raw value (no engine access): a handle is invalid only when it is
// INVALID_EHANDLE_INDEX (-1) or 0. A CEntityHandle is a uint32 view (entity index in the low 15
// bits, serial above); a few minutes after the server starts the serial passes 65535, bit 31 gets
// set and every valid handle reads as a NEGATIVE int (-921534371 = 0xC912805D = index 93,
// serial 102949). The sign means nothing. 0 is index 0 serial 0 - the world, never a player or a
// prop - and is also the "none" most of these helpers return, so it stays invalid here.
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// 1-based entity index -> s2sdk handle (0 when invalid).
int Handle(int iIndex);
int IndexOf(int iHandle);
// True while the handle still refers to a live entity (it carries the serial, so a recycled index
// cannot pass). Silent - s2sdk's entity functions log a warning for an invalid handle, which must
// never happen once per frame, so every cached handle is checked with this first.
bool ValidHandle(int iHandle);

int64_t GetIntField(int iHandle, const char* szClass, const char* szField, int64_t iDefault = 0, int iElement = 0);
float GetFloatField(int iHandle, const char* szClass, const char* szField, float flDefault = 0.0f);
bool GetBoolField(int iHandle, const char* szClass, const char* szField, bool bDefault = false);
std::string GetStringField(int iHandle, const char* szClass, const char* szField);
plg::vec3 GetVectorField(int iHandle, const char* szClass, const char* szField);
int GetEntityField(int iHandle, const char* szClass, const char* szField); // entity handle or 0

void SetIntField(int iHandle, const char* szClass, const char* szField, int64_t iValue, bool bChangeState = true,
				 int iElement = 0);
void SetFloatField(int iHandle, const char* szClass, const char* szField, float flValue, bool bChangeState = true);
void SetBoolField(int iHandle, const char* szClass, const char* szField, bool bValue, bool bChangeState = true);
void SetVectorField(int iHandle, const char* szClass, const char* szField, const plg::vec3& v,
					bool bChangeState = true);
void SetEntityField(int iHandle, const char* szClass, const char* szField, int iValue, bool bChangeState = true);
void StateChanged(int iHandle, const char* szClass, const char* szField);

// Address of a component field of an entity (s2sdk GetEntSchemaPtr returns the field address),
// nullptr when missing. This is the component itself for INLINE ("ref") fields like
// CBaseModelEntity::m_Collision.
void* GetComponentPtr(int iHandle, const char* szOwnerClass, const char* szField);
// The object stored in a pointer-typed ("ptr") field (m_pWeaponServices, m_pCameraServices,
// m_CBodyComponent): reads the pointer out of the field address GetComponentPtr returns, nullptr
// when the field or the stored pointer is empty.
void* GetComponentPointer(int iHandle, const char* szOwnerClass, const char* szField);

// Invokes a named input with a string value (the original's AcceptEntityInput + variant_t).
// activator/caller are s2sdk handles; 0 means "none".
void AcceptInputString(int iHandle, const char* szInput, const std::string& sValue, int iActivatorHandle,
					   int iCallerHandle);

// Component field access (the pointer must come from GetComponentPtr/GetComponentPointer).
int64_t GetComponentInt(void* pComponent, const char* szClass, const char* szField, int64_t iDefault = 0,
						int iElement = 0);
float GetComponentFloat(void* pComponent, const char* szClass, const char* szField, float flDefault = 0.0f);
void SetComponentInt(void* pComponent, const char* szClass, const char* szField, int64_t iValue,
					 bool bChangeState = true, int iElement = 0);
void SetComponentFloat(void* pComponent, const char* szClass, const char* szField, float flValue,
					   bool bChangeState = true);
void SetComponentEntity(void* pComponent, const char* szClass, const char* szField, int iValue,
						bool bChangeState = true);
// Same write through the raw offset path (4-byte entity handle, no schema type dereference). Used
// for the camera's m_hViewEntity: the typed setter above dereferences the CSchemaType* of s2sdk's
// per-class table, and a table the engine builds around map start can carry the invalid -1 there.
bool SetComponentEntityRaw(void* pComponent, const char* szClass, const char* szField, int iValue,
						   bool bChangeState = true);

// Cached schema offset (-1 when the class/field is unknown); pisex::SchemaOffset with its own cache.
int Offset(const char* szClass, const char* szField);
int ChainOffset(const char* szClass);

// Raw memory access at an entity + offset (the original's raw schema writes, e.g. for the fields
// of an inline schema class); changeState goes through s2sdk's network state change.
void RawSetInt(int iHandle, int iOffset, int64_t iValue, int iSize, bool bChangeState, int iChainOffset);
int64_t RawGetInt(int iHandle, int iOffset, int iSize);
