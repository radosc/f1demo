// Conversation state driven by scripts (beta "say*" opcodes and release "gsay_*" opcodes).
//
// A script builds replies between sayStart and sayEnd: sayReply/sayMessage open a new reply, sayOption adds
// choices to the reply opened last. sayEnd shows the first reply and runs the conversation: choosing an option
// either jumps to a named reply or calls a script procedure; replies added by that procedure are shown next,
// and the conversation ends when a procedure adds none.
#pragma once
#include "game/object.h"

namespace fo {

struct ScriptInstance;
class Vm;

struct DialogOption {
    std::string text;
    int proc = -1;            // procedure to call when chosen
    std::string gotoReply;    // or a named reply to jump to
    int reaction = 0;         // gsay_option reaction adjustment (release scripts)
};

struct DialogReply {
    std::string name;
    std::string text;
    std::vector<DialogOption> options;
    bool message = false;     // sayMessage/gsay_message: no options, click to continue
};

struct DialogHistoryEntry {
    bool player = false;
    std::string text;
};

class DialogSystem {
public:
    // start_gdialog / end_dialogue
    void begin(Object* speaker, ScriptInstance* script, int headFid, int mood, int background);
    void end();
    bool inGameDialog() const { return m_inGameDialog; }
    bool active() const { return !m_frames.empty() || m_inGameDialog; }
    Object* speaker() const;
    std::string speakerName() const;
    int mood() const { return m_mood; }
    void setMood(int m) { m_mood = m; }
    int headFid() const { return m_head; }

    // say* family
    void sayStart(Vm& vm);
    void sayReply(const std::string& name, const std::string& text);
    void sayOption(const std::string& text, int proc, const std::string& gotoReply = "", int reaction = 0);
    void sayMessage(const std::string& name, const std::string& text);
    // Runs the conversation until it ends (modal).
    void sayEnd(Vm& vm);
    void sayQuit();

    // UI interface (used by the dialog screen)
    const DialogReply* current() const;
    // Chooses option `i` of the current reply. Returns false if the conversation ended.
    bool choose(int i);
    // Continue after a reply without options. Returns false if the conversation ended.
    bool advance();
    bool finished() const { return m_frames.empty() || m_frames.back().finished; }
    const std::vector<DialogHistoryEntry>& history() const { return m_history; }

    // Hooks set by the UI layer
    std::function<void()> runModal;      // shows the dialog screen until `finished()`
    std::function<void()> onBarter;

private:
    struct Frame {
        Vm* vm = nullptr;
        std::vector<DialogReply> replies;
        int current = -1;   // reply on screen
        int build = -1;     // reply receiving sayOption calls
        bool running = false;
        bool quit = false;
        bool finished = false;
    };
    Frame* top() { return m_frames.empty() ? nullptr : &m_frames.back(); }
    const Frame* top() const { return m_frames.empty() ? nullptr : &m_frames.back(); }
    void show(Frame& f, int index);
    void finish(Frame& f) { f.finished = true; }

    bool m_inGameDialog = false;
    u32 m_speaker = 0;
    ScriptInstance* m_script = nullptr;
    int m_head = -1, m_mood = 0, m_background = 0;
    std::vector<Frame> m_frames;
    std::vector<DialogHistoryEntry> m_history;
};

extern DialogSystem g_dialog;

} // namespace fo
