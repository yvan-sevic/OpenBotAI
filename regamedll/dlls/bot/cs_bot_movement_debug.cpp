#include "precompiled.h"
#include "bot/cs_bot_init.h"
#include "bot/cs_bot_movement_debug.h"

namespace
{
constexpr int kMoveDbgMaxSlots = 40;

static FILE *s_moveDbgLog = nullptr;
static char s_moveDbgLogPath[MAX_PATH];
static bool s_moveDbgConfigBannerWritten = false;

static void CloseMoveDebugLogFile()
{
	if (s_moveDbgLog)
	{
		fclose(s_moveDbgLog);
		s_moveDbgLog = nullptr;
	}
	s_moveDbgLogPath[0] = 0;
	s_moveDbgConfigBannerWritten = false;
}

static void WriteMovementDebugConfigBanner()
{
	if (!s_moveDbgLog || !gpGlobals)
		return;

	const char *fn = (cv_bot_move_debug_logfile.string && cv_bot_move_debug_logfile.string[0])
		? cv_bot_move_debug_logfile.string
		: "bot_move_debug.log";

	const char *watchedMode = "off";
	if (cv_bot_debug.value == 1.0f || cv_bot_debug.value == 3.0f)
		watchedMode = "spectated_bot_only";
	else if (cv_bot_debug.value == 2.0f || cv_bot_debug.value == 4.0f)
		watchedMode = "all_bots";

	int specIdx = 0;
	const char *specName = "none";
	CBasePlayer *loc = UTIL_GetLocalPlayer();
	if (loc && ((loc->pev->flags & FL_SPECTATOR) != 0 || loc->m_iTeam == SPECTATOR))
	{
		specIdx = loc->pev->iuser2;
		if (specIdx >= 1)
		{
			CBasePlayer *watched = UTIL_PlayerByIndex(specIdx);
			if (watched && watched->pev)
			{
				const char *nn = STRING(watched->pev->netname);
				if (nn && nn[0])
					specName = nn;
			}
		}
	}

	fprintf(s_moveDbgLog,
		"[movedbg-config] t=%.3f bot_debug=%.0f bot_move_debug_log=%.0f logfile=%s watched_mode=%s "
		"spectate_target_idx=%i spectate_target_name=%s hunt_noise_markers_file=yes file_gate=movement_log_plus_watched_bot\n",
		gpGlobals->time,
		cv_bot_debug.value,
		cv_bot_move_debug_log.value,
		fn,
		watchedMode,
		specIdx,
		specName);
	fflush(s_moveDbgLog);
}

// Same text as PrintIfWatched body (bot prefix + message), plus leading time for sorting. No gameplay effect.
static void AppendMovementDebugFileLine(CCSBot *bot, const char *messageBody)
{
	if (!bot || !cv_bot_move_debug_log.value || !messageBody || !gpGlobals)
	{
		if (!cv_bot_move_debug_log.value)
			CloseMoveDebugLogFile();
		return;
	}

	char gameDir[MAX_PATH];
	GET_GAME_DIR(gameDir);
	const char *fn = (cv_bot_move_debug_logfile.string && cv_bot_move_debug_logfile.string[0])
		? cv_bot_move_debug_logfile.string
		: "bot_move_debug.log";

	char path[MAX_PATH];
	Q_snprintf(path, sizeof(path), "%s/%s", gameDir, fn);

	if (!s_moveDbgLog || Q_strcmp(path, s_moveDbgLogPath) != 0)
	{
		CloseMoveDebugLogFile();
		s_moveDbgLog = fopen(path, "a");
		if (s_moveDbgLog)
			Q_strncpy(s_moveDbgLogPath, path, sizeof(s_moveDbgLogPath));
	}

	if (!s_moveDbgLog)
		return;

	if (!s_moveDbgConfigBannerWritten)
	{
		s_moveDbgConfigBannerWritten = true;
		WriteMovementDebugConfigBanner();
	}

	const char *name = bot->pev ? STRING(bot->pev->netname) : "(NULL pev)";
	fprintf(s_moveDbgLog, "%.3f %s: %s\n", gpGlobals->time, name ? name : "(NULL netname)", messageBody);
	fflush(s_moveDbgLog);
}

struct MoveDbgSlot
{
	bool init;
	BotState *prevState;
	CCSBot::TaskType prevTask;
	Vector prevGoal;
	unsigned prevPathDestAreaId;
	int prevPathLen;
	int prevPathIdx;
	unsigned prevLastKnownAreaId;
	unsigned prevCurrentAreaId;
	bool prevHasPath;
	bool prevStuck;
	int prevAvoidEnt;
	float prevNoiseTime;
	Vector prevNoisePos;
	unsigned prevNoiseAreaId;
	float prevLastSawEnemyTime;
	Vector prevLastEnemyPos;
	int prevLookReason;
	char prevLookDesc[48];
	bool prevRoamHold;
	unsigned char prevRoamHoldSource;
	int prevCombatStrafeDir;
	bool prevFriendBlock;
	bool prevWaitFriend;

	char ring[8][224];
	int ringWrite;

	// Nav transition oscillation (alternating dest/cluster keys, 8s window)
	uint32_t navOscA;
	uint32_t navOscB;
	int navOscFlipCount;
	float navOscWindowStart;
	uint32_t navLastOscKey;
	char navTransRing[6][288];
	int navTransRingWrite;

	void ClearRing()
	{
		ringWrite = 0;
		for (auto &r : ring)
			r[0] = 0;
	}

	void PushRing(const char *line)
	{
		Q_strncpy(ring[ringWrite % 8], line, int(sizeof(ring[0])));
		ringWrite++;
	}

	void ClearNavRing()
	{
		navTransRingWrite = 0;
		navOscA = navOscB = navLastOscKey = 0;
		navOscFlipCount = 0;
		navOscWindowStart = 0.0f;
		for (auto &r : navTransRing)
			r[0] = 0;
	}

	void PushNavRing(const char *line)
	{
		Q_strncpy(navTransRing[navTransRingWrite % 6], line, int(sizeof(navTransRing[0])));
		navTransRingWrite++;
	}
};

MoveDbgSlot s_slots[kMoveDbgMaxSlots];

inline int EntSlotIndex(CCSBot *bot)
{
	if (!bot)
		return -1;
	const int ei = bot->entindex();
	if (ei < 1 || ei >= kMoveDbgMaxSlots)
		return -1;
	return ei;
}

inline unsigned AreaIdConst(const CNavArea *a)
{
	return a ? static_cast<unsigned>(a->GetID()) : 0u;
}

inline void CopyLookDesc(char *dst, size_t dstSize, const char *src)
{
	if (!dst || dstSize == 0)
		return;
	dst[0] = 0;
	if (src)
		Q_strncpy(dst, src, int(dstSize));
}

inline const char *GoalSrcName(MoveDbgGoalSource s)
{
	switch (s)
	{
	case MoveDbgGoalSource::BuildTrivialPath: return "BuildTrivialPath";
	case MoveDbgGoalSource::ComputePathWaypoint: return "ComputePathWaypoint";
	case MoveDbgGoalSource::LadderApproach: return "LadderApproach";
	case MoveDbgGoalSource::LadderDismount: return "LadderDismount";
	case MoveDbgGoalSource::LadderMoveToDestination: return "LadderMoveToDest";
	case MoveDbgGoalSource::MapLearn: return "MapLearn";
	default: return "Unknown";
	}
}

inline const char *PathClrName(MoveDbgPathClearReason r)
{
	switch (r)
	{
	case MoveDbgPathClearReason::RepathCompute: return "RepathCompute";
	case MoveDbgPathClearReason::PathFailure: return "PathFailure";
	case MoveDbgPathClearReason::ReachedGoal: return "ReachedGoal";
	case MoveDbgPathClearReason::EnemyEngage: return "EnemyEngage";
	case MoveDbgPathClearReason::TaskOrStateChange: return "TaskOrStateChange";
	case MoveDbgPathClearReason::StuckTimeout: return "StuckTimeout";
	case MoveDbgPathClearReason::ExplicitCancel: return "ExplicitCancel";
	case MoveDbgPathClearReason::FriendAvoidRepath: return "FriendAvoidRepath";
	case MoveDbgPathClearReason::LadderFail: return "LadderFail";
	case MoveDbgPathClearReason::LadderFallOff: return "LadderFallOff";
	case MoveDbgPathClearReason::FollowPathNotProgressing: return "FollowPathNotProg";
	case MoveDbgPathClearReason::CombatKnifePathEnd: return "CombatKnifePathEnd";
	case MoveDbgPathClearReason::GetOffLadder: return "GetOffLadder";
	case MoveDbgPathClearReason::HideOrRetask: return "HideOrRetask";
	default: return "Unknown";
	}
}

uint32_t NavOscillationKey(const CCSBot *bot, unsigned pathDestAreaId, const Vector &goalPos)
{
	const CNavArea *gArea = TheNavAreaGrid.GetNavArea(&goalPos);
	const unsigned gAid = AreaIdConst(gArea);
	const int qx = (int)floorf(goalPos.x * (1.0f / 128.0f));
	const int qy = (int)floorf(goalPos.y * (1.0f / 128.0f));
	const unsigned cluster = (unsigned)((qx & 0xFF) | ((qy & 0xFF) << 8));
	const unsigned pda = pathDestAreaId ? pathDestAreaId : gAid;
	return (uint32_t)((pda << 16) | (cluster & 0xFFFFu));
}

bool MovementDebugWatchedGate(const CCSBot *bot)
{
	if (!bot || !cv_bot_debug.value || !gpGlobals)
		return false;
	return (bot->IsLocalPlayerWatchingMe() && (cv_bot_debug.value == 1 || cv_bot_debug.value == 3))
		|| (cv_bot_debug.value == 2 || cv_bot_debug.value == 4);
}

void EmitNavLogLine(CCSBot *bot, const char *line)
{
	if (!bot || !line)
		return;
	bot->PrintIfWatched("%s\n", line);
	AppendMovementDebugFileLine(bot, line);
}

void UpdateNavOscillationFromKey(CCSBot *bot, uint32_t key, const char *navLine)
{
	const int si = EntSlotIndex(bot);
	if (si < 0 || !navLine || key == 0u)
		return;

	MoveDbgSlot &S = s_slots[si];
	S.PushNavRing(navLine);

	const float winMax = 8.0f;
	if (S.navLastOscKey == key)
		return;

	const float t = gpGlobals->time;

	if (S.navOscFlipCount == 0 || (t - S.navOscWindowStart) > winMax)
	{
		S.navOscA = S.navOscB = 0;
		S.navOscFlipCount = 0;
		S.navOscWindowStart = t;
	}

	if (S.navLastOscKey != 0u)
	{
		const uint32_t from = S.navLastOscKey;
		if (S.navOscFlipCount == 0)
		{
			S.navOscA = from;
			S.navOscB = key;
			if (S.navOscA != S.navOscB)
				S.navOscFlipCount = 1;
		}
		else if (S.navOscA != 0u && S.navOscB != 0u)
		{
			const bool aToB = (from == S.navOscA && key == S.navOscB);
			const bool bToA = (from == S.navOscB && key == S.navOscA);
			if (!aToB && !bToA)
			{
				S.navOscA = from;
				S.navOscB = key;
				S.navOscFlipCount = (S.navOscA != S.navOscB) ? 1 : 0;
				S.navOscWindowStart = t;
			}
			else
			{
				S.navOscFlipCount++;
			}
		}
	}

	S.navLastOscKey = key;

	const float winDur = t - S.navOscWindowStart;
	if (S.navOscFlipCount > 3 && winDur <= winMax)
	{
		char head[160];
		Q_snprintf(head, sizeof(head),
			"[OSCILLATION SUSPECT] flips=%i win=%.2fs dest/cluster A=%08X B=%08X",
			S.navOscFlipCount, winDur, S.navOscA, S.navOscB);
		EmitNavLogLine(bot, head);
		for (int k = 0; k < 6; k++)
		{
			const int wi = S.navTransRingWrite - 1 - k;
			if (wi < 0)
				break;
			const char *row = S.navTransRing[wi % 6];
			if (row[0])
			{
				char pref[320];
				Q_snprintf(pref, sizeof(pref), "  %s", row);
				EmitNavLogLine(bot, pref);
			}
		}
		S.navOscFlipCount = 0;
		S.navOscWindowStart = t;
		S.navLastOscKey = 0;
		S.ClearNavRing();
	}
}
} // namespace

void EmitMovementWatchedDebugLine(CCSBot *bot, const char *line)
{
	if (!bot || !line || !gpGlobals)
		return;
	if (!MovementDebugWatchedGate(bot))
		return;
	bot->PrintIfWatched("%s\n", line);
	AppendMovementDebugFileLine(bot, line);
}

void CCSBot::BuildMovementLowCertTags(char *buf, size_t bufSize) const
{
	if (!buf || bufSize == 0)
		return;
	buf[0] = 0;
	bool first = true;
	auto appendTag = [&](const char *tag)
	{
		if (!tag || !tag[0])
			return;
		if (!first)
			Q_strncat(buf, "+", int(bufSize));
		first = false;
		Q_strncat(buf, tag, int(bufSize));
	};
	const char *sn = m_state ? m_state->GetName() : nullptr;
	if (sn && !Q_strcmp(sn, "Hunt"))
		appendTag("hunt");
	if (sn && !Q_strcmp(sn, "InvestigateNoise"))
		appendTag("noise");
	if (sn && !Q_strcmp(sn, "Idle"))
		appendTag("roam");
	if (sn && !Q_strcmp(sn, "Hide"))
		appendTag("search");
	if (sn && !Q_strcmp(sn, "EscapeFromBomb"))
		appendTag("danger");
	if (buf[0] == 0)
		Q_strncpy(buf, "none", int(bufSize));
}

void CCSBot::LogMovementNavDebug(
	const char *ev,
	MoveDbgGoalSource goalSrc,
	MoveDbgPathClearReason pathClear,
	const char *srcFunc,
	const Vector &oldGoal,
	unsigned oldPathDest,
	const Vector &newGoal,
	unsigned newPathDest)
{
	if (!MovementDebugWatchedGate(this) || !gpGlobals)
		return;

	char lowTags[48];
	BuildMovementLowCertTags(lowTags, sizeof(lowTags));

	Vector og = oldGoal;
	Vector ng = newGoal;
	const unsigned oldGoalA = AreaIdConst(TheNavAreaGrid.GetNavArea(&og));
	const unsigned newGoalA = AreaIdConst(TheNavAreaGrid.GetNavArea(&ng));

	float enMem = -1.0f;
	if (GetLastSawEnemyTimestamp() > 0.0f)
		enMem = gpGlobals->time - GetLastSawEnemyTimestamp();

	const char *stName = (m_state && m_state->GetName()) ? m_state->GetName() : "NULL";

	char line[288];
	Q_snprintf(line, sizeof(line),
		"[movedbg-nav] t=%.2f ev=%s fn=%s gsrc=%s pclr=%s "
		"oldG=(%.0f,%.0f,%.0f) oGA=%u newG=(%.0f,%.0f,%.0f) nGA=%u "
		"oldPD=%u newPD=%u eye=%i enMem=%.2f low=%s task=%i st=%s",
		gpGlobals->time,
		ev ? ev : "?",
		srcFunc ? srcFunc : "?",
		GoalSrcName(goalSrc),
		PathClrName(pathClear),
		oldGoal.x, oldGoal.y, oldGoal.z,
		oldGoalA,
		newGoal.x, newGoal.y, newGoal.z,
		newGoalA,
		oldPathDest,
		newPathDest,
		IsEnemyVisible() ? 1 : 0,
		enMem,
		lowTags,
		static_cast<int>(GetTask()),
		stName);

	EmitNavLogLine(this, line);
	const uint32_t ok = NavOscillationKey(this, newPathDest, newGoal);
	UpdateNavOscillationFromKey(this, ok, line);
}

void CCSBot::AssignMovementGoalPosition(const Vector &pos, MoveDbgGoalSource src, const char *srcFunc)
{
	const Vector oldG = m_goalPosition;
	const unsigned oldPD = GetPathDestAreaIDForDebug();
	m_goalPosition = pos;
	const unsigned newPD = GetPathDestAreaIDForDebug();

	if ((pos - oldG).LengthSquared() < 0.01f && oldPD == newPD)
		return;

	LogMovementNavDebug("goal", src, MoveDbgPathClearReason::Unknown, srcFunc, oldG, oldPD, pos, newPD);
}

void CCSBot::DestroyPath(MoveDbgPathClearReason reason, const char *srcFunc)
{
	const bool had = HasPath();
	const Vector g = m_goalPosition;
	const unsigned oldPD = GetPathDestAreaIDForDebug();

	m_pathLength = 0;
	m_pathLadder = nullptr;

	if (!had)
		return;

	LogMovementNavDebug("pathclr", MoveDbgGoalSource::Unknown, reason, srcFunc, g, oldPD, g, 0u);
}

void CCSBot::MaybeLogMovementDecisionOscillationDebug()
{
	const int si = EntSlotIndex(this);
	if (si < 0)
		return;

	if (!cv_bot_debug.value || !gpGlobals)
		return;

	const bool watchedGate = (IsLocalPlayerWatchingMe() && (cv_bot_debug.value == 1 || cv_bot_debug.value == 3))
		|| (cv_bot_debug.value == 2 || cv_bot_debug.value == 4);
	if (!watchedGate)
		return;

	MoveDbgSlot &S = s_slots[si];

	BotState *curState = m_state;
	const TaskType curTask = GetTask();
	const Vector &curGoal = m_goalPosition;

	unsigned pathDest = 0;
	if (HasPath() && m_pathLength > 0 && m_path[m_pathLength - 1].area)
		pathDest = static_cast<unsigned>(m_path[m_pathLength - 1].area->GetID());

	int pathIdxSafe = m_pathIndex;
	if (pathIdxSafe < 0 || pathIdxSafe >= m_pathLength)
		pathIdxSafe = (m_pathLength > 0) ? 0 : -1;

	unsigned nextArea = 0;
	if (pathIdxSafe >= 0 && pathIdxSafe < m_pathLength && m_path[pathIdxSafe].area)
		nextArea = static_cast<unsigned>(m_path[pathIdxSafe].area->GetID());

	const unsigned lastKnownId = AreaIdConst(m_lastKnownArea);
	const unsigned currentId = AreaIdConst(m_currentArea);
	const bool curHasPath = HasPath();

	int avoidEnt = 0;
	if (m_avoid.IsValid())
	{
		CBasePlayer *ap = m_avoid.Get<CBasePlayer>();
		if (ap)
			avoidEnt = ap->entindex();
	}

	const LookTarget &lt = GetLookTarget();
	const char *lookDesc = lt.debugDesc ? lt.debugDesc : m_lookAtDesc;
	const int lookReason = static_cast<int>(lt.reason);

	char lookDescBuf[48];
	CopyLookDesc(lookDescBuf, sizeof(lookDescBuf), lookDesc);

	int combatStrafeDir = 0;
	if (m_state == static_cast<BotState *>(&m_attackState))
		combatStrafeDir = m_attackState.GetCombatRepositionDirForDebug();

	float lastEnemyAge = -1.0f;
	if (m_lastSawEnemyTimestamp > 0.0f)
		lastEnemyAge = gpGlobals->time - m_lastSawEnemyTimestamp;

	const float stuckDur = m_isStuck ? (gpGlobals->time - m_stuckTimestamp) : 0.0f;

	if (!S.init)
	{
		S.prevState = curState;
		S.prevTask = curTask;
		S.prevGoal = curGoal;
		S.prevPathDestAreaId = pathDest;
		S.prevPathLen = m_pathLength;
		S.prevPathIdx = m_pathIndex;
		S.prevLastKnownAreaId = lastKnownId;
		S.prevCurrentAreaId = currentId;
		S.prevHasPath = curHasPath;
		S.prevStuck = m_isStuck;
		S.prevAvoidEnt = avoidEnt;
		S.prevNoiseTime = m_noiseTimestamp;
		S.prevNoisePos = m_noisePosition;
		S.prevNoiseAreaId = AreaIdConst(m_noiseArea);
		S.prevLastSawEnemyTime = m_lastSawEnemyTimestamp;
		S.prevLastEnemyPos = m_lastEnemyPosition;
		S.prevLookReason = lookReason;
		CopyLookDesc(S.prevLookDesc, sizeof(S.prevLookDesc), lookDescBuf);
		S.prevRoamHold = m_roamPoiHoldActive;
		S.prevRoamHoldSource = m_roamPoiHoldSource;
		S.prevCombatStrafeDir = combatStrafeDir;
		S.prevFriendBlock = m_isFriendInTheWay;
		S.prevWaitFriend = m_isWaitingBehindFriend;
		S.ClearRing();
		S.ClearNavRing();
		S.init = true;
		return;
	}

	unsigned chg = 0;
	if (curState != S.prevState)
		chg |= 1u;
	if (curTask != S.prevTask)
		chg |= 2u;
	if ((curGoal - S.prevGoal).LengthSquared() > 4096.0f)
		chg |= 4u;
	if (pathDest != S.prevPathDestAreaId || m_pathLength != S.prevPathLen || m_pathIndex != S.prevPathIdx || curHasPath != S.prevHasPath)
		chg |= 8u;
	if (lastKnownId != S.prevLastKnownAreaId || currentId != S.prevCurrentAreaId)
		chg |= 0x10u;
	if (m_noiseTimestamp != S.prevNoiseTime || (m_noisePosition - S.prevNoisePos).LengthSquared() > 64.0f || AreaIdConst(m_noiseArea) != S.prevNoiseAreaId)
		chg |= 0x20u;
	if (m_lastSawEnemyTimestamp != S.prevLastSawEnemyTime || (m_lastEnemyPosition - S.prevLastEnemyPos).LengthSquared() > 64.0f)
		chg |= 0x40u;
	if (m_isStuck != S.prevStuck)
		chg |= 0x80u;
	if (avoidEnt != S.prevAvoidEnt)
		chg |= 0x100u;
	if (lookReason != S.prevLookReason || Q_strcmp(lookDescBuf, S.prevLookDesc) != 0)
		chg |= 0x200u;
	if (m_isFriendInTheWay != S.prevFriendBlock || m_isWaitingBehindFriend != S.prevWaitFriend)
		chg |= 0x400u;
	if (m_roamPoiHoldActive != S.prevRoamHold || m_roamPoiHoldSource != S.prevRoamHoldSource)
		chg |= 0x800u;
	if (combatStrafeDir != S.prevCombatStrafeDir)
		chg |= 0x1000u;

	if (chg == 0)
		return;

	const char *stateName = curState ? curState->GetName() : "NULL";
	const char *pfail = "none";
	if (S.prevHasPath && !curHasPath)
		pfail = "path_cleared";
	else if ((chg & 8u) && (m_pathLength != S.prevPathLen || m_pathIndex != S.prevPathIdx || pathDest != S.prevPathDestAreaId))
		pfail = "path_changed";

	const unsigned goalAreaId = AreaIdConst(TheNavAreaGrid.GetNavArea(&m_goalPosition));

	char line[224];
	Q_snprintf(line, sizeof(line),
		"[movedbg] t=%.2f chg=0x%x st=%s task=%i goal=(%.0f,%.0f,%.0f) gA=%u lkA=%u cA=%u path=%i len=%i idx=%i destA=%u nextA=%u pfail=%s "
		"eyeEnemy=%i enMemAge=%.2f enPos=(%.0f,%.0f,%.0f) look=%i:\"%.47s\" poiHold=%i/%u stuck=%i sdur=%.2f avoid=#%i strafe=%i friBlk=%i waitFr=%i wgl=%i",
		gpGlobals->time,
		chg,
		stateName,
		static_cast<int>(curTask),
		curGoal.x, curGoal.y, curGoal.z,
		goalAreaId,
		lastKnownId,
		currentId,
		curHasPath ? 1 : 0,
		m_pathLength,
		m_pathIndex,
		pathDest,
		nextArea,
		pfail,
		IsEnemyVisible() ? 1 : 0,
		lastEnemyAge,
		m_lastEnemyPosition.x, m_lastEnemyPosition.y, m_lastEnemyPosition.z,
		lookReason,
		lookDescBuf,
		m_roamPoiHoldActive ? 1 : 0,
		static_cast<unsigned>(m_roamPoiHoldSource),
		m_isStuck ? 1 : 0,
		stuckDur,
		avoidEnt,
		combatStrafeDir,
		m_isFriendInTheWay ? 1 : 0,
		m_isWaitingBehindFriend ? 1 : 0,
		static_cast<int>(m_wiggleDirection));

	PrintIfWatched("%s\n", line);
	AppendMovementDebugFileLine(this, line);
	S.PushRing(line);

	S.prevState = curState;
	S.prevTask = curTask;
	S.prevGoal = curGoal;
	S.prevPathDestAreaId = pathDest;
	S.prevPathLen = m_pathLength;
	S.prevPathIdx = m_pathIndex;
	S.prevLastKnownAreaId = lastKnownId;
	S.prevCurrentAreaId = currentId;
	S.prevHasPath = curHasPath;
	S.prevStuck = m_isStuck;
	S.prevAvoidEnt = avoidEnt;
	S.prevNoiseTime = m_noiseTimestamp;
	S.prevNoisePos = m_noisePosition;
	S.prevNoiseAreaId = AreaIdConst(m_noiseArea);
	S.prevLastSawEnemyTime = m_lastSawEnemyTimestamp;
	S.prevLastEnemyPos = m_lastEnemyPosition;
	S.prevLookReason = lookReason;
	CopyLookDesc(S.prevLookDesc, sizeof(S.prevLookDesc), lookDescBuf);
	S.prevRoamHold = m_roamPoiHoldActive;
	S.prevRoamHoldSource = m_roamPoiHoldSource;
	S.prevCombatStrafeDir = combatStrafeDir;
	S.prevFriendBlock = m_isFriendInTheWay;
	S.prevWaitFriend = m_isWaitingBehindFriend;
}
