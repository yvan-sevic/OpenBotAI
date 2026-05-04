# HE Grenade Spots - Quick Start Guide

## What Was Fixed

Your bots weren't using the 23 HE grenade positions you recorded in `de_dust2.grenade.json` because:

1. **Distance tolerance was too strict** - Bots needed to be within 360 units, now it's **650 units**
2. **Random probability gate** - Even near spots, bots only tried 26-81% of the time. Now HE spots bypass this completely
3. **Combat blocking** - Bots couldn't throw during combat. This is now removed for HE grenades
4. **No feedback** - You couldn't see what was happening. Added extensive debug logging

## Installation (Choose One Method)

### Method 1: Automatic (Linux)
```bash
cd /home/kemt/Downloads/ReGameDLL_CS-master
./install_grenade_fix.sh
```

### Method 2: Manual
```bash
# Copy the compiled library
cp /home/kemt/Downloads/ReGameDLL_CS-master/build/regamedll/cs.so \
   /home/kemt/.local/share/Steam/steamapps/common/Half-Life/cstrike/dlls/cs.so

# Backup the original first if you haven't:
cp /home/kemt/.local/share/Steam/steamapps/common/Half-Life/cstrike/dlls/cs.so \
   /home/kemt/.local/share/Steam/steamapps/common/Half-Life/cstrike/dlls/cs.so.backup
```

## Testing

### 1. Enable Debug Mode
In console:
```
bot_grenade_spots_debug 1
sv_grenade_throw_log_enable 1
```

The first command shows when bots find/use spots.
The second command logs every bot grenade throw to CSV.

### 2. Start a Map
```
map de_dust2
bot_add_t
bot_add_ct
```

### 3. What You Should See

**In Console (when map loads):**
```
[GrenadeSpots] Loaded 23 spots from .../cstrike/maps/de_dust2.grenade.json
  HE: 23, Smoke: 0, Flash: 0, Any: 0
  CT: 7, T: 16, Any team: 0
```

**When Bot Uses Spot:**
```
[BOT PlayerName] Found HE spot #5 'human_log_006' (distance from recorded: 234.5 units)
```

**Visually:**
- Cyan beams showing throw position → target position
- Bots stopping near recorded positions and throwing HE grenades

## Your Recorded Spots

**CT Spots (7 total):**
- human_log_001, 016, 017, 018, 019, 020, 021, 022, 023

**T Spots (16 total):**
- human_log_002 through human_log_015

## Troubleshooting

### "Bots still not throwing grenades"

**Check these:**

1. **Do bots have HE grenades?**
   - Use `mp_startmoney 16000` to ensure they can buy
   - Bots need to actually purchase HE grenades during buy time

2. **Is the debug output showing spots loaded?**
   ```
   bot_grenade_spots_debug 1
   ```
   Then restart the map. You should see "Loaded X spots..." message

3. **Are bots getting close enough?**
   - Use `sv_cheats 1` and `noclip`
   - Fly to the "from" positions in your JSON file
   - Bots need to pass within 650 units (a pretty large radius now)

4. **Check grenade.json file location:**
   ```bash
   ls -lh ~/.local/share/Steam/steamapps/common/Half-Life/cstrike/maps/de_dust2.grenade.json
   ```
   File should exist and be readable

5. **Are teammates blocking?**
   - The `WouldGrenadeAimRiskTeammates()` check might prevent throws
   - Try with fewer bots or watch isolated bots

### "No spots loaded" message

Your JSON file might not be in the right location. The game looks for:
```
<game_dir>/maps/<mapname>.grenade.json
```

For de_dust2, that's:
```
~/.local/share/Steam/steamapps/common/Half-Life/cstrike/maps/de_dust2.grenade.json
```

Verify it exists:
```bash
cat ~/.local/share/Steam/steamapps/common/Half-Life/cstrike/maps/de_dust2.grenade.json | head -20
```

## Adding More Spots

To add more grenade positions:

1. Start a **listen server** (must be listen, not dedicated)
2. Join a team (CT or T)
3. Stand where you want to throw from
4. Aim at where the grenade should land
5. Type in console:
   ```
   bot_grenade_spot he my_spot_name T
   ```

This adds the spot to the JSON file immediately.

**Arguments:**
- `he` = grenade type (he, smoke, flash, or any)
- `my_spot_name` = custom name for the spot (optional)
- `T` = which team should use it (T, CT, or any)

## Technical Details

**What Changed in the Code:**

1. `cs_bot_grenade_spots.cpp`:
   - maxFrom: 360 → 650 units for HE grenades
   - minFrom: 12 → 8 units
   - Added debug logging when spots found/used
   - Added loading summary with counts

2. `cs_bot_weapon.cpp`:
   - Moved HE spot check before random skill gate
   - Removed IsAttacking() block for grenades
   - Increased general grenade probability slightly

3. `fix/upgrade.md`:
   - Updated documentation to reflect new HE grenade tolerance

## Still Having Issues?

Check the full documentation:
- `GRENADE_SPOTS_FIX.md` - Detailed technical explanation
- `fix/upgrade.md` - Complete feature documentation

Or provide:
1. Console output with `bot_grenade_spots_debug 1`
2. Screenshot showing bot positions vs recorded positions
3. The content of your de_dust2.grenade.json file
