// Video capture (--record <file.mp4>): the game runs on a fixed time step, every frame is piped to ffmpeg,
// and the sounds played meanwhile are mixed into the soundtrack when the recording ends.
#pragma once
#include "audio/sound.h"
#include "core/base.h"

namespace fo {

struct Image;

class Recorder {
public:
    // `ffmpeg`: path of ffmpeg.exe (or just "ffmpeg" when it is on PATH).
    bool start(const std::string& path, int width, int height, int fps, const std::string& ffmpeg);
    bool active() const { return m_pipe != nullptr; }
    int fps() const { return m_fps; }
    double seconds() const { return (double)m_frames / (double)m_fps; }
    void addFrame(const Image& rgba);
    // Closes the video and adds the soundtrack.
    void finish();

private:
    bool run(const std::string& args, void* stdinHandle, void** process);

    std::string m_path, m_videoPath, m_ffmpeg;
    int m_width = 0, m_height = 0, m_fps = 30;
    long long m_frames = 0;
    void* m_pipe = nullptr;
    void* m_process = nullptr;
    std::vector<SoundEvent> m_events;
};

} // namespace fo
