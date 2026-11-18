#include "runtime/game/tweaks.h"

#include "runtime/cpu/recomp.h"
#include "runtime/platform/config.h"

#include <algorithm>

namespace p2::game {

f32 reward_multiplier(u32 encounter_rate) {
    if (encounter_rate >= 100) return 1.0f;
    if (encounter_rate == 0) return kMaxRewardMultiplier;
    return std::min(kMaxRewardMultiplier, 100.0f / f32(encounter_rate));
}

} // namespace p2::game

namespace p2 {

void game_hook(Cpu* c, u8* m, u32 pc) {
    (void)c;
    (void)m;
    (void)pc;
}

} // namespace p2
