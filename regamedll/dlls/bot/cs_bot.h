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

#pragma once

#include "bot/cs_gamestate.h"
#include "bot/cs_bot_manager.h"
#include "bot/cs_bot_chatter.h"
#include "bot/cs_bot_non_combat_aim_drift.h"
#include "bot/cs_bot_look_target_selector.h"
#include "bot/cs_bot_movement_debug.h"
#include "aim_log.h"

const int MAX_BUY_WEAPON_PRIMARY   = 13;
const int MAX_BUY_WEAPON_SECONDARY = 3;

enum
{
	BOT_PROGGRESS_DRAW = 0, // draw status bar progress
	BOT_PROGGRESS_START,    // init status bar progress
	BOT_PROGGRESS_HIDE,     // hide status bar progress
};

class CCSBot;
class BotChatterInterface;

class BotState
{
public:
	virtual void OnEnter(CCSBot *me) {}
	virtual void OnUpdate(CCSBot *me) {}
	virtual void OnExit(CCSBot *me) {}
	virtual const char *GetName() const = 0;
};

class IdleState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual const char *GetName() const { return "Idle"; }
};

class HuntState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Hunt"; }

	void ClearHuntArea() { m_huntArea = nullptr; m_huntAreaCommitTime = 0.0f; }
	CNavArea *GetHuntArea() { return m_huntArea; }

private:
	void EmitHuntTargetDebugMarker(
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
		unsigned newAreaId);
	void UpdateHuntRepeatFailLog(
		CCSBot *me,
		unsigned areaId,
		const Vector &goalPos,
		int pathResultInt,
		unsigned destA,
		bool hasPath,
		bool stuck);

	// One pick-from-scratch + ComputePath; returns true if committed.
	bool ExecuteHuntPickAndCommit(CCSBot *me, const char *debugReason);

	CNavArea *m_huntArea;
	float m_huntAreaCommitTime = 0.0f;
	bool m_huntConsumedGuardThisUpdate = false;
	unsigned m_huntRepeatFailArea = 0;
	float m_huntRepeatFailWindowStart = 0.0f;
	int m_huntRepeatFailCount = 0;
};

class AttackState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Attack"; }

	int GetCombatRepositionDirForDebug() const { return m_combatRepositionDir; }

	void SetCrouchAndHold(bool crouch) { m_crouchAndHold = crouch; }
	void StopAttacking(CCSBot *me);

protected:
	enum DodgeStateType
	{
		STEADY_ON,
		SLIDE_LEFT,
		SLIDE_RIGHT,
		JUMP,
		NUM_ATTACK_STATES
	} m_dodgeState;

	float m_nextDodgeStateTimestamp;
	CountdownTimer m_repathTimer;
	float m_scopeTimestamp;
	bool m_haveSeenEnemy;
	bool m_isEnemyHidden;
	float m_reacquireTimestamp;
	float m_shieldToggleTimestamp;
	bool m_shieldForceOpen;
	float m_pinnedDownTimestamp;
	float m_stopAndShootTimestamp;
	float m_nextStopAndShootTimestamp;
	float m_nextCombatRepositionTimestamp;
	float m_combatRepositionEndTimestamp;
	int m_combatRepositionDir;
	float m_nextSuspiciousLookTimestamp;
	bool m_crouchAndHold;
	bool m_didAmbushCheck;
	bool m_dodge;
	bool m_firstDodge;
	bool m_isCoward;
	CountdownTimer m_retreatTimer;
};

class InvestigateNoiseState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "InvestigateNoise"; }

private:
	void AttendCurrentNoise(CCSBot *me);
	Vector m_checkNoisePosition;
	Vector m_noiseSourcePosition;
};

class BuyState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Buy"; }

private:
	void UpdatePostBuyFreezeBehavior(CCSBot *me);

	bool m_isInitialDelay;
	int m_prefRetries;
	int m_prefIndex;
	int m_retries;
	int m_buyStep;
	float m_nextBuyTime;
	float m_buyIntervalScale;
	float m_nextFreezeLookTime;
	float m_nextFreezeJumpTime;
	int m_freezeLooksRemaining;
	int m_freezeJumpsRemaining;
	bool m_doneBuying;
	bool m_freezePostBuyInitialized;
	bool m_isEcoRound;
	bool m_buyDefuseKit;
	bool m_buyGrenade;
	bool m_buyShield;
	bool m_buyPistol;
};

class MoveToState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "MoveTo"; }

	void SetGoalPosition(const Vector &pos) { m_goalPosition = pos; }
	void SetRouteType(RouteType route) { m_routeType = route; }

private:
	Vector m_goalPosition;
	RouteType m_routeType;
	int m_repathAttempts;
	float m_nextRepathTime;
	bool m_radioedPlan;
	bool m_askedForCover;
};

class FetchBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual const char *GetName() const { return "FetchBomb"; }
};

class PlantBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "PlantBomb"; }
};

class DefuseBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "DefuseBomb"; }
};

class HideState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Hide"; }

public:
	void SetHidingSpot(const Vector &pos) { m_hidingSpot = pos; }
	const Vector &GetHidingSpot() const { return m_hidingSpot; }

	void SetSearchArea(CNavArea *area) { m_searchFromArea = area; }
	CNavArea *GetSearchArea() { return m_searchFromArea; }
	void SetSearchRange(float range) { m_range = range; }

	void SetDuration(float time) { m_duration = time; }
	void SetHoldPosition(bool hold) { m_isHoldingPosition = hold; }

	bool IsAtSpot() const { return m_isAtSpot; }

private:
	CNavArea *m_searchFromArea;
	float m_range;

	Vector m_hidingSpot;
	bool m_isAtSpot;
	float m_duration;
	bool m_isHoldingPosition;
	float m_holdPositionTime;
	bool m_heardEnemy;

	float m_firstHeardEnemyTime;
	int m_retry;
	Vector m_leaderAnchorPos;
};

class EscapeFromBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "EscapeFromBomb"; }
};

class FollowState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Follow"; }

	void SetLeader(CBasePlayer *pLeader) { m_leader = pLeader; }

private:
	void ComputeLeaderMotionState(float leaderSpeed);

	EntityHandle<CBasePlayer> m_leader;
	Vector m_lastLeaderPos;
	bool m_isStopped;
	float m_stoppedTimestamp;

	enum LeaderMotionStateType
	{
		INVALID,
		STOPPED,
		WALKING,
		RUNNING

	} m_leaderMotionState;

	IntervalTimer m_leaderMotionStateTime;

	bool m_isSneaking;
	float m_lastSawLeaderTime;
	CountdownTimer m_repathInterval;

	IntervalTimer m_walkTime;
	bool m_isAtWalkSpeed;

	float m_waitTime;
	CountdownTimer m_idleTimer;
};

class UseEntityState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "UseEntity"; }

	void SetEntity(CBaseEntity *pEntity) { m_entity = pEntity; }

private:
	EntityHandle<CBaseEntity> m_entity;
};

// The Counter-strike Bot
class CCSBot: public CBot
{
public:
	CCSBot();																											// constructor initializes all values to zero
	virtual BOOL TakeDamage(entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType);		// invoked when injured by something (EXTEND) - returns the amount of damage inflicted
	virtual void Killed(entvars_t *pevAttacker, int iGib);																// invoked when killed (EXTEND)
	virtual void RoundRespawn();
	virtual void Blind(float duration, float holdTime, float fadeTime, int alpha = 255);								// player blinded by a flashbang
	virtual void OnTouchingWeapon(CWeaponBox *box);																		// invoked when in contact with a CWeaponBox

	virtual bool Initialize(const BotProfile *profile);						// (EXTEND) prepare bot for action
	virtual void SpawnBot();												// (EXTEND) spawn the bot into the game

	virtual void Upkeep();													// lightweight maintenance, invoked frequently
	virtual void Update();													// heavyweight algorithms, invoked less often

	void BotThink() override;

	virtual void Walk();
	virtual bool Jump(bool mustJump = false);								// returns true if jump was started

	virtual void OnEvent(GameEventType event, CBaseEntity *pEntity = nullptr, CBaseEntity *pOther = nullptr);		// invoked when event occurs in the game (some events have NULL entity)

	#define CHECK_FOV true
	virtual bool IsVisible(const Vector *pos, bool testFOV = false) const;											// return true if we can see the point
	virtual bool IsVisible(CBasePlayer *pPlayer, bool testFOV = false, unsigned char *visParts = nullptr) const;	// return true if we can see any part of the player

	virtual bool IsEnemyPartVisible(VisiblePartType part) const;													// if enemy is visible, return the part we see for our current enemy

public:
	void Disconnect();

	// behavior properties
	float GetCombatRange() const;
	bool IsRogue() const;					// return true if we dont listen to teammates or pursue scenario goals
	void SetRogue(bool rogue);
	bool IsHurrying() const;				// return true if we are in a hurry
	void Hurry(float duration);				// force bot to hurry
	bool IsSafe() const;					// return true if we are in a safe region
	bool IsWellPastSafe() const;			// return true if it is well past the early, "safe", part of the round
	bool IsEndOfSafeTime() const;			// return true if we were in the safe time last update, but not now
	float GetSafeTimeRemaining() const;		// return the amount of "safe time" we have left
	float GetSafeTime() const;				// return what we think the total "safe time" for this map is
	bool ShouldEquipKnifeForSafeRun();		// return true if this bot personally switches to knife during safe time
	//bool IsUnhealthy() const;				// returns true if bot is low on health

	// behaviors
	void Idle();
	void Hide(CNavArea *searchFromArea = nullptr, float duration = -1.0f, float hideRange = 750.0f, bool holdPosition = false);						// DEPRECATED: Use TryToHide() instead
	#define USE_NEAREST true
	bool TryToHide(CNavArea *searchFromArea = nullptr, float duration = -1.0f, float hideRange = 750.0f, bool holdPosition = false, bool useNearest = false);			// try to hide nearby, return false if cannot

	void Hide(const Vector *hidingSpot, float duration = -1.0f, bool holdPosition = false);			// move to the given hiding place
	bool IsHiding() const;																			// returns true if bot is currently hiding
	bool IsAtHidingSpot() const;																	// return true if we are hiding and at our hiding spot
	bool TryToRetreat();																			// retreat to a nearby hiding spot, away from enemies

	void Hunt();
	bool IsHunting() const;								// returns true if bot is currently hunting

	void Attack(CBasePlayer *victim);
	void FireWeaponAtEnemy();							// fire our active weapon towards our current enemy
	void StopAttacking();
	bool IsAttacking() const;							// returns true if bot is currently engaging a target

	void MoveTo(const Vector *pos, RouteType route = SAFEST_ROUTE); // move to potentially distant position
	bool IsMovingTo() const;							// return true if we are in the MoveTo state

	void PlantBomb();

	void FetchBomb();									// bomb has been dropped - go get it
	bool NoticeLooseBomb() const;						// return true if we noticed the bomb on the ground or on radar
	bool CanSeeLooseBomb() const;						// return true if we directly see the loose bomb
	bool IsCarryingBomb() const;

	void DefuseBomb();
	bool IsDefusingBomb() const;						// returns true if bot is currently defusing the bomb
	bool CanSeePlantedBomb() const;						// return true if we directly see the planted bomb

	void EscapeFromBomb();
	bool IsEscapingFromBomb() const;					// return true if we are escaping from the bomb

	void RescueHostages();
	void UseEntity(CBaseEntity *pEntity);				// use the entity

	bool IsBuying() const;

#ifdef REGAMEDLL_FIXES
	void Kill();
#endif

	void Panic(CBasePlayer *pEnemy);					// look around in panic
	void Follow(CBasePlayer *pPlayer);					// begin following given Player
	void ContinueFollowing();							// continue following our leader after finishing what we were doing
	void StopFollowing();								// stop following
	bool IsFollowing() const;							// return true if we are following someone (not necessarily in the follow state)
	CBasePlayer *GetFollowLeader();						// return the leader we are following
	float GetFollowDuration() const;					// return how long we've been following our leader
	bool CanAutoFollow() const;							// return true if we can auto-follow

	bool IsNotMoving() const;							// return true if we are currently standing still

	void AimAtEnemy();									// point our weapon towards our enemy
	void StopAiming();									// stop aiming at enemy
	bool IsAimingAtEnemy() const;						// returns true if we are trying to aim at an enemy

	bool IsSurprised() const;							// return true if we are "surprised"
	float GetSurpriseDelay() const;
	void ClearSurpriseDelay();

	float GetStateTimestamp() const;					// get time current state was entered

	bool IsDoingScenario() const;						// return true if we will do scenario-related tasks

	// scenario / gamestate
	CSGameState *GetGameState();						// return an interface to this bot's gamestate
	const CSGameState *GetGameState() const;			// return an interface to this bot's gamestate

	bool IsAtBombsite();								// return true if we are in a bomb planting zone
	bool GuardRandomZone(float range = 500.0f);			// pick a random zone and hide near it

	bool IsBusy() const;								// return true if we are busy doing something important

	// high-level tasks
	enum TaskType
	{
		SEEK_AND_DESTROY,
		PLANT_BOMB,
		FIND_TICKING_BOMB,
		DEFUSE_BOMB,
		GUARD_TICKING_BOMB,
		GUARD_BOMB_DEFUSER,
		GUARD_LOOSE_BOMB,
		GUARD_BOMB_ZONE,
		ESCAPE_FROM_BOMB,
		HOLD_POSITION,
		FOLLOW,
		VIP_ESCAPE,
		GUARD_VIP_ESCAPE_ZONE,
		COLLECT_HOSTAGES,
		RESCUE_HOSTAGES,
		GUARD_HOSTAGES,
		GUARD_HOSTAGE_RESCUE_ZONE,
		MOVE_TO_LAST_KNOWN_ENEMY_POSITION,
		MOVE_TO_SNIPER_SPOT,
		SNIPING,

		NUM_TASKS
	};

	void SetTask(TaskType task, CBaseEntity *pEntity = nullptr);				// set our current "task"
	TaskType GetTask() const;
	CBaseEntity *GetTaskEntity();
	bool HasCommittedBombsite() const;
	int GetCommittedBombsite() const;
	void SetCommittedBombsite(int zoneIndex);
	void ClearCommittedBombsite();

	// behavior modifiers
	enum DispositionType
	{
		ENGAGE_AND_INVESTIGATE,	// engage enemies on sight and investigate enemy noises
		OPPORTUNITY_FIRE,		// engage enemies on sight, but only look towards enemy noises, dont investigate
		SELF_DEFENSE,			// only engage if fired on, or very close to enemy
		IGNORE_ENEMIES,			// ignore all enemies - useful for ducking around corners, running away, etc

		NUM_DISPOSITIONS
	};

	void SetDisposition(DispositionType disposition);			// define how we react to enemies
	DispositionType GetDisposition() const;						// return enum describing current disposition

	void IgnoreEnemies(float duration);							// ignore enemies for a short duration

	enum MoraleType
	{
		TERRIBLE = -3,
		BAD = -2,
		NEGATIVE = -1,
		NEUTRAL = 0,
		POSITIVE = 1,
		GOOD = 2,
		EXCELLENT = 3,
	};

	MoraleType GetMorale() const;								// return enum describing current morale
	void IncreaseMorale();
	void DecreaseMorale();

	// listening for noises
	bool IsNoiseHeard() const;									// return true if we have heard a noise
	bool ShouldInvestigateNoise(float *retNoiseDist = nullptr);
	void InvestigateNoise();									// investigate recent enemy noise
	const Vector *GetNoisePosition() const;						// return position of last heard noise, or NULL if none heard
	CNavArea *GetNoiseArea() const;								// return area where noise was heard
	float GetNoiseHeardTimestamp() const;						// gpGlobals->time when last noise was recorded (<=0 if none)
	void ForgetNoise();											// clear the last heard noise
	bool CanSeeNoisePosition() const;							// return true if we directly see where we think the noise came from
	float GetNoiseRange() const;								// return approximate distance to last noise heard

	bool CanHearNearbyEnemyGunfire(float range = -1.0f) const;	// return true if we hear nearby threatening enemy gunfire within given range (-1 == infinite)
	PriorityType GetNoisePriority() const;						// return priority of last heard noise
	// Humanization: extra delay chosen once when non-urgent noise is recorded (see AssignNoiseHumanReactionDelayOnStimulus).
	float GetNoiseHumanDelay() const { return m_noiseHumanDelay; }
	bool GetNoiseHumanDelayApplied() const { return m_noiseHumanDelayApplied; }
	const char *GetNoiseHumanDelayDebugWhy() const { return m_noiseHumanDelayDebugWhy; }
	float GetEffectiveNoiseReactionTime() const;					// profile base + human delay when applicable

	// radio and chatter
	void SendRadioMessage(GameEventType event);					// send voice chatter
	BotChatterInterface *GetChatter();							// return an interface to this bot's chatter system
	bool RespondToHelpRequest(CBasePlayer *them, Place place, float maxRange = -1.0f);	// decide if we should move to help the player, return true if we will
	void StartVoiceFeedback(float duration);
	bool IsUsingVoice() const;									// new-style "voice" chatter gets voice feedback

	// enemies
	// BOTPORT: GetEnemy() collides with GetEnemy() in CBaseEntity - need to use different nomenclature

	void SetEnemy(CBasePlayer *pEnemy);							// set given player as our current enemy
	CBasePlayer *GetEnemy();
	int GetNearbyEnemyCount() const;							// return max number of nearby enemies we've seen recently
	unsigned int GetEnemyPlace() const;							// return location where we see the majority of our enemies
	bool CanSeeBomber() const;									// return true if we can see the bomb carrier
	CBasePlayer *GetBomber() const;

	int GetNearbyFriendCount() const;							// return number of nearby teammates
	CBasePlayer *GetClosestVisibleFriend() const;				// return the closest friend that we can see
	CBasePlayer *GetClosestVisibleHumanFriend() const;			// return the closest human friend that we can see

	bool IsOutnumbered() const;									// return true if we are outnumbered by enemies
	int OutnumberedCount() const;								// return number of enemies we are outnumbered by

	#define ONLY_VISIBLE_ENEMIES true
	CBasePlayer *GetImportantEnemy(bool checkVisibility = false) const;			// return the closest "important" enemy for the given scenario (bomb carrier, VIP, hostage escorter)

	void UpdateReactionQueue();									// update our reaction time queue
	CBasePlayer *GetRecognizedEnemy();							// return the most dangerous threat we are "conscious" of
	bool IsRecognizedEnemyReloading();							// return true if the enemy we are "conscious" of is reloading
	bool IsRecognizedEnemyProtectedByShield();					// return true if the enemy we are "conscious" of is hiding behind a shield
	float GetRangeToNearestRecognizedEnemy();					// return distance to closest enemy we are "conscious" of

	CBasePlayer *GetAttacker() const;							// return last enemy that hurt us
	float GetTimeSinceAttacked() const;							// return duration since we were last injured by an attacker
	float GetFirstSawEnemyTimestamp() const;					// time since we saw any enemies
	float GetLastSawEnemyTimestamp() const;
	float GetTimeSinceLastSawEnemy() const;
	float GetTimeSinceAcquiredCurrentEnemy() const;
	bool IsCombatTargetCommitActive() const;
	float GetCombatTargetCommitRemain() const;
	void ClearCombatTargetCommitState();
	void MaybeLogCombatTargetCommitBlock(CBasePlayer *candidate, CBasePlayer *current);
	// Diff-only movement / oscillation telemetry (watched bot + bot_debug only; no gameplay effect).
	void MaybeLogMovementDecisionOscillationDebug();
	bool HasNotSeenEnemyForLongTime() const;					// return true if we haven't seen an enemy for "a long time"
	const Vector &GetLastKnownEnemyPosition() const;
	bool IsEnemyVisible() const;								// is our current enemy visible
	float GetEnemyDeathTimestamp() const;
	bool IsFriendInLineOfFire();								// return true if a friend is in our weapon's way
	bool IsAwareOfEnemyDeath() const;							// return true if we *noticed* that our enemy died
	int GetLastVictimID() const;								// return the ID (entindex) of the last victim we killed, or zero

#ifdef REGAMEDLL_ADD
	bool CanSeeSniper(void) const;							///< return true if we can see an enemy sniper
	bool HasSeenSniperRecently(void) const;					///< return true if we have seen a sniper recently
#endif

	// navigation
	bool HasPath() const;
	void DestroyPath(MoveDbgPathClearReason reason = MoveDbgPathClearReason::Unknown, const char *srcFunc = nullptr);
	void AssignMovementGoalPosition(const Vector &pos, MoveDbgGoalSource src, const char *srcFunc = nullptr);
	unsigned GetPathDestAreaIDForDebug() const;
	const Vector &GetMovementGoalPosition() const { return m_goalPosition; }

	float GetFeetZ() const;										// return Z of bottom of feet

	void OnDestroyNavDataNotify(NavNotifyDestroyType navNotifyType, void *dead);
	void RemovePath(CNavArea *area);
	void RemoveHidingSpot(HidingSpot *spot);

	enum PathResult
	{
		PROGRESSING,	// we are moving along the path
		END_OF_PATH,	// we reached the end of the path
		PATH_FAILURE,	// we failed to reach the end of the path
	};
	#define NO_SPEED_CHANGE false
	PathResult UpdatePathMovement(bool allowSpeedChange = true);				// move along our computed path - if allowSpeedChange is true, bot will walk when near goal to ensure accuracy

	bool AStarSearch(CNavArea *startArea, CNavArea *goalArea);					// find shortest path from startArea to goalArea - don't actually buid the path
	bool ComputePath(CNavArea *goalArea, const Vector *goal, RouteType route);	// compute path to goal position
	bool StayOnNavMesh();
	CNavArea *GetLastKnownArea() const;									// return the last area we know we were inside of
	const Vector &GetPathEndpoint() const;								// return final position of our current path
	float GetPathDistanceRemaining() const;								// eturn estimated distance left to travel along path
	void ResetStuckMonitor();
	bool IsNavStuck() const { return m_isStuck; }
	bool IsAreaVisible(CNavArea *area) const;							// is any portion of the area visible to this bot
	const Vector &GetPathPosition(int numpath) const;
	bool GetSimpleGroundHeightWithFloor(const Vector *pos, float *height, Vector *normal = nullptr);	// find "simple" ground height, treating current nav area as part of the floor

	Place GetPlace() const;												// get our current radio chatter place

	bool IsUsingLadder() const;											// returns true if we are in the process of negotiating a ladder
	void GetOffLadder();

	void SetGoalEntity(CBaseEntity *pEntity);

	template <typename T = CBaseEntity>
	T *GetGoalEntity();

	bool IsNearJump() const;											// return true if nearing a jump in the path
	float GetApproximateFallDamage(float height) const;					// return how much damage will will take from the given fall height

	void ForceRun(float duration);										// force the bot to run if it moves for the given duration
	void Wiggle();														// random movement, for getting un-stuck

	bool IsFriendInTheWay(const Vector *goalPos) const;					// return true if a friend is between us and the given position
	void FeelerReflexAdjustment(Vector *goalPosition);					// do reflex avoidance movements if our "feelers" are touched

	// looking around / aim (see cs_bot_aim_controller.cpp + cs_bot_vision.cpp):
	// SetLookAngles sets desired m_lookYaw/m_lookPitch; UpdateLookAngles integrates to pev->v_angle.
	// Roaming targets: UpdateLookAround / UpdateRoamLook / SetLookAt. Movement look-ahead: path (m_lookAhead*).
	void SetLookAngles(float yaw, float pitch);			// set our desired look angles
	// Final integration to pev->v_angle: spring/damper extracted as HumanAimMotorController in cs_bot_vision.cpp (not a second motor).
	void UpdateLookAngles();							// move actual view angles towards desired ones
	void UpdateAimController();							// update combat and non-combat view intent
	void UpdateCombatAimController();					// update phase-driven enemy aim
	void UpdateNonCombatAimController();				// update roaming/attention look
	void UpdateRecoilControlState(bool firingControlledWeapon, float skill, float invSkill);
	Vector GetRecoilControlOffset(const Vector &desiredAimSpot, bool firingControlledWeapon, float skill, float invSkill);
	void UpdateLookAround(bool updateNow = false);		// update "looking around" mechanism
	// Moving roam glance *decision* is implemented in LookTargetSelector (see cs_bot_look_target_selector.*); applied here via SetLookAt/ClearLookAt.
	bool UpdateRoamLook(bool updateNow = false);		// update intentional moving glances
	// Passive debug for roam POI settle hold (todo7); read only under bot_debug / PrintIfWatched.
	float GetDebugRoamPoiHoldRemain() const { return m_roamPoiHoldDebugRemain; }
	const char *GetDebugRoamPoiHoldYieldTag() const { return m_roamPoiHoldDebugYieldTag; }
	bool GetDebugRoamPoiHoldActive() const { return m_roamPoiHoldActive; }
	unsigned char GetDebugRoamPoiHoldSource() const { return m_roamPoiHoldSource; }
	float GetDebugRoamPoiHoldScore() const { return m_roamPoiHoldScore; }
	void InhibitLookAround(float duration);				// block all "look at" and "looking around" behavior for given duration - just look ahead
	// Non-combat idle/roam: commit windows and anti–immediate-backtrack between glance sectors.
	bool IsIdleLookCommitActive() const;
	void BeginIdleLookCommit(float acceptedYaw, bool isMoving);
	void CancelIdleLookCommit();
	bool WouldIdleLookPingPong(float candidateYaw) const;
	bool ShouldOverrideIdleLookCommit() const;
	static float ApplyIdleLookYawVariation(float yawDeg);
	float YawDegreesToWorldPos(const Vector &worldAim) const;
	Vector PerturbIdleLookWorldPosition(const Vector &worldAim) const;
	// Maps/<map>.camp_look.json rotation (multiple valid spots at one hold); used by BotCampLookSpots.
	unsigned CampLookRotateSlot(int validCount) const;
	void CampLookRotateAdvance();

	// TODO: Clean up notion of "forward angle" and "look ahead angle"
	void SetForwardAngle(float angle);					// define our forward facing
	void SetLookAheadAngle(float angle);				// define default look ahead angle

	// Structured snapshot of current SetLookAt / m_lookAt* legacy state (for tooling; selection logic unchanged).
	enum LookTargetReason
	{
		LOOK_TARGET_REASON_NONE = 0,
		LOOK_TARGET_REASON_UNKNOWN,
		LOOK_TARGET_REASON_GRENADE_THROW,
		LOOK_TARGET_REASON_ROAM_DANGER,
		LOOK_TARGET_REASON_ROAM_TERRAIN,
		LOOK_TARGET_REASON_ROAM_CORNER,
		LOOK_TARGET_REASON_NOISE,
		LOOK_TARGET_REASON_NOISE_DANGEROUS,
		LOOK_TARGET_REASON_LAST_ENEMY,
		LOOK_TARGET_REASON_SUSPICIOUS_LAST_SEEN,
		LOOK_TARGET_REASON_SUSPICIOUS_ENEMY_ANGLE,
		LOOK_TARGET_REASON_WALL_STARE,
		LOOK_TARGET_REASON_LEARNED_DANGER,
		LOOK_TARGET_REASON_OPEN_LANE,
		LOOK_TARGET_REASON_APPROACH_POINT,
		LOOK_TARGET_REASON_ENCOUNTER_SPOT,
		LOOK_TARGET_REASON_HOSTAGE,
		LOOK_TARGET_REASON_NEARBY_GUNFIRE,
		LOOK_TARGET_REASON_PLANT_BOMB,
		LOOK_TARGET_REASON_DEFUSE_BOMB,
		LOOK_TARGET_REASON_USE_ENTITY,
		LOOK_TARGET_REASON_BREAKABLE,
		LOOK_TARGET_REASON_PANIC,
		LOOK_TARGET_REASON_POST_BUY,
		LOOK_TARGET_REASON_RECORDED_CAMP
	};
	enum LookTargetPhase
	{
		LOOK_TARGET_PHASE_INACTIVE = 0,
		LOOK_TARGET_PHASE_TOWARDS_SPOT,
		LOOK_TARGET_PHASE_AT_SPOT
	};
	struct LookTarget
	{
		bool isActive;
		Vector position;
		PriorityType priority;
		LookTargetReason reason;
		const char *debugDesc;
		LookTargetPhase phase;
		float startTime;
		float durationSeconds;
		float computedEndTime;
		float angleTolerance;
		bool clearIfClose;
		// True while a look is active and its priority is strictly above PRIORITY_LOW, i.e. SetLookAt would ignore a strictly lower-priority request (see first guard in SetLookAt).
		bool rejectsLowerPriorityTargets;
	};
	const LookTarget &GetLookTarget() const { return m_lookTarget; }

	// look at the given point in space for the given duration (-1 means forever)
	void SetLookAt(const char *desc, const Vector *pos, PriorityType pri, float duration = -1.0f, bool clearIfClose = false, float angleTolerance = 5.0f);
	// After tactical stationary hide/camp looks, defer Avoid Wall Stare briefly (see UpdateLookAround).
	void UpdateAvoidWallStareSuppressionForTacticalLook(const char *desc, PriorityType pri, float duration);
	void ClearLookAt();												// stop looking at a point in space and just look ahead
	bool IsLookingAtSpot(PriorityType pri = PRIORITY_LOW) const;	// return true if we are looking at spot with equal or higher priority
	bool IsViewMoving(float angleVelThreshold = 1.0f) const;		// returns true if bot's view angles are rotating (not still)

	// Stationary camp/hold: nudge horizontal aim toward visibility asymmetry (choke/corner edge). Implemented in cs_bot_vision.cpp.
	struct CampEdgeBiasTelemetry
	{
		float baseYaw{};
		float finalYaw{};
		float yawOffset{};
		float baseTraceDist{};
		float finalTraceDist{};
		float edgeScoreBase{};
		float edgeScoreBest{};
		bool applied{};
		char rejectReason[64]{};
	};
	// If return is false, *inOutAimWorld unchanged (or invalid input). Optional telemetry for logging.
	bool AdjustStationaryCampLookForEdgeBias(Vector *inOutAimWorld, CampEdgeBiasTelemetry *tel = nullptr) const;

	const Vector &GetEyePosition() const
	{
		m_eyePos = pev->origin + pev->view_ofs;
		return m_eyePos;
	}
	float ComputeWeaponSightRange();					// return line-of-sight distance to obstacle along weapon fire ray
	bool IsSignificantlyCloser(const CBasePlayer *testPlayer, const CBasePlayer *referencePlayer) const;	// return true if testPlayer is significantly closer than referencePlayer

	// approach points
	void ComputeApproachPoints();						// determine the set of "approach points" representing where the enemy can enter this region
	void UpdateApproachPoints();						// recompute the approach point set if we have moved far enough to invalidate the current ones
	void ClearApproachPoints();
	void DrawApproachPoints();										// for debugging
	float GetHidingSpotCheckTimestamp(HidingSpot *spot) const;		// return time when given spot was last checked
	void SetHidingSpotCheckTimestamp(HidingSpot *spot);				// set the timestamp of the given spot to now

	// weapon query and equip
	#define MUST_EQUIP true
	void EquipBestWeapon(bool mustEquip = false);	// equip the best weapon we are carrying that has ammo
	void EquipPistol(bool mustEquip = false);		// equip our pistol
	void EquipKnife();								// equip our knife

	#define DONT_USE_SMOKE_GRENADE true
	bool EquipGrenade(bool noSmoke = false);		// equip a grenade, return false if we cant

	bool IsUsingKnife() const;						// returns true if we have knife equipped
	bool IsUsingPistol() const;						// returns true if we have pistol equipped
	bool IsUsingGrenade() const;					// returns true if we have grenade equipped
	bool IsUsingSniperRifle() const;				// returns true if using a "sniper" rifle
	bool IsUsingAWP() const;						// returns true if we have AWP equipped

	bool IsSniper() const;							// return true if we have a sniper rifle in our inventory
	bool IsSniping() const;							// return true if we are actively sniping (moving to sniper spot or settled in)
	bool IsUsingShotgun() const;					// returns true if using a shotgun
	bool IsUsingMachinegun() const;					// returns true if using the big 'ol machinegun
	void ThrowGrenade(const Vector *target);		// begin the process of throwing the grenade
	bool IsThrowingGrenade() const;					// return true if we are in the process of throwing a grenade
	bool HasGrenade() const;						// return true if we have a grenade in our inventory
	bool HasGrenadeKind(int weaponId) const;				// WEAPON_HEGRENADE / FLASHBANG / SMOKEGRENADE
	bool EquipGrenadeKind(int weaponId);					// select that grenade from the slot chain
	bool TryTacticalGrenadeThrow();					// mid-round intent: smoke site, flash peek, HE danger.json
	bool TryManualHEGrenadeThrow();					// proactive pre-nade from maps/<map>.grenade.json
	bool FindFlashbangPeekTarget(Vector *pos);				// choke peek arc (path + blind offset)
	// True if throwing/nading toward aimPoint would likely hit or blind a same-team player near the aim or along the line of throw.
	bool WouldGrenadeAimRiskTeammates(const Vector &aimPoint, int grenadeWeaponId) const;

	bool DoesActiveWeaponHaveSilencer() const;
	bool IsUsingHEGrenade() const;
	bool IsUsingFlashbang() const;
	bool IsUsingSmokeGrenade() const;
	void StartRapidFire();
	void StopRapidFire();
	bool IsRapidFiring() const;

	enum ZoomType { NO_ZOOM, LOW_ZOOM, HIGH_ZOOM };
	ZoomType GetZoomLevel() const;					// return the current zoom level of our weapon

	bool AdjustZoom(float range);					// change our zoom level to be appropriate for the given range

	bool IsPrimaryWeaponEmpty() const;				// return true if primary weapon doesn't exist or is totally out of ammo
	bool IsPistolEmpty() const;						// return true if secondary weapon doesn't exist or is totally out of ammo

	int GetHostageEscortCount() const;
	void IncreaseHostageEscortCount();
	float GetRangeToFarthestEscortedHostage() const;
	void ResetWaitForHostagePatience();
	void ResetValues();								// reset internal data to initial state
	void BotDeathThink();
	CBasePlayer *FindNearbyPlayer();
	void AdjustSafeTime();							// called when enemy seen to adjust safe time for this round
	void EXPORT BotTouch(CBaseEntity *pOther);
	bool HasAnyAmmo(CBasePlayerWeapon *weapon) const;

protected:
	void PostBotThinkAfterUpdate() override;

private:
	friend class CCSBotManager;

	// TODO: Get rid of these
	friend class AttackState;
	friend class BuyState;
	friend class HideState;
	friend void NonCombatAimDrift::ApplySinusoidalDriftToDesiredLook(CCSBot *bot);
	friend void NonCombatAimDrift::ApplySampledMicroOffsetsBeforeMotor(CCSBot *bot, float &useYaw, float &usePitch, float &stiffness, float &maxAccel);
	friend LookTargetSelector::RoamLookDecision LookTargetSelector::SelectRoamMovingGlance(const LookTargetSelector::RoamLookContext &, CCSBot *);

	// Goal/path source logging (cs_bot_movement_debug.cpp). No gameplay effect.
	void LogMovementNavDebug(const char *ev, MoveDbgGoalSource goalSrc, MoveDbgPathClearReason pathClear,
		const char *srcFunc, const Vector &oldGoal, unsigned oldPathDest, const Vector &newGoal, unsigned newPathDest);
	void BuildMovementLowCertTags(char *buf, size_t bufSize) const;
	void AssignNoiseHumanReactionDelayOnStimulus(PriorityType priority);

	char m_name[64];						// copied from STRING(pev->netname) for debugging

	// behavior properties
	float m_combatRange;					// desired distance between us and them during gunplay
	mutable bool m_isRogue;					// if true, the bot is a "rogue" and listens to no-one
	mutable CountdownTimer m_rogueTimer;
	MoraleType m_morale;					// our current morale, based on our win/loss history
	bool m_diedLastRound;					// true if we died last round
	float m_safeTime;						// duration at the beginning of the round where we feel "safe"
	float m_safeTimeKnifeSwitchTimestamp;	// when this bot may switch to knife during safe time
	bool m_safeTimeWantsKnife;				// whether this bot wants knife movement this round
	bool m_wasSafe;							// true if we were in the safe time last update
	NavRelativeDirType m_blindMoveDir;		// which way to move when we're blind
	bool m_blindFire;						// if true, fire weapon while blinded

	// TODO: implement through CountdownTimer
	float m_surpriseDelay;					// when we were surprised
	float m_surpriseTimestamp;

	bool m_isFollowing;						// true if we are following someone
	EntityHandle<CBasePlayer> m_leader;		// the ID of who we are following
	float m_followTimestamp;				// when we started following
	float m_allowAutoFollowTime;			// time when we can auto follow

	CountdownTimer m_hurryTimer;			// if valid, bot is in a hurry

	// instances of each possible behavior state, to avoid dynamic memory allocation during runtime
	IdleState m_idleState;
	HuntState m_huntState;
	AttackState m_attackState;
	InvestigateNoiseState m_investigateNoiseState;
	BuyState m_buyState;
	MoveToState m_moveToState;
	FetchBombState m_fetchBombState;
	PlantBombState m_plantBombState;
	DefuseBombState m_defuseBombState;
	HideState m_hideState;
	EscapeFromBombState m_escapeFromBombState;
	FollowState m_followState;
	UseEntityState m_useEntityState;

	// TODO: Allow multiple simultaneous state machines (look around, etc)
	void SetState(BotState *state);				// set the current behavior state
	BotState *m_state;							// current behavior state
	float m_stateTimestamp;						// time state was entered
	bool m_isAttacking;							// if true, special Attack state is overriding the state machine

	TaskType m_task;							// our current task
	EntityHandle<CBaseEntity> m_taskEntity;		// an entity used for our task
	int m_committedBombsite;					// stable round attack/defense zone intent
	float m_committedBombsiteTimestamp;			// when the zone intent was chosen

	// navigation
	Vector m_goalPosition;
	EHandle m_goalEntity;
	void MoveTowardsPosition(const Vector *pos);				// move towards position, independant of view angle
	void MoveAwayFromPosition(const Vector *pos);				// move away from position, independant of view angle
	void StrafeAwayFromPosition(const Vector *pos);				// strafe (sidestep) away from position, independant of view angle
	void StuckCheck();											// check if we have become stuck

	CNavArea *m_currentArea;					// the nav area we are standing on
	CNavArea *m_lastKnownArea;					// the last area we were in
	EntityHandle<CBasePlayer> m_avoid;			// higher priority player we need to make way for
	float m_avoidTimestamp;
	bool m_isJumpCrouching;
	bool m_isJumpCrouched;
	float m_jumpCrouchTimestamp;

	// path navigation data
	enum { MAX_PATH_LENGTH = 256 };
	struct ConnectInfo
	{
		CNavArea *area;				// the area along the path
		NavTraverseType how;		// how to enter this area from the previous one
		Vector pos;					// our movement goal position at this point in the path
		const CNavLadder *ladder;	// if "how" refers to a ladder, this is it
	}
	m_path[MAX_PATH_LENGTH];
	int m_pathLength;
	int m_pathIndex;
	float m_areaEnteredTimestamp;
	void BuildTrivialPath(const Vector *goal);									// build trivial path to goal, assuming we are already in the same area
	bool FindGrenadeTossPathTarget(Vector *pos);

	CountdownTimer m_repathTimer;												// must have elapsed before bot can pathfind again

	bool ComputePathPositions();												// determine actual path positions bot will move between along the path
	void SetupLadderMovement();
	void SetPathIndex(int newIndex);											// set the current index along the path
	void DrawPath();
	int FindOurPositionOnPath(Vector *close, bool local = false) const;												// compute the closest point to our current position on our path
	int FindPathPoint(float aheadRange, Vector *point, int *prevIndex = nullptr);									// compute a point a fixed distance ahead along our path.
	bool FindClosestPointOnPath(const Vector *worldPos, int startIndex, int endIndex, Vector *close) const;			// compute closest point on path to given point
	bool IsStraightLinePathWalkable(const Vector *goal) const;														// test for un-jumpable height change, or unrecoverable fall

	mutable CountdownTimer m_avoidFriendTimer;	// used to throttle how often we check for friends in our path
	mutable bool m_isFriendInTheWay;			// true if a friend is blocking our path
	CountdownTimer m_politeTimer;				// we'll wait for friend to move until this runs out
	bool m_isWaitingBehindFriend;				// true if we are waiting for a friend to move

	#define ONLY_JUMP_DOWN true
	bool DiscontinuityJump(float ground, bool onlyJumpDown = false, bool mustJump = false);					// check if we need to jump due to height change

	enum LadderNavState
	{
		APPROACH_ASCENDING_LADDER,		// prepare to scale a ladder
		APPROACH_DESCENDING_LADDER,		// prepare to go down ladder
		FACE_ASCENDING_LADDER,
		FACE_DESCENDING_LADDER,
		MOUNT_ASCENDING_LADDER,			// move toward ladder until "on" it
		MOUNT_DESCENDING_LADDER,		// move toward ladder until "on" it
		ASCEND_LADDER,					// go up the ladder
		DESCEND_LADDER,					// go down the ladder
		DISMOUNT_ASCENDING_LADDER,		// get off of the ladder
		DISMOUNT_DESCENDING_LADDER,		// get off of the ladder
		MOVE_TO_DESTINATION,			// dismount ladder and move to destination area
	}
	m_pathLadderState;
	bool m_pathLadderFaceIn;						// if true, face towards ladder, otherwise face away
	const CNavLadder *m_pathLadder;					// the ladder we need to use to reach the next area
	bool UpdateLadderMovement();					// called by UpdatePathMovement()
	NavRelativeDirType m_pathLadderDismountDir;		// which way to dismount
	float m_pathLadderDismountTimestamp;			// time when dismount started
	float m_pathLadderEnd;							// if ascending, z of top, if descending z of bottom
	void ComputeLadderEndpoint(bool isAscending);
	float m_pathLadderTimestamp;					// time when we started using ladder - for timeout check

	CountdownTimer m_mustRunTimer;					// if nonzero, bot cannot walk

	// game scenario mechanisms
	CSGameState m_gameState;

	// hostages mechanism
	byte m_hostageEscortCount;
	void UpdateHostageEscortCount();
	float m_hostageEscortCountTimestamp;
	bool m_isWaitingForHostage;
	CountdownTimer m_inhibitWaitingForHostageTimer;
	CountdownTimer m_waitForHostageTimer;

	// listening mechanism
	Vector m_noisePosition;				// position we last heard non-friendly noise
	float m_noiseTimestamp;				// when we heard it (can get zeroed)
	CNavArea *m_noiseArea;				// the nav area containing the noise
	float m_noiseCheckTimestamp;
	PriorityType m_noisePriority;		// priority of currently heard noise
	float m_noiseHumanDelay;			// per-stimulus extra reaction time (non-urgent noise only; chosen at record time)
	bool m_noiseHumanDelayApplied;
	char m_noiseHumanDelayDebugWhy[64];
	bool UpdateLookAtNoise();			// return true if we decided to look towards the most recent noise source
	bool m_isNoiseTravelRangeChecked;

	// "looking around" mechanism
	float m_lookAroundStateTimestamp;	// time of next state change
	enum RoamLookPhase
	{
		ROAM_LOOK_FORWARD,
		ROAM_LOOK_DANGER_ANGLE,
		ROAM_LOOK_CORNER_CHECK,
		ROAM_LOOK_TERRAIN_CHECK,
		ROAM_LOOK_RECENTER
	}
	m_roamLookPhase;
	float m_roamLookPhaseTimestamp;		// time of next moving roam glance decision
	float m_lookAheadAngle;				// our desired forward look angle
	float m_lookAheadPitch;				// desired path-following pitch for ramps, drops, and stairs
	float m_pathLookAheadRange;			// variable path look-ahead distance for less robotic navigation
	float m_nextPathLookAheadRangeUpdate;
	float m_forwardAngle;				// our current forward facing direction
	float m_inhibitLookAroundTimestamp;	// time when we can look around again
	// Non-combat idle/roam look commitment (anti ping-pong between glance and recenter / prior sector).
	float m_idleLookCommitUntil;
	float m_lastIdleLookYaw;
	float m_prevIdleLookYaw;
	// gpGlobals->time until Avoid Wall Stare is suppressed after tactical hide stationary looks (Learned Danger, approach/open lane, etc.).
	float m_suppressAvoidWallStareUntil;
	float m_nextAvoidWallStareSkipLogTime; // throttle [lookArbitration] skip lines
	float m_nextWallLookClassifyLogTime;	  // throttle [wallLookClassify] during stationary Hide
	float m_lookLosRejectLogNextTime;		  // throttle [lookLOSReject] per bot
	unsigned int m_campLookRotateCursor;	  // cycles maps/<map>.camp_look.json picks at current hold

	enum LookAtSpotState
	{
		NOT_LOOKING_AT_SPOT,	// not currently looking at a point in space
		LOOK_TOWARDS_SPOT,		// in the process of aiming at m_lookAtSpot
		LOOK_AT_SPOT,			// looking at m_lookAtSpot
		NUM_LOOK_AT_SPOT_STATES
	}
	m_lookAtSpotState;

	Vector m_lookAtSpot;						// the spot we're currently looking at
	PriorityType m_lookAtSpotPriority;
	float m_lookAtSpotDuration;					// how long we need to look at the spot
	float m_lookAtSpotTimestamp;				// when we actually began looking at the spot
	float m_lookAtSpotAngleTolerance;			// how exactly we must look at the spot
	bool m_lookAtSpotClearIfClose;				// if true, the look at spot is cleared if it gets close to us
	const char *m_lookAtDesc;					// for debugging
	LookTarget m_lookTarget;					// mirror of legacy look-at state (kept in sync only via SyncLookTargetFromLegacyState)
	// todo7: non-combat roam POI settle hold only (see LookTargetSelector). Yields to combat/precision looks; does not change motor/drift/path.
	bool m_roamPoiHoldActive;
	float m_roamPoiHoldEndTime;
	Vector m_roamPoiHoldAimPos;
	unsigned char m_roamPoiHoldSource; // LookTargetSelector::RoamLookCandidateSource
	float m_roamPoiHoldScore;
	Vector m_roamPoiHoldCommitOrigin;
	float m_roamPoiHoldDebugRemain;
	const char *m_roamPoiHoldDebugYieldTag;
	// Returns false if look-at was aborted this tick (e.g. LOS rejected for low/medium priority).
	bool UpdateLookAt();
	void SyncLookTargetFromLegacyState();
	void UpdatePeripheralVision();				// update enounter spot timestamps, etc
	float m_peripheralTimestamp;

	enum { MAX_APPROACH_POINTS = 16 };
	Vector m_approachPoint[MAX_APPROACH_POINTS];
	unsigned char m_approachPointCount;
	Vector m_approachPointViewPosition;			// the position used when computing current approachPoint set

	bool BendLineOfSight(const Vector *eye, const Vector *point, Vector *bend) const;	// "bend" our line of sight until we can see the target point. Return bend point, false if cant bend.
	bool FindApproachPointNearestPath(Vector *pos);										// find the approach point that is nearest to our current path, ahead of us

	bool m_isWaitingToTossGrenade;				// lining up throw
	bool m_grenadePinPulled;					// true after holding attack for at least one frame
	bool m_grenadeReleaseSent;					// true after releasing attack; wait for weapon idle to spawn grenade
	bool m_grenadeThrowLogged;					// true once this throw attempt has been written to CSV
	CountdownTimer m_tossGrenadeTimer;			// timeout timer for grenade tossing
	float m_nextTacticalGrenadeTimestamp;			// gpGlobals->time before next intent-based grenade
	float m_grenadeHeldNoThrowSince;			// time when grenade selected but throw wind-up not started (0 = n/a)
	bool m_openingGrenadeChecked;				// true once this bot has rolled its early-round utility intent
	bool m_openingGrenadeWantsThrow;			// about 60% of bots try early HE pressure
	float m_openingGrenadeTimestamp;			// staggered early throw time
	
	// Grenade spot logging info
	Vector m_grenadeThrowTarget;				// target position for current grenade throw
	bool m_grenadeUsedManualSpot;				// true if current throw uses a manual spot
	float m_grenadeManualSpotDistance;			// distance from manual spot position
	int m_grenadeManualSpotIndex;				// index of manual spot used
	char m_grenadeManualSpotName[64];			// name of manual spot used

	SpotEncounter *m_spotEncounter;				// the spots we will encounter as we move thru our current area
	float m_spotCheckTimestamp;					// when to check next encounter spot

	// TODO: Add timestamp for each possible client to hiding spots
	enum { MAX_CHECKED_SPOTS = 64 };
	struct HidingSpotCheckInfo
	{
		HidingSpot *spot;
		float timestamp;
	}
	m_checkedHidingSpot[MAX_CHECKED_SPOTS];
	int m_checkedHidingSpotCount;

	// view angle mechanism
	float m_lookPitch;						// our desired look pitch angle
	float m_lookPitchVel;
	float m_lookYaw;						// our desired look yaw angle
	float m_lookYawVel;
	float m_nonCombatLookYawOffset;			// non-combat sampled offsets (NonCombatAimDrift::ApplySampledMicroOffsetsBeforeMotor)
	float m_nonCombatLookPitchOffset;
	float m_nextNonCombatLookOffsetUpdate;
	float m_nonCombatLookMotorScale;
	float m_nextNonCombatLookMotorUpdate;
	float m_nonCombatLookPitchBias;
	float m_nextNonCombatLookPitchBiasUpdate;

	// aim angle mechanism
	mutable Vector m_eyePos;
	Vector m_aimOffset;						// current error added to victim's position to get actual aim spot
	Vector m_aimOffsetGoal;					// desired aim offset
	float m_aimOffsetTimestamp;				// time of next offset adjustment
	float m_aimSpreadTimestamp;				// time used to determine max spread as it begins to tighten up
	void SetAimOffset(float accuracy);		// set the current aim offset
	void UpdateAimOffset();					// wiggle aim error based on m_accuracy
	Vector m_aimSpot;						// the spot we are currently aiming to fire at
	Vector m_perceivedEnemyPosition;			// remembered enemy position used for humanized aim
	Vector m_perceivedEnemyVelocity;			// remembered enemy velocity used for imperfect lead
	Vector m_aimInertiaSpot;					// smoothed aim target before converting to look angles
	float m_perceivedEnemyTimestamp;
	float m_nextPerceivedEnemyUpdate;
	float m_aimInertiaTimestamp;
	Vector m_aimHandJitter;
	float m_nextAimJitterUpdate;
	Vector m_aimCurveOffset;
	float m_nextAimCurveUpdate;
	float m_aimHumanMotorScale;
	float m_nextAimMotorUpdate;
	Vector m_recoilControlOffset;
	float m_recoilControlStartTimestamp;
	float m_recoilControlLastFireTimestamp;
	float m_nextRecoilControlUpdate;
	float m_recoilControlSide;
	int m_recoilControlWeapon;
	enum CombatAimPhase
	{
		COMBAT_AIM_TRACKING,
		COMBAT_AIM_REACTING,
		COMBAT_AIM_FLICKING,
		COMBAT_AIM_CORRECTING,
		COMBAT_AIM_RECOIL_CONTROL,
		COMBAT_AIM_LOST_TARGET
	}
	m_combatAimPhase;
	float m_combatAimPhaseTimestamp;
	float m_combatAimPhaseEndTimestamp;
	Vector m_combatAimOvershoot;
	float m_combatAimConfidence;
	int m_perceivedEnemyIndex;

	// Aim diagnostics only (read in PostBotThink for CSV/PrintIfWatched). Not used by aim math.
	AimMotorStepDiag m_aimDiagMotorStep;
	float m_aimDiagJitterMag;
	float m_aimDiagAimOffsetMag;
	float m_aimDiagRecoilMag;
	float m_aimDiagDistEnemy;
	bool m_aimDiagRefusedAimCone;
	bool m_aimDiagAcquisitionBurst;
	float m_aimDiagPrintNextTime;
	// FireWeaponAtEnemy aim cone snapshot (same tick as combat CSV when gate runs).
	bool m_aimDiagConeEvaluated;
	float m_aimDiagAimDot;
	float m_aimDiagRequiredAimDot;
	float m_aimDiagAimDotMargin;
	float m_aimDiagConeErrorDeg;
	float m_aimDiagConeToleranceDeg;
	float m_aimDiagConeRatio;
	int m_aimDiagConePass;
	int m_aimDiagDidFire;

	// attack state data
	DispositionType m_disposition;				// how we will react to enemies
	CountdownTimer m_ignoreEnemiesTimer;		// how long will we ignore enemies
	mutable EntityHandle<CBasePlayer> m_enemy;	// our current enemy
	bool m_isEnemyVisible;						// result of last visibility test on enemy
	unsigned char m_visibleEnemyParts;			// which parts of the visible enemy do we see
	Vector m_lastEnemyPosition;					// last place we saw the enemy
	float m_lastSawEnemyTimestamp;
	float m_firstSawEnemyTimestamp;
	float m_currentEnemyAcquireTimestamp;
	// todo10: marginal combat target switches suppressed until this time (gpGlobals->time).
	float m_combatTargetCommitUntil;
	float m_combatTargetCommitDbgNextLog;
	float m_enemyDeathTimestamp;			// if m_enemy is dead, this is when he died
	bool m_isLastEnemyDead;					// true if we killed or saw our last enemy die
	int m_nearbyEnemyCount;					// max number of enemies we've seen recently
	unsigned int m_enemyPlace;				// the location where we saw most of our enemies

	struct WatchInfo
	{
		float timestamp;
		bool isEnemy;
	}
	m_watchInfo[MAX_CLIENTS];
	mutable EntityHandle<CBasePlayer> m_bomber;						// points to bomber if we can see him

	int m_nearbyFriendCount;										// number of nearby teammates
	mutable EntityHandle<CBasePlayer> m_closestVisibleFriend;		// the closest friend we can see
	mutable EntityHandle<CBasePlayer> m_closestVisibleHumanFriend;	// the closest human friend we can see

#ifdef REGAMEDLL_ADD
	IntervalTimer m_attentionInterval;								// time between attention checks
#endif

	CBasePlayer *m_attacker;				// last enemy that hurt us (may not be same as m_enemy)
	float m_attackedTimestamp;				// when we were hurt by the m_attacker

	int m_lastVictimID;						// the entindex of the last victim we killed, or zero
	bool m_isAimingAtEnemy;					// if true, we are trying to aim at our enemy
	bool m_isRapidFiring;					// if true, RunUpkeep() will toggle our primary attack as fast as it can
	IntervalTimer m_equipTimer;				// how long have we had our current weapon equipped
	bool DoEquip(CBasePlayerWeapon *pWeapon);	// equip the given item
	void StartBoltSniperQuickSwitch();
	bool UpdateBoltSniperQuickSwitch();

	void ReloadCheck();						// reload our weapon if we must
	void SilencerCheck();					// use silencer

	float m_fireWeaponTimestamp;
	float m_nextReloadMoveTimestamp;
	int m_reloadMoveDir;
	float m_nextFunKnifeSlashTimestamp;
	float m_funKnifeSlashEndTimestamp;
	int m_boltSniperQuickSwitchState;
	float m_boltSniperQuickSwitchAwayTimestamp;
	float m_boltSniperQuickSwitchReturnTimestamp;

#ifdef REGAMEDLL_ADD
	bool m_isEnemySniperVisible;									///< do we see an enemy sniper right now
	CountdownTimer m_sawEnemySniperTimer;							///< tracking time since saw enemy sniper
#endif

	// reaction time system
	enum { MAX_ENEMY_QUEUE = 20 };
	struct ReactionState
	{
		// NOTE: player position & orientation is not currently stored separately
		EntityHandle<CBasePlayer> player;
		bool isReloading;
		bool isProtectedByShield;
	}
	m_enemyQueue[MAX_ENEMY_QUEUE];				// round-robin queue for simulating reaction times

	byte m_enemyQueueIndex;
	byte m_enemyQueueCount;
	byte m_enemyQueueAttendIndex;				// index of the timeframe we are "conscious" of

	CBasePlayer *FindMostDangerousThreat();		// return most dangerous threat in my field of view (feeds into reaction time queue)

	// stuck detection
	bool m_isStuck;
	float m_stuckTimestamp;					// time when we got stuck
	Vector m_stuckSpot;						// the location where we became stuck
	NavRelativeDirType m_wiggleDirection;
	float m_wiggleTimestamp;
	float m_stuckJumpTimestamp;				// time for next jump when stuck

	enum { MAX_VEL_SAMPLES = 5 };
	float m_avgVel[MAX_VEL_SAMPLES];
	int m_avgVelIndex;
	int m_avgVelCount;
	Vector m_lastOrigin;

	// chatter mechanism
	GameEventType m_lastRadioCommand;			// last radio command we recieved
	void RespondToRadioCommands();
	bool IsRadioCommand(GameEventType event) const;	// returns true if the radio message is an order to do something

	#define NO_FORCE false
	void EndVoiceFeedback(bool force = true);
	float m_lastRadioRecievedTimestamp;			// time we recieved a radio message
	float m_lastRadioSentTimestamp;				// time when we send a radio message
	EntityHandle<CBasePlayer> m_radioSubject;	// who issued the radio message
	Vector m_radioPosition;						// position referred to in radio message
	float m_voiceFeedbackStartTimestamp;
	float m_voiceFeedbackEndTimestamp;			// new-style "voice" chatter gets voice feedback
	BotChatterInterface m_chatter;

	// learn map mechanism
	const CNavNode *m_navNodeList;
	CNavNode *m_currentNode;
	NavDirType m_generationDir;

	enum ProcessType
	{
		PROCESS_NORMAL,
		PROCESS_LEARN,
		PROCESS_ANALYZE_ALPHA,
		PROCESS_ANALYZE_BETA,
		PROCESS_SAVE,
	}
	m_processMode;
	CountdownTimer m_mumbleTimer;
	CountdownTimer m_booTimer;
	CountdownTimer m_relocateTimer;

	CNavNode *AddNode(const Vector *destPos, const Vector *normal, NavDirType dir, CNavNode *source);
	void StartLearnProcess();
	void UpdateLearnProcess();
	bool LearnStep();
	void StartAnalyzeAlphaProcess();
	void UpdateAnalyzeAlphaProcess();
	bool AnalyzeAlphaStep();
	void StartAnalyzeBetaProcess();
	void UpdateAnalyzeBetaProcess();
	bool AnalyzeBetaStep();
	void StartSaveProcess();
	void UpdateSaveProcess();
	void StartNormalProcess();

#ifdef REGAMEDLL_ADD
	bool IsNoticable(const CBasePlayer *pPlayer, unsigned char visibleParts) const;	// return true if we "notice" given player
#endif
};

// Inlines
inline float CCSBot::GetCombatRange() const
{
	return m_combatRange;
}

inline void CCSBot::SetRogue(bool rogue)
{
	m_isRogue = rogue;
}

inline void CCSBot::Hurry(float duration)
{
	m_hurryTimer.Start(duration);
}

inline float CCSBot::GetSafeTime() const
{
	return m_safeTime;
}

inline bool CCSBot::IsCarryingBomb() const
{
	return m_bHasC4;
}

inline bool CCSBot::IsFollowing() const
{
	return m_isFollowing;
}

inline CBasePlayer *CCSBot::GetFollowLeader()
{
	return m_leader;
}

inline float CCSBot::GetFollowDuration() const
{
	return gpGlobals->time - m_followTimestamp;
}

inline bool CCSBot::CanAutoFollow() const
{
	return (gpGlobals->time > m_allowAutoFollowTime);
}

inline void CCSBot::AimAtEnemy()
{
	m_isAimingAtEnemy = true;
}

inline void CCSBot::StopAiming()
{
	m_isAimingAtEnemy = false;
}

inline bool CCSBot::IsAimingAtEnemy() const
{
	return m_isAimingAtEnemy;
}

inline bool CCSBot::IsSurprised() const
{
	if (gpGlobals->time - m_surpriseTimestamp < 5.0f)
		return true;

	return false;
}

inline float CCSBot::GetSurpriseDelay() const
{
	if (!IsSurprised())
		return 0.0f;

	return m_surpriseDelay;
}

inline void CCSBot::ClearSurpriseDelay()
{
	m_surpriseDelay = 0.0f;
	m_surpriseTimestamp = 0.0f;
}

inline float CCSBot::GetStateTimestamp() const
{
	return m_stateTimestamp;
}

inline CSGameState *CCSBot::GetGameState()
{
	return &m_gameState;
}

inline const CSGameState *CCSBot::GetGameState() const
{
	return &m_gameState;
}

inline bool CCSBot::IsAtBombsite()
{
	return (m_signals.GetState() & SIGNAL_BOMB) == SIGNAL_BOMB;
}

inline CCSBot::MoraleType CCSBot::GetMorale() const
{
#ifdef REGAMEDLL_ADD
	if (cv_bot_excellent_morale.value != 0.0f)
		return EXCELLENT;
#endif

	return m_morale;
}

inline bool CCSBot::IsNoiseHeard() const
{
	if (m_noiseTimestamp <= 0.0f)
		return false;

	// primitive reaction time simulation - cannot "hear" noise until reaction time has elapsed
	const float base = GetProfile()->GetReactionTime();
	const float extra = m_noiseHumanDelayApplied ? m_noiseHumanDelay : 0.0f;
	if (gpGlobals->time - m_noiseTimestamp >= base + extra)
		return true;

	return false;
}

inline float CCSBot::GetEffectiveNoiseReactionTime() const
{
	const float base = GetProfile()->GetReactionTime();
	if (m_noiseTimestamp > 0.0f && m_noiseHumanDelayApplied)
		return base + m_noiseHumanDelay;
	return base;
}

inline float CCSBot::GetNoiseHeardTimestamp() const
{
	return m_noiseTimestamp;
}

inline void CCSBot::SetTask(TaskType task, CBaseEntity *pEntity)
{
	m_task = task;
	m_taskEntity = pEntity;
}

inline CCSBot::TaskType CCSBot::GetTask() const
{
	return m_task;
}

inline CBaseEntity *CCSBot::GetTaskEntity()
{
	return m_taskEntity;
}

inline bool CCSBot::HasCommittedBombsite() const
{
	return m_committedBombsite >= 0;
}

inline int CCSBot::GetCommittedBombsite() const
{
	return m_committedBombsite;
}

inline void CCSBot::SetCommittedBombsite(int zoneIndex)
{
	m_committedBombsite = zoneIndex;
	m_committedBombsiteTimestamp = gpGlobals ? gpGlobals->time : 0.0f;
}

inline void CCSBot::ClearCommittedBombsite()
{
	m_committedBombsite = -1;
	m_committedBombsiteTimestamp = 0.0f;
}

inline CNavArea *CCSBot::GetNoiseArea() const
{
	return m_noiseArea;
}

inline void CCSBot::ForgetNoise()
{
	m_noiseTimestamp = 0.0f;
	m_noiseHumanDelay = 0.0f;
	m_noiseHumanDelayApplied = false;
	m_noiseHumanDelayDebugWhy[0] = '\0';
}

inline PriorityType CCSBot::GetNoisePriority() const
{
	return m_noisePriority;
}

inline BotChatterInterface *CCSBot::GetChatter()
{
	return &m_chatter;
}

inline bool CCSBot::IsUsingVoice() const
{
	return (m_voiceFeedbackEndTimestamp != 0.0f);
}

inline CBasePlayer *CCSBot::GetEnemy()
{
	return m_enemy;
}

inline int CCSBot::GetNearbyEnemyCount() const
{
	return Q_min(GetEnemiesRemaining(), m_nearbyEnemyCount);
}

inline unsigned int CCSBot::GetEnemyPlace() const
{
	return m_enemyPlace;
}

inline bool CCSBot::CanSeeBomber() const
{
	return m_bomber.IsValid();
}

inline CBasePlayer *CCSBot::GetBomber() const
{
	return m_bomber;
}

inline int CCSBot::GetNearbyFriendCount() const
{
	return Q_min(GetFriendsRemaining(), m_nearbyFriendCount);
}

inline CBasePlayer *CCSBot::GetClosestVisibleFriend() const
{
	return m_closestVisibleFriend;
}

inline CBasePlayer *CCSBot::GetClosestVisibleHumanFriend() const
{
	return m_closestVisibleHumanFriend;
}

inline float CCSBot::GetTimeSinceAttacked() const
{
	return gpGlobals->time - m_attackedTimestamp;
}

inline float CCSBot::GetFirstSawEnemyTimestamp() const
{
	return m_firstSawEnemyTimestamp;
}

inline float CCSBot::GetLastSawEnemyTimestamp() const
{
	return m_lastSawEnemyTimestamp;
}

inline float CCSBot::GetTimeSinceLastSawEnemy() const
{
	return gpGlobals->time - m_lastSawEnemyTimestamp;
}

inline float CCSBot::GetTimeSinceAcquiredCurrentEnemy() const
{
	return gpGlobals->time - m_currentEnemyAcquireTimestamp;
}

inline bool CCSBot::IsCombatTargetCommitActive() const
{
	return gpGlobals && m_combatTargetCommitUntil > gpGlobals->time;
}

inline float CCSBot::GetCombatTargetCommitRemain() const
{
	if (!gpGlobals || m_combatTargetCommitUntil <= 0.0f)
		return 0.0f;
	const float r = m_combatTargetCommitUntil - gpGlobals->time;
	return (r > 0.0f) ? r : 0.0f;
}

inline void CCSBot::ClearCombatTargetCommitState()
{
	m_combatTargetCommitUntil = 0.0f;
	m_combatTargetCommitDbgNextLog = 0.0f;
}

inline const Vector &CCSBot::GetLastKnownEnemyPosition() const
{
	return m_lastEnemyPosition;
}

inline bool CCSBot::IsEnemyVisible() const
{
	return m_isEnemyVisible;
}

inline float CCSBot::GetEnemyDeathTimestamp() const
{
	return m_enemyDeathTimestamp;
}

inline int CCSBot::GetLastVictimID() const
{
	return m_lastVictimID;
}

#ifdef REGAMEDLL_ADD
inline bool CCSBot::CanSeeSniper(void) const
{
	return m_isEnemySniperVisible;
}

inline bool CCSBot::HasSeenSniperRecently(void) const
{
	return !m_sawEnemySniperTimer.IsElapsed();
}
#endif

inline bool CCSBot::HasPath() const
{
	return m_pathLength != 0;
}

inline unsigned CCSBot::GetPathDestAreaIDForDebug() const
{
	if (!HasPath() || m_pathLength <= 0 || !m_path[m_pathLength - 1].area)
		return 0;
	return static_cast<unsigned>(m_path[m_pathLength - 1].area->GetID());
}

inline CNavArea *CCSBot::GetLastKnownArea() const
{
	return m_lastKnownArea;
}

inline const Vector &CCSBot::GetPathEndpoint() const
{
	return m_path[m_pathLength - 1].pos;
}

inline const Vector &CCSBot::GetPathPosition(int numpath) const
{
	return m_path[numpath].pos;
}

inline bool CCSBot::IsUsingLadder() const
{
	return m_pathLadder != nullptr;
}

inline void CCSBot::SetGoalEntity(CBaseEntity *pEntity)
{
	m_goalEntity = pEntity;
}

template <typename T>
inline T *CCSBot::GetGoalEntity()
{
	return m_goalEntity.Get<T>();
}

inline void CCSBot::ForceRun(float duration)
{
	Run();
	m_mustRunTimer.Start(duration);
}

inline void CCSBot::SetLookAngles(float yaw, float pitch)
{
	m_lookYaw = yaw;
	m_lookPitch = pitch;
}

inline void CCSBot::SetForwardAngle(float angle)
{
	m_forwardAngle = angle;
}

inline void CCSBot::SetLookAheadAngle(float angle)
{
	m_lookAheadAngle = angle;
}

inline bool CCSBot::IsLookingAtSpot(PriorityType pri) const
{
	if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority >= pri)
		return true;

	return false;
}

inline bool CCSBot::IsViewMoving(float angleVelThreshold) const
{
	if (m_lookYawVel < angleVelThreshold && m_lookYawVel > -angleVelThreshold &&
		m_lookPitchVel < angleVelThreshold && m_lookPitchVel > -angleVelThreshold)
	{
		return false;
	}

	return true;
}

inline bool CCSBot::IsSignificantlyCloser(const CBasePlayer *testPlayer, const CBasePlayer *referencePlayer) const
{
	if (!referencePlayer || !testPlayer)
		return true;

	float testDist = (pev->origin - testPlayer->pev->origin).Length();
	float referenceDist = (pev->origin - referencePlayer->pev->origin).Length();

	const float significantRangeFraction = 0.7f;
	if (testDist < referenceDist * significantRangeFraction)
		return true;

	return false;
}

inline void CCSBot::ClearApproachPoints()
{
	m_approachPointCount = 0;
}

inline bool CCSBot::IsThrowingGrenade() const
{
	return m_isWaitingToTossGrenade;
}

inline void CCSBot::StartRapidFire()
{
	m_isRapidFiring = true;
}

inline void CCSBot::StopRapidFire()
{
	m_isRapidFiring = false;
}

inline CCSBot::ZoomType CCSBot::GetZoomLevel() const
{
	if (m_iFOV > 60.0f)
		return NO_ZOOM;

	if (m_iFOV > 25.0f)
		return LOW_ZOOM;

	return HIGH_ZOOM;
}

inline int CCSBot::GetHostageEscortCount() const
{
	return m_hostageEscortCount;
}

inline void CCSBot::IncreaseHostageEscortCount()
{
	m_hostageEscortCount++;
}

inline void CCSBot::ResetWaitForHostagePatience()
{
	m_isWaitingForHostage = false;
	m_inhibitWaitingForHostageTimer.Invalidate();
}

inline float CCSBot::GetFeetZ() const
{
	if (IsCrouching())
	{
		const Vector crouch(0, 0, -StepHeight);
		return (pev->origin + crouch).z;
	}
	else
	{
		const Vector stand(0, 0, -HalfHumanHeight);
		return (pev->origin + stand).z;
	}
}

inline const Vector *CCSBot::GetNoisePosition() const
{
	if (m_noiseTimestamp > 0.0f)
		return &m_noisePosition;

	return nullptr;
}

inline bool CCSBot::IsAwareOfEnemyDeath() const
{
	if (GetEnemyDeathTimestamp() == 0.0f)
		return false;

	if (!m_enemy.IsValid())
		return true;

	if (!m_enemy->IsAlive() && gpGlobals->time - GetEnemyDeathTimestamp() > (1.0f - GetProfile()->GetSkill()))
		return true;

	return false;
}

inline bool CCSBot::IsNotMoving() const
{
	const float stillSpeed = 10.0f;
	return pev->velocity.IsLengthLessThan(stillSpeed);
}

inline bool CCSBot::HasAnyAmmo(CBasePlayerWeapon *weapon) const
{
	return (weapon->m_iClip != 0 || m_rgAmmo[weapon->m_iPrimaryAmmoType] > 0);
}

class CollectRetreatSpotsFunctor
{
public:
	CollectRetreatSpotsFunctor(CCSBot *me, float range)
	{
		m_me = me;
		m_count = 0;
		m_range = range;
	}

	enum { MAX_SPOTS = 256 };

	bool operator()(CNavArea *area)
	{
		// collect all the hiding spots in this area
		for (auto const spot : *area->GetHidingSpotList())
		{
			if (m_count >= MAX_SPOTS)
				break;

			// make sure hiding spot is in range
			if (m_range > 0.0f)
			{
				if ((*spot->GetPosition() - m_me->pev->origin).IsLengthGreaterThan(m_range))
					continue;
			}

			// if a Player is using this hiding spot, don't consider it
			if (IsSpotOccupied(m_me, spot->GetPosition()))
			{
				// player is in hiding spot
				// TODO: Check if player is moving or sitting still
				continue;
			}

			// don't select spot if an enemy can see it
			if (UTIL_IsVisibleToTeam(*spot->GetPosition() + Vector(0, 0, HalfHumanHeight), OtherTeam(m_me->m_iTeam)))
				continue;

			// don't select spot if it is closest to an enemy
			CBasePlayer *owner = UTIL_GetClosestPlayer(spot->GetPosition());
			if (owner && m_me->m_iTeam != owner->m_iTeam)
				continue;

			m_spot[m_count++] = spot->GetPosition();
		}

		// if we've filled up, stop searching
		if (m_count == MAX_SPOTS)
			return false;

		return true;
	}

	CCSBot *m_me;
	float m_range;

	const Vector *m_spot[MAX_SPOTS];
	int m_count;
};

class FarthestHostage
{
public:
	FarthestHostage(const CCSBot *me)
	{
		m_me = me;
		m_farRange = -1.0f;
	}
	bool operator()(CHostage *hostage)
	{
		if (hostage->IsFollowing(m_me))
		{
			float range = (hostage->Center() - m_me->pev->origin).Length();

			if (range > m_farRange)
			{
				m_farRange = range;
			}
		}

		return true;
	}

	const CCSBot *m_me;
	float m_farRange;
};

// Functor used with NavAreaBuildPath()
class PathCost
{
public:
	PathCost(CCSBot *pBot, RouteType route = SAFEST_ROUTE)
	{
		m_bot = pBot;
		m_route = route;
	}
	float operator()(CNavArea *area, CNavArea *fromArea, const CNavLadder *ladder)
	{
		const float baseDangerFactor = 100.0f;

		// respond to the danger modulated by our aggression (even super-aggressives pay SOME attention to danger)
		float dangerFactor = (1.0f - (0.95f * m_bot->GetProfile()->GetAggression())) * baseDangerFactor;

		if (fromArea == nullptr)
		{
			if (m_route == FASTEST_ROUTE)
				return 0.0f;

			// first area in path, cost is just danger
			return dangerFactor * area->GetDanger(m_bot->m_iTeam - 1);
		}
		else if ((fromArea->GetAttributes() & NAV_JUMP) && (area->GetAttributes() & NAV_JUMP))
		{
			// cannot actually walk in jump areas - disallow moving from jump area to jump area
			return -1.0f;
		}
		else
		{
			// compute distance from previous area to this area
			float dist;
			if (ladder)
			{
				// ladders are slow to use
				const float ladderPenalty = 1.0f;
				dist = ladderPenalty * ladder->m_length;

				// if we are currently escorting hostages, avoid ladders (hostages are confused by them)
				//if (m_bot->GetHostageEscortCount())
				//	dist *= 100.0f;
			}
			else
			{
				dist = (*area->GetCenter() - *fromArea->GetCenter()).Length();
			}

			// compute distance travelled along path so far
			float cost = dist + fromArea->GetCostSoFar();

			// zombies ignore all path penalties
			if (cv_bot_zombie.value > 0.0f)
				return cost;

			// add cost of "jump down" pain unless we're jumping into water
			if (!area->IsConnected(fromArea, NUM_DIRECTIONS))
			{
				// this is a "jump down" (one way drop) transition - estimate damage we will take to traverse it
				float fallDistance = -fromArea->ComputeHeightChange(area);

				// if it's a drop-down ladder, estimate height from the bottom of the ladder to the lower area
				//if (ladder && ladder->m_bottom.z < fromArea->GetCenter()->z && ladder->m_bottom.z > area->GetCenter()->z)
				//{
				//	fallDistance = ladder->m_bottom.z - area->GetCenter()->z;
				//}

				float fallDamage = m_bot->GetApproximateFallDamage(fallDistance);

				if (fallDamage > 0.0f)
				{
					// if the fall would kill us, don't use it
					const float deathFallMargin = 10.0f;
					if (fallDamage + deathFallMargin >= m_bot->pev->health)
						return -1.0f;

					// if we need to get there in a hurry, ignore minor pain
					const float painTolerance = 15.0f * m_bot->GetProfile()->GetAggression() + 10.0f;
					if (m_route != FASTEST_ROUTE || fallDamage > painTolerance)
					{
						// cost is proportional to how much it hurts when we fall
						// 10 points - not a big deal, 50 points - ouch!
						cost += 100.0f * fallDamage * fallDamage;
					}
				}
			}

			// if this is a "crouch" area, add penalty
			if (area->GetAttributes() & NAV_CROUCH)
			{
				// these areas are very slow to move through
				real_t crouchPenalty = (m_route == FASTEST_ROUTE) ? 20.0f : 5.0f;

				// avoid crouch areas if we are rescuing hostages
				if (m_bot->GetHostageEscortCount())
				{
					crouchPenalty *= 3.0f;
				}

				cost += crouchPenalty * dist;
			}

			// if this is a "jump" area, add penalty
			if (area->GetAttributes() & NAV_JUMP)
			{
				// jumping can slow you down
				//const float jumpPenalty = (m_route == FASTEST_ROUTE) ? 100.0f : 0.5f;
				const float jumpPenalty = 1.0f;
				cost += jumpPenalty * dist;
			}

			if (m_route == SAFEST_ROUTE)
			{
				// add in the danger of this path - danger is per unit length travelled
				cost += dist * dangerFactor * area->GetDanger(m_bot->m_iTeam - 1);
			}

			if (!m_bot->IsAttacking())
			{
				// add in cost of teammates in the way
				// approximate density of teammates based on area
				float size = (area->GetSizeX() + area->GetSizeY()) / 2.0f;

				// degenerate check
				if (size >= 1.0f)
				{
					// cost is proportional to the density of teammates in this area
					const float costPerFriendPerUnit = 50000.0f;
					cost += costPerFriendPerUnit * float(area->GetPlayerCount(m_bot->m_iTeam, m_bot)) / size;
				}
			}

			return cost;
		}

		return 0.0f;
	}

private:
	CCSBot *m_bot;
	RouteType m_route;
};

class FollowTargetCollector
{
public:
	FollowTargetCollector(CBasePlayer *pPlayer)
	{
		m_player = pPlayer;
		m_forward.x = pPlayer->pev->velocity.x;
		m_forward.y = pPlayer->pev->velocity.y;

		float speed = m_forward.NormalizeInPlace();

		const float walkSpeed = 100.0f;
		if (speed < walkSpeed)
		{
			m_cutoff.x = pPlayer->pev->origin.x;
			m_cutoff.y = pPlayer->pev->origin.y;

			m_forward.x = 0.0f;
			m_forward.y = 0.0f;
		}
		else
		{
			const float k = 1.5f;
			real_t trimSpeed = (speed < 200.0f) ? speed : 200.0f;

			m_cutoff.x = pPlayer->pev->origin.x + k * trimSpeed * m_forward.x;
			m_cutoff.y = pPlayer->pev->origin.y + k * trimSpeed * m_forward.y;
		}

		m_targetAreaCount = 0;
	}

	enum { MAX_TARGET_AREAS = 128 };
	bool operator()(CNavArea *area)
	{
		if (m_targetAreaCount >= MAX_TARGET_AREAS)
			return false;

		// only use two-way connections
		if (!area->GetParent() || area->IsConnected(area->GetParent(), NUM_DIRECTIONS))
		{
			if (m_forward.IsZero())
			{
				m_targetArea[m_targetAreaCount++] = area;
			}
			else
			{
				// collect areas in the direction of the player's forward motion
				Vector2D to(((*area->GetCenter()).x - m_cutoff.x), (*area->GetCenter()).y - m_cutoff.y);
				to.NormalizeInPlace();

				if (DotProduct(to, m_forward) > 0.7071f)
				{
					m_targetArea[m_targetAreaCount++] = area;
				}
			}
		}

		return (m_targetAreaCount < MAX_TARGET_AREAS);
	}

	CBasePlayer *m_player;
	Vector2D m_forward;
	Vector2D m_cutoff;
	CNavArea *m_targetArea[MAX_TARGET_AREAS];
	int m_targetAreaCount;
};

void InstallBotControl();
void Bot_ServerCommand();
void Bot_RegisterCVars();
int GetBotFollowCount(CBasePlayer *pLeader);
const Vector *FindNearbyRetreatSpot(CCSBot *me, float maxRange);

void drawProgressMeter(float progress, char *title);
void startProgressMeter(const char *title);
void hideProgressMeter();

bool isSniperRifle(CBasePlayerItem *item);
float StayOnLadderLine(CCSBot *me, const CNavLadder *ladder);
