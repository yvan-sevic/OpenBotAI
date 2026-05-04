#include "precompiled.h"
#include "bot/cs_bot_grenade_spots.h"

#include <ctype.h>
#include <vector>

extern cvar_t bot_grenade_spots_debug;
extern cvar_t bot_grenade_he_danger_fallback;

namespace BotGrenadeSpots
{
	enum SpotKind
	{
		KIND_ANY = 0,
		KIND_SMOKE,
		KIND_FLASH,
		KIND_HE
	};

	struct ManualSpot
	{
		Vector from;
		Vector target;
		SpotKind kind;
		int teamUse;
		char name[48];
	};

	static std::vector<ManualSpot> s_spots;
	static char s_mapName[64];
	static bool s_loaded = false;

	static const char *KindName(SpotKind k)
	{
		switch (k)
		{
		case KIND_SMOKE: return "smoke";
		case KIND_FLASH: return "flash";
		case KIND_HE: return "he";
		default: return "any";
		}
	}

	static SpotKind KindFromArg(const char *a)
	{
		if (!a || !a[0])
			return KIND_ANY;
		if (!Q_stricmp(a, "smoke") || !Q_stricmp(a, "sgren"))
			return KIND_SMOKE;
		if (!Q_stricmp(a, "flash") || !Q_stricmp(a, "fb"))
			return KIND_FLASH;
		if (!Q_stricmp(a, "he") || !Q_stricmp(a, "hegren"))
			return KIND_HE;
		return KIND_ANY;
	}

	static int TeamFromArg(const char *a)
	{
		if (!a || !Q_stricmp(a, "any"))
			return 0;
		if (!Q_stricmp(a, "CT") || !Q_stricmp(a, "ct"))
			return CT;
		if (!Q_stricmp(a, "T") || !Q_stricmp(a, "t") || !Q_stricmp(a, "terrorist"))
			return TERRORIST;
		return 0;
	}

	static const char *TeamJsonName(int t)
	{
		if (t == CT)
			return "CT";
		if (t == TERRORIST)
			return "T";
		return "any";
	}

	static bool KindMatchesGrenade(SpotKind k, int weaponId)
	{
		if (k == KIND_ANY)
			return true;
		if (k == KIND_SMOKE && weaponId == WEAPON_SMOKEGRENADE)
			return true;
		if (k == KIND_FLASH && weaponId == WEAPON_FLASHBANG)
			return true;
		if (k == KIND_HE && weaponId == WEAPON_HEGRENADE)
			return true;
		return false;
	}

	static bool TeamMatches(int spotTeam, int botTeam)
	{
		if (spotTeam == 0)
			return true;
		return spotTeam == botTeam;
	}

	static bool IsPriorityMidHeSpot(const ManualSpot &s)
	{
		return s.kind == KIND_HE &&
			(!Q_stricmp(s.name, "spawn_to_mid") ||
			 !Q_stricmp(s.name, "spawn_to_mid_ga_way") ||
			 !Q_stricmp(s.name, "CT_spawn_mid_door_1") ||
			 !Q_stricmp(s.name, "CT_spawn_mid_door_2") ||
			 Q_strstr(s.name, "mid"));
	}

	static bool IsOpeningMidPreshotSpot(const ManualSpot &s)
	{
		return s.kind == KIND_HE &&
			(!Q_stricmp(s.name, "spawn_to_mid") ||
			 !Q_stricmp(s.name, "spawn_to_mid_ga_way") ||
			 !Q_stricmp(s.name, "CT_spawn_mid_door_1") ||
			 !Q_stricmp(s.name, "CT_spawn_mid_door_2"));
	}

	static bool IsCoverSmokeSpot(const ManualSpot &s)
	{
		return s.kind == KIND_SMOKE && Q_strstr(s.name, "_cover");
	}

	static bool HasRecentTargetActivity(CCSBot *bot, const ManualSpot &s)
	{
		if (!bot || !gpGlobals)
			return false;

		if (bot->GetTimeSinceLastSawEnemy() < 4.0f &&
			(bot->GetLastKnownEnemyPosition() - s.target).Length2D() < 950.0f)
			return true;

		const Vector *noise = bot->GetNoisePosition();
		if (noise && bot->IsNoiseHeard() && bot->GetNoisePriority() >= PRIORITY_MEDIUM &&
			(*noise - s.target).Length2D() < 950.0f)
			return true;

		return false;
	}

	static bool HasTeamCheckedTargetArea(CCSBot *bot, const ManualSpot &s)
	{
		if (!bot || !gpGlobals || !TheCSBots() || s.kind != KIND_HE)
			return false;

		CNavArea *targetArea = TheNavAreaGrid.GetNearestNavArea(&s.target);
		if (!targetArea)
			return false;

		const float clearedTime = targetArea->GetClearedTimestamp(bot->m_iTeam - 1);
		return clearedTime >= TheCSBots()->GetRoundStartTime();
	}

	static bool IsSpotUsableNow(CCSBot *bot, const ManualSpot &s)
	{
		if (IsCoverSmokeSpot(s))
			return bot && bot->GetGameState() && bot->GetGameState()->IsBombPlanted();

		const bool hasTargetActivity = HasRecentTargetActivity(bot, s);

		if (s.kind == KIND_HE && HasTeamCheckedTargetArea(bot, s) && !hasTargetActivity)
			return false;

		if (!IsOpeningMidPreshotSpot(s))
			return true;

		const float elapsedRoundTime = TheCSBots() ? TheCSBots()->GetElapsedRoundTime() : 9999.0f;
		if (elapsedRoundTime <= 35.0f)
			return true;

		return hasTargetActivity;
	}

	static void BuildPath(char *path, size_t len)
	{
		const char *mapName = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
		char gameDir[MAX_PATH];
		GET_GAME_DIR(gameDir);
		Q_snprintf(path, len, "%s/maps/%s.grenade.json", gameDir, mapName);
	}

	static void JsonEscape(FILE *fp, const char *text)
	{
		for (const char *p = text ? text : ""; *p; ++p)
		{
			if (*p == '"' || *p == '\\')
				fputc('\\', fp);
			fputc(*p, fp);
		}
	}

	static const char *SkipWs(const char *p)
	{
		while (p && *p && isspace((unsigned char)*p))
			++p;
		return p;
	}

	static bool ExtractString(const char *objStart, const char *objEnd, const char *key, char *out, size_t outLen)
	{
		char needle[64];
		Q_snprintf(needle, sizeof(needle), "\"%s\"", key);
		const char *p = Q_strstr(objStart, needle);
		if (!p || p >= objEnd)
			return false;
		p = Q_strstr(p + Q_strlen(needle), ":");
		if (!p || p >= objEnd)
			return false;
		p = SkipWs(p + 1);
		if (!p || *p != '"' || p >= objEnd)
			return false;
		++p;
		size_t len = 0;
		while (*p && p < objEnd && *p != '"' && len + 1 < outLen)
		{
			if (*p == '\\' && p + 1 < objEnd)
				++p;
			out[len++] = *p++;
		}
		out[len] = '\0';
		return true;
	}

	static bool ExtractFloat(const char *objStart, const char *objEnd, const char *key, float *out)
	{
		char needle[64];
		Q_snprintf(needle, sizeof(needle), "\"%s\"", key);
		const char *p = Q_strstr(objStart, needle);
		if (!p || p >= objEnd)
			return false;
		p = Q_strstr(p + Q_strlen(needle), ":");
		if (!p || p >= objEnd)
			return false;
		p = SkipWs(p + 1);
		if (!p || p >= objEnd)
			return false;
		*out = (float)Q_atof(p);
		return true;
	}

	void LoadForCurrentMap()
	{
		s_spots.clear();
		s_loaded = true;
		const char *mapName = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
		Q_strlcpy(s_mapName, mapName, sizeof(s_mapName));

		char path[MAX_PATH];
		BuildPath(path, sizeof(path));

		FILE *fp = fopen(path, "rb");
		if (!fp)
			return;

		fseek(fp, 0, SEEK_END);
		long len = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if (len <= 0 || len > 1024 * 1024)
		{
			fclose(fp);
			return;
		}

		std::vector<char> data(len + 1);
		if (fread(data.data(), 1, len, fp) != (size_t)len)
		{
			fclose(fp);
			return;
		}
		fclose(fp);
		data[len] = '\0';

		const char *arr = Q_strstr(data.data(), "\"spots\"");
		if (!arr)
		{
			CONSOLE_ECHO("[GrenadeSpots] Loaded %s but found no 'spots' array\n", path);
			return;
		}
		const char *p = Q_strstr(arr, "[");
		if (!p)
		{
			CONSOLE_ECHO("[GrenadeSpots] Loaded %s but spots array malformed\n", path);
			return;
		}
		++p;

		while (*p)
		{
			const char *objStart = Q_strstr(p, "{");
			const char *arrayEnd = Q_strstr(p, "]");
			if (!objStart || (arrayEnd && arrayEnd < objStart))
				break;

			const char *objEnd = Q_strstr(objStart, "}");
			if (!objEnd)
				break;

			ManualSpot spot;
			Q_memset(&spot, 0, sizeof(spot));
			char typeStr[32] = "";
			char teamStr[32] = "";
			float fx, fy, fz, tx, ty, tz;

			if (ExtractFloat(objStart, objEnd, "from_x", &fx) &&
				ExtractFloat(objStart, objEnd, "from_y", &fy) &&
				ExtractFloat(objStart, objEnd, "from_z", &fz) &&
				ExtractFloat(objStart, objEnd, "target_x", &tx) &&
				ExtractFloat(objStart, objEnd, "target_y", &ty) &&
				ExtractFloat(objStart, objEnd, "target_z", &tz))
			{
				spot.from = Vector(fx, fy, fz);
				spot.target = Vector(tx, ty, tz);
				ExtractString(objStart, objEnd, "type", typeStr, sizeof(typeStr));
				ExtractString(objStart, objEnd, "team", teamStr, sizeof(teamStr));
				spot.kind = KindFromArg(typeStr);
				spot.teamUse = TeamFromArg(teamStr);
				if (!ExtractString(objStart, objEnd, "name", spot.name, sizeof(spot.name)))
					Q_strlcpy(spot.name, "spot", sizeof(spot.name));

				s_spots.push_back(spot);
			}

			p = objEnd + 1;
		}
		
		if (bot_grenade_spots_debug.value > 0.0f)
		{
			int heCount = 0, smokeCount = 0, flashCount = 0, anyCount = 0;
			int ctCount = 0, tCount = 0, anyTeamCount = 0;
			
			for (size_t i = 0; i < s_spots.size(); ++i)
			{
				const ManualSpot &s = s_spots[i];
				switch(s.kind) {
					case KIND_HE: heCount++; break;
					case KIND_SMOKE: smokeCount++; break;
					case KIND_FLASH: flashCount++; break;
					case KIND_ANY: anyCount++; break;
				}
				if (s.teamUse == CT) ctCount++;
				else if (s.teamUse == TERRORIST) tCount++;
				else anyTeamCount++;
			}
			
			CONSOLE_ECHO("[GrenadeSpots] Loaded %d spots from %s\n", (int)s_spots.size(), path);
			CONSOLE_ECHO("  HE: %d, Smoke: %d, Flash: %d, Any: %d\n", heCount, smokeCount, flashCount, anyCount);
			CONSOLE_ECHO("  CT: %d, T: %d, Any team: %d\n", ctCount, tCount, anyTeamCount);
		}
	}

	void Save()
	{
		if (!s_loaded)
			return;

		char path[MAX_PATH];
		BuildPath(path, sizeof(path));

		FILE *fp = fopen(path, "wb");
		if (!fp)
			return;

		fprintf(fp, "{\n  \"map\": \"");
		JsonEscape(fp, s_mapName);
		fprintf(fp, "\",\n  \"version\": 1,\n  \"spots\": [\n");

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const ManualSpot &s = s_spots[i];
			fprintf(fp, "    {\n");
			fprintf(fp, "      \"name\": \"");
			JsonEscape(fp, s.name);
			fprintf(fp, "\",\n");
			fprintf(fp, "      \"type\": \"%s\",\n", KindName(s.kind));
			fprintf(fp, "      \"team\": \"%s\",\n", TeamJsonName(s.teamUse));
			fprintf(fp, "      \"from_x\": %.1f,\n      \"from_y\": %.1f,\n      \"from_z\": %.1f,\n", s.from.x, s.from.y, s.from.z);
			fprintf(fp, "      \"target_x\": %.1f,\n      \"target_y\": %.1f,\n      \"target_z\": %.1f\n", s.target.x, s.target.y, s.target.z);
			fprintf(fp, "    }%s\n", (i + 1 < (int)s_spots.size()) ? "," : "");
		}

		fprintf(fp, "  ]\n}\n");
		fclose(fp);
	}

	void ServerCommandAddSpot()
	{
		if (CMD_ARGC() < 2)
		{
			CONSOLE_ECHO("usage: bot_grenade_spot <smoke|flash|he|any> [name] [team]\n");
			CONSOLE_ECHO("  optional: name label, team = CT | T | any (default: your team)\n");
			CONSOLE_ECHO("Stand where you throw, aim at landing/pop, then run this (listen server).\n");
			return;
		}
		if (!gpGlobals || FStringNull(gpGlobals->mapname))
		{
			CONSOLE_ECHO("No active map.\n");
			return;
		}

		CBasePlayer *player = UTIL_GetLocalPlayer();
		if (!player || !player->IsAlive())
		{
			CONSOLE_ECHO("Requires an alive local player (start a listen server).\n");
			return;
		}

		if (!s_loaded)
			LoadForCurrentMap();

		ManualSpot spot;
		Q_memset(&spot, 0, sizeof(spot));
		spot.kind = KindFromArg(CMD_ARGV(1));

		if (CMD_ARGC() >= 4)
		{
			Q_strlcpy(spot.name, CMD_ARGV(2), sizeof(spot.name));
			spot.teamUse = TeamFromArg(CMD_ARGV(3));
		}
		else if (CMD_ARGC() >= 3 && CMD_ARGV(2) && CMD_ARGV(2)[0])
		{
			Q_strlcpy(spot.name, CMD_ARGV(2), sizeof(spot.name));
			spot.teamUse = player->m_iTeam;
		}
		else
		{
			Q_snprintf(spot.name, sizeof(spot.name), "spot_%03d", (int)s_spots.size() + 1);
			spot.teamUse = player->m_iTeam;
		}

		spot.from = player->pev->origin;

		UTIL_MakeVectors(player->pev->v_angle);
		Vector eye = player->pev->origin + player->pev->view_ofs;
		Vector dst = eye + gpGlobals->v_forward * 8192.0f;

		TraceResult tr;
		UTIL_TraceLine(eye, dst, dont_ignore_monsters, dont_ignore_glass, ENT(player->pev), &tr);
		spot.target = tr.vecEndPos;
		spot.target.z += 4.0f;

		s_spots.push_back(spot);
		Save();

		CONSOLE_ECHO("Grenade spot '%s' (%s, team=%s) -> maps/%s.grenade.json (%d spots)\n",
			spot.name, KindName(spot.kind), TeamJsonName(spot.teamUse), STRING(gpGlobals->mapname), (int)s_spots.size());
		CONSOLE_ECHO("  from (%.0f, %.0f, %.0f) target (%.0f, %.0f, %.0f)\n",
			spot.from.x, spot.from.y, spot.from.z, spot.target.x, spot.target.y, spot.target.z);
	}

	bool HasAnyLineupForGrenadeAndTeam(int grenadeWeaponId, int botTeam)
	{
		if (!s_loaded || s_spots.empty())
			return false;

		for (size_t i = 0; i < s_spots.size(); ++i)
		{
			const ManualSpot &s = s_spots[i];
			if (!KindMatchesGrenade(s.kind, grenadeWeaponId))
				continue;
			if (!TeamMatches(s.teamUse, botTeam))
				continue;
			return true;
		}

		return false;
	}

	bool ShouldAttemptDangerHeFallback(CCSBot *bot)
	{
		if (!bot)
			return false;

		if (!HasAnyLineupForGrenadeAndTeam(WEAPON_HEGRENADE, bot->m_iTeam))
			return true;

		const float frac = bot_grenade_he_danger_fallback.value;
		if (frac <= 0.0f)
			return false;
		if (frac >= 1.0f)
			return true;

		return RANDOM_FLOAT(0.0f, 1.0f) <= frac;
	}

	bool IsNearManualThrowSpot(CCSBot *bot, int grenadeWeaponId, float normalRadius, float priorityRadius)
	{
		if (!bot || s_spots.empty())
			return false;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const ManualSpot &s = s_spots[i];
			if (!KindMatchesGrenade(s.kind, grenadeWeaponId))
				continue;
			if (!TeamMatches(s.teamUse, bot->m_iTeam))
				continue;
			if (!IsSpotUsableNow(bot, s))
				continue;

			const float radius = (s.kind == KIND_HE || IsPriorityMidHeSpot(s)) ? priorityRadius : normalRadius;
			if ((bot->pev->origin - s.from).Length2D() <= radius)
				return true;
		}

		return false;
	}

	bool FindManualThrowTarget(CCSBot *bot, int grenadeWeaponId, Vector *targetOut, FoundSpotInfo *foundSpotOut)
	{
		if (!bot || !targetOut || s_spots.empty())
			return false;

		const float skill = TheCSBots()->GetEffectiveSkill(bot);
		float bestScore = -999999.0f;
		int bestIdx = -1;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const ManualSpot &s = s_spots[i];
			if (!KindMatchesGrenade(s.kind, grenadeWeaponId))
				continue;
			if (!TeamMatches(s.teamUse, bot->m_iTeam))
				continue;
			if (!IsSpotUsableNow(bot, s))
				continue;

			const float dFrom = (bot->pev->origin - s.from).Length2D();
			float minFrom = 20.0f;
			float maxFrom = 295.0f;
			float minTargetDist = 80.0f;
			if (grenadeWeaponId == WEAPON_HEGRENADE)
			{
				minFrom = 8.0f;
				maxFrom = 160.0f;
				minTargetDist = 60.0f;
				if (IsPriorityMidHeSpot(s))
					maxFrom = 150.0f;
			}
			else if (grenadeWeaponId == WEAPON_SMOKEGRENADE)
			{
				minFrom = 8.0f;
				maxFrom = IsCoverSmokeSpot(s) ? 420.0f : 240.0f;
				minTargetDist = 40.0f;
			}
			if (dFrom < minFrom || dFrom > maxFrom)
				continue;

			const float d3 = (s.target - bot->pev->origin).Length();
			if (d3 < minTargetDist || d3 > 2400.0f)
				continue;

			float score = (grenadeWeaponId == WEAPON_HEGRENADE) ? 580.0f - dFrom * 0.55f : 410.0f - dFrom * 0.70f;
			score += skill * ((grenadeWeaponId == WEAPON_HEGRENADE) ? 90.0f : 60.0f);
			score += Q_max(0.0f, 220.0f - dFrom) * ((grenadeWeaponId == WEAPON_HEGRENADE) ? 0.55f : 0.35f);
			if (grenadeWeaponId == WEAPON_HEGRENADE && !HasTeamCheckedTargetArea(bot, s))
				score += 140.0f;
			if (grenadeWeaponId == WEAPON_SMOKEGRENADE)
			{
				if (IsCoverSmokeSpot(s))
					score += (bot && bot->GetGameState() && bot->GetGameState()->IsBombPlanted()) ? 180.0f : -500.0f;
				else if (bot && bot->GetGameState() && bot->GetGameState()->IsBombPlanted())
					score -= 80.0f;
			}
			if (grenadeWeaponId == WEAPON_HEGRENADE && IsPriorityMidHeSpot(s))
			{
				score += 90.0f;
				score += Q_max(0.0f, 220.0f - dFrom) * 0.20f;
			}
			score -= (d3 > 680.0f ? d3 - 680.0f : 680.0f - d3) * 0.02f;
			score += RANDOM_FLOAT(-6.0f, 6.0f);

			if (score > bestScore)
			{
				bestScore = score;
				bestIdx = i;
			}
		}

		if (bestIdx < 0)
			return false;

		*targetOut = s_spots[bestIdx].target;
		
		if (foundSpotOut)
		{
			foundSpotOut->spotIndex = bestIdx;
			foundSpotOut->distanceFromSpot = (bot->pev->origin - s_spots[bestIdx].from).Length2D();
			foundSpotOut->spotName = s_spots[bestIdx].name;
		}
		
		if (bot_grenade_spots_debug.value > 0.0f)
		{
			CONSOLE_ECHO("[BOT %s] Found HE spot #%d '%s' (distance from recorded: %.1f units)\n",
				STRING(bot->pev->netname), bestIdx, s_spots[bestIdx].name, 
				(bot->pev->origin - s_spots[bestIdx].from).Length2D());
		}
		
		return true;
	}

	void DrawDebug()
	{
		if (bot_grenade_spots_debug.value <= 0.0f || !s_loaded)
			return;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const ManualSpot &s = s_spots[i];
			Vector a = s.from + Vector(0, 0, 12.0f);
			Vector b = s.target + Vector(0, 0, 8.0f);
			UTIL_DrawBeamPoints(a, b, 1, 50, 255, 180);
		}
	}
}
