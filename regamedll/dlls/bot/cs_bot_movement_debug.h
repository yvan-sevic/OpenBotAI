#pragma once

// Telemetry only (console + optional file). See cs_bot_movement_debug.cpp.

enum class MoveDbgGoalSource : uint8_t
{
	Unknown = 0,
	BuildTrivialPath,
	ComputePathWaypoint,
	LadderApproach,
	LadderDismount,
	LadderMoveToDestination,
	MapLearn,
};

enum class MoveDbgPathClearReason : uint8_t
{
	Unknown = 0,
	RepathCompute,
	PathFailure,
	ReachedGoal,
	EnemyEngage,
	TaskOrStateChange,
	StuckTimeout,
	ExplicitCancel,
	FriendAvoidRepath,
	LadderFail,
	LadderFallOff,
	FollowPathNotProgressing,
	CombatKnifePathEnd,
	GetOffLadder,
	HideOrRetask,
};

class CCSBot;

// Console (PrintIfWatched) + bot_move_debug.log when movement log is on. Same per-bot gate as [movedbg-nav].
void EmitMovementWatchedDebugLine(CCSBot *bot, const char *line);
