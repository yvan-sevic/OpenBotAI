/*
*
*   Roam glance *decision* only: mirrors legacy UpdateRoamLook branching. Orchestration
*   (UpdateLookAround) and application (SetLookAt / ClearLookAt) stay in cs_bot_vision.cpp.
 *   todo7: deterministic settle hold for MeaningfulPoi reduces twitch among near-equivalent roam POIs; yields to combat/precision.
 *   todo8: urgent non-combat gaze can bypass roam phase defer for fresh threat/noise memory (scored POIs only); not combat aim.
 *
*/

#pragma once

#include <stdint.h>

#include "game_shared/bot/bot_util.h"
#include "vector.h"

class CCSBot;

namespace LookTargetSelector {

// World positions for SetLookAt must approximate human eye/chest height unless already traced in aim space.
enum class RoamLookAimHeightKind : unsigned char
{
	// Ground/nav or noise/last-enemy foot position — add HalfHumanHeight before SetLookAt.
	GroundNav,
	// Already spot.pos + HalfHumanHeight, trace hit, camp aim trace endpoint, or synthetic eye ray.
	EyeAim
};

// Snapshot of bot state needed for moving roam glance selection (no private CCSBot access in selector).
// lookAtSpotState uses the same numeric values as CCSBot::LookAtSpotState.
struct RoamLookContext
{
	float time;
	bool updateNow;
	bool isNotMoving;
	bool isUsingLadder;
	bool isAimingAtEnemy;
	bool isSafe;
	float effectiveSkill;
	int lookAtSpotState;
	PriorityType lookAtSpotPriority;
	float lookAtSpotTimestamp;
	float roamLookPhaseTimestamp;
	float lookAheadAngle;
	float lookAheadPitch;

	const char *currentLookDesc;

	Vector lastEnemyPosition;
	float lastSawEnemyTimestamp;

	Vector noisePosition;
	float noiseTimestamp;
	bool noiseHeard;
	PriorityType noisePriority;

	// Up to 16 approach points sampled from CCSBot (when more exist on nav, only the first 16 are visible here).
	unsigned char approachPointCount;
	Vector approachPoints[16];

	bool isHidingOrCamp;
	// True when AiChat "typing" freeze is active (matches UpdateLookAngles early-out intent; do not select new POI glances).
	bool chatFreezeLook;
	// todo8: true while the bot has LOS combat sight on its current enemy; suppresses urgent non-combat roam cadence bypass.
	bool isEnemyVisible;
	// todo8: tracked combat enemy still alive (GetEnemy); suppresses urgent bypass even when not aiming this tick.
	bool hasValidCombatEnemy;
	// When true, roam POI settle-hold must not run (combat/precision/grenade/mimic/chat-freeze, etc.).
	bool blockRoamPoiHold;
};

enum class RoamLookCandidateSource : unsigned char
{
	None,
	DangerMemoryUnsafe,
	DangerMemoryNearLastEnemy,
	LastEnemySeen,
	HeardNoise,
	ApproachPoint,
	CampRecorded
};

struct RoamLookCandidate
{
	RoamLookCandidateSource source;
	float score;
	Vector position;
	RoamLookAimHeightKind heightKind;
	const char *reason;
	// Tie-break metadata (deterministic pick only; set at collection time).
	unsigned char stabilityRank; // higher = preferred when scores are near-tied
	float freshnessSecs;	     // lower = fresher threat/sound; large for non-temporal sources
};

// High-level outcome of SelectRoamMovingGlance (legacy + MeaningfulPoi).
enum class RoamLookDecisionType : unsigned char
{
	Ineligible,
	DeferPhaseTimer,
	KeepCurrentGlance,
	MeaningfulPoi,
	DangerAngle,
	TerrainLane,
	CornerCheck,
	RecenterClear,

	Count_
};

// Must match CCSBot::RoamLookPhase numeric order (see static_assert in .cpp). Meaningful only when updateRoamPhase is true.
enum class RoamLookPhaseStep : int
{
	Forward = 0,
	DangerAngle = 1,
	CornerCheck = 2,
	TerrainCheck = 3,
	Recenter = 4
};

// Typed apply bundle: UpdateRoamLook maps this to SetLookAt / ClearLookAt and m_roamLookPhase*.
struct RoamLookDecision
{
	RoamLookDecisionType type;
	bool roamLookReturn;

	bool applySetLookAt;
	bool applyClearLookAt;

	bool updateRoamPhase;
	RoamLookPhaseStep roamPhaseStep;
	float roamPhaseTimestamp;

	const char *lookDesc;
	Vector lookPosition;
	PriorityType priority;
	float duration;
	bool clearIfClose;
	float angleTolerance;

	// Debug / regression (todo4); unused when bot_debug is off.
	RoamLookCandidateSource debugPoiSource;
	float debugPoiScore;
	bool debugUsedHeightBoost;
	bool debugUsedSyntheticFallback;
	// Post-todo4 safety/tuning (tie-break label + RoamPoiCollectDiag flags); only when bot_debug/watched.
	const char *debugPoiTieBreak;
	uint32_t debugPoiDiag;

	// todo8: watched-bot debug only — urgent non-combat gaze cadence bypass (not combat acquisition, not final aim motor).
	bool debugUrgentCadenceBypass;
	const char *debugUrgentBypassReason;
	bool debugUrgentHoldChallenged;

	// Watched-bot summary only: POI winner vs hold score delta when hold existed before this decision (does not affect selection).
	bool debugRoamSelHaveScoreCmp;
	float debugRoamSelPoiVsHoldDelta;
};

// todo8: Non-combat roam gaze — lets fresh threat/noise memory cues refresh scored POI candidates before the roam phase timer
// defer would normally allow. Does not alter combat target acquisition, HumanAimMotorController, drift, or pathing.
bool HasUrgentRoamGazeCue(const RoamLookContext &ctx, const char **outReason);
bool ShouldBypassRoamLookCadence(const RoamLookContext &ctx, const char **outReason);
bool ShouldRefreshRoamPoiCandidatesNow(const RoamLookContext &ctx, const char **outReason);

RoamLookDecision SelectRoamMovingGlance(const RoamLookContext &ctx, CCSBot *bot);

// When bot_debug is on, logs one line (via PrintIfWatched) and increments per-type counters for regression compares.
void DebugLogRoamDecisionApply(CCSBot *me, const RoamLookContext &ctx, const RoamLookDecision &decision);

// Optional: reset counters before a timed test run (only useful with bot_debug non-zero).
void DebugResetRoamDecisionStats();

} // namespace LookTargetSelector
