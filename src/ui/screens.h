// Factories for the interface screens (each runs modally on top of the game screen).
#pragma once
#include "ui/app.h"

namespace fo {

struct Object;

enum InventoryMode : int { INV_NORMAL = 0, INV_LOOT, INV_STEAL, INV_USE_ON };

std::unique_ptr<Screen> makeMainMenu();
std::unique_ptr<Screen> makeGameScreen();
// Inventory. INV_LOOT/INV_STEAL show `target`'s inventory next to the player's; INV_USE_ON picks an item
// to use on `target` (the chosen item uid is written to *chosen).
std::unique_ptr<Screen> makeInventoryScreen(int mode, Object* target, u32* chosen = nullptr);
std::unique_ptr<Screen> makeCharacterScreen(bool creation, bool* accepted = nullptr);
// Skilldex: writes the chosen skill (or -1) to *skill.
std::unique_ptr<Screen> makeSkilldexScreen(int* skill);
std::unique_ptr<Screen> makePipboyScreen();
std::unique_ptr<Screen> makeAutomapScreen();
std::unique_ptr<Screen> makeOptionsScreen(bool* quitToMenu);
// Stand-alone LOAD GAME screen (main menu); *loaded = true after a successful gameLoad().
std::unique_ptr<Screen> makeLoadGameScreen(bool* loaded);
std::unique_ptr<Screen> makeHelpScreen();
// Called shot location picker: writes HIT_* (or -1 when cancelled).
std::unique_ptr<Screen> makeCalledShotScreen(Object* target, int hitMode, int* location);
// Simple message box (text lines, OK button); `yesNo` adds a second button and writes the answer to *yes.
std::unique_ptr<Screen> makeMessageBox(const std::vector<std::string>& lines, bool yesNo = false, bool* yes = nullptr);
// Full screen movie (data/movies/<name>.mve); finishes at the end or when skipped.
std::unique_ptr<Screen> makeMovieScreen(const std::string& name);
// Map editor for maps/<map>.json (a new map when it does not exist); saves to mods/<mod>/maps/<saveAs>.json.
std::unique_ptr<Screen> makeEditorScreen(const std::string& map, const std::string& mod, const std::string& saveAs);

} // namespace fo
