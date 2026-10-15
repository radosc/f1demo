// User preferences (options screen), stored in user/settings.json.
#pragma once
#include "core/base.h"

namespace fo {

struct Settings {
    // game
    int gameDifficulty = 1;        // 0 easy, 1 normal, 2 hard
    int combatDifficulty = 1;      // 0 wimpy, 1 normal, 2 rough
    int violenceLevel = 3;         // 0 none, 1 minimal, 2 normal, 3 maximum blood
    int targetHighlight = 2;       // 0 off, 1 on, 2 targeting only
    bool combatMessagesVerbose = true;
    bool combatTaunts = true;
    bool languageFilter = false;
    bool alwaysRun = false;
    bool subtitles = false;
    bool itemHighlight = true;
    int combatSpeed = 0;           // 0..50 (higher = faster combat animations)
    float textDelay = 3.5f;        // floating text base display time in seconds (1..6)
    // sound
    float masterVolume = 1.0f, musicVolume = 0.7f, sfxVolume = 1.0f, speechVolume = 1.0f;
    // display / input
    float brightness = 1.0f;       // 1.0 .. 1.18 like the original slider
    float mouseSensitivity = 1.0f; // 1.0 .. 2.5
    // port extras
    bool hdSprites = true;
    bool fullscreen = false;
    bool vsync = true;

    // Pushes the values into the game state, sound mixer, palette and sprite cache.
    void apply() const;
};

extern Settings g_settings;
void settingsLoad();   // reads <user dir>/settings.json (defaults when missing)
void settingsSave();

} // namespace fo
