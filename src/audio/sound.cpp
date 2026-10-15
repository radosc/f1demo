#include "audio/sound.h"
#include "core/fs.h"
#include "game/hex.h"
#include "game/object.h"

#include <windows.h>
#include <xaudio2.h>

#include <deque>

namespace fo {

namespace {

struct WaveData {
    WAVEFORMATEX fmt = {};
    Bytes pcm;
    bool valid = false;
};

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SubmixVoice* g_sfxBus = nullptr;
IXAudio2SubmixVoice* g_musicBus = nullptr;
std::unordered_map<std::string, std::shared_ptr<WaveData>> g_cache;
struct Playing {
    IXAudio2SourceVoice* voice;
    std::shared_ptr<WaveData> data;
};
std::vector<Playing> g_playing;
IXAudio2SourceVoice* g_musicVoice = nullptr;
std::shared_ptr<WaveData> g_musicData;
std::string g_musicName;
std::function<void(const SoundEvent&)> g_tap;

void tap(int kind, const std::string& path, float volume = 1.0f, bool loop = false)
{
    if (!g_tap) return;
    SoundEvent e;
    e.kind = kind;
    e.path = path;
    e.volume = volume;
    e.loop = loop;
    g_tap(e);
}

IXAudio2SourceVoice* g_streamVoice = nullptr;
std::deque<std::vector<i16>> g_streamChunks; // queued buffers, kept alive until played
float g_masterVol = 1.0f, g_sfxVol = 1.0f, g_musicVol = 0.7f;
bool g_comInit = false;

bool parseWav(const Bytes& b, WaveData& w)
{
    if (b.size() < 44 || memcmp(b.data(), "RIFF", 4) || memcmp(&b[8], "WAVE", 4)) return false;
    size_t pos = 12;
    bool haveFmt = false;
    while (pos + 8 <= b.size()) {
        u32 len = rdLE32(&b[pos + 4]);
        const u8* body = &b[pos + 8];
        if (!memcmp(&b[pos], "fmt ", 4) && len >= 16) {
            w.fmt.wFormatTag = rdLE16(body);
            w.fmt.nChannels = rdLE16(body + 2);
            w.fmt.nSamplesPerSec = rdLE32(body + 4);
            w.fmt.nAvgBytesPerSec = rdLE32(body + 8);
            w.fmt.nBlockAlign = rdLE16(body + 12);
            w.fmt.wBitsPerSample = rdLE16(body + 14);
            w.fmt.cbSize = 0;
            haveFmt = true;
        } else if (!memcmp(&b[pos], "data", 4)) {
            size_t n = std::min<size_t>(len, b.size() - pos - 8);
            w.pcm.assign(body, body + n);
        }
        pos += 8 + len + (len & 1);
    }
    w.valid = haveFmt && w.fmt.wFormatTag == WAVE_FORMAT_PCM && !w.pcm.empty();
    return w.valid;
}

std::shared_ptr<WaveData> load(const std::string& vpath)
{
    std::string key = pathNormalize(vpath);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    auto w = std::make_shared<WaveData>();
    Bytes b;
    if (g_vfs.read(key, b)) parseWav(b, *w);
    if (!w->valid) w.reset();
    g_cache[key] = w;
    return w;
}

bool g_muted = false;

void applyVolumes()
{
    if (g_master) g_master->SetVolume(g_muted ? 0.0f : g_masterVol);
    if (g_sfxBus) g_sfxBus->SetVolume(g_sfxVol);
    if (g_musicBus) g_musicBus->SetVolume(g_musicVol);
}

} // namespace

bool soundInit()
{
    if (g_xa) return true;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    g_comInit = SUCCEEDED(hr);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        LOGW("XAudio2 unavailable - sound disabled");
        g_xa = nullptr;
        return false;
    }
    if (FAILED(g_xa->CreateMasteringVoice(&g_master))) {
        LOGW("No audio device - sound disabled");
        g_xa->Release();
        g_xa = nullptr;
        return false;
    }
    XAUDIO2_VOICE_DETAILS md;
    g_master->GetVoiceDetails(&md);
    g_xa->CreateSubmixVoice(&g_sfxBus, md.InputChannels, md.InputSampleRate);
    g_xa->CreateSubmixVoice(&g_musicBus, md.InputChannels, md.InputSampleRate);
    applyVolumes();
    LOGI("Audio initialized (%u channels, %u Hz)", md.InputChannels, md.InputSampleRate);
    return true;
}

void soundShutdown()
{
    soundStreamStop();
    soundStopMusic();
    for (auto& p : g_playing) p.voice->DestroyVoice();
    g_playing.clear();
    if (g_sfxBus) g_sfxBus->DestroyVoice(), g_sfxBus = nullptr;
    if (g_musicBus) g_musicBus->DestroyVoice(), g_musicBus = nullptr;
    if (g_master) g_master->DestroyVoice(), g_master = nullptr;
    if (g_xa) g_xa->Release(), g_xa = nullptr;
    g_cache.clear();
    if (g_comInit) CoUninitialize();
}

void soundUpdate()
{
    if (g_streamVoice) {
        XAUDIO2_VOICE_STATE st;
        g_streamVoice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (g_streamChunks.size() > st.BuffersQueued) g_streamChunks.pop_front();
    }
    for (size_t i = 0; i < g_playing.size();) {
        XAUDIO2_VOICE_STATE st;
        g_playing[i].voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) {
            g_playing[i].voice->DestroyVoice();
            g_playing.erase(g_playing.begin() + (long long)i);
        } else {
            i++;
        }
    }
}

bool soundExists(const std::string& name) { return load("sound/sfx/" + strLower(name) + ".wav") != nullptr; }

static bool playData(const std::shared_ptr<WaveData>& w, float volume)
{
    if (!g_xa || !w || g_playing.size() > 48) return false;
    XAUDIO2_SEND_DESCRIPTOR send = { 0, g_sfxBus };
    XAUDIO2_VOICE_SENDS sends = { 1, &send };
    IXAudio2SourceVoice* v = nullptr;
    if (FAILED(g_xa->CreateSourceVoice(&v, &w->fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends))) return false;
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = (UINT32)w->pcm.size();
    buf.pAudioData = w->pcm.data();
    buf.Flags = XAUDIO2_END_OF_STREAM;
    v->SetVolume(volume);
    v->SubmitSourceBuffer(&buf);
    v->Start();
    g_playing.push_back({ v, w });
    return true;
}

bool soundPlaySfx(const std::string& name, float volume)
{
    if (name.empty()) return false;
    std::string path = "sound/sfx/" + strLower(name) + ".wav";
    auto w = load(path);
    if (!w) return false;
    tap(SNDEV_SFX, path, volume);
    return playData(w, volume);
}

bool soundPlaySfxAt(const std::string& name, Object* source)
{
    float vol = 1.0f;
    Object* dude = g_world.dude;
    if (source && dude && source->tile >= 0 && dude->tile >= 0) {
        if (source->elevation != dude->elevation) return false;
        int d = hex::distance(source->tile, dude->tile);
        vol = clampv(1.0f - (float)d / 40.0f, 0.15f, 1.0f);
    }
    return soundPlaySfx(name, vol);
}

void soundPlayMusic(const std::string& name, bool loop)
{
    if (!g_xa) return;
    if (g_musicVoice && g_musicName == strLower(name)) return;
    soundStopMusic();
    std::string path = "sound/music/" + strLower(name) + ".wav";
    auto w = load(path);
    if (!w) return;
    tap(SNDEV_MUSIC, path, 1.0f, loop);
    XAUDIO2_SEND_DESCRIPTOR send = { 0, g_musicBus };
    XAUDIO2_VOICE_SENDS sends = { 1, &send };
    if (FAILED(g_xa->CreateSourceVoice(&g_musicVoice, &w->fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends))) {
        g_musicVoice = nullptr;
        return;
    }
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = (UINT32)w->pcm.size();
    buf.pAudioData = w->pcm.data();
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    g_musicVoice->SubmitSourceBuffer(&buf);
    g_musicVoice->Start();
    g_musicData = w;
    g_musicName = strLower(name);
}

void soundStopMusic()
{
    if (g_musicVoice) tap(SNDEV_MUSIC_STOP, g_musicName);
    if (g_musicVoice) {
        g_musicVoice->Stop();
        g_musicVoice->DestroyVoice();
        g_musicVoice = nullptr;
    }
    g_musicData.reset();
    g_musicName.clear();
}

void soundPauseMusic(bool pause)
{
    if (!g_musicVoice) return;
    tap(pause ? SNDEV_MUSIC_PAUSE : SNDEV_MUSIC_RESUME, g_musicName);
    if (pause) g_musicVoice->Stop();
    else g_musicVoice->Start();
}

bool soundStreamStart(int rate, int channels)
{
    soundStreamStop();
    if (!g_xa || rate <= 0 || channels <= 0) return false;
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = (WORD)channels;
    fmt.nSamplesPerSec = (DWORD)rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(channels * 2);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    XAUDIO2_SEND_DESCRIPTOR send = { 0, g_musicBus };
    XAUDIO2_VOICE_SENDS sends = { 1, &send };
    if (FAILED(g_xa->CreateSourceVoice(&g_streamVoice, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends))) {
        g_streamVoice = nullptr;
        return false;
    }
    g_streamVoice->Start();
    return true;
}

void soundStreamQueue(std::vector<i16> samples)
{
    if (!g_streamVoice || samples.empty()) return;
    g_streamChunks.push_back(std::move(samples));
    const auto& chunk = g_streamChunks.back();
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = (UINT32)(chunk.size() * sizeof(i16));
    buf.pAudioData = (const BYTE*)chunk.data();
    g_streamVoice->SubmitSourceBuffer(&buf);
}

void soundStreamStop()
{
    if (g_streamVoice) {
        g_streamVoice->Stop();
        g_streamVoice->DestroyVoice();
        g_streamVoice = nullptr;
    }
    g_streamChunks.clear();
}

void soundSetTap(std::function<void(const SoundEvent&)> t) { g_tap = std::move(t); }

void soundSetMuted(bool muted)
{
    g_muted = muted;
    applyVolumes();
}

namespace {

// One source sample frame as stereo floats (8-bit unsigned or 16-bit signed PCM, mono or stereo).
void sampleAt(const WaveData& w, size_t frame, float& l, float& r)
{
    int ch = std::max<int>(1, w.fmt.nChannels);
    if (w.fmt.wBitsPerSample == 8) {
        const u8* p = w.pcm.data() + frame * (size_t)ch;
        l = ((int)p[0] - 128) / 128.0f;
        r = ch > 1 ? ((int)p[1] - 128) / 128.0f : l;
    } else {
        const u8* p = w.pcm.data() + frame * (size_t)ch * 2;
        l = (i16)rdLE16(p) / 32768.0f;
        r = ch > 1 ? (i16)rdLE16(p + 2) / 32768.0f : l;
    }
}

size_t frameCount(const WaveData& w)
{
    size_t bytesPerFrame = (size_t)std::max<int>(1, w.fmt.nChannels) * (w.fmt.wBitsPerSample == 8 ? 1 : 2);
    return w.pcm.size() / bytesPerFrame;
}

// Adds `w` (resampled) into `mix` (stereo 44.1 kHz) for `count` output frames from frame `at`, starting at
// `srcSeconds` into the sound; loops when asked. Returns the position reached in the sound.
double addSound(std::vector<float>& mix, const WaveData& w, size_t at, size_t count, double srcSeconds, bool loop, float gain)
{
    const double rate = 44100.0;
    size_t frames = frameCount(w);
    if (!frames || !w.fmt.nSamplesPerSec) return srcSeconds;
    double srcLen = (double)frames / w.fmt.nSamplesPerSec;
    size_t total = mix.size() / 2;
    for (size_t i = 0; i < count && at + i < total; i++) {
        double t = srcSeconds + (double)i / rate;
        if (t >= srcLen) {
            if (!loop) break;
            t = fmod(t, srcLen);
        }
        double pos = t * w.fmt.nSamplesPerSec;
        size_t f0 = std::min((size_t)pos, frames - 1), f1 = std::min(f0 + 1, frames - 1);
        float frac = (float)(pos - (double)f0);
        float l0, r0, l1, r1;
        sampleAt(w, f0, l0, r0);
        sampleAt(w, f1, l1, r1);
        mix[(at + i) * 2] += (l0 + (l1 - l0) * frac) * gain;
        mix[(at + i) * 2 + 1] += (r0 + (r1 - r0) * frac) * gain;
    }
    double end = srcSeconds + (double)count / rate;
    return loop ? fmod(end, srcLen) : end;
}

} // namespace

bool soundMixToWav(const std::vector<SoundEvent>& events, double seconds, const std::string& outPath)
{
    const double rate = 44100.0;
    size_t total = (size_t)(seconds * rate);
    if (!total) return false;
    std::vector<float> mix(total * 2, 0.0f);
    auto frameAt = [&](double t) { return std::min(total, (size_t)std::max(0.0, t * rate)); };
    // music: one track at a time, with pauses
    std::shared_ptr<WaveData> music;
    bool musicLoop = false, playing = false;
    double musicPos = 0, segStart = 0;
    auto flushMusic = [&](double until) {
        if (music && playing && until > segStart) {
            size_t a = frameAt(segStart), b = frameAt(until);
            musicPos = addSound(mix, *music, a, b - a, musicPos, musicLoop, g_musicVol * g_masterVol);
        }
        segStart = until;
    };
    for (auto& e : events) {
        switch (e.kind) {
        case SNDEV_SFX:
            if (auto w = load(e.path)) {
                size_t a = frameAt(e.time);
                addSound(mix, *w, a, total - a, 0.0, false, e.volume * g_sfxVol * g_masterVol);
            }
            break;
        case SNDEV_MUSIC:
            flushMusic(e.time);
            music = load(e.path);
            musicLoop = e.loop;
            musicPos = 0;
            playing = music != nullptr;
            break;
        case SNDEV_MUSIC_STOP:
            flushMusic(e.time);
            music.reset();
            playing = false;
            break;
        case SNDEV_MUSIC_PAUSE:
            flushMusic(e.time);
            playing = false;
            break;
        case SNDEV_MUSIC_RESUME:
            flushMusic(e.time);
            playing = music != nullptr;
            break;
        }
    }
    flushMusic(seconds);
    // 16-bit stereo WAV
    Bytes out(44 + total * 4);
    auto put32 = [&](size_t at, u32 v) { memcpy(&out[at], &v, 4); };
    auto put16 = [&](size_t at, u16 v) { memcpy(&out[at], &v, 2); };
    memcpy(&out[0], "RIFF", 4);
    put32(4, (u32)(out.size() - 8));
    memcpy(&out[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, 44100);
    put32(28, 44100 * 4);
    put16(32, 4);
    put16(34, 16);
    memcpy(&out[36], "data", 4);
    put32(40, (u32)(total * 4));
    for (size_t i = 0; i < total * 2; i++) {
        int v = (int)lrintf(clampv(mix[i], -1.0f, 1.0f) * 32767.0f);
        put16(44 + i * 2, (u16)(i16)v);
    }
    return fileWrite(outPath, out.data(), out.size());
}

void soundSetVolumes(float master, float sfx, float music)
{
    g_masterVol = clampv(master, 0.0f, 1.0f);
    g_sfxVol = clampv(sfx, 0.0f, 1.0f);
    g_musicVol = clampv(music, 0.0f, 1.0f);
    applyVolumes();
}

float soundMasterVolume() { return g_masterVol; }
float soundSfxVolume() { return g_sfxVol; }
float soundMusicVolume() { return g_musicVol; }

} // namespace fo
