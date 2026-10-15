// Conversation screen: the talk interface (alltlk/di_talk) over the world view centered on the speaker,
// the reply window, the options list and the review screen.
#include "ui/uikit.h"
#include "game/critter.h"
#include "game/dialog.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/map.h"
#include "game/worldview.h"

#include <windows.h>

namespace fo {

namespace {

// Layout of the original game (640x480 coordinates).
constexpr float kReplyX = 135, kReplyY = 225, kReplyW = 379, kReplyH = 58;
constexpr float kOptX = 127, kOptY = 335, kOptW = 393, kOptH = 117;
constexpr float kPanelY = 290;                 // di_talk.frm
constexpr float kHeadCX = 320, kHeadCY = 112;  // centre of the transparent window in alltlk.frm
constexpr int kFont = 101;

class DialogScreen : public Screen {
public:
    void onEnter() override
    {
        m_camX = g_view.camX;
        m_camY = g_view.camY;
        m_hexColor = g_view.hoverHexColor;
        g_view.hoverHexColor = 0;
        g_view.pathPreview.clear();
        // lift the roof over the speaker so the talk window shows them
        if (Object* s = g_dialog.speaker()) g_map.updateRoofVisibility(s->tile, s->elevation);
    }
    void onExit() override
    {
        g_view.camX = m_camX;
        g_view.camY = m_camY;
        g_view.hoverHexColor = m_hexColor;
        if (Object* d = g_world.dude) g_map.updateRoofVisibility(d->tile, d->elevation);
    }
    bool showsWorld() const override { return true; }

    void update(float dt) override
    {
        m_time += dt;
        centerCamera();
        if (g_dialog.finished()) {
            done = true;
            return;
        }
        m_choice = -1;
        {
            ui::Pass pass(false);
            if (m_review) layoutReview();
            else layoutTalk();
        }
        if (m_choice >= 0) {
            const DialogReply* r = g_dialog.current();
            bool more;
            if (!r || r->options.empty()) more = g_dialog.advance();
            else more = g_dialog.choose(m_choice);
            m_replyScroll = 0;
            if (!more || g_dialog.finished()) done = true;
        }
    }

    void drawUI() override
    {
        ui::Pass pass(true);
        if (m_review) layoutReview();
        else layoutTalk();
    }

private:
    void centerCamera()
    {
        Object* s = g_dialog.speaker();
        if (!s || s->tile < 0) return;
        int x, y;
        hex::center(s->tile, x, y);
        // window pixel of the head window centre -> world canvas pixel
        float wx = (ui::originX() + kHeadCX) * g_app.uiScale / g_app.worldZoom;
        float wy = (ui::originY() + kHeadCY) * g_app.uiScale / g_app.worldZoom;
        g_view.camX = (float)x - (wx - g_view.canvasW * 0.5f);
        g_view.camY = (float)(y - 30) - (wy - g_view.canvasH * 0.5f);
    }

    void layoutTalk()
    {
        using namespace ui;
        letterbox(kBlack);
        draw("alltlk", 0, 0);
        draw("di_talk", 0, kPanelY);

        const DialogReply* r = g_dialog.current();
        int lh = lineHeight(kFont);

        // reply window (scrolls when the text does not fit)
        if (r) {
            auto lines = wrap(kFont, r->text, (int)kReplyW - 10);
            int visible = std::max(1, (int)(kReplyH - 10) / lh);
            int maxScroll = std::max(0, (int)lines.size() - visible);
            if (!drawing() && hovered(kReplyX, kReplyY, kReplyW, kReplyH)) {
                if (g_app.input.wheel > 0) m_replyScroll--;
                if (g_app.input.wheel < 0) m_replyScroll++;
                if (area(kReplyX, kReplyY, kReplyW, kReplyH / 2)) m_replyScroll--;
                if (area(kReplyX, kReplyY + kReplyH / 2, kReplyW, kReplyH / 2)) m_replyScroll++;
            }
            m_replyScroll = clampv(m_replyScroll, 0, maxScroll);
            float y = kReplyY + 10;
            for (int i = m_replyScroll; i < (int)lines.size() && i < m_replyScroll + visible; i++) {
                text(kFont, lines[i], kReplyX + 5, y, kGreen);
                y += (float)lh;
            }
            if (m_replyScroll > 0) text(kFont, "\x18", kReplyX + kReplyW - 10, kReplyY + 2, kGreen);
            if (m_replyScroll < maxScroll) text(kFont, "\x19", kReplyX + kReplyW - 10, kReplyY + kReplyH - lh, kGreen);
        }

        // options
        std::vector<std::string> options;
        if (r) {
            // some demo scripts pass an empty string for their "leave" option
            for (auto& o : r->options) options.push_back(strTrim(o.text).empty() ? std::string("[done]") : o.text);
        }
        if (options.empty()) options.push_back(r && r->message ? "[more]" : "[done]");
        float y = kOptY + 5;
        for (int i = 0; i < (int)options.size(); i++) {
            auto lines = wrap(kFont, std::string("\x95 ") + options[i], (int)kOptW - 12);
            float h = (float)(lines.size() * lh);
            if (y + h > kOptY + kOptH) break;
            bool hot = hovered(kOptX, y - 1, kOptW, h + 2);
            int hotkey = i < 9 ? '1' + i : 0;
            if (area(kOptX, y - 1, kOptW, h + 2, hotkey, SND_NONE)) m_choice = i;
            float ly = y;
            for (auto& line : lines) {
                text(kFont, line, kOptX + 5, ly, hot ? kYellow : kGreen);
                ly += (float)lh;
            }
            y += h + 3;
        }
        if (options.size() == 1 && !drawing() && (key(VK_RETURN) || key(VK_SPACE))) m_choice = 0;

        // buttons on the lower panel
        if (button("di_rest1", "di_rest2", 13, kPanelY + 154, 'R', SND_SMALL)) {
            m_review = true;
            m_reviewTop = std::max(0, (int)g_dialog.history().size() - 6);
        }
        if (button("di_rdbt1", "di_rdbt2", 593, kPanelY + 41, 'B', SND_MEDIUM)) {
            if (g_dialog.onBarter) g_dialog.onBarter();
        }
        button("di_rdbt1", "di_rdbt2", 593, kPanelY + 116, 0, SND_MEDIUM);
    }

    void layoutReview()
    {
        using namespace ui;
        letterbox(kBlack);
        draw("review", 0, 0);
        const auto& hist = g_dialog.history();
        int lh = lineHeight(kFont);
        std::string who = g_dialog.speakerName() + ":";
        std::string me = g_game.player.name + ":";
        float y = 76;
        for (int i = m_reviewTop; i < (int)hist.size(); i++) {
            const DialogHistoryEntry& e = hist[i];
            text(kFont, e.player ? me : who, 88, y, e.player ? kGrey : kGreen);
            y += (float)lh;
            y = textBox(kFont, e.text, 113, y, 309, e.player ? kDarkGrey : kDarkGreen, 418);
            y += 4;
            if (y >= 407) break;
        }
        if (button("di_bgup1", "di_bgup2", 475, 152, VK_UP, SND_MEDIUM) || (!drawing() && g_app.input.wheel > 0))
            m_reviewTop = std::max(0, m_reviewTop - 1);
        if (button("di_bgdn1", "di_bgdn2", 475, 191, VK_DOWN, SND_MEDIUM) || (!drawing() && g_app.input.wheel < 0))
            m_reviewTop = std::min(std::max(0, (int)hist.size() - 1), m_reviewTop + 1);
        if (button("di_done1", "di_done2", 499, 398, VK_ESCAPE, SND_SMALL) || key(VK_RETURN)) m_review = false;
    }

    float m_camX = 0, m_camY = 0;
    u32 m_hexColor = 0;
    float m_time = 0;
    int m_choice = -1;
    int m_replyScroll = 0;
    bool m_review = false;
    int m_reviewTop = 0;
};

} // namespace

void installDialogUi()
{
    g_dialog.runModal = [] { g_app.runModal(std::make_unique<DialogScreen>()); };
}

} // namespace fo
