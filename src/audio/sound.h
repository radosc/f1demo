// XAudio2 sound effects and music (WAV files from data/sound).
#pragma once
#include "core/base.h"

namespace fo {

struct Object;

bool soundInit();
void soundShutdown();
void soundUpdate();
// Plays data/sound/sfx/<name>.wav (name without extension, case-insensitive). Returns false if missing.
bool soundPlaySfx(const std::string& name, float volume = 1.0f);
// Positional variant: volume falls off with the distance from the player.
bool soundPlaySfxAt(const std::string& name, Object* source);
void soundPlayMusic(const std::string& name, bool loop = true);
void soundStopMusic();
// Pauses / resumes the current music (movies play in silence over a paused menu track).
void soundPauseMusic(bool pause);
// Recording (--record): every sound and music change is reported, so the soundtrack can be mixed offline.
enum SoundEventKind : int { SNDEV_SFX = 0, SNDEV_MUSIC, SNDEV_MUSIC_STOP, SNDEV_MUSIC_PAUSE, SNDEV_MUSIC_RESUME };
struct SoundEvent {
    double time = 0;   // seconds into the recording
    int kind = SNDEV_SFX;
    std::string path;  // data path of the WAV
    float volume = 1;  // before the sfx / music / master volumes
    bool loop = false;
};
void soundSetTap(std::function<void(const SoundEvent&)> tap);
// Silences the speakers without changing the volume settings (--mute: recordings and tests).
void soundSetMuted(bool muted);
// Mixes the events into a 44.1 kHz stereo 16-bit WAV of `seconds` length (current volume settings).
bool soundMixToWav(const std::vector<SoundEvent>& events, double seconds, const std::string& outPath);

// Streamed signed 16-bit interleaved PCM (movie soundtracks), one stream at a time on the music bus.
bool soundStreamStart(int rate, int channels);
void soundStreamQueue(std::vector<i16> samples);
void soundStreamStop();
void soundSetVolumes(float master, float sfx, float music);
float soundMasterVolume();
float soundSfxVolume();
float soundMusicVolume();
bool soundExists(const std::string& name);

} // namespace fo
