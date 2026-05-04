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
#include "bot/cs_bot_init.h"
#include "bot/cs_bot_danger_memory.h"
#include "aim_log.h"
#include "bot/cs_bot_grenade_spots.h"

extern cvar_t bot_grenade_he_use_danger_memory;
extern cvar_t bot_grenade_debug_he_only;

static bool IsFunKnifeSlashLaneClear(CCSBot *bot)
{
	if (!bot)
		return false;

	UTIL_MakeVectors(bot->pev->v_angle);
	Vector forward = gpGlobals->v_forward;
	Vector origin = bot->pev->origin;

	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CBasePlayer *player = UTIL_PlayerByIndex(i);
		if (!player || player == bot || !player->IsPlayer() || !player->IsAlive())
			continue;

		Vector toPlayer = player->pev->origin - origin;
		float range = toPlayer.Length2D();

		// Never mess around with knife swings inside teammate shoulder range.
		if (bot->BotRelationship(player) == CBot::BOT_TEAMMATE && range < 135.0f)
			return false;

		if (range > 115.0f)
			continue;

		float along = DotProduct(toPlayer, forward);
		if (along <= 0.0f)
			continue;

		Vector closest = origin + forward * along;
		float lateral = (player->pev->origin - closest).Length2D();
		if (lateral > 58.0f)
			continue;

		TraceResult trace;
		UTIL_TraceLine(origin + Vector(0, 0, HalfHumanHeight), player->EyePosition(), dont_ignore_monsters, ignore_glass, ENT(bot->pev), &trace);
		if (trace.pHit == player->edict())
			return false;
	}

	return true;
}

static bool CanDoFunKnifeSlash(CCSBot *bot)
{
	return bot &&
		bot->IsUsingKnife() &&
		!bot->IsNotMoving() &&
		!bot->IsAttacking() &&
		!bot->IsBuying() &&
		!bot->IsDefusingBomb() &&
		!bot->IsEscapingFromBomb() &&
		bot->GetTask() != CCSBot::PLANT_BOMB &&
		!bot->IsEnemyVisible() &&
		bot->GetTimeSinceLastSawEnemy() > 4.0f &&
		IsFunKnifeSlashLaneClear(bot);
}

static bool ShouldSwitchCombatTargetBase(CCSBot *bot, CBasePlayer *threat)
{
	if (!bot || !threat)
		return false;

	CBasePlayer *enemy = bot->GetEnemy();
	if (!enemy || !bot->IsAttacking())
		return true;

	if (bot->BotRelationship(enemy) != CBot::BOT_ENEMY)
	{
		bot->ClearCombatTargetCommitState();
		return true;
	}

	if (enemy == threat)
		return true;

	const float threatDistSq = (threat->pev->origin - bot->pev->origin).LengthSquared();
	const float enemyDistSq = (enemy->pev->origin - bot->pev->origin).LengthSquared();

	if (bot->GetAttacker() == threat && bot->GetTimeSinceAttacked() < 0.75f)
		return true;

	if (bot->IsEnemyVisible() && bot->GetTimeSinceLastSawEnemy() < 0.65f)
	{
		const float closeThreatSq = 320.0f * 320.0f;
		const float muchCloserSq = 450.0f * 450.0f;
		return threatDistSq < closeThreatSq && enemyDistSq > threatDistSq + muchCloserSq;
	}

	if (bot->GetTimeSinceLastSawEnemy() < 1.15f)
		return false;

	const float closerSq = 250.0f * 250.0f;
	return threatDistSq + closerSq < enemyDistSq;
}

// During the short post-acquisition commit window, only override for clearly better threats
// (same bars as base logic, lost-target grace, or visible candidate while current is not visible).
static bool CombatTargetCommitAllowsSwitchAway(CCSBot *bot, CBasePlayer *threat)
{
	if (!bot || !threat)
		return true;

	CBasePlayer *enemy = bot->GetEnemy();
	if (!enemy)
		return true;

	if (bot->GetAttacker() == threat && bot->GetTimeSinceAttacked() < 0.75f)
		return true;

	const float threatDistSq = (threat->pev->origin - bot->pev->origin).LengthSquared();
	const float enemyDistSq = (enemy->pev->origin - bot->pev->origin).LengthSquared();

	if (bot->IsEnemyVisible() && bot->GetTimeSinceLastSawEnemy() < 0.65f)
	{
		const float closeThreatSq = 320.0f * 320.0f;
		const float muchCloserSq = 450.0f * 450.0f;
		if (threatDistSq < closeThreatSq && enemyDistSq > threatDistSq + muchCloserSq)
			return true;
	}

	if (bot->GetTimeSinceLastSawEnemy() >= 1.15f)
		return true;

	if (!bot->IsEnemyVisible() && bot->IsVisible(threat, false))
		return true;

	return false;
}

static bool ShouldSwitchCombatTarget(CCSBot *bot, CBasePlayer *threat)
{
	if (!ShouldSwitchCombatTargetBase(bot, threat))
		return false;

	if (!bot || !threat)
		return false;

	CBasePlayer *enemy = bot->GetEnemy();
	if (!enemy || enemy == threat || !bot->IsAttacking())
		return true;

	if (!bot->IsCombatTargetCommitActive())
		return true;

	if (CombatTargetCommitAllowsSwitchAway(bot, threat))
		return true;

	bot->MaybeLogCombatTargetCommitBlock(threat, enemy);
	return false;
}

static bool IsNearOwnSpawn(CCSBot *bot, float radius)
{
	if (!bot || !gpGlobals)
		return false;

	const char *spawnClass = nullptr;
	if (bot->m_iTeam == TERRORIST)
		spawnClass = "info_player_deathmatch";
	else if (bot->m_iTeam == CT)
		spawnClass = "info_player_start";
	else
		return false;

	CBaseEntity *spawn = nullptr;
	while ((spawn = UTIL_FindEntityByClassname(spawn, spawnClass)))
	{
		if ((bot->pev->origin - spawn->pev->origin).Length2D() <= radius)
			return true;
	}

	// Some servers generate neutral nav-based spawn points. Use a smaller radius
	// so these don't accidentally cover too much of the map.
	spawn = nullptr;
	while ((spawn = UTIL_FindEntityByClassname(spawn, "info_spawn_point")))
	{
		if ((bot->pev->origin - spawn->pev->origin).Length2D() <= radius * 0.65f)
			return true;
	}

	return false;
}

static bool ShouldLeaveOwnSpawnHold(CCSBot *bot)
{
	if (!bot || !gpGlobals || !TheCSBots())
		return false;

	if (TheCSBots()->GetElapsedRoundTime() < 25.0f || bot->IsBuying() || bot->IsEscapingFromBomb())
		return false;

	if (!IsNearOwnSpawn(bot, 900.0f))
		return false;

	if (bot->GetGameState()->IsBombPlanted() || bot->GetGameState()->IsLooseBombLocationKnown() || TheCSBots()->GetLooseBomb())
		return false;

	const float nearestEnemyRange = bot->GetRangeToNearestRecognizedEnemy();
	const bool recentDanger = bot->IsEnemyVisible()
		|| bot->GetNearbyEnemyCount() > 0
		|| bot->GetTimeSinceLastSawEnemy() < 4.0f
		|| bot->GetTimeSinceAttacked() < 3.0f
		|| (nearestEnemyRange > 0.0f && nearestEnemyRange < 1100.0f)
		|| (bot->IsNoiseHeard() && bot->GetNoisePriority() >= PRIORITY_MEDIUM);
	if (recentDanger)
		return false;

	switch (bot->GetTask())
	{
	case CCSBot::HOLD_POSITION:
	case CCSBot::MOVE_TO_SNIPER_SPOT:
	case CCSBot::SNIPING:
	case CCSBot::SEEK_AND_DESTROY:
		break;
	default:
		return false;
	}

	return bot->IsHiding() || bot->IsAtHidingSpot() || bot->IsNotMoving();
}

// Aim update cadence (CCS bot):
// - CBot::BotThink() calls Upkeep() every g_flBotCommandInterval.
// - Upkeep() -> UpdateAimController() -> UpdateCombatAimController() or UpdateNonCombatAimController()
//   sets desired m_lookYaw / m_lookPitch (via SetLookAngles), then UpdateLookAngles() integrates into pev->v_angle.
// - Roaming look *target selection* (SetLookAt, UpdateLookAround, UpdateRoamLook) runs from behavior states
//   during Update() on g_flBotFullThinkInterval; it only changes m_lookAt* state consumed later by Upkeep().
// - Final pev->v_angle ownership: CCSBot::UpdateLookAngles() for normal bots; early-outs for mimic and AI chat freeze.
// - ExecuteCommand() must only forward pev->v_angle into usercmd_t (bot.cpp).

// Lightweight maintenance, invoked frequently
void CCSBot::Upkeep()
{
	if (TheCSBots()->IsLearningMap() || !IsAlive())
		return;

	if (m_isRapidFiring)
		TogglePrimaryAttack();

	if (m_funKnifeSlashEndTimestamp > 0.0f)
	{
		if (gpGlobals->time < m_funKnifeSlashEndTimestamp && CanDoFunKnifeSlash(this))
		{
			PrimaryAttack();
		}
		else
		{
			ClearPrimaryAttack();
			m_funKnifeSlashEndTimestamp = 0.0f;
		}
	}

	UpdateAimController();
	MaybeLogMovementDecisionOscillationDebug();
}

// Heavyweight processing, invoked less often
void CCSBot::Update()
{
	if (TheCSBots()->IsAnalysisRequested() && m_processMode == PROCESS_NORMAL)
	{
		TheCSBots()->AckAnalysisRequest();
		StartAnalyzeAlphaProcess();
	}

	switch (m_processMode)
	{
	case PROCESS_LEARN:         UpdateLearnProcess();        return;
	case PROCESS_ANALYZE_ALPHA: UpdateAnalyzeAlphaProcess(); return;
	case PROCESS_ANALYZE_BETA:  UpdateAnalyzeBetaProcess();  return;
	case PROCESS_SAVE:          UpdateSaveProcess();         return;
	}

	// update our radio chatter
	// need to allow bots to finish their chatter even if they are dead
	GetChatter()->Update();

	if (m_voiceFeedbackEndTimestamp != 0.0f
		&& (m_voiceFeedbackEndTimestamp <= gpGlobals->time || gpGlobals->time < m_voiceFeedbackStartTimestamp))
	{
		EndVoiceFeedback(NO_FORCE);
	}

	// check if we are dead
	if (!IsAlive())
	{
		// remember that we died
		m_diedLastRound = true;
		BotDeathThink();
		return;
	}

	// show line of fire
	if ((cv_bot_traceview.value == 100.0 && IsLocalPlayerWatchingMe()) || cv_bot_traceview.value == 101.0)
	{
		UTIL_MakeVectors(pev->punchangle + pev->v_angle);

		if (!IsFriendInLineOfFire())
		{
			Vector vecAiming = gpGlobals->v_forward;
			Vector vecSrc = GetGunPosition();

			if (m_iTeam == TERRORIST)
				UTIL_DrawBeamPoints(vecSrc, vecSrc + 2000.0f * vecAiming, 1, 255, 0, 0);
			else
				UTIL_DrawBeamPoints(vecSrc, vecSrc + 2000.0f * vecAiming, 1, 0, 50, 255);
		}
	}

	//
	// Debug beam rendering
	//

	// show approach points
	if ((cv_bot_traceview.value == 2.0f && IsLocalPlayerWatchingMe()) || cv_bot_traceview.value == 3.0f)
		DrawApproachPoints();

	// show encounter spot data
	if ((cv_bot_traceview.value == 4.0f && IsLocalPlayerWatchingMe()) || cv_bot_traceview.value == 5.0f)
	{
		if (m_spotEncounter)
		{
			UTIL_DrawBeamPoints(m_spotEncounter->path.from, m_spotEncounter->path.to, 3, 0, 0, 255);

			Vector dir = m_spotEncounter->path.to - m_spotEncounter->path.from;
			float length = dir.NormalizeInPlace();

			for (auto& order : m_spotEncounter->spotList) {
				UTIL_DrawBeamPoints(m_spotEncounter->path.from + order.t * length * dir, *order.spot->GetPosition(), 3, 0, 255, 255);
			}
		}
	}

	// show path navigation data
	if (cv_bot_traceview.value == 1.0f && IsLocalPlayerWatchingMe())
	{
		Vector from = GetEyePosition();
		const float size = 50.0f;

		Vector arrow(size * float(Q_cos(m_lookAheadAngle * M_PI / 180.0f)), size * float(Q_sin(m_lookAheadAngle * M_PI / 180.0f)), 0.0f);
		UTIL_DrawBeamPoints(from, from + arrow, 1, 0, 255, 255);
	}

	if (cv_bot_stop.value != 0.0f)
		return;

	// check if we are stuck
	StuckCheck();

	// if our current 'noise' was heard a long time ago, forget it
	const float rememberNoiseDuration = 20.0f;
	if (m_noiseTimestamp > 0.0f && gpGlobals->time - m_noiseTimestamp > rememberNoiseDuration)
	{
		ForgetNoise();
	}

	// where are we
	if (!m_currentArea || !m_currentArea->Contains(&pev->origin))
	{
		m_currentArea = TheNavAreaGrid.GetNavArea(&pev->origin);
	}

	// track the last known area we were in
	if (m_currentArea && m_currentArea != m_lastKnownArea)
	{
		m_lastKnownArea = m_currentArea;
		// assume that we "clear" an area of enemies when we enter it
		m_currentArea->SetClearedTimestamp(m_iTeam - 1);
	}

	// update approach points
	const float recomputeApproachPointTolerance = 50.0f;
	if ((m_approachPointViewPosition - pev->origin).IsLengthGreaterThan(recomputeApproachPointTolerance))
	{
		ComputeApproachPoints();
		m_approachPointViewPosition = pev->origin;
	}

	if (cv_bot_show_nav.value > 0.0f && m_lastKnownArea)
	{
		m_lastKnownArea->DrawConnectedAreas();
	}

	if (ShouldLeaveOwnSpawnHold(this))
	{
		ClearPrimaryAttack();
		EquipBestWeapon(MUST_EQUIP);
		SetTask(SEEK_AND_DESTROY);
		SetDisposition(ENGAGE_AND_INVESTIGATE);
		Run();
		Hunt();
		return;
	}

	// if we're blind, retreat!
	if (IsBlind())
	{
		if (!IsAtHidingSpot())
		{
			switch (m_blindMoveDir)
			{
			case FORWARD:  MoveForward();  break;
			case RIGHT:    StrafeRight();  break;
			case BACKWARD: MoveBackward(); break;
			case LEFT:     StrafeLeft();   break;
			default:       Crouch();       break;
			}
		}

		if (m_blindFire)
		{
			PrimaryAttack();
		}

		return;
	}

	// Enemy acquisition and attack initiation
	// take a snapshot and update our reaction time queue
	UpdateReactionQueue();

	// "threat" may be the same as our current enemy
	CBasePlayer* threat = GetRecognizedEnemy();
	if (threat)
	{
		// adjust our personal "safe" time
		AdjustSafeTime();

		if (IsUsingGrenade())
		{
			Vector throwAt = threat->pev->origin;
			const CBasePlayerWeapon *w = GetActiveWeapon();
			const int gw = w ? w->m_iId : 0;
			if ((gw == WEAPON_FLASHBANG || gw == WEAPON_HEGRENADE || gw == WEAPON_SMOKEGRENADE)
			    && WouldGrenadeAimRiskTeammates(throwAt, gw))
				EquipBestWeapon(MUST_EQUIP);
			else
				ThrowGrenade(&throwAt);
		}
		else
		{
			// Decide if we should attack
			bool doAttack = false;
			switch (GetDisposition())
			{
			case IGNORE_ENEMIES:
			{
				// never attack
				doAttack = false;
				break;
			}
			case SELF_DEFENSE:
			{
				// attack if fired on
				doAttack = IsPlayerLookingAtMe(threat);

				// attack if enemy very close
				if (!doAttack)
				{
					const float selfDefenseRange = 750.0f;
					doAttack = (pev->origin - threat->pev->origin).IsLengthLessThan(selfDefenseRange);
				}
				break;
			}
			case ENGAGE_AND_INVESTIGATE:
			case OPPORTUNITY_FIRE:
			{
				// normal combat range
				doAttack = true;
				break;
			}
			}

			if (doAttack)
			{
				if (ShouldSwitchCombatTarget(this, threat))
				{
					if (IsUsingKnife() && IsHiding())
					{
						// if hiding with a knife, wait until threat is close
						const float knifeAttackRange = 250.0f;
						if ((pev->origin - threat->pev->origin).IsLengthLessThan(knifeAttackRange))
						{
							Attack(threat);
						}
					}
					else
					{
						Attack(threat);
					}
				}
			}
			else
			{
				// dont attack, but keep track of nearby enemies
				SetEnemy(threat);
				m_isEnemyVisible = true;
			}
		}

		// if we aren't attacking but we are being attacked, retaliate
		if (GetDisposition() != IGNORE_ENEMIES && !IsAttacking())
		{
			const float recentAttackDuration = 1.0f;
			if (GetTimeSinceAttacked() < recentAttackDuration)
			{
				// we may not be attacking our attacker, but at least we're not just taking it
				// (since m_attacker isn't reaction-time delayed, we can't directly use it)
				Attack(threat);
				PrintIfWatched("Ouch! Retaliating!\n");
			}
		}

		TheCSBots()->SetLastSeenEnemyTimestamp();
	}

	// Validate existing enemy, if any
	if (m_enemy.IsValid())
	{
		if (IsAwareOfEnemyDeath())
		{
			// we have noticed that our enemy has died
			m_enemy = nullptr;
			m_isEnemyVisible = false;
			m_combatTargetCommitUntil = 0.0f;
			m_combatTargetCommitDbgNextLog = 0.0f;
		}
		else
		{
			// check LOS to current enemy (chest & head), in case he's dead (GetNearestEnemy() only returns live players)
			// note we're not checking FOV - once we've acquired an enemy (which does check FOV), assume we know roughly where he is
			if (IsVisible(m_enemy, false, &m_visibleEnemyParts))
			{
				m_isEnemyVisible = true;
				m_lastSawEnemyTimestamp = gpGlobals->time;
				m_lastEnemyPosition = m_enemy->pev->origin;
			}
			else
			{
				m_isEnemyVisible = false;
			}

			// check if enemy died
			if (m_enemy->IsAlive())
			{
				m_enemyDeathTimestamp = 0.0f;
				m_isLastEnemyDead = false;
			}
			else if (m_enemyDeathTimestamp == 0.0f)
			{
				// note time of death (to allow bots to overshoot for a time)
				m_enemyDeathTimestamp = gpGlobals->time;
				m_isLastEnemyDead = true;
			}
		}
	}
	else
	{
		m_isEnemyVisible = false;
		ClearCombatTargetCommitState();
	}

	// If LOS is lost, only keep a very short visual memory. Long wall tracking looks like wallhack.
	const float seenRecentTime = IsEnemyVisible() ? 3.0f : 0.45f;
	if (m_enemy.IsValid() && GetTimeSinceLastSawEnemy() < seenRecentTime)
	{
		AimAtEnemy();
	}
	else
	{
		StopAiming();
	}

	// Hack to fire while retreating
	// TODO: Encapsulate aiming and firing on enemies separately from current task
	if (GetDisposition() == IGNORE_ENEMIES)
	{
		FireWeaponAtEnemy();
	}

	// Manual HE lineups are "map knowledge" pre-nades, not danger reactions.
	// Check them while rotating so bots throw common spots even with no visible enemy.
	if (gpGlobals && TheCSBots()->AllowGrenades() && HasGrenadeKind(WEAPON_HEGRENADE)
		&& !IsBuying() && !IsEscapingFromBomb() && !IsUsingGrenade() && !m_isWaitingToTossGrenade)
	{
		const float nearestEnemyRange = GetRangeToNearestRecognizedEnemy();
		const bool enemyTooClose = IsEnemyVisible() || (nearestEnemyRange > 0.0f && nearestEnemyRange < 650.0f) || GetNearbyEnemyCount() > 0;

		if (bot_grenade_debug_he_only.value > 0.0f || !enemyTooClose)
		{
			if (m_nextTacticalGrenadeTimestamp <= 0.0f)
				m_nextTacticalGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(0.20f, 0.75f);

			if (gpGlobals->time >= m_nextTacticalGrenadeTimestamp)
			{
				if (TryManualHEGrenadeThrow())
					m_nextTacticalGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(2.0f, 4.0f);
				else if (bot_grenade_debug_he_only.value > 0.0f
					&& !enemyTooClose
					&& BotGrenadeSpots::IsNearManualThrowSpot(this, WEAPON_HEGRENADE, 560.0f, 1150.0f)
					&& EquipGrenadeKind(WEAPON_HEGRENADE))
				{
					m_grenadeHeldNoThrowSince = gpGlobals->time;
					m_nextTacticalGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(0.20f, 0.45f);
				}
				else
					m_nextTacticalGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(0.25f, 0.85f);
			}
		}
	}

	// Smoke lineups are tactical utility: use recorded map smokes when a bot
	// reaches the lineup, without waiting for combat danger.
	if (gpGlobals && TheCSBots()->AllowGrenades() && HasGrenadeKind(WEAPON_SMOKEGRENADE)
		&& !IsBuying() && !IsEscapingFromBomb() && !IsUsingGrenade() && !m_isWaitingToTossGrenade)
	{
		const float nearestEnemyRange = GetRangeToNearestRecognizedEnemy();
		const bool enemyTooClose = IsEnemyVisible() || (nearestEnemyRange > 0.0f && nearestEnemyRange < 650.0f) || GetNearbyEnemyCount() > 0;
		const bool terroristPostPlantSmoke = (m_iTeam == TERRORIST
			&& GetGameState()->IsBombPlanted()
			&& GetGameState()->GetBombPosition()
			&& (pev->origin - *GetGameState()->GetBombPosition()).Length2D() < 1400.0f
			&& BotGrenadeSpots::IsNearManualThrowSpot(this, WEAPON_SMOKEGRENADE, 420.0f, 420.0f));

		if (!enemyTooClose && (terroristPostPlantSmoke || BotGrenadeSpots::IsNearManualThrowSpot(this, WEAPON_SMOKEGRENADE, 240.0f, 240.0f)))
		{
			if (m_nextTacticalGrenadeTimestamp <= 0.0f)
				m_nextTacticalGrenadeTimestamp = gpGlobals->time + (terroristPostPlantSmoke ? RANDOM_FLOAT(0.10f, 0.35f) : RANDOM_FLOAT(0.20f, 0.75f));

			if (gpGlobals->time >= m_nextTacticalGrenadeTimestamp)
			{
				if (TryTacticalGrenadeThrow())
					m_nextTacticalGrenadeTimestamp = gpGlobals->time + (terroristPostPlantSmoke ? RANDOM_FLOAT(7.0f, 12.0f) : RANDOM_FLOAT(4.0f, 8.0f));
				else
					m_nextTacticalGrenadeTimestamp = gpGlobals->time + (terroristPostPlantSmoke ? RANDOM_FLOAT(0.20f, 0.55f) : RANDOM_FLOAT(0.35f, 0.90f));
			}
		}
	}

	// A common human pattern is early HE pressure: most bots try to spend
	// an HE shortly after leaving spawn, with manual lineups preferred below.
	if (!m_openingGrenadeChecked && gpGlobals && TheCSBots()->GetElapsedRoundTime() > 1.0f)
	{
		m_openingGrenadeChecked = true;
		m_openingGrenadeWantsThrow = (RANDOM_FLOAT(0.0f, 100.0f) < 90.0f);
		m_openingGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(0.25f, 2.25f);
	}

	if (m_openingGrenadeWantsThrow && gpGlobals && gpGlobals->time >= m_openingGrenadeTimestamp
		&& TheCSBots()->AllowGrenades() && HasGrenadeKind(WEAPON_HEGRENADE)
		&& !IsBuying() && !IsEscapingFromBomb() && !m_isWaitingToTossGrenade)
	{
		if (!IsUsingGrenade())
		{
			const bool triedThrow = TryTacticalGrenadeThrow();
			if (!triedThrow && bot_grenade_debug_he_only.value <= 0.0f && EquipGrenadeKind(WEAPON_HEGRENADE))
				m_grenadeHeldNoThrowSince = gpGlobals->time;
		}
	}

	if (IsUsingGrenade() && !m_isWaitingToTossGrenade)
	{
		const float nearestEnemyRange = GetRangeToNearestRecognizedEnemy();
		const bool enemyTooClose = (nearestEnemyRange > 0.0f && nearestEnemyRange < 650.0f);
		const bool immediateThreat = IsEnemyVisible() || enemyTooClose || GetNearbyEnemyCount() > 0 || GetTimeSinceAttacked() < 1.0f;

		if (immediateThreat)
		{
			ClearPrimaryAttack();
			EquipBestWeapon(MUST_EQUIP);
			m_grenadePinPulled = false;
			m_grenadeReleaseSent = false;
			m_grenadeThrowLogged = false;
			m_grenadeHeldNoThrowSince = 0.0f;
			m_openingGrenadeWantsThrow = false;
		}
	}

	// Resolve a throw target while holding a grenade (pin ready, not in wind-up).
	// NOTE: IsEndOfSafeTime() is only true for one frame — never use it here or HE/flash/smoke almost never get a target.
	if (IsUsingGrenade() && (!IsUsingHEGrenade() || !IsSafe() || IsWellPastSafe() || m_openingGrenadeWantsThrow) && !m_isWaitingToTossGrenade)
	{
		Vector target;
		bool haveTarget = false;
		BotGrenadeSpots::FoundSpotInfo spotInfo;
		bool usedSpot = false;

		const bool strictManualHE = (bot_grenade_debug_he_only.value > 0.0f && IsUsingHEGrenade());

		if (IsUsingHEGrenade() && BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_HEGRENADE, &target, &spotInfo))
		{
			haveTarget = true;
			usedSpot = true;
		}
		else if (!strictManualHE && IsUsingHEGrenade() && bot_grenade_he_use_danger_memory.value > 0.0f
			 && BotGrenadeSpots::ShouldAttemptDangerHeFallback(this)
			 && BotDangerMemory::FindBestHeGrenadeTargetFromMemory(this, &target))
			haveTarget = true;
		else if (!strictManualHE && IsUsingHEGrenade() && m_openingGrenadeWantsThrow && FindGrenadeTossPathTarget(&target))
			haveTarget = true;
		else if (IsUsingFlashbang() && BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_FLASHBANG, &target, &spotInfo))
		{
			haveTarget = true;
			usedSpot = true;
		}
		else if (IsUsingFlashbang() && FindFlashbangPeekTarget(&target))
			haveTarget = true;
		else if (IsUsingSmokeGrenade() && BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_SMOKEGRENADE, &target, &spotInfo))
		{
			haveTarget = true;
			usedSpot = true;
		}
		else if (IsUsingSmokeGrenade()
			&& !BotGrenadeSpots::HasAnyLineupForGrenadeAndTeam(WEAPON_SMOKEGRENADE, m_iTeam)
			&& FindGrenadeTossPathTarget(&target))
		{
			if (!WouldGrenadeAimRiskTeammates(target, WEAPON_SMOKEGRENADE))
				haveTarget = true;
		}

		if (haveTarget)
		{
			const CBasePlayerWeapon *w = GetActiveWeapon();
			const int gw = w ? w->m_iId : 0;
			if ((gw == WEAPON_FLASHBANG || gw == WEAPON_HEGRENADE || gw == WEAPON_SMOKEGRENADE)
			    && WouldGrenadeAimRiskTeammates(target, gw))
			{
				EquipBestWeapon(MUST_EQUIP);
			}
			else
			{
				m_grenadeThrowTarget = target;
				m_grenadeUsedManualSpot = usedSpot;
				if (usedSpot)
				{
					m_grenadeManualSpotDistance = spotInfo.distanceFromSpot;
					m_grenadeManualSpotIndex = spotInfo.spotIndex;
					Q_strlcpy(m_grenadeManualSpotName, spotInfo.spotName, sizeof(m_grenadeManualSpotName));
				}
				else
				{
					m_grenadeManualSpotDistance = 0.0f;
					m_grenadeManualSpotIndex = -1;
					m_grenadeManualSpotName[0] = '\0';
				}
				ThrowGrenade(&target);
				if (IsUsingHEGrenade())
					m_openingGrenadeWantsThrow = false;
			}
		}
	}

	if (IsUsingGrenade())
	{
		CBasePlayerWeapon *activeGrenade = static_cast<CBasePlayerWeapon *>(GetActiveWeapon());
		bool doToss = (m_isWaitingToTossGrenade && m_grenadePinPulled && !m_grenadeReleaseSent && m_tossGrenadeTimer.IsElapsed());

		if (doToss)
		{
			const int activeGrenadeId = activeGrenade ? activeGrenade->m_iId : 0;
			if (activeGrenadeId == WEAPON_HEGRENADE)
			{
				const Vector eye = GetGunPosition();
				Vector desired = m_grenadeThrowTarget - eye;
				const float desiredLen = desired.NormalizeInPlace();
				UTIL_MakeVectors(pev->v_angle + pev->punchangle);
				const float onTarget = (desiredLen > 1.0f) ? DotProduct(desired, gpGlobals->v_forward) : 1.0f;

				TraceResult tr;
				UTIL_TraceLine(eye, eye + gpGlobals->v_forward * 384.0f, dont_ignore_monsters, ignore_glass, ENT(pev), &tr);
				if (onTarget < 0.94f || tr.flFraction < 0.55f)
				{
					ClearPrimaryAttack();
					EquipBestWeapon(MUST_EQUIP);
					m_isWaitingToTossGrenade = false;
					m_grenadePinPulled = false;
					m_grenadeReleaseSent = false;
					m_grenadeThrowLogged = false;
					m_grenadeHeldNoThrowSince = 0.0f;
					m_openingGrenadeWantsThrow = false;
					return;
				}
			}

			// Log bot grenade throw before the actual throw
			if (!m_grenadeThrowLogged)
			{
				AimLog::RecordBotGrenadeThrow(
					this,
					&m_grenadeThrowTarget,
					m_grenadeUsedManualSpot,
					m_grenadeManualSpotDistance,
					m_grenadeManualSpotIndex,
					m_grenadeManualSpotName
				);
				m_grenadeThrowLogged = true;
			}
			
			ClearPrimaryAttack();
			m_grenadeReleaseSent = true;
			m_tossGrenadeTimer.Start(1.0f);
		}
		else if (m_grenadeReleaseSent)
		{
			ClearPrimaryAttack();

			// The grenade entity is spawned from WeaponIdle after IN_ATTACK is released.
			// Stay committed until that happens, otherwise later weapon-switch logic can holster/cancel it.
			if (!activeGrenade || activeGrenade->m_flStartThrow == 0.0f || m_tossGrenadeTimer.IsElapsed())
			{
				m_isWaitingToTossGrenade = false;
				m_grenadePinPulled = false;
				m_grenadeReleaseSent = false;
				m_grenadeThrowLogged = false;
			}
		}
		else if (m_isWaitingToTossGrenade)
		{
			if (m_tossGrenadeTimer.IsElapsed() && !m_grenadePinPulled)
			{
				ClearPrimaryAttack();
				EquipBestWeapon(MUST_EQUIP);
				m_isWaitingToTossGrenade = false;
				m_grenadePinPulled = false;
				m_grenadeReleaseSent = false;
				m_grenadeThrowLogged = false;
				m_grenadeHeldNoThrowSince = 0.0f;
				m_openingGrenadeWantsThrow = false;
			}
			else
			{
				PrimaryAttack();
			}

			if (!m_grenadePinPulled && activeGrenade && activeGrenade->m_flStartThrow != 0.0f)
			{
				m_grenadePinPulled = true;
				m_tossGrenadeTimer.Start(0.20f);
			}
		}
		else
		{
			ClearPrimaryAttack();
		}
	}
	else
	{
		m_isWaitingToTossGrenade = false;
		m_grenadePinPulled = false;
		m_grenadeReleaseSent = false;
		m_grenadeThrowLogged = false;
		m_grenadeHeldNoThrowSince = 0.0f;
	}

	if (!IsUsingGrenade() || m_isWaitingToTossGrenade)
		m_grenadeHeldNoThrowSince = 0.0f;
	else if (m_grenadeHeldNoThrowSince <= 0.0f)
		m_grenadeHeldNoThrowSince = gpGlobals->time;
	else
	{
		const bool recentEnemy = (m_lastSawEnemyTimestamp > 0.0f && gpGlobals->time - m_lastSawEnemyTimestamp < 2.25f);
		const bool heardDanger = IsNoiseHeard() && GetNoisePriority() >= PRIORITY_MEDIUM;
		const bool hurtRecently = GetTimeSinceAttacked() < 1.85f;
		const float heldTime = gpGlobals->time - m_grenadeHeldNoThrowSince;
		const bool heldTooLong = heldTime > 1.15f;
		const bool campingWithGrenade = (IsAtHidingSpot() || IsHiding() || IsNotMoving()) && heldTime > 0.75f;

		const bool allowManualPrepHold = bot_grenade_debug_he_only.value > 0.0f
			&& (IsUsingHEGrenade() || IsUsingSmokeGrenade())
			&& !campingWithGrenade
			&& !heardDanger
			&& !hurtRecently
			&& !IsEnemyVisible()
			&& BotGrenadeSpots::IsNearManualThrowSpot(this, IsUsingSmokeGrenade() ? WEAPON_SMOKEGRENADE : WEAPON_HEGRENADE, 560.0f, 1150.0f)
			&& heldTime <= 2.25f;

		if (!allowManualPrepHold && (campingWithGrenade || heardDanger || hurtRecently || (recentEnemy && GetDisposition() != IGNORE_ENEMIES) || heldTooLong))
		{
			ClearPrimaryAttack();
			EquipBestWeapon(MUST_EQUIP);
			m_grenadePinPulled = false;
			m_grenadeReleaseSent = false;
			m_grenadeThrowLogged = false;
			m_grenadeHeldNoThrowSince = 0.0f;
			m_openingGrenadeWantsThrow = false;
			if (m_nextTacticalGrenadeTimestamp < gpGlobals->time + 2.0f)
				m_nextTacticalGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(3.5f, 8.0f);
		}
	}

	if (UpdateBoltSniperQuickSwitch())
		return;

	if (IsWellPastSafe() && TheCSBots()->AllowGrenades() && !IsBuying() && !IsSafe()
	    && (!IsAttacking() || HasGrenadeKind(WEAPON_HEGRENADE))
	    && !m_isWaitingToTossGrenade && !IsUsingGrenade())
	{
		if (m_nextTacticalGrenadeTimestamp <= 0.0f)
			m_nextTacticalGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(2.0f, 6.0f);

		if (gpGlobals->time >= m_nextTacticalGrenadeTimestamp && TryTacticalGrenadeThrow())
			m_nextTacticalGrenadeTimestamp = gpGlobals->time + RANDOM_FLOAT(4.0f, 9.0f);
	}

#ifdef REGAMEDLL_FIXES
	// bots with low skill cannot switch weapons underwater
	if (TheCSBots()->GetEffectiveSkill(this) > 0.4f && pev->waterlevel == 3 && !IsActiveWeaponCanShootUnderwater())
	{
		EquipBestWeapon(MUST_EQUIP);
	}
#endif

	if (IsHunting() && IsWellPastSafe() && IsUsingGrenade() && !m_isWaitingToTossGrenade)
	{
		EquipBestWeapon(MUST_EQUIP);
	}

	// check if our weapon is totally out of ammo
	// or if we no longer feel "safe", equip our weapon
	if (!IsSafe() && !IsUsingGrenade() && IsActiveWeaponOutOfAmmo())
	{
		EquipBestWeapon();
	}

	// TODO: This doesn't work if we are restricted to just knives and sniper rifles because we cant use the rifle at close range
	if (!IsSafe() && !IsUsingGrenade() && IsUsingKnife() && !IsEscapingFromBomb())
	{
		EquipBestWeapon();
	}

	// if we haven't seen an enemy in awhile, and we switched to our pistol during combat,
	// switch back to our primary weapon (if it still has ammo left)
	const float safeRearmTime = 5.0f;
	if (!IsActiveWeaponReloading() && IsUsingPistol() && !IsPrimaryWeaponEmpty() && GetTimeSinceLastSawEnemy() > safeRearmTime)
	{
		EquipBestWeapon();
	}

	// reload our weapon if we must
	ReloadCheck();

	// equip silencer
	SilencerCheck();

	// Humans often swing the knife once in a while while running around safely.
	if (CanDoFunKnifeSlash(this))
	{
		if (m_nextFunKnifeSlashTimestamp == 0.0f)
			m_nextFunKnifeSlashTimestamp = gpGlobals->time + RANDOM_FLOAT(2.0f, 6.0f);

		if (gpGlobals->time >= m_nextFunKnifeSlashTimestamp)
		{
			PrimaryAttack();
			if (RANDOM_FLOAT(0.0f, 100.0f) < 38.0f)
			{
				m_funKnifeSlashEndTimestamp = gpGlobals->time + RANDOM_FLOAT(0.20f, 0.85f);
				m_nextFunKnifeSlashTimestamp = m_funKnifeSlashEndTimestamp + RANDOM_FLOAT(3.0f, 9.0f);
			}
			else
			{
				m_funKnifeSlashEndTimestamp = 0.0f;
				m_nextFunKnifeSlashTimestamp = gpGlobals->time + RANDOM_FLOAT(3.5f, 10.0f);
			}
		}
	}
	else
	{
		m_nextFunKnifeSlashTimestamp = 0.0f;
		m_funKnifeSlashEndTimestamp = 0.0f;
	}

	// listen to the radio
	RespondToRadioCommands();

	// make way
	const float avoidTime = 0.33f;
	if (gpGlobals->time - m_avoidTimestamp < avoidTime && m_avoid)
	{
		StrafeAwayFromPosition(&m_avoid->pev->origin);
	}
	else
	{
		m_avoid = nullptr;
	}

	if (m_isJumpCrouching)
	{
		const float duration = 0.75f;
		const float crouchDelayTime = 0.05f;
		const float standUpTime = 0.6f;
		float elapsed = gpGlobals->time - m_jumpCrouchTimestamp;

		if (elapsed > crouchDelayTime && elapsed < standUpTime)
			Crouch();

		if (elapsed >= standUpTime)
			StandUp();

		if (elapsed > duration)
			m_isJumpCrouching = false;
	}

	// if we're using a sniper rifle and are no longer attacking, stop looking thru scope
	if (!IsAtHidingSpot() && !IsAttacking() && IsUsingSniperRifle() && IsUsingScope())
	{
		SecondaryAttack();
	}

#ifdef REGAMEDLL_ADD
	if (!IsBlind())
	{
#endif
	// check encounter spots
	UpdatePeripheralVision();

	// Update gamestate
	if (m_bomber)
	{
		GetChatter()->SpottedBomber(GetBomber());
	}

#ifdef REGAMEDLL_ADD
	// watch for snipers
	if (CanSeeSniper() && !HasSeenSniperRecently())
	{
		GetChatter()->SpottedSniper();

		const float sniperRecentInterval = 20.0f;
		m_sawEnemySniperTimer.Start(sniperRecentInterval);
	}
#endif

	if (CanSeeLooseBomb())
	{
		GetChatter()->SpottedLooseBomb(TheCSBots()->GetLooseBomb());
	}
#ifdef REGAMEDLL_ADD
}
#endif
	// Scenario interrupts
	switch (TheCSBots()->GetScenario())
	{
	case CCSBotManager::SCENARIO_DEFUSE_BOMB:
	{
		// flee if the bomb is ready to blow and we aren't defusing it or attacking and we know where the bomb is
		// (aggressive players wait until its almost too late)
		float gonnaBlowTime = 8.0f - (2.0f * GetProfile()->GetAggression());

		// if we have a defuse kit, can wait longer
		if (m_bHasDefuser)
			gonnaBlowTime *= 0.66f;

		if (!IsEscapingFromBomb()								// we aren't already escaping the bomb
			&& TheCSBots()->IsBombPlanted()						// is the bomb planted
			&& GetGameState()->IsPlantedBombLocationKnown()		// we know where the bomb is
			&& TheCSBots()->GetBombTimeLeft() < gonnaBlowTime	// is the bomb about to explode
			&& !IsDefusingBomb()								// we aren't defusing the bomb
			&& !IsAttacking())									// we aren't in the midst of a firefight
		{
			EscapeFromBomb();
			break;
		}
		break;
	}
	case CCSBotManager::SCENARIO_RESCUE_HOSTAGES:
	{
		if (m_iTeam == CT)
		{
			UpdateHostageEscortCount();
		}
		else
		{
			// Terrorists have imperfect information on status of hostages
			CSGameState::ValidateStatusType status = GetGameState()->ValidateHostagePositions();

			if (status & CSGameState::HOSTAGES_ALL_GONE)
			{
				GetChatter()->HostagesTaken();
				Idle();
			}
			else if (status & CSGameState::HOSTAGE_GONE)
			{
				GetGameState()->HostageWasTaken();
				Idle();
			}
		}
		break;
	}
	}

	// Follow nearby humans if our co-op is high and we have nothing else to do
	// If we were just following someone, don't auto-follow again for a short while to
	// give us a chance to do something else.
	const float earliestAutoFollowTime = 5.0f;
	const float minAutoFollowTeamwork = 0.4f;
	if (TheCSBots()->GetElapsedRoundTime() > earliestAutoFollowTime
		&& GetProfile()->GetTeamwork() > minAutoFollowTeamwork
		&& CanAutoFollow()
		&& !IsBusy()
		&& !IsFollowing()
		&& !GetGameState()->IsAtPlantedBombsite())
	{
		// chance of following is proportional to teamwork attribute
		float followChance = GetProfile()->GetTeamwork();
		if (TheCSBots()->GetElapsedRoundTime() < 25.0f)
			followChance *= 0.45f + 0.25f * GetProfile()->GetTeamwork();

		if (followChance > RANDOM_FLOAT(0.0f, 1.0f))
		{
			CBasePlayer *pLeader = GetClosestVisibleHumanFriend();
			if (pLeader && pLeader->IsAutoFollowAllowed())
			{
				// count how many bots are already following this player
				const float maxFollowCount = (TheCSBots()->GetElapsedRoundTime() < 25.0f) ? 1 : 2;
				if (GetBotFollowCount(pLeader) < maxFollowCount)
				{
					const float autoFollowRange = 300.0f;
					if ((pLeader->pev->origin - pev->origin).IsLengthLessThan(autoFollowRange))
					{
						CNavArea *leaderArea = TheNavAreaGrid.GetNavArea(&pLeader->pev->origin);
						if (leaderArea)
						{
							PathCost cost(this, FASTEST_ROUTE);
							float travelRange = NavAreaTravelDistance(GetLastKnownArea(), leaderArea, cost);
							if (/*travelRange >= 0.0f &&*/ travelRange < autoFollowRange)
							{
								// follow this human
								Follow(pLeader);
								PrintIfWatched("Auto-Following %s\n", STRING(pLeader->pev->netname));

								if (CSGameRules()->IsCareer())
								{
									GetChatter()->Say("FollowingCommander", 10.0f);
								}
								else
								{
									GetChatter()->Say("FollowingSir", 10.0f);
								}
							}
						}
					}
				}
			}
		}
		else
		{
			// we decided not to follow, don't re-check for a duration
			m_allowAutoFollowTime = gpGlobals->time + 15.0f + (1.0f - GetProfile()->GetTeamwork()) * 30.0f;
		}
	}

	if (IsFollowing())
	{
		// if we are following someone, make sure they are still alive
		CBaseEntity *pLeader = m_leader;
		if (!pLeader || !pLeader->IsAlive())
		{
			StopFollowing();
		}

		// decide whether to continue following them
		const float highTeamwork = 0.85f;
		if (GetProfile()->GetTeamwork() < highTeamwork)
		{
			float minFollowDuration = 15.0f;
			if (GetFollowDuration() > minFollowDuration + 40.0f * GetProfile()->GetTeamwork())
			{
				// we are bored of following our leader
				StopFollowing();
				PrintIfWatched("Stopping following - bored\n");
			}
		}
	}
	else
	{
		if (GetMorale() < NEUTRAL && IsSafe() && GetSafeTimeRemaining() < 2.0f && IsHunting())
		{
			if (GetMorale() * -40.0 > RANDOM_FLOAT(0.0f, 100.0f))
			{
				if (TheCSBots()->IsOnOffense(this) || !TheCSBots()->ShouldDefenderRush(this))
				{
					SetDisposition(OPPORTUNITY_FIRE);
					Hide(m_lastKnownArea, RANDOM_FLOAT(3.0f, 15.0f));
					GetChatter()->Say("WaitingHere");
				}
			}
		}
	}

	// Execute state machine
	if (m_isAttacking)
	{
		m_attackState.OnUpdate(this);
	}
	else
	{
		m_state->OnUpdate(this);
	}

	if (m_isWaitingToTossGrenade && (m_grenadePinPulled || m_grenadeReleaseSent))
	{
		ResetStuckMonitor();
		ClearMovement();
	}

	if (IsReloading())
	{
		ResetStuckMonitor();

		const bool reloadDanger =
			GetNearbyEnemyCount() > 0 ||
			GetTimeSinceLastSawEnemy() < 4.0f ||
			GetTimeSinceAttacked() < 2.5f;

		bool pickedNewReloadMove = false;
		if (gpGlobals->time >= m_nextReloadMoveTimestamp)
		{
			pickedNewReloadMove = true;
			m_reloadMoveDir = RANDOM_LONG(0, reloadDanger ? 4 : 2);
			m_nextReloadMoveTimestamp = gpGlobals->time + RANDOM_FLOAT(0.35f, reloadDanger ? 0.72f : 1.05f);

			if (reloadDanger && !IsHiding() && !IsMovingTo() && RANDOM_FLOAT(0.0f, 100.0f) < 28.0f + 45.0f * TheCSBots()->GetEffectiveSkill(this))
			{
				PrintIfWatched("Trying to reload from a safer spot.\n");
				TryToRetreat();
			}
		}

		if (!IsBuying() && !IsDefusingBomb() && !m_isWaitingToTossGrenade)
		{
			if (reloadDanger)
			{
				ForceRun(0.6f);
				MoveBackward();
			}

			switch (m_reloadMoveDir)
			{
			case 0:
				StrafeLeft();
				break;
			case 1:
				StrafeRight();
				break;
			case 2:
				if (reloadDanger)
					MoveForward();
				break;
			case 3:
			case 4:
				if (reloadDanger && pickedNewReloadMove && !IsUsingLadder())
					Jump();
				break;
			}
		}
	}
	else
	{
		m_nextReloadMoveTimestamp = 0.0f;
	}

	// if we get too far ahead of the hostages we are escorting, wait for them
	if (!IsAttacking() && m_inhibitWaitingForHostageTimer.IsElapsed())
	{
		const float waitForHostageRange = 500.0f;
		if (GetTask() == RESCUE_HOSTAGES && GetRangeToFarthestEscortedHostage() > waitForHostageRange)
		{
			if (!m_isWaitingForHostage)
			{
				// just started waiting
				m_isWaitingForHostage = true;
				m_waitForHostageTimer.Start(10.0f);
			}
			else
			{
				// we've been waiting
				if (m_waitForHostageTimer.IsElapsed())
				{
					// give up waiting for awhile
					m_isWaitingForHostage = false;
					m_inhibitWaitingForHostageTimer.Start(3.0f);
				}
				else
				{
					// keep waiting
					ResetStuckMonitor();
					ClearMovement();
				}
			}
		}
	}

	// remember our prior safe time status
	m_wasSafe = IsSafe();
}

void CCSBot::BotThink()
{
	m_aimDiagMotorStep.valid = false;
	m_aimDiagRefusedAimCone = false;
	m_aimDiagConeEvaluated = false;
	m_aimDiagDidFire = 0;
	m_aimDiagAimDot = 0.0f;
	m_aimDiagRequiredAimDot = 0.0f;
	m_aimDiagAimDotMargin = 0.0f;
	m_aimDiagConeErrorDeg = 0.0f;
	m_aimDiagConeToleranceDeg = 0.0f;
	m_aimDiagConeRatio = 0.0f;
	m_aimDiagConePass = 0;
	CBot::BotThink();
}

void CCSBot::PostBotThinkAfterUpdate()
{
	if (!gpGlobals || !IsAlive() || !IsAimingAtEnemy())
		return;

	CBasePlayer *enemy = GetEnemy();
	if (!enemy)
		return;

	CBasePlayerWeapon *wpn = m_pActiveItem ? static_cast<CBasePlayerWeapon *>(m_pActiveItem) : nullptr;
	const int weaponId = wpn ? wpn->m_iId : 0;

	const float acquireAge = gpGlobals->time - m_currentEnemyAcquireTimestamp;
	const bool motorOk = m_aimDiagMotorStep.valid;

	AimCombatDiagRecord rec;
	Q_memset(&rec, 0, sizeof(rec));

	rec.time = gpGlobals->time;
	rec.botEnt = entindex();
	rec.botName = (pev && pev->netname) ? STRING(pev->netname) : "unknown";
	rec.enemyEnt = enemy->entindex();
	rec.weaponId = weaponId;
	rec.weaponName = AimLog::GetActiveWeaponEntityName(this);
	rec.enemyVisible = IsEnemyVisible() ? 1 : 0;
	rec.aimingAtEnemy = 1;
	rec.combatPhase = m_combatAimPhase;
	rec.confidence = m_combatAimConfidence;
	rec.motorScale = m_aimHumanMotorScale;
	rec.acquisitionBurst = m_aimDiagAcquisitionBurst ? 1 : 0;
	rec.acquireAge = acquireAge;

	if (motorOk)
	{
		rec.yawErrBeforeMotor = m_aimDiagMotorStep.yawErrorBefore;
		rec.pitchErrBeforeMotor = m_aimDiagMotorStep.pitchErrorBefore;
		rec.yawVel = m_aimDiagMotorStep.yawVelAfter;
		rec.pitchVel = m_aimDiagMotorStep.pitchVelAfter;
		rec.yawAccel = m_aimDiagMotorStep.yawAccelApplied;
		rec.pitchAccel = m_aimDiagMotorStep.pitchAccelApplied;
		rec.microYawSnap = m_aimDiagMotorStep.microYawSnap ? 1 : 0;
		rec.yawSnapMode = m_aimDiagMotorStep.yawSnapMode;
		rec.deltaYaw = m_aimDiagMotorStep.deltaYawWritten;
		rec.deltaPitch = m_aimDiagMotorStep.deltaPitchWritten;
	}
	else
	{
		rec.yawVel = m_lookYawVel;
		rec.pitchVel = m_lookPitchVel;
	}

	rec.jitterMag = m_aimDiagJitterMag;
	rec.aimOffsetMag = m_aimDiagAimOffsetMag;
	rec.recoilMag = m_aimDiagRecoilMag;
	rec.distEnemy = m_aimDiagDistEnemy;
	rec.firedAttackIntent = (GetPendingBotButtonFlags() & IN_ATTACK) ? 1 : 0;
	rec.refusedAimCone = m_aimDiagRefusedAimCone ? 1 : 0;
	rec.sniperViewMotionThresh = IsUsingSniperRifle() ? 20.0f : 0.0f;

	rec.aimConeEvaluated = m_aimDiagConeEvaluated ? 1 : 0;
	rec.aimDot = m_aimDiagAimDot;
	rec.requiredAimDot = m_aimDiagRequiredAimDot;
	rec.aimDotMargin = m_aimDiagAimDotMargin;
	rec.aimConeErrorDeg = m_aimDiagConeErrorDeg;
	rec.aimConeToleranceDeg = m_aimDiagConeToleranceDeg;
	rec.aimConeRatio = m_aimDiagConeRatio;
	rec.aimConePass = m_aimDiagConePass;
	rec.didFire = m_aimDiagDidFire;

	if (m_aimDiagMotorStep.motorForensicValid)
	{
		rec.motorDt = m_aimDiagMotorStep.motorDt;
		rec.yawVelPre = m_aimDiagMotorStep.yawVelPre;
		rec.pitchVelPre = m_aimDiagMotorStep.pitchVelPre;
		rec.yawAccelRaw = m_aimDiagMotorStep.yawAccelRaw;
		rec.pitchAccelRaw = m_aimDiagMotorStep.pitchAccelRaw;
		rec.yawAccelClamped = m_aimDiagMotorStep.yawAccelClamped;
		rec.pitchAccelClamped = m_aimDiagMotorStep.pitchAccelClamped;
		rec.yawVelPostIntegrate = m_aimDiagMotorStep.yawVelPostIntegrate;
		rec.pitchVelPostIntegrate = m_aimDiagMotorStep.pitchVelPostIntegrate;
		rec.yawStepRaw = m_aimDiagMotorStep.yawStepRaw;
		rec.pitchStepRaw = m_aimDiagMotorStep.pitchStepRaw;
		rec.yawStepApplied = m_aimDiagMotorStep.yawStepApplied;
		rec.pitchStepApplied = m_aimDiagMotorStep.pitchStepApplied;
		rec.yawVelFinal = m_aimDiagMotorStep.yawVelFinal;
		rec.pitchVelFinal = m_aimDiagMotorStep.pitchVelFinal;
		rec.yawErrPost = m_aimDiagMotorStep.yawErrPost;
		rec.pitchErrPost = m_aimDiagMotorStep.pitchErrPost;
		rec.zeroedYawVelReason = m_aimDiagMotorStep.zeroedYawVelReason;
		rec.zeroedPitchVelReason = m_aimDiagMotorStep.zeroedPitchVelReason;
		rec.directAngleWriteReason = m_aimDiagMotorStep.directAngleWriteReason;
		rec.motorDtInvalid = m_aimDiagMotorStep.motorDtInvalid;
	}

	AimLog::RecordBotCombatAimDiag(this, rec);

	if (!cv_bot_debug.value)
		return;

	const bool watchedGate = (IsLocalPlayerWatchingMe() && (cv_bot_debug.value == 1 || cv_bot_debug.value == 3))
		|| (cv_bot_debug.value == 2 || cv_bot_debug.value == 4);
	if (!watchedGate)
		return;

	if (gpGlobals->time < m_aimDiagPrintNextTime)
		return;

	m_aimDiagPrintNextTime = gpGlobals->time + 0.22f;

	PrintIfWatched(
		"[aimdiag] vis=%d phase=%d conf=%.2f mscale=%.2f burst=%d age=%.3f "
		"yPre=%.3f pPre=%.3f yVel=%.3f snap=%d dYaw=%.3f dPt=%.3f jit=%.3f off=%.3f rcl=%.3f "
		"dist=%.0f fire=%d refuseCone=%d sniperVm=%.1f\n",
		rec.enemyVisible,
		rec.combatPhase,
		rec.confidence,
		rec.motorScale,
		rec.acquisitionBurst,
		rec.acquireAge,
		rec.yawErrBeforeMotor,
		rec.pitchErrBeforeMotor,
		rec.yawVel,
		rec.microYawSnap,
		rec.deltaYaw,
		rec.deltaPitch,
		rec.jitterMag,
		rec.aimOffsetMag,
		rec.recoilMag,
		rec.distEnemy,
		rec.firedAttackIntent,
		rec.refusedAimCone,
		rec.sniperViewMotionThresh);
}

void CCSBot::MaybeLogCombatTargetCommitBlock(CBasePlayer *candidate, CBasePlayer *current)
{
	if (!cv_bot_debug.value || !gpGlobals || !candidate || !current)
		return;

	const bool watchedGate = (IsLocalPlayerWatchingMe() && (cv_bot_debug.value == 1 || cv_bot_debug.value == 3))
		|| (cv_bot_debug.value == 2 || cv_bot_debug.value == 4);
	if (!watchedGate)
		return;

	if (m_combatTargetCommitDbgNextLog > gpGlobals->time)
		return;

	m_combatTargetCommitDbgNextLog = gpGlobals->time + 0.5f;

	const char *candName = candidate->pev ? STRING(candidate->pev->netname) : nullptr;
	const char *curName = current->pev ? STRING(current->pev->netname) : nullptr;

	PrintIfWatched(
		"[tgtcommit] switch=blocked remain=%.2fs cand=%s(#%i) cur=%s(#%i) commit_active=y reason=hold\n",
		GetCombatTargetCommitRemain(),
		candName ? candName : "?",
		candidate->entindex(),
		curName ? curName : "?",
		current->entindex());
}
