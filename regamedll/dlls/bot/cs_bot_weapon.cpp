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
#include "bot/cs_bot_grenade_spots.h"

extern cvar_t bot_grenade_he_use_danger_memory;
extern cvar_t bot_grenade_debug_he_only;

static bool IsBoltActionSniper(CBasePlayerWeapon *weapon);
static bool ShouldHoldFullAuto(CCSBot *bot, float rangeToEnemy);

// Fire our active weapon towards our current enemy
// NOTE: Aiming our weapon is handled in RunBotUpkeep()
void CCSBot::FireWeaponAtEnemy()
{
	CBasePlayer *pEnemy = GetEnemy();
	if (!pEnemy)
	{
		StopRapidFire();
		return;
	}

	if (IsUsingSniperRifle())
	{
		// if we're using a sniper rifle, don't fire until we are standing still, are zoomed in, and not rapidly moving our view
		if (!IsNotMoving() || GetZoomLevel() == NO_ZOOM || IsViewMoving(20.0f))
		{
			return;
		}
	}

	if (gpGlobals->time > m_fireWeaponTimestamp && GetTimeSinceAcquiredCurrentEnemy() >= GetProfile()->GetAttackDelay() && GetTimeSinceAcquiredCurrentEnemy() >= GetSurpriseDelay())
	{
		ClearSurpriseDelay();

		if (!(IsRecognizedEnemyProtectedByShield() && IsPlayerFacingMe(pEnemy))		// dont shoot at enemies behind shields
			&& !IsActiveWeaponReloading()
			&& !IsActiveWeaponClipEmpty()
			&& IsEnemyVisible())
		{
			// we have a clear shot - pull trigger if we are aiming at enemy
			Vector2D toAimSpot = (m_aimSpot - pev->origin).Make2D();
			float rangeToEnemy = toAimSpot.NormalizeInPlace();

			const real_t halfPI = (M_PI / 180.0f);
			real_t yaw = pev->v_angle[YAW] * halfPI;

			Vector2D dir(Q_cos(yaw), Q_sin(yaw));
			real_t onTarget = DotProduct(toAimSpot, dir);

			// aim more precisely with a sniper rifle; no-scoped or sweeping shots look very unnatural.
			const real_t halfSize = (IsUsingSniperRifle()) ? HalfHumanWidth * 0.42f : 2.0f * HalfHumanWidth;

			// aiming tolerance depends on how close the target is - closer targets subtend larger angles
			real_t aimTolerance = Q_cos(Q_atan(halfSize / rangeToEnemy));

			m_aimDiagConeEvaluated = true;
			m_aimDiagDidFire = 0;
			m_aimDiagAimDot = (float)onTarget;
			m_aimDiagRequiredAimDot = (float)aimTolerance;
			m_aimDiagAimDotMargin = (float)(onTarget - aimTolerance);
			{
				const float dotC = Q_max(-1.0f, Q_min(1.0f, (float)onTarget));
				const float tolC = Q_max(-1.0f, Q_min(1.0f, (float)aimTolerance));
				m_aimDiagConeErrorDeg = Q_acos(dotC) * (180.0f / float(M_PI));
				m_aimDiagConeToleranceDeg = Q_acos(tolC) * (180.0f / float(M_PI));
				const float tolDeg = m_aimDiagConeToleranceDeg;
				m_aimDiagConeRatio = (tolDeg > 0.001f) ? (m_aimDiagConeErrorDeg / tolDeg) : 0.0f;
			}
			m_aimDiagConePass = (onTarget > aimTolerance) ? 1 : 0;

			if (onTarget > aimTolerance)
			{
				bool doAttack;

				// if friendly fire is on, don't fire if a teammate is blocking our line of fire
				if (TheCSBots()->AllowFriendlyFireDamage())
				{
					if (IsFriendInLineOfFire())
						doAttack = false;
					else
						doAttack = true;
				}
				else
				{
					// fire freely
					doAttack = true;
				}

				if (doAttack)
				{
					// if we are using a knife, only swing it if we're close
					if (IsUsingKnife())
					{
						const float knifeRange = 75.0f; // 50.0f
						if (rangeToEnemy < knifeRange)
						{
							// since we've given ourselves away - run!
							ForceRun(5.0f);

							// if our prey is facing away, backstab him!
							if (!IsPlayerFacingMe(pEnemy))
							{
								SecondaryAttack();
								m_aimDiagDidFire = 1;
							}
							else
							{
								// randomly choose primary and secondary attacks with knife
								const float knifeStabChance = 33.3f;
								if (RANDOM_FLOAT(0, 100) < knifeStabChance)
									SecondaryAttack();
								else
									PrimaryAttack();
								m_aimDiagDidFire = 1;
							}
						}
					}
					else
					{
						const bool boltSniperShot = IsBoltActionSniper(GetActiveWeapon());
						PrimaryAttack();
						m_aimDiagDidFire = 1;
						if (boltSniperShot)
							StartBoltSniperQuickSwitch();
					}
				}

				if (IsUsingPistol())
				{
					// high-skill bots fire their pistols quickly at close range
					const float closePistolRange = 999999.9f;
					if (TheCSBots()->GetEffectiveSkill(this) > 0.75f && rangeToEnemy < closePistolRange)
					{
						StartRapidFire();

						// fire as fast as possible
						m_fireWeaponTimestamp = 0.0f;
					}
					else
					{
						// fire somewhat quickly
						m_fireWeaponTimestamp = RANDOM_FLOAT(0.15f, 0.4f);
					}
				}
				// not using a pistol
				else
				{
					if (IsUsingSniperRifle())
					{
						// Snipers should click once, recover, then correct aim instead of holding fire.
						m_fireWeaponTimestamp = RANDOM_FLOAT(0.70f, 1.25f);
					}
					else
					{
						if (ShouldHoldFullAuto(this, rangeToEnemy))
						{
							m_fireWeaponTimestamp = 0.0f;
						}
						else
						{
							const float distantTargetRange = 800.0f;
							if (rangeToEnemy > distantTargetRange)
							{
								// if very far away, fire slowly for better accuracy
								m_fireWeaponTimestamp = RANDOM_FLOAT(0.3f, 0.7f);
							}
							else
							{
								// fire short bursts for accuracy
								m_fireWeaponTimestamp = RANDOM_FLOAT(0.15f, 0.5f); // 0.15f, 0.25f
							}
						}
					}
				}

				// subtract system latency
				m_fireWeaponTimestamp -= g_flBotFullThinkInterval;
				m_fireWeaponTimestamp += gpGlobals->time;
			}
			else
			{
				m_aimDiagRefusedAimCone = true;
			}
		}
	}
}

static bool ShouldHoldFullAuto(CCSBot *bot, float rangeToEnemy)
{
	if (!bot)
		return false;

	if (bot->IsUsingSniperRifle() || bot->IsUsingPistol() || bot->IsUsingShotgun() || bot->IsUsingGrenade() || bot->IsUsingKnife())
		return false;

	if (bot->IsUsingMachinegun())
		return true;

	const float skill = TheCSBots()->GetEffectiveSkill(bot);
	const bool multipleEnemies = bot->GetNearbyEnemyCount() >= 2;
	const bool recentlyHurt = bot->GetTimeSinceAttacked() < 0.75f;
	const bool recoilAlreadyHigh = bot->IsActiveWeaponRecoilHigh();

	// Humans often commit to a held spray when the fight is close, crowded, or panicky.
	float sprayRange = 520.0f + (1.0f - skill) * 180.0f;
	if (multipleEnemies)
		sprayRange += 220.0f;
	if (recentlyHurt)
		sprayRange += 260.0f;
	if (recoilAlreadyHigh)
		sprayRange += 140.0f;

	if (rangeToEnemy < sprayRange)
		return true;

	if (multipleEnemies && rangeToEnemy < 950.0f && RANDOM_FLOAT(0.0f, 100.0f) < 58.0f - skill * 18.0f)
		return true;

	if (recentlyHurt && rangeToEnemy < 1050.0f && RANDOM_FLOAT(0.0f, 100.0f) < 70.0f - skill * 22.0f)
		return true;

	return false;
}

// Set the current aim offset using given accuracy (1.0 = perfect aim, 0.0f = terrible aim)
void CCSBot::SetAimOffset(float accuracy)
{
	// if our accuracy is less than perfect, it will improve as we "focus in" while not rotating our view
	if (accuracy < 1.0f)
	{
		// if we moved our view, reset our "focus" mechanism
		if (IsViewMoving(100.0f))
		{
			m_aimSpreadTimestamp = gpGlobals->time;
		}

		// focusTime is the time it takes for a bot to "focus in" for very good aim, from 2 to 5 seconds
		const float focusTime = Q_max(5.0f * (1.0f - accuracy), 2.0f);

		float focusInterval = gpGlobals->time - m_aimSpreadTimestamp;
		float focusAccuracy = focusInterval / focusTime;

		// limit how much "focus" will help
		const float maxFocusAccuracy = 0.75f;

		if (focusAccuracy > maxFocusAccuracy)
			focusAccuracy = maxFocusAccuracy;

		accuracy = Q_max(accuracy, focusAccuracy);
	}

	PrintIfWatched("Accuracy = %4.3f\n", accuracy);

	float range = (m_lastEnemyPosition - pev->origin).Length();
	const real_t maxOffset = range * (real_t(m_iFOV) / DEFAULT_FOV) * 0.1;
	float error = maxOffset * (1 - accuracy);

	m_aimOffsetGoal[0] = RANDOM_FLOAT(-error, error);
	m_aimOffsetGoal[1] = RANDOM_FLOAT(-error, error);
	m_aimOffsetGoal[2] = RANDOM_FLOAT(-error, error);

	// define time when aim offset will automatically be updated
	m_aimOffsetTimestamp = gpGlobals->time + RANDOM_FLOAT(0.25f, 1.0f);
}

// Wiggle aim error based on GetProfile()->GetSkill()
void CCSBot::UpdateAimOffset()
{
	if (gpGlobals->time >= m_aimOffsetTimestamp)
	{
		float accuracy = TheCSBots()->GetEffectiveSkill(this);
		const float acquireAge = gpGlobals->time - m_currentEnemyAcquireTimestamp;

		if (acquireAge < 0.45f)
			accuracy *= 0.55f + 0.45f * (acquireAge / 0.45f);

		if (!IsEnemyVisible())
			accuracy *= 0.70f;

		if (IsViewMoving(120.0f))
			accuracy *= 0.85f;

		if (IsActiveWeaponRecoilHigh() && !IsUsingPistol() && !IsUsingSniperRifle())
			accuracy *= 0.75f;

		SetAimOffset(Q_max(0.05f, Q_min(accuracy, 1.0f)));
	}

	// move current offset towards goal offset
	Vector d = m_aimOffsetGoal - m_aimOffset;
	const float stiffness = 0.1f;

	m_aimOffset.x += stiffness * d.x;
	m_aimOffset.y += stiffness * d.y;
	m_aimOffset.z += stiffness * d.z;
}

// Change our zoom level to be appropriate for the given range.
// Return true if the zoom level changed.
bool CCSBot::AdjustZoom(float range)
{
	bool adjustZoom = false;

	if (IsUsingSniperRifle())
	{
		// NOTE: This must be less than sniperMinRange in AttackState
		const float sniperZoomRange = 300.0f; //150.0f
		const float sniperFarZoomRange = 1500.0f;

		// if range is too close, don't zoom
		if (range <= sniperZoomRange)
		{
			// zoom out
			if (GetZoomLevel() != NO_ZOOM)
			{
				adjustZoom = true;
			}
		}
		else if (range < sniperFarZoomRange)
		{
			// maintain low zoom
			if (GetZoomLevel() != LOW_ZOOM)
			{
				adjustZoom = true;
			}
		}
		else
		{
			// maintain high zoom
			if (GetZoomLevel() != HIGH_ZOOM)
			{
				adjustZoom = true;
			}
		}
	}
	else
	{
		// zoom out
		if (GetZoomLevel() != NO_ZOOM)
		{
			adjustZoom = true;
		}
	}

	if (adjustZoom)
	{
		SecondaryAttack();
	}

	return adjustZoom;
}

// Return true if the given weapon is a sniper rifle
bool isSniperRifle(CBasePlayerItem *item)
{
	switch (item->m_iId)
	{
	case WEAPON_SCOUT:
	case WEAPON_SG550:
	case WEAPON_AWP:
	case WEAPON_G3SG1:
		return true;

	default:
		return false;
	}
}

bool CCSBot::IsUsingAWP() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (pCurrentWeapon && pCurrentWeapon->m_iId == WEAPON_AWP)
		return true;

	return false;
}

static bool IsBoltActionSniper(CBasePlayerWeapon *weapon)
{
	return weapon && (weapon->m_iId == WEAPON_AWP || weapon->m_iId == WEAPON_SCOUT);
}

void CCSBot::StartBoltSniperQuickSwitch()
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (!IsBoltActionSniper(pCurrentWeapon))
		return;

	CBasePlayerWeapon *pSwitchWeapon[3];
	int switchWeaponCount = 0;

	CBasePlayerWeapon *pPistol = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PISTOL_SLOT]);
	if (pPistol && HasAnyAmmo(pPistol))
		pSwitchWeapon[switchWeaponCount++] = pPistol;

	CBasePlayerWeapon *pKnife = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[KNIFE_SLOT]);
	if (pKnife)
		pSwitchWeapon[switchWeaponCount++] = pKnife;

	CBasePlayerWeapon *pGrenade = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[GRENADE_SLOT]);
	if (pGrenade && HasAnyAmmo(pGrenade))
		pSwitchWeapon[switchWeaponCount++] = pGrenade;

	if (switchWeaponCount <= 0)
		return;

	m_boltSniperQuickSwitchState = 1;
	m_boltSniperQuickSwitchAwayTimestamp = gpGlobals->time + Q_max(g_flBotFullThinkInterval, 0.03f);
	m_boltSniperQuickSwitchReturnTimestamp = m_boltSniperQuickSwitchAwayTimestamp + RANDOM_FLOAT(0.11f, 0.18f);
}

bool CCSBot::UpdateBoltSniperQuickSwitch()
{
	if (m_boltSniperQuickSwitchState <= 0)
		return false;

	CBasePlayerWeapon *pPrimary = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]);
	if (!IsBoltActionSniper(pPrimary))
	{
		m_boltSniperQuickSwitchState = 0;
		return false;
	}

	if (m_boltSniperQuickSwitchState == 1)
	{
		if (gpGlobals->time < m_boltSniperQuickSwitchAwayTimestamp)
			return true;

		CBasePlayerWeapon *pSwitchWeapon[3];
		int switchWeaponCount = 0;

		CBasePlayerWeapon *pPistol = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PISTOL_SLOT]);
		if (pPistol && HasAnyAmmo(pPistol))
			pSwitchWeapon[switchWeaponCount++] = pPistol;

		CBasePlayerWeapon *pKnife = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[KNIFE_SLOT]);
		if (pKnife)
			pSwitchWeapon[switchWeaponCount++] = pKnife;

		CBasePlayerWeapon *pGrenade = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[GRENADE_SLOT]);
		if (pGrenade && HasAnyAmmo(pGrenade))
			pSwitchWeapon[switchWeaponCount++] = pGrenade;

		if (switchWeaponCount <= 0)
		{
			m_boltSniperQuickSwitchState = 0;
			return false;
		}

		CBasePlayerWeapon *pSelected = pSwitchWeapon[RANDOM_LONG(0, switchWeaponCount - 1)];
		SelectItem(STRING(pSelected->pev->classname));
		m_boltSniperQuickSwitchState = 2;
		return true;
	}

	if (gpGlobals->time >= m_boltSniperQuickSwitchReturnTimestamp)
	{
		SelectItem(STRING(pPrimary->pev->classname));
		m_equipTimer.Start();
		m_boltSniperQuickSwitchState = 0;
		return true;
	}

	return true;
}

// Returns true if we are using a weapon with a removable silencer
bool CCSBot::DoesActiveWeaponHaveSilencer() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (!pCurrentWeapon)
		return false;

	if (pCurrentWeapon->m_iId == WEAPON_M4A1 || pCurrentWeapon->m_iId == WEAPON_USP)
		return true;

	return false;
}

// Return true if we are using a sniper rifle
bool CCSBot::IsUsingSniperRifle() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (pCurrentWeapon && isSniperRifle(pCurrentWeapon))
		return true;

	return false;
}

// Return true if we have a sniper rifle in our inventory
bool CCSBot::IsSniper() const
{
	auto sniperItem = this->ForEachItem([](CBasePlayerItem *pItem) {
		return isSniperRifle(pItem);
	});

	return sniperItem ? true : false;
}

// Return true if we are actively sniping (moving to sniper spot or settled in)
bool CCSBot::IsSniping() const
{
	if (GetTask() == MOVE_TO_SNIPER_SPOT || GetTask() == SNIPING)
		return true;

	return false;
}

// Return true if we are using a shotgun
bool CCSBot::IsUsingShotgun() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (!pCurrentWeapon)
		return false;

	if (pCurrentWeapon->m_iId == WEAPON_XM1014 || pCurrentWeapon->m_iId == WEAPON_M3)
		return true;

	return false;
}

// Returns true if using the big 'ol machinegun
bool CCSBot::IsUsingMachinegun() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (pCurrentWeapon && pCurrentWeapon->m_iId == WEAPON_M249)
		return true;

	return false;
}

// Return true if primary weapon doesn't exist or is totally out of ammo
bool CCSBot::IsPrimaryWeaponEmpty() const
{
	CBasePlayerWeapon *pCurrentWeapon = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]);
	if (!pCurrentWeapon)
		return true;

	// check if gun has any ammo left
	if (HasAnyAmmo(pCurrentWeapon))
		return false;

	return true;
}

// Return true if pistol doesn't exist or is totally out of ammo
bool CCSBot::IsPistolEmpty() const
{
	CBasePlayerWeapon *pCurrentWeapon = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PISTOL_SLOT]);
	if (!pCurrentWeapon)
		return true;

	// check if gun has any ammo left
	if (HasAnyAmmo(pCurrentWeapon))
	{
		return false;
	}

	return true;
}

// Equip the given item
bool CCSBot::DoEquip(CBasePlayerWeapon *pWeapon)
{
	if (!pWeapon)
		return false;

#ifdef REGAMEDLL_FIXES
	if (TheCSBots()->GetEffectiveSkill(this) > 0.4f && pev->waterlevel == 3 && (pWeapon->iFlags() & ITEM_FLAG_NOFIREUNDERWATER))
		return false;
#endif

	// check if weapon has any ammo left
	if (!HasAnyAmmo(pWeapon))
		return false;

	// equip it
	SelectItem(STRING(pWeapon->pev->classname));
	m_equipTimer.Start();

	return true;
}

// throttle how often equipping is allowed
const float minEquipInterval = 5.0f;

// Equip the best weapon we are carrying that has ammo
void CCSBot::EquipBestWeapon(bool mustEquip)
{
	// throttle how often equipping is allowed
	if (!mustEquip && m_equipTimer.GetElapsedTime() < minEquipInterval)
		return;

	CBasePlayerWeapon *pPrimary = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]);
	if (pPrimary)
	{
		WeaponClassType weaponClass = WeaponIDToWeaponClass(pPrimary->m_iId);

		if ((TheCSBots()->AllowShotguns() && weaponClass == WEAPONCLASS_SHOTGUN)
			|| (TheCSBots()->AllowMachineGuns() && weaponClass == WEAPONCLASS_MACHINEGUN)
			|| (TheCSBots()->AllowRifles() && weaponClass == WEAPONCLASS_RIFLE)
			|| (TheCSBots()->AllowSnipers() && weaponClass == WEAPONCLASS_SNIPERRIFLE)
			|| (TheCSBots()->AllowSubMachineGuns() && weaponClass == WEAPONCLASS_SUBMACHINEGUN)
			|| (TheCSBots()->AllowTacticalShield() && pPrimary->m_iId == WEAPON_SHIELDGUN))
		{
			if (DoEquip(pPrimary))
				return;
		}
	}

	if (TheCSBots()->AllowPistols())
	{
		if (DoEquip(static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PISTOL_SLOT])))
			return;
	}

	// always have a knife
	EquipKnife();
}

// Equip our pistol
void CCSBot::EquipPistol(bool mustEquip)
{
	// throttle how often equipping is allowed
	if (!mustEquip && m_equipTimer.GetElapsedTime() < minEquipInterval)
		return;

	if (TheCSBots()->AllowPistols() && !IsUsingPistol())
	{
		CBasePlayerWeapon *pistol = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PISTOL_SLOT]);
		DoEquip(pistol);
	}
}

// Equip the knife
void CCSBot::EquipKnife()
{
	if (!IsUsingKnife())
	{
		CBasePlayerWeapon *pKnife = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[KNIFE_SLOT]);
		if (pKnife)
		{
			SelectItem(STRING(pKnife->pev->classname));
		}
	}
}

// Return true if we have a grenade in our inventory
bool CCSBot::HasGrenade() const
{
	for (CBasePlayerItem *item = m_rgpPlayerItems[GRENADE_SLOT]; item; item = item->m_pNext)
	{
		CBasePlayerWeapon *w = static_cast<CBasePlayerWeapon *>(item);
		if (w && HasAnyAmmo(w))
			return true;
	}

	return false;
}

bool CCSBot::HasGrenadeKind(int weaponId) const
{
	for (CBasePlayerItem *item = m_rgpPlayerItems[GRENADE_SLOT]; item; item = item->m_pNext)
	{
		CBasePlayerWeapon *w = static_cast<CBasePlayerWeapon *>(item);
		if (w && w->m_iId == weaponId && HasAnyAmmo(w))
			return true;
	}

	return false;
}

bool CCSBot::EquipGrenadeKind(int weaponId)
{
	if (IsSniper())
		return false;

	if (IsUsingGrenade())
	{
		CBasePlayerWeapon *cur = GetActiveWeapon();
		if (cur && cur->m_iId == weaponId)
			return true;
	}

	for (CBasePlayerItem *item = m_rgpPlayerItems[GRENADE_SLOT]; item; item = item->m_pNext)
	{
		CBasePlayerWeapon *w = static_cast<CBasePlayerWeapon *>(item);
		if (w && w->m_iId == weaponId && HasAnyAmmo(w))
		{
			SelectItem(STRING(w->pev->classname));
			return true;
		}
	}

	return false;
}

// Equip a grenade, return false if we cant
bool CCSBot::EquipGrenade(bool noSmoke)
{
	// snipers don't use grenades
	if (IsSniper())
		return false;

	if (IsUsingGrenade())
		return true;

	for (CBasePlayerItem *item = m_rgpPlayerItems[GRENADE_SLOT]; item; item = item->m_pNext)
	{
		CBasePlayerWeapon *pGrenade = static_cast<CBasePlayerWeapon *>(item);
		if (!pGrenade || !HasAnyAmmo(pGrenade))
			continue;

		if (noSmoke && pGrenade->m_iId == WEAPON_SMOKEGRENADE)
			continue;

		SelectItem(STRING(pGrenade->pev->classname));
		return true;
	}

	return false;
}

// Returns true if we have knife equipped
bool CCSBot::IsUsingKnife() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (pCurrentWeapon && pCurrentWeapon->m_iId == WEAPON_KNIFE)
		return true;

	return false;
}

// Returns true if we have pistol equipped
bool CCSBot::IsUsingPistol() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (pCurrentWeapon && pCurrentWeapon->IsPistol())
		return true;

	return false;
}

// Returns true if we have a grenade equipped
bool CCSBot::IsUsingGrenade() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();

	if (!pCurrentWeapon)
		return false;

	if (pCurrentWeapon->m_iId == WEAPON_SMOKEGRENADE
		|| pCurrentWeapon->m_iId == WEAPON_FLASHBANG
		|| pCurrentWeapon->m_iId == WEAPON_HEGRENADE)
		return true;

	return false;
}

bool CCSBot::IsUsingHEGrenade() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	if (pCurrentWeapon && pCurrentWeapon->m_iId == WEAPON_HEGRENADE)
		return true;

	return false;
}

bool CCSBot::IsUsingFlashbang() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	return pCurrentWeapon && pCurrentWeapon->m_iId == WEAPON_FLASHBANG;
}

bool CCSBot::IsUsingSmokeGrenade() const
{
	CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
	return pCurrentWeapon && pCurrentWeapon->m_iId == WEAPON_SMOKEGRENADE;
}

bool CCSBot::WouldGrenadeAimRiskTeammates(const Vector &aimPoint, int grenadeWeaponId) const
{
	if (m_iTeam != CT && m_iTeam != TERRORIST)
		return false;

	float sphereR = 300.0f;
	if (grenadeWeaponId == WEAPON_FLASHBANG)
		sphereR = 420.0f;
	else if (grenadeWeaponId == WEAPON_HEGRENADE)
		sphereR = 380.0f;
	else if (grenadeWeaponId == WEAPON_SMOKEGRENADE)
		sphereR = 280.0f;
	else
		return false;

	CBaseEntity *pEntity = nullptr;
	while ((pEntity = UTIL_FindEntityInSphere(pEntity, aimPoint, sphereR)))
	{
		if (!pEntity->IsPlayer())
			continue;

		CBasePlayer *pl = static_cast<CBasePlayer *>(pEntity);
		if (!pl->IsAlive() || pl == this)
			continue;
		if (pl->m_iTeam == m_iTeam && (pl->m_iTeam == CT || pl->m_iTeam == TERRORIST))
			return true;
	}

	const Vector eye = GetGunPosition();
	TraceResult tr;
	UTIL_TraceLine(eye, aimPoint, dont_ignore_monsters, ignore_glass, ENT(pev), &tr);
	if (grenadeWeaponId == WEAPON_HEGRENADE && tr.flFraction < 1.0f)
	{
		const float missDist = (tr.vecEndPos - aimPoint).Length();
		if (tr.flFraction < 0.92f && missDist > 96.0f)
			return true;
	}
	if (tr.flFraction < 1.0f && tr.pHit)
	{
		CBaseEntity *hit = CBaseEntity::Instance(tr.pHit);
		if (hit && hit->IsPlayer())
		{
			CBasePlayer *pl = static_cast<CBasePlayer *>(hit);
			if (pl != this && pl->IsAlive() && pl->m_iTeam == m_iTeam && (pl->m_iTeam == CT || pl->m_iTeam == TERRORIST))
				return true;
		}
	}

	const Vector vForward = (aimPoint - eye);
	const float throwLen = vForward.Length();
	if (throwLen > 8.0f)
	{
		const float sampleRadius = 80.0f;
		const int samples = (grenadeWeaponId == WEAPON_FLASHBANG) ? 5 : 4;
		const Vector dir = vForward * (1.0f / throwLen);
		for (int s = 1; s <= samples; s++)
		{
			const Vector sample = eye + dir * (throwLen * (float)s / (float)(samples + 1));
			pEntity = nullptr;
			while ((pEntity = UTIL_FindEntityInSphere(pEntity, sample, sampleRadius)))
			{
				if (!pEntity->IsPlayer())
					continue;
				CBasePlayer *pl = static_cast<CBasePlayer *>(pEntity);
				if (!pl->IsAlive() || pl == this)
					continue;
				if (pl->m_iTeam == m_iTeam && (pl->m_iTeam == CT || pl->m_iTeam == TERRORIST))
					return true;
			}
		}
	}

	return false;
}

// Begin the process of throwing the grenade
void CCSBot::ThrowGrenade(const Vector *target)
{
	if (IsUsingGrenade() && !m_isWaitingToTossGrenade)
	{
		const float angleTolerance = 3.0f;

		SetLookAt("GrenadeThrow", target, PRIORITY_UNINTERRUPTABLE, 2.0f, false, angleTolerance);

		m_isWaitingToTossGrenade = true;
		m_grenadePinPulled = false;
		m_grenadeReleaseSent = false;
		m_grenadeThrowLogged = false;
		m_tossGrenadeTimer.Start(2.0f);
	}
}

bool CCSBot::FindFlashbangPeekTarget(Vector *pos)
{
	if (!pos || !HasPath())
		return false;

	int i;
	for (i = m_pathIndex; i < m_pathLength; i++)
	{
		if (!FVisible(m_path[i].pos + Vector(0, 0, HalfHumanHeight)))
			break;
	}

	if (i == m_pathIndex)
		return false;

	Vector dir = m_path[i].pos - m_path[i - 1].pos;
	float length = dir.NormalizeInPlace();

	const float inc = 25.0f;
	Vector p;
	Vector visibleSpot = m_path[i - 1].pos;
	for (float t = 0.0f; t < length; t += inc)
	{
		p = m_path[i - 1].pos + t * dir;
		p.z += HalfHumanHeight;

		if (!FVisible(p))
			break;

		visibleSpot = p;
	}

	visibleSpot.z += 10.0f;

	Vector blindPush = m_path[i].pos - visibleSpot;
	blindPush.z = 0.0f;
	float blindLen = blindPush.NormalizeInPlace();
	if (blindLen > 8.0f)
	{
		const float push = Q_min(150.0f, blindLen * 0.45f);
		visibleSpot = visibleSpot + blindPush * push;
	}

	visibleSpot.z += 8.0f;
	*pos = visibleSpot;
	return true;
}

bool CCSBot::TryTacticalGrenadeThrow()
{
	if (!TheCSBots()->AllowGrenades() || m_isWaitingToTossGrenade || IsUsingGrenade())
		return false;

	if (!HasPath() || IsBuying() || IsEscapingFromBomb())
		return false;

	if (IsSniper() && !HasGrenadeKind(WEAPON_HEGRENADE) && !HasGrenadeKind(WEAPON_SMOKEGRENADE) && !HasGrenadeKind(WEAPON_FLASHBANG))
		return false;

	const float skill = TheCSBots()->GetEffectiveSkill(this);
	const bool bombMap = (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB);
	Vector target;
	BotGrenadeSpots::FoundSpotInfo spotInfo;
	const bool hasManualSmokePlan = BotGrenadeSpots::HasAnyLineupForGrenadeAndTeam(WEAPON_SMOKEGRENADE, m_iTeam);
	
	// Check for manual HE grenade spots first with higher priority (less random gating)
	if (HasGrenadeKind(WEAPON_HEGRENADE) && BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_HEGRENADE, &target, &spotInfo))
	{
		if (!WouldGrenadeAimRiskTeammates(target, WEAPON_HEGRENADE) && EquipGrenadeKind(WEAPON_HEGRENADE))
		{
			m_grenadeThrowTarget = target;
			m_grenadeUsedManualSpot = true;
			m_grenadeManualSpotDistance = spotInfo.distanceFromSpot;
			m_grenadeManualSpotIndex = spotInfo.spotIndex;
			Q_strlcpy(m_grenadeManualSpotName, spotInfo.spotName, sizeof(m_grenadeManualSpotName));
			ThrowGrenade(&target);
			return true;
		}
	}

	if (bombMap && HasGrenadeKind(WEAPON_SMOKEGRENADE) && BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_SMOKEGRENADE, &target, &spotInfo))
	{
		if (!WouldGrenadeAimRiskTeammates(target, WEAPON_SMOKEGRENADE) && EquipGrenadeKind(WEAPON_SMOKEGRENADE))
		{
			m_grenadeThrowTarget = target;
			m_grenadeUsedManualSpot = true;
			m_grenadeManualSpotDistance = spotInfo.distanceFromSpot;
			m_grenadeManualSpotIndex = spotInfo.spotIndex;
			Q_strlcpy(m_grenadeManualSpotName, spotInfo.spotName, sizeof(m_grenadeManualSpotName));
			ThrowGrenade(&target);
			return true;
		}
	}

	if (bot_grenade_debug_he_only.value > 0.0f)
		return false;

	if (HasGrenadeKind(WEAPON_FLASHBANG) && BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_FLASHBANG, &target, &spotInfo))
	{
		if (!WouldGrenadeAimRiskTeammates(target, WEAPON_FLASHBANG) && EquipGrenadeKind(WEAPON_FLASHBANG))
		{
			m_grenadeThrowTarget = target;
			m_grenadeUsedManualSpot = true;
			m_grenadeManualSpotDistance = spotInfo.distanceFromSpot;
			m_grenadeManualSpotIndex = spotInfo.spotIndex;
			Q_strlcpy(m_grenadeManualSpotName, spotInfo.spotName, sizeof(m_grenadeManualSpotName));
			ThrowGrenade(&target);
			return true;
		}
	}
	
	// Standard random skill check for other grenades
	if (RANDOM_FLOAT(0.0f, 100.0f) > 32.0f + 60.0f * skill)
		return false;

	// --- Smoke: manual lineups (.grenade.json) then site execute / retake ---
	if (bombMap && HasGrenadeKind(WEAPON_SMOKEGRENADE))
	{
		if (BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_SMOKEGRENADE, &target, &spotInfo))
		{
			if (!WouldGrenadeAimRiskTeammates(target, WEAPON_SMOKEGRENADE) && EquipGrenadeKind(WEAPON_SMOKEGRENADE))
			{
				m_grenadeThrowTarget = target;
				m_grenadeUsedManualSpot = true;
				m_grenadeManualSpotDistance = spotInfo.distanceFromSpot;
				m_grenadeManualSpotIndex = spotInfo.spotIndex;
				Q_strlcpy(m_grenadeManualSpotName, spotInfo.spotName, sizeof(m_grenadeManualSpotName));
				ThrowGrenade(&target);
				return true;
			}
		}
		if (hasManualSmokePlan)
			return false;
		if (m_iTeam == CT && GetGameState()->IsBombPlanted() && GetGameState()->GetBombPosition())
		{
			const Vector bomb = *GetGameState()->GetBombPosition();
			const float dist = (pev->origin - bomb).Length2D();
			const float pathRem = GetPathDistanceRemaining();

			if (dist > 175.0f && dist < 1320.0f && pathRem > 60.0f && pathRem < 520.0f && !IsDefusingBomb())
			{
				target = bomb + Vector(0, 0, 10.0f);
				if (!WouldGrenadeAimRiskTeammates(target, WEAPON_SMOKEGRENADE) && EquipGrenadeKind(WEAPON_SMOKEGRENADE))
				{
					m_grenadeThrowTarget = target;
					m_grenadeUsedManualSpot = false;
					ThrowGrenade(&target);
					return true;
				}
			}
		}
		else if (m_iTeam == TERRORIST && (GetTask() == PLANT_BOMB || IsCarryingBomb()))
		{
			const float pathRem = GetPathDistanceRemaining();
			if (pathRem > 85.0f && pathRem < 820.0f)
			{
				int z = HasCommittedBombsite() ? GetCommittedBombsite() : TheCSBots()->GetDirectorRecommendedBombsite(this);
				if (z >= 0 && z < TheCSBots()->GetZoneCount())
				{
					const CCSBotManager::Zone *zone = TheCSBots()->GetZone(z);
					const Vector *sitePos = zone ? TheCSBots()->GetRandomPositionInZone(zone) : nullptr;
					if (sitePos)
					{
						target = *sitePos + Vector(0, 0, 12.0f);
						if (!WouldGrenadeAimRiskTeammates(target, WEAPON_SMOKEGRENADE) && EquipGrenadeKind(WEAPON_SMOKEGRENADE))
						{
							m_grenadeThrowTarget = target;
							m_grenadeUsedManualSpot = false;
							ThrowGrenade(&target);
							return true;
						}
					}
				}
			}
		}
	}

	// --- Flash: manual spots, then corner peek with threat cue ---
	if (HasGrenadeKind(WEAPON_FLASHBANG))
	{
		if (BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_FLASHBANG, &target, &spotInfo))
		{
			if (!WouldGrenadeAimRiskTeammates(target, WEAPON_FLASHBANG) && EquipGrenadeKind(WEAPON_FLASHBANG))
			{
				m_grenadeThrowTarget = target;
				m_grenadeUsedManualSpot = true;
				m_grenadeManualSpotDistance = spotInfo.distanceFromSpot;
				m_grenadeManualSpotIndex = spotInfo.spotIndex;
				Q_strlcpy(m_grenadeManualSpotName, spotInfo.spotName, sizeof(m_grenadeManualSpotName));
				ThrowGrenade(&target);
				return true;
			}
		}

		const bool heardFight = IsNoiseHeard() && GetNoisePriority() >= PRIORITY_MEDIUM;
		const bool lostCloseEnemy = !IsEnemyVisible() && GetTimeSinceLastSawEnemy() < 2.0f;

		if ((heardFight || lostCloseEnemy) && FindFlashbangPeekTarget(&target))
		{
			if (!WouldGrenadeAimRiskTeammates(target, WEAPON_FLASHBANG) && EquipGrenadeKind(WEAPON_FLASHBANG))
			{
				m_grenadeThrowTarget = target;
				m_grenadeUsedManualSpot = false;
				ThrowGrenade(&target);
				return true;
			}
		}
	}

	// --- HE: danger memory fallback (.danger.json) ---
	if (HasGrenadeKind(WEAPON_HEGRENADE))
	{
		if (bot_grenade_he_use_danger_memory.value > 0.0f
		    && BotGrenadeSpots::ShouldAttemptDangerHeFallback(this)
		    && BotDangerMemory::FindBestHeGrenadeTargetFromMemory(this, &target))
		{
			if (!WouldGrenadeAimRiskTeammates(target, WEAPON_HEGRENADE) && EquipGrenadeKind(WEAPON_HEGRENADE))
			{
				m_grenadeThrowTarget = target;
				m_grenadeUsedManualSpot = false;
				ThrowGrenade(&target);
				return true;
			}
		}
	}

	return false;
}

bool CCSBot::TryManualHEGrenadeThrow()
{
	if (!TheCSBots()->AllowGrenades() || m_isWaitingToTossGrenade || IsUsingGrenade())
		return false;

	if (!HasPath() || IsBuying() || IsEscapingFromBomb() || !HasGrenadeKind(WEAPON_HEGRENADE))
		return false;

	Vector target;
	BotGrenadeSpots::FoundSpotInfo spotInfo;
	if (!BotGrenadeSpots::FindManualThrowTarget(this, WEAPON_HEGRENADE, &target, &spotInfo))
		return false;

	if (WouldGrenadeAimRiskTeammates(target, WEAPON_HEGRENADE) || !EquipGrenadeKind(WEAPON_HEGRENADE))
		return false;

	m_grenadeThrowTarget = target;
	m_grenadeUsedManualSpot = true;
	m_grenadeManualSpotDistance = spotInfo.distanceFromSpot;
	m_grenadeManualSpotIndex = spotInfo.spotIndex;
	Q_strlcpy(m_grenadeManualSpotName, spotInfo.spotName, sizeof(m_grenadeManualSpotName));
	ThrowGrenade(&target);
	return true;
}

// Find spot to throw grenade ahead of us and "around the corner" along our path
bool CCSBot::FindGrenadeTossPathTarget(Vector *pos)
{
	if (!HasPath())
		return false;

	// find farthest point we can see on the path
	int i;
	for (i = m_pathIndex; i < m_pathLength; i++)
	{
		if (!FVisible(m_path[i].pos + Vector(0, 0, HalfHumanHeight)))
			break;
	}

	if (i == m_pathIndex)
		return false;

	// find exact spot where we lose sight
	Vector dir = m_path[i].pos - m_path[i - 1].pos;
	float length = dir.NormalizeInPlace();

	const float inc = 25.0f;
	Vector p;
	Vector visibleSpot = m_path[i - 1].pos;
	for (float t = 0.0f; t < length; t += inc)
	{
		p = m_path[i - 1].pos + t * dir;
		p.z += HalfHumanHeight;

		if (!FVisible(p))
			break;

		visibleSpot = p;
	}

	// massage the location a bit
	visibleSpot.z += 10.0f;

	const float bufferRange = 50.0f;
	TraceResult result;
	Vector check;

	// check +X
	check = visibleSpot + Vector(999.9f, 0, 0);
	UTIL_TraceLine(visibleSpot, check, dont_ignore_monsters, ignore_glass, ENT(pev), &result);

	if (result.flFraction < 1.0f)
	{
		float range = result.vecEndPos.x - visibleSpot.x;
		if (range < bufferRange)
		{
			visibleSpot.x = result.vecEndPos.x - bufferRange;
		}
	}

	// check -X
	check = visibleSpot + Vector(-999.9f, 0, 0);
	UTIL_TraceLine(visibleSpot, check, dont_ignore_monsters, ignore_glass, ENT(pev), &result);

	if (result.flFraction < 1.0f)
	{
		float range = visibleSpot.x - result.vecEndPos.x;
		if (range < bufferRange)
		{
			visibleSpot.x = result.vecEndPos.x + bufferRange;
		}
	}

	// check +Y
	check = visibleSpot + Vector(0, 999.9f, 0);
	UTIL_TraceLine(visibleSpot, check, dont_ignore_monsters, ignore_glass, ENT(pev), &result);

	if (result.flFraction < 1.0f)
	{
		float range = result.vecEndPos.y - visibleSpot.y;
		if (range < bufferRange)
		{
			visibleSpot.y = result.vecEndPos.y - bufferRange;
		}
	}

	// check -Y
	check = visibleSpot + Vector(0, -999.9f, 0);
	UTIL_TraceLine(visibleSpot, check, dont_ignore_monsters, ignore_glass, ENT(pev), &result);

	if (result.flFraction < 1.0f)
	{
		float range = visibleSpot.y - result.vecEndPos.y;
		if (range < bufferRange)
		{
			visibleSpot.y = result.vecEndPos.y + bufferRange;
		}
	}

	*pos = visibleSpot;
	return true;
}

// Reload our weapon if we must
void CCSBot::ReloadCheck()
{
	const float safeReloadWaitTime = 3.0f;
	const float reloadAmmoRatio = 0.6f;

	// don't bother to reload if there are no enemies left
	if (GetEnemiesRemaining() == 0)
		return;

	if (IsDefusingBomb())
		return;

	CBasePlayerWeapon *activeWeapon = GetActiveWeapon();
	const float nearestEnemyRange = GetRangeToNearestRecognizedEnemy();
	const bool closeFight = IsEnemyVisible() || GetNearbyEnemyCount() > 0 ||
		(nearestEnemyRange > 0.0f && nearestEnemyRange < 850.0f) ||
		GetTimeSinceLastSawEnemy() < 1.25f;
	const bool canUsePistol = activeWeapon && !activeWeapon->IsPistol() && !IsPistolEmpty();
	if (closeFight && canUsePistol &&
		(IsActiveWeaponReloading() || IsActiveWeaponClipEmpty() || GetActiveWeaponAmmoRatio() <= 0.22f))
	{
		EquipPistol(MUST_EQUIP);
		return;
	}

	if (IsActiveWeaponReloading())
		return;

	if (IsActiveWeaponClipEmpty())
	{
		// high-skill players switch to pistol instead of reloading during combat
		if (TheCSBots()->GetEffectiveSkill(this) > 0.5f && IsAttacking())
		{
			if (!GetActiveWeapon()->IsPistol() && !IsPistolEmpty())
			{
				// switch to pistol instead of reloading
				EquipPistol(MUST_EQUIP);
				return;
			}
		}
	}
	else if (GetTimeSinceLastSawEnemy() > safeReloadWaitTime && GetActiveWeaponAmmoRatio() <= reloadAmmoRatio)
	{
		// high-skill players use all their ammo and switch to pistol instead of reloading during combat
		if (TheCSBots()->GetEffectiveSkill(this) > 0.5f && IsAttacking())
			return;
	}
	else
	{
		// do not need to reload
		return;
	}

	// don't reload the AWP until it is totally out of ammo
	if (IsUsingAWP() && !IsActiveWeaponClipEmpty())
		return;

	Reload();

	// move to cover to reload if there are enemies nearby
	if (GetNearbyEnemyCount())
	{
		// avoid enemies while reloading (above 0.75 skill always hide to reload)
	const float hideChance = 25.0f + 100.0f * TheCSBots()->GetEffectiveSkill(this);

		if (!IsHiding() && RANDOM_FLOAT(0.0f, 100.0f) < hideChance)
		{
			const float safeTime = 5.0f;
			if (GetTimeSinceLastSawEnemy() < safeTime)
			{
				PrintIfWatched("Retreating to a safe spot to reload!\n");
				const Vector *spot = FindNearbyRetreatSpot(this, 1000.0f);
				if (spot)
				{
					// ignore enemies for a second to give us time to hide
					// reaching our hiding spot clears our disposition
					IgnoreEnemies(10.0f);

					Run();
					StandUp();
					Hide(spot, 0.0f);
				}
			}
		}
	}
}

// Silence/unsilence our weapon if we must
void CCSBot::SilencerCheck()
{
	// longer than reload check because reloading should take precedence
	const float safeSilencerWaitTime = 3.5f;

	if (IsDefusingBomb() || IsActiveWeaponReloading() || IsAttacking())
		return;

	// M4A1 and USP are the only weapons with removable silencers
	if (!DoesActiveWeaponHaveSilencer())
		return;

#ifdef REGAMEDLL_FIXES
	if (GetTimeSinceLastSawEnemy() < safeSilencerWaitTime)
		return;
#endif

	// don't touch the silencer if there are enemies nearby
	if (GetNearbyEnemyCount() == 0)
	{
		CBasePlayerWeapon *pCurrentWeapon = GetActiveWeapon();
		if (!pCurrentWeapon)
			return;

		bool isSilencerOn = (pCurrentWeapon->m_iWeaponState & (WPNSTATE_M4A1_SILENCED | WPNSTATE_USP_SILENCED)) != 0;

#ifndef REGAMEDLL_FIXES
		if (isSilencerOn != GetProfile()->PrefersSilencer() && !HasShield())
#else

		if (pCurrentWeapon->m_flNextSecondaryAttack >= gpGlobals->time)
			return;

		bool wantsSilencer = GetProfile()->PrefersSilencer();
		if (pCurrentWeapon->m_iId == WEAPON_M4A1 && TheCSBots()->GetEffectiveSkill(this) > 0.7f)
			wantsSilencer = true;

		if (isSilencerOn != wantsSilencer && !HasShield())
#endif
		{
			PrintIfWatched("%s silencer!\n", (isSilencerOn) ? "Unequipping" : "Equipping");
			pCurrentWeapon->SecondaryAttack();
		}
	}
}

// Invoked when in contact with a CWeaponBox
void CCSBot::OnTouchingWeapon(CWeaponBox *box)
{
	auto pDroppedWeapon = box->m_rgpPlayerItems[PRIMARY_WEAPON_SLOT];

	// right now we only care about primary weapons on the ground
	if (pDroppedWeapon)
	{
		CBasePlayerWeapon *pWeapon = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]);

		// if the gun on the ground is the same one we have, dont bother
		if (pWeapon && pWeapon->IsWeapon() && pDroppedWeapon->m_iId != pWeapon->m_iId)
		{
			// if we don't have a weapon preference, give up
			if (GetProfile()->HasPrimaryPreference())
			{
				// don't change weapons if we've seen enemies recently
				const float safeTime = 2.5f;
				if (GetTimeSinceLastSawEnemy() >= safeTime)
				{
					// we have a primary weapon - drop it if the one on the ground is better
					for (int i = 0; i < GetProfile()->GetWeaponPreferenceCount(); i++)
					{
						int prefID = GetProfile()->GetWeaponPreference(i);
						if (!IsPrimaryWeapon(prefID))
							continue;

						// if the gun we are using is more desirable, give up
						if (prefID == pWeapon->m_iId)
							break;

						if (prefID == pDroppedWeapon->m_iId)
						{
							// the gun on the ground is better than the one we have - drop our gun
							DropPrimary();
							break;
						}
					}
				}
			}
		}
	}
}

// Return true if a friend is in our weapon's way
// TODO: Check more rays for safety.
bool CCSBot::IsFriendInLineOfFire()
{
	if (CSGameRules()->IsFreeForAll())
		return false;

	UTIL_MakeVectors(pev->punchangle + pev->v_angle);

	// compute the unit vector along our view
	Vector aimDir = gpGlobals->v_forward;
	Vector eye = GetGunPosition();
	Vector target = eye;

	// trace the bullet's path
	TraceResult result;
	UTIL_TraceLine(eye, target + 10000.0f * aimDir, dont_ignore_monsters, ignore_glass, ENT(pev), &result);

	if (result.pHit)
	{
		CBasePlayer *pVictim = CBasePlayer::Instance(result.pHit);
		if (pVictim && pVictim->IsPlayer() && pVictim->IsAlive())
		{
			if (BotRelationship(pVictim) == BOT_TEAMMATE)
				return true;
		}
	}

	CBasePlayer *enemy = GetEnemy();
	float dangerRange = enemy ? (enemy->EyePosition() - eye).Length() + 80.0f : 1600.0f;
	if (dangerRange < 300.0f)
		dangerRange = 300.0f;

	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || pPlayer == this || !pPlayer->IsPlayer() || !pPlayer->IsAlive())
			continue;

		if (BotRelationship(pPlayer) != BOT_TEAMMATE)
			continue;

		Vector friendSpot = pPlayer->EyePosition();
		Vector toFriend = friendSpot - eye;
		float along = DotProduct(toFriend, aimDir);
		if (along <= 0.0f || along > dangerRange)
			continue;

		Vector closest = eye + aimDir * along;
		float lateral = (friendSpot - closest).Length();
		float dangerRadius = 34.0f + along * 0.018f;
		if (dangerRadius > 72.0f)
			dangerRadius = 72.0f;

		if (lateral > dangerRadius)
			continue;

		TraceResult friendTrace;
		UTIL_TraceLine(eye, friendSpot, dont_ignore_monsters, ignore_glass, ENT(pev), &friendTrace);
		if (friendTrace.pHit == pPlayer->edict())
			return true;
	}

	return false;
}

// Return line-of-sight distance to obstacle along weapon fire ray
// TODO: Re-use this computation with IsFriendInLineOfFire()
float CCSBot::ComputeWeaponSightRange()
{
	UTIL_MakeVectors(pev->punchangle + pev->v_angle);

	// compute the unit vector along our view
	Vector aimDir = gpGlobals->v_forward;
	Vector target = GetGunPosition();

	// trace the bullet's path
	TraceResult result;
	UTIL_TraceLine(GetGunPosition(), target + 10000.0f * aimDir, dont_ignore_monsters, ignore_glass, ENT(pev), &result);

	return (GetGunPosition() - result.vecEndPos).Length();
}
