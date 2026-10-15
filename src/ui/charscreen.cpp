// Character screen (the original character editor).
//
// Creation mode (edtrcrte): SPECIAL with character points, name / age / sex windows, tag skills, optional
// traits and the OPTIONS window, used before the game starts. In-game mode (edtredt): the character sheet
// with level / experience, the slider that spends unspent skill points and the perks / karma / kills folders.
// Both modes share the derived stat boxes, the skill list and the info card (picture, title, description).
//
// Coordinates follow the original editor. Where the 1997 beta backgrounds differ from the release (big number
// slots, SPECIAL +/- buttons, trait button sockets, bottom red buttons) they are matched to the beta art.
// The beta skilldex.lst is the old GURPS picture set, so info card pictures are picked from it by name.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/item.h"
#include "game/msg.h"
#include "game/perks.h"
#include "game/proto.h"

#include <windows.h>
#include <optional>

namespace fo {

namespace {

// Palette colours (RGB555 colour table entries).
constexpr u32 kLabel = 0xFF948C18u;     // colorTable[18979] plate labels
constexpr u32 kLabelDark = 0xFF736318u; // colorTable[14723] pressed / unselected labels
constexpr u32 kOff = 0xFF084A08u;       // colorTable[1313] condition not present
constexpr u32 kOffSel = 0xFF7B7B29u;    // colorTable[15845] condition not present, selected
constexpr u32 kInk = 0xFF000000u;       // colorTable[0] info card text
constexpr u32 kRedTint = 0xFFFF4848u;   // over the limit big numbers (the beta bignum.frm has no red half)

constexpr int kFont = 101;
constexpr int kTitleFont = 102;
constexpr int kLabelFont = 103;

constexpr int kSpecialPoints = 40; // seven primary stats at 5 plus 5 character points
constexpr int kTagSkills = 3;
constexpr int kSkillMax = 200;     // SKILL_LEVEL_MAX of Fallout 1
constexpr int kNameMax = 11;
constexpr int kLevelMax = 21;
constexpr int kListRow = 11;       // skill list / tag buttons / folder lines

// Info card ids (the original editor's info_line numbering).
constexpr int kInfoStat = 0;        // 0..6 primary stats
constexpr int kInfoLevel = 7;       // 7 level (creation: character points), 8 experience, 9 next level
constexpr int kInfoFolder = 10;     // 10..41 folder lines
constexpr int kInfoFolderTab = 42;  // folder header card
constexpr int kInfoHitPoints = 43;  // 43 hit points, 44..50 conditions
constexpr int kInfoDerived = 51;    // 51..60
constexpr int kInfoSkill = 61;      // 61..78
constexpr int kInfoTagArea = 79;    // tag skills (creation) / skill points (in game)
constexpr int kInfoSkillsTitle = 80;
constexpr int kInfoTraitsTitle = 81;
constexpr int kInfoTrait = 82;      // 82..97

enum Folder : int { FOLDER_PERKS = 0, FOLDER_KARMA, FOLDER_KILLS };
enum Popup : int { POPUP_NONE = 0, POPUP_NAME, POPUP_AGE, POPUP_SEX, POPUP_OPTIONS, POPUP_PERKS };

const int kStatY[7] = { 37, 70, 103, 136, 169, 202, 235 };
const int kTraitY[8] = { 352, 365, 379, 392, 405, 418, 432, 445 }; // trait button sockets of the beta art

// Info card pictures (art/skilldex).
const char* const kStatPic[7] = { "sexappl", "alertnes", "highpain", "charisma", "intuitin", "acrobat", "luck" };
const char* const kSkillPic[SKILL_COUNT] = { "gunpistl", "gunner", "beampist", "brawling", "axemace2", "throwing", "firstaid",
    "physicin", "stealth", "lockpick", "pikpockt", "traps", "compops", "mechanic", "fastalk", "merchant", "gambling", "survival" };
const char* const kTraitPic[TRAIT_COUNT] = { "rapdheal", "overweit", "skinny", "amdidext", "slythand", "berserk", "badtempr",
    "fastdraw", "blodlust", "badluck", "pacificm", "addict", "strgwill", "nightviz", "comnsens", "handsome" };
const char* const kPerkPic[PERK_COUNT] = { "vision", "brawling", "karate", "running", "gunpistl", "spedload", "cmbtrflx",
    "rapdheal", "pvision", "nightviz", "voice", "chemstry", "tufness", "literacy", "gunner", "stealth", "survival", "merchant",
    "eidetic", "physicin", "prospect" };

// Rows below "Hit Points" in the upper derived box. flag 0: poisoned / radiated (checked by level).
struct ConditionRow {
    u32 flag;
    int label, desc; // editor.msg
    const char* pic;
};
const ConditionRow kConditions[7] = {
    { 0, 312, 400, "poisons" },
    { 0, 313, 401, "electrop" },
    { DAM_BLIND, 314, 402, "oneeye" },
    { DAM_CRIP_ARM_RIGHT, 315, 403, "lowpain" },
    { DAM_CRIP_ARM_LEFT, 316, 404, "lowpain" },
    { DAM_CRIP_LEG_RIGHT, 317, 405, "lowpain" },
    { DAM_CRIP_LEG_LEFT, 318, 406, "lowpain" },
};

// Lower derived box.
struct DerivedRow {
    int stat;
    int label; // editor.msg
    bool percent;
    const char* pic;
};
const DerivedRow kDerived[10] = {
    { STAT_AC, 302, false, "dangrsns" },
    { STAT_MAX_AP, 301, false, "running" },
    { STAT_CARRY_WEIGHT, 311, false, "literacy" },
    { STAT_MELEE_DMG, 304, false, "karate" },
    { STAT_DR_NORMAL, 305, true, "cmbtpar" },
    { STAT_POISON_RESIST, 306, true, "poisons" },
    { STAT_RAD_RESIST, 307, true, "chemstry" },
    { STAT_SEQUENCE, 308, false, "cmbtrflx" },
    { STAT_HEAL_RATE, 309, false, "rapdheal" },
    { STAT_CRIT_CHANCE, 310, true, "pvision" },
};

// In-game editor state kept between openings (like the original's globals).
int s_info = kInfoStat;
int s_cursor = 0;
int s_folder = FOLDER_PERKS;

// ------------------------------------------------------------------ helpers
std::string msgText(const char* file, int num, const char* fallback = nullptr)
{
    std::string s = strTrim(gameMsg(file, num));
    if (fallback && (s.empty() || s == "Error")) return fallback;
    return s;
}

std::string edMsg(int num, const char* fallback = nullptr) { return msgText("editor", num, fallback); }
std::string statName(int stat) { return msgText("stat", 100 + stat); }
std::string statDesc(int stat) { return msgText("stat", 200 + stat); }
std::string skillName(int skill) { return msgText("skill", 100 + skill); }
std::string traitName(int trait) { return msgText("trait", 100 + trait); }
std::string perkName(int perk)
{
    const std::string& n = perkRule(perk).name;
    return n.empty() ? msgText("perk", 101 + perk) : n;
}

// Line height of a font as the original text_height() (glyph height + line spacing).
int fontLine(int id)
{
    Font* f = font(id);
    return f ? f->lineHeight() : 10;
}

std::string thousands(int v)
{
    std::string s = std::to_string(v < 0 ? -(long long)v : (long long)v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return v < 0 ? "-" + s : s;
}

// The name plate holds about 100 pixels; long names are cut (at a space when there is one).
std::string fitName(const std::string& name)
{
    if (ui::textWidth(kLabelFont, name) <= 100) return name;
    std::string s = name;
    size_t n = s.size();
    while (n > 1 && ui::textWidth(kLabelFont, s.substr(0, n)) > 100) n--;
    s.resize(n);
    size_t sp = s.rfind(' ');
    if (sp != std::string::npos && sp > 0) s.resize(sp);
    return s;
}

// Left button pressed inside the region this frame (logic pass); used by the info areas.
bool pressIn(float x, float y, float w, float h) { return !ui::drawing() && g_app.input.pressed[0] && ui::mouseIn(x, y, w, h); }

// The mouse is over an opaque pixel of the mask art drawn at (x, y).
bool maskHit(const char* mask, float x, float y)
{
    Sprite* s = ui::art(mask);
    if (!s) return true;
    return s->opaqueAt(0, 0, (int)floorf(ui::mouseX() - x), (int)floorf(ui::mouseY() - y));
}

// Leftmost column of dark ink in an info card picture (at most 150). The description may use the blank
// parchment left of it, like the original DrawCard.
int inkLeft(const std::string& name, Sprite* s)
{
    static std::unordered_map<std::string, int> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    int left = 150;
    const Image& img = s->image;
    const SheetFrame& f = s->frame(0, 0);
    if (!img.pixels.empty()) {
        for (int y = 0; y < f.h; y++) {
            int py = f.y + y;
            if (py < 0 || py >= img.height) continue;
            const u8* row = img.row(py);
            for (int x = 0; x < f.w && x < left; x++) {
                int px = f.x + x;
                if (px < 0 || px >= img.width) break;
                u32 c;
                if (img.indexed) {
                    u8 idx = row[px];
                    if (idx == 0 || (size_t)idx >= img.palette.size()) continue;
                    c = img.palette[idx];
                } else {
                    const u8* p = row + (size_t)px * 4;
                    if (p[3] < 128) continue;
                    c = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
                }
                int hi = std::max((int)((c >> 16) & 255), std::max((int)((c >> 8) & 255), (int)(c & 255)));
                if (hi < 16) { // HighRGB < 2 in the original
                    left = x;
                    break;
                }
            }
        }
    }
    cache[name] = left;
    return left;
}

std::vector<int> ownedPerks()
{
    std::vector<int> v;
    for (int p = 0; p < PERK_COUNT; p++)
        if (perkLevel(p) > 0) v.push_back(p);
    return v;
}

int freeTags()
{
    int n = 0;
    for (int i = 0; i < kTagSkills; i++)
        if (g_game.player.tags[i] < 0) n++;
    return n;
}

void fullHealth(Object* d) { d->critter.hp = critterStat(d, STAT_MAX_HP); }

struct Card {
    std::string pic, title, desc, attrs;
};

Card makeCard(const char* pic, std::string title, std::string desc, std::string attrs = std::string())
{
    return Card{ pic ? pic : "", std::move(title), std::move(desc), std::move(attrs) };
}

Card traitCard(int t)
{
    if (t < 0 || t >= TRAIT_COUNT) return Card();
    return makeCard(kTraitPic[t], traitName(t), msgText("trait", 200 + t));
}

Card perkCard(int p)
{
    if (p < 0 || p >= PERK_COUNT) return Card();
    return makeCard(kPerkPic[p], perkName(p), perkDescription(p));
}

// ------------------------------------------------------------------ screen
class CharacterScreen : public Screen {
public:
    CharacterScreen(bool creation, bool* accepted) : m_creation(creation), m_accepted(accepted)
    {
        if (m_accepted) *m_accepted = false;
    }

    void onEnter() override
    {
        Object* d = g_world.dude;
        m_dude = d ? d->uid : 0;
        if (!d) return;
        // everything CANCEL has to restore
        if (Proto* p = critterProto(d)) {
            m_proto = *p;
            m_haveProto = true;
        }
        m_player = g_game.player;
        m_hp = d->critter.hp;
        m_ap = d->critter.ap;
        for (int s = 0; s < SKILL_COUNT; s++) m_skillStart[s] = critterSkillPoints(d, s);
        if (m_creation) {
            int sum = 0;
            for (int s = STAT_ST; s <= STAT_LK; s++) sum += critterBaseStatDirect(d, s);
            m_charPoints = std::max(0, kSpecialPoints - sum);
            // derived stats keep the premade's values until something changes (the beta premades do not
            // follow the release formulas exactly)
            fullHealth(d);
        } else {
            m_info = clampv(s_info, kInfoStat, kInfoTrait + TRAIT_COUNT - 1);
            m_cursor = clampv(s_cursor, 0, SKILL_COUNT - 1);
            m_folder = clampv(s_folder, (int)FOLDER_PERKS, (int)FOLDER_KILLS);
            // a perk earned at a level up is chosen first, as in the original
            if (g_game.player.freePerks > 0) openPerkChoice();
        }
    }

    void onExit() override
    {
        if (m_creation) return;
        s_info = m_info;
        s_cursor = m_cursor;
        s_folder = m_folder;
    }

    void update(float dt) override
    {
        m_dt = dt;
        if (!dude()) {
            finish(false);
            return;
        }
        {
            ui::Pass pass(false);
            if (m_popup != POPUP_NONE) layoutPopup();
            else layout();
        }
        runPendingBox();
    }

    void drawUI() override
    {
        ui::Pass pass(true);
        layout();
        if (m_popup != POPUP_NONE) layoutPopup();
    }

private:
    Object* dude() const { return m_dude ? g_world.byUid(m_dude) : nullptr; }

    // ------------------------------------------------------------------ main layout (both passes)
    void layout()
    {
        using namespace ui;
        letterbox(kBlack);
        draw(m_creation ? "edtrcrte" : "edtredt", 0, 0);
        Object* d = dude();
        if (!d) return;
        layoutIdentity(d);
        layoutPrimary(d);
        if (m_creation) layoutCharPoints();
        else layoutLevel();
        layoutConditions(d);
        layoutDerived(d);
        layoutSkills(d);
        if (m_creation) layoutTraits();
        else layoutFolder();
        if (drawing()) drawCard();
        layoutCommands();
    }

    // Name / age / sex plates on top: buttons opening their windows in creation mode, plain plates in game.
    void layoutIdentity(Object* d)
    {
        std::string age = strFormat("%s %d", edMsg(104).c_str(), critterStat(d, STAT_AGE));
        std::string sex = edMsg(107 + clampv(critterStat(d, STAT_GENDER), 0, 1));
        identityPlate("nameoff", "nameon", 9, fitName(g_game.player.name), 3, 'N', POPUP_NAME);
        identityPlate("ageoff", "ageon", 154, age, 1, 'A', POPUP_AGE);
        identityPlate("sexoff", "sexon", 235, sex, 0, 'S', POPUP_SEX);
    }

    void identityPlate(const char* off, const char* on, float x, const std::string& label, int nudge, int hotkey, int popup)
    {
        using namespace ui;
        int w = artW(off), h = artH(off);
        bool pressed = false;
        if (m_creation) {
            pressed = hovered(x, 0, (float)w, (float)h) && g_app.input.down[0];
            if (button(off, on, x, 0, hotkey, SND_LARGE)) openPopup(popup);
        } else {
            draw(off, x, 0);
        }
        int tx = w / 2 + nudge - textWidth(kLabelFont, label) / 2;
        text(kLabelFont, label, x + (float)tx, 6, pressed ? kLabelDark : kLabel);
    }

    void layoutPrimary(Object* d)
    {
        using namespace ui;
        for (int s = STAT_ST; s <= STAT_LK; s++) {
            float y = (float)kStatY[s];
            int value;
            std::string level;
            if (m_creation) {
                value = critterBaseStat(d, s) + critterBonusStat(d, s);
                level = edMsg(199 + clampv(critterStat(d, s), 1, 10));
            } else {
                value = critterStat(d, s);
                level = msgText("stat", 300 + clampv(value, 1, 10));
            }
            bigNum(s, 59, y + 1, value, m_creation && value > statMax(s));
            text(kFont, level, 103, y + 8, m_info == kInfoStat + s ? kYellow : kGreen);
            if (m_creation) {
                if (holdButton("splsoff", "splson", 149, y + 2, 1 + s)) raiseStat(s);
                if (holdButton("snegoff", "snegon", 149, y + 13, 11 + s)) lowerStat(s);
            }
        }
        if (pressIn(19, 38, 125, 227)) m_info = kInfoStat + clampv((int)floorf((mouseY() - 32) / 33), 0, 6);
    }

    void layoutCharPoints()
    {
        using namespace ui;
        text(kLabelFont, edMsg(116), 14, 286, kLabel);
        bigNum(10, 127, 283, m_charPoints, false);
        if (pressIn(28, 280, 124, 32)) m_info = kInfoLevel;
    }

    void layoutLevel()
    {
        using namespace ui;
        const PlayerData& pl = g_game.player;
        int lh = fontLine(kFont) + 1;
        int next = pl.level < kLevelMax ? expForLevel(pl.level + 1) : -1;
        std::string lines[3] = {
            strFormat("%s %d", edMsg(113).c_str(), pl.level),
            edMsg(114) + " " + thousands(pl.experience),
            edMsg(115) + " " + (next < 0 || next == INT32_MAX ? std::string("------") : thousands(next)),
        };
        for (int i = 0; i < 3; i++) text(kFont, lines[i], 32, 280.0f + (float)(lh * i), m_info == kInfoLevel + i ? kYellow : kGreen);
        if (pressIn(28, 280, 124, 32)) m_info = kInfoLevel + clampv((int)floorf((mouseY() - 280) / (float)lh), 0, 2);
    }

    static bool conditionActive(Object* d, int row)
    {
        if (row == 0) return d->critter.poison != 0;
        if (row == 1) return d->critter.radiation != 0;
        return (d->critter.results & kConditions[row].flag) != 0;
    }

    void layoutConditions(Object* d)
    {
        using namespace ui;
        int step = fontLine(kFont) + 3;
        int maxHp = critterStat(d, STAT_MAX_HP);
        int hp = m_creation ? maxHp : d->critter.hp;
        float y = 46;
        text(kFont, strFormat("%s %d/%d", edMsg(300).c_str(), hp, maxHp), 194, y, m_info == kInfoHitPoints ? kYellow : kGreen);
        for (int i = 0; i < 7; i++) {
            y += (float)step;
            bool on = conditionActive(d, i);
            bool sel = m_info == kInfoHitPoints + 1 + i;
            text(kFont, edMsg(kConditions[i].label), 194, y, sel ? (on ? kYellow : kOffSel) : (on ? kGreen : kOff));
        }
        if (pressIn(191, 41, 122, 110)) m_info = kInfoHitPoints + clampv((int)floorf((mouseY() - 41) / (float)step), 0, 7);
    }

    void layoutDerived(Object* d)
    {
        using namespace ui;
        int step = fontLine(kFont) + 3;
        float y = 179;
        for (int i = 0; i < 10; i++) {
            const DerivedRow& r = kDerived[i];
            u32 c = m_info == kInfoDerived + i ? kYellow : kGreen;
            int v = critterStat(d, r.stat);
            text(kFont, edMsg(r.label), 194, y, c);
            text(kFont, r.percent ? strFormat("%d%%", v) : std::to_string(v), 288, y, c);
            y += (float)step;
        }
        if (pressIn(191, 175, 122, 135)) m_info = kInfoDerived + clampv((int)floorf((mouseY() - 175) / (float)step), 0, 9);
    }

    void layoutSkills(Object* d)
    {
        using namespace ui;
        text(kLabelFont, edMsg(117), 380, 5, kLabel);
        for (int s = 0; s < SKILL_COUNT; s++) {
            float y = 27.0f + (float)(kListRow * s);
            bool tagged = skillTagged(s);
            bool sel = m_info == kInfoSkill + s;
            u32 c = tagged ? (sel ? kWhite : kGrey) : (sel ? kYellow : kGreen);
            text(kFont, skillName(s), 380, y, c);
            text(kFont, strFormat("%d%%", critterSkill(d, s)), 573, y, c);
            if (m_creation && button("tgskloff", "tgsklon", 347, y - 1, 0, SND_SMALL)) toggleTag(s);
        }
        if (m_creation) {
            text(kLabelFont, edMsg(138), 422, 233, kLabel);
            bigNum(20, 523, 229, freeTags(), false);
        } else {
            text(kLabelFont, edMsg(112), 400, 233, kLabel);
            bigNum(20, 523, 229, g_game.player.unspentSkillPoints, false);
            // slider beside the selected skill with its +/- buttons
            float sy = 16.0f + (float)(kListRow * m_cursor);
            draw("slider", 592, sy);
            bool plus = holdButton("splsoff", "splson", 614, sy + 4, 30);
            bool minus = holdButton("snegoff", "snegon", 614, sy + 15, 31);
            if (plus || key(VK_ADD) || key(VK_OEM_PLUS) || key('N') || key(VK_RIGHT)) raiseSkill();
            if (minus || key(VK_SUBTRACT) || key(VK_OEM_MINUS) || key('J') || key(VK_LEFT)) lowerSkill();
            if (key(VK_UP)) selectSkill(m_cursor - 1);
            if (key(VK_DOWN)) selectSkill(m_cursor + 1);
            if (!drawing() && g_app.input.wheel && hovered(370, 27, 223, 195)) selectSkill(m_cursor - (g_app.input.wheel > 0 ? 1 : -1));
        }
        if (pressIn(376, 5, 223, 20)) m_info = kInfoSkillsTitle;
        if (pressIn(370, 27, 223, 195)) selectSkill((int)floorf((mouseY() - 27) / (float)kListRow));
        if (pressIn(396, 228, 171, 25)) m_info = kInfoTagArea;
    }

    void layoutTraits()
    {
        using namespace ui;
        text(kLabelFont, edMsg(139), 52, 326, kLabel);
        for (int t = 0; t < TRAIT_COUNT; t++) {
            bool right = t >= TRAIT_COUNT / 2;
            float by = (float)kTraitY[t % (TRAIT_COUNT / 2)];
            bool sel = m_info == kInfoTrait + t;
            u32 c = hasTrait(t) ? (sel ? kWhite : kGrey) : (sel ? kYellow : kGreen);
            text(kFont, traitName(t), right ? 199.0f : 47.0f, by + 1, c);
            if (button("tgskloff", "tgsklon", right ? 298.0f : 23.0f, by, 0, SND_SMALL)) toggleTrait(t);
        }
        if (pressIn(52, 324, 169, 20)) m_info = kInfoTraitsTitle;
        if (pressIn(47, 353, 245, 100)) {
            int row = 0;
            for (int i = 1; i < 8; i++)
                if (mouseY() >= (float)kTraitY[i] - 1) row = i;
            m_info = kInfoTrait + row + (mouseX() >= 169 ? TRAIT_COUNT / 2 : 0);
        }
    }

    // PERKS / KARMA / KILLS folders (in game).
    void layoutFolder()
    {
        using namespace ui;
        static const char* const kArt[3] = { "perksfdr", "karmafdr", "killsfdr" };
        static const float kTabX[3] = { 72, 170, 268 };
        draw(kArt[m_folder], 11, 327);
        for (int i = 0; i < 3; i++) {
            std::string label = edMsg(109 + i);
            text(kLabelFont, label, kTabX[i] - (float)(textWidth(kLabelFont, label) / 2), 332, i == m_folder ? kLabel : kLabelDark);
        }
        if (pressIn(11, 327, 318, 33) && maskHit("fldrmask", 11, 327)) {
            float mx = mouseX();
            m_folder = mx >= 208 ? FOLDER_KILLS : (mx > 110 ? FOLDER_KARMA : FOLDER_PERKS);
            m_info = kInfoFolderTab;
            playClick(SND_LARGE, true);
        }
        if (m_folder == FOLDER_PERKS) listPerks();
        else if (m_folder == FOLDER_KARMA) listKarma();
        else listKills();

        if (pressIn(28, 363, 283, 105)) {
            float my = mouseY();
            int line;
            if (m_folder == FOLDER_PERKS) line = my < 439 ? (int)floorf((my - 364) / kListRow) : (my < 450 ? 7 : 8 + (int)floorf((my - 450) / kListRow));
            else line = (int)floorf((my - 362) / kListRow);
            line = clampv(line, 0, 9);
            if (m_folder == FOLDER_KILLS && mouseX() > 174) line += 10;
            m_info = kInfoFolder + line;
        }
    }

    void listPerks()
    {
        using namespace ui;
        std::vector<int> perks = ownedPerks();
        float y = 364;
        for (int i = 0; i < (int)perks.size() && i < 7; i++) {
            int p = perks[i];
            std::string name = perkName(p);
            if (perkLevel(p) > 1) name += strFormat(" (%d)", perkLevel(p));
            text(kFont, name, 34, y, m_info == kInfoFolder + i ? kYellow : kGreen);
            y += kListRow;
        }
        // traits below a "TRAITS" separator line
        float sy = 362.0f + 7 * kListRow;
        std::string title = edMsg(156, "TRAITS");
        int tw = textWidth(kFont, title);
        float tx = (float)((280 - tw) / 2 + 34 - 3);
        float ly = sy + (float)(fontLine(kFont) / 2);
        text(kFont, title, tx, sy, kGreen);
        fill(34, ly, std::max(0.0f, tx - 14 - 34), 1, kGreen);
        fill(tx + (float)tw + 14, ly, std::max(0.0f, 308 - (tx + (float)tw + 14)), 1, kGreen);
        float ty = sy + kListRow;
        for (int i = 0; i < 2; i++) {
            int t = g_game.player.traits[i];
            if (t < 0 || t >= TRAIT_COUNT) continue;
            text(kFont, traitName(t), 34, ty, m_info == kInfoFolder + 8 + i ? kYellow : kGreen);
            ty += kListRow;
        }
    }

    // Kill counts per critter type in two columns (the beta has no kill type names, so they are built in).
    void listKills()
    {
        using namespace ui;
        static const char* const kNames[16] = { "Men", "Women", "Children", "Super Mutants", "Ghouls", "Brahmin", "Radscorpions",
            "Rats", "Floaters", "Centaurs", "Robots", "Dogs", "Mantids", "Deathclaws", "Plants", "Others" };
        int row = 0;
        for (int k = 0; k < 16; k++) {
            int n = g_game.player.kills[k];
            if (n <= 0) continue;
            float x = row < 10 ? 34.0f : 180.0f;
            float y = 364.0f + (float)(row % 10) * kListRow;
            text(kFont, kNames[k], x, y, kGreen);
            text(kFont, std::to_string(n), x + 132, y, kGreen, T_RIGHT);
            row++;
        }
    }

    // The beta has no karma titles (editor.msg 1000+), so the folder lists the karma and reputation values.
    void listKarma()
    {
        using namespace ui;
        const PlayerData& pl = g_game.player;
        std::string lines[2] = {
            strFormat("%s: %d", msgText("stat", 404).c_str(), pl.karma),
            strFormat("%s: %d", msgText("stat", 403).c_str(), pl.reputation),
        };
        for (int i = 0; i < 2; i++) text(kFont, lines[i], 34, 362.0f + (float)(kListRow * i), m_info == kInfoFolder + i ? kYellow : kGreen);
    }

    // DONE / CANCEL / OPTIONS (PRINT in game).
    void layoutCommands()
    {
        using namespace ui;
        text(kLabelFont, edMsg(m_creation ? 101 : 103), 364, 454, kLabel);
        text(kLabelFont, edMsg(100), 476, 454, kLabel);
        text(kLabelFont, edMsg(102), 572, 454, kLabel);
        if (button("lilredup", "lilreddn", 344, 454, m_creation ? 'O' : 'P', SND_SMALL)) {
            if (m_creation) openPopup(POPUP_OPTIONS);
            else showBox({ edMsg(5000), edMsg(5001) }, true); // the demo cannot print
        }
        if (button("lilredup", "lilreddn", 456, 454, 'D', SND_SMALL) || key(VK_RETURN)) tryDone();
        if (button("lilredup", "lilreddn", 553, 454, 'C', SND_SMALL) || key(VK_ESCAPE)) finish(false);
    }

    // ------------------------------------------------------------------ info card
    Card currentCard() const
    {
        if (m_popup == POPUP_PERKS && m_perkSel >= 0 && m_perkSel < (int)m_perkChoices.size()) return perkCard(m_perkChoices[(size_t)m_perkSel]);
        int i = m_info;
        if (i >= kInfoStat && i < kInfoStat + 7) return makeCard(kStatPic[i], statName(i), statDesc(i));
        if (i >= kInfoLevel && i < kInfoFolder) {
            if (m_creation) return makeCard("quirks", edMsg(120), edMsg(121));
            if (i == kInfoLevel) return makeCard("leadrsp", msgText("stat", 401), msgText("stat", 501));
            if (i == kInfoLevel + 1) return makeCard("eidetic", msgText("stat", 402), msgText("stat", 502));
            return makeCard("jump", edMsg(122), edMsg(123));
        }
        if (i >= kInfoFolder && i < kInfoHitPoints) return folderCard(i - kInfoFolder);
        if (i == kInfoHitPoints) return makeCard("tufness", edMsg(300), statDesc(STAT_MAX_HP));
        if (i > kInfoHitPoints && i < kInfoDerived) {
            const ConditionRow& r = kConditions[i - kInfoHitPoints - 1];
            return makeCard(r.pic, edMsg(r.label), edMsg(r.desc));
        }
        if (i >= kInfoDerived && i < kInfoSkill) {
            const DerivedRow& r = kDerived[i - kInfoDerived];
            return makeCard(r.pic, statName(r.stat), statDesc(r.stat));
        }
        if (i >= kInfoSkill && i < kInfoSkill + SKILL_COUNT) {
            int s = i - kInfoSkill;
            std::string attrs = strFormat("%s %d%% %s", edMsg(137).c_str(), skillBaseValue(s), msgText("skill", 300 + s).c_str());
            return makeCard(kSkillPic[s], skillName(s), msgText("skill", 200 + s), attrs);
        }
        // the beta has no "Skills" card text (editor.msg 150/151): the skills title shows the tag / skill point card
        if (i == kInfoTagArea || i == kInfoSkillsTitle) {
            if (m_creation) return makeCard("research", edMsg(144), edMsg(145));
            return makeCard("research", edMsg(130), edMsg(131));
        }
        if (i == kInfoTraitsTitle) return makeCard("quirks", edMsg(146), edMsg(147));
        if (i >= kInfoTrait && i < kInfoTrait + TRAIT_COUNT) return traitCard(i - kInfoTrait);
        return Card();
    }

    Card folderCard(int line) const
    {
        const PlayerData& pl = g_game.player;
        switch (m_folder) {
        case FOLDER_PERKS: {
            std::vector<int> perks = ownedPerks();
            if (line < (int)perks.size() && line < 7) return perkCard(perks[line]);
            if ((line == 8 || line == 9) && pl.traits[line - 8] >= 0) return traitCard(pl.traits[line - 8]);
            if (line >= 7 && line <= 10) return makeCard("quirks", edMsg(146), edMsg(147));
            return makeCard("savoirfr", edMsg(124), edMsg(127));
        }
        case FOLDER_KARMA:
            if (line == 0) return makeCard(pl.karma < 0 ? "badrep" : "goodrep", msgText("stat", 404), msgText("stat", 504));
            if (line == 1) return makeCard(pl.reputation < 0 ? "badrep" : "goodrep", msgText("stat", 403), msgText("stat", 503));
            return makeCard("goodrep", edMsg(125), edMsg(128));
        default:
            return makeCard("dlusion", edMsg(126), edMsg(129));
        }
    }

    // Picture at (484, 309), title in the tall font with a double rule, description wrapped around the picture.
    void drawCard()
    {
        using namespace ui;
        Card c = currentCard();
        if (c.title.empty()) return;
        int ink = 150;
        if (!c.pic.empty()) {
            if (Sprite* s = g_sprites.get("art/skilldex/" + c.pic)) {
                drawSprite(s, 0, 0, 484, 309);
                ink = inkLeft(c.pic, s);
            }
        }
        int titleH = fontLine(kTitleFont);
        text(kTitleFont, c.title, 348, 272, kInk);
        if (!c.attrs.empty()) {
            float ax = 348.0f + (float)textWidth(kTitleFont, c.title) + 8;
            text(kFont, c.attrs, ax, (float)(268 + titleH - fontLine(kFont)), kInk);
        }
        fill(348, (float)(272 + titleH), 266, 2, kInk);
        int lh = fontLine(kFont);
        float y = 315;
        for (auto& line : wrap(kFont, c.desc, 136 + std::max(0, ink - 8))) {
            if (y + (float)lh > 444) break;
            text(kFont, line, 348, y, kInk);
            y += (float)lh;
        }
    }

    // ------------------------------------------------------------------ widgets
    // Two digit counter from bignum.frm (14x24 tiles; tile 11 is the flip frame shown when a digit changes).
    void bigNum(int id, float x, float y, int value, bool red)
    {
        using namespace ui;
        if (!drawing()) return;
        int v = (value >= 0 && value <= 99) ? value : 99;
        NumAnim& a = m_nums[id];
        double now = g_app.time;
        if (a.shown != v) {
            if (a.shown >= 0) {
                a.flip = (a.shown % 10 != v % 10 ? 1 : 0) | (a.shown / 10 != v / 10 ? 2 : 0);
                a.flipUntil = now + 0.123;
            }
            a.shown = v;
        }
        bool flipping = now < a.flipUntil;
        int tens = (flipping && (a.flip & 2)) ? 11 : v / 10;
        int ones = (flipping && (a.flip & 1)) ? 11 : v % 10;
        DrawParams p;
        if (red) p.color = kRedTint;
        drawPart("bignum", 14 * tens, 0, 14, 24, x, y, p);
        drawPart("bignum", 14 * ones, 0, 14, 24, x + 14, y, p);
    }

    // Image button that fires when pressed and keeps firing while held, accelerating like the original
    // editor's +/- buttons (first repeat after about 0.8 s, then 5 up to 24 times per second).
    bool holdButton(const char* up, const char* down, float x, float y, int id, int sound = ui::SND_SMALL)
    {
        using namespace ui;
        float w = (float)artW(up), h = (float)artH(up);
        bool inside = mouseIn(x, y, w, h);
        const Input& in = g_app.input;
        if (drawing()) {
            draw(m_holdId == id && in.down[0] && inside ? down : up, x, y);
            return false;
        }
        if (in.pressed[0] && inside && !mouseTaken()) {
            takeMouse();
            playClick(sound, true);
            m_holdId = id;
            m_holdTime = 0;
            m_holdNext = 0.8f;
            m_holdRate = 4;
            return true;
        }
        if (m_holdId != id) return false;
        takeMouse();
        if (!in.down[0]) {
            m_holdId = -1;
            return false;
        }
        m_holdTime += m_dt;
        if (!inside || m_holdTime < m_holdNext) return false;
        m_holdRate = std::min(24, m_holdRate + 1);
        m_holdNext = m_holdTime + 1.0f / (float)m_holdRate;
        return true;
    }

    // A held button reached a limit: no more repeats until it is pressed again.
    void stopRepeat() { m_holdNext = 1e9f; }

    // ------------------------------------------------------------------ rules
    void statsChanged(Object* d)
    {
        critterRecalcDerived(d);
        if (m_creation) fullHealth(d);
    }

    void raiseStat(int s)
    {
        Object* d = dude();
        if (!d) return;
        m_info = kInfoStat + s;
        int raw = critterBaseStatDirect(d, s);
        int cur = critterBaseStat(d, s) + critterBonusStat(d, s);
        if (m_charPoints <= 0 || cur >= statMax(s) || raw >= statMax(s)) {
            stopRepeat();
            return;
        }
        critterSetBaseStat(d, s, raw + 1);
        m_charPoints--;
        statsChanged(d);
    }

    void lowerStat(int s)
    {
        Object* d = dude();
        if (!d) return;
        m_info = kInfoStat + s;
        int raw = critterBaseStatDirect(d, s);
        if (raw <= statMin(s)) {
            stopRepeat();
            return;
        }
        critterSetBaseStat(d, s, raw - 1);
        m_charPoints++;
        statsChanged(d);
    }

    static bool primaryOverLimit(Object* d)
    {
        for (int s = STAT_ST; s <= STAT_LK; s++)
            if (critterBaseStat(d, s) + critterBonusStat(d, s) > statMax(s)) return true;
        return false;
    }

    void toggleTag(int skill)
    {
        PlayerData& pl = g_game.player;
        m_info = kInfoSkill + skill;
        for (int i = 0; i < 4; i++) {
            if (pl.tags[i] != skill) continue;
            for (int j = i; j < 3; j++) pl.tags[j] = pl.tags[j + 1];
            pl.tags[3] = -1;
            return;
        }
        if (freeTags() <= 0) {
            showBox({ edMsg(140), edMsg(141) }, true);
            return;
        }
        for (int i = 0; i < kTagSkills; i++) {
            if (pl.tags[i] >= 0) continue;
            pl.tags[i] = skill;
            break;
        }
    }

    void toggleTrait(int trait)
    {
        PlayerData& pl = g_game.player;
        m_info = kInfoTrait + trait;
        if (pl.traits[0] == trait) {
            pl.traits[0] = pl.traits[1];
            pl.traits[1] = -1;
        } else if (pl.traits[1] == trait) {
            pl.traits[1] = -1;
        } else if (pl.traits[0] < 0) {
            pl.traits[0] = trait;
        } else if (pl.traits[1] < 0) {
            pl.traits[1] = trait;
        } else {
            showBox({ edMsg(148), edMsg(149) }, true);
            return;
        }
        if (Object* d = dude()) statsChanged(d); // traits modify stats
    }

    void selectSkill(int skill)
    {
        skill = clampv(skill, 0, SKILL_COUNT - 1);
        if (!m_creation) m_cursor = skill;
        m_info = kInfoSkill + skill;
    }

    // In game: one unspent skill point per +1 (tagged skills gain 2% per point through critterSkill).
    void raiseSkill()
    {
        Object* d = dude();
        if (!d) return;
        int s = m_cursor;
        m_info = kInfoSkill + s;
        PlayerData& pl = g_game.player;
        int cost = std::max(1, skillCostToRaise(d, s));
        if (pl.unspentSkillPoints < cost) {
            showBox({ edMsg(136) }, true);
            return;
        }
        if (critterSkill(d, s) >= kSkillMax) {
            showBox({ skillName(s) + ":", edMsg(132), edMsg(133) }, true);
            return;
        }
        critterSetSkillPoints(d, s, critterSkillPoints(d, s) + 1);
        pl.unspentSkillPoints -= cost;
    }

    // Only points spent in this session can be taken back.
    void lowerSkill()
    {
        Object* d = dude();
        if (!d) return;
        int s = m_cursor;
        m_info = kInfoSkill + s;
        int points = critterSkillPoints(d, s);
        if (points <= m_skillStart[s]) {
            showBox({ skillName(s) + ":", edMsg(134), edMsg(135) }, true);
            return;
        }
        critterSetSkillPoints(d, s, points - 1);
        g_game.player.unspentSkillPoints += std::max(1, skillCostToRaise(d, s));
    }

    void tryDone()
    {
        if (done) return;
        Object* d = dude();
        if (m_creation && d) {
            if (m_charPoints != 0) {
                showBox({ edMsg(118), edMsg(119) }, true);
                return;
            }
            if (freeTags() > 0) {
                showBox({ edMsg(142), edMsg(143) }, true);
                return;
            }
            if (primaryOverLimit(d)) {
                showBox({ edMsg(157, "All stats must be between 1 and 10"), edMsg(158, "before starting the game!") }, true);
                return;
            }
        }
        finish(true);
    }

    void finish(bool ok)
    {
        if (done) return;
        Object* d = dude();
        if (!ok) restore();
        else if (d && m_creation) {
            fullHealth(d);
            d->critter.ap = critterStat(d, STAT_MAX_AP);
        }
        if (d && m_creation) critterUpdateArt(d); // the gender picks the vault suit art
        if (!m_creation) g_game.player.levelUpPending = false;
        if (m_accepted) *m_accepted = ok;
        m_popup = POPUP_NONE;
        m_box.reset();
        done = true;
    }

    void restore()
    {
        Object* d = dude();
        if (!d) return;
        if (m_haveProto) {
            if (Proto* p = critterProto(d)) *p = m_proto;
        }
        g_game.player = m_player;
        d->critter.hp = m_hp;
        d->critter.ap = m_ap;
    }

    // OPTIONS > ERASE: back to the default character (like the original ResetPlayer).
    void eraseCharacter()
    {
        Object* d = dude();
        Proto* p = critterProto(d);
        if (!d || !p) return;
        for (int s = STAT_ST; s <= STAT_LK; s++) {
            p->baseStats[s] = 5;
            p->bonusStats[s] = 0;
        }
        p->baseStats[STAT_AGE] = 25;
        p->baseStats[STAT_GENDER] = GENDER_MALE;
        for (int s = 0; s < SKILL_COUNT; s++) p->skills[s] = 0;
        PlayerData& pl = g_game.player;
        pl.name = "None";
        for (int& t : pl.tags) t = -1;
        pl.traits[0] = pl.traits[1] = -1;
        for (int& k : pl.perks) k = 0;
        m_charPoints = kSpecialPoints - 5 * (STAT_LK - STAT_ST + 1);
        m_info = kInfoStat;
        statsChanged(d);
    }

    // ------------------------------------------------------------------ message boxes (run after the logic pass)
    void showBox(std::vector<std::string> lines, bool error)
    {
        if (error) soundPlaySfx("iisxxxx1");
        m_box = PendingBox{ std::move(lines), nullptr };
        m_holdId = -1;
    }

    void askYesNo(std::vector<std::string> lines, std::function<void()> onYes)
    {
        m_box = PendingBox{ std::move(lines), std::move(onYes) };
        m_holdId = -1;
    }

    void runPendingBox()
    {
        if (!m_box || done) return;
        PendingBox b = std::move(*m_box);
        m_box.reset();
        bool yes = false;
        g_app.runModal(makeMessageBox(b.lines, (bool)b.onYes, &yes));
        if (b.onYes && yes) b.onYes();
    }

    // ------------------------------------------------------------------ windows over the editor (creation)
    void openPopup(int popup)
    {
        Object* d = dude();
        if (!d) return;
        m_popup = popup;
        m_holdId = -1;
        if (popup == POPUP_NAME) {
            m_nameEdit = g_game.player.name == "None" ? std::string() : g_game.player.name;
            if ((int)m_nameEdit.size() > kNameMax) m_nameEdit.resize(kNameMax);
        } else if (popup == POPUP_AGE) {
            m_ageSaved = critterBaseStatDirect(d, STAT_AGE);
        }
    }

    void layoutPopup()
    {
        switch (m_popup) {
        case POPUP_NAME: layoutNameWindow(); break;
        case POPUP_AGE: layoutAgeWindow(); break;
        case POPUP_SEX: layoutSexWindow(); break;
        case POPUP_OPTIONS: layoutOptionsWindow(); break;
        case POPUP_PERKS: layoutPerkWindow(); break;
        default: break;
        }
    }

    // ------------------------------------------------------------------ perk choice
    void openPerkChoice()
    {
        m_perkChoices.clear();
        for (int p = 0; p < PERK_COUNT; p++)
            if (perkAvailable(p)) m_perkChoices.push_back(p);
        if (m_perkChoices.empty()) return; // nothing qualifies yet: the perk waits for a later visit
        m_perkSel = 0;
        m_popup = POPUP_PERKS;
    }

    // The perks the player qualifies for; the info card shows the highlighted one.
    void layoutPerkWindow()
    {
        using namespace ui;
        const float X = 22, Y = 36, W = 300, H = 404;
        if (drawing()) {
            fill(X, Y, W, H, 0xF0201810u);
            outline(X, Y, W, H, kLabel);
            outline(X + 3, Y + 3, W - 6, H - 6, kLabelDark);
        }
        text(kTitleFont, edMsg(109, "PERKS"), X + W / 2, Y + 10, kLabel, T_CENTER);
        float y = Y + 14 + (float)fontLine(kTitleFont);
        int waiting = g_game.player.freePerks;
        text(kFont, waiting > 1 ? strFormat("Choose a perk (%d to choose)", waiting) : std::string("Choose a perk"), X + W / 2, y, kGreen, T_CENTER);
        y += (float)fontLine(kFont) + 10;
        int lh = fontLine(kFont) + 3;
        for (int i = 0; i < (int)m_perkChoices.size(); i++) {
            float ry = y + (float)(lh * i);
            if (ry + (float)lh > Y + H - 40) break;
            int p = m_perkChoices[(size_t)i];
            std::string name = perkName(p);
            int ranks = perkRule(p).ranks;
            if (ranks > 1) name += strFormat("  (%d/%d)", perkLevel(p) + 1, ranks); // the rank it becomes
            if (area(X + 8, ry, W - 16, (float)lh)) m_perkSel = i;
            text(kFont, name, X + 16, ry, i == m_perkSel ? kYellow : kGreen);
        }
        if (key(VK_UP)) m_perkSel = std::max(0, m_perkSel - 1);
        if (key(VK_DOWN)) m_perkSel = std::min((int)m_perkChoices.size() - 1, m_perkSel + 1);
        float by = Y + H - 30;
        text(kLabelFont, edMsg(100), X + 70, by, kLabel);
        text(kLabelFont, edMsg(102), X + 190, by, kLabel);
        if (button("lilredup", "lilreddn", X + 50, by, 0, SND_SMALL) || key(VK_RETURN)) {
            takePerk();
            return;
        }
        if (button("lilredup", "lilreddn", X + 170, by, 0, SND_SMALL) || key(VK_ESCAPE)) m_popup = POPUP_NONE;
    }

    void takePerk()
    {
        if (m_perkSel < 0 || m_perkSel >= (int)m_perkChoices.size()) return;
        PlayerData& pl = g_game.player;
        pl.perks[m_perkChoices[(size_t)m_perkSel]]++;
        pl.freePerks = std::max(0, pl.freePerks - 1); // its bonuses apply through critterStat at once
        m_popup = POPUP_NONE;
        m_folder = FOLDER_PERKS;
        if (pl.freePerks > 0) openPerkChoice(); // another one waiting
    }

    void layoutNameWindow()
    {
        using namespace ui;
        const float X = 17, Y = 0;
        draw("charwin", X, Y);
        draw("namebox", X + 13, Y + 13);
        draw("donebox", X + 13, Y + 40);
        text(kLabelFont, edMsg(100), X + 50, Y + 44, kLabel);
        bool ok = button("lilredup", "lilreddn", X + 26, Y + 44, 0, SND_SMALL);
        text(kFont, m_nameEdit, X + 23, Y + 19, kGreen);
        if (drawing()) {
            if (std::fmod(g_app.time, 0.25) < 0.125) { // blinking block cursor
                int cw = std::max(1, textWidth(kFont, "_") - 4);
                float cx = X + 23 + (float)(textWidth(kFont, m_nameEdit + " ") - cw);
                fill(cx, Y + 19, (float)cw, (float)(fontLine(kFont) - 2), kGreen);
            }
            return;
        }
        for (u32 ch : chars())
            if (ch >= 32 && ch < 127 && (int)m_nameEdit.size() < kNameMax) m_nameEdit += (char)ch;
        if (key(VK_BACK) && !m_nameEdit.empty()) m_nameEdit.pop_back();
        if (ok || key(VK_RETURN)) {
            std::string name = strTrim(m_nameEdit);
            if (!name.empty()) g_game.player.name = name;
            m_popup = POPUP_NONE;
        } else if (key(VK_ESCAPE)) {
            m_popup = POPUP_NONE;
        }
    }

    void changeAge(Object* d, int delta)
    {
        int v = critterBaseStatDirect(d, STAT_AGE) + delta;
        if (v < statMin(STAT_AGE) || v > statMax(STAT_AGE)) {
            stopRepeat();
            return;
        }
        critterSetBaseStat(d, STAT_AGE, v);
    }

    void layoutAgeWindow()
    {
        using namespace ui;
        Object* d = dude();
        if (!d) return;
        const float X = 154, Y = 0;
        draw("charwin", X, Y);
        draw("agebox", X + 8, Y + 7);
        draw("donebox", X + 13, Y + 40);
        text(kLabelFont, edMsg(100), X + 50, Y + 44, kLabel);
        bigNum(30, X + 55, Y + 10, critterStat(d, STAT_AGE), false);
        bool ok = button("lilredup", "lilreddn", X + 26, Y + 44, 0, SND_SMALL);
        bool inc = holdButton("sru", "srd", X + 105, Y + 13, 40, SND_MEDIUM);
        bool dec = holdButton("slu", "sld", X + 19, Y + 13, 41, SND_MEDIUM);
        if (drawing()) return;
        int wheel = g_app.input.wheel;
        if (inc || key(VK_ADD) || key(VK_OEM_PLUS) || key('N') || key(VK_RIGHT) || wheel > 0) changeAge(d, 1);
        if (dec || key(VK_SUBTRACT) || key(VK_OEM_MINUS) || key('J') || key(VK_LEFT) || wheel < 0) changeAge(d, -1);
        if (ok || key(VK_RETURN)) {
            m_popup = POPUP_NONE;
        } else if (key(VK_ESCAPE)) {
            critterSetBaseStat(d, STAT_AGE, m_ageSaved);
            m_popup = POPUP_NONE;
        }
    }

    void layoutSexWindow()
    {
        using namespace ui;
        Object* d = dude();
        if (!d) return;
        const float X = 235, Y = 0;
        draw("charwin", X, Y);
        draw("donebox", X + 15, Y + 44);
        text(kLabelFont, edMsg(100), X + 52, Y + 48, kLabel);
        bool ok = button("lilredup", "lilreddn", X + 28, Y + 48, 0, SND_SMALL);
        int gender = critterBaseStatDirect(d, STAT_GENDER);
        if (button(gender == GENDER_MALE ? "maleon" : "maleoff", "maleon", X + 22, Y + 2, 0, SND_SMALL))
            critterSetBaseStat(d, STAT_GENDER, GENDER_MALE);
        if (button(gender == GENDER_FEMALE ? "femon" : "femoff", "femon", X + 71, Y + 3, 0, SND_SMALL))
            critterSetBaseStat(d, STAT_GENDER, GENDER_FEMALE);
        if (!drawing() && (ok || key(VK_RETURN) || key(VK_ESCAPE))) m_popup = POPUP_NONE;
    }

    // SAVE / LOAD / PRINT TO FILE / ERASE / DONE. The demo only explains that saving, loading and printing
    // need the full version (editor.msg 5000-5005).
    void layoutOptionsWindow()
    {
        using namespace ui;
        const float X = 238, Y = 90;
        static const int kKeys[5] = { 'S', 'L', 'P', 'E', 'D' };
        draw("opbase", X, Y);
        float bw = (float)artW("opbtnoff"), bh = (float)artH("opbtnoff");
        int chosen = -1;
        for (int i = 0; i < 5; i++) {
            float by = Y + 17 + (bh + 3) * (float)i;
            bool pressed = hovered(X + 13, by, bw, bh) && g_app.input.down[0];
            if (button("opbtnoff", "opbtnon", X + 13, by, kKeys[i], SND_LARGE)) chosen = i;
            std::string label = edMsg(600 + i);
            text(kLabelFont, label, X + 13 + floorf(bw / 2) - (float)(textWidth(kLabelFont, label) / 2), by + 7, pressed ? kLabelDark : kLabel);
        }
        if (drawing()) return;
        if (key(VK_RETURN) || key(VK_ESCAPE)) chosen = 4;
        switch (chosen) {
        case 0: showBox({ edMsg(5002), edMsg(5003) }, true); break;
        case 1: showBox({ edMsg(5004), edMsg(5005) }, true); break;
        case 2: showBox({ edMsg(5000), edMsg(5001) }, true); break;
        case 3: askYesNo({ edMsg(605), edMsg(606) }, [this] { eraseCharacter(); }); break;
        case 4: m_popup = POPUP_NONE; break;
        default: break;
        }
    }

    struct NumAnim {
        int shown = -1;
        int flip = 0; // bit 0: ones, bit 1: tens
        double flipUntil = 0;
    };
    struct PendingBox {
        std::vector<std::string> lines;
        std::function<void()> onYes; // set for a yes / no question
    };

    bool m_creation;
    bool* m_accepted;
    u32 m_dude = 0;

    // backup for CANCEL
    Proto m_proto;
    bool m_haveProto = false;
    PlayerData m_player;
    int m_hp = 0, m_ap = 0;
    int m_skillStart[SKILL_COUNT] = {};

    int m_charPoints = 0;
    int m_info = kInfoStat;
    int m_cursor = 0; // skill with the slider (in game)
    int m_folder = FOLDER_PERKS;
    int m_popup = POPUP_NONE;
    std::vector<int> m_perkChoices; // perks offered by the perk window
    int m_perkSel = 0;
    std::string m_nameEdit;
    int m_ageSaved = 25;
    std::optional<PendingBox> m_box;
    std::map<int, NumAnim> m_nums;

    float m_dt = 0;
    int m_holdId = -1;
    float m_holdTime = 0, m_holdNext = 0;
    int m_holdRate = 4;
};

} // namespace

std::unique_ptr<Screen> makeCharacterScreen(bool creation, bool* accepted) { return std::make_unique<CharacterScreen>(creation, accepted); }

} // namespace fo
