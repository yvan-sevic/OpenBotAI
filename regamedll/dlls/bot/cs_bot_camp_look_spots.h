#pragma once

class CCSBot;
class Vector;

// Manual per-map camp hold angles: maps/<map>.camp_look.json
// Record with: stand at hold position, aim at the angle to watch, then:
//   bot_camp_look_spot [name] [CT|T|any]
namespace BotCampLookSpots
{
	void LoadForCurrentMap();
	void Save();
	void ServerCommandAddSpot();

	bool FindLookTarget(CCSBot *bot, Vector *targetOut, const char **spotNameOut = nullptr);
	// Same eligibility as FindLookTarget, but cycles through all valid spots in JSON order (per-bot cursor).
	bool FindLookTargetRotating(CCSBot *bot, Vector *targetOut, const char **spotNameOut = nullptr, bool advanceCursor = true);
}
