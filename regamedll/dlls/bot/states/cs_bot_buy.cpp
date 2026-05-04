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
#include "bot/cs_bot_grenade_spots.h"

extern cvar_t bot_grenade_debug_he_only;

bool HasDefaultPistol(CCSBot *me)
{
	CBasePlayerWeapon *pSecondary = static_cast<CBasePlayerWeapon *>(me->m_rgpPlayerItems[PISTOL_SLOT]);

	if (!pSecondary)
		return false;

	if (me->m_iTeam == TERRORIST && pSecondary->m_iId == WEAPON_GLOCK18)
		return true;

	if (me->m_iTeam == CT && pSecondary->m_iId == WEAPON_USP)
		return true;

	return false;
}

static bool ShouldEcoPrimaryBuy(CCSBot *me);
static float GetRoleGrenadeBuyChance(CCSBot *me);
static const char *ChooseRoleGrenadeAlias(CCSBot *me, bool allBots);

// Buy weapons, armor, etc.
void BuyState::OnEnter(CCSBot *me)
{
	m_retries = 0;
	m_prefRetries = 0;
	m_prefIndex = 0;
	m_buyStep = 0;
	m_buyIntervalScale = RANDOM_FLOAT(0.85f, 1.85f) - 0.25f * TheCSBots()->GetEffectiveSkill(me);
	m_buyIntervalScale = Q_max(0.75f, Q_min(m_buyIntervalScale, 1.75f));
	m_nextFreezeLookTime = 0.0f;
	m_nextFreezeJumpTime = 0.0f;
	m_freezeLooksRemaining = 0;
	m_freezeJumpsRemaining = 0;

	m_doneBuying = false;
	m_freezePostBuyInitialized = false;
	m_isEcoRound = false;
	m_isInitialDelay = true;
	if (gpGlobals)
	{
		const bool freezeBuy = CSGameRules()->IsMultiplayer() && CSGameRules()->IsFreezePeriod();
		const float waitToBuyTime = freezeBuy ? 0.0f : 2.0f;
		const float botOffset = (me->entindex() % 8) * (freezeBuy ? 0.10f : 0.04f);
		const float randomOffset = freezeBuy ? RANDOM_FLOAT(0.05f, 1.10f) : RANDOM_FLOAT(0.0f, 0.20f);
		m_nextBuyTime = gpGlobals->time + waitToBuyTime + randomOffset + botOffset;
	}
	else
		m_nextBuyTime = 0.0f;

	// this will force us to stop holding live grenade
	me->EquipBestWeapon();

	m_buyDefuseKit = false;
	m_buyShield = false;

	if (me->m_iTeam == CT)
	{
		if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB)
		{
			// CT's sometimes buy defuse kits in the bomb scenario (except in career mode, where the player should defuse)
			if (!CSGameRules()->IsCareer())
			{
				CCSBotManager::DirectorRoleType role = TheCSBots()->GetDirectorRole(me);
				float buyDefuseKitChance = 50.0f;	// 100.0f * (me->GetProfile()->GetSkill() + 0.2f);
				if (role == CCSBotManager::DIRECTOR_ROLE_ROTATOR)
					buyDefuseKitChance = 66.0f;
				else if (role == CCSBotManager::DIRECTOR_ROLE_ANCHOR)
					buyDefuseKitChance = 56.0f;
				else if (role == CCSBotManager::DIRECTOR_ROLE_SNIPER)
					buyDefuseKitChance = 36.0f;
				if (RANDOM_FLOAT(0.0f, 100.0f) < buyDefuseKitChance)
				{
					m_buyDefuseKit = true;
				}
			}
		}

		// Tactical shields are intentionally excluded from bot buys.
	}

	if (TheCSBots()->AllowGrenades())
	{
		m_buyGrenade = (RANDOM_FLOAT(0.0f, 100.0f) < GetRoleGrenadeBuyChance(me)) ? true : false;
	}
	else
	{
		m_buyGrenade = false;
	}

	m_buyPistol = false;

	if (TheCSBots()->AllowPistols())
	{
		CBasePlayerWeapon *pSecondary = static_cast<CBasePlayerWeapon *>(me->m_rgpPlayerItems[PISTOL_SLOT]);

		// check if we have a pistol
		if (pSecondary)
		{
			// if we have our default pistol, think about buying a different one
			if (HasDefaultPistol(me))
			{
				// if everything other than pistols is disallowed, buy a pistol
				if (!TheCSBots()->AllowShotguns()
					&& !TheCSBots()->AllowSubMachineGuns()
					&& !TheCSBots()->AllowRifles()
					&& !TheCSBots()->AllowMachineGuns()
					&& !TheCSBots()->AllowSnipers())
				{
					m_buyPistol = (RANDOM_FLOAT(0, 100) < 75.0f);
				}
				else if (me->m_iAccount < 1000)
				{
					// if we're low on cash, buy a pistol
					m_buyPistol = (RANDOM_FLOAT(0, 100) < 75.0f);
				}
				else
				{
					m_buyPistol = (RANDOM_FLOAT(0, 100) < 33.3f);
				}
			}
		}
		else
		{
			// we dont have a pistol - buy one
			m_buyPistol = true;
		}
	}

	m_isEcoRound = ShouldEcoPrimaryBuy(me);
	if (m_isEcoRound)
	{
		m_buyDefuseKit = false;
		m_buyGrenade = false;
		m_buyShield = false;
		m_buyPistol = (me->m_iAccount >= DEAGLE_PRICE && RANDOM_FLOAT(0.0f, 100.0f) < 20.0f);
	}
}

enum WeaponType
{
	PISTOL,
	SHOTGUN,
	SUB_MACHINE_GUN,
	RIFLE,
	MACHINE_GUN,
	SNIPER_RIFLE,
	GRENADE,
	NUM_WEAPON_TYPES,
};

enum BuyStep
{
	BUY_STEP_PRIMARY,
	BUY_STEP_PRIMARY_AMMO,
	BUY_STEP_ARMOR_HELMET,
	BUY_STEP_ARMOR,
	BUY_STEP_PISTOL,
	BUY_STEP_PISTOL_AMMO,
	BUY_STEP_GRENADE,
	BUY_STEP_DEFUSE_KIT,
	BUY_STEP_DONE,
};

struct BuyInfo
{
	WeaponType type;
	bool preferred; // more challenging bots prefer these weapons
	char *buyAlias; // the buy alias for this equipment
};

// These tables MUST be kept in sync with the CT and T buy aliases
BuyInfo primaryWeaponBuyInfoCT[MAX_BUY_WEAPON_PRIMARY] =
{
	{ SHOTGUN,          false, "m3"     }, // WEAPON_M3
	{ SHOTGUN,          false, "xm1014" }, // WEAPON_XM1014
	{ SUB_MACHINE_GUN,  false, "tmp"    }, // WEAPON_TMP
	{ SUB_MACHINE_GUN,  false, "mp5"    }, // WEAPON_MP5N
	{ SUB_MACHINE_GUN,  false, "ump45"  }, // WEAPON_UMP45
	{ SUB_MACHINE_GUN,  false, "p90"    }, // WEAPON_P90
	{ RIFLE,            true,  "famas"  }, // WEAPON_FAMAS
	{ SNIPER_RIFLE,     false, "scout"  }, // WEAPON_SCOUT
	{ RIFLE,            true,  "m4a1"   }, // WEAPON_M4A1
	{ RIFLE,            false, "aug"    }, // WEAPON_AUG
	{ SNIPER_RIFLE,     true,  "sg550"  }, // WEAPON_SG550
	{ SNIPER_RIFLE,     true,  "awp"    }, // WEAPON_AWP
	{ MACHINE_GUN,      false, "m249"   }, // WEAPON_M249
};

BuyInfo secondaryWeaponBuyInfoCT[MAX_BUY_WEAPON_SECONDARY] =
{
//	{ PISTOL, false, "glock"  },
//	{ PISTOL, false, "usp"    },
	{ PISTOL, true,  "p228"   },
	{ PISTOL, true,  "deagle" },
	{ PISTOL, true,  "fn57"   },
};

BuyInfo primaryWeaponBuyInfoT[MAX_BUY_WEAPON_PRIMARY] =
{
	{ SHOTGUN,          false, "m3"     }, // WEAPON_M3
	{ SHOTGUN,          false, "xm1014" }, // WEAPON_XM1014
	{ SUB_MACHINE_GUN,  false, "mac10"  }, // WEAPON_MAC10
	{ SUB_MACHINE_GUN,  false, "mp5"    }, // WEAPON_MP5N
	{ SUB_MACHINE_GUN,  false, "ump45"  }, // WEAPON_UMP45
	{ SUB_MACHINE_GUN,  false, "p90"    }, // WEAPON_P90
	{ RIFLE,            true,  "galil"  }, // WEAPON_GALIL
	{ RIFLE,            true,  "ak47"   }, // WEAPON_AK47
	{ SNIPER_RIFLE,     false, "scout"  }, // WEAPON_SCOUT
	{ RIFLE,            true,  "sg552"  }, // WEAPON_SG552
	{ SNIPER_RIFLE,     true,  "awp"    }, // WEAPON_AWP
	{ SNIPER_RIFLE,     true,  "g3sg1"  }, // WEAPON_G3SG1
	{ MACHINE_GUN,      false, "m249"   }, // WEAPON_M249
};

BuyInfo secondaryWeaponBuyInfoT[MAX_BUY_WEAPON_SECONDARY] =
{
//	{ PISTOL, false, "glock"  },
//	{ PISTOL, false, "usp"    },
	{ PISTOL, true,  "p228"   },
	{ PISTOL, true,  "deagle" },
	{ PISTOL, true,  "elites" },
};

// Given a weapon alias, return the kind of weapon it is
inline WeaponType GetWeaponType(const char *alias)
{
	int i;
	for (i = 0; i < MAX_BUY_WEAPON_PRIMARY; i++)
	{
		if (!Q_stricmp(alias, primaryWeaponBuyInfoCT[i].buyAlias))
			return primaryWeaponBuyInfoCT[i].type;

		if (!Q_stricmp(alias, primaryWeaponBuyInfoT[i].buyAlias))
			return primaryWeaponBuyInfoT[i].type;
	}

	for (i = 0; i < MAX_BUY_WEAPON_SECONDARY; i++)
	{
		if (!Q_stricmp(alias, secondaryWeaponBuyInfoCT[i].buyAlias))
			return secondaryWeaponBuyInfoCT[i].type;

		if (!Q_stricmp(alias, secondaryWeaponBuyInfoT[i].buyAlias))
			return secondaryWeaponBuyInfoT[i].type;
	}

	return NUM_WEAPON_TYPES;
}

static bool IsWeaponTypeAllowed(WeaponType type)
{
	switch (type)
	{
	case PISTOL:
		return TheCSBots()->AllowPistols();
	case SHOTGUN:
		return TheCSBots()->AllowShotguns();
	case SUB_MACHINE_GUN:
		return TheCSBots()->AllowSubMachineGuns();
	case RIFLE:
		return TheCSBots()->AllowRifles();
	case MACHINE_GUN:
		return TheCSBots()->AllowMachineGuns();
	case SNIPER_RIFLE:
		return TheCSBots()->AllowSnipers();
	default:
		return false;
	}
}

static bool IsBuyableWeaponChoice(CCSBot *me, int weaponID)
{
	if (!me || weaponID <= WEAPON_NONE)
		return false;

	const char *buyAlias = WeaponIDToAlias(weaponID);
	if (!buyAlias || !IsWeaponTypeAllowed(GetWeaponType(buyAlias)))
		return false;

	if (!CanBuyThis(me, weaponID))
		return false;

	WeaponInfoStruct *info = GetWeaponInfo(weaponID);
	return info && info->cost <= me->m_iAccount;
}

static bool IsBuyableWeaponChoice(CCSBot *me, const BuyInfo *info)
{
	if (!info || !info->buyAlias)
		return false;

	if (!IsWeaponTypeAllowed(info->type))
		return false;

	return IsBuyableWeaponChoice(me, AliasToWeaponID(info->buyAlias));
}

static float GetRoleGrenadeBuyChance(CCSBot *me)
{
	if (bot_grenade_debug_he_only.value > 0.0f)
		return 100.0f;

	CCSBotManager::DirectorRoleType role = TheCSBots()->GetDirectorRole(me);
	float chance = 86.0f;

	switch (role)
	{
	case CCSBotManager::DIRECTOR_ROLE_ENTRY:
		chance = 96.0f;
		break;
	case CCSBotManager::DIRECTOR_ROLE_SUPPORT:
		chance = 98.0f;
		break;
	case CCSBotManager::DIRECTOR_ROLE_LURK:
		chance = 82.0f;
		break;
	case CCSBotManager::DIRECTOR_ROLE_ANCHOR:
		chance = 92.0f;
		break;
	case CCSBotManager::DIRECTOR_ROLE_ROTATOR:
		chance = 96.0f;
		break;
	case CCSBotManager::DIRECTOR_ROLE_SNIPER:
		chance = 72.0f;
		break;
	default:
		break;
	}

	chance += TheCSBots()->GetEffectiveSkill(me) * 10.0f;
	return Q_max(70.0f, Q_min(chance, 99.0f));
}

static const char *ChooseRoleGrenadeAlias(CCSBot *me, bool allBots)
{
	if (bot_grenade_debug_he_only.value > 0.0f)
	{
		if (BotGrenadeSpots::HasAnyLineupForGrenadeAndTeam(WEAPON_SMOKEGRENADE, me->m_iTeam)
			&& !me->HasGrenadeKind(WEAPON_SMOKEGRENADE)
			&& RANDOM_FLOAT(0.0f, 100.0f) < 55.0f)
			return "sgren";

		return "hegren";
	}

	CCSBotManager::DirectorRoleType role = TheCSBots()->GetDirectorRole(me);
	float rnd = RANDOM_FLOAT(0.0f, 100.0f);
	const bool hasSmokePlan = BotGrenadeSpots::HasAnyLineupForGrenadeAndTeam(WEAPON_SMOKEGRENADE, me->m_iTeam);

	if (hasSmokePlan)
	{
		switch (role)
		{
		case CCSBotManager::DIRECTOR_ROLE_SUPPORT:
			if (!me->HasGrenadeKind(WEAPON_SMOKEGRENADE) && rnd < 88.0f)
				return "sgren";
			break;
		case CCSBotManager::DIRECTOR_ROLE_ROTATOR:
		case CCSBotManager::DIRECTOR_ROLE_ANCHOR:
			if (!me->HasGrenadeKind(WEAPON_SMOKEGRENADE) && rnd < 82.0f)
				return "sgren";
			break;
		case CCSBotManager::DIRECTOR_ROLE_ENTRY:
			if (!me->HasGrenadeKind(WEAPON_SMOKEGRENADE) && rnd < 72.0f)
				return "sgren";
			break;
		case CCSBotManager::DIRECTOR_ROLE_SNIPER:
			if (!me->HasGrenadeKind(WEAPON_SMOKEGRENADE) && rnd < 66.0f)
				return "sgren";
			break;
		default:
			if (!me->HasGrenadeKind(WEAPON_SMOKEGRENADE) && rnd < 62.0f)
				return "sgren";
			break;
		}
	}

	if (!allBots)
	{
		// Avoid blinding friendly humans; keep flashes mostly for all-bot teams.
		// HE is the default util buy in CS — bias heavily toward hegren.
		switch (role)
		{
		case CCSBotManager::DIRECTOR_ROLE_SUPPORT:
		case CCSBotManager::DIRECTOR_ROLE_ROTATOR:
		case CCSBotManager::DIRECTOR_ROLE_ANCHOR:
			return (rnd < 34.0f) ? "sgren" : "hegren";
		case CCSBotManager::DIRECTOR_ROLE_SNIPER:
			return (rnd < 30.0f) ? "sgren" : "hegren";
		default:
			return (rnd < 18.0f) ? "sgren" : "hegren";
		}
	}

	switch (role)
	{
	case CCSBotManager::DIRECTOR_ROLE_ENTRY:
		if (rnd < 18.0f)
			return "flash";
		return (rnd < 40.0f) ? "sgren" : "hegren";
	case CCSBotManager::DIRECTOR_ROLE_SUPPORT:
		if (rnd < 14.0f)
			return "flash";
		return (rnd < 48.0f) ? "sgren" : "hegren";
	case CCSBotManager::DIRECTOR_ROLE_LURK:
		return (rnd < 24.0f) ? "sgren" : "hegren";
	case CCSBotManager::DIRECTOR_ROLE_ANCHOR:
		return (rnd < 42.0f) ? "sgren" : "hegren";
	case CCSBotManager::DIRECTOR_ROLE_ROTATOR:
		if (rnd < 8.0f)
			return "flash";
		return (rnd < 44.0f) ? "sgren" : "hegren";
	case CCSBotManager::DIRECTOR_ROLE_SNIPER:
		return (rnd < 36.0f) ? "sgren" : "hegren";
	default:
		if (rnd < 6.0f)
			return "sgren";
		if (rnd < 18.0f)
			return "flash";
		return "hegren";
	}
}

static bool ShouldEcoPrimaryBuy(CCSBot *me)
{
	if (!me || me->m_bHasPrimary)
		return false;

	if (!TheCSBots()->AllowShotguns() &&
		!TheCSBots()->AllowSubMachineGuns() &&
		!TheCSBots()->AllowRifles() &&
		!TheCSBots()->AllowMachineGuns() &&
		!TheCSBots()->AllowSnipers())
	{
		return false;
	}

	const int money = me->m_iAccount;
	float ecoChance = 0.0f;

	if (money < MP5NAVY_PRICE)
		ecoChance = 85.0f;
	else if (money < GALIL_PRICE)
		ecoChance = 65.0f;
	else if (money < AK47_PRICE)
		ecoChance = 35.0f;
	else if (money < M4A1_PRICE)
		ecoChance = 15.0f;

	// Better bots are more willing to save for a proper rifle buy instead of forcing weak guns.
	ecoChance += TheCSBots()->GetEffectiveSkill(me) * 10.0f;

	return ecoChance > 0.0f && RANDOM_FLOAT(0.0f, 100.0f) < ecoChance;
}

inline float GetHumanizedBuyInterval(bool isPrimaryBuy, float scale)
{
	if (isPrimaryBuy)
		return RANDOM_FLOAT(0.10f, 0.32f) * scale;

	return RANDOM_FLOAT(0.16f, 0.46f) * scale;
}

static Vector BuildFreezeLookSpot(CCSBot *me)
{
	const Vector eye = me->GetEyePosition();
	const float yaw = me->pev->v_angle.y + RANDOM_FLOAT(-75.0f, 75.0f);
	const float pitch = RANDOM_FLOAT(-4.0f, 5.0f);
	const float range = RANDOM_FLOAT(520.0f, 980.0f);
	const float flatRange = range * BotCOS(pitch);

	return eye + Vector(flatRange * BotCOS(yaw), flatRange * BotSIN(yaw), -range * BotSIN(pitch));
}

void BuyState::UpdatePostBuyFreezeBehavior(CCSBot *me)
{
	if (!m_freezePostBuyInitialized)
	{
		m_freezePostBuyInitialized = true;

		const float skill = TheCSBots()->GetEffectiveSkill(me);
		const float aggression = me->GetProfile()->GetAggression();
		const bool wantsLookAround = RANDOM_FLOAT(0.0f, 100.0f) < 42.0f + skill * 28.0f;
		const bool wantsJump = RANDOM_FLOAT(0.0f, 100.0f) < 15.0f + aggression * 22.0f;

		m_freezeLooksRemaining = wantsLookAround ? RANDOM_LONG(1, RANDOM_FLOAT(0.0f, 100.0f) < 28.0f ? 3 : 2) : 0;
		m_freezeJumpsRemaining = wantsJump ? RANDOM_LONG(1, RANDOM_FLOAT(0.0f, 100.0f) < 22.0f ? 2 : 1) : 0;
		m_nextFreezeLookTime = gpGlobals->time + RANDOM_FLOAT(0.20f, 1.35f);
		m_nextFreezeJumpTime = gpGlobals->time + RANDOM_FLOAT(0.45f, 2.10f);
	}

	if (m_freezeLooksRemaining > 0 && gpGlobals->time >= m_nextFreezeLookTime)
	{
		Vector spot = BuildFreezeLookSpot(me);
		me->SetLookAt("Post-buy freeze look", &spot, PRIORITY_LOW, RANDOM_FLOAT(0.35f, 0.95f), true, 18.0f);
		m_freezeLooksRemaining--;
		m_nextFreezeLookTime = gpGlobals->time + RANDOM_FLOAT(0.70f, 1.85f);
	}

	if (m_freezeJumpsRemaining > 0 && gpGlobals->time >= m_nextFreezeJumpTime)
	{
		me->Jump();
		m_freezeJumpsRemaining--;
		m_nextFreezeJumpTime = gpGlobals->time + RANDOM_FLOAT(0.85f, 1.90f);
	}
}

void BuyState::OnUpdate(CCSBot *me)
{
	// wait for a Navigation Mesh
	if (!TheNavAreaList.size())
		return;

	// The game rejects buys too early after spawn, so stagger bots after the normal buy gate.
	if (m_isInitialDelay)
	{
		if (gpGlobals->time < m_nextBuyTime)
			return;

		m_isInitialDelay = false;
		m_nextBuyTime = gpGlobals->time;
	}

	// if we're done buying and still in the freeze period, wait
	if (m_doneBuying)
	{
		if (CSGameRules()->IsMultiplayer() && CSGameRules()->IsFreezePeriod())
		{
#ifdef REGAMEDLL_FIXES
			// make sure we're locked and loaded
			me->EquipBestWeapon(MUST_EQUIP);
			me->Reload();
			me->ResetStuckMonitor();
			UpdatePostBuyFreezeBehavior(me);
#endif
			return;
		}

		me->Idle();

#ifdef REGAMEDLL_FIXES
		return;
#endif
	}

	// is the bot spawned outside of a buy zone?
	if (!(me->m_signals.GetState() & SIGNAL_BUY))
	{
		m_doneBuying = true;
		UTIL_DPrintf("%s bot spawned outside of a buy zone (%d, %d, %d)\n", (me->m_iTeam == CT) ? "CT" : "Terrorist", int(me->pev->origin.x), int(me->pev->origin.y), int(me->pev->origin.z));
		return;
	}

	// Try to buy one item at a time so bots look like humans using the buy menu.
	if (gpGlobals->time >= m_nextBuyTime)
	{
		me->m_stateTimestamp = gpGlobals->time;
		m_nextBuyTime = gpGlobals->time + GetHumanizedBuyInterval(m_buyStep == BUY_STEP_PRIMARY, m_buyIntervalScale);

		if (m_buyStep == BUY_STEP_PRIMARY)
		{
			if (m_isEcoRound && !me->m_bHasPrimary)
			{
				me->PrintIfWatched("Eco round - skipping primary weapon buy.\n");
				m_buyStep = BUY_STEP_PISTOL;
				return;
			}

			bool isPreferredAllDisallowed = true;

			// try to buy our preferred weapons first
			if (m_prefIndex < me->GetProfile()->GetWeaponPreferenceCount())
			{
				// need to retry because sometimes first buy fails??
				const int maxPrefRetries = 2;
				if (m_prefRetries >= maxPrefRetries)
				{
					// try to buy next preferred weapon
					m_prefIndex++;
					m_prefRetries = 0;
					return;
				}

				int weaponPreference = me->GetProfile()->GetWeaponPreference(m_prefIndex);

				// don't buy it again if we still have one from last round
				CBasePlayerWeapon *pPrimary = static_cast<CBasePlayerWeapon *>(me->m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]);
				if (pPrimary && pPrimary->m_iId == weaponPreference)
				{
					// done with buying preferred weapon
					m_prefIndex = 9999;
					return;
				}

				if (me->HasShield() && weaponPreference == WEAPON_SHIELDGUN)
				{
					// done with buying preferred weapon
					m_prefIndex = 9999;
					return;
				}

				const char *buyAlias = nullptr;
				if (weaponPreference != WEAPON_SHIELDGUN)
				{
					buyAlias = WeaponIDToAlias(weaponPreference);
					WeaponType type = GetWeaponType(buyAlias);

					switch (type)
					{
					case PISTOL:
						if (!TheCSBots()->AllowPistols())
							buyAlias = nullptr;
						break;
					case SHOTGUN:
						if (!TheCSBots()->AllowShotguns())
							buyAlias = nullptr;
						break;
					case SUB_MACHINE_GUN:
						if (!TheCSBots()->AllowSubMachineGuns())
							buyAlias = nullptr;
						break;
					case RIFLE:
						if (!TheCSBots()->AllowRifles())
							buyAlias = nullptr;
						break;
					case MACHINE_GUN:
						if (!TheCSBots()->AllowMachineGuns())
							buyAlias = nullptr;
						break;
					case SNIPER_RIFLE:
						if (!TheCSBots()->AllowSnipers())
							buyAlias = nullptr;
						break;
					}

					if (buyAlias && !IsBuyableWeaponChoice(me, weaponPreference))
						buyAlias = nullptr;
				}

				if (buyAlias)
				{
					me->ClientCommand(buyAlias);
					me->PrintIfWatched("Tried to buy preferred weapon %s.\n", buyAlias);

					isPreferredAllDisallowed = false;
				}

				m_prefRetries++;

				// bail out so we dont waste money on other equipment
				// unless everything we prefer has been disallowed, then buy at random
				if (isPreferredAllDisallowed == false)
					return;
			}

			// if we have no preferred primary weapon (or everything we want is disallowed), buy at random
			if (!me->m_bHasPrimary && (isPreferredAllDisallowed || !me->GetProfile()->HasPrimaryPreference()))
			{
				m_buyShield = false;

				// build list of allowable weapons to buy
				BuyInfo *masterPrimary = (me->m_iTeam == TERRORIST) ? primaryWeaponBuyInfoT : primaryWeaponBuyInfoCT;
				BuyInfo *stockPrimary[MAX_BUY_WEAPON_PRIMARY];
				int stockPrimaryCount = 0;

				// dont choose sniper rifles as often
				CCSBotManager::DirectorRoleType role = TheCSBots()->GetDirectorRole(me);
				float sniperRifleChance = (role == CCSBotManager::DIRECTOR_ROLE_SNIPER) ? 74.0f : 34.0f;
				if (role == CCSBotManager::DIRECTOR_ROLE_ENTRY)
					sniperRifleChance = 18.0f;
				else if (role == CCSBotManager::DIRECTOR_ROLE_SUPPORT || role == CCSBotManager::DIRECTOR_ROLE_ROTATOR)
					sniperRifleChance = 26.0f;
				bool wantSniper = (RANDOM_FLOAT(0, 100) < sniperRifleChance) ? true : false;

				for (int i = 0; i < MAX_BUY_WEAPON_PRIMARY; i++)
				{
					if (((masterPrimary[i].type != SNIPER_RIFLE) || wantSniper) && IsBuyableWeaponChoice(me, &masterPrimary[i]))
					{
						stockPrimary[stockPrimaryCount++] = &masterPrimary[i];
					}
				}

				if (stockPrimaryCount)
				{
					// buy primary weapon if we don't have one
					int which;

					// on hard difficulty levels, bots try to buy preferred weapons on the first pass
					if (m_retries == 0 && TheCSBots()->GetDifficultyLevel() >= BOT_HARD)
					{
						// count up available preferred weapons
						int prefCount = 0;
						for (which = 0; which < stockPrimaryCount; which++)
						{
							if (stockPrimary[which]->preferred)
								prefCount++;
						}

						if (prefCount)
						{
							int whichPref = RANDOM_LONG(0, prefCount - 1);
							for (which = 0; which < stockPrimaryCount; which++)
							{
								if (stockPrimary[which]->preferred && whichPref-- == 0)
									break;
							}
						}
						else
						{
							// no preferred weapons available, just pick randomly
							which = RANDOM_LONG(0, stockPrimaryCount - 1);
						}
					}
					else
					{
						which = RANDOM_LONG(0, stockPrimaryCount - 1);
					}

					me->ClientCommand(stockPrimary[which]->buyAlias);
					me->PrintIfWatched("Tried to buy %s.\n", stockPrimary[which]->buyAlias);
				}
			}

			const bool canBuyPrimary =
				TheCSBots()->AllowShotguns() ||
				TheCSBots()->AllowSubMachineGuns() ||
				TheCSBots()->AllowRifles() ||
				TheCSBots()->AllowMachineGuns() ||
				TheCSBots()->AllowSnipers();
			const int maxPrimaryRetries = (canBuyPrimary && me->m_iAccount >= 1400) ? 20 : 10;

			// If we now have a weapon, or have tried for too long, move to equipment.
			if (me->m_bHasPrimary || m_retries++ > maxPrimaryRetries)
				m_buyStep = BUY_STEP_PRIMARY_AMMO;

			return;
		}

		switch (m_buyStep)
		{
		case BUY_STEP_PRIMARY_AMMO:
			if (!m_isEcoRound && me->m_bHasPrimary)
				me->ClientCommand("primammo");
			break;
		case BUY_STEP_ARMOR_HELMET:
			// buy armor after the primary attempt, like a human prioritizing the gun first
			if (!m_isEcoRound)
				me->ClientCommand("vesthelm");
			break;
		case BUY_STEP_ARMOR:
			if (!m_isEcoRound)
				me->ClientCommand("vest");
			break;
		case BUY_STEP_PISTOL:
			if (TheCSBots()->AllowPistols()
#ifndef REGAMEDLL_FIXES
				&& !me->GetProfile()->HasPistolPreference()
#endif
			)
			{
				if (m_buyPistol)
				{
#ifdef REGAMEDLL_FIXES
					// pistols - if we have no preferred pistol, buy at random
					if (!me->GetProfile()->HasPistolPreference())
#endif
					{
						BuyInfo *masterSecondary = (me->m_iTeam == TERRORIST) ? secondaryWeaponBuyInfoT : secondaryWeaponBuyInfoCT;
						BuyInfo *stockSecondary[MAX_BUY_WEAPON_SECONDARY];
						int stockSecondaryCount = 0;

						for (int i = 0; i < MAX_BUY_WEAPON_SECONDARY; ++i)
						{
							if (IsBuyableWeaponChoice(me, &masterSecondary[i]))
								stockSecondary[stockSecondaryCount++] = &masterSecondary[i];
						}

						if (stockSecondaryCount)
						{
							int which = RANDOM_LONG(0, stockSecondaryCount - 1);
							me->ClientCommand(stockSecondary[which]->buyAlias);
						}
					}

					// only buy one pistol
					m_buyPistol = false;
				}
			}
			break;
		case BUY_STEP_PISTOL_AMMO:
			if (TheCSBots()->AllowPistols()
#ifndef REGAMEDLL_FIXES
				&& !me->GetProfile()->HasPistolPreference()
#endif
			)
			{
				me->ClientCommand("secammo");
			}
			break;
		case BUY_STEP_GRENADE:
			// Buy the chosen utility type even if another grenade type is already carried.
			if (m_buyGrenade)
			{
				const char *grenadeAlias = ChooseRoleGrenadeAlias(me, UTIL_IsTeamAllBots(me->m_iTeam));
				const int grenadeId = AliasToWeaponID(grenadeAlias);
				if ((grenadeId == WEAPON_HEGRENADE || grenadeId == WEAPON_FLASHBANG || grenadeId == WEAPON_SMOKEGRENADE)
					&& !me->HasGrenadeKind(grenadeId))
				{
					me->ClientCommand(grenadeAlias);
					me->PrintIfWatched("Director role buy: grenade %s.\n", grenadeAlias);
				}
			}
			break;
		case BUY_STEP_DEFUSE_KIT:
			if (m_buyDefuseKit)
				me->ClientCommand("defuser");
			break;
		default:
			break;
		}

		if (++m_buyStep >= BUY_STEP_DONE)
			m_doneBuying = true;
	}
}

void BuyState::OnExit(CCSBot *me)
{
	me->ResetStuckMonitor();
	me->EquipBestWeapon();
}
