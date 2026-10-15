#include "ui/recorder.h"
#include "core/fs.h"
#include "core/png.h"

#include <windows.h>

namespace fo {

// Starts ffmpeg with `args`; its standard input is `stdinHandle` (or none), its messages go to record.log.
bool Recorder::run(const std::string& args, void* stdinHandle, void** process)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    std::wstring logPath = utf8ToWide(pathJoin(exeDir(), "record.log"));
    HANDLE log = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinHandle ? (HANDLE)stdinHandle : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log;
    si.hStdError = log;
    std::wstring cmd = utf8ToWide("\"" + m_ffmpeg + "\" " + args);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        LOGE("Recording: could not start %s", m_ffmpeg.c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    if (process) *process = pi.hProcess;
    else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        return code == 0;
    }
    return true;
}

bool Recorder::start(const std::string& path, int width, int height, int fps, const std::string& ffmpeg)
{
    m_path = path;
    m_videoPath = path + ".video.mp4";
    m_ffmpeg = ffmpeg;
    m_width = width & ~1; // x264 with 4:2:0 colour needs even sizes
    m_height = height & ~1;
    m_fps = std::max(1, fps);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 32 << 20)) return false;
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
    std::string args = strFormat("-y -loglevel error -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - -an -c:v libx264 -preset slow -crf 16 "
                                 "-pix_fmt yuv420p \"%s\"",
        m_width, m_height, m_fps, m_videoPath.c_str());
    if (!run(args, readEnd, &m_process)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return false;
    }
    CloseHandle(readEnd);
    m_pipe = writeEnd;
    soundSetTap([this](const SoundEvent& e) {
        m_events.push_back(e);
        m_events.back().time = seconds();
    });
    LOGI("Recording %dx%d at %d fps to %s", m_width, m_height, m_fps, path.c_str());
    return true;
}

void Recorder::addFrame(const Image& img)
{
    if (!m_pipe || img.indexed || img.width < m_width || img.height < m_height) return;
    for (int y = 0; y < m_height; y++) {
        const u8* row = img.row(y);
        DWORD left = (DWORD)m_width * 4, written = 0;
        while (left > 0) {
            if (!WriteFile((HANDLE)m_pipe, row, left, &written, nullptr) || written == 0) {
                LOGE("Recording: ffmpeg stopped reading (see record.log)");
                CloseHandle((HANDLE)m_pipe);
                m_pipe = nullptr;
                return;
            }
            row += written;
            left -= written;
        }
    }
    m_frames++;
}

void Recorder::finish()
{
    if (!m_process) return;
    soundSetTap(nullptr);
    if (m_pipe) CloseHandle((HANDLE)m_pipe);
    m_pipe = nullptr;
    WaitForSingleObject((HANDLE)m_process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess((HANDLE)m_process, &code);
    CloseHandle((HANDLE)m_process);
    m_process = nullptr;
    if (code != 0) {
        LOGE("Recording: ffmpeg failed (see record.log)");
        return;
    }
    std::string wav = m_path + ".audio.wav";
    bool muxed = soundMixToWav(m_events, seconds(), wav) &&
        run(strFormat("-y -nostdin -loglevel error -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k -shortest -movflags +faststart \"%s\"",
                m_videoPath.c_str(), wav.c_str(), m_path.c_str()),
            nullptr, nullptr);
    if (muxed) {
        DeleteFileW(utf8ToWide(m_videoPath).c_str());
        DeleteFileW(utf8ToWide(wav).c_str());
    } else {
        MoveFileExW(utf8ToWide(m_videoPath).c_str(), utf8ToWide(m_path).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    LOGI("Recording finished: %lld frames (%.1f s), %d sounds -> %s", m_frames, seconds(), (int)m_events.size(), m_path.c_str());
}

} // namespace fo
