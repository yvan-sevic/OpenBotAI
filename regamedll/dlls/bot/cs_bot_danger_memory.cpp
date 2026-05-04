#include "precompiled.h"
#include "bot/cs_bot_danger_memory.h"

#include <ctype.h>

extern cvar_t bot_danger_memory_enable;
extern cvar_t bot_danger_memory_learn;
extern cvar_t bot_danger_memory_debug;

namespace BotDangerMemory
{
	struct DangerSpot
	{
		Vector pos;
		int teamSeenBy;
		int sightings;
		int deaths;
		float lastSeen;
		float weight;
		unsigned int place;
	};

	const int kMaxDangerSpots = 256;
	const float kClusterRange = 220.0f;
	const float kAutoSaveInterval = 20.0f;
	const float kSightingInterval = 1.15f;

	static std::vector<DangerSpot> s_spots;
	static char s_mapName[64];
	static bool s_loaded = false;
	static bool s_dirty = false;
	static float s_nextSaveTime = 0.0f;
	static float s_lastSighting[MAX_CLIENTS][MAX_CLIENTS];
	static float s_lastNoise[MAX_CLIENTS];

	static const char *TeamName(int team)
	{
		return (team == CT) ? "CT" : ((team == TERRORIST) ? "T" : "UNKNOWN");
	}

	static int TeamFromName(const char *team)
	{
		if (!team)
			return UNASSIGNED;
		if (!Q_stricmp(team, "CT"))
			return CT;
		if (!Q_stricmp(team, "T") || !Q_stricmp(team, "TERRORIST"))
			return TERRORIST;
		return UNASSIGNED;
	}

	static bool IsEnabled()
	{
		return bot_danger_memory_enable.value > 0.0f;
	}

	static bool CanLearn()
	{
		return bot_danger_memory_learn.value > 0.0f;
	}

	static void BuildPath(char *path, size_t len)
	{
		const char *mapName = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
		char gameDir[MAX_PATH];
		GET_GAME_DIR(gameDir);
		Q_snprintf(path, len, "%s/maps/%s.danger.json", gameDir, mapName);
	}

	static void MarkDirty()
	{
		s_dirty = true;
		if (gpGlobals && s_nextSaveTime <= 0.0f)
			s_nextSaveTime = gpGlobals->time + kAutoSaveInterval;
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

	static bool ExtractInt(const char *objStart, const char *objEnd, const char *key, int *out)
	{
		float value = 0.0f;
		if (!ExtractFloat(objStart, objEnd, key, &value))
			return false;
		*out = (int)value;
		return true;
	}

	static unsigned int FindPlace(const Vector &pos)
	{
		CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&pos);
		return area ? area->GetPlace() : 0;
	}

	static void AddSpotForTeam(int teamSeenBy, const Vector &pos, float weight, bool death)
	{
		if (teamSeenBy != CT && teamSeenBy != TERRORIST)
			return;

		int best = -1;
		float bestDistSq = kClusterRange * kClusterRange;
		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			if (s_spots[i].teamSeenBy != teamSeenBy)
				continue;

			float distSq = (s_spots[i].pos - pos).LengthSquared();
			if (distSq < bestDistSq)
			{
				best = i;
				bestDistSq = distSq;
			}
		}

		if (best >= 0)
		{
			DangerSpot &spot = s_spots[best];
			const float oldWeight = Q_max(spot.weight, 1.0f);
			const float blend = weight / (oldWeight + weight);
			spot.pos = spot.pos * (1.0f - blend) + pos * blend;
			spot.sightings++;
			if (death)
				spot.deaths++;
			spot.weight = Q_min(9999.0f, spot.weight + weight);
			spot.lastSeen = gpGlobals ? gpGlobals->time : 0.0f;
			spot.place = FindPlace(spot.pos);
			MarkDirty();
			return;
		}

		if ((int)s_spots.size() >= kMaxDangerSpots)
		{
			int weakest = 0;
			for (int i = 1; i < (int)s_spots.size(); ++i)
			{
				if (s_spots[i].weight < s_spots[weakest].weight)
					weakest = i;
			}
			s_spots.erase(s_spots.begin() + weakest);
		}

		DangerSpot spot;
		spot.pos = pos;
		spot.teamSeenBy = teamSeenBy;
		spot.sightings = 1;
		spot.deaths = death ? 1 : 0;
		spot.lastSeen = gpGlobals ? gpGlobals->time : 0.0f;
		spot.weight = weight;
		spot.place = FindPlace(pos);
		s_spots.push_back(spot);
		MarkDirty();
	}

	void LoadForCurrentMap()
	{
		s_spots.clear();
		Q_memset(s_lastSighting, 0, sizeof(s_lastSighting));
		Q_memset(s_lastNoise, 0, sizeof(s_lastNoise));
		s_loaded = true;
		s_dirty = false;
		s_nextSaveTime = gpGlobals ? gpGlobals->time + kAutoSaveInterval : 0.0f;

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

		const char *spots = Q_strstr(data.data(), "\"spots\"");
		if (!spots)
			return;

		const char *p = Q_strstr(spots, "[");
		if (!p)
			return;
		++p;

		while (*p && s_spots.size() < kMaxDangerSpots)
		{
			const char *objStart = Q_strstr(p, "{");
			const char *arrayEnd = Q_strstr(p, "]");
			if (!objStart || (arrayEnd && arrayEnd < objStart))
				break;

			const char *objEnd = Q_strstr(objStart, "}");
			if (!objEnd)
				break;

			DangerSpot spot;
			Q_memset(&spot, 0, sizeof(spot));
			char team[32] = "";
			if (ExtractFloat(objStart, objEnd, "x", &spot.pos.x) &&
				ExtractFloat(objStart, objEnd, "y", &spot.pos.y) &&
				ExtractFloat(objStart, objEnd, "z", &spot.pos.z) &&
				ExtractString(objStart, objEnd, "team_seen_by", team, sizeof(team)))
			{
				spot.teamSeenBy = TeamFromName(team);
				ExtractInt(objStart, objEnd, "sightings", &spot.sightings);
				ExtractInt(objStart, objEnd, "deaths", &spot.deaths);
				ExtractFloat(objStart, objEnd, "last_seen", &spot.lastSeen);
				ExtractFloat(objStart, objEnd, "weight", &spot.weight);
				spot.place = FindPlace(spot.pos);
				if ((spot.teamSeenBy == CT || spot.teamSeenBy == TERRORIST) && spot.weight > 0.0f)
					s_spots.push_back(spot);
			}

			p = objEnd + 1;
		}
	}

	void Save()
	{
		if (!s_loaded || !s_dirty)
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
			const DangerSpot &spot = s_spots[i];
			const char *placeName = spot.place ? TheBotPhrases->IDToName(spot.place) : nullptr;
			if (!placeName && spot.place)
				placeName = TheNavAreaGrid.IDToName(spot.place);

			fprintf(fp, "    {\n");
			fprintf(fp, "      \"name\": \"auto_%03d\",\n", i + 1);
			fprintf(fp, "      \"x\": %.1f,\n      \"y\": %.1f,\n      \"z\": %.1f,\n", spot.pos.x, spot.pos.y, spot.pos.z);
			fprintf(fp, "      \"place\": \"");
			JsonEscape(fp, placeName ? placeName : "");
			fprintf(fp, "\",\n");
			fprintf(fp, "      \"team_seen_by\": \"%s\",\n", TeamName(spot.teamSeenBy));
			fprintf(fp, "      \"sightings\": %d,\n      \"deaths\": %d,\n", spot.sightings, spot.deaths);
			fprintf(fp, "      \"last_seen\": %.1f,\n      \"weight\": %.1f\n", spot.lastSeen, spot.weight);
			fprintf(fp, "    }%s\n", (i + 1 < (int)s_spots.size()) ? "," : "");
		}

		fprintf(fp, "  ]\n}\n");
		fclose(fp);
		s_dirty = false;
		s_nextSaveTime = gpGlobals ? gpGlobals->time + kAutoSaveInterval : 0.0f;
	}

	void MaybeAutoSave()
	{
		if (!s_dirty || !gpGlobals || gpGlobals->time < s_nextSaveTime)
			return;
		Save();
	}

	void RecordSighting(CCSBot *observer, CBasePlayer *enemy)
	{
		if (!s_loaded || !CanLearn() || !observer || !enemy || !enemy->IsPlayer())
			return;
		if (observer->m_iTeam == enemy->m_iTeam)
			return;

		int oi = observer->entindex() - 1;
		int ei = enemy->entindex() - 1;
		if (oi < 0 || oi >= MAX_CLIENTS || ei < 0 || ei >= MAX_CLIENTS)
			return;

		if (gpGlobals && gpGlobals->time - s_lastSighting[oi][ei] < kSightingInterval)
			return;
		s_lastSighting[oi][ei] = gpGlobals ? gpGlobals->time : 0.0f;

		AddSpotForTeam(observer->m_iTeam, enemy->pev->origin, 1.0f, false);
	}

	void RecordDamage(CBasePlayer *victim, CBasePlayer *attacker)
	{
		if (!s_loaded || !CanLearn() || !victim || !attacker || !victim->IsPlayer() || !attacker->IsPlayer())
			return;
		if (victim == attacker || victim->m_iTeam == attacker->m_iTeam)
			return;

		AddSpotForTeam(victim->m_iTeam, attacker->pev->origin, 4.0f, false);
	}

	void RecordDeath(CBasePlayer *victim, CBasePlayer *killer)
	{
		if (!s_loaded || !CanLearn() || !victim || !killer || !victim->IsPlayer() || !killer->IsPlayer())
			return;
		if (victim == killer || victim->m_iTeam == killer->m_iTeam)
			return;

		AddSpotForTeam(victim->m_iTeam, killer->pev->origin, 9.0f, true);
	}

	void RecordEnemyNoise(CBasePlayer *source)
	{
		if (!s_loaded || !CanLearn() || !source || !source->IsPlayer())
			return;

		int index = source->entindex() - 1;
		if (index < 0 || index >= MAX_CLIENTS)
			return;

		if (gpGlobals && gpGlobals->time - s_lastNoise[index] < 0.9f)
			return;
		s_lastNoise[index] = gpGlobals ? gpGlobals->time : 0.0f;

		if (source->m_iTeam == CT)
			AddSpotForTeam(TERRORIST, source->pev->origin, 0.35f, false);
		else if (source->m_iTeam == TERRORIST)
			AddSpotForTeam(CT, source->pev->origin, 0.35f, false);
	}

	bool FindBestLookSpot(CCSBot *bot, Vector *out)
	{
		if (!s_loaded || !IsEnabled() || !bot || !out || s_spots.empty())
			return false;

		const Vector eye = bot->pev->origin + bot->pev->view_ofs;
		float bestScore = -999999.0f;
		int best = -1;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const DangerSpot &spot = s_spots[i];
			if (spot.teamSeenBy != bot->m_iTeam)
				continue;
			if (bot->IsUsingSniperRifle() && spot.weight < 3.0f)
				continue;

			Vector target = spot.pos + Vector(0, 0, HalfHumanHeight);
			const float range = (target - bot->pev->origin).Length2D();
			if (range < 180.0f || range > 3200.0f)
				continue;

			TraceResult result;
			UTIL_TraceLine(eye, target, ignore_monsters, ignore_glass, ENT(bot->pev), &result);
			if (result.flFraction < 0.35f)
				continue;

			float score = spot.weight + spot.deaths * 3.0f + spot.sightings * 0.08f;
			score += result.flFraction * 40.0f;
			score -= range * 0.006f;
			score += RANDOM_FLOAT(-2.0f, 2.0f);

			if (score > bestScore)
			{
				bestScore = score;
				best = i;
			}
		}

		if (best < 0)
			return false;

		*out = s_spots[best].pos + Vector(0, 0, HalfHumanHeight);
		return true;
	}

	bool FindBestLookSpotNear(CCSBot *bot, const Vector &nearPos, float maxRange, Vector *out)
	{
		if (!s_loaded || !IsEnabled() || !bot || !out || s_spots.empty())
			return false;

		const Vector eye = bot->pev->origin + bot->pev->view_ofs;
		float bestScore = -999999.0f;
		int best = -1;
		const float maxRangeSq = maxRange * maxRange;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const DangerSpot &spot = s_spots[i];
			if (spot.teamSeenBy != bot->m_iTeam)
				continue;

			const float nearDistSq = (spot.pos - nearPos).LengthSquared();
			if (nearDistSq > maxRangeSq)
				continue;

			Vector target = spot.pos + Vector(0, 0, HalfHumanHeight);
			const float botRange = (target - bot->pev->origin).Length2D();
			if (botRange < 120.0f || botRange > 2800.0f)
				continue;

			TraceResult result;
			UTIL_TraceLine(eye, target, ignore_monsters, ignore_glass, ENT(bot->pev), &result);
			if (result.flFraction < 0.18f)
				continue;

			float score = spot.weight + spot.deaths * 4.0f + spot.sightings * 0.1f;
			score += result.flFraction * 32.0f;
			score -= Q_sqrt(nearDistSq) * 0.018f;
			score -= botRange * 0.004f;
			score += RANDOM_FLOAT(-1.5f, 1.5f);

			if (score > bestScore)
			{
				bestScore = score;
				best = i;
			}
		}

		if (best < 0)
			return false;

		*out = s_spots[best].pos + Vector(0, 0, HalfHumanHeight);
		return true;
	}

	bool FindBestHeGrenadeTargetFromMemory(CCSBot *bot, Vector *out)
	{
		// Uses learned/manual spots from maps/<map>.danger.json; does not require bot_danger_memory_enable
		// so HE intent still works when only the JSON exists.
		if (!s_loaded || !bot || !out || s_spots.empty())
			return false;

		UTIL_MakeVectors(bot->pev->v_angle);
		Vector forward = gpGlobals->v_forward;
		forward.z = 0.0f;
		if (forward.NormalizeInPlace() < 0.01f)
			forward = Vector(1, 0, 0);

		const Vector eye = bot->pev->origin + bot->pev->view_ofs;
		float bestScore = -999999.0f;
		int best = -1;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const DangerSpot &spot = s_spots[i];
			if (spot.teamSeenBy != bot->m_iTeam)
				continue;

			Vector target = spot.pos + Vector(0, 0, HalfHumanHeight);
			Vector toTarget = target - bot->pev->origin;
			const float range = toTarget.Length2D();
			if (range < 300.0f || range > 1700.0f)
				continue;

			Vector toTarget2D = toTarget;
			toTarget2D.z = 0.0f;
			if (toTarget2D.NormalizeInPlace() < 0.01f)
				continue;

			const float align = DotProduct(toTarget2D, forward);
			if (align < -0.25f)
				continue;

			TraceResult result;
			UTIL_TraceLine(eye, target, ignore_monsters, ignore_glass, ENT(bot->pev), &result);
			if (result.flFraction < 0.15f)
				continue;

			float score = spot.weight * 1.5f + spot.deaths * 6.0f + spot.sightings * 0.06f;
			score += align * 32.0f;
			score -= Q_fabs(range - 750.0f) * 0.0035f;
			score += RANDOM_FLOAT(-2.0f, 2.0f);

			if (score > bestScore)
			{
				bestScore = score;
				best = i;
			}
		}

		if (best < 0)
			return false;

		*out = s_spots[best].pos + Vector(0, 0, 12.0f);
		return true;
	}

	void DrawDebug()
	{
		if (!s_loaded || bot_danger_memory_debug.value <= 0.0f)
			return;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			Vector top = s_spots[i].pos + Vector(0, 0, 70.0f);
			if (s_spots[i].teamSeenBy == CT)
				UTIL_DrawBeamPoints(s_spots[i].pos, top, 1, 0, 80, 255);
			else
				UTIL_DrawBeamPoints(s_spots[i].pos, top, 1, 255, 80, 0);
		}
	}
}
