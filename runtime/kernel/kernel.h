#pragma once

#include "runtime/cpu/cpu.h"

#include <memory>
#include <string>
#include <vector>

// The part of the PSP kernel the game relies on: threads, the scheduler, the clock and the
// primitives everything else waits on. All of it runs on one host thread; every guest thread is a
// fiber, so a blocking system call is simply a switch to another fiber.
namespace p2::kernel {

using Uid = s32;

// --- error codes ----------------------------------------------------------------------------------------------
enum : u32 {
    kOk = 0,
    kErrError = 0x80020001,
    kErrIllegalContext = 0x80020064,
    kErrIllegalPerm = 0x800200D1,
    kErrIllegalArgument = 0x800200D2,
    kErrIllegalAddr = 0x800200D3,
    kErrNoMemory = 0x80020190,
    kErrIllegalAttr = 0x80020191,
    kErrIllegalEntry = 0x80020192,
    kErrIllegalPriority = 0x80020193,
    kErrIllegalStackSize = 0x80020194,
    kErrIllegalMode = 0x80020195,
    kErrIllegalThid = 0x80020197,
    kErrUnknownThid = 0x80020198,
    kErrUnknownSemid = 0x80020199,
    kErrUnknownEvfid = 0x8002019A,
    kErrUnknownCbid = 0x800201A1,
    kErrDormant = 0x800201A2,
    kErrNotDormant = 0x800201A4,
    kErrCanNotWait = 0x800201A7,
    kErrWaitTimeout = 0x800201A8,
    kErrWaitCancel = 0x800201A9,
    kErrReleaseWait = 0x800201AA,
    kErrThreadTerminated = 0x800201AC,
    kErrSemaZero = 0x800201AD,
    kErrSemaOvf = 0x800201AE,
    kErrEvfCond = 0x800201AF,
    kErrEvfMulti = 0x800201B0,
    kErrEvfIlpat = 0x800201B1,
    kErrWaitDelete = 0x800201B5,
    kErrIllegalMemblock = 0x800201B6,
    kErrIllegalMemsize = 0x800201B7,
    kErrIllegalCount = 0x800201BD,
    kErrLwMutexNotFound = 0x800201CA,
    kErrLwMutexLocked = 0x800201CB,
    kErrLwMutexUnlocked = 0x800201CC,
    kErrLwMutexLockOvf = 0x800201CD,
    kErrLwMutexUnlockUdf = 0x800201CE,
    kErrLwMutexRecursive = 0x800201CF,
    kErrUnknownUid = 0x800200CB,
};

// --- objects --------------------------------------------------------------------------------------------------

enum class ObjType : u8 { Thread, Sema, EventFlag, LwMutex, Callback, MemBlock, Module, Other };

struct Object {
    virtual ~Object() = default;
    ObjType type = ObjType::Other;
    Uid uid = 0;
    std::string name;
};

Uid add_object(std::unique_ptr<Object> object);
Object* find_object(Uid uid);
void destroy_object(Uid uid);

template <class T>
T* get(Uid uid) {
    Object* o = find_object(uid);
    return o && o->type == T::kType ? static_cast<T*>(o) : nullptr;
}

// --- time and events ------------------------------------------------------------------------------------------

constexpr s64 kCyclesPerUs = 333; // one guest instruction is charged as one cycle of a 333 MHz clock

u64 now_us();
s64 now_cycles();

using EventFn = void (*)(u64 userdata);
// Calls `fn` on the scheduler once `delay_us` of guest time has passed. Returns a handle for cancel_event.
u32 schedule_event(u64 delay_us, EventFn fn, u64 userdata);
void cancel_event(u32 handle);

// --- threads --------------------------------------------------------------------------------------------------

enum class ThreadState : u8 { Dormant, Ready, Running, Waiting, Dead };

enum class WaitType : u8 {
    None, Sleep, Delay, Sema, EventFlag, ThreadEnd = 9, Vblank, Audio, Io, Umd, Ge, LwMutex, Dialog,
};

enum : u32 {
    kThreadAttrUser = 0x80000000,
    kThreadAttrVfpu = 0x00004000,
    kThreadAttrNoFillStack = 0x00100000,
    kThreadAttrClearStack = 0x00200000,
};

struct Thread final : Object {
    static constexpr ObjType kType = ObjType::Thread;

    Cpu cpu;
    ThreadState state = ThreadState::Dormant;
    u32 entry = 0;
    u32 attr = 0;
    s32 init_priority = 0;
    s32 priority = 0;
    u32 stack_base = 0;
    u32 stack_size = 0;
    Uid stack_block = 0;
    s32 exit_status = s32(kErrDormant);

    // Current wait, if any. The three parameters mean whatever the wait type needs.
    WaitType wait_type = WaitType::None;
    Uid wait_id = 0;
    u32 wait_param[3] = {};
    u32 wait_timeout_ptr = 0; // guest address of the caller's timeout variable
    u32 wait_event = 0;       // pending timeout event
    u32 wait_serial = 0;      // distinguishes this wait from earlier ones in timeout events
    s32 wait_result = 0;
    bool wait_callbacks = false;

    std::vector<Uid> pending_callbacks;
    s32 slice_start = 0; // downcount value when the running slice began
    u64 ready_order = 0; // FIFO position among threads of equal priority
    void* fiber = nullptr;
    std::vector<u32> shadow_stack;
};

bool init(std::string* error);

// Null while the scheduler itself or an interrupt handler runs.
Thread* current_thread();

Uid create_thread(const std::string& name, u32 entry, s32 priority, u32 stack_size, u32 attr, u32* error);
u32 start_thread(Uid uid, u32 arg_size, u32 arg_ptr);
[[noreturn]] void exit_current_thread(s32 status, bool remove);
u32 terminate_thread(Uid uid, bool remove);
u32 delete_thread(Uid uid);
u32 change_priority(Uid uid, s32 priority);

// Blocks the calling guest thread until wake_thread() or the timeout. Returns the wait result.
// `timeout_ptr` is the guest address of a microsecond count (0: wait forever). With `callbacks`,
// pending guest callbacks run before and during the wait.
s32 wait_current(WaitType type, Uid id, u32 timeout_ptr, bool callbacks);
// Same, with the timeout given directly. A WaitType::Delay that times out returns 0.
s32 wait_current_for(WaitType type, Uid id, u64 timeout_us, bool callbacks);
// Ends a thread's wait with `result`. Returns false when it was not waiting.
bool wake_thread(Thread* thread, s32 result);
// Lets a higher-priority ready thread take over. Safe to call anywhere; deferred inside interrupts.
void reschedule();

// Called by recompiled code when its time slice is used up: delivers due events and lets other
// threads run.
void on_slice_end(Cpu* cpu);

// Runs the scheduler until the guest asks to quit or `stop` is requested.
void run();
void request_stop(int exit_code);
int exit_code();

// --- guest calls from host code ---------------------------------------------------------------------------------

// Calls a guest function on the current thread's context, preserving its registers (used for
// callbacks). Returns the function's v0.
u32 call_guest_on_thread(Thread* thread, u32 entry, u32 a0, u32 a1 = 0, u32 a2 = 0);
// Calls a guest function in interrupt context, on the dedicated interrupt stack.
u32 call_guest_interrupt(u32 entry, u32 a0, u32 a1 = 0, u32 a2 = 0);

// --- callbacks ------------------------------------------------------------------------------------------------

Uid create_callback(const std::string& name, u32 entry, u32 common_arg);
u32 delete_callback(Uid uid);
// Runs the pending callbacks of the current thread. Returns true if any ran.
bool run_pending_callbacks();

// --- interrupts -----------------------------------------------------------------------------------------------

constexpr u32 kIntrVblank = 30;

u32 register_sub_interrupt(u32 intr, u32 sub, u32 handler, u32 arg);
u32 release_sub_interrupt(u32 intr, u32 sub);
u32 enable_sub_interrupt(u32 intr, u32 sub, bool enable);
// Runs every enabled handler of the interrupt line.
void trigger_interrupt(u32 intr);

// --- memory partitions ----------------------------------------------------------------------------------------

enum class AllocType : u32 { Low = 0, High = 1, Addr = 2, LowAligned = 3, HighAligned = 4 };

// Returns the block's address, or 0 when the request cannot be satisfied.
u32 alloc_user(const std::string& name, AllocType type, u32 size, u32 addr_or_align = 0);
u32 alloc_kernel(const std::string& name, u32 size);
bool free_user(u32 addr);
u32 user_max_free();
u32 user_total_free();
void reserve_user(u32 start, u32 end, const std::string& name);

struct MemBlock final : Object {
    static constexpr ObjType kType = ObjType::MemBlock;
    u32 addr = 0;
    u32 size = 0;
};

// --- module ---------------------------------------------------------------------------------------------------

struct ModuleInfo {
    Uid uid = 0;
    std::string name;
    u32 base = 0;
    u32 end = 0;
    u32 gp = 0;
    u32 sdk_version = 0;
    u32 compiler_version = 0;
};
ModuleInfo& module();

// Dumps threads and their guest call stacks to the log (used on fatal errors).
void dump_state();
// One line naming the running thread and, in development builds, its guest return addresses.
std::string describe_current();

} // namespace p2::kernel
