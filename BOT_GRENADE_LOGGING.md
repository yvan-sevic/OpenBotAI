# Bot Grenade Behavior Logging

## Overview

This system records bot grenade throws to a CSV file, allowing you to compare bot behavior with your manually recorded grenade positions in `grenade.json`.

## Features

- Records every bot grenade throw (HE, Smoke, Flash)
- Tracks whether the bot used a manual spot from `grenade.json`
- Logs bot position, aim angles, and target coordinates
- Shows distance from manual spot when applicable
- Compatible with existing human grenade logging

## CSV Files Generated

The system creates CSV files in your game directory (e.g., `cstrike/`):

### `grenade_bot_events.csv`
Records bot grenade throws with these columns:

| Column | Description |
|--------|-------------|
| `time` | Game time when grenade was thrown |
| `map` | Map name |
| `entindex` | Bot entity index |
| `name` | Bot name |
| `team` | Bot team (CT/T) |
| `grenade_type` | Type of grenade (he/flash/smoke) |
| `grenade_items_in_inventory` | Number of grenades in inventory |
| `weapon_entity` | Weapon entity name |
| `origin_x/y/z` | Bot's feet position |
| `eye_x/y/z` | Bot's eye/camera position |
| `pitch/yaw/roll` | Bot's view angles |
| `forward_x/y/z` | Forward vector from view angles |
| `trace_x/y/z` | Where aim line hits (wall/floor) |
| `target_x/y/z` | **Intended target position** (from manual spot or calculated) |
| `used_manual_spot` | "yes" if used `grenade.json` spot, "no" otherwise |
| `manual_spot_distance` | Distance (2D) from recorded spot position |
| `manual_spot_index` | Index of spot in `grenade.json` |
| `manual_spot_name` | Name of the spot (e.g., "human_log_001") |

### `grenade_human_events.csv` (Already Exists)
Records human player grenade throws for comparison.

## Usage

### 1. Enable Logging

Add to your `server.cfg` or type in console:

```
sv_grenade_throw_log_enable 1
```

### 2. Play and Record

- Start your server on de_dust2
- Add bots: `bot_add_t` and `bot_add_ct`
- Let bots play for several rounds
- Bots will automatically use your manual spots from `grenade.json`

### 3. Analyze the Data

The CSV file will be in your game directory:
```bash
# Example path
~/.local/share/Steam/steamapps/common/Half-Life/cstrike/grenade_bot_events.csv
```

Open it with:
- Excel, Google Sheets, LibreOffice Calc
- Python/pandas for analysis
- Any CSV viewer

### 4. Compare with Your Manual Recordings

Compare bot behavior with your `grenade.json` entries:

#### Your Manual Recording (from grenade.json):
```json
{
  "name": "human_log_001",
  "type": "he",
  "team": "CT",
  "from_x": -415.2,
  "from_y": 1680.6,
  "from_z": -92.0,
  "target_x": -412.7,
  "target_y": 1605.7,
  "target_z": -67.2
}
```

#### Bot's Usage (from CSV):
```csv
time,map,name,team,grenade_type,origin_x,origin_y,origin_z,target_x,target_y,target_z,used_manual_spot,manual_spot_distance,manual_spot_index,manual_spot_name
45.231,de_dust2,Bot,CT,he,-398.5,1672.3,-92.0,-412.7,1605.7,-67.2,yes,18.4,0,human_log_001
```

**Analysis:**
- Bot was 18.4 units away from your recorded position
- Bot used the same target you specified
- Bot successfully found and used your manual spot

## What to Look For

### Good Signs:
1. **`used_manual_spot: yes`** - Bot is using your recordings
2. **`manual_spot_distance < 100`** - Bot is reasonably close to your position
3. **Target matches your grenade.json** - Bot is aiming at your intended target

### Issues to Check:
1. **`used_manual_spot: no`** with manual spots available
   - Bot path might not go near your recorded positions
   - Distance tolerance might still be too strict
   - Check `manual_spot_distance` in debug mode

2. **Large `manual_spot_distance`** (> 300 units)
   - Bots are using spots from far away
   - Might indicate we need tighter or looser tolerance

3. **Wrong `target_x/y/z`**
   - Bot found the spot but target doesn't match your recording
   - Potential bug in spot selection

## Analysis Examples

### Example 1: Count Manual Spot Usage

Using command line:
```bash
cd ~/.local/share/Steam/steamapps/common/Half-Life/cstrike
grep ",yes," grenade_bot_events.csv | wc -l  # Count manual spot uses
grep ",no," grenade_bot_events.csv | wc -l   # Count non-manual throws
```

### Example 2: Python Analysis

```python
import pandas as pd

# Load the data
df = pd.read_csv('grenade_bot_events.csv')

# Filter HE grenades only
he_grenades = df[df['grenade_type'] == 'he']

# Count manual vs non-manual
manual_count = he_grenades[he_grenades['used_manual_spot'] == 'yes'].shape[0]
other_count = he_grenades[he_grenades['used_manual_spot'] == 'no'].shape[0]

print(f"Manual spot usage: {manual_count}")
print(f"Other throws: {other_count}")
print(f"Manual usage rate: {manual_count / len(he_grenades) * 100:.1f}%")

# Average distance from manual spots
manual_throws = he_grenades[he_grenades['used_manual_spot'] == 'yes']
avg_distance = manual_throws['manual_spot_distance'].mean()
print(f"Average distance from recorded spot: {avg_distance:.1f} units")

# Most used spots
spot_usage = manual_throws['manual_spot_name'].value_counts()
print("\nMost used spots:")
print(spot_usage.head(5))
```

### Example 3: Excel Analysis

1. Open `grenade_bot_events.csv` in Excel
2. Filter by `used_manual_spot` = "yes"
3. Create a pivot table:
   - Rows: `manual_spot_name`
   - Values: Count of throws
   - Shows which spots bots use most frequently

## Troubleshooting

### "No CSV file created"

Make sure:
```
sv_grenade_throw_log_enable 1
```

And bots are actually throwing grenades. Check:
```
mp_startmoney 16000      // So bots can buy grenades
mp_buytime 90            // Enough time to buy
bot_difficulty 3         // Higher skill = more grenades
```

### "All entries show used_manual_spot: no"

Possible causes:
1. No `grenade.json` file in `maps/de_dust2.grenade.json`
2. Bots are not pathing near your recorded positions
3. Enable debug mode to see if spots are loaded:
   ```
   bot_grenade_spots_debug 1
   ```

bot_grenade_spot he spot_name T
bot_grenade_debug_he_only 1
sv_grenade_throw_log_enable 1
bot_grenade_spots_debug 1
mp_startmoney 16000
mp_restartgame 1
### "Bot positions are way off from my recordings"

This is expected! Bots have different pathfinding than humans. The tolerance is set to 650 units for HE grenades to account for this. If bots never get within 650 units of your spots, you'll need to record spots along bot paths.

## Console Variables

```
sv_grenade_throw_log_enable 1     // Enable bot grenade logging
bot_grenade_spots_debug 1         // Show debug info about spot loading/usage
```

## Tips for Comparison

1. **Record spots where bots actually path**: Use `noclip` to follow bots and record spots along their natural routes

2. **Check the CSV regularly**: See which spots get used vs ignored

3. **Adjust tolerance if needed**: If bots are using spots from too far away, we can tighten the distance check

4. **Compare teams separately**: CTs and Ts have different paths, analyze them separately

5. **Look for patterns**: If certain spots never get used, they might be off bot paths

## What's Next?

After collecting data:
1. Identify which manual spots bots use successfully
2. Find positions where bots throw but don't have manual spots
3. Record new spots along bot paths
4. Adjust existing spot positions if needed
5. Compare bot throw success rate with manual vs calculated throws

The goal is to maximize `used_manual_spot: yes` entries while ensuring bots throw grenades effectively!
