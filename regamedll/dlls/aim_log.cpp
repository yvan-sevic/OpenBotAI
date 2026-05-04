#include "precompiled.h"

#include "game_shared/bot/bot.h"
#include "aim_log.h"
#include "weapons.h"

extern cvar_t sv_aim_log_enable;
extern cvar_t sv_aim_log_interval;
extern cvar_t sv_roam_aim_log_enable;
extern cvar_t sv_grenade_throw_log_enable;

namespace
{
	const char *kBotLogPath = "aim_bot_events.csv";
	const char *kHumanLogPath = "aim_human_events.csv";
	const char *kRoamBotLogPath = "roam_aim_bot_events.csv";
	const char *kRoamHumanLogPath = "roam_aim_human_events.csv";
	const char *kGrenadeHumanLogPath = "grenade_human_events.csv";
	const char *kGrenadeBotLogPath = "grenade_bot_events.csv";
	const char *kCombatDiagBotLogPath = "aim_bot_combat_diag.csv";

	void BuildGameDirFilePath(char *path, size_t pathLen, const char *fileName)
	{
		char gameDir[MAX_PATH];
		GET_GAME_DIR(gameDir);
		Q_snprintf(path, pathLen, "%s/%s", gameDir, fileName);
	}

	FILE *s_botLog = nullptr;
	FILE *s_humanLog = nullptr;
	FILE *s_roamBotLog = nullptr;
	FILE *s_roamHumanLog = nullptr;
	FILE *s_grenadeHumanLog = nullptr;
	FILE *s_grenadeBotLog = nullptr;
	float s_nextBotSample[MAX_CLIENTS + 1] = { 0.0f };
	float s_nextHumanSample[MAX_CLIENTS + 1] = { 0.0f };
	float s_nextRoamBotSample[MAX_CLIENTS + 1] = { 0.0f };
	float s_nextRoamHumanSample[MAX_CLIENTS + 1] = { 0.0f };
	float s_lastRoamYaw[MAX_CLIENTS + 1] = { 0.0f };
	float s_lastRoamPitch[MAX_CLIENTS + 1] = { 0.0f };
	float s_lastRoamTime[MAX_CLIENTS + 1] = { 0.0f };
	bool s_hasLastRoamAngles[MAX_CLIENTS + 1] = { false };
	float s_lastGrenadeThrowLog[MAX_CLIENTS + 1] = { 0.0f };
	float s_lastBotGrenadeThrowLog[MAX_CLIENTS + 1] = { 0.0f };
	float s_nextCombatDiagSample[MAX_CLIENTS + 1] = { 0.0f };

	const char *TeamShort(TeamName team)
	{
		switch (team)
		{
		case CT: return "CT";
		case TERRORIST: return "T";
		case SPECTATOR: return "SPEC";
		default: return "UNK";
		}
	}

	const char *ActiveWeaponName(CBasePlayer *player)
	{
		if (!player || !player->m_pActiveItem)
			return "unknown";

		CBasePlayerWeapon *weapon = static_cast<CBasePlayerWeapon *>(player->m_pActiveItem);
		if (!weapon)
			return "unknown";

		WeaponInfoStruct *info = GetWeaponInfo(weapon->m_iId);
		if (!info || !info->entityName || !info->entityName[0])
			return "unknown";

		return info->entityName;
	}

	void CsvSafe(FILE *fp, const char *text)
	{
		if (!fp || !text)
			return;

		for (const char *p = text; *p; ++p)
		{
			const char c = *p;
			if (c == ',' || c == '\n' || c == '\r' || c == '"')
				fputc('_', fp);
			else
				fputc(c, fp);
		}
	}

	FILE *OpenLog(FILE *&fp, const char *fileName)
	{
		if (fp)
			return fp;

		char path[MAX_PATH];
		BuildGameDirFilePath(path, sizeof(path), fileName);

		fp = fopen(path, "a");
		if (!fp)
			return nullptr;

		fseek(fp, 0, SEEK_END);
		if (ftell(fp) == 0)
		{
			fprintf(fp,
				"time,map,entindex,kind,event,name,team,weapon,buttons,speed,"
				"origin_x,origin_y,origin_z,eye_x,eye_y,eye_z,"
				"view_pitch,view_yaw,view_roll,punch_pitch,punch_yaw,punch_roll,"
				"aim_x,aim_y,aim_z,hit_ent,hit_name,target_ent,target_name\n");
			fflush(fp);
		}

		return fp;
	}

	FILE *OpenGrenadeHumanLog()
	{
		if (s_grenadeHumanLog)
			return s_grenadeHumanLog;

		char path[MAX_PATH];
		BuildGameDirFilePath(path, sizeof(path), kGrenadeHumanLogPath);

		s_grenadeHumanLog = fopen(path, "a");
		if (!s_grenadeHumanLog)
			return nullptr;

		fseek(s_grenadeHumanLog, 0, SEEK_END);
		if (ftell(s_grenadeHumanLog) == 0)
		{
			fprintf(s_grenadeHumanLog,
				"time,map,entindex,name,team,grenade_type,grenade_items_in_inventory,weapon_entity,"
				"origin_x,origin_y,origin_z,eye_x,eye_y,eye_z,pitch,yaw,roll,"
				"forward_x,forward_y,forward_z,trace_x,trace_y,trace_z\n");
			fflush(s_grenadeHumanLog);
		}

		return s_grenadeHumanLog;
	}

	FILE *OpenCombatDiagLog()
	{
		static FILE *fp = nullptr;
		if (fp)
			return fp;

		char path[MAX_PATH];
		BuildGameDirFilePath(path, sizeof(path), kCombatDiagBotLogPath);

		fp = fopen(path, "a");
		if (!fp)
			return nullptr;

		fseek(fp, 0, SEEK_END);
		if (ftell(fp) == 0)
		{
			fprintf(fp,
				"time,map,bot_ent,bot_name,enemy_ent,weapon_id,weapon,"
				"enemy_vis,aim_at_enemy,phase,conf,motor_scale,acq_burst,acq_age,"
				"yaw_err_pre,pitch_err_pre,yaw_vel,pitch_vel,yaw_acc,pitch_acc,micro_snap,yaw_snap_mode,"
				"d_yaw,d_pitch,jitter,aim_off_mag,recoil_mag,dist_enemy,fire_intent,refuse_cone,sniper_vm_thresh,"
				"aim_cone_eval,aim_dot,req_aim_dot,aim_dot_margin,aim_cone_err_deg,aim_cone_tol_deg,aim_cone_ratio,"
				"aim_cone_pass,did_fire,motor_dt,yaw_vel_pre,pitch_vel_pre,yaw_acc_raw,pitch_acc_raw,yaw_acc_clm,pitch_acc_clm,"
				"yaw_vel_post_int,pitch_vel_post_int,yaw_step_raw,pitch_step_raw,yaw_step_applied,pitch_step_applied,"
				"yaw_vel_fin,pitch_vel_fin,yaw_err_post,pitch_err_post,zero_yaw_r,zero_pitch_r,direct_wr_motor,motor_dt_inv\n");
			fflush(fp);
		}

		return fp;
	}

	FILE *OpenRoamLog(FILE *&fp, const char *fileName)
	{
		if (fp)
			return fp;

		char path[MAX_PATH];
		BuildGameDirFilePath(path, sizeof(path), fileName);

		fp = fopen(path, "a");
		if (!fp)
			return nullptr;

		fseek(fp, 0, SEEK_END);
		if (ftell(fp) == 0)
		{
			fprintf(fp,
				"time,map,entindex,kind,state,name,team,weapon,buttons,speed,"
				"origin_x,origin_y,origin_z,view_pitch,view_yaw,"
				"delta_pitch,delta_yaw,pitch_speed,yaw_speed,aim_x,aim_y,aim_z,hit_ent,hit_name\n");
			fflush(fp);
		}

		return fp;
	}

	FILE *OpenGrenadeBotLog()
	{
		if (s_grenadeBotLog)
			return s_grenadeBotLog;

		char path[MAX_PATH];
		BuildGameDirFilePath(path, sizeof(path), kGrenadeBotLogPath);

		s_grenadeBotLog = fopen(path, "a");
		if (!s_grenadeBotLog)
			return nullptr;

		fseek(s_grenadeBotLog, 0, SEEK_END);
		if (ftell(s_grenadeBotLog) == 0)
		{
			fprintf(s_grenadeBotLog,
				"time,map,entindex,name,team,grenade_type,grenade_items_in_inventory,weapon_entity,"
				"origin_x,origin_y,origin_z,eye_x,eye_y,eye_z,pitch,yaw,roll,"
				"forward_x,forward_y,forward_z,trace_x,trace_y,trace_z,"
				"target_x,target_y,target_z,used_manual_spot,manual_spot_distance,manual_spot_index,manual_spot_name\n");
			fflush(s_grenadeBotLog);
		}

		return s_grenadeBotLog;
	}

	bool ShouldSample(CBasePlayer *player, float nextSample[], bool firing)
	{
		if (!player || !gpGlobals || sv_aim_log_enable.value <= 0.0f)
			return false;
		if (!player->IsAlive())
			return false;

		const int ent = player->entindex();
		if (ent < 1 || ent > MAX_CLIENTS)
			return false;

		float interval = sv_aim_log_interval.value;
		if (interval <= 0.0f)
			interval = 0.10f;

		// Keep sprays readable without logging every engine frame forever.
		if (firing)
			interval = Q_min(interval, 0.035f);

		if (gpGlobals->time < nextSample[ent])
			return false;

		nextSample[ent] = gpGlobals->time + interval;
		return true;
	}

	bool ShouldRoamSample(CBasePlayer *player, float nextSample[])
	{
		if (!player || !gpGlobals || sv_roam_aim_log_enable.value <= 0.0f)
			return false;
		if (!player->IsAlive() || (player->pev->button & IN_ATTACK))
			return false;

		const int ent = player->entindex();
		if (ent < 1 || ent > MAX_CLIENTS)
			return false;

		const float speed = player->pev->velocity.Make2D().Length();
		if (speed < 35.0f)
			return false;

		float interval = sv_aim_log_interval.value;
		if (interval <= 0.0f)
			interval = 0.10f;

		if (gpGlobals->time < nextSample[ent])
			return false;

		nextSample[ent] = gpGlobals->time + interval;
		return true;
	}

	Vector TraceAimPoint(CBasePlayer *player, CBaseEntity **hitEntity)
	{
		if (hitEntity)
			*hitEntity = nullptr;

		const Vector eye = player->GetGunPosition();
		UTIL_MakeVectors(player->pev->v_angle + player->pev->punchangle);
		const Vector end = eye + gpGlobals->v_forward * 8192.0f;

		TraceResult tr;
		UTIL_TraceLine(eye, end, dont_ignore_monsters, player->edict(), &tr);
		if (hitEntity && tr.pHit)
			*hitEntity = CBaseEntity::Instance(tr.pHit);

		return tr.vecEndPos;
	}

	void WriteSample(FILE *fp, CBasePlayer *player, const char *kind, const char *eventName, const Vector &aimPoint, CBaseEntity *hitEntity, CBaseEntity *targetEntity)
	{
		if (!fp || !player || !player->pev || !gpGlobals)
			return;

		const char *mapName = (gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
		const char *playerName = STRING(player->pev->netname);
		const Vector eye = player->GetGunPosition();
		const float speed = player->pev->velocity.Make2D().Length();

		fprintf(fp, "%.3f,", gpGlobals->time);
		CsvSafe(fp, mapName);
		fprintf(fp, ",%d,%s,%s,", player->entindex(), kind, eventName ? eventName : "unknown");
		CsvSafe(fp, playerName && *playerName ? playerName : "unknown");
		fprintf(fp, ",%s,", TeamShort(player->m_iTeam));
		CsvSafe(fp, ActiveWeaponName(player));
		fprintf(fp, ",%d,%.2f,", int(player->pev->button), speed);
		fprintf(fp, "%.2f,%.2f,%.2f,", player->pev->origin.x, player->pev->origin.y, player->pev->origin.z);
		fprintf(fp, "%.2f,%.2f,%.2f,", eye.x, eye.y, eye.z);
		fprintf(fp, "%.3f,%.3f,%.3f,", player->pev->v_angle.x, player->pev->v_angle.y, player->pev->v_angle.z);
		fprintf(fp, "%.3f,%.3f,%.3f,", player->pev->punchangle.x, player->pev->punchangle.y, player->pev->punchangle.z);
		fprintf(fp, "%.2f,%.2f,%.2f,", aimPoint.x, aimPoint.y, aimPoint.z);

		fprintf(fp, "%d,", hitEntity ? hitEntity->entindex() : 0);
		CsvSafe(fp, (hitEntity && hitEntity->IsPlayer() && hitEntity->pev) ? STRING(hitEntity->pev->netname) : "");
		fprintf(fp, ",%d,", targetEntity ? targetEntity->entindex() : 0);
		CsvSafe(fp, (targetEntity && targetEntity->IsPlayer() && targetEntity->pev) ? STRING(targetEntity->pev->netname) : "");
		fprintf(fp, "\n");
		fflush(fp);
	}

	void WriteRoamSample(FILE *fp, CBasePlayer *player, const char *kind, const char *stateName, const Vector &aimPoint, CBaseEntity *hitEntity)
	{
		if (!fp || !player || !player->pev || !gpGlobals)
			return;

		const int ent = player->entindex();
		if (ent < 1 || ent > MAX_CLIENTS)
			return;

		float deltaYaw = 0.0f;
		float deltaPitch = 0.0f;
		float yawSpeed = 0.0f;
		float pitchSpeed = 0.0f;
		if (s_hasLastRoamAngles[ent])
		{
			const float dt = Q_max(0.001f, gpGlobals->time - s_lastRoamTime[ent]);
			deltaYaw = NormalizeAngle(player->pev->v_angle.y - s_lastRoamYaw[ent]);
			deltaPitch = NormalizeAngle(player->pev->v_angle.x - s_lastRoamPitch[ent]);
			yawSpeed = deltaYaw / dt;
			pitchSpeed = deltaPitch / dt;
		}

		s_lastRoamYaw[ent] = player->pev->v_angle.y;
		s_lastRoamPitch[ent] = player->pev->v_angle.x;
		s_lastRoamTime[ent] = gpGlobals->time;
		s_hasLastRoamAngles[ent] = true;

		const char *mapName = (gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";
		const char *playerName = STRING(player->pev->netname);
		const float speed = player->pev->velocity.Make2D().Length();

		fprintf(fp, "%.3f,", gpGlobals->time);
		CsvSafe(fp, mapName);
		fprintf(fp, ",%d,%s,", ent, kind);
		CsvSafe(fp, stateName ? stateName : "roaming");
		fprintf(fp, ",");
		CsvSafe(fp, playerName && *playerName ? playerName : "unknown");
		fprintf(fp, ",%s,", TeamShort(player->m_iTeam));
		CsvSafe(fp, ActiveWeaponName(player));
		fprintf(fp, ",%d,%.2f,", int(player->pev->button), speed);
		fprintf(fp, "%.2f,%.2f,%.2f,", player->pev->origin.x, player->pev->origin.y, player->pev->origin.z);
		fprintf(fp, "%.3f,%.3f,", player->pev->v_angle.x, player->pev->v_angle.y);
		fprintf(fp, "%.3f,%.3f,%.3f,%.3f,", deltaPitch, deltaYaw, pitchSpeed, yawSpeed);
		fprintf(fp, "%.2f,%.2f,%.2f,", aimPoint.x, aimPoint.y, aimPoint.z);
		fprintf(fp, "%d,", hitEntity ? hitEntity->entindex() : 0);
		CsvSafe(fp, (hitEntity && hitEntity->IsPlayer() && hitEntity->pev) ? STRING(hitEntity->pev->netname) : "");
		fprintf(fp, "\n");
		fflush(fp);
	}
}

void AimLog::RecordHumanSample(CBasePlayer *player)
{
	if (!player || player->IsBot())
		return;

	const bool firing = (player->pev->button & IN_ATTACK) != 0;
	if (!ShouldSample(player, s_nextHumanSample, firing))
		return;

	CBaseEntity *hitEntity = nullptr;
	const Vector aimPoint = TraceAimPoint(player, &hitEntity);
	FILE *fp = OpenLog(s_humanLog, kHumanLogPath);
	WriteSample(fp, player, "human", firing ? "firing" : "roaming", aimPoint, hitEntity, nullptr);
}

void AimLog::RecordBotSample(CBasePlayer *bot, const Vector &botAimSpot, bool hasBotAimSpot, CBaseEntity *target)
{
	if (!bot || !bot->IsBot())
		return;

	const bool firing = (bot->pev->button & IN_ATTACK) != 0;
	if (!ShouldSample(bot, s_nextBotSample, firing))
		return;

	CBaseEntity *hitEntity = nullptr;
	Vector aimPoint = botAimSpot;
	if (!hasBotAimSpot)
		aimPoint = TraceAimPoint(bot, &hitEntity);

	FILE *fp = OpenLog(s_botLog, kBotLogPath);
	WriteSample(fp, bot, "bot", firing ? "firing" : "roaming", aimPoint, hitEntity, target);
}

void AimLog::RecordHumanRoamSample(CBasePlayer *player)
{
	if (!player || player->IsBot())
		return;
	if (!ShouldRoamSample(player, s_nextRoamHumanSample))
		return;

	CBaseEntity *hitEntity = nullptr;
	const Vector aimPoint = TraceAimPoint(player, &hitEntity);
	FILE *fp = OpenRoamLog(s_roamHumanLog, kRoamHumanLogPath);
	WriteRoamSample(fp, player, "human", "roaming", aimPoint, hitEntity);
}

void AimLog::RecordBotRoamSample(CBasePlayer *bot, const char *stateName)
{
	if (!bot || !bot->IsBot())
		return;
	if (!ShouldRoamSample(bot, s_nextRoamBotSample))
		return;

	CBaseEntity *hitEntity = nullptr;
	const Vector aimPoint = TraceAimPoint(bot, &hitEntity);
	FILE *fp = OpenRoamLog(s_roamBotLog, kRoamBotLogPath);
	WriteRoamSample(fp, bot, "bot", stateName ? stateName : "roaming", aimPoint, hitEntity);
}

void AimLog::RecordHumanGrenadeThrowIfAny(CBasePlayer *player)
{
	if (!player || player->IsBot() || !gpGlobals || sv_grenade_throw_log_enable.value <= 0.0f)
		return;
	if (!player->IsAlive() || !player->pev)
		return;

	// Match button semantics with the rest of the player code: CBasePlayer::m_afButtonPressed is set in
	// PreThink from (m_afButtonLast ^ pev->button) & pev->button. pev->oldbuttons is not updated for
	// players here, so relying on it never produced a fire "edge" and the CSV never opened.
	const bool attackPressedThisFrame = (player->m_afButtonPressed & IN_ATTACK) != 0;
	if (!attackPressedThisFrame)
		return;

	CBasePlayerWeapon *weapon = player->m_pActiveItem ? static_cast<CBasePlayerWeapon *>(player->m_pActiveItem) : nullptr;
	if (!weapon)
		return;

	const int wid = weapon->m_iId;
	if (wid != WEAPON_HEGRENADE && wid != WEAPON_FLASHBANG && wid != WEAPON_SMOKEGRENADE)
		return;

	const int ent = player->entindex();
	if (ent < 1 || ent > MAX_CLIENTS)
		return;

	if (gpGlobals->time - s_lastGrenadeThrowLog[ent] < 0.45f)
		return;
	s_lastGrenadeThrowLog[ent] = gpGlobals->time;

	int grenadeInventory = 0;
	for (CBasePlayerItem *item = player->m_rgpPlayerItems[GRENADE_SLOT]; item; item = item->m_pNext)
		grenadeInventory++;

	const char *grenadeType = "he";
	if (wid == WEAPON_FLASHBANG)
		grenadeType = "flash";
	else if (wid == WEAPON_SMOKEGRENADE)
		grenadeType = "smoke";

	WeaponInfoStruct *info = GetWeaponInfo(wid);
	const char *weaponEntity = (info && info->entityName && info->entityName[0]) ? info->entityName : "unknown";

	FILE *fpGrenade = OpenGrenadeHumanLog();
	if (!fpGrenade)
		return;

	const Vector eye = player->GetGunPosition();
	UTIL_MakeVectors(player->pev->v_angle + player->pev->punchangle);
	Vector forward = gpGlobals->v_forward;
	const Vector traceEnd = eye + forward * 8192.0f;

	TraceResult tr;
	UTIL_TraceLine(eye, traceEnd, dont_ignore_monsters, player->edict(), &tr);
	const Vector hit = tr.vecEndPos;

	const char *mapName = gpGlobals->mapname ? STRING(gpGlobals->mapname) : "";
	const char *playerName = STRING(player->pev->netname);

	fprintf(s_grenadeHumanLog, "%.3f,", gpGlobals->time);
	CsvSafe(s_grenadeHumanLog, mapName);
	fprintf(s_grenadeHumanLog, ",%d,", ent);
	CsvSafe(s_grenadeHumanLog, playerName && *playerName ? playerName : "unknown");
	fprintf(s_grenadeHumanLog, ",");
	switch (player->m_iTeam)
	{
	case CT: fprintf(s_grenadeHumanLog, "CT"); break;
	case TERRORIST: fprintf(s_grenadeHumanLog, "T"); break;
	case SPECTATOR: fprintf(s_grenadeHumanLog, "SPEC"); break;
	default: fprintf(s_grenadeHumanLog, "UNK"); break;
	}
	fprintf(s_grenadeHumanLog, ",%s,%d,", grenadeType, grenadeInventory);
	CsvSafe(s_grenadeHumanLog, weaponEntity);
	fprintf(s_grenadeHumanLog, ",%.2f,%.2f,%.2f,", player->pev->origin.x, player->pev->origin.y, player->pev->origin.z);
	fprintf(s_grenadeHumanLog, "%.2f,%.2f,%.2f,", eye.x, eye.y, eye.z);
	fprintf(s_grenadeHumanLog, "%.3f,%.3f,%.3f,", player->pev->v_angle.x, player->pev->v_angle.y, player->pev->v_angle.z);
	fprintf(s_grenadeHumanLog, "%.4f,%.4f,%.4f,", forward.x, forward.y, forward.z);
	fprintf(s_grenadeHumanLog, "%.2f,%.2f,%.2f\n", hit.x, hit.y, hit.z);
	fflush(s_grenadeHumanLog);
}

void AimLog::RecordBotCombatAimDiag(CBasePlayer *bot, const AimCombatDiagRecord &rec)
{
	if (!bot || !bot->IsBot() || !bot->pev || !gpGlobals)
		return;

	CBot *cbot = static_cast<CBot *>(bot);
	const bool firing = (cbot->GetPendingBotButtonFlags() & IN_ATTACK) != 0;
	if (!ShouldSample(bot, s_nextCombatDiagSample, firing))
		return;

	FILE *fp = OpenCombatDiagLog();
	if (!fp)
		return;

	const char *mapName = (gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "";

	fprintf(fp, "%.3f,", rec.time);
	CsvSafe(fp, mapName);
	fprintf(fp, ",%d,", rec.botEnt);
	CsvSafe(fp, rec.botName && rec.botName[0] ? rec.botName : "unknown");
	fprintf(fp, ",%d,%d,", rec.enemyEnt, rec.weaponId);
	CsvSafe(fp, rec.weaponName ? rec.weaponName : "unknown");
	fprintf(fp, ",%d,%d,%d,%.4f,%.4f,%d,%.4f,", rec.enemyVisible, rec.aimingAtEnemy, rec.combatPhase, rec.confidence, rec.motorScale,
		rec.acquisitionBurst, rec.acquireAge);
	fprintf(fp, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,", rec.yawErrBeforeMotor, rec.pitchErrBeforeMotor, rec.yawVel, rec.pitchVel,
		rec.yawAccel, rec.pitchAccel, rec.microYawSnap, rec.yawSnapMode);
	fprintf(fp, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,", rec.deltaYaw, rec.deltaPitch, rec.jitterMag, rec.aimOffsetMag, rec.recoilMag,
		rec.distEnemy, rec.firedAttackIntent, rec.refusedAimCone, rec.sniperViewMotionThresh);
	fprintf(fp, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,"
		"%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%d\n",
		rec.aimConeEvaluated,
		rec.aimDot, rec.requiredAimDot, rec.aimDotMargin,
		rec.aimConeErrorDeg, rec.aimConeToleranceDeg, rec.aimConeRatio,
		rec.aimConePass, rec.didFire,
		rec.motorDt, rec.yawVelPre, rec.pitchVelPre,
		rec.yawAccelRaw, rec.pitchAccelRaw, rec.yawAccelClamped, rec.pitchAccelClamped,
		rec.yawVelPostIntegrate, rec.pitchVelPostIntegrate,
		rec.yawStepRaw, rec.pitchStepRaw, rec.yawStepApplied, rec.pitchStepApplied,
		rec.yawVelFinal, rec.pitchVelFinal, rec.yawErrPost, rec.pitchErrPost,
		rec.zeroedYawVelReason, rec.zeroedPitchVelReason, rec.directAngleWriteReason, rec.motorDtInvalid);
	fflush(fp);
}

const char *AimLog::GetActiveWeaponEntityName(CBasePlayer *player)
{
	return ActiveWeaponName(player);
}

void AimLog::RecordBotGrenadeThrow(CBasePlayer *bot, const Vector *targetPosition, bool usedManualSpot, float distanceFromManualSpot, int manualSpotIndex, const char *manualSpotName)
{
	if (!bot || !bot->IsBot() || !gpGlobals || sv_grenade_throw_log_enable.value <= 0.0f)
		return;
	if (!bot->IsAlive() || !bot->pev)
		return;

	const int ent = bot->entindex();
	if (ent < 1 || ent > MAX_CLIENTS)
		return;

	// Throttle logging to avoid spam from same bot
	if (gpGlobals->time - s_lastBotGrenadeThrowLog[ent] < 0.45f)
		return;
	s_lastBotGrenadeThrowLog[ent] = gpGlobals->time;

	CBasePlayerWeapon *weapon = bot->m_pActiveItem ? static_cast<CBasePlayerWeapon *>(bot->m_pActiveItem) : nullptr;
	if (!weapon)
		return;

	const int wid = weapon->m_iId;
	if (wid != WEAPON_HEGRENADE && wid != WEAPON_FLASHBANG && wid != WEAPON_SMOKEGRENADE)
		return;

	int grenadeInventory = 0;
	for (CBasePlayerItem *item = bot->m_rgpPlayerItems[GRENADE_SLOT]; item; item = item->m_pNext)
		grenadeInventory++;

	const char *grenadeType = "he";
	if (wid == WEAPON_FLASHBANG)
		grenadeType = "flash";
	else if (wid == WEAPON_SMOKEGRENADE)
		grenadeType = "smoke";

	WeaponInfoStruct *info = GetWeaponInfo(wid);
	const char *weaponEntity = (info && info->entityName && info->entityName[0]) ? info->entityName : "unknown";

	FILE *fpGrenade = OpenGrenadeBotLog();
	if (!fpGrenade)
		return;

	const Vector eye = bot->GetGunPosition();
	UTIL_MakeVectors(bot->pev->v_angle + bot->pev->punchangle);
	Vector forward = gpGlobals->v_forward;
	const Vector traceEnd = eye + forward * 8192.0f;

	TraceResult tr;
	UTIL_TraceLine(eye, traceEnd, dont_ignore_monsters, bot->edict(), &tr);
	const Vector hit = tr.vecEndPos;

	const char *mapName = gpGlobals->mapname ? STRING(gpGlobals->mapname) : "";
	const char *botName = STRING(bot->pev->netname);

	// Write CSV line
	fprintf(s_grenadeBotLog, "%.3f,", gpGlobals->time);
	CsvSafe(s_grenadeBotLog, mapName);
	fprintf(s_grenadeBotLog, ",%d,", ent);
	CsvSafe(s_grenadeBotLog, botName && *botName ? botName : "unknown");
	fprintf(s_grenadeBotLog, ",");
	switch (bot->m_iTeam)
	{
	case CT: fprintf(s_grenadeBotLog, "CT"); break;
	case TERRORIST: fprintf(s_grenadeBotLog, "T"); break;
	case SPECTATOR: fprintf(s_grenadeBotLog, "SPEC"); break;
	default: fprintf(s_grenadeBotLog, "UNK"); break;
	}
	fprintf(s_grenadeBotLog, ",%s,%d,", grenadeType, grenadeInventory);
	CsvSafe(s_grenadeBotLog, weaponEntity);
	fprintf(s_grenadeBotLog, ",%.2f,%.2f,%.2f,", bot->pev->origin.x, bot->pev->origin.y, bot->pev->origin.z);
	fprintf(s_grenadeBotLog, "%.2f,%.2f,%.2f,", eye.x, eye.y, eye.z);
	fprintf(s_grenadeBotLog, "%.3f,%.3f,%.3f,", bot->pev->v_angle.x, bot->pev->v_angle.y, bot->pev->v_angle.z);
	fprintf(s_grenadeBotLog, "%.4f,%.4f,%.4f,", forward.x, forward.y, forward.z);
	fprintf(s_grenadeBotLog, "%.2f,%.2f,%.2f,", hit.x, hit.y, hit.z);
	
	// Target position (from manual spot or calculated)
	if (targetPosition)
		fprintf(s_grenadeBotLog, "%.2f,%.2f,%.2f,", targetPosition->x, targetPosition->y, targetPosition->z);
	else
		fprintf(s_grenadeBotLog, "0.0,0.0,0.0,");
	
	// Manual spot info
	fprintf(s_grenadeBotLog, "%s,", usedManualSpot ? "yes" : "no");
	fprintf(s_grenadeBotLog, "%.2f,%d,", distanceFromManualSpot, manualSpotIndex);
	CsvSafe(s_grenadeBotLog, manualSpotName ? manualSpotName : "");
	fprintf(s_grenadeBotLog, "\n");
	fflush(s_grenadeBotLog);

	if (wid == WEAPON_HEGRENADE || wid == WEAPON_SMOKEGRENADE)
	{
		const char *teamName = (bot->m_iTeam == CT) ? "CT" : ((bot->m_iTeam == TERRORIST) ? "T" : "UNK");
		CONSOLE_ECHO("[BotGrenade] %s %s threw %s", teamName, botName && *botName ? botName : "unknown", grenadeType);
		if (usedManualSpot && manualSpotName && manualSpotName[0])
			CONSOLE_ECHO(" from spot '%s'", manualSpotName);
		CONSOLE_ECHO("\n");
	}
}
