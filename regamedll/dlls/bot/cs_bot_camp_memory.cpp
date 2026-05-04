#include "precompiled.h"
#include "bot/cs_bot_camp_memory.h"

#include <ctype.h>

extern cvar_t bot_camp_memory_enable;
extern cvar_t bot_camp_memory_learn;
extern cvar_t bot_camp_memory_debug;

namespace BotCampMemory
{
	struct CampSpot
	{
		Vector pos;
		int team;
		int deaths;
		int damageEvents;
		int pathFails;
		float lastBad;
		float badWeight;
		unsigned int place;
	};

	struct BotCampState
	{
		Vector spot;
		float timestamp;
		bool valid;
	};

	const int kMaxCampSpots = 256;
	const float kClusterRange = 180.0f;
	const float kAutoSaveInterval = 20.0f;
	const float kAvoidBadWeight = 5.0f;

	static std::vector<CampSpot> s_spots;
	static BotCampState s_botCamp[MAX_CLIENTS];
	static char s_mapName[64];
	static bool s_loaded = false;
	static bool s_dirty = false;
	static float s_nextSaveTime = 0.0f;

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
		return bot_camp_memory_enable.value > 0.0f;
	}

	static bool CanLearn()
	{
		return bot_camp_memory_learn.value > 0.0f;
	}

	static void BuildPath(char *path, size_t len)
	{
		const char *mapName = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
		char gameDir[MAX_PATH];
		GET_GAME_DIR(gameDir);
		Q_snprintf(path, len, "%s/maps/%s.camp.json", gameDir, mapName);
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

	static void AddBadSpot(int team, const Vector &pos, float weight, bool death, bool damage, bool pathFail)
	{
		if (team != CT && team != TERRORIST)
			return;

		int best = -1;
		float bestDistSq = kClusterRange * kClusterRange;
		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			if (s_spots[i].team != team)
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
			CampSpot &spot = s_spots[best];
			const float oldWeight = Q_max(spot.badWeight, 1.0f);
			const float blend = weight / (oldWeight + weight);
			spot.pos = spot.pos * (1.0f - blend) + pos * blend;
			spot.badWeight = Q_min(9999.0f, spot.badWeight + weight);
			spot.lastBad = gpGlobals ? gpGlobals->time : 0.0f;
			spot.place = FindPlace(spot.pos);
			if (death)
				spot.deaths++;
			if (damage)
				spot.damageEvents++;
			if (pathFail)
				spot.pathFails++;
			MarkDirty();
			return;
		}

		if ((int)s_spots.size() >= kMaxCampSpots)
		{
			int weakest = 0;
			for (int i = 1; i < (int)s_spots.size(); ++i)
			{
				if (s_spots[i].badWeight < s_spots[weakest].badWeight)
					weakest = i;
			}
			s_spots.erase(s_spots.begin() + weakest);
		}

		CampSpot spot;
		Q_memset(&spot, 0, sizeof(spot));
		spot.pos = pos;
		spot.team = team;
		spot.deaths = death ? 1 : 0;
		spot.damageEvents = damage ? 1 : 0;
		spot.pathFails = pathFail ? 1 : 0;
		spot.lastBad = gpGlobals ? gpGlobals->time : 0.0f;
		spot.badWeight = weight;
		spot.place = FindPlace(pos);
		s_spots.push_back(spot);
		MarkDirty();
	}

	static bool GetRecentCampSpot(CCSBot *bot, Vector *spot)
	{
		if (!bot || !spot)
			return false;

		int index = bot->entindex() - 1;
		if (index < 0 || index >= MAX_CLIENTS || !s_botCamp[index].valid)
			return false;

		const float recentLeaveTime = 4.0f;
		if (!bot->IsAtHidingSpot() && gpGlobals && gpGlobals->time - s_botCamp[index].timestamp > recentLeaveTime)
			return false;

		*spot = s_botCamp[index].spot;
		return true;
	}

	void LoadForCurrentMap()
	{
		s_spots.clear();
		Q_memset(s_botCamp, 0, sizeof(s_botCamp));
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

		const char *spots = Q_strstr(data.data(), "\"bad_spots\"");
		if (!spots)
			return;

		const char *p = Q_strstr(spots, "[");
		if (!p)
			return;
		++p;

		while (*p && s_spots.size() < kMaxCampSpots)
		{
			const char *objStart = Q_strstr(p, "{");
			const char *arrayEnd = Q_strstr(p, "]");
			if (!objStart || (arrayEnd && arrayEnd < objStart))
				break;

			const char *objEnd = Q_strstr(objStart, "}");
			if (!objEnd)
				break;

			CampSpot spot;
			Q_memset(&spot, 0, sizeof(spot));
			char team[32] = "";
			if (ExtractFloat(objStart, objEnd, "x", &spot.pos.x) &&
				ExtractFloat(objStart, objEnd, "y", &spot.pos.y) &&
				ExtractFloat(objStart, objEnd, "z", &spot.pos.z) &&
				ExtractString(objStart, objEnd, "team", team, sizeof(team)))
			{
				spot.team = TeamFromName(team);
				ExtractInt(objStart, objEnd, "deaths", &spot.deaths);
				ExtractInt(objStart, objEnd, "damage_events", &spot.damageEvents);
				ExtractInt(objStart, objEnd, "path_fails", &spot.pathFails);
				ExtractFloat(objStart, objEnd, "last_bad", &spot.lastBad);
				ExtractFloat(objStart, objEnd, "bad_weight", &spot.badWeight);
				spot.place = FindPlace(spot.pos);
				if ((spot.team == CT || spot.team == TERRORIST) && spot.badWeight > 0.0f)
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
		fprintf(fp, "\",\n  \"version\": 1,\n  \"bad_spots\": [\n");

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const CampSpot &spot = s_spots[i];
			const char *placeName = spot.place ? TheBotPhrases->IDToName(spot.place) : nullptr;
			if (!placeName && spot.place)
				placeName = TheNavAreaGrid.IDToName(spot.place);

			fprintf(fp, "    {\n");
			fprintf(fp, "      \"x\": %.1f,\n      \"y\": %.1f,\n      \"z\": %.1f,\n", spot.pos.x, spot.pos.y, spot.pos.z);
			fprintf(fp, "      \"place\": \"");
			JsonEscape(fp, placeName ? placeName : "");
			fprintf(fp, "\",\n");
			fprintf(fp, "      \"team\": \"%s\",\n", TeamName(spot.team));
			fprintf(fp, "      \"deaths\": %d,\n      \"damage_events\": %d,\n      \"path_fails\": %d,\n", spot.deaths, spot.damageEvents, spot.pathFails);
			fprintf(fp, "      \"last_bad\": %.1f,\n      \"bad_weight\": %.1f\n", spot.lastBad, spot.badWeight);
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

	void DrawDebug()
	{
		if (!s_loaded || bot_camp_memory_debug.value <= 0.0f)
			return;

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			Vector top = s_spots[i].pos + Vector(0, 0, 80.0f);
			UTIL_DrawBeamPoints(s_spots[i].pos, top, 1, 255, 0, 180);
		}
	}

	void RecordCampStart(CCSBot *bot, const Vector &spot)
	{
		if (!s_loaded || !bot)
			return;

		int index = bot->entindex() - 1;
		if (index < 0 || index >= MAX_CLIENTS)
			return;

		s_botCamp[index].spot = spot;
		s_botCamp[index].timestamp = gpGlobals ? gpGlobals->time : 0.0f;
		s_botCamp[index].valid = true;
	}

	void RecordDamage(CBasePlayer *victim, CBasePlayer *attacker)
	{
		if (!s_loaded || !CanLearn() || !victim || !victim->IsBot() || !attacker || !attacker->IsPlayer())
			return;
		if (victim == attacker || victim->m_iTeam == attacker->m_iTeam)
			return;

		CCSBot *bot = static_cast<CCSBot *>(victim);
		Vector spot;
		if (GetRecentCampSpot(bot, &spot))
			AddBadSpot(bot->m_iTeam, spot, 2.0f, false, true, false);
	}

	void RecordDeath(CBasePlayer *victim, CBasePlayer *killer)
	{
		if (!s_loaded || !CanLearn() || !victim || !victim->IsBot() || !killer || !killer->IsPlayer())
			return;
		if (victim == killer || victim->m_iTeam == killer->m_iTeam)
			return;

		CCSBot *bot = static_cast<CCSBot *>(victim);
		Vector spot;
		if (GetRecentCampSpot(bot, &spot))
			AddBadSpot(bot->m_iTeam, spot, 6.0f, true, false, false);
	}

	void RecordPathFail(CCSBot *bot, const Vector &spot)
	{
		if (!s_loaded || !CanLearn() || !bot)
			return;

		AddBadSpot(bot->m_iTeam, spot, 1.5f, false, false, true);
	}

	float GetBadCampWeight(CBaseEntity *ent, const Vector &spot)
	{
		if (!s_loaded || !IsEnabled() || !ent || !ent->IsPlayer())
			return 0.0f;

		CBasePlayer *player = static_cast<CBasePlayer *>(ent);
		const int team = player->m_iTeam;
		if (team != CT && team != TERRORIST)
			return 0.0f;

		float weight = 0.0f;
		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const CampSpot &bad = s_spots[i];
			if (bad.team != team)
				continue;

			if ((bad.pos - spot).IsLengthLessThan(kClusterRange))
				weight = Q_max(weight, bad.badWeight);
		}

		return weight;
	}

	bool IsBadCampSpot(CBaseEntity *ent, const Vector &spot)
	{
		return GetBadCampWeight(ent, spot) >= kAvoidBadWeight;
	}
}
