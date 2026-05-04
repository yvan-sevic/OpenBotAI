# AI Bot Chat (ReGameDLL_CS + Ollama)

This fork adds **Ollama-backed AI bot chat** (async HTTP; does not block the game thread) and optional **`ChatPersona`** lines in **`BotProfile.db`** so each bot can steer LLM/voice tone. Use **`sv_ai_chat_enable`** (default **on** in this build) and the other **`sv_ai_*`** cvars to tune behavior; set **`sv_ai_chat_enable 0`** to turn chat off entirely.

---

## Installation (what to do, in order)

Do these once (or whenever you rebuild / change configs).

1. **Build the GameDLL** from this repo (see [§ Build](#1-build-the-gamedll-cs_so)). Output: `build/regamedll/cs.so`.

2. **Install the DLL into Counter-Strike** (Linux): copy `cs.so` to **`cstrike/dlls/rg.so`** (the game loads **`rg.so`**, not `cs.so`, via `cstrike/liblist.gam`). You need this build so the game **parses `ChatPersona`** in `BotProfile.db` and runs AI chat code.

3. **Install `BotProfile.db`** (optional but recommended): copy this repo’s **`BotProfile.db`** into your game’s **`cstrike/`** folder (same folder as `config.cfg`), or merge the `ChatPersona = ...` lines into your existing file. The file is read at startup; **`ChatPersona` is unknown to stock CS**—use this fork’s `rg.so` or the parser will error.

4. **Configure cvars** in **`cstrike/game_init.cfg`** (or set from console). Defaults enable AI chat; set **`sv_ai_ollama_url`**, **`sv_ai_ollama_model`**, and **`sv_ai_timeout_ms`** as needed (see [§ Configure](#4-configure-cs-cvars-recommended-cstrikegame_initcfg)).

5. Run **Ollama** on the host you pointed **`sv_ai_ollama_url`** at and **warm the model** once (see [§ Ollama](#3-start-ollama)).

6. **Fully quit Counter-Strike** (or restart the dedicated server) and start again. The GameDLL and **`BotProfile.db`** are loaded when the process starts; a running game will not reliably pick up a replaced DLL or DB on disk.

7. **Smoke test**: `game version` → `exec game_init.cfg` (if you edited it) → `bot_add` → watch in-game chat and/or server console with `sv_ai_debug 1`.

Typical Linux paths (adjust to your machine):

- Steam Half-Life root: `~/.local/share/Steam/steamapps/common/Half-Life/`
- GameDLL: `.../Half-Life/cstrike/dlls/rg.so`
- Bot DB: `.../Half-Life/cstrike/BotProfile.db`
- Auto-exec: `.../Half-Life/cstrike/game_init.cfg`

---

## 1) Build the GameDLL (`cs.so`)

From the **repository root** (replace the path with your clone):

```bash
cd /home/kemt/Downloads/ReGameDLL_CS-master && \
rm -rf build && \
CC=gcc CXX=g++ cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -B build && \
cmake --build build -j"$(nproc)"
```

Incremental rebuild after small changes:

```bash
cmake --build build -j"$(nproc)"
```

Build output:

- `build/regamedll/cs.so`

---

## 2) Deploy to your Steam CS install (Linux)

On Linux, CS loads **`cstrike/dlls/rg.so`**. Copy the built library there:

```bash
cp -v /path/to/ReGameDLL_CS-master/build/regamedll/cs.so \
  ~/.local/share/Steam/steamapps/common/Half-Life/cstrike/dlls/rg.so
```

Then **fully restart Counter-Strike** (exit the game process). The DLL stays mapped until the process exits.

---

## 3) Start Ollama

Start the Ollama server:

```bash
ollama serve
```

Verify it’s running:

```bash
curl -sS http://127.0.0.1:11434/api/tags
```

Warm the model once (reduces first-response delay a lot):

```bash
ollama run llama3.1:8b "reply with one short cs chat line"
```

If Ollama runs on another PC, set `sv_ai_ollama_url` accordingly (e.g. `http://192.168.1.50:11434`).

---

## 4) Configure CS cvars (recommended: `cstrike/game_init.cfg`)

Create or edit:

- `Half-Life/cstrike/game_init.cfg` (next to `config.cfg`)

### Example `game_init.cfg` (Ollama)

```cfg
sv_ai_chat_enable 1
sv_ai_ollama_url "http://127.0.0.1:11434"
sv_ai_ollama_model "llama3.1:8b"
sv_ai_timeout_ms 60000
sv_ai_max_chars 80
sv_ai_debug 1
sv_ai_chat_scope "auto"
sv_ai_chat_team_tag 0
```

To disable AI chat entirely:

```cfg
sv_ai_chat_enable 0
```

Notes:

- **`sv_ai_chat_enable`**: defaults to **1** in this repo; set **0** to disable the feature and stop the Ollama worker.
- **`sv_ai_debug 1`**: server console lines prefixed with `[ai]` (useful for delivery / Ollama issues). Each line shows **`trace=`** (which code path queued it) and **`event=`** (game hook from ReGameDLL):

  | `trace` | `GameEventType` (see `game_shared/GameEvent.h`) |
  |--------|---------|
  | `round_start` | `EVENT_ROUND_START` |
  | `bomb_planted` | `EVENT_BOMB_PLANTED` |
  | `bomb_defused` | `EVENT_BOMB_DEFUSED` |
  | `bomb_exploded` | `EVENT_BOMB_EXPLODED` |
  | `bomb_defusing` | `EVENT_BOMB_DEFUSING` |
  | `bomb_defuse_aborted` | `EVENT_BOMB_DEFUSE_ABORTED` |
  | `bomb_dropped` | `EVENT_BOMB_DROPPED` |
  | `bomb_picked_up` | `EVENT_BOMB_PICKED_UP` |
  | `round_outcome` | `EVENT_TERRORISTS_WIN` / `EVENT_CTS_WIN` / `EVENT_ROUND_DRAW` |
  | `vip_escaped` | `EVENT_VIP_ESCAPED` |
  | `vip_assassinated` | `EVENT_VIP_ASSASSINATED` |
  | `hostage_rescued` | `EVENT_HOSTAGE_RESCUED` |
  | `hostage_all_rescued` | `EVENT_ALL_HOSTAGES_RESCUED` |
  | `hostage_killed` | `EVENT_HOSTAGE_KILLED` |
  | `hostage_damaged` | `EVENT_HOSTAGE_DAMAGED` (heavily throttled) |
  | `grenade_exploded` | `EVENT_HE_GRENADE_EXPLODED` / `EVENT_FLASHBANG_GRENADE_EXPLODED` / `EVENT_SMOKE_GRENADE_EXPLODED` |
  | `player_flashed` | `EVENT_PLAYER_BLINDED_BY_FLASHBANG` |
  | `game_commence` | `EVENT_NEW_MATCH` / `EVENT_GAME_COMMENCE` |
  | `player_spawned` | `EVENT_PLAYER_SPAWNED` (bot only) |
  | `friendly_fire_repeat` | `EVENT_PLAYER_TOOK_DAMAGE` (bot victim; repeated team damage) |
  | `kill_alive_killer` | `EVENT_PLAYER_DIED` — alive killer bot |
  | `kill_victim` | `EVENT_PLAYER_DIED` — victim bot |
  | `kill_dead_spectator` | `EVENT_PLAYER_DIED` — dead spec bot |
  | `kill_teammate_react` | `EVENT_PLAYER_DIED` — dead teammate reaction |
  | `chat_mention_reply` / `chat_compliment_reply` | Dead-chat reply after a bot mention |
  | `dead_chat_reply` | Dead-chat back-and-forth |
  | `round_end_react` | Extra praise/blame after round-ending outcomes |
  | `defuse_timing_react` | Rare reaction when a CT starts a timed defuse |

  Bots do **not** run a separate “camping detector”: phrasing like “camping” comes from the **LLM** interpreting **`ROUND_SNAPSHOT`** / map context in the user prompt. If the model contradicts live counts, delivery filters may drop the line.
- **`sv_ai_chat_team_tag 1`**: prefixes team-only AI lines with `(TEAM)` in chat (still team-filtered for enemies).
- **`sv_ai_chat_scope`**: `"auto"` | `"all"` | `"team"` — controls default all vs team for events (see `ai_chat.cpp` for current auto rules).
- **Throttling** (`regamedll/dlls/ai_chat.cpp`): per-round **line budgets**, per-event **speak probability** (higher on knife / clutch kills), global gaps, and per-bot cooldowns. After Ollama returns, **alive** bots use a **reaction** window then **simulated typing** ≈ **0.20–0.30 seconds per character** (capped).
- **Debug prompt visibility**: with **`sv_ai_debug 1`**, prompt debug is intentionally trimmed to print only **`BOT_STATE`** JSON. `BOT_STATE` includes real bot AI data for **alive** speakers (task/disposition, bomb/follow/nearby counts, etc.). For **dead** speakers it is intentionally smaller to avoid nonsense context; instead it includes a short **last-death snapshot** (killer, kill type, death place).
- **Freeze while typing**: alive bots freeze **movement and look/aim** while they are in the simulated “typing” window (no mouse tracking).

### Recent behavior changes (pickup notes)

- **Alive bots are very strict now**: they only speak on:
  - **`friendly_fire_repeat`** (you got team-shot)
  - **`kill_alive_killer`** only for **spectacular** kills (knife / clutch moments)
  Everything else is suppressed for alive speakers to avoid useless narration that gets you killed mid-typing.
- **Typing/reaction is faster** (to reduce “10s for a short line” feel):
  - Typing per char ~**0.20–0.30s** (capped ~**5s**)
  - Alive reaction delays reduced for kill / quick events
- **Friendly fire complains sooner**:
  - Can trigger on the **1st** hit sometimes, and by the **2nd** hit often
  - **Won’t trigger while the victim bot is in a fight** (attacking or enemies nearby)
- **Dead `BOT_STATE` cleanup**:
  - Removed dead-speaker AI “live state” fields (`bot_task`, `bot_disposition`, `is_following`, `follow_leader`, nearby counts, etc.)
  - Dead speakers now include a last-death snapshot: **`killed_by`**, **`kill_type`**, and **`death_location`** as a **place name** (e.g. bombsite / spawn) instead of raw coordinates.
- **Repeat-death memory**:
  - `BOT_STATE` tracks **`killed_by_same_attacker_count`**, **`is_repeat_death_to_same_attacker`**, **`most_killed_by`**, and **`most_killed_by_count`**.
  - This gives the model a factual reason to say “again” only after the same player has killed that bot multiple times, and can make the bot mood more salty/tilted.
- **Dead chat can react to chat**:
  - Dead bot lines and human chat are recorded into a small **`RECENT_CHAT`** buffer.
  - Mentioning a dead bot can trigger a reply; compliment-like mentions have a higher chance to get a short positive response.
- **Round-end reactions**:
  - Bomb defused, bomb exploded, all hostages rescued, and generic CT/T wins can trigger rare extra **praise/blame** lines.
  - Winning-team bots lean toward relief/compliments; losing-team bots lean toward blame/salt.
- **Defuse timing reaction**:
  - When a CT starts defusing, the code compares C4 time left with the needed defuse time (**5s with kit**, **10s without**).
  - Rarely, a bot reacts differently depending on whether the defuse can finish before explosion.

Reload config without restarting the game (cvars only):

```cfg
exec game_init.cfg
```

Changing **`rg.so`** or **`BotProfile.db`** still requires a **full game restart** to be safe.

---

## 5) Per-bot character (`BotProfile.db`)

The game loads **`BotProfile.db`** (cvar **`bot_profile_db`**, default `BotProfile.db` in **`cstrike/`**).

Optional attribute (supported by **this** `rg.so`):

```text
ChatPersona = your_snake_case_hint_here
```

Rules:

- **One parser token** after `=` (no spaces); use **underscores** instead of spaces.
- **`ChatPersona = none`**: clears a value inherited from a template.
- The Ollama **system** prompt includes **`CHARACTER_VOICE=...`** from **`ChatPersona`** so the model can match the vibe (same string can later drive voice selection).

This repo’s **`BotProfile.db`** adds **`ChatPersona`** on **every** named bot profile (millennial / 2000s LAN–pub style). To regenerate or bulk-edit personas:

1. Edit **`scripts/apply_bot_chat_personas.py`** (the `PERSONAS` dict).
2. From the repo root: **`python3 scripts/apply_bot_chat_personas.py`**
3. Copy the updated **`BotProfile.db`** into **`cstrike/`** and restart the game.

---

## 6) What triggers chat right now

All of these are **`GameEventType`** values from `game_shared/GameEvent.h` (the game calls `TheBots->OnEvent(...)`; `AiChat::OnEvent` only reacts to the list below). Each path is **throttled** and still subject to **per-bot round budget** and the **Ollama speak roll** (except knife/clutch boosts on kills).

- **Round:** `EVENT_ROUND_START`, `EVENT_TERRORISTS_WIN`, `EVENT_CTS_WIN`, `EVENT_ROUND_DRAW`
- **Bomb:** `EVENT_BOMB_PLANTED`, `EVENT_BOMB_DEFUSED`, `EVENT_BOMB_EXPLODED`, `EVENT_BOMB_DEFUSING`, `EVENT_BOMB_DEFUSE_ABORTED`, `EVENT_BOMB_DROPPED`, `EVENT_BOMB_PICKED_UP`
- **Combat / life:** `EVENT_PLAYER_DIED`, `EVENT_PLAYER_SPAWNED` (bots), `EVENT_PLAYER_BLINDED_BY_FLASHBANG`, `EVENT_PLAYER_TOOK_DAMAGE` (friendly-fire repeat only)
- **Grenades:** `EVENT_HE_GRENADE_EXPLODED`, `EVENT_FLASHBANG_GRENADE_EXPLODED`, `EVENT_SMOKE_GRENADE_EXPLODED` (one shared throttle)
- **Hostage maps:** `EVENT_HOSTAGE_RESCUED`, `EVENT_ALL_HOSTAGES_RESCUED`, `EVENT_HOSTAGE_KILLED`, `EVENT_HOSTAGE_DAMAGED`
- **VIP (as\_):** `EVENT_VIP_ESCAPED`, `EVENT_VIP_ASSASSINATED`
- **Match:** `EVENT_NEW_MATCH`, `EVENT_GAME_COMMENCE`

Not wired to AI chat (too noisy or low value): footsteps, weapon fire, radio spam, bullet impact, doors, breakables, etc.

---

## 7) Quick test checklist

1. Confirm the new DLL: **`game version`** (build date should match your build).

2. **`exec game_init.cfg`** if you use one.

3. Add bots: **`bot_add`**

4. If chat is quiet or empty:

   - Raise timeout: **`sv_ai_timeout_ms 60000`**
   - Warm the model: **`ollama run …`**
   - Enable **`sv_ai_debug 1`** and read **`[ai]`** lines on the **server** console (delivery filters, Ollama errors, etc.).

5. Team vs all: join the **same team** as the speaking bot to see **team-only** lines; dead/alive chat follows normal CS rules unless you changed **`sv_allchat`**.

---

## Requirements summary

| Piece | Role |
|--------|------|
| **`build/regamedll/cs.so` → `cstrike/dlls/rg.so`** | AI chat + **`ChatPersona`** parsing |
| **`cstrike/BotProfile.db`** | Bot names + stats + **`ChatPersona`** hints |
| **`cstrike/game_init.cfg`** | `sv_ai_*` (and optional `sv_ai_chat_team_tag`) |
| **Ollama** | HTTP API at `sv_ai_ollama_url` |

Without this fork’s **GameDLL**, do **not** add **`ChatPersona`** lines to stock **`BotProfile.db`** (unknown attribute / parse error).
