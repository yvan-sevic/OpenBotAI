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
#include "bot/cs_bot_danger_memory.h"
#include "bot/cs_bot_movement_debug.h"

namespace
{
constexpr float huntAreaMinDwellTime = 1.5f;
constexpr float huntNoisePreemptMinAge = 1.25f;
constexpr float huntRepeatFailWindowSeconds = 5.0f;
} // namespace

static void EmitHuntNoiseDecisionMarker(
	CCSBot *me,
	HuntState *hunt,
	const char *tag,
	float dwellAge,
	unsigned commitAreaId,
	unsigned candidateAreaId,
	const Vector &candidateGoal,
	bool hasCandidateGoal,
	int pathResultInt,
	float noiseAge,
	float noiseMinAge,
	const char *reason,
	float baseReactionTime = -1.0f,
	float noiseHumanDelay = 0.0f,
	float effectiveNoiseReactionTime = -1.0f,
	int noisePriorityForLog = -1,
	int humanDelayApplied = -1,
	const char *humanDelayWhy = nullptr)
{
	const CNavArea *lk = me->GetLastKnownArea();
	const unsigned cA = lk ? static_cast<unsigned>(lk->GetID()) : 0u;
	const unsigned destA = me->GetPathDestAreaIDForDebug();
	CNavArea *hA = hunt->GetHuntArea();
	const unsigned huntA = hA ? static_cast<unsigned>(hA->GetID()) : 0u;
	const Vector &g = me->GetMovementGoalPosition();

	float enMem = -1.0f;
	if (me->GetLastSawEnemyTimestamp() > 0.0f)
		enMem = gpGlobals->time - me->GetLastSawEnemyTimestamp();

	const float cgX = hasCandidateGoal ? candidateGoal.x : 0.0f;
	const float cgY = hasCandidateGoal ? candidateGoal.y : 0.0f;
	const float cgZ = hasCandidateGoal ? candidateGoal.z : 0.0f;

	const char *hWhy = (humanDelayWhy && humanDelayWhy[0]) ? humanDelayWhy : "-";

	char buf[1024];
	Q_snprintf(buf, sizeof(buf),
		"[movedbg-hunt] t=%.3f tag=%s st=Hunt task=%i cA=%u destA=%u huntA=%u commitA=%u candA=%u "
		"goal=(%.0f,%.0f,%.0f) candGoal=(%.0f,%.0f,%.0f) dwellAge=%.3f visibleEnemy=%i enMem=%.2f "
		"noiseAge=%.3f noiseMin=%.3f baseRt=%.3f effRt=%.3f noiseHumanDelay=%.3f humanDelayAp=%i nPri=%i hWhy=%s pr=%i reason=%s",
		gpGlobals->time,
		tag,
		static_cast<int>(me->GetTask()),
		cA,
		destA,
		huntA,
		commitAreaId,
		candidateAreaId,
		g.x, g.y, g.z,
		cgX, cgY, cgZ,
		dwellAge,
		me->IsEnemyVisible() ? 1 : 0,
		enMem,
		noiseAge,
		noiseMinAge,
		baseReactionTime,
		effectiveNoiseReactionTime,
		noiseHumanDelay,
		humanDelayApplied,
		noisePriorityForLog,
		hWhy,
		pathResultInt,
		reason ? reason : "");

	EmitMovementWatchedDebugLine(me, buf);
}

void HuntState::EmitHuntTargetDebugMarker(
	CCSBot *me,
	const char *tag,
	const char *reason,
	int pathResultInt,
	unsigned selectedAreaId,
	unsigned committedAreaId,
	const Vector &goalPos,
	bool hasGoal,
	int rejectedCandidates,
	unsigned oldAreaId,
	unsigned newAreaId)
{
	const CNavArea *lk = me->GetLastKnownArea();
	const unsigned cA = lk ? static_cast<unsigned>(lk->GetID()) : 0u;
	const unsigned destA = me->GetPathDestAreaIDForDebug();
	const Vector &botPos = me->pev->origin;
	const float gX = hasGoal ? goalPos.x : 0.0f;
	const float gY = hasGoal ? goalPos.y : 0.0f;
	const float gZ = hasGoal ? goalPos.z : 0.0f;

	char buf[880];
	Q_snprintf(buf, sizeof(buf),
		"[movedbg-hunt] t=%.3f tag=%s st=Hunt task=%i cA=%u destA=%u selA=%u commitA=%u "
		"botPos=(%.0f,%.0f,%.0f) goal=(%.0f,%.0f,%.0f) pathResult=%i hasPath=%i stuck=%i "
		"rej=%i oldA=%u newA=%u reason=%s",
		gpGlobals->time,
		tag,
		static_cast<int>(me->GetTask()),
		cA,
		destA,
		selectedAreaId,
		committedAreaId,
		botPos.x, botPos.y, botPos.z,
		gX, gY, gZ,
		pathResultInt,
		me->HasPath() ? 1 : 0,
		me->IsNavStuck() ? 1 : 0,
		rejectedCandidates,
		oldAreaId,
		newAreaId,
		reason ? reason : "");

	EmitMovementWatchedDebugLine(me, buf);
}

void HuntState::UpdateHuntRepeatFailLog(
	CCSBot *me,
	unsigned areaId,
	const Vector &goalPos,
	int pathResultInt,
	unsigned destA,
	bool hasPath,
	bool stuck)
{
	const float now = gpGlobals->time;

	if (m_huntRepeatFailArea == areaId && m_huntRepeatFailCount > 0 && (now - m_huntRepeatFailWindowStart) <= huntRepeatFailWindowSeconds)
	{
		++m_huntRepeatFailCount;
	}
	else
	{
		m_huntRepeatFailArea = areaId;
		m_huntRepeatFailWindowStart = now;
		m_huntRepeatFailCount = 1;
		return;
	}

	if (m_huntRepeatFailCount < 2)
		return;

	const float windowAge = now - m_huntRepeatFailWindowStart;
	char rbuf[160];
	Q_snprintf(rbuf, sizeof(rbuf), "repeat_fail failCnt=%i window_s=%.2f", m_huntRepeatFailCount, windowAge);

	EmitHuntTargetDebugMarker(
		me,
		"[HUNT TARGET REPEAT FAIL]",
		rbuf,
		pathResultInt,
		areaId,
		areaId,
		goalPos,
		true,
		0,
		0,
		0);
}

bool HuntState::ExecuteHuntPickAndCommit(CCSBot *me, const char * /*debugReason*/)
{
	m_huntArea = nullptr;
	Vector huntPos;
	bool hasSpecificHuntPos = false;

	const float recentEnemyMemoryTime = 12.0f;
	if (gpGlobals->time - me->GetLastSawEnemyTimestamp() < recentEnemyMemoryTime)
	{
		if (BotDangerMemory::FindBestLookSpotNear(me, me->GetLastKnownEnemyPosition(), 900.0f, &huntPos))
		{
			m_huntArea = TheNavAreaGrid.GetNearestNavArea(&huntPos);
			hasSpecificHuntPos = (m_huntArea != nullptr);
		}

		if (!m_huntArea)
		{
			m_huntArea = TheNavAreaGrid.GetNearestNavArea(&me->GetLastKnownEnemyPosition());
			if (m_huntArea)
			{
				huntPos = me->GetLastKnownEnemyPosition();
				hasSpecificHuntPos = true;
			}
		}
	}

	float oldest = 0.0f;

	if (!m_huntArea)
	{
		int areaCount = 0;
		const float minSize = 150.0f;
		for (auto area : TheNavAreaList)
		{
			areaCount++;

			const Extent *extent = area->GetExtent();
			if (extent->hi.x - extent->lo.x < minSize || extent->hi.y - extent->lo.y < minSize)
				continue;

			real_t age = gpGlobals->time - area->GetClearedTimestamp(me->m_iTeam - 1);
			if (age > oldest)
			{
				oldest = age;
				m_huntArea = area;
			}
		}

		if (!m_huntArea && areaCount > 0)
		{
			int which = RANDOM_LONG(0, areaCount - 1);

			areaCount = 0;
			for (auto area : TheNavAreaList)
			{
				m_huntArea = area;

				if (which == areaCount)
					break;

				which--;
			}
		}
	}

	if (!m_huntArea || m_huntArea->GetID() == 0)
	{
		m_huntArea = nullptr;
		return false;
	}

	if (hasSpecificHuntPos)
		GetSimpleGroundHeight(&huntPos, &huntPos.z);

	const unsigned commitArea = static_cast<unsigned>(m_huntArea->GetID());
	if (!me->ComputePath(m_huntArea, hasSpecificHuntPos ? &huntPos : nullptr, SAFEST_ROUTE))
	{
		m_huntArea = nullptr;
		return false;
	}

	const float nowCommit = gpGlobals->time;
	const float prevCommitTime = m_huntAreaCommitTime;
	const float dwellBeforeCommit = (prevCommitTime > 0.0f) ? (nowCommit - prevCommitTime) : 0.0f;
	m_huntAreaCommitTime = nowCommit;

	EmitHuntNoiseDecisionMarker(me, this, "[HUNT COMMIT]", dwellBeforeCommit, commitArea, commitArea, huntPos,
		hasSpecificHuntPos, static_cast<int>(CCSBot::PROGRESSING), -1.0f, huntNoisePreemptMinAge, "commit_compute_path_ok area_checked");

	return true;
}

// Begin the hunt
void HuntState::OnEnter(CCSBot *me)
{
	m_huntAreaCommitTime = 0.0f;
	m_huntConsumedGuardThisUpdate = false;
	m_huntRepeatFailArea = 0;
	m_huntRepeatFailWindowStart = 0.0f;
	m_huntRepeatFailCount = 0;

	// lurking death
	if (me->IsUsingKnife() && me->IsWellPastSafe() && !me->IsHurrying())
		me->Walk();
	else
		me->Run();

	me->StandUp();
	me->SetDisposition(CCSBot::ENGAGE_AND_INVESTIGATE);
	me->SetTask(CCSBot::SEEK_AND_DESTROY);
	me->DestroyPath(MoveDbgPathClearReason::TaskOrStateChange, __FUNCTION__);
}

// Hunt down our enemies
void HuntState::OnUpdate(CCSBot *me)
{
	m_huntConsumedGuardThisUpdate = false;

	// if we've been hunting for a long time, drop into Idle for a moment to
	// select something else to do
	const float huntingTooLongTime = 30.0f;
	if (gpGlobals->time - me->GetStateTimestamp() > huntingTooLongTime)
	{
		// stop being a rogue and do the scenario, since there must not be many enemies left to hunt
		me->PrintIfWatched("Giving up hunting, and being a rogue\n");
		me->SetRogue(false);
		me->Idle();
		return;
	}

	// scenario logic
	if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB)
	{
		if (me->m_iTeam == TERRORIST)
		{
			// if we have the bomb and it's time to plant, or we happen to be in a bombsite and it seems safe, do it
			if (me->IsCarryingBomb())
			{
				const float safeTime = 3.0f;
				if (TheCSBots()->IsTimeToPlantBomb() || (me->IsAtBombsite() && gpGlobals->time - me->GetLastSawEnemyTimestamp() > safeTime))
				{
					me->Idle();
					return;
				}
			}

			// if we notice the bomb lying on the ground, go get it
			if (me->NoticeLooseBomb())
			{
				me->FetchBomb();
				return;
			}

			// if bomb has been planted, and we hear it, move to a hiding spot near the bomb and watch it
			const Vector *bombPos = me->GetGameState()->GetBombPosition();
			if (!me->IsRogue() && me->GetGameState()->IsBombPlanted() && bombPos)
			{
				me->SetTask(CCSBot::GUARD_TICKING_BOMB);
				me->Hide(TheNavAreaGrid.GetNavArea(bombPos));
				return;
			}
		}
		// CT
		else
		{
			if (!me->IsRogue() && me->CanSeeLooseBomb())
			{
				CNavArea *looseBombArea = TheCSBots()->GetLooseBombArea();

				// if we are near the loose bomb and can see it, hide nearby and guard it
				me->SetTask(CCSBot::GUARD_LOOSE_BOMB);
				me->Hide(looseBombArea);

				if (looseBombArea)
					me->GetChatter()->AnnouncePlan("GoingToGuardLooseBomb", looseBombArea->GetPlace());

				return;
			}
			else if (TheCSBots()->IsBombPlanted())
			{
				// rogues will defuse a bomb, but not guard the defuser
				if (!me->IsRogue() || !TheCSBots()->GetBombDefuser())
				{
					// search for the planted bomb to defuse
					me->Idle();
					return;
				}
			}
		}
	}
	else if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES)
	{
		if (me->m_iTeam == TERRORIST)
		{
			if (me->GetGameState()->AreAllHostagesBeingRescued())
			{
				// all hostages are being rescued, head them off at the escape zones
				if (me->GuardRandomZone())
				{
					me->SetTask(CCSBot::GUARD_HOSTAGE_RESCUE_ZONE);
					me->PrintIfWatched("Trying to beat them to an escape zone!\n");
					me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
					me->GetChatter()->GuardingHostageEscapeZone(IS_PLAN);
					return;
				}
			}

			// if safe time is up, and we stumble across a hostage, guard it
			if (!me->IsRogue() && !me->IsSafe())
			{
				CHostage *pHostage = me->GetGameState()->GetNearestVisibleFreeHostage();
				if (pHostage)
				{
					CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&pHostage->pev->origin);
					if (area)
					{
						// we see a free hostage, guard it
						me->SetTask(CCSBot::GUARD_HOSTAGES);
						me->Hide(area);
						me->PrintIfWatched("I'm guarding hostages\n");
						me->GetChatter()->GuardingHostages(area->GetPlace(), IS_PLAN);
						return;
					}
				}
			}
		}
	}

	// listen for enemy noises
	if (me->ShouldInvestigateNoise())
	{
		const float noiseTs = me->GetNoiseHeardTimestamp();
		const float noiseAge = (noiseTs > 0.0f) ? (gpGlobals->time - noiseTs) : huntNoisePreemptMinAge;
		const bool allowPreempt = me->IsEnemyVisible() || noiseAge >= huntNoisePreemptMinAge;
		const float nowNoise = gpGlobals->time;
		const float dwellForNoise = (m_huntAreaCommitTime > 0.0f) ? (nowNoise - m_huntAreaCommitTime) : -1.0f;
		const unsigned commitForNoise = (m_huntAreaCommitTime > 0.0f && m_huntArea)
			? static_cast<unsigned>(m_huntArea->GetID())
			: 0u;

		if (!allowPreempt)
		{
			EmitHuntNoiseDecisionMarker(me, this, "[NOISE PREEMPT DEFER]", dwellForNoise, commitForNoise, 0u,
				Vector(0, 0, 0), false, -1, noiseAge, huntNoisePreemptMinAge, "defer_noise_age_below_min",
				me->GetProfile()->GetReactionTime(),
				me->GetNoiseHumanDelay(),
				me->GetEffectiveNoiseReactionTime(),
				static_cast<int>(me->GetNoisePriority()),
				me->GetNoiseHumanDelayApplied() ? 1 : 0,
				me->GetNoiseHumanDelayDebugWhy());
		}
		else
		{
			const char *allowReason = me->IsEnemyVisible() ? "allow_enemy_visible" : "allow_noise_age_ok";
			EmitHuntNoiseDecisionMarker(me, this, "[NOISE PREEMPT ALLOW]", dwellForNoise, commitForNoise, 0u,
				Vector(0, 0, 0), false, -1, noiseAge, huntNoisePreemptMinAge, allowReason,
				me->GetProfile()->GetReactionTime(),
				me->GetNoiseHumanDelay(),
				me->GetEffectiveNoiseReactionTime(),
				static_cast<int>(me->GetNoisePriority()),
				me->GetNoiseHumanDelayApplied() ? 1 : 0,
				me->GetNoiseHumanDelayDebugWhy());

			me->InvestigateNoise();
			return;
		}
	}

	// look around
	me->UpdateLookAround();

	const bool atHuntArea = (m_huntArea != nullptr && me->GetLastKnownArea() == m_huntArea);
	CCSBot::PathResult pathResult = CCSBot::PROGRESSING;

	if (!atHuntArea)
		pathResult = me->UpdatePathMovement();

	const unsigned destA = me->GetPathDestAreaIDForDebug();
	const bool pathDead = !me->HasPath() && destA == 0u;
	bool skipHeavyRepath = false;

	// Invalidate spurious committed hunt when movement reports PATH_FAILURE with no path/destination.
	if (!atHuntArea
		&& pathResult == CCSBot::PATH_FAILURE
		&& pathDead
		&& (m_huntArea != nullptr || m_huntAreaCommitTime > 0.0f))
	{
		const unsigned oldSelA = m_huntArea ? static_cast<unsigned>(m_huntArea->GetID()) : 0u;
		const unsigned oldCommitA = (m_huntAreaCommitTime > 0.0f && oldSelA != 0u) ? oldSelA : 0u;
		const Vector goalSnap = me->GetMovementGoalPosition();

		EmitHuntTargetDebugMarker(
			me,
			"[HUNT TARGET INVALID]",
			"path_failure_no_path",
			static_cast<int>(pathResult),
			oldSelA,
			oldCommitA,
			goalSnap,
			true,
			0,
			0,
			0);

		UpdateHuntRepeatFailLog(me, oldSelA != 0u ? oldSelA : oldCommitA, goalSnap,
			static_cast<int>(pathResult), destA, me->HasPath(), me->IsNavStuck());

		ClearHuntArea();
		skipHeavyRepath = true;

		if (!m_huntConsumedGuardThisUpdate)
		{
			m_huntConsumedGuardThisUpdate = true;
			const bool ok = ExecuteHuntPickAndCommit(me, "reselect_after_path_failure_no_path");
			const unsigned postNew = m_huntArea ? static_cast<unsigned>(m_huntArea->GetID()) : 0u;
			EmitHuntTargetDebugMarker(
				me,
				"[HUNT TARGET RESELECT]",
				ok ? "reselect_ok" : "reselect_no_commit",
				static_cast<int>(pathResult),
				postNew,
				postNew,
				me->GetMovementGoalPosition(),
				ok,
				0,
				oldSelA,
				postNew);
		}
	}

	const bool needRepath = atHuntArea || pathResult != CCSBot::PROGRESSING;

	if (needRepath)
	{
		const float now = gpGlobals->time;
		const bool inDwellWindow = m_huntAreaCommitTime > 0.0f && (now - m_huntAreaCommitTime < huntAreaMinDwellTime);

		if (atHuntArea && inDwellWindow && !me->IsEnemyVisible())
		{
			const float dwellAge = now - m_huntAreaCommitTime;
			const unsigned commitA = (m_huntArea) ? static_cast<unsigned>(m_huntArea->GetID()) : 0u;
			EmitHuntNoiseDecisionMarker(me, this, "[HUNT DWELL KEEP]", dwellAge, commitA, 0u, Vector(0, 0, 0), false,
				static_cast<int>(pathResult), -1.0f, -1.0f, "dwell_hold_at_hunt_area");
		}
		else if (!skipHeavyRepath)
		{
			if (inDwellWindow)
			{
				const unsigned commitRel = (m_huntArea) ? static_cast<unsigned>(m_huntArea->GetID()) : 0u;
				const float dwellRel = now - m_huntAreaCommitTime;
				const char *relReason = "release_dwell_bypass";
				if (me->IsEnemyVisible())
					relReason = "release_enemy_visible";
				else if (!atHuntArea)
					relReason = "release_path_result";
				EmitHuntNoiseDecisionMarker(me, this, "[HUNT RELEASE]", dwellRel, commitRel, 0u, Vector(0, 0, 0), false,
					static_cast<int>(pathResult), -1.0f, -1.0f, relReason);
			}

			Vector huntPos;
			bool hasSpecificHuntPos = false;
			int huntZeroRejections = 0;

			for (int huntPickRound = 0; huntPickRound < 2; ++huntPickRound)
			{
				m_huntArea = nullptr;
				hasSpecificHuntPos = false;

				const float recentEnemyMemoryTime = 12.0f;
				if (gpGlobals->time - me->GetLastSawEnemyTimestamp() < recentEnemyMemoryTime)
				{
					if (BotDangerMemory::FindBestLookSpotNear(me, me->GetLastKnownEnemyPosition(), 900.0f, &huntPos))
					{
						m_huntArea = TheNavAreaGrid.GetNearestNavArea(&huntPos);
						hasSpecificHuntPos = (m_huntArea != nullptr);
					}

					if (!m_huntArea)
					{
						m_huntArea = TheNavAreaGrid.GetNearestNavArea(&me->GetLastKnownEnemyPosition());
						if (m_huntArea)
						{
							huntPos = me->GetLastKnownEnemyPosition();
							hasSpecificHuntPos = true;
						}
					}
				}

				float oldest = 0.0f;

				if (!m_huntArea)
				{
					int areaCount = 0;
					const float minSize = 150.0f;
					for (auto area : TheNavAreaList)
					{
						areaCount++;

						const Extent *extent = area->GetExtent();
						if (extent->hi.x - extent->lo.x < minSize || extent->hi.y - extent->lo.y < minSize)
							continue;

						real_t age = gpGlobals->time - area->GetClearedTimestamp(me->m_iTeam - 1);
						if (age > oldest)
						{
							oldest = age;
							m_huntArea = area;
						}
					}

					if (!m_huntArea && areaCount > 0)
					{
						int which = RANDOM_LONG(0, areaCount - 1);

						areaCount = 0;
						for (auto area : TheNavAreaList)
						{
							m_huntArea = area;

							if (which == areaCount)
								break;

							which--;
						}
					}
				}

				if (!m_huntArea || m_huntArea->GetID() == 0)
				{
					m_huntArea = nullptr;
					const Vector goalSnap = me->GetMovementGoalPosition();
					++huntZeroRejections;
					EmitHuntTargetDebugMarker(
						me,
						"[HUNT TARGET INVALID]",
						"hunt_area_zero",
						static_cast<int>(pathResult),
						0u,
						0u,
						goalSnap,
						false,
						huntZeroRejections,
						0u,
						0u);
					UpdateHuntRepeatFailLog(me, 0u, goalSnap,
						static_cast<int>(pathResult), me->GetPathDestAreaIDForDebug(), me->HasPath(), me->IsNavStuck());

					if (m_huntConsumedGuardThisUpdate || huntPickRound == 1)
						break;

					m_huntConsumedGuardThisUpdate = true;
					EmitHuntTargetDebugMarker(
						me,
						"[HUNT TARGET RESELECT]",
						"retry_after_hunt_area_zero",
						static_cast<int>(pathResult),
						0u,
						0u,
						goalSnap,
						false,
						huntZeroRejections,
						0u,
						0u);
					continue;
				}

				if (hasSpecificHuntPos)
					GetSimpleGroundHeight(&huntPos, &huntPos.z);
				const float prevCommitTime = m_huntAreaCommitTime;
				const float nowCommit = gpGlobals->time;
				const float dwellBeforeCommit = (prevCommitTime > 0.0f) ? (nowCommit - prevCommitTime) : 0.0f;
				const unsigned commitArea = static_cast<unsigned>(m_huntArea->GetID());

				if (me->ComputePath(m_huntArea, hasSpecificHuntPos ? &huntPos : nullptr, SAFEST_ROUTE))
				{
					m_huntAreaCommitTime = nowCommit;
					EmitHuntNoiseDecisionMarker(me, this, "[HUNT COMMIT]", dwellBeforeCommit, commitArea, commitArea, huntPos,
						hasSpecificHuntPos, static_cast<int>(pathResult), -1.0f, huntNoisePreemptMinAge, "commit_compute_path_ok area_checked");
					break;
				}

				m_huntArea = nullptr;
				break;
			}
		}
	}
}

// Done hunting
void HuntState::OnExit(CCSBot *me)
{
	(void)me;
}
