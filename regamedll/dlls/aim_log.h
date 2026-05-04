#pragma once

class CBasePlayer;
class CBaseEntity;
class Vector;

// Zeroed *lookYawVel / *lookPitchVel attribution (motor forensics CSV; only diagnostic).
enum AimMotorZeroedVelReason : int
{
	AimMotorZeroVel_None = 0,
	AimMotorZeroVel_SnapEpsilon = 1,
	AimMotorZeroVel_OvershootPrevention = 2,
	AimMotorZeroVel_MinStepDeadzone = 3,
	AimMotorZeroVel_StaleYawGuard = 4,
	AimMotorZeroVel_AngleNormalization = 5,
	AimMotorZeroVel_DtInvalid = 6,
	AimMotorZeroVel_LadderOverride = 7,
	AimMotorZeroVel_MimicOrChat = 8,
	AimMotorZeroVel_Unknown = 9,
};

enum AimMotorDirectAngleWriteReason : int
{
	AimMotorDirectWrite_None = 0,
	AimMotorDirectWrite_EnemyMicroSnap = 1,
	AimMotorDirectWrite_Ladder = 2,
	AimMotorDirectWrite_MimicChatSkip = 3,
	AimMotorDirectWrite_StaleGuard = 4,
	AimMotorDirectWrite_Other = 5,
	AimMotorDirectWrite_NonCombatTinySnap = 6,
};

// Enemy yaw snap path (combat aim only); logged to combat CSV.
enum AimYawSnapMode : int
{
	AimYawSnapMode_None = 0,
	AimYawSnapMode_HardTinySnap = 1,
	AimYawSnapMode_SoftenedSpringBand = 2,
	AimYawSnapMode_BypassedLegacyBand = 3,
};

// Filled by HumanAimMotorController::ApplySpringDamperToViewAngles when diagnostics pointer non-null.
struct AimMotorStepDiag
{
	bool valid;
	float stiffness;
	float damping;
	float maxAccel;
	float yawErrorBefore;
	float pitchErrorBefore;
	float yawAccelApplied;
	float pitchAccelApplied;
	float yawVelAfter;
	float pitchVelAfter;
	bool microYawSnap;
	float deltaYawWritten;
	float deltaPitchWritten;
	int yawSnapMode; // AimYawSnapMode

	// Extended forensics when sv_aim_log_enable > 0 (same combat tick as valid).
	bool motorForensicValid;
	float motorDt;
	float yawVelPre;
	float pitchVelPre;
	float yawAccelRaw;
	float pitchAccelRaw;
	float yawAccelClamped;
	float pitchAccelClamped;
	float yawVelPostIntegrate;
	float pitchVelPostIntegrate;
	float yawStepRaw;
	float pitchStepRaw;
	float yawStepApplied;
	float pitchStepApplied;
	float yawVelFinal;
	float pitchVelFinal;
	float yawErrPost;
	float pitchErrPost;
	int zeroedYawVelReason;
	int zeroedPitchVelReason;
	int directAngleWriteReason;
	int motorDtInvalid; // 1 if deltaT <= 0 at entry (informational; does not change motor math)
};

// One CSV row for combat-aim diagnostics (no logic — AimLog only serializes).
struct AimCombatDiagRecord
{
	float time;
	int botEnt;
	const char *botName;
	int enemyEnt;
	int weaponId;
	const char *weaponName;
	int enemyVisible;
	int aimingAtEnemy;
	int combatPhase;
	float confidence;
	float motorScale;
	int acquisitionBurst;
	float acquireAge;
	float yawErrBeforeMotor;
	float pitchErrBeforeMotor;
	float yawVel;
	float pitchVel;
	float yawAccel;
	float pitchAccel;
	int microYawSnap;
	int yawSnapMode;
	float deltaYaw;
	float deltaPitch;
	float jitterMag;
	float aimOffsetMag;
	float recoilMag;
	float distEnemy;
	int firedAttackIntent;
	int refusedAimCone;
	float sniperViewMotionThresh;

	// FireWeaponAtEnemy aim gate (2D forward vs aim spot), when evaluated this think.
	int aimConeEvaluated;
	float aimDot;
	float requiredAimDot;
	float aimDotMargin;
	float aimConeErrorDeg;
	float aimConeToleranceDeg;
	float aimConeRatio;
	int aimConePass;
	int didFire;

	// Motor forensics (mirror AimMotorStepDiag when motorForensicValid).
	float motorDt;
	float yawVelPre;
	float pitchVelPre;
	float yawAccelRaw;
	float pitchAccelRaw;
	float yawAccelClamped;
	float pitchAccelClamped;
	float yawVelPostIntegrate;
	float pitchVelPostIntegrate;
	float yawStepRaw;
	float pitchStepRaw;
	float yawStepApplied;
	float pitchStepApplied;
	float yawVelFinal;
	float pitchVelFinal;
	float yawErrPost;
	float pitchErrPost;
	int zeroedYawVelReason;
	int zeroedPitchVelReason;
	int directAngleWriteReason;
	int motorDtInvalid;
};

// Bot aim pipeline observability (no behavior by itself):
// - CSV aim_bot_events.csv / roam_aim_bot_events.csv when sv_aim_log_enable / sv_roam_aim_log_enable are on.
// - Server: bot_debug drives CBot::PrintIfWatched; bot_traceview draws nav/debug beams (see cs_bot_update.cpp).
//
// Aim pipeline stage labels (documentation; see CCSBot::UpdateAimController):
//   UpdateAimController -> (UpdateCombatAimController | UpdateNonCombatAimController) -> UpdateLookAngles -> pev->v_angle
//
// RecordBotSample "event" column: "firing" or "roaming" from IN_ATTACK only (not the same as combat vs non-combat aim).
//
// RecordBotRoamSample stateName labels (from CCSBot::UpdateAimController): "moving", "hiding", "grenade",
// "knife_run", or default "roaming".

namespace AimLog
{
	void RecordHumanSample(CBasePlayer *player);
	void RecordBotSample(CBasePlayer *bot, const Vector &botAimSpot, bool hasBotAimSpot, CBaseEntity *target);
	void RecordHumanRoamSample(CBasePlayer *player);
	void RecordBotRoamSample(CBasePlayer *bot, const char *stateName);
	void RecordHumanGrenadeThrowIfAny(CBasePlayer *player);
	void RecordBotGrenadeThrow(CBasePlayer *bot, const Vector *targetPosition, bool usedManualSpot, float distanceFromManualSpot, int manualSpotIndex, const char *manualSpotName);

	// CSV aim_bot_combat_diag.csv when sv_aim_log_enable > 0 (same interval throttle as RecordBotSample).
	void RecordBotCombatAimDiag(CBasePlayer *bot, const AimCombatDiagRecord &rec);
	const char *GetActiveWeaponEntityName(CBasePlayer *player);
}
