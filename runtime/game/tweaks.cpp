#include "runtime/game/tweaks.h"

#include "lib/common/log.h"
#include "runtime/cpu/ops.h"
#include "runtime/cpu/recomp.h"
#include "runtime/platform/config.h"

#include <algorithm>
#include <cmath>

namespace p2::game {

f32 reward_multiplier(u32 encounter_rate) {
    if (encounter_rate >= 100) return 1.0f;
    if (encounter_rate == 0) return kMaxRewardMultiplier;
    return std::min(kMaxRewardMultiplier, 100.0f / f32(encounter_rate));
}

} // namespace p2::game

namespace p2 {

namespace {

// --- the game's random encounters --------------------------------------------------------------------------
// Each step in a dungeon, the field code calls the encounter roll with the step's encounter value in a0.
// The roll adds it to a counter (at 0x08EE22A4); once the counter is past 2048, every time it crosses a
// multiple of 512 the game rolls against the area's encounter table. Scaling the value scales how often
// that happens. A value of 32767 is the game forcing a battle (the counter is also set to 2048 then):
// left alone.
constexpr u32 kEncounterRoll = 0x089818A4;
constexpr u32 kForcedEncounter = 32767;
u32 g_step_remainder = 0; // hundredths carried over, so small values still add up at odd rates

void scale_encounter_step(Cpu* c) {
    const u32 rate = host::settings().encounter_rate;
    const s32 value = s32(c->r[4]);
    if (rate == 100 || value <= 0 || u32(value) >= kForcedEncounter) return;
    const u64 scaled = u64(value) * rate + g_step_remainder;
    c->r[4] = u32(std::min<u64>(scaled / 100, kForcedEncounter - 1));
    g_step_remainder = u32(scaled % 100);
}

// --- battle rewards ------------------------------------------------------------------------------------------
// The results screen's payout starts here. The battle's experience and money totals (summed while the
// enemies fell) are at 0x08F1CD8C and 0x08F1CD90; the function applies the game's own doubling and halving
// items to them, shows them and pays them out, so multiplying them here boosts all of that.
constexpr u32 kBattlePayout = 0x089790C8;
constexpr u32 kBattleExp = 0x08F1CD8C;
constexpr u32 kBattleMoney = 0x08F1CD90;
u32 g_boosted_exp = 0, g_boosted_money = 0; // what was last written, so a second call does not compound

void boost_rewards(u8* m) {
    const f32 multiplier = game::reward_multiplier(host::settings().encounter_rate);
    if (multiplier <= 1.0f) return;
    const u32 exp = rd32(m, kBattleExp), money = rd32(m, kBattleMoney);
    if ((exp | money) == 0 || (exp == g_boosted_exp && money == g_boosted_money)) return;
    auto boost = [&](u32 value) { return u32(std::min(std::lround(f64(value) * f64(multiplier)), 99999999L)); };
    g_boosted_exp = boost(exp);
    g_boosted_money = boost(money);
    wr32(m, kBattleExp, g_boosted_exp);
    wr32(m, kBattleMoney, g_boosted_money);
    LOG_INFO("tweaks", "battle rewards x%.2f: experience %u -> %u, money %u -> %u", f64(multiplier), exp, g_boosted_exp, money,
             g_boosted_money);
}

} // namespace

void game_hook(Cpu* c, u8* m, u32 pc) {
    switch (pc) {
    case kEncounterRoll: scale_encounter_step(c); break;
    case kBattlePayout: boost_rewards(m); break;
    default: break;
    }
}

} // namespace p2
