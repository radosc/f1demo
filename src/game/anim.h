// Animation sequences: walking/running along paths, playing critter/scenery animations, scripted chains.
#pragma once
#include "game/object.h"

namespace fo {

enum AnimSeqFlags : int {
    ANIMSEQ_NONE = 0,
    ANIMSEQ_UNINTERRUPTIBLE = 0x01, // keeps running if a new sequence starts for the same object
    ANIMSEQ_NO_STAND = 0x02,        // do not return to the stand pose at the end
    ANIMSEQ_REPLACE = 0x04,         // stop any running sequence of the objects involved
    ANIMSEQ_COMBAT = 0x08,          // counts as a combat animation (combat waits for it)
};

// Builder API (like the original register_begin / register_object_* / register_end).
// `delay` of a description: -1 = start after all previous descriptions finished, 0 = start together with the
// previous one, N > 0 = start after the previous description has played N frames (e.g. an attack's action frame).
void animBegin(int flags = ANIMSEQ_REPLACE);
void animMoveToTile(Object* obj, int tile, int elevation, int apLimit = -1, bool run = false, int delay = -1);
void animMoveToObject(Object* obj, Object* target, int apLimit = -1, bool run = false, int delay = -1);
void animMoveStraight(Object* obj, int tile, int elevation, int anim, int delay = -1);
void animAnimate(Object* obj, int anim, int delay = -1);
void animAnimateReverse(Object* obj, int anim, int delay = -1);
void animAnimateAndHide(Object* obj, int anim, int delay = -1);
void animAnimateForever(Object* obj, int anim, int delay = -1);
void animRotateToTile(Object* obj, int tile);
void animRotateClockwise(Object* obj);
void animRotateCounterClockwise(Object* obj);
void animHide(Object* obj, int delay = -1);
void animCallback(Object* obj, std::function<void()> fn, int delay = -1, bool forced = false);
void animSetFid(Object* obj, int fid, int delay = -1);
void animTakeOut(Object* obj, int weaponCode, int delay = -1);
void animPlaySfx(Object* obj, const std::string& sfx, int delay = -1);
void animSetFlag(Object* obj, u32 flag, int delay = -1);
void animUnsetFlag(Object* obj, u32 flag, int delay = -1);
void animSetLight(Object* obj, int radius, int delay = -1);
int animEnd(); // starts the sequence (returns -1 if empty/invalid)
void animClear(); // abandons the sequence under construction

bool animBusy(Object* obj);
bool animBusyAny(bool combatOnly = false);
void animStop(Object* obj);
void animStopAll();
void animUpdate(float dt);

// Puts a critter into its standing pose (handles weapon offsets like the original).
void critterStand(Object* obj, int rotation);
// FID of `obj` with a different animation (keeps weapon code unless the anim is unarmed-only).
int animFid(Object* obj, int anim);
bool animArtExists(Object* obj, int anim);
// Ticks per frame for an animation (ms), honours combat speed settings.
int animFrameTime(Object* obj, int fid);

// Set by the combat module: called for every hex a critter enters while moving in combat.
// Returns false to stop the movement (out of action points).
extern std::function<bool(Object*)> g_animCombatStep;
// Called when a moving object enters a new hex (spatial scripts, roof visibility...).
extern std::function<void(Object*)> g_animOnTileChanged;
extern float g_animSpeed; // global animation speed multiplier (options)

} // namespace fo
