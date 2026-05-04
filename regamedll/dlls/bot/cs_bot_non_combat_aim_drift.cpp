#include "precompiled.h"

#include "bot/cs_bot_non_combat_aim_drift.h"
#include "bot/cs_bot_init.h"

namespace NonCombatAimDrift
{

uint32_t g_debugDriftSinTicks = 0;
uint32_t g_debugDriftMicroPathTicks = 0;
float g_debugLastSinYawDelta = 0.0f;
float g_debugLastSinPitchDelta = 0.0f;
float g_debugLastMicroYawAdd = 0.0f;
float g_debugLastMicroPitchAdd = 0.0f;
MicroDriftDebugPath g_debugMicroDriftPath = MicroDriftDebug_Applied;

void RecordMicroDriftSkipped(MicroDriftDebugPath path)
{
	g_debugMicroDriftPath = path;
	g_debugLastMicroYawAdd = 0.0f;
	g_debugLastMicroPitchAdd = 0.0f;
}

void ApplySinusoidalDriftToDesiredLook(CCSBot *bot)
{
	if (!bot)
		return;

	// Non-combat roaming drift (slow oscillation on desired angles; applied before UpdateLookAngles).
	float driftAmplitude = bot->IsNotMoving() ? 0.55f : 0.18f;
	if (bot->IsUsingSniperRifle() && bot->IsUsingScope())
		driftAmplitude = bot->IsNotMoving() ? 0.22f : 0.08f;

	const float phase = gpGlobals->time + bot->entindex() * 0.71f;
	const float dYaw = driftAmplitude * BotCOS(2.8f * phase);
	const float dPitch = driftAmplitude * 1.35f * BotSIN(1.7f * phase);
	bot->m_lookYaw += dYaw;
	bot->m_lookPitch += dPitch;
	g_debugLastSinYawDelta = dYaw;
	g_debugLastSinPitchDelta = dPitch;

	if (cv_bot_debug.value != 0.0f)
		++g_debugDriftSinTicks;
}

void ApplySampledMicroOffsetsBeforeMotor(CCSBot *bot, float &useYaw, float &usePitch, float &stiffness, float &maxAccel)
{
	if (!bot)
		return;

	const float yawBefore = useYaw;
	const float pitchBefore = usePitch;

	const bool isMoving = !bot->IsNotMoving();

	if (gpGlobals->time >= bot->m_nextNonCombatLookOffsetUpdate)
	{
		const bool hasLookAt = (bot->m_lookAtSpotState != CCSBot::NOT_LOOKING_AT_SPOT);
		const float yawAmplitude = isMoving
			? (hasLookAt ? RANDOM_FLOAT(0.04f, 0.18f) : RANDOM_FLOAT(0.08f, 0.45f))
			: RANDOM_FLOAT(0.04f, 0.30f);
		const float pitchAmplitude = isMoving
			? (hasLookAt ? RANDOM_FLOAT(0.04f, 0.18f) : RANDOM_FLOAT(0.08f, 0.55f))
			: RANDOM_FLOAT(0.05f, 0.28f);

		bot->m_nonCombatLookYawOffset = RANDOM_FLOAT(-yawAmplitude, yawAmplitude);
		bot->m_nonCombatLookPitchOffset = RANDOM_FLOAT(-pitchAmplitude, pitchAmplitude);

		// Occasional tiny "mouse correction" while pathing breaks long perfect arcs.
		if (isMoving && !hasLookAt && RANDOM_FLOAT(0.0f, 100.0f) < 3.0f)
			bot->m_nonCombatLookYawOffset += RANDOM_FLOAT(-0.22f, 0.22f);

		bot->m_nextNonCombatLookOffsetUpdate = gpGlobals->time + RANDOM_FLOAT(isMoving ? 0.75f : 0.30f, isMoving ? 2.35f : 1.20f);
	}

	if (isMoving && bot->m_lookAtSpotState == CCSBot::NOT_LOOKING_AT_SPOT && gpGlobals->time >= bot->m_nextNonCombatLookPitchBiasUpdate)
	{
		const float r = RANDOM_FLOAT(0.0f, 100.0f);
		if (r < 26.0f)
			bot->m_nonCombatLookPitchBias = RANDOM_FLOAT(-2.8f, -0.65f); // quick floor/feet check
		else if (r < 52.0f)
			bot->m_nonCombatLookPitchBias = RANDOM_FLOAT(0.65f, 2.6f); // glance slightly up/ahead
		else
			bot->m_nonCombatLookPitchBias = RANDOM_FLOAT(-0.55f, 0.75f);

		bot->m_nextNonCombatLookPitchBiasUpdate = gpGlobals->time + RANDOM_FLOAT(1.25f, 3.40f);
	}

	if (gpGlobals->time >= bot->m_nextNonCombatLookMotorUpdate)
	{
		bot->m_nonCombatLookMotorScale = isMoving ? RANDOM_FLOAT(0.70f, 0.98f) : RANDOM_FLOAT(0.62f, 0.88f);
		if (isMoving && RANDOM_FLOAT(0.0f, 100.0f) < 4.0f)
			bot->m_nonCombatLookMotorScale = RANDOM_FLOAT(1.02f, 1.14f);

		bot->m_nextNonCombatLookMotorUpdate = gpGlobals->time + RANDOM_FLOAT(0.28f, isMoving ? 0.85f : 1.10f);
	}

	bot->m_nonCombatLookYawOffset *= isMoving ? 0.985f : 0.96f;
	bot->m_nonCombatLookPitchOffset *= isMoving ? 0.984f : 0.95f;
	bot->m_nonCombatLookPitchBias *= 0.992f;
	useYaw += bot->m_nonCombatLookYawOffset;
	usePitch += bot->m_nonCombatLookPitchOffset + bot->m_nonCombatLookPitchBias;
	if (isMoving && bot->m_lookAtSpotState == CCSBot::NOT_LOOKING_AT_SPOT)
	{
		if (usePitch < -8.0f)
			usePitch = -8.0f;
		else if (usePitch > 7.5f)
			usePitch = 7.5f;
	}
	stiffness *= bot->m_nonCombatLookMotorScale;
	maxAccel *= bot->m_nonCombatLookMotorScale;

	g_debugLastMicroYawAdd = useYaw - yawBefore;
	g_debugLastMicroPitchAdd = usePitch - pitchBefore;
	g_debugMicroDriftPath = MicroDriftDebug_Applied;

	if (cv_bot_debug.value != 0.0f)
		++g_debugDriftMicroPathTicks;
}

} // namespace NonCombatAimDrift
