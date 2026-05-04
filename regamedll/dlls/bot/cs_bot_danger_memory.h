#pragma once

class CBasePlayer;
class CCSBot;
class Vector;

namespace BotDangerMemory
{
	void LoadForCurrentMap();
	void Save();
	void MaybeAutoSave();

	void RecordSighting(CCSBot *observer, CBasePlayer *enemy);
	void RecordDamage(CBasePlayer *victim, CBasePlayer *attacker);
	void RecordDeath(CBasePlayer *victim, CBasePlayer *killer);
	void RecordEnemyNoise(CBasePlayer *source);

	bool FindBestLookSpot(CCSBot *bot, Vector *spot);
	bool FindBestLookSpotNear(CCSBot *bot, const Vector &nearPos, float maxRange, Vector *spot);
	// Aim point for HE toward high-weight .danger.json threats (enemy-occupied spots).
	bool FindBestHeGrenadeTargetFromMemory(CCSBot *bot, Vector *spot);
	void DrawDebug();
}
