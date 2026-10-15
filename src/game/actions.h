// High level interactions: talk, look, use, pick up, doors, containers, skills, items.
#pragma once
#include "game/object.h"

namespace fo {

// Look at (name) or examine (description, health). Prints to the message window.
void actionLook(Object* who, Object* target, bool detailed);
// Walks into talking range and starts the conversation.
void actionTalk(Object* who, Object* target);
// Uses a world object: scenery (doors, switches, stairs), opens containers, loots bodies.
void actionUse(Object* who, Object* target);
// Uses an inventory item on a world object (or on a critter / the user).
void actionUseItemOn(Object* who, Object* item, Object* target);
// Uses a skill on a target (target may be the user). Sneak toggles without a target.
void actionUseSkill(Object* who, int skill, Object* target);
// Picks up a ground item (containers are opened for looting instead).
void actionPickup(Object* who, Object* item);
// Drops `count` of an inventory item at the critter's feet.
void actionDrop(Object* who, Object* item, int count);
// Uses an item from the inventory (drugs, flares...). Returns true if one was consumed.
bool actionUseItem(Object* who, Object* item);
// Walks (or runs) to a hex.
void actionMoveTo(Object* who, int tile, bool run);
// Rotates a critter one step clockwise (the "rotate" command on the player).
void actionRotate(Object* who);

// Doors & containers
bool objIsDoor(Object* o);
bool objIsOpen(Object* o);
bool objIsLocked(Object* o);
bool objIsJammed(Object* o);
void objSetLocked(Object* o, bool locked);
// Opens/closes a door. `passThrough`: open and close again behind a walking critter.
void objUseDoor(Object* user, Object* door, bool passThrough);
void objOpen(Object* o);
void objClose(Object* o);
bool objIsContainer(Object* o);

// Sound effect names for doors/containers. action: 0 open, 1 close, 2 locked, 3 unlock, 4 use.
std::string sfxOpenName(Object* o, int action);

// Default command-cursor action for `target`.
enum ActionKind : int {
    ACT_NONE = -1, ACT_CANCEL = 0, ACT_DROP, ACT_INVENTORY, ACT_LOOK, ACT_ROTATE, ACT_TALK, ACT_USE, ACT_UNLOAD, ACT_SKILL, ACT_COUNT
};
int defaultActionFor(Object* target);
// Full command list for the action menu (cancel last).
std::vector<int> actionMenuFor(Object* target);
bool canTalkTo(Object* target);

// Runs a script procedure on `target` and returns true if the script called script_overrides().
bool scriptHandled(Object* target, int proc, Object* source, Object* targetObj = nullptr, int param = -9999, int actionBeingUsed = -1);

// Drug effects (inventory use / use on critter)
bool drugTake(Object* critter, Object* item);

// Interface requests raised by actions; the game screen opens the matching screen.
enum UiRequestKind : int { UIREQ_LOOT = 1, UIREQ_STEAL = 2 };
struct UiRequest {
    int kind = 0;
    u32 target = 0;
};
void actionsRequestUi(int kind, Object* target);
bool actionsPollUi(UiRequest& out);

} // namespace fo
