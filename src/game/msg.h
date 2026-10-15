// Fallout .MSG message lists: "{number}{sound}{text}" entries.
#pragma once
#include "core/base.h"

namespace fo {

struct MessageEntry {
    std::string sound;
    std::string text;
};

class MessageList {
public:
    // Loads the file from every data layer (base data first); mods override single messages.
    bool load(const std::string& vpath);
    // `merge`: keep the current entries and add / replace the parsed ones.
    bool parse(const std::string& text, bool merge = false);
    const MessageEntry* find(int num) const;
    // Returns the text or "Error" (like the original) when missing.
    std::string get(int num) const;
    bool has(int num) const { return find(num) != nullptr; }
    const std::map<int, MessageEntry>& entries() const { return m_entries; }
    bool loaded() const { return m_loaded; }

private:
    std::map<int, MessageEntry> m_entries;
    bool m_loaded = false;
};

// Cached lists by virtual path ("text/english/game/proto.msg").
MessageList& messages(const std::string& vpath);
// Shortcut for game message files: gameMsg("misc", 100)
std::string gameMsg(const std::string& file, int num);
void messagesClearCache();
extern std::string g_language; // "english"

} // namespace fo
