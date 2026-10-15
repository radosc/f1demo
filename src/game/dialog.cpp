#include "game/dialog.h"
#include "game/hex.h"
#include "game/anim.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/proto.h"
#include "game/script.h"

namespace fo {

DialogSystem g_dialog;

void DialogSystem::begin(Object* speaker, ScriptInstance* script, int headFid, int mood, int background)
{
    m_inGameDialog = true;
    m_speaker = speaker ? speaker->uid : 0;
    m_script = script;
    m_head = headFid;
    m_mood = mood;
    m_background = background;
    m_history.clear();
    Object* dude = g_world.dude;
    if (speaker && dude && speaker != dude && speaker->tile >= 0 && dude->tile >= 0 && speaker->elevation == dude->elevation) {
        // face each other, like the original does when a conversation starts
        if (speaker->isCritter() && critterIsActive(speaker)) speaker->rotation = hex::direction(speaker->tile, dude->tile);
        if (critterIsActive(dude)) dude->rotation = hex::direction(dude->tile, speaker->tile);
    }
}

void DialogSystem::end()
{
    m_inGameDialog = false;
    m_speaker = 0;
    m_script = nullptr;
}

Object* DialogSystem::speaker() const { return g_world.byUid(m_speaker); }

std::string DialogSystem::speakerName() const
{
    Object* s = speaker();
    if (!s) return std::string();
    return s->isCritter() ? critterName(s) : g_protos.name(s->pid);
}

void DialogSystem::sayStart(Vm& vm)
{
    if (m_frames.size() >= 4) {
        LOGW("Dialogue nested too deeply");
        return;
    }
    Frame f;
    f.vm = &vm;
    m_frames.push_back(std::move(f));
}

void DialogSystem::sayReply(const std::string& name, const std::string& text)
{
    Frame* f = top();
    if (!f) return;
    DialogReply r;
    r.name = name;
    r.text = text;
    f->replies.push_back(std::move(r));
    f->build = (int)f->replies.size() - 1;
}

void DialogSystem::sayMessage(const std::string& name, const std::string& text)
{
    Frame* f = top();
    if (!f) return;
    DialogReply r;
    r.name = name;
    r.text = text;
    r.message = true;
    f->replies.push_back(std::move(r));
    f->build = (int)f->replies.size() - 1;
}

void DialogSystem::sayOption(const std::string& text, int proc, const std::string& gotoReply, int reaction)
{
    Frame* f = top();
    if (!f || f->build < 0 || f->build >= (int)f->replies.size()) return;
    DialogOption o;
    o.text = text;
    o.proc = proc;
    o.gotoReply = gotoReply;
    o.reaction = reaction;
    f->replies[f->build].options.push_back(std::move(o));
}

void DialogSystem::sayQuit()
{
    if (Frame* f = top()) f->quit = true;
}

const DialogReply* DialogSystem::current() const
{
    const Frame* f = top();
    if (!f || f->current < 0 || f->current >= (int)f->replies.size()) return nullptr;
    return &f->replies[f->current];
}

void DialogSystem::show(Frame& f, int index)
{
    f.current = index;
    if (index >= 0 && index < (int)f.replies.size()) {
        const std::string& t = f.replies[index].text;
        if (!t.empty()) m_history.push_back({ false, t });
    }
}

void DialogSystem::sayEnd(Vm&)
{
    if (m_frames.empty()) return;
    size_t fi = m_frames.size() - 1;
    if (m_frames[fi].replies.empty()) {
        m_frames.pop_back();
        return;
    }
    m_frames[fi].running = true;
    show(m_frames[fi], 0);
    if (runModal) {
        runModal();
    } else {
        // headless (tests): always take the first option
        int guard = 0;
        while (!m_frames[fi].finished && guard++ < 100) {
            const DialogReply* r = current();
            if (!r || r->options.empty()) {
                if (!advance()) break;
            } else if (!choose(0)) {
                break;
            }
        }
    }
    m_frames.resize(fi);
}

bool DialogSystem::choose(int i)
{
    if (m_frames.empty()) return false;
    size_t fi = m_frames.size() - 1;
    {
        Frame& f = m_frames[fi];
        if (f.finished) return false;
        if (f.current < 0 || f.current >= (int)f.replies.size()) {
            finish(f);
            return false;
        }
        if (i < 0 || i >= (int)f.replies[f.current].options.size()) return true;
    }
    DialogOption opt = m_frames[fi].replies[m_frames[fi].current].options[i];
    if (!opt.text.empty()) m_history.push_back({ true, opt.text });
    size_t before = m_frames[fi].replies.size();
    if (!opt.gotoReply.empty()) {
        Frame& f = m_frames[fi];
        for (int r = 0; r < (int)f.replies.size(); r++) {
            if (strIEquals(f.replies[r].name, opt.gotoReply)) {
                show(f, r);
                return true;
            }
        }
        LOGW("Dialogue: reply '%s' not found", opt.gotoReply.c_str());
        finish(f);
        return false;
    }
    if (opt.proc >= 0 && m_frames[fi].vm) {
        m_frames[fi].build = -1;
        m_frames[fi].vm->callProc(opt.proc);
    }
    if (fi >= m_frames.size()) return false; // the procedure tore the conversation down
    Frame& f = m_frames[fi];
    if (f.quit || !g_world.dude) {
        finish(f);
        return false;
    }
    if (f.replies.size() > before) {
        show(f, (int)before);
        return true;
    }
    finish(f);
    return false;
}

bool DialogSystem::advance()
{
    Frame* f = top();
    if (!f || f->finished) return false;
    if (f->quit) {
        finish(*f);
        return false;
    }
    if (f->current + 1 < (int)f->replies.size()) {
        show(*f, f->current + 1);
        return true;
    }
    finish(*f);
    return false;
}

} // namespace fo
