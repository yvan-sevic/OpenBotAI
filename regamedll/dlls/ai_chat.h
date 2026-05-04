#pragma once

#include "game_shared/GameEvent.h"

class CBaseEntity;
class CBasePlayer;

namespace AiChat
{
	// Called from game thread only.
	void OnEvent(GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther);

	// Called from game thread once per frame.
	void Frame();

	// True while an alive bot is in the AI-chat "typing" window (movement should stop).
	bool ShouldFreezeMovementForChat(int entindex);

	// Record a chat line (human or bot) into RECENT_CHAT buffer for future prompts.
	// Called from the game thread (e.g. Host_Say).
	void OnChatLine(CBasePlayer *sender, const char *msg, bool teamonly, bool senderDead, const char *placeName);
}

