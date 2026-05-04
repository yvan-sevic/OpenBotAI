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

// range for snipers to select a hiding spot
const float sniperHideRange = 2000.0f;

static bool IsValidBombsiteZoneIndex(int zoneIndex)
{
	if (zoneIndex < 0 || zoneIndex >= TheCSBots()->GetZoneCount())
		return false;

	const CCSBotManager::Zone *zone = TheCSBots()->GetZone(zoneIndex);
	return zone && zone->m_areaCount > 0;
}

static const CCSBotManager::Zone *GetValidBombsiteZone(int zoneIndex)
{
	return IsValidBombsiteZoneIndex(zoneIndex) ? TheCSBots()->GetZone(zoneIndex) : nullptr;
}

static int GetClosestBombsiteZoneIndex(CCSBot *me)
{
	float travelDistance = 0.0f;
	const CCSBotManager::Zone *zone = TheCSBots()->GetClosestZone(me->GetLastKnownArea(), PathCost(me), &travelDistance);
	return zone ? zone->m_index : -1;
}

static int GetRandomBombsiteZoneIndex()
{
	if (TheCSBots()->GetZoneCount() <= 0)
		return -1;

	for (int attempt = 0; attempt < 8; attempt++)
	{
		int zoneIndex = RANDOM_LONG(0, TheCSBots()->GetZoneCount() - 1);
		if (IsValidBombsiteZoneIndex(zoneIndex))
			return zoneIndex;
	}

	for (int zoneIndex = 0; zoneIndex < TheCSBots()->GetZoneCount(); zoneIndex++)
	{
		if (IsValidBombsiteZoneIndex(zoneIndex))
			return zoneIndex;
	}

	return -1;
}

static const CCSBotManager::Zone *GetCommittedAttackZone(CCSBot *me)
{
	if (!me)
		return nullptr;

	if (IsValidBombsiteZoneIndex(me->GetCommittedBombsite()))
		return TheCSBots()->GetZone(me->GetCommittedBombsite());

	int zoneIndex = -1;
	int directorZoneIndex = TheCSBots()->GetDirectorRecommendedBombsite(me);
	if (IsValidBombsiteZoneIndex(directorZoneIndex))
		zoneIndex = directorZoneIndex;

	if (zoneIndex < 0 && !me->IsCarryingBomb() && me->CanSeeBomber())
	{
		CBasePlayer *bomber = me->GetBomber();
		if (bomber)
		{
			if (bomber->IsBot())
			{
				CCSBot *bomberBot = static_cast<CCSBot *>(bomber);
				if (IsValidBombsiteZoneIndex(bomberBot->GetCommittedBombsite()))
					zoneIndex = bomberBot->GetCommittedBombsite();
			}

			if (zoneIndex < 0)
			{
				const CCSBotManager::Zone *zone = TheCSBots()->GetClosestZone(bomber);
				if (zone && IsValidBombsiteZoneIndex(zone->m_index))
					zoneIndex = zone->m_index;
			}
		}
	}

	if (zoneIndex < 0)
	{
		int closestZone = GetClosestBombsiteZoneIndex(me);
		int randomZone = GetRandomBombsiteZoneIndex();

		// Humans usually pick a site and stick with it, but not everyone blindly takes the shortest route.
		float closestSiteChance = me->IsCarryingBomb() ? 62.0f : 42.0f;
		closestSiteChance += 18.0f * me->GetProfile()->GetTeamwork();
		closestSiteChance -= 12.0f * me->GetProfile()->GetAggression();

		if (IsValidBombsiteZoneIndex(closestZone) && RANDOM_FLOAT(0.0f, 100.0f) < closestSiteChance)
			zoneIndex = closestZone;
		else if (IsValidBombsiteZoneIndex(randomZone))
			zoneIndex = randomZone;
		else
			zoneIndex = closestZone;
	}

	if (!IsValidBombsiteZoneIndex(zoneIndex))
		return nullptr;

	me->SetCommittedBombsite(zoneIndex);
	return TheCSBots()->GetZone(zoneIndex);
}

static bool TryCommittedAttackPlan(CCSBot *me)
{
	if (!me || me->IsRogue() || me->IsCarryingBomb() || me->GetGameState()->IsBombPlanted())
		return false;

	if (me->GetGameState()->IsLooseBombLocationKnown() || me->GetFriendsRemaining() <= 0)
		return false;

	const CCSBotManager::Zone *zone = GetCommittedAttackZone(me);
	if (!zone)
		return false;

	CCSBotManager::DirectorRoleType role = TheCSBots()->GetDirectorRole(me);
	if (role == CCSBotManager::DIRECTOR_ROLE_LURK && TheCSBots()->GetElapsedRoundTime() < 22.0f && RANDOM_FLOAT(0.0f, 100.0f) < 68.0f)
	{
		CNavArea *area = me->GetLastKnownArea();
		if (area)
		{
			me->SetTask(CCSBot::SEEK_AND_DESTROY);
			me->Hide(area, RANDOM_FLOAT(5.0f, 12.0f), 450.0f);
			me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
			me->PrintIfWatched("Director role: lurking before joining the attack\n");
			return true;
		}
	}
	else if (role == CCSBotManager::DIRECTOR_ROLE_SNIPER && TheCSBots()->GetElapsedRoundTime() < 18.0f && RANDOM_FLOAT(0.0f, 100.0f) < 52.0f)
	{
		CNavArea *area = me->GetLastKnownArea();
		if (area)
		{
			me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
			me->Hide(area, RANDOM_FLOAT(8.0f, 18.0f), sniperHideRange);
			me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
			me->PrintIfWatched("Director role: holding sniper pressure before rotating\n");
			return true;
		}
	}

	if (zone->m_extent.Contains(&me->pev->origin))
	{
		CNavArea *area = TheCSBots()->GetRandomAreaInZone(zone);
		if (area)
		{
			me->SetTask(CCSBot::SEEK_AND_DESTROY);
			me->Hide(area, RANDOM_FLOAT(4.0f, 12.0f), 700.0f);
			me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
			me->PrintIfWatched("Holding committed attack site instead of rotating away\n");
			return true;
		}
	}

	const Vector *pos = TheCSBots()->GetRandomPositionInZone(zone);
	if (!pos)
		return false;

	me->SetTask(CCSBot::SEEK_AND_DESTROY);
	me->Run();
	me->MoveTo(pos, (role == CCSBotManager::DIRECTOR_ROLE_ENTRY && RANDOM_FLOAT(0.0f, 100.0f) < 58.0f) ? FASTEST_ROUTE : SAFEST_ROUTE);
	me->PrintIfWatched("Supporting committed bombsite attack plan\n");
	return true;
}

static bool TryEarlyIndependentPlan(CCSBot *me)
{
	if (!me || me->IsRogue() || me->IsCarryingBomb() || me->GetGameState()->IsBombPlanted())
		return false;

	const float elapsed = TheCSBots()->GetElapsedRoundTime();
	if (elapsed > 18.0f || me->GetFriendsRemaining() <= 1)
		return false;

	float chance = 14.0f + (1.0f - me->GetProfile()->GetTeamwork()) * 34.0f;
	chance -= me->GetProfile()->GetAggression() * 8.0f;
	CCSBotManager::DirectorRoleType role = TheCSBots()->GetDirectorRole(me);

	if (TheCSBots()->IsOnDefense(me))
		chance += 16.0f;
	if (role == CCSBotManager::DIRECTOR_ROLE_ANCHOR || role == CCSBotManager::DIRECTOR_ROLE_SNIPER)
		chance += 10.0f;
	else if (role == CCSBotManager::DIRECTOR_ROLE_ROTATOR)
		chance -= 8.0f;
	if (me->IsSniper())
		chance += 20.0f;
	if (me->GetNearbyFriendCount() >= 2)
		chance += 14.0f;
	if (me->IsSafe())
		chance += 6.0f;

	chance = Q_max(6.0f, Q_min(chance, 58.0f));
	if (RANDOM_FLOAT(0.0f, 100.0f) > chance)
		return false;

	if (TheCSBots()->IsOnDefense(me))
	{
		const CCSBotManager::Zone *zone = GetValidBombsiteZone(TheCSBots()->GetDirectorRecommendedBombsite(me));
		if (!zone)
			zone = TheCSBots()->GetRandomZone();
		CNavArea *area = zone ? TheCSBots()->GetRandomAreaInZone(zone) : nullptr;
		if (area)
		{
			if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB)
				me->SetTask(CCSBot::GUARD_BOMB_ZONE);
			else if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES)
				me->SetTask(CCSBot::GUARD_HOSTAGES);
			else if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_ESCORT_VIP)
				me->SetTask(CCSBot::GUARD_VIP_ESCAPE_ZONE);
			me->Hide(area, RANDOM_FLOAT(6.0f, 18.0f), 650.0f + 450.0f * me->GetProfile()->GetAggression());
			me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
			me->PrintIfWatched("Breaking early pack to hold a defensive angle\n");
			return true;
		}
	}

	CNavArea *area = me->GetLastKnownArea();
	if (area)
	{
		me->Hide(area, RANDOM_FLOAT(3.5f, 9.0f), 300.0f);
		me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
		me->PrintIfWatched("Breaking early pack with a short independent hold\n");
		return true;
	}

	return false;
}

// The Idle state.
// We never stay in the Idle state - it is a "home base" for the state machine that
// does various checks to determine what we should do next.
void IdleState::OnEnter(CCSBot *me)
{
	me->DestroyPath(MoveDbgPathClearReason::TaskOrStateChange, __FUNCTION__);
	me->SetEnemy(nullptr);

	// lurking death
	if (me->IsUsingKnife() && me->IsWellPastSafe() && !me->IsHurrying())
		me->Walk();

	// Since Idle assigns tasks, we assume that coming back to Idle means our task is complete
	me->SetTask(CCSBot::SEEK_AND_DESTROY);
	me->SetDisposition(CCSBot::ENGAGE_AND_INVESTIGATE);
}

// Determine what we should do next
void IdleState::OnUpdate(CCSBot *me)
{
	// all other states assume GetLastKnownArea() is valid, ensure that it is
	if (!me->GetLastKnownArea() && me->StayOnNavMesh() == false)
		return;

	// zombies never leave the Idle state
	if (cv_bot_zombie.value > 0.0f)
	{
		me->ResetStuckMonitor();
		return;
	}

	// if we are in the early "safe" time, grab a knife or grenade
	if (me->IsSafe())
	{
		// if we have a grenade, use it
		if (!me->EquipGrenade())
		{
			// Some players keep a weapon out while rotating during freeze/safe time.
			if (TheCSBots()->GetEffectiveSkill(me) > 0.33f && me->ShouldEquipKnifeForSafeRun())
			{
				me->EquipKnife();
			}
		}
	}

	// if round is over, hunt
	if (me->GetGameState()->IsRoundOver())
	{
		// if we are escorting hostages, try to get to the rescue zone
		if (me->GetHostageEscortCount())
		{
			const CCSBotManager::Zone *zone = TheCSBots()->GetClosestZone(me->GetLastKnownArea(), PathCost(me, FASTEST_ROUTE));
			const Vector *zonePos = TheCSBots()->GetRandomPositionInZone(zone);
#ifdef REGAMEDLL_FIXES
			if (zonePos)
#endif
			{
				me->SetTask(CCSBot::RESCUE_HOSTAGES);
				me->Run();
				me->SetDisposition(CCSBot::SELF_DEFENSE);
				me->MoveTo(zonePos, FASTEST_ROUTE);
				me->PrintIfWatched("Trying to rescue hostages at the end of the round\n");
				return;
			}
		}

		me->Hunt();
		return;
	}

	const float defenseSniperCampChance = 75.0f;
	const float offenseSniperCampChance = 10.0f;

	// if we were following someone, continue following them
	if (me->IsFollowing())
	{
		me->ContinueFollowing();
		return;
	}

	if (TryEarlyIndependentPlan(me))
		return;

	// Scenario logic
	switch (TheCSBots()->GetScenario())
	{
	case CCSBotManager::SCENARIO_DEFUSE_BOMB:
	{
		// if this is a bomb game and we have the bomb, go plant it
		if (me->m_iTeam == TERRORIST)
		{
			if (me->GetGameState()->IsBombPlanted())
			{
				if (me->GetGameState()->GetPlantedBombsite() != CSGameState::UNKNOWN)
				{
					// T's always know where the bomb is - go defend it
					const CCSBotManager::Zone *zone = TheCSBots()->GetZone(me->GetGameState()->GetPlantedBombsite());
#ifdef REGAMEDLL_FIXES
					if (zone)
#endif
					{
						me->SetTask(CCSBot::GUARD_TICKING_BOMB);

						Place place = TheNavAreaGrid.GetPlace(&zone->m_center);
						if (place != UNDEFINED_PLACE)
						{
							// pick a random hiding spot in this place
							const Vector *spot = FindRandomHidingSpot(me, place, me->IsSniper());
							if (spot)
							{
								me->Hide(spot);
								return;
							}
						}

						// hide nearby
						me->Hide(TheNavAreaGrid.GetNearestNavArea(&zone->m_center));
						return;
					}
				}
				else
				{
					// ask our teammates where the bomb is
					me->GetChatter()->RequestBombLocation();

					// we dont know where the bomb is - we must search the bombsites
					int zoneIndex = me->GetGameState()->GetNextBombsiteToSearch();

					// move to bombsite - if we reach it, we'll update its cleared status, causing us to select another
					const Vector *pos = TheCSBots()->GetRandomPositionInZone(TheCSBots()->GetZone(zoneIndex));
					if (pos)
					{
						me->SetTask(CCSBot::FIND_TICKING_BOMB);
						me->MoveTo(pos);
						return;
					}
				}
			}
			else if (me->IsCarryingBomb())
			{
				// if we're at a bomb site, plant the bomb
				if (me->IsAtBombsite())
				{
					// plant it
					me->SetTask(CCSBot::PLANT_BOMB);
					me->PlantBomb();

					// radio to the team
					me->GetChatter()->PlantingTheBomb(me->GetPlace());

					return;
				}

				const CCSBotManager::Zone *zone = GetCommittedAttackZone(me);
				if (zone)
				{
					// move to the committed bomb site instead of re-picking a new site after every interruption
					const Vector *pos = TheCSBots()->GetRandomPositionInZone(zone);
					if (pos)
					{
						me->SetTask(CCSBot::PLANT_BOMB);
						me->Run();
						me->MoveTo(pos);

						return;
					}
				}
			}
			else
			{
				// small chance of sniper camping on offense, if we aren't carrying the bomb
				if (me->GetFriendsRemaining() && me->IsSniper() && RANDOM_FLOAT(0, 100.0f) < offenseSniperCampChance)
				{
					me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
					me->Hide(me->GetLastKnownArea(), RANDOM_FLOAT(10.0f, 30.0f), sniperHideRange);
					me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
					me->PrintIfWatched("Sniping!\n");
					return;
				}

				// if the bomb is loose (on the ground), go get it
				if (me->NoticeLooseBomb())
				{
					me->FetchBomb();
					return;
				}

				if (TryCommittedAttackPlan(me))
					return;

				// if bomb has been planted, and we hear it, move to a hiding spot near the bomb and guard it
				if (!me->IsRogue() && me->GetGameState()->IsBombPlanted() && me->GetGameState()->GetBombPosition())
				{
					const Vector *bombPos = me->GetGameState()->GetBombPosition();

					if (bombPos)
					{
						me->SetTask(CCSBot::GUARD_TICKING_BOMB);
						me->Hide(TheNavAreaGrid.GetNavArea(bombPos));
						return;
					}
				}
			}
		}
		// CT
		else
		{
			if (me->GetGameState()->IsBombPlanted())
			{
				// if the bomb has been planted, attempt to defuse it
				const Vector *bombPos = me->GetGameState()->GetBombPosition();
				if (bombPos)
				{
					// if someone is defusing the bomb, guard them
					if (TheCSBots()->GetBombDefuser())
					{
						if (!me->IsRogue())
						{
							me->SetTask(CCSBot::GUARD_BOMB_DEFUSER);
							me->Hide(TheNavAreaGrid.GetNavArea(bombPos));
							return;
						}
					}
					else if (me->IsDoingScenario())
					{
						// move to the bomb and defuse it
						me->SetTask(CCSBot::DEFUSE_BOMB);
						me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
						me->MoveTo(bombPos);
						return;
					}
					else
					{
						// we're not allowed to defuse, guard the bomb zone
						me->SetTask(CCSBot::GUARD_BOMB_ZONE);
						me->Hide(TheNavAreaGrid.GetNavArea(bombPos));
						me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
						return;
					}
				}
				else if (me->GetGameState()->GetPlantedBombsite() != CSGameState::UNKNOWN)
				{
					// we know which bombsite, but not exactly where the bomb is, go there
					const CCSBotManager::Zone *zone = TheCSBots()->GetZone(me->GetGameState()->GetPlantedBombsite());
					if (zone)
					{
						if (me->IsDoingScenario())
						{
							me->SetTask(CCSBot::DEFUSE_BOMB);
							me->MoveTo(&zone->m_center);
							me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
							return;
						}
						else
						{
							// we're not allowed to defuse, guard the bomb zone
							me->SetTask(CCSBot::GUARD_BOMB_ZONE);
							me->Hide(TheNavAreaGrid.GetNavArea(&zone->m_center));
							me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
							return;
						}
					}
				}
				else
				{
					// we dont know where the bomb is - we must search the bombsites
					// find closest un-cleared bombsite
					const CCSBotManager::Zone *zone = nullptr;
					float travelDistance = 9999999.9f;

					for (int z = 0; z < TheCSBots()->GetZoneCount(); z++)
					{
						if (TheCSBots()->GetZone(z)->m_areaCount == 0)
							continue;

						// don't check bombsites that have been cleared
						if (me->GetGameState()->IsBombsiteClear(z))
							continue;

						// just use the first overlapping nav area as a reasonable approximation
						ShortestPathCost pathCost = ShortestPathCost();
						real_t dist = NavAreaTravelDistance(me->GetLastKnownArea(), TheNavAreaGrid.GetNearestNavArea(&TheCSBots()->GetZone(z)->m_center), pathCost);

#ifdef REGAMEDLL_FIXES
						if (dist < 0.0f)
							continue;
#endif

						if (dist < travelDistance)
						{
							zone = TheCSBots()->GetZone(z);
							travelDistance = dist;
						}
					}

					if (zone)
					{
						const float farAwayRange = 2000.0f;
						if (travelDistance > farAwayRange)
						{
							zone = nullptr;
						}
					}

					// if closest bombsite is "far away", pick one at random
					if (!zone)
					{
						int zoneIndex = me->GetGameState()->GetNextBombsiteToSearch();
						zone = TheCSBots()->GetZone(zoneIndex);
					}

					// move to bombsite - if we reach it, we'll update its cleared status, causing us to select another
					if (zone)
					{
						const Vector *pos = TheCSBots()->GetRandomPositionInZone(zone);
						if (pos)
						{
							me->SetTask(CCSBot::FIND_TICKING_BOMB);
							me->MoveTo(pos);
							return;
						}
					}
				}

				DbgAssert(!"A CT bot doesn't know what to do while the bomb is planted!\n");
			}

			// if we have a sniper rifle, we like to camp, whether rogue or not
			if (me->IsSniper())
			{
				if (RANDOM_FLOAT(0, 100) <= defenseSniperCampChance)
				{
					CNavArea *snipingArea = nullptr;

					// if the bomb is loose, snipe near it
					if (me->GetGameState()->IsLooseBombLocationKnown())
					{
						snipingArea = TheNavAreaGrid.GetNearestNavArea(me->GetGameState()->GetBombPosition());
						me->PrintIfWatched("Sniping near loose bomb\n");
					}
					else
					{
						// snipe bomb zone(s)
						const CCSBotManager::Zone *zone = GetValidBombsiteZone(TheCSBots()->GetDirectorRecommendedBombsite(me));
						if (!zone)
							zone = TheCSBots()->GetRandomZone();
						if (zone)
						{
							snipingArea = TheCSBots()->GetRandomAreaInZone(zone);
							me->PrintIfWatched("Sniping near bombsite\n");
						}
					}

					if (snipingArea)
					{
						me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
						me->Hide(snipingArea, -1.0f, sniperHideRange);
						me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
						return;
					}
				}
			}

			// rogues just hunt, unless they want to snipe
			// if this defender joined the local rush group, hunt
			// if we know the bomb is dropped, hunt for enemies and the loose bomb
			if (me->IsRogue() || TheCSBots()->ShouldDefenderRush(me) || me->GetGameState()->IsLooseBombLocationKnown())
			{
				me->Hunt();
				return;
			}

			// the lower our morale gets, the more we want to camp the bomb zone(s)
			// only decide to camp at the start of the round, or if we haven't seen anything for a long time
			if (me->IsSafe() || me->HasNotSeenEnemyForLongTime())
			{
				float guardBombsiteChance = -34.0f * me->GetMorale();

				if (RANDOM_FLOAT(0.0f, 100.0f) < guardBombsiteChance)
				{
					float guardRange = 500.0f + 100.0f * (me->GetMorale() + 3);

					// guard bomb zone(s)
					const CCSBotManager::Zone *zone = GetValidBombsiteZone(TheCSBots()->GetDirectorRecommendedBombsite(me));
					if (!zone)
						zone = TheCSBots()->GetRandomZone();
					if (zone)
					{
						CNavArea *area = TheCSBots()->GetRandomAreaInZone(zone);
						if (area)
						{
							me->PrintIfWatched("I'm guarding a bombsite\n");
							me->GetChatter()->AnnouncePlan("GoingToDefendBombsite", area->GetPlace());
							me->SetTask(CCSBot::GUARD_BOMB_ZONE);
							me->Hide(area, -1.0, guardRange);
							me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
							return;
						}
					}
				}
			}
		}
		break;
	}
	case CCSBotManager::SCENARIO_ESCORT_VIP:
	{
		if (me->m_iTeam == TERRORIST)
		{
			// if we have a sniper rifle, we like to camp, whether rogue or not
			if (me->IsSniper())
			{
				if (RANDOM_FLOAT(0, 100) <= defenseSniperCampChance)
				{
					// snipe escape zone(s)
					const CCSBotManager::Zone *zone = TheCSBots()->GetRandomZone();
					if (zone)
					{
						CNavArea *area = TheCSBots()->GetRandomAreaInZone(zone);
						if (area)
						{
							me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
							me->Hide(area, -1.0, sniperHideRange);
							me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
							me->PrintIfWatched("Sniping near escape zone\n");
							return;
						}
					}
				}
			}

			// rogues just hunt, unless they want to snipe
			// if this defender joined the local rush group, hunt
			if (me->IsRogue() || TheCSBots()->ShouldDefenderRush(me))
				break;

			// the lower our morale gets, the more we want to camp the escape zone(s)
			float guardEscapeZoneChance = -34.0f * me->GetMorale();

			if (RANDOM_FLOAT(0.0f, 100.0f) < guardEscapeZoneChance)
			{
				// guard escape zone(s)
				const CCSBotManager::Zone *zone = TheCSBots()->GetRandomZone();
				if (zone)
				{
					CNavArea *area = TheCSBots()->GetRandomAreaInZone(zone);
					if (area)
					{
						// guard the escape zone - stay closer if our morale is low
						me->SetTask(CCSBot::GUARD_VIP_ESCAPE_ZONE);
						me->PrintIfWatched("I'm guarding an escape zone\n");

						float escapeGuardRange = 750.0f + 250.0f * (me->GetMorale() + 3);
						me->Hide(area, -1.0, escapeGuardRange);
						me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
						return;
					}
				}
			}
		}
		// CT
		else
		{
			if (me->m_bIsVIP)
			{
				// if early in round, pick a random zone, otherwise pick closest zone
				const float earlyTime = 20.0f;
				const CCSBotManager::Zone *zone = nullptr;

				if (TheCSBots()->GetElapsedRoundTime() < earlyTime)
				{
					// pick random zone
					zone = TheCSBots()->GetRandomZone();
				}
				else
				{
					// pick closest zone
					zone = TheCSBots()->GetClosestZone(me->GetLastKnownArea(), PathCost(me));
				}

				if (zone)
				{
					// pick a random spot within the escape zone
					const Vector *pos = TheCSBots()->GetRandomPositionInZone(zone);
					if (pos)
					{
						// move to escape zone
						me->SetTask(CCSBot::VIP_ESCAPE);
						me->Run();
						me->MoveTo(pos);

						// tell team to follow
						const float repeatTime = 30.0f;
						if (me->GetFriendsRemaining() && TheCSBots()->GetRadioMessageInterval(EVENT_RADIO_FOLLOW_ME, me->m_iTeam) > repeatTime)
							me->SendRadioMessage(EVENT_RADIO_FOLLOW_ME);
						return;
					}
				}
			}
			else
			{
				// small chance of sniper camping on offense, if we aren't VIP
				if (me->GetFriendsRemaining() && me->IsSniper() && RANDOM_FLOAT(0, 100.0f) < offenseSniperCampChance)
				{
					me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
					me->Hide(me->GetLastKnownArea(), RANDOM_FLOAT(10.0f, 30.0f), sniperHideRange);
					me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
					me->PrintIfWatched("Sniping!\n");
					return;
				}
			}
		}
		break;
	}
	case CCSBotManager::SCENARIO_RESCUE_HOSTAGES:
	{
		if (me->m_iTeam == TERRORIST)
		{
			bool campHostages;

			// if we are in early game, camp the hostages
			if (me->IsSafe())
			{
				campHostages = true;
			}
			else if (me->GetGameState()->HaveSomeHostagesBeenTaken() || me->GetGameState()->AreAllHostagesBeingRescued())
			{
				campHostages = false;
			}
			else
			{
				// later in the game, camp either hostages or escape zone
				const float campZoneChance = 100.0f * (TheCSBots()->GetElapsedRoundTime() - me->GetSafeTime()) / 120.0f;
				campHostages = (RANDOM_FLOAT(0, 100) > campZoneChance) ? true : false;
			}

			// if we have a sniper rifle, we like to camp, whether rogue or not
			if (me->IsSniper())
			{
				if (RANDOM_FLOAT(0, 100) <= defenseSniperCampChance)
				{
					const Vector *hostagePos = me->GetGameState()->GetRandomFreeHostagePosition();
					if (hostagePos && campHostages)
					{
						me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
						me->PrintIfWatched("Sniping near hostages\n");
						me->Hide(TheNavAreaGrid.GetNearestNavArea(hostagePos), -1.0, sniperHideRange);
						me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
						return;
					}
					else
					{
						// camp the escape zone(s)
						if (me->GuardRandomZone(sniperHideRange))
						{
							me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
							me->PrintIfWatched("Sniping near a rescue zone\n");
							me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
							return;
						}
					}
				}
			}

			// if safe time is up, and we stumble across a hostage, guard it
			if (!me->IsSafe() && !me->IsRogue())
			{
				CBaseEntity *pHostage = me->GetGameState()->GetNearestVisibleFreeHostage();
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

			// decide if we want to hunt, or guard
			const float huntChance = 70.0f + 25.0f * me->GetMorale();

			// rogues just hunt, unless they want to snipe
			// if this defender joined the local rush group, hunt
			if (me->GetFriendsRemaining())
			{
				if (me->IsRogue() || TheCSBots()->ShouldDefenderRush(me) || RANDOM_FLOAT(0, 100) < huntChance)
				{
					me->Hunt();
					return;
				}
			}

			// decide whether to camp the hostages or the escape zones
			const Vector *hostagePos = me->GetGameState()->GetRandomFreeHostagePosition();
			if (hostagePos && campHostages)
			{
				CNavArea *area = TheNavAreaGrid.GetNearestNavArea(hostagePos);
				if (area)
				{
					// guard the hostages - stay closer to hostages if our morale is low
					me->SetTask(CCSBot::GUARD_HOSTAGES);
					me->PrintIfWatched("I'm guarding hostages\n");

					float hostageGuardRange = 750.0f + 250.0f * (me->GetMorale() + 3);
					me->Hide(area, -1.0, hostageGuardRange);
					me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);

					if (RANDOM_FLOAT(0, 100) < 50)
						me->GetChatter()->GuardingHostages(area->GetPlace(), IS_PLAN);

					return;
				}
			}

			// guard rescue zone(s)
			if (me->GuardRandomZone())
			{
				me->SetTask(CCSBot::GUARD_HOSTAGE_RESCUE_ZONE);
				me->PrintIfWatched("I'm guarding a rescue zone\n");
				me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
				me->GetChatter()->GuardingHostageEscapeZone(IS_PLAN);
				return;
			}
		}
		// CT
		else
		{
			// only decide to do something else if we aren't already rescuing hostages
			if (!me->GetHostageEscortCount())
			{
				// small chance of sniper camping on offense
				if (me->GetFriendsRemaining() && me->IsSniper() && RANDOM_FLOAT(0, 100.0f) < offenseSniperCampChance)
				{
					me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
					me->Hide(me->GetLastKnownArea(), RANDOM_FLOAT(10.0f, 30.0f), sniperHideRange);
					me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
					me->PrintIfWatched("Sniping!\n");
					return;
				}

				if (me->GetFriendsRemaining() && !me->GetHostageEscortCount())
				{
					// rogues just hunt, unless all friends are dead
					// if we have friends left, we might go hunting instead of hostage rescuing
					const float huntChance = 33.3f;
					if (me->IsRogue() || RANDOM_FLOAT(0.0f, 100.0f) < huntChance)
					{
						me->Hunt();
						return;
					}
				}
			}

			// look for free hostages - CT's have radar so they know where hostages are at all times
			CHostage *pHostage = me->GetGameState()->GetNearestFreeHostage();

			// if we are not allowed to do the scenario, guard the hostages to clear the area for the human(s)
			if (!me->IsDoingScenario())
			{
				if (pHostage)
				{
					CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&pHostage->pev->origin);
					if (area)
					{
						me->SetTask(CCSBot::GUARD_HOSTAGES);
						me->Hide(area);
						me->PrintIfWatched("I'm securing the hostages for a human to rescue\n");
						return;
					}
				}

				me->Hunt();
				return;
			}

			bool fetchHostages = false;
			bool rescueHostages = false;
			const CCSBotManager::Zone *zone = nullptr;
			me->SetGoalEntity(nullptr);

			// if we are escorting hostages, determine where to take them
			if (me->GetHostageEscortCount())
				zone = TheCSBots()->GetClosestZone(me->GetLastKnownArea(), PathCost(me, FASTEST_ROUTE));

			// if we are escorting hostages and there are more hostages to rescue,
			// determine whether it's faster to rescue the ones we have, or go get the remaining ones
			if (pHostage)
			{
				if (zone)
				{
					PathCost pathCost(me, FASTEST_ROUTE);
					float toZone = NavAreaTravelDistance(me->GetLastKnownArea(), zone->m_area[0], pathCost);
					float toHostage = NavAreaTravelDistance(me->GetLastKnownArea(), TheNavAreaGrid.GetNearestNavArea(&pHostage->pev->origin), pathCost);

					if (toHostage < 0.0f)
					{
						rescueHostages = true;
					}
					else
					{
						if (toZone < toHostage)
							rescueHostages = true;
						else
							fetchHostages = true;
					}
				}
				else
				{
					fetchHostages = true;
				}
			}
			else if (zone)
			{
				rescueHostages = true;
			}

			if (fetchHostages)
			{
				// go get hostages
				me->SetTask(CCSBot::COLLECT_HOSTAGES);
				me->Run();
				me->SetGoalEntity(pHostage);
				me->ResetWaitForHostagePatience();

				// if we already have some hostages, move to the others by the quickest route
				RouteType route = (me->GetHostageEscortCount()) ? FASTEST_ROUTE : SAFEST_ROUTE;
				me->MoveTo(&pHostage->pev->origin, route);
				me->PrintIfWatched("I'm collecting hostages\n");
				return;
			}

			if (rescueHostages)
			{
				me->SetTask(CCSBot::RESCUE_HOSTAGES);
				me->Run();
				me->SetDisposition(CCSBot::SELF_DEFENSE);
				me->MoveTo(TheCSBots()->GetRandomPositionInZone(zone), FASTEST_ROUTE);
				me->PrintIfWatched("I'm rescuing hostages\n");
				me->GetChatter()->EscortingHostages();
				return;
			}
		}
		break;
	}
	case CCSBotManager::SCENARIO_ESCAPE:
	{
		if (me->m_iTeam == TERRORIST)
		{
			// if early in round, pick a random zone, otherwise pick closest zone
			const float earlyTime = 20.0f;
			const CCSBotManager::Zone *zone = nullptr;

			if (TheCSBots()->GetElapsedRoundTime() < earlyTime)
			{
				// pick random zone
				zone = TheCSBots()->GetRandomZone();
			}
			else
			{
				// pick closest zone
				zone = TheCSBots()->GetClosestZone(me->GetLastKnownArea(), PathCost(me));
			}

			if (zone)
			{
				// pick a random spot within the escape zone
				const Vector *pos = TheCSBots()->GetRandomPositionInZone(zone);
				if (pos)
				{
					// move to escape zone
				//	me->SetTask(CCSBot::VIP_ESCAPE);
					me->Run();
					me->MoveTo(pos);
					return;
				}
			}
		}
		// CT
		else
		{
			if (me->IsSniper())
			{
				if (RANDOM_FLOAT(0, 100) <= defenseSniperCampChance)
				{
					// snipe escape zone(s)
					const CCSBotManager::Zone *zone = TheCSBots()->GetRandomZone();
					if (zone)
					{
						CNavArea *area = TheCSBots()->GetRandomAreaInZone(zone);
						if (area)
						{
							me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
							me->Hide(area, -1.0, sniperHideRange);
							me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
							me->PrintIfWatched("Sniping near escape zone\n");
							return;
						}
					}
				}
			}

			// rogues just hunt, unless they want to snipe
			// if this defender joined the local rush group, hunt
			if (me->IsRogue() || TheCSBots()->ShouldDefenderRush(me))
				break;

			// the lower our morale gets, the more we want to camp the escape zone(s)
			float guardEscapeZoneChance = -34.0f * me->GetMorale();

			if (RANDOM_FLOAT(0.0f, 100.0f) < guardEscapeZoneChance)
			{
				// guard escape zone(s)
				const CCSBotManager::Zone *zone = TheCSBots()->GetRandomZone();
				if (zone)
				{
					CNavArea *area = TheCSBots()->GetRandomAreaInZone(zone);
					if (area)
					{
						// guard the escape zone - stay closer if our morale is low
						//me->SetTask(CCSBot::GUARD_VIP_ESCAPE_ZONE);
						me->PrintIfWatched("I'm guarding an escape zone\n");

						float escapeGuardRange = 750.0f + 250.0f * (me->GetMorale() + 3);
						me->Hide(area, -1.0, escapeGuardRange);
						me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
						return;
					}
				}
			}
		}
	}
	// deathmatch
	default:
	{
		// sniping check
		if (me->GetFriendsRemaining() && me->IsSniper() && RANDOM_FLOAT(0, 100.0f) < offenseSniperCampChance)
		{
			me->SetTask(CCSBot::MOVE_TO_SNIPER_SPOT);
			me->Hide(me->GetLastKnownArea(), RANDOM_FLOAT(10.0f, 30.0f), sniperHideRange);
			me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
			me->PrintIfWatched("Sniping!\n");
			return;
		}
		break;
	}
	}

	// if we have nothing special to do, go hunting for enemies
	me->Hunt();
}
