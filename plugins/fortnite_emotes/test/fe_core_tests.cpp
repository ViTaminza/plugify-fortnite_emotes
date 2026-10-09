// Tests for plugins/fortnite_emotes/src/config.cpp (the engine-free part of the ported Fortnite
// Emotes plugin: settings.ini + emotes.ini + phrases + the D.3 name lookup).
//
// The files come from the live server (ref/prod-27016/addons: configs/fortnite_emotes/{settings,
// emotes}.ini and translations/fortnite_emotes.phrases.txt), driven through pisexcompat's path
// resolver exactly like the plugin does it.
//
// The engine-facing half of the plugin (props, camera, weapons, hooks) needs a live server; the
// pure helpers of it (the double-E style state machines) are covered by the graffiti plugin's
// tests, this file stays on the config layer.

#include <plg/plugin.hpp>

#include <pisex/paths.hpp>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "fe.hpp"

// The plugin gets these from PLUGIFY_PLUGIN; pisexcompat references them, the test leaves them null.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

#ifndef FE_TEST_CONFIGS_DIR
#define FE_TEST_CONFIGS_DIR "."
#endif
#ifndef FE_TEST_TRANSLATIONS_DIR
#define FE_TEST_TRANSLATIONS_DIR "."
#endif

// config.cpp logs through these; the test prints them (they are defined in plugin.cpp for the real
// plugin).
void LogInfo(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	std::printf("   info: ");
	std::vprintf(fmt, args);
	std::printf("\n");
	va_end(args);
}

void LogWarning(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	std::printf("   warn: ");
	std::vprintf(fmt, args);
	std::printf("\n");
	va_end(args);
}

void DebugLog(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	std::printf("   dbg:  ");
	std::vprintf(fmt, args);
	std::printf("\n");
	va_end(args);
}

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name)
{
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),             \
						g_section.data(), __FILE__, __LINE__, #expr);                              \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_ = (actual);                                                           \
		const std::string e_ = (expected);                                                         \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_.c_str(), e_.c_str());                                                   \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                            \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_, e_);                                                                   \
		}                                                                                          \
	} while (false)

void TestConfig()
{
	Section("config: the production settings.ini / emotes.ini / phrases");

	// exactly like the plugin: the configs live in the plugin dir, the phrases in translations/
	pisex::SetPluginName("fortnite_emotes");
	pisex::SetConfigDir(FE_TEST_CONFIGS_DIR);
	pisex::SetTranslationsDir(FE_TEST_TRANSLATIONS_DIR);

	CHECK(LoadConfig());

	// --- settings.ini -----------------------------------------------------
	CHECK_EQ_INT(g_Settings.EmoteAllowedPeriod, 0);
	CHECK(!g_Settings.StopEmoteAfterFreezetimeEnd);
	CHECK(!g_Settings.StopDamageWhenInEmote);
	CHECK_EQ_INT(g_Settings.EmoteHidePlayers, 0);
	CHECK(g_Settings.EmoteFreezePlayer);
	CHECK(!g_Settings.ForcePlayerVisibility);
	CHECK(g_Settings.EmoteGlovesFix);
	CHECK(g_Settings.CameraMoveable);
	CHECK(g_Settings.CameraBlockObject);
	CHECK_EQ_INT(g_Settings.EmoteCooldown, 20);
	CHECK_EQ_INT(g_Settings.EmoteVIPCooldown, 15);
	CHECK(g_Settings.SoundModuleEnabled);
	CHECK(g_Settings.ChatTriggersEnabled);
	CHECK_EQ_INT(g_Settings.SoundRecipients, 0);
	CHECK(!g_Settings.DebugLogs);
	CHECK(g_Settings.MaxEmoteSeconds > 59.0f && g_Settings.MaxEmoteSeconds < 61.0f);
	CHECK_EQ_STR(g_Settings.VIPCooldownFeature, "emotes_cooldown");

	// EmoteCancelButtons "w,a,s,d,jump,crouch,leftclick" -> IN_FORWARD|IN_BACK|IN_MOVELEFT|
	// IN_MOVERIGHT|IN_JUMP|IN_DUCK|IN_ATTACK
	const uint64_t kWanted = (1ull << 3) | (1ull << 4) | (1ull << 9) | (1ull << 10) | (1ull << 1) | (1ull << 2)
		| (1ull << 0);
	CHECK_EQ_INT(g_Settings.CancelButtons, kWanted);

	// command and access lists
	CHECK_EQ_INT(g_Settings.EmoteCommand.size(), 2);
	CHECK_EQ_STR(g_Settings.EmoteCommand[0], "emote");
	CHECK_EQ_STR(g_Settings.EmoteCommand[1], "emotes");
	CHECK_EQ_STR(g_Settings.DanceCommand[0], "dance");
	CHECK_EQ_STR(g_Settings.AdminSetEmoteCommand[0], "setemote");
	CHECK_EQ_STR(g_Settings.AdminSetDanceCommand[0], "setdance");
	CHECK_EQ_INT(g_Settings.EmoteDanceCommandPerm.size(), 1);
	CHECK_EQ_STR(g_Settings.EmoteDanceCommandPerm[0], "vip:emotes");
	CHECK(g_Settings.VIPPerm.size() >= 1);
	CHECK(!g_Settings.SoundEventFiles.empty());
	CHECK_EQ_STR(g_Settings.SoundEventFiles[0], "soundevents/fortnite_emotes.vsndevts");

	// --- emotes.ini -------------------------------------------------------
	CHECK_EQ_INT(g_Emotes.size(), 80); // 37 emotes + 43 dances of the official v1.1.8 config
	int iEmotes = 0, iDances = 0;
	for (const Emote& e : g_Emotes)
		(e.IsEmote ? iEmotes : iDances)++;
	CHECK_EQ_INT(iEmotes, 37);
	CHECK_EQ_INT(iDances, 43);

	const Emote& first = g_Emotes[0];
	CHECK_EQ_STR(first.Name, "DanceMoves");
	CHECK(!first.IsEmote);
	CHECK_EQ_STR(first.Model, "characters/kolka/fortnite_dance.vmdl");
	CHECK_EQ_STR(first.AnimIntro, "DanceMoves");
	CHECK(first.AnimLoop.empty());
	CHECK_EQ_INT((int)first.AnimLoopAfter, -1);
	CHECK_EQ_INT((int)first.Duration, -1);
	CHECK_EQ_STR(first.Sound, "DanceMoves1");
	CHECK(first.SoundVolume > 0.69f && first.SoundVolume < 0.71f);
	CHECK(first.SoundLoopAfter > 6.7f && first.SoundLoopAfter < 6.8f);
	CHECK_EQ_INT(first.Triggers.size(), 1);
	CHECK_EQ_STR(first.Triggers[0], "dance");
	CHECK_EQ_INT(first.Permission.size(), 1);
	CHECK(first.Permission[0].empty());

	// a record with several triggers
	int iMultiTrigger = -1;
	for (size_t i = 0; i < g_Emotes.size(); i++)
		if (g_Emotes[i].Triggers.size() > 1) { iMultiTrigger = (int)i; break; }
	CHECK(iMultiTrigger >= 0);

	// --- chat triggers ----------------------------------------------------
	CHECK(g_ChatTriggers.size() >= 80);
	auto it = g_ChatTriggers.find("dance");
	CHECK(it != g_ChatTriggers.end() && it->second == 0);
	CHECK_EQ_INT(g_TriggerOrder.size(), (int)g_ChatTriggers.size());
	CHECK_EQ_STR(g_TriggerOrder[0], "dance");

	// --- precache list ----------------------------------------------------
	const std::vector<std::string> vPrecache = GetPrecacheList();
	CHECK(!vPrecache.empty());
	bool bHasModel = false, bHasCamera = false, bHasSound = false;
	for (const std::string& s : vPrecache)
	{
		bHasModel |= s == "characters/kolka/fortnite_dance.vmdl";
		bHasCamera |= s == "models/chicken/chicken.vmdl";
		bHasSound |= s == "soundevents/fortnite_emotes.vsndevts";
	}
	CHECK(bHasModel);
	CHECK(bHasCamera);
	CHECK(bHasSound);
	// distinct entries only
	bool bUnique = true;
	for (size_t i = 0; i < vPrecache.size(); i++)
		for (size_t j = i + 1; j < vPrecache.size(); j++)
			bUnique &= vPrecache[i] != vPrecache[j];
	CHECK(bUnique);

	// --- phrases ----------------------------------------------------------
	const std::string sPrefix = Prefix();
	CHECK(!sPrefix.empty());
	CHECK(sPrefix.find("CR-Emotes") != std::string::npos); // the production prefix phrase
	CHECK(Phrase("emote.playing").find("{0}") != std::string::npos);
	const std::string sFormatted = FormatPhrase("emote.playing", {"DanceMoves"});
	CHECK(sFormatted.find("DanceMoves") != std::string::npos);
	CHECK(sFormatted.find("{0}") == std::string::npos);
	// an unknown key falls back to the key itself
	CHECK_EQ_STR(Phrase("no.such.key"), "no.such.key");
	// the display name of an emote is its translation
	CHECK(EmoteDisplayName(g_Emotes[0]).size() > 0);
}

void TestNameLookup()
{
	Section("name lookup (SPEC §D.3)");
	// exact name, exact display name
	CHECK_EQ_INT(GetEmoteByName("DanceMoves", false, false), 0);
	// the Emote_/Dance_ prefixes are optional, case does not matter
	const int iByPartial = GetEmoteByName("dancemoves", false, true);
	CHECK_EQ_INT(iByPartial, 0);
	// a word that appears in the config name of a dance is found
	const int iElectro = GetEmoteByName("electro", false, true);
	CHECK(iElectro >= 0);
	CHECK_EQ_STR(g_Emotes[iElectro].Name, "ElectroShuffle");
	// an emote (not a dance) is not returned for a dance query and vice versa
	CHECK_EQ_INT(GetEmoteByName("dancemoves", true, true), -1);
	CHECK_EQ_INT(GetEmoteByName("no-such-emote-at-all", false, true), -1);
	CHECK_EQ_INT(GetEmoteByName("", false, true), -1);
	// an emote is found through its config-name token (the Emote_ prefix is ignored)
	const int iDab = GetEmoteByName("dab", true, true);
	CHECK(iDab >= 0);
	CHECK(g_Emotes[iDab].IsEmote);
	CHECK_EQ_STR(g_Emotes[iDab].Name, "Emote_Dab");
	// the same word queries the dance list and finds a dance (or nothing, never an emote)
	const int iDabDance = GetEmoteByName("dab", false, true);
	CHECK(iDabDance != iDab);
	if (iDabDance >= 0) CHECK(!g_Emotes[iDabDance].IsEmote);
	// a record whose name starts with "Emote_" can still be a dance (the config says so)
	int iDanceWithEmoteName = -1;
	for (size_t i = 0; i < g_Emotes.size(); i++)
		if (!g_Emotes[i].IsEmote && g_Emotes[i].Name.rfind("Emote_", 0) == 0) { iDanceWithEmoteName = (int)i; break; }
	CHECK(iDanceWithEmoteName >= 0);
	CHECK_EQ_INT(GetEmoteByName(g_Emotes[iDanceWithEmoteName].Name, true, false), -1); // queried as an emote
	CHECK_EQ_INT(GetEmoteByName(g_Emotes[iDanceWithEmoteName].Name, false, false), iDanceWithEmoteName);
}

void TestHandleValues()
{
	Section("entity handle values (the int32 view of CEntityHandle)");

	// Valid <=> neither 0 nor INVALID_EHANDLE_INDEX (-1); the sign means nothing. Live example from
	// the test server: the tester's pawn handle was -921534371 = 0xC912805D = index 93 with serial
	// 102949 - minutes after the server start the serial passes 65535 and sets bit 31, so a sign
	// test would throw every new entity away.
	CHECK(IsValidHandle(-921534371));
	CHECK(IsValidHandle((int32_t)0xC912805Du));
	CHECK(IsValidHandle(1));           // a first-map entity: index 1, serial 0
	CHECK(IsValidHandle(0x7FFFFFFF));  // index 0x7FFF, serial 0xFFFF (the last "positive" handle)
	CHECK(!IsValidHandle(0));          // "none", also index 0 serial 0 = the world
	CHECK(!IsValidHandle(-1));         // INVALID_EHANDLE_INDEX
}

} // namespace

int main()
{
	TestConfig();
	TestNameLookup();
	TestHandleValues();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
