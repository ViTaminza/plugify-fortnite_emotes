// fortnite_emotes — core emote lifecycle (the original's emote.cpp: SPEC §E.1, §E.2, §E.3, §E.6,
// §E.9, §E.11, §E.12). Owns the per-player state, the start/stop sequence, the rig + clone props,
// the timers and the tick loop.
//
// Engine access is s2sdk: props are prop_dynamic entities created with s2sdk::CreateEntityByName +
// DispatchSpawn2("model"/"targetname"), animations are "SetAnimation" inputs, the clone follows the
// rig through the "FollowEntity" input, alpha goes through the render colour and freezing uses the
// client move type (MOVETYPE_OBSOLETE = 1).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

#include "fe.hpp"

PlayerState g_States[FE_MAX_PLAYERS];

// Set by CheckEmoteBindings() (plugin.cpp) at plugin start.
bool g_bEmoteBindings = true;

namespace {

// ---------------------------------------------------------------------------------------------
// Small queries
// ---------------------------------------------------------------------------------------------

bool IsMenuOpenFor(int iSlot)
{
	return pisex::g_pMenus != nullptr && pisex::g_pMenus->IsMenuOpen(iSlot);
}

bool IsAllowedPeriod()
{
	if (g_Settings.EmoteAllowedPeriod == 0) return true;

	// Between the map end and the next map start the game-rules object is gone (s2sdk nulls it
	// when the gamerules entity is deleted) and the client list is torn down; nothing is allowed
	// then, and the typed pointer-2 reads below must not run on a half-destroyed world.
	if (!pisex::IsGameReady()) return false;

	void* pRules = __s2sdk_GetGameRules ? s2sdk::GetGameRules() : nullptr;
	const bool bWarmup = pRules != nullptr && GetComponentInt(pRules, "CCSGameRules", "m_bWarmupPeriod") != 0;
	const bool bFreeze = pRules != nullptr && GetComponentInt(pRules, "CCSGameRules", "m_bFreezePeriod") != 0;
	const bool bRoundEnd = g_Settings.EmoteAllowedPeriod == 2 && g_bRoundEnd;
	return bWarmup || bFreeze || bRoundEnd;
}

bool IsScopedPawn(int iPawnHandle)
{
	return GetBoolField(iPawnHandle, "CCSPlayerPawn", "m_bIsScoped");
}

// Standing on the world (entity index 0) or on an unresolved ground handle is fine (SPEC §E.1.7).
bool IsGroundAcceptable(int iPawnHandle)
{
	const int iGround = GetEntityField(iPawnHandle, "CBaseEntity", "m_hGroundEntity");
	// no handle at all: free-floating;  IndexOf answers 0 for a vanished one, which counts as
	// acceptable here as well (a recycled index would be a live ground entity, never index 0)
	if (!IsValidHandle(iGround)) return true;
	return IndexOf(iGround) == 0;
}

// The pawn handle is passed in and checked first: s2sdk's GetClientButtons goes through the pawn's
// movement services and *logs a warning* when they are missing, which would be once per frame for
// a dancing player whose pawn is gone (dead, disconnecting). Without a live pawn the answer is 0,
// exactly like the original with no pawn.
uint64_t GetPressedButtons(int iSlot, int iPawnHandle)
{
	if (!__s2sdk_GetClientButtons || !ValidHandle(iPawnHandle)) return 0;
	try { return s2sdk::GetClientButtons(iSlot, 0); }
	catch (...) { return 0; }
}

uint32_t GetMoveType(int iSlot)
{
	if (!__s2sdk_GetClientMoveType) return 0;
	try { return (uint32_t)s2sdk::GetClientMoveType(iSlot); }
	catch (...) { return 0; }
}

// The original's panicking-free page guard: only read/write a pointer after mincore() confirmed
// the page is mapped (a wrong offset guess must not take the server down).
bool IsPageMapped(const void* pAddr)
{
	static const long s_iPageSize = sysconf(_SC_PAGESIZE);
	const size_t uPageSize = s_iPageSize > 0 ? (size_t)s_iPageSize : 4096;
	const uintptr_t uPage = (uintptr_t)pAddr & ~(uintptr_t)(uPageSize - 1);
	unsigned char vec = 0;
	return mincore((void*)uPage, uPageSize, &vec) == 0;
}

bool SafeReadU64(const void* pAddr, uint64_t& out)
{
	if (!pAddr) return false;
	const uint8_t* pBytes = (const uint8_t*)pAddr;
	if (!IsPageMapped(pBytes) || !IsPageMapped(pBytes + sizeof(uint64_t) - 1)) return false;
	memcpy(&out, pAddr, sizeof(uint64_t));
	return true;
}

bool SafeWriteU64(void* pAddr, uint64_t value)
{
	if (!pAddr) return false;
	uint8_t* pBytes = (uint8_t*)pAddr;
	if (!IsPageMapped(pBytes) || !IsPageMapped(pBytes + sizeof(uint64_t) - 1)) return false;
	memcpy(pAddr, &value, sizeof(uint64_t));
	return true;
}

// One byte (the collision group attribute of the physics attribute block): the original wrote a
// uint8 there, so nothing beyond the field may be touched.
bool SafeWriteU8(void* pAddr, uint8_t value)
{
	if (!pAddr) return false;
	if (!IsPageMapped(pAddr)) return false;
	memcpy(pAddr, &value, sizeof(uint8_t));
	return true;
}

// ---------------------------------------------------------------------------------------------
// The two pawn fields of the per-frame path, resolved once
// ---------------------------------------------------------------------------------------------
// GetAlivePawn() runs three times per frame per dancing player and read m_iHealth and the stored
// m_CBodyComponent pointer through the string-keyed helpers - each call builds the class/field key
// strings and takes the schema table's shared_mutex. Both offsets are resolved once (the first call
// runs behind GetAlivePawn's GetController() gate, i.e. with a live map) and then read raw, the way
// weapons_gloves.cpp reads the glove fields.
struct PawnFieldOffsets
{
	int iHealth = -1; // CBaseEntity::m_iHealth, int32
	int iBody = -1;   // CBaseEntity::m_CBodyComponent, a stored pointer ("ptr" field)
	bool bResolved = false;

	void Resolve()
	{
		if (bResolved) return;
		bResolved = true;
		iHealth = Offset("CBaseEntity", "m_iHealth");
		iBody = Offset("CBaseEntity", "m_CBodyComponent");
	}
};

PawnFieldOffsets g_PawnFields;

int64_t ReadHealth(int iHandle)
{
	PawnFieldOffsets& o = g_PawnFields;
	o.Resolve();
	return o.iHealth >= 0 ? RawGetInt(iHandle, o.iHealth, 4) : 0;
}

// The object the "ptr" field stores (the pointer read of GetComponentPointer in fe_entity.cpp);
// nullptr when the field is missing or the pointer is null.
void* ReadBodyComponent(int iHandle)
{
	PawnFieldOffsets& o = g_PawnFields;
	o.Resolve();
	if (o.iBody < 0) return nullptr;
	return (void*)(uintptr_t)RawGetInt(iHandle, o.iBody, 8);
}

// CBaseAnimGraph::m_bUseAnimGraph: the original resolves the field at runtime and skips the write
// when the build has no such field. This build has none on CBaseAnimGraph (the schema dump carries
// it on CDynamicProp instead - a write there would be a behaviour change nobody has verified, so
// it is not attempted), and every failed s2sdk lookup prints
// "'m_bUseAnimGraph' was not found in 'CBaseAnimGraph'!" plus "Cannot find offset ..." - the setter
// was called twice per emote start, so the field is looked up exactly once (the first call runs
// inside PlayEmote, i.e. with a live map and an initialised schema system) and a missing field is
// remembered for the rest of the plugin's life.
bool HasUseAnimGraphField()
{
	static const bool s_bHas = Offset("CBaseAnimGraph", "m_bUseAnimGraph") >= 0;
	return s_bHas;
}

void SetUseAnimGraph(int iHandle, bool bUseAnimGraph)
{
	if (!HasUseAnimGraphField()) return;
	SetBoolField(iHandle, "CBaseAnimGraph", "m_bUseAnimGraph", bUseAnimGraph, true);
}

// Invisible animation rig: EF_NOSHADOW + fully transparent (SPEC §E.2 SetPropInvisible).
void SetPropInvisible(int iHandle)
{
	constexpr int64_t EF_NOSHADOW = 1ll << 4; // 0x10; 1 << 5 = 0x20 is EF_NODRAW (not transmitted)
	const int64_t iEffects = GetIntField(iHandle, "CBaseEntity", "m_fEffects");
	if (!(iEffects & EF_NOSHADOW))
		SetIntField(iHandle, "CBaseEntity", "m_fEffects", iEffects | EF_NOSHADOW, true);
	SetEntityAlpha(iHandle, 0);
}

// Copy the pawn's mesh group mask onto the clone, so the body matches (skins, charms). Resolved
// through the schema: CBaseEntity::m_CBodyComponent -> CBodyComponent::m_pSceneNode ->
// CSkeletonInstance::m_modelState (CModelState) -> m_MeshGroupMask.
void CopyMeshGroupMask(int iPawnHandle, int iCloneHandle)
{
	const int iModelState = Offset("CSkeletonInstance", "m_modelState");
	const int iMask = Offset("CModelState", "m_MeshGroupMask");
	if (iModelState < 0 || iMask < 0) return;

	// m_CBodyComponent and m_pSceneNode are stored-pointer ("ptr") fields: the objects are the
	// pointers stored in the fields (the original dereferenced the body component and then its
	// scene-node pointer), not the field addresses s2sdk's GetEntSchemaPtr* return.
	void* pPawnBody = GetComponentPointer(iPawnHandle, "CBaseEntity", "m_CBodyComponent");
	void* pCloneBody = GetComponentPointer(iCloneHandle, "CBaseEntity", "m_CBodyComponent");
	if (!pPawnBody || !pCloneBody) return;

	void* pPawnSkelField = __s2sdk_GetEntSchemaPtr2 ? s2sdk::GetEntSchemaPtr2(pPawnBody, "CBodyComponent", "m_pSceneNode", 0) : nullptr;
	void* pCloneSkelField = __s2sdk_GetEntSchemaPtr2 ? s2sdk::GetEntSchemaPtr2(pCloneBody, "CBodyComponent", "m_pSceneNode", 0) : nullptr;
	void* pPawnSkel = pPawnSkelField ? *reinterpret_cast<void**>(pPawnSkelField) : nullptr;
	void* pCloneSkel = pCloneSkelField ? *reinterpret_cast<void**>(pCloneSkelField) : nullptr;
	if (!pPawnSkel || !pCloneSkel) return;

	uint64_t uMask = 0;
	if (!SafeReadU64((uint8_t*)pPawnSkel + iModelState + iMask, uMask) || uMask == 0) return;
	if (!SafeWriteU64((uint8_t*)pCloneSkel + iModelState + iMask, uMask)) return;

	// the body component carries the model state - mark it so the clients rebuild the clone
	StateChanged(iCloneHandle, "CBaseEntity", "m_CBodyComponent");
}

void RemoveProp(int& iHandle)
{
	// a handle may be negative (the serial sets bit 31); only 0/-1 mean "nothing to remove"
	if (!IsValidHandle(iHandle)) return;
	const int iToRemove = iHandle;
	iHandle = 0;
	// The props belonged to the map that just ended: between the map end and the next map start
	// the entity system is torn down and IsValidEntHandle/RemoveEntity would dereference it
	// (every stop path - events, the frame pump, plugin end - can run in that window, and the
	// map end force-stops every dancer). The handle is dropped without the s2sdk call.
	if (!pisex::IsGameReady()) return;
	// only a live handle is passed on: the prop may already be gone (engine cleanup, map change),
	// and s2sdk logs a warning for an invalid handle
	if (!__s2sdk_RemoveEntity || (__s2sdk_IsValidEntHandle && !s2sdk::IsValidEntHandle(iToRemove))) return;
	try { s2sdk::RemoveEntity(iToRemove); }
	catch (...) {}
}

// §E.3: arm the loop switch and/or the stop timer; with no timer at all the emote ends when the
// engine reports OnAnimationDone for the rig.
void ScheduleAnimationEnd(int iSlot, int iEmoteIndex, uint32_t iGeneration)
{
	PlayerState& state = g_States[iSlot];
	state.StopOnAnimationDone = false;

	const Emote& emote = g_Emotes[iEmoteIndex];

	if (!emote.AnimLoop.empty() && emote.AnimLoopAfter > 0)
	{
		const float flLoopAfter = emote.AnimLoopAfter;
		const float flDuration = emote.Duration;
		const std::string szLoop = emote.AnimLoop;

		CreatePluginTimer(flLoopAfter, [iSlot, iGeneration, flDuration, szLoop]() -> float
		{
			PlayerState& s = g_States[iSlot];
			if (s.Generation != iGeneration || !s.IsDancing) return -1.0f;

			if (IsValidHandle(s.Rig))
				AcceptInputString(s.Rig, "SetAnimation", szLoop, 0, 0);

			// the stop timer is armed from the loop switch, total = loop_after + duration
			if (flDuration > 0)
			{
				CreatePluginTimer(flDuration, [iSlot, iGeneration]() -> float
				{
					PlayerState& s2 = g_States[iSlot];
					if (s2.Generation != iGeneration || !s2.IsDancing) return -1.0f;
					StopEmote(iSlot);
					return -1.0f;
				});
			}
			return -1.0f;
		});
	}
	else if (emote.Duration > 0)
	{
		const float flDuration = emote.Duration;
		CreatePluginTimer(flDuration, [iSlot, iGeneration]() -> float
		{
			PlayerState& s = g_States[iSlot];
			if (s.Generation != iGeneration || !s.IsDancing) return -1.0f;
			StopEmote(iSlot);
			return -1.0f;
		});
	}
	else
	{
		state.StopOnAnimationDone = true;
	}
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------------------------

int GetController(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS || !pisex::IsGameReady()) return 0;
	if (!pisex::IsConnected(iSlot)) return 0;
	return pisex::GetControllerEntity(iSlot);
}

int GetAlivePawn(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return 0;
	if (GetController(iSlot) <= 0 || !pisex::IsAlive(iSlot)) return 0;

	const int iPawn = pisex::GetPawnEntity(iSlot);
	if (iPawn <= 0) return 0;
	// one silent validity check here covers every caller (this runs per frame while dancing);
	// s2sdk's entity functions log a warning for an invalid handle
	const int iPawnHandle = Handle(iPawn);
	if (!ValidHandle(iPawnHandle)) return 0;
	if (ReadHealth(iPawnHandle) <= 0) return 0;
	// the animations need the body component; the original required it as well
	if (!ReadBodyComponent(iPawnHandle)) return 0;
	return iPawn;
}

bool IsValidHuman(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS || !pisex::IsGameReady()) return false;
	if (!pisex::IsConnected(iSlot) || !pisex::IsInGame(iSlot) || pisex::IsFakeClient(iSlot)) return false;
	return GetController(iSlot) > 0;
}

bool IsDancing(int iSlot)
{
	return iSlot >= 0 && iSlot < FE_MAX_PLAYERS && g_States[iSlot].IsDancing;
}

bool IsReadyForDancing(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return false;

	// no convar/cooldown checks here (SPEC §E.1 IsReadyForDancing)
	if (g_States[iSlot].IsDancing || !IsAllowedPeriod() || !IsValidHuman(iSlot)) return false;

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return false;
	if (!HasCameraServices(Handle(iPawn))) return false;

	const int64_t fFlags = GetIntField(Handle(iPawn), "CBaseEntity", "m_fFlags");
	constexpr int64_t FL_ONGROUND = 1 << 0;
	constexpr int64_t FL_DUCKING = 1 << 1;
	return (fFlags & FL_ONGROUND) != 0 && IsGroundAcceptable(Handle(iPawn)) && !(fFlags & FL_DUCKING)
		&& !IsScopedPawn(Handle(iPawn));
}

// ---------------------------------------------------------------------------------------------
// Entity helpers (fe.hpp)
// ---------------------------------------------------------------------------------------------

void SetEntityAlpha(int iHandle, uint8_t alpha)
{
	if (!ValidHandle(iHandle)) return; // silent; s2sdk logs for invalid handles
	plg::vec4 color = __s2sdk_GetEntityRenderColor ? s2sdk::GetEntityRenderColor(iHandle) : plg::vec4{255, 255, 255, 255};
	if ((uint8_t)color.w == alpha) return;
	color.w = (float)alpha;
	try { s2sdk::SetEntityRenderColor(iHandle, color); }
	catch (...) {}
}

// Bit 2 of CEntityIdentity::m_flags: cleared by the original on every prop it creates - the
// "don't send to client" hint, EF_IN_STAGING_LIST. The camera prop existed and m_hViewEntity
// pointed at it, yet the dancer's client kept the first-person view. Offsets come from the
// schema (CEntityInstance::m_pEntity is a stored pointer, CEntityIdentity::m_flags a uint32) and
// every address is page-checked.
void ClearIdentityFlag(int iHandle, uint32_t uFlag)
{
	if (!__s2sdk_EntHandleToEntPointer) return;
	const int iIdentityOffset = Offset("CEntityInstance", "m_pEntity");
	const int iFlagsOffset = Offset("CEntityIdentity", "m_flags");
	if (iIdentityOffset < 0 || iFlagsOffset < 0) return;
	void* pEntity = nullptr;
	try { pEntity = s2sdk::EntHandleToEntPointer(iHandle); }
	catch (...) { return; }
	uint64_t uIdentity = 0;
	if (!pEntity || !SafeReadU64((const uint8_t*)pEntity + iIdentityOffset, uIdentity) || uIdentity == 0) return;
	uint8_t* pFlags = (uint8_t*)(uintptr_t)uIdentity + iFlagsOffset;
	if (!IsPageMapped(pFlags) || !IsPageMapped(pFlags + sizeof(uint32_t) - 1)) return;
	uint32_t uFlags = 0;
	memcpy(&uFlags, pFlags, sizeof(uFlags));
	uFlags &= ~uFlag;
	memcpy(pFlags, &uFlags, sizeof(uFlags));
}

void PrepareProp(int iHandle, int solidType)
{
	if (!IsValidHandle(iHandle)) return;
	(void)solidType; // SOLID_NONE: the props are not solid in this port either
	ClearIdentityFlag(iHandle, 1u << 2);

	// CCollisionProperty is a component: nonphysical group, not solid, trigger flags. The
	// original wrote the same four fields through raw offsets.
	void* pCollision = GetComponentPtr(iHandle, "CBaseModelEntity", "m_Collision");
	if (pCollision)
	{
		constexpr int64_t COLLISION_GROUP_NONPHYSICAL = 10;
		SetComponentInt(pCollision, "CCollisionProperty", "m_CollisionGroup", COLLISION_GROUP_NONPHYSICAL, true);
		SetComponentInt(pCollision, "CCollisionProperty", "m_nSolidType", 0 /* SOLID_NONE */, true);
		SetComponentInt(pCollision, "CCollisionProperty", "m_usSolidFlags", 12 /* NOT_SOLID|TRIGGER */, true);
		const int iAttrOffset = Offset("CCollisionProperty", "m_collisionAttribute");
		const int iAttrGroupOffset = Offset("VPhysicsCollisionAttribute_t", "m_nCollisionGroup");
		if (iAttrOffset >= 0 && iAttrGroupOffset >= 0)
			SafeWriteU8((uint8_t*)pCollision + iAttrOffset + iAttrGroupOffset, (uint8_t)COLLISION_GROUP_NONPHYSICAL);
	}
}

int CreateProp(const char* szModel, const plg::vec3& origin, const plg::vec3& angles, const char* szName)
{
	if (!szModel || !szModel[0]) return 0;

	// CreateEntityByName answers a HANDLE (INVALID_EHANDLE_INDEX when it failed), not an index
	int iHandle = 0;
	try { iHandle = s2sdk::CreateEntityByName("prop_dynamic"); }
	catch (...) { return 0; }
	if (!IsValidHandle(iHandle)) return 0;

	// "targetname" is what the engine's name lookup (FollowEntity/FireOutput) resolves against
	plg::vector<plg::string> keys;
	plg::vector<plg::any> values;
	keys.push_back(plg::string("model"));
	values.push_back(plg::any(plg::string(szModel)));
	if (szName && szName[0])
	{
		keys.push_back(plg::string("targetname"));
		values.push_back(plg::any(plg::string(szName)));
	}
	try { s2sdk::DispatchSpawn2(iHandle, keys, values); }
	catch (...) {}
	try { s2sdk::TeleportEntity(iHandle, origin, angles, plg::vec3{0, 0, 0}); }
	catch (...) {}

	return iHandle;
}

// ---------------------------------------------------------------------------------------------
// Start (SPEC §E.1)
// ---------------------------------------------------------------------------------------------

bool PlayEmote(int iSlot, int iEmoteIndex, std::string& error, bool bForced)
{
	error.clear();

	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return false;

	if (!g_bEmoteBindings)
	{
		error = " " + Prefix() + " " + Phrase("emote.unknownerror");
		return false;
	}

	if (iEmoteIndex < 0 || iEmoteIndex >= (int)g_Emotes.size())
	{
		error = " " + Prefix() + " " + Phrase("emote.unknownerror");
		return false;
	}

	const Emote& emote = g_Emotes[iEmoteIndex];

	// 1. allowed period
	if (!IsAllowedPeriod())
	{
		error = " " + Prefix() + " " + Phrase(g_Settings.EmoteAllowedPeriod == 1
			? "emote.notallowed.warmupftcheck" : "emote.notallowed.warmupftrecheck");
		return false;
	}

	// 3. valid + alive + fully spawned
	const int iPawn = GetAlivePawn(iSlot);
	if (GetController(iSlot) <= 0 || iPawn <= 0 || !IsValidHuman(iSlot))
	{
		error = " " + Prefix() + " " + Phrase(bForced ? "emote.player.alivecheck" : "emote.alivecheck");
		return false;
	}
	const int iPawnHandle = Handle(iPawn);
	if (!HasCameraServices(iPawnHandle))
	{
		error = " " + Prefix() + " " + Phrase(bForced ? "emote.player.alivecheck" : "emote.alivecheck");
		return false;
	}

	// 4. not scoping
	if (IsScopedPawn(iPawnHandle))
	{
		error = " " + Prefix() + " " + Phrase(bForced ? "emote.player.scopecheck" : "emote.scopecheck");
		return false;
	}

	PlayerState& state = g_States[iSlot];

	// 6. cooldown (skipped for admin-forced plays)
	if (!bForced)
	{
		const double flNow = FE_Now();
		if (state.CooldownUntil > flNow)
		{
			const int iSecondsLeft = (int)(state.CooldownUntil - flNow);
			error = " " + Prefix() + " " + FormatPhrase("emote.cooldowncheck", { std::to_string(iSecondsLeft) });
			return false;
		}
	}

	// 7./8. ground, then ducking
	const int64_t fFlags = GetIntField(iPawnHandle, "CBaseEntity", "m_fFlags");
	constexpr int64_t FL_ONGROUND = 1 << 0;
	constexpr int64_t FL_DUCKING = 1 << 1;
	if (!(fFlags & FL_ONGROUND) || !IsGroundAcceptable(iPawnHandle))
	{
		error = " " + Prefix() + " " + Phrase(bForced ? "emote.player.groundcheck" : "emote.groundcheck");
		return false;
	}

	if (fFlags & FL_DUCKING)
	{
		error = " " + Prefix() + " " + Phrase(bForced ? "emote.player.duckcheck" : "emote.duckcheck");
		return false;
	}

	// 10. master switch
	if (!g_bEnabled)
	{
		error = " " + Prefix() + " " + Phrase(bForced ? "emote.player.disabledbyadmin" : "emote.disabledbyadmin");
		return false;
	}

	// 11. restart
	if (state.IsDancing) StopEmote(iSlot);

	// 12. model (the pawn's model name; s2sdk reads it for the client directly)
	std::string szModel;
	try
	{
		const plg::string sModel = s2sdk::GetClientModel(iSlot);
		szModel.assign(sModel.data(), sModel.size());
	}
	catch (...)
	{
	}
	if (szModel.empty())
	{
		error = " " + Prefix() + " " + Phrase("emote.unknownerror");
		return false;
	}

	const plg::vec3 vecOrigin = __s2sdk_GetEntityAbsOrigin ? s2sdk::GetEntityAbsOrigin(iPawnHandle) : plg::vec3{};
	const plg::vec3 angRotation = __s2sdk_GetEntityAngRotation ? s2sdk::GetEntityAngRotation(iPawnHandle) : plg::vec3{};

	// 13. invisible animation rig
	char szRigName[32];
	snprintf(szRigName, sizeof(szRigName), "emoteEnt_%d", 1000000 + std::rand() % 8999999);

	const int iRig = CreateProp(szModel.c_str(), vecOrigin, angRotation, szRigName);
	if (!IsValidHandle(iRig))
	{
		error = " " + Prefix() + " " + Phrase("emote.unknownerror");
		return false;
	}

	PrepareProp(iRig, 0);
	SetUseAnimGraph(iRig, false);
	SetPropInvisible(iRig);

	// 14. visible clone (bone-merges onto the rig)
	char szCloneName[64];
	snprintf(szCloneName, sizeof(szCloneName), "%s_clone", szRigName);

	const int iClone = CreateProp(szModel.c_str(), vecOrigin, angRotation, szCloneName);
	if (!IsValidHandle(iClone))
	{
		try { s2sdk::RemoveEntity(iRig); } catch (...) {}
		error = " " + Prefix() + " " + Phrase("emote.unknownerror");
		return false;
	}

	PrepareProp(iClone, 0);
	CopyMeshGroupMask(iPawnHandle, iClone);

	if (g_Settings.EmoteGlovesFix)
		AcceptInputString(iClone, "SetBodygroup", "first_or_third_person,0", 0, 0);

	SetUseAnimGraph(iClone, false);
	SetEntityAlpha(iClone, 255);

	// FollowEntity drives the clone from the rig's bones: the "!activator" spelling with the rig
	// as the activator is the proven form
	AcceptInputString(iClone, "FollowEntity", "!activator", iRig, iClone);

	// 15. state (alpha captured before IsDancing - the tick loop may hide the pawn first)
	if (g_Settings.EmoteFreezePlayer)
	{
		// MOVETYPE_WALK is 2 in this build (the original's MOVETYPE_WALK); the stop writes
		// PreviousMoveType back, so 0 (MOVETYPE_NONE = frozen) must never be stored here
		state.PreviousMoveType = IsMenuOpenFor(iSlot) ? 2 /* MOVETYPE_WALK */ : (int)GetMoveType(iSlot);
		pisex::SetMoveType(iSlot, 1 /* MOVETYPE_OBSOLETE */);
	}

	state.EmoteIndex = iEmoteIndex;
	state.Rig = iRig;
	state.Clone = iClone;
	state.RigName = szRigName;
	state.PlayerAlpha = (uint8_t)(__s2sdk_GetEntityRenderColor ? s2sdk::GetEntityRenderColor(iPawnHandle).w : 255.0f);
	state.StopOnAnimationDone = false;
	state.StartedAt = FE_Now();
	state.PrevButtons = 0;
	state.CurrentSound.clear();
	state.IsDancing = true;
	state.Generation++;
	const uint32_t iGeneration = state.Generation;

	// 16./17. gloves + weapons
	HideEmoteGloves(iSlot);
	ClearPlayerWeapons(iSlot);

	// 18. rig takes the emote model and the intro sequence (not in the spawn frame)
	const std::string szEmoteModel = emote.Model;
	const std::string szIntro = emote.AnimIntro;
	pisex::NextFrame([iSlot, iGeneration, szEmoteModel, szIntro]()
	{
		PlayerState& s = g_States[iSlot];
		if (s.Generation != iGeneration || !s.IsDancing) return;

		const int iRigEnt = s.Rig;
		if (!ValidHandle(iRigEnt)) return;

		if (!szEmoteModel.empty() && __s2sdk_SetEntityModel)
		{
			try { s2sdk::SetEntityModel(iRigEnt, plg::string(szEmoteModel)); }
			catch (...) {}
		}
		if (!szIntro.empty()) AcceptInputString(iRigEnt, "SetAnimation", szIntro, 0, 0);
	});

	// 19. animation timers
	ScheduleAnimationEnd(iSlot, iEmoteIndex, iGeneration);

	// 20. camera
	state.Camera = SetCam(iSlot);

	// 21. cooldown arm
	if (!bForced)
	{
		const int iCooldown = GetCooldownSeconds(iSlot);
		state.CooldownUntil = FE_Now() + (iCooldown <= 0 ? 0.1 : (double)iCooldown);
	}

	// 22. print
	Print(iSlot, " " + Prefix() + " " + FormatPhrase("emote.playing", { EmoteDisplayName(emote) }));

	// 23. sound
	if (g_Settings.SoundModuleEnabled && !emote.Sound.empty()) EmitEmoteSound(iSlot);

	// 24. hide other players
	ApplyHidePlayers(iSlot);

	// hide the pawn (alpha 0) once the clone is there to replace it
	pisex::NextFrame([iSlot, iGeneration]()
	{
		PlayerState& s = g_States[iSlot];
		if (s.Generation != iGeneration || !s.IsDancing) return;

		const int iNow = pisex::GetPawnEntity(iSlot);
		if (iNow > 0) SetEntityAlpha(Handle(iNow), 0);
	});

	return true;
}

// ---------------------------------------------------------------------------------------------
// Stop (SPEC §E.11)
// ---------------------------------------------------------------------------------------------

void StopEmote(int iSlot, bool bForce)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	PlayerState& state = g_States[iSlot];
	if (!state.IsDancing && !bForce) return;

	// A forced stop of a slot that never danced has nothing to undo: every snapshot below is taken
	// during a dance and consumed by the stop, and the entity/schema work would otherwise run for
	// every bot at map start (player_spawn/player_death force the stop for all slots). That path is
	// what ran ResetCam's m_hViewEntity write on the first frame after a changelevel - the crash
	// this guard prevents (see ResetCam). Only the bookkeeping is dropped here.
	if (!state.IsDancing)
	{
		DebugLog("StopEmote slot %d (not dancing, bookkeeping only)", iSlot);
		state.BlockedTransmit.clear();
		return;
	}

	DebugLog("StopEmote slot %d", iSlot);

	state.IsDancing = false;
	state.Generation++; // cancels the generation-guarded timers/NextFrame callbacks

	RestoreEmoteGloves(iSlot);
	StopEmoteSound(iSlot);

	const int iPawn = GetAlivePawn(iSlot);

	ResetCam(iSlot);
	GivePlayerWeaponsBack(iSlot); // always consumes the snapshot

	if (iPawn > 0)
	{
		SetEntityAlpha(Handle(iPawn), state.PlayerAlpha);
		if (!IsMenuOpenFor(iSlot) && g_States[iSlot].PreviousMoveType != (int)GetMoveType(iSlot))
			pisex::SetMoveType(iSlot, state.PreviousMoveType);
		ResetPlayerAttackTicks(iSlot);
	}

	ClearHidePlayers(iSlot);
	state.BlockedTransmit.clear();

	RemoveProp(state.Rig);
	RemoveProp(state.Clone);
	RemoveProp(state.Camera);

	state.EmoteIndex = -1;
	state.RigName.clear();
	state.PlayerAlpha = 255;
	state.PreviousMoveType = 2; // MOVETYPE_WALK, the original's reset value
	state.StopOnAnimationDone = false;
	state.StartedAt = 0;
	state.CurrentSound.clear();
	state.PrevButtons = 0;
	// state.CooldownUntil survives the stop (SPEC §E.11 step 11)
}

void StopAllEmotes()
{
	for (int iSlot = 0; iSlot < FE_MAX_PLAYERS; ++iSlot)
		if (g_States[iSlot].IsDancing) StopEmote(iSlot);
}

void ResetPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	StopEmote(iSlot, true);
	DropGloveSnapshot(iSlot);
	g_States[iSlot] = PlayerState{};
}

// ---------------------------------------------------------------------------------------------
// Tick loop (SPEC §E.9) and OnAnimationDone
// ---------------------------------------------------------------------------------------------

void OnFrame()
{
	for (int iSlot = 0; iSlot < FE_MAX_PLAYERS; ++iSlot)
	{
		PlayerState& state = g_States[iSlot];
		if (!state.IsDancing) continue;

		if (GetController(iSlot) <= 0)
		{
			StopEmote(iSlot, true);
			continue;
		}

		const int iPawn = pisex::GetPawnEntity(iSlot);
		const int iPawnHandle = ValidHandle(Handle(iPawn)) ? Handle(iPawn) : 0;
		const uint64_t uButtons = GetPressedButtons(iSlot, iPawnHandle);

		// 3. forced visibility
		if (g_Settings.ForcePlayerVisibility) SetEntityAlpha(iPawnHandle, 0);

		// 4. alive-only maintenance
		if (IsValidHandle(iPawnHandle) && pisex::IsAlive(iSlot) && ReadHealth(iPawnHandle) > 0)
		{
			HideEmoteGloves(iSlot);

			if (g_Settings.EmoteFreezePlayer)
			{
				const plg::vec3 vecVelocity = __s2sdk_GetClientAbsVelocity ? s2sdk::GetClientAbsVelocity(iSlot) : plg::vec3{};
				if (std::round(sqrtf(vecVelocity.x * vecVelocity.x + vecVelocity.y * vecVelocity.y)) > 350.0f)
				{
					state.PrevButtons = uButtons;
					StopEmote(iSlot);
					continue;
				}
			}

			if (!IsMenuOpenFor(iSlot) && g_Settings.CancelButtons != 0)
			{
				const uint64_t uPressed = uButtons & ~state.PrevButtons;
				if (uPressed & g_Settings.CancelButtons)
				{
					state.PrevButtons = uButtons;
					StopEmote(iSlot);
					continue;
				}
			}

			BlockPlayerAttack(iSlot);
		}

		state.PrevButtons = uButtons;

		// 5. camera (the function itself bails on dead players)
		if (g_Settings.CameraMoveable && IsValidHandle(state.Camera)) UpdateCamera(iSlot);

		// 6. rig follow for walkable emotes
		if (!g_Settings.EmoteFreezePlayer && IsValidHandle(state.Rig)) UpdateRigFollow(iSlot);

		// safety net: an emote waiting for an OnAnimationDone that never comes
		if (state.StopOnAnimationDone && g_Settings.MaxEmoteSeconds > 0.0f
			&& FE_Now() - state.StartedAt > (double)g_Settings.MaxEmoteSeconds)
			StopEmote(iSlot);
	}
}

void OnRigAnimationDone(int iRigHandle)
{
	if (!IsValidHandle(iRigHandle)) return; // the output hook hands out handles (may be negative)

	for (int iSlot = 0; iSlot < FE_MAX_PLAYERS; ++iSlot)
	{
		PlayerState& state = g_States[iSlot];
		if (!state.IsDancing || !state.StopOnAnimationDone) continue;
		if (state.Rig != iRigHandle) continue;

		DebugLog("OnAnimationDone (slot %d)", iSlot);

		// never remove entities inside their own output dispatch: stop on the next frame
		const uint32_t iGeneration = state.Generation;
		pisex::NextFrame([iSlot, iGeneration]()
		{
			PlayerState& s = g_States[iSlot];
			if (s.Generation != iGeneration || !s.IsDancing) return;
			StopEmote(iSlot);
		});
		return;
	}
}
