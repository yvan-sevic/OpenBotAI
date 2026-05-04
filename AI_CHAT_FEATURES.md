# AI chat — feature checklist (pickup notes)

Short map of what this fork’s **`regamedll/dlls/ai_chat.cpp`** (+ related files) does. For install / cvars / Ollama setup see **`AI_CHAT_README.md`**.

## Core behavior

- **Ollama only** — canned mode removed; **`sv_ai_chat_mode`** removed. **`sv_ai_chat_enable`** toggles everything (default **on** in `game.cpp`).
- **Async HTTP** worker → response queue → **`AiChat::Frame()`** delivers lines; **`ShouldFreezeMovementForChat`** freezes alive bots while “typing”.

## Prompts (Ollama)

- **`system`**: short style/fact rules, hard output rules (raw line, no quotes/`*`/meta, no own name, don’t invent player names), and no copyable dead-chat templates.
- **`user`**: **`GAME_STATE` JSON + `BOT_STATE` JSON**, plus map + CS1.6 grounding, bomb role rules, **`ROUND_SNAPSHOT`** (alive T/CT, bomb planted/defused/exploded), score/round/time, **`RECENT_CHAT`** buffer, event intent, actors / kill roles / kill facts when relevant.
- **Debug**: `sv_ai_debug 1` now prints only **`BOT_STATE`** prompt debug to keep console output readable.

## Rate limits & selection

- **Per-round line budget** per bot (rolled on **`EVENT_ROUND_START`**): 25% / 40% / 25% / 10% → 0 / 1 / 2 / 3 lines; spent on **each queued** line (failed roll does not spend).
- **Speak roll** before queue: normal **~15–30%**; **knife or clutch kill** **~35–60%** (clutch = solo alive vs ≥2 enemies). Mode-1-style extra kill RNG removed.
- **Global gap** between any two delivered lines; **per-bot post-speak cooldown**; stale responses dropped.

## Game hooks (`GameEventType`)

- Wired in **`AiChat::OnEvent`** (see **`game_shared/GameEvent.h`**): round start/outcome, full bomb lifecycle (plant/defuse/explode/defusing/abort/drop/pickup), **`PLAYER_DIED`** (killer / victim / spectator paths), hostages/VIP, grenade pops (shared throttle), flash blind, match commence, **bot** spawn.
- Friendly-fire complaint: repeated team damage on **`EVENT_PLAYER_TOOK_DAMAGE`** (`trace=friendly_fire_repeat`) with ~50% chance (still budgeted).
- Dead-chat replies: bot/human chat feeds **`RECENT_CHAT`**; dead bot mentions can queue replies, and compliment-like mentions have a higher positive-reply chance.
- Round-end reactions: bomb defused/exploded, all hostages rescued, and CT/T round wins can queue rare **praise/blame** lines (`trace=round_end_react`).
- Defuse timing reaction: when a CT starts defusing, compare C4 blow time with 5s/10s defuse time and rarely react (`trace=defuse_timing_react`).
- **Not** wired: footsteps, weapon fire, radio, bullet impact, doors, breakables (too noisy).
- Logs use **`GameEventName[]`** (same table as bots). **`trace=`** tags (`round_start`, `kill_victim`, `bomb_exploded`, …) on **`[ai]`** lines when **`sv_ai_debug 1`**.

## Context memory

- Dead `BOT_STATE` includes last-death facts: `killed_by`, `death_location`, `kill_type`, and weapon.
- Repeated death memory tracks `killed_by_same_attacker_count`, `is_repeat_death_to_same_attacker`, `most_killed_by`, and `most_killed_by_count`.
- Kill prompts include `KILL_FACTS` with the current weapon/kill type and repeat count, so words like “again” have a factual trigger.

## Sanitizers (before send)

- Strip **prompt leaks** / LLM meta (`(Note:`, ` reflects`, `task=`, etc.); trim `*`; self-name / profile name cleanup; map hallucination (e.g. dust vs dust2); polished clichés; non‑CS16 jargon; CT plant claims; **bomb-state / T-defuse** contradictions; alive **self-sitrep** trim.
- Tone/fact control: discourage “wtf”/`lol` loops, block unsupported weapon verbs (`awped`, `famassed`, `gl0cked`, etc.), enforce “again/same” only when repeat facts support it, and keep noisy phrases out of `RECENT_CHAT`.

## Chat routing

- **`sv_ai_chat_scope`**: `auto` | `all` | `team`; **dead bots → always global** (`teamonly=0`); alive **auto** uses **`LooksTactical()`** + event list (includes new bomb/round/hostage events) + length heuristic.
- Optional **`(TEAM)`** prefix via **`sv_ai_chat_team_tag`**.

## Typing freeze

- Alive bots freeze **movement and look/aim** while “typing” (prevents mouse tracking during chat delay).

## Files touched (mental index)

| Area | Files |
|------|--------|
| AI logic | `regamedll/dlls/ai_chat.cpp`, `regamedll/dlls/ai_chat.h` |
| Cvars | `regamedll/dlls/game.cpp`, `regamedll/dlls/game.h` |
| Bot hook | `regamedll/dlls/bot/cs_bot_manager.cpp` → `AiChat::OnEvent` |
| Persona text | `BotProfile.db` + parser in `regamedll/game_shared/bot/bot_profile.cpp` |
| User-facing doc | `AI_CHAT_README.md` |

## Quick resume ideas

- Tune throttles / budgets / speak % in **`ai_chat.cpp`** (`AiChat::OnEvent`, `PassesEventSpeakRoll`, `RollBotRoundChatBudgets`).
- Add more **`GameEventType`** cases in the same `switch` (keep throttles strict).
- Tighten **`Strip*`** / **`EVENT_DETAIL`** strings if the model keeps misbehaving on one scenario.
