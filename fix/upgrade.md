# Fix / Upgrade Notes

This file tracks the gameplay humanization changes being added to this build.

## Bot Buying Humanization

- Bots no longer buy everything in one instant batch.
- Each bot starts buying immediately during freeze time, then gets a small per-bot timing offset; outside freeze time the engine-safe buy delay is still respected.
- Buy commands are issued step by step, so weapons, ammo, armor, pistols, grenades, and kits happen over short human-like intervals.
- Freeze-time buying now uses wider per-bot start delays and personal buy-menu speed, so bots finish at different moments instead of all buying instantly together.
- After finishing freeze-time buys, some bots now glance around spawn and some jump once or twice at staggered personal timings instead of standing perfectly still.
- Bots stay in the buy state during freeze time after finishing, then move out normally.

## Weapon Choice Improvements

- Primary weapon choices are filtered before a buy command is sent.
- Bots no longer buy tactical shields, including from random CT buys or shield entries in weapon preferences.
- Bots now avoid trying weapons they cannot afford.
- Bots now avoid trying weapons that are invalid for their team or map rules.
- Bots now respect allowed weapon-class cvars before choosing preferred or random weapons.
- Random fallback buys now choose from actually buyable weapons instead of the full allowed-class table.
- Existing primary checks use the primary weapon slot, not the currently active weapon, so bots do not rebuy just because they are holding a pistol, knife, or grenade.

## Eco Behavior

- Bots can decide to eco when they have low money and no primary weapon.
- Eco bots skip primary weapon, armor, grenade, and defuse-kit spending so they actually save for a better round.
- Eco bots may occasionally buy a cheap pistol, giving some variety without destroying the save.
- Higher-skill bots are slightly more willing to save instead of forcing a weak buy.

## Aim Humanization

- Bots now aim at perceived enemy snapshots instead of perfectly refreshing the exact target point every tick.
- Visible enemy positions are sampled with a short human-like delay and small distance-based perception error.
- Aim target movement uses inertia before converting to look angles, which softens lock-on and tracking.
- Aim correction speed now varies over time, with small slowdowns and short catch-up bursts instead of one linear glide.
- Tiny hand jitter is applied while tracking, especially during fast view movement, to avoid keyboard-like straight aim paths.
- Aim corrections can arc slightly sideways and decay, so the crosshair does not travel in perfectly straight lines.
- Newly acquired enemies are tracked more slowly for the first moment instead of snapping instantly.
- Dynamic aim error now reacts to target acquisition, lost sight, fast view movement, and weapon recoil.
- Attack view-angle springs are slightly softer, reducing the "terminator lock" feel while keeping bots responsive.
- Non-combat movement look keeps a clear path direction and stable forward pitch while adding controlled mouse imperfections, mild uneven turn speed, path look-ahead variance, side glances, and subtle up/down checks while avoiding ladder precision changes.
- Aim-log tuning pass: roaming bots now scan less horizontally, hold standing view direction longer, and use slower non-combat yaw acceleration to better match human roaming samples.
- Gunfights now damp horizontal aim sliding while adding stronger rifle/pistol vertical micro-correction, so firing aim has more human-like recoil-control pitch movement.
- Low-priority moving spot checks now behave like short glances instead of locking the bot's view to a fixed world point while walking.
- Bots stop hard-tracking recently seen enemies shortly after line-of-sight breaks, reducing the wallhack-looking behavior around corners.
- Path-following look now adapts pitch to height changes ahead, so bots look down drops/ramps and up toward boxes/stairs while roaming.
- Bot view movement now adds tiny cross-axis bleed, avoiding perfectly horizontal-only or vertical-only mouse movements except on ladders.
- Roaming look now uses short intentional attention phases, checking danger angles, corners, terrain changes, then recentering, instead of relying on continuous random camera drift.
- Enemy tracking keeps a small moving miss-margin and avoids combat yaw snapping, so target aim has human hesitation instead of settling to perfect X/Y zero.
- Combat aim now uses explicit human-like phases: a short recognition pause, a fast flick from current crosshair position, overshoot correction, tracking, recoil control, and lost-target decay.
- Experimental aim controller extraction moves both combat aim and non-combat look intent into `cs_bot_aim_controller.cpp`, leaving `Upkeep()` as the lightweight caller for easier comparison/tuning.
- AK/rifle combat aim now uses stronger vertical correction and combat-only cross-axis bleed, while roaming pitch noise is increased to better match human off-fight samples.
- Enemy acquisition now has a short recognition pause followed by a faster catch-up burst when the target is off-angle, instead of one constant mouse speed.
- Existing bots are preserved while a human is spectating, so spectator mode can be used to watch bot behavior without clearing them.
- Bots can now fill normal/fill/match quota after map change while the human is still spectator/unassigned, so learned danger data can keep accumulating before team selection.
- Combat tactics pass: bots now use a wider teammate danger corridor before firing, strafe to clear blocked lanes, briefly settle before rifle shots, and dodge less mechanically.
- Rifle/SMG bots are now more willing to hold full-auto fire at close range, against multiple nearby enemies, or shortly after being hurt, matching common human panic/crowd spray behavior.
- Roaming pitch now stays closer to center crosshair height, and guarding bots prefer visible/far approach lanes instead of randomly staring away from likely entrances.
- Learned danger memory records enemy sightings, damage, deaths, and weapon noise into `maps/<map>.danger.json`, then biases guarding/roaming looks toward the strongest visible danger spots.
- Learned bad camp memory records deaths, damage, and path failures from hiding/sniping positions into `maps/<map>.camp.json`, then avoids high-weight bad spots while still falling back to the least-bad option if every nearby camp is risky.
- Manual de_dust2 bad camp markers from human aim-log standing/firing samples were added to `cstrike/maps/de_dust2.camp.json` with high weights for both teams where needed.
- Noise investigation is less direct: bots now approach offset suspicion points near the sound, stopping short or checking from a side angle based on urgency, skill, and aggression.
- Hiding/ambush behavior is less generic: hold chance and duration now depend on task, weapon role, morale, personality, and whether the bot is following, while watched approach lanes bias toward recent enemy/noise/objective context.
- Sniper bots now keep scope stable while settling aim, ignore weak one-off danger noise, and fire deliberate scoped shots instead of repeatedly clicking while sweeping.
- AWP/Scout bots now perform a brief human-like quick-switch to pistol/knife/grenade after a shot, then return to the sniper and re-scope through the normal zoom logic.
- Quiet walking is now limited to nearby enemy noise or sensitive bomb-planted tasks instead of general knife/route movement.
- USP silencer handling no longer gets forced on by high skill, while M4A1 can still prefer silencer for stronger bots.
- Camping look fallback now avoids blocked/near-wall approach points and uses open sight lanes when no learned danger spot is available.
- Non-combat look logic now detects when a bot is staring into close wall geometry and redirects its view toward a nearby open lane unless a real threat has priority.
- Objective movement now retries/repaths to the current destination before falling back to idle task selection, reducing random route changes caused by weak distractions or path hiccups.
- Committed opening rush/objective movement now suppresses low-priority roam, corner, encounter, and weak noise glances, while still allowing visible enemies, recent enemy memory, and dangerous gunfire to pull attention.
- A first-pass Bot Director now chooses a visible per-round defuse-map plan, assigns bot roles such as entry/support/lurk/anchor/rotator, nudges T attack-site commitments and CT guard sites, and prints its raw decisions in chat when `bot_director_debug` is enabled.
- The Bot Director now remembers previous-round attack zone, plant success, planted site, and winner, then adapts the next round by retrying successful pressure, avoiding repeatedly failed sites, or stacking CT defense toward the last planted site.
- Director roles now affect behavior: entry/support/rotator bots buy more role-appropriate utility, CT anchors/rotators adjust defuse-kit chances, sniper roles prefer sniper buys/holds, entries take faster attack routes, and lurkers/snipers delay before joining pressure.
- Director output is advisory, not a master command: each bot rolls whether it follows the recommended target zone or keeps loose local decision-making, and role buy/movement effects are intentionally softened to preserve pub-server unpredictability.
- `bot_difficulty` sets the **base** step on a **1–15** scale (default 5). Integer **0–3** is still accepted as legacy easy/normal/hard/expert and maps to **1 / 6 / 11 / 15**. The Bot Director adds its offset on top for **effective** difficulty (clamped 0–15) for aim, awareness, economy, reload, sniper, and reaction behaviors; larger stomps move the offset faster (−6…+12 vs base).
- Combat target focus is stickier: bots now avoid switching away from an active visible enemy unless the new threat is very close, recently hurt them, or the current enemy has been lost long enough.
- Combat re-acquisition now refreshes the visible enemy snapshot immediately and uses a short catch-up flick/correction instead of slowly hovering back after line-of-sight returns.
- Reloading no longer freezes bots in place: they keep strafing, back off under recent danger, may retreat to a safer nearby spot, and occasionally jump while reloading.
- Combat movement now avoids freezing in crowded fights: stop-and-shoot is shorter and less frequent when multiple enemies are nearby, while skilled/outnumbered bots strafe or back off during short reposition windows.
- Combat strafing now chooses clearer side lanes with hull/line-of-sight checks, holds a lane briefly instead of metronome left-right switching, and avoids stacking dodge movement on top of active repositioning.
- Path movement now preserves momentum on true step-up ledges: running bots can trigger upward jumps from farther ahead only when a hull trace shows the ledge blocks normal movement, while walkable steps no longer cause extra jump attempts.
- Stair climbing now filters out gradual step-ups before ledge jumps, so bots keep moving forward up stairs instead of hopping when the higher lookahead point is several stair steps above them.
- Post-jump path steering now suppresses backward correction during the short jump-crouch momentum window, preventing bots from braking immediately after a successful ledge jump.
- Memory/suspicion behavior now checks plausible last-seen escape angles: when enemies vanish, bots briefly look at nearby learned danger spots or noisy predicted positions instead of only staring at the exact last coordinate.
- Hunt logic is now less random: bots prioritize recent last-seen/danger memory, then least-recently-cleared areas, with random hunting only as a fallback if no useful area is found.
- Defensive rush is no longer a whole-team global flip; the round can still have a rush mood, but only a stable local subset of aggressive defenders joins it while others may hold or snipe.
- Safe running knife swings now require a clear local swing lane, so bots stop messing around with the knife when teammates or other players are close enough to be clipped.
- Safe-time knife switching is now a per-bot choice with a delayed switch time, so some bots keep their weapon out and others pull knife at different moments.
- Slope navigation look pitch now favors the forward lane instead of the floor, limiting downhill/floor pitch during path-following and roam terrain checks while keeping modest uphill awareness.
- Rush/follow pack behavior is looser: radio rush commands and follow requests now use per-bot personality/distance rolls, so some bots join while others keep holding, rotating, or sniping.
- Human radio commands now behave more like requests: bots independently obey, silently ignore, or decline based on teamwork, aggression, current task, combat pressure, role, distance, and rogue behavior.
- Early-round movement is more independent: some bots deliberately hold or guard their own angle instead of joining the first wave, and early auto-follow is reduced so humans do not create a bot train at spawn.
- `bot_danger_memory_enable`, `bot_danger_memory_learn`, and `bot_danger_memory_debug` control using, learning, and drawing learned danger spots.
- `bot_camp_memory_enable`, `bot_camp_memory_learn`, and `bot_camp_memory_debug` control using, learning, and drawing bad camp spots as purple/red bars.
- Danger files are JSON objects with a `spots` array containing `x/y/z`, `place`, `team_seen_by`, `sightings`, `deaths`, `last_seen`, and `weight`. HE grenades bias toward high-weight spots from that data (even if live learning is off), while flashes use choke peek arcs (path corner + blind offset) when there is medium+ noise or very recent lost line-of-sight on an enemy. Smokes fire on defuse-map retakes toward the planted bomb (CT, final approach) and on T site executes (committed or Director site, medium path remaining). Mid-round tactical throws share a per-bot cooldown; round-open throws still run at safe-time end but HE/flash/smoke prefer manual `.grenade.json` lineups when the bot is near the recorded throw tile.
- **Manual grenade lineups** live in `maps/<map>.grenade.json`. On a **listen server**, stand on the throw tile, aim at where the grenade should land/pop, then run `bot_grenade_spot <smoke|flash|he|any> [name] [team]` (`team`: `CT`, `T`, or `any`; default is your current team). The game writes `from_*` (feet) and `target_*` (trace along view). Use `bot_grenade_spots_debug 1` to draw cyan beams from throw point to target and show loading/usage logs. Bots use smoke/flash lineups when within ~20–295 units of `from` and hold the matching grenade type. HE grenade spots have much wider tolerance (~8–650 units) since they're tactical area-denial throws that don't require pixel-perfect positioning, and manual HE spots bypass the random skill gate for guaranteed tactical usage.
- Camp files are JSON objects with a `bad_spots` array containing `x/y/z`, `place`, `team`, `deaths`, `damage_events`, `path_fails`, `last_bad`, and `bad_weight`.
- `sv_aim_log_enable 1` records comparable aim samples to `aim_bot_events.csv` and `aim_human_events.csv`; `sv_aim_log_interval` controls roaming sample spacing.
- Aim logs include roaming/firing event type, view angles, punch angles, origin/eye position, aim coordinate, hit target, tracked bot target, weapon, speed, team, and player name.
- Bot recoil control now has dedicated helper state/functions for weapon-specific pull-down, delayed correction, horizontal drift, and spray recovery, with the first AK/M4/Galil/Famas/SMG pass tuned from human firing CSV samples.

## Movement Flavor

- Bots may occasionally swing their knife while running safely outside combat, similar to human players messing around during rotation.
- Safe knife messing-around can now be a short held full-auto slash instead of only one click.

## Build Command

Use the project build command from `AI_CHAT_README.md`:

```sh
cd /home/kemt/Downloads/ReGameDLL_CS-master && \
rm -rf build && \
CC=gcc CXX=g++ cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -B build && \
cmake --build build -j"$(nproc)"
```
