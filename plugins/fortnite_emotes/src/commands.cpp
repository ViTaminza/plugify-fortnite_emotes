// fortnite_emotes — commands, chat triggers, the emote menu and the access/cooldown helpers
// (the reference's commands.cpp: SPEC §B, §C, §D).
//
// pisexcompat hands the callback the registered spelling as the first token followed by the
// arguments, exactly like the reference's utils callbacks, so the parsing is unchanged. Chat names
// are registered with their "!" trigger and dispatched by s2sdk for the trigger characters it is
// configured for ("!" and "/"); the reference's third spelling (".") cannot be dispatched by the
// framework and is not registered (documented deviation).
#include "fe.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <admin_api.hpp> // consumer wrapper of the admin_system plugin (HasPermission)

// The vip plugin is consumed through its generated bindings directly (four methods, all of them
// checked against their binding slot before the call): a shared wrapper would pull in the whole
// IVIPApi surface, which this plugin does not use.
#ifndef FE_HAS_VIP
#define FE_HAS_VIP 1
#endif
#if FE_HAS_VIP
#include <vip/vip.hpp>
#endif

namespace {

std::string StripQuotes(const std::string& sText)
{
	std::string s = sText;
	while (!s.empty() && s.front() == '"') s.erase(0, 1);
	while (!s.empty() && s.back() == '"') s.pop_back();
	return s;
}

std::string ToLower(const std::string& sText)
{
	std::string s = sText;
	for (char& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

bool ContainsNoCase(const std::string& sHaystack, const std::string& sNeedle)
{
	return ToLower(sHaystack).find(ToLower(sNeedle)) != std::string::npos;
}

// " {prefix} {phrase}" - the donor prints replies with a leading space (SPEC §B)
std::string PrefixLine(const char* szPhraseKey)
{
	return " " + Prefix() + " " + Phrase(szPhraseKey);
}

// Console senders (slot < 0) have no chat: drop the colour tags and print to the server console
void PrintLine(int iSlot, const std::string& sMessage)
{
	if (iSlot >= 0)
	{
		Print(iSlot, sMessage);
		return;
	}
	std::string sPlain;
	for (size_t i = 0; i < sMessage.size(); i++)
	{
		if (sMessage[i] == '{')
		{
			const size_t iEnd = sMessage.find('}', i);
			if (iEnd != std::string::npos)
			{
				i = iEnd;
				continue;
			}
		}
		sPlain += sMessage[i];
	}
	pisex::g_pUtils->PrintToConsole(iSlot, "%s", sPlain.c_str());
}

// "mm_setemote bob dance1" -> { "bob", "dance1" }: the command itself (first token) is dropped
std::vector<std::string> ParseCommandArgs(const char* szContent)
{
	std::vector<std::string> vArgs;
	std::istringstream stream(StripQuotes(szContent ? szContent : ""));
	std::string sToken;
	while (stream >> sToken)
	{
		while (!sToken.empty() && sToken.front() == '"') sToken.erase(0, 1);
		while (!sToken.empty() && sToken.back() == '"') sToken.pop_back();
		if (!sToken.empty()) vArgs.push_back(sToken);
	}
	if (!vArgs.empty()) vArgs.erase(vArgs.begin());
	return vArgs;
}

// GetAllValidPlayers(): connected and in game; bots count as valid here (SPEC §B.4)
bool IsValidPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS || !pisex::IsGameReady()) return false;
	return pisex::IsConnected(iSlot) && pisex::IsInGame(iSlot);
}

// The VIP / admin_system integrations are optional: without the plugins (or without their
// bindings, see the CMake FE_HAS_VIP switch) their checks simply never pass. Every call checks the
// binding slot itself - an unloaded vip plugin leaves all of them null.
bool VipAvailable()
{
#if FE_HAS_VIP
	return __vip_VIP_IsVIPLoaded && __vip_VIP_IsClientVIP && __vip_VIP_GetClientFeatureBool
		&& __vip_VIP_GetClientFeatureInt;
#else
	return false;
#endif
}

bool VipIsClientVIP(int iSlot)
{
#if FE_HAS_VIP
	if (!VipAvailable()) return false;
	try { return vip::VIP_IsVIPLoaded() && vip::VIP_IsClientVIP(iSlot); }
	catch (...) { return false; }
#else
	(void)iSlot;
	return false;
#endif
}

bool VipFeatureEnabled(int iSlot, const char* szFeature)
{
#if FE_HAS_VIP
	if (!VipAvailable() || !szFeature) return false;
	try
	{
		return vip::VIP_IsVIPLoaded() && vip::VIP_IsClientVIP(iSlot)
			&& vip::VIP_GetClientFeatureBool(iSlot, plg::string(szFeature));
	}
	catch (...)
	{
		return false;
	}
#else
	(void)iSlot;
	(void)szFeature;
	return false;
#endif
}

int VipFeatureInt(int iSlot, const char* szFeature)
{
#if FE_HAS_VIP
	if (!VipAvailable() || !szFeature) return -1;
	try
	{
		if (!vip::VIP_IsVIPLoaded() || !vip::VIP_IsClientVIP(iSlot)) return -1;
		return (int)vip::VIP_GetClientFeatureInt(iSlot, plg::string(szFeature));
	}
	catch (...)
	{
		return -1;
	}
#else
	(void)iSlot;
	(void)szFeature;
	return -1;
#endif
}

bool AdminHasPermission(int iSlot, const char* szPermission)
{
	return admin_system::AdminApi::Get().HasPermission(iSlot, szPermission);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Access and cooldown (SPEC §A.1, §E.1 step 6)
// ---------------------------------------------------------------------------------------------

bool HasAccess(int iSlot, const std::vector<std::string>& vPerms)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return true; // console / invalid slot: no restrictions
	for (const std::string& sPerm : vPerms)
	{
		if (sPerm.empty()) return true; // "" = everyone
		if (sPerm == "vip")
		{
			if (VipIsClientVIP(iSlot)) return true;
			continue;
		}
		if (sPerm.compare(0, 4, "vip:") == 0)
		{
			if (VipFeatureEnabled(iSlot, sPerm.c_str() + 4)) return true;
			continue;
		}
		if (AdminHasPermission(iSlot, sPerm.c_str())) return true;
	}
	return false;
}

int GetCooldownSeconds(int iSlot)
{
	if (iSlot >= 0 && VipIsClientVIP(iSlot))
	{
		// a VIP_INT feature: a positive per-group cooldown overrides both settings
		const int iFeature = VipFeatureInt(iSlot, g_Settings.VIPCooldownFeature.c_str());
		if (iFeature > 0) return iFeature;
	}
	return HasAccess(iSlot, g_Settings.VIPPerm) ? g_Settings.EmoteVIPCooldown : g_Settings.EmoteCooldown;
}

// ---------------------------------------------------------------------------------------------
// Target resolution for !setemote / !setdance (SPEC §B.4, §B.5)
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<int> ResolveTargets(int iCaller, const std::string& sArg)
{
	std::vector<int> vTargets;
	if (sArg.empty()) return vTargets;

	// "#<userid>": the framework reports the event userid as the player slot
	if (sArg[0] == '#')
	{
		if (sArg.size() > 1)
		{
			const int iUserID = atoi(sArg.c_str() + 1);
			if (IsValidPlayer(iUserID)) vTargets.push_back(iUserID);
		}
		return vTargets;
	}

	if (sArg[0] == '@')
	{
		if (iCaller < 0) return vTargets;
		const std::string sMode = ToLower(sArg.substr(1));
		if (sMode == "me")
		{
			vTargets.push_back(iCaller);
			return vTargets;
		}
		if (sMode == "aim")
		{
			const int iCallerPawn = GetAlivePawn(iCaller);
			if (iCallerPawn <= 0) return vTargets;

			const int iHit = TraceFromEyes(Handle(iCallerPawn), 16384.0f);
			if (iHit <= 0) return vTargets;

			// the slot of the entity the caller looks at (a controller's entity index is not
			// generally the slot + 1, so the owner is resolved through the engine)
			const int iTarget = pisex::GetOwnerSlot(iHit);
			if (IsValidPlayer(iTarget)) vTargets.push_back(iTarget);
			return vTargets;
		}
		for (int i = 0; i < FE_MAX_PLAYERS; i++)
		{
			if (!IsValidPlayer(i)) continue;
			if (sMode == "all")
			{
				vTargets.push_back(i);
			}
			else if (sMode == "t" || sMode == "ct")
			{
				const int iTeam = (sMode == "t") ? 2 : 3;
				if (pisex::GetPlayerTeam(i) == iTeam) vTargets.push_back(i);
			}
			else if (sMode == "alive")
			{
				if (GetAlivePawn(i) > 0) vTargets.push_back(i);
			}
			else if (sMode == "dead")
			{
				if (GetAlivePawn(i) <= 0) vTargets.push_back(i);
			}
			else if (sMode == "bots")
			{
				if (pisex::IsFakeClient(i)) vTargets.push_back(i);
			}
			else if (sMode == "human")
			{
				if (!pisex::IsFakeClient(i)) vTargets.push_back(i);
			}
		}
		return vTargets;
	}

	// exact name first, then substring; both case-insensitive
	for (int i = 0; i < FE_MAX_PLAYERS; i++)
	{
		if (!IsValidPlayer(i)) continue;
		const char* szName = pisex::GetPlayerName(i);
		if (szName && ToLower(szName) == ToLower(sArg))
		{
			vTargets.push_back(i);
			return vTargets;
		}
	}
	for (int i = 0; i < FE_MAX_PLAYERS; i++)
	{
		if (!IsValidPlayer(i)) continue;
		const char* szName = pisex::GetPlayerName(i);
		if (szName && ContainsNoCase(szName, sArg)) vTargets.push_back(i);
	}
	return vTargets;
}

// SPEC §B.5: nobody may force an emote on an admin, and the console may force it on anyone.
bool CanTarget(int iCaller, int iTarget)
{
	if (iCaller < 0 || iCaller == iTarget) return true;
	if (!HasAccess(iCaller, g_Settings.AdminSetEmoteDanceCommandPerm)) return false;
	return !HasAccess(iTarget, g_Settings.AdminSetEmoteDanceCommandPerm);
}

// ---------------------------------------------------------------------------------------------
// Menu (SPEC §D)
// ---------------------------------------------------------------------------------------------

// Slots with an emote menu opened by this plugin (Unload() closes them through this flag).
bool g_bMenuShown[FE_MAX_PLAYERS] = {};

void OnEmoteMenuItem(const char* szBack, const char* szFront, int iItem, int iSlot)
{
	(void)szFront;
	(void)iItem;
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	// this callback (any item, "exit" included) means the menu is no longer open
	g_bMenuShown[iSlot] = false;

	if (!szBack || !szBack[0]) return;
	if (!strcmp(szBack, "exit") || !strcmp(szBack, "back") || !strcmp(szBack, "next")) return;
	const int iEmote = atoi(szBack);
	if (iEmote < 0 || iEmote >= (int)g_Emotes.size()) return;

	const uint32_t iGeneration = g_States[iSlot].Generation;
	// the start is deferred: the menu core is still dispatching this callback, closing the menu here
	// would clear the very std::function being called
	pisex::g_pUtils->NextFrame([iSlot, iEmote, iGeneration]()
	{
		if (pisex::g_pMenus) pisex::g_pMenus->ClosePlayerMenu(iSlot);
		pisex::g_pUtils->NextFrame([iSlot, iEmote, iGeneration]()
		{
			if (!IsValidPlayer(iSlot)) return;
			if (g_States[iSlot].Generation != iGeneration) return;
			std::string sError;
			if (!PlayEmote(iSlot, iEmote, sError, false)) Print(iSlot, sError);
		});
	});
}

} // namespace

void ShowEmoteMenu(int iSlot, bool bDance)
{
	if (!pisex::g_pMenus || iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	pisex::Menu hMenu;
	bool bAny = false;
	for (size_t i = 0; i < g_Emotes.size(); i++)
	{
		const Emote& emote = g_Emotes[i];
		if (emote.IsEmote == bDance) continue; // dances have IsEmote == false
		const std::string sLabel = EmoteDisplayName(emote);
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), sLabel.c_str(),
									 HasAccess(iSlot, emote.Permission) ? pisex::ITEM_DEFAULT : pisex::ITEM_DISABLED);
		bAny = true;
	}
	if (!bAny)
	{
		PrintLine(iSlot, " " + Prefix() + " " + (bDance ? "No dances found." : "No emotes found."));
		return;
	}

	pisex::g_pMenus->SetTitleMenu(hMenu, Phrase(bDance ? "emote.dancemenutitle" : "emote.emotemenutitle").c_str());
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, OnEmoteMenuItem);
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
	g_bMenuShown[iSlot] = true;
}

void CloseEmoteMenus()
{
	for (int i = 0; i < FE_MAX_PLAYERS; i++)
	{
		if (!g_bMenuShown[i]) continue;
		if (pisex::g_pMenus) pisex::g_pMenus->ClosePlayerMenu(i);
		g_bMenuShown[i] = false;
	}
}

// ---------------------------------------------------------------------------------------------
// !emote / !dance (SPEC §B.1) and !setemote / !setdance (SPEC §B.2)
// ---------------------------------------------------------------------------------------------

namespace {

void HandleEmoteCommand(int iSlot, bool bDance, const std::vector<std::string>& vArgs)
{
	if (!IsValidPlayer(iSlot))
	{
		// this reply has no leading space before the prefix (donor formatting, SPEC §B.1)
		PrintLine(iSlot, Prefix() + " " + Phrase("emote.command.client-only"));
		return;
	}
	if (!HasAccess(iSlot, g_Settings.EmoteDanceCommandPerm))
	{
		PrintLine(iSlot, PrefixLine("emote.command.no-permission"));
		return;
	}

	if (vArgs.empty())
	{
		ShowEmoteMenu(iSlot, bDance);
		return;
	}

	const int iEmote = GetEmoteByName(vArgs[0], !bDance, true);
	if (iEmote < 0)
	{
		PrintLine(iSlot, PrefixLine("emote.not-found"));
		return;
	}
	if (!HasAccess(iSlot, g_Emotes[iEmote].Permission))
	{
		PrintLine(iSlot, PrefixLine("emote.no-access"));
		return;
	}

	std::string sError;
	if (!PlayEmote(iSlot, iEmote, sError, false)) PrintLine(iSlot, sError);
}

void HandleSetCommand(int iSlot, bool bDance, const std::string& sName, const std::vector<std::string>& vArgs)
{
	if (iSlot >= 0 && !HasAccess(iSlot, g_Settings.AdminSetEmoteDanceCommandPerm))
	{
		PrintLine(iSlot, PrefixLine("emote.command.no-permission"));
		return;
	}

	if (vArgs.size() < 2)
	{
		PrintLine(iSlot, " " + Prefix() + " " + FormatPhrase(
			bDance ? "emote.setdance-usage" : "emote.setemote-usage", { sName }));
		return;
	}

	const int iEmote = GetEmoteByName(vArgs[1], !bDance, true);
	if (iEmote < 0)
	{
		PrintLine(iSlot, PrefixLine("emote.not-found"));
		return;
	}

	const std::vector<int> vTargets = ResolveTargets(iSlot, vArgs[0]);
	if (vTargets.empty())
	{
		PrintLine(iSlot, PrefixLine("emote.target-not-found"));
		return;
	}

	int iCount = 0;
	for (int iTarget : vTargets)
	{
		if (!CanTarget(iSlot, iTarget)) continue;
		std::string sError;
		if (PlayEmote(iTarget, iEmote, sError, true)) iCount++;
		else if (iSlot >= 0) PrintLine(iSlot, sError); // per-target failures to a player caller only
	}

	if (iCount > 0)
		PrintLine(iSlot, " " + Prefix() + " " + FormatPhrase(
			bDance ? "emote.played-dance-on-target" : "emote.played-emote-on-target", { std::to_string(iCount) }));
	else
		PrintLine(iSlot, PrefixLine(bDance ? "emote.failplayed-dance-on-target" : "emote.failplayed-emote-on-target"));
}

// ---------------------------------------------------------------------------------------------
// Chat triggers (SPEC §C)
// ---------------------------------------------------------------------------------------------

bool OnChatTrigger(int iSlot, const char* szContent, bool)
{
	if (!g_Settings.ChatTriggersEnabled || g_ChatTriggers.empty()) return true;
	if (!szContent || !szContent[0]) return true;

	// exact, whole-message, case-sensitive match; no trimming
	const std::string sText = StripQuotes(szContent);
	if (sText.empty()) return true;
	if (sText[0] == '!' || sText[0] == '/' || sText[0] == '.') return true;

	auto it = g_ChatTriggers.find(sText);
	if (it == g_ChatTriggers.end()) return true;
	const int iEmote = it->second;
	if (iEmote < 0 || iEmote >= (int)g_Emotes.size()) return true;
	const Emote& emote = g_Emotes[iEmote];

	if (!IsValidPlayer(iSlot)) return true; // the message stays visible

	if (!HasAccess(iSlot, g_Settings.EmoteDanceCommandPerm) || !HasAccess(iSlot, emote.Permission))
	{
		Print(iSlot, PrefixLine("emote.no-access"));
		return false; // a trigger always hides the message (SPEC §C.2)
	}

	std::string sError;
	if (!PlayEmote(iSlot, iEmote, sError, false)) Print(iSlot, sError);
	return false;
}

// ---------------------------------------------------------------------------------------------
// Trigger list commands (SPEC §B.3)
// ---------------------------------------------------------------------------------------------

void ShowTriggerList(int iSlot)
{
	if (iSlot < 0)
	{
		std::string sList;
		for (size_t i = 0; i < g_TriggerOrder.size(); i++)
		{
			if (i > 0) sList += ", ";
			sList += g_TriggerOrder[i];
		}
		PrintLine(iSlot, sList);
		return;
	}

	if (!g_Settings.ChatTriggersEnabled) return; // silent no-op for players

	PrintLine(iSlot, PrefixLine("emote.list-chat-triggers"));

	std::string sBatch;
	for (const std::string& sTrigger : g_TriggerOrder)
	{
		while (!sBatch.empty() && sBatch.size() + sTrigger.size() > 507)
		{
			PrintLine(iSlot, " {LIGHTPURPLE}" + sBatch + ",");
			sBatch.clear();
		}
		if (!sBatch.empty()) sBatch += ", ";
		sBatch += sTrigger;
	}
	if (!sBatch.empty()) PrintLine(iSlot, " {LIGHTPURPLE}" + sBatch);
}

void RegisterEmoteCommand(const std::string& sName, bool bDance, bool bAdmin)
{
	if (sName.empty()) return;

	// Every spelling the engine dispatches lands on the same handler; s2sdk strips the trigger
	// characters of its settings ("!" and "/") from a chat command name.
	const std::vector<std::string> vConsole = { "mm_" + sName };
	const std::vector<std::string> vChat = { "!" + sName };
	pisex::g_pUtils->RegCommand(vConsole, vChat,
		[sName, bDance, bAdmin](int iSlot, const char* szContent) -> bool
		{
			const std::vector<std::string> vArgs = ParseCommandArgs(szContent);
			if (bAdmin) HandleSetCommand(iSlot, bDance, sName, vArgs);
			else HandleEmoteCommand(iSlot, bDance, vArgs);
			return true; // hide the chat line
		});
}

} // namespace

void RegisterCommands()
{
	if (!pisex::g_pUtils) return;

	for (const std::string& sName : g_Settings.EmoteCommand) RegisterEmoteCommand(sName, false, false);
	for (const std::string& sName : g_Settings.DanceCommand) RegisterEmoteCommand(sName, true, false);
	for (const std::string& sName : g_Settings.AdminSetEmoteCommand) RegisterEmoteCommand(sName, false, true);
	for (const std::string& sName : g_Settings.AdminSetDanceCommand) RegisterEmoteCommand(sName, true, true);

	// "css_et" and friends: the trigger list commands
	static const char* kBareNames[] = { "et", "etrigger", "etriggers", "dtrigger", "dtriggers" };
	static const char* kConsoleNames[] = { "css_et", "css_etrigger", "css_etriggers", "css_dtrigger", "css_dtriggers" };
	for (size_t i = 0; i < 5; i++)
	{
		const std::string sBare(kBareNames[i]);
		const std::string sConsole(kConsoleNames[i]);
		const std::vector<std::string> vConsole = { sConsole, "mm_" + sConsole };
		const std::vector<std::string> vChat = { "!" + sBare };
		pisex::g_pUtils->RegCommand(vConsole, vChat,
			[](int iSlot, const char*) -> bool
			{
				ShowTriggerList(iSlot);
				return true;
			});
	}

	pisex::g_pUtils->AddChatListenerPre(OnChatTrigger);
}
