// Map editor (FalloutDemoHD.exe --editor <map>): places, moves and deletes objects, paints floor and roof
// tiles, sets the entrance and saves the map into a mod folder (mods/<mod>/maps/<map>.json), so the base
// data stays untouched. F5 saves and starts the game on the map in a second window.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "core/fs.h"
#include "core/json.h"
#include "game/art.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/map.h"
#include "game/proto.h"
#include "game/scriptnames.h"
#include "game/worldview.h"

#include <windows.h>

namespace fo {
namespace {

constexpr int kFont = 101;
constexpr float kBarH = 14;          // top and bottom bars
constexpr float kCellW = 84, kCellH = 70;
constexpr size_t kMaxUndo = 100;
constexpr u32 kPanelBg = 0xE0101410u;
constexpr u32 kSelectOutline = 0xFFFFFF00u;

enum Tool : int { TOOL_OBJECTS = 0, TOOL_FLOOR, TOOL_ROOF, TOOL_ENTRANCE, TOOL_COUNT };
const char* const kToolNames[TOOL_COUNT] = { "1 Objects", "2 Floor", "3 Roof", "4 Entrance" };
constexpr int kCategoryCount = 5;
const int kCategoryTypes[kCategoryCount] = { OBJ_ITEM, OBJ_CRITTER, OBJ_SCENERY, OBJ_WALL, OBJ_MISC };
const char* const kCategoryNames[kCategoryCount] = { "Items", "Critters", "Scenery", "Walls", "Misc" };
const char* const kProtoDirs[OBJ_TYPE_COUNT] = { "items", "critters", "scenery", "walls", "tiles", "misc" };
enum Focus : int { FOCUS_NONE = 0, FOCUS_FILTER, FOCUS_SCRIPT };

struct PaletteEntry {
    int id = 0;  // pid (objects) or tiles.lst line (tiles)
    int fid = -1;
    std::string name;
    std::string search; // lower case text the filter matches
};

struct Snapshot {
    std::string objects; // JSON array of the map objects
    std::vector<u16> floor[kElevations], roof[kElevations];
    bool hasElevation[kElevations] = {};
    int enterTile = 0, enterElevation = 0, enterRotation = 0;
};

// Layout px per world canvas px (the world and the interface use different canvases).
float worldToLayout() { return g_app.worldZoom / g_app.uiScale; }
float layoutX(float canvasX) { return canvasX * worldToLayout() - ui::originX(); }
float layoutY(float canvasY) { return canvasY * worldToLayout() - ui::originY(); }

std::string fitText(const std::string& s, float width)
{
    if ((float)ui::textWidth(kFont, s) <= width) return s;
    std::string t = s;
    while (!t.empty() && (float)ui::textWidth(kFont, t + "..") > width) t.pop_back();
    return t + "..";
}

class EditorScreen : public Screen {
public:
    EditorScreen(const std::string& map, const std::string& mod, const std::string& saveAs)
        : m_mapName(strLower(map)), m_mod(mod), m_saveName(strLower(saveAs.empty() ? map : saveAs))
    {
    }

    bool showsWorld() const override { return true; }

    void onEnter() override
    {
        gameEnd();
        soundStopMusic();
        std::string text;
        if (g_vfs.readText("maps/" + m_mapName + ".json", text)) {
            std::string err;
            m_source = Json::parse(text, &err);
            if (!g_map.load(m_mapName, &err)) setStatus("Could not load map " + m_mapName + ": " + err);
            else setStatus(strFormat("Loaded %s (%d objects). F1 shows the keys.", m_mapName.c_str(), countObjects()));
        } else {
            // desert floor mixed from the demo's ground tile variants (edg5000 most often, like Scrapheap)
            std::vector<int> ground;
            for (int i = 0; i < 8; i++) {
                int t = g_art.indexOf(OBJ_TILE, strFormat("edg500%d", i));
                if (t > 1) ground.push_back(t);
            }
            g_map.createBlank(m_mapName, ground.empty() ? 1 : ground[0]);
            u32 seed = 12345;
            for (int sq = 0; sq < kSquares && ground.size() > 1; sq++) {
                seed = seed * 1664525u + 1013904223u;
                size_t r = (seed >> 16) % (ground.size() + 2);
                g_map.floor[0][(size_t)sq] = (u16)ground[r < 3 ? 0 : r - 2];
            }
            m_source = Json::object();
            m_dirty = true;
            setStatus("New map " + m_mapName + ". F1 shows the keys.");
        }
        g_map.name = m_saveName;
        g_app.toolUi = true;
        g_app.zoomUser = 1.0f;
        g_app.onResize(g_renderer.width(), g_renderer.height());
        g_game.lightLevel = 65536;
        g_game.elevation = g_map.enterElevation;
        g_view.centerOn(g_map.enterTile);
        g_view.eggEnabled = false;
        g_view.markSeen = false;
        g_app.onClose = [this]() {
            if (!m_dirty) return false;
            m_closeAsked = true;
            return true;
        };
        m_entranceFid = makeFid(OBJ_CRITTER, std::max(0, g_art.indexOf(OBJ_CRITTER, "hmjmps")));
    }

    void onExit() override
    {
        g_app.onClose = nullptr;
        g_app.toolUi = false;
        g_app.zoomUser = 1.0f;
        g_app.onResize(g_renderer.width(), g_renderer.height());
        g_view.eggEnabled = true;
        g_view.markSeen = true;
        g_view.showRoofs = true;
        g_view.hoverHex = -1;
    }

    void update(float dt) override
    {
        if (m_status.size()) m_statusTime -= dt;
        if (m_closeAsked) {
            m_closeAsked = false;
            if (confirmDiscard()) g_app.requestQuit();
            return;
        }
        g_view.showRoofs = m_tool == TOOL_ROOF;
        updateCamera(dt);
        {
            ui::Pass pass(false);
            layout();
        }
        if (done) return;
        handleKeys();
        if (done) return;
        handleWorld();
    }

    void drawUI() override
    {
        ui::Pass pass(true);
        drawWorldOverlays();
        layout();
    }

private:
    // ------------------------------------------------------------------ state
    std::string m_mapName, m_mod, m_saveName;
    Json m_source;
    bool m_dirty = false, m_closeAsked = false, m_help = false;
    int m_tool = TOOL_OBJECTS;
    int m_category = 2; // scenery
    std::vector<PaletteEntry> m_palettes[OBJ_TYPE_COUNT];
    bool m_paletteBuilt[OBJ_TYPE_COUNT] = {};
    int m_brush[OBJ_TYPE_COUNT] = { -1, -1, -1, -1, -1, -1 }; // palette entry id per type (pid / tile)
    int m_scroll[OBJ_TYPE_COUNT] = {};
    std::string m_filter;
    std::vector<int> m_visible; // filtered palette indices
    std::string m_visibleKey;
    int m_focus = FOCUS_NONE;
    std::string m_edit;
    int m_rotation = 0;
    int m_brushSize = 1;
    u32 m_selected = 0;
    bool m_dragging = false, m_dragMoved = false, m_painting = false;
    int m_hoverHex = -1, m_hoverSquare = -1;
    bool m_overUi = false;
    bool m_midDrag = false;
    float m_midX = 0, m_midY = 0;
    std::vector<Snapshot> m_undo, m_redo;
    std::string m_status;
    float m_statusTime = 0;
    int m_entranceFid = -1;

    int countObjects() const
    {
        int n = 0;
        g_world.forEach([&](Object* o) {
            if (o->tile >= 0 && !o->owner) n++;
        });
        return n;
    }

    void setStatus(const std::string& s)
    {
        m_status = s;
        m_statusTime = 6.0f;
        LOGI("editor: %s", s.c_str());
    }

    bool confirmDiscard()
    {
        if (!m_dirty) return true;
        bool yes = false;
        g_app.runModal(makeMessageBox({ "The map has unsaved changes.", "Quit without saving?" }, true, &yes));
        return yes;
    }

    int paletteType() const
    {
        if (m_tool == TOOL_FLOOR || m_tool == TOOL_ROOF) return OBJ_TILE;
        if (m_tool == TOOL_OBJECTS) return kCategoryTypes[m_category];
        return -1;
    }

    Object* selected() const { return m_selected ? g_world.byUid(m_selected) : nullptr; }

    void select(Object* o)
    {
        if (Object* old = selected()) old->outline = 0;
        m_selected = o ? o->uid : 0;
        if (o) o->outline = kSelectOutline;
    }

    // ------------------------------------------------------------------ palettes
    std::vector<PaletteEntry>& palette(int type)
    {
        if (!m_paletteBuilt[type]) {
            m_paletteBuilt[type] = true;
            auto& out = m_palettes[type];
            if (type == OBJ_TILE) {
                PaletteEntry empty;
                empty.id = 1;
                empty.name = "(empty)";
                empty.search = "empty";
                out.push_back(empty);
                for (int i = 2; i < g_art.count(OBJ_TILE); i++) {
                    PaletteEntry e;
                    e.id = i;
                    e.fid = makeFid(OBJ_TILE, i);
                    e.name = g_art.name(OBJ_TILE, i);
                    e.search = strLower(e.name);
                    if (e.search != "reserved" && artExists(e.fid)) out.push_back(e);
                }
            } else {
                for (auto& file : g_vfs.list(std::string("proto/") + kProtoDirs[type], ".json")) {
                    int num = atoi(pathFile(file).c_str());
                    if (num <= 0) continue;
                    int pid = (type << 24) | num;
                    const Proto* p = g_protos.get(pid);
                    // the demo only ships the art its map uses
                    if (!p || !artExists(p->fid)) continue;
                    PaletteEntry e;
                    e.id = pid;
                    e.fid = p->fid;
                    std::string art = g_art.name(fidType(p->fid), p->fid & 0xFFF);
                    e.name = g_protos.name(pid);
                    if (e.name.empty()) e.name = art;
                    e.search = strLower(e.name + " " + art + " " + jsonHex((u32)pid));
                    out.push_back(e);
                }
                std::sort(out.begin(), out.end(), [](const PaletteEntry& a, const PaletteEntry& b) { return a.id < b.id; });
            }
        }
        return m_palettes[type];
    }

    static bool artExists(int fid)
    {
        std::string path = g_art.pathFor(fid);
        return !path.empty() && g_vfs.exists(path + ".png");
    }

    const std::vector<int>& visibleEntries()
    {
        int type = paletteType();
        std::string key = strFormat("%d|", type) + m_filter;
        if (key != m_visibleKey) {
            m_visibleKey = key;
            m_visible.clear();
            if (type >= 0) {
                std::string f = strLower(m_filter);
                auto& pal = palette(type);
                for (int i = 0; i < (int)pal.size(); i++)
                    if (f.empty() || pal[i].search.find(f) != std::string::npos) m_visible.push_back(i);
            }
        }
        return m_visible;
    }

    const PaletteEntry* brushEntry()
    {
        int type = paletteType();
        if (type < 0 || m_brush[type] < 0) return nullptr;
        for (auto& e : palette(type))
            if (e.id == m_brush[type]) return &e;
        return nullptr;
    }

    // Selects the palette entry `id` of `type` as the brush and scrolls the palette to it.
    void pickBrush(int type, int id)
    {
        if (type == OBJ_TILE) {
            if (m_tool != TOOL_FLOOR && m_tool != TOOL_ROOF) m_tool = TOOL_FLOOR;
        } else {
            m_tool = TOOL_OBJECTS;
            for (int c = 0; c < kCategoryCount; c++)
                if (kCategoryTypes[c] == type) m_category = c;
        }
        m_brush[type] = id;
        m_filter.clear();
        auto& vis = visibleEntries();
        auto& pal = palette(type);
        for (int i = 0; i < (int)vis.size(); i++)
            if (pal[vis[i]].id == id) m_scroll[type] = std::max(0, i / columns() - 1);
    }

    // ------------------------------------------------------------------ undo
    Snapshot snapshot() const
    {
        Snapshot s;
        Json objects = Json::array();
        g_world.forEach([&](Object* o) {
            if (o->tile >= 0 && !o->owner) objects.push(objectToJson(o, true, false));
        });
        s.objects = objects.dump(-1);
        for (int e = 0; e < kElevations; e++) {
            s.floor[e] = g_map.floor[e];
            s.roof[e] = g_map.roof[e];
            s.hasElevation[e] = g_map.hasElevation[e];
        }
        s.enterTile = g_map.enterTile;
        s.enterElevation = g_map.enterElevation;
        s.enterRotation = g_map.enterRotation;
        return s;
    }

    void restore(const Snapshot& s)
    {
        select(nullptr);
        m_dragging = m_painting = false;
        g_world.clear();
        Json objects = Json::parse(s.objects);
        for (auto& oj : objects.items()) {
            Object* o = objectFromJson(g_world, oj);
            if (o) g_world.place(o, oj["tile"].toInt(-1), oj["elevation"].toInt(0));
        }
        for (int e = 0; e < kElevations; e++) {
            g_map.floor[e] = s.floor[e];
            g_map.roof[e] = s.roof[e];
            g_map.hasElevation[e] = s.hasElevation[e];
        }
        g_map.enterTile = s.enterTile;
        g_map.enterElevation = s.enterElevation;
        g_map.enterRotation = s.enterRotation;
        m_dirty = true;
    }

    // Call before every change.
    void pushUndo()
    {
        m_undo.push_back(snapshot());
        if (m_undo.size() > kMaxUndo) m_undo.erase(m_undo.begin());
        m_redo.clear();
        m_dirty = true;
    }

    void undo(bool redo)
    {
        auto& from = redo ? m_redo : m_undo;
        auto& to = redo ? m_undo : m_redo;
        if (from.empty()) {
            setStatus(redo ? "Nothing to redo." : "Nothing to undo.");
            return;
        }
        to.push_back(snapshot());
        restore(from.back());
        from.pop_back();
        setStatus(redo ? "Redone." : "Undone.");
    }

    // ------------------------------------------------------------------ saving and play testing
    std::string modDir() const { return pathJoin(g_app.modsDir, m_mod); }

    bool save()
    {
        std::string dir = modDir();
        std::string file = pathJoin(dir, "maps/" + m_saveName + ".json");
        dirCreate(pathDir(file));
        if (!fileWrite(file, g_map.toJson(m_source).dump())) {
            setStatus("Could not write " + file);
            return false;
        }
        std::string modJson = pathJoin(dir, "mod.json");
        if (!fileExists(modJson)) {
            Json mj = Json::object();
            mj.set("name", m_mod);
            mj.set("description", "Maps made with the map editor.");
            fileWrite(modJson, mj.dump());
        }
        std::string note = ensureModEnabled();
        bool layered = false;
        for (auto& l : g_vfs.layers())
            if (strIEquals(pathNormalize(l), pathNormalize(dir))) layered = true;
        if (!layered) g_vfs.pushLayer(dir);
        m_dirty = false;
        setStatus("Saved mods\\" + m_mod + "\\maps\\" + m_saveName + ".json" + note);
        return true;
    }

    // mods.txt, when present, lists the mods that load: the editor's mod is added to it.
    std::string ensureModEnabled()
    {
        std::string listPath = pathJoin(g_app.modsDir, "mods.txt");
        Bytes data;
        if (!fileRead(listPath, data)) return {};
        std::string text(data.begin(), data.end());
        for (auto& line : strSplit(text, '\n')) {
            std::string t = strTrim(line);
            if (strIEquals(t, m_mod)) return {};
            if (t.size() > 1 && t[0] == '-' && strIEquals(strTrim(t.substr(1)), m_mod)) return " (the mod is disabled in mods.txt)";
        }
        if (!text.empty() && text.back() != '\n') text += "\r\n";
        text += m_mod + "\r\n";
        fileWrite(listPath, text);
        return " (added to mods.txt)";
    }

    void playTest()
    {
        if (m_dirty && !save()) return;
        if (!m_dirty && !fileExists(pathJoin(modDir(), "maps/" + m_saveName + ".json")) && !save()) return;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --map " + utf8ToWide(m_saveName);
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::wstring dir = utf8ToWide(exeDir());
        if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            setStatus("Started the game on " + m_saveName + " in a new window.");
        } else {
            setStatus("Could not start the game.");
        }
    }

    // ------------------------------------------------------------------ input
    float left() const { return -ui::originX(); }
    float top() const { return -ui::originY(); }
    float right() const { return (float)g_app.uiW - ui::originX(); }
    float bottom() const { return (float)g_app.uiH - ui::originY(); }
    float panelW() const { return clampv(floorf((float)g_app.uiW * 0.22f), 180.0f, 360.0f); }
    float panelX() const { return right() - panelW(); }
    int columns() const { return std::max(1, (int)((panelW() - 8) / kCellW)); }

    bool mouseOverUi() const
    {
        float mx = ui::mouseX(), my = ui::mouseY();
        if (m_help) return true;
        return my < top() + kBarH || my >= bottom() - kBarH || (mx >= panelX() && paletteType() >= 0) || (m_tool == TOOL_ENTRANCE && mx >= panelX());
    }

    void updateCamera(float dt)
    {
        Input& in = g_app.input;
        float speed = 700.0f * dt / std::max(0.5f, g_app.zoomUser);
        float dx = 0, dy = 0;
        bool typing = m_focus != FOCUS_NONE;
        if (!typing || !in.ctrl) {
            if (in.keyDown[VK_LEFT]) dx -= speed;
            if (in.keyDown[VK_RIGHT]) dx += speed;
            if (in.keyDown[VK_UP]) dy -= speed;
            if (in.keyDown[VK_DOWN]) dy += speed;
        }
        m_overUi = mouseOverUi();
        if (in.mouseInWindow && in.focused && !m_overUi) {
            int mx = in.mouseX, my = in.mouseY;
            int edge = std::max(2, g_renderer.width() / 300);
            if (mx <= edge) dx -= speed;
            if (my >= g_renderer.height() - 1 - edge) dy += speed;
        }
        // middle mouse drag
        if (in.pressed[2]) {
            m_midDrag = true;
            m_midX = (float)in.mouseX;
            m_midY = (float)in.mouseY;
        }
        if (m_midDrag) {
            if (!in.down[2]) m_midDrag = false;
            else {
                dx -= ((float)in.mouseX - m_midX) / g_app.worldZoom;
                dy -= ((float)in.mouseY - m_midY) / g_app.worldZoom;
                m_midX = (float)in.mouseX;
                m_midY = (float)in.mouseY;
            }
        }
        if (dx || dy) g_view.scroll(dx, dy);
        if (in.wheel) {
            if (m_overUi && paletteType() >= 0 && ui::mouseX() >= panelX()) {
                int type = paletteType();
                m_scroll[type] = std::max(0, m_scroll[type] - in.wheel);
            } else if (!m_overUi) {
                g_app.zoomUser = clampv(g_app.zoomUser * (in.wheel > 0 ? 1.15f : 1.0f / 1.15f), 0.35f, 2.5f);
                g_app.onResize(g_renderer.width(), g_renderer.height());
            }
        }
    }

    void handleKeys()
    {
        Input& in = g_app.input;
        if (m_focus != FOCUS_NONE) {
            handleTyping();
            if (in.ctrl && in.key('S')) save();
            return;
        }
        if (m_help) {
            if (in.key(VK_ESCAPE) || in.key(VK_F1) || in.pressed[0]) m_help = false;
            return;
        }
        if (in.key(VK_F1)) m_help = true;
        if (in.key(VK_F10) || (in.ctrl && in.key('Q'))) {
            if (confirmDiscard()) g_app.requestQuit();
            return;
        }
        if (in.key(VK_F12)) g_app.saveScreenshot("");
        if (in.ctrl && in.key('S')) save();
        if (in.ctrl && in.key('Z')) undo(false);
        if (in.ctrl && in.key('Y')) undo(true);
        if (in.ctrl && in.key('F')) m_focus = FOCUS_FILTER;
        if (in.key(VK_F5)) playTest();
        if (in.ctrl) return;
        for (int t = 0; t < TOOL_COUNT; t++)
            if (in.key('1' + t)) setTool(t);
        if (in.key(VK_TAB) && m_tool == TOOL_OBJECTS) m_category = (m_category + (in.shift ? kCategoryCount - 1 : 1)) % kCategoryCount;
        if (in.key(VK_PRIOR) || in.key(VK_NEXT)) {
            int e = clampv(g_game.elevation + (in.key(VK_PRIOR) ? 1 : -1), 0, kElevations - 1);
            if (e != g_game.elevation) {
                select(nullptr);
                g_game.elevation = e;
                setStatus(strFormat("Elevation %d%s", e, g_map.hasElevation[e] ? "" : " (empty: painting a floor tile creates it)"));
            }
        }
        if (in.key(VK_OEM_4)) m_brushSize = std::max(1, m_brushSize - 2);
        if (in.key(VK_OEM_6)) m_brushSize = std::min(9, m_brushSize + 2);
        if (in.key(VK_ESCAPE)) {
            int type = paletteType();
            if (type >= 0 && m_brush[type] >= 0) m_brush[type] = -1;
            else select(nullptr);
        }
        if (in.key('R')) rotate(in.shift ? 5 : 1);
        Object* sel = selected();
        if (sel && in.key(VK_DELETE)) {
            pushUndo();
            select(nullptr);
            g_world.destroy(sel);
            setStatus("Deleted.");
        }
        if (sel && in.key(VK_F2)) {
            m_focus = FOCUS_SCRIPT;
            m_edit = sel->scriptIndex >= 0 ? g_scriptNames.name(sel->scriptIndex) : "";
        }
    }

    void handleTyping()
    {
        Input& in = g_app.input;
        std::string& text = m_focus == FOCUS_FILTER ? m_filter : m_edit;
        for (u32 c : in.chars)
            if (c >= 32 && c < 127 && text.size() < 40) text += (char)c;
        if (in.key(VK_BACK) && !text.empty()) text.pop_back();
        if (m_focus == FOCUS_FILTER) {
            if (in.key(VK_RETURN) || in.key(VK_ESCAPE)) m_focus = FOCUS_NONE;
            if (paletteType() >= 0) m_scroll[paletteType()] = 0;
            return;
        }
        if (in.key(VK_ESCAPE)) m_focus = FOCUS_NONE;
        if (in.key(VK_RETURN)) {
            m_focus = FOCUS_NONE;
            Object* sel = selected();
            if (!sel) return;
            std::string name = strLower(strTrim(m_edit));
            int idx = name.empty() || name == "-" ? -1 : g_scriptNames.indexOf(name);
            if (!name.empty() && name != "-" && idx < 0) {
                setStatus("Unknown script '" + name + "' (scripts are listed in scripts\\scripts.lst).");
                return;
            }
            pushUndo();
            sel->scriptIndex = idx;
            setStatus(idx >= 0 ? "Script set to " + name + "." : "Script removed.");
        }
    }

    void setTool(int t)
    {
        m_tool = t;
        m_focus = FOCUS_NONE;
        m_dragging = m_painting = false;
        if (t != TOOL_OBJECTS) select(nullptr);
    }

    void rotate(int step)
    {
        if (m_tool == TOOL_ENTRANCE) {
            pushUndo();
            g_map.enterRotation = (g_map.enterRotation + step) % 6;
            return;
        }
        if (m_tool != TOOL_OBJECTS) return;
        if (m_brush[paletteType()] >= 0) {
            m_rotation = (m_rotation + step) % 6;
        } else if (Object* sel = selected()) {
            pushUndo();
            sel->rotation = (sel->rotation + step) % 6;
        }
    }

    void handleWorld()
    {
        Input& in = g_app.input;
        float wx = in.worldX, wy = in.worldY;
        bool over = m_overUi || ui::mouseTaken() || !in.mouseInWindow;
        m_hoverHex = over ? -1 : g_view.hexAt(wx, wy);
        int px = (int)g_view.toWorldX(wx), py = (int)g_view.toWorldY(wy);
        m_hoverSquare = over ? -1 : (m_tool == TOOL_ROOF ? hex::roofFromWorld(px, py) : hex::squareFromWorld(px, py));
        g_view.hoverHex = (m_tool == TOOL_OBJECTS || m_tool == TOOL_ENTRANCE) ? m_hoverHex : -1;
        g_view.hoverHexColor = 0xFF40FF40u;
        int e = g_game.elevation;

        switch (m_tool) {
        case TOOL_OBJECTS: {
            int type = paletteType();
            int brush = m_brush[type];
            if (in.pressed[1] && !over) {
                // eyedropper: the object under the mouse becomes the brush
                if (Object* o = g_view.pick(wx, wy, ~(1u << OBJ_TILE), true)) {
                    int t = pidType(o->pid);
                    if (t != OBJ_TILE && t < OBJ_TYPE_COUNT) {
                        pickBrush(t, o->pid);
                        m_rotation = o->rotation;
                    }
                }
                break;
            }
            if (brush >= 0) {
                if (in.pressed[0] && !over && m_hoverHex >= 0) {
                    pushUndo();
                    Object* o = g_world.create(brush);
                    o->rotation = m_rotation;
                    g_world.place(o, m_hoverHex, e);
                    select(o);
                }
                break;
            }
            if (in.pressed[0] && !over) {
                Object* o = g_view.pick(wx, wy, ~(1u << OBJ_TILE), true);
                select(o);
                m_dragging = o != nullptr;
                m_dragMoved = false;
            }
            if (m_dragging) {
                Object* sel = selected();
                if (!in.down[0] || !sel) m_dragging = false;
                else if (m_hoverHex >= 0 && m_hoverHex != sel->tile) {
                    if (!m_dragMoved) {
                        pushUndo();
                        m_dragMoved = true;
                    }
                    g_world.moveTo(sel, m_hoverHex, e);
                }
            }
            break;
        }
        case TOOL_FLOOR:
        case TOOL_ROOF: {
            auto& layer = m_tool == TOOL_ROOF ? g_map.roof[e] : g_map.floor[e];
            if (in.pressed[1] && !over && m_hoverSquare >= 0) {
                pickBrush(OBJ_TILE, std::max(1, (int)layer[m_hoverSquare]));
                break;
            }
            int brush = m_brush[OBJ_TILE];
            if (in.pressed[0] && !over && m_hoverSquare >= 0) {
                if (brush < 0) {
                    setStatus("Choose a tile in the palette first (or right click a tile to pick it).");
                    break;
                }
                pushUndo();
                m_painting = true;
            }
            if (m_painting) {
                if (!in.down[0] || brush < 0) m_painting = false;
                else if (m_hoverSquare >= 0) paint(layer, m_hoverSquare, brush);
            }
            break;
        }
        case TOOL_ENTRANCE:
            if (in.pressed[0] && !over && m_hoverHex >= 0) {
                pushUndo();
                g_map.enterTile = m_hoverHex;
                g_map.enterElevation = e;
                setStatus(strFormat("Entrance set to hex %d, elevation %d.", m_hoverHex, e));
            }
            break;
        }
    }

    void paint(std::vector<u16>& layer, int square, int tile)
    {
        int cx = square % kSquareW, cy = square / kSquareW;
        int r = m_brushSize / 2;
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++)
                if (x >= 0 && y >= 0 && x < kSquareW && y < kSquareH) layer[(size_t)(y * kSquareW + x)] = (u16)tile;
        g_map.hasElevation[g_game.elevation] = true;
    }

    // ------------------------------------------------------------------ drawing on the world
    void drawGhost(int fid, int tile, int rotation, float alpha)
    {
        Object ghost;
        ghost.fid = fid;
        ghost.tile = tile;
        ghost.rotation = rotation;
        float x, y, w, h;
        Sprite* s = g_art.sprite(fid);
        if (!s || !g_view.objectRect(&ghost, x, y, w, h)) return;
        float k = worldToLayout();
        DrawParams p;
        p.alphaMul = alpha;
        ui::drawSpriteScaled(s, rotation, 0, layoutX(x), layoutY(y), w * k, h * k, p);
    }

    void drawTileGhost(int square, int tile, bool roof)
    {
        Sprite* s = g_art.sprite(makeFid(OBJ_TILE, tile));
        int wx, wy;
        if (roof) hex::roofToWorld(square, wx, wy);
        else hex::squareToWorld(square, wx, wy);
        float k = worldToLayout();
        float x = layoutX(g_view.toCanvasX((float)wx)), y = layoutY(g_view.toCanvasY((float)wy));
        if (s && tile > 1) {
            DrawParams p;
            p.alphaMul = 0.7f;
            const SheetFrame& f = s->frame(0, 0);
            ui::drawSpriteScaled(s, 0, 0, x, y, f.w * k, f.h * k, p);
        }
        // outline of the 80x36 tile diamond
        auto px = [&](float v) { return (x + v * k + ui::originX()) * ui::texScale(); };
        auto py = [&](float v) { return (y + v * k + ui::originY()) * ui::texScale(); };
        const float dx[4] = { 0, 48, 80, 32 }, dy[4] = { 12, 0, 24, 36 };
        const int order[4] = { 0, 1, 2, 3 };
        for (int i = 0; i < 4; i++) {
            int a = order[i], b = order[(i + 1) % 4];
            g_renderer.line(px(dx[a]), py(dy[a]), px(dx[b]), py(dy[b]), 0xFF40FF40u, ui::texScale());
        }
    }

    void drawWorldOverlays()
    {
        int e = g_game.elevation;
        // entrance marker
        if (g_map.enterElevation == e && m_entranceFid >= 0) {
            drawGhost(m_entranceFid, g_map.enterTile, g_map.enterRotation, m_tool == TOOL_ENTRANCE ? 0.9f : 0.5f);
            int cx, cy;
            hex::center(g_map.enterTile, cx, cy);
            ui::text(kFont, "Entrance", layoutX(g_view.toCanvasX((float)cx)), layoutY(g_view.toCanvasY((float)cy)) + 4, ui::kYellow,
                ui::T_CENTER | ui::T_SHADOW);
        }
        // spatial (script trigger) areas
        for (auto& s : g_map.spatials) {
            if (s.elevation != e || !hex::valid(s.tile)) continue;
            int cx, cy;
            hex::center(s.tile, cx, cy);
            float x = layoutX(g_view.toCanvasX((float)cx)), y = layoutY(g_view.toCanvasY((float)cy));
            ui::outline(x - 3, y - 3, 7, 7, 0xFFFF40FFu);
            ui::text(kFont, strFormat("%s r%d", g_scriptNames.name(s.scriptIndex).c_str(), s.radius), x, y + 5, 0xFFFF80FFu,
                ui::T_CENTER | ui::T_SHADOW);
        }
        if (m_overUi) return;
        // brush previews
        if (m_tool == TOOL_OBJECTS && m_hoverHex >= 0) {
            if (const PaletteEntry* b = brushEntry()) drawGhost(b->fid, m_hoverHex, m_rotation, 0.6f);
        }
        if ((m_tool == TOOL_FLOOR || m_tool == TOOL_ROOF) && m_hoverSquare >= 0) {
            int tile = m_brush[OBJ_TILE];
            int cx = m_hoverSquare % kSquareW, cy = m_hoverSquare / kSquareW, r = m_brushSize / 2;
            for (int y = cy - r; y <= cy + r; y++)
                for (int x = cx - r; x <= cx + r; x++)
                    if (x >= 0 && y >= 0 && x < kSquareW && y < kSquareH) drawTileGhost(y * kSquareW + x, tile, m_tool == TOOL_ROOF);
        }
    }

    // ------------------------------------------------------------------ interface (both passes)
    void layout()
    {
        using namespace ui;
        float L = left(), T = top(), R = right(), B = bottom();
        // top bar: tools, elevation, map
        fill(L, T, R - L, kBarH, kPanelBg);
        float x = L + 4;
        for (int t = 0; t < TOOL_COUNT; t++) {
            int w = textWidth(kFont, kToolNames[t]) + 10;
            if (area(x - 2, T, (float)w, kBarH)) setTool(t);
            text(kFont, kToolNames[t], x, T + 2, t == m_tool ? kYellow : kGreen);
            x += (float)w;
        }
        x += 8;
        std::string elev = strFormat("Elevation %d (PgUp/PgDn)", g_game.elevation);
        text(kFont, elev, x, T + 2, kGreen);
        x += (float)textWidth(kFont, elev) + 16;
        if (area(x - 2, T, (float)textWidth(kFont, "Save") + 6, kBarH)) save();
        text(kFont, "Save", x, T + 2, m_dirty ? kYellow : kGreen);
        x += (float)textWidth(kFont, "Save") + 12;
        if (area(x - 2, T, (float)textWidth(kFont, "Play") + 6, kBarH)) playTest();
        text(kFont, "Play", x, T + 2, kGreen);
        x += (float)textWidth(kFont, "Play") + 12;
        if (area(x - 2, T, (float)textWidth(kFont, "Help") + 6, kBarH)) m_help = !m_help;
        text(kFont, "Help", x, T + 2, kGreen);
        std::string title = strFormat("%s%s -> mods\\%s", m_saveName.c_str(), m_dirty ? "*" : "", m_mod.c_str());
        text(kFont, title, R - 6, T + 2, kGrey, T_RIGHT);

        // bottom bar: status, hover and selection
        fill(L, B - kBarH, R - L, kBarH, kPanelBg);
        text(kFont, bottomText(), L + 4, B - kBarH + 2, m_focus == FOCUS_SCRIPT ? kYellow : kGreen);

        layoutPanel();
        if (m_help) layoutHelp();
    }

    std::string bottomText()
    {
        if (m_focus == FOCUS_SCRIPT) return "Script of the selected object (empty = none, Enter = apply, Esc = cancel): " + m_edit + "_";
        std::string s;
        if (m_statusTime > 0 && !m_status.empty()) s = m_status + "   ";
        if (Object* sel = selected()) {
            s += strFormat("%s  %s  hex %d  rot %d  script %s", g_protos.name(sel->pid).c_str(), jsonHex((u32)sel->pid).c_str(), sel->tile, sel->rotation,
                sel->scriptIndex >= 0 ? g_scriptNames.name(sel->scriptIndex).c_str() : "-");
            s += "   (Del delete, R rotate, F2 script)";
        } else if (m_hoverHex >= 0) {
            s += strFormat("hex %d  square %d", m_hoverHex, hex::squareOfTile(m_hoverHex));
        } else if (m_hoverSquare >= 0) {
            s += strFormat("square %d", m_hoverSquare);
        }
        return s;
    }

    void layoutPanel()
    {
        using namespace ui;
        float x0 = panelX(), y = top() + kBarH, B = bottom() - kBarH;
        const float kPanelW = panelW();
        if (m_tool == TOOL_ENTRANCE) {
            fill(x0, y, kPanelW, B - y, kPanelBg);
            textBox(kFont,
                "Click a hex to place the player's entrance on the current elevation. R / Shift+R turns the player.\n\n"
                "A map script can still move the player on arrival with override_map_start; the demo's Scrapheap "
                "script does that.",
                x0 + 6, y + 6, (int)kPanelW - 12, kGreen);
            return;
        }
        int type = paletteType();
        if (type < 0) return;
        fill(x0, y, kPanelW, B - y, kPanelBg);
        y += 3;
        if (type != OBJ_TILE) {
            // category tabs
            float tx = x0 + 4;
            for (int c = 0; c < kCategoryCount; c++) {
                const char* n = kCategoryNames[c];
                float w = (float)textWidth(kFont, n) + 6;
                if (tx + w > x0 + kPanelW) {
                    tx = x0 + 4;
                    y += 12;
                }
                if (area(tx - 2, y, w, 12)) {
                    m_category = c;
                    m_focus = FOCUS_NONE;
                }
                text(kFont, n, tx, y, c == m_category ? kYellow : kGreen);
                tx += w;
            }
            y += 14;
        } else {
            text(kFont, strFormat("Tiles  (brush %dx%d: [ ])", m_brushSize, m_brushSize), x0 + 4, y, kGreen);
            y += 14;
        }
        // filter field
        bool focused = m_focus == FOCUS_FILTER;
        outline(x0 + 4, y, kPanelW - 8, 13, focused ? kYellow : kDarkGreen);
        if (area(x0 + 4, y, kPanelW - 8, 13)) m_focus = FOCUS_FILTER;
        std::string ft = m_filter.empty() && !focused ? "filter (Ctrl+F)" : m_filter + (focused ? "_" : "");
        text(kFont, fitText(ft, kPanelW - 14), x0 + 7, y + 2, m_filter.empty() && !focused ? kDarkGrey : kGreen);
        y += 17;

        // grid
        const auto& vis = visibleEntries();
        auto& pal = palette(type);
        int cols = columns();
        int rowsVisible = std::max(1, (int)((B - y) / kCellH));
        int totalRows = ((int)vis.size() + cols - 1) / cols;
        int& scroll = m_scroll[type];
        scroll = clampv(scroll, 0, std::max(0, totalRows - rowsVisible));
        for (int r = 0; r < rowsVisible; r++) {
            for (int c = 0; c < cols; c++) {
                int i = (scroll + r) * cols + c;
                if (i >= (int)vis.size()) break;
                const PaletteEntry& e = pal[vis[i]];
                float cx = x0 + 4 + c * kCellW, cy = y + r * kCellH;
                bool chosen = m_brush[type] == e.id;
                if (area(cx, cy, kCellW - 2, kCellH - 2)) {
                    m_brush[type] = chosen ? -1 : e.id;
                    m_focus = FOCUS_NONE;
                    if (type != OBJ_TILE) select(nullptr);
                }
                if (!drawing()) continue;
                fill(cx, cy, kCellW - 2, kCellH - 2, chosen ? 0xC0305830u : (hovered(cx, cy, kCellW - 2, kCellH - 2) ? 0xC0283028u : 0xC0181C18u));
                if (chosen) outline(cx, cy, kCellW - 2, kCellH - 2, kYellow);
                drawThumb(e, type, cx + 2, cy + 2, kCellW - 6, kCellH - 18);
                text(kFont, fitText(e.name, kCellW - 6), cx + 2, cy + kCellH - 14, chosen ? kYellow : kGreen);
            }
        }
        if (drawing() && totalRows > rowsVisible) {
            // scroll bar
            float barH = B - y - 2;
            float h = std::max(10.0f, barH * rowsVisible / totalRows);
            float pos = (barH - h) * scroll / std::max(1, totalRows - rowsVisible);
            fill(x0 + kPanelW - 4, y + pos, 2, h, kDarkGreen);
        }
    }

    void drawThumb(const PaletteEntry& e, int type, float x, float y, float w, float h)
    {
        if (e.fid < 0) {
            ui::text(kFont, "-", x + w / 2, y + h / 2 - 5, ui::kDarkGrey, ui::T_CENTER);
            return;
        }
        Sprite* s = g_art.sprite(e.fid);
        if (!s) {
            ui::text(kFont, "?", x + w / 2, y + h / 2 - 5, ui::kDarkGrey, ui::T_CENTER);
            return;
        }
        int dir = (type == OBJ_CRITTER && s->meta.dirCount > 1) ? 2 : 0;
        const SheetFrame& f = s->frame(dir, 0);
        if (f.w <= 0 || f.h <= 0) return;
        float k = std::min(1.5f, std::min(w / f.w, h / f.h));
        ui::drawSpriteScaled(s, dir, 0, floorf(x + (w - f.w * k) / 2), floorf(y + (h - f.h * k) / 2), f.w * k, f.h * k);
    }

    void layoutHelp()
    {
        using namespace ui;
        static const char* const kLines[][2] = {
            { "1 2 3 4", "tool: objects, floor tiles, roof tiles, entrance" },
            { "Left click", "place the palette brush / select and drag an object / paint" },
            { "Right click", "pick the object or tile under the mouse as the brush" },
            { "Esc", "drop the brush, then the selection" },
            { "R, Shift+R", "turn the brush, the selected object or the entrance" },
            { "Delete", "delete the selected object" },
            { "F2", "set the selected object's script (scripts\\scripts.lst)" },
            { "[ ]", "tile brush size" },
            { "PgUp PgDn", "elevation" },
            { "Tab, Ctrl+F", "next object category, palette filter" },
            { "Ctrl+Z, Ctrl+Y", "undo, redo" },
            { "Ctrl+S", "save into mods\\<mod>\\maps\\<map>.json" },
            { "F5", "save and play the map in a new window" },
            { "Arrows, edges", "scroll (or drag with the middle button)" },
            { "Mouse wheel", "zoom (over the palette: scroll)" },
            { "F10, Ctrl+Q", "quit" },
        };
        const int n = (int)(sizeof(kLines) / sizeof(kLines[0]));
        float w = 470, h = 30.0f + 12.0f * n;
        float x = (left() + panelX()) / 2 - w / 2, y = (top() + bottom()) / 2 - h / 2;
        fill(x, y, w, h, 0xF0101410u);
        outline(x, y, w, h, kGreen);
        text(kFont, "MAP EDITOR", x + w / 2, y + 6, kYellow, T_CENTER);
        for (int i = 0; i < n; i++) {
            text(kFont, kLines[i][0], x + 10, y + 22 + 12.0f * i, kYellow);
            text(kFont, kLines[i][1], x + 112, y + 22 + 12.0f * i, kGreen);
        }
    }
};

} // namespace

std::unique_ptr<Screen> makeEditorScreen(const std::string& map, const std::string& mod, const std::string& saveAs)
{
    return std::make_unique<EditorScreen>(map, mod, saveAs);
}

} // namespace fo
