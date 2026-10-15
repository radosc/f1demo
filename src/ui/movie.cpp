// Full screen movie player for Interplay MVE files (data/movies/<name>.mve). A click or Esc / Space / Enter
// skips. The menu music pauses while a movie plays; a movie's own soundtrack (the demo's have none) streams.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "core/fs.h"
#include "media/mve.h"

#include <windows.h>

namespace fo {
namespace {

class MovieScreen : public Screen {
public:
    explicit MovieScreen(std::string name) : m_name(std::move(name)) {}

    void onEnter() override
    {
        Bytes data;
        std::string err;
        if (!g_vfs.read("movies/" + m_name + ".mve", data)) err = "file missing";
        else if (!m_mve.open(std::move(data), &err) && err.empty()) err = "not a movie";
        if (!err.empty()) {
            LOGW("Movie %s not played: %s", m_name.c_str(), err.c_str());
            done = true;
            return;
        }
        LOGI("Movie %s: %dx%d, %d frames at %.2f fps%s", m_name.c_str(), m_mve.width(), m_mve.height(), m_mve.frameCount(),
            1.0 / m_mve.frameDuration(), m_mve.hasAudio() ? ", with sound" : "");
        soundPauseMusic(true);
        g_app.setCursorVisible(false);
    }

    void onExit() override
    {
        soundStreamStop();
        soundPauseMusic(false);
        g_app.setCursorVisible(true);
    }

    void update(float) override
    {
        Input& in = g_app.input;
        if (in.pressed[0] || in.pressed[1] || in.key(VK_ESCAPE) || in.key(VK_SPACE) || in.key(VK_RETURN)) {
            done = true;
            return;
        }
        if (m_start < 0) m_start = g_app.time;
        double t = g_app.time - m_start;
        int want = (int)(t / m_mve.frameDuration());
        bool decoded = false;
        while (!m_ended && m_mve.frameIndex() < want) {
            if (!m_mve.nextFrame()) {
                m_ended = true;
                break;
            }
            decoded = true;
            if (m_mve.hasAudio()) {
                // audio sample 0 belongs to frame 0: the voice starts with the first frame
                if (m_mve.frameIndex() == 0) soundStreamStart(m_mve.audioRate(), m_mve.audioChannels());
                soundStreamQueue(m_mve.takeAudio());
            }
        }
        if (decoded) upload();
        // the last frame stays up for its own duration
        if (m_ended && t >= (double)std::max(1, m_mve.frameIndex() + 1) * m_mve.frameDuration()) done = true;
    }

    void drawUI() override
    {
        ui::Pass pass(true);
        float L = -ui::originX(), T = -ui::originY(), W = (float)g_app.uiW, H = (float)g_app.uiH;
        ui::fill(L, T, W, H, ui::kBlack);
        if (!m_tex) return;
        // fit the window, keeping the aspect ratio
        float s = std::min(W / (float)m_mve.width(), H / (float)m_mve.height());
        float w = m_mve.width() * s, h = m_mve.height() * s;
        float S = ui::texScale();
        g_renderer.draw(m_tex, 0, 0, (float)m_tex->width, (float)m_tex->height, (W - w) / 2 * S, (H - h) / 2 * S, w * S, h * S);
    }

private:
    // The frame as an RGBA texture (scaled up bilinearly when drawn).
    void upload()
    {
        const auto& idx = m_mve.frame();
        const u32* pal = m_mve.palette();
        Image img;
        img.allocRGBA(m_mve.width(), m_mve.height());
        u8* p = img.pixels.data();
        for (size_t i = 0; i < idx.size(); i++, p += 4) {
            u32 c = pal[idx[i]];
            p[0] = (u8)(c >> 16);
            p[1] = (u8)(c >> 8);
            p[2] = (u8)c;
            p[3] = 255;
        }
        m_tex = g_renderer.createTexture(img);
    }

    std::string m_name;
    MveDecoder m_mve;
    TexturePtr m_tex;
    double m_start = -1;
    bool m_ended = false;
};

} // namespace

std::unique_ptr<Screen> makeMovieScreen(const std::string& name) { return std::make_unique<MovieScreen>(name); }

} // namespace fo
