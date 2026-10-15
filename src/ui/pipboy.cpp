// Pipboy 2000 (pip.frm): date and time, STATUS (quests), CLUES, AUTOMAPS, ARCHIVES and the ALARM CLOCK. Resting
// advances game time through the event queue in small steps (like the original's TimedRest) and heals the player.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/map.h"
#include "game/msg.h"
#include "game/script.h"

#include <windows.h>

namespace fo {

namespace {

// Monitor of the original layout: content view and the 350 px text column.
constexpr float kViewX = 254, kViewY = 46, kViewW = 374, kViewH = 410;
constexpr float kTextW = 350;
constexpr int kFont = 101;

constexpr u32 kText = 0xFF00FF00u;         // colorTable[992]
constexpr u32 kHot = 0xFFFFFF5Au;          // colorTable[32747]
constexpr u32 kFinished = 0xFF429C21u;     // colorTable[8804]
constexpr u32 kWallColor = 0xFF00FF00u;    // colorTable[992]
constexpr u32 kSceneryColor = 0xFF007B00u; // colorTable[480]

constexpr int PID_BLOCK_HEX_INVISO = 0x02000158;
constexpr int kHealTicks = 6 * kTicksPerHour; // stat.msg 214: "When you rest, you heal every six hours."
constexpr int kEnemyRange = 10;               // hostile critters this close prevent resting
constexpr int kRestOptions = 13;              // pipboy.msg 302..314
constexpr int kRestUntilHealed = 12;
constexpr float kRestStep = 0.05f;            // seconds per rest animation step (original: 50 ms)

enum PipFlags : u32 { PF_CENTER = 0x02, PF_UNDERLINE = 0x08, PF_STRIKE = 0x40, PF_NO_INDENT = 0x80 };

// STATUS: pipboy.msg holds the quest threads (1000 + 10 * location = location name, + n = quest n); the global
// variable of each quest is 0 (unknown), 1 (active) or 2+ (done).
struct QuestDef {
    int location;
    int quest;
    const char* gvar;
};
const QuestDef kQuests[] = {
    { 0, 1, "FIND_WATER_CHIP" },
    { 2, 1, "KILL_RADSCORPIONS" }, { 2, 2, "RESCUE_TANDI" },
    { 3, 1, "KILL_KILLIAN" }, { 3, 2, "CAPTURE_GIZMO" }, { 3, 3, "SAVE_SINTHIA" }, { 3, 4, "FSCOUT_1" }, { 3, 5, "CSCOUT_1" },
    { 3, 6, "TRISH_QUEST" }, { 3, 7, "SAUL_QUEST" },
    { 5, 1, "KILL_SUPER_MUTANTS" },
    { 6, 1, "MISSING_CARAVAN" }, { 6, 2, "STEAL_NECKLACE" }, { 6, 3, "KILL_MERCHANT" }, { 6, 4, "KILL_JAIN" },
    { 7, 1, "BECOME_AN_INITIATE" }, { 7, 2, "FIND_LOST_INITIATE" }, { 7, 3, "WARNED_BROTHERHOOD" },
    { 10, 1, "ROMEO_JULIET" }, { 10, 3, "FIND_AGENT" }, { 10, 4, "BECOME_BLADE" }, { 10, 5, "FIX_FARM" },
};
constexpr int kQuestLocations = 12;

// CLUES: pipboy.msg topics (2000 + 30 * n) and clues, with the demo's clue variables (1 known, 2 resolved) as
// set by its Scrapheap scripts.
struct ClueDef {
    int topic;
    int clue;
    const char* gvar;
};
const ClueDef kClues[] = {
    { 2210, 2211, "GANG_1" }, { 2210, 2212, "GANG_2" },
    { 2240, 2241, "GENERATOR_1" }, { 2240, 2242, "GENERATOR_2" }, { 2240, 2243, "GENERATOR_3" },
    { 2270, 2271, "DOG_1" }, { 2270, 2272, "DOG_2" },
};

struct Holiday {
    int month, day, msg;
};
const Holiday kHolidays[] = { { 1, 1, 100 }, { 2, 14, 101 }, { 4, 1, 102 }, { 7, 4, 104 }, { 10, 6, 103 }, { 10, 31, 105 }, { 11, 28, 106 },
    { 12, 25, 107 } };

const char* const kRestFallback[kRestOptions] = { "Rest for ten minutes", "Rest for thirty minutes", "Rest for an hour", "Rest for two hours",
    "Rest for three hours", "Rest for four hours", "Rest for five hours", "Rest for six hours", "Rest until morning (0600)",
    "Rest until noon (1200)", "Rest until evening (1800)", "Rest until midnight (2400)", "Rest until healed" };

std::string msgOr(const char* file, int num, const char* fallback)
{
    std::string s = num >= 0 ? gameMsg(file, num) : std::string();
    return s.empty() || s == "Error" ? std::string(fallback) : s;
}

MessageList& pipMessages() { return messages("text/" + g_language + "/game/pipboy.msg"); }

// Game calendar: the game starts on December 5, 2161.
void gameDate(int& month, int& day, int& year)
{
    static const int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int d = gameDay() + 4, m = 11, y = 2161;
    while (d >= kDays[m]) {
        d -= kDays[m];
        if (++m == 12) {
            m = 0;
            y++;
        }
    }
    month = m + 1;
    day = d + 1;
    year = y;
}

// Minutes until the clock shows hour:00 (a full day when it already does).
int minutesUntil(int hour)
{
    int now = gameHour();
    int h = now / 100, m = now % 100;
    if (h == hour && m == 0) return 24 * 60;
    int mins = (hour - h) * 60 - m;
    while (mins <= 0) mins += 24 * 60;
    return mins;
}

int restMinutes(int option)
{
    switch (option) {
    case 0: return 10;
    case 1: return 30;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: return (option - 1) * 60;
    case 8: return minutesUntil(6);
    case 9: return minutesUntil(12);
    case 10: return minutesUntil(18);
    case 11: return minutesUntil(0);
    default: return 0;
    }
}

std::string mapTitle()
{
    if (!g_map.displayName.empty() && g_map.displayName != g_map.name) return g_map.displayName;
    std::string file = strUpper(g_map.name) + ".MAP";
    MessageList& ml = messages("text/" + g_language + "/game/map.msg");
    for (auto& kv : ml.entries()) {
        if (kv.first >= 100 || !strIEquals(strTrim(kv.second.text), file)) continue;
        if (const MessageEntry* e = ml.find(kv.first + 100)) return strTrim(e->text);
    }
    return strUpper(g_map.name);
}

std::string levelName(int elevation) { return msgOr("lsgame", 129, "Level") + strFormat(" %d", elevation + 1); }

void listClick()
{
    if (!soundPlaySfx("ib1p1xx1")) soundPlaySfx("butin1");
}

class PipboyScreen : public Screen {
public:
    void onEnter() override
    {
        m_startTime = g_game.time;
        for (int i = 0; i < (int)g_game.globalVarNames.size(); i++) m_gvars[g_game.globalVarNames[i]] = i;
        if (Font* f = font(kFont)) {
            m_lineH = std::max(1, f->lineHeight());
            m_glyphH = f->height();
        }
        m_bottomLine = (int)kViewH / m_lineH - 1;
        m_useSeen = false;
        g_world.forEach([&](Object* o) {
            if (o->flags & OF_SEEN) m_useSeen = true;
        });
        soundPlaySfx("pipon");
    }

    void onExit() override
    {
        // time passed while resting: let the map scripts catch up (the original runs them when closing)
        if (g_game.time != m_startTime && g_game.running && g_map.loaded()) g_scripts.mapUpdate();
    }

    void update(float dt) override
    {
        m_time += dt;
        if (!g_world.dude) {
            done = true;
            return;
        }
        if (m_rest.active) {
            g_app.setCursor("wait2", 6, 7, (int)(m_time * 10) % 6);
            updateRest(dt);
            if (m_bail) done = true;
            return;
        }
        m_action = A_NONE;
        {
            ui::Pass pass(false);
            layout();
        }
        perform();
    }

    void drawUI() override
    {
        if (!g_world.dude) return;
        ui::Pass pass(true);
        layout();
    }

private:
    enum Mode { M_LOGO, M_STATUS, M_STATUS_DETAIL, M_CLUES, M_CLUES_DETAIL, M_AUTOMAPS, M_AUTOMAP_VIEW, M_ARCHIVES, M_ALARM };
    // A_OPEN: page button (has its own click sound); A_SELECT: a line on the monitor.
    enum Action { A_NONE, A_CLOSE, A_OPEN, A_SELECT, A_REST };

    struct Entry {
        std::string text;
        int value = 0;
    };
    struct Topic {
        std::string title;
        std::vector<Entry> entries;
    };

    struct RestState {
        bool active = false;
        int option = -1;
        int start = 0;   // game time when the rest began
        int total = 0;   // ticks to rest
        int steps = 1;   // animation steps
        int step = 0;
        float acc = 0;
        bool untilHealed = false;
    };

    // ------------------------------------------------------------------ layout
    void layout()
    {
        using namespace ui;
        letterbox(kBlack);
        if (art("pip")) draw("pip", 0, 0);
        else fill(0, 0, 640, 480, 0xFF181810u);
        if (drawing()) {
            drawClock();
            drawNote();
        }

        if (button("alarmout", "alarmin", 124, 13, 0, SND_MEDIUM)) setAction(A_OPEN, M_ALARM);
        static const int kModes[4] = { M_STATUS, M_CLUES, M_AUTOMAPS, M_ARCHIVES };
        for (int i = 0; i < 5; i++) {
            if (!button("lilredup", "lilreddn", 53, 341 + 27.0f * i, 0, SND_SMALL)) continue;
            if (i == 4) setAction(A_CLOSE);
            else setAction(A_OPEN, kModes[i]);
        }
        if (key(VK_ESCAPE) || key(VK_RETURN) || key('P') || key('Z')) setAction(A_CLOSE);

        switch (m_mode) {
        case M_LOGO: layoutLogo(); break;
        case M_STATUS: layoutTopics(false); break;
        case M_STATUS_DETAIL: layoutTopicDetail(false); break;
        case M_CLUES: layoutTopics(true); break;
        case M_CLUES_DETAIL: layoutTopicDetail(true); break;
        case M_AUTOMAPS: layoutAutomaps(); break;
        case M_AUTOMAP_VIEW: layoutAutomapView(); break;
        case M_ARCHIVES: layoutArchives(); break;
        case M_ALARM: layoutAlarm(); break;
        }
        if (!drawing() && (g_app.input.pressed[0] || g_app.input.pressed[1])) takeMouse();
    }

    void setAction(int action, int arg = 0, int arg2 = 0)
    {
        m_action = action;
        m_arg = arg;
        m_arg2 = arg2;
    }

    void perform()
    {
        switch (m_action) {
        case A_CLOSE: done = true; break;
        case A_SELECT: listClick(); [[fallthrough]];
        case A_OPEN:
            m_mode = m_arg;
            m_detail = m_arg2;
            break;
        case A_REST: startRest(m_arg); break;
        default: break;
        }
        m_action = A_NONE;
    }

    // ------------------------------------------------------------------ monitor text
    float lineY(int line) const { return kViewY + (float)(line * m_lineH); }

    void print(const std::string& s, int line, u32 flags, u32 color)
    {
        using namespace ui;
        if (s.empty() || line < 0 || line > m_bottomLine) return;
        int len = textWidth(kFont, s);
        float left = (flags & PF_NO_INDENT) ? 1.0f : 8.0f;
        if (flags & PF_CENTER) left = floorf((kTextW - (float)len) / 2.0f);
        float x = kViewX + left, y = lineY(line);
        text(kFont, s, x, y, color);
        if (flags & PF_UNDERLINE) fill(x, y + (float)(m_glyphH - 1), (float)len, 1, color);
        if (flags & PF_STRIKE) fill(x, y + 3, (float)len, 1, color);
    }

    void title(const std::string& s) { print(s, 0, PF_CENTER | PF_UNDERLINE, kText); }

    // Clickable monitor line (350 px wide, like the original's hot lines).
    bool lineHovered(int line) const { return ui::hovered(kViewX, lineY(line), kTextW, (float)m_lineH); }
    bool lineClicked(int line) const { return ui::area(kViewX, lineY(line), kTextW, (float)m_lineH, 0, ui::SND_NONE); }

    void backLine(int parentMode)
    {
        bool hot = lineHovered(m_bottomLine);
        print(msgOr("pipboy", 201, "BACK"), m_bottomLine, PF_CENTER, hot ? kHot : kText);
        if (lineClicked(m_bottomLine)) setAction(A_SELECT, parentMode);
    }

    int gvar(const char* name) const
    {
        auto it = m_gvars.find(name);
        return it == m_gvars.end() ? 0 : globalVar(it->second);
    }

    // ------------------------------------------------------------------ pages
    void layoutLogo()
    {
        using namespace ui;
        if (!drawing()) return;
        draw("pipx", 323, 156);
        int month, day, year;
        gameDate(month, day, year);
        for (const Holiday& h : kHolidays) {
            if (h.month != month || h.day != day) continue;
            std::string s = msgOr("pipboy", h.msg, "");
            int lw = artW("pipx") > 0 ? artW("pipx") : 204, lh = artH("pipx") > 0 ? artH("pipx") : 141;
            text(kFont, s, (float)(329 + lw / 2 - textWidth(kFont, s) / 2), (float)(lh + 174), kText);
        }
    }

    std::vector<Topic> statusTopics() const
    {
        std::vector<Topic> out;
        MessageList& ml = pipMessages();
        for (int loc = 0; loc < kQuestLocations; loc++) {
            Topic t;
            for (const QuestDef& q : kQuests) {
                int num = 1000 + 10 * loc + q.quest;
                if (q.location != loc || !ml.has(num)) continue;
                int v = gvar(q.gvar);
                if (v > 0) t.entries.push_back({ strTrim(ml.get(num)), v });
            }
            if (t.entries.empty()) continue;
            t.title = ml.has(1000 + 10 * loc) ? strTrim(ml.get(1000 + 10 * loc)) : std::string("?");
            out.push_back(t);
        }
        return out;
    }

    std::vector<Topic> clueTopics() const
    {
        std::vector<Topic> out;
        std::vector<int> ids;
        MessageList& ml = pipMessages();
        for (const ClueDef& c : kClues) {
            int v = gvar(c.gvar);
            if (v <= 0 || !ml.has(c.clue)) continue;
            size_t i = (size_t)(std::find(ids.begin(), ids.end(), c.topic) - ids.begin());
            if (i == ids.size()) {
                ids.push_back(c.topic);
                out.push_back(Topic());
                out.back().title = ml.has(c.topic) ? strTrim(ml.get(c.topic)) : std::string("?");
            }
            out[i].entries.push_back({ strTrim(ml.get(c.clue)), v });
        }
        return out;
    }

    // STATUS / CLUES: list of locations (topics); a click shows the details.
    void layoutTopics(bool clues)
    {
        std::vector<Topic> topics = clues ? clueTopics() : statusTopics();
        title(clues ? msgOr("pipboy", 204, "CLUES") : msgOr("pipboy", 202, "STATUS"));
        if (topics.empty()) {
            print(clues ? std::string("No current clues.") : msgOr("pipboy", 203, "No current tasks."), 2, 0, kText);
            return;
        }
        for (int i = 0; i < (int)topics.size(); i++) {
            int line = 2 + 2 * i;
            if (line >= m_bottomLine) break;
            print(topics[i].title, line, 0, lineHovered(line) ? kHot : kText);
            if (lineClicked(line)) setAction(A_SELECT, clues ? M_CLUES_DETAIL : M_STATUS_DETAIL, i);
        }
    }

    void layoutTopicDetail(bool clues)
    {
        std::vector<Topic> topics = clues ? clueTopics() : statusTopics();
        int parent = clues ? M_CLUES : M_STATUS;
        if (m_detail < 0 || m_detail >= (int)topics.size()) {
            if (!ui::drawing()) setAction(A_OPEN, parent);
            return;
        }
        const Topic& t = topics[m_detail];
        print(t.title, 1, PF_UNDERLINE, kText);
        int line = 3, number = 1;
        for (const Entry& e : t.entries) {
            bool finished = e.value >= 2;
            std::string s = strFormat("%d. %s", number++, e.text.c_str());
            for (auto& l : ui::wrap(kFont, s, (int)kTextW)) {
                if (line >= m_bottomLine - 1) break;
                print(l, line, finished ? PF_STRIKE : 0, finished ? kFinished : kText);
                line += 2;
            }
        }
        backLine(parent);
    }

    int firstElevation() const
    {
        Object* d = g_world.dude;
        if (d && d->tile >= 0 && d->elevation >= 0 && d->elevation < kElevations && g_map.hasElevation[d->elevation]) return d->elevation;
        for (int e = 0; e < kElevations; e++)
            if (g_map.hasElevation[e]) return e;
        return 0;
    }

    // AUTOMAPS: the maps with automap data (the current one), then its levels with a small map.
    void layoutAutomaps()
    {
        title(msgOr("pipboy", 205, "AUTOMAPS"));
        if (!g_map.loaded()) return;
        print(mapTitle(), 2, 0, lineHovered(2) ? kHot : kText);
        if (lineClicked(2)) setAction(A_SELECT, M_AUTOMAP_VIEW, firstElevation());
    }

    void layoutAutomapView()
    {
        if (!g_map.loaded()) {
            if (!ui::drawing()) setAction(A_OPEN, M_AUTOMAPS);
            return;
        }
        title(msgOr("pipboy", 205, "AUTOMAPS"));
        if (ui::drawing()) drawMiniMap(m_detail);
        print(mapTitle(), 2, PF_CENTER, kText);
        int line = 4;
        for (int e = 0; e < kElevations; e++) {
            if (!g_map.hasElevation[e]) continue;
            print(levelName(e), line, 0, e == m_detail || lineHovered(line) ? kHot : kText);
            if (lineClicked(line)) setAction(A_SELECT, M_AUTOMAP_VIEW, e);
            line += 2;
        }
        backLine(M_AUTOMAPS);
    }

    // The original's pipboy automap: two pixels per hex at (238 + 2 * (200 - col), 105 + 2 * row).
    void drawMiniMap(int elevation)
    {
        m_grid.assign((size_t)kHexes, (u8)0);
        g_world.forEach([&](Object* o) {
            if (o->tile < 0 || o->tile >= kHexes || o->elevation != elevation || o->isHidden()) return;
            if (m_useSeen && !(o->flags & OF_SEEN)) return;
            u8 mark = 0;
            if (o->isWall()) mark = 2;
            else if (o->isScenery() && o->pid != PID_BLOCK_HEX_INVISO) mark = 1;
            u8& cell = m_grid[(size_t)o->tile];
            if (mark > cell) cell = mark;
        });
        const float left = kViewX, right = kViewX + kViewW, top = kViewY, bottom = kViewY + kViewH;
        for (int row = 0; row < kHexH; row++) {
            float y = 105.0f + 2.0f * row;
            if (y < top || y >= bottom) continue;
            const u8* line = &m_grid[(size_t)row * kHexW];
            int col = kHexW - 1;
            while (col >= 0) {
                u8 m = line[col];
                if (!m) {
                    col--;
                    continue;
                }
                int end = col;
                while (end > 0 && line[end - 1] == m) end--;
                float x0 = std::max(left, 638.0f - 2.0f * col), x1 = std::min(right, 640.0f - 2.0f * end);
                if (x1 > x0) ui::fill(x0, y, x1 - x0, 1, m == 2 ? kWallColor : kSceneryColor);
                col = end - 1;
            }
        }
    }

    void layoutArchives()
    {
        title(msgOr("pipboy", 206, "VIDEO ARCHIVES"));
        // the demo's movies (pipboy.msg 500..503) are all marked as not shown in the pipboy
        print("No video archives.", 2, 0, kText);
    }

    void layoutAlarm()
    {
        Object* d = g_world.dude;
        title(msgOr("pipboy", 300, "ALARM CLOCK"));
        if (!d) return;
        print(msgOr("pipboy", 301, "Hit Points") + strFormat(" %d/%d", d->critter.hp, critterStat(d, STAT_MAX_HP)), 2, PF_CENTER, kText);
        for (int i = 0; i < kRestOptions; i++) {
            int line = 5 + 2 * i;
            bool hot = m_rest.active ? m_rest.option == i : lineHovered(line);
            print(msgOr("pipboy", 302 + i, kRestFallback[i]), line, 0, hot ? kHot : kText);
            if (lineClicked(line)) setAction(A_REST, i);
        }
    }

    void drawNumber(int value, int digits, float x, float y)
    {
        value = std::max(0, value);
        for (int i = digits - 1; i >= 0; i--) {
            ui::drawPart("numbers", 9 * (value % 10), 0, 9, 17, x + 9.0f * i, y);
            value /= 10;
        }
    }

    void drawClock()
    {
        int month, day, year;
        gameDate(month, day, year);
        drawNumber(day, 2, 20, 17);
        ui::drawPart("months", 0, 15 * (month - 1), 29, 14, 46, 18);
        drawNumber(year, 4, 83, 17);
        drawNumber(gameHour(), 4, 155, 17);
    }

    // The water chip note with the days left (only while the vault's water is running out).
    void drawNote()
    {
        int days = gvar("VAULT_WATER");
        if (days <= 0 || gvar("FIND_WATER_CHIP") == 2) return;
        ui::draw("pip2", 32, 83);
        int h = ui::artH("notenums") > 0 ? ui::artH("notenums") : 13;
        float x = 92, y = 180;
        while (days > 0) {
            int digit = days % 10;
            ui::drawPart("notenums", 12 * digit, 0, 12, h, x, y);
            if (digit == 1) x += 6;
            days /= 10;
            x -= 12;
            y += 2;
        }
    }

    // ------------------------------------------------------------------ resting
    bool enemiesNearby() const
    {
        Object* d = g_world.dude;
        if (!d || d->tile < 0) return false;
        if (inCombat()) return true;
        for (Object* c : g_world.critters(d->elevation)) {
            if (c == d || !critterIsActive(c)) continue;
            if (critterIsHostileTo(c, d) && hex::distance(c->tile, d->tile) <= kEnemyRange) return true;
        }
        return false;
    }

    void startRest(int option)
    {
        Object* d = g_world.dude;
        if (!d || option < 0 || option >= kRestOptions) return;
        if (enemiesNearby()) {
            soundPlaySfx("iisxxxx1");
            g_app.runModal(makeMessageBox({ msgOr("pipboy", 215, "You cannot rest at this location!"), "There are enemies nearby." }));
            return;
        }
        listClick();
        long long ticks;
        bool untilHealed = option == kRestUntilHealed;
        if (untilHealed) {
            int maxHp = critterStat(d, STAT_MAX_HP);
            if (d->critter.hp >= maxHp) return; // nobody needs healing
            int rate = std::max(1, critterStat(d, STAT_HEAL_RATE));
            long long heals = (maxHp - d->critter.hp + rate - 1) / rate;
            ticks = heals * kHealTicks - m_restTicks;
        } else {
            ticks = (long long)restMinutes(option) * kTicksPerMinute;
        }
        ticks = clampv(ticks, 1LL, 60LL * kTicksPerDay);
        m_rest = RestState();
        m_rest.active = true;
        m_rest.option = option;
        m_rest.start = g_game.time;
        m_rest.total = (int)ticks;
        m_rest.untilHealed = untilHealed;
        // pacing of the original: 0.25 s + 3.5 s per rested day, 20 steps per second (capped at 10 s)
        double seconds = (double)ticks / kTicksPerMinute / 1440.0 * 3.5 + 0.25;
        m_rest.steps = clampv((int)(seconds / kRestStep), 1, 200);
    }

    void updateRest(float dt)
    {
        if (g_app.input.key(VK_ESCAPE)) {
            stopRest();
            return;
        }
        m_rest.acc += dt;
        while (m_rest.active && m_rest.acc >= kRestStep) {
            m_rest.acc -= kRestStep;
            m_rest.step++;
            int target = m_rest.start + (int)((long long)m_rest.total * m_rest.step / m_rest.steps);
            if (!advanceTime(target)) {
                // something happened (combat, a script wants the player): close the pipboy
                m_rest.active = false;
                m_bail = true;
                return;
            }
            Object* d = g_world.dude;
            bool healed = m_rest.untilHealed && d->critter.hp >= critterStat(d, STAT_MAX_HP);
            if (healed || m_rest.step >= m_rest.steps) stopRest();
        }
    }

    void stopRest()
    {
        m_rest.active = false;
        soundPlaySfx("ib2lu1x1");
    }

    bool restInterrupted() const
    {
        Object* d = g_world.dude;
        return !g_game.running || !d || d->isDead() || inCombat() || g_game.pendingDialog != 0 || !g_game.pendingMap.empty() || g_game.demoOver;
    }

    // Advances game time to `target`, running the queued events on the way. False if the rest must stop.
    bool advanceTime(int target)
    {
        std::vector<GameEvent>& events = queueEvents();
        int guard = 0;
        while (!events.empty() && events.front().time <= target && guard++ < 1000) {
            int t = std::max(g_game.time, events.front().time);
            passTime(t - g_game.time);
            g_game.time = t;
            queueProcess();
            if (restInterrupted()) return false;
        }
        if (target > g_game.time) {
            passTime(target - g_game.time);
            g_game.time = target;
        }
        return !restInterrupted();
    }

    // Healing while resting: healing rate hit points every six hours.
    void passTime(int ticks)
    {
        if (ticks <= 0) return;
        m_restTicks += ticks;
        while (m_restTicks >= kHealTicks) {
            m_restTicks -= kHealTicks;
            Object* d = g_world.dude;
            int rate = d ? critterStat(d, STAT_HEAL_RATE) : 0;
            if (rate > 0 && !d->isDead() && d->critter.hp < critterStat(d, STAT_MAX_HP)) critterAdjustHp(d, rate);
        }
    }

    int m_mode = M_LOGO;
    int m_detail = 0; // selected topic (details) or elevation (automap view)
    int m_action = A_NONE, m_arg = 0, m_arg2 = 0;
    int m_lineH = 10, m_glyphH = 9, m_bottomLine = 40;
    std::unordered_map<std::string, int> m_gvars;
    bool m_useSeen = false;
    std::vector<u8> m_grid;
    RestState m_rest;
    long long m_restTicks = 0; // rest time towards the next heal
    bool m_bail = false;
    int m_startTime = 0;
    float m_time = 0;
};

} // namespace

std::unique_ptr<Screen> makePipboyScreen() { return std::make_unique<PipboyScreen>(); }

} // namespace fo
