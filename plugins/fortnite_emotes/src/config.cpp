// fortnite_emotes — settings, emote list, chat triggers, phrases and name lookup
// (the reference's config.cpp: SPEC §A, §C, §D.3, §F).
//
//   * configs/fortnite_emotes/settings.ini -> g_Settings
//   * configs/fortnite_emotes/emotes.ini   -> g_Emotes, g_ChatTriggers, g_TriggerOrder
//   * translations/fortnite_emotes.phrases.txt -> phrase table (parsed here as well, like the
//     original, because the Utils phrase store has no getter)
#include "fe.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

// ---------------------------------------------------------------------------------------------
// hl2sdk in_buttons.h bits (used by EmoteCancelButtons; the port has no hl2sdk).
// ---------------------------------------------------------------------------------------------

namespace
{
constexpr uint64_t IN_ATTACK = 1ull << 0;
constexpr uint64_t IN_JUMP = 1ull << 1;
constexpr uint64_t IN_DUCK = 1ull << 2;
constexpr uint64_t IN_FORWARD = 1ull << 3;
constexpr uint64_t IN_BACK = 1ull << 4;
constexpr uint64_t IN_USE = 1ull << 5;
constexpr uint64_t IN_MOVELEFT = 1ull << 9;
constexpr uint64_t IN_MOVERIGHT = 1ull << 10;
constexpr uint64_t IN_ATTACK2 = 1ull << 11;
constexpr uint64_t IN_SCORE = 1ull << 16;
constexpr uint64_t IN_SPEED = 1ull << 17;
constexpr uint64_t IN_LOOK_AT_WEAPON = 1ull << 25; // "inspect"
} // namespace

Settings g_Settings;
std::vector<Emote> g_Emotes;
std::unordered_map<std::string, int> g_ChatTriggers;
std::vector<std::string> g_TriggerOrder;

namespace {

const char* kSettingsPath = "settings.ini";
const char* kEmotesPath = "emotes.ini";
const char* kPhrasesPath = "fortnite_emotes.phrases.txt";

const char* kCameraModel = "models/chicken/chicken.vmdl";
const char* kDefaultSoundEventFile = "soundevents/fortnite_emotes.vsndevts";

// phrase key (lower cased) -> language -> text
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_mPhrases;

std::string Trim(const std::string& s)
{
	size_t iStart = 0, iEnd = s.size();
	while (iStart < iEnd && isspace((unsigned char)s[iStart])) iStart++;
	while (iEnd > iStart && isspace((unsigned char)s[iEnd - 1])) iEnd--;
	return s.substr(iStart, iEnd - iStart);
}

std::string Lower(const std::string& s)
{
	std::string result = s;
	for (char& c : result) c = (char)tolower((unsigned char)c);
	return result;
}

bool EqualsIgnoreCase(const std::string& a, const char* szB)
{
	const size_t iLen = strlen(szB);
	if (a.size() != iLen) return false;
	for (size_t i = 0; i < iLen; i++)
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)szB[i])) return false;
	return true;
}

std::string RemoveChars(const std::string& s, const char* szChars)
{
	std::string result = s;
	for (const char* p = szChars; *p; p++)
		result.erase(std::remove(result.begin(), result.end(), *p), result.end());
	return result;
}

// ";"-separated list; bKeepEmpty keeps empty entries ("" = everyone for the access lists)
std::vector<std::string> SplitList(const char* szValue, bool bKeepEmpty)
{
	std::vector<std::string> vec;
	if (!szValue) return vec;

	std::string sValue(szValue);
	size_t iStart = 0;
	while (true)
	{
		const size_t iPos = sValue.find(';', iStart);
		std::string sToken = Trim(sValue.substr(iStart, iPos == std::string::npos ? std::string::npos : iPos - iStart));
		if (bKeepEmpty || !sToken.empty()) vec.push_back(sToken);
		if (iPos == std::string::npos) break;
		iStart = iPos + 1;
	}
	return vec;
}

bool HasKey(pisex::KeyValues* kv, const char* szKey)
{
	return kv->FindKey(szKey) != nullptr;
}

std::vector<std::string> GetList(pisex::KeyValues* kv, const char* szKey, bool bKeepEmpty,
								 std::initializer_list<const char*> defaults)
{
	if (!HasKey(kv, szKey)) return std::vector<std::string>(defaults.begin(), defaults.end());
	return SplitList(kv->GetString(szKey, ""), bKeepEmpty);
}

std::string GetText(pisex::KeyValues* kv, const char* szKey, const char* szDefault)
{
	if (!HasKey(kv, szKey)) return szDefault;
	return kv->GetString(szKey, szDefault);
}

// ---------------------------------------------------------------------------------------------
// CSSharp colour tags -> the Utils upper-case form
// ---------------------------------------------------------------------------------------------

struct ColorTag
{
	const char* szFrom;
	const char* szTo;
};

const ColorTag kColorTags[] =
{
	{ "default", "{DEFAULT}" },
	{ "white", "{WHITE}" },
	{ "red", "{RED}" },
	{ "lightpurple", "{LIGHTPURPLE}" },
	{ "green", "{GREEN}" },
	{ "lime", "{LIME}" },
	{ "lightgreen", "{LIGHTGREEN}" },
	{ "darkred", "{DARKRED}" },
	{ "gray", "{GRAY}" },
	{ "lightolive", "{LIGHTOLIVE}" },
	{ "olive", "{OLIVE}" },
	{ "lightblue", "{LIGHTBLUE}" },
	{ "blue", "{BLUE}" },
	{ "purple", "{PURPLE}" },
	{ "lightred", "{LIGHTRED}" },
	{ "grayblue", "{GRAYBLUE}" },
};

std::string ConvertColorTags(const std::string& sText)
{
	std::string sOut;
	sOut.reserve(sText.size());

	size_t i = 0;
	while (i < sText.size())
	{
		if (sText[i] != '{')
		{
			sOut += sText[i++];
			continue;
		}

		const size_t iEnd = sText.find('}', i + 1);
		if (iEnd == std::string::npos || iEnd - i > 32)
		{
			sOut += sText[i++];
			continue;
		}

		const std::string sName = sText.substr(i + 1, iEnd - i - 1);
		const char* szReplacement = nullptr;
		for (const ColorTag& tag : kColorTags)
		{
			if (EqualsIgnoreCase(sName, tag.szFrom))
			{
				szReplacement = tag.szTo;
				break;
			}
		}

		if (szReplacement) sOut += szReplacement;
		else sOut += sText.substr(i, iEnd - i + 1); // {0} placeholders and unknown tags stay
		i = iEnd + 1;
	}
	return sOut;
}

// ---------------------------------------------------------------------------------------------
// settings.ini / emotes.ini
// ---------------------------------------------------------------------------------------------

struct ButtonBit
{
	const char* szName;
	uint64_t uiBit;
};

const ButtonBit kCancelButtons[] =
{
	{ "w", IN_FORWARD },
	{ "s", IN_BACK },
	{ "a", IN_MOVELEFT },
	{ "d", IN_MOVERIGHT },
	{ "use", IN_USE },
	{ "speed", IN_SPEED },
	{ "walk", IN_SPEED },
	{ "jump", IN_JUMP },
	{ "leftclick", IN_ATTACK },
	{ "rightclick", IN_ATTACK2 },
	{ "crouch", IN_DUCK },
	{ "scoreboard", IN_SCORE },
	{ "inspect", IN_LOOK_AT_WEAPON },
};

uint64_t ParseCancelButtons(const char* szValue)
{
	uint64_t uiButtons = 0;
	if (!szValue) return uiButtons;

	const std::string sValue(szValue);
	size_t iStart = 0;
	while (true)
	{
		const size_t iComma = sValue.find(',', iStart), iSemi = sValue.find(';', iStart);
		const size_t iPos = iComma < iSemi ? iComma : iSemi;
		const std::string sName = Lower(Trim(sValue.substr(iStart, iPos == std::string::npos ? std::string::npos : iPos - iStart)));
		if (!sName.empty())
		{
			for (const ButtonBit& button : kCancelButtons)
			{
				if (sName == button.szName)
				{
					uiButtons |= button.uiBit;
					break;
				}
			}
		}
		if (iPos == std::string::npos) break;
		iStart = iPos + 1;
	}
	return uiButtons;
}

bool LoadSettings(Settings& settings)
{
	const std::string sPath = pisex::ConfigDir() + "/" + kSettingsPath;
	bool ok = false;
	const std::string sText = pisex::ReadFile(sPath, &ok);
	pisex::KeyValues kvSettings("Settings");
	if (!ok || !kvSettings.LoadFromBuffer(sText))
	{
		LogWarning("Failed to load %s, using built-in defaults", sPath.c_str());
		return false;
	}

	settings.EmoteAllowedPeriod = kvSettings.GetInt("EmoteAllowedPeriod", settings.EmoteAllowedPeriod);
	settings.StopEmoteAfterFreezetimeEnd = kvSettings.GetBool("StopEmoteAfterFreezetimeEnd", settings.StopEmoteAfterFreezetimeEnd);
	settings.StopDamageWhenInEmote = kvSettings.GetBool("StopDamageWhenInEmote", settings.StopDamageWhenInEmote);
	settings.EmoteHidePlayers = kvSettings.GetInt("EmoteHidePlayers", settings.EmoteHidePlayers);
	settings.EmoteFreezePlayer = kvSettings.GetBool("EmoteFreezePlayer", settings.EmoteFreezePlayer);
	settings.ForcePlayerVisibility = kvSettings.GetBool("ForcePlayerVisibility", settings.ForcePlayerVisibility);
	settings.EmoteGlovesFix = kvSettings.GetBool("EmoteGlovesFix", settings.EmoteGlovesFix);
	settings.CameraMoveable = kvSettings.GetBool("CameraMoveable", settings.CameraMoveable);
	settings.CameraBlockObject = kvSettings.GetBool("CameraBlockObject", settings.CameraBlockObject);
	settings.EmoteCooldown = kvSettings.GetInt("EmoteCooldown", settings.EmoteCooldown);
	settings.EmoteVIPCooldown = kvSettings.GetInt("EmoteVIPCooldown", settings.EmoteVIPCooldown);
	settings.CancelButtons = ParseCancelButtons(kvSettings.GetString("EmoteCancelButtons", ""));
	settings.SoundModuleEnabled = kvSettings.GetBool("SoundModuleEnabled", settings.SoundModuleEnabled);
	settings.ChatTriggersEnabled = kvSettings.GetBool("ChatTriggersEnabled", settings.ChatTriggersEnabled);
	settings.SoundRecipients = kvSettings.GetInt("SoundRecipients", settings.SoundRecipients);
	settings.MaxEmoteSeconds = kvSettings.GetFloat("MaxEmoteSeconds", settings.MaxEmoteSeconds);
	settings.DebugLogs = kvSettings.GetBool("DebugLogs", settings.DebugLogs);

	// The reference's gamedata patterns (SigFireOutputInternal / SigTraceShape /
	// SigGetGameTraceManager) are not used in this port - the engine functions they resolved are
	// reached through s2sdk (entity output hook, traces).
	if (HasKey(&kvSettings, "SigFireOutputInternal") || HasKey(&kvSettings, "SigTraceShape")
		|| HasKey(&kvSettings, "SigGetGameTraceManager"))
		LogInfo("the Sig* gamedata keys of the reference are ignored (s2sdk provides the same features)");

	settings.SoundEventFiles = SplitList(kvSettings.GetString("EmoteSoundEventFiles", ""), false);
	if (settings.SoundEventFiles.empty()) settings.SoundEventFiles.push_back(kDefaultSoundEventFile);

	settings.EmoteCommand = GetList(&kvSettings, "EmoteCommand", false, { "emote", "emotes" });
	settings.DanceCommand = GetList(&kvSettings, "DanceCommand", false, { "dance", "dances" });
	settings.AdminSetEmoteCommand = GetList(&kvSettings, "AdminSetEmoteCommand", false, { "setemote", "setemotes" });
	settings.AdminSetDanceCommand = GetList(&kvSettings, "AdminSetDanceCommand", false, { "setdance", "setdances" });

	settings.EmoteDanceCommandPerm = GetList(&kvSettings, "EmoteDanceCommandPerm", true, { "" });
	settings.AdminSetEmoteDanceCommandPerm = GetList(&kvSettings, "AdminSetEmoteDanceCommandPerm", true, { "emotes.admin" });
	settings.VIPPerm = GetList(&kvSettings, "VIPPerm", true, { "vip" });

	settings.VIPCooldownFeature = Trim(GetText(&kvSettings, "VIPCooldownFeature", "emotes_cooldown"));

	// config-compat keys: EmoteMenuType / KitsuneMenuDeveloperDisplay do not change behaviour
	const int iMenuType = kvSettings.GetInt("EmoteMenuType", 2);
	if (iMenuType != 2)
		LogWarning("%s: EmoteMenuType %d is not supported, the port always uses its own menu (2 = KitsuneMenu)", sPath.c_str(), iMenuType);
	const int iConfigVersion = kvSettings.GetInt("ConfigVersion", 5);
	if (iConfigVersion < 5)
		LogWarning("%s: ConfigVersion %d is older than %d", sPath.c_str(), iConfigVersion, 5);

	return true;
}

bool LoadEmotes(std::vector<Emote>& vecEmotes)
{
	const std::string sPath = pisex::ConfigDir() + "/" + kEmotesPath;
	bool ok = false;
	const std::string sText = pisex::ReadFile(sPath, &ok);
	pisex::KeyValues kvEmotes("Emotes");
	if (!ok || !kvEmotes.LoadFromBuffer(sText))
	{
		LogWarning("Failed to load %s", sPath.c_str());
		return false;
	}

	for (pisex::KeyValues* pKey = kvEmotes.GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
	{
		const char* szName = pKey->GetName();
		if (!szName || !*szName) continue;

		Emote emote;
		emote.Name = szName;
		emote.IsEmote = Lower(pKey->GetString("type", "emote")) != "dance";
		emote.Model = pKey->GetString("model", "");
		emote.AnimIntro = pKey->GetString("anim_intro", "");
		emote.AnimLoop = pKey->GetString("anim_loop", "");
		emote.AnimLoopAfter = pKey->GetFloat("anim_loop_after", -1.0f);
		emote.Duration = pKey->GetFloat("duration", -1.0f);
		emote.Sound = pKey->GetString("sound", "");
		emote.SoundVolume = pKey->GetFloat("sound_volume", 0.7f);
		emote.SoundLoopAfter = pKey->GetFloat("sound_loop_after", -1.0f);
		emote.Triggers = SplitList(pKey->GetString("triggers", ""), false);
		emote.Permission = SplitList(pKey->GetString("permission", ""), true);
		if (emote.Permission.empty()) emote.Permission.push_back("");

		vecEmotes.push_back(std::move(emote));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// phrases
// ---------------------------------------------------------------------------------------------

void LoadPhrases()
{
	g_mPhrases.clear();

	const std::string sPath = pisex::TranslationsDir() + "/" + kPhrasesPath;
	bool ok = false;
	const std::string sText = pisex::ReadFile(sPath, &ok);
	pisex::KeyValues kvPhrases("Phrases");
	if (!ok || !kvPhrases.LoadFromBuffer(sText))
	{
		LogWarning("Failed to load %s", sPath.c_str());
		return;
	}

	for (pisex::KeyValues* pKey = kvPhrases.GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
	{
		std::unordered_map<std::string, std::string> mLanguages;
		for (pisex::KeyValues* pLang = pKey->GetFirstSubKey(); pLang; pLang = pLang->GetNextKey())
			mLanguages[Lower(pLang->GetName())] = std::string(pLang->GetString(nullptr, ""));
		g_mPhrases[Lower(pKey->GetName())] = std::move(mLanguages);
	}
	DebugLog("Loaded %d phrases from %s", (int)g_mPhrases.size(), sPath.c_str());
}

} // namespace

// ---------------------------------------------------------------------------------------------
// config entry points (fe.hpp)
// ---------------------------------------------------------------------------------------------

bool LoadConfig()
{
	Settings settings;
	LoadSettings(settings);

	std::vector<Emote> vecEmotes;
	LoadEmotes(vecEmotes);

	g_Settings = settings;
	g_Emotes = std::move(vecEmotes);

	// chat triggers: exact, case-sensitive, first entry wins, order for the trigger list
	g_ChatTriggers.clear();
	g_TriggerOrder.clear();
	int iEmotes = 0, iDances = 0;
	for (size_t i = 0; i < g_Emotes.size(); i++)
	{
		if (g_Emotes[i].IsEmote) iEmotes++;
		else iDances++;

		for (const std::string& sTrigger : g_Emotes[i].Triggers)
		{
			if (g_ChatTriggers.emplace(sTrigger, (int)i).second)
				g_TriggerOrder.push_back(sTrigger);
		}
	}

	LoadPhrases();

	LogInfo("%d emotes, %d dances", iEmotes, iDances);
	if (g_Emotes.empty())
	{
		LogWarning("No emotes/dances configured! Check %s", (pisex::ConfigDir() + "/" + kEmotesPath).c_str());
		return false;
	}
	return true;
}

std::vector<std::string> GetPrecacheList()
{
	std::vector<std::string> vecPrecache;
	auto add = [&vecPrecache](const std::string& sModel)
	{
		if (sModel.empty()) return;
		if (std::find(vecPrecache.begin(), vecPrecache.end(), sModel) != vecPrecache.end()) return;
		vecPrecache.push_back(sModel);
	};

	for (const Emote& emote : g_Emotes) add(emote.Model);
	add(kCameraModel);

	bool bHasSoundFile = false;
	for (const std::string& sSoundFile : g_Settings.SoundEventFiles)
	{
		if (sSoundFile.empty()) continue;
		bHasSoundFile = true;
		add(sSoundFile);
	}
	if (!bHasSoundFile) add(kDefaultSoundEventFile);

	return vecPrecache;
}

std::string Phrase(const char* key)
{
	if (!key) return {};

	auto it = g_mPhrases.find(Lower(key));
	if (it == g_mPhrases.end()) return key; // no translation -> the key itself

	const char* szLanguage = pisex::g_pUtils->GetLanguage();
	if (szLanguage && *szLanguage)
	{
		auto itLang = it->second.find(Lower(szLanguage));
		if (itLang != it->second.end() && !itLang->second.empty()) return itLang->second;
	}

	auto itEn = it->second.find("en");
	if (itEn != it->second.end() && !itEn->second.empty()) return itEn->second;

	return key;
}

std::string FormatPhrase(const char* key, std::initializer_list<std::string> args)
{
	const std::string sText = Phrase(key);
	const std::vector<std::string> vecArgs(args);

	std::string sOut;
	sOut.reserve(sText.size());
	for (size_t i = 0; i < sText.size();)
	{
		if (sText[i] == '{')
		{
			size_t j = i + 1;
			int iIndex = 0;
			bool bDigits = false;
			while (j < sText.size() && sText[j] >= '0' && sText[j] <= '9')
			{
				iIndex = iIndex * 10 + (sText[j] - '0');
				j++;
				bDigits = true;
			}
			if (bDigits && j < sText.size() && sText[j] == '}' && (size_t)iIndex < vecArgs.size())
			{
				sOut += vecArgs[iIndex];
				i = j + 1;
				continue;
			}
		}
		sOut += sText[i++];
	}
	return sOut;
}

std::string Prefix()
{
	return Phrase("emote.prefix");
}

void Print(int iSlot, const std::string& msg)
{
	if (!pisex::g_pUtils || iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	const std::string sText = ConvertColorTags(msg);
	pisex::g_pUtils->PrintToChat(iSlot, "%s", sText.c_str());
}

std::string EmoteDisplayName(const Emote& emote)
{
	return Phrase(emote.Name.c_str());
}

namespace {

// "_"-separated tokens of the config name; the words Emote/Dance are ignored
bool NameTokenMatches(const std::string& sName, const std::string& sLowerInput)
{
	size_t iStart = 0;
	while (iStart <= sName.size())
	{
		const size_t iPos = sName.find('_', iStart);
		const std::string sToken = sName.substr(iStart, iPos == std::string::npos ? std::string::npos : iPos - iStart);
		if (!sToken.empty() && !EqualsIgnoreCase(sToken, "Emote") && !EqualsIgnoreCase(sToken, "Dance")
			&& Lower(sToken) == sLowerInput)
			return true;
		if (iPos == std::string::npos) break;
		iStart = iPos + 1;
	}
	return false;
}

// space-separated tokens of the translated display name
bool DisplayTokenMatches(const std::string& sDisplay, const std::string& sLowerInput)
{
	size_t iStart = 0;
	while (iStart <= sDisplay.size())
	{
		const size_t iPos = sDisplay.find(' ', iStart);
		const std::string sToken = sDisplay.substr(iStart, iPos == std::string::npos ? std::string::npos : iPos - iStart);
		if (!sToken.empty() && Lower(sToken) == sLowerInput) return true;
		if (iPos == std::string::npos) break;
		iStart = iPos + 1;
	}
	return false;
}

// name without the leading Emote_/Dance_ and without "_", or the display name without spaces
bool CompactMatches(const Emote& emote, const std::string& sCompactInput)
{
	std::string sName = emote.Name;
	const std::string sLowerName = Lower(sName);
	if (sLowerName.rfind("emote_", 0) == 0 || sLowerName.rfind("dance_", 0) == 0)
		sName = sName.substr(6);
	if (Lower(RemoveChars(sName, "_")) == sCompactInput) return true;

	const std::string sDisplay = RemoveChars(EmoteDisplayName(emote), " ");
	return Lower(sDisplay) == sCompactInput;
}

} // namespace

int GetEmoteByName(const std::string& input, bool isEmote, bool partial)
{
	if (input.empty()) return -1;
	const std::string sLowerInput = Lower(input);

	if (!partial)
	{
		for (size_t i = 0; i < g_Emotes.size(); i++)
		{
			if (g_Emotes[i].IsEmote != isEmote) continue;
			if (Lower(g_Emotes[i].Name) == sLowerInput) return (int)i;
			if (Lower(EmoteDisplayName(g_Emotes[i])) == sLowerInput) return (int)i;
		}
		return -1;
	}

	const std::string sCompactInput = RemoveChars(sLowerInput, " _");
	int iTokenMatch = -1, iCompactMatch = -1;
	for (size_t i = 0; i < g_Emotes.size(); i++)
	{
		const Emote& emote = g_Emotes[i];
		if (emote.IsEmote != isEmote) continue;

		if (iTokenMatch < 0
			&& (NameTokenMatches(emote.Name, sLowerInput) || DisplayTokenMatches(EmoteDisplayName(emote), sLowerInput)))
			iTokenMatch = (int)i;
		if (iCompactMatch < 0 && CompactMatches(emote, sCompactInput))
			iCompactMatch = (int)i;
	}

	return iTokenMatch >= 0 ? iTokenMatch : iCompactMatch;
}
