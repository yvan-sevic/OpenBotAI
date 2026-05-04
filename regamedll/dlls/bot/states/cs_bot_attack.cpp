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

static float ScoreCombatStrafeSide(CCSBot *me, CBasePlayer *enemy, int side)
{
	Vector toEnemy = enemy->pev->origin - me->pev->origin;
	toEnemy.z = 0.0f;
	if (toEnemy.NormalizeInPlace() < 1.0f)
		return 0.0f;

	Vector lateral(-toEnemy.y, toEnemy.x, 0.0f);
	lateral = lateral * static_cast<float>(side);

	Vector start = me->pev->origin + Vector(0.0f, 0.0f, 18.0f);
	Vector end = start + lateral * 150.0f;

	TraceResult moveTrace;
	UTIL_TraceHull(start, end, ignore_monsters, human_hull, ENT(me->pev), &moveTrace);

	float score = moveTrace.flFraction;
	if (moveTrace.fStartSolid)
		score -= 1.0f;

	// Prefer strafes that keep a usable firing lane instead of sliding into a wall angle.
	Vector peekEye = me->GetEyePosition() + lateral * 70.0f;
	TraceResult sightTrace;
	UTIL_TraceLine(peekEye, enemy->EyePosition(), ignore_monsters, ignore_glass, ENT(me->pev), &sightTrace);
	if (sightTrace.flFraction < 0.95f)
		score -= 0.35f;

	return score;
}

static int ChooseCombatStrafeDir(CCSBot *me, CBasePlayer *enemy, int currentDir)
{
	const float leftScore = ScoreCombatStrafeSide(me, enemy, -1);
	const float rightScore = ScoreCombatStrafeSide(me, enemy, 1);
	const float currentScore = (currentDir < 0) ? leftScore : rightScore;

	if (Q_fabs(leftScore - rightScore) > 0.18f)
		return (leftScore > rightScore) ? -1 : 1;

	// Human strafes often hold a lane briefly; avoid perfect left/right metronomes.
	if (currentScore > 0.45f && RANDOM_FLOAT(0.0f, 100.0f) < 68.0f)
		return currentDir;

	return (RANDOM_LONG(0, 1) == 0) ? -1 : 1;
}

static void ApplyCombatStrafe(CCSBot *me, int dir)
{
	if (dir < 0)
		me->StrafeLeft();
	else
		me->StrafeRight();
}

static bool BuildSuspiciousEnemyLookSpot(CCSBot *me, CBasePlayer *enemy, float notSeenEnemyTime, Vector *out)
{
	const Vector &lastKnown = me->GetLastKnownEnemyPosition();

	if (BotDangerMemory::FindBestLookSpotNear(me, lastKnown, 650.0f + 250.0f * TheCSBots()->GetEffectiveSkill(me), out))
		return true;

	Vector toLast = lastKnown - me->pev->origin;
	toLast.z = 0.0f;
	if (toLast.NormalizeInPlace() < 1.0f)
		return false;

	Vector side(-toLast.y, toLast.x, 0.0f);
	if (RANDOM_LONG(0, 1) == 0)
		side = side * -1.0f;

	const float lead = RANDOM_FLOAT(120.0f, 280.0f + 180.0f * TheCSBots()->GetEffectiveSkill(me));
	const float sideLead = RANDOM_FLOAT(40.0f, 140.0f) * ((RANDOM_LONG(0, 1) == 0) ? -1.0f : 1.0f);
	Vector spot = lastKnown + toLast * lead + side * sideLead;

	// If we still know the entity velocity, use it only as a soft bias and keep it noisy.
	if (enemy && enemy->pev->velocity.Make2D().Length() > 80.0f && notSeenEnemyTime < 0.9f)
	{
		Vector vel = enemy->pev->velocity;
		vel.z = 0.0f;
		vel.NormalizeInPlace();
		spot = spot * 0.65f + (lastKnown + vel * RANDOM_FLOAT(120.0f, 260.0f)) * 0.35f;
	}

	if (GetSimpleGroundHeight(&spot, &spot.z))
	{
		spot.z += HalfHumanHeight;
		*out = spot;
		return true;
	}

	spot.z = lastKnown.z + HalfHumanHeight;
	*out = spot;
	return true;
}

// Begin attacking
void AttackState::OnEnter(CCSBot *me)
{
	CBasePlayer *pEnemy = me->GetEnemy();

	// store our posture when the attack began
	me->PushPostureContext();
	me->DestroyPath(MoveDbgPathClearReason::EnemyEngage, __FUNCTION__);

	// if we are using a knife, try to sneak up on the enemy
	if (pEnemy && me->IsUsingKnife() && !me->IsPlayerFacingMe(pEnemy))
		me->Walk();
	else
		me->Run();

	me->GetOffLadder();
	me->ResetStuckMonitor();

	m_repathTimer.Invalidate();
	m_haveSeenEnemy = me->IsEnemyVisible();
	m_nextDodgeStateTimestamp = 0.0f;
	m_firstDodge = true;
	m_isEnemyHidden = false;
	m_reacquireTimestamp = 0.0f;

	m_pinnedDownTimestamp = gpGlobals->time + RANDOM_FLOAT(7.0f, 10.0f);
	m_shieldToggleTimestamp = gpGlobals->time + RANDOM_FLOAT(2.0f, 10.0f);
	m_shieldForceOpen = false;
	m_stopAndShootTimestamp = 0.0f;
	m_nextStopAndShootTimestamp = 0.0f;
	m_nextCombatRepositionTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, 0.95f);
	m_combatRepositionEndTimestamp = 0.0f;
	m_combatRepositionDir = (RANDOM_LONG(0, 1) == 0) ? -1 : 1;
	if (pEnemy)
		m_combatRepositionDir = ChooseCombatStrafeDir(me, pEnemy, m_combatRepositionDir);
	m_nextSuspiciousLookTimestamp = 0.0f;

	// if we encountered someone while escaping, grab our weapon and fight!
	if (me->IsEscapingFromBomb())
		me->EquipBestWeapon();

	if (me->IsUsingKnife())
	{
		// can't crouch and hold with a knife
		m_crouchAndHold = false;
		me->StandUp();
	}
	else
	{
		// decide whether to crouch where we are, or run and gun (if we havent already - see CCSBot::Attack())
		if (!m_crouchAndHold)
		{
			if (pEnemy)
			{
				const float crouchFarRange = 750.0f;
				float crouchChance;

				// more likely to crouch if using sniper rifle or if enemy is far away
				if (me->IsUsingSniperRifle())
					crouchChance = 50.0f;
				else if ((me->pev->origin - pEnemy->pev->origin).IsLengthGreaterThan(crouchFarRange))
					crouchChance = 50.0f;
				else
					crouchChance = 20.0f * (1.0f - me->GetProfile()->GetAggression());

				if (RANDOM_FLOAT(0.0f, 100.0f) < crouchChance)
				{
					// make sure we can still see if we crouch
					TraceResult result;

					Vector origin = me->pev->origin;
					if (!me->IsCrouching())
					{
						// we are standing, adjust for lower crouch origin
						origin.z -= 20.0f;
					}

					UTIL_TraceLine(origin, pEnemy->EyePosition(), ignore_monsters, ignore_glass, ENT(me->pev), &result);

					if (result.flFraction == 1.0f)
					{
						m_crouchAndHold = true;
					}
				}
			}
		}

		if (m_crouchAndHold)
		{
			me->Crouch();
			me->PrintIfWatched("Crouch and hold attack!\n");
		}
	}

	m_scopeTimestamp = 0;
	m_didAmbushCheck = false;

	float skill = TheCSBots()->GetEffectiveSkill(me);

	// tendency to dodge is proportional to skill
	float dodgeChance = 80.0f * skill;

	if (me->IsUsingKnife())
	{
		dodgeChance *= 2.0f;
	}

	// high skill bots always dodge if outnumbered, or they see a sniper
	if (skill > 0.5f && me->IsOutnumbered())
	{
		dodgeChance = 100.0f;
	}

	m_dodge = (RANDOM_FLOAT(0.0f, 100.0f) < dodgeChance) != 0;

	// decide whether we might bail out of this fight
	m_isCoward = (RANDOM_FLOAT(0.0f, 100.0f) > 100.0f * me->GetProfile()->GetAggression());
}

void AttackState::StopAttacking(CCSBot *me)
{
	if (me->m_task == CCSBot::SNIPING)
	{
		// stay in our hiding spot
		me->Hide(me->GetLastKnownArea(), -1.0f, 50.0f);
	}
	else
	{
		me->StopAttacking();
	}
}

// Perform attack behavior
void AttackState::OnUpdate(CCSBot *me)
{
	// can't be stuck while attacking
	me->ResetStuckMonitor();
	me->StopRapidFire();

	CBasePlayerWeapon *pWeapon = me->GetActiveWeapon();
	if (pWeapon)
	{
		if (pWeapon->m_iId == WEAPON_C4 ||
			pWeapon->m_iId == WEAPON_HEGRENADE ||
			pWeapon->m_iId == WEAPON_FLASHBANG ||
			pWeapon->m_iId == WEAPON_SMOKEGRENADE)
		{
			me->EquipBestWeapon();
		}
	}

	CBasePlayer *pEnemy = me->GetEnemy();
	if (!pEnemy)
	{
		StopAttacking(me);
		return;
	}

	const float skill = TheCSBots()->GetEffectiveSkill(me);

	// keep track of whether we have seen our enemy at least once yet
	if (!m_haveSeenEnemy)
		m_haveSeenEnemy = me->IsEnemyVisible();

	// Retreat check
	// Do not retreat if the enemy is too close
	if (m_retreatTimer.IsElapsed())
	{
		// If we've been fighting this battle for awhile, we're "pinned down" and
		// need to do something else.
		// If we are outnumbered, retreat.
		bool isPinnedDown = (gpGlobals->time > m_pinnedDownTimestamp);

		if (me->GetNearbyEnemyCount() >= 3 && me->IsEnemyVisible() && gpGlobals->time >= m_nextCombatRepositionTimestamp)
		{
			m_retreatTimer.Start(RANDOM_FLOAT(1.4f, 3.0f));
			m_nextCombatRepositionTimestamp = gpGlobals->time + RANDOM_FLOAT(0.85f, 1.85f);
			m_combatRepositionEndTimestamp = gpGlobals->time + RANDOM_FLOAT(0.28f, 0.62f);
			m_combatRepositionDir = ChooseCombatStrafeDir(me, pEnemy, m_combatRepositionDir);
		}
		else if (isPinnedDown ||
			(me->IsOutnumbered() && m_isCoward) ||
			(me->OutnumberedCount() >= 2 && me->GetProfile()->GetAggression() < 1.0f))
		{
			// tell our teammates our plight
			if (isPinnedDown)
				me->GetChatter()->PinnedDown();
			else
				me->GetChatter()->Scared();

			m_retreatTimer.Start(RANDOM_FLOAT(3.0f, 15.0f));

			// try to retreat
			if (me->TryToRetreat())
			{
				// if we are a sniper, equip our pistol so we can fire while retreating
				if (me->IsUsingSniperRifle())
				{
					me->EquipPistol();
				}
			}
			else
			{
				me->PrintIfWatched("I want to retreat, but no safe spots nearby!\n");
			}
		}
	}

	// Knife fighting
	// We need to pathfind right to the enemy to cut him
	if (me->IsUsingKnife())
	{
		// can't crouch and hold with a knife
		m_crouchAndHold = false;
		me->StandUp();

		// if we are using a knife and our prey is looking towards us, run at him
		if (me->IsPlayerFacingMe(pEnemy))
		{
			me->ForceRun(5.0f);
			me->Hurry(10.0f);
		}
		else
		{
			me->Walk();
		}

		// slash our victim
		me->FireWeaponAtEnemy();

		// if our victim has moved, repath
		bool repath = false;
		if (me->HasPath())
		{
			const float repathRange = 100.0f;
			if ((me->GetPathEndpoint() - pEnemy->pev->origin).IsLengthGreaterThan(repathRange))
			{
				repath = true;
			}
		}
		else
		{
			repath = true;
		}

		if (repath && m_repathTimer.IsElapsed())
		{
			me->ComputePath(TheNavAreaGrid.GetNearestNavArea(&pEnemy->pev->origin), &pEnemy->pev->origin, FASTEST_ROUTE);

			const float repathInterval = 0.5f;
			m_repathTimer.Start(repathInterval);
		}

		// move towards victim
		if (me->UpdatePathMovement(NO_SPEED_CHANGE) != CCSBot::PROGRESSING)
		{
			me->DestroyPath(MoveDbgPathClearReason::CombatKnifePathEnd, __FUNCTION__);
		}

		return;
	}

	// Simple shield usage
	if (me->HasShield())
	{
		if (me->IsEnemyVisible() && !m_shieldForceOpen)
		{
			if (!me->IsRecognizedEnemyReloading() && !me->IsReloading() && me->IsPlayerLookingAtMe(pEnemy))
			{
				// close up - enemy is pointing his gun at us
				if (!me->IsProtectedByShield())
					me->SecondaryAttack();
			}
			else
			{
				// enemy looking away or reloading his weapon - open up and shoot him
				if (me->IsProtectedByShield())
					me->SecondaryAttack();
			}
		}
		else
		{
			// can't see enemy, open up
			if (me->IsProtectedByShield())
				me->SecondaryAttack();
		}

		if (gpGlobals->time > m_shieldToggleTimestamp)
		{
			m_shieldToggleTimestamp = gpGlobals->time + RANDOM_FLOAT(0.5, 2.0f);

			// toggle shield force open
			m_shieldForceOpen = !m_shieldForceOpen;
		}
	}

	// check if our weapon range is bad and we should switch to pistol
	if (me->IsUsingSniperRifle())
	{
		// if we have a sniper rifle and our enemy is too close, switch to pistol
		// NOTE: Must be larger than NO_ZOOM range in AdjustZoom()
		const float sniperMinRange = 310.0f;
		if ((pEnemy->pev->origin - me->pev->origin).IsLengthLessThan(sniperMinRange))
			me->EquipPistol();
	}
	else if (me->IsUsingShotgun())
	{
		// if we have a shotgun equipped and enemy is too far away, switch to pistol
		const float shotgunMaxRange = 1000.0f;
		if ((pEnemy->pev->origin - me->pev->origin).IsLengthGreaterThan(shotgunMaxRange))
			me->EquipPistol();
	}

	// if we're sniping, look through the scope - need to do this here in case a reload resets our scope
	if (me->IsUsingSniperRifle())
	{
		// for Scouts and AWPs, we need to wait for zoom to resume
		if (me->m_bResumeZoom)
		{
			m_scopeTimestamp = gpGlobals->time;
			return;
		}

		Vector toAimSpot3D = me->m_aimSpot - me->pev->origin;
		float targetRange = toAimSpot3D.Length();

		// dont adjust zoom level if we're already zoomed in - just fire
		if (me->GetZoomLevel() == CCSBot::NO_ZOOM && me->AdjustZoom(targetRange))
			m_scopeTimestamp = gpGlobals->time;

		const float waitScopeTime = 0.2f + me->GetProfile()->GetReactionTime();
		if (gpGlobals->time - m_scopeTimestamp < waitScopeTime)
		{
			// force us to wait until zoomed in before firing
			return;
		}
	}

	// see if we "notice" that our prey is dead
	if (me->IsAwareOfEnemyDeath())
	{
		// let team know if we killed the last enemy
		if (me->GetLastVictimID() == pEnemy->entindex() && me->GetNearbyEnemyCount() <= 1)
		{
			me->GetChatter()->KilledMyEnemy(pEnemy->entindex());
		}

		StopAttacking(me);
		return;
	}

	float notSeenEnemyTime = gpGlobals->time - me->GetLastSawEnemyTimestamp();

	// if we haven't seen our enemy for a moment, continue on if we dont want to fight, or decide to ambush if we do
	if (!me->IsEnemyVisible())
	{
		// attend to nearby enemy gunfire
		if (notSeenEnemyTime > 1.25f && me->CanHearNearbyEnemyGunfire())
		{
			// give up the attack, since we didn't want it in the first place
			StopAttacking(me);

			me->SetLookAt("Nearby enemy gunfire", me->GetNoisePosition(), PRIORITY_HIGH, 0.0f);
			me->PrintIfWatched("Checking nearby threatening enemy gunfire!\n");
			return;
		}

		// check if we have lost track of our enemy during combat
		if (notSeenEnemyTime > 0.25f)
		{
			m_isEnemyHidden = true;
		}

		if (m_haveSeenEnemy &&
			notSeenEnemyTime > 0.16f &&
			notSeenEnemyTime < 1.4f &&
			gpGlobals->time >= m_nextSuspiciousLookTimestamp)
		{
			Vector suspiciousSpot;
			if (BuildSuspiciousEnemyLookSpot(me, pEnemy, notSeenEnemyTime, &suspiciousSpot))
			{
				const float skill = TheCSBots()->GetEffectiveSkill(me);
				me->SetLookAt("Suspicious enemy angle", &suspiciousSpot, PRIORITY_HIGH, RANDOM_FLOAT(0.20f, 0.48f), true, 9.0f - skill * 3.0f);
				m_nextSuspiciousLookTimestamp = gpGlobals->time + RANDOM_FLOAT(0.30f + (1.0f - skill) * 0.28f, 0.72f + (1.0f - skill) * 0.55f);
			}
		}

		if (notSeenEnemyTime > 0.1f)
		{
			if (me->GetDisposition() == CCSBot::ENGAGE_AND_INVESTIGATE)
			{
				// decide whether we should hide and "ambush" our enemy
				if (m_haveSeenEnemy && !m_didAmbushCheck)
				{
					const float hideChance = 33.3f;

					if (RANDOM_FLOAT(0.0, 100.0f) < hideChance)
					{
						float ambushTime = RANDOM_FLOAT(3.0f, 15.0f);

						// hide in ambush nearby
						// TODO: look towards where we know enemy is
						const Vector *spot = FindNearbyRetreatSpot(me, 200.0f);
						if (spot)
						{
							me->IgnoreEnemies(1.0f);
							me->Run();
							me->StandUp();
							me->Hide(spot, ambushTime, true);
							return;
						}
					}

					// don't check again
					m_didAmbushCheck = true;
				}
			}
			else
			{
				// give up the attack, since we didn't want it in the first place
				StopAttacking(me);
				return;
			}
		}
	}
	else
	{
		// we can see the enemy again - reset our ambush check
		m_didAmbushCheck = false;

		// if the enemy is coming out of hiding, we need time to react
		if (m_isEnemyHidden)
		{
			m_reacquireTimestamp = gpGlobals->time + Q_min(me->GetProfile()->GetReactionTime(), 0.08f + (1.0f - TheCSBots()->GetEffectiveSkill(me)) * 0.08f);
			m_isEnemyHidden = false;
		}
	}

	// if we haven't seen our enemy for a long time, chase after them
	float chaseTime = 2.0f + 2.0f * (1.0f - me->GetProfile()->GetAggression());

	// if we are sniping, be very patient
	if (me->IsUsingSniperRifle())
		chaseTime += 3.0f;
	// if we are crouching, be a little patient
	else if (me->IsCrouching())
		chaseTime += 1.0f;

	// if we can't see the enemy, and have either seen him but currently lost sight of him,
	// or haven't yet seen him, chase after him (unless we are a sniper)
	if (!me->IsEnemyVisible() && (notSeenEnemyTime > chaseTime || !m_haveSeenEnemy))
	{
		// snipers don't chase their prey - they wait for their prey to come to them
		if (me->GetTask() == CCSBot::SNIPING)
		{
			StopAttacking(me);
			return;
		}
		else
		{
			// move to last known position of enemy
			me->SetTask(CCSBot::MOVE_TO_LAST_KNOWN_ENEMY_POSITION, pEnemy);
			me->MoveTo(&me->GetLastKnownEnemyPosition());
			return;
		}
	}

	// if we can't see our enemy at the moment, and were shot by
	// a different visible enemy, engage them instead
	const float hurtRecentlyTime = 3.0f;
	if (!me->IsEnemyVisible() &&
		me->GetTimeSinceAttacked() < hurtRecentlyTime &&
		me->GetAttacker() &&
		me->GetAttacker() != me->GetEnemy())
	{
		const float immediateThreatRangeSq = 420.0f * 420.0f;
		const bool immediateThreat = (me->GetAttacker()->pev->origin - me->pev->origin).LengthSquared() < immediateThreatRangeSq;

		// if we can see a stronger immediate attacker, switch; otherwise keep the current fight sticky
		if (me->IsVisible(me->GetAttacker(), CHECK_FOV))
		{
			if (immediateThreat || notSeenEnemyTime > 0.85f)
			{
				me->Attack(me->GetAttacker());
				me->PrintIfWatched("Switching targets to retaliate against new attacker!\n");
			}
		}
		return;
	}

	if (gpGlobals->time > m_reacquireTimestamp)
	{
		if (me->IsEnemyVisible() && TheCSBots()->AllowFriendlyFireDamage() && me->IsFriendInLineOfFire())
		{
			me->ClearMovement();
			m_combatRepositionDir = ChooseCombatStrafeDir(me, pEnemy, m_combatRepositionDir);
			ApplyCombatStrafe(me, m_combatRepositionDir);

			m_nextDodgeStateTimestamp = gpGlobals->time + RANDOM_FLOAT(0.25f, 0.55f);
			return;
		}

		if (me->IsEnemyVisible() &&
			!me->IsUsingKnife() &&
			!me->IsUsingSniperRifle() &&
			!me->IsUsingShotgun() &&
			!me->IsUsingMachinegun() &&
			!me->IsUsingPistol())
		{
			const float speed = me->pev->velocity.Make2D().Length();
			if (gpGlobals->time >= m_nextStopAndShootTimestamp && speed > 95.0f)
			{
				const bool crowdedFight = me->GetNearbyEnemyCount() >= 2 || me->IsOutnumbered();
				const float stopChance = crowdedFight ? (18.0f + skill * 18.0f) : (45.0f + skill * 30.0f);
				if (RANDOM_FLOAT(0.0f, 100.0f) < stopChance)
					m_stopAndShootTimestamp = gpGlobals->time + RANDOM_FLOAT(0.055f, crowdedFight ? 0.11f : 0.18f);
				m_nextStopAndShootTimestamp = gpGlobals->time + RANDOM_FLOAT(crowdedFight ? 0.75f : 0.45f, crowdedFight ? 1.45f : 0.95f);
			}

			if (gpGlobals->time < m_stopAndShootTimestamp)
			{
				me->ClearMovement();
				if (me->GetNearbyEnemyCount() >= 2)
				{
					m_combatRepositionDir = ChooseCombatStrafeDir(me, pEnemy, m_combatRepositionDir);
					ApplyCombatStrafe(me, m_combatRepositionDir);
				}
				return;
			}
		}

		me->FireWeaponAtEnemy();
	}

	bool didCombatReposition = false;
	if (gpGlobals->time < m_combatRepositionEndTimestamp && me->IsEnemyVisible() && !me->IsUsingSniperRifle() && !m_crouchAndHold)
	{
		ApplyCombatStrafe(me, m_combatRepositionDir);

		if (me->GetNearbyEnemyCount() >= 3 || me->OutnumberedCount() >= 2)
			me->MoveBackward();

		didCombatReposition = true;
	}

	// do dodge behavior
	// If sniping or crouching, stand still.
	if (m_dodge && !didCombatReposition && !me->IsUsingSniperRifle() && !m_crouchAndHold)
	{
		Vector toEnemy = pEnemy->pev->origin - me->pev->origin;
		float range = toEnemy.Length2D();

		const float hysterisRange = 125.0f;		// (+/-) m_combatRange

		float minRange = me->GetCombatRange() - hysterisRange;
		float maxRange = me->GetCombatRange() + hysterisRange;

		// move towards (or away from) enemy if we are using a knife, behind a corner, or we aren't very skilled
		if (TheCSBots()->GetEffectiveSkill(me) < 0.66f || !me->IsEnemyVisible())
		{
			if (range > maxRange)
				me->MoveForward();
			else if (range < minRange)
				me->MoveBackward();
		}

		// don't dodge if enemy is facing away
		const float dodgeRange = 2000.0f;
		if (range > dodgeRange || !me->IsPlayerFacingMe(pEnemy))
		{
			m_dodgeState = STEADY_ON;
			m_nextDodgeStateTimestamp = 0.0f;
		}
		else if (gpGlobals->time >= m_nextDodgeStateTimestamp)
		{
			int next;
			const bool enemyVisible = me->IsEnemyVisible();
			const float speed = me->pev->velocity.Make2D().Length();

			// select next dodge state that is different that our current one
			do
			{
				// high-skill bots may jump when first engaging the enemy (if they are moving)
				const float jumpChance = 10.0f;
				const bool crowdedFight = me->GetNearbyEnemyCount() >= 2 || me->IsOutnumbered();
				if (m_firstDodge && skill > 0.65f && !crowdedFight && RANDOM_FLOAT(0, 100) < jumpChance && !me->IsNotMoving())
					next = RANDOM_LONG(0, NUM_ATTACK_STATES - 1);
				else if (crowdedFight && enemyVisible && RANDOM_FLOAT(0.0f, 100.0f) < 72.0f)
				{
					m_combatRepositionDir = ChooseCombatStrafeDir(me, pEnemy, m_combatRepositionDir);
					next = (m_combatRepositionDir < 0) ? SLIDE_LEFT : SLIDE_RIGHT;
				}
				else if (enemyVisible && speed > 120.0f && RANDOM_FLOAT(0.0f, 100.0f) < 28.0f)
					next = STEADY_ON;
				else
					next = RANDOM_LONG(0, NUM_ATTACK_STATES - 2);
			}
			while (!m_firstDodge && next == m_dodgeState);

			m_dodgeState = (DodgeStateType)next;
			m_nextDodgeStateTimestamp = gpGlobals->time + RANDOM_FLOAT(enemyVisible ? 0.34f : 0.55f, enemyVisible ? 0.92f : 1.55f);
			m_firstDodge = false;
		}

		switch (m_dodgeState)
		{
		case STEADY_ON:
		{
			break;
		}
		case SLIDE_LEFT:
		{
			ApplyCombatStrafe(me, -1);
			break;
		}
		case SLIDE_RIGHT:
		{
			ApplyCombatStrafe(me, 1);
			break;
		}
		case JUMP:
		{
			if (me->m_isEnemyVisible)
			{
				me->Jump();
			}
			break;
		}
		}
	}
}

// Finish attack
void AttackState::OnExit(CCSBot *me)
{
	me->PrintIfWatched("AttackState:OnExit()\n");

	m_crouchAndHold = false;

	// clear any noises we heard during battle
	me->ForgetNoise();
	me->ResetStuckMonitor();

	// resume our original posture
	me->PopPostureContext();

	// put shield away
	if (me->IsProtectedByShield())
		me->SecondaryAttack();

	me->StopRapidFire();
	me->ClearSurpriseDelay();
}
