#include "precompiled.h"

#include "ai_chat.h"

#include "bot/cs_bot_manager.h"
#include "bot/cs_bot.h"
#include "gamerules.h"
#include "game_shared/bot/bot.h"
#include "weapons.h"

// Minimal async Ollama client (HTTP over TCP) and response pump.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <chrono>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Cvars defined/registered in dlls/game.cpp
extern cvar_t sv_ai_chat_enable;
extern cvar_t sv_ai_ollama_url;
extern cvar_t sv_ai_ollama_model;
extern cvar_t sv_ai_timeout_ms;
extern cvar_t sv_ai_max_chars;
extern cvar_t sv_ai_debug;
extern cvar_t sv_ai_chat_scope;
extern cvar_t sv_ai_chat_team_tag;

extern int gmsgSayText;

// Absolute gpGlobals->time until which this client index should not move (alive AI "typing").
static float s_chatMoveFreezeUntil[MAX_CLIENTS + 1] = { 0.0f };

static void ChatFreezeSet(int ent, float untilTime)
{
	if (ent >= 1 && ent <= MAX_CLIENTS)
		s_chatMoveFreezeUntil[ent] = Q_max(s_chatMoveFreezeUntil[ent], untilTime);
}

static void ChatFreezeClear(int ent)
{
	if (ent >= 1 && ent <= MAX_CLIENTS)
		s_chatMoveFreezeUntil[ent] = 0.0f;
}

static void ChatFreezeClearAll()
{
	for (int i = 1; i <= MAX_CLIENTS; ++i)
		s_chatMoveFreezeUntil[i] = 0.0f;
}

namespace
{
	// Human-ish timing / alive speak rates (hardcoded; change in source if needed).
	namespace AiTiming
	{
		constexpr float kAliveKillSpeakProb = 0.28f;
		constexpr float kAliveBombSpeakProb = 0.52f;
		constexpr float kAliveRoundSpeakProb = 0.50f;
		// Seconds before "typing" may finish after the event (game time).
		// Reaction delay before "typing" begins after an event was queued (game time).
		// Keep alive reactions short; long delays look like bots lagging.
		constexpr float kAliveBombRoundReactMin = 3.0f;
		constexpr float kAliveBombRoundReactMax = 6.5f;
		constexpr float kAliveKillReactMin = 1.2f;
		constexpr float kAliveKillReactMax = 2.9f;
		constexpr float kAliveQuickReactMin = 0.5f; // e.g. friendly fire, flashed
		constexpr float kAliveQuickReactMax = 1.2f;
		constexpr float kDeadReactMin = 1.4f;
		constexpr float kDeadReactMax = 4.0f;
		// Extra lines on kills (victim / dead spectators), independent of killer line.
		constexpr float kVictimDeathChatProb = 0.55f;
		constexpr float kDeadSpectatorKillProb = 0.38f;
		constexpr float kDeadBombReactMin = 2.2f;
		constexpr float kDeadBombReactMax = 5.5f;
		// Minimum game time between ANY two delivered AI chat lines (all bots).
		constexpr float kGlobalMinGapBetweenAnyLines = 5.5f;
		// After a bot speaks, block that entindex from speaking again for this long.
		constexpr float kAlivePostSpeakMin = 22.0f;
		constexpr float kAlivePostSpeakMax = 42.0f;
		constexpr float kDeadPostSpeakMin = 5.0f;
		constexpr float kDeadPostSpeakMax = 14.0f;
		// Typing sim (alive only). Keep it snappy: long typing feels like lag.
		constexpr float kTypeCharMin = 0.20f;
		constexpr float kTypeCharMax = 0.30f;
		constexpr float kTypeCapSec = 5.0f;
		// Ollama output cap (alive bots: very short lines only).
		constexpr int kAliveMaxCharsPredict = 34;
		constexpr size_t kAliveMaxWordsDeliver = 5u;
	}

	const char *BotChatPersona(CBasePlayer *p)
	{
		if (!p || !p->IsBot())
			return nullptr;
		const CBot *bot = static_cast<const CBot *>(p);
		const BotProfile *prof = bot->GetProfile();
		if (!prof)
			return nullptr;
		const char *s = prof->GetChatPersona();
		return (s && s[0]) ? s : nullptr;
	}

	// Names from game_shared/GameEvent.h + game_shared/bot/bot_manager.cpp (GameEventName[]).
	const char *EventName(GameEventType event)
	{
		const int e = (int)event;
		if (e >= 0 && e < NUM_GAME_EVENTS && GameEventName[e] && GameEventName[e][0])
			return GameEventName[e];
		return "EVENT_BAD_INDEX";
	}

	// Engine current level: gpGlobals->mapname (BSP name, no path), e.g. de_dust vs de_dust2.
	static std::string MapInternalLower(const char *mapRaw)
	{
		if (!mapRaw || !mapRaw[0])
			return {};
		std::string m = mapRaw;
		for (char &c : m)
		{
			if (c >= 'A' && c <= 'Z')
				c = char(c - 'A' + 'a');
		}
		return m;
	}

	static void AppendMapPlayContext(std::string &prompt, const char *mapInternalRaw)
	{
		const std::string low = MapInternalLower(mapInternalRaw);
		prompt += "CURRENT_MAP_FILE=";
		prompt += low.empty() ? "unknown" : low;
		prompt += " (exact server map from engine; only strategies/callouts for THIS layout)\n";

		if (low == "de_dust" || low == "cs_dust")
		{
			prompt += "MAP_PLAY_CONTEXT= Original **Dust** (small 1.6 defuse map). Players say \"dust\" or tunnels/A/B style. ";
			prompt += "**Not** Dust II — never say dust2 or d2 for this server.\n";
		}
		else if (low == "de_dust2" || low == "cs_dust2")
		{
			prompt += "MAP_PLAY_CONTEXT= **Dust II** (big desert defuse). Players say dust2 or d2. **Not** classic de_dust.\n";
		}
		else if (!low.empty())
		{
			size_t off = 0;
			if (low.size() > 3 && !Q_strnicmp(low.c_str(), "de_", 3))
				off = 3;
			else if (low.size() > 3 && !Q_strnicmp(low.c_str(), "cs_", 3))
				off = 3;
			else if (low.size() > 3 && !Q_strnicmp(low.c_str(), "as_", 3))
				off = 3;
			else if (low.size() > 3 && !Q_strnicmp(low.c_str(), "fy_", 3))
				off = 3;
			const std::string spoken = (off > 0 && off < low.size()) ? low.substr(off) : low;
			prompt += "MAP_PLAY_CONTEXT= This server is ";
			prompt += low;
			prompt += ". In comms people often shorten it to \"";
			prompt += spoken;
			prompt += "\". Do not pretend we are on some other famous map.\n";
		}
		else
		{
			prompt += "MAP_PLAY_CONTEXT= Map unknown; avoid naming specific maps.\n";
		}
	}

	// Hard anchor: CS 1.6 only — map, layout, team, event. No other-game or modern ranked jargon.
	static void AppendCs16Grounding(std::string &prompt, GameEventType event, const char *speakerTeam)
	{
		prompt += "GAME= Counter-Strike 1.6 (GoldSrc): bomb defuse / hostage pickup pub. Not Valorant, not MOBA, not modern ranked team builders.\n";
		prompt += "ANCHOR_RULES= You may ONLY lean on: (1) CS 1.6 gameplay, (2) THIS map's real layout from CURRENT_MAP_FILE / MAP_PLAY_CONTEXT (bombsites, tunnels, CT/T spawn flavor — not made-up lanes), ";
		prompt += "(3) your side SPEAKER_TEAM=";
		prompt += (speakerTeam && *speakerTeam) ? speakerTeam : "UNKNOWN";
		prompt += " (terrorists vs CT as in 1.6), (4) the EVENT= and CONTEXT= lines — nothing else (no invented match systems).\n";
		prompt += "FORBIDDEN_NONCS= No draft / pick order / \"picked last\" / role queue / ranked ladder / ELO / MMR / matchmaking jokes / jungler / lanes / other FPS titles. Those are not part of 1.6 public servers.\n";
		prompt += "EVENT_MEANS= ";
		switch (event)
		{
		case EVENT_ROUND_START: prompt += "new round starting, players buying / moving out."; break;
		case EVENT_PLAYER_DIED: prompt += "someone just died in this round (see ACTOR/OTHER and KILL_ROLES if present)."; break;
		case EVENT_BOMB_PLANTED: prompt += "C4 is planted; CT retake / T post-plant situation."; break;
		case EVENT_BOMB_DEFUSED: prompt += "bomb was defused; round outcome shift."; break;
		default: prompt += "in-round moment tied to CONTEXT."; break;
		}
		prompt += "\n";
	}

	// C4 plant is Terrorist-only in defuse; CTs defuse / retake.
	static void AppendBombPlantRoleRules(std::string &prompt, TeamName team)
	{
		if (team == CT)
		{
			prompt += "BOMB_ROLE= You are **CT**. Only **Terrorists plant** the C4. Never say *you* need to plant, will plant, are planting, or \"plant at/in A/B\" as yourself. ";
			prompt += "CT job: hold, retake, save kit, **defuse** — not plant. You may warn that Ts might plant or bomb is down.\n";
		}
		else if (team == TERRORIST)
		{
			prompt += "BOMB_ROLE= You are **Terrorist**: your side **plants** C4; CTs **defuse**. Do not say *you* are defusing the bomb as a T.\n";
		}
		else if (team == SPECTATOR)
		{
			prompt += "BOMB_ROLE= Spectating: do not roleplay as actively planting or defusing in first person.\n";
		}
	}

	// Live round facts so chat cannot contradict obvious game state (e.g. "Ts camping" when all Ts dead).
	static void AppendRoundSnapshot(std::string &prompt)
	{
		if (!gpGlobals)
			return;

		int aliveT = 0;
		int aliveCT = 0;
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsPlayer())
				continue;
			if (p->m_iTeam == TERRORIST && p->IsAlive())
				++aliveT;
			else if (p->m_iTeam == CT && p->IsAlive())
				++aliveCT;
		}

		const char *bombPlanted = "unknown";
		if (TheBots)
			bombPlanted = TheCSBots()->IsBombPlanted() ? "yes" : "no";

		const char *bombDefused = "unknown";
		const char *bombExploded = "unknown";
		if (g_pGameRules)
		{
			CHalfLifeMultiplay *mp = static_cast<CHalfLifeMultiplay *>(g_pGameRules);
			bombDefused = mp->m_bBombDefused ? "yes" : "no";
			bombExploded = mp->m_bTargetBombed ? "yes" : "no";
		}

		prompt += "ROUND_SNAPSHOT= alive_T=";
		prompt += std::to_string(aliveT);
		prompt += " alive_CT=";
		prompt += std::to_string(aliveCT);
		prompt += " bomb_planted=";
		prompt += bombPlanted;
		prompt += " bomb_defused_this_round=";
		prompt += bombDefused;
		prompt += " bomb_exploded_this_round=";
		prompt += bombExploded;
		prompt += ". FACT_RULE= Your line must match this snapshot. If alive_T=0, **all Terrorists are dead** — do not imply living Ts are still camping, stacked, rushing sites, or \"still\" doing T-side things; ";
		prompt += "do not vague-blame Ts as active campers. If alive_CT=0, all CTs are dead — same for CT. If bomb_planted=no, the bomb is **not** live on a site — no \"bomb is down\" as if it just got planted; ";
		prompt += "if you are **Terrorist** and bomb_planted=no, do **not** ask for defuse help (only CTs defuse). If bomb_defused_this_round=yes or bomb_exploded_this_round=yes, the round's C4 situation is **over** — do not talk as if the bomb is still active or needs plant/defuse now.\n";
	}

	// Per-bot lines allowed this round (rolled at freeze/round start). Spent on each queued/sent AI line.
	static uint8_t s_botRoundChatBudget[MAX_CLIENTS + 1];
	static char s_botRoundTilt[MAX_CLIENTS + 1][16];
	// Per-bot death snapshot so dead spectators have fewer "unknown" fields.
	static char s_lastKilledBy[MAX_CLIENTS + 1][32];
	static int s_lastKilledByEnt[MAX_CLIENTS + 1];
	static char s_lastDeathLoc[MAX_CLIENTS + 1][48];
	static char s_lastDeathType[MAX_CLIENTS + 1][16];
	static char s_lastDeathWeapon[MAX_CLIENTS + 1][24];
	static uint16_t s_killedByCount[MAX_CLIENTS + 1][MAX_CLIENTS + 1];
	static std::deque<std::string> s_recentChatLines;

	static void ResetKillHistory()
	{
		for (int i = 1; i <= MAX_CLIENTS; ++i)
		{
			s_lastKilledByEnt[i] = 0;
			for (int j = 1; j <= MAX_CLIENTS; ++j)
				s_killedByCount[i][j] = 0;
		}
	}

	static void RollBotRoundChatBudgets()
	{
		if (!gpGlobals)
			return;
		for (int i = 1; i <= MAX_CLIENTS; ++i)
		{
			s_botRoundChatBudget[i] = 0;
			s_botRoundTilt[i][0] = '\0';
		}
		for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_CLIENTS; ++i)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsBot())
				continue;
			const float r = RANDOM_FLOAT(0.0f, 1.0f);
			int b;
			if (r < 0.25f)
				b = 0;
			else if (r < 0.65f)
				b = 1;
			else if (r < 0.90f)
				b = 2;
			else
				b = 3;
			// Debug-friendly: scale budgets up so bots can be very chatty.
			// (Alive chat is still heavily gated elsewhere; this mostly increases dead chatter.)
			b *= 3;
			s_botRoundChatBudget[i] = (uint8_t)Q_max(0, Q_min(b, 250));
			static const char *tilts[] = { "bored", "tilted", "chill", "salty", "checked_out" };
			Q_strlcpy(s_botRoundTilt[i], tilts[RANDOM_LONG(0, int(sizeof(tilts) / sizeof(tilts[0])) - 1)], sizeof(s_botRoundTilt[0]));
		}
	}

	static bool BotHasRoundChatBudget(CBasePlayer *speaker)
	{
		if (!speaker || !speaker->IsBot())
			return false;
		const int ei = speaker->entindex();
		if (ei < 1 || ei > MAX_CLIENTS)
			return false;
		return s_botRoundChatBudget[ei] > 0;
	}

	static bool ConsumeRoundChatBudget(CBasePlayer *speaker)
	{
		if (!speaker || !speaker->IsBot())
			return false;
		const int ei = speaker->entindex();
		if (ei < 1 || ei > MAX_CLIENTS)
			return false;
		if (s_botRoundChatBudget[ei] == 0)
			return false;
		--s_botRoundChatBudget[ei];
		return true;
	}

	static void RecordRecentAiChatLine(const char *line)
	{
		if (!line || !line[0])
			return;
		std::string s = line;
		std::string norm;
		norm.reserve(s.size());
		for (unsigned char uc : s)
			norm.push_back((char)std::tolower(uc));

		// Don't let bad chat poison future prompts.
		const char *bad[] = {
			"brb", " afk", "afk ", "lag city", "lazy af", "spinned", "|", "awped", "headshotted", "hax",
			"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"
		};
		for (const char *b : bad)
		{
			if (norm.find(b) != std::string::npos)
				return;
		}
		const std::string padded = " " + norm + " ";
		if (padded.find(" nt ") != std::string::npos || padded.find(" lol ") != std::string::npos || padded.find(" ofc ") != std::string::npos)
			return;
		if (s.size() > 96)
			s.resize(96);
		s_recentChatLines.push_back(std::move(s));
		while (s_recentChatLines.size() > 12)
			s_recentChatLines.pop_front();
	}

	static void AppendRecentChatBuffer(std::string &prompt)
	{
		if (s_recentChatLines.empty())
		{
			prompt += "RECENT_CHAT= (none recorded this session)\n";
			return;
		}
		prompt += "RECENT_CHAT= last lines on server (newest last): ";
		int emitted = 0;
		const size_t start = (s_recentChatLines.size() > 6) ? (s_recentChatLines.size() - 6) : 0;
		for (size_t i = start; i < s_recentChatLines.size(); ++i)
		{
			if (emitted)
				prompt += " | ";
			prompt += s_recentChatLines[i];
			++emitted;
		}
		prompt += "\n";
	}

	static void AppendScoreRoundUser(std::string &prompt)
	{
		if (!gpGlobals || !g_pGameRules)
			return;
		CHalfLifeMultiplay *mp = static_cast<CHalfLifeMultiplay *>(g_pGameRules);
		prompt += "SCORE_CT=";
		prompt += std::to_string((int)mp->m_iNumCTWins);
		prompt += " SCORE_T=";
		prompt += std::to_string((int)mp->m_iNumTerroristWins);
		prompt += " ROUNDS_PLAYED=";
		prompt += std::to_string(mp->m_iTotalRoundsPlayed);
		prompt += " ROUND_TIME_LEFT_SEC≈";
		const float left = mp->GetRoundRemainingTime();
		prompt += std::to_string((int)Q_max(0.0f, left));
		prompt += "\n";
	}

	static void AppendRosterUser(std::string &prompt)
	{
		if (!gpGlobals)
			return;
		prompt += "ROSTER= ";
		bool first = true;
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsPlayer() || !p->pev)
				continue;
			const char *nm = STRING(p->pev->netname);
			if (!nm || !nm[0])
				continue;
			if (!first)
				prompt += " | ";
			first = false;
			prompt += nm;
			prompt += " team=";
			prompt += GetTeam(p->m_iTeam);
			prompt += " state=";
			prompt += (p->pev->deadflag == DEAD_NO) ? "alive" : "dead";
		}
		prompt += "\n";
		prompt += "NAME_STATE_RULE= If you mention a player by name, do NOT describe them as camping/rushing/rotating/planting/defusing unless ROSTER shows them alive. If they are dead, only dead/spectator banter.\n";
	}

	static bool IsKnifeKill(CBasePlayer *killer)
	{
		if (!killer || killer->pev->deadflag != DEAD_NO)
			return false;
		CBasePlayerItem *item = killer->m_pActiveItem;
		if (!item)
			return false;
		const CBasePlayerWeapon *wpn = static_cast<CBasePlayerWeapon *>(item);
		return wpn->m_iId == WEAPON_KNIFE;
	}

	static const char *ActiveWeaponEntityName(CBasePlayer *player)
	{
		if (!player || !player->m_pActiveItem)
			return "unknown";
		const CBasePlayerWeapon *wpn = static_cast<CBasePlayerWeapon *>(player->m_pActiveItem);
		if (!wpn)
			return "unknown";
		WeaponInfoStruct *info = GetWeaponInfo(wpn->m_iId);
		if (!info || !info->entityName || !info->entityName[0])
			return "unknown";
		return info->entityName;
	}

	static const char *KillTypeForDeath(CBasePlayer *victim, CBasePlayer *killer)
	{
		if (killer && IsKnifeKill(killer))
			return "knife";
		if (victim && victim->m_LastHitGroup == HITGROUP_HEAD)
			return "headshot";
		return "kill";
	}

	static bool IsClutchKillMoment(CBasePlayer *killer)
	{
		if (!killer || !gpGlobals || killer->pev->deadflag != DEAD_NO)
			return false;
		const TeamName kt = killer->m_iTeam;
		if (kt != CT && kt != TERRORIST)
			return false;
		int alliesAlive = 0;
		int enemiesAlive = 0;
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsPlayer() || !p->IsAlive())
				continue;
			if (p->m_iTeam == kt)
				++alliesAlive;
			else if (p->m_iTeam == CT || p->m_iTeam == TERRORIST)
				++enemiesAlive;
		}
		return alliesAlive == 1 && enemiesAlive >= 2;
	}

	static bool PassesEventSpeakRoll(bool spectacular)
	{
		const float p = spectacular ? RANDOM_FLOAT(0.35f, 0.60f) : RANDOM_FLOAT(0.15f, 0.30f);
		return RANDOM_FLOAT(0.0f, 1.0f) < p;
	}

	static bool PassesSpeakProb(float p)
	{
		p = Q_max(0.0f, Q_min(p, 1.0f));
		return RANDOM_FLOAT(0.0f, 1.0f) < p;
	}

	static std::string PromptJsonEscape(const char *s)
	{
		if (!s)
			return "null";
		std::string out;
		out.reserve(Q_strlen(s) + 8);
		for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
		{
			const char c = (char)*p;
			if (c == '\\')
				out += "\\\\";
			else if (c == '"')
				out += "\\\"";
			else if (c == '\n' || c == '\r' || c == '\t')
				out += ' ';
			else if ((unsigned char)c < 0x20)
				out += ' ';
			else
				out += c;
		}
		return out;
	}

	static const char *TeamNameShort(TeamName t)
	{
		switch (t)
		{
		case CT: return "CT";
		case TERRORIST: return "T";
		case SPECTATOR: return "SPEC";
		default: return "UNK";
		}
	}

	static const char *BombStatusString()
	{
		if (TheBots && TheCSBots()->IsBombPlanted())
			return "planted";
		if (g_pGameRules)
		{
			CHalfLifeMultiplay *mp = static_cast<CHalfLifeMultiplay *>(g_pGameRules);
			if (mp->m_bBombDefused)
				return "defused";
			if (mp->m_bTargetBombed)
				return "exploded";
		}
		return "not_planted";
	}

	static const char *ZoneIndexToSiteLetter(int zoneIndex)
	{
		// Most defuse maps have A/B only; if more zones exist keep it unknown.
		if (zoneIndex == 0) return "A";
		if (zoneIndex == 1) return "B";
		return "unknown";
	}

	static CGrenade *FindPlantedC4()
	{
		CGrenade *pBomb = (CGrenade *)UTIL_FindEntityByClassname(nullptr, "grenade");
		while (pBomb)
		{
			if (pBomb->m_bIsC4)
				return pBomb;
			pBomb = (CGrenade *)UTIL_FindEntityByClassname(pBomb, "grenade");
		}
		return nullptr;
	}

	static void BuildGameStateJson(std::string &out, const char *mapName, const CSGameState *botGs)
	{
		out.clear();
		out.reserve(512);

		int aliveT = 0, aliveCT = 0;
		if (gpGlobals)
		{
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				CBasePlayer *p = UTIL_PlayerByIndex(i);
				if (!p || !p->IsPlayer() || !p->IsAlive())
					continue;
				if (p->m_iTeam == TERRORIST)
					++aliveT;
				else if (p->m_iTeam == CT)
					++aliveCT;
			}
		}

		int scoreCT = 0, scoreT = 0, roundsPlayed = 0, roundTimeLeftSec = 0;
		if (g_pGameRules && gpGlobals)
		{
			CHalfLifeMultiplay *mp = static_cast<CHalfLifeMultiplay *>(g_pGameRules);
			scoreCT = (int)mp->m_iNumCTWins;
			scoreT = (int)mp->m_iNumTerroristWins;
			roundsPlayed = mp->m_iTotalRoundsPlayed;
			roundTimeLeftSec = (int)Q_max(0.0f, mp->GetRoundRemainingTime());
		}

		out += "{";
		out += "\"map\":\"" + PromptJsonEscape(mapName) + "\"";
		out += ",\"round\":" + std::to_string(roundsPlayed + 1);
		out += ",\"rounds_played\":" + std::to_string(roundsPlayed);
		out += ",\"score_ct\":" + std::to_string(scoreCT);
		out += ",\"score_t\":" + std::to_string(scoreT);
		out += ",\"alive_ct\":" + std::to_string(aliveCT);
		out += ",\"alive_t\":" + std::to_string(aliveT);
		out += ",\"bomb_status\":\"";
		out += BombStatusString();
		out += "\"";
		out += ",\"round_time_left_sec\":" + std::to_string(roundTimeLeftSec);
		const char *site = "unknown";
		// Prefer the actual planted C4 position (global), not a bot's belief (botGs can be wrong).
		if (TheBots && TheCSBots()->IsBombPlanted())
		{
			CGrenade *pBomb = FindPlantedC4();
			if (pBomb)
			{
				const CCSBotManager::Zone *zone = TheCSBots()->GetZone(&pBomb->pev->origin);
				if (zone)
					site = ZoneIndexToSiteLetter(zone->m_index);
			}
		}
		if (!Q_stricmp(site, "unknown") && botGs && botGs->IsBombPlanted())
			site = ZoneIndexToSiteLetter(botGs->GetPlantedBombsite());
		out += ",\"site\":\"";
		out += site;
		out += "\"";
		out += "}";
	}

	static void BuildBotStateJson(std::string &out, CBasePlayer *speaker, bool alive, GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther)
	{
		out.clear();
		out.reserve(512);

		const char *name = (speaker && speaker->pev) ? STRING(speaker->pev->netname) : "unknown";
		const char *team = speaker ? TeamNameShort(speaker->m_iTeam) : "UNK";
		const int money = speaker ? speaker->m_iAccount : 0;
		const int ei = speaker ? speaker->entindex() : 0;
		// Personalities disabled for now: keep prompts clean/stable.
		const char *mood = "neutral";

		const char *activeWeapon = "unknown";
		if (speaker && speaker->m_pActiveItem)
		{
			const CBasePlayerWeapon *wpn = static_cast<CBasePlayerWeapon *>(speaker->m_pActiveItem);
			if (wpn)
			{
				if (WeaponInfoStruct *info = GetWeaponInfo(wpn->m_iId))
				{
					if (info->entityName && info->entityName[0])
						activeWeapon = info->entityName; // e.g. weapon_ak47
				}
			}
		}

		const bool isSelfDeathEvent = (event == EVENT_PLAYER_DIED
			&& speaker && pEntity && pEntity->IsPlayer()
			&& static_cast<CBasePlayer *>(pEntity) == speaker);

		const char *killedBy = "";
		if (isSelfDeathEvent && pOther && pOther->IsPlayer())
		{
			CBasePlayer *killer = static_cast<CBasePlayer *>(pOther);
			killedBy = STRING(killer->pev->netname);
		}

		const char *deathLoc = "";
		const char *killType = "";
		int killedBySameAttackerCount = 0;
		// Dead speakers: include last-death snapshot (even if current event isn't their death).
		if (!alive && ei >= 1 && ei <= MAX_CLIENTS)
		{
			if (activeWeapon && !Q_stricmp(activeWeapon, "unknown") && s_lastDeathWeapon[ei][0])
				activeWeapon = s_lastDeathWeapon[ei];
			if ((!killedBy || !*killedBy) && s_lastKilledBy[ei][0])
				killedBy = s_lastKilledBy[ei];
			if (s_lastDeathLoc[ei][0])
				deathLoc = s_lastDeathLoc[ei];
			if (s_lastDeathType[ei][0])
				killType = s_lastDeathType[ei];
			if (s_lastKilledByEnt[ei] >= 1 && s_lastKilledByEnt[ei] <= MAX_CLIENTS)
			{
				killedBySameAttackerCount = s_killedByCount[ei][s_lastKilledByEnt[ei]];
				if (killedBySameAttackerCount >= 3)
					mood = "tilted";
				else if (killedBySameAttackerCount >= 2)
					mood = "salty";
			}
		}

		int mostKilledByEnt = 0;
		int mostKilledByCount = 0;
		if (ei >= 1 && ei <= MAX_CLIENTS)
		{
			for (int k = 1; k <= MAX_CLIENTS; ++k)
			{
				if (s_killedByCount[ei][k] > mostKilledByCount)
				{
					mostKilledByCount = s_killedByCount[ei][k];
					mostKilledByEnt = k;
				}
			}
			if (alive && mostKilledByCount >= 3)
				mood = "tilted";
		}

		out += "{";
		out += "\"name\":\"" + PromptJsonEscape(name) + "\"";
		out += ",\"team\":\"";
		out += team;
		out += "\"";
		out += ",\"status\":\"";
		out += alive ? "alive" : "dead";
		out += "\"";
		out += ",\"money\":" + std::to_string(money);
		out += ",\"mood\":\"" + PromptJsonEscape(mood) + "\"";
		out += ",\"weapon\":\"" + PromptJsonEscape(activeWeapon) + "\"";
		if (alive || (!alive && ei >= 1 && ei <= MAX_CLIENTS))
		{
			out += ",\"killed_by\":\"" + PromptJsonEscape(killedBy) + "\"";
			out += ",\"death_location\":\"" + PromptJsonEscape((deathLoc && *deathLoc) ? deathLoc : "unknown") + "\"";
			out += ",\"kill_type\":\"" + PromptJsonEscape((killType && *killType) ? killType : "unknown") + "\"";
			out += ",\"killed_by_same_attacker_count\":" + std::to_string(killedBySameAttackerCount);
			out += ",\"is_repeat_death_to_same_attacker\":";
			out += (killedBySameAttackerCount >= 2) ? "true" : "false";
		}

		if (mostKilledByCount > 0)
		{
			CBasePlayer *rival = UTIL_PlayerByIndex(mostKilledByEnt);
			const char *rivalName = (rival && rival->pev) ? STRING(rival->pev->netname) : "";
			out += ",\"most_killed_by\":\"" + PromptJsonEscape((rivalName && *rivalName) ? rivalName : "unknown") + "\"";
			out += ",\"most_killed_by_count\":" + std::to_string(mostKilledByCount);
		}

		// Rich behavior state from CCSBot (real bot AI, not guessed).
		CCSBot *me = (speaker && speaker->IsBot()) ? static_cast<CCSBot *>(speaker) : nullptr;
		if (me && alive)
		{
			auto taskToStr = [](CCSBot::TaskType t) -> const char * {
				switch (t)
				{
				case CCSBot::SEEK_AND_DESTROY: return "SEEK_AND_DESTROY";
				case CCSBot::PLANT_BOMB: return "PLANT_BOMB";
				case CCSBot::FIND_TICKING_BOMB: return "FIND_TICKING_BOMB";
				case CCSBot::DEFUSE_BOMB: return "DEFUSE_BOMB";
				case CCSBot::GUARD_TICKING_BOMB: return "GUARD_TICKING_BOMB";
				case CCSBot::GUARD_BOMB_DEFUSER: return "GUARD_BOMB_DEFUSER";
				case CCSBot::GUARD_LOOSE_BOMB: return "GUARD_LOOSE_BOMB";
				case CCSBot::GUARD_BOMB_ZONE: return "GUARD_BOMB_ZONE";
				case CCSBot::ESCAPE_FROM_BOMB: return "ESCAPE_FROM_BOMB";
				case CCSBot::HOLD_POSITION: return "HOLD_POSITION";
				case CCSBot::FOLLOW: return "FOLLOW";
				case CCSBot::VIP_ESCAPE: return "VIP_ESCAPE";
				case CCSBot::GUARD_VIP_ESCAPE_ZONE: return "GUARD_VIP_ESCAPE_ZONE";
				case CCSBot::COLLECT_HOSTAGES: return "COLLECT_HOSTAGES";
				case CCSBot::RESCUE_HOSTAGES: return "RESCUE_HOSTAGES";
				case CCSBot::GUARD_HOSTAGES: return "GUARD_HOSTAGES";
				case CCSBot::GUARD_HOSTAGE_RESCUE_ZONE: return "GUARD_HOSTAGE_RESCUE_ZONE";
				case CCSBot::MOVE_TO_LAST_KNOWN_ENEMY_POSITION: return "MOVE_TO_LAST_KNOWN_ENEMY_POSITION";
				case CCSBot::MOVE_TO_SNIPER_SPOT: return "MOVE_TO_SNIPER_SPOT";
				case CCSBot::SNIPING: return "SNIPING";
				default: return "UNKNOWN_TASK";
				}
			};

			auto dispToStr = [](CCSBot::DispositionType d) -> const char * {
				switch (d)
				{
				case CCSBot::ENGAGE_AND_INVESTIGATE: return "ENGAGE_AND_INVESTIGATE";
				case CCSBot::OPPORTUNITY_FIRE: return "OPPORTUNITY_FIRE";
				case CCSBot::SELF_DEFENSE: return "SELF_DEFENSE";
				case CCSBot::IGNORE_ENEMIES: return "IGNORE_ENEMIES";
				default: return "UNKNOWN_DISPOSITION";
				}
			};

			out += ",\"bot_task\":\"";
			out += taskToStr(me->GetTask());
			out += "\"";
			out += ",\"bot_disposition\":\"";
			out += dispToStr(me->GetDisposition());
			out += "\"";

			out += ",\"is_busy\":";
			out += me->IsBusy() ? "true" : "false";
			out += ",\"is_hiding\":";
			out += me->IsHiding() ? "true" : "false";
			out += ",\"is_hunting\":";
			out += me->IsHunting() ? "true" : "false";
			out += ",\"is_attacking\":";
			out += me->IsAttacking() ? "true" : "false";
			out += ",\"is_buying\":";
			out += me->IsBuying() ? "true" : "false";
			out += ",\"is_moving_to\":";
			out += me->IsMovingTo() ? "true" : "false";

			out += ",\"is_carrying_bomb\":";
			out += me->IsCarryingBomb() ? "true" : "false";
			out += ",\"is_defusing_bomb\":";
			out += me->IsDefusingBomb() ? "true" : "false";
			out += ",\"is_escaping_bomb\":";
			out += me->IsEscapingFromBomb() ? "true" : "false";
			out += ",\"is_at_bombsite\":";
			out += me->IsAtBombsite() ? "true" : "false";

			out += ",\"is_following\":";
			out += me->IsFollowing() ? "true" : "false";
			CBasePlayer *leader = me->GetFollowLeader();
			out += ",\"follow_leader\":\"";
			out += PromptJsonEscape((leader && leader->pev) ? STRING(leader->pev->netname) : "");
			out += "\"";

			out += ",\"nearby_enemy_count\":";
			out += std::to_string(me->GetNearbyEnemyCount());
			out += ",\"nearby_friend_count\":";
			out += std::to_string(me->GetNearbyFriendCount());

			// Scenario gamestate (per-bot view).
			const CSGameState *gs = me->GetGameState();
			out += ",\"bot_gamestate_bomb_planted\":";
			out += (gs && gs->IsBombPlanted()) ? "true" : "false";
		}
		out += "}";
	}

	static void BuildOllamaSystemPrompt(std::string &out, CBasePlayer *speaker, bool alive, int maxChars, const char *personaLine)
	{
		out.clear();
		out.reserve(1100);
		out += "You generate exactly one Counter-Strike 1.6 pub chat line.\n";
		// Personalities disabled for now: keep prompts clean/stable.
		(void)speaker;
		(void)personaLine;
		out += "Output only the chat message. No quotes. No actions. No explanations. Do not output the speaker name.\n";
		out += "Voice: lowercase mostly, 1-6 words, casual bored pub slang. Alive speakers type tiny fragments. Dead speakers can type short banter.\n";
		out += "Use natural old pub shorthand, but no fixed catchwords from examples. No esports/caster tone.\n";
		out += "Hard rules: do not invent player names; do not repeat recent chat phrases; avoid catchphrase loops; no pipe character; no real-life topics; no map names unless explicitly required; talk only about the current round.\n";
		out += "Never mention or address yourself by name.\n";
		out += "Use lol rarely; never as filler or at the end of most lines.\n";
		out += "Default tone is neutral or mildly annoyed. Do not roast/noob/trash players unless the current message or repeat-death facts clearly invite it.\n";
		out += "Use normal gaming words only. Do not use awkward words like spinned, headshotted, awped, famassed, or weapon-as-a-verb.\n";
		out += "Fact rules: for death talk, say again only if BOT_STATE killed_by_same_attacker_count or KILL_FACTS victim_killed_by_killer_count is 2+. Do not mention a weapon unless KILL_FACTS/BOT_STATE proves it. Do not say sniped/headshot/knife unless kill_type or weapon supports it.\n";
		out += "Prefer ACTOR/OTHER names only when natural. Otherwise short reactions are fine.\n";
		if (!alive)
			out += "Dead chat must vary; no copyable templates, no ofc loops, no fake 'me/my' victim talk unless BOT_STATE is your own death.\n";
		if (!alive)
			out += "Do not copy RECENT_CHAT exactly.\n";
		out += "Max ";
		out += std::to_string(maxChars);
		out += " characters.\n";
	}

	static void StripWtfUnlessFriendlyFire(std::string &txt, const char *traceTag)
	{
		if (txt.empty())
			return;
		if (traceTag && !Q_stricmp(traceTag, "friendly_fire_repeat"))
			return;

		// Remove/soft-replace wtf spam in general chat.
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}

		// If it contains wtf as a token, replace with "yo" (keeps vibe, less repetitive).
		// Simple offsets: rebuild after each replace.
		for (;;)
		{
			char *pos = Q_strstr(buf, "wtf");
			if (!pos)
				break;
			const size_t off = size_t(pos - buf);
			if (off >= txt.size())
				break;

			// token-ish boundaries in original string
			const bool leftOk = (off == 0) || !std::isalnum((unsigned char)txt[off - 1]);
			const bool rightOk = (off + 3 >= txt.size()) || !std::isalnum((unsigned char)txt[off + 3]);
			if (!leftOk || !rightOk)
			{
				// skip this occurrence
				buf[off] = 'x';
				continue;
			}

			txt.replace(off, 3, "yo");
			Q_strlcpy(buf, txt.c_str(), sizeof(buf));
			for (char *p = buf; *p; ++p)
				if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
		}

		// cleanup double spaces
		for (size_t i = 1; i < txt.size(); )
		{
			if (txt[i] == ' ' && txt[i - 1] == ' ')
				txt.erase(i, 1);
			else
				++i;
		}
		while (!txt.empty() && txt.front() == ' ') txt.erase(txt.begin());
		while (!txt.empty() && txt.back() == ' ') txt.pop_back();
	}

	static void StripDustinHallucination(std::string &txt)
	{
		// Common model failure: "dustin" appears as a fake player name on dust maps.
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		if (!Q_strstr(buf, "dustin"))
			return;

		// Replace token-ish "dustin" with "this map" to preserve meaning without hardcoding a new name.
		for (;;)
		{
			char *pos = Q_strstr(buf, "dustin");
			if (!pos)
				break;
			const size_t off = size_t(pos - buf);
			const size_t len = 6;
			if (off >= txt.size())
				break;
			const bool leftOk = (off == 0) || !std::isalnum((unsigned char)txt[off - 1]);
			const bool rightOk = (off + len >= txt.size()) || !std::isalnum((unsigned char)txt[off + len]);
			if (!leftOk || !rightOk)
			{
				buf[off] = 'x';
				continue;
			}
			txt.replace(off, len, "this map");
			Q_strlcpy(buf, txt.c_str(), sizeof(buf));
			for (char *p = buf; *p; ++p)
				if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
		}

		for (size_t i = 1; i < txt.size(); )
		{
			if (txt[i] == ' ' && txt[i - 1] == ' ')
				txt.erase(i, 1);
			else
				++i;
		}
		while (!txt.empty() && txt.front() == ' ') txt.erase(txt.begin());
		while (!txt.empty() && txt.back() == ' ') txt.pop_back();
	}

	static void StripModernClipPhrases(std::string &txt)
	{
		// "clipped" is modern shooter slang; in CS 1.6 pub chat people say "dropped" / "picked".
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		if (!Q_strstr(buf, "clipped"))
			return;

		for (;;)
		{
			char *pos = Q_strstr(buf, "clipped");
			if (!pos)
				break;
			const size_t off = size_t(pos - buf);
			if (off >= txt.size())
				break;
			txt.replace(off, 7, "dropped");
			Q_strlcpy(buf, txt.c_str(), sizeof(buf));
			for (char *p = buf; *p; ++p)
				if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
		}
	}

	static void StripPipeAndAwkwardGamingWords(std::string &txt)
	{
		if (txt.empty())
			return;
		for (char &c : txt)
		{
			if (c == '|')
				c = ' ';
		}

		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
			if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');

		for (;;)
		{
			char *pos = Q_strstr(buf, "spinned");
			if (!pos)
				break;
			const size_t off = size_t(pos - buf);
			if (off >= txt.size())
				break;
			txt.replace(off, 7, "dropped");
			Q_strlcpy(buf, txt.c_str(), sizeof(buf));
			for (char *p = buf; *p; ++p)
				if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
		}

		for (size_t i = 1; i < txt.size(); )
		{
			if (txt[i] == ' ' && txt[i - 1] == ' ')
				txt.erase(i, 1);
			else
				++i;
		}
		while (!txt.empty() && txt.front() == ' ') txt.erase(txt.begin());
		while (!txt.empty() && txt.back() == ' ') txt.pop_back();
	}

	static void StripLowValueFiller(std::string &txt)
	{
		if (txt.empty())
			return;

		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
			if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');

		for (const char *bad : { "lazy af", "pub brain", "somewhere lazy", "random af" })
		{
			if (Q_strstr(buf, bad))
			{
				txt.clear();
				return;
			}
		}

		// Tail "lol" became filler in short factual event lines.
		const char *tail = " lol";
		const size_t tailLen = 4;
		if (txt.size() >= tailLen)
		{
			std::string low;
			low.reserve(txt.size());
			for (unsigned char uc : txt)
				low.push_back((char)std::tolower(uc));
			if (low.compare(low.size() - tailLen, tailLen, tail) == 0)
			{
				txt.resize(txt.size() - tailLen);
				while (!txt.empty() && txt.back() == ' ') txt.pop_back();
			}
		}
	}

	static void StripLagCity(std::string &txt)
	{
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		if (!Q_strstr(buf, "lag city"))
			return;

		// If the whole line is basically "lag city", drop it; otherwise remove just the phrase.
		for (char *p = buf; *p; ++p)
		{
			if (*p == ' ')
				continue;
			// keep as-is
		}
		if (!Q_stricmp(buf, "lag city"))
		{
			txt.clear();
			return;
		}

		for (;;)
		{
			char *pos = Q_strstr(buf, "lag city");
			if (!pos)
				break;
			const size_t off = size_t(pos - buf);
			if (off >= txt.size())
				break;
			txt.erase(off, 8);
			Q_strlcpy(buf, txt.c_str(), sizeof(buf));
			for (char *p = buf; *p; ++p)
				if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
		}

		// cleanup spaces
		for (size_t i = 1; i < txt.size(); )
		{
			if (txt[i] == ' ' && txt[i - 1] == ' ')
				txt.erase(i, 1);
			else
				++i;
		}
		while (!txt.empty() && txt.front() == ' ') txt.erase(txt.begin());
		while (!txt.empty() && txt.back() == ' ') txt.pop_back();
	}

	static bool ContainsCampWord(const std::string &s)
	{
		if (s.empty())
			return false;
		char buf[256];
		Q_strlcpy(buf, s.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
			if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
		return Q_strstr(buf, "camp") != nullptr;
	}

	static bool MentionsDeadPlayerName(const std::string &msg)
	{
		if (!gpGlobals || msg.empty())
			return false;
		char m[256];
		Q_strlcpy(m, msg.c_str(), sizeof(m));
		for (char *p = m; *p; ++p)
			if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');

		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsPlayer() || !p->pev)
				continue;
			if (p->pev->deadflag == DEAD_NO)
				continue;
			const char *nm = STRING(p->pev->netname);
			if (!nm || !nm[0])
				continue;
			char n[64];
			Q_strlcpy(n, nm, sizeof(n));
			for (char *q = n; *q; ++q)
				if (*q >= 'A' && *q <= 'Z') *q = char(*q - 'A' + 'a');
			if (Q_strstr(m, n))
				return true;
		}
		return false;
	}

	// Last line of defense if the model still names the wrong famous map.
	static void DropLineIfMapHallucination(std::string &txt, const std::string &mapLower)
	{
		if (txt.empty() || mapLower.empty())
			return;

		std::string t;
		t.reserve(txt.size());
		for (unsigned char uc : txt)
		{
			const char c = (char)uc;
			if (c >= 'A' && c <= 'Z')
				t += char(c - 'A' + 'a');
			else
				t += c;
		}

		if (mapLower == "de_dust" || mapLower == "cs_dust")
		{
			if (t.find("dust2") != std::string::npos)
			{
				txt.clear();
				return;
			}
			for (size_t i = 0; i + 1 < t.size(); ++i)
			{
				if (t[i] != 'd' || t[i + 1] != '2')
					continue;
				const bool leftOk = (i == 0) || !std::isalnum((unsigned char)t[i - 1]);
				const bool rightOk = (i + 2 >= t.size()) || !std::isalnum((unsigned char)t[i + 2]);
				if (leftOk && rightOk)
				{
					txt.clear();
					return;
				}
			}
		}
	}

	static void StripMapNameUnlessExplicit(std::string &txt, const std::string &mapLower)
	{
		if (txt.empty() || mapLower.empty())
			return;

		// If the player literally says "this map", it's fine.
		auto normalize = [](const std::string &s) -> std::string
		{
			std::string out;
			out.reserve(s.size());
			bool lastSpace = false;
			for (char c : s)
			{
				const unsigned char uc = (unsigned char)c;
				if (std::isalnum(uc))
				{
					out.push_back((char)std::tolower(uc));
					lastSpace = false;
				}
				else if (std::isspace(uc))
				{
					if (!lastSpace)
						out.push_back(' ');
					lastSpace = true;
				}
			}
			while (!out.empty() && out.front() == ' ') out.erase(out.begin());
			while (!out.empty() && out.back() == ' ') out.pop_back();
			return out;
		};

		std::string low = normalize(txt);
		if (low.find("this map") != std::string::npos)
			return;

		// Remove "de_" prefix already handled elsewhere; work with common variants.
		std::string map = mapLower;
		auto stripPrefix = [](std::string &s, const char *pfx)
		{
			const size_t n = std::strlen(pfx);
			if (s.size() > n && s.rfind(pfx, 0) == 0)
				s.erase(0, n);
		};
		stripPrefix(map, "de_");
		stripPrefix(map, "cs_");
		stripPrefix(map, "as_");
		stripPrefix(map, "fy_");

		if (map.size() < 3)
			return;

		// If the outgoing message mentions the map token, strip it (keeps strategy clean).
		const std::string token = map;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
			if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');

		if (!Q_strstr(buf, token.c_str()))
			return;

		// crude erase loop on original string using lowercase mirror
		for (;;)
		{
			std::string mirror = normalize(txt);
			size_t pos = mirror.find(token);
			if (pos == std::string::npos)
				break;
			// best-effort: remove token from the original by searching case-insensitively.
			auto toLower = [](unsigned char c) { return (unsigned char)std::tolower(c); };
			auto it = std::search(txt.begin(), txt.end(), token.begin(), token.end(),
				[&](char a, char b) { return toLower((unsigned char)a) == toLower((unsigned char)b); });
			if (it == txt.end())
				break;
			txt.erase(it, it + token.size());
		}

		// cleanup spaces
		for (size_t i = 1; i < txt.size(); )
		{
			if (txt[i] == ' ' && txt[i - 1] == ' ')
				txt.erase(i, 1);
			else
				++i;
		}
		while (!txt.empty() && txt.front() == ' ') txt.erase(txt.begin());
		while (!txt.empty() && txt.back() == ' ') txt.pop_back();
	}

	// Drop lines containing known LLM garbage tokens (case-insensitive).
	static void RejectKnownGibberishTokens(std::string &txt)
	{
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		if (Q_strstr(buf, "bappy"))
			txt.clear();
	}

	static bool TokenHasVowel(const std::string &t)
	{
		for (unsigned char uc : t)
		{
			const char c = (char)std::tolower(uc);
			if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u')
				return true;
		}
		return false;
	}

	// Drop lines containing standalone 4–6 letter tokens with no vowels (common LLM keyboard-mash like "fvfr").
	static void DropLineIfNoVowelGarbageToken(std::string &txt)
	{
		if (txt.empty())
			return;

		std::string cur;
		cur.reserve(8);
		auto flush = [&]()
		{
			if (cur.size() >= 4 && cur.size() <= 6 && !TokenHasVowel(cur))
			{
				txt.clear();
				return true;
			}
			cur.clear();
			return false;
		};

		for (size_t i = 0; i <= txt.size(); ++i)
		{
			const bool end = (i == txt.size());
			const unsigned char uc = end ? 0 : (unsigned char)txt[i];
			if (!end && std::isalpha(uc))
			{
				cur.push_back((char)uc);
				if (cur.size() > 6)
					cur.clear();
			}
			else
			{
				if (flush())
					return;
			}
		}
	}

	// Drop lines that still read like polished announcer / esports chat.
	static void StripPolishedClichés(std::string &txt)
	{
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		for (const char *b : {
			"nice frag", "good frag", "great frag", "you got fragged", "well played",
			"good job", "nice job", "let's go", "lets go", "good luck have fun", "gl hf"
		})
		{
			if (Q_strstr(buf, b))
			{
				txt.clear();
				return;
			}
		}
	}

	// Drop lines that reference mechanics from other eras/games (draft pick, ranked, etc.).
	static void StripModernNonCS16Jargon(std::string &txt)
	{
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		for (const char *s : {
			"picked last", "got picked last", "last pick", "first pick", "pick ban", "pick/ban",
			" in draft", "the draft", "role queue", " ranked ", " elo ", " mmr ", "matchmaking",
			"jungler", " top lane", " adc ", "valorant", "overwatch", "league of", "dota ",
			"team captain", " toxic queue", "smurf queue"
		})
		{
			if (Q_strstr(buf, s))
			{
				txt.clear();
				return;
			}
		}
	}

	// Model sometimes echoes prompt scaffolding — never send that in-world.
	static void StripPromptLeaksFromOutput(std::string &txt)
	{
		if (txt.empty())
			return;
		std::string low;
		low.resize(txt.size());
		for (size_t i = 0; i < txt.size(); ++i)
			low[i] = (char)std::tolower((unsigned char)txt[i]);
		static const char *leaks[] = {
			" ( context", " (context", "(context", " context:", " event=", " event_detail", " round_snap", " speaker=", " fact_rule",
			" bomb_role=", " kill_roles=", " game=", " mapsignal", " map=", " char=", " character=",
			"(note:", "(note ", " (note", " note:", " this line", " reflects ", " reflects the", " as an ai",
			" language model", " i cannot", "i can't ", "i cant ", "task=", "game_state", "output_rules",
			"character_voice", " user:", " assistant:", "system:", "prompt:"
		};
		for (const char *leak : leaks)
		{
			const size_t p = low.find(leak);
			if (p != std::string::npos)
			{
				txt.resize(p);
				while (!txt.empty() && std::isspace((unsigned char)txt.back()))
					txt.pop_back();
				return;
			}
		}
	}

	// Drop lines that contradict live bomb state (defused/exploded, or T asking for defuse when C4 is not live).
	static void StripBombStateInconsistencies(std::string &txt, TeamName team)
	{
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}

		const bool planted = TheBots && TheCSBots()->IsBombPlanted();
		bool defused = false;
		bool exploded = false;
		if (g_pGameRules)
		{
			CHalfLifeMultiplay *mp = static_cast<CHalfLifeMultiplay *>(g_pGameRules);
			defused = mp->m_bBombDefused;
			exploded = mp->m_bTargetBombed;
		}

		if (team == TERRORIST && !planted)
		{
			for (const char *s : {
				"defus", "defuse", "disarm", " retake ", " retake.", "retake the", "defuser"
			})
			{
				if (Q_strstr(buf, s))
				{
					txt.clear();
					return;
				}
			}
		}

		if (defused || exploded)
		{
			for (const char *s : {
				"bomb is down", "bomb down", "bombs down", "need help defus", "needa defus", "needs defus",
				"go defus", " go plant", "gotta plant", "need to plant", "must plant", "still plant",
				"bomb needs", "needs defusing", "need defusing", "hurry defus", "defuse fast"
			})
			{
				if (Q_strstr(buf, s))
				{
					txt.clear();
					return;
				}
			}
		}
	}

	// CTs cannot plant — drop first-person plant lines if the model ignores the prompt.
	static void StripCtOwnPlantClaims(std::string &txt, TeamName team)
	{
		if (txt.empty() || team != CT)
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		for (const char *s : {
			"need to plant", "need 2 plant", "i need to plant", "we need to plant",
			"gonna plant", "going to plant", "ill plant", "i'll plant", "im planting", "i'm planting",
			" i plant", "i plant ", "plant the bomb", "plant bomb", "to plant in", "to plant at",
			"plant in b", "plant in a", "plant at b", "plant at a", " gotta plant", " must plant"
		})
		{
			if (Q_strstr(buf, s))
			{
				txt.clear();
				return;
			}
		}
	}

	static std::string LowerAsciiCopy(const std::string &s)
	{
		std::string out;
		out.reserve(s.size());
		for (unsigned char uc : s)
			out.push_back((char)std::tolower(uc));
		return out;
	}

	static void DropUnsupportedWeaponClaims(std::string &txt, const std::string &factContextLower)
	{
		if (txt.empty())
			return;
		const std::string low = LowerAsciiCopy(txt);
		const std::string paddedText = " " + low + " ";
		const std::string paddedFact = " " + factContextLower + " ";
		auto hasText = [&](const char *s) { return low.find(s) != std::string::npos; };
		auto hasTextToken = [&](const char *s) { return paddedText.find(std::string(" ") + s + " ") != std::string::npos; };
		auto hasFact = [&](const char *s) { return factContextLower.find(s) != std::string::npos; };
		auto hasFactToken = [&](const char *s) { return paddedFact.find(std::string(" ") + s + " ") != std::string::npos; };

		for (const char *badVerb : {
			"headshotted", "famassed", "awped", "ak'd", "akd", "m4'd", "m4d",
			"aug'd", "augd", "sg'd", "sgd", "usp'd", "uspd", "glocked", "gl0cked"
		})
		{
			if (hasText(badVerb))
			{
				txt.clear();
				return;
			}
		}

		const bool awpProved = hasFact("weapon_awp") || hasFact(" weapon=awp");
		if ((hasText("awp") || hasText("awped")) && !awpProved)
		{
			txt.clear();
			return;
		}

		const bool scoutProved = hasFact("weapon_scout") || hasFact(" weapon=scout");
		if ((hasText("scout") || hasText("scouted")) && !scoutProved)
		{
			txt.clear();
			return;
		}

		const bool deagleProved = hasFact("weapon_deagle");
		if ((hasText("deag") || hasText("deagle")) && !deagleProved)
		{
			txt.clear();
			return;
		}

		struct WeaponMentionRule
		{
			const char *say;
			const char *entity;
			const char *alias;
			bool substringOk;
		};
		const WeaponMentionRule weaponRules[] = {
			{ "m4", "weapon_m4a1", "m4", true },
			{ "m4a1", "weapon_m4a1", "m4a1", true },
			{ "ak", "weapon_ak47", "ak", false },
			{ "ak47", "weapon_ak47", "ak47", true },
			{ "galil", "weapon_galil", "galil", true },
			{ "famas", "weapon_famas", "famas", true },
			{ "aug", "weapon_aug", "aug", true },
			{ "sg552", "weapon_sg552", "sg552", true },
			{ "sg", "weapon_sg552", "sg", false },
			{ "usp", "weapon_usp", "usp", true },
			{ "glock", "weapon_glock18", "glock", true },
			{ "glock18", "weapon_glock18", "glock18", true },
			{ "mp5", "weapon_mp5navy", "mp5", true },
			{ "p90", "weapon_p90", "p90", true },
			{ "xm1014", "weapon_xm1014", "xm1014", true },
			{ "m3", "weapon_m3", "m3", false },
			{ "tmp", "weapon_tmp", "tmp", true },
			{ "mac10", "weapon_mac10", "mac10", true },
			{ "elite", "weapon_elite", "elite", true },
			{ "five seven", "weapon_fiveseven", "five seven", true },
			{ "fiveseven", "weapon_fiveseven", "fiveseven", true },
			{ "knife", "weapon_knife", "knife", true }
		};
		for (const WeaponMentionRule &rule : weaponRules)
		{
			const bool mentioned = rule.substringOk ? hasText(rule.say) : (Q_strstr(rule.say, " ") ? hasText(rule.say) : hasTextToken(rule.say));
			if (!mentioned)
				continue;
			const bool proved = hasFact(rule.entity) || hasFactToken(rule.alias);
			if (!proved)
			{
				txt.clear();
				return;
			}
		}

		const bool sniperProved = awpProved || scoutProved;
		if ((hasText("sniped") || hasText("sniper") || hasText("no scope") || hasText("noscope")) && !sniperProved)
			txt.clear();
	}

	static bool LineMentionsKnownWeapon(const std::string &txtLower)
	{
		const std::string padded = " " + txtLower + " ";
		for (const char *s : {
			"m4", "m4a1", "ak47", "galil", "famas", "aug", "sg552", "usp", "glock",
			"glock18", "mp5", "p90", "xm1014", "tmp", "mac10", "elite", "fiveseven",
			"five seven", "awp", "scout", "deagle"
		})
		{
			if (txtLower.find(s) != std::string::npos)
				return true;
		}
		return padded.find(" ak ") != std::string::npos
			|| padded.find(" sg ") != std::string::npos
			|| padded.find(" m3 ") != std::string::npos;
	}

	static int ExtractCountAfterKey(const std::string &s, const char *key)
	{
		const size_t p = s.find(key);
		if (p == std::string::npos)
			return 0;
		size_t i = p + Q_strlen(key);
		while (i < s.size() && (s[i] == ':' || s[i] == '=' || s[i] == ' ' || s[i] == '"'))
			++i;
		int n = 0;
		while (i < s.size() && std::isdigit((unsigned char)s[i]))
		{
			n = n * 10 + (s[i] - '0');
			++i;
		}
		return n;
	}

	static bool RepeatFactAllowsAgain(const std::string &factContextLower)
	{
		return ExtractCountAfterKey(factContextLower, "victim_killed_by_killer_count") >= 2
			|| ExtractCountAfterKey(factContextLower, "killed_by_same_attacker_count") >= 2
			|| ExtractCountAfterKey(factContextLower, "most_killed_by_count") >= 2;
	}

	static void StripUnsupportedRepeatClaims(std::string &txt, const std::string &factContextLower, const char *traceTag)
	{
		if (txt.empty())
			return;
		if (traceTag && !Q_stricmp(traceTag, "friendly_fire_repeat"))
			return;
		if (RepeatFactAllowsAgain(factContextLower))
			return;

		const std::string padded = " " + LowerAsciiCopy(txt) + " ";
		if (padded.find(" same ") != std::string::npos)
		{
			txt.clear();
			return;
		}

		if (padded.find(" again ") == std::string::npos)
			return;

		std::string out;
		out.reserve(txt.size());
		size_t i = 0;
		while (i < txt.size())
		{
			if ((i == 0 || !std::isalnum((unsigned char)txt[i - 1]))
				&& i + 5 <= txt.size()
				&& std::tolower((unsigned char)txt[i]) == 'a'
				&& std::tolower((unsigned char)txt[i + 1]) == 'g'
				&& std::tolower((unsigned char)txt[i + 2]) == 'a'
				&& std::tolower((unsigned char)txt[i + 3]) == 'i'
				&& std::tolower((unsigned char)txt[i + 4]) == 'n'
				&& (i + 5 == txt.size() || !std::isalnum((unsigned char)txt[i + 5])))
			{
				i += 5;
				continue;
			}
			out.push_back(txt[i++]);
		}
		txt.swap(out);
		for (size_t j = 1; j < txt.size(); )
		{
			if (txt[j] == ' ' && txt[j - 1] == ' ')
				txt.erase(j, 1);
			else
				++j;
		}
		while (!txt.empty() && txt.front() == ' ') txt.erase(txt.begin());
		while (!txt.empty() && txt.back() == ' ') txt.pop_back();
	}

	static void DropInvalidSpectatorSelfClaims(std::string &txt, const char *traceTag)
	{
		if (txt.empty() || !traceTag)
			return;
		if (Q_stricmp(traceTag, "kill_dead_spectator") && Q_stricmp(traceTag, "kill_teammate_react"))
			return;

		const std::string padded = " " + LowerAsciiCopy(txt) + " ";
		for (const char *s : {
			" me ", " my ", " mine ", " i died", " i got", " got me", " killed me",
			" headshot me", " baited me", " dropped me"
		})
		{
			if (padded.find(s) != std::string::npos)
			{
				txt.clear();
				return;
			}
		}
	}

	// Alive bots must not leak self position/tactics in chat; drop obvious sitrep if model ignores prompt.
	static void StripAliveSelfSitrep(std::string &txt)
	{
		if (txt.empty())
			return;
		char buf[256];
		Q_strlcpy(buf, txt.c_str(), sizeof(buf));
		for (char *p = buf; *p; ++p)
		{
			if (*p >= 'A' && *p <= 'Z')
				*p = char(*p - 'A' + 'a');
		}
		for (const char *s : {
			"stuck in ", "getting mid", "im getting ", "i'm getting ", "i am getting ",
			"with no flash", "no flashbang", "no nade",
			"cant get shot", "can't get shot"
		})
		{
			if (Q_strstr(buf, s))
			{
				txt.clear();
				return;
			}
		}
	}

	struct OllamaRequest
	{
		int speakerEntIndex = 0;
		float createdAt = 0.0f;
		uint64_t wallSentMs = 0;
		// Seconds after createdAt before "typing" may complete (reaction / thinking).
		float reactionDelay = 0.0f;
		bool speakerAlive = false;
		int numPredictChars = 120;
		GameEventType event = EVENT_INVALID;
		// Short tag for logs: which emit path queued this (e.g. kill_victim, bomb_planted).
		char traceTag[32]{};
		// Lowercase STRING(gpGlobals->mapname), e.g. de_dust — same BSP the server is running.
		std::string mapInternalLower;
		// Lowercase current factual prompt context only. RECENT_CHAT is intentionally excluded.
		std::string factContextLower;
		std::string allowedMentionNamesLower;
		bool needsReplyValidation = false;
		std::string replySourceMessage;
		std::string systemPrompt;
		std::string userPrompt;
	};

	struct OllamaResponse
	{
		int speakerEntIndex = 0;
		float createdAt = 0.0f;
		uint64_t wallSentMs = 0;
		uint64_t wallResponseMs = 0;
		float deliverAfter = 0.0f;
		// Worker must not read gpGlobals; Frame sets deliverAfter from these fields.
		bool scheduleDeliverOnMain = false;
		float deliverReactionEnd = 0.0f; // game time: createdAt + reactionDelay
		float reactionDelay = 0.0f;
		float deliverTypingSec = 0.0f;
		bool speakerAlive = false;
		GameEventType event = EVENT_INVALID;
		char traceTag[32]{};
		std::string mapInternalLower;
		std::string factContextLower;
		std::string allowedMentionNamesLower;
		bool replyValidationFailed = false;
		std::string text;
	};

	std::mutex g_reqMutex;
	std::condition_variable g_reqCv;
	std::queue<OllamaRequest> g_reqQ;

	std::mutex g_resMutex;
	std::queue<OllamaResponse> g_resQ;

	std::mutex g_dbgMutex;
	std::queue<std::string> g_dbgQ;

	void DebugLog(const char *fmt, ...)
	{
		if (sv_ai_debug.value <= 0.0f)
			return;

		char buf[2048];
		va_list ap;
		va_start(ap, fmt);
		Q_vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);

		std::lock_guard<std::mutex> lk(g_dbgMutex);
		if (g_dbgQ.size() > 64)
			g_dbgQ.pop();
		g_dbgQ.push(std::string(buf));
	}

	std::atomic<bool> g_workerRunning{ false };
	std::thread g_worker;

	static bool ParseHttpUrl(const char *url, std::string &host, std::string &port, std::string &basePath)
	{
		host.clear();
		port = "80";
		basePath.clear();

		if (!url || !*url)
			return false;

		const char *p = url;
		if (!Q_strnicmp(p, "http://", 7))
			p += 7;
		else
			return false; // http only

		const char *slash = Q_strchr(p, '/');
		std::string hostport = slash ? std::string(p, slash - p) : std::string(p);
		basePath = slash ? std::string(slash) : std::string();

		if (hostport.empty())
			return false;

		const size_t colon = hostport.find(':');
		if (colon != std::string::npos)
		{
			host = hostport.substr(0, colon);
			port = hostport.substr(colon + 1);
		}
		else
		{
			host = hostport;
		}

		if (host.empty() || port.empty())
			return false;

		return true;
	}

	static std::string JsonEscape(const std::string &in)
	{
		std::string out;
		out.reserve(in.size() + 16);
		for (char c : in)
		{
			switch (c)
			{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if ((unsigned char)c < 0x20)
					out += ' ';
				else
					out += c;
			}
		}
		return out;
	}

	static bool SetSockTimeoutMs(int fd, int timeoutMs)
	{
		if (timeoutMs <= 0)
			timeoutMs = 1;

		struct timeval tv;
		tv.tv_sec = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;
		if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
			return false;
		if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
			return false;
		return true;
	}

	static bool HttpPost(const std::string &host, const std::string &port, const std::string &path, const std::string &body, int timeoutMs, std::string &outResponse)
	{
		outResponse.clear();

		struct addrinfo hints;
		Q_memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		struct addrinfo *res = nullptr;
		const int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
		if (gai != 0 || !res)
		{
			DebugLog("[ai] getaddrinfo failed for %s:%s (%d)", host.c_str(), port.c_str(), gai);
			return false;
		}

		int fd = -1;
		for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
		{
			fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (fd < 0)
				continue;

			SetSockTimeoutMs(fd, timeoutMs);

			if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
				break;

			close(fd);
			fd = -1;
		}

		freeaddrinfo(res);

		if (fd < 0)
		{
			DebugLog("[ai] connect failed to %s:%s", host.c_str(), port.c_str());
			return false;
		}

		std::string req;
		req.reserve(256 + body.size());
		req += "POST ";
		req += path.empty() ? "/api/generate" : (path + "/api/generate");
		req += " HTTP/1.1\r\n";
		req += "Host: " + host + "\r\n";
		req += "Content-Type: application/json\r\n";
		req += "Connection: close\r\n";
		req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		req += "\r\n";
		req += body;

		const char *data = req.c_str();
		size_t left = req.size();
		while (left)
		{
			const ssize_t n = send(fd, data, left, 0);
			if (n <= 0)
			{
				DebugLog("[ai] send failed (%d)", errno);
				close(fd);
				return false;
			}
			data += n;
			left -= size_t(n);
		}

		char buf[4096];
		while (true)
		{
			const ssize_t n = recv(fd, buf, sizeof(buf), 0);
			if (n <= 0)
				break;
			outResponse.append(buf, buf + n);
			// safety cap
			if (outResponse.size() > 1024 * 1024)
				break;
		}

		close(fd);
		return !outResponse.empty();
	}

	static bool ExtractJsonStringField(const std::string &json, const char *field, std::string &out)
	{
		out.clear();
		const std::string key = std::string("\"") + field + "\"";
		size_t pos = json.find(key);
		if (pos == std::string::npos)
			return false;
		pos = json.find(':', pos + key.size());
		if (pos == std::string::npos)
			return false;
		pos = json.find('"', pos);
		if (pos == std::string::npos)
			return false;
		pos++;
		std::string val;
		while (pos < json.size())
		{
			char c = json[pos++];
			if (c == '"')
				break;
			if (c == '\\' && pos < json.size())
			{
				char e = json[pos++];
				switch (e)
				{
				case 'n': val.push_back('\n'); break;
				case 'r': val.push_back('\r'); break;
				case 't': val.push_back('\t'); break;
				case '\\': val.push_back('\\'); break;
				case '"': val.push_back('"'); break;
				default: val.push_back(e); break;
				}
			}
			else
			{
				val.push_back(c);
			}
			if (val.size() > 4096)
				break;
		}
		out.swap(val);
		return !out.empty();
	}

	static bool ValidateReplyCandidate(
		const std::string &host,
		const std::string &port,
		const std::string &basePath,
		int timeoutMs,
		const OllamaRequest &req,
		const std::string &candidate)
	{
		if (!req.needsReplyValidation)
			return true;
		if (candidate.empty() || req.replySourceMessage.empty())
			return false;

		std::string ctx = req.userPrompt;
		if ((int)ctx.size() > 1200)
			ctx.resize(1200);

		std::string prompt;
		prompt.reserve(1800);
		prompt += "PREVIOUS_MESSAGE=\"";
		prompt += PromptJsonEscape(req.replySourceMessage.c_str());
		prompt += "\"\nCANDIDATE_REPLY=\"";
		prompt += PromptJsonEscape(candidate.c_str());
		prompt += "\"\nCONTEXT=\"";
		prompt += PromptJsonEscape(ctx.c_str());
		prompt += "\"\n";
		prompt += "Judge if CANDIDATE_REPLY is a natural direct reply to PREVIOUS_MESSAGE in Counter-Strike dead chat.\n";
		prompt += "Output OK only if it clearly responds to the previous message and does not copy garbage/slop.\n";
		prompt += "Output BAD if it changes topic, repeats afk/brb/lol spam, invents weapons/facts, answers a different player, or makes no sense.\n";
		prompt += "Output BAD if it adds an insult/roast (noob, bot, trash, bad, idiot, stfu, uninstall) when PREVIOUS_MESSAGE was not already hostile.\n";
		prompt += "Output exactly one token: OK or BAD.";

		const std::string body =
			std::string("{\"model\":\"") + JsonEscape(sv_ai_ollama_model.string) +
			"\",\"system\":\"You are a strict validator for CS 1.6 bot chat replies. Output exactly OK or BAD.\""
			",\"prompt\":\"" + JsonEscape(prompt) +
			"\",\"stream\":false,\"options\":{\"num_predict\":3,\"temperature\":0,\"top_p\":0.40,\"repeat_penalty\":1.10,\"stop\":[\"\\n\"]}}";

		std::string http;
		if (!HttpPost(host, port, basePath, body, timeoutMs, http))
		{
			DebugLog("[ai] reply judge trace=%s verdict=BAD", req.traceTag[0] ? req.traceTag : "?");
			return false;
		}

		size_t hdrEnd = http.find("\r\n\r\n");
		std::string payload = (hdrEnd == std::string::npos) ? http : http.substr(hdrEnd + 4);

		std::string verdict;
		if (!ExtractJsonStringField(payload, "response", verdict))
		{
			DebugLog("[ai] reply judge trace=%s verdict=BAD", req.traceTag[0] ? req.traceTag : "?");
			return false;
		}

		for (char &c : verdict)
		{
			if (c == '\n' || c == '\r' || c == '\t')
				c = ' ';
			if (c >= 'a' && c <= 'z')
				c = char(c - 'a' + 'A');
		}
		while (!verdict.empty() && std::isspace((unsigned char)verdict.front()))
			verdict.erase(verdict.begin());
		while (!verdict.empty() && std::isspace((unsigned char)verdict.back()))
			verdict.pop_back();

		const bool ok = (verdict.find("OK") == 0);
		DebugLog("[ai] reply judge trace=%s verdict=%s", req.traceTag[0] ? req.traceTag : "?", ok ? "OK" : "BAD");
		return ok;
	}

	static void WorkerMain()
	{
		while (g_workerRunning.load())
		{
			OllamaRequest req;
			{
				std::unique_lock<std::mutex> lk(g_reqMutex);
				g_reqCv.wait(lk, [] { return !g_workerRunning.load() || !g_reqQ.empty(); });
				if (!g_workerRunning.load())
					break;
				if (g_reqQ.empty())
					continue;
				req = std::move(g_reqQ.front());
				g_reqQ.pop();
			}

			std::string host, port, basePath;
			if (!ParseHttpUrl(sv_ai_ollama_url.string, host, port, basePath))
			{
				DebugLog("[ai] invalid sv_ai_ollama_url: %s", sv_ai_ollama_url.string);
				if (req.speakerAlive && req.speakerEntIndex >= 1 && req.speakerEntIndex <= MAX_CLIENTS)
					ChatFreezeClear(req.speakerEntIndex);
				continue;
			}

			const int timeoutMs = int(sv_ai_timeout_ms.value);
			const int maxChars = Q_max(20, Q_min(req.numPredictChars, 240));
			const int predictTokens = Q_max(12, Q_min(maxChars, req.speakerAlive ? 18 : 24));

			std::string sys = req.systemPrompt;
			std::string usr = req.userPrompt;
			if (sys.empty())
				sys = "You output one short CS 1.6 pub chat line. Raw text only, no quotes.";
			if (usr.empty())
				usr = "Say one short Counter-Strike 1.6 public server chat line.";
			if ((int)sys.size() > 2200)
				sys.resize(2200);
			if ((int)usr.size() > 2200)
				usr.resize(2200);

			const std::string body =
				std::string("{\"model\":\"") + JsonEscape(sv_ai_ollama_model.string) +
				"\",\"system\":\"" + JsonEscape(sys) +
				"\",\"prompt\":\"" + JsonEscape(usr) +
				"\",\"stream\":false,\"options\":{\"num_predict\":" + std::to_string(predictTokens) +
				",\"temperature\":0.70,\"top_p\":0.85,\"repeat_penalty\":1.15,\"stop\":[\"\\n\"]}}";

			std::string http;
			if (!HttpPost(host, port, basePath, body, timeoutMs, http))
			{
				DebugLog("[ai] HTTP POST failed (timeout=%dms)", timeoutMs);
				if (req.speakerAlive && req.speakerEntIndex >= 1 && req.speakerEntIndex <= MAX_CLIENTS)
					ChatFreezeClear(req.speakerEntIndex);
				continue;
			}

			// Strip headers
			size_t hdrEnd = http.find("\r\n\r\n");
			std::string payload = (hdrEnd == std::string::npos) ? http : http.substr(hdrEnd + 4);

			std::string responseText;
			if (!ExtractJsonStringField(payload, "response", responseText))
			{
				DebugLog("[ai] could not parse Ollama JSON response");
				if (req.speakerAlive && req.speakerEntIndex >= 1 && req.speakerEntIndex <= MAX_CLIENTS)
					ChatFreezeClear(req.speakerEntIndex);
				continue;
			}

			// Normalize whitespace and cap.
			for (char &c : responseText)
			{
				if (c == '\n' || c == '\r' || c == '\t')
					c = ' ';
			}
			while (!responseText.empty() && responseText.back() == ' ')
				responseText.pop_back();
			while (!responseText.empty() && responseText.front() == ' ')
				responseText.erase(responseText.begin());

			if ((int)responseText.size() > maxChars)
				responseText.resize(maxChars);

			StripPromptLeaksFromOutput(responseText);

			if (req.needsReplyValidation && !ValidateReplyCandidate(host, port, basePath, timeoutMs, req, responseText))
			{
				DebugLog("[ai] dropping reply candidate trace=%s ent=%d", req.traceTag[0] ? req.traceTag : "?", req.speakerEntIndex);
				continue;
			}

			OllamaResponse out;
			out.speakerEntIndex = req.speakerEntIndex;
			out.createdAt = req.createdAt;
			out.event = req.event;
			out.speakerAlive = req.speakerAlive;
			out.wallSentMs = req.wallSentMs;
			out.wallResponseMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			out.mapInternalLower = req.mapInternalLower;
			out.factContextLower = req.factContextLower;
			out.allowedMentionNamesLower = req.allowedMentionNamesLower;
			out.text = responseText;
			Q_strlcpy(out.traceTag, req.traceTag, sizeof(out.traceTag));

			// Simulated typing duration only here — never use gpGlobals on this thread.
			float typing = 0.0f;
			if (req.speakerAlive && !responseText.empty())
			{
				const float perChar = RANDOM_FLOAT(AiTiming::kTypeCharMin, AiTiming::kTypeCharMax);
				typing = perChar * float(responseText.size());
				if (typing > AiTiming::kTypeCapSec)
					typing = AiTiming::kTypeCapSec;
			}
			out.deliverReactionEnd = req.createdAt + req.reactionDelay;
			out.reactionDelay = req.reactionDelay;
			out.deliverTypingSec = typing;
			out.scheduleDeliverOnMain = true;
			out.deliverAfter = 0.0f;

			{
				std::lock_guard<std::mutex> lk(g_resMutex);
				g_resQ.push(std::move(out));
			}

			// Log before any further moves/copies confuse sizes.
			DebugLog("[ai] response ready trace=%s event=%s ent=%d (%zu chars): %.80s",
				req.traceTag[0] ? req.traceTag : "?",
				EventName(req.event),
				req.speakerEntIndex,
				responseText.size(),
				responseText.c_str());
		}
	}

	static void EnsureWorker()
	{
		if (g_workerRunning.load())
			return;
		g_workerRunning.store(true);
		g_worker = std::thread(WorkerMain);
	}

	static void StopWorker()
	{
		if (!g_workerRunning.load())
			return;
		g_workerRunning.store(false);
		g_reqCv.notify_all();
		if (g_worker.joinable())
			g_worker.join();
		ChatFreezeClearAll();
	}

	CBasePlayer *PickRandomBot()
	{
		int count = 0;
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (p && p->IsBot())
				count++;
		}
		if (count <= 0)
			return nullptr;

		int which = RANDOM_LONG(0, count - 1);
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (p && p->IsBot() && which-- == 0)
				return p;
		}
		return nullptr;
	}

	// Prefer a dead bot for ambient lines (spectators chat more).
	static CBasePlayer *PickRandomBotPreferDead()
	{
		int deadIdx[MAX_CLIENTS];
		int anyIdx[MAX_CLIENTS];
		int deadCount = 0;
		int anyCount = 0;

		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsBot())
				continue;
			if (anyCount < MAX_CLIENTS)
				anyIdx[anyCount++] = i;
			if (p->pev->deadflag != DEAD_NO && deadCount < MAX_CLIENTS)
				deadIdx[deadCount++] = i;
		}

		if (anyCount <= 0)
			return nullptr;

		if (deadCount > 0 && RANDOM_FLOAT(0.0f, 1.0f) < 0.86f)
		{
			const int pick = deadIdx[RANDOM_LONG(0, deadCount - 1)];
			return UTIL_PlayerByIndex(pick);
		}

		return UTIL_PlayerByIndex(anyIdx[RANDOM_LONG(0, anyCount - 1)]);
	}

	static CBasePlayer *PickRandomBotPreferDeadExcluding(CBasePlayer *exclude)
	{
		int deadIdx[MAX_CLIENTS];
		int anyIdx[MAX_CLIENTS];
		int deadCount = 0;
		int anyCount = 0;

		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsBot() || p == exclude)
				continue;
			if (anyCount < MAX_CLIENTS)
				anyIdx[anyCount++] = i;
			if (p->pev->deadflag != DEAD_NO && deadCount < MAX_CLIENTS)
				deadIdx[deadCount++] = i;
		}

		if (anyCount <= 0)
			return nullptr;

		if (deadCount > 0 && RANDOM_FLOAT(0.0f, 1.0f) < 0.86f)
			return UTIL_PlayerByIndex(deadIdx[RANDOM_LONG(0, deadCount - 1)]);

		return UTIL_PlayerByIndex(anyIdx[RANDOM_LONG(0, anyCount - 1)]);
	}

	// Random dead bot for spectator banter (optionally exclude victim/killer).
	static CBasePlayer *PickRandomDeadBotExcluding(CBasePlayer *a, CBasePlayer *b)
	{
		int idx[MAX_CLIENTS];
		int n = 0;
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsBot() || p->pev->deadflag == DEAD_NO)
				continue;
			if (a && p == a)
				continue;
			if (b && p == b)
				continue;
			if (n < MAX_CLIENTS)
				idx[n++] = i;
		}
		if (n <= 0)
			return nullptr;
		return UTIL_PlayerByIndex(idx[RANDOM_LONG(0, n - 1)]);
	}

	static void MaybeQueueDeadChatReplies(CBasePlayer *sender, const char *msg, bool senderDead);

	void SayAsPlayer(CBasePlayer *speaker, const char *msg, BOOL teamonly, GameEventType gameEvent, const char *traceTag)
	{
		if (!speaker || !speaker->IsBot() || !msg || !msg[0])
			return;

		if (gmsgSayText == 0)
		{
			DebugLog("[ai] SayAsPlayer: gmsgSayText=0 (messages not linked yet?)");
			return;
		}

		const bool bSenderDead = (speaker->pev->deadflag != DEAD_NO);

		// Match Host_Say tokens: dead team-say uses CT/T dead loc strings; dead global uses AllDead.
		const char *pszFormat = "#Cstrike_Chat_All";
		if (bSenderDead)
		{
			if (teamonly && (speaker->m_iTeam == CT || speaker->m_iTeam == TERRORIST))
				pszFormat = (speaker->m_iTeam == CT) ? "#Cstrike_Chat_CT_Dead" : "#Cstrike_Chat_T_Dead";
			else
				pszFormat = "#Cstrike_Chat_AllDead";
		}
		else if (!teamonly)
			pszFormat = "#Cstrike_Chat_All";

		char text[128];
		if (teamonly && sv_ai_chat_team_tag.value > 0.0f)
			Q_strlcpy(text, "(TEAM) ", sizeof(text));
		else
			text[0] = '\0';
		Q_strlcat(text, msg, sizeof(text));
		Q_strlcat(text, "\n", sizeof(text));

		int considered = 0;
		int sent = 0;

		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pReceiver = UTIL_PlayerByIndex(i);
			if (!pReceiver || !pReceiver->IsNetClient() || pReceiver->IsDormant())
				continue;

			considered++;

			// mute / voice block
			if (gpGlobals->deathmatch != 0.0f && CSGameRules()->m_VoiceGameMgr.PlayerHasBlockedPlayer(pReceiver, speaker))
				continue;

			// team chat filter
			if (teamonly
#ifdef REGAMEDLL_FIXES
				&& CSGameRules()->PlayerRelationship(speaker, pReceiver) != GR_TEAMMATE
#else
				&& pReceiver->m_iTeam != speaker->m_iTeam
#endif
				)
				continue;

			// Intentionally no Host_Say dead/alive skip: AI lines must reach living clients too (pub feel).

			MESSAGE_BEGIN(MSG_ONE, gmsgSayText, nullptr, pReceiver->pev);
				WRITE_BYTE(speaker->entindex());
				WRITE_STRING(pszFormat);
				WRITE_STRING("");
				WRITE_STRING(text);
			MESSAGE_END();

			sent++;
		}

		if (sent > 0)
		{
			RecordRecentAiChatLine(msg);
			// AI-generated dead chat should also be able to produce back-and-forth replies.
			if (!(traceTag && (!Q_stricmp(traceTag, "dead_chat_reply") || !Q_stricmp(traceTag, "chat_mention_reply"))))
				MaybeQueueDeadChatReplies(speaker, msg, bSenderDead);
		}

		DebugLog("[ai] SayAsPlayer trace=%s event=%s speaker=%d teamonly=%d receivers=%d sent=%d msg='%s'",
			(traceTag && traceTag[0]) ? traceTag : "?",
			EventName(gameEvent),
			speaker->entindex(), teamonly ? 1 : 0, considered, sent, msg);
	}

	static void StripQuotesAndTrim(std::string &s)
	{
		// trim spaces
		while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
		while (!s.empty() && (s.back() == ' ')) s.pop_back();

		// strip surrounding quotes
		if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		{
			s = s.substr(1, s.size() - 2);
			while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
			while (!s.empty() && (s.back() == ' ')) s.pop_back();
		}

		// remove any remaining quotes
		for (char &c : s)
		{
			if (c == '"')
				c = ' ';
		}
		// collapse double spaces (cheap)
		for (size_t i = 1; i < s.size(); )
		{
			if (s[i] == ' ' && s[i - 1] == ' ')
				s.erase(i, 1);
			else
				i++;
		}
		while (!s.empty() && s.back() == ' ') s.pop_back();
	}

	static void RemoveOwnName(std::string &s, const char *speakerName)
	{
		if (!speakerName || !*speakerName || s.empty())
			return;

		auto toLower = [](unsigned char c) { return (unsigned char)std::tolower(c); };
		std::string needle(speakerName);
		if (needle.empty())
			return;

		// case-insensitive find/erase (simple; short strings)
		for (;;)
		{
			auto it = std::search(
				s.begin(), s.end(),
				needle.begin(), needle.end(),
				[&](char a, char b) { return toLower((unsigned char)a) == toLower((unsigned char)b); });

			if (it == s.end())
				break;

			s.erase(it, it + needle.size());
		}

		// cleanup spaces
		for (size_t i = 1; i < s.size(); )
		{
			if (s[i] == ' ' && s[i - 1] == ' ')
				s.erase(i, 1);
			else
				i++;
		}
		while (!s.empty() && s.front() == ' ') s.erase(s.begin());
		while (!s.empty() && s.back() == ' ') s.pop_back();
	}

	static std::string NameToAlnumLower(const char *name)
	{
		std::string out;
		if (!name)
			return out;
		for (const char *p = name; *p; ++p)
		{
			if (std::isalnum((unsigned char)*p))
				out.push_back((char)std::tolower((unsigned char)*p));
		}
		return out;
	}

	static void AppendNameTokensLower(std::string &out, const char *name)
	{
		if (!name || !*name)
			return;

		auto appendToken = [&](const std::string &token)
		{
			if (token.size() < 3)
				return;
			if (token == "the" || token == "noob" || token == "online" || token == "plays" || token == "player")
				return;
			out += " ";
			out += token;
			out += " ";
		};

		appendToken(NameToAlnumLower(name));

		std::string part;
		for (const char *p = name; ; ++p)
		{
			const unsigned char uc = (unsigned char)*p;
			if (*p && std::isalnum(uc))
			{
				part.push_back((char)std::tolower(uc));
				continue;
			}
			appendToken(part);
			part.clear();
			if (!*p)
				break;
		}
	}

	static bool ContainsAllowedNameToken(const std::string &allowedTokens, const std::string &token)
	{
		if (token.size() < 3)
			return true;
		const std::string needle = " " + token + " ";
		return allowedTokens.find(needle) != std::string::npos;
	}

	static bool TextMentionsNameToken(const std::string &textNorm, const std::string &token)
	{
		if (token.size() < 3)
			return false;
		if (token == "the" || token == "noob" || token == "online" || token == "plays" || token == "player")
			return false;
		return textNorm.find(token) != std::string::npos;
	}

	static void DropUnrelatedKillNameMentions(std::string &txt, const std::string &allowedTokens)
	{
		if (txt.empty() || allowedTokens.empty() || !gpGlobals)
			return;

		const std::string textNorm = NameToAlnumLower(txt.c_str());
		if (LineMentionsKnownWeapon(LowerAsciiCopy(txt)))
		{
			bool mentionsAllowedPlayer = false;
			size_t allowPos = 0;
			while (allowPos < allowedTokens.size())
			{
				while (allowPos < allowedTokens.size() && allowedTokens[allowPos] == ' ')
					++allowPos;
				size_t allowEnd = allowPos;
				while (allowEnd < allowedTokens.size() && allowedTokens[allowEnd] != ' ')
					++allowEnd;
				if (allowEnd > allowPos)
				{
					const std::string token = allowedTokens.substr(allowPos, allowEnd - allowPos);
					if (TextMentionsNameToken(textNorm, token))
					{
						mentionsAllowedPlayer = true;
						break;
					}
				}
				allowPos = allowEnd;
			}
			if (!mentionsAllowedPlayer)
			{
				txt.clear();
				return;
			}
		}

		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			CBasePlayer *p = UTIL_PlayerByIndex(i);
			if (!p || !p->IsPlayer() || !p->pev)
				continue;

			const char *name = STRING(p->pev->netname);
			if (!name || !*name)
				continue;

			std::string tokens;
			AppendNameTokensLower(tokens, name);
			size_t pos = 0;
			while (pos < tokens.size())
			{
				while (pos < tokens.size() && tokens[pos] == ' ')
					++pos;
				size_t end = pos;
				while (end < tokens.size() && tokens[end] != ' ')
					++end;
				if (end > pos)
				{
					const std::string token = tokens.substr(pos, end - pos);
					if (TextMentionsNameToken(textNorm, token) && !ContainsAllowedNameToken(allowedTokens, token))
					{
						txt.clear();
						return;
					}
				}
				pos = end;
			}
		}
	}

	// Remove core nickname (e.g. "-LMK-" -> "lmk") as a word so "LMK goes mid" is cleaned.
	static void RemoveAlnumCoreAsWord(std::string &s, const std::string &coreLower)
	{
		if (coreLower.size() < 2)
			return;

		const size_t len = coreLower.size();
		for (;;)
		{
			bool removed = false;
			for (size_t pos = 0; pos + len <= s.size(); ++pos)
			{
				bool same = true;
				for (size_t j = 0; j < len; ++j)
				{
					if (std::tolower((unsigned char)s[pos + j]) != (unsigned char)coreLower[j])
					{
						same = false;
						break;
					}
				}
				if (!same)
					continue;
				if (pos > 0 && std::isalnum((unsigned char)s[pos - 1]))
					continue;
				if (pos + len < s.size() && std::isalnum((unsigned char)s[pos + len]))
					continue;

				s.erase(pos, len);
				removed = true;
				break;
			}
			if (!removed)
				break;
		}

		for (size_t i = 1; i < s.size(); )
		{
			if (s[i] == ' ' && s[i - 1] == ' ')
				s.erase(i, 1);
			else
				i++;
		}
		while (!s.empty() && s.front() == ' ') s.erase(s.begin());
		while (!s.empty() && s.back() == ' ') s.pop_back();
	}

	static void StripLeadingNameColon(std::string &s, const char *rawName, const std::string &coreLower)
	{
		const size_t c = s.find(':');
		if (c == std::string::npos || c > 28u)
			return;

		std::string left = s.substr(0, c);
		while (!left.empty() && std::isspace((unsigned char)left.front())) left.erase(left.begin());
		while (!left.empty() && std::isspace((unsigned char)left.back())) left.pop_back();

		if (left.empty())
			return;

		auto lowerEq = [](const std::string &a, const char *b) {
			if (!b)
				return false;
			if (a.size() != Q_strlen(b))
				return false;
			for (size_t i = 0; i < a.size(); ++i)
			{
				if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
					return false;
			}
			return true;
		};

		const std::string leftCore = NameToAlnumLower(left.c_str());
		if (!lowerEq(left, rawName) && leftCore != coreLower)
			return;

		s.erase(0, c + 1);
		while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
	}

	static void SanitizeSelfReferences(std::string &s, CBasePlayer *speaker)
	{
		if (!speaker || !speaker->IsBot())
			return;

		auto removeNameParts = [&](const char *name)
		{
			if (!name || !*name)
				return;

			std::string part;
			for (const char *p = name; ; ++p)
			{
				const unsigned char uc = (unsigned char)*p;
				if (*p && std::isalnum(uc))
				{
					part.push_back((char)std::tolower(uc));
					continue;
				}

				if (part.size() >= 3)
				{
					bool hasAlpha = false;
					for (char c : part)
					{
						if (std::isalpha((unsigned char)c))
						{
							hasAlpha = true;
							break;
						}
					}
					if (hasAlpha)
						RemoveAlnumCoreAsWord(s, part);
				}
				part.clear();

				if (!*p)
					break;
			}
		};

		const char *net = STRING(speaker->pev->netname);
		RemoveOwnName(s, net);

		const std::string core = NameToAlnumLower(net);
		RemoveAlnumCoreAsWord(s, core);
		removeNameParts(net);

		StripLeadingNameColon(s, net, core);

		if (const BotProfile *pf = static_cast<CBot *>(speaker)->GetProfile())
		{
			const char *pn = pf->GetName();
			if (pn && *pn && Q_stricmp(pn, net))
			{
				RemoveOwnName(s, pn);
				RemoveAlnumCoreAsWord(s, NameToAlnumLower(pn));
				removeNameParts(pn);
			}
		}
	}

	static size_t CountWords(const std::string &s)
	{
		size_t n = 0;
		size_t i = 0;
		while (i < s.size())
		{
			while (i < s.size() && std::isspace((unsigned char)s[i]))
				++i;
			if (i >= s.size())
				break;
			++n;
			while (i < s.size() && !std::isspace((unsigned char)s[i]))
				++i;
		}
		return n;
	}

	// Keep at most maxWords words (cuts tail).
	static void TruncateToMaxWords(std::string &s, size_t maxWords)
	{
		if (maxWords == 0 || CountWords(s) <= maxWords)
			return;

		size_t words = 0;
		size_t wordStart = 0;
		for (size_t i = 0; i <= s.size(); ++i)
		{
			const bool atEnd = (i == s.size());
			const bool ws = !atEnd && std::isspace((unsigned char)s[i]);
			if (atEnd || ws)
			{
				if (i > wordStart)
				{
					++words;
					if (words == maxWords)
					{
						if (!atEnd)
						{
							s.resize(i);
							while (!s.empty() && std::isspace((unsigned char)s.back()))
								s.pop_back();
						}
						return;
					}
				}
				wordStart = i + 1;
			}
		}
	}

	// Fix common LLM typo "de_dust2" / "de_dust" -> drop the "de_" prefix.
	static void StripDeMapPrefix(std::string &s)
	{
		for (size_t k = 0; k < 4u; ++k)
		{
			bool changed = false;
			for (size_t i = 0; i + 3 < s.size(); ++i)
			{
				if (std::tolower((unsigned char)s[i]) == 'd'
					&& std::tolower((unsigned char)s[i + 1]) == 'e'
					&& s[i + 2] == '_')
				{
					s.erase(i, 3);
					changed = true;
					break;
				}
			}
			if (!changed)
				break;
		}
		while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
		while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
	}

	static const char *PlaceNameForOrigin(const Vector &origin)
	{
		const char *placeName = nullptr;
		if ((AreRunningCZero()) && TheBotPhrases)
		{
			const Place playerPlace = TheNavAreaGrid.GetPlace(&origin);
			const BotPhraseList *placeList = TheBotPhrases->GetPlaceList();
			for (auto phrase : *placeList)
			{
				if (phrase->GetID() == playerPlace)
				{
					placeName = phrase->GetName();
					break;
				}
			}
			if (!placeName)
				placeName = TheNavAreaGrid.IDToName(playerPlace);
		}
		return (placeName && placeName[0]) ? placeName : nullptr;
	}

	static bool ContainsCaseInsensitive(const std::string &hay, const char *needleLower)
	{
		if (!needleLower || !*needleLower)
			return false;
		const size_t nlen = std::strlen(needleLower);
		if (hay.size() < nlen)
			return false;

		auto toLower = [](unsigned char c) { return (unsigned char)std::tolower(c); };
		const char *nb = needleLower;
		const char *ne = needleLower + nlen;
		auto it = std::search(hay.begin(), hay.end(), nb, ne,
			[&](char a, char b) { return toLower((unsigned char)a) == toLower((unsigned char)b); });
		return it != hay.end();
	}

	// Alive speakers shouldn't do low-signal narration ("that guy got X").
	static void DropUselessAliveNarration(std::string &s)
	{
		if (s.empty())
			return;
		if (ContainsCaseInsensitive(s, "that guy got"))
			s.clear();
	}

	static std::string NormalizeForRepeatCheck(const std::string &s)
	{
		std::string out;
		out.reserve(s.size());
		bool lastSpace = false;
		for (char c : s)
		{
			const unsigned char uc = (unsigned char)c;
			if (std::isalnum(uc))
			{
				out.push_back((char)std::tolower(uc));
				lastSpace = false;
			}
			else if (std::isspace(uc))
			{
				if (!lastSpace)
					out.push_back(' ');
				lastSpace = true;
			}
		}
		while (!out.empty() && out.front() == ' ') out.erase(out.begin());
		while (!out.empty() && out.back() == ' ') out.pop_back();
		return out;
	}

	static bool MatchesRecentChat(const std::string &line)
	{
		if (line.empty())
			return false;
		const std::string norm = NormalizeForRepeatCheck(line);
		if (norm.size() < 4)
			return false;
		for (const std::string &r : s_recentChatLines)
		{
			if (NormalizeForRepeatCheck(r) == norm)
				return true;
		}
		return false;
	}

	static const char *FallbackLineForEvent(GameEventType event, const char *traceTag, bool speakerDead)
	{
		(void)event;
		(void)speakerDead;
		static const char *ffLines[] = { "yo stop", "watch it", "chill", "stop shooting me" };

		if (traceTag && !Q_stricmp(traceTag, "friendly_fire_repeat"))
			return ffLines[RANDOM_LONG(0, int(sizeof(ffLines) / sizeof(ffLines[0])) - 1)];
		return nullptr;
	}

	static bool LooksTactical(const char *msg)
	{
		if (!msg || !*msg)
			return false;

		// Very small heuristic: if it contains plan/callout keywords, keep it in team chat.
		// We prefer false negatives over false positives.
		char buf[256];
		Q_strlcpy(buf, msg, sizeof(buf));
		for (char *p = buf; *p; ++p)
			*p = (char)std::tolower((unsigned char)*p);

		const char *needles[] = {
			" go ", " going ", " lets go", " let's go", " rush", " push", " rotate", " rotate ", " exec",
			" mid", " b", " a", " long", " short", " ct", " t spawn", " ramp", " palace", " apps", " banana",
			" plant", " defuse", " defus", " save", " stack", " split", " smoke", " flash", " molly", " nade",
			" need help", " help ", " bomb ", " c4 "
		};

		// add spaces around buffer for cheap word-ish matching
		char padded[300];
		Q_snprintf(padded, sizeof(padded), " %s ", buf);

		for (const char *n : needles)
		{
			if (Q_strstr(padded, n))
				return true;
		}

		// Site callouts like "go b" / "go a" without spaces around letters
		if (Q_strstr(padded, " go b") || Q_strstr(padded, " go a") || Q_strstr(padded, " to b") || Q_strstr(padded, " to a"))
			return true;

		return false;
	}

	static void QueueDeadChatReply(CBasePlayer *replier, CBasePlayer *sender, const char *msg, const char *traceTag, const char *detail)
	{
		if (!replier || !replier->IsBot() || replier->pev->deadflag == DEAD_NO || !gpGlobals)
			return;

		EnsureWorker();
		const char *mapName = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";

		CCSBot *me = static_cast<CCSBot *>(replier);
		std::string gameStateJson;
		BuildGameStateJson(gameStateJson, mapName, me ? me->GetGameState() : nullptr);

		std::string botStateJson;
		BuildBotStateJson(botStateJson, replier, false, EVENT_PLAYER_DIED, nullptr, nullptr);

		std::string senderStateJson;
		if (sender && sender->IsPlayer())
			BuildBotStateJson(senderStateJson, sender, sender->pev->deadflag == DEAD_NO, EVENT_PLAYER_DIED, nullptr, nullptr);

		std::string sys;
		BuildOllamaSystemPrompt(sys, replier, false, Q_max(20, Q_min(int(sv_ai_max_chars.value), 240)), nullptr);

		std::string user;
		user.reserve(2000);
		user += "TASK: write one dead chat reply\n";
		user += "GAME_STATE=" + gameStateJson + "\n";
		user += "BOT_STATE=" + botStateJson + "\n";
		if (!senderStateJson.empty())
			user += "OTHER_BOT_STATE=" + senderStateJson + "\n";
		user += "EVENT=";
		user += traceTag ? traceTag : "dead_chat_reply";
		user += "\nINTENT=";
		user += detail ? detail : "reply to dead chat banter";
		user += "\nMESSAGE_FROM=";
		user += (sender && sender->pev) ? STRING(sender->pev->netname) : "someone";
		user += "\nMESSAGE=\"";
		user += PromptJsonEscape(msg ? msg : "");
		user += "\"\n";
		user += "REPLY_FACT_RULE= OTHER_BOT_STATE is the player you are replying to. Prefer player names over weapon names. Do not mention a weapon unless MESSAGE or OTHER_BOT_STATE proves that exact weapon. Do not claim they were also/same/sniped/headshot/knifed unless OTHER_BOT_STATE kill_type or weapon proves it.\n";
		AppendRecentChatBuffer(user);
		AppendRosterUser(user);

		OllamaRequest req;
		req.speakerEntIndex = replier->entindex();
		req.createdAt = gpGlobals ? gpGlobals->time : 0.0f;
		req.wallSentMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		req.event = EVENT_PLAYER_DIED;
		Q_strlcpy(req.traceTag, (traceTag && *traceTag) ? traceTag : "dead_chat_reply", sizeof(req.traceTag));
		req.speakerAlive = false;
		req.numPredictChars = Q_max(20, Q_min(int(sv_ai_max_chars.value), 240));
		req.mapInternalLower = MapInternalLower((mapName && *mapName) ? mapName : "");
		req.reactionDelay = RANDOM_FLOAT(AiTiming::kDeadReactMin, AiTiming::kDeadReactMax);
		req.systemPrompt = std::move(sys);
		req.factContextLower = LowerAsciiCopy(gameStateJson + "\n" + botStateJson + "\n" + senderStateJson + "\n" + (detail ? detail : "") + "\nMESSAGE=" + (msg ? msg : ""));
		req.needsReplyValidation = true;
		req.replySourceMessage = msg ? msg : "";
		req.userPrompt = std::move(user);

		{
			std::lock_guard<std::mutex> lk(g_reqMutex);
			while (g_reqQ.size() > 16)
				g_reqQ.pop();
			g_reqQ.push(std::move(req));
		}
		g_reqCv.notify_one();
	}

	static bool LooksLikeCompliment(const char *msgLower)
	{
		if (!msgLower || !*msgLower)
			return false;
		char padded[256];
		Q_snprintf(padded, sizeof(padded), " %s ", msgLower);
		const char *needles[] = {
			" ns ", " gj ", " nice ", " good ", " good shot ", " nice shot ", " clean ", " sick ",
			" insane ", " clutch ", " wp ", " owned ", " got him ", " nice one "
		};
		for (const char *n : needles)
		{
			if (Q_strstr(padded, n))
				return true;
		}
		return false;
	}

	static void MaybeQueueDeadChatReplies(CBasePlayer *sender, const char *msg, bool senderDead)
	{
		if (!gpGlobals || !sender || !sender->IsBot() || !senderDead || !msg || !msg[0])
			return;

		// High-priority mention reply: if a bot directly names another dead bot, that bot usually answers.
		char msgLower[192];
		Q_strlcpy(msgLower, msg, sizeof(msgLower));
		for (char *p = msgLower; *p; ++p)
			if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');

		static float s_lastDirectMentionReply[MAX_CLIENTS + 1] = { 0.0f };
		for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_CLIENTS; ++i)
		{
			CBasePlayer *target = UTIL_PlayerByIndex(i);
			if (!target || !target->IsBot() || target == sender || target->pev->deadflag == DEAD_NO)
				continue;
			if (gpGlobals->time - s_lastDirectMentionReply[i] < 2.5f)
				continue;
			const char *tname = STRING(target->pev->netname);
			if (!tname || !*tname)
				continue;
			char tLower[64];
			Q_strlcpy(tLower, tname, sizeof(tLower));
			for (char *p = tLower; *p; ++p)
				if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
			if (!Q_strstr(msgLower, tLower))
				continue;
			const bool compliment = LooksLikeCompliment(msgLower);
			if (!PassesSpeakProb(compliment ? 0.90f : 0.86f))
				continue;
			s_lastDirectMentionReply[i] = gpGlobals->time;
			QueueDeadChatReply(target, sender, msg,
				compliment ? "chat_compliment_reply" : "chat_mention_reply",
				compliment
					? "you were complimented in dead chat; reply positive but restrained, 1-3 words, like ty/thx/cheers; do not overdo lol"
					: "you were mentioned in dead chat; answer back naturally, short, no coaching");
			return;
		}

		// Lightweight dead-only back-and-forth without explicit mentions.
		static float s_lastDeadReplyGlobal = 0.0f;
		if (gpGlobals->time - s_lastDeadReplyGlobal < 1.4f)
			return;
		if (!PassesSpeakProb(0.42f))
			return;

		CBasePlayer *replier = PickRandomDeadBotExcluding(sender, nullptr);
		if (!replier || !replier->IsBot() || replier == sender)
			return;
		s_lastDeadReplyGlobal = gpGlobals->time;
		QueueDeadChatReply(replier, sender, msg, "dead_chat_reply",
			"reply to the previous dead chat line with short pub banter, no coaching");
	}
}

void AiChat::OnChatLine(CBasePlayer *sender, const char *msg, bool teamonly, bool senderDead, const char *placeName)
{
	if (!sender || !sender->IsPlayer() || !msg || !msg[0])
		return;

	const char *name = (sender->pev) ? STRING(sender->pev->netname) : "";
	char buf[196];
	buf[0] = '\0';

	// Keep it compact and copy-pasteable for the LLM.
	// Example: "*dead* (team) nick @ bsite: message"
	if (senderDead)
		Q_strlcpy(buf, "*dead* ", sizeof(buf));

	if (teamonly)
		Q_strlcat(buf, "(team) ", sizeof(buf));

	if (name && *name)
	{
		Q_strlcat(buf, name, sizeof(buf));
		Q_strlcat(buf, ": ", sizeof(buf));
	}

	// Optional location hint (human chat already prints it client-side sometimes; we keep it short here).
	if (placeName && placeName[0])
	{
		Q_strlcat(buf, "@", sizeof(buf));
		Q_strlcat(buf, placeName, sizeof(buf));
		Q_strlcat(buf, " ", sizeof(buf));
	}

	Q_strlcat(buf, msg, sizeof(buf));

	RecordRecentAiChatLine(buf);
	MaybeQueueDeadChatReplies(sender, msg, senderDead);
}

void AiChat::Frame()
{
	// Flush debug messages to server console on main thread
	{
		std::queue<std::string> dbg;
		{
			std::lock_guard<std::mutex> lk(g_dbgMutex);
			std::swap(dbg, g_dbgQ);
		}
		while (!dbg.empty())
		{
			UTIL_ServerPrint("%s\n", dbg.front().c_str());
			dbg.pop();
		}
	}

	// Deliver any completed Ollama responses.
	std::queue<OllamaResponse> local;
	{
		std::lock_guard<std::mutex> lk(g_resMutex);
		std::swap(local, g_resQ);
	}

	// Per-bot cooldown to prevent rapid-fire spam.
	static float s_nextBotSpeakTime[MAX_CLIENTS + 1] = { 0.0f };
	// Last time any AI chat line was actually delivered (all bots share spacing).
	static float s_lastAnyAiLineTime = -1.0e9f;

	while (!local.empty())
	{
		OllamaResponse r = std::move(local.front());
		local.pop();

		// Finish scheduling using server time only on the game thread (worker must not touch gpGlobals).
		if (r.scheduleDeliverOnMain && gpGlobals)
		{
			const float readyToType = Q_max(r.deliverReactionEnd, gpGlobals->time);
			r.deliverAfter = readyToType + r.deliverTypingSec;
			r.scheduleDeliverOnMain = false;
			if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
				ChatFreezeSet(r.speakerEntIndex, r.deliverAfter);
		}

		// Add a human-ish delay so messages don't feel instant/robotic.
		if (r.deliverAfter > 0.0f && gpGlobals && gpGlobals->time < r.deliverAfter)
		{
			// Not ready yet, requeue.
			if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
				ChatFreezeSet(r.speakerEntIndex, r.deliverAfter);
			std::lock_guard<std::mutex> lk(g_resMutex);
			g_resQ.push(std::move(r));
			continue;
		}

		// Drop stale responses (they feel "random/out of context" if they arrive too late)
		const float maxAge = 45.0f;
		if (r.createdAt > 0.0f && gpGlobals && (gpGlobals->time - r.createdAt) > maxAge)
		{
			DebugLog("[ai] dropping stale response event=%s ent=%d age=%.2fs",
				EventName(r.event), r.speakerEntIndex, gpGlobals->time - r.createdAt);
			if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
				ChatFreezeClear(r.speakerEntIndex);
			continue;
		}

		CBasePlayer *speaker = UTIL_PlayerByIndex(r.speakerEntIndex);
		if (!speaker || !speaker->IsBot())
			speaker = PickRandomBot();

		if (speaker && speaker->IsBot() && !r.text.empty())
		{
			const int idx = speaker->entindex();
			if (idx > 0 && idx <= MAX_CLIENTS && gpGlobals && gpGlobals->time < s_nextBotSpeakTime[idx])
			{
				// Not ready, push it out a bit more.
				r.deliverAfter = Q_max(r.deliverAfter, s_nextBotSpeakTime[idx] + RANDOM_FLOAT(1.0f, 2.8f));
				if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
					ChatFreezeSet(r.speakerEntIndex, r.deliverAfter);
				std::lock_guard<std::mutex> lk(g_resMutex);
				g_resQ.push(std::move(r));
				continue;
			}

			std::string txt = std::move(r.text);
			StripQuotesAndTrim(txt);
			StripPromptLeaksFromOutput(txt);
			StripWtfUnlessFriendlyFire(txt, r.traceTag);
			StripDustinHallucination(txt);
			StripModernClipPhrases(txt);
			StripPipeAndAwkwardGamingWords(txt);
			StripLowValueFiller(txt);
			StripLagCity(txt);
			for (char &c : txt)
			{
				if (c == '*')
					c = ' ';
			}
			// Prevent "X camping" when X is already dead.
			if (ContainsCampWord(txt) && MentionsDeadPlayerName(txt))
				txt.clear();
			SanitizeSelfReferences(txt, speaker);
			StripDeMapPrefix(txt);
			StripMapNameUnlessExplicit(txt, r.mapInternalLower);
			DropLineIfMapHallucination(txt, r.mapInternalLower);
			RejectKnownGibberishTokens(txt);
			DropLineIfNoVowelGarbageToken(txt);
			StripPolishedClichés(txt);
			StripModernNonCS16Jargon(txt);
			StripCtOwnPlantClaims(txt, speaker->m_iTeam);
			DropUnsupportedWeaponClaims(txt, r.factContextLower);
			StripUnsupportedRepeatClaims(txt, r.factContextLower, r.traceTag);
			DropInvalidSpectatorSelfClaims(txt, r.traceTag);
			DropUnrelatedKillNameMentions(txt, r.allowedMentionNamesLower);
			StripBombStateInconsistencies(txt, speaker->m_iTeam);
			if (speaker->pev->deadflag == DEAD_NO)
			{
				TruncateToMaxWords(txt, AiTiming::kAliveMaxWordsDeliver);
				StripAliveSelfSitrep(txt);
				DropUselessAliveNarration(txt);
			}
			else
			{
				// Dead-chat debug can get very spammy; hard-drop exact repeats from recent chat buffer.
				if (MatchesRecentChat(txt))
					txt.clear();
			}
			if (txt.empty())
			{
				if (const char *fallback = FallbackLineForEvent(r.event, r.traceTag, speaker->pev->deadflag != DEAD_NO))
				{
					txt = fallback;
					if (speaker->pev->deadflag != DEAD_NO && MatchesRecentChat(txt))
						txt.clear();
				}
			}
			if (!txt.empty())
			{
				// Debug timing: wall-clock from request→response→delivered, plus scheduled delays.
				if (sv_ai_debug.value > 0.0f && r.wallSentMs)
				{
					const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count();
					const uint64_t ollamaMs = (r.wallResponseMs > r.wallSentMs) ? (r.wallResponseMs - r.wallSentMs) : 0;
					const uint64_t totalMs = (nowMs > r.wallSentMs) ? (nowMs - r.wallSentMs) : 0;
					const uint64_t postMs = (nowMs > r.wallResponseMs) ? (nowMs - r.wallResponseMs) : 0;
					DebugLog("[ai] deliver timing trace=%s event=%s ent=%d total_ms=%llu ollama_ms=%llu post_ms=%llu reaction=%.2fs typing=%.2fs game_age=%.2fs",
						r.traceTag[0] ? r.traceTag : "?",
						EventName(r.event),
						r.speakerEntIndex,
						(unsigned long long)totalMs,
						(unsigned long long)ollamaMs,
						(unsigned long long)postMs,
						r.reactionDelay,
						r.deliverTypingSec,
						(gpGlobals && r.createdAt > 0.0f) ? (gpGlobals->time - r.createdAt) : 0.0f);
				}

				// Space out lines from different bots queued in the same burst.
				const bool speakerDead = (speaker->pev->deadflag != DEAD_NO);
				const float globalGap = (sv_ai_debug.value > 0.0f && speakerDead) ? 2.0f : AiTiming::kGlobalMinGapBetweenAnyLines;
				if (gpGlobals && gpGlobals->time < s_lastAnyAiLineTime + globalGap)
				{
					r.deliverAfter = s_lastAnyAiLineTime + globalGap + RANDOM_FLOAT(0.20f, 0.85f);
					if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
						ChatFreezeSet(r.speakerEntIndex, r.deliverAfter);
					std::lock_guard<std::mutex> lk(g_resMutex);
					g_resQ.push(std::move(r));
					continue;
				}

				// Scope: dead bots always use global chat (everyone sees it; vanilla dead team-say would not reach alive).
				bool teamonly = true;
				if (speakerDead)
					teamonly = false;
				else if (!Q_stricmp(sv_ai_chat_scope.string, "all"))
					teamonly = false;
				else if (!Q_stricmp(sv_ai_chat_scope.string, "team"))
					teamonly = true;
				else
				{
					// auto: short casual lines may go to all chat; anything tactical or longer stays team-only.
					teamonly = LooksTactical(txt.c_str())
						|| r.event == EVENT_ROUND_START || r.event == EVENT_BOMB_PLANTED || r.event == EVENT_BOMB_DEFUSED
						|| r.event == EVENT_BOMB_EXPLODED || r.event == EVENT_BOMB_DEFUSING || r.event == EVENT_BOMB_DEFUSE_ABORTED
						|| r.event == EVENT_BOMB_DROPPED || r.event == EVENT_BOMB_PICKED_UP
						|| r.event == EVENT_TERRORISTS_WIN || r.event == EVENT_CTS_WIN || r.event == EVENT_ROUND_DRAW
						|| r.event == EVENT_VIP_ESCAPED || r.event == EVENT_VIP_ASSASSINATED
						|| r.event == EVENT_HOSTAGE_RESCUED || r.event == EVENT_ALL_HOSTAGES_RESCUED || r.event == EVENT_HOSTAGE_KILLED
						|| r.event == EVENT_HOSTAGE_DAMAGED
						|| txt.length() > 36;
				}

				const int beforeCount = (sv_ai_debug.value > 0.0f) ? 1 : 0;
				(void)beforeCount;
				SayAsPlayer(speaker, txt.c_str(), teamonly ? TRUE : FALSE, r.event, r.traceTag);
				if (gpGlobals)
					s_lastAnyAiLineTime = gpGlobals->time;
				if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
					ChatFreezeClear(r.speakerEntIndex);
				if (idx > 0 && idx <= MAX_CLIENTS && gpGlobals)
				{
					const bool aliveNow = (speaker->pev->deadflag == DEAD_NO);
					const bool debugChattyDead = (sv_ai_debug.value > 0.0f && !aliveNow);
					s_nextBotSpeakTime[idx] = gpGlobals->time
						+ (aliveNow
							? RANDOM_FLOAT(AiTiming::kAlivePostSpeakMin, AiTiming::kAlivePostSpeakMax)
							: (debugChattyDead
								? RANDOM_FLOAT(2.0f, 6.0f)
								: RANDOM_FLOAT(AiTiming::kDeadPostSpeakMin, AiTiming::kDeadPostSpeakMax)));
				}
			}
			else if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
				ChatFreezeClear(r.speakerEntIndex);
		}
		else if (r.speakerAlive && r.speakerEntIndex >= 1 && r.speakerEntIndex <= MAX_CLIENTS)
			ChatFreezeClear(r.speakerEntIndex);
	}

	// If AI chat is off, stop the worker to avoid background activity.
	if (sv_ai_chat_enable.value <= 0.0f)
	{
		StopWorker();
	}
}

void AiChat::OnEvent(GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther)
{
	if (sv_ai_chat_enable.value <= 0.0f)
		return;

	// Basic throttles to avoid spam.
	static float s_lastRoundStart = 0.0f;
	static float s_lastBomb = 0.0f;
	static float s_lastKill = 0.0f;
	static float s_lastBombExploded = 0.0f;
	static float s_lastBombDefusing = 0.0f;
	static float s_lastBombDefuseAbort = 0.0f;
	static float s_lastBombDropped = 0.0f;
	static float s_lastBombPickedUp = 0.0f;
	static float s_lastRoundOutcome = 0.0f;
	static float s_lastVipEvent = 0.0f;
	static float s_lastHostageRescued = 0.0f;
	static float s_lastHostageAll = 0.0f;
	static float s_lastHostageKilled = 0.0f;
	static float s_lastHostageDamaged = 0.0f;
	static float s_lastGrenadeExploded = 0.0f;
	static float s_lastFlashBlinded = 0.0f;
	static float s_lastGameCommence = 0.0f;
	static float s_lastPlayerSpawnChat = 0.0f;

	auto emit = [&](CBasePlayer *bot, const char *canned, bool spectacularEvent, const char *traceTag)
	{
		CBasePlayer *speaker = bot ? bot : PickRandomBotPreferDead();
		if (!speaker || !speaker->IsBot())
			return;

		const bool alive = (speaker->pev->deadflag == DEAD_NO);

		// Alive bots: only speak for very high-signal events (avoid useless narration that gets you killed mid-typing).
		// - Friendly-fire repeat: allowed
		// - Alive killer: only if spectacular (knife/clutch)
		if (alive)
		{
			const bool isFf = (traceTag && !Q_stricmp(traceTag, "friendly_fire_repeat"));
			const bool isSpectacularKiller = (traceTag && !Q_stricmp(traceTag, "kill_alive_killer") && spectacularEvent);
			const bool isRoundEndReaction = (traceTag && !Q_stricmp(traceTag, "round_end_react"));
			if (!isFf && !isSpectacularKiller && !isRoundEndReaction)
				return;
		}

		if (!BotHasRoundChatBudget(speaker))
		{
			DebugLog("[ai] skip no round chat budget ent=%d", speaker->entindex());
			return;
		}

		// Friendly-fire complaints are explicitly configured at 50% (handled by caller).
		if (!spectacularEvent && traceTag && !Q_stricmp(traceTag, "friendly_fire_repeat"))
		{
			// already rolled in caller
		}
		else if (!PassesEventSpeakRoll(spectacularEvent))
		{
			// Debug mode: let dead bots be very chatty so we can validate prompts/filters quickly.
			if (!(sv_ai_debug.value > 0.0f && !alive))
				return;
		}
		if (!ConsumeRoundChatBudget(speaker))
			return;

		EnsureWorker();

		{
			const int globalMax = Q_max(20, Q_min(int(sv_ai_max_chars.value), 240));
			const int maxChars = alive ? Q_min(globalMax, AiTiming::kAliveMaxCharsPredict) : globalMax;

			const char *speakerName = STRING(speaker->pev->netname);
			const char *speakerTeam = GetTeam(speaker->m_iTeam);
			const char *mapName = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
			const char *actorName = "";
			const char *actorTeam = "";
			const char *otherName = "";
			const char *otherTeam = "";

			if (pEntity && pEntity->IsPlayer())
			{
				CBasePlayer *a = static_cast<CBasePlayer *>(pEntity);
				actorName = STRING(a->pev->netname);
				actorTeam = GetTeam(a->m_iTeam);
			}
			if (pOther && pOther->IsPlayer())
			{
				CBasePlayer *o = static_cast<CBasePlayer *>(pOther);
				otherName = STRING(o->pev->netname);
				otherTeam = GetTeam(o->m_iTeam);
			}

			std::string gameStateJson;
			const CSGameState *botGs = nullptr;
			if (speaker && speaker->IsBot())
			{
				const CCSBot *me = static_cast<const CCSBot *>(speaker);
				botGs = me->GetGameState();
			}
			BuildGameStateJson(gameStateJson, mapName, botGs);
			std::string botStateJson;
			BuildBotStateJson(botStateJson, speaker, alive, event, pEntity, pOther);
			std::string factContextRaw = gameStateJson + "\n" + botStateJson + "\n" + (canned ? canned : "");
			std::string allowedMentionNamesLower;

			std::string user;
			user.reserve(2400);
			user += "TASK: write one ";
			user += alive ? "alive" : "dead";
			user += " ";
			user += speakerTeam ? speakerTeam : "player";
			user += " chat line\n";
			user += "SPEAKER=";
			user += PromptJsonEscape((speakerName && *speakerName) ? speakerName : "unknown");
			user += "\nDO_NOT_OUTPUT_SPEAKER_NAME=true\n";
			user += "GAME_STATE=";
			user += gameStateJson;
			user += "\nBOT_STATE=";
			user += botStateJson;
			user += "\n";
			user += "EVENT=";
			user += PromptJsonEscape(EventName(event));
			user += "\nINTENT=";
			user += PromptJsonEscape(canned);
			user += "\n";

			AppendRecentChatBuffer(user);
			AppendRosterUser(user);

			if (actorName && *actorName)
			{
				user += "ACTOR=\"";
				user += PromptJsonEscape(actorName);
				user += "\" ACTOR_TEAM=\"";
				user += PromptJsonEscape(actorTeam ? actorTeam : "");
				user += "\"\n";
			}
			if (otherName && *otherName)
			{
				user += "OTHER=\"";
				user += PromptJsonEscape(otherName);
				user += "\" OTHER_TEAM=\"";
				user += PromptJsonEscape(otherTeam ? otherTeam : "");
				user += "\"\n";
			}

			if (event == EVENT_PLAYER_DIED && pEntity && pEntity->IsPlayer() && pOther && pOther->IsPlayer())
			{
				CBasePlayer *victimP = static_cast<CBasePlayer *>(pEntity);
				CBasePlayer *killerP = static_cast<CBasePlayer *>(pOther);
				const char *killType = KillTypeForDeath(victimP, killerP);
				const char *killWeapon = ActiveWeaponEntityName(killerP);
				int victimKilledByKillerCount = 0;
				const int vi = victimP ? victimP->entindex() : 0;
				const int ki = killerP ? killerP->entindex() : 0;
				if (vi >= 1 && vi <= MAX_CLIENTS && ki >= 1 && ki <= MAX_CLIENTS)
					victimKilledByKillerCount = s_killedByCount[vi][ki];
				user += "\nKILL_ROLES= ACTOR=VICTIM OTHER=KILLER VICTIM_state=";
				user += (victimP->pev->deadflag != DEAD_NO) ? "dead" : "alive";
				user += " KILLER_state=";
				user += (killerP->pev->deadflag != DEAD_NO) ? "dead" : "alive";
				user += ". If VICTIM is dead, no live coaching to them; past-tense or general banter only.\n";
				user += "KILL_FACTS= victim=\"";
				user += PromptJsonEscape(actorName ? actorName : "");
				user += "\" killer=\"";
				user += PromptJsonEscape(otherName ? otherName : "");
				user += "\" kill_type=\"";
				user += PromptJsonEscape(killType);
				user += "\" weapon=\"";
				user += PromptJsonEscape(killWeapon);
				user += "\" victim_killed_by_killer_count=";
				user += std::to_string(victimKilledByKillerCount);
				user += ". Only mention this weapon/kill_type, never invent another. Say again only when victim_killed_by_killer_count is 2+.\n";
				factContextRaw += "\nKILL_FACTS kill_type=";
				factContextRaw += killType;
				factContextRaw += " weapon=";
				factContextRaw += killWeapon;
				factContextRaw += " victim_killed_by_killer_count=";
				factContextRaw += std::to_string(victimKilledByKillerCount);
				AppendNameTokensLower(allowedMentionNamesLower, speakerName);
				AppendNameTokensLower(allowedMentionNamesLower, actorName);
				AppendNameTokensLower(allowedMentionNamesLower, otherName);
				if (traceTag && (Q_stricmp(traceTag, "kill_dead_spectator") == 0 || Q_stricmp(traceTag, "kill_teammate_react") == 0))
					user += "PERSPECTIVE_RULE= You are not the victim in KILL_FACTS. Do not say me/my/i got/i died; react as a spectator.\n";
				user += "NAME_RULE= Prefer ACTOR or OTHER if natural. Do not invent names.\n";
			}

			std::string sys;
			BuildOllamaSystemPrompt(sys, speaker, alive, maxChars, BotChatPersona(speaker));
			if (!(traceTag && !Q_stricmp(traceTag, "friendly_fire_repeat")))
				sys += "EXTRA_RULE= Avoid saying 'wtf' (overused). Use other short reactions.\n";

			if (sv_ai_debug.value > 0.0f)
			{
				DebugLog("[ai] ollama user BOT_STATE=%s", botStateJson.c_str());
			}

			OllamaRequest req;
			req.speakerEntIndex = speaker->entindex();
			req.createdAt = gpGlobals ? gpGlobals->time : 0.0f;
			req.wallSentMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			req.event = event;
			Q_strlcpy(req.traceTag, (traceTag && traceTag[0]) ? traceTag : "?", sizeof(req.traceTag));
			req.speakerAlive = alive;
			req.numPredictChars = maxChars;
			req.mapInternalLower = MapInternalLower((mapName && *mapName) ? mapName : "");
			req.factContextLower = LowerAsciiCopy(factContextRaw);
			req.allowedMentionNamesLower = std::move(allowedMentionNamesLower);
			float reaction;
			if (!alive)
			{
				if (event == EVENT_BOMB_PLANTED || event == EVENT_BOMB_DEFUSED)
					reaction = RANDOM_FLOAT(AiTiming::kDeadBombReactMin, AiTiming::kDeadBombReactMax);
				else
					reaction = RANDOM_FLOAT(AiTiming::kDeadReactMin, AiTiming::kDeadReactMax);
			}
			else
			{
				// Alive quick reactions for immediate personal events.
				if (event == EVENT_PLAYER_TOOK_DAMAGE
					|| event == EVENT_PLAYER_BLINDED_BY_FLASHBANG
					|| (traceTag && !Q_stricmp(traceTag, "friendly_fire_repeat")))
					reaction = RANDOM_FLOAT(AiTiming::kAliveQuickReactMin, AiTiming::kAliveQuickReactMax);
				else if (event == EVENT_PLAYER_DIED)
					reaction = RANDOM_FLOAT(AiTiming::kAliveKillReactMin, AiTiming::kAliveKillReactMax);
				else
					reaction = RANDOM_FLOAT(AiTiming::kAliveBombRoundReactMin, AiTiming::kAliveBombRoundReactMax);
			}
			req.reactionDelay = reaction;
			req.systemPrompt = std::move(sys);
			req.userPrompt = std::move(user);

			{
				std::lock_guard<std::mutex> lk(g_reqMutex);
				while (g_reqQ.size() > 16)
					g_reqQ.pop();
				g_reqQ.push(std::move(req));
			}
			g_reqCv.notify_one();
			DebugLog("[ai] queued trace=%s event=%s speaker=%d timeout=%dms",
				(traceTag && traceTag[0]) ? traceTag : "?",
				EventName(event), speaker->entindex(), int(sv_ai_timeout_ms.value));
		}
	};

	auto emitRoundEndReactions = [&](const char *outcome, const char *winTeam)
	{
		if (!outcome || !*outcome || !winTeam || !*winTeam)
			return;
		static float s_lastRoundEndReact = -1.0e9f;
		if (gpGlobals && gpGlobals->time - s_lastRoundEndReact < 5.0f)
			return;
		if (!PassesSpeakProb(0.72f))
			return;
		if (gpGlobals)
			s_lastRoundEndReact = gpGlobals->time;

		CBasePlayer *first = PickRandomBotPreferDead();
		char ctx[260];
		Q_snprintf(ctx, sizeof(ctx),
			"round ended: %s. winning_team=%s. If your BOT_STATE team won, give one short compliment or relief. If your team lost, give one short disappointed line. Keep it mostly neutral; no noob/trash insults unless RECENT_CHAT is already hostile. Mention a real player only if natural; no fake facts.",
			outcome, winTeam);
		emit(first, ctx, true, "round_end_react");

		if (PassesSpeakProb(0.35f))
		{
			CBasePlayer *second = PickRandomBotPreferDeadExcluding(first);
			if (second)
				emit(second, ctx, true, "round_end_react");
		}
	};

	auto emitDefuseTimingReaction = [&](CBasePlayer *defuser)
	{
		if (!defuser || !gpGlobals)
			return;
		static float s_lastDefuseTimingReact = -1.0e9f;
		if (gpGlobals->time - s_lastDefuseTimingReact < 8.0f)
			return;
		if (!PassesSpeakProb(0.26f))
			return;

		CGrenade *bomb = FindPlantedC4();
		if (!bomb || bomb->m_flC4Blow <= gpGlobals->time)
			return;

		const float secondsToBlow = bomb->m_flC4Blow - gpGlobals->time;
		const float defuseSeconds = defuser->m_bHasDefuser ? 5.0f : 10.0f;
		const float margin = secondsToBlow - defuseSeconds;
		const bool canDefuse = margin >= 0.0f;

		s_lastDefuseTimingReact = gpGlobals->time;
		char ctx[300];
		Q_snprintf(ctx, sizeof(ctx),
			"ct %s started defusing. bomb_time_left=%.1fs defuse_time=%.1fs can_finish_before_explosion=%s margin=%.1fs. Rare reaction: if can_finish is yes, short tense compliment/encourage; if no, short blame/panic. No fake certainty beyond these facts.",
			STRING(defuser->pev->netname),
			secondsToBlow,
			defuseSeconds,
			canDefuse ? "yes" : "no",
			margin);

		CBasePlayer *speaker = PickRandomDeadBotExcluding(defuser, nullptr);
		if (!speaker)
			speaker = PickRandomBotPreferDeadExcluding(defuser);
		emit(speaker, ctx, true, "defuse_timing_react");
	};

	switch (event)
	{
	case EVENT_ROUND_START:
		if (gpGlobals->time - s_lastRoundStart > 3.0f)
		{
			s_lastRoundStart = gpGlobals->time;
			RollBotRoundChatBudgets();
			emit(PickRandomBotPreferDead(),
				"round started on this map. type one lazy pub line — bored, half broken, maybe site letter. "
				"NOT match intro NOT gl hf stack NOT motivational.",
				false, "round_start");
		}
		break;

	case EVENT_BOMB_PLANTED:
		if (gpGlobals->time - s_lastBomb > 2.0f)
		{
			s_lastBomb = gpGlobals->time;
			CBasePlayer *planter = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			emit(planter && planter->IsBot() ? planter : nullptr,
				"bomb planted on this map. one gross casual reaction — panic or site word, messy typing ok. not a briefing.",
				false, "bomb_planted");
		}
		break;

	case EVENT_BOMB_DEFUSED:
		if (gpGlobals->time - s_lastBomb > 2.0f)
		{
			s_lastBomb = gpGlobals->time;
			CBasePlayer *defuser = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			emit(defuser && defuser->IsBot() ? defuser : nullptr,
				"bomb defused on this map. one tired pub line — not esports polished.",
				false, "bomb_defused");
			emitRoundEndReactions("ct defused the bomb", "CT");
		}
		break;

	case EVENT_BOMB_EXPLODED:
		if (gpGlobals->time - s_lastBombExploded > 2.5f)
		{
			s_lastBombExploded = gpGlobals->time;
			emit(nullptr,
				"bomb just exploded (round basically over). one messy pub reaction — shock or relief. not a recap essay, no trash/noob insults.",
				false, "bomb_exploded");
			emitRoundEndReactions("bomb exploded", "T");
		}
		break;

	case EVENT_BOMB_DEFUSING:
		if (gpGlobals->time - s_lastBombDefusing > 2.8f)
		{
			s_lastBombDefusing = gpGlobals->time;
			CBasePlayer *defuser = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			emit(defuser && defuser->IsBot() ? defuser : nullptr,
				"someone started defusing the bomb. one tense half line — panic typo ok. not a callout essay.",
				false, "bomb_defusing");
			if (defuser)
				emitDefuseTimingReaction(defuser);
		}
		break;

	case EVENT_BOMB_DEFUSE_ABORTED:
		if (gpGlobals->time - s_lastBombDefuseAbort > 2.0f)
		{
			s_lastBombDefuseAbort = gpGlobals->time;
			emit(nullptr,
				"bomb defuse got interrupted or stopped. one annoyed pub mutter — lowercase sloppy ok.",
				false, "bomb_defuse_aborted");
		}
		break;

	case EVENT_BOMB_DROPPED:
		if (gpGlobals->time - s_lastBombDropped > 3.5f)
		{
			s_lastBombDropped = gpGlobals->time;
			emit(nullptr,
				"bomb carrier dropped c4. one short factual pub line; useful or annoyed is ok. do not say somewhere, lazy, af, or lol.",
				false, "bomb_dropped");
		}
		break;

	case EVENT_BOMB_PICKED_UP:
		if (gpGlobals->time - s_lastBombPickedUp > 3.0f)
		{
			s_lastBombPickedUp = gpGlobals->time;
			CBasePlayer *carrier = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			emit(carrier && carrier->IsBot() ? carrier : nullptr,
				"someone picked the bomb back up. one short pub line — messy typing ok. not tactical briefing.",
				false, "bomb_picked_up");
		}
		break;

	case EVENT_TERRORISTS_WIN:
	case EVENT_CTS_WIN:
	case EVENT_ROUND_DRAW:
		if (gpGlobals->time - s_lastRoundOutcome > 4.0f)
		{
			s_lastRoundOutcome = gpGlobals->time;
			const char *ctx =
				(event == EVENT_TERRORISTS_WIN)
					? "terrorists won the round. one tired pub line — lowercase sloppy ok. not gg wp stack."
				: (event == EVENT_CTS_WIN)
					? "cts won the round. one tired pub line — lowercase sloppy ok. not gg wp stack."
					: "round was a draw. one lazy pub shrug line — bored energy ok.";
			emit(nullptr, ctx, false, "round_outcome");
			if (event == EVENT_TERRORISTS_WIN)
				emitRoundEndReactions("terrorists won the round", "T");
			else if (event == EVENT_CTS_WIN)
				emitRoundEndReactions("cts won the round", "CT");
		}
		break;

	case EVENT_VIP_ESCAPED:
		if (gpGlobals->time - s_lastVipEvent > 5.0f)
		{
			s_lastVipEvent = gpGlobals->time;
			emit(nullptr,
				"vip got away (as map). one short pub reaction — lowercase ok. not announcer.",
				false, "vip_escaped");
		}
		break;

	case EVENT_VIP_ASSASSINATED:
		if (gpGlobals->time - s_lastVipEvent > 5.0f)
		{
			s_lastVipEvent = gpGlobals->time;
			emit(nullptr,
				"vip got dropped (as map). one messy pub line — not polished fragmovie.",
				false, "vip_assassinated");
		}
		break;

	case EVENT_HOSTAGE_RESCUED:
		if (gpGlobals->time - s_lastHostageRescued > 3.5f)
		{
			s_lastHostageRescued = gpGlobals->time;
			CBasePlayer *rescuer = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			emit(rescuer && rescuer->IsBot() ? rescuer : nullptr,
				"a hostage got rescued. one casual pub line — lowercase ok. not mission briefing.",
				false, "hostage_rescued");
		}
		break;

	case EVENT_ALL_HOSTAGES_RESCUED:
		if (gpGlobals->time - s_lastHostageAll > 5.0f)
		{
			s_lastHostageAll = gpGlobals->time;
			emit(nullptr,
				"all hostages out (cs map). one short pub reaction — relief or disappointment ok, no roasting.",
				false, "hostage_all_rescued");
			emitRoundEndReactions("ct rescued all hostages", "CT");
		}
		break;

	case EVENT_HOSTAGE_KILLED:
		if (gpGlobals->time - s_lastHostageKilled > 4.0f)
		{
			s_lastHostageKilled = gpGlobals->time;
			CBasePlayer *killer = (pOther && pOther->IsPlayer()) ? static_cast<CBasePlayer *>(pOther) : nullptr;
			emit(killer && killer->IsBot() ? killer : nullptr,
				"someone killed a hostage (bad look). one short pub line — shock or blame energy sloppy ok.",
				false, "hostage_killed");
		}
		break;

	case EVENT_HOSTAGE_DAMAGED:
		if (gpGlobals->time - s_lastHostageDamaged > 14.0f)
		{
			s_lastHostageDamaged = gpGlobals->time;
			CBasePlayer *attacker = (pOther && pOther->IsPlayer()) ? static_cast<CBasePlayer *>(pOther) : nullptr;
			emit(attacker && attacker->IsBot() ? attacker : nullptr,
				"hostage took damage. one annoyed pub mutter — dont write a paragraph.",
				false, "hostage_damaged");
		}
		break;

	case EVENT_HE_GRENADE_EXPLODED:
	case EVENT_FLASHBANG_GRENADE_EXPLODED:
	case EVENT_SMOKE_GRENADE_EXPLODED:
		if (gpGlobals->time - s_lastGrenadeExploded > 12.0f)
		{
			s_lastGrenadeExploded = gpGlobals->time;
			CBasePlayer *thrower = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			const char *kind =
				(event == EVENT_HE_GRENADE_EXPLODED) ? "he nade popped" : (event == EVENT_FLASHBANG_GRENADE_EXPLODED) ? "flash popped" : "smoke popped";
			char ctx[200];
			Q_snprintf(ctx, sizeof(ctx),
				"%s nearby. one short pub line about it — lowercase messy ok. not play by play.", kind);
			emit(thrower && thrower->IsBot() ? thrower : nullptr, ctx, false, "grenade_exploded");
		}
		break;

	case EVENT_PLAYER_BLINDED_BY_FLASHBANG:
		if (gpGlobals->time - s_lastFlashBlinded > 9.0f)
		{
			s_lastFlashBlinded = gpGlobals->time;
			CBasePlayer *flashed = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			emit(flashed && flashed->IsBot() ? flashed : nullptr,
				"you got full white / flashed hard. one salty pub fragment — blind af etc. lowercase.",
				false, "player_flashed");
		}
		break;

	case EVENT_NEW_MATCH:
	case EVENT_GAME_COMMENCE:
		if (gpGlobals->time - s_lastGameCommence > 45.0f)
		{
			s_lastGameCommence = gpGlobals->time;
			ResetKillHistory();
			emit(nullptr,
				"match / game is starting up. one lazy pub line — half awake not esports intro.",
				false, "game_commence");
		}
		break;

	case EVENT_PLAYER_SPAWNED:
		if (gpGlobals->time - s_lastPlayerSpawnChat > 10.0f)
		{
			s_lastPlayerSpawnChat = gpGlobals->time;
			CBasePlayer *spawned = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
			if (spawned && spawned->IsBot())
			{
				const int si = spawned->entindex();
				if (si >= 1 && si <= MAX_CLIENTS)
				{
					s_lastKilledBy[si][0] = '\0';
					s_lastKilledByEnt[si] = 0;
					s_lastDeathLoc[si][0] = '\0';
					s_lastDeathType[si][0] = '\0';
					s_lastDeathWeapon[si][0] = '\0';
				}
				emit(spawned,
					"you just respawned / spawned in. one tiny pub thought — bored or hungry for buys. max a few words vibe.",
					false, "player_spawned");
			}
		}
		break;

	case EVENT_PLAYER_TOOK_DAMAGE:
	{
		// Friendly fire escalation: if the same teammate hits you repeatedly in a short window,
		// the victim bot complains quickly (AI), but only if they're not actively fighting.
		CBasePlayer *victim = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
		CBasePlayer *attacker = (pOther && pOther->IsPlayer()) ? static_cast<CBasePlayer *>(pOther) : nullptr;
		if (!victim || !attacker || victim == attacker)
			break;
		if (!victim->IsBot())
			break;
		if (victim->m_iTeam != attacker->m_iTeam)
			break;
		if (victim->m_iTeam != CT && victim->m_iTeam != TERRORIST)
			break;

		static int s_lastFfAttacker[MAX_CLIENTS + 1]{};
		static int s_ffCount[MAX_CLIENTS + 1]{};
		static float s_lastFfTime[MAX_CLIENTS + 1]{};
		static float s_lastFfChat[MAX_CLIENTS + 1]{};

		const int v = victim->entindex();
		const int a = attacker->entindex();
		if (v < 1 || v > MAX_CLIENTS || a < 1 || a > MAX_CLIENTS || !gpGlobals)
			break;

		const float now = gpGlobals->time;
		const float window = 14.0f;
		if (s_lastFfAttacker[v] == a && (now - s_lastFfTime[v]) < window)
			s_ffCount[v] += 1;
		else
			s_ffCount[v] = 1;
		s_lastFfAttacker[v] = a;
		s_lastFfTime[v] = now;

		// Don't stop to type if we're actively fighting (feels dumb / gets the bot killed).
		CCSBot *me = static_cast<CCSBot *>(victim);
		const bool inFight = (me && me->IsAttacking());
		if (inFight)
			break;

		// Complain early: sometimes on the first FF, often by the second.
		const float minGap = (s_ffCount[v] >= 2) ? 3.5f : 6.0f;
		if ((now - s_lastFfChat[v]) < minGap)
			break;
		// Make it much less sluggish: by 2nd hit it should almost always complain (unless fighting).
		const float p = (s_ffCount[v] >= 2) ? 0.95f : 0.45f;
		if (!PassesSpeakProb(p))
			break;

		s_lastFfChat[v] = now;
		char ctx[220];
		Q_snprintf(ctx, sizeof(ctx),
			"you just got shot by your teammate %s again (friendly fire). one short angry pub line as warning — lowercase messy ok. no long rant.",
			STRING(attacker->pev->netname));
		emit(victim, ctx, false, "friendly_fire_repeat");
		break;
	}

	case EVENT_PLAYER_DIED:
	{
		CBasePlayer *victim = (pEntity && pEntity->IsPlayer()) ? static_cast<CBasePlayer *>(pEntity) : nullptr;
		CBasePlayer *killer = (pOther && pOther->IsPlayer()) ? static_cast<CBasePlayer *>(pOther) : nullptr;

		// Record victim death snapshot so later dead-bot BOT_STATE isn't full of "unknown".
		if (victim && gpGlobals)
		{
			const int vi = victim->entindex();
			if (vi >= 1 && vi <= MAX_CLIENTS)
			{
				s_lastKilledBy[vi][0] = '\0';
				s_lastKilledByEnt[vi] = 0;
				s_lastDeathLoc[vi][0] = '\0';
				s_lastDeathType[vi][0] = '\0';
				s_lastDeathWeapon[vi][0] = '\0';

				if (killer && killer->IsPlayer())
				{
					Q_strlcpy(s_lastKilledBy[vi], STRING(killer->pev->netname), sizeof(s_lastKilledBy[0]));
					const int ki = killer->entindex();
					if (ki >= 1 && ki <= MAX_CLIENTS && ki != vi)
					{
						s_lastKilledByEnt[vi] = ki;
						if (s_killedByCount[vi][ki] < 65535)
							++s_killedByCount[vi][ki];
					}
				}

				const char *place = PlaceNameForOrigin(victim->pev->origin);
				Q_strlcpy(s_lastDeathLoc[vi], place ? place : "unknown", sizeof(s_lastDeathLoc[0]));

				const char *kt = KillTypeForDeath(victim, killer);
				Q_strlcpy(s_lastDeathType[vi], kt, sizeof(s_lastDeathType[0]));

				Q_strlcpy(s_lastDeathWeapon[vi], ActiveWeaponEntityName(killer), sizeof(s_lastDeathWeapon[0]));
			}
		}

		static float s_lastKillSpectator = 0.0f;
		const float killGap = (killer && killer->IsBot() && killer->pev->deadflag != DEAD_NO) ? 0.75f : 2.5f;
		const float spectatorGap = 3.5f;

		const bool spectacularKill = killer && killer->IsPlayer() && killer->pev->deadflag == DEAD_NO
			&& (IsKnifeKill(killer) || IsClutchKillMoment(killer));

		// Alive killer bot: short frag comm (plain words; victim is dead — no live orders to them).
		if (gpGlobals->time - s_lastKill > killGap && killer && killer->IsBot() && killer->pev->deadflag == DEAD_NO)
		{
			s_lastKill = gpGlobals->time;
			const char *victimName = victim ? STRING(victim->pev->netname) : "";
			char ctx[220];
			if (victimName && *victimName)
				Q_snprintf(ctx, sizeof(ctx),
					"you dropped %s while YOU are still alive. lowercase messy ok. "
					"no nice frag templates. victim is dead no coaching. "
					"do NOT say your own position site nades or what you are doing (no mid/b stuck no flash etc).",
					victimName);
			else
				Q_strlcpy(ctx, "you got a kill while alive. one lazy line. no announcer phrases. no self sitrep about where you are.", sizeof(ctx));
			emit(killer, ctx, spectacularKill, "kill_alive_killer");
		}

		// Victim bot: react to own death (often delayed by queue — still feels alive in chat).
		if (victim && victim->IsBot() && victim->pev->deadflag != DEAD_NO)
		{
			// Roll chance: victims often complain; headshot/knife gets extra likelihood.
			float p = AiTiming::kVictimDeathChatProb;
			if (killer && IsKnifeKill(killer))
				p = Q_max(p, 0.80f);
			else if (victim->m_LastHitGroup == HITGROUP_HEAD)
				p = Q_max(p, 0.75f);
			if (PassesSpeakProb(p))
			{
			const char *killerName = (killer && killer->IsPlayer()) ? STRING(killer->pev->netname) : "";
			char ctx[192];
			if (killerName && *killerName)
			{
				const char *kt = KillTypeForDeath(victim, killer);
				Q_snprintf(ctx, sizeof(ctx),
					"you just died to %s (%s). one salty half line — damn, wtf, rip, whatever. lowercase.",
					killerName, kt);
			}
			else
				Q_strlcpy(ctx, "you died. one annoyed fragment. lowercase. no nice try.", sizeof(ctx));
			emit(victim, ctx, spectacularKill, "kill_victim");
			}
		}

		bool queuedTeamDeathReact = false;

		// Teammate reacts to the death (dead-only, same team) for quick pub banter.
		if (victim && victim->IsPlayer() && (victim->m_iTeam == CT || victim->m_iTeam == TERRORIST))
		{
			static float s_lastTeamDeathReact = 0.0f;
			if (gpGlobals->time - s_lastTeamDeathReact > 2.6f && PassesSpeakProb(0.40f))
			{
				CBasePlayer *reactor = nullptr;
				for (int tries = 0; tries < 8 && !reactor; ++tries)
				{
					CBasePlayer *cand = PickRandomDeadBotExcluding(victim, killer);
					if (cand && cand->m_iTeam == victim->m_iTeam)
						reactor = cand;
				}
				if (reactor)
				{
					s_lastTeamDeathReact = gpGlobals->time;
					const char *vn = STRING(victim->pev->netname);
					const char *kn = (killer && killer->IsPlayer()) ? STRING(killer->pev->netname) : "someone";
					const char *kt = KillTypeForDeath(victim, killer);
					const char *kw = ActiveWeaponEntityName(killer);
					char ctx[220];
					Q_snprintf(ctx, sizeof(ctx),
						"your teammate %s just died to %s (%s, %s). one short dead pub reaction. Mild disappointment or surprise is ok. Avoid noob/trash insults unless repeat death facts support tilt. use at least one name. do not invent weapon.",
						(vn && *vn) ? vn : "teammate",
						(kn && *kn) ? kn : "enemy",
						kt,
						kw);
					emit(reactor, ctx, spectacularKill, "kill_teammate_react");
					queuedTeamDeathReact = true;
				}
			}
		}

		// Another dead bot spectating: fallback comment. Skip if a teammate reaction already queued for this death.
		if (!queuedTeamDeathReact && gpGlobals->time - s_lastKillSpectator > spectatorGap)
		{
			CBasePlayer *spec = PickRandomDeadBotExcluding(victim, killer);
			if (spec)
			{
				s_lastKillSpectator = gpGlobals->time;
				const char *vn = victim ? STRING(victim->pev->netname) : "someone";
				const char *kn = killer ? STRING(killer->pev->netname) : "someone";
				const char *kt = KillTypeForDeath(victim, killer);
				const char *kw = ActiveWeaponEntityName(killer);
				char ctx[220];
				Q_snprintf(ctx, sizeof(ctx),
					"you are dead spectating. %s died to %s (%s, %s). one lazy dead chat mumble. no coaching, no nice frag energy, do not invent weapon.",
					(vn && *vn) ? vn : "teammate",
					(kn && *kn) ? kn : "enemy",
					kt,
					kw);
				emit(spec, ctx, spectacularKill, "kill_dead_spectator");
			}
		}
		break;
	}

	default:
		break;
	}

	// Delivered asynchronously via AiChat::Frame()
}

bool AiChat::ShouldFreezeMovementForChat(int entindex)
{
	if (entindex < 1 || entindex > MAX_CLIENTS || !gpGlobals)
		return false;
	if (gpGlobals->time >= s_chatMoveFreezeUntil[entindex])
		return false;
	CBasePlayer *p = UTIL_PlayerByIndex(entindex);
	if (!p || !p->IsBot())
		return false;
	if (p->pev->deadflag != DEAD_NO)
		return false;
	return true;
}

