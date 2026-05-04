# HE Grenade Spots Fix

## Problem
Bots were not using the recorded grenade positions from `de_dust2.grenade.json` even though 23 HE grenade spots were recorded.

## Root Causes Identified

1. **Distance tolerance too strict**: Bots needed to be within 360 units of recorded position, but bot pathfinding often doesn't pass through exact same positions as human players
2. **Random skill gate**: Even when near a spot, bots only attempted grenades 26-81% of the time due to probabilistic check
3. **IsAttacking() blocking**: Bots couldn't throw grenades during combat, missing tactical opportunities
4. **No debug feedback**: Difficult to diagnose why bots weren't using spots

## Changes Made

### 1. Increased HE Grenade Distance Tolerance
**File**: `regamedll/dlls/bot/cs_bot_grenade_spots.cpp`

Changed HE grenade distance requirements:
- `minFrom`: 12.0f → **8.0f** (closer tolerance)
- `maxFrom`: 360.0f → **650.0f** (much more lenient)
- `minTargetDist`: 70.0f → **60.0f** (slightly more flexible)

This allows bots to use spots even when their path doesn't perfectly match the recorded position.

### 2. Prioritized Manual HE Spots
**File**: `regamedll/dlls/bot/cs_bot_weapon.cpp`

- Moved HE grenade manual spot check **before** the random skill gate
- Bots now **always** try to use manual HE spots when within range (no probability gate)
- Removed `IsAttacking()` from blocking conditions so bots can throw during combat
- Slightly increased probability for other grenades (32%+60%*skill vs 26%+55%*skill)

### 3. Added Debug Logging
**File**: `regamedll/dlls/bot/cs_bot_grenade_spots.cpp`

Added console output when `bot_grenade_spots_debug 1`:
- Shows total spots loaded and breakdown by type (HE/Smoke/Flash)
- Shows breakdown by team (CT/T/Any)
- Shows when a bot finds and uses a spot with distance info
- Shows errors if JSON file is malformed

## Testing Instructions

### 1. Copy the compiled DLL
```bash
# The compiled library should be in:
# build/regamedll/cs.so (Linux)
# Copy it to your game's dlls folder
```

### 2. Enable Debug Mode
In your server console or `autoexec.cfg`:
```
bot_grenade_spots_debug 1
```

This will:
- Print spot loading info when map starts
- Draw cyan beams from throw position to target
- Show console messages when bots find/use spots

### 3. Watch Bot Behavior
- Add bots to de_dust2
- Watch them move around the map with HE grenades
- When a bot gets within ~650 units of a recorded position, they should throw
- Check console for messages like: `[BOT PlayerName] Found HE spot #5 'human_log_006' (distance: 234.5 units)`

### 4. Your Recorded Spots
Your `de_dust2.grenade.json` has **23 HE grenade spots**:
- **CT**: 7 spots (human_log_001, 016-022)
- **T**: 16 spots (human_log_002-015, 023)

All spots should now be usable with the increased distance tolerance.

## Additional Console Commands

```
bot_grenade_spots_debug 1          // Enable debug visualization and logging
bot_grenade_spots_debug 0          // Disable debug mode
bot_grenade_he_use_danger_memory 1 // Enable danger memory fallback for HE
```

## Expected Behavior After Fix

1. **When bot spawns as T**: Bot moves around the map, and when they pass near any of the 16 T spots while holding HE, they should stop and throw at the recorded target
2. **When bot spawns as CT**: Same behavior, but using the 7 CT spots
3. **Debug beams**: You'll see cyan lines from recorded throw positions to targets
4. **Console messages**: You'll see which spots bots are finding and using

## If Bots Still Don't Use Spots

Check these conditions:
1. Bot must have an HE grenade in inventory
2. Bot must not be buying or escaping bomb
3. Bot must have a navigation path
4. Target must not risk hitting teammates (check `WouldGrenadeAimRiskTeammates`)
5. Verify spots loaded: Look for console message showing "Loaded X spots from..."

## Technical Details

The scoring algorithm for choosing the best spot when multiple spots match:
```cpp
score = 210.0 - distanceFromRecordedPosition
score += botSkill * 45.0
score -= abs(targetDistance - 680.0) * 0.02
score += random(-6.0, 6.0)
```

Bots prefer:
- Spots closer to their current position
- Targets around 680 units away
- Higher skill bots get bonus score (more likely to use spots)
