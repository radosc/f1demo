// Turn based combat: sequence, action points, attacks, damage, AI turns.
#pragma once
#include "game/object.h"

namespace fo {

struct AttackResult {
    Object* attacker = nullptr;
    Object* target = nullptr;
    Object* weapon = nullptr;
    int hitMode = 0;
    int location = 8;       // HIT_UNCALLED
    bool hit = false;
    bool critical = false;
    int damage = 0;
    u32 targetFlags = 0;    // DAM_* applied to the target
    u32 attackerFlags = 0;
    int rounds = 1;
    std::vector<std::pair<Object*, int>> extras; // other objects hit (bursts) and damage
};

bool combatActive();
Object* combatWhoseTurn();
// Starts combat (or adds participants). `attacker` gets the first turn when given.
void combatStart(Object* attacker, Object* target);
// Ends combat. `force` = immediately (map change), otherwise only if no hostiles remain.
bool combatEnd(bool force);
void combatUpdate(float dt);
// Player requested end of turn / end of combat.
void combatEndPlayerTurn();
bool combatTryEnd();
bool combatIsPlayerTurn();
int combatFreeMove();
// Spends action points for a non-attack action. Always true outside combat; prints
// "You don't have enough action points." for the player when short.
bool combatUseAp(Object* critter, int ap);

// Attack resolution (also used outside combat by scripts).
int combatToHit(Object* attacker, int tile, Object* target, int hitMode, int location, bool checkRange = true);
bool combatAttack(Object* attacker, Object* target, int hitMode, int location);
// Scripted attack (attack_complex): starts combat with `attacker` hostile to `target`.
void combatScriptAttack(Object* attacker, Object* target, int calledShot, int numAttacks, int bonus, int minDmg, int maxDmg, int attackerResults,
    int targetResults);

// Explosion at a tile: damages everything within 2 hexes (explosion opcode, grenades, dynamite).
void combatExplosion(int tile, int elevation, int minDmg, int maxDmg, Object* source);

// Damage / death
void critterApplyDamage(Object* target, int amount, Object* source, u32 resultFlags, int damageType);
void critterKill(Object* critter, int deathAnim, bool animate);
void critterKnockOut(Object* critter, int turns);
void critterWakeUp(Object* critter);
bool critterIsProne(Object* critter);

// Detailed turn / AI decision logging (--combat-log).
extern bool g_combatLog;

// Combat message text helpers (combat.msg)
std::string combatMsg(int num);

} // namespace fo
