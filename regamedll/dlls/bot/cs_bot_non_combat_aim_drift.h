/*
 * Coherent non-combat aim drift: slow sinusoidal shaping of desired look angles plus
 * infrequently resampled micro-offsets before the final spring/damper motor step.
 * This is not per-frame random jitter and not a second aim motor - pev->v_angle is still
 * owned exclusively by UpdateLookAngles via HumanAimMotorController::ApplySpringDamperToViewAngles.
 */
#pragma once

#include <cstdint>

class CCSBot;

namespace NonCombatAimDrift
{
// Passive debugger/tuning state only (never affects aim). Last-computed deltas for this process.
// Micro path: RecordMicroDriftSkipped when ApplySampledMicroOffsetsBeforeMotor is not invoked
// (combat aim or ladder). TODO: mimic/chat early return in UpdateLookAngles leaves this stale;
// consider per-bot mirrors or explicit clears without touching aim.
enum MicroDriftDebugPath : uint8_t
{
	MicroDriftDebug_Applied = 0,
	MicroDriftDebug_SkippedCombatAim,
	MicroDriftDebug_SkippedLadder,
};

// Runs at end of UpdateNonCombatAimController when not in combat aim (caller guarantees path).
void ApplySinusoidalDriftToDesiredLook(CCSBot *bot);

// Applies sampled offsets and non-combat motor scale. Caller must match legacy guards:
// !IsAimingAtEnemy() && !IsUsingLadder().
void ApplySampledMicroOffsetsBeforeMotor(CCSBot *bot, float &useYaw, float &usePitch, float &stiffness, float &maxAccel);

// When micro drift is not applied, record why (combat vs ladder). Passive only.
void RecordMicroDriftSkipped(MicroDriftDebugPath path);

// Tick only when bot_debug != 0 (for regression / tuning in debugger); no console spam.
extern uint32_t g_debugDriftSinTicks;
extern uint32_t g_debugDriftMicroPathTicks;
extern float g_debugLastSinYawDelta;
extern float g_debugLastSinPitchDelta;
extern float g_debugLastMicroYawAdd;
extern float g_debugLastMicroPitchAdd;
extern MicroDriftDebugPath g_debugMicroDriftPath;
} // namespace NonCombatAimDrift
