#include "game/scriptnames.h"
#include "core/fs.h"

namespace fo {

ScriptNames g_scriptNames;

void ScriptNames::loadFrom(const std::string& text)
{
    m_entries.clear();
    m_byName.clear();
    for (auto& rawLine : strSplit(text, '\n')) {
        std::string line = rawLine;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ScriptListEntry e;
        size_t semi = line.find(';');
        std::string head = semi == std::string::npos ? line : line.substr(0, semi);
        std::string tail = semi == std::string::npos ? std::string() : line.substr(semi + 1);
        size_t hash = tail.find('#');
        std::string info = hash == std::string::npos ? std::string() : tail.substr(hash + 1);
        e.comment = strTrim(hash == std::string::npos ? tail : tail.substr(0, hash));
        std::string file = strLower(strTrim(head));
        size_t dot = file.find('.');
        e.name = dot == std::string::npos ? file : file.substr(0, dot);
        if (info.find("map_init") != std::string::npos) e.mapInit = true;
        size_t lv = info.find("local_vars=");
        if (lv != std::string::npos) e.localVars = atoi(info.c_str() + lv + 11);
        if (e.name.empty() && strTrim(line).empty()) continue;
        if (!e.name.empty() && !m_byName.count(e.name)) m_byName[e.name] = (int)m_entries.size();
        m_entries.push_back(e);
    }
    m_loaded = true;
}

void ScriptNames::ensureLoaded()
{
    if (m_loaded) return;
    std::string text;
    if (!g_vfs.readText("scripts/scripts.lst", text)) LOGW("scripts/scripts.lst missing");
    loadFrom(text);
}

const ScriptListEntry* ScriptNames::get(int index)
{
    ensureLoaded();
    if (index < 0 || index >= (int)m_entries.size()) return nullptr;
    return &m_entries[index];
}

std::string ScriptNames::name(int index)
{
    const ScriptListEntry* e = get(index);
    return e ? e->name : std::string();
}

int ScriptNames::indexOf(const std::string& n)
{
    ensureLoaded();
    auto it = m_byName.find(strLower(n));
    return it == m_byName.end() ? -1 : it->second;
}

} // namespace fo
