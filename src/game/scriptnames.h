// scripts.lst: maps script indices (as used by protos/maps/opcodes) to script file names.
#pragma once
#include "core/base.h"

namespace fo {

struct ScriptListEntry {
    std::string name;     // file stem, lower case ("lex")
    std::string comment;  // text after ';'
    bool mapInit = false; // "# map_init" run-info flag
    int localVars = 0;    // "# local_vars=N"
};

class ScriptNames {
public:
    void loadFrom(const std::string& text);
    void ensureLoaded();
    int count() { ensureLoaded(); return (int)m_entries.size(); }
    // 0-based index -> entry (nullptr if out of range)
    const ScriptListEntry* get(int index);
    std::string name(int index);
    int indexOf(const std::string& name); // -1 if unknown
    void reset() { m_loaded = false; m_entries.clear(); m_byName.clear(); }

private:
    bool m_loaded = false;
    std::vector<ScriptListEntry> m_entries;
    std::unordered_map<std::string, int> m_byName;
};

extern ScriptNames g_scriptNames;

} // namespace fo
