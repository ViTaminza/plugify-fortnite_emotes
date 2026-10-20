// fortnite_emotes — weapons, attack blocking and econ gloves (the original's weapons_gloves.cpp:
// SPEC §E.5, §E.7, bugs H-1, H-4, H-5).
//
// Portable parts:
//   * the loadout is snapshotted (classname, clip, reserve ammo, whole ammo array), the weapons are
//     dropped and removed, and given back when the emote stops; the snapshot is always consumed.
//   * the attack ticks of the active weapon are pushed into the future while the emote runs.
//   * the econ gloves are hidden by zeroing CEconItemView::m_iItemDefinitionIndex / m_bInitialized
//     inside CCSPlayerPawn::m_EconGloves (an inline schema class: the write goes through the raw
//     offsets of the pawn, exactly like the original's raw references), followed by the glove
//     counters and the bodygroup rebuild that makes the clients drop the old glove model.
//
// The original's item-definition-index -> classname table is gone: s2sdk hands out the entity
// classname directly, which is what GiveNamedItem() takes.
#include "fe.hpp"

#include <string>
#include <vector>

namespace {

// SPEC §E.5 assumes 64 tick/s: skip the write while the secondary attack is blocked this far ahead.
constexpr int32_t kAttackBlockThreshold = 64 * 60;
constexpr int32_t kAttackBlockFor = 64 * 120;

// SPEC §E.7: bodygroup 0 now, bodygroup 1 once the new glove state has replicated.
constexpr float kGloveRebuildDelay = 0.2f;

std::string ClassnameOf(int iHandle)
{
	try
	{
		const plg::string s = s2sdk::GetEntityClassname(iHandle);
		return std::string(s.data(), s.size());
	}
	catch (...)
	{
		return {};
	}
}

// The econ gloves are an inline CEconItemView inside the pawn: field offsets are the pawn offset of
// m_EconGloves plus the offset inside CEconItemView.
struct GloveOffsets
{
	int iGloves = -1;
	int iDefIndex = -1;
	int iInitialized = -1;
	int iItemID = -1;
	bool bResolved = false;

	void Resolve()
	{
		if (bResolved) return;
		bResolved = true;
		iGloves = Offset("CCSPlayerPawn", "m_EconGloves");
		iDefIndex = Offset("CEconItemView", "m_iItemDefinitionIndex");
		iInitialized = Offset("CEconItemView", "m_bInitialized");
		iItemID = Offset("CEconItemView", "m_iItemID");
	}

	bool Valid() const { return iGloves >= 0 && iDefIndex >= 0 && iInitialized >= 0; }
};

GloveOffsets g_Gloves;

// The attack-block check runs every frame while a player dances (OnFrame -> BlockPlayerAttack);
// the typed getter built the class+field schema-key strings per call, so the leaf offsets are
// resolved once and the values read raw. The writes below stay on the typed (announcing) setters:
// they only happen when the block has to be re-armed.
struct AttackTickOffsets
{
	int iSecondary = -1; // CBasePlayerWeapon::m_nNextSecondaryAttackTick, int32
	bool bResolved = false;

	void Resolve()
	{
		if (bResolved) return;
		bResolved = true;
		iSecondary = Offset("CBasePlayerWeapon", "m_nNextSecondaryAttackTick");
	}
};

AttackTickOffsets g_AttackTicks;

int32_t ReadSecondaryAttackTick(int iWeaponHandle)
{
	AttackTickOffsets& o = g_AttackTicks;
	o.Resolve();
	if (o.iSecondary < 0) return 0;
	return (int32_t)RawGetInt(iWeaponHandle, o.iSecondary, 4);
}

int ChainOffsetOfPawn()
{
	static int s_iChain = -2;
	if (s_iChain == -2) s_iChain = ChainOffset("CCSPlayerPawn");
	return s_iChain;
}

uint16_t ReadGloveDefIndex(int iPawnHandle)
{
	GloveOffsets& o = g_Gloves;
	o.Resolve();
	if (!o.Valid()) return 0;
	return (uint16_t)RawGetInt(iPawnHandle, o.iGloves + o.iDefIndex, 2);
}

bool ReadGloveInitialized(int iPawnHandle)
{
	GloveOffsets& o = g_Gloves;
	o.Resolve();
	if (!o.Valid() || o.iInitialized < 0) return false;
	return RawGetInt(iPawnHandle, o.iGloves + o.iInitialized, 1) != 0;
}

uint64_t ReadGloveItemID(int iPawnHandle)
{
	GloveOffsets& o = g_Gloves;
	o.Resolve();
	if (!o.Valid() || o.iItemID < 0) return 0;
	return (uint64_t)RawGetInt(iPawnHandle, o.iGloves + o.iItemID, 8);
}

void WriteGloveFields(int iPawnHandle, uint16_t iDefIndex, bool bInitialized)
{
	GloveOffsets& o = g_Gloves;
	o.Resolve();
	if (!o.Valid()) return;
	const int iChain = ChainOffsetOfPawn();
	// The per-leaf announcements are the networked part (the original announced each changed leaf
	// field), and the chain offset is the pawn's own
	// (CCSPlayerPawn has no __m_pChainEntity, so ChainOffset answers -1 and the pawn is the entity).
	RawSetInt(iPawnHandle, o.iGloves + o.iDefIndex, iDefIndex, 2, /*changeState=*/true, iChain);
	if (o.iInitialized >= 0)
		RawSetInt(iPawnHandle, o.iGloves + o.iInitialized, bInitialized ? 1 : 0, 1, /*changeState=*/true, iChain);

	// m_nEconGlovesChanged: the counter move is what makes the client rebuild the glove model, and
	// it is a networked leaf field. Announcing m_EconGloves itself (the container) is what made the
	// engine print "Couldn't resolve offset 4904 in CCSPlayerPawn" plus "requested resolve all N
	// changes, actually resolved only M" and the whole send-table dump on every emote - only leaf
	// fields may be announced (the WeaponSkins BumpEconGlovesChanged recipe, engine.cpp:1412).
	int64_t iChanged = GetIntField(iPawnHandle, "CCSPlayerPawn", "m_nEconGlovesChanged");
	SetIntField(iPawnHandle, "CCSPlayerPawn", "m_nEconGlovesChanged", iChanged + 1, true);
}

// Clients only rebuild the glove model on a mesh-group (bodygroup) change (SPEC §E.7).
void RebuildClientGloves(int iPawnHandle)
{
	if (!IsValidHandle(iPawnHandle)) return; // a handle may be negative (the serial sets bit 31)

	AcceptInputString(iPawnHandle, "SetBodygroup", "first_or_third_person,0", iPawnHandle, iPawnHandle);

	// plugin-owned timer: StopAllEmotes() schedules these during unload too, RemovePluginTimers
	// takes whichever is still live down
	const int iPawn = iPawnHandle;
	CreatePluginTimer(kGloveRebuildDelay, [iPawn]() -> float
	{
		// the handle carries the serial, so a recycled entity index cannot pass: IsValidEntHandle()
		// is only true while this very entity is alive, and IsEntityPlayerPawn() confirms the class
		// ("cs_player_pawn" is not the engine's class name - s2sdk answers that question)
		if (__s2sdk_IsValidEntHandle && s2sdk::IsValidEntHandle(iPawn)
			&& __s2sdk_IsEntityPlayerPawn && s2sdk::IsEntityPlayerPawn(iPawn))
			AcceptInputString(iPawn, "SetBodygroup", "first_or_third_person,1", iPawn, iPawn);
		return -1.0f;
	});
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Weapons (SPEC §E.5)
// ---------------------------------------------------------------------------------------------

void ClearPlayerWeapons(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return;
	const int iPawnHandle = Handle(iPawn);

	PlayerState& state = g_States[iSlot];
	// restart safety: never merge an old snapshot into a new one
	state.HasWeaponSnapshot = false;
	state.Weapons.clear();
	state.Ammo.clear();

	std::vector<int> vWeapons;
	try
	{
		// GetClientWeapons hands out the m_hMyWeapons HANDLES (may be negative once the serial
		// passed 65535); an empty slot reads as 0 and an unset one as -1
		const plg::vector<int32_t> weapons = s2sdk::GetClientWeapons(iSlot);
		for (int32_t h : weapons)
			if (IsValidHandle(h)) vWeapons.push_back(h);
	}
	catch (...)
	{
	}

	for (int iWeapon : vWeapons)
	{
		// a weapon that vanished this very frame would only produce invalid-handle warnings
		if (!ValidHandle(iWeapon)) continue;
		WeaponEntry entry;
		entry.Classname = ClassnameOf(iWeapon);
		if (entry.Classname.empty()) continue;
		entry.Clip = (int)GetIntField(iWeapon, "CBasePlayerWeapon", "m_iClip1");
		// H-5: not every weapon has a reserve ammo array (knife, grenades, ...)
		const int iReserve = (int)GetIntField(iWeapon, "CBasePlayerWeapon", "m_pReserveAmmo", 0, 0);
		if (iReserve >= 0) entry.Reserve = iReserve;
		state.Weapons.push_back(std::move(entry));
	}

	// whole ammo array - captured before the weapons are dropped
	void* pWeaponServices = GetComponentPointer(iPawnHandle, "CBasePlayerPawn", "m_pWeaponServices");
	if (pWeaponServices && __s2sdk_GetEntSchemaArraySize2)
	{
		int iCount = 32;
		try
		{
			const int iSize = s2sdk::GetEntSchemaArraySize2(pWeaponServices, "CPlayer_WeaponServices", "m_iAmmo");
			if (iSize > 0 && iSize <= 128) iCount = iSize;
		}
		catch (...)
		{
		}
		for (int i = 0; i < iCount; i++)
			state.Ammo.push_back((uint16_t)GetComponentInt(pWeaponServices, "CPlayer_WeaponServices", "m_iAmmo", 0, i));
	}

	// committed before the strip: even if a drop fails half-way the snapshot is restored on stop
	state.HasWeaponSnapshot = true;

	for (int iWeapon : vWeapons)
	{
		if (!ValidHandle(iWeapon)) continue; // vanished meanwhile: silently skipped
		try
		{
			// drop first, then remove: the engine detaches the weapon cleanly
			s2sdk::DropWeapon(iSlot, iWeapon, plg::vec3{0, 0, 0}, plg::vec3{0, 0, 0});
			s2sdk::RemoveEntity(iWeapon);
		}
		catch (...)
		{
		}
	}
}

void GivePlayerWeaponsBack(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	PlayerState& state = g_States[iSlot];
	if (!state.HasWeaponSnapshot) return;

	// the pawn must exist even when it is dead: the snapshot is dropped either way (H-1)
	const int iPawn = GetAlivePawn(iSlot);
	const int iPawnHandle = iPawn > 0 ? Handle(iPawn) : 0;
	void* pWeaponServices = IsValidHandle(iPawnHandle) ? GetComponentPointer(iPawnHandle, "CBasePlayerPawn", "m_pWeaponServices") : nullptr;

	if (!IsValidHandle(iPawnHandle) || !pWeaponServices)
	{
		DebugLog("GivePlayerWeaponsBack: slot %d cannot receive weapons, dropping the snapshot", iSlot);
	}
	else
	{
		for (const WeaponEntry& entry : state.Weapons)
		{
			int iWeapon = 0;
			try { iWeapon = s2sdk::GiveNamedItem(iSlot, plg::string(entry.Classname)); }
			catch (...) { iWeapon = 0; }
			if (!ValidHandle(iWeapon))
			{
				LogWarning("GivePlayerWeaponsBack: failed to give %s to slot %d", entry.Classname.c_str(), iSlot);
				continue;
			}

			SetIntField(iWeapon, "CBasePlayerWeapon", "m_iClip1", entry.Clip, true);
			SetIntField(iWeapon, "CBasePlayerWeapon", "m_pReserveAmmo", entry.Reserve, true, 0);
		}

		// restore the ammo array by index
		const int nAmmoCount = (int)state.Ammo.size();
		for (int i = 0; i < nAmmoCount; i++)
			SetComponentInt(pWeaponServices, "CPlayer_WeaponServices", "m_iAmmo", state.Ammo[i], false, i);
		if (nAmmoCount > 0)
			SetComponentInt(pWeaponServices, "CPlayer_WeaponServices", "m_iAmmo", 0, /*changeState=*/true, 0);
	}

	state.HasWeaponSnapshot = false;
	state.Weapons.clear();
	state.Ammo.clear();
}

void BlockPlayerAttack(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return;

	int iWeapon = 0;
	try { iWeapon = s2sdk::GetClientActiveWeapon(iSlot); }
	catch (...) { return; }
	if (!ValidHandle(iWeapon)) return;

	pisex::GlobalVars* pGlobals = pisex::GetCGlobalVars();
	if (!pGlobals) return;
	const int32_t iTick = pGlobals->tickcount;

	if (ReadSecondaryAttackTick(iWeapon) > iTick + kAttackBlockThreshold)
		return; // already blocked

	const int32_t iUntil = iTick + kAttackBlockFor;
	SetIntField(iWeapon, "CBasePlayerWeapon", "m_nNextPrimaryAttackTick", iUntil, true);
	SetIntField(iWeapon, "CBasePlayerWeapon", "m_nNextSecondaryAttackTick", iUntil, true);
}

void ResetPlayerAttackTicks(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return;

	int iWeapon = 0;
	try { iWeapon = s2sdk::GetClientActiveWeapon(iSlot); }
	catch (...) { return; }
	if (!ValidHandle(iWeapon)) return;

	SetIntField(iWeapon, "CBasePlayerWeapon", "m_nNextPrimaryAttackTick", -1, true);
	SetIntField(iWeapon, "CBasePlayerWeapon", "m_nNextSecondaryAttackTick", -1, true);
}

// ---------------------------------------------------------------------------------------------
// Econ gloves (SPEC §E.7)
// ---------------------------------------------------------------------------------------------

void HideEmoteGloves(int iSlot)
{
	if (!g_Settings.EmoteGlovesFix) return;
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return;
	const int iPawnHandle = Handle(iPawn);
	if (!ValidHandle(iPawnHandle)) return; // called every frame while dancing

	PlayerState& state = g_States[iSlot];
	const uint32_t hPawn = (uint32_t)iPawnHandle;

	// H-4: a snapshot of another pawn (state survived a silent pawn replacement) is dropped
	if (state.Gloves.Valid && state.Gloves.PawnHandle != hPawn) state.Gloves = GloveSnapshot();

	// (re-)capture every tick: gloves re-equipped mid-dance get hidden again
	const uint16_t iDefIndex = ReadGloveDefIndex(iPawnHandle);
	if (iDefIndex != 0)
	{
		state.Gloves.Valid = true;
		state.Gloves.PawnHandle = hPawn;
		state.Gloves.DefIndex = iDefIndex;
		state.Gloves.Initialized = ReadGloveInitialized(iPawnHandle);
		state.Gloves.ItemID = ReadGloveItemID(iPawnHandle);
	}

	if (!state.Gloves.Valid) return;
	if (iDefIndex == 0 && !ReadGloveInitialized(iPawnHandle)) return; // already hidden

	WriteGloveFields(iPawnHandle, 0, false);
	RebuildClientGloves(iPawnHandle);
}

void RestoreEmoteGloves(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	PlayerState& state = g_States[iSlot];
	const GloveSnapshot snapshot = state.Gloves; // take...
	state.Gloves = GloveSnapshot();              // ...and clear, whatever happens below
	if (!snapshot.Valid) return;

	// Map end to next map start: the pawn (and with it the glove state) died with the old map;
	// the entity reads/writes below would dereference the torn-down entity system. The snapshot
	// is dropped instead of restored.
	if (!pisex::IsGameReady()) return;

	// A player who died with an emote running must not get the hidden glove written back onto the
	// corpse (RestoreEmoteGloves runs from StopEmote, which player_death triggers while the dying
	// pawn and the ragdoll are still being processed). The snapshot was already consumed above, so
	// this is a drop, never a later restore.
	if (!pisex::IsAlive(iSlot)) return;

	const int iPawnHandle = snapshot.PawnHandle; // stored raw (uint32): 0 = no snapshot
	if (!IsValidHandle(iPawnHandle) || IndexOf(iPawnHandle) <= 0) return;

	// restore only what was hidden: if another plugin owns the gloves now, drop the snapshot
	if (ReadGloveDefIndex(iPawnHandle) != 0 || ReadGloveInitialized(iPawnHandle)
		|| ReadGloveItemID(iPawnHandle) != snapshot.ItemID)
		return;

	WriteGloveFields(iPawnHandle, snapshot.DefIndex, snapshot.Initialized);
	RebuildClientGloves(iPawnHandle);
}

void DropGloveSnapshot(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	g_States[iSlot].Gloves = GloveSnapshot();
}
