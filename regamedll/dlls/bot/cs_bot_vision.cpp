/*
*
*   This program is free software; you can redistribute it and/or modify it
*   under the terms of the GNU General Public License as published by the
*   Free Software Foundation; either version 2 of the License, or (at
*   your option) any later version.
*
*   This program is distributed in the hope that it will be useful, but
*   WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*   General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software Foundation,
*   Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*   In addition, as a special exception, the author gives permission to
*   link the code of this program with the Half-Life Game Engine ("HL
*   Engine") and Modified Game Libraries ("MODs") developed by Valve,
*   L.L.C ("Valve").  You must obey the GNU General Public License in all
*   respects for all of the code used other than the HL Engine and MODs
*   from Valve.  If you modify this file, you may extend this exception
*   to your version of the file, but you are not obligated to do so.  If
*   you do not wish to do so, delete this exception statement from your
*   version.
*
*/

#include "precompiled.h"
#include "aim_log.h"
#include "bot/cs_bot_camp_look_spots.h"
#include "bot/cs_bot_danger_memory.h"
#include "bot/cs_bot_look_target_selector.h"
#include "bot/cs_bot_movement_debug.h"
#include "ai_chat.h"

extern cvar_t sv_aim_log_enable;

// Used to update view angles to stay on a ladder
float StayOnLadderLine(CCSBot *me, const CNavLadder *ladder)
{
	// determine our facing
	NavDirType faceDir = AngleToDirection(me->pev->v_angle.y);
	const float stiffness = 1.0f;

	// move toward ladder mount point
	switch (faceDir)
	{
	case NORTH:
		return (stiffness * (ladder->m_top.x - me->pev->origin.x));
	case EAST:
		return (stiffness * (ladder->m_top.y - me->pev->origin.y));
	case SOUTH:
		return (-stiffness * (ladder->m_top.x - me->pev->origin.x));
	case WEST:
		return (-stiffness * (ladder->m_top.y - me->pev->origin.y));
	}

	return 0.0f;
}

static bool FindOpenLookLane(CCSBot *bot, Vector *spot)
{
	if (!bot || !spot)
		return false;

	TraceResult bestResult;
	float bestRange = 0.0f;

	for (float angle = 0.0f; angle < 360.0f; angle += 30.0f)
	{
		Vector end = bot->GetEyePosition() + 1200.0f * Vector(BotCOS(angle), BotSIN(angle), 0.0f);
		TraceResult result;
		UTIL_TraceLine(bot->GetEyePosition(), end, ignore_monsters, ignore_glass, ENT(bot->pev), &result);

		const float range = (result.vecEndPos - bot->GetEyePosition()).Length2D();
		if (range > bestRange)
		{
			bestRange = range;
			bestResult = result;
		}
	}

	if (bestRange < 320.0f)
		return false;

	*spot = bestResult.vecEndPos;
	spot->z = bot->GetEyePosition().z;
	return true;
}

static bool IsLookingIntoCloseGeometry(CCSBot *bot)
{
	if (!bot)
		return false;

	const float checkRange = 260.0f;
	const Vector eye = bot->GetEyePosition();
	const float pitch = bot->pev->v_angle.x;
	const float flatRange = checkRange * BotCOS(pitch);
	const Vector forward(flatRange * BotCOS(bot->pev->v_angle.y), flatRange * BotSIN(bot->pev->v_angle.y), -checkRange * BotSIN(pitch));

	TraceResult result;
	UTIL_TraceLine(eye, eye + forward, ignore_monsters, ignore_glass, ENT(bot->pev), &result);

	if (result.fStartSolid || result.flFraction >= 1.0f)
		return false;

	const float hitRange = (result.vecEndPos - eye).Length();
	if (hitRange > checkRange)
		return false;

	// Floor/ceiling glances can be intentional terrain awareness; near-vertical surfaces are wall stares.
	return result.vecPlaneNormal.z < 0.65f && hitRange < 220.0f;
}

// Tighter threshold than IsLookingIntoCloseGeometry — only true for an oppressive near-wall stare.
static bool IsLookingIntoExtremeCloseGeometry(CCSBot *bot)
{
	if (!bot)
		return false;

	const float checkRange = 260.0f;
	const Vector eye = bot->GetEyePosition();
	const float pitch = bot->pev->v_angle.x;
	const float flatRange = checkRange * BotCOS(pitch);
	const Vector forward(flatRange * BotCOS(bot->pev->v_angle.y), flatRange * BotSIN(bot->pev->v_angle.y), -checkRange * BotSIN(pitch));

	TraceResult result;
	UTIL_TraceLine(eye, eye + forward, ignore_monsters, ignore_glass, ENT(bot->pev), &result);

	if (result.fStartSolid || result.flFraction >= 1.0f)
		return false;

	const float hitRange = (result.vecEndPos - eye).Length();
	if (hitRange > checkRange)
		return false;

	return result.vecPlaneNormal.z < 0.65f && hitRange < 95.0f;
}

namespace
{

void CampEdgeTraceHoriz(const CCSBot *bot, const Vector &eye, float yawDeg, float maxRay, float *dist2d, float *frac)
{
	const float y = NormalizeAngle(yawDeg);
	const Vector end = eye + maxRay * Vector(BotCOS(y), BotSIN(y), 0.0f);
	TraceResult tr;
	UTIL_TraceLine(eye, end, ignore_monsters, ignore_glass, ENT(bot->pev), &tr);
	*frac = tr.flFraction;
	*dist2d = (tr.vecEndPos - eye).Length2D();
}

enum class StationaryWallLookQuality : uint8_t
{
	WALL_LOOK_CLEAR,
	WALL_LOOK_TIGHT_ANGLE,
	WALL_LOOK_BAD_STARE
};

struct StationaryWallLookTelemetry
{
	float centerDist{};
	float minDist{};
	float maxDist{};
	int blockedCount{};
	int openCount{};
	float blockedPct{};
	float asymmetry{};
};

static const char *StationaryWallLookQualityString(StationaryWallLookQuality q)
{
	switch (q)
	{
	case StationaryWallLookQuality::WALL_LOOK_CLEAR:
		return "CLEAR";
	case StationaryWallLookQuality::WALL_LOOK_TIGHT_ANGLE:
		return "TIGHT_ANGLE";
	case StationaryWallLookQuality::WALL_LOOK_BAD_STARE:
		return "BAD_STARE";
	default:
		return "?";
	}
}

// Horizontal cone sample at current view yaw (stationary hide / camp wall-stare arbitration).
static StationaryWallLookQuality ClassifyStationaryWallLook(CCSBot *bot, StationaryWallLookTelemetry *tel)
{
	if (tel)
	{
		tel->centerDist = 0.0f;
		tel->minDist = 0.0f;
		tel->maxDist = 0.0f;
		tel->blockedCount = 0;
		tel->openCount = 0;
		tel->blockedPct = 0.0f;
		tel->asymmetry = 0.0f;
	}

	if (!bot || !bot->pev)
		return StationaryWallLookQuality::WALL_LOOK_CLEAR;

	const Vector eye = bot->GetEyePosition();
	const float baseYaw = bot->pev->v_angle.y;
	constexpr float kRay = 700.0f;
	static const float kYawOff[7] = { 0.0f, -4.0f, 4.0f, -8.0f, 8.0f, -12.0f, 12.0f };

	float dist[7];
	for (int i = 0; i < 7; ++i)
	{
		float frac = 0.0f;
		CampEdgeTraceHoriz(bot, eye, baseYaw + kYawOff[i], kRay, &dist[i], &frac);
		(void)frac;
	}

	float minD = dist[0];
	float maxD = dist[0];
	int blocked = 0;
	int open = 0;
	for (int i = 0; i < 7; ++i)
	{
		minD = Q_min(minD, dist[i]);
		maxD = Q_max(maxD, dist[i]);
		if (dist[i] < 96.0f)
			blocked++;
		if (dist[i] > 360.0f)
			open++;
	}

	const float center = dist[0];
	const float blockedPct = blocked / 7.0f;
	const float asym = maxD - minD;

	if (tel)
	{
		tel->centerDist = center;
		tel->minDist = minD;
		tel->maxDist = maxD;
		tel->blockedCount = blocked;
		tel->openCount = open;
		tel->blockedPct = blockedPct;
		tel->asymmetry = asym;
	}

	if (center >= 160.0f)
		return StationaryWallLookQuality::WALL_LOOK_CLEAR;

	if (open > 0 && asym >= 220.0f)
		return StationaryWallLookQuality::WALL_LOOK_TIGHT_ANGLE;

	if (blockedPct >= 0.70f && maxD < 360.0f)
		return StationaryWallLookQuality::WALL_LOOK_BAD_STARE;

	return StationaryWallLookQuality::WALL_LOOK_TIGHT_ANGLE;
}

} // namespace

bool CCSBot::AdjustStationaryCampLookForEdgeBias(Vector *inOutAimWorld, CampEdgeBiasTelemetry *tel) const
{
	if (tel)
	{
		tel->baseYaw = 0.0f;
		tel->finalYaw = 0.0f;
		tel->yawOffset = 0.0f;
		tel->baseTraceDist = 0.0f;
		tel->finalTraceDist = 0.0f;
		tel->edgeScoreBase = 0.0f;
		tel->edgeScoreBest = 0.0f;
		tel->applied = false;
		tel->rejectReason[0] = '\0';
	}

	if (!inOutAimWorld || !pev || !gpGlobals)
		return false;

	const Vector eye = GetEyePosition();
	const Vector toBase = *inOutAimWorld - eye;
	const float len2d = toBase.Length2D();
	if (len2d < 1.0f)
	{
		if (tel)
			Q_strncpy(tel->rejectReason, "closeGeometry", sizeof(tel->rejectReason));
		return false;
	}

	const float baseYaw = UTIL_VecToYaw(toBase);

	constexpr float kMaxRay = 1200.0f;
	constexpr float kMinFrac = 0.45f;
	constexpr float kNeighborStep = 6.0f;
	static const int kYawOffsets[7] = { 0, -18, -12, -8, 8, 12, 18 };

	float dist[7];
	float frac[7];
	for (int i = 0; i < 7; ++i)
		CampEdgeTraceHoriz(this, eye, baseYaw + static_cast<float>(kYawOffsets[i]), kMaxRay, &dist[i], &frac[i]);

	bool valid[7];
	for (int i = 0; i < 7; ++i)
		valid[i] = (frac[i] >= kMinFrac - 0.001f);

	float scores[7];
	float maxAsym = 0.0f;

	for (int i = 0; i < 7; ++i)
	{
		if (!valid[i])
		{
			scores[i] = -1.0e9f;
			continue;
		}

		const float y = NormalizeAngle(baseYaw + static_cast<float>(kYawOffsets[i]));
		float dl, fl, dr, fr;
		CampEdgeTraceHoriz(this, eye, y - kNeighborStep, kMaxRay, &dl, &fl);
		CampEdgeTraceHoriz(this, eye, y + kNeighborStep, kMaxRay, &dr, &fr);
		if (fl < 0.35f)
			dl *= 0.5f;
		if (fr < 0.35f)
			dr *= 0.5f;

		const float neighborDelta = Q_fabs(dl - dr);
		maxAsym = Q_max(maxAsym, neighborDelta);

		const float visibleDistance = dist[i];
		const float closeWallPenalty = (visibleDistance < 110.0f) ? 400.0f : 0.0f;
		float edgeScore = visibleDistance * 0.4f + neighborDelta * 0.8f - closeWallPenalty;

		if (dl < dr - 30.0f && kYawOffsets[i] < 0)
			edgeScore += 10.0f;
		else if (dr < dl - 30.0f && kYawOffsets[i] > 0)
			edgeScore += 10.0f;

		if (kYawOffsets[i] == 0)
		{
			float maxOtherQuick = -1.0e9f;
			for (int j = 0; j < 7; ++j)
			{
				if (j == i || !valid[j])
					continue;
				maxOtherQuick = Q_max(maxOtherQuick, dist[j] * 0.4f + Q_fabs(dist[j] - dist[i]) * 0.15f);
			}
			if (maxOtherQuick > edgeScore + 20.0f)
				edgeScore -= 28.0f;
		}

		scores[i] = edgeScore;
	}

	bool anyValid = false;
	for (int i = 0; i < 7; ++i)
	{
		if (valid[i])
			anyValid = true;
	}

	if (!anyValid)
	{
		if (tel)
		{
			tel->baseYaw = baseYaw;
			tel->finalYaw = baseYaw;
			Q_strncpy(tel->rejectReason, "lowTrace", sizeof(tel->rejectReason));
		}
		return false;
	}

	int bestIdx = 0;
	for (int i = 1; i < 7; ++i)
	{
		if (scores[i] > scores[bestIdx])
			bestIdx = i;
	}

	const float baseScore = scores[0];
	const float bestScore = scores[bestIdx];

	const float margin = Q_max(35.0f, 0.14f * ((baseScore > -1.0e8f) ? baseScore : 100.0f));

	bool shouldApply = false;
	if (bestIdx != 0 && valid[bestIdx] && bestScore >= baseScore + margin)
		shouldApply = true;
	if (baseScore < -1.0e8f && bestIdx != 0 && valid[bestIdx] && bestScore > 55.0f)
		shouldApply = true;

	if (tel)
	{
		tel->baseYaw = baseYaw;
		tel->edgeScoreBase = (valid[0]) ? baseScore : 0.0f;
		tel->edgeScoreBest = bestScore;
		tel->baseTraceDist = valid[0] ? dist[0] : 0.0f;
		tel->finalYaw = baseYaw;
		tel->finalTraceDist = tel->baseTraceDist;
		tel->yawOffset = 0.0f;
	}

	if (!shouldApply)
	{
		if (tel)
		{
			if (maxAsym < 22.0f)
				Q_strncpy(tel->rejectReason, "noEdgeDelta", sizeof(tel->rejectReason));
			else
				Q_strncpy(tel->rejectReason, "noScoreImprovement", sizeof(tel->rejectReason));
		}
		return false;
	}

	const float yBest = NormalizeAngle(baseYaw + static_cast<float>(kYawOffsets[bestIdx]));
	const Vector aimDir(BotCOS(yBest), BotSIN(yBest), 0.0f);
	TraceResult trFinal;
	UTIL_TraceLine(eye, eye + kMaxRay * aimDir, ignore_monsters, ignore_glass, ENT(pev), &trFinal);
	if (trFinal.flFraction < kMinFrac - 0.001f)
	{
		if (tel)
		{
			tel->finalYaw = yBest;
			Q_strncpy(tel->rejectReason, "lowTrace", sizeof(tel->rejectReason));
		}
		return false;
	}

	// Use the trace impact as the aim point (including Z). Forcing the old approach
	// Z onto a new XY from a nearer/different hit makes eye→target nearly vertical
	// (large pitch / sky stare) when horizontal distance shrinks but Δz stays large.
	Vector newPos = trFinal.vecEndPos;
	*inOutAimWorld = newPos;

	if (tel)
	{
		tel->finalYaw = yBest;
		tel->yawOffset = NormalizeAngle(yBest - baseYaw);
		tel->finalTraceDist = (trFinal.vecEndPos - eye).Length2D();
		tel->applied = true;
		tel->rejectReason[0] = '\0';
	}

	return true;
}

static void EmitCampLookCandidateLine(CCSBot *bot, const char *srcTag, const CCSBot::CampEdgeBiasTelemetry &tel)
{
	if (!bot || !srcTag)
		return;

	char buf[384];
	Q_snprintf(buf, sizeof(buf),
		"[campLookCandidate] bot=%s ent=%i src=%s baseYaw=%.2f finalYaw=%.2f off=%.2f baseDist=%.0f finalDist=%.0f scBase=%.1f scBest=%.1f applied=%i rej=%s",
		bot->pev ? STRING(bot->pev->netname) : "?",
		bot->entindex(),
		srcTag,
		tel.baseYaw, tel.finalYaw, tel.yawOffset,
		tel.baseTraceDist, tel.finalTraceDist,
		tel.edgeScoreBase, tel.edgeScoreBest,
		tel.applied ? 1 : 0,
		tel.rejectReason[0] ? tel.rejectReason : "-");

	EmitMovementWatchedDebugLine(bot, buf);
}

// HumanAimMotorController: spring/damper integration that historically lived inline in CCSBot::UpdateLookAngles.
// This is not a competing final motor — UpdateLookAngles remains the sole orchestrator and the only place that
// applies this step to pev->v_angle (after ladder / non-combat offset / bleed preprocessing).
namespace HumanAimMotorController
{

struct AimMotorParams
{
	float stiffness;
	float damping;
	float maxAccel;
};

inline AimMotorParams SelectBaseParams(bool isAttacking, bool isAimingAtEnemy, float aimHumanMotorScale)
{
	AimMotorParams p{};
	if (isAttacking)
	{
		p.stiffness = 220.0f;
		p.damping = 26.0f;
		p.maxAccel = 2200.0f;

		if (isAimingAtEnemy)
		{
			p.stiffness *= Q_max(0.75f, Q_min(aimHumanMotorScale, 1.35f));
			p.maxAccel *= Q_max(0.80f, Q_min(aimHumanMotorScale, 1.50f));
		}
	}
	else
	{
		p.stiffness = 125.0f;
		p.damping = 24.0f;
		p.maxAccel = 1350.0f;
	}

	return p;
}

inline void ApplySpringDamperToViewAngles(
	entvars_t *pev,
	float deltaT,
	float useYaw,
	float usePitch,
	float stiffness,
	float damping,
	float maxAccel,
	bool firingGun,
	bool acquisitionBurst,
	bool isAimingAtEnemy,
	float *lookYawVel,
	float *lookPitchVel,
	CCSBot *motorDebugSubject,
	AimMotorStepDiag *outDiag)
{
	const float yawStart = pev->v_angle.y;
	const float pitchStart = pev->v_angle.x;
	const float yawErrBefore = NormalizeAngle(useYaw - yawStart);

	float yawAccelApplied = 0.0f;
	bool microYawSnap = false;
	int directAngleWriteReason = AimMotorDirectWrite_None;

	const float yawVelPreGlobal = *lookYawVel;
	const float pitchVelPreGlobal = *lookPitchVel;

	// Forensic intermediates (read when sv_aim_log_enable; must mirror live math exactly).
	float yawAccelRawVal = 0.0f;
	float yawAccelClampVal = 0.0f;
	float yawVelPostIntVal = 0.0f;
	float yawStepRawVal = 0.0f;
	bool staleYawClearedVel = false;

	// Yaw
	float angleDiff = yawErrBefore;
	const float yawMaxAccel = maxAccel * (acquisitionBurst ? 1.65f : (firingGun ? 0.70f : 1.0f));

	// Enemy combat yaw: previously hard-snapped within ~0.08° (direct pev write + zero vel). Now only
	// a tiny epsilon uses hard snap; (epsilon, legacy outer] runs unchanged spring-damper (softer close).
	const float kYawEnemyHardSnapEpsDeg = 0.02f;
	const float kYawEnemyLegacySnapOuterDeg = 0.08f;
	// Non-combat / idle: only this tiny epsilon may hard-set yaw (float noise). The old ±1° direct-write band
	// is retired — (epsilon, ~1°+] is closed by the spring-damper like combat's softened outer band.
	const float kYawIdleHardSnapEpsDeg = kYawEnemyHardSnapEpsDeg;

	int yawSnapMode = AimYawSnapMode_None;
	const float absYawErr = Q_abs(angleDiff);

	if (isAimingAtEnemy)
	{
		if (absYawErr <= kYawEnemyHardSnapEpsDeg)
		{
			yawSnapMode = AimYawSnapMode_HardTinySnap;
			*lookYawVel = 0.0f;
			pev->v_angle.y = useYaw;
			microYawSnap = true;
			yawAccelApplied = 0.0f;
			directAngleWriteReason = AimMotorDirectWrite_EnemyMicroSnap;
			yawAccelRawVal = 0.0f;
			yawAccelClampVal = 0.0f;
			yawVelPostIntVal = 0.0f;
			yawStepRawVal = NormalizeAngle(useYaw - yawStart);
		}
		else
		{
			if (absYawErr <= kYawEnemyLegacySnapOuterDeg)
				yawSnapMode = AimYawSnapMode_BypassedLegacyBand;

			// Drop stale yaw velocity when the shortest-path error wants to turn one way but integrator
			// momentum still rotates the other way (common after m_lookYaw / useYaw jumps). Zeroing is
			// deterministic and avoids a long wrong-way sweep until damping flips the velocity.
			const float staleYawAngleGateDeg = 4.0f;
			const float staleYawVelEpsilon = 0.5f;
			const float oldYawVel = *lookYawVel;
			if (Q_abs(angleDiff) > staleYawAngleGateDeg && Q_abs(oldYawVel) > staleYawVelEpsilon
				&& ((angleDiff > 0.0f && oldYawVel < 0.0f) || (angleDiff < 0.0f && oldYawVel > 0.0f)))
			{
				*lookYawVel = 0.0f;
				staleYawClearedVel = true;
				if (motorDebugSubject && gpGlobals)
				{
					static float s_lastStaleYawMotorLogTime = -1.0e12f;
					const float staleYawMotorLogInterval = 0.25f;
					if (gpGlobals->time - s_lastStaleYawMotorLogTime >= staleYawMotorLogInterval)
					{
						s_lastStaleYawMotorLogTime = gpGlobals->time;
						motorDebugSubject->PrintIfWatched(
							"aim motor: stale yaw vel guard angleDiff=%.2f oldVel=%.2f newVel=%.2f cleared=1\n",
							angleDiff, oldYawVel, *lookYawVel);
					}
				}
			}
			float accel = stiffness * angleDiff - damping * (*lookYawVel);

			yawAccelRawVal = accel;

			if (accel > yawMaxAccel)
				accel = yawMaxAccel;
			else if (accel < -yawMaxAccel)
				accel = -yawMaxAccel;

			yawAccelClampVal = accel;
			yawAccelApplied = accel;
			*lookYawVel += deltaT * accel;
			yawVelPostIntVal = *lookYawVel;
			yawStepRawVal = deltaT * yawVelPostIntVal;
			pev->v_angle.y += yawStepRawVal;
		}
	}
	else if (absYawErr <= kYawIdleHardSnapEpsDeg)
	{
		// !isAimingAtEnemy only (reached after the combat branch). Distinct from enemy micro-snap for forensics.
		*lookYawVel = 0.0f;
		pev->v_angle.y = useYaw;
		microYawSnap = true;
		yawAccelApplied = 0.0f;
		directAngleWriteReason = AimMotorDirectWrite_NonCombatTinySnap;
		yawAccelRawVal = 0.0f;
		yawAccelClampVal = 0.0f;
		yawVelPostIntVal = 0.0f;
		yawStepRawVal = NormalizeAngle(useYaw - yawStart);
	}
	else
	{
		const float staleYawAngleGateDeg = 4.0f;
		const float staleYawVelEpsilon = 0.5f;
		const float oldYawVel = *lookYawVel;
		if (Q_abs(angleDiff) > staleYawAngleGateDeg && Q_abs(oldYawVel) > staleYawVelEpsilon
			&& ((angleDiff > 0.0f && oldYawVel < 0.0f) || (angleDiff < 0.0f && oldYawVel > 0.0f)))
		{
			*lookYawVel = 0.0f;
			staleYawClearedVel = true;
			if (motorDebugSubject && gpGlobals)
			{
				static float s_lastStaleYawMotorLogTime = -1.0e12f;
				const float staleYawMotorLogInterval = 0.25f;
				if (gpGlobals->time - s_lastStaleYawMotorLogTime >= staleYawMotorLogInterval)
				{
					s_lastStaleYawMotorLogTime = gpGlobals->time;
					motorDebugSubject->PrintIfWatched(
						"aim motor: stale yaw vel guard angleDiff=%.2f oldVel=%.2f newVel=%.2f cleared=1\n",
						angleDiff, oldYawVel, *lookYawVel);
				}
			}
		}
		float accel = stiffness * angleDiff - damping * (*lookYawVel);

		yawAccelRawVal = accel;

		if (accel > yawMaxAccel)
			accel = yawMaxAccel;
		else if (accel < -yawMaxAccel)
			accel = -yawMaxAccel;

		yawAccelClampVal = accel;
		yawAccelApplied = accel;
		*lookYawVel += deltaT * accel;
		yawVelPostIntVal = *lookYawVel;
		yawStepRawVal = deltaT * yawVelPostIntVal;
		pev->v_angle.y += yawStepRawVal;
	}

	// Pitch
	// Actually, this is negative pitch.
	const float pitchErrBefore = NormalizeAngle(usePitch - pev->v_angle.x);
	angleDiff = usePitch - pev->v_angle.x;

	angleDiff = NormalizeAngle(angleDiff);
	const float pitchMaxAccel = maxAccel * (acquisitionBurst ? 1.85f : (firingGun ? 1.22f : 1.0f));

	float pitchAccelApplied = 0.0f;

	float pitchAccelRawVal = 0.0f;
	float pitchAccelClampVal = 0.0f;
	float pitchVelPostIntVal = 0.0f;
	float pitchStepRawVal = 0.0f;

	if (false && angleDiff < 1.0f && angleDiff > -1.0f)
	{
		*lookPitchVel = 0.0f;
		pev->v_angle.x = usePitch;
	}
	else
	{
		// simple angular spring/damper
		// double the stiffness since pitch is only +/- 90 and yaw is +/- 180
		float accel = 2.0f * stiffness * angleDiff - damping * (*lookPitchVel);

		pitchAccelRawVal = accel;

		// limit rate
		if (accel > pitchMaxAccel)
			accel = pitchMaxAccel;

		else if (accel < -pitchMaxAccel)
			accel = -pitchMaxAccel;

		pitchAccelClampVal = accel;
		pitchAccelApplied = accel;
		*lookPitchVel += deltaT * accel;
		pitchVelPostIntVal = *lookPitchVel;
		pitchStepRawVal = deltaT * pitchVelPostIntVal;
		pev->v_angle.x += pitchStepRawVal;
	}

	const float pitchBeforeClamp = pev->v_angle.x;

	// limit range - avoid gimbal lock
	if (pev->v_angle.x < -89.0f)
		pev->v_angle.x = -89.0f;
	else if (pev->v_angle.x > 89.0f)
		pev->v_angle.x = 89.0f;

	int zeroedPitchVelReason = AimMotorZeroVel_None;
	if (pev->v_angle.x != pitchBeforeClamp && *lookPitchVel == 0.0f)
		zeroedPitchVelReason = AimMotorZeroVel_AngleNormalization;

	pev->v_angle.z = 0.0f;

	const float deltaYawApplied = NormalizeAngle(pev->v_angle.y - yawStart);
	const float deltaPitchApplied = pev->v_angle.x - pitchStart;

	int zeroedYawVelReason = AimMotorZeroVel_None;
	if (*lookYawVel == 0.0f)
	{
		if (microYawSnap)
			zeroedYawVelReason = AimMotorZeroVel_SnapEpsilon;
		else if (staleYawClearedVel)
			zeroedYawVelReason = AimMotorZeroVel_StaleYawGuard;
		else if (Q_abs(yawErrBefore) > 0.12f)
			zeroedYawVelReason = AimMotorZeroVel_Unknown;
	}

	const int motorDtInvalid = (deltaT <= 0.0f) ? 1 : 0;

	if (outDiag)
	{
		outDiag->valid = true;
		outDiag->stiffness = stiffness;
		outDiag->damping = damping;
		outDiag->maxAccel = maxAccel;
		outDiag->yawErrorBefore = yawErrBefore;
		outDiag->pitchErrorBefore = pitchErrBefore;
		outDiag->yawAccelApplied = yawAccelApplied;
		outDiag->pitchAccelApplied = pitchAccelApplied;
		outDiag->yawVelAfter = *lookYawVel;
		outDiag->pitchVelAfter = *lookPitchVel;
		outDiag->microYawSnap = microYawSnap;
		outDiag->yawSnapMode = yawSnapMode;
		outDiag->deltaYawWritten = deltaYawApplied;
		outDiag->deltaPitchWritten = deltaPitchApplied;

		const bool logMotorForensic = (sv_aim_log_enable.value > 0.0f);
		outDiag->motorForensicValid = logMotorForensic;
		if (logMotorForensic)
		{
			outDiag->motorDt = deltaT;
			outDiag->yawVelPre = yawVelPreGlobal;
			outDiag->pitchVelPre = pitchVelPreGlobal;
			outDiag->yawAccelRaw = yawAccelRawVal;
			outDiag->pitchAccelRaw = pitchAccelRawVal;
			outDiag->yawAccelClamped = yawAccelClampVal;
			outDiag->pitchAccelClamped = pitchAccelClampVal;
			outDiag->yawVelPostIntegrate = yawVelPostIntVal;
			outDiag->pitchVelPostIntegrate = pitchVelPostIntVal;
			outDiag->yawStepRaw = yawStepRawVal;
			outDiag->pitchStepRaw = pitchStepRawVal;
			outDiag->yawStepApplied = deltaYawApplied;
			outDiag->pitchStepApplied = deltaPitchApplied;
			outDiag->yawVelFinal = *lookYawVel;
			outDiag->pitchVelFinal = *lookPitchVel;
			outDiag->yawErrPost = NormalizeAngle(useYaw - pev->v_angle.y);
			outDiag->pitchErrPost = NormalizeAngle(usePitch - pev->v_angle.x);
			outDiag->zeroedYawVelReason = zeroedYawVelReason;
			outDiag->zeroedPitchVelReason = zeroedPitchVelReason;
			outDiag->directAngleWriteReason = directAngleWriteReason;
			outDiag->motorDtInvalid = motorDtInvalid;
		}
		else
		{
			outDiag->motorForensicValid = false;
			outDiag->motorDt = 0.0f;
			outDiag->yawVelPre = 0.0f;
			outDiag->pitchVelPre = 0.0f;
			outDiag->yawAccelRaw = 0.0f;
			outDiag->pitchAccelRaw = 0.0f;
			outDiag->yawAccelClamped = 0.0f;
			outDiag->pitchAccelClamped = 0.0f;
			outDiag->yawVelPostIntegrate = 0.0f;
			outDiag->pitchVelPostIntegrate = 0.0f;
			outDiag->yawStepRaw = 0.0f;
			outDiag->pitchStepRaw = 0.0f;
			outDiag->yawStepApplied = 0.0f;
			outDiag->pitchStepApplied = 0.0f;
			outDiag->yawVelFinal = 0.0f;
			outDiag->pitchVelFinal = 0.0f;
			outDiag->yawErrPost = 0.0f;
			outDiag->pitchErrPost = 0.0f;
			outDiag->zeroedYawVelReason = 0;
			outDiag->zeroedPitchVelReason = 0;
			outDiag->directAngleWriteReason = 0;
			outDiag->motorDtInvalid = 0;
		}
	}
}

} // namespace HumanAimMotorController

// Integrate desired m_lookYaw / m_lookPitch (from SetLookAngles) into pev->v_angle.
//
// Normal ownership: this is the routine place bot view angles are written to pev->v_angle.
// Special cases: early return for bot mimic; early return for AI chat freeze; ladder overrides useYaw/usePitch;
// non-combat-only coherent drift offsets inside this function when !IsAimingAtEnemy() && !IsUsingLadder()
// (see NonCombatAimDrift::ApplySampledMicroOffsetsBeforeMotor).
//
// INVARIANTS (for refactors): keep final integration here; do not solve aim in ExecuteCommand();
// ladder and high-priority SetLookAt must remain authoritative while active; combat path should not depend
// on roaming-only coherent drift (NonCombatAimDrift) applied in UpdateNonCombatAimController.
//
// TODO: Make stiffness and turn rate constants timestep invariant.
void CCSBot::UpdateLookAngles()
{
	const float deltaT = g_flBotCommandInterval;

#ifdef REGAMEDLL_ADD
	// If mimicing the player, don't modify the view angles
	if (cv_bot_mimic.value > 0)
		return;
#endif

	// When the bot is "typing" AI chat, freeze look direction too (no mouse tracking).
	if (AiChat::ShouldFreezeMovementForChat(entindex()))
		return;

	HumanAimMotorController::AimMotorParams motor = HumanAimMotorController::SelectBaseParams(
		IsAttacking(), IsAimingAtEnemy(), m_aimHumanMotorScale);
	float stiffness = motor.stiffness;
	float damping = motor.damping;
	float maxAccel = motor.maxAccel;

	// these may be overridden by ladder logic
	float useYaw = m_lookYaw;
	float usePitch = m_lookPitch;
	const bool firingGun = IsAttacking() && IsAimingAtEnemy() && !IsUsingGrenade() && !IsUsingKnife();
	const float acquireAge = IsAimingAtEnemy() ? (gpGlobals->time - m_currentEnemyAcquireTimestamp) : 999.0f;
	const bool acquisitionBurst = IsAimingAtEnemy() && acquireAge > 0.07f && acquireAge < 0.42f;

	// Ladders require precise movement, therefore we need to look at the
	// ladder as we approach and ascend/descend it.
	// If we are on a ladder, we need to look up or down to traverse it - override pitch in this case.
	// If we're trying to break something, though, we actually need to look at it before we can
	// look at the ladder
	if (IsUsingLadder())
	{
		// set yaw to aim at ladder
		Vector to = m_pathLadder->m_top - pev->origin;
		float idealYaw = UTIL_VecToYaw(to);

		NavDirType faceDir = m_pathLadder->m_dir;

		if (m_pathLadderFaceIn)
		{
			faceDir = OppositeDirection(faceDir);
		}

		const float lookAlongLadderRange = 100.0f;
		const float ladderPitch = 60.0f;

		// adjust pitch to look up/down ladder as we ascend/descend
		switch (m_pathLadderState)
		{
		case APPROACH_ASCENDING_LADDER:
		{
			Vector to = m_goalPosition - pev->origin;
			useYaw = idealYaw;

			if (to.IsLengthLessThan(lookAlongLadderRange))
				usePitch = -ladderPitch;
			break;
		}
		case APPROACH_DESCENDING_LADDER:
		{
			Vector to = m_goalPosition - pev->origin;
			useYaw = idealYaw;

			if (to.IsLengthLessThan(lookAlongLadderRange))
				usePitch = ladderPitch;
			break;
		}
		case FACE_ASCENDING_LADDER:
		{
			useYaw = idealYaw;
			usePitch = -ladderPitch;
			break;
		}
		case FACE_DESCENDING_LADDER:
		{
			useYaw = idealYaw;
			usePitch = ladderPitch;
			break;
		}
		case MOUNT_ASCENDING_LADDER:
		case ASCEND_LADDER:
		{
			useYaw = DirectionToAngle(faceDir) + StayOnLadderLine(this, m_pathLadder);
			usePitch = -ladderPitch;
			break;
		}
		case MOUNT_DESCENDING_LADDER:
		case DESCEND_LADDER:
		{
			useYaw = DirectionToAngle(faceDir) + StayOnLadderLine(this, m_pathLadder);
			usePitch = ladderPitch;
			break;
		}
		case DISMOUNT_ASCENDING_LADDER:
		case DISMOUNT_DESCENDING_LADDER:
		{
			useYaw = DirectionToAngle(faceDir);
			break;
		}
		}
	}

	if (!IsAimingAtEnemy() && !IsUsingLadder())
		NonCombatAimDrift::ApplySampledMicroOffsetsBeforeMotor(this, useYaw, usePitch, stiffness, maxAccel);
	else
	{
		if (IsAimingAtEnemy())
			NonCombatAimDrift::RecordMicroDriftSkipped(NonCombatAimDrift::MicroDriftDebug_SkippedCombatAim);
		else
			NonCombatAimDrift::RecordMicroDriftSkipped(NonCombatAimDrift::MicroDriftDebug_SkippedLadder);
	}

	if (!IsUsingLadder())
	{
		const float yawDiff = NormalizeAngle(useYaw - pev->v_angle.y);
		const float pitchDiff = NormalizeAngle(usePitch - pev->v_angle.x);
		const float crossPhase = gpGlobals->time * (firingGun ? 15.5f : 8.7f) + entindex() * 1.37f;

		// Human mouse motion is rarely a perfectly pure X or Y axis. Add tiny cross-axis bleed
		// when the desired movement is otherwise too mechanically horizontal or vertical.
		if (Q_abs(yawDiff) > 1.2f && Q_abs(pitchDiff) < (firingGun ? 1.80f : 0.35f))
		{
			const float maxPitchBleed = firingGun ? 5.5f : 1.60f;
			const float pitchScale = firingGun ? 0.125f : 0.034f;
			const float pitchVelScale = firingGun ? 0.0060f : 0.0024f;
			const float pitchBleed = Q_max(-maxPitchBleed, Q_min(Q_abs(yawDiff) * pitchScale + Q_abs(m_lookYawVel) * pitchVelScale, maxPitchBleed));
			usePitch += pitchBleed * (BotSIN(crossPhase) >= 0.0f ? 1.0f : -1.0f);
		}

		if (Q_abs(pitchDiff) > 0.8f && Q_abs(yawDiff) < 0.28f)
		{
			const float yawBleed = Q_max(-0.75f, Q_min(Q_abs(pitchDiff) * 0.035f + Q_abs(m_lookPitchVel) * 0.0012f, 0.75f));
			useYaw += yawBleed * (BotCOS(crossPhase * 0.83f) >= 0.0f ? 1.0f : -1.0f);
		}
	}

	AimMotorStepDiag *motorDiagOut = nullptr;
	if (IsAimingAtEnemy() && m_enemy.IsValid())
	{
		m_aimDiagMotorStep.valid = false;
		m_aimDiagAcquisitionBurst = acquisitionBurst;
		motorDiagOut = &m_aimDiagMotorStep;
	}

	HumanAimMotorController::ApplySpringDamperToViewAngles(
		pev, deltaT, useYaw, usePitch, stiffness, damping, maxAccel, firingGun, acquisitionBurst, IsAimingAtEnemy(),
		&m_lookYawVel, &m_lookPitchVel, this, motorDiagOut);
}

// Return true if we can see the point
bool CCSBot::IsVisible(const Vector *pos, bool testFOV) const
{
	// we can't see anything if we're blind
	if (IsBlind())
		return false;

	// is it in my general viewcone?
	if (testFOV && !(const_cast<CCSBot *>(this)->FInViewCone(pos)))
		return false;

	// check line of sight against smoke
	if (TheCSBots()->IsLineBlockedBySmoke(&GetEyePosition(), pos))
		return false;

	// check line of sight
	// Must include CONTENTS_MONSTER to pick up all non-brush objects like barrels
	TraceResult result;
	UTIL_TraceLine(GetEyePosition(), *pos, ignore_monsters, ignore_glass, ENT(pev), &result);

	if (result.flFraction != 1.0f)
		return false;

	return true;
}

// Return true if we can see any part of the player
// Check parts in order of importance. Return the first part seen in "visParts" if it is non-NULL.
bool CCSBot::IsVisible(CBasePlayer *pPlayer, bool testFOV, unsigned char *visParts) const
{
#ifdef REGAMEDLL_ADD // REGAMEDLL_FIXES ?
	if ((pPlayer->pev->flags & FL_NOTARGET) || (pPlayer->pev->effects & EF_NODRAW))
		return false;
#endif

	Vector spot = pPlayer->pev->origin;
	unsigned char testVisParts = NONE;

	// finish chest check
	if (IsVisible(&spot, testFOV))
		testVisParts |= CHEST;

	// check top of head
	spot = spot + Vector(0, 0, 25.0f);

	if (IsVisible(&spot, testFOV))
		testVisParts |= HEAD;

	// check feet
	const float standFeet = 34.0f;
	const float crouchFeet = 14.0f;

	if (pPlayer->pev->flags & FL_DUCKING)
		spot.z = pPlayer->pev->origin.z - crouchFeet;
	else
		spot.z = pPlayer->pev->origin.z - standFeet;

	// check feet
	if (IsVisible(&spot, testFOV))
		testVisParts |= FEET;

	// check "edges"
	const float edgeOffset = 13.0f;
	Vector2D dir = (pPlayer->pev->origin - pev->origin).Make2D();
	dir.NormalizeInPlace();

	Vector2D perp(-dir.y, dir.x);

	spot = pPlayer->pev->origin + Vector(perp.x * edgeOffset, perp.y * edgeOffset, 0);

	if (IsVisible(&spot, testFOV))
		testVisParts |= LEFT_SIDE;

	spot = pPlayer->pev->origin - Vector(perp.x * edgeOffset, perp.y * edgeOffset, 0);

	if (IsVisible(&spot, testFOV))
		testVisParts |= RIGHT_SIDE;

	if (visParts)
		*visParts = testVisParts;

	if (testVisParts != NONE)
		return true;

	return false;
}

bool CCSBot::IsEnemyPartVisible(VisiblePartType part) const
{
	if (!IsEnemyVisible())
		return false;

	return (m_visibleEnemyParts & part) != 0;
}

// Map debug string from SetLookAt into LookTargetReason; unlisted strings -> LOOK_TARGET_REASON_UNKNOWN.
static CCSBot::LookTargetReason InferLookTargetReasonForDesc(const char *desc)
{
	if (!desc || !desc[0])
		return CCSBot::LOOK_TARGET_REASON_UNKNOWN;

	if (FStrEq(desc, "GrenadeThrow"))
		return CCSBot::LOOK_TARGET_REASON_GRENADE_THROW;
	if (FStrEq(desc, "Roam Danger Angle"))
		return CCSBot::LOOK_TARGET_REASON_ROAM_DANGER;
	if (FStrEq(desc, "Roam Terrain Lane"))
		return CCSBot::LOOK_TARGET_REASON_ROAM_TERRAIN;
	if (FStrEq(desc, "Roam Corner Check"))
		return CCSBot::LOOK_TARGET_REASON_ROAM_CORNER;
	if (FStrEq(desc, "Noise"))
		return CCSBot::LOOK_TARGET_REASON_NOISE;
	if (FStrEq(desc, "Check dangerous noise"))
		return CCSBot::LOOK_TARGET_REASON_NOISE_DANGEROUS;
	if (FStrEq(desc, "Last Enemy Position"))
		return CCSBot::LOOK_TARGET_REASON_LAST_ENEMY;
	if (FStrEq(desc, "Suspicious Last-Seen Danger"))
		return CCSBot::LOOK_TARGET_REASON_SUSPICIOUS_LAST_SEEN;
	if (FStrEq(desc, "Suspicious enemy angle"))
		return CCSBot::LOOK_TARGET_REASON_SUSPICIOUS_ENEMY_ANGLE;
	if (FStrEq(desc, "Avoid Wall Stare"))
		return CCSBot::LOOK_TARGET_REASON_WALL_STARE;
	if (FStrEq(desc, "Learned Danger"))
		return CCSBot::LOOK_TARGET_REASON_LEARNED_DANGER;
	if (FStrEq(desc, "Open Lane (Hiding)"))
		return CCSBot::LOOK_TARGET_REASON_OPEN_LANE;
	if (FStrEq(desc, "Approach Point (Hiding)"))
		return CCSBot::LOOK_TARGET_REASON_APPROACH_POINT;
	if (FStrEq(desc, "Encounter Spot"))
		return CCSBot::LOOK_TARGET_REASON_ENCOUNTER_SPOT;
	if (FStrEq(desc, "Hostage"))
		return CCSBot::LOOK_TARGET_REASON_HOSTAGE;
	if (FStrEq(desc, "Nearby enemy gunfire"))
		return CCSBot::LOOK_TARGET_REASON_NEARBY_GUNFIRE;
	if (FStrEq(desc, "Plant bomb on floor"))
		return CCSBot::LOOK_TARGET_REASON_PLANT_BOMB;
	if (FStrEq(desc, "Defuse bomb"))
		return CCSBot::LOOK_TARGET_REASON_DEFUSE_BOMB;
	if (FStrEq(desc, "Use entity"))
		return CCSBot::LOOK_TARGET_REASON_USE_ENTITY;
	if (FStrEq(desc, "Breakable"))
		return CCSBot::LOOK_TARGET_REASON_BREAKABLE;
	if (FStrEq(desc, "Panic"))
		return CCSBot::LOOK_TARGET_REASON_PANIC;
	if (FStrEq(desc, "Post-buy freeze look"))
		return CCSBot::LOOK_TARGET_REASON_POST_BUY;
	if (FStrEq(desc, "Recorded camp look"))
		return CCSBot::LOOK_TARGET_REASON_RECORDED_CAMP;

	return CCSBot::LOOK_TARGET_REASON_UNKNOWN;
}

void CCSBot::SyncLookTargetFromLegacyState()
{
	if (m_lookAtSpotState == NOT_LOOKING_AT_SPOT)
	{
		m_lookTarget.isActive = false;
		m_lookTarget.position = Vector(0, 0, 0);
		m_lookTarget.priority = PRIORITY_LOW;
		m_lookTarget.reason = LOOK_TARGET_REASON_NONE;
		m_lookTarget.debugDesc = nullptr;
		m_lookTarget.phase = LOOK_TARGET_PHASE_INACTIVE;
		m_lookTarget.startTime = 0.0f;
		m_lookTarget.durationSeconds = 0.0f;
		m_lookTarget.computedEndTime = -1.0f;
		m_lookTarget.angleTolerance = 5.0f;
		m_lookTarget.clearIfClose = false;
		m_lookTarget.rejectsLowerPriorityTargets = false;
		return;
	}

	m_lookTarget.isActive = true;
	m_lookTarget.position = m_lookAtSpot;
	m_lookTarget.priority = m_lookAtSpotPriority;
	m_lookTarget.debugDesc = m_lookAtDesc;
	m_lookTarget.reason = InferLookTargetReasonForDesc(m_lookAtDesc);
	m_lookTarget.startTime = m_lookAtSpotTimestamp;
	m_lookTarget.durationSeconds = m_lookAtSpotDuration;
	if (m_lookAtSpotDuration >= 0.0f)
		m_lookTarget.computedEndTime = m_lookAtSpotTimestamp + m_lookAtSpotDuration;
	else
		m_lookTarget.computedEndTime = -1.0f;
	m_lookTarget.angleTolerance = m_lookAtSpotAngleTolerance;
	m_lookTarget.clearIfClose = m_lookAtSpotClearIfClose;
	m_lookTarget.rejectsLowerPriorityTargets = (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority > PRIORITY_LOW);

	if (m_lookAtSpotState == LOOK_TOWARDS_SPOT)
		m_lookTarget.phase = LOOK_TARGET_PHASE_TOWARDS_SPOT;
	else if (m_lookAtSpotState == LOOK_AT_SPOT)
		m_lookTarget.phase = LOOK_TARGET_PHASE_AT_SPOT;
	else
		m_lookTarget.phase = LOOK_TARGET_PHASE_INACTIVE;
}

// Converts m_lookAtSpot (and metadata set by SetLookAt) into angles and calls SetLookAngles.
bool CCSBot::UpdateLookAt()
{
	// High-priority looks (noise response, grenade, bomb, use, etc.): preserve existing exact aim.
	// Combat vs visible enemy: non-combat aim path normally inactive; skip LOS rejection defensively
	// so we never override intentional look intents in edge cases.
	const bool skipLosCheck = (m_lookAtSpotPriority > PRIORITY_MEDIUM) || (IsAimingAtEnemy() && IsEnemyVisible());
	bool skipLearnedDangerLosCamp = false;
	if (!skipLosCheck && m_lookAtDesc && FStrEq(m_lookAtDesc, "Learned Danger")
		&& m_lookAtSpotPriority == PRIORITY_MEDIUM && IsHiding() && IsNotMoving()
		&& !IsEnemyVisible() && !IsBlind())
	{
		Vector campProbe{};
		skipLearnedDangerLosCamp = BotCampLookSpots::FindLookTarget(this, &campProbe, nullptr);
	}

	if (!skipLosCheck && !skipLearnedDangerLosCamp)
	{
		if (!IsVisible(&m_lookAtSpot, false))
		{
			const bool holdIdleCommitYaw =
				IsNotMoving() && !IsUsingLadder() && IsIdleLookCommitActive() && !ShouldOverrideIdleLookCommit();

			float usePitch = m_lookAheadPitch;
			if (!IsNotMoving() && !IsUsingLadder())
				usePitch = Q_max(-7.5f, Q_min(usePitch, 3.5f));

			float useYaw = m_lookAheadAngle;
			if (holdIdleCommitYaw)
				useYaw = m_lastIdleLookYaw;

			const Vector &eye = GetEyePosition();
			const Vector toSpot = m_lookAtSpot - eye;
			const float dist2D = toSpot.Length2D();
			const float dz = m_lookAtSpot.z - eye.z;
			Vector desiredAngles = UTIL_VecToAngles(toSpot);
			desiredAngles.x = 360.0f - desiredAngles.x;
			const float desiredPitch = desiredAngles.x;
			const float desiredYaw = desiredAngles.y;
			const float curPitch = pev->v_angle.x;
			const float curYaw = pev->v_angle.y;
			const float enMemAge =
				(m_lastSawEnemyTimestamp > 0.0f && gpGlobals) ? GetTimeSinceLastSawEnemy() : -1.0f;
			const Vector &enPos = GetLastKnownEnemyPosition();

			if (gpGlobals && gpGlobals->time >= m_lookLosRejectLogNextTime)
			{
				m_lookLosRejectLogNextTime = gpGlobals->time + 0.35f;
				char buf[640];
				Q_snprintf(
					buf, sizeof(buf),
					"[lookLOSReject] bot=%s ent=%i st=%s task=%i reason=%s "
					"origin=(%.0f %.0f %.0f) eye=(%.0f %.0f %.0f) spot=(%.0f %.0f %.0f) "
					"dz=%.0f dist2D=%.0f desiredPitch=%.1f desiredYaw=%.1f "
					"curPitch=%.1f curYaw=%.1f lookAheadPitch=%.1f lookAheadYaw=%.1f "
					"finalPitch=%.1f finalYaw=%.1f hiding=%i notMoving=%i ladder=%i holdIdleYaw=%i "
					"enMemAge=%.2f enPos=(%.0f %.0f %.0f) eyeEnemy=%i",
					pev ? STRING(pev->netname) : "?",
					entindex(),
					m_state ? m_state->GetName() : "?",
					static_cast<int>(GetTask()),
					m_lookAtDesc ? m_lookAtDesc : "?",
					pev->origin.x, pev->origin.y, pev->origin.z,
					eye.x, eye.y, eye.z,
					m_lookAtSpot.x, m_lookAtSpot.y, m_lookAtSpot.z,
					dz, dist2D, desiredPitch, desiredYaw,
					curPitch, curYaw, m_lookAheadPitch, m_lookAheadAngle,
					usePitch, useYaw,
					IsHiding() ? 1 : 0,
					IsNotMoving() ? 1 : 0,
					IsUsingLadder() ? 1 : 0,
					holdIdleCommitYaw ? 1 : 0,
					enMemAge,
					enPos.x, enPos.y, enPos.z,
					IsEnemyVisible() ? 1 : 0);
				EmitMovementWatchedDebugLine(this, buf);
			}

			ClearLookAt();
			SetLookAngles(useYaw, usePitch);
			return false;
		}
	}

	Vector to = m_lookAtSpot - EyePosition();
	Vector idealAngle = UTIL_VecToAngles(to);
	idealAngle.x = 360.0f - idealAngle.x;

	if (m_lookAtSpotPriority <= PRIORITY_LOW && !IsNotMoving() && !IsUsingLadder())
	{
		const float yawDiff = NormalizeAngle(idealAngle.y - m_lookAheadAngle);
		const float limitedYawDiff = Q_max(-32.0f, Q_min(yawDiff * 0.38f, 32.0f));
		idealAngle.y = m_lookAheadAngle + limitedYawDiff;
		idealAngle.x = Q_max(-5.0f, Q_min(idealAngle.x * 0.45f, 5.0f));
	}

	SetLookAngles(idealAngle.y, idealAngle.x);
	return true;
}

// Stores explicit world look target: m_lookAtSpot, priority, duration, state (LOOK_TOWARDS_SPOT / ...).
// Higher m_lookAtSpotPriority rejects lower-priority SetLookAt calls; preserves grenade / bomb / use / hostage looks.
// Look at the given point in space for the given duration (-1 means forever)
void CCSBot::UpdateAvoidWallStareSuppressionForTacticalLook(const char *desc, PriorityType pri, float duration)
{
	if (!desc || !gpGlobals || pri != PRIORITY_MEDIUM)
		return;
	if (!IsHiding() || !IsNotMoving())
		return;

	if (!FStrEq(desc, "Learned Danger") && !FStrEq(desc, "Approach Point (Hiding)") && !FStrEq(desc, "Approach Point (Hide Arrival)")
		&& !FStrEq(desc, "Open Lane (Hiding)") && !FStrEq(desc, "Recorded camp look"))
		return;

	float d = duration;
	if (d < 0.0f)
		d = 3.5f;

	constexpr float kPostTacticalGrace = 1.15f;
	const float until = gpGlobals->time + d + kPostTacticalGrace;
	m_suppressAvoidWallStareUntil = Q_max(m_suppressAvoidWallStareUntil, until);
}

void CCSBot::SetLookAt(const char *desc, const Vector *pos, PriorityType pri, float duration, bool clearIfClose, float angleTolerance)
{
	if (!pos)
		return;

	// if currently looking at a point in space with higher priority, ignore this request
	if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority > pri)
		return;

	// if already looking at this spot, just extend the time
	const float tolerance = 10.0f;
	if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && VectorsAreEqual(pos, &m_lookAtSpot, tolerance))
	{
		m_lookAtSpotDuration = duration;

		if (m_lookAtSpotPriority < pri)
			m_lookAtSpotPriority = pri;
	}
	else
	{
		// look at new spot
		m_lookAtSpot = *pos;
		m_lookAtSpotState = LOOK_TOWARDS_SPOT;
		m_lookAtSpotDuration = duration;
		m_lookAtSpotTimestamp = gpGlobals->time;
		m_lookAtSpotPriority = pri;
	}

	m_lookAtSpotAngleTolerance = angleTolerance;
	m_lookAtSpotClearIfClose = clearIfClose;
	m_lookAtDesc = desc;

	UpdateAvoidWallStareSuppressionForTacticalLook(desc, pri, m_lookAtSpotDuration);

	SyncLookTargetFromLegacyState();
}

void CCSBot::ClearLookAt()
{
	m_lookAtSpotPriority = PRIORITY_LOW;
	m_lookAtSpotState = NOT_LOOKING_AT_SPOT;
	SyncLookTargetFromLegacyState();
}

// Block all "look at" and "look around" behavior for given duration - just look ahead
void CCSBot::InhibitLookAround(float duration)
{
	m_inhibitLookAroundTimestamp = gpGlobals->time + duration;
}

// Update enounter spot timestamps, etc
void CCSBot::UpdatePeripheralVision()
{
	// if we update at 10Hz, this ensures we test once every three
	const float peripheralUpdateInterval = 0.29f;
	if (gpGlobals->time - m_peripheralTimestamp < peripheralUpdateInterval)
		return;

	m_peripheralTimestamp = gpGlobals->time;

	if (m_spotEncounter)
	{
		// check LOS to all spots in case we see them with our "peripheral vision"
		Vector pos;
		for (auto &spotOrder : m_spotEncounter->spotList)
		{
			const Vector *spotPos = spotOrder.spot->GetPosition();

			pos.x = spotPos->x;
			pos.y = spotPos->y;
			pos.z = spotPos->z + HalfHumanHeight;

			if (!IsVisible(&pos, CHECK_FOV))
				continue;

			// can see hiding spot, remember when we saw it last
			SetHidingSpotCheckTimestamp(spotOrder.spot);
		}
	}
}

static bool IsCommittedMovementIntent(CCSBot *me)
{
	if (!me || me->IsNotMoving() || me->IsUsingLadder() || me->IsAimingAtEnemy())
		return false;

	if (me->IsHunting() && me->IsSafe())
		return true;

	if (me->IsFollowing() && me->IsSafe() && me->GetFollowDuration() < 8.0f)
		return true;

	if (!me->IsMovingTo())
		return false;

	switch (me->GetTask())
	{
	case CCSBot::PLANT_BOMB:
	case CCSBot::FIND_TICKING_BOMB:
	case CCSBot::DEFUSE_BOMB:
	case CCSBot::MOVE_TO_LAST_KNOWN_ENEMY_POSITION:
	case CCSBot::VIP_ESCAPE:
	case CCSBot::COLLECT_HOSTAGES:
	case CCSBot::RESCUE_HOSTAGES:
		return true;
	default:
		break;
	}

	return me->IsSafe() || me->IsHurrying();
}

namespace
{
constexpr float kIdleLookBacktrackRejectDeg = 7.0f;
constexpr float kRecentDamageIdleLookOverrideSecs = 2.5f;
} // namespace

bool CCSBot::IsIdleLookCommitActive() const
{
	return gpGlobals && gpGlobals->time < m_idleLookCommitUntil;
}

void CCSBot::CancelIdleLookCommit()
{
	m_idleLookCommitUntil = 0.0f;
}

void CCSBot::BeginIdleLookCommit(float acceptedYaw, bool isMoving)
{
	if (!gpGlobals)
		return;

	const float dwell = isMoving ? RANDOM_FLOAT(1.25f, 3.0f) : RANDOM_FLOAT(2.5f, 6.0f);
	m_prevIdleLookYaw = m_lastIdleLookYaw;
	m_lastIdleLookYaw = NormalizeAngle(acceptedYaw);
	m_idleLookCommitUntil = gpGlobals->time + dwell;
}

bool CCSBot::WouldIdleLookPingPong(float candidateYaw) const
{
	if (!IsIdleLookCommitActive())
		return false;

	const float dPrev = Q_abs(NormalizeAngle(candidateYaw - m_prevIdleLookYaw));
	return dPrev < kIdleLookBacktrackRejectDeg;
}

unsigned CCSBot::CampLookRotateSlot(int validCount) const
{
	if (validCount <= 0)
		return 0;
	return m_campLookRotateCursor % static_cast<unsigned>(validCount);
}

void CCSBot::CampLookRotateAdvance()
{
	++m_campLookRotateCursor;
}

bool CCSBot::ShouldOverrideIdleLookCommit() const
{
	if (!IsAlive() || !pev || !gpGlobals)
		return true;
	if (IsAimingAtEnemy())
		return true;
	if (IsEnemyVisible())
		return true;
	if (IsBlind())
		return true;
	if (IsNoiseHeard())
		return true;
	if (m_attackedTimestamp > 0.0f && gpGlobals->time - m_attackedTimestamp < kRecentDamageIdleLookOverrideSecs)
		return true;
	if (IsCommittedMovementIntent(this))
		return true;
	if (IsLookingAtSpot(PRIORITY_HIGH))
		return true;

	const LookTarget &lt = GetLookTarget();
	if (lt.isActive && lt.priority > PRIORITY_MEDIUM)
		return true;

	return false;
}

float CCSBot::ApplyIdleLookYawVariation(float yawDeg)
{
	const float mag = RANDOM_FLOAT(3.0f, 8.0f);
	const float sign = (RANDOM_LONG(0, 1) == 0) ? -1.0f : 1.0f;
	return NormalizeAngle(yawDeg + sign * mag);
}

float CCSBot::YawDegreesToWorldPos(const Vector &worldAim) const
{
	const Vector to = worldAim - GetEyePosition();
	return UTIL_VecToAngles(to).y;
}

Vector CCSBot::PerturbIdleLookWorldPosition(const Vector &worldAim) const
{
	const Vector eye = GetEyePosition();
	Vector to = worldAim - eye;
	float dist = to.Length();
	if (dist < 1.0f)
		dist = 640.0f;
	else
		to = to * (1.0f / dist);

	const float yaw = UTIL_VecToAngles(to).y;
	const float newYaw = ApplyIdleLookYawVariation(yaw);
	const float deltaDeg = NormalizeAngle(newYaw - yaw);
	const double rad = static_cast<double>(deltaDeg) * (M_PI / 180.0);
	const float c = static_cast<float>(Q_cos(rad));
	const float s = static_cast<float>(Q_sin(rad));
	const Vector hor(to.x * c - to.y * s, to.x * s + to.y * c, to.z);
	return eye + hor * dist;
}

// Moving-only roaming glance targets; calls SetLookAt with PRIORITY_LOW (corner / terrain / danger).
// Orchestrated from UpdateLookAround during full-think Update(), not from Upkeep().
// todo7: non-combat roam MeaningfulPoi uses a short deterministic settle hold (LookTargetSelector) so near-equivalent
// POIs do not flicker; it yields immediately to combat, ladder, precision SetLookAt, grenade, mimic, and chat freeze.
// todo8: fresh threat/noise memory can bypass roam phase defer so POI scoring runs sooner (not combat acquisition / not motor).
// Final view ownership, HumanAimMotorController, NonCombatAimDrift, and pathing are unchanged.
bool CCSBot::UpdateRoamLook(bool updateNow)
{
	static_assert(static_cast<int>(LookTargetSelector::RoamLookPhaseStep::Forward) == static_cast<int>(ROAM_LOOK_FORWARD));
	static_assert(static_cast<int>(LookTargetSelector::RoamLookPhaseStep::DangerAngle) == static_cast<int>(ROAM_LOOK_DANGER_ANGLE));
	static_assert(static_cast<int>(LookTargetSelector::RoamLookPhaseStep::CornerCheck) == static_cast<int>(ROAM_LOOK_CORNER_CHECK));
	static_assert(static_cast<int>(LookTargetSelector::RoamLookPhaseStep::TerrainCheck) == static_cast<int>(ROAM_LOOK_TERRAIN_CHECK));
	static_assert(static_cast<int>(LookTargetSelector::RoamLookPhaseStep::Recenter) == static_cast<int>(ROAM_LOOK_RECENTER));
	static_assert(static_cast<int>(NOT_LOOKING_AT_SPOT) == 0, "RoamLookContext lookAtSpotState uses 0 for not-looking");

	LookTargetSelector::RoamLookContext ctx{};
	ctx.time = gpGlobals->time;
	ctx.updateNow = updateNow;
	ctx.isNotMoving = IsNotMoving();
	ctx.isUsingLadder = IsUsingLadder();
	ctx.isAimingAtEnemy = IsAimingAtEnemy();
	ctx.isSafe = IsSafe();
	ctx.effectiveSkill = TheCSBots()->GetEffectiveSkill(this);
	ctx.lookAtSpotState = static_cast<int>(m_lookAtSpotState);
	ctx.lookAtSpotPriority = m_lookAtSpotPriority;
	ctx.lookAtSpotTimestamp = m_lookAtSpotTimestamp;
	ctx.roamLookPhaseTimestamp = m_roamLookPhaseTimestamp;
	ctx.lookAheadAngle = m_lookAheadAngle;
	ctx.lookAheadPitch = m_lookAheadPitch;

	ctx.currentLookDesc = m_lookAtDesc;
	ctx.lastEnemyPosition = m_lastEnemyPosition;
	ctx.lastSawEnemyTimestamp = m_lastSawEnemyTimestamp;
	ctx.noisePosition = m_noisePosition;
	ctx.noiseTimestamp = m_noiseTimestamp;
	ctx.noiseHeard = IsNoiseHeard();
	ctx.noisePriority = m_noisePriority;
	ctx.approachPointCount = Q_min(m_approachPointCount, static_cast<unsigned char>(16));
	for (int i = 0; i < ctx.approachPointCount && i < 16; ++i)
		ctx.approachPoints[i] = m_approachPoint[i];
	ctx.isHidingOrCamp = IsHiding();
	ctx.chatFreezeLook = AiChat::ShouldFreezeMovementForChat(entindex());
	ctx.isEnemyVisible = IsEnemyVisible();
	{
		CBasePlayer *combatEnemy = GetEnemy();
		ctx.hasValidCombatEnemy = combatEnemy != nullptr && combatEnemy->IsAlive();
	}

	const LookTarget &roamLt = GetLookTarget();
	ctx.blockRoamPoiHold =
		IsThrowingGrenade()
		|| IsDefusingBomb()
		|| AiChat::ShouldFreezeMovementForChat(entindex())
#ifdef REGAMEDLL_ADD
		|| cv_bot_mimic.value > 0
#endif
		|| (roamLt.isActive && roamLt.priority > PRIORITY_LOW)
		|| (roamLt.isActive
			&& (roamLt.reason == LOOK_TARGET_REASON_HOSTAGE || roamLt.reason == LOOK_TARGET_REASON_PLANT_BOMB
				|| roamLt.reason == LOOK_TARGET_REASON_DEFUSE_BOMB || roamLt.reason == LOOK_TARGET_REASON_USE_ENTITY
				|| roamLt.reason == LOOK_TARGET_REASON_BREAKABLE || roamLt.reason == LOOK_TARGET_REASON_GRENADE_THROW));

	LookTargetSelector::RoamLookDecision dec = LookTargetSelector::SelectRoamMovingGlance(ctx, this);

	const bool legacyGlanceTypes =
		dec.applySetLookAt && dec.priority == PRIORITY_LOW
		&& (dec.type == LookTargetSelector::RoamLookDecisionType::DangerAngle
			|| dec.type == LookTargetSelector::RoamLookDecisionType::TerrainLane
			|| dec.type == LookTargetSelector::RoamLookDecisionType::CornerCheck);

	Vector legacyPerturbedPos = dec.lookPosition;
	bool skipLegacyGlance = false;
	if (legacyGlanceTypes)
	{
		legacyPerturbedPos = PerturbIdleLookWorldPosition(dec.lookPosition);
		const float yTry = YawDegreesToWorldPos(legacyPerturbedPos);
		if (!ShouldOverrideIdleLookCommit() && WouldIdleLookPingPong(yTry))
		{
			skipLegacyGlance = true;
			dec.applySetLookAt = false;
			dec.updateRoamPhase = false;
			m_roamLookPhaseTimestamp = gpGlobals->time + RANDOM_FLOAT(0.22f, 0.48f);
		}
	}

	if (dec.updateRoamPhase)
	{
		m_roamLookPhase = static_cast<RoamLookPhase>(static_cast<int>(dec.roamPhaseStep));
		m_roamLookPhaseTimestamp = dec.roamPhaseTimestamp;
	}

	if (dec.applyClearLookAt)
	{
		if (dec.type == LookTargetSelector::RoamLookDecisionType::RecenterClear)
			CancelIdleLookCommit();

		ClearLookAt();
	}

	if (dec.applySetLookAt && !skipLegacyGlance)
	{
		Vector applyPos = dec.lookPosition;

		if (legacyGlanceTypes)
		{
			applyPos = legacyPerturbedPos;
			BeginIdleLookCommit(YawDegreesToWorldPos(applyPos), true);
		}

		SetLookAt(dec.lookDesc, &applyPos, dec.priority, dec.duration, dec.clearIfClose, dec.angleTolerance);

		if (dec.type == LookTargetSelector::RoamLookDecisionType::MeaningfulPoi
			&& dec.debugPoiSource == LookTargetSelector::RoamLookCandidateSource::CampRecorded)
		{
			CampLookRotateAdvance();
		}
	}

	LookTargetSelector::DebugLogRoamDecisionApply(this, ctx, dec);

	return dec.roamLookReturn;
}

// Non-combat look target selection while idle or moving: noise, last enemy, danger memory, approach points,
// encounter spots, UpdateRoamLook, etc. Does not set pev->v_angle; only SetLookAt / ClearLookAt and timing.
// Update the "looking around" behavior.
void CCSBot::UpdateLookAround(bool updateNow)
{
	// check if looking around has been inhibited
	// Moved inhibit to allow high priority enemy lookats to still occur
	if (gpGlobals->time < m_inhibitLookAroundTimestamp)
		return;

	const float recentThreatTime = 0.25f; // 1.0f;

	// Unless we can hear them moving, in which case look towards the noise
	if (!IsEnemyVisible())
	{
		const float noiseStartleRange = 1000.0f;
		if (CanHearNearbyEnemyGunfire(noiseStartleRange))
		{
			Vector spot = m_noisePosition;
			spot.z += HalfHumanHeight;

			SetLookAt("Check dangerous noise", &spot, PRIORITY_HIGH, recentThreatTime);
			InhibitLookAround(RANDOM_FLOAT(2.0f, 4.0f));

			return;
		}
	}

	// If we recently saw an enemy, look towards where we last saw them
	if (!IsLookingAtSpot(PRIORITY_MEDIUM) && gpGlobals->time - m_lastSawEnemyTimestamp < recentThreatTime)
	{
		ClearLookAt();

		Vector spot = m_lastEnemyPosition;

		// find enemy position on the ground
		if (GetSimpleGroundHeight(&m_lastEnemyPosition, &spot.z))
		{
			spot.z += HalfHumanHeight;
			SetLookAt("Last Enemy Position", &spot, PRIORITY_MEDIUM, RANDOM_FLOAT(2.0f, 3.0f), true);
			return;
		}
	}

	if (!IsLookingAtSpot(PRIORITY_MEDIUM) && gpGlobals->time - m_lastSawEnemyTimestamp < 1.65f)
	{
		Vector suspiciousSpot;
		if (BotDangerMemory::FindBestLookSpotNear(this, m_lastEnemyPosition, 720.0f, &suspiciousSpot))
		{
			SetLookAt("Suspicious Last-Seen Danger", &suspiciousSpot, PRIORITY_MEDIUM, RANDOM_FLOAT(0.45f, 1.05f), true, 10.0f);
			return;
		}
	}

	if (IsCommittedMovementIntent(this))
	{
		// Keep rush/objective movement visually committed unless a real threat overrides it.
		if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority <= PRIORITY_LOW)
			ClearLookAt();

		m_roamLookPhaseTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, 0.85f);
		return;
	}

	if (!IsLookingAtSpot(PRIORITY_MEDIUM) && IsLookingIntoCloseGeometry(this))
	{
		const bool stationaryHide = IsHiding() && IsNotMoving();
		const float cooldownRemain = Q_max(0.0f, m_suppressAvoidWallStareUntil - gpGlobals->time);
		const bool legacyExtreme = IsLookingIntoExtremeCloseGeometry(this);

		StationaryWallLookTelemetry wtel{};
		StationaryWallLookQuality wq = StationaryWallLookQuality::WALL_LOOK_BAD_STARE;
		bool extremeTinyCenter = false;

		if (stationaryHide)
		{
			wq = ClassifyStationaryWallLook(this, &wtel);
			extremeTinyCenter = wtel.centerDist < 40.0f;

			if (gpGlobals->time >= m_nextWallLookClassifyLogTime)
			{
				m_nextWallLookClassifyLogTime = gpGlobals->time + 0.5f;
				char wbuf[320];
				Q_snprintf(wbuf, sizeof(wbuf),
					"[wallLookClassify] bot=%s ent=%i state=%s center=%.0f min=%.0f max=%.0f blockedPct=%.2f openCount=%i asym=%.0f result=%s",
					pev ? STRING(pev->netname) : "?",
					entindex(),
					m_state ? m_state->GetName() : "?",
					wtel.centerDist, wtel.minDist, wtel.maxDist, wtel.blockedPct, wtel.openCount, wtel.asymmetry,
					StationaryWallLookQualityString(wq));
				EmitMovementWatchedDebugLine(this, wbuf);
			}
		}

		const bool forceAvoidWall = legacyExtreme || extremeTinyCenter;

		// Mapped camp holds: legacy "extreme close" must not override scripted aim. Do not require
		// stationaryHide or silence(!ShouldOverrideIdleLookCommit) — noise/heard-memory was clearing
		// protection and caused AvoidWallStare <-> Learned Danger ping-pong.
		Vector campAimProbe{};
		const bool campWallSuppression =
			IsHiding()
			&& !IsEnemyVisible()
			&& !IsBlind()
			&& BotCampLookSpots::FindLookTarget(this, &campAimProbe, nullptr);

		const bool classifierAllows = !campWallSuppression
			&& (!stationaryHide
				|| (wq == StationaryWallLookQuality::WALL_LOOK_BAD_STARE)
				|| forceAvoidWall);

		if (!classifierAllows)
		{
			if (gpGlobals->time >= m_nextAvoidWallStareSkipLogTime)
			{
				m_nextAvoidWallStareSkipLogTime = gpGlobals->time + 0.65f;
				const char *rsn;
				if (campWallSuppression)
					rsn = "mappedCampHold";
				else if (wq == StationaryWallLookQuality::WALL_LOOK_CLEAR)
					rsn = "clearCamping";
				else
					rsn = "tightAngle";
				char buf[300];
				Q_snprintf(buf, sizeof(buf),
					"[lookArbitration] skip=AvoidWallStare reason=%s center=%.0f max=%.0f blockedPct=%.2f st=%s task=%i",
					rsn, wtel.centerDist, wtel.maxDist, wtel.blockedPct, m_state ? m_state->GetName() : "?", GetTask());
				EmitMovementWatchedDebugLine(this, buf);
			}
		}
		else if (stationaryHide && !forceAvoidWall && gpGlobals->time < m_suppressAvoidWallStareUntil)
		{
			if (gpGlobals->time >= m_nextAvoidWallStareSkipLogTime)
			{
				m_nextAvoidWallStareSkipLogTime = gpGlobals->time + 0.65f;
				char buf[300];
				Q_snprintf(buf, sizeof(buf),
					"[lookArbitration] skip=AvoidWallStare reason=recentTacticalLook suppressUntil=%.2f remain=%.2f st=%s task=%i",
					m_suppressAvoidWallStareUntil, cooldownRemain, m_state ? m_state->GetName() : "?", GetTask());
				EmitMovementWatchedDebugLine(this, buf);
			}
		}
		else
		{
			Vector openSpot;
			if (FindOpenLookLane(this, &openSpot))
			{
				const char *fireRsn = "badStare";
				if (!stationaryHide)
					fireRsn = "legacyCloseGeom";
				else if (forceAvoidWall && wq != StationaryWallLookQuality::WALL_LOOK_BAD_STARE)
					fireRsn = "extremeClose";

				char buf[320];
				Q_snprintf(buf, sizeof(buf),
					"[lookArbitration] fire=AvoidWallStare reason=%s center=%.0f max=%.0f blockedPct=%.2f extremeClose=%i cooldownRemain=%.2f st=%s task=%i",
					fireRsn,
					stationaryHide ? wtel.centerDist : 0.0f,
					stationaryHide ? wtel.maxDist : 0.0f,
					stationaryHide ? wtel.blockedPct : 0.0f,
					forceAvoidWall ? 1 : 0,
					cooldownRemain,
					m_state ? m_state->GetName() : "?", GetTask());

				ClearLookAt();
				SetLookAt("Avoid Wall Stare", &openSpot, PRIORITY_MEDIUM, RANDOM_FLOAT(1.2f, 2.4f), false, 16.0f);
				EmitMovementWatchedDebugLine(this, buf);
				return;
			}
		}
	}

	// Look at nearby enemy noises
	if (UpdateLookAtNoise())
		return;

	if (IsNotMoving())
	{
		// if we're sniping, zoom in to watch our approach points
		if (IsUsingSniperRifle())
		{
			// low skill bots don't pre-zoom
			if (TheCSBots()->GetEffectiveSkill(this) > 0.4f)
			{
				if (!IsViewMoving())
				{
					if (GetZoomLevel() == NO_ZOOM)
					{
						float range = ComputeWeaponSightRange();
						AdjustZoom(range);
					}
				}
			}
		}

		if (!m_lastKnownArea)
			return;

		if (gpGlobals->time < m_lookAroundStateTimestamp)
			return;

		if (IsIdleLookCommitActive() && !ShouldOverrideIdleLookCommit())
		{
			m_lookAroundStateTimestamp = Q_max(m_lookAroundStateTimestamp, m_idleLookCommitUntil);
			return;
		}

		// if we're sniping, switch look-at spots less often
		if (IsUsingSniperRifle())
			m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(5.0f, 10.0f);
		else
			m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(1.8f, 3.4f);

		// Recorded camp hold angles dominate when at a mapped hold and nothing urgent overrides idle look.
		// Cycles through all valid spots in maps/<map>.camp_look.json (see BotCampLookSpots).
		Vector campLook;
		const char *campSpotName = nullptr;
		if (!ShouldOverrideIdleLookCommit() &&
			BotCampLookSpots::FindLookTargetRotating(this, &campLook, &campSpotName))
		{
			const Vector pCamp = PerturbIdleLookWorldPosition(campLook);
			const float yCamp = YawDegreesToWorldPos(pCamp);
			if (!ShouldOverrideIdleLookCommit() && WouldIdleLookPingPong(yCamp))
			{
				m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, 0.65f);
				return;
			}

			BeginIdleLookCommit(yCamp, false);
			SetLookAheadAngle(yCamp);
			SetLookAt("Recorded camp look", &pCamp, PRIORITY_MEDIUM, RANDOM_FLOAT(1.8f, 3.2f), false, 8.0f);
			(void)campSpotName;
			return;
		}

		Vector dangerSpot;
		if (BotDangerMemory::FindBestLookSpot(this, &dangerSpot))
		{
			const Vector pDanger = PerturbIdleLookWorldPosition(dangerSpot);
			const float yDanger = YawDegreesToWorldPos(pDanger);
			if (!ShouldOverrideIdleLookCommit() && WouldIdleLookPingPong(yDanger))
			{
				m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, 0.65f);
				return;
			}

			BeginIdleLookCommit(yDanger, false);
			SetLookAt("Learned Danger", &pDanger, PRIORITY_MEDIUM, RANDOM_FLOAT(2.0f, 4.0f), false, 12.0f);
			return;
		}

		if (m_approachPointCount == 0)
		{
			Vector openSpot;
			if (FindOpenLookLane(this, &openSpot))
			{
				CampEdgeBiasTelemetry edgeTel{};
				AdjustStationaryCampLookForEdgeBias(&openSpot, &edgeTel);
				EmitCampLookCandidateLine(this, "openLaneHiding", edgeTel);
				const Vector pOpen = PerturbIdleLookWorldPosition(openSpot);
				const float yOpen = YawDegreesToWorldPos(pOpen);
				if (!ShouldOverrideIdleLookCommit() && WouldIdleLookPingPong(yOpen))
				{
					m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, 0.65f);
					return;
				}

				BeginIdleLookCommit(yOpen, false);
				SetLookAt("Open Lane (Hiding)", &pOpen, PRIORITY_MEDIUM, RANDOM_FLOAT(1.8f, 3.2f), false, 16.0f);
			}
			else if (!IsIdleLookCommitActive() || ShouldOverrideIdleLookCommit())
				ClearLookAt();
			return;
		}

		const int apCount = Q_min(static_cast<int>(m_approachPointCount), 16);
		float scoreOf[16]{};
		for (int i = 0; i < apCount; ++i)
		{
			Vector point = m_approachPoint[i];
			point.z += HalfHumanHeight;

			TraceResult result;
			UTIL_TraceLine(GetEyePosition(), point, ignore_monsters, ignore_glass, ENT(pev), &result);

			const float range = (m_approachPoint[i] - pev->origin).Length2D();
			float score = range;
			if (result.flFraction >= 1.0f)
				score += 650.0f;
			else
				score += 250.0f * result.flFraction;

			if (range < 220.0f)
				score -= 400.0f;
			if (result.flFraction < 0.45f)
				score -= 700.0f;

			scoreOf[i] = score;
		}

		int order[16]{};
		for (int i = 0; i < apCount; ++i)
			order[i] = i;

		for (int a = 0; a < apCount; ++a)
		{
			for (int b = a + 1; b < apCount; ++b)
			{
				if (scoreOf[order[a]] < scoreOf[order[b]])
				{
					const int tmp = order[a];
					order[a] = order[b];
					order[b] = tmp;
				}
			}
		}

		int chosen = -1;
		Vector spotChosen;
		for (int ord = 0; ord < apCount; ++ord)
		{
			const int i = order[ord];
			Vector spotTry = m_approachPoint[i];
			TraceResult check;
			Vector checkSpotTry = spotTry + Vector(0, 0, HalfHumanHeight);
			UTIL_TraceLine(GetEyePosition(), checkSpotTry, ignore_monsters, ignore_glass, ENT(pev), &check);
			if (check.flFraction < 0.45f)
				continue;

			spotTry.z += HalfHumanHeight;
			CampEdgeBiasTelemetry edgeTel{};
			AdjustStationaryCampLookForEdgeBias(&spotTry, &edgeTel);
			EmitCampLookCandidateLine(this, "approachPointHiding", edgeTel);
			const Vector pAp = PerturbIdleLookWorldPosition(spotTry);
			const float yAp = YawDegreesToWorldPos(pAp);
			if (!ShouldOverrideIdleLookCommit() && WouldIdleLookPingPong(yAp))
				continue;

			chosen = i;
			spotChosen = pAp;
			break;
		}

		if (chosen < 0)
		{
			Vector openSpot;
			if (FindOpenLookLane(this, &openSpot))
			{
				CampEdgeBiasTelemetry edgeTel{};
				AdjustStationaryCampLookForEdgeBias(&openSpot, &edgeTel);
				EmitCampLookCandidateLine(this, "openLaneHidingFallback", edgeTel);
				const Vector pOpen = PerturbIdleLookWorldPosition(openSpot);
				const float yOpen = YawDegreesToWorldPos(pOpen);
				if (!ShouldOverrideIdleLookCommit() && WouldIdleLookPingPong(yOpen))
				{
					m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, 0.65f);
					return;
				}

				BeginIdleLookCommit(yOpen, false);
				SetLookAt("Open Lane (Hiding)", &pOpen, PRIORITY_MEDIUM, RANDOM_FLOAT(1.8f, 3.2f), false, 16.0f);
				return;
			}

			m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, 0.65f);
			return;
		}

		BeginIdleLookCommit(YawDegreesToWorldPos(spotChosen), false);
		SetLookAt("Approach Point (Hiding)", &spotChosen, PRIORITY_MEDIUM, RANDOM_FLOAT(2.0f, 4.0f), false, 12.0f);
		return;
	}

	if (UpdateRoamLook(updateNow))
		return;

	// Glance at "encouter spots" as we move past them
	if (m_spotEncounter)
	{
		// Check encounter spots
		if (!IsSafe() && !IsLookingAtSpot(PRIORITY_LOW))
		{
			// allow a short time to look where we're going
			if (gpGlobals->time < m_spotCheckTimestamp)
				return;

			// TODO: Use skill parameter instead of accuracy

			// lower skills have exponentially longer delays
		real_t asleep = (1.0f - TheCSBots()->GetEffectiveSkill(this));
			asleep *= asleep;
			asleep *= asleep;

			m_spotCheckTimestamp = gpGlobals->time + asleep * RANDOM_FLOAT(14.0f, 42.0f);

			// figure out how far along the path segment we are
			Vector delta = m_spotEncounter->path.to - m_spotEncounter->path.from;
			real_t length = delta.Length();

#ifdef REGAMEDLL_FIXES
			float adx = Q_abs(delta.x);
			float ady = Q_abs(delta.y);
#else
			float adx = float(Q_abs(int64(delta.x)));
			float ady = float(Q_abs(int64(delta.y)));
#endif
			real_t t;

			if (adx > ady)
				t = (pev->origin.x - m_spotEncounter->path.from.x) / delta.x;
			else
				t = (pev->origin.y - m_spotEncounter->path.from.y) / delta.y;

			// advance parameter a bit so we "lead" our checks
			const float leadCheckRange = 50.0f;
			t += leadCheckRange / length;

			if (t < 0.0f)
				t = 0.0f;
			else if (t > 1.0f)
				t = 1.0f;

			// collect the unchecked spots so far
			const int MAX_DANGER_SPOTS = 8;
			HidingSpot *dangerSpot[MAX_DANGER_SPOTS];
			int dangerSpotCount = 0;
			int dangerIndex = 0;

			const float checkTime = 10.0f;

			for (auto &spotOrder : m_spotEncounter->spotList)
			{
				// if we have seen this spot recently, we don't need to look at it
				if (gpGlobals->time - GetHidingSpotCheckTimestamp(spotOrder.spot) <= checkTime)
					continue;

				if (spotOrder.t > t)
					break;

				dangerSpot[dangerIndex++] = spotOrder.spot;
				if (dangerIndex >= MAX_DANGER_SPOTS)
					dangerIndex = 0;

				if (dangerSpotCount < MAX_DANGER_SPOTS)
					dangerSpotCount++;
			}

			if (dangerSpotCount)
			{
				// pick one of the spots at random
				int which = RANDOM_LONG(0, dangerSpotCount - 1);

				const Vector *checkSpot = dangerSpot[which]->GetPosition();

				Vector pos = *checkSpot;
				pos.z += HalfHumanHeight;

				// glance at the spot for minimum time
				SetLookAt("Encounter Spot", &pos, PRIORITY_LOW, 0, true, 14.0f);

				// immediately mark it as "checked", so we don't check it again
				// if we get distracted before we check it - that's the way it goes
				SetHidingSpotCheckTimestamp(dangerSpot[which]);
			}
		}
	}
}

// "Bend" our line of sight around corners until we can "see" the point.
bool CCSBot::BendLineOfSight(const Vector *eye, const Vector *point, Vector *bend) const
{
	// if we can directly see the point, use it
	TraceResult result;
	UTIL_TraceLine(*eye, *point + Vector(0, 0, HalfHumanHeight), ignore_monsters, ENT(pev), &result);

	if (result.flFraction == 1.0f && !result.fStartSolid)
	{
		// can directly see point, no bending needed
		*bend = *point;
		return true;
	}

	// "bend" our line of sight until we can see the approach point
	Vector v = *point - *eye;
	float startAngle = UTIL_VecToYaw(v);
	float length = v.Length2D();
	v.NormalizeInPlace();

	float angleInc = 10.0f;
	for (float angle = angleInc; angle <= 135.0f; angle += angleInc)
	{
		// check both sides at this angle offset
		for (int side = 0; side < 2; side++)
		{
			float actualAngle = side ? (startAngle + angle) : (startAngle - angle);

			float dx = BotCOS(actualAngle);
			float dy = BotSIN(actualAngle);

			// compute rotated point ray endpoint
			Vector rotPoint(eye->x + length * dx, eye->y + length * dy, point->z);

			TraceResult result;
			UTIL_TraceLine(*eye, rotPoint + Vector(0, 0, HalfHumanHeight), ignore_monsters, ENT(pev), &result);

			// if this ray started in an obstacle, skip it
			if (result.fStartSolid)
			{
				continue;
			}

			Vector ray = rotPoint - *eye;
			float rayLength = ray.NormalizeInPlace();
			float visibleLength = rayLength * result.flFraction;

			// step along ray, checking if point is visible from ray point
			const float bendStepSize = 50.0f;
			for (float bendLength = bendStepSize; bendLength <= visibleLength; bendLength += bendStepSize)
			{
				// compute point along ray
				Vector rayPoint = *eye + bendLength * ray;

				// check if we can see approach point from this bend point
				UTIL_TraceLine(rayPoint, *point + Vector(0, 0, HalfHumanHeight), ignore_monsters, ENT(pev), &result);

				if (result.flFraction == 1.0f && !result.fStartSolid)
				{
					// target is visible from this bend point on the ray - use this point on the ray as our point

					// keep "bent" point at correct height along line of sight
					if (!GetGroundHeight(&rayPoint, &rayPoint.z))
					{
						rayPoint.z = point->z;
					}

					*bend = rayPoint;
					return true;
				}
			}
		}
	}

	*bend = *point;

	// bending rays didn't help - still can't see the point
	return false;
}

CBasePlayer *CCSBot::FindMostDangerousThreat()
{
	// maximum number of simulataneously attendable threats
#ifdef REGAMEDLL_FIXES
	const int MAX_THREATS = MAX_CLIENTS;
#else
	const int MAX_THREATS = 16;
#endif

	struct CloseInfo
	{
		CBasePlayer *enemy;
		float range;
	};

	CloseInfo threat[MAX_THREATS];
	int threatCount = 0;

#ifdef REGAMEDLL_ADD
	int prevIndex = m_enemyQueueIndex - 1;
	if (prevIndex < 0)
		prevIndex = MAX_ENEMY_QUEUE - 1;

	CBasePlayer *currentThreat = m_enemyQueue[prevIndex].player;
#endif

	m_bomber = nullptr;
	m_closestVisibleFriend = nullptr;
	m_closestVisibleHumanFriend = nullptr;

#ifdef REGAMEDLL_ADD
	m_isEnemySniperVisible = false;
	CBasePlayer* sniperThreat = NULL;
	float sniperThreatRange = 99999999999.9f;
	bool sniperThreatIsFacingMe = false;
#endif

	float closeFriendRange = 99999999999.9f;
	float closeHumanFriendRange = 99999999999.9f;

	int i;

	{
		for (i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);

			if (!UTIL_IsValidPlayer(pPlayer))
				continue;

			// is it a player?
			if (!pPlayer->IsPlayer())
				continue;

			// ignore self
			if (pPlayer->entindex() == entindex())
				continue;

			// is it alive?
			if (!pPlayer->IsAlive())
				continue;

#ifdef REGAMEDLL_ADD // REGAMEDLL_FIXES ?
			if ((pPlayer->pev->flags & FL_NOTARGET) || (pPlayer->pev->effects & EF_NODRAW))
				continue;
#endif
			// is it an enemy?
			if (BotRelationship(pPlayer) == BOT_TEAMMATE)
			{
				TraceResult result;
				UTIL_TraceLine(GetEyePosition(), pPlayer->pev->origin, ignore_monsters, ignore_glass, edict(), &result);
				if (result.flFraction == 1.0f)
				{
					// update watch timestamp
					int idx = pPlayer->entindex() - 1;
					m_watchInfo[idx].timestamp = gpGlobals->time;
					m_watchInfo[idx].isEnemy = false;

					// keep track of our closest friend
					Vector to = pev->origin - pPlayer->pev->origin;
					float rangeSq = to.LengthSquared();
					if (rangeSq < closeFriendRange)
					{
						m_closestVisibleFriend = pPlayer;
						closeFriendRange = rangeSq;
					}

					// keep track of our closest human friend
					if (!pPlayer->IsBot() && rangeSq < closeHumanFriendRange)
					{
						m_closestVisibleHumanFriend = pPlayer;
						closeHumanFriendRange = rangeSq;
					}
				}

				continue;
			}

			// check if this enemy is fully
			unsigned char visParts;
			if (!IsVisible(pPlayer, CHECK_FOV, &visParts))
				continue;

#ifdef REGAMEDLL_ADD
			// do we notice this enemy? (always notice current enemy)
			if (pPlayer != currentThreat)
			{
				if (!IsNoticable(pPlayer, visParts))
				{
					continue;
				}
			}
#endif

			// update watch timestamp
			int idx = pPlayer->entindex() - 1;
			m_watchInfo[idx].timestamp = gpGlobals->time;
			m_watchInfo[idx].isEnemy = true;
			BotDangerMemory::RecordSighting(this, pPlayer);

			// note if we see the bomber
			if (pPlayer->IsBombGuy())
			{
				m_bomber = pPlayer;
			}

			// keep track of all visible threats
			Vector d = pev->origin - pPlayer->pev->origin;
			float distSq = d.LengthSquared();

#ifdef REGAMEDLL_ADD
			CBasePlayerWeapon *pCurrentWeapon = static_cast<CBasePlayerWeapon *>(pPlayer->m_pActiveItem);
			if (pCurrentWeapon && isSniperRifle(pCurrentWeapon))
			{
				m_isEnemySniperVisible = true;
				if (sniperThreat)
				{
					if (IsPlayerLookingAtMe(pPlayer))
					{
						if (sniperThreatIsFacingMe)
						{
							// several snipers are facing us - keep closest
							if (distSq < sniperThreatRange)
							{
								sniperThreat = pPlayer;
								sniperThreatRange = distSq;
								sniperThreatIsFacingMe = true;
							}
						}
						else
						{
							// even if this sniper is farther away, keep it because he's aiming at us
							sniperThreat = pPlayer;
							sniperThreatRange = distSq;
							sniperThreatIsFacingMe = true;
						}
					}
					else
					{
						// this sniper is not looking at us, only consider it if we dont have a sniper facing us
						if (!sniperThreatIsFacingMe && distSq < sniperThreatRange)
						{
							sniperThreat = pPlayer;
							sniperThreatRange = distSq;
						}
					}
				}
				else
				{
					// first sniper we see
					sniperThreat = pPlayer;
					sniperThreatRange = distSq;
					sniperThreatIsFacingMe = IsPlayerLookingAtMe(pPlayer);
				}
			}
#endif

			// maintain set of visible threats, sorted by increasing distance
			if (threatCount == 0)
			{
				threat[0].enemy = pPlayer;
				threat[0].range = distSq;
				threatCount = 1;
			}
			else
			{
				// find insertion point
				int j;
				for (j = 0; j < threatCount; j++)
				{
					if (distSq < threat[j].range)
						break;
				}

				// shift lower half down a notch
				for (int k = threatCount - 1; k >= j; k--)
					threat[k + 1] = threat[k];

				// insert threat into sorted list
				threat[j].enemy = pPlayer;
				threat[j].range = distSq;

				if (threatCount < MAX_THREATS)
					threatCount++;
			}
		}
	}
	{
		// track the maximum enemy and friend counts we've seen recently
		int prevEnemies = m_nearbyEnemyCount;
		int prevFriends = m_nearbyFriendCount;
		m_nearbyEnemyCount = 0;
		m_nearbyFriendCount = 0;

		for (i = 0; i < MAX_CLIENTS; i++)
		{
			if (m_watchInfo[i].timestamp <= 0.0f)
				continue;

			const float recentTime = 3.0f;
			if (gpGlobals->time - m_watchInfo[i].timestamp < recentTime)
			{
				if (m_watchInfo[i].isEnemy)
					m_nearbyEnemyCount++;
				else
					m_nearbyFriendCount++;
			}
		}

		// note when we saw this batch of enemies
		if (prevEnemies == 0 && m_nearbyEnemyCount > 0)
		{
			m_firstSawEnemyTimestamp = gpGlobals->time;
		}

		if (prevEnemies != m_nearbyEnemyCount || prevFriends != m_nearbyFriendCount)
		{
			PrintIfWatched("Nearby friends = %d, enemies = %d\n", m_nearbyFriendCount, m_nearbyEnemyCount);
		}
	}
	{
		// Track the place where we saw most of our enemies
		struct PlaceRank
		{
			unsigned int place;
			int count;
		};
		static PlaceRank placeRank[MAX_PLACES_PER_MAP];
		int locCount = 0;

		PlaceRank common;
		common.place = 0;
		common.count = 0;

		for (i = 0; i < threatCount; i++)
		{
			// find the area the player/bot is standing on
			CNavArea *area;
			CCSBot *pBot = static_cast<CCSBot *>(threat[i].enemy);

			if (pBot->IsBot())
			{
				area = pBot->GetLastKnownArea();
			}
			else
			{
				area = TheNavAreaGrid.GetNearestNavArea(&threat[i].enemy->pev->origin);
			}

			if (!area)
				continue;

			unsigned int threatLoc = area->GetPlace();
			if (!threatLoc)
				continue;

			// if place is already in set, increment count
			int j;
			for (j = 0; j < locCount; j++)
			{
				if (placeRank[j].place == threatLoc)
					break;
			}

			if (j == locCount)
			{
				// new place
				if (locCount < MAX_PLACES_PER_MAP)
				{
					placeRank[locCount].place = threatLoc;
					placeRank[locCount].count = 1;

					if (common.count == 0)
						common = placeRank[locCount];

					locCount++;
				}
			}
			else
			{
				// others are in that place, increment
				placeRank[j].count++;

				// keep track of the most common place
				if (placeRank[j].count > common.count)
					common = placeRank[j];
			}
		}

		// remember most common place
		m_enemyPlace = common.place;
	}

	{
		if (threatCount == 0)
			return nullptr;

		int t;

#ifdef REGAMEDLL_ADD
		bool sawCloserThreat = false;
		bool sawCurrentThreat = false;
		for (t = 0; t < threatCount; t++)
		{
			if (threat[t].enemy == currentThreat)
			{
				sawCurrentThreat = true;
			}
			else if (threat[t].enemy != currentThreat && IsSignificantlyCloser(threat[t].enemy, currentThreat))
			{
				sawCloserThreat = true;
			}
		}

		if (sawCurrentThreat && !sawCloserThreat)
		{
			return currentThreat;
		}

		// if we are a sniper and we see a sniper threat, attack it unless
		// there are other close enemies facing me
		if (IsSniper() && sniperThreat)
		{
			const float closeCombatRange = 500.0f;

			for (t = 0; t < threatCount; ++t)
			{
				if (threat[t].range < closeCombatRange && IsPlayerLookingAtMe(threat[t].enemy))
				{
					return threat[t].enemy;
				}
			}

			return sniperThreat;
		}
#endif

		// otherwise, find the closest threat that without using shield
		for (t = 0; t < threatCount; t++)
		{
			if (!threat[t].enemy->IsProtectedByShield())
			{
				return threat[t].enemy;
			}
		}
	}

	// return closest threat
	return threat[0].enemy;
}

// Update our reaction time queue
void CCSBot::UpdateReactionQueue()
{
	// zombies dont see any threats
	if (cv_bot_zombie.value > 0.0f)
		return;

	// find biggest threat at this instant
	CBasePlayer *threat = FindMostDangerousThreat();

	int now = m_enemyQueueIndex;

#ifdef REGAMEDLL_ADD
	// reset timer
	m_attentionInterval.Start();
#endif

	// store a snapshot of its state at the end of the reaction time queue
	if (threat)
	{
		m_enemyQueue[now].player = threat;
		m_enemyQueue[now].isReloading = threat->IsReloading();
		m_enemyQueue[now].isProtectedByShield = threat->IsProtectedByShield();
	}
	else
	{
		m_enemyQueue[now].player = nullptr;
		m_enemyQueue[now].isReloading = false;
		m_enemyQueue[now].isProtectedByShield = false;
	}

	// queue is round-robin
	if (++m_enemyQueueIndex >= MAX_ENEMY_QUEUE)
		m_enemyQueueIndex = 0;

	if (m_enemyQueueCount < MAX_ENEMY_QUEUE)
		m_enemyQueueCount++;

	// clamp reaction time to enemy queue size
	float reactionTime = GetProfile()->GetReactionTime();
	float maxReactionTime = (MAX_ENEMY_QUEUE * g_flBotFullThinkInterval) - 0.01f;
	if (reactionTime > maxReactionTime)
		reactionTime = maxReactionTime;

	// "rewind" time back to our reaction time
	int reactionTimeSteps = int((reactionTime / g_flBotFullThinkInterval) + 0.5f);

	int i = now - reactionTimeSteps;
	if (i < 0)
		i += MAX_ENEMY_QUEUE;

	m_enemyQueueAttendIndex = byte(i);
}

// Return the most dangerous threat we are "conscious" of
CBasePlayer *CCSBot::GetRecognizedEnemy()
{
	if (m_enemyQueueAttendIndex >= m_enemyQueueCount)
		return nullptr;

	return m_enemyQueue[m_enemyQueueAttendIndex].player;
}

// Return true if the enemy we are "conscious" of is reloading
bool CCSBot::IsRecognizedEnemyReloading()
{
	if (m_enemyQueueAttendIndex >= m_enemyQueueCount)
		return false;

	return m_enemyQueue[m_enemyQueueAttendIndex].isReloading;
}

// Return true if the enemy we are "conscious" of is hiding behind a shield
bool CCSBot::IsRecognizedEnemyProtectedByShield()
{
	if (m_enemyQueueAttendIndex >= m_enemyQueueCount)
		return false;

	return m_enemyQueue[m_enemyQueueAttendIndex].isProtectedByShield;
}

// Return distance to closest enemy we are "conscious" of
float CCSBot::GetRangeToNearestRecognizedEnemy()
{
	const CBasePlayer *pEnemy = GetRecognizedEnemy();

	if (pEnemy)
	{
		return (pev->origin - pEnemy->pev->origin).Length();
	}

	return 99999999.9f;
}

// Blind the bot for the given duration
void CCSBot::Blind(float duration, float holdTime, float fadeTime, int alpha)
{
	// extend
	CBasePlayer::Blind(duration, holdTime, fadeTime, alpha);

	PrintIfWatched("I'm blind!\n");

	if (RANDOM_FLOAT(0.0f, 100.0f) < 33.3f)
	{
		GetChatter()->Say("Blinded", 1.0f);
	}

	// decide which way to move while blind
	m_blindMoveDir = static_cast<NavRelativeDirType>(RANDOM_LONG(1, NUM_RELATIVE_DIRECTIONS - 1));

	// if blinded while in combat - then spray and pray!
	m_blindFire = (RANDOM_FLOAT(0.0f, 100.0f) < 10.0f) != 0;

	// no longer safe
	AdjustSafeTime();
}

#ifdef REGAMEDLL_ADD
bool CCSBot::IsNoticable(const CBasePlayer *pPlayer, unsigned char visibleParts) const
{
	float deltaT = m_attentionInterval.GetElapsedTime();

	// all chances are specified in terms of a standard "quantum" of time
	// in which a normal person would notice something
	const float noticeQuantum = 0.25f;

	// determine percentage of player that is visible
	float coverRatio = 0.0f;

	if (visibleParts & CHEST)
	{
		const float chance = 40.0f;
		coverRatio += chance;
	}

	if (visibleParts & HEAD)
	{
		const float chance = 10.0f;
		coverRatio += chance;
	}

	if (visibleParts & LEFT_SIDE)
	{
		const float chance = 20.0f;
		coverRatio += chance;
	}

	if (visibleParts & RIGHT_SIDE)
	{
		const float chance = 20.0f;
		coverRatio += chance;
	}

	if (visibleParts & FEET)
	{
		const float chance = 10.0f;
		coverRatio += chance;
	}

	// compute range modifier - farther away players are harder to notice, depeding on what they are doing
	float range = (pPlayer->pev->origin - pev->origin).Length();
	const float closeRange = 300.0f;
	const float farRange = 1000.0f;

	float rangeModifier;
	if (range < closeRange)
	{
		rangeModifier = 0.0f;
	}
	else if (range > farRange)
	{
		rangeModifier = 1.0f;
	}
	else
	{
		rangeModifier = (range - closeRange) / (farRange - closeRange);
	}

	// harder to notice when crouched
	bool isCrouching = (pPlayer->pev->flags & FL_DUCKING) == FL_DUCKING;
	// moving players are easier to spot
	float playerSpeedSq = pPlayer->pev->velocity.LengthSquared();
	const float runSpeed = 200.0f;
	const float walkSpeed = 30.0f;
	float farChance, closeChance;
	if (playerSpeedSq > runSpeed * runSpeed)
	{
		// running players are always easy to spot (must be standing to run)
		return true;
	}
	else if (playerSpeedSq > walkSpeed * walkSpeed)
	{
		// walking players are less noticable far away
		if (isCrouching)
		{
			closeChance = 90.0f;
			farChance = 60.0f;
		}
		// standing
		else
		{
			closeChance = 100.0f;
			farChance = 75.0f;
		}
	}
	else
	{
		// motionless players are hard to notice
		if (isCrouching)
		{
			// crouching and motionless - very tough to notice
			closeChance = 80.0f;
			farChance = 5.0f;		// takes about three seconds to notice (50% chance)
		}
		// standing
		else
		{
			closeChance = 100.0f;
			farChance = 10.0f;
		}
	}

	// combine posture, speed, and range chances
	float dispositionChance = closeChance + (farChance - closeChance) * rangeModifier;

	// determine actual chance of noticing player
	float noticeChance = dispositionChance * coverRatio/100.0f;

	// scale by skill level
	noticeChance *= (0.5f + 0.5f * TheCSBots()->GetEffectiveSkill(this));

	// if we are alert, our chance of noticing is much higher
	//if (IsAlert())
	//{
	//	const float alertBonus = 50.0f;
	//	noticeChance += alertBonus;
	//}

	// scale by time quantum
	noticeChance *= deltaT / noticeQuantum;

	// there must always be a chance of detecting the enemy
	const float minChance = 0.1f;
	if (noticeChance < minChance)
	{
		noticeChance = minChance;
	}

	//PrintIfWatched("Notice chance = %3.2f\n", noticeChance);
	return (RANDOM_FLOAT(0.0f, 100.0f) < noticeChance);
}
#endif
