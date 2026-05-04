#pragma once

class CCSBot;
class CBasePlayer;
class Vector;

// Manual per-map grenade lineup hints: maps/<map>.grenade.json
// Record with: stand at throw position, aim at landing/pop spot, then bot_grenade_spot <smoke|flash|he|any> [optional_name]
namespace BotGrenadeSpots
{
	void LoadForCurrentMap();
	void Save();

	// Invoked from game console (reads CMD_ARGV).
	void ServerCommandAddSpot();

	// grenadeWeaponId: WEAPON_SMOKEGRENADE, WEAPON_FLASHBANG, WEAPON_HEGRENADE
	// Returns true if found and writes to targetOut. If foundSpotOut is not null, writes spot info.
	struct FoundSpotInfo
	{
		int spotIndex;
		float distanceFromSpot;
		const char *spotName;
	};
	bool FindManualThrowTarget(CCSBot *bot, int grenadeWeaponId, Vector *targetOut, FoundSpotInfo *foundSpotOut = nullptr);
	bool IsNearManualThrowSpot(CCSBot *bot, int grenadeWeaponId, float normalRadius, float priorityRadius);

	// True if this map has at least one manual spot for that grenade and team (ignores distance).
	// botTeam: CT / TERRORIST (TeamName).
	bool HasAnyLineupForGrenadeAndTeam(int grenadeWeaponId, int botTeam);

	// When manual HE lineups exist for this bot, limits danger.json HE fallback (see cvar bot_grenade_he_danger_fallback).
	bool ShouldAttemptDangerHeFallback(CCSBot *bot);

	void DrawDebug();
}
