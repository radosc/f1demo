#include "game/msg.h"
#include "core/fs.h"

namespace fo {

std::string g_language = "english";

// Every copy of the file is read, base data first: a mod's .msg only needs the lines it changes.
bool MessageList::load(const std::string& vpath)
{
    m_entries.clear();
    bool any = false;
    for (auto& path : g_vfs.resolveAll(vpath)) {
        Bytes b;
        if (!fileRead(path, b)) continue;
        parse(std::string(b.begin(), b.end()), true);
        any = true;
    }
    return any;
}

bool MessageList::parse(const std::string& text, bool merge)
{
    if (!merge) m_entries.clear();
    size_t pos = 0;
    auto readField = [&](std::string& out) -> int {
        out.clear();
        while (pos < text.size() && text[pos] != '{') {
            if (text[pos] == '}') return 2;
            pos++;
        }
        if (pos >= text.size()) return 1;
        pos++;
        while (pos < text.size() && text[pos] != '}') {
            char c = text[pos++];
            if (c != '\n' && c != '\r') out += c;
        }
        if (pos >= text.size()) return 3;
        pos++;
        return 0;
    };
    for (;;) {
        std::string num, sound, body;
        int rc = readField(num);
        if (rc == 1) break;
        if (rc != 0) {
            LOGW("Message file: mismatched delimiters near offset %d", (int)pos);
            // resynchronize past the stray brace
            pos++;
            continue;
        }
        if (readField(sound) != 0 || readField(body) != 0) break;
        int n = atoi(strTrim(num).c_str());
        m_entries[n] = MessageEntry{ sound, body };
    }
    m_loaded = true;
    return true;
}

const MessageEntry* MessageList::find(int num) const
{
    auto it = m_entries.find(num);
    return it == m_entries.end() ? nullptr : &it->second;
}

std::string MessageList::get(int num) const
{
    const MessageEntry* e = find(num);
    return e ? e->text : std::string("Error");
}

static std::unordered_map<std::string, std::unique_ptr<MessageList>> g_msgCache;

MessageList& messages(const std::string& vpath)
{
    std::string key = pathNormalize(vpath);
    auto it = g_msgCache.find(key);
    if (it != g_msgCache.end()) return *it->second;
    auto ml = std::make_unique<MessageList>();
    if (!ml->load(key)) LOGW("Missing message file %s", key.c_str());
    MessageList& ref = *ml;
    g_msgCache[key] = std::move(ml);
    return ref;
}

std::string gameMsg(const std::string& file, int num)
{
    return messages("text/" + g_language + "/game/" + file + ".msg").get(num);
}

void messagesClearCache() { g_msgCache.clear(); }

} // namespace fo
