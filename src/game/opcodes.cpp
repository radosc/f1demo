// Game opcodes (0x80A1..0x8155) and intlib dialog opcodes used by the demo scripts.
#include "game/script.h"
#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/dialog.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/item.h"
#include "game/map.h"
#include "game/msg.h"
#include "game/proto.h"
#include "game/scriptnames.h"
#include "game/worldview.h"
#include "audio/sound.h"

namespace fo {

namespace {

ScriptInstance* S(Vm& vm) { return scriptOf(vm); }
Object* self(Vm& vm)
{
    ScriptInstance* s = S(vm);
    return s ? s->owner() : nullptr;
}
Object* popObj(Vm& vm) { return g_world.byUid(vm.popPtr()); }
void pushObj(Vm& vm, Object* o) { vm.pushPtr(o ? o->uid : 0); }

void warn(Vm& vm, const char* op, const char* what)
{
    LOGW("Script %s: %s: %s", vm.prog ? vm.prog->name.c_str() : "?", op, what);
}

// ROLL_*
enum { ROLL_CRITICAL_FAILURE = 0, ROLL_FAILURE = 1, ROLL_SUCCESS = 2, ROLL_CRITICAL_SUCCESS = 3 };

int rollCheckCritical(int delta, int critMod)
{
    bool firstDay = gameTime() / kTicksPerDay < 1;
    if (delta < 0) {
        if (!firstDay && g_rng.range(1, 100) <= -delta / 10) return ROLL_CRITICAL_FAILURE;
        return ROLL_FAILURE;
    }
    if (!firstDay && g_rng.range(1, 100) <= delta / 10 + critMod) return ROLL_CRITICAL_SUCCESS;
    return ROLL_SUCCESS;
}

int rollCheck(int difficulty, int critMod, int* howMuch)
{
    int delta = difficulty - g_rng.range(1, 100);
    if (howMuch) *howMuch = delta;
    return rollCheckCritical(delta, critMod);
}

std::string dialogMsg(int listIndex, int num)
{
    if (num < 1) return "Error";
    std::string name = g_scriptNames.name(listIndex - 1);
    if (name.empty()) return "Error";
    MessageList& ml = messages("text/" + g_language + "/dialog/" + name + ".msg");
    const MessageEntry* e = ml.find(num);
    if (!e) {
        LOGW("Missing message %d in %s.msg", num, name.c_str());
        return "Error";
    }
    return e->text;
}

bool canSee(Object* a, Object* b)
{
    int diff = abs(a->rotation - hex::direction(a->tile, b->tile));
    return diff == 0 || diff == 1 || diff == 5;
}

bool withinPerception(Object* a, Object* b)
{
    if (!a || !b || a->tile < 0 || b->tile < 0 || a->elevation != b->elevation) return false;
    int dist = hex::distance(a->tile, b->tile);
    int pe = critterStat(a, STAT_PE);
    int maxD;
    bool sneaking = b == g_world.dude && b->critter.sneaking;
    if (canSee(a, b)) {
        maxD = pe * 5;
        if (b->flags & OF_TRANS_GLASS) maxD /= 2;
    } else {
        maxD = inCombat() ? pe * 2 : pe;
    }
    if (sneaking) maxD /= 4;
    return dist <= maxD;
}

// line of sight: the first blocker on the straight line must be the target
bool lineOfSight(Object* from, Object* to)
{
    std::vector<int> line = hex::line(from->tile, to->tile);
    for (size_t i = 1; i < line.size(); i++) {
        int t = line[i];
        if (t == to->tile) return true;
        Object* b = g_world.blockingAt(from, t, from->elevation);
        if (b && b != to) {
            if (b->isCritter()) continue; // critters do not block sight
            if (g_world.sightBlockingAt(t, from->elevation)) return false;
        }
    }
    return true;
}

int correctDeath(Object* critter, int anim, bool forceBack)
{
    if (anim >= ANIM_BIG_HOLE_SF && anim <= ANIM_FALL_FRONT_BLOOD_SF) {
        bool standard = g_game.violenceLevel < 3 || !animArtExists(critter, anim);
        if (standard) {
            if (forceBack) anim = ANIM_FALL_BACK;
            else anim = animArtExists(critter, ANIM_FALL_FRONT) ? ANIM_FALL_FRONT : ANIM_FALL_BACK;
        }
    }
    return anim;
}

// ------------------------------------------------------------------ 0x80A1..
void op_give_exp_points(Vm& vm) { playerAddExperience(vm.popInt()); }

void op_scr_return(Vm& vm) { vm.popInt(); }

void op_play_sfx(Vm& vm) { soundPlaySfx(vm.popString()); }

void op_obj_name(Vm& vm)
{
    Object* o = popObj(vm);
    std::string n = o ? (o->isCritter() ? critterName(o) : g_protos.name(o->pid)) : std::string();
    vm.pushString(n);
}

void op_sfx_build_open_name(Vm& vm)
{
    int action = vm.popInt();
    Object* o = popObj(vm);
    std::string name;
    if (o) name = sfxOpenName(o, action);
    vm.pushString(name);
}

void op_get_pc_stat(Vm& vm)
{
    int stat = vm.popInt();
    int v = 0;
    PlayerData& pl = g_game.player;
    switch (stat) {
    case PCSTAT_UNSPENT_SKILL_POINTS: v = pl.unspentSkillPoints; break;
    case PCSTAT_LEVEL: v = pl.level; break;
    case PCSTAT_EXPERIENCE: v = pl.experience; break;
    case PCSTAT_REPUTATION: v = pl.reputation; break;
    case PCSTAT_KARMA: v = pl.karma; break;
    }
    vm.pushInt(v);
}

void op_tile_contains_pid_obj(Vm& vm)
{
    int pid = vm.popInt();
    int elev = vm.popInt();
    int tile = vm.popInt();
    Object* found = nullptr;
    for (Object* o : g_world.at(tile, elev))
        if (o->pid == pid) {
            found = o;
            break;
        }
    pushObj(vm, found);
}

void op_set_map_start(Vm& vm)
{
    int rot = vm.popInt(), elev = vm.popInt(), y = vm.popInt(), x = vm.popInt();
    int tile = 200 * y + x;
    if (!hex::valid(tile)) return;
    g_map.enterTile = tile;
    g_map.enterElevation = elev;
    g_map.enterRotation = rot;
    gameSetElevation(elev);
    g_view.centerOn(tile);
}

void op_override_map_start(Vm& vm)
{
    int rot = vm.popInt(), elev = vm.popInt(), y = vm.popInt(), x = vm.popInt();
    int tile = 200 * y + x;
    Object* dude = g_world.dude;
    if (!dude || !hex::valid(tile)) return;
    dude->rotation = clampv(rot, 0, 5);
    g_world.place(dude, tile, elev);
    gameSetElevation(elev);
    g_view.centerOn(tile);
    g_map.updateRoofVisibility(tile, elev);
}

void op_has_skill(Vm& vm)
{
    int skill = vm.popInt();
    Object* o = popObj(vm);
    vm.pushInt(o && o->isCritter() ? critterSkill(o, skill) : 0);
}

void op_using_skill(Vm& vm)
{
    int skill = vm.popInt();
    Object* o = popObj(vm);
    vm.pushInt(skill == SKILL_SNEAK && o && o == g_world.dude && o->critter.sneaking ? 1 : 0);
}

void op_roll_vs_skill(Vm& vm)
{
    int mod = vm.popInt();
    int skill = vm.popInt();
    Object* o = popObj(vm);
    int roll = ROLL_CRITICAL_FAILURE;
    if (o && o->isCritter()) {
        ScriptInstance* s = S(vm);
        int level = critterSkill(o, skill);
        roll = rollCheck(level + mod, critterStat(o, STAT_CRIT_CHANCE), s ? &s->howMuch : nullptr);
    }
    vm.pushInt(roll);
}

void op_skill_contest(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    vm.popInt();
    vm.pushInt(0);
}

void op_do_check(Vm& vm)
{
    int mod = vm.popInt();
    int stat = vm.popInt();
    Object* o = popObj(vm);
    int roll = 0;
    if (o && stat >= 0 && stat < STAT_PRIMARY_COUNT) {
        ScriptInstance* s = S(vm);
        int howMuch = 0;
        bool ok = critterStatRoll(o, stat, mod, &howMuch);
        if (s) s->howMuch = howMuch;
        roll = ok ? ROLL_SUCCESS : ROLL_FAILURE;
    }
    vm.pushInt(roll);
}

void op_is_success(Vm& vm)
{
    int r = vm.popInt();
    vm.pushInt(r == ROLL_SUCCESS || r == ROLL_CRITICAL_SUCCESS ? 1 : (r == ROLL_FAILURE || r == ROLL_CRITICAL_FAILURE ? 0 : -1));
}

void op_is_critical(Vm& vm)
{
    int r = vm.popInt();
    vm.pushInt(r == ROLL_CRITICAL_SUCCESS || r == ROLL_CRITICAL_FAILURE ? 1 : (r == ROLL_SUCCESS || r == ROLL_FAILURE ? 0 : -1));
}

void op_how_much(Vm& vm)
{
    vm.popInt();
    ScriptInstance* s = S(vm);
    vm.pushInt(s ? s->howMuch : 0);
}

void op_reaction_roll(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    vm.popInt();
    vm.pushInt(0);
}

void op_reaction_influence(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    vm.popInt();
    vm.pushInt(0);
}

void op_random(Vm& vm)
{
    int hi = vm.popInt();
    int lo = vm.popInt();
    vm.pushInt(g_rng.range(std::min(lo, hi), std::max(lo, hi)));
}

void op_roll_dice(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    vm.pushInt(0);
}

void op_move_to(Vm& vm)
{
    int elev = vm.popInt();
    int tile = vm.popInt();
    Object* o = popObj(vm);
    int result = -1;
    if (o && hex::valid(tile)) {
        g_world.place(o, tile, elev);
        result = tile;
        if (o == g_world.dude) {
            gameSetElevation(elev);
            g_view.centerOn(tile);
            g_map.updateRoofVisibility(tile, elev);
        }
    }
    vm.pushInt(result);
}

void op_create_object_sid(Vm& vm)
{
    int sid = vm.popInt();
    int elev = vm.popInt();
    int tile = vm.popInt();
    int pid = vm.popInt();
    Object* o = nullptr;
    if (g_protos.get(pid)) {
        o = g_world.create(pid);
        if (tile == -1) tile = 0;
        g_world.place(o, tile, elev);
        if (sid != -1) {
            o->scriptIndex = sid - 1;
            g_scripts.attach(o, sid - 1);
        } else if (o->scriptIndex >= 0) {
            g_scripts.attach(o, o->scriptIndex);
        }
        if (o->isCritter()) critterUpdateArt(o);
    } else {
        warn(vm, "create_object_sid", "unknown pid");
    }
    pushObj(vm, o);
}

void op_display_msg(Vm& vm) { displayMessage(vm.popString()); }

void op_script_overrides(Vm& vm)
{
    if (ScriptInstance* s = S(vm)) s->overrides = true;
}

void op_obj_is_carrying_obj_pid(Vm& vm)
{
    int pid = vm.popInt();
    Object* o = popObj(vm);
    vm.pushInt(o ? g_world.inventoryCount(o, pid) : 0);
}

void op_tile_contains_obj_pid(Vm& vm)
{
    int pid = vm.popInt();
    int elev = vm.popInt();
    int tile = vm.popInt();
    int r = 0;
    for (Object* o : g_world.at(tile, elev))
        if (o->pid == pid) r = 1;
    vm.pushInt(r);
}

void op_self_obj(Vm& vm) { pushObj(vm, self(vm)); }

void op_source_obj(Vm& vm)
{
    ScriptInstance* s = S(vm);
    vm.pushPtr(s ? s->source : 0);
}

void op_target_obj(Vm& vm)
{
    ScriptInstance* s = S(vm);
    vm.pushPtr(s ? s->target : 0);
}

void op_dude_obj(Vm& vm) { pushObj(vm, g_world.dude); }

void op_local_var(Vm& vm)
{
    int idx = vm.popInt();
    ScriptInstance* s = S(vm);
    if (s && idx >= 0 && idx < (int)s->localVars.size()) vm.pushInt(s->localVars[idx]);
    else vm.pushInt(-1);
}

void op_set_local_var(Vm& vm)
{
    Value v = vm.pop();
    int idx = vm.popInt();
    ScriptInstance* s = S(vm);
    if (!s || idx < 0) return;
    if (idx >= (int)s->localVars.size()) s->localVars.resize((size_t)idx + 1, 0);
    s->localVars[idx] = v.toInt();
}

void op_map_var(Vm& vm)
{
    int idx = vm.popInt();
    vm.pushInt(idx >= 0 && idx < (int)g_game.mapVars.size() ? g_game.mapVars[idx] : -1);
}

void op_set_map_var(Vm& vm)
{
    Value v = vm.pop();
    int idx = vm.popInt();
    if (idx < 0) return;
    if (idx >= (int)g_game.mapVars.size()) g_game.mapVars.resize((size_t)idx + 1, 0);
    g_game.mapVars[idx] = v.toInt();
}

void op_global_var(Vm& vm) { vm.pushInt(globalVar(vm.popInt())); }

void op_set_global_var(Vm& vm)
{
    int value = vm.popInt();
    int idx = vm.popInt();
    setGlobalVar(idx, value);
}

void op_script_action(Vm& vm)
{
    ScriptInstance* s = S(vm);
    vm.pushInt(s ? s->action : 0);
}

void op_obj_type(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o ? fidType(o->fid) : -1);
}

void op_obj_item_subtype(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o && o->isItem() ? itemType(o) : -1);
}

void op_get_critter_stat(Vm& vm)
{
    int stat = vm.popInt();
    Object* o = popObj(vm);
    vm.pushInt(o ? critterStat(o, stat) : -1);
}

void op_set_critter_stat(Vm& vm)
{
    int value = vm.popInt();
    int stat = vm.popInt();
    Object* o = popObj(vm);
    int result = -1;
    if (o && o == g_world.dude) {
        critterSetBaseStat(o, stat, critterBaseStatDirect(o, stat) + value);
        result = 0;
    }
    vm.pushInt(result);
}

void op_animate_stand_obj(Vm& vm)
{
    Object* o = popObj(vm);
    if (!o) o = self(vm);
    if (!o || inCombat()) return;
    animBegin(ANIMSEQ_REPLACE);
    animAnimate(o, ANIM_STAND, 0);
    animEnd();
}

void op_animate_stand_reverse_obj(Vm& vm)
{
    Object* o = popObj(vm);
    if (!o) o = self(vm);
    if (!o || inCombat()) return;
    animBegin(ANIMSEQ_REPLACE);
    animAnimateReverse(o, ANIM_STAND, 0);
    animEnd();
}

void op_animate_move_obj_to_tile(Vm& vm)
{
    int flags = vm.popInt();
    Value tv = vm.pop();
    Object* o = popObj(vm);
    if (!o) return;
    int tile = tv.type == VT_INT ? tv.i : -1;
    if (tile <= -1 || !critterIsActive(o) || inCombat()) return;
    if (flags & 0x10) {
        animStop(o);
        flags &= ~0x10;
    }
    animBegin(ANIMSEQ_REPLACE);
    animMoveToTile(o, tile, o->elevation, -1, flags != 0, 0);
    animEnd();
}

void op_animate_jump(Vm&) {}
void op_make_daytime(Vm&) {}

void op_tile_distance(Vm& vm)
{
    int t2 = vm.popInt(), t1 = vm.popInt();
    vm.pushInt(t1 != -1 && t2 != -1 ? hex::distance(t1, t2) : 9999);
}

void op_tile_distance_objs(Vm& vm)
{
    Object* b = popObj(vm);
    Object* a = popObj(vm);
    int d = 9999;
    if (a && b && a->elevation == b->elevation && a->tile >= 0 && b->tile >= 0) d = hex::distance(a->tile, b->tile);
    vm.pushInt(d);
}

void op_tile_num(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o ? (o->tile >= 0 ? o->tile : (o->root()->tile)) : -1);
}

void op_tile_num_in_direction(Vm& vm)
{
    int dist = vm.popInt();
    int rot = vm.popInt();
    int origin = vm.popInt();
    int tile = -1;
    if (origin != -1 && rot >= 0 && rot < 6 && dist != 0) tile = hex::inDirection(origin, rot, dist);
    vm.pushInt(tile);
}

void op_pickup_obj(Vm& vm)
{
    Object* item = popObj(vm);
    ScriptInstance* s = S(vm);
    Object* who = s ? g_world.byUid(s->target) : nullptr;
    if (item && who) actionPickup(who, item);
}

void op_drop_obj(Vm& vm)
{
    Object* item = popObj(vm);
    ScriptInstance* s = S(vm);
    Object* who = s ? g_world.byUid(s->target) : nullptr;
    if (item && who) actionDrop(who, item, 1);
}

void op_add_obj_to_inven(Vm& vm)
{
    Object* item = popObj(vm);
    Object* owner = popObj(vm);
    if (!owner || !item) return;
    if (item->owner) {
        warn(vm, "add_obj_to_inven", "item already owned");
        return;
    }
    g_world.addToInventory(owner, item, 1);
}

void op_rm_obj_from_inven(Vm& vm)
{
    Object* item = popObj(vm);
    Object* owner = popObj(vm);
    if (!owner || !item) return;
    bool equipped = (item->flags & OF_EQUIPPED) != 0;
    Object* removed = g_world.removeFromInventory(owner, item, 1);
    if (removed) {
        removed->tile = -1;
        if (equipped) critterUpdateArt(owner);
    }
}

void op_wield_obj_critter(Vm& vm)
{
    Object* item = popObj(vm);
    Object* critter = popObj(vm);
    if (!critter || !item || !critter->isCritter()) return;
    if (item->owner != critter) g_world.addToInventory(critter, item, 1);
    int slot = itemType(item) == ITEM_ARMOR ? 2 : (critter == g_world.dude ? g_game.activeHand : 1);
    itemEquip(critter, item, slot);
}

void op_use_obj(Vm& vm)
{
    Object* o = popObj(vm);
    if (!o) return;
    ScriptInstance* s = S(vm);
    Object* user = s ? g_world.byUid(s->target) : nullptr;
    Object* me = self(vm);
    if (me && me->isCritter()) actionUse(user ? user : me, o);
    else actionUse(me, o);
}

void op_obj_can_see_obj(Vm& vm)
{
    Object* b = popObj(vm);
    Object* a = popObj(vm);
    int r = 0;
    if (a && b && b->tile >= 0 && a->tile >= 0 && withinPerception(a, b) && lineOfSight(a, b)) r = 1;
    vm.pushInt(r);
}

void op_attack(Vm& vm)
{
    int data[7];
    for (int i = 0; i < 7; i++) data[i] = vm.popInt();
    Object* target = popObj(vm);
    Object* me = self(vm);
    if (!target || !me) return;
    if (!critterIsActive(me) || me->isHidden()) return;
    if (!critterIsActive(target) || target->isHidden()) return;
    if (target->critter.maneuver & 0x04) return; // fleeing
    if (g_game.uiDisabled) return;
    // data: [0]=target results [1]=attacker results [2]=max dmg [3]=min dmg [4]=bonus [5]=num attacks [6]=called shot
    // Combat itself starts once the current script (and any dialogue) has finished.
    combatScriptAttack(me, target, data[6], data[5], data[4], data[3], data[2], data[1], data[0]);
}

void op_start_gdialog(Vm& vm)
{
    int bg = vm.popInt();
    int head = vm.popInt();
    int mood = vm.popInt();
    Object* o = popObj(vm);
    vm.popInt(); // message list (unused)
    if (inCombat() || !o) return;
    g_dialog.begin(o, S(vm), head, mood, bg);
}

void op_end_dialogue(Vm&) { g_dialog.end(); }

void op_dialogue_reaction(Vm& vm) { g_dialog.setMood(vm.popInt()); }

void objsAreaOnOff(int e1, int e2, int t1, int t2, bool on)
{
    if (e1 > e2) std::swap(e1, e2);
    if (t1 > t2) std::swap(t1, t2);
    for (int e = e1; e <= e2; e++) {
        g_world.forEach([&](Object* o) {
            if (o->elevation != e || o->tile < t1 || o->tile > t2) return;
            int c1 = t1 % kHexW, c2 = t2 % kHexW, c = o->tile % kHexW;
            if (c < std::min(c1, c2) || c > std::max(c1, c2)) return;
            if (on) o->flags &= ~OF_HIDDEN;
            else o->flags |= OF_HIDDEN;
        });
    }
}

void op_turn_off_objs_in_area(Vm& vm)
{
    int d0 = vm.popInt(), d1 = vm.popInt(), d2 = vm.popInt(), d3 = vm.popInt();
    objsAreaOnOff(d3, d2, d1, d0, false);
}

void op_turn_on_objs_in_area(Vm& vm)
{
    int d0 = vm.popInt(), d1 = vm.popInt(), d2 = vm.popInt(), d3 = vm.popInt();
    objsAreaOnOff(d3, d2, d1, d0, true);
}

void op_set_obj_visibility(Vm& vm)
{
    int invisible = vm.popInt();
    Object* o = popObj(vm);
    if (!o) return;
    if (invisible) {
        o->flags |= OF_HIDDEN;
        if (o->isCritter()) o->flags |= OF_NO_BLOCK;
    } else {
        if (o->isCritter()) o->flags &= ~OF_NO_BLOCK;
        o->flags &= ~OF_HIDDEN;
    }
}

void op_load_map(Vm& vm)
{
    int param = vm.popInt();
    Value v = vm.pop();
    std::string name;
    if (v.type == VT_STRING) name = pathStem(strLower(v.toString()));
    else if (v.i >= 0) name = strFormat("map%03d", v.i);
    if (!name.empty()) gameRequestMap(name, param);
}

void op_barter_offer(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    vm.popInt();
}

void op_barter_asking(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    vm.popInt();
}

void op_anim_busy(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o && animBusy(o) ? 1 : 0);
}

void op_critter_heal(Vm& vm)
{
    int amount = vm.popInt();
    Object* o = popObj(vm);
    if (o) critterAdjustHp(o, amount);
    vm.pushInt(0);
}

void op_set_light_level(Vm& vm)
{
    static const int kLevels[3] = { 0x4000, 0xA000, 0x10000 };
    int v = vm.popInt();
    int intensity;
    if (v == 50) intensity = kLevels[1];
    else if (v > 50) intensity = kLevels[1] + v * (kLevels[2] - kLevels[1]) / 100;
    else intensity = kLevels[0] + v * (kLevels[1] - kLevels[0]) / 100;
    g_game.lightLevel = clampv(intensity, 0x4000, 0x10000);
}

void op_game_time(Vm& vm) { vm.pushInt(gameTime()); }
void op_game_time_in_seconds(Vm& vm) { vm.pushInt(gameTime() / 10); }

void op_elevation(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o ? o->elevation : 0);
}

void op_kill_critter(Vm& vm)
{
    int deathAnim = vm.popInt();
    Object* o = popObj(vm);
    if (!o) return;
    animStop(o);
    critterKill(o, deathAnim, true);
}

void op_kill_critter_type(Vm& vm)
{
    static const int kList[11] = { ANIM_FALL_BACK_BLOOD_SF, ANIM_BIG_HOLE_SF, ANIM_CHARRED_BODY_SF, ANIM_CHUNKS_OF_FLESH_SF,
        ANIM_FALL_FRONT_BLOOD_SF, ANIM_FALL_BACK_BLOOD_SF, ANIM_DANCING_AUTOFIRE_SF, ANIM_SLICED_IN_HALF_SF, ANIM_EXPLODED_TO_NOTHING_SF,
        ANIM_FALL_BACK_BLOOD_SF, ANIM_FALL_FRONT_BLOOD_SF };
    int deathFrame = vm.popInt();
    int pid = vm.popInt();
    int v3 = 0;
    std::vector<Object*> victims;
    g_world.forEach([&](Object* o) {
        if (o->pid == pid && !(o->flags & OF_HIDDEN) && o->isCritter() && !o->isDead()) victims.push_back(o);
    });
    for (Object* o : victims) {
        animStop(o);
        if (deathFrame != 0) {
            if (deathFrame == 1) {
                critterKill(o, correctDeath(o, kList[v3], true), true);
                v3 = (v3 + 1) % 11;
            } else {
                critterKill(o, ANIM_FALL_BACK_SF, true);
            }
        } else {
            g_world.destroy(o);
        }
    }
}

void op_critter_damage(Vm& vm)
{
    int typeFlags = vm.popInt();
    int amount = vm.popInt();
    Object* o = popObj(vm);
    if (!o || !o->isCritter()) return;
    int dt = typeFlags & ~(0x100 | 0x200);
    u32 flags = (typeFlags & 0x100) ? DAM_BYPASS : 0;
    critterApplyDamage(o, amount, nullptr, flags, dt);
}

void op_add_timer_event(Vm& vm)
{
    int param = vm.popInt();
    int delay = vm.popInt();
    Object* o = popObj(vm);
    if (!o) return;
    queueAdd(delay, o, EV_SCRIPT, param);
}

void op_rm_timer_event(Vm& vm)
{
    Object* o = popObj(vm);
    if (o) queueRemove(o, -1);
}

void op_game_ticks(Vm& vm) { vm.pushInt(std::max(0, vm.popInt()) * 10); }

void op_has_trait(Vm& vm)
{
    int param = vm.popInt();
    Object* o = popObj(vm);
    int type = vm.popInt();
    int r = 0;
    if (o) {
        switch (type) {
        case 0: r = param >= 0 && param < PERK_COUNT ? perkLevel(param) : 0; break;
        case 1:
            switch (param) {
            case 5: r = o->isCritter() ? o->critter.aiPacket : 0; break;
            case 6: r = o->isCritter() ? o->critter.team : 0; break;
            case 10: r = o->rotation; break;
            case 666: r = (o->flags & OF_HIDDEN) == 0; break;
            case 669: r = inventoryWeight(o); break;
            }
            break;
        case 2: r = hasTrait(param) ? 1 : 0; break;
        }
    }
    vm.pushInt(r);
}

void op_destroy_object(Vm& vm)
{
    Object* o = popObj(vm);
    if (!o) return;
    Object* me = self(vm);
    if (o == me) {
        // deleting itself: hide now, destroy after the script returns
        o->flags |= OF_HIDDEN | OF_NO_SAVE;
        if (o->owner) {
            if (InvItem* e = g_world.invEntry(o->owner, o)) g_world.removeFromInventory(o->owner, o, e->count);
        }
        g_world.unplace(o);
        g_world.destroy(o);
        vm.flags |= Vm::F_STOPPED;
        return;
    }
    animStop(o);
    g_world.destroy(o);
}

void op_obj_can_hear_obj(Vm& vm)
{
    Object* b = popObj(vm);
    Object* a = popObj(vm);
    vm.pushInt(a && b && withinPerception(a, b) ? 1 : 0);
}

void op_game_time_hour(Vm& vm) { vm.pushInt(gameHour()); }

void op_fixed_param(Vm& vm)
{
    ScriptInstance* s = S(vm);
    vm.pushInt(s ? s->fixedParam : 0);
}

void op_tile_is_visible(Vm& vm)
{
    int tile = vm.popInt();
    int x, y;
    hex::center(tile, x, y);
    float cx = g_view.toCanvasX((float)x), cy = g_view.toCanvasY((float)y);
    vm.pushInt(cx >= -32 && cy >= -32 && cx < g_view.canvasW + 32 && cy < g_view.canvasH + 32 ? 1 : 0);
}

void op_dialogue_system_enter(Vm& vm)
{
    Object* me = self(vm);
    if (!me) return;
    if (me->isCritter() && !critterIsActive(me)) return;
    if (inCombat()) return;
    // the original requests the dialog game state, which then runs talk_p_proc
    gameRequestDialog(me);
}

void op_action_being_used(Vm& vm)
{
    ScriptInstance* s = S(vm);
    vm.pushInt(s ? s->actionBeingUsed : -1);
}

void op_critter_state(Vm& vm)
{
    Object* c = popObj(vm);
    int state = 1; // dead
    if (c && c->isCritter() && critterIsActive(c)) {
        state = 0;
        int anim = fidAnim(c->fid);
        if (anim >= ANIM_FALL_BACK_SF && anim <= ANIM_FALL_FRONT_SF) state = 2;
        state |= (int)(c->critter.results & DAM_CRIP);
    }
    vm.pushInt(state);
}

void op_game_time_advance(Vm& vm)
{
    int ticks = vm.popInt();
    int days = ticks / kTicksPerDay, rem = ticks % kTicksPerDay;
    for (int d = 0; d < days; d++) {
        g_game.time += kTicksPerDay;
        queueProcess();
    }
    g_game.time += rem;
    queueProcess();
}

void op_radiation_inc(Vm& vm)
{
    int amount = vm.popInt();
    Object* o = popObj(vm);
    if (o && o->isCritter()) o->critter.radiation = std::max(0, o->critter.radiation + amount);
}

void op_radiation_dec(Vm& vm)
{
    int amount = vm.popInt();
    Object* o = popObj(vm);
    if (o && o->isCritter()) o->critter.radiation = std::max(0, o->critter.radiation - amount);
}

void op_critter_attempt_placement(Vm& vm)
{
    int elev = vm.popInt();
    int tile = vm.popInt();
    Object* c = popObj(vm);
    if (!c) {
        vm.pushInt(-1);
        return;
    }
    // try the tile, then rings around it
    int placed = -1;
    for (int r = 0; r < 5 && placed < 0; r++) {
        if (r == 0) {
            if (!g_world.blockingAt(c, tile, elev)) placed = tile;
            continue;
        }
        for (int d = 0; d < 6 && placed < 0; d++) {
            int t = hex::inDirection(tile, d, r);
            if (!g_world.blockingAt(c, t, elev)) placed = t;
        }
    }
    if (placed < 0) placed = tile;
    g_world.place(c, placed, elev);
    vm.pushInt(0);
}

void op_obj_pid(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o ? o->pid : -1);
}

void op_cur_map_index(Vm& vm) { vm.pushInt(g_game.mapIndex); }

void op_critter_add_trait(Vm& vm)
{
    int value = vm.popInt();
    int param = vm.popInt();
    int kind = vm.popInt();
    Object* o = popObj(vm);
    if (o && o->isCritter()) {
        if (kind == 0) {
            if (param >= 0 && param < PERK_COUNT) g_game.player.perks[param]++;
        } else if (kind == 1) {
            if (param == 5) o->critter.aiPacket = value;
            else if (param == 6) o->critter.team = value;
        }
    }
    vm.pushInt(-1);
}

void op_critter_rm_trait(Vm& vm)
{
    vm.popInt();
    int param = vm.popInt();
    int kind = vm.popInt();
    Object* o = popObj(vm);
    if (o && o->isCritter() && kind == 0 && param >= 0 && param < PERK_COUNT) g_game.player.perks[param] = 0;
    vm.pushInt(-1);
}

void op_proto_data(Vm& vm)
{
    int member = vm.popInt();
    int pid = vm.popInt();
    const Proto* p = g_protos.get(pid);
    int v = 0;
    if (p) {
        // common members: 0 pid, 1 message id, 2 fid, 3/4 light, 5 flags, 6 ext flags, 7 sid
        switch (member) {
        case 0: v = p->pid; break;
        case 1: v = p->messageId; break;
        case 2: v = p->fid; break;
        case 3: v = p->lightDistance; break;
        case 4: v = p->lightIntensity; break;
        case 5: v = (int)p->flags; break;
        case 6: v = (int)p->extFlags; break;
        case 7: v = p->sid; break;
        default:
            if (p->type() == OBJ_ITEM) {
                switch (member) {
                case 8: v = p->itemType; break;
                case 9: v = p->material; break;
                case 10: v = p->size; break;
                case 11: v = p->weight; break;
                case 12: v = p->cost; break;
                case 13: v = p->invFid; break;
                }
            }
            break;
        }
    }
    vm.pushInt(v);
}

void op_message_str(Vm& vm)
{
    int num = vm.popInt();
    int list = vm.popInt();
    vm.pushString(dialogMsg(list, num));
}

void op_critter_inven_obj(Vm& vm)
{
    int type = vm.popInt();
    Object* c = popObj(vm);
    if (!c || !c->isCritter()) {
        vm.pushInt(0);
        return;
    }
    switch (type) {
    case 0: pushObj(vm, itemWorn(c)); break;
    case 1:
        if (c == g_world.dude && g_game.activeHand == 0) pushObj(vm, nullptr);
        else pushObj(vm, itemRightHand(c));
        break;
    case 2:
        if (c == g_world.dude && g_game.activeHand != 0) pushObj(vm, nullptr);
        else pushObj(vm, itemLeftHand(c));
        break;
    case -2: vm.pushInt((int)c->inventory.size()); break;
    default: vm.pushInt(0); break;
    }
}

void op_obj_set_light_level(Vm& vm)
{
    int dist = vm.popInt();
    int intensity = vm.popInt();
    Object* o = popObj(vm);
    if (!o) return;
    o->lightRadius = dist;
    o->lightIntensity = intensity ? intensity * 65536 / 100 : 0;
    if (o->lightIntensity > 0) o->flags |= OF_LIGHTING;
}

void op_world_map(Vm&) { LOGI("world_map: not available in the demo"); }
void op_town_map(Vm&) { LOGI("town_map: not available in the demo"); }

void op_float_msg(Vm& vm)
{
    static int lastColor = 1;
    int type = vm.popInt();
    Value sv = vm.pop();
    Object* o = popObj(vm);
    if (!o || sv.type != VT_STRING) return;
    std::string text = sv.toString();
    if (text.empty() || o->elevation != g_game.elevation) return;
    if (type == -1) {
        type = lastColor + 1;
        if (type >= 13) type = 1;
        lastColor = type;
    }
    u32 color = 0xFFFFFF55u;
    int fontId = 101;
    switch (type) {
    case -2: color = 0xFFFF0000u; fontId = 103; g_view.centerOn(g_world.dude ? g_world.dude->tile : o->tile); break;
    case 0:
    case 8: color = 0xFFFFFF55u; break;
    case 1:
    case 5:
    case 10: color = 0xFF505050u; break;
    case 2: color = 0xFFFF0000u; break;
    case 3: color = 0xFF00FF00u; break;
    case 4: color = 0xFF0000FFu; break;
    case 6: color = 0xFFA0A0A0u; break;
    case 7: color = 0xFFFF9090u; break;
    case 9: color = 0xFFFFFFFFu; break;
    case 11: color = 0xFF404040u; break;
    case 12: color = 0xFF787878u; break;
    }
    g_view.addFloatText(o, text, color, fontId);
}

void op_metarule(Vm& vm)
{
    Value param = vm.pop();
    int rule = vm.popInt();
    switch (rule) {
    case 13: // signal end game
        g_game.demoOver = true;
        vm.pushInt(0);
        break;
    case 14: vm.pushInt(g_game.mapSaved ? 0 : 1); break; // first run (map not restored from a save)
    case 15: vm.pushInt(0); (void)param; break;
    case 16: vm.pushInt(1); break; // party count (player only)
    case 22: vm.pushInt(g_game.loadingGame ? 1 : 0); break; // is load game
    default: vm.pushInt(0); break;
    }
}

void op_anim(Vm& vm)
{
    int frame = vm.popInt();
    int anim = vm.popInt();
    Object* o = popObj(vm);
    if (!o) return;
    if (anim < ANIM_COUNT) {
        anim = correctDeath(o, anim, true);
        animBegin(ANIMSEQ_REPLACE);
        if (frame == 0) {
            animAnimate(o, anim, 0);
            if (anim >= ANIM_FALL_BACK && anim <= ANIM_FALL_FRONT_BLOOD) animSetFid(o, animFid(o, anim + 28), -1);
            if (o->isCritter()) o->critter.results &= DAM_KNOCKED_DOWN;
        } else {
            animAnimateReverse(o, anim, 0);
            int fid = animFid(o, anim);
            if (anim == ANIM_PRONE_TO_STANDING) fid = animFid(o, ANIM_FALL_FRONT_SF);
            else if (anim == ANIM_BACK_TO_STANDING) fid = animFid(o, ANIM_FALL_BACK_SF);
            if (o->isCritter()) o->critter.results |= DAM_KNOCKED_DOWN;
            animSetFid(o, fid, -1);
        }
        animEnd();
    } else if (anim == 1000) {
        if (frame >= 0 && frame < 6) o->rotation = frame;
    } else if (anim == 1010) {
        o->frame = frame;
    }
}

void op_obj_carrying_pid_obj(Vm& vm)
{
    int pid = vm.popInt();
    Object* o = popObj(vm);
    pushObj(vm, o ? g_world.findInInventory(o, pid) : nullptr);
}

void op_reg_anim_func(Vm& vm)
{
    Value arg = vm.pop();
    int func = vm.popInt();
    if (inCombat()) return;
    switch (func) {
    case 1: animBegin(arg.toInt() == 0 ? ANIMSEQ_REPLACE : ANIMSEQ_REPLACE | ANIMSEQ_UNINTERRUPTIBLE); break; // ANIM_BEGIN
    case 2: animClear(); if (Object* o = g_world.byUid(arg.type == VT_PTR ? arg.u : (u32)arg.i)) animStop(o); break; // ANIM_CLEAR
    case 3: animEnd(); break; // ANIM_END
    }
}

void op_reg_anim_animate(Vm& vm)
{
    int delay = vm.popInt();
    int anim = vm.popInt();
    Object* o = popObj(vm);
    if (o && !inCombat()) animAnimate(o, anim, delay);
}

void op_reg_anim_animate_reverse(Vm& vm)
{
    int delay = vm.popInt();
    int anim = vm.popInt();
    Object* o = popObj(vm);
    if (o && !inCombat()) animAnimateReverse(o, anim, delay);
}

void op_reg_anim_obj_move_to_obj(Vm& vm)
{
    int delay = vm.popInt();
    Object* dest = popObj(vm);
    Object* o = popObj(vm);
    if (o && dest && !inCombat()) animMoveToObject(o, dest, -1, false, delay);
}

void op_reg_anim_obj_run_to_obj(Vm& vm)
{
    int delay = vm.popInt();
    Object* dest = popObj(vm);
    Object* o = popObj(vm);
    if (o && dest && !inCombat()) animMoveToObject(o, dest, -1, true, delay);
}

void op_reg_anim_obj_move_to_tile(Vm& vm)
{
    int delay = vm.popInt();
    int tile = vm.popInt();
    Object* o = popObj(vm);
    if (o && !inCombat()) animMoveToTile(o, tile, o->elevation, -1, false, delay);
}

void op_reg_anim_obj_run_to_tile(Vm& vm)
{
    int delay = vm.popInt();
    int tile = vm.popInt();
    Object* o = popObj(vm);
    if (o && !inCombat()) animMoveToTile(o, tile, o->elevation, -1, true, delay);
}

void op_play_gmovie(Vm& vm)
{
    int movie = vm.popInt();
    LOGI("play_gmovie %d: movies are not part of the demo", movie);
}

void op_add_mult_objs_to_inven(Vm& vm)
{
    int count = vm.popInt();
    Object* item = popObj(vm);
    Object* owner = popObj(vm);
    if (!owner || !item) return;
    if (count < 1) count = 1;
    if (count > 99999) count = 500;
    g_world.addToInventory(owner, item, count);
}

void op_rm_mult_objs_from_inven(Vm& vm)
{
    int count = vm.popInt();
    Object* item = popObj(vm);
    Object* owner = popObj(vm);
    if (!owner || !item) {
        vm.pushInt(0);
        return;
    }
    InvItem* e = g_world.invEntry(owner, item);
    int have = e ? e->count : 0;
    int n = std::min(count, have);
    bool equipped = (item->flags & OF_EQUIPPED) != 0;
    if (n > 0) {
        Object* removed = g_world.removeFromInventory(owner, item, n);
        if (removed && removed != item) g_world.destroy(removed);
        if (equipped) critterUpdateArt(owner);
    }
    vm.pushInt(n);
}

void op_get_month(Vm& vm)
{
    static const int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int day = (gameTime() / kTicksPerDay + 4) % 365;
    int month = 11;
    while (day >= kDays[month]) {
        day -= kDays[month];
        month = (month + 1) % 12;
    }
    vm.pushInt(month + 1);
}

void op_get_day(Vm& vm)
{
    static const int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int day = (gameTime() / kTicksPerDay + 4) % 365;
    int month = 11;
    while (day >= kDays[month]) {
        day -= kDays[month];
        month = (month + 1) % 12;
    }
    vm.pushInt(day + 1);
}

void op_explosion(Vm& vm)
{
    int maxDmg = vm.popInt();
    int elev = vm.popInt();
    int tile = vm.popInt();
    combatExplosion(tile, elev, maxDmg / 2, maxDmg, nullptr);
}

void op_days_since_visited(Vm& vm) { vm.pushInt(0); }

// release dialog opcodes (mapped onto the same dialog system)
void op_gsay_start(Vm& vm) { g_dialog.sayStart(vm); }
void op_gsay_end(Vm& vm) { g_dialog.sayEnd(vm); }

void op_gsay_reply(Vm& vm)
{
    Value msg = vm.pop();
    int list = vm.popInt();
    std::string text = msg.type == VT_STRING ? msg.toString() : dialogMsg(list, msg.toInt());
    g_dialog.sayReply("", text);
}

void op_gsay_option(Vm& vm)
{
    int reaction = vm.popInt();
    Value proc = vm.pop();
    Value msg = vm.pop();
    int list = vm.popInt();
    std::string text = msg.type == VT_STRING ? msg.toString() : dialogMsg(list, msg.toInt());
    if (proc.type == VT_STRING) {
        int idx = vm.prog->findProc(proc.toString());
        g_dialog.sayOption(text, idx, "", reaction);
    } else {
        g_dialog.sayOption(text, proc.toInt(), "", reaction);
    }
}

void op_gsay_message(Vm& vm)
{
    vm.popInt(); // reaction
    Value msg = vm.pop();
    int list = vm.popInt();
    std::string text = msg.type == VT_STRING ? msg.toString() : dialogMsg(list, msg.toInt());
    g_dialog.sayMessage("", text);
}

void op_giq_option(Vm& vm)
{
    int reaction = vm.popInt();
    Value proc = vm.pop();
    Value msg = vm.pop();
    int list = vm.popInt();
    int iq = vm.popInt();
    int playerIq = critterStat(g_world.dude, STAT_IN);
    if (iq < 0) {
        if (-playerIq < iq) return;
    } else if (playerIq < iq) {
        return;
    }
    std::string text = msg.type == VT_STRING ? msg.toString() : dialogMsg(list, msg.toInt());
    int idx = proc.type == VT_STRING ? vm.prog->findProc(proc.toString()) : proc.toInt();
    g_dialog.sayOption(text, idx, "", reaction);
}

void op_poison(Vm& vm)
{
    int amount = vm.popInt();
    Object* o = popObj(vm);
    if (o && o->isCritter()) o->critter.poison = std::max(0, o->critter.poison + amount);
}

void op_get_poison(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o && o->isCritter() ? o->critter.poison : 0);
}

void op_party_add(Vm& vm) { popObj(vm); }
void op_party_remove(Vm& vm) { popObj(vm); }

void op_reg_anim_animate_forever(Vm& vm)
{
    int delay = vm.popInt();
    int anim = vm.popInt();
    Object* o = popObj(vm);
    if (o && !inCombat()) animAnimateForever(o, anim, delay);
}

void op_critter_injure(Vm& vm)
{
    int flags = vm.popInt();
    Object* o = popObj(vm);
    if (!o || !o->isCritter()) return;
    o->critter.results |= (u32)(flags & DAM_CRIP);
}

void op_combat_is_initialized(Vm& vm) { vm.pushInt(inCombat() ? 1 : 0); }

void op_gdialog_barter(Vm& vm)
{
    vm.popInt();
    if (g_dialog.onBarter) g_dialog.onBarter();
}

void op_difficulty_level(Vm& vm) { vm.pushInt(g_game.difficulty); }
void op_running_burning_guy(Vm& vm) { vm.pushInt(1); }

void op_inven_unwield(Vm& vm)
{
    Object* me = self(vm);
    if (!me) return;
    Object* w = itemRightHand(me);
    if (w) itemUnequip(me, w);
}

void op_obj_is_locked(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o && objIsLocked(o) ? 1 : 0);
}

void op_obj_lock(Vm& vm)
{
    Object* o = popObj(vm);
    if (o) objSetLocked(o, true);
}

void op_obj_unlock(Vm& vm)
{
    Object* o = popObj(vm);
    if (o) objSetLocked(o, false);
}

void op_obj_is_open(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o && objIsOpen(o) ? 1 : 0);
}

void op_obj_open(Vm& vm)
{
    Object* o = popObj(vm);
    if (o) objOpen(o);
}

void op_obj_close(Vm& vm)
{
    Object* o = popObj(vm);
    if (o) objClose(o);
}

void op_game_ui_disable(Vm&) { g_game.uiDisabled = true; }
void op_game_ui_enable(Vm&) { g_game.uiDisabled = false; }
void op_game_ui_is_disabled(Vm& vm) { vm.pushInt(g_game.uiDisabled ? 1 : 0); }
void op_gfade_out(Vm& vm) { vm.popInt(); gameFade(true); }
void op_gfade_in(Vm& vm) { vm.popInt(); gameFade(false); }

void op_item_caps_total(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o ? capsCount(o) : 0);
}

void op_item_caps_adjust(Vm& vm)
{
    int amount = vm.popInt();
    Object* o = popObj(vm);
    int rc = -1;
    if (o) {
        if (amount < 0 && capsCount(o) < -amount) rc = -1;
        else {
            capsAdjust(o, amount);
            rc = 0;
        }
    }
    vm.pushInt(rc);
}

void op_anim_action_frame(Vm& vm)
{
    int anim = vm.popInt();
    Object* o = popObj(vm);
    int r = 0;
    if (o) {
        if (Sprite* s = g_art.sprite(animFid(o, anim))) r = s->meta.actionFrame;
    }
    vm.pushInt(r);
}

void op_reg_anim_play_sfx(Vm& vm)
{
    int delay = vm.popInt();
    std::string name = vm.popString();
    Object* o = popObj(vm);
    if (o && !inCombat()) animPlaySfx(o, name, delay);
}

void op_critter_mod_skill(Vm& vm)
{
    int points = vm.popInt();
    int skill = vm.popInt();
    Object* o = popObj(vm);
    if (o && o->isCritter() && o == g_world.dude) critterSetSkillPoints(o, skill, critterSkillPoints(o, skill) + points);
    vm.pushInt(0);
}

void op_sfx_build_char_name(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    popObj(vm);
    vm.pushString("");
}

void op_sfx_build_ambient_name(Vm& vm) { vm.pushString(vm.popString()); }
void op_sfx_build_interface_name(Vm& vm) { vm.pushString(vm.popString()); }

void op_sfx_build_item_name(Vm& vm) { vm.pushString(vm.popString()); }

void op_sfx_build_weapon_name(Vm& vm)
{
    popObj(vm);
    vm.popInt();
    popObj(vm);
    vm.popInt();
    vm.pushString("");
}

void op_sfx_build_scenery_name(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    popObj(vm);
    vm.pushString("");
}

void op_attack_setup(Vm& vm)
{
    Object* defender = popObj(vm);
    Object* attacker = popObj(vm);
    if (!attacker || !defender || !critterIsActive(attacker) || !critterIsActive(defender)) return;
    combatScriptAttack(attacker, defender, 0, 1, 0, 0, 0x7FFFFFFF, 0, 0);
}

void op_destroy_mult_objs(Vm& vm)
{
    int count = vm.popInt();
    Object* item = popObj(vm);
    int n = 0;
    if (item) {
        Object* owner = item->owner;
        if (owner) {
            InvItem* e = g_world.invEntry(owner, item);
            n = std::min(count, e ? e->count : 0);
            Object* removed = g_world.removeFromInventory(owner, item, n);
            if (removed) g_world.destroy(removed);
        } else {
            g_world.destroy(item);
            n = 1;
        }
    }
    vm.pushInt(n);
}

void op_use_obj_on_obj(Vm& vm)
{
    Object* target = popObj(vm);
    Object* item = popObj(vm);
    Object* me = self(vm);
    if (item && target && me) actionUseItemOn(me, item, target);
}

void op_endgame_slideshow(Vm&) { g_game.demoOver = true; }

void op_move_obj_inven_to_obj(Vm& vm)
{
    Object* dest = popObj(vm);
    Object* src = popObj(vm);
    if (!src || !dest) return;
    auto items = src->inventory;
    for (auto& e : items) {
        Object* it = g_world.removeFromInventory(src, e.obj, e.count);
        if (it) g_world.addToInventory(dest, it, e.count);
    }
    if (src->isCritter()) critterUpdateArt(src);
}

void op_endgame_movie(Vm&) { g_game.demoOver = true; }

void op_obj_art_fid(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o ? o->fid : 0);
}

void op_art_anim(Vm& vm) { vm.pushInt(fidAnim(vm.popInt())); }

void op_party_member_obj(Vm& vm)
{
    vm.popInt();
    pushObj(vm, nullptr);
}

void op_rotation_to_tile(Vm& vm)
{
    int t2 = vm.popInt(), t1 = vm.popInt();
    vm.pushInt(hex::direction(t1, t2));
}

void op_jam_lock(Vm& vm)
{
    Object* o = popObj(vm);
    if (o) o->dataFlags |= LOCK_FLAG_JAMMED;
}

void op_gdialog_set_barter_mod(Vm& vm) { vm.popInt(); }

void op_combat_difficulty(Vm& vm) { vm.pushInt(g_game.combatDifficulty); }

void op_obj_on_screen(Vm& vm)
{
    Object* o = popObj(vm);
    int r = 0;
    if (o && o->tile >= 0 && o->elevation == g_game.elevation) {
        float x, y, w, h;
        if (g_view.objectRect(o, x, y, w, h)) r = (x + w >= 0 && y + h >= 0 && x < g_view.canvasW && y < g_view.canvasH) ? 1 : 0;
    }
    vm.pushInt(r);
}

void op_critter_is_fleeing(Vm& vm)
{
    Object* o = popObj(vm);
    vm.pushInt(o && o->isCritter() && (o->critter.maneuver & 0x04) ? 1 : 0);
}

void op_critter_set_flee_state(Vm& vm)
{
    int on = vm.popInt();
    Object* o = popObj(vm);
    if (!o || !o->isCritter()) return;
    if (on) o->critter.maneuver |= 0x04;
    else o->critter.maneuver &= ~0x04;
}

void op_terminate_combat(Vm&) { combatEnd(true); }
void op_debug_msg(Vm& vm) { LOGI("[script] %s", vm.popString().c_str()); }

void op_critter_stop_attacking(Vm& vm)
{
    Object* o = popObj(vm);
    if (o && o->isCritter()) {
        o->critter.maneuver = 0;
        o->critter.whoHitMe = 0;
    }
}

// ------------------------------------------------------------------ intlib "say" dialog opcodes
void op_sayQuit(Vm&) { g_dialog.sayQuit(); }
void op_sayEnd(Vm& vm) { g_dialog.sayEnd(vm); }
void op_sayStart(Vm& vm) { g_dialog.sayStart(vm); }

void op_sayStartPos(Vm& vm)
{
    vm.popInt();
    g_dialog.sayStart(vm);
}

void op_sayReplyTitle(Vm& vm) { vm.pop(); }

void op_sayGotoReply(Vm& vm) { vm.pop(); }

void op_sayReply(Vm& vm)
{
    Value text = vm.pop();
    Value name = vm.pop();
    g_dialog.sayReply(name.type == VT_STRING ? name.toString() : std::string(), text.type == VT_STRING ? text.toString() : std::string());
}

void op_sayOption(Vm& vm)
{
    Value target = vm.pop();
    Value text = vm.pop();
    std::string t = text.type == VT_STRING ? text.toString() : std::string();
    if (target.type == VT_STRING) g_dialog.sayOption(t, -1, target.toString());
    else g_dialog.sayOption(t, target.toInt());
}

void op_sayMessage(Vm& vm)
{
    Value text = vm.pop();
    Value name = vm.pop();
    g_dialog.sayMessage(name.type == VT_STRING ? name.toString() : std::string(), text.type == VT_STRING ? text.toString() : std::string());
}

// ignored intlib window/say presentation opcodes: pop their arguments
template <int N> void op_popN(Vm& vm)
{
    for (int i = 0; i < N; i++) vm.pop();
}
template <int N> void op_popNpush0(Vm& vm)
{
    for (int i = 0; i < N; i++) vm.pop();
    vm.pushInt(0);
}

} // namespace

void registerGameOpcodes()
{
#define REG(code, fn) Vm::registerOp(code, fn, #fn + 3)
    REG(0x80A1, op_give_exp_points);
    REG(0x80A2, op_scr_return);
    REG(0x80A3, op_play_sfx);
    REG(0x80A4, op_obj_name);
    REG(0x80A5, op_sfx_build_open_name);
    REG(0x80A6, op_get_pc_stat);
    REG(0x80A7, op_tile_contains_pid_obj);
    REG(0x80A8, op_set_map_start);
    REG(0x80A9, op_override_map_start);
    REG(0x80AA, op_has_skill);
    REG(0x80AB, op_using_skill);
    REG(0x80AC, op_roll_vs_skill);
    REG(0x80AD, op_skill_contest);
    REG(0x80AE, op_do_check);
    REG(0x80AF, op_is_success);
    REG(0x80B0, op_is_critical);
    REG(0x80B1, op_how_much);
    REG(0x80B2, op_reaction_roll);
    REG(0x80B3, op_reaction_influence);
    REG(0x80B4, op_random);
    REG(0x80B5, op_roll_dice);
    REG(0x80B6, op_move_to);
    REG(0x80B7, op_create_object_sid);
    REG(0x80B8, op_display_msg);
    REG(0x80B9, op_script_overrides);
    REG(0x80BA, op_obj_is_carrying_obj_pid);
    REG(0x80BB, op_tile_contains_obj_pid);
    REG(0x80BC, op_self_obj);
    REG(0x80BD, op_source_obj);
    REG(0x80BE, op_target_obj);
    REG(0x80BF, op_dude_obj);
    REG(0x80C0, op_target_obj); // obj_being_used_with (same as target_obj in the original)
    REG(0x80C1, op_local_var);
    REG(0x80C2, op_set_local_var);
    REG(0x80C3, op_map_var);
    REG(0x80C4, op_set_map_var);
    REG(0x80C5, op_global_var);
    REG(0x80C6, op_set_global_var);
    REG(0x80C7, op_script_action);
    REG(0x80C8, op_obj_type);
    REG(0x80C9, op_obj_item_subtype);
    REG(0x80CA, op_get_critter_stat);
    REG(0x80CB, op_set_critter_stat);
    REG(0x80CC, op_animate_stand_obj);
    REG(0x80CD, op_animate_stand_reverse_obj);
    REG(0x80CE, op_animate_move_obj_to_tile);
    REG(0x80CF, op_animate_jump);
    REG(0x80D0, op_attack);
    REG(0x80D1, op_make_daytime);
    REG(0x80D2, op_tile_distance);
    REG(0x80D3, op_tile_distance_objs);
    REG(0x80D4, op_tile_num);
    REG(0x80D5, op_tile_num_in_direction);
    REG(0x80D6, op_pickup_obj);
    REG(0x80D7, op_drop_obj);
    REG(0x80D8, op_add_obj_to_inven);
    REG(0x80D9, op_rm_obj_from_inven);
    REG(0x80DA, op_wield_obj_critter);
    REG(0x80DB, op_use_obj);
    REG(0x80DC, op_obj_can_see_obj);
    REG(0x80DD, op_attack);
    REG(0x80DE, op_start_gdialog);
    REG(0x80DF, op_end_dialogue);
    REG(0x80E0, op_dialogue_reaction);
    REG(0x80E1, op_turn_off_objs_in_area);
    REG(0x80E2, op_turn_on_objs_in_area);
    REG(0x80E3, op_set_obj_visibility);
    REG(0x80E4, op_load_map);
    REG(0x80E5, op_barter_offer);
    REG(0x80E6, op_barter_asking);
    REG(0x80E7, op_anim_busy);
    REG(0x80E8, op_critter_heal);
    REG(0x80E9, op_set_light_level);
    REG(0x80EA, op_game_time);
    REG(0x80EB, op_game_time_in_seconds);
    REG(0x80EC, op_elevation);
    REG(0x80ED, op_kill_critter);
    REG(0x80EE, op_kill_critter_type);
    REG(0x80EF, op_critter_damage);
    REG(0x80F0, op_add_timer_event);
    REG(0x80F1, op_rm_timer_event);
    REG(0x80F2, op_game_ticks);
    REG(0x80F3, op_has_trait);
    REG(0x80F4, op_destroy_object);
    REG(0x80F5, op_obj_can_hear_obj);
    REG(0x80F6, op_game_time_hour);
    REG(0x80F7, op_fixed_param);
    REG(0x80F8, op_tile_is_visible);
    REG(0x80F9, op_dialogue_system_enter);
    REG(0x80FA, op_action_being_used);
    REG(0x80FB, op_critter_state);
    REG(0x80FC, op_game_time_advance);
    REG(0x80FD, op_radiation_inc);
    REG(0x80FE, op_radiation_dec);
    REG(0x80FF, op_critter_attempt_placement);
    REG(0x8100, op_obj_pid);
    REG(0x8101, op_cur_map_index);
    REG(0x8102, op_critter_add_trait);
    REG(0x8103, op_critter_rm_trait);
    REG(0x8104, op_proto_data);
    REG(0x8105, op_message_str);
    REG(0x8106, op_critter_inven_obj);
    REG(0x8107, op_obj_set_light_level);
    REG(0x8108, op_world_map);
    REG(0x8109, op_town_map);
    REG(0x810A, op_float_msg);
    REG(0x810B, op_metarule);
    REG(0x810C, op_anim);
    REG(0x810D, op_obj_carrying_pid_obj);
    REG(0x810E, op_reg_anim_func);
    REG(0x810F, op_reg_anim_animate);
    REG(0x8110, op_reg_anim_animate_reverse);
    REG(0x8111, op_reg_anim_obj_move_to_obj);
    REG(0x8112, op_reg_anim_obj_run_to_obj);
    REG(0x8113, op_reg_anim_obj_move_to_tile);
    REG(0x8114, op_reg_anim_obj_run_to_tile);
    REG(0x8115, op_play_gmovie);
    REG(0x8116, op_add_mult_objs_to_inven);
    REG(0x8117, op_rm_mult_objs_from_inven);
    REG(0x8118, op_get_month);
    REG(0x8119, op_get_day);
    REG(0x811A, op_explosion);
    REG(0x811B, op_days_since_visited);
    REG(0x811C, op_gsay_start);
    REG(0x811D, op_gsay_end);
    REG(0x811E, op_gsay_reply);
    REG(0x811F, op_gsay_option);
    REG(0x8120, op_gsay_message);
    REG(0x8121, op_giq_option);
    REG(0x8122, op_poison);
    REG(0x8123, op_get_poison);
    REG(0x8124, op_party_add);
    REG(0x8125, op_party_remove);
    REG(0x8126, op_reg_anim_animate_forever);
    REG(0x8127, op_critter_injure);
    REG(0x8128, op_combat_is_initialized);
    REG(0x8129, op_gdialog_barter);
    REG(0x812A, op_difficulty_level);
    REG(0x812B, op_running_burning_guy);
    REG(0x812C, op_inven_unwield);
    REG(0x812D, op_obj_is_locked);
    REG(0x812E, op_obj_lock);
    REG(0x812F, op_obj_unlock);
    REG(0x8130, op_obj_is_open);
    REG(0x8131, op_obj_open);
    REG(0x8132, op_obj_close);
    REG(0x8133, op_game_ui_disable);
    REG(0x8134, op_game_ui_enable);
    REG(0x8135, op_game_ui_is_disabled);
    REG(0x8136, op_gfade_out);
    REG(0x8137, op_gfade_in);
    REG(0x8138, op_item_caps_total);
    REG(0x8139, op_item_caps_adjust);
    REG(0x813A, op_anim_action_frame);
    REG(0x813B, op_reg_anim_play_sfx);
    REG(0x813C, op_critter_mod_skill);
    REG(0x813D, op_sfx_build_char_name);
    REG(0x813E, op_sfx_build_ambient_name);
    REG(0x813F, op_sfx_build_interface_name);
    REG(0x8140, op_sfx_build_item_name);
    REG(0x8141, op_sfx_build_weapon_name);
    REG(0x8142, op_sfx_build_scenery_name);
    REG(0x8143, op_attack_setup);
    REG(0x8144, op_destroy_mult_objs);
    REG(0x8145, op_use_obj_on_obj);
    REG(0x8146, op_endgame_slideshow);
    REG(0x8147, op_move_obj_inven_to_obj);
    REG(0x8148, op_endgame_movie);
    REG(0x8149, op_obj_art_fid);
    REG(0x814A, op_art_anim);
    REG(0x814B, op_party_member_obj);
    REG(0x814C, op_rotation_to_tile);
    REG(0x814D, op_jam_lock);
    REG(0x814E, op_gdialog_set_barter_mod);
    REG(0x814F, op_combat_difficulty);
    REG(0x8150, op_obj_on_screen);
    REG(0x8151, op_critter_is_fleeing);
    REG(0x8152, op_critter_set_flee_state);
    REG(0x8153, op_terminate_combat);
    REG(0x8154, op_debug_msg);
    REG(0x8155, op_critter_stop_attacking);

    // intlib dialog ("say") opcodes used by the beta scripts
    REG(0x804C, op_sayQuit);
    REG(0x804D, op_sayEnd);
    REG(0x804E, op_sayStart);
    REG(0x804F, op_sayStartPos);
    REG(0x8050, op_sayReplyTitle);
    REG(0x8051, op_sayGotoReply);
    REG(0x8052, op_sayReply);
    REG(0x8053, op_sayOption);
    REG(0x8054, op_sayMessage);
    // presentation-only intlib opcodes (windows / colours / fonts) are accepted and ignored
    Vm::registerOp(0x8055, op_popN<5>, "sayReplyWindow");
    Vm::registerOp(0x8056, op_popN<5>, "sayOptionWindow");
    Vm::registerOp(0x8057, op_popN<2>, "sayBorder");
    Vm::registerOp(0x8058, op_popN<7>, "sayScrollUp");
    Vm::registerOp(0x8059, op_popN<7>, "sayScrollDown");
    Vm::registerOp(0x805A, op_popN<1>, "saySetSpacing");
    Vm::registerOp(0x805B, op_popN<3>, "sayOptionColor");
    Vm::registerOp(0x805C, op_popN<3>, "sayReplyColor");
    Vm::registerOp(0x805D, op_popN<0>, "sayRestart");
    Vm::registerOp(0x805E, op_popNpush0<0>, "sayGetLastPos");
    Vm::registerOp(0x805F, op_popN<1>, "sayReplyFlags");
    Vm::registerOp(0x8060, op_popN<1>, "sayOptionFlags");
    Vm::registerOp(0x8061, op_popN<1>, "sayMessageTimeout");
#undef REG
}

} // namespace fo
