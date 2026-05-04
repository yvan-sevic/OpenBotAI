#!/bin/bash
# HE Grenade Fix Installation Script

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== HE Grenade Spots Fix Installation ===${NC}"
echo ""

# Check if compiled DLL exists
if [ ! -f "build/regamedll/cs.so" ]; then
    echo -e "${RED}Error: build/regamedll/cs.so not found!${NC}"
    echo "Please build the project first with:"
    echo "  ./build.sh"
    exit 1
fi

# Find game directory
GAME_DIR="/home/kemt/.local/share/Steam/steamapps/common/Half-Life/cstrike"

if [ ! -d "$GAME_DIR" ]; then
    echo -e "${YELLOW}Warning: Could not find game directory at:${NC}"
    echo "  $GAME_DIR"
    echo ""
    read -p "Enter your cstrike directory path: " GAME_DIR
fi

if [ ! -d "$GAME_DIR" ]; then
    echo -e "${RED}Error: Directory does not exist: $GAME_DIR${NC}"
    exit 1
fi

# Create backup
DLL_PATH="$GAME_DIR/dlls/cs.so"
if [ -f "$DLL_PATH" ]; then
    BACKUP_PATH="$DLL_PATH.backup_$(date +%Y%m%d_%H%M%S)"
    echo -e "${YELLOW}Creating backup:${NC}"
    echo "  $BACKUP_PATH"
    cp "$DLL_PATH" "$BACKUP_PATH"
fi

# Copy new DLL
echo -e "${GREEN}Installing fixed DLL to:${NC}"
echo "  $DLL_PATH"
cp build/regamedll/cs.so "$DLL_PATH"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Installation successful!${NC}"
    echo ""
    echo -e "${YELLOW}Next steps:${NC}"
    echo "1. Start your CS 1.6 server"
    echo "2. Add this to your server.cfg or type in console:"
    echo "   ${GREEN}bot_grenade_spots_debug 1${NC}        # Show when bots use spots"
    echo "   ${GREEN}sv_grenade_throw_log_enable 1${NC}   # Log bot grenades to CSV"
    echo "3. Load de_dust2 and add bots"
    echo "4. Watch for console messages when bots use HE spots"
    echo "5. Check ${GREEN}grenade_bot_events.csv${NC} in your game folder"
    echo ""
    echo "Your grenade.json file is at:"
    echo "  $GAME_DIR/maps/de_dust2.grenade.json"
    echo ""
    echo "See GRENADE_SPOTS_FIX.md for detailed testing instructions."
else
    echo -e "${RED}✗ Installation failed!${NC}"
    exit 1
fi
