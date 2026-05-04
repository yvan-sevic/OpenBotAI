#include "precompiled.h"
#include "aim_log.h"

// ---- Bot aim controller (intent -> desired look angles) ----
// UpdateAimController(): combat vs non-combat, then always UpdateLookAngles().
// - Combat: UpdateCombatAimController() -> SetLookAngles (enemy world aim).
// - Non-combat: UpdateNonCombatAimController() -> path look-ahead and/or SetLookAt state; roaming drift
//   at end adjusts m_lookYaw / m_lookPitch before UpdateLookAngles().
// - SetLookAt state lives in CCSBot::SetLookAt (cs_bot_vision.cpp); priority protects high-priority looks
//   (e.g. grenade PRIORITY_UNINTERRUPTABLE, bomb/use/hostage) from interruption by lower requests.
// Logging: AimLog::RecordBotSample / RecordBotRoamSample; roam state strings documented in aim_log.h.

namespace
{
	inline float AimClamp(float value, float minValue, float maxValue)
	{
		if (value < minValue)
			return minValue;
		if (value > maxValue)
			return maxValue;
		return value;
	}

	inline Vector AimLerp(const Vector &from, const Vector &to, float t)
	{
		return from + (to - from) * AimClamp(t, 0.0f, 1.0f);
	}

	inline Vector CurrentViewSpot(CCSBot *bot, float range)
	{
		Vector eye = bot->GetEyePosition();
		float flatRange = range * BotCOS(bot->pev->v_angle.x);
		return eye + Vector(flatRange * BotCOS(bot->pev->v_angle.y), flatRange * BotSIN(bot->pev->v_angle.y), -range * BotSIN(bot->pev->v_angle.x));
	}

	struct RecoilControlProfile
	{
		float reactionDelay;
		float verticalPull;
		float punchPitchScale;
		float horizontalDrift;
		float punchYawScale;
		float maxVertical;
		float maxHorizontal;
	};

	int ActiveWeaponId(CBasePlayer *player)
	{
		if (!player || !player->m_pActiveItem)
			return WEAPON_NONE;

		CBasePlayerWeapon *weapon = static_cast<CBasePlayerWeapon *>(player->m_pActiveItem);
		if (!weapon)
			return WEAPON_NONE;

		return weapon->m_iId;
	}

	RecoilControlProfile GetRecoilControlProfile(int weaponId)
	{
		RecoilControlProfile profile = { 0.045f, 0.0080f, 0.0018f, 0.0018f, 0.0015f, 16.0f, 8.0f };

		switch (weaponId)
		{
		case WEAPON_AK47:
			profile.reactionDelay = 0.030f;
			profile.verticalPull = 0.0155f;
			profile.punchPitchScale = 0.0042f;
			profile.horizontalDrift = 0.0068f;
			profile.punchYawScale = 0.0035f;
			profile.maxVertical = 34.0f;
			profile.maxHorizontal = 28.0f;
			break;
		case WEAPON_M4A1:
		case WEAPON_AUG:
		case WEAPON_SG552:
			profile.reactionDelay = 0.035f;
			profile.verticalPull = 0.0110f;
			profile.punchPitchScale = 0.0030f;
			profile.horizontalDrift = 0.0036f;
			profile.punchYawScale = 0.0024f;
			profile.maxVertical = 26.0f;
			profile.maxHorizontal = 16.0f;
			break;
		case WEAPON_GALIL:
		case WEAPON_FAMAS:
			profile.reactionDelay = 0.040f;
			profile.verticalPull = 0.0095f;
			profile.punchPitchScale = 0.0026f;
			profile.horizontalDrift = 0.0030f;
			profile.punchYawScale = 0.0020f;
			profile.maxVertical = 23.0f;
			profile.maxHorizontal = 14.0f;
			break;
		case WEAPON_M249:
			profile.reactionDelay = 0.055f;
			profile.verticalPull = 0.0140f;
			profile.punchPitchScale = 0.0032f;
			profile.horizontalDrift = 0.0048f;
			profile.punchYawScale = 0.0028f;
			profile.maxVertical = 34.0f;
			profile.maxHorizontal = 22.0f;
			break;
		case WEAPON_MP5N:
		case WEAPON_P90:
		case WEAPON_MAC10:
		case WEAPON_TMP:
		case WEAPON_UMP45:
			profile.reactionDelay = 0.040f;
			profile.verticalPull = 0.0060f;
			profile.punchPitchScale = 0.0014f;
			profile.horizontalDrift = 0.0018f;
			profile.punchYawScale = 0.0013f;
			profile.maxVertical = 14.0f;
			profile.maxHorizontal = 8.0f;
			break;
		}

		return profile;
	}
}

void CCSBot::UpdateRecoilControlState(bool firingControlledWeapon, float skill, float invSkill)
{
	const int weaponId = ActiveWeaponId(this);

	if (!firingControlledWeapon)
	{
		m_recoilControlOffset = AimLerp(m_recoilControlOffset, Vector(0, 0, 0), g_flBotCommandInterval * 7.0f);
		if (gpGlobals && gpGlobals->time - m_recoilControlLastFireTimestamp > 0.28f)
		{
			m_recoilControlStartTimestamp = 0.0f;
			m_recoilControlWeapon = WEAPON_NONE;
		}
		return;
	}

	if (m_recoilControlStartTimestamp <= 0.0f || weaponId != m_recoilControlWeapon || gpGlobals->time - m_recoilControlLastFireTimestamp > 0.22f)
	{
		m_recoilControlStartTimestamp = gpGlobals->time;
		m_nextRecoilControlUpdate = gpGlobals->time;
		m_recoilControlOffset = Vector(0, 0, 0);
		m_recoilControlSide = (RANDOM_LONG(0, 1) == 0) ? -1.0f : 1.0f;
		m_recoilControlWeapon = weaponId;
	}

	m_recoilControlLastFireTimestamp = gpGlobals->time;

	if (gpGlobals->time >= m_nextRecoilControlUpdate)
	{
		const float updateMin = 0.020f + invSkill * 0.016f;
		const float updateMax = 0.048f + invSkill * 0.042f;
		m_nextRecoilControlUpdate = gpGlobals->time + RANDOM_FLOAT(updateMin, updateMax);

		if (RANDOM_FLOAT(0.0f, 100.0f) < 16.0f + invSkill * 24.0f)
			m_recoilControlSide = -m_recoilControlSide;
		else if (RANDOM_FLOAT(0.0f, 100.0f) < 12.0f + skill * 8.0f)
			m_recoilControlSide *= RANDOM_FLOAT(0.65f, 1.25f);

		m_recoilControlSide = AimClamp(m_recoilControlSide, -1.6f, 1.6f);
	}
}

Vector CCSBot::GetRecoilControlOffset(const Vector &desiredAimSpot, bool firingControlledWeapon, float skill, float invSkill)
{
	if (!firingControlledWeapon || m_recoilControlStartTimestamp <= 0.0f)
		return m_recoilControlOffset;

	const RecoilControlProfile profile = GetRecoilControlProfile(m_recoilControlWeapon);
	const float range = AimClamp((desiredAimSpot - GetEyePosition()).Length(), 160.0f, 2400.0f);
	const float age = gpGlobals->time - m_recoilControlStartTimestamp;
	const float reactionDelay = profile.reactionDelay + invSkill * 0.055f;
	const float response = AimClamp((age - reactionDelay) / (0.24f + invSkill * 0.24f), 0.0f, 1.0f);
	const float lateSpray = AimClamp((age - 0.42f) / 1.05f, 0.0f, 1.0f);

	Vector toTarget = desiredAimSpot - pev->origin;
	Vector sideways(-toTarget.y, toTarget.x, 0.0f);
	sideways.NormalizeInPlace();

	const float pitchPunch = Q_abs(pev->punchangle.x);
	const float yawPunch = pev->punchangle.y;
	const float humanOverUnder = 0.82f + skill * 0.28f + BotSIN(gpGlobals->time * 11.0f + entindex()) * (0.08f + invSkill * 0.12f);

	Vector targetOffset = Vector(0, 0, 0);
	targetOffset.z -= AimClamp(range * (profile.verticalPull * response * humanOverUnder + pitchPunch * profile.punchPitchScale), 0.0f, profile.maxVertical);

	const float horizontalWave = BotSIN(age * (8.0f + skill * 2.0f) + entindex() * 0.37f);
	const float horizontalPush = 0.45f + lateSpray * 1.35f;
	float horizontal = range * profile.horizontalDrift * response * horizontalPush * horizontalWave * m_recoilControlSide;
	horizontal += range * profile.punchYawScale * yawPunch * (0.45f + response * 0.55f);
	horizontal = AimClamp(horizontal, -profile.maxHorizontal, profile.maxHorizontal);
	targetOffset = targetOffset + sideways * horizontal;

	const float settleRate = firingControlledWeapon ? (8.5f + skill * 5.0f) : 9.0f;
	m_recoilControlOffset = AimLerp(m_recoilControlOffset, targetOffset, g_flBotCommandInterval * settleRate);
	return m_recoilControlOffset;
}

void CCSBot::UpdateAimController()
{
	if (IsAimingAtEnemy())
		UpdateCombatAimController();
	else
		UpdateNonCombatAimController();

	// Refresh the LookTarget mirror from legacy m_lookAt* state; aim still comes from the existing combat/non-combat paths above.
	SyncLookTargetFromLegacyState();

	UpdateLookAngles();

	AimLog::RecordBotSample(this, m_aimSpot, IsAimingAtEnemy(), m_enemy);
	// Roam CSV "state" column labels (keep in sync with AimLog::RecordBotRoamSample documentation):
	const char *roamState = "roaming";
	if (IsUsingGrenade())
		roamState = "grenade";
	else if (IsHiding() || IsAtHidingSpot())
		roamState = "hiding";
	else if (IsMovingTo())
		roamState = "moving";
	else if (IsUsingKnife())
		roamState = "knife_run";
	AimLog::RecordBotRoamSample(this, roamState);
}

void CCSBot::UpdateCombatAimController()
{
	UpdateAimOffset();

	if (!m_enemy.IsValid())
		return;

	float feetOffset = pev->origin.z - GetFeetZ();
	const float skill = TheCSBots()->GetEffectiveSkill(this);
	const float invSkill = 1.0f - skill;
	const bool firingGun = IsAttacking() && !IsUsingGrenade() && !IsUsingKnife();
	const bool recoilControlledWeapon = firingGun && !IsUsingPistol() && !IsUsingSniperRifle() && !IsUsingShotgun();
	const bool firingRifle = recoilControlledWeapon && IsActiveWeaponRecoilHigh();
	Vector observedEnemyPosition = m_lastEnemyPosition;
	Vector observedEnemyVelocity = Vector(0, 0, 0);

	UpdateRecoilControlState(recoilControlledWeapon, skill, invSkill);

	if (IsEnemyVisible())
	{
		if (skill > 0.5f)
		{
			const float k = 3.0f;
			observedEnemyPosition = (m_enemy->pev->velocity - pev->velocity) * g_flBotCommandInterval * k + m_enemy->pev->origin;
		}
		else
			observedEnemyPosition = m_enemy->pev->origin;

		observedEnemyVelocity = m_enemy->pev->velocity;

		bool aimBlocked = false;
		const float sharpshooter = 0.8f;
		if (IsUsingAWP() || IsUsingShotgun() || IsUsingMachinegun() || skill < sharpshooter
			|| (IsActiveWeaponRecoilHigh() && !IsUsingPistol() && !IsUsingSniperRifle()))
		{
			if (IsEnemyPartVisible(CHEST))
				aimBlocked = true;
		}

		if (aimBlocked)
			observedEnemyPosition.z -= feetOffset * 0.25f;
		else if (!IsEnemyPartVisible(HEAD))
		{
			if (IsEnemyPartVisible(CHEST))
			{
				observedEnemyPosition.z -= feetOffset * 0.5f;
			}
			else if (IsEnemyPartVisible(LEFT_SIDE) || IsEnemyPartVisible(RIGHT_SIDE))
			{
				Vector2D to = (m_enemy->pev->origin - pev->origin).Make2D();
				to.NormalizeInPlace();
				const float side = IsEnemyPartVisible(LEFT_SIDE) ? -1.0f : 1.0f;
				observedEnemyPosition.x += side * to.y * 16.0f;
				observedEnemyPosition.y -= side * to.x * 16.0f;
				observedEnemyPosition.z -= feetOffset * 0.5f;
			}
			else
			{
				observedEnemyPosition.z -= (feetOffset + feetOffset);
			}
		}
	}

	const bool newPerceivedEnemy = (m_perceivedEnemyIndex != m_enemy->entindex()) || m_perceivedEnemyTimestamp == 0.0f;
	if (newPerceivedEnemy)
	{
		const float range = Q_max(160.0f, (observedEnemyPosition - GetEyePosition()).Length());
		m_perceivedEnemyPosition = observedEnemyPosition;
		m_perceivedEnemyVelocity = observedEnemyVelocity;
		m_aimInertiaSpot = CurrentViewSpot(this, range);
		m_perceivedEnemyTimestamp = gpGlobals->time;
		m_nextPerceivedEnemyUpdate = gpGlobals->time;
		m_aimInertiaTimestamp = gpGlobals->time;
		m_combatAimPhase = COMBAT_AIM_REACTING;
		m_combatAimPhaseTimestamp = gpGlobals->time;
		m_combatAimPhaseEndTimestamp = gpGlobals->time + RANDOM_FLOAT(0.055f + invSkill * 0.055f, 0.11f + invSkill * 0.12f);
		m_combatAimOvershoot = Vector(0, 0, 0);
		m_combatAimConfidence = 0.0f;
		m_perceivedEnemyIndex = m_enemy->entindex();
	}

	if (IsEnemyVisible() && gpGlobals->time >= m_nextPerceivedEnemyUpdate)
	{
		const float sampleInterval = RANDOM_FLOAT(0.045f + invSkill * 0.08f, 0.09f + invSkill * 0.16f);
		m_nextPerceivedEnemyUpdate = gpGlobals->time + sampleInterval;

		const float range = (observedEnemyPosition - pev->origin).Length();
		float snapshotError = range * (0.0025f + invSkill * 0.009f);
		if (IsActiveWeaponRecoilHigh() && !IsUsingPistol() && !IsUsingSniperRifle())
			snapshotError *= 1.6f;

		observedEnemyPosition.x += RANDOM_FLOAT(-snapshotError, snapshotError);
		observedEnemyPosition.y += RANDOM_FLOAT(-snapshotError, snapshotError);
		observedEnemyPosition.z += RANDOM_FLOAT(-snapshotError * 0.5f, snapshotError * 0.5f);

		m_perceivedEnemyPosition = observedEnemyPosition;
		m_perceivedEnemyVelocity = AimLerp(m_perceivedEnemyVelocity, observedEnemyVelocity, 0.55f + skill * 0.35f);
		m_perceivedEnemyTimestamp = gpGlobals->time;
	}
	else if (!IsEnemyVisible() && m_perceivedEnemyTimestamp > 0.0f)
	{
		const float memoryAge = gpGlobals->time - m_perceivedEnemyTimestamp;
		m_perceivedEnemyPosition = m_perceivedEnemyPosition + m_perceivedEnemyVelocity * AimClamp(g_flBotCommandInterval * (0.12f + skill * 0.18f), 0.0f, 0.018f);
		if (memoryAge > 0.30f)
			m_perceivedEnemyPosition = AimLerp(m_perceivedEnemyPosition, m_lastEnemyPosition, 0.06f);
	}

	Vector desiredAimSpot = m_perceivedEnemyPosition + m_aimOffset;

	if (!IsEnemyVisible() && m_perceivedEnemyTimestamp > 0.0f && gpGlobals->time - m_perceivedEnemyTimestamp > 0.12f)
	{
		if (m_combatAimPhase != COMBAT_AIM_LOST_TARGET)
		{
			m_combatAimPhase = COMBAT_AIM_LOST_TARGET;
			m_combatAimPhaseTimestamp = gpGlobals->time;
			m_combatAimPhaseEndTimestamp = gpGlobals->time + RANDOM_FLOAT(0.18f, 0.36f);
			m_combatAimOvershoot = Vector(0, 0, 0);
		}
	}
	else if (IsEnemyVisible() && m_combatAimPhase == COMBAT_AIM_LOST_TARGET)
	{
		m_perceivedEnemyPosition = observedEnemyPosition;
		m_perceivedEnemyVelocity = observedEnemyVelocity;
		m_perceivedEnemyTimestamp = gpGlobals->time;
		m_nextPerceivedEnemyUpdate = gpGlobals->time + RANDOM_FLOAT(0.025f, 0.055f + invSkill * 0.035f);
		desiredAimSpot = m_perceivedEnemyPosition + m_aimOffset;

		const Vector lag = desiredAimSpot - m_aimInertiaSpot;
		const float lagLength = lag.Length();
		Vector sideways(-lag.y, lag.x, 0.0f);
		sideways.NormalizeInPlace();

		const float side = (RANDOM_LONG(0, 1) == 0) ? -1.0f : 1.0f;
		m_combatAimOvershoot = lag * RANDOM_FLOAT(0.015f, 0.055f + invSkill * 0.035f);
		m_combatAimOvershoot = m_combatAimOvershoot + sideways * (side * AimClamp(lagLength * RANDOM_FLOAT(0.010f, 0.035f + invSkill * 0.020f), 0.0f, 10.0f + invSkill * 10.0f));
		m_combatAimOvershoot.z += RANDOM_FLOAT(-AimClamp(lagLength * 0.018f, 0.0f, 8.0f + invSkill * 8.0f), AimClamp(lagLength * 0.018f, 0.0f, 8.0f + invSkill * 8.0f));

		m_combatAimPhase = COMBAT_AIM_FLICKING;
		m_combatAimPhaseTimestamp = gpGlobals->time;
		m_combatAimPhaseEndTimestamp = gpGlobals->time + RANDOM_FLOAT(0.045f + invSkill * 0.015f, 0.085f + invSkill * 0.040f);
		m_combatAimConfidence = Q_max(m_combatAimConfidence, 0.35f + skill * 0.25f);
	}

	if (m_combatAimPhase == COMBAT_AIM_REACTING)
	{
		if (gpGlobals->time < m_combatAimPhaseEndTimestamp)
		{
			desiredAimSpot = AimLerp(m_aimInertiaSpot, desiredAimSpot, 0.05f + skill * 0.08f);
			m_combatAimConfidence = Q_max(0.0f, m_combatAimConfidence - g_flBotCommandInterval * 2.0f);
		}
		else
		{
			const Vector lag = desiredAimSpot - m_aimInertiaSpot;
			const float lagLength = lag.Length();
			Vector sideways(-lag.y, lag.x, 0.0f);
			sideways.NormalizeInPlace();

			const float side = (RANDOM_LONG(0, 1) == 0) ? -1.0f : 1.0f;
			m_combatAimOvershoot = lag * RANDOM_FLOAT(0.04f, 0.12f + invSkill * 0.07f);
			m_combatAimOvershoot = m_combatAimOvershoot + sideways * (side * AimClamp(lagLength * RANDOM_FLOAT(0.025f, 0.075f + invSkill * 0.04f), 0.0f, 24.0f + invSkill * 20.0f));
			m_combatAimOvershoot.z += RANDOM_FLOAT(-AimClamp(lagLength * 0.035f, 0.0f, 14.0f + invSkill * 12.0f), AimClamp(lagLength * 0.035f, 0.0f, 14.0f + invSkill * 12.0f));
			m_combatAimPhase = COMBAT_AIM_FLICKING;
			m_combatAimPhaseTimestamp = gpGlobals->time;
			m_combatAimPhaseEndTimestamp = gpGlobals->time + RANDOM_FLOAT(0.08f + invSkill * 0.03f, 0.15f + invSkill * 0.07f);
		}
	}

	if (m_combatAimPhase == COMBAT_AIM_FLICKING)
	{
		float phaseLen = Q_max(0.01f, m_combatAimPhaseEndTimestamp - m_combatAimPhaseTimestamp);
		float t = AimClamp((gpGlobals->time - m_combatAimPhaseTimestamp) / phaseLen, 0.0f, 1.0f);
		desiredAimSpot = desiredAimSpot + m_combatAimOvershoot * (0.35f + 0.65f * t);
		m_combatAimConfidence = Q_min(0.68f, m_combatAimConfidence + g_flBotCommandInterval * (1.8f + skill));

		if (t >= 1.0f)
		{
			m_combatAimPhase = COMBAT_AIM_CORRECTING;
			m_combatAimPhaseTimestamp = gpGlobals->time;
			m_combatAimPhaseEndTimestamp = gpGlobals->time + RANDOM_FLOAT(0.12f + invSkill * 0.04f, 0.26f + invSkill * 0.10f);
		}
	}

	if (m_combatAimPhase == COMBAT_AIM_CORRECTING)
	{
		float phaseLen = Q_max(0.01f, m_combatAimPhaseEndTimestamp - m_combatAimPhaseTimestamp);
		float t = AimClamp((gpGlobals->time - m_combatAimPhaseTimestamp) / phaseLen, 0.0f, 1.0f);
		desiredAimSpot = desiredAimSpot + m_combatAimOvershoot * (1.0f - t);
		m_combatAimConfidence = Q_min(0.78f, m_combatAimConfidence + g_flBotCommandInterval * (1.6f + skill));

		if (t >= 1.0f)
		{
			m_combatAimPhase = firingRifle ? COMBAT_AIM_RECOIL_CONTROL : COMBAT_AIM_TRACKING;
			m_combatAimPhaseTimestamp = gpGlobals->time;
			m_combatAimPhaseEndTimestamp = gpGlobals->time;
			m_combatAimOvershoot = Vector(0, 0, 0);
		}
	}

	if (m_combatAimPhase == COMBAT_AIM_TRACKING && firingRifle)
	{
		m_combatAimPhase = COMBAT_AIM_RECOIL_CONTROL;
		m_combatAimPhaseTimestamp = gpGlobals->time;
	}
	else if (m_combatAimPhase == COMBAT_AIM_RECOIL_CONTROL && !firingRifle)
	{
		m_combatAimPhase = COMBAT_AIM_TRACKING;
		m_combatAimPhaseTimestamp = gpGlobals->time;
	}

	if (m_combatAimPhase == COMBAT_AIM_TRACKING || m_combatAimPhase == COMBAT_AIM_RECOIL_CONTROL)
		m_combatAimConfidence = Q_min(1.0f, m_combatAimConfidence + g_flBotCommandInterval * (1.2f + skill));
	else if (m_combatAimPhase == COMBAT_AIM_LOST_TARGET)
		m_combatAimConfidence = Q_max(0.0f, m_combatAimConfidence - g_flBotCommandInterval * 2.5f);

	if (gpGlobals->time >= m_nextAimJitterUpdate)
	{
		const float range = (desiredAimSpot - pev->origin).Length();
		float jitter = range * (0.0006f + invSkill * 0.0020f);
		if (IsViewMoving(80.0f))
			jitter *= 1.8f;
		jitter *= 1.18f - 0.36f * m_combatAimConfidence;

		const float horizontalJitterScale = firingRifle ? 0.30f : (firingGun ? 0.42f : 1.0f);
		const float verticalJitterScale = firingRifle ? 5.40f : (firingGun ? 2.35f : 0.70f);
		m_aimHandJitter.x = RANDOM_FLOAT(-jitter * horizontalJitterScale, jitter * horizontalJitterScale);
		m_aimHandJitter.y = RANDOM_FLOAT(-jitter * horizontalJitterScale, jitter * horizontalJitterScale);
		m_aimHandJitter.z = RANDOM_FLOAT(-jitter * verticalJitterScale, jitter * verticalJitterScale);
		m_nextAimJitterUpdate = gpGlobals->time + RANDOM_FLOAT(0.035f, 0.11f + invSkill * 0.08f);
	}

	Vector correction = desiredAimSpot - m_aimInertiaSpot;
	const float correctionLength = correction.Length();
	if (gpGlobals->time >= m_nextAimCurveUpdate)
	{
		if (correctionLength > 8.0f)
		{
			Vector sideways(-correction.y, correction.x, 0.0f);
			sideways.NormalizeInPlace();

			float curveAmount = AimClamp(correctionLength * RANDOM_FLOAT(0.025f, 0.11f + invSkill * 0.04f), 0.0f, 26.0f + invSkill * 18.0f);
			if (firingGun)
				curveAmount *= firingRifle ? 0.28f : 0.38f;

			const float side = (RANDOM_LONG(0, 1) == 0) ? -1.0f : 1.0f;
			m_aimCurveOffset = sideways * (curveAmount * side);

			const float verticalCurveChance = firingGun ? (firingRifle ? 92.0f : 72.0f) : (22.0f + invSkill * 18.0f);
			const float verticalCurveScale = firingRifle ? 3.10f : (firingGun ? 1.30f : 0.55f);
			if (RANDOM_FLOAT(0.0f, 100.0f) < verticalCurveChance)
				m_aimCurveOffset.z += RANDOM_FLOAT(-curveAmount * verticalCurveScale, curveAmount * verticalCurveScale);
		}
		else
		{
			m_aimCurveOffset = Vector(0, 0, 0);
		}

		m_nextAimCurveUpdate = gpGlobals->time + RANDOM_FLOAT(0.08f, 0.24f + invSkill * 0.10f);
	}

	m_aimCurveOffset = AimLerp(m_aimCurveOffset, Vector(0, 0, 0), g_flBotCommandInterval * RANDOM_FLOAT(3.5f, 7.0f));
	desiredAimSpot = desiredAimSpot + m_aimHandJitter + m_aimCurveOffset;
	desiredAimSpot = desiredAimSpot + GetRecoilControlOffset(desiredAimSpot, recoilControlledWeapon, skill, invSkill);

	Vector toTarget = desiredAimSpot - pev->origin;
	const float targetRange = toTarget.Length();
	Vector sideways(-toTarget.y, toTarget.x, 0.0f);
	sideways.NormalizeInPlace();

	const float marginScale = 0.0012f + invSkill * 0.0020f;
	const float phase = gpGlobals->time * (firingGun ? (firingRifle ? 17.0f : 13.0f) : 8.5f) + entindex() * 0.91f;
	const float horizontalMargin = AimClamp(targetRange * marginScale, 0.75f, firingRifle ? 3.5f : (firingGun ? 4.8f : 8.0f));
	const float verticalMargin = AimClamp(targetRange * marginScale * (firingRifle ? 4.20f : (firingGun ? 1.85f : 1.10f)), 0.65f, firingRifle ? 16.0f : (firingGun ? 8.5f : 6.0f));
	desiredAimSpot = desiredAimSpot + sideways * (horizontalMargin * BotSIN(phase));
	desiredAimSpot.z += verticalMargin * BotCOS(phase * 0.77f);

	if (firingGun)
	{
		const float range = (desiredAimSpot - pev->origin).Length();
		const float verticalWave = BotSIN(gpGlobals->time * (firingRifle ? 42.0f : 34.0f) + entindex() * 0.73f);
		const float verticalScale = firingRifle ? 0.0095f : 0.0026f;
		desiredAimSpot.z += range * verticalScale * verticalWave;
	}

	float deltaT = gpGlobals->time - m_aimInertiaTimestamp;
	if (deltaT < 0.0f || deltaT > 0.25f)
		deltaT = g_flBotCommandInterval;

	m_aimInertiaTimestamp = gpGlobals->time;

	const float acquireAge = gpGlobals->time - m_currentEnemyAcquireTimestamp;
	const float preAimLag = (desiredAimSpot - m_aimInertiaSpot).Length();
	float followRate = 5.5f + skill * 7.0f;
	if (m_combatAimPhase == COMBAT_AIM_REACTING)
		followRate *= 0.10f + skill * 0.10f;
	else if (m_combatAimPhase == COMBAT_AIM_FLICKING)
		followRate *= 2.15f + skill * 1.10f;
	else if (m_combatAimPhase == COMBAT_AIM_CORRECTING)
		followRate *= 1.10f + skill * 0.45f;
	else if (m_combatAimPhase == COMBAT_AIM_RECOIL_CONTROL)
		followRate *= 0.72f + skill * 0.26f;
	else if (m_combatAimPhase == COMBAT_AIM_LOST_TARGET)
		followRate *= 0.18f + skill * 0.08f;
	else if (acquireAge < 0.42f)
		followRate *= 1.0f + AimClamp((preAimLag - 45.0f) / 210.0f, 0.0f, 0.75f);

	if (IsActiveWeaponRecoilHigh() && !IsUsingPistol() && !IsUsingSniperRifle())
		followRate *= (m_combatAimPhase == COMBAT_AIM_RECOIL_CONTROL) ? 1.05f : 0.78f;

	if (gpGlobals->time >= m_nextAimMotorUpdate)
	{
		m_aimHumanMotorScale = RANDOM_FLOAT(0.62f, 1.22f + skill * 0.28f);

		const float lag = (desiredAimSpot - m_aimInertiaSpot).Length();
		if (m_combatAimPhase == COMBAT_AIM_FLICKING)
			m_aimHumanMotorScale = RANDOM_FLOAT(1.80f, 2.75f + skill * 0.45f);
		else if (m_combatAimPhase == COMBAT_AIM_CORRECTING)
			m_aimHumanMotorScale = RANDOM_FLOAT(0.86f, 1.34f + skill * 0.24f);
		else if (m_combatAimPhase == COMBAT_AIM_RECOIL_CONTROL)
			m_aimHumanMotorScale = RANDOM_FLOAT(0.82f, 1.18f + skill * 0.24f);
		else if (lag > 80.0f && RANDOM_FLOAT(0.0f, 100.0f) < 28.0f + skill * 22.0f)
			m_aimHumanMotorScale = RANDOM_FLOAT(1.35f, 2.05f);
		if (m_combatAimPhase == COMBAT_AIM_TRACKING && acquireAge > 0.07f && acquireAge < 0.42f && lag > 55.0f)
			m_aimHumanMotorScale = Q_max(m_aimHumanMotorScale, RANDOM_FLOAT(1.55f, 2.55f));

		m_nextAimMotorUpdate = gpGlobals->time + RANDOM_FLOAT(0.055f, 0.18f + invSkill * 0.10f);
	}

	followRate *= m_aimHumanMotorScale;
	m_aimInertiaSpot = AimLerp(m_aimInertiaSpot, desiredAimSpot, deltaT * followRate);
	m_aimSpot = m_aimInertiaSpot;

	Vector toEnemy = m_aimSpot - pev->origin;
	Vector idealAngle = UTIL_VecToAngles(toEnemy);

	idealAngle.x = 360.0 - idealAngle.x;
	if (firingGun)
	{
		const float yawFollow = firingRifle ? 0.36f : 0.62f;
		float phaseYawFollow = yawFollow;
		if (m_combatAimPhase == COMBAT_AIM_FLICKING)
			phaseYawFollow = Q_max(phaseYawFollow, 0.82f);
		else if (m_combatAimPhase == COMBAT_AIM_CORRECTING)
			phaseYawFollow = Q_max(phaseYawFollow, 0.62f);
		else if (m_combatAimPhase == COMBAT_AIM_RECOIL_CONTROL)
			phaseYawFollow = Q_max(phaseYawFollow, 0.54f);

		idealAngle.y = pev->v_angle.y + NormalizeAngle(idealAngle.y - pev->v_angle.y) * phaseYawFollow;
	}

	m_aimDiagJitterMag = m_aimHandJitter.Length();
	m_aimDiagAimOffsetMag = m_aimOffset.Length();
	m_aimDiagRecoilMag = m_recoilControlOffset.Length();
	m_aimDiagDistEnemy = (m_enemy->pev->origin - pev->origin).Length();

	SetLookAngles(idealAngle.y, idealAngle.x);
}

// Non-combat aim: path default (m_lookAheadAngle / m_lookAheadPitch) and/or look-at spots
// (m_lookAtSpot state + UpdateLookAt). Roaming coherent drift at end: NonCombatAimDrift::ApplySinusoidalDriftToDesiredLook;
// combat aim does not use this tail. Extra sampled micro-offsets: NonCombatAimDrift::ApplySampledMicroOffsetsBeforeMotor in UpdateLookAngles.
void CCSBot::UpdateNonCombatAimController()
{
	if (m_lookAtSpotClearIfClose)
	{
		const float tooCloseRange = 100.0f;
		if ((m_lookAtSpot - pev->origin).IsLengthLessThan(tooCloseRange))
			m_lookAtSpotState = NOT_LOOKING_AT_SPOT;
	}

	switch (m_lookAtSpotState)
	{
	case NOT_LOOKING_AT_SPOT:
	{
		float usePitch = m_lookAheadPitch;
		if (!IsNotMoving() && !IsUsingLadder())
			usePitch = Q_max(-7.5f, Q_min(usePitch, 3.5f));

		float useYaw = m_lookAheadAngle;
		// Path lookahead is the default attractor here; during an idle look commit (perturbed camp POI etc.)
		// that remains stronger than momentary NOT_LOOKING gaps (clear-if-close, LOS, timers). Using
		// m_lookAheadAngle every tick re-snaps to "middle" and drives twitch-to-center / A↔B ping-pong.
		if (IsNotMoving() && !IsUsingLadder() && IsIdleLookCommitActive() && !ShouldOverrideIdleLookCommit())
			useYaw = m_lastIdleLookYaw;

		SetLookAngles(useYaw, usePitch);
		break;
	}
	case LOOK_TOWARDS_SPOT:
	{
		if (m_lookAtSpotPriority <= PRIORITY_LOW && !IsNotMoving() && gpGlobals->time - m_lookAtSpotTimestamp > 0.55f)
		{
			m_lookAtSpotState = NOT_LOOKING_AT_SPOT;
			break;
		}

		if (!UpdateLookAt())
			break;

		if (IsLookingAtPosition(&m_lookAtSpot, m_lookAtSpotAngleTolerance))
		{
			m_lookAtSpotState = LOOK_AT_SPOT;
			m_lookAtSpotTimestamp = gpGlobals->time;
		}
		break;
	}
	case LOOK_AT_SPOT:
	{
		if (!UpdateLookAt())
			break;

		if (m_lookAtSpotPriority <= PRIORITY_LOW && !IsNotMoving() && gpGlobals->time - m_lookAtSpotTimestamp > 0.35f)
		{
			m_lookAtSpotState = NOT_LOOKING_AT_SPOT;
			m_lookAtSpotDuration = 0.0f;
			break;
		}

		if (m_lookAtSpotDuration >= 0.0f && gpGlobals->time - m_lookAtSpotTimestamp > m_lookAtSpotDuration)
		{
			// Idle look commitment can outlast SetLookAt duration (e.g. perturbed camp POI). Ending LOOK_AT_SPOT
			// here would fall through to NOT_LOOKING_AT_SPOT; without defer, that path used to always take path
			// lookahead yaw and snap back to middle before commit expires — visible micro twitch-to-center.
			// (NOT_LOOKING now holds m_lastIdleLookYaw while commit is active.) Defer expiry until commit ends
			// or stimulus override (ShouldOverrideIdleLookCommit). Does not affect PRIORITY_HIGH / combat.
			const bool deferSpotEndForIdleCommit =
				m_lookAtSpotPriority <= PRIORITY_MEDIUM && IsIdleLookCommitActive() && !ShouldOverrideIdleLookCommit();

			if (!deferSpotEndForIdleCommit)
			{
				m_lookAtSpotState = NOT_LOOKING_AT_SPOT;
				m_lookAtSpotDuration = 0.0f;
			}
		}
		break;
	}
	}

	NonCombatAimDrift::ApplySinusoidalDriftToDesiredLook(this);
}
