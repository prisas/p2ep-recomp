#pragma once

#include "runtime/ge/ge_state.h"
#include "runtime/ge/renderer.h"

// The display-list processor: executes the command lists the game submits and turns them into
// draw calls for the renderer.
namespace p2::ge {

void init(Renderer* renderer);
Renderer* renderer();
State& state();

// --- the sceGe interface ----------------------------------------------------------------------------------------

// Returns a list id (>= 0) or an error code.
s32 enqueue(u32 list_addr, u32 stall_addr, s32 callback_id, bool head);
s32 update_stall(s32 list_id, u32 stall_addr);
// mode 0: block until done (lists run to completion at submission, so this never waits);
// mode 1: report the list's status.
s32 list_sync(s32 list_id, u32 mode);
s32 draw_sync(u32 mode);
s32 set_callback(u32 signal_fn, u32 signal_arg, u32 finish_fn, u32 finish_arg);
s32 unset_callback(s32 id);

struct Stats {
    u64 lists = 0;
    u64 commands = 0;
    u64 draws = 0;
    u64 vertices = 0;
    u64 ticks = 0; // host time spent processing lists, in host_ticks() units
};

// A monotonic host clock for coarse profiling, and its rate.
u64 host_ticks();
u64 host_ticks_per_second();
Stats& stats();

// While on, every draw and block transfer is written to the log with the state that shapes it.
void set_draw_trace(bool on);

} // namespace p2::ge
