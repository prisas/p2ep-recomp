#pragma once

#include "lib/common/types.h"

// Gameplay adjustments the program makes to the game, through hooks the recompiler places at the
// addresses in data/hooks.txt (see game_hook in runtime/cpu/recomp.h).
//
// Encounter rate: random battles come at `encounter_rate` percent of the game's own chance (0 to
// 200). Below 100%, each battle's experience and money are multiplied to make up for the battles
// that no longer happen: 100% / rate, at most 3 times.
namespace p2::game {

constexpr u32 kMaxEncounterRate = 200;
constexpr f32 kMaxRewardMultiplier = 3.0f;

// The experience and money multiplier for an encounter rate in percent.
f32 reward_multiplier(u32 encounter_rate);

} // namespace p2::game
