#include "game/settings.h"
#include "core/fs.h"
#include "core/json.h"
#include "audio/sound.h"
#include "game/anim.h"
#include "game/game.h"
#include "gfx/font.h"
#include "gfx/palette.h"
#include "gfx/sprite.h"
#include "ui/app.h"

namespace fo {

Settings g_settings;

static std::string settingsPath() { return pathJoin(g_app.userDir.empty() ? exeDir() : g_app.userDir, "settings.json"); }

void Settings::apply() const
{
    g_game.difficulty = gameDifficulty;
    g_game.combatDifficulty = combatDifficulty;
    g_game.violenceLevel = violenceLevel;
    soundSetVolumes(masterVolume, sfxVolume, musicVolume);
    g_palette.setBrightness(brightness);
    g_sprites.setUseHD(hdSprites);
    fontsSetUseHD(hdSprites);
    g_animSpeed = 1.0f + combatSpeed / 50.0f;
}

void settingsLoad()
{
    Bytes data;
    if (fileRead(settingsPath(), data)) {
        Json j = Json::parse(std::string(data.begin(), data.end()));
        Settings& s = g_settings;
        s.gameDifficulty = clampv(j["gameDifficulty"].toInt(s.gameDifficulty), 0, 2);
        s.combatDifficulty = clampv(j["combatDifficulty"].toInt(s.combatDifficulty), 0, 2);
        s.violenceLevel = clampv(j["violenceLevel"].toInt(s.violenceLevel), 0, 3);
        s.targetHighlight = clampv(j["targetHighlight"].toInt(s.targetHighlight), 0, 2);
        s.combatMessagesVerbose = j["combatMessagesVerbose"].b(s.combatMessagesVerbose);
        s.combatTaunts = j["combatTaunts"].b(s.combatTaunts);
        s.languageFilter = j["languageFilter"].b(s.languageFilter);
        s.alwaysRun = j["alwaysRun"].b(s.alwaysRun);
        s.subtitles = j["subtitles"].b(s.subtitles);
        s.itemHighlight = j["itemHighlight"].b(s.itemHighlight);
        s.combatSpeed = clampv(j["combatSpeed"].toInt(s.combatSpeed), 0, 50);
        s.textDelay = clampv((float)j["textDelay"].d(s.textDelay), 1.0f, 6.0f);
        s.masterVolume = clampv((float)j["masterVolume"].d(s.masterVolume), 0.0f, 1.0f);
        s.musicVolume = clampv((float)j["musicVolume"].d(s.musicVolume), 0.0f, 1.0f);
        s.sfxVolume = clampv((float)j["sfxVolume"].d(s.sfxVolume), 0.0f, 1.0f);
        s.speechVolume = clampv((float)j["speechVolume"].d(s.speechVolume), 0.0f, 1.0f);
        s.brightness = clampv((float)j["brightness"].d(s.brightness), 1.0f, 1.18f);
        s.mouseSensitivity = clampv((float)j["mouseSensitivity"].d(s.mouseSensitivity), 1.0f, 2.5f);
        s.hdSprites = j["hdSprites"].b(s.hdSprites);
        s.fullscreen = j["fullscreen"].b(s.fullscreen);
        s.vsync = j["vsync"].b(s.vsync);
    }
    g_settings.apply();
}

void settingsSave()
{
    const Settings& s = g_settings;
    Json j = Json::object();
    j.set("gameDifficulty", Json(s.gameDifficulty));
    j.set("combatDifficulty", Json(s.combatDifficulty));
    j.set("violenceLevel", Json(s.violenceLevel));
    j.set("targetHighlight", Json(s.targetHighlight));
    j.set("combatMessagesVerbose", Json(s.combatMessagesVerbose));
    j.set("combatTaunts", Json(s.combatTaunts));
    j.set("languageFilter", Json(s.languageFilter));
    j.set("alwaysRun", Json(s.alwaysRun));
    j.set("subtitles", Json(s.subtitles));
    j.set("itemHighlight", Json(s.itemHighlight));
    j.set("combatSpeed", Json(s.combatSpeed));
    j.set("textDelay", Json((double)s.textDelay));
    j.set("masterVolume", Json((double)s.masterVolume));
    j.set("musicVolume", Json((double)s.musicVolume));
    j.set("sfxVolume", Json((double)s.sfxVolume));
    j.set("speechVolume", Json((double)s.speechVolume));
    j.set("brightness", Json((double)s.brightness));
    j.set("mouseSensitivity", Json((double)s.mouseSensitivity));
    j.set("hdSprites", Json(s.hdSprites));
    j.set("fullscreen", Json(s.fullscreen));
    j.set("vsync", Json(s.vsync));
    std::string text = j.dump(2);
    fileWrite(settingsPath(), text.data(), text.size());
}

} // namespace fo
