/*
*
*   Roam glance decision — moving-only POI scoring (todo4) with deterministic settle hold for roam POIs (todo7).
*   Legacy synthetic fallback unchanged except hold clears when leaving MeaningfulPoi.
*
*/

#include "precompiled.h"
#include "bot/cs_bot_look_target_selector.h"
#include "bot/cs_bot.h"
#include "bot/cs_bot_danger_memory.h"
#include "bot/cs_bot_init.h"
#include "bot/cs_bot_camp_look_spots.h"

namespace {

constexpr float kPoiMinScoreToPrefer = 72.0f;
// Scores within this margin of the top qualifying score compete via stable tie-breakers (not random).
constexpr float kPoiScoreNearTieEpsilon = 2.0f;
// Clear win over an active roam POI settle hold: must exceed the near-tie band used by PickBestPoiCandidate.
constexpr float kPoiHoldUrgentBeatMargin = 2.0f * kPoiScoreNearTieEpsilon;
constexpr float kPoiNonTemporalFreshness = 9000.0f;
constexpr int kRoamPoiMaxApproachSamples = 6;

enum RoamPoiCollectDiagFlags : uint32_t
{
	ROAM_POI_DIAG_STALE_LAST_ENEMY = 1u << 0,
	ROAM_POI_DIAG_APPROACH_CLAMPED = 1u << 1,
	ROAM_POI_DIAG_DANGER_POI_ROLL_MISS = 1u << 2,
	ROAM_POI_DIAG_CHAT_SKIP         = 1u << 3,
};

struct RoamPoiCollectDiag
{
	uint32_t flags;
};

Vector BuildRoamLookSpot(CCSBot *me, float yaw, float pitch, float range)
{
	Vector eye = me->GetEyePosition();
	float flatRange = range * BotCOS(pitch);
	return eye + Vector(flatRange * BotCOS(yaw), flatRange * BotSIN(yaw), -range * BotSIN(pitch));
}

void KeepRoamLookSpotInView(CCSBot *me, Vector *spot)
{
	TraceResult result;
	Vector eye = me->GetEyePosition();
	UTIL_TraceLine(eye, *spot, ignore_monsters, ignore_glass, ENT(me->pev), &result);

	if (result.flFraction < 0.95f && !result.fStartSolid)
	{
		Vector to = *spot - eye;
		*spot = eye + to * Q_max(0.35f, result.flFraction - 0.08f);
	}
}

inline Vector FinalizeRoamLookAimPosition(const Vector &worldPos, LookTargetSelector::RoamLookAimHeightKind kind)
{
	if (kind == LookTargetSelector::RoamLookAimHeightKind::GroundNav)
		return worldPos + Vector(0, 0, HalfHumanHeight);
	return worldPos;
}

inline void PushCandidate(
	LookTargetSelector::RoamLookCandidate *list,
	int *count,
	int maxCount,
	LookTargetSelector::RoamLookCandidateSource src,
	float score,
	const Vector &worldPos,
	LookTargetSelector::RoamLookAimHeightKind kind,
	const char *reason,
	unsigned char stabilityRank,
	float freshnessSecs)
{
	if (*count >= maxCount)
		return;

	LookTargetSelector::RoamLookCandidate &c = list[(*count)++];
	c.source = src;
	c.score = score;
	c.position = FinalizeRoamLookAimPosition(worldPos, kind);
	c.heightKind = kind;
	c.reason = reason;
	c.stabilityRank = stabilityRank;
	c.freshnessSecs = freshnessSecs;
}

int CollectRoamPoiCandidates(
	const LookTargetSelector::RoamLookContext &ctx,
	CCSBot *bot,
	bool cachedDangerValid,
	const Vector &cachedDangerSpot,
	LookTargetSelector::RoamLookCandidate *list,
	int maxCount,
	RoamPoiCollectDiag *diagOut)
{
	int n = 0;
	if (!bot || maxCount <= 0)
		return 0;

	const bool unsafe = !ctx.isSafe;
	const float skill = ctx.effectiveSkill;
	Vector spot;

	// Match legacy roam danger probability so POI danger does not always starve noise / other candidates.
	// TODO: POI danger and legacy danger each roll independently — total danger-memory frequency may exceed
	// a single legacy roll; revisit with shared roll or merged probability if tuning demands it.
	if (unsafe && cachedDangerValid)
	{
		if (RANDOM_FLOAT(0.0f, 100.0f) < 48.0f + skill * 18.0f)
		{
			PushCandidate(list, &n, maxCount, LookTargetSelector::RoamLookCandidateSource::DangerMemoryUnsafe,
				88.0f + skill * 0.35f, cachedDangerSpot, LookTargetSelector::RoamLookAimHeightKind::EyeAim, "Roam POI Danger",
				5, kPoiNonTemporalFreshness);
		}
		else if (diagOut)
		{
			diagOut->flags |= ROAM_POI_DIAG_DANGER_POI_ROLL_MISS;
		}
	}

	// Learned danger / last-seen foot cues: require a real last-seen timestamp (avoid round-start / zero baseline).
	if (ctx.lastSawEnemyTimestamp <= 0.0f)
	{
		if (diagOut)
			diagOut->flags |= ROAM_POI_DIAG_STALE_LAST_ENEMY;
	}
	else
	{
		const float enemyAge = ctx.time - ctx.lastSawEnemyTimestamp;
		// Same windows as POI collection before this pass (near-memory 3s; raw last position ~1.45s).
		if (enemyAge >= 0.0f && enemyAge < 3.0f
			&& BotDangerMemory::FindBestLookSpotNear(bot, ctx.lastEnemyPosition, 720.0f, &spot))
		{
			const float ageFactor = Q_max(0.35f, 1.0f - enemyAge * 0.33f);
			PushCandidate(list, &n, maxCount, LookTargetSelector::RoamLookCandidateSource::DangerMemoryNearLastEnemy,
				76.0f * ageFactor, spot, LookTargetSelector::RoamLookAimHeightKind::EyeAim, "Roam POI NearMemory",
				4, enemyAge);
		}
		else if (enemyAge >= 0.0f && enemyAge < 1.45f)
		{
			const float ageFactor = Q_max(0.2f, 1.0f - enemyAge * 0.65f);
			PushCandidate(list, &n, maxCount, LookTargetSelector::RoamLookCandidateSource::LastEnemySeen,
				82.0f * ageFactor, ctx.lastEnemyPosition, LookTargetSelector::RoamLookAimHeightKind::GroundNav, "Roam POI LastEnemy",
				4, enemyAge);
		}
	}

	// Heard noise (foot position → head height).
	if (ctx.noiseHeard && ctx.noiseTimestamp > 0.0f)
	{
		const float noiseAge = ctx.time - ctx.noiseTimestamp;
		if (noiseAge >= 0.0f && noiseAge < 11.0f)
		{
			const float pri = static_cast<float>(ctx.noisePriority);
			const float score = 66.0f + pri * 9.0f - noiseAge * 1.15f;
			const unsigned char noiseRank = (ctx.noisePriority >= PRIORITY_HIGH) ? 4 : 3;
			PushCandidate(list, &n, maxCount, LookTargetSelector::RoamLookCandidateSource::HeardNoise, score, ctx.noisePosition,
				LookTargetSelector::RoamLookAimHeightKind::GroundNav, "Roam POI Noise", noiseRank, noiseAge);
		}
	}

	// Approach points: closest-first, capped tries (no heavy loops; max 16 nav points × cheap sort).
	if (ctx.approachPointCount > 0)
	{
		const int nPts = static_cast<int>(ctx.approachPointCount);
		if (diagOut && nPts > kRoamPoiMaxApproachSamples)
			diagOut->flags |= ROAM_POI_DIAG_APPROACH_CLAMPED;

		int order[16];
		const int nOrder = Q_min(nPts, 16);
		for (int i = 0; i < nOrder; ++i)
			order[i] = i;

		for (int i = 1; i < nOrder; ++i)
		{
			const int key = order[i];
			Vector apk = ctx.approachPoints[key];
			const float dk = (Vector(apk.x, apk.y, 0) - Vector(bot->pev->origin.x, bot->pev->origin.y, 0)).Length2D();
			int j = i - 1;
			while (j >= 0)
			{
				Vector apj = ctx.approachPoints[order[j]];
				const float dj = (Vector(apj.x, apj.y, 0) - Vector(bot->pev->origin.x, bot->pev->origin.y, 0)).Length2D();
				if (dk >= dj)
					break;
				order[j + 1] = order[j];
				--j;
			}
			order[j + 1] = key;
		}

		const int nTry = Q_min(kRoamPoiMaxApproachSamples, nOrder);
		for (int t = 0; t < nTry; ++t)
		{
			const int idx = order[t];
			Vector ap = ctx.approachPoints[idx];
			const float range2d = (Vector(ap.x, ap.y, 0) - Vector(bot->pev->origin.x, bot->pev->origin.y, 0)).Length2D();
			Vector test = ap + Vector(0, 0, HalfHumanHeight);
			TraceResult tr;
			UTIL_TraceLine(bot->GetEyePosition(), test, ignore_monsters, ignore_glass, ENT(bot->pev), &tr);
			const float score = 52.0f + 28.0f * tr.flFraction - range2d * 0.0045f;
			PushCandidate(list, &n, maxCount, LookTargetSelector::RoamLookCandidateSource::ApproachPoint, score, ap,
				LookTargetSelector::RoamLookAimHeightKind::GroundNav, "Roam POI Approach", 2, 400.0f + range2d * 0.12f);
		}
	}

	// Recorded camp hold aim traces — already eye/impact space.
	if (ctx.isHidingOrCamp)
	{
		const char *campName = nullptr;
		if (BotCampLookSpots::FindLookTargetRotating(bot, &spot, &campName, false))
		{
			const float score = ctx.isSafe ? 96.0f : 58.0f;
			const unsigned char rank = ctx.isSafe ? 6u : 2u;
			PushCandidate(list, &n, maxCount, LookTargetSelector::RoamLookCandidateSource::CampRecorded, score, spot,
				LookTargetSelector::RoamLookAimHeightKind::EyeAim, campName ? campName : "Roam POI Camp", rank, 600.0f);
		}
	}

	return n;
}

// Prefer higher scores only when separated by more than kPoiScoreNearTieEpsilon; otherwise use stability
// metadata so tiny score deltas (float noise or formula churn) do not twitch the selected POI ahead of
// continuation, source tier, or fresher memory.
inline bool RoamPoiBandEntryBetter(float scoreA, bool contA, unsigned char rankA, float freshA, int idxA,
	float scoreB, bool contB, unsigned char rankB, float freshB, int idxB)
{
	if (scoreA > scoreB + kPoiScoreNearTieEpsilon)
		return true;
	if (scoreB > scoreA + kPoiScoreNearTieEpsilon)
		return false;

	// Near-tie band: stability / safety metadata (deterministic).
	if (contA != contB)
		return contA;
	if (rankA != rankB)
		return rankA > rankB;
	if (freshA != freshB)
		return freshA < freshB;
	if (scoreA != scoreB)
		return scoreA > scoreB;
	return idxA < idxB;
}

float RoamPoiHoldSecondsForSource(LookTargetSelector::RoamLookCandidateSource s)
{
	using LookTargetSelector::RoamLookCandidateSource;
	switch (s)
	{
	case RoamLookCandidateSource::DangerMemoryUnsafe:
		return 0.38f;
	case RoamLookCandidateSource::DangerMemoryNearLastEnemy:
		return 0.40f;
	case RoamLookCandidateSource::LastEnemySeen:
		return 0.42f;
	case RoamLookCandidateSource::HeardNoise:
		return 0.40f;
	case RoamLookCandidateSource::ApproachPoint:
		return 0.72f;
	case RoamLookCandidateSource::CampRecorded:
		return 0.78f;
	default:
		return 0.50f;
	}
}

bool RoamPoiAnyCandidateNearHold(const LookTargetSelector::RoamLookCandidate *list, int n, const Vector &holdAim)
{
	for (int i = 0; i < n; ++i)
	{
		if ((list[i].position - holdAim).Length() < 72.0f)
			return true;
	}
	return false;
}

bool RoamPoiHoldBadGeometry(CCSBot *bot, const Vector &holdAim)
{
	if (!bot)
		return true;

	const Vector eye = bot->GetEyePosition();
	Vector to = holdAim - eye;
	const float dist = to.Length();
	if (dist < 12.0f)
		return false;

	to.x /= dist;
	to.y /= dist;
	to.z /= dist;

	Vector forward(BotCOS(bot->pev->v_angle.y), BotSIN(bot->pev->v_angle.y), 0.0f);
	forward.NormalizeInPlace();

	Vector toHoriz(to.x, to.y, 0.0f);
	const float lenH = toHoriz.Length();
	if (lenH > 0.001f)
	{
		toHoriz.x /= lenH;
		toHoriz.y /= lenH;
		if (DotProduct(forward, toHoriz) < -0.28f)
			return true;
	}

	TraceResult tr;
	UTIL_TraceLine(eye, holdAim, ignore_monsters, ignore_glass, ENT(bot->pev), &tr);
	if (!tr.fStartSolid && tr.flFraction < 0.12f)
		return true;

	return false;
}

bool PickBestPoiCandidate(
	const LookTargetSelector::RoamLookContext &ctx,
	LookTargetSelector::RoamLookCandidate *list,
	int n,
	LookTargetSelector::RoamLookCandidate *winner,
	const char **outTieBreak)
{
	if (n <= 0 || !winner)
		return false;

	float bandTop = -1e9f;
	for (int i = 0; i < n; ++i)
	{
		if (list[i].score >= kPoiMinScoreToPrefer)
			bandTop = Q_max(bandTop, list[i].score);
	}

	if (bandTop < kPoiMinScoreToPrefer)
		return false;

	// Candidates with score in [bandTop - epsilon, bandTop] compete; RoamPoiBandEntryBetter resolves near-ties.
	const float bandFloor = bandTop - kPoiScoreNearTieEpsilon;
	int winIdx = -1;
	bool winCont = false;
	int bandCount = 0;

	for (int i = 0; i < n; ++i)
	{
		if (list[i].score < kPoiMinScoreToPrefer || list[i].score < bandFloor)
			continue;
		bandCount++;
		const bool cont = ctx.currentLookDesc && list[i].reason && !Q_strcmp(ctx.currentLookDesc, list[i].reason);
		if (winIdx < 0
			|| RoamPoiBandEntryBetter(list[i].score, cont, list[i].stabilityRank, list[i].freshnessSecs, i,
				list[winIdx].score, winCont, list[winIdx].stabilityRank, list[winIdx].freshnessSecs, winIdx))
		{
			winIdx = i;
			winCont = cont;
		}
	}

	if (winIdx < 0)
		return false;

	*winner = list[winIdx];
	if (outTieBreak)
	{
		if (winCont)
			*outTieBreak = "continuation";
		else
			*outTieBreak = (bandCount > 1) ? "nearTieMeta" : "uniqueBest";
	}

	return true;
}

LookTargetSelector::RoamLookDecision DecisionFromPoiWinner(const LookTargetSelector::RoamLookContext &ctx, const LookTargetSelector::RoamLookCandidate &w)
{
	LookTargetSelector::RoamLookDecision d{};
	d.type = LookTargetSelector::RoamLookDecisionType::MeaningfulPoi;
	d.roamLookReturn = true;
	d.applySetLookAt = true;
	d.applyClearLookAt = false;
	d.updateRoamPhase = true;
	d.lookDesc = w.reason;
	d.lookPosition = w.position;
	d.priority = PRIORITY_LOW;
	d.clearIfClose = true;

	const bool threatLike =
		(w.source == LookTargetSelector::RoamLookCandidateSource::DangerMemoryUnsafe
		 || w.source == LookTargetSelector::RoamLookCandidateSource::DangerMemoryNearLastEnemy
		 || w.source == LookTargetSelector::RoamLookCandidateSource::LastEnemySeen
		 || w.source == LookTargetSelector::RoamLookCandidateSource::HeardNoise);

	if (threatLike)
	{
		d.roamPhaseStep = LookTargetSelector::RoamLookPhaseStep::DangerAngle;
		d.roamPhaseTimestamp = ctx.time + RANDOM_FLOAT(0.65f, 1.45f);
		d.duration = RANDOM_FLOAT(0.22f, 0.52f);
		d.angleTolerance = 14.0f;
	}
	else
	{
		d.roamPhaseStep = LookTargetSelector::RoamLookPhaseStep::CornerCheck;
		d.roamPhaseTimestamp = ctx.time + RANDOM_FLOAT(0.55f, !ctx.isSafe ? 1.25f : 1.75f);
		d.duration = RANDOM_FLOAT(0.16f, 0.42f);
		d.angleTolerance = 16.0f;
	}

	d.debugPoiSource = w.source;
	d.debugPoiScore = w.score;
	d.debugUsedHeightBoost = (w.heightKind == LookTargetSelector::RoamLookAimHeightKind::GroundNav);
	d.debugUsedSyntheticFallback = false;
	d.debugPoiTieBreak = nullptr;
	d.debugPoiDiag = 0;

	return d;
}

// Light counters for comparing roam selection mix across builds (only ticked when bot_debug != 0).
uint32_t g_roamDecisionCountByType[static_cast<size_t>(LookTargetSelector::RoamLookDecisionType::Count_)];

inline LookTargetSelector::RoamLookDecision FinishDecision(LookTargetSelector::RoamLookDecision d)
{
	if (cv_bot_debug.value != 0.0f && d.type != LookTargetSelector::RoamLookDecisionType::Ineligible)
	{
		const size_t idx = static_cast<size_t>(d.type);
		if (idx < ARRAYSIZE(g_roamDecisionCountByType))
			g_roamDecisionCountByType[idx]++;
	}

	return d;
}

} // namespace

namespace LookTargetSelector {

namespace {
// todo8: conservative freshness for urgent non-combat gaze cadence bypass (existing timestamps only).
constexpr float kUrgentEnemyMemoryMaxAge = 1.2f;
constexpr float kUrgentNoiseMaxAgeMediumPlus = 1.0f;
constexpr float kUrgentNoiseMaxAgeLowPri = 0.35f;
} // namespace

static const char *RoamLookDecisionTypeLabel(RoamLookDecisionType t)
{
	switch (t)
	{
	case RoamLookDecisionType::Ineligible:
		return "Ineligible";
	case RoamLookDecisionType::DeferPhaseTimer:
		return "DeferPhaseTimer";
	case RoamLookDecisionType::KeepCurrentGlance:
		return "KeepCurrentGlance";
	case RoamLookDecisionType::MeaningfulPoi:
		return "MeaningfulPoi";
	case RoamLookDecisionType::DangerAngle:
		return "DangerAngle";
	case RoamLookDecisionType::TerrainLane:
		return "TerrainLane";
	case RoamLookDecisionType::CornerCheck:
		return "CornerCheck";
	case RoamLookDecisionType::RecenterClear:
		return "RecenterClear";
	case RoamLookDecisionType::Count_:
		return "Count_";
	default:
		return "?";
	}
}

static const char *RoamSelRouteCompact(RoamLookDecisionType t)
{
	switch (t)
	{
	case RoamLookDecisionType::DeferPhaseTimer:
		return "defer";
	case RoamLookDecisionType::KeepCurrentGlance:
		return "keep";
	case RoamLookDecisionType::MeaningfulPoi:
		return "poi";
	case RoamLookDecisionType::DangerAngle:
	case RoamLookDecisionType::TerrainLane:
	case RoamLookDecisionType::CornerCheck:
	case RoamLookDecisionType::RecenterClear:
		return "leg";
	case RoamLookDecisionType::Ineligible:
		return "x";
	default:
		return "?";
	}
}

static const char *RoamLookCandidateSourceLabel(RoamLookCandidateSource s)
{
	switch (s)
	{
	case RoamLookCandidateSource::DangerMemoryUnsafe:
		return "DangerUnsafe";
	case RoamLookCandidateSource::DangerMemoryNearLastEnemy:
		return "DangerNearEnemy";
	case RoamLookCandidateSource::LastEnemySeen:
		return "LastEnemy";
	case RoamLookCandidateSource::HeardNoise:
		return "Noise";
	case RoamLookCandidateSource::ApproachPoint:
		return "Approach";
	case RoamLookCandidateSource::CampRecorded:
		return "Camp";
	default:
		return "None";
	}
}

bool HasUrgentRoamGazeCue(const RoamLookContext &ctx, const char **outReason)
{
	if (outReason)
		*outReason = nullptr;

	// Shared last-seen timeline for raw last position and danger-near-last-enemy POI sources.
	if (ctx.lastSawEnemyTimestamp > 0.0f)
	{
		const float enemyAge = ctx.time - ctx.lastSawEnemyTimestamp;
		if (enemyAge >= 0.0f && enemyAge <= kUrgentEnemyMemoryMaxAge)
		{
			if (outReason)
				*outReason = "enemyMem";
			return true;
		}
	}

	if (ctx.noiseHeard && ctx.noiseTimestamp > 0.0f)
	{
		const float noiseAge = ctx.time - ctx.noiseTimestamp;
		if (noiseAge >= 0.0f)
		{
			if (ctx.noisePriority >= PRIORITY_MEDIUM && noiseAge <= kUrgentNoiseMaxAgeMediumPlus)
			{
				if (outReason)
					*outReason = "noise";
				return true;
			}
			if (ctx.noisePriority == PRIORITY_LOW && noiseAge <= kUrgentNoiseMaxAgeLowPri)
			{
				if (outReason)
					*outReason = "noise";
				return true;
			}
		}
	}

	// TODO(todo8): DangerMemoryUnsafe POI has no exposed age/freshness in RoamLookContext (random roll + position only);
	// omit as an urgent cue until a deterministic timestamp is plumbed, to avoid guessing.

	return false;
}

bool ShouldBypassRoamLookCadence(const RoamLookContext &ctx, const char **outReason)
{
	if (ctx.isNotMoving || ctx.isUsingLadder || ctx.isAimingAtEnemy)
		return false;
	if (ctx.blockRoamPoiHold || ctx.chatFreezeLook)
		return false;
	if (ctx.isEnemyVisible || ctx.hasValidCombatEnemy)
		return false;
	return HasUrgentRoamGazeCue(ctx, outReason);
}

bool ShouldRefreshRoamPoiCandidatesNow(const RoamLookContext &ctx, const char **outReason)
{
	return ShouldBypassRoamLookCadence(ctx, outReason);
}

void DebugResetRoamDecisionStats()
{
	for (size_t i = 0; i < ARRAYSIZE(g_roamDecisionCountByType); ++i)
		g_roamDecisionCountByType[i] = 0;
}

void DebugLogRoamDecisionApply(CCSBot *me, const RoamLookContext &ctx, const RoamLookDecision &d)
{
	if (!me || !cv_bot_debug.value)
		return;

	const float computedEnd = (d.applySetLookAt && d.duration >= 0.0f) ? (ctx.time + d.duration) : -1.0f;
	const int phaseInt = d.updateRoamPhase ? static_cast<int>(d.roamPhaseStep) : -1;

	const bool holdOn = me->GetDebugRoamPoiHoldActive();
	const char *sumHs = "-";
	if (holdOn)
		sumHs = RoamLookCandidateSourceLabel(static_cast<RoamLookCandidateSource>(me->GetDebugRoamPoiHoldSource()));
	const float sumHsc = holdOn ? me->GetDebugRoamPoiHoldScore() : 0.0f;

	char dvsBuf[24];
	const char *dvs = "-";
	if (d.debugRoamSelHaveScoreCmp)
	{
		Q_snprintf(dvsBuf, sizeof(dvsBuf), "%+.2f", d.debugRoamSelPoiVsHoldDelta);
		dvs = dvsBuf;
	}

	me->PrintIfWatched(
		"[roamsel] type=%s desc=%s pri=%d dur=%.3f end=%.3f phaseStep=%d phaseNext=%.3f ret=%d unsafe=%d skill=%.3f set=%d clear=%d "
		"poiSrc=%s poiScore=%.1f hBoost=%d synthFb=%d tie=%s diag=0x%X chatFrz=%d holdA=%d holdRem=%.3f hy=%s "
		"urgBy=%d urgR=%s urgHoldChg=%d | sum rt=%s mp=%d blk=%d hs=%s hvs=%.1f dvs=%s\n",
		RoamLookDecisionTypeLabel(d.type),
		d.lookDesc ? d.lookDesc : "(null)",
		static_cast<int>(d.priority),
		d.duration,
		computedEnd,
		phaseInt,
		d.roamPhaseTimestamp,
		d.roamLookReturn ? 1 : 0,
		ctx.isSafe ? 0 : 1,
		ctx.effectiveSkill,
		d.applySetLookAt ? 1 : 0,
		d.applyClearLookAt ? 1 : 0,
		RoamLookCandidateSourceLabel(d.debugPoiSource),
		d.debugPoiScore,
		d.debugUsedHeightBoost ? 1 : 0,
		d.debugUsedSyntheticFallback ? 1 : 0,
		d.debugPoiTieBreak ? d.debugPoiTieBreak : "-",
		static_cast<unsigned int>(d.debugPoiDiag),
		ctx.chatFreezeLook ? 1 : 0,
		me->GetDebugRoamPoiHoldActive() ? 1 : 0,
		me->GetDebugRoamPoiHoldRemain(),
		me->GetDebugRoamPoiHoldYieldTag() ? me->GetDebugRoamPoiHoldYieldTag() : "-",
		d.debugUrgentCadenceBypass ? 1 : 0,
		d.debugUrgentBypassReason ? d.debugUrgentBypassReason : "-",
		d.debugUrgentHoldChallenged ? 1 : 0,
		RoamSelRouteCompact(d.type),
		(d.type == RoamLookDecisionType::MeaningfulPoi) ? 1 : 0,
		ctx.blockRoamPoiHold ? 1 : 0,
		sumHs,
		sumHsc,
		dvs);
}

RoamLookDecision SelectRoamMovingGlance(const RoamLookContext &ctx, CCSBot *bot)
{
	RoamLookDecision d{};
	d.type = RoamLookDecisionType::Ineligible;
	d.lookDesc = nullptr;
	d.lookPosition = Vector(0, 0, 0);
	d.priority = PRIORITY_LOW;
	d.duration = -1.0f;
	d.clearIfClose = false;
	d.angleTolerance = 5.0f;
	d.debugPoiSource = RoamLookCandidateSource::None;
	d.debugPoiScore = 0.0f;
	d.debugUsedHeightBoost = false;
	d.debugUsedSyntheticFallback = true;
	d.debugPoiTieBreak = nullptr;
	d.debugPoiDiag = 0;
	d.debugUrgentCadenceBypass = false;
	d.debugUrgentBypassReason = nullptr;
	d.debugUrgentHoldChallenged = false;
	d.debugRoamSelHaveScoreCmp = false;
	d.debugRoamSelPoiVsHoldDelta = 0.0f;

	// TODO(todo4+): BotDangerMemory / camp / traces still need CCSBot; a narrower context API could decouple further.

	if (!bot)
		return FinishDecision(d);

	if (ctx.isNotMoving || ctx.isUsingLadder || ctx.isAimingAtEnemy)
	{
		bot->m_roamPoiHoldActive = false;
		bot->m_roamPoiHoldDebugRemain = -1.0f;
		bot->m_roamPoiHoldDebugYieldTag = nullptr;
		return FinishDecision(d);
	}

	if (ctx.lookAtSpotState != 0)
	{
		if (ctx.lookAtSpotPriority >= PRIORITY_MEDIUM)
		{
			bot->m_roamPoiHoldActive = false;
			bot->m_roamPoiHoldDebugRemain = -1.0f;
			bot->m_roamPoiHoldDebugYieldTag = nullptr;
			d.type = RoamLookDecisionType::KeepCurrentGlance;
			d.roamLookReturn = true;
			d.debugUsedSyntheticFallback = false;
			return FinishDecision(d);
		}

		// Roam POI glances use scoring + settle-hold (todo7) instead of this blind window so urgent POI wins can pre-empt.
		const bool isRoamPoiGlance = ctx.currentLookDesc && !Q_strncmp(ctx.currentLookDesc, "Roam POI", 8);
		if (!isRoamPoiGlance && ctx.time - ctx.lookAtSpotTimestamp < 0.45f)
		{
			bot->m_roamPoiHoldActive = false;
			bot->m_roamPoiHoldDebugRemain = -1.0f;
			bot->m_roamPoiHoldDebugYieldTag = nullptr;
			d.type = RoamLookDecisionType::KeepCurrentGlance;
			d.roamLookReturn = true;
			d.debugUsedSyntheticFallback = false;
			return FinishDecision(d);
		}
	}

	// Expire settle hold before phase-timer defer so debug/holdRem is not stale (hold active with negative remain).
	if (bot->m_roamPoiHoldActive && ctx.time > bot->m_roamPoiHoldEndTime)
		bot->m_roamPoiHoldActive = false;

	bot->m_roamPoiHoldDebugRemain =
		(bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime) ? (bot->m_roamPoiHoldEndTime - ctx.time) : -1.0f;

	// todo8: urgent non-combat gaze — bypass roam phase-timer defer only for fresh threat/noise memory (scored POIs).
	const char *urgentReason = nullptr;
	const bool refreshUrgent = ShouldRefreshRoamPoiCandidatesNow(ctx, &urgentReason);
	if (refreshUrgent)
	{
		d.debugUrgentCadenceBypass = true;
		d.debugUrgentBypassReason = urgentReason;
		const bool holdWasActiveForUrgent =
			!ctx.blockRoamPoiHold && bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime;
		if (holdWasActiveForUrgent)
			d.debugUrgentHoldChallenged = true;
	}

	if (!ctx.updateNow && ctx.time < ctx.roamLookPhaseTimestamp && !refreshUrgent)
	{
		d.type = RoamLookDecisionType::DeferPhaseTimer;
		d.debugUsedSyntheticFallback = false;
		return FinishDecision(d);
	}

	const bool urgentBypassedPhaseDefer =
		refreshUrgent && !ctx.updateNow && ctx.time < ctx.roamLookPhaseTimestamp;

	RoamPoiCollectDiag poiDiag{};
	RoamLookCandidate poiList[16];
	Vector cachedDangerLook;
	bool cachedDangerOk = false;
	if (!ctx.isSafe && bot)
		cachedDangerOk = BotDangerMemory::FindBestLookSpot(bot, &cachedDangerLook);

	const char *poiTieBreak = nullptr;
	if (ctx.chatFreezeLook)
	{
		bot->m_roamPoiHoldActive = false;
		poiDiag.flags |= ROAM_POI_DIAG_CHAT_SKIP;
	}
	else
	{
		if (ctx.blockRoamPoiHold)
			bot->m_roamPoiHoldActive = false;

		const int poiCount = CollectRoamPoiCandidates(ctx, bot, cachedDangerOk, cachedDangerLook, poiList, ARRAYSIZE(poiList), &poiDiag);
		RoamLookCandidate poiWinner{};
		if (poiCount > 0 && PickBestPoiCandidate(ctx, poiList, poiCount, &poiWinner, &poiTieBreak))
		{
			const bool hadHoldForDbg = !ctx.blockRoamPoiHold && bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime;
			const float holdScForDbg = hadHoldForDbg ? bot->m_roamPoiHoldScore : 0.0f;

			bool takeNewPoi = true;
			if (!ctx.blockRoamPoiHold && bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime)
			{
				const char *yieldTag = nullptr;
				if (RoamPoiHoldBadGeometry(bot, bot->m_roamPoiHoldAimPos))
					yieldTag = "geom";
				else if ((bot->pev->origin - bot->m_roamPoiHoldCommitOrigin).Length() > 260.0f)
					yieldTag = "moved";
				else if (!RoamPoiAnyCandidateNearHold(poiList, poiCount, bot->m_roamPoiHoldAimPos))
					yieldTag = "noCandNearHold";

				if (!yieldTag && poiWinner.score > bot->m_roamPoiHoldScore + kPoiHoldUrgentBeatMargin)
					yieldTag = "urgentScore";

				if (yieldTag)
				{
					bot->m_roamPoiHoldActive = false;
					bot->m_roamPoiHoldDebugYieldTag = yieldTag;
				}
				else
				{
					const float scoreDelta = poiWinner.score - bot->m_roamPoiHoldScore;
					const float distToHold = (poiWinner.position - bot->m_roamPoiHoldAimPos).Length();

					if (distToHold < 48.0f || scoreDelta <= kPoiHoldUrgentBeatMargin)
						takeNewPoi = false;

					if (!takeNewPoi)
						bot->m_roamPoiHoldDebugYieldTag = nullptr;
				}
			}

			if (!takeNewPoi)
			{
				RoamLookDecision k{};
				k.type = RoamLookDecisionType::KeepCurrentGlance;
				k.roamLookReturn = true;
				k.debugUsedSyntheticFallback = false;
				k.debugPoiTieBreak = "poiSettleHold";
				k.debugPoiDiag = poiDiag.flags;
				k.debugUrgentCadenceBypass = d.debugUrgentCadenceBypass;
				k.debugUrgentBypassReason = d.debugUrgentBypassReason;
				k.debugUrgentHoldChallenged = d.debugUrgentHoldChallenged;
				if (hadHoldForDbg)
				{
					k.debugRoamSelHaveScoreCmp = true;
					k.debugRoamSelPoiVsHoldDelta = poiWinner.score - holdScForDbg;
				}
				bot->m_roamPoiHoldDebugRemain =
					(bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime) ? (bot->m_roamPoiHoldEndTime - ctx.time) : -1.0f;
				return FinishDecision(k);
			}

			RoamLookDecision dec = DecisionFromPoiWinner(ctx, poiWinner);
			dec.debugPoiTieBreak = poiTieBreak;
			dec.debugPoiDiag = poiDiag.flags;
			dec.debugUrgentCadenceBypass = d.debugUrgentCadenceBypass;
			dec.debugUrgentBypassReason = d.debugUrgentBypassReason;
			dec.debugUrgentHoldChallenged = d.debugUrgentHoldChallenged;
			if (hadHoldForDbg)
			{
				dec.debugRoamSelHaveScoreCmp = true;
				dec.debugRoamSelPoiVsHoldDelta = poiWinner.score - holdScForDbg;
			}
			if (!ctx.blockRoamPoiHold)
			{
				bot->m_roamPoiHoldActive = true;
				bot->m_roamPoiHoldEndTime = ctx.time + RoamPoiHoldSecondsForSource(poiWinner.source);
				bot->m_roamPoiHoldAimPos = poiWinner.position;
				bot->m_roamPoiHoldSource = static_cast<unsigned char>(poiWinner.source);
				bot->m_roamPoiHoldScore = poiWinner.score;
				bot->m_roamPoiHoldCommitOrigin = bot->pev->origin;
				bot->m_roamPoiHoldDebugYieldTag = nullptr;
			}
			bot->m_roamPoiHoldDebugRemain =
				(bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime) ? (bot->m_roamPoiHoldEndTime - ctx.time) : -1.0f;
			return FinishDecision(dec);
		}
	}

	// Urgent cadence bypass ran POI collection but no scored winner: do not fall through to legacy random/synthetic
	// (that would clear a valid settle hold without a POI margin win).
	if (urgentBypassedPhaseDefer)
	{
		if (!ctx.chatFreezeLook && !ctx.blockRoamPoiHold && bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime)
		{
			RoamLookDecision k{};
			k.type = RoamLookDecisionType::KeepCurrentGlance;
			k.roamLookReturn = true;
			k.debugUsedSyntheticFallback = false;
			k.debugPoiTieBreak = "urgentNoPoiWinnerHold";
			k.debugPoiDiag = poiDiag.flags;
			k.debugUrgentCadenceBypass = d.debugUrgentCadenceBypass;
			k.debugUrgentBypassReason = d.debugUrgentBypassReason;
			k.debugUrgentHoldChallenged = d.debugUrgentHoldChallenged;
			k.debugRoamSelHaveScoreCmp = d.debugRoamSelHaveScoreCmp;
			k.debugRoamSelPoiVsHoldDelta = d.debugRoamSelPoiVsHoldDelta;
			bot->m_roamPoiHoldDebugRemain =
				(bot->m_roamPoiHoldActive && ctx.time <= bot->m_roamPoiHoldEndTime) ? (bot->m_roamPoiHoldEndTime - ctx.time) : -1.0f;
			return FinishDecision(k);
		}

		d.type = RoamLookDecisionType::DeferPhaseTimer;
		d.debugUsedSyntheticFallback = false;
		return FinishDecision(d);
	}

	auto finishLegacyStamped = [&poiDiag, bot](RoamLookDecision dec) -> RoamLookDecision {
		if (bot)
			bot->m_roamPoiHoldActive = false;
		dec.debugPoiDiag = poiDiag.flags;
		dec.debugPoiTieBreak = nullptr;
		return FinishDecision(dec);
	};

	const bool unsafe = !ctx.isSafe;
	const float skill = ctx.effectiveSkill;
	Vector spot;

	// TODO: Independent roll from POI danger above — combined danger glance rate may be higher than legacy-only.
	if (unsafe && RANDOM_FLOAT(0.0f, 100.0f) < 48.0f + skill * 18.0f && cachedDangerOk)
	{
		d.type = RoamLookDecisionType::DangerAngle;
		d.updateRoamPhase = true;
		d.roamPhaseStep = RoamLookPhaseStep::DangerAngle;
		d.roamPhaseTimestamp = ctx.time + RANDOM_FLOAT(0.65f, 1.45f);
		d.applySetLookAt = true;
		d.lookDesc = "Roam Danger Angle";
		d.lookPosition = cachedDangerLook;
		d.priority = PRIORITY_LOW;
		d.duration = RANDOM_FLOAT(0.22f, 0.52f);
		d.clearIfClose = true;
		d.angleTolerance = 14.0f;
		d.roamLookReturn = true;
		return finishLegacyStamped(d);
	}

	const float terrainPitch = Q_max(-7.5f, Q_min(ctx.lookAheadPitch, 3.5f));
	if (Q_abs(terrainPitch) > 1.35f && RANDOM_FLOAT(0.0f, 100.0f) < 68.0f)
	{
		d.type = RoamLookDecisionType::TerrainLane;
		d.updateRoamPhase = true;
		d.roamPhaseStep = RoamLookPhaseStep::TerrainCheck;
		d.roamPhaseTimestamp = ctx.time + RANDOM_FLOAT(0.55f, 1.15f);
		spot = BuildRoamLookSpot(bot, ctx.lookAheadAngle + RANDOM_FLOAT(-5.0f, 5.0f), terrainPitch, RANDOM_FLOAT(620.0f, 1040.0f));
		KeepRoamLookSpotInView(bot, &spot);
		d.applySetLookAt = true;
		d.lookDesc = "Roam Terrain Lane";
		d.lookPosition = spot;
		d.priority = PRIORITY_LOW;
		d.duration = RANDOM_FLOAT(0.24f, 0.52f);
		d.clearIfClose = true;
		d.angleTolerance = 16.0f;
		d.roamLookReturn = true;
		return finishLegacyStamped(d);
	}

	if (unsafe || RANDOM_FLOAT(0.0f, 100.0f) < 35.0f)
	{
		const float side = (RANDOM_LONG(0, 1) == 0) ? -1.0f : 1.0f;
		const float sideAngle = side * RANDOM_FLOAT(18.0f, unsafe ? 48.0f : 34.0f);
		const float pitch = Q_max(-7.0f, Q_min(ctx.lookAheadPitch + RANDOM_FLOAT(-0.8f, 1.0f), 3.5f));

		d.type = RoamLookDecisionType::CornerCheck;
		d.updateRoamPhase = true;
		d.roamPhaseStep = RoamLookPhaseStep::CornerCheck;
		d.roamPhaseTimestamp = ctx.time + RANDOM_FLOAT(0.55f, unsafe ? 1.25f : 1.75f);
		spot = BuildRoamLookSpot(bot, ctx.lookAheadAngle + sideAngle, pitch, RANDOM_FLOAT(560.0f, 980.0f));
		KeepRoamLookSpotInView(bot, &spot);
		d.applySetLookAt = true;
		d.lookDesc = "Roam Corner Check";
		d.lookPosition = spot;
		d.priority = PRIORITY_LOW;
		d.duration = RANDOM_FLOAT(0.16f, 0.42f);
		d.clearIfClose = true;
		d.angleTolerance = 16.0f;
		d.roamLookReturn = true;
		return finishLegacyStamped(d);
	}

	if (bot && bot->IsIdleLookCommitActive() && !bot->ShouldOverrideIdleLookCommit())
	{
		RoamLookDecision k{};
		k.type = RoamLookDecisionType::KeepCurrentGlance;
		k.roamLookReturn = true;
		k.debugUsedSyntheticFallback = false;
		k.debugPoiTieBreak = "idleCommitDeferRecenter";
		return FinishDecision(k);
	}

	d.type = RoamLookDecisionType::RecenterClear;
	d.updateRoamPhase = true;
	d.roamPhaseStep = RoamLookPhaseStep::Recenter;
	d.roamPhaseTimestamp = ctx.time + RANDOM_FLOAT(0.30f, 0.85f);
	d.applyClearLookAt = true;
	d.debugUsedSyntheticFallback = true;
	return finishLegacyStamped(d);
}

} // namespace LookTargetSelector
