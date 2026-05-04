#include "precompiled.h"
#include "bot/cs_bot_camp_look_spots.h"
#include "util.h"

#include <ctype.h>
#include <vector>

namespace BotCampLookSpots
{
	struct CampLookSpot
	{
		Vector from;
		Vector look;
		int teamUse;
		char name[48];
	};

	// Spots are recorded with origin = feet and look = trace from (origin + view_ofs). Standing players
	// use ~VEC_VIEW over feet; hiding bots often crouch (lower eye), which makes the same world "look"
	// aim too high. Re-base the aim along the recorded view ray from the bot's actual eye.
	static Vector AimWorldForBotEye(const CampLookSpot &s, CCSBot *bot)
	{
		if (!bot)
			return s.look;
		const Vector eyeRef = s.from + VEC_VIEW;
		const Vector eyeNow = bot->GetEyePosition();
		Vector delta = s.look - eyeRef;
		const float dist = delta.Length();
		if (dist < 1.0f)
			return s.look;
		delta = delta * (1.0f / dist);
		return eyeNow + delta * dist;
	}

	// When true, caller should bias/sort aims toward dir (last enemy / heard noise). Without a hint,
	// path m_lookAheadAngle often points into the corner and must not steer camp picks.
	static bool PreferredWatchDir2D(CCSBot *bot, Vector2D *outDir)
	{
		if (!bot || !outDir)
			return false;

		const Vector eye = bot->GetEyePosition();

		if (bot->GetTimeSinceLastSawEnemy() < 18.0f)
		{
			Vector2D to(
				bot->GetLastKnownEnemyPosition().x - eye.x,
				bot->GetLastKnownEnemyPosition().y - eye.y);
			const float len = to.Length();
			if (len > 40.0f)
			{
				*outDir = to * (1.0f / len);
				return true;
			}
		}

		const Vector *noise = bot->GetNoisePosition();
		if (noise && bot->IsNoiseHeard())
		{
			Vector2D to(noise->x - eye.x, noise->y - eye.y);
			const float len = to.Length();
			if (len > 40.0f)
			{
				*outDir = to * (1.0f / len);
				return true;
			}
		}

		return false;
	}

	static float AimAlignmentDot(const CampLookSpot &s, CCSBot *bot, const Vector2D &pref)
	{
		if (!bot)
			return 0.0f;

		const Vector eye = bot->GetEyePosition();
		const Vector aim = AimWorldForBotEye(s, bot);
		Vector2D to(aim.x - eye.x, aim.y - eye.y);
		const float len = to.Length();
		if (len < 8.0f)
			return 0.0f;

		to = to * (1.0f / len);
		return to.x * pref.x + to.y * pref.y;
	}

	static std::vector<CampLookSpot> s_spots;
	static char s_mapName[64];
	static bool s_loaded = false;

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

	static const char *TeamJsonName(int team)
	{
		if (team == CT)
			return "CT";
		if (team == TERRORIST)
			return "T";
		return "any";
	}

	static void BuildPath(char *path, size_t len)
	{
		const char *mapName = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
		char gameDir[MAX_PATH];
		GET_GAME_DIR(gameDir);
		Q_snprintf(path, len, "%s/maps/%s.camp_look.json", gameDir, mapName);
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
		if (!p || p >= objEnd || *p != '"')
			return false;
		++p;

		size_t n = 0;
		while (*p && p < objEnd && *p != '"' && n + 1 < outLen)
		{
			if (*p == '\\' && p[1])
				++p;
			out[n++] = *p++;
		}
		out[n] = '\0';
		return n > 0;
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
			return;
		const char *p = Q_strstr(arr, "[");
		if (!p)
			return;
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

			CampLookSpot spot;
			Q_memset(&spot, 0, sizeof(spot));
			char teamStr[32] = "";
			float fx, fy, fz, lx, ly, lz;

			if (ExtractFloat(objStart, objEnd, "from_x", &fx) &&
				ExtractFloat(objStart, objEnd, "from_y", &fy) &&
				ExtractFloat(objStart, objEnd, "from_z", &fz) &&
				ExtractFloat(objStart, objEnd, "look_x", &lx) &&
				ExtractFloat(objStart, objEnd, "look_y", &ly) &&
				ExtractFloat(objStart, objEnd, "look_z", &lz))
			{
				spot.from = Vector(fx, fy, fz);
				spot.look = Vector(lx, ly, lz);
				ExtractString(objStart, objEnd, "team", teamStr, sizeof(teamStr));
				spot.teamUse = TeamFromArg(teamStr);
				// Existing camp look files were recorded from one side, but the angles are useful to any bot holding that position.
				spot.teamUse = 0;
				if (!ExtractString(objStart, objEnd, "name", spot.name, sizeof(spot.name)))
					Q_strlcpy(spot.name, "camp_look", sizeof(spot.name));
				s_spots.push_back(spot);
			}

			p = objEnd + 1;
		}
	}

	void Save()
	{
		if (!s_loaded || s_spots.empty())
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
			const CampLookSpot &s = s_spots[i];
			fprintf(fp, "    {\n");
			fprintf(fp, "      \"name\": \"");
			JsonEscape(fp, s.name);
			fprintf(fp, "\",\n");
			fprintf(fp, "      \"team\": \"%s\",\n", TeamJsonName(s.teamUse));
			fprintf(fp, "      \"from_x\": %.1f,\n      \"from_y\": %.1f,\n      \"from_z\": %.1f,\n", s.from.x, s.from.y, s.from.z);
			fprintf(fp, "      \"look_x\": %.1f,\n      \"look_y\": %.1f,\n      \"look_z\": %.1f\n", s.look.x, s.look.y, s.look.z);
			fprintf(fp, "    }%s\n", (i + 1 < (int)s_spots.size()) ? "," : "");
		}
		fprintf(fp, "  ]\n}\n");
		fclose(fp);
	}

	void ServerCommandAddSpot()
	{
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

		CampLookSpot spot;
		Q_memset(&spot, 0, sizeof(spot));
		if (CMD_ARGC() >= 2 && CMD_ARGV(1) && CMD_ARGV(1)[0])
			Q_strlcpy(spot.name, CMD_ARGV(1), sizeof(spot.name));
		else
			Q_snprintf(spot.name, sizeof(spot.name), "camp_look_%03d", (int)s_spots.size() + 1);

		if (CMD_ARGC() >= 3)
			spot.teamUse = TeamFromArg(CMD_ARGV(2));
		else
			spot.teamUse = 0;

		spot.from = player->pev->origin;

		UTIL_MakeVectors(player->pev->v_angle);
		Vector eye = player->pev->origin + player->pev->view_ofs;
		Vector dst = eye + gpGlobals->v_forward * 8192.0f;

		TraceResult tr;
		UTIL_TraceLine(eye, dst, dont_ignore_monsters, dont_ignore_glass, ENT(player->pev), &tr);
		spot.look = tr.vecEndPos;
		spot.look.z += 4.0f;

		s_spots.push_back(spot);
		Save();

		CONSOLE_ECHO("Camp look spot '%s' (team=%s) -> maps/%s.camp_look.json (%d spots)\n",
			spot.name, TeamJsonName(spot.teamUse), STRING(gpGlobals->mapname), (int)s_spots.size());
		CONSOLE_ECHO("  from (%.0f, %.0f, %.0f) look (%.0f, %.0f, %.0f)\n",
			spot.from.x, spot.from.y, spot.from.z, spot.look.x, spot.look.y, spot.look.z);
	}

	static void SortValidCampIndicesByPreferredAim(CCSBot *bot, int *valid, int nValid)
	{
		if (!bot || !valid || nValid <= 1)
			return;

		Vector2D pref;
		if (!PreferredWatchDir2D(bot, &pref))
			return;

		for (int i = 1; i < nValid; ++i)
		{
			const int keyIdx = valid[i];
			const float keyDot = AimAlignmentDot(s_spots[keyIdx], bot, pref);
			int j = i - 1;
			while (j >= 0)
			{
				const float d = AimAlignmentDot(s_spots[valid[j]], bot, pref);
				if (d >= keyDot)
					break;

				valid[j + 1] = valid[j];
				--j;
			}
			valid[j + 1] = keyIdx;
		}
	}

	bool FindLookTarget(CCSBot *bot, Vector *targetOut, const char **spotNameOut)
	{
		if (!bot || !targetOut || s_spots.empty())
			return false;

		float bestScore = -999999.0f;
		int bestIdx = -1;
		Vector2D pref{};
		const bool hasPrefDir = PreferredWatchDir2D(bot, &pref);

		for (int i = 0; i < (int)s_spots.size(); ++i)
		{
			const CampLookSpot &s = s_spots[i];
			if (s.teamUse != 0 && s.teamUse != bot->m_iTeam)
				continue;

			const float d = (bot->pev->origin - s.from).Length2D();
			if (d > 420.0f)
				continue;

			const Vector aimWorld = AimWorldForBotEye(s, bot);
			TraceResult tr;
			UTIL_TraceLine(bot->GetEyePosition(), aimWorld, ignore_monsters, ignore_glass, ENT(bot->pev), &tr);
			if (tr.flFraction < 0.35f)
				continue;

			float score = 420.0f - d;
			score += tr.flFraction * 180.0f;
			if (hasPrefDir)
				score += AimAlignmentDot(s, bot, pref) * 260.0f;
			if (score > bestScore)
			{
				bestScore = score;
				bestIdx = i;
			}
		}

		if (bestIdx < 0)
			return false;

		*targetOut = AimWorldForBotEye(s_spots[bestIdx], bot);
		if (spotNameOut)
			*spotNameOut = s_spots[bestIdx].name;
		return true;
	}

	bool FindLookTargetRotating(CCSBot *bot, Vector *targetOut, const char **spotNameOut, bool advanceCursor)
	{
		if (!bot || !targetOut || s_spots.empty())
			return false;

		int valid[128];
		int nValid = 0;
		for (int i = 0; i < static_cast<int>(s_spots.size()) && nValid < 128; ++i)
		{
			const CampLookSpot &s = s_spots[i];
			if (s.teamUse != 0 && s.teamUse != bot->m_iTeam)
				continue;

			const float d = (bot->pev->origin - s.from).Length2D();
			if (d > 420.0f)
				continue;

			const Vector aimWorld = AimWorldForBotEye(s, bot);
			TraceResult tr;
			UTIL_TraceLine(bot->GetEyePosition(), aimWorld, ignore_monsters, ignore_glass, ENT(bot->pev), &tr);
			if (tr.flFraction < 0.35f)
				continue;

			valid[nValid++] = i;
		}

		if (nValid <= 0)
			return false;

		SortValidCampIndicesByPreferredAim(bot, valid, nValid);

		const int pick = valid[bot->CampLookRotateSlot(nValid)];
		if (advanceCursor)
			bot->CampLookRotateAdvance();

		*targetOut = AimWorldForBotEye(s_spots[pick], bot);
		if (spotNameOut)
			*spotNameOut = s_spots[pick].name;
		return true;
	}
}
