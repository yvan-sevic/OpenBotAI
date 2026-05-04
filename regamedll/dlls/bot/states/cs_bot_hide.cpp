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
#include "bot/cs_bot_camp_look_spots.h"
#include "bot/cs_bot_camp_memory.h"
#include "bot/cs_bot_movement_debug.h"

namespace
{

void EmitCampArrivalLookDebug(
	CCSBot *me,
	const char *source,
	const char *ctxType,
	float yawDeg,
	const Vector *pos,
	float score,
	float duration,
	int perturbApplied,
	unsigned areaId,
	const CCSBot::CampEdgeBiasTelemetry *edge = nullptr)
{
	if (!me || !gpGlobals)
		return;

	const float px = pos ? pos->x : 0.0f;
	const float py = pos ? pos->y : 0.0f;
	const float pz = pos ? pos->z : 0.0f;

	char buf[768];

	if (edge)
	{
		Q_snprintf(buf, sizeof(buf),
			"[campArrivalLook] t=%.3f bot=%s ent=%i src=%s ctx=%s area=%u yaw=%.2f pos=(%.0f,%.0f,%.0f) score=%.1f dur=%.2f pert=%i st=Hide task=%i"
			" edge baseYaw=%.2f finalYaw=%.2f off=%.2f baseDist=%.0f finalDist=%.0f scBase=%.1f scBest=%.1f applied=%i rej=%s",
			gpGlobals->time,
			me->pev ? STRING(me->pev->netname) : "?",
			me->entindex(),
			source ? source : "-",
			ctxType ? ctxType : "none",
			areaId,
			yawDeg,
			px, py, pz,
			score,
			duration,
			perturbApplied,
			static_cast<int>(me->GetTask()),
			edge->baseYaw, edge->finalYaw, edge->yawOffset,
			edge->baseTraceDist, edge->finalTraceDist,
			edge->edgeScoreBase, edge->edgeScoreBest,
			edge->applied ? 1 : 0,
			edge->rejectReason[0] ? edge->rejectReason : "-");
	}
	else
	{
		Q_snprintf(buf, sizeof(buf),
			"[campArrivalLook] t=%.3f bot=%s ent=%i src=%s ctx=%s area=%u yaw=%.2f pos=(%.0f,%.0f,%.0f) score=%.1f dur=%.2f pert=%i st=Hide task=%i",
			gpGlobals->time,
			me->pev ? STRING(me->pev->netname) : "?",
			me->entindex(),
			source ? source : "-",
			ctxType ? ctxType : "none",
			areaId,
			yawDeg,
			px, py, pz,
			score,
			duration,
			perturbApplied,
			static_cast<int>(me->GetTask()));
	}

	EmitMovementWatchedDebugLine(me, buf);
}

} // namespace

static bool IsGuardTask(CCSBot *me)
{
	if (!me)
		return false;

	switch (me->GetTask())
	{
	case CCSBot::GUARD_TICKING_BOMB:
	case CCSBot::GUARD_BOMB_DEFUSER:
	case CCSBot::GUARD_LOOSE_BOMB:
	case CCSBot::GUARD_BOMB_ZONE:
	case CCSBot::HOLD_POSITION:
	case CCSBot::GUARD_VIP_ESCAPE_ZONE:
	case CCSBot::GUARD_HOSTAGES:
	case CCSBot::GUARD_HOSTAGE_RESCUE_ZONE:
	case CCSBot::SNIPING:
	case CCSBot::MOVE_TO_SNIPER_SPOT:
		return true;
	default:
		return false;
	}
}

static float GetContextualAmbushChance(CCSBot *me)
{
	float chance = 34.0f;
	if (!me)
		return chance;

	chance += me->GetProfile()->GetAggression() * 18.0f;
	chance += TheCSBots()->GetEffectiveSkill(me) * 16.0f;
	chance -= me->GetProfile()->GetTeamwork() * 8.0f;

	if (me->IsSniper())
		chance += 22.0f;
	if (IsGuardTask(me))
		chance += 18.0f;
	if (me->IsFollowing())
		chance -= 28.0f;
	if (me->IsRogue())
		chance += 10.0f;
	if (me->GetMorale() < CCSBot::NEUTRAL)
		chance += float(-me->GetMorale()) * 7.0f;

	return Q_max(12.0f, Q_min(chance, 86.0f));
}

static float GetContextualHoldTime(CCSBot *me)
{
	if (!me)
		return RANDOM_FLOAT(3.0f, 10.0f);

	float minTime = 2.5f + TheCSBots()->GetEffectiveSkill(me) * 2.0f;
	float maxTime = 8.0f + me->GetProfile()->GetAggression() * 6.0f;

	if (me->IsSniper())
	{
		minTime += 4.0f;
		maxTime += 10.0f;
	}

	if (IsGuardTask(me))
	{
		minTime += 2.0f;
		maxTime += 8.0f;
	}

	if (me->IsFollowing())
	{
		minTime = Q_min(minTime, 3.0f);
		maxTime = Q_min(maxTime, 6.0f);
	}

	return RANDOM_FLOAT(minTime, Q_max(minTime + 1.0f, maxTime));
}

// Begin moving to a nearby hidey-hole.
// NOTE: Do not forget this state may include a very long "move-to" time to get to our hidey spot!
void HideState::OnEnter(CCSBot *me)
{
	m_isAtSpot = false;

	// if duration is "infinite", set it to a reasonably long time to prevent infinite camping
	if (m_duration < 0.0f)
	{
		m_duration = RANDOM_FLOAT(30.0f, 60.0f);
	}

	// decide whether to "ambush" or not - never set to false so as not to override external setting
	if (!m_isHoldingPosition && RANDOM_FLOAT(0.0f, 100.0f) < GetContextualAmbushChance(me))
	{
		m_isHoldingPosition = true;
	}

	// if we are holding position, decide for how long
	if (m_isHoldingPosition)
	{
		m_holdPositionTime = GetContextualHoldTime(me);
	}
	else
	{
		m_holdPositionTime = 0.0f;
	}

	m_heardEnemy = false;
	m_firstHeardEnemyTime = 0.0f;
	m_retry = 0;

	if (me->IsFollowing())
	{
		m_leaderAnchorPos = me->GetFollowLeader()->pev->origin;
	}
}

// Move to a nearby hidey-hole.
// NOTE: Do not forget this state may include a very long "move-to" time to get to our hidey spot!
void HideState::OnUpdate(CCSBot *me)
{
	// wait until finished reloading to leave hide state
	if (!me->IsActiveWeaponReloading())
	{
		if (gpGlobals->time - me->GetStateTimestamp() > m_duration)
		{
			if (me->GetTask() == CCSBot::GUARD_LOOSE_BOMB)
			{
				// if we're guarding the loose bomb, continue to guard it but pick a new spot
				me->Hide(TheCSBots()->GetLooseBombArea());
				return;
			}
			else if (me->GetTask() == CCSBot::GUARD_BOMB_ZONE)
			{
				// if we're guarding a bombsite, continue to guard it but pick a new spot
				const CCSBotManager::Zone *zone = TheCSBots()->GetClosestZone(&me->pev->origin);
				if (zone)
				{
					CNavArea *area = TheCSBots()->GetRandomAreaInZone(zone);
					if (area)
					{
						me->Hide(area);
						return;
					}
				}
			}
			else if (me->GetTask() == CCSBot::GUARD_HOSTAGE_RESCUE_ZONE)
			{
				// if we're guarding a rescue zone, continue to guard this or another rescue zone
				if (me->GuardRandomZone())
				{
					me->SetTask(CCSBot::GUARD_HOSTAGE_RESCUE_ZONE);
					me->PrintIfWatched("Continuing to guard hostage rescue zones\n");
					me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
					me->GetChatter()->GuardingHostageEscapeZone(IS_PLAN);
					return;
				}
			}

			me->Idle();
			return;
		}

		// if we are momentarily hiding while following someone, check to see if he has moved on
		if (me->IsFollowing())
		{
			CBasePlayer *pLeader = me->GetFollowLeader();

			// BOTPORT: Determine walk/run velocity thresholds
			float runThreshold = 200.0f;
			if (pLeader->pev->velocity.IsLengthGreaterThan(runThreshold))
			{
				// leader is running, stay with him
				me->Follow(pLeader);
				return;
			}

			// if leader has moved, stay with him
			const float followRange = 250.0f;
			if ((m_leaderAnchorPos - pLeader->pev->origin).IsLengthGreaterThan(followRange))
			{
				me->Follow(pLeader);
				return;
			}
		}

		// if we see a nearby buddy in combat, join him
		// TODO: Perhaps tie in to TakeDamage(), so it works for human players, too

		// Scenario logic
		switch (TheCSBots()->GetScenario())
		{
		case CCSBotManager::SCENARIO_DEFUSE_BOMB:
		{
			if (me->m_iTeam == CT)
			{
				// if we are just holding position (due to a radio order) and the bomb has just planted, go defuse it
				if (me->GetTask() == CCSBot::HOLD_POSITION &&
					TheCSBots()->IsBombPlanted() &&
					TheCSBots()->GetBombPlantTimestamp() > me->GetStateTimestamp())
				{
					me->Idle();
					return;
				}

				// if we are guarding the defuser and he dies/gives up, stop hiding (to choose another defuser)
				if (me->GetTask() == CCSBot::GUARD_BOMB_DEFUSER && !TheCSBots()->GetBombDefuser())
				{
					me->Idle();
					return;
				}

				// if we are guarding the loose bomb and it is picked up, stop hiding
				if (me->GetTask() == CCSBot::GUARD_LOOSE_BOMB && !TheCSBots()->GetLooseBomb())
				{
					me->GetChatter()->TheyPickedUpTheBomb();
					me->Idle();
					return;
				}

				// if we are guarding a bombsite and the bomb is dropped and we hear about it, stop guarding
				if (me->GetTask() == CCSBot::GUARD_BOMB_ZONE && me->GetGameState()->IsLooseBombLocationKnown())
				{
					me->Idle();
					return;
				}

				// if we are guarding (bombsite, initial encounter, etc) and the bomb is planted, go defuse it
				if (me->IsDoingScenario() && me->GetTask() == CCSBot::GUARD_BOMB_ZONE && TheCSBots()->IsBombPlanted())
				{
					me->Idle();
					return;
				}

			}
			// TERRORIST
			else
			{
				// if we are near the ticking bomb and someone starts defusing it, attack!
				if (TheCSBots()->GetBombDefuser())
				{
					Vector toDefuser = TheCSBots()->GetBombDefuser()->pev->origin;
					const float hearDefuseRange = 2000.0f;
					if ((toDefuser - me->pev->origin).IsLengthLessThan(hearDefuseRange))
					{
						// if we are nearby, attack, otherwise move to the bomb (which will cause us to attack when we see defuser)
						if (me->CanSeePlantedBomb())
						{
							me->Attack(TheCSBots()->GetBombDefuser());
						}
						else
						{
							me->MoveTo(&toDefuser, FASTEST_ROUTE);
							me->InhibitLookAround(10.0f);
						}

						return;
					}
				}
			}
			break;
		}
		case CCSBotManager::SCENARIO_RESCUE_HOSTAGES:
		{
			// if we're guarding the hostages and they all die or are taken, do something else
			if (me->GetTask() == CCSBot::GUARD_HOSTAGES)
			{
				if (me->GetGameState()->AreAllHostagesBeingRescued() || me->GetGameState()->AreAllHostagesGone())
				{
					me->Idle();
					return;
				}
			}
			else if (me->GetTask() == CCSBot::GUARD_HOSTAGE_RESCUE_ZONE)
			{
				// if we stumble across a hostage, guard it
				CHostage *pHostage = me->GetGameState()->GetNearestVisibleFreeHostage();
				if (pHostage)
				{
					// we see a free hostage, guard it
					CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&pHostage->pev->origin);
					if (area)
					{
						me->SetTask(CCSBot::GUARD_HOSTAGES);
						me->Hide(area);
						me->PrintIfWatched("I'm guarding hostages I found\n");
						// don't chatter here - he'll tell us when he's in his hiding spot
						return;
					}
				}
			}
		}
		}

		bool isSettledInSniper = (me->IsSniper() && m_isAtSpot) ? true : false;

		// only investigate noises if we are initiating attacks, and we aren't a "settled in" sniper
		// dont investigate noises if we are reloading
		if (!me->IsActiveWeaponReloading() &&
			!isSettledInSniper &&
			me->GetDisposition() == CCSBot::ENGAGE_AND_INVESTIGATE)
		{
			// if we are holding position, and have heard the enemy nearby, investigate after our hold time is up
			if (m_isHoldingPosition && m_heardEnemy && (gpGlobals->time - m_firstHeardEnemyTime > m_holdPositionTime))
			{
				// TODO: We might need to remember specific location of last enemy noise here
				me->InvestigateNoise();
				return;
			}

			// investigate nearby enemy noises
			if (me->ShouldInvestigateNoise())
			{
				// if we are holding position, check if enough time has elapsed since we first heard the enemy
				if (m_isAtSpot && m_isHoldingPosition)
				{
					if (!m_heardEnemy)
					{
						// first time we heard the enemy
						m_heardEnemy = true;
						m_firstHeardEnemyTime = gpGlobals->time;
						me->PrintIfWatched("Heard enemy, holding position for %f2.1 seconds...\n", m_holdPositionTime);
					}
				}
				else
				{
					// not holding position - investigate enemy noise
					me->InvestigateNoise();
					return;
				}
			}
		}
	}

	// look around
	me->UpdateLookAround();

	// if we are at our hiding spot, crouch and wait
	if (m_isAtSpot)
	{
		me->Crouch();

		// if we have a shield, hide behind it
		if (me->HasShield() && !me->IsProtectedByShield())
			me->SecondaryAttack();

		// while sitting at our hiding spot, if we are being attacked but can't see our attacker, move somewhere else
		const float hurtRecentlyTime = 1.0f;
		if (!me->IsEnemyVisible() && me->GetTimeSinceAttacked() < hurtRecentlyTime)
		{
			me->Idle();
			return;
		}

		// encourage the human player
		if (!me->IsDoingScenario())
		{
			if (me->m_iTeam == CT)
			{
				if (me->GetTask() == CCSBot::GUARD_BOMB_ZONE && me->IsAtHidingSpot() && TheCSBots()->IsBombPlanted())
				{
					if (me->GetNearbyEnemyCount() == 0)
					{
						const float someTime = 30.0f;
						const float littleTime = 11.0;

						if (TheCSBots()->GetBombTimeLeft() > someTime)
							me->GetChatter()->Encourage("BombsiteSecure", RANDOM_FLOAT(10.0f, 15.0f));
						else if (TheCSBots()->GetBombTimeLeft() > littleTime)
							me->GetChatter()->Encourage("WaitingForHumanToDefuseBomb", RANDOM_FLOAT(5.0f, 8.0f));
						else
							me->GetChatter()->Encourage("WaitingForHumanToDefuseBombPanic", RANDOM_FLOAT(3.0f, 4.0f));
					}
				}

				if (me->GetTask() == CCSBot::GUARD_HOSTAGES && me->IsAtHidingSpot())
				{
					if (me->GetNearbyEnemyCount() == 0)
					{
						CHostage *pHostage = me->GetGameState()->GetNearestVisibleFreeHostage();
						if (pHostage)
						{
							me->GetChatter()->Encourage("WaitingForHumanToRescueHostages", RANDOM_FLOAT(10.0f, 15.0f));
						}
					}
				}
			}
		}
	}
	else
	{
		// if a Player is using this hiding spot, give up
		float range;
		CBasePlayer *pCamper = UTIL_GetClosestPlayer(&m_hidingSpot, &range);

		const float closeRange = 75.0f;
		if (pCamper && pCamper != me && range < closeRange && me->IsVisible(pCamper, CHECK_FOV))
		{
			// player is in our hiding spot
			me->PrintIfWatched("Someone's in my hiding spot - picking another...\n");
			BotCampMemory::RecordPathFail(me, m_hidingSpot);

			const int maxRetries = 3;
			if (m_retry++ >= maxRetries)
			{
				me->PrintIfWatched("Can't find a free hiding spot, giving up.\n");
				me->Idle();
				return;
			}

			// pick another hiding spot near where we were planning on hiding
			me->Hide(TheNavAreaGrid.GetNavArea(&m_hidingSpot));
			return;
		}

		Vector toSpot;
		toSpot.x = m_hidingSpot.x - me->pev->origin.x;
		toSpot.y = m_hidingSpot.y - me->pev->origin.y;
		toSpot.z = m_hidingSpot.z - me->GetFeetZ(); // use feet location
		float dist = toSpot.Length();

		const float crouchDist = 200.0f;
		if (dist < crouchDist)
			me->Crouch();

		const float atDist = 20.0f;
		if (dist < atDist)
		{
			m_isAtSpot = true;
			BotCampMemory::RecordCampStart(me, m_hidingSpot);

			// make sure our approach points are valid, since we'll be watching them
			me->ComputeApproachPoints();

			// ready our weapon and prepare to attack
			me->EquipBestWeapon(MUST_EQUIP);
			me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);

			// if we are a sniper, update our task
			if (me->GetTask() == CCSBot::MOVE_TO_SNIPER_SPOT)
			{
				me->SetTask(CCSBot::SNIPING);
			}

			// determine which way to look
			TraceResult result;
			float outAngle = 0.0f;
			float outAngleRange = 0.0f;
			for (float angle = 0.0f; angle < 360.0f; angle += 45.0f)
			{
				UTIL_TraceLine(me->GetEyePosition(), me->GetEyePosition() + 1000.0f * Vector(BotCOS(angle), BotSIN(angle), 0.0f), ignore_monsters, ignore_glass, ENT(me->pev), &result);

				if (result.flFraction > outAngleRange)
				{
					outAngle = angle;
					outAngleRange = result.flFraction;
				}
			}

			Vector manualLookTarget;
			const char *manualLookName = nullptr;
			const unsigned arrivalAreaId = me->GetLastKnownArea() ? static_cast<unsigned>(me->GetLastKnownArea()->GetID()) : 0u;

			if (BotCampLookSpots::FindLookTargetRotating(me, &manualLookTarget, &manualLookName))
			{
				const float campDur = RANDOM_FLOAT(1.8f, 3.6f);
				const float yRec = me->YawDegreesToWorldPos(manualLookTarget);
				me->BeginIdleLookCommit(yRec, false);
				me->SetLookAheadAngle(yRec);
				me->SetLookAt("Recorded camp look", &manualLookTarget, PRIORITY_MEDIUM, campDur, false, 8.0f);
				EmitCampArrivalLookDebug(me, "recordedCampLook", "none", yRec, &manualLookTarget, -1.0f, campDur, 0, arrivalAreaId);
				(void)manualLookName;
			}
			else if (me->m_approachPointCount > 0)
			{
				Vector contextSpot;
				bool hasContextSpot = false;
				const char *ctxLabel = "none";

				if (me->GetTimeSinceLastSawEnemy() < 12.0f)
				{
					contextSpot = me->GetLastKnownEnemyPosition();
					hasContextSpot = true;
					ctxLabel = "lastEnemy";
				}
				else if (me->GetNoisePosition())
				{
					contextSpot = *me->GetNoisePosition();
					hasContextSpot = true;
					ctxLabel = "noise";
				}
				else if ((me->GetTask() == CCSBot::GUARD_TICKING_BOMB || me->GetTask() == CCSBot::GUARD_BOMB_DEFUSER) && me->GetGameState()->GetBombPosition())
				{
					contextSpot = *me->GetGameState()->GetBombPosition();
					hasContextSpot = true;
					ctxLabel = "bomb";
				}

				int which = 0;
				float bestScore = -999999.0f;
				for (int i = 0; i < me->m_approachPointCount; ++i)
				{
					Vector to = me->m_approachPoint[i] - me->pev->origin;
					float score = to.Length2D();

					TraceResult approachResult;
					Vector spot = me->m_approachPoint[i] + Vector(0, 0, HalfHumanHeight);
					UTIL_TraceLine(me->GetEyePosition(), spot, ignore_monsters, ignore_glass, ENT(me->pev), &approachResult);
					if (approachResult.flFraction >= 1.0f)
						score += 650.0f;
					else
						score += 250.0f * approachResult.flFraction;
					if (approachResult.flFraction < 0.45f)
						score -= 700.0f;

					if (hasContextSpot)
					{
						const float contextRange = (me->m_approachPoint[i] - contextSpot).Length2D();
						score += Q_max(0.0f, 420.0f - contextRange * 0.35f);
					}

					if (me->IsSniper() && to.IsLengthLessThan(450.0f))
						score -= 260.0f;

					if (score > bestScore)
					{
						bestScore = score;
						which = i;
					}
				}

				Vector checkSpot = me->m_approachPoint[which] + Vector(0, 0, HalfHumanHeight);
				TraceResult checkResult;
				UTIL_TraceLine(me->GetEyePosition(), checkSpot, ignore_monsters, ignore_glass, ENT(me->pev), &checkResult);

				if (checkResult.flFraction >= 0.45f)
				{
					Vector spotForLook = me->m_approachPoint[which];
					spotForLook.z += HalfHumanHeight;
					CCSBot::CampEdgeBiasTelemetry edgeTel{};
					me->AdjustStationaryCampLookForEdgeBias(&spotForLook, &edgeTel);
					const Vector pAp = me->PerturbIdleLookWorldPosition(spotForLook);
					const float yAp = me->YawDegreesToWorldPos(pAp);
					const float arrivalDur = RANDOM_FLOAT(2.0f, 4.0f);
					me->BeginIdleLookCommit(yAp, false);
					me->SetLookAt("Approach Point (Hide Arrival)", &pAp, PRIORITY_MEDIUM, arrivalDur, false, 12.0f);
					me->SetLookAheadAngle(yAp);
					const char *arrivalSrc = edgeTel.applied ? "approachPointSetLookAtEdgeAdjusted" : "approachPointSetLookAt";
					EmitCampArrivalLookDebug(me, arrivalSrc, ctxLabel, yAp, &pAp, bestScore, arrivalDur, 1, arrivalAreaId, &edgeTel);
				}
				else
				{
					me->SetLookAheadAngle(outAngle);
					EmitCampArrivalLookDebug(me, "outAngleFallback", ctxLabel, outAngle, nullptr, bestScore, 0.0f, 0, arrivalAreaId);
				}
			}
			else
			{
				me->SetLookAheadAngle(outAngle);
				EmitCampArrivalLookDebug(me, "outAngleFallback", "none", outAngle, nullptr, -1.0f, 0.0f, 0, arrivalAreaId);
			}
		}

		// move to hiding spot
		if (me->UpdatePathMovement() != CCSBot::PROGRESSING && !m_isAtSpot)
		{
			// we couldn't get to our hiding spot - pick another
			me->PrintIfWatched("Can't get to my hiding spot - finding another...\n");
			BotCampMemory::RecordPathFail(me, m_hidingSpot);

			// search from hiding spot, since we know it was valid
			const Vector *pos = FindNearbyHidingSpot(me, &m_hidingSpot, m_searchFromArea, m_range, me->IsSniper());
			if (!pos)
			{
				// no available hiding spots
				me->PrintIfWatched("No available hiding spots - hiding where I'm at.\n");

				// hide where we are
#ifndef REGAMEDLL_FIXES
				m_hidingSpot.x = me->pev->origin.x;
				m_hidingSpot.x = me->pev->origin.y;
				m_hidingSpot.z = me->GetFeetZ();
#else
				m_hidingSpot.x = me->pev->origin.x;
				m_hidingSpot.y = me->pev->origin.y;
				m_hidingSpot.z = me->GetFeetZ();
#endif
			}
			else
			{
				m_hidingSpot = *pos;
			}

			// build a path to our new hiding spot
			CNavArea *hideArea = TheNavAreaGrid.GetNavArea(&m_hidingSpot);
			if (!hideArea)
			{
				me->Idle();
				return;
			}

			if (me->ComputePath(hideArea, &m_hidingSpot, FASTEST_ROUTE) == false)
			{
				me->PrintIfWatched("Can't pathfind to hiding spot\n");
				BotCampMemory::RecordPathFail(me, m_hidingSpot);
				me->Idle();
				return;
			}
		}
	}
}

void HideState::OnExit(CCSBot *me)
{
	m_isHoldingPosition = false;

	me->StandUp();
	me->ResetStuckMonitor();
	me->ClearLookAt();
	me->ClearApproachPoints();

	// if we have a shield, put it away
	if (me->HasShield() && me->IsProtectedByShield())
		me->SecondaryAttack();
}
