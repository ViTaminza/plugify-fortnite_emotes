// fortnite_emotes — engine hooks, transmit hiding and sound (the original's hooks.cpp: SPEC §E.3,
// §E.4, §E.8, §E.10).
//
// The original detoured CEntityIOOutput::FireOutputInternal with funchook and SourceHook'ed
// CheckTransmit / OnBuildGameSessionManifest. This port uses the s2sdk equivalents:
//   * OnAnimationDone  -> s2sdk::HookEntityOutput("*", "OnAnimationDone", cb, HookMode::Post)
//     (the entity output manager of s2sdk hooks the same engine function; the callback receives the
//     activator/caller handles),
//   * CheckTransmit    -> s2sdk::OnServerCheckTransmit_Register (per client info, with the
//     transmit-info helpers of s2sdk::transmit),
//   * precache         -> s2sdk::OnBuildGameSessionManifest_Register + s2sdk::Precache,
//   * StopDamageWhenInEmote has no s2sdk equivalent (there is no TakeDamage hook): the setting is
//     NOT ported and logs a warning when it is enabled.
#include "fe.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool g_bHooksInstalled = false;

// ---------------------------------------------------------------------------------------------
// OnAnimationDone (SPEC §E.3)
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnAnimationDoneOutput(int32_t, int32_t iCaller, float)
{
	// a callback of s2sdk (it runs under the output manager's lock): nothing may unwind out of here
	try
	{
		// only the rig of a dancing player is interesting; the caller is a handle (a negative one
		// is valid), and a caller that vanished cannot be a rig of ours
		if (!IsValidHandle(iCaller)) return s2sdk::ResultType::Continue;
		OnRigAnimationDone(iCaller);
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

// ---------------------------------------------------------------------------------------------
// Transmit hiding (SPEC §E.10): the blocked entity handles live in g_States[slot].BlockedTransmit
// ---------------------------------------------------------------------------------------------

void OnCheckTransmit(const plg::vector<void*>& infos)
{
	// a callback of s2sdk: nothing may unwind out of here
	try
	{
		if (!__s2sdk_GetTransmitInfoPlayerSlot || !__s2sdk_ClearTransmitInfoEntity || !__s2sdk_IsValidEntHandle)
			return;

		for (void* pInfo : infos)
		{
			if (!pInfo) continue;
			const int iSlot = s2sdk::GetTransmitInfoPlayerSlot(pInfo);
			if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) continue;

			const PlayerState& state = g_States[iSlot];
			if (!state.IsDancing || state.BlockedTransmit.empty()) continue;

			const int iOwnPawn = Handle(pisex::GetPawnEntity(iSlot));
			for (int iBlocked : state.BlockedTransmit)
			{
				// handles from ApplyHidePlayers; a vanished entity simply disappears from the list
				if (!IsValidHandle(iBlocked) || iBlocked == iOwnPawn) continue;
				if (!s2sdk::IsValidEntHandle(iBlocked)) continue;
				s2sdk::ClearTransmitInfoEntity(pInfo, iBlocked);
			}
		}
	}
	catch (...)
	{
	}
}

// ---------------------------------------------------------------------------------------------
// Precache on map load
// ---------------------------------------------------------------------------------------------

void OnBuildManifest()
{
	// a callback of s2sdk: nothing may unwind out of here
	try
	{
		if (!__s2sdk_Precache) return;
		for (const std::string& sResource : GetPrecacheList())
		{
			if (sResource.empty()) continue;
			s2sdk::Precache(plg::string(sResource));
		}
	}
	catch (...)
	{
	}
}

// ---------------------------------------------------------------------------------------------
// Sound (SPEC §E.4)
// ---------------------------------------------------------------------------------------------

// 0 = only the dancer (the original's behaviour), 1 = everybody. The dancer is always included.
std::vector<int> GetSoundRecipients(int iSlot)
{
	std::vector<int> vRecipients;
	if (g_Settings.SoundRecipients == 1)
	{
		for (int i = 0; i < FE_MAX_PLAYERS; i++)
			if (IsValidHuman(i)) vRecipients.push_back(i);
	}
	if (std::find(vRecipients.begin(), vRecipients.end(), iSlot) == vRecipients.end())
		vRecipients.push_back(iSlot);
	return vRecipients;
}

void StartSoundLoop(int iSlot, uint32_t uGeneration, float flInterval)
{
	CreatePluginTimer(flInterval, [iSlot, uGeneration, flInterval]() -> float
	{
		PlayerState& state = g_States[iSlot];
		if (!state.IsDancing || state.Generation != uGeneration || !g_Settings.SoundModuleEnabled) return -1.0f;
		if (state.EmoteIndex < 0 || state.EmoteIndex >= (int)g_Emotes.size()) return -1.0f;

		const Emote& emote = g_Emotes[state.EmoteIndex];
		if (emote.Sound.empty() || state.CurrentSound != emote.Sound) return -1.0f;

		const int iPawn = pisex::GetPawnEntity(iSlot);
		if (iPawn <= 0) return -1.0f;

		const std::vector<int> vRecipients = GetSoundRecipients(iSlot);
		if (vRecipients.empty()) return -1.0f;

		plg::vector<int32_t> vSlots(vRecipients.begin(), vRecipients.end());
		const int iHandle = Handle(iPawn);
		if (!ValidHandle(iHandle)) return -1.0f;
		try { s2sdk::EmitSoundToClient(iHandle, vSlots, plg::string(emote.Sound), emote.SoundVolume, 100.0f); }
		catch (...) {}
		return flInterval;
	});
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public interface (fe.hpp)
// ---------------------------------------------------------------------------------------------

bool InstallHooks()
{
	if (g_bHooksInstalled) return true;

	bool bAllOk = true;

	// 1. OnAnimationDone. A missing hook is not fatal: the emote stops through MaxEmoteSeconds.
	if (!__s2sdk_HookEntityOutput)
	{
		bAllOk = false;
		LogWarning("s2sdk HookEntityOutput is missing, OnAnimationDone stops are disabled");
	}
	else
	{
		try
		{
			if (!s2sdk::HookEntityOutput(plg::string("*"), plg::string("OnAnimationDone"), &OnAnimationDoneOutput,
										 s2sdk::HookMode::Post))
			{
				bAllOk = false;
				LogWarning("hooking OnAnimationDone failed, OnAnimationDone stops are disabled");
			}
		}
		catch (...)
		{
			bAllOk = false;
			LogWarning("hooking OnAnimationDone threw, OnAnimationDone stops are disabled");
		}
	}

	// 2. CheckTransmit: only needed while players can be hidden from the dancer.
	if (g_Settings.EmoteHidePlayers != 0)
	{
		if (!__s2sdk_OnServerCheckTransmit_Register)
		{
			bAllOk = false;
			LogWarning("s2sdk OnServerCheckTransmit is missing, the hide-players feature is off");
		}
		else
		{
			try { s2sdk::OnServerCheckTransmit_Register(&OnCheckTransmit); }
			catch (...)
			{
				bAllOk = false;
				LogWarning("registering the CheckTransmit listener threw");
			}
		}
	}

	// 3. precache every map load
	if (__s2sdk_OnBuildGameSessionManifest_Register)
	{
		try { s2sdk::OnBuildGameSessionManifest_Register(&OnBuildManifest); }
		catch (...)
		{
			bAllOk = false;
			LogWarning("registering the game session manifest listener threw, resources are not precached");
		}
	}
	else
	{
		bAllOk = false;
	}

	// 4. damage: not ported (s2sdk has no TakeDamage hook)
	if (g_Settings.StopDamageWhenInEmote)
	{
		LogWarning("StopDamageWhenInEmote is enabled in settings.ini, but this port has no damage hook "
				   "(s2sdk provides none) - the setting is ignored, emotes can be interrupted by damage");
	}

	g_bHooksInstalled = true;
	if (bAllOk) LogInfo("Hooks installed");
	return bAllOk;
}

void RemoveHooks()
{
	if (__s2sdk_UnhookEntityOutput)
	{
		try
		{
			s2sdk::UnhookEntityOutput(plg::string("*"), plg::string("OnAnimationDone"), &OnAnimationDoneOutput,
									  s2sdk::HookMode::Post);
		}
		catch (...)
		{
		}
	}
	if (__s2sdk_OnServerCheckTransmit_Unregister)
	{
		try { s2sdk::OnServerCheckTransmit_Unregister(&OnCheckTransmit); }
		catch (...) {}
	}
	if (__s2sdk_OnBuildGameSessionManifest_Unregister)
	{
		try { s2sdk::OnBuildGameSessionManifest_Unregister(&OnBuildManifest); }
		catch (...) {}
	}
	g_bHooksInstalled = false;
}

// Fills g_States[viewer].BlockedTransmit with the pawn HANDLES hidden from that viewer (SPEC §E.10).
void ApplyHidePlayers(int iViewerSlot)
{
	if (iViewerSlot < 0 || iViewerSlot >= FE_MAX_PLAYERS) return;

	PlayerState& viewer = g_States[iViewerSlot];
	viewer.BlockedTransmit.clear();

	const int iMode = g_Settings.EmoteHidePlayers;
	if (iMode <= 0) return;

	if (GetController(iViewerSlot) <= 0) return;
	const int iViewerTeam = pisex::GetPlayerTeam(iViewerSlot);

	for (int i = 0; i < FE_MAX_PLAYERS; i++)
	{
		if (i == iViewerSlot || !IsValidHuman(i)) continue;

		const int iPawnHandle = Handle(GetAlivePawn(i));
		if (!IsValidHandle(iPawnHandle)) continue;
		if (GetController(i) <= 0) continue;

		// raw team compare (preserve), SPEC §E.10
		const int iTeam = pisex::GetPlayerTeam(i);
		const bool bHide = (iMode == 1 && iViewerTeam != iTeam)
			|| (iMode == 2 && iViewerTeam == iTeam)
			|| (iMode == 3);
		if (bHide) viewer.BlockedTransmit.push_back(iPawnHandle);
	}
}

void ClearHidePlayers(int iViewerSlot)
{
	if (iViewerSlot < 0 || iViewerSlot >= FE_MAX_PLAYERS) return;
	g_States[iViewerSlot].BlockedTransmit.clear();
}

void EmitEmoteSound(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	StopEmoteSound(iSlot); // SPEC §E.4: stop the previous sound first

	if (!g_Settings.SoundModuleEnabled) return;

	PlayerState& state = g_States[iSlot];
	if (state.EmoteIndex < 0 || state.EmoteIndex >= (int)g_Emotes.size()) return;

	const Emote& emote = g_Emotes[state.EmoteIndex];
	if (emote.Sound.empty()) return;

	const int iPawn = pisex::GetPawnEntity(iSlot);
	if (iPawn <= 0) return;

	const std::vector<int> vRecipients = GetSoundRecipients(iSlot);
	if (vRecipients.empty()) return;

	const int iHandle = Handle(iPawn);
	if (!ValidHandle(iHandle) || !__s2sdk_EmitSoundToClient) return;
	plg::vector<int32_t> vSlots(vRecipients.begin(), vRecipients.end());
	try { s2sdk::EmitSoundToClient(iHandle, vSlots, plg::string(emote.Sound), emote.SoundVolume, 100.0f); }
	catch (...) {}
	state.CurrentSound = emote.Sound;

	if (emote.SoundLoopAfter > 0.0f) StartSoundLoop(iSlot, state.Generation, emote.SoundLoopAfter);
}

void StopEmoteSound(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	PlayerState& state = g_States[iSlot];
	if (state.CurrentSound.empty()) return;

	// the sound is stopped on the dancer's own pawn (the recipients of a "sound to everybody" case
	// are covered by the same entity; the original stopped the sound event through the pawn too)
	const int iPawn = pisex::GetPawnEntity(iSlot);
	const int iHandle = iPawn > 0 ? Handle(iPawn) : 0;
	if (ValidHandle(iHandle) && __s2sdk_StopSound)
	{
		try { s2sdk::StopSound(iHandle, plg::string(state.CurrentSound)); }
		catch (...) {}
	}

	state.CurrentSound.clear();
}
