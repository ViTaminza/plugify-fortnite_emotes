// fortnite_emotes — plugin entry point: globals, logging, the ConVar, the game events and the
// per-frame pump (the original's fortnite_emotes.cpp).
//
// The original's utils frame timer is the Plugify OnPluginUpdate pump here; everything else keeps
// its shape: ConVar css_fortnite_emotes_enable, player_spawn/death/hurt/team/disconnect,
// round_prestart/round_end/round_freeze_end, the round-end stop timer that honours
// mp_round_restart_delay, and the dependency check (MultiAddonManager).
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <pisex/pisex.hpp>

#include "fe.hpp"

bool g_bEnabled = true;  // css_fortnite_emotes_enable
bool g_bRoundEnd = false;

namespace {

const char* kLogTag = "[FortniteEmotes]";

void LogV(const char* fmt, va_list args)
{
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + buffer, plg::Severity::Info, plg::source_location::current());
}

} // namespace

void LogInfo(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	LogV(fmt, args);
	va_end(args);
}

void LogWarning(const char* fmt, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + buffer, plg::Severity::Warning, plg::source_location::current());
}

void DebugLog(const char* fmt, ...)
{
	if (!g_Settings.DebugLogs) return;
	va_list args;
	va_start(args, fmt);
	LogV(fmt, args);
	va_end(args);
}

double FE_Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------------------------
// Plugin-owned timers (fe.hpp)
// ---------------------------------------------------------------------------------------------

namespace {

struct TimerSlot
{
	pisex::CTimer* pTimer = nullptr;
};

std::vector<TimerSlot*> g_TimerSlots;

// Drops the slots of timers that expired themselves.
void PruneTimerSlots()
{
	for (size_t i = 0; i < g_TimerSlots.size();)
	{
		if (g_TimerSlots[i]->pTimer)
		{
			++i;
			continue;
		}
		delete g_TimerSlots[i];
		g_TimerSlots[i] = g_TimerSlots.back();
		g_TimerSlots.pop_back();
	}
}

} // namespace

pisex::CTimer* CreatePluginTimer(float flInterval, std::function<float()> fn)
{
	if (!pisex::g_pUtils) return nullptr;

	PruneTimerSlots();

	TimerSlot* pSlot = new TimerSlot();
	pSlot->pTimer = pisex::g_pUtils->CreateTimer(flInterval, [pSlot, fn]() -> float
	{
		const float flResult = fn();
		if (flResult < 0.0f) pSlot->pTimer = nullptr; // the registry deletes the timer right after
		return flResult;
	});

	g_TimerSlots.push_back(pSlot);
	return pSlot->pTimer;
}

void RemovePluginTimers()
{
	for (TimerSlot* pSlot : g_TimerSlots)
	{
		if (pSlot->pTimer) pisex::g_pUtils->RemoveTimer(pSlot->pTimer);
		delete pSlot;
	}
	g_TimerSlots.clear();
}

namespace {

// ---------------------------------------------------------------------------------------------
// ConVar and the flags the frame pump consumes
// ---------------------------------------------------------------------------------------------

uint64_t g_iConVarHandle = 0;
bool g_bStopAllRequested = false; // "the ConVar changed": applied from the frame pump

// The original stopped the emotes from inside the ConVar callback. s2sdk dispatches convar changes
// from its own manager, so the callback only records the request and the frame pump applies it
// (never call back into a subsystem that is calling you).
void OnConVarChanged(uint64_t, const plg::string&, const plg::string& sNewValue)
{
	// a callback of s2sdk: the value is only recorded, nothing may unwind out of here
	try
	{
		g_bEnabled = !(sNewValue == plg::string("0"));
		g_bStopAllRequested = true;
	}
	catch (...)
	{
	}
}

uint32_t g_iRoundSerial = 0;

uint64_t g_iRestartDelayConVar = 0; // mp_round_restart_delay (the round-end stop timer)

// ---------------------------------------------------------------------------------------------
// Map start
// ---------------------------------------------------------------------------------------------

// The original ran this from StartupServer, which cs2-menus fired on every map load. pisexcompat's
// StartupServer runs once per server start, so the per-map reset is registered as a map-start
// callback and the one-time part stays where it was.
void OnMapStart(const char*)
{
	// the previous map took every entity with it: reset the per-slot state directly (no
	// RemoveEntity on stale handles, no weapons into a dead map - see the original's note)
	++g_iRoundSerial;
	for (int i = 0; i < FE_MAX_PLAYERS; ++i)
	{
		DropGloveSnapshot(i);
		g_States[i] = PlayerState{};
	}
	CloseEmoteMenus();
	g_bRoundEnd = false;

	DebugLog("Map loaded");
}

void OnStartupServer()
{
	// the first map: same reset (the map-start callback above handles every later one)
	OnMapStart(nullptr);
}

// ---------------------------------------------------------------------------------------------
// Game events (SPEC §E.12)
// ---------------------------------------------------------------------------------------------

void OnPlayerSpawnEvent(const char*, pisex::IGameEvent* pEvent, bool)
{
	const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	// the new life's loadout owns the gloves now; discard the snapshot before the forced stop
	DropGloveSnapshot(iSlot);
	StopEmote(iSlot, true);
}

void OnPlayerDeathEvent(const char*, pisex::IGameEvent* pEvent, bool)
{
	const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	StopEmote(iSlot, true);
}

void OnPlayerHurtEvent(const char*, pisex::IGameEvent* pEvent, bool)
{
	const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	if (IsDancing(iSlot)) StopEmote(iSlot);
}

void OnPlayerTeamEvent(const char*, pisex::IGameEvent* pEvent, bool)
{
	const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	// EmoteHidePlayers compares teams, so the blocked set has to be rebuilt
	if (IsDancing(iSlot)) ApplyHidePlayers(iSlot);
}

void OnPlayerDisconnectEvent(const char*, pisex::IGameEvent* pEvent, bool)
{
	const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	ResetPlayer(iSlot);
}

void OnRoundPrestartEvent(const char*, pisex::IGameEvent*, bool)
{
	// note: stop the emotes first, so GivePlayerWeaponsBack still finds the weapon snapshots
	StopAllEmotes();
	g_bRoundEnd = false;
	++g_iRoundSerial;
}

void OnRoundEndEvent(const char*, pisex::IGameEvent*, bool)
{
	g_bRoundEnd = true;
	++g_iRoundSerial;
	const uint32_t iSerial = g_iRoundSerial;

	// SPEC §E.13: stop all emotes mp_round_restart_delay - 0.2 s after round end (min 0.1 s)
	float flDelay = 7.0f;
	if (g_iRestartDelayConVar && __s2sdk_GetConVarFloat)
	{
		try { flDelay = (float)s2sdk::GetConVarFloat(g_iRestartDelayConVar); }
		catch (...) { flDelay = 7.0f; }
	}
	flDelay = std::max(flDelay - 0.2f, 0.1f);

	CreatePluginTimer(flDelay, [iSerial]() -> float
	{
		// a new round or map in the meantime invalidates this timer
		if (iSerial == g_iRoundSerial) StopAllEmotes();
		return -1.0f;
	});
}

void OnRoundFreezeEndEvent(const char*, pisex::IGameEvent*, bool)
{
	if (g_Settings.StopEmoteAfterFreezetimeEnd) StopAllEmotes();
}

// ---------------------------------------------------------------------------------------------
// Dependency check (SPEC §0): warnings only, never fatal
// ---------------------------------------------------------------------------------------------

void CheckDependencies()
{
	if (!__s2sdk_FindConVar || !__s2sdk_GetConVarValue)
	{
		LogWarning("cannot check the mounted workshop addons (no s2sdk convar API); emotes and dances may have no models and no sounds");
		return;
	}

	// This stack mounts addons through s2sdk's MultiAddonManager: the addon list is
	// 's2_extra_addons' (s2sdk regenerates it from csgo/cfg/plugify/multi_addon_manager.cfg and
	// rewrites the convar on every map load - it is NOT mm_extra_addons). Servers that also run
	// the Metamod MultiAddonManager still have 'mm_extra_addons', so that is the fallback; the
	// warning only fires when neither lists the addon. s2sdk's FindConVar answers the cvar
	// system's invalid handle (0xFFFFFFFF) for a name that does not resolve - it must
	// not be passed to GetConVarValue (that logs "Invalid ConVar handle" for every call).
	const auto validHandle = [](uint64_t handle) {
		return handle != 0 && handle != 0xFFFFFFFFull && handle != 0xFFFFFFFFFFFFFFFFull;
	};
	const char* kAddonId = "3328582199";
	std::string checked;
	for (const char* name : {"s2_extra_addons", "mm_extra_addons"})
	{
		const uint64_t handle = s2sdk::FindConVar(plg::string(name));
		if (!validHandle(handle))
		{
			continue;
		}
		if (!checked.empty())
		{
			checked += ", ";
		}
		checked += name;
		try
		{
			const plg::string sValue = s2sdk::GetConVarValue(handle);
			const std::string sAddons(sValue.data(), sValue.size());
			if (sAddons.find(kAddonId) != std::string::npos)
			{
				return; // listed: the addon is mounted, nothing to warn about
			}
		}
		catch (...)
		{
		}
	}

	if (checked.empty())
	{
		LogWarning("no addon list convar exists (neither s2_extra_addons nor mm_extra_addons); emotes and dances will have no models and no sounds");
		return;
	}
	const std::string message = std::string("Workshop addon ") + kAddonId + " is not listed in " + checked +
								"; emotes and dances will have no models and no sounds";
	LogWarning("%s", message.c_str());
}

} // namespace

// Every s2sdk entry point the emote lifecycle calls directly, checked once at plugin start (the
// fe_entity helpers guard themselves; this covers the call sites that need a whole set of
// bindings to be consistent). A missing one means this plugin and the installed s2sdk do not
// match - the plugin then refuses to start emotes instead of crashing in the middle of one.
void CheckEmoteBindings()
{
	const bool bOk = __s2sdk_CreateEntityByName && __s2sdk_DispatchSpawn2 && __s2sdk_TeleportEntity
		&& __s2sdk_SetEntityModel && __s2sdk_RemoveEntity && __s2sdk_IsValidEntHandle
		&& __s2sdk_IsEntityPlayerPawn && __s2sdk_AcceptEntityInput && __s2sdk_GetClientModel
		&& __s2sdk_GetClientAbsOrigin && __s2sdk_GetClientAbsAngles && __s2sdk_GetClientButtons
		&& __s2sdk_GetClientAbsVelocity && __s2sdk_GetClientMoveType && __s2sdk_GetEntityAbsOrigin
		&& __s2sdk_GetEntityAngRotation && __s2sdk_GetEntityRenderColor && __s2sdk_SetEntityRenderColor
		&& __s2sdk_GetEntSchema && __s2sdk_SetEntSchema && __s2sdk_GetEntSchemaVector3D
		&& __s2sdk_SetEntSchemaVector3D && __s2sdk_GetEntSchemaFloat && __s2sdk_SetEntSchemaFloat
		&& __s2sdk_GetEntSchemaString && __s2sdk_GetEntSchemaEnt && __s2sdk_SetEntSchemaEnt
		&& __s2sdk_GetEntSchemaPtr && __s2sdk_GetEntSchema2 && __s2sdk_SetEntSchema2
		&& __s2sdk_GetEntData && __s2sdk_SetEntData && __s2sdk_NetworkStateChanged
		&& __s2sdk_EmitSoundToClient && __s2sdk_StopSound && __s2sdk_GetClientWeapons
		&& __s2sdk_GetClientActiveWeapon && __s2sdk_GiveNamedItem && __s2sdk_DropWeapon;
	if (bOk) return;

	g_bEmoteBindings = false;
	LogWarning("the s2sdk bindings needed for the emote lifecycle are incomplete - emotes and dances "
			   "stay disabled (the commands answer with the usual error message)");
}

// ---------------------------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------------------------

class FortniteEmotesPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override
	{
		try
		{
			pisex::Init("fortnite_emotes");

			// css_fortnite_emotes_enable (the original's ConVar, SPEC §0)
			if (__s2sdk_CreateConVarBool)
			{
				g_iConVarHandle = s2sdk::CreateConVarBool(plg::string("css_fortnite_emotes_enable"), true,
					plg::string("Fortnite Emotes & Dances master switch: 0 disables emotes and dances"),
					s2sdk::ConVarFlag::None, false, false, false, false);
				if (g_iConVarHandle && __s2sdk_HookConVarChange)
					s2sdk::HookConVarChange(g_iConVarHandle, &OnConVarChanged);

				// A ConVar outlives the plugin and re-registering returns the existing one, and the
				// change callback does not fire on registration: read the value the engine holds so
				// a reload honours an existing "0".
				if (g_iConVarHandle && __s2sdk_GetConVarBool)
					g_bEnabled = s2sdk::GetConVarBool(g_iConVarHandle);
			}
			else
			{
				LogWarning("the s2sdk convar API is missing, css_fortnite_emotes_enable is not registered");
			}

			LoadConfig();
			InitCameraTrace();
			CheckEmoteBindings();
			InstallHooks();
			RegisterCommands();

			pisex::g_pUtils->StartupServer(&OnStartupServer);
			pisex::RegisterMapStartCallback(&OnMapStart);

			if (__s2sdk_FindConVar)
			{
				g_iRestartDelayConVar = s2sdk::FindConVar(plg::string("mp_round_restart_delay"));
				if (!g_iRestartDelayConVar)
					LogWarning("mp_round_restart_delay not found, the round-end stop timer uses 7 s");
			}
			else
			{
				LogWarning("no convar API: mp_round_restart_delay cannot be read");
			}

			pisex::g_pUtils->HookEvent("player_spawn", OnPlayerSpawnEvent);
			pisex::g_pUtils->HookEvent("player_death", OnPlayerDeathEvent);
			pisex::g_pUtils->HookEvent("player_hurt", OnPlayerHurtEvent);
			pisex::g_pUtils->HookEvent("player_team", OnPlayerTeamEvent);
			pisex::g_pUtils->HookEvent("player_disconnect", OnPlayerDisconnectEvent);
			pisex::g_pUtils->HookEvent("round_prestart", OnRoundPrestartEvent);
			pisex::g_pUtils->HookEvent("round_end", OnRoundEndEvent);
			pisex::g_pUtils->HookEvent("round_freeze_end", OnRoundFreezeEndEvent);

			// SPEC §0: dependency check 1 s after load
			CreatePluginTimer(1.0f, []() -> float
			{
				CheckDependencies();
				return -1.0f;
			});

			LogInfo("v%s loaded", "2.0.0");
		}
		catch (const std::exception& ex)
		{
			LogWarning("OnPluginStart failed: %s", ex.what());
		}
		catch (...)
		{
			LogWarning("OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed: that stops every plugify plugin
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override
	{
		(void)dt;
		try
		{
			pisex::Poll();
			if (g_bStopAllRequested)
			{
				g_bStopAllRequested = false;
				StopAllEmotes();
			}
			OnFrame();
		}
		catch (const std::exception& ex)
		{
			LogWarning("update failed: %s", ex.what());
		}
		catch (...)
		{
			LogWarning("update failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override
	{
		try
		{
			// stop the emotes while the entity system and the API are still alive; the glove
			// rebuild timers that StopAllEmotes may schedule are taken down right after
			StopAllEmotes();
			RemovePluginTimers();

			// a menu opened by this plugin holds a callback into this library: close it first
			CloseEmoteMenus();

			RemoveHooks();
			if (g_iConVarHandle && __s2sdk_UnhookConVarChange)
				s2sdk::UnhookConVarChange(g_iConVarHandle, &OnConVarChanged);
			pisex::Shutdown();
		}
		catch (const std::exception& ex)
		{
			LogWarning("OnPluginEnd failed: %s", ex.what());
		}
		catch (...)
		{
			LogWarning("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

FortniteEmotesPlugin g_fortniteEmotesPlugin;

PLUGIFY_PLUGIN(PLUGIN_API, &g_fortniteEmotesPlugin)
