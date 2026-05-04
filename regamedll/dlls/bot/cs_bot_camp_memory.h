#pragma once

class CBaseEntity;
class CBasePlayer;
class CCSBot;
class Vector;

namespace BotCampMemory
{
	void LoadForCurrentMap();
	void Save();
	void MaybeAutoSave();
	void DrawDebug();

	void RecordCampStart(CCSBot *bot, const Vector &spot);
	void RecordDamage(CBasePlayer *victim, CBasePlayer *attacker);
	void RecordDeath(CBasePlayer *victim, CBasePlayer *killer);
	void RecordPathFail(CCSBot *bot, const Vector &spot);

	float GetBadCampWeight(CBaseEntity *bot, const Vector &spot);
	bool IsBadCampSpot(CBaseEntity *bot, const Vector &spot);
}
