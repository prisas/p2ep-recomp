#pragma once

#include "runtime/cpu/cpu.h"

namespace p2 {

// Fills the dispatch table from the generated entry list. Call once before any guest code runs.
void init_dispatch();

// The recompiled function at a guest address (a reporting stub when there is none).
GuestFn guest_function(u32 addr);

// Re-reads whether system calls are to be traced, after the set of enabled log channels changed.
void refresh_hle_trace();

} // namespace p2
