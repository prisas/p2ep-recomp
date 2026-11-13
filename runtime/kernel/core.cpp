#include "runtime/kernel/kernel.h"

#include "lib/common/file.h"
#include "lib/common/log.h"
#include "runtime/cpu/recomp.h"
#include "runtime/mem/memory.h"

#include <algorithm>
#include <map>
#include <unordered_map>

#include <windows.h>

namespace p2::kernel {

namespace {

constexpr s32 kMaxSlice = 20000;         // cycles; about 60 microseconds of guest time
constexpr u32 kReturnMagic = 0x08000010; // $ra of an entry point called by the host; never executed
constexpr size_t kFiberStackReserve = 16u << 20;
constexpr size_t kShadowDepth = 1u << 16;

// --- objects ---
std::unordered_map<Uid, std::unique_ptr<Object>> g_objects;
Uid g_next_uid = 0x101;

// --- time and events ---
struct Event {
    u32 handle;
    EventFn fn;
    u64 userdata;
};
s64 g_cycles = 0;
std::multimap<s64, Event> g_events;
std::unordered_map<u32, std::multimap<s64, Event>::iterator> g_event_index;
u32 g_next_event = 1;
bool g_processing_events = false;

// --- scheduler ---
Thread* g_current = nullptr;
Cpu* g_active_cpu = nullptr; // context currently executing guest code
s32 g_slice_start = 0;
std::vector<Thread*> g_ready;
s64 g_order_back = 0, g_order_front = 0;
void* g_idle_fiber = nullptr;
bool g_stop = false;
int g_exit_code = 0;
std::vector<void*> g_dead_fibers;
std::vector<Uid> g_dead_threads;

// --- interrupts ---
struct SubHandler {
    u32 handler = 0;
    u32 arg = 0;
    bool enabled = false;
};
std::map<std::pair<u32, u32>, SubHandler> g_sub_handlers;
int g_interrupt_depth = 0;
Cpu g_intr_cpu;
u32 g_intr_stack_top = 0;
std::vector<u32> g_intr_shadow;

ModuleInfo g_module;

struct Callback final : Object {
    static constexpr ObjType kType = ObjType::Callback;
    u32 entry = 0;
    u32 common = 0;
    Uid owner = 0;
    u32 notify_count = 0;
    u32 notify_arg = 0;
};

// Charges the cycles the active context has used since the last call to the global clock.
void sync_clock() {
    if (!g_active_cpu) return;
    g_cycles += g_slice_start - g_active_cpu->downcount;
    g_slice_start = g_active_cpu->downcount;
}

// Gives `cpu` a slice that ends no later than the next scheduled event.
void begin_slice(Cpu* cpu) {
    s64 slice = kMaxSlice;
    if (!g_events.empty()) slice = std::min<s64>(slice, g_events.begin()->first - g_cycles);
    if (slice < 1) slice = 1;
    cpu->downcount = s32(slice);
    g_slice_start = s32(slice);
    g_active_cpu = cpu;
}

void process_events() {
    if (g_processing_events) return;
    g_processing_events = true;
    while (!g_events.empty() && g_events.begin()->first <= g_cycles) {
        Event e = g_events.begin()->second;
        g_event_index.erase(e.handle);
        g_events.erase(g_events.begin());
        e.fn(e.userdata);
    }
    g_processing_events = false;
}

Thread* pick_ready() {
    Thread* best = nullptr;
    for (Thread* t : g_ready)
        if (!best || t->priority < best->priority || (t->priority == best->priority && t->ready_order < best->ready_order))
            best = t;
    return best;
}

void make_ready(Thread* t, bool front = false) {
    t->state = ThreadState::Ready;
    t->ready_order = u64(front ? --g_order_front : ++g_order_back);
    if (std::find(g_ready.begin(), g_ready.end(), t) == g_ready.end()) g_ready.push_back(t);
}

void remove_ready(Thread* t) { g_ready.erase(std::remove(g_ready.begin(), g_ready.end(), t), g_ready.end()); }

// Hands the host thread to the best ready guest thread, or to the scheduler loop when none is.
void switch_to_next() {
    sync_clock();
    g_active_cpu = nullptr;
    Thread* next = pick_ready();
    if (next) {
        remove_ready(next);
        next->state = ThreadState::Running;
        g_current = next;
        SwitchToFiber(next->fiber);
    } else {
        g_current = nullptr;
        SwitchToFiber(g_idle_fiber);
    }
}

// The calling guest thread stops running until something resumes its fiber.
void suspend_current() {
    Thread* self = g_current;
    switch_to_next();
    // Resumed: whoever switched here already made this thread current.
    g_current = self;
    self->state = ThreadState::Running;
    begin_slice(&self->cpu);
}

void wake_end_waiters(Thread* ended) {
    for (auto& [uid, object] : g_objects) {
        if (object->type != ObjType::Thread) continue;
        Thread* w = static_cast<Thread*>(object.get());
        if (w->wait_type == WaitType::ThreadEnd && w->wait_id == ended->uid) wake_thread(w, ended->exit_status);
    }
}

void timeout_event(u64 userdata) {
    Thread* t = get<Thread>(Uid(userdata >> 32));
    if (!t || t->wait_type == WaitType::None || t->wait_serial != u32(userdata)) return;
    t->wait_event = 0;
    if (t->wait_timeout_ptr && mem::valid(t->wait_timeout_ptr, 4)) mem::w32(t->wait_timeout_ptr, 0);
    // A plain delay ends normally; every other wait reports the timeout.
    wake_thread(t, t->wait_type == WaitType::Delay ? 0 : s32(kErrWaitTimeout));
}

void CALLBACK fiber_main(void* param) {
    Thread* t = static_cast<Thread*>(param);
    begin_slice(&t->cpu);
    fpu_apply_mode(&t->cpu);
    lookup_guest_fn(t->entry)(&t->cpu, mem::g_base);
    exit_current_thread(s32(t->cpu.r[kRegV0]), false); // returning from the entry point ends the thread
}

void release_thread_resources(Thread* t) {
    if (t->stack_base) {
        free_user(t->stack_base);
        t->stack_base = 0;
    }
}

void reap() {
    for (void* fiber : g_dead_fibers) DeleteFiber(fiber);
    g_dead_fibers.clear();
    for (Uid uid : g_dead_threads) {
        if (Thread* t = get<Thread>(uid)) {
            release_thread_resources(t);
            destroy_object(uid);
        }
    }
    g_dead_threads.clear();
}

} // namespace

// --- objects --------------------------------------------------------------------------------------------------

Uid add_object(std::unique_ptr<Object> object) {
    Uid uid = g_next_uid++;
    object->uid = uid;
    g_objects[uid] = std::move(object);
    return uid;
}

Object* find_object(Uid uid) {
    auto it = g_objects.find(uid);
    return it == g_objects.end() ? nullptr : it->second.get();
}

void destroy_object(Uid uid) { g_objects.erase(uid); }

// --- time -----------------------------------------------------------------------------------------------------

s64 now_cycles() {
    sync_clock();
    return g_cycles;
}

u64 now_us() { return u64(now_cycles() / kCyclesPerUs); }

u32 schedule_event(u64 delay_us, EventFn fn, u64 userdata) {
    sync_clock();
    u32 handle = g_next_event++;
    if (g_next_event == 0) g_next_event = 1;
    auto it = g_events.insert({g_cycles + s64(delay_us) * kCyclesPerUs, Event{handle, fn, userdata}});
    g_event_index[handle] = it;
    if (g_active_cpu) begin_slice(g_active_cpu); // the running slice may now have to end sooner
    return handle;
}

void cancel_event(u32 handle) {
    auto it = g_event_index.find(handle);
    if (it == g_event_index.end()) return;
    g_events.erase(it->second);
    g_event_index.erase(it);
}

// --- threads --------------------------------------------------------------------------------------------------

Thread* current_thread() { return g_interrupt_depth ? nullptr : g_current; }
bool in_interrupt() { return g_interrupt_depth != 0; }

Uid create_thread(const std::string& name, u32 entry, s32 priority, u32 stack_size, u32 attr, u32* error) {
    stack_size = align_up<u32>(stack_size, 0x100);
    if (stack_size < 0x1000) stack_size = 0x1000;
    u32 stack = alloc_user("stack/" + name, AllocType::High, stack_size);
    if (!stack) {
        if (error) *error = kErrNoMemory;
        return 0;
    }
    auto t = std::make_unique<Thread>();
    t->type = ObjType::Thread;
    t->name = name;
    t->entry = entry;
    t->attr = attr;
    t->init_priority = t->priority = priority;
    t->stack_base = stack;
    t->stack_size = stack_size;
    t->cpu.reset();
    t->shadow_stack.resize(kShadowDepth);
    Thread* raw = t.get();
    Uid uid = add_object(std::move(t));
    raw->cpu.thread = raw;
    if (error) *error = 0;
    return uid;
}

u32 start_thread(Uid uid, u32 arg_size, u32 arg_ptr) {
    Thread* t = get<Thread>(uid);
    if (!t) return kErrUnknownThid;
    if (t->state != ThreadState::Dormant) return kErrNotDormant;

    Cpu& c = t->cpu;
    c.reset();
    c.thread = t;
    c.shadow_base = c.shadow_sp = t->shadow_stack.data();

    const u32 top = t->stack_base + t->stack_size;
    if (!(t->attr & kThreadAttrNoFillStack)) std::memset(mem::ptr(t->stack_base), 0xFF, t->stack_size);
    // The top 256 bytes hold a small control block the firmware keeps there; $k0 points at it.
    u32 sp = top - 0x100;
    std::memset(mem::ptr(sp), 0, 0x100);
    c.r[kRegK0] = sp;
    mem::w32(sp + 0xC0, u32(uid));
    mem::w32(sp + 0xC8, sp);
    mem::w32(sp + 0xF8, 0xFFFFFFFFu);
    mem::w32(sp + 0xFC, 0xFFFFFFFFu);
    mem::w32(t->stack_base, u32(uid));

    if (arg_size && arg_ptr && mem::valid(arg_ptr, arg_size)) {
        sp -= align_up<u32>(arg_size, 16);
        std::memmove(mem::ptr(sp), mem::ptr(arg_ptr), arg_size);
        c.r[kRegA0] = arg_size;
        c.r[kRegA1] = sp;
    }
    sp -= 64;
    c.r[kRegSp] = sp;
    c.r[kRegFp] = sp;
    c.r[kRegGp] = g_module.gp;
    c.r[kRegRa] = kReturnMagic;
    c.pc = t->entry;

    t->priority = t->init_priority;
    t->exit_status = s32(kErrNotDormant);
    t->wait_type = WaitType::None;
    t->pending_callbacks.clear();
    t->fiber = CreateFiberEx(64 << 10, kFiberStackReserve, FIBER_FLAG_FLOAT_SWITCH, fiber_main, t);
    if (!t->fiber) P2_FATAL("cannot create a fiber for thread '%s'", t->name.c_str());
    make_ready(t);
    reschedule();
    return kOk;
}

void exit_current_thread(s32 status, bool remove) {
    Thread* t = g_current;
    P2_ASSERT(t != nullptr && g_interrupt_depth == 0);
    LOG_CH("thread", "thread %d '%s' exits with %d%s", t->uid, t->name.c_str(), status, remove ? " (deleted)" : "");
    t->exit_status = status;
    t->state = ThreadState::Dormant;
    t->wait_type = WaitType::None;
    wake_end_waiters(t);
    g_dead_fibers.push_back(t->fiber);
    t->fiber = nullptr;
    if (remove) {
        t->state = ThreadState::Dead;
        g_dead_threads.push_back(t->uid);
    }
    switch_to_next();
    P2_FATAL("a finished thread was resumed");
}

u32 terminate_thread(Uid uid, bool remove) {
    Thread* t = get<Thread>(uid);
    if (!t) return kErrUnknownThid;
    if (t == g_current) exit_current_thread(s32(kErrThreadTerminated), remove);

    if (t->state != ThreadState::Dormant) {
        if (t->wait_event) cancel_event(t->wait_event);
        t->wait_event = 0;
        t->wait_type = WaitType::None;
        remove_ready(t);
        t->state = ThreadState::Dormant;
        t->exit_status = s32(kErrThreadTerminated);
        wake_end_waiters(t);
        if (t->fiber) g_dead_fibers.push_back(t->fiber);
        t->fiber = nullptr;
    }
    if (remove) {
        t->state = ThreadState::Dead;
        g_dead_threads.push_back(uid);
    }
    return kOk;
}

u32 delete_thread(Uid uid) {
    Thread* t = get<Thread>(uid);
    if (!t) return kErrUnknownThid;
    if (t->state != ThreadState::Dormant) return kErrNotDormant;
    t->state = ThreadState::Dead;
    g_dead_threads.push_back(uid);
    return kOk;
}

u32 change_priority(Uid uid, s32 priority) {
    Thread* t = uid == 0 ? g_current : get<Thread>(uid);
    if (!t) return kErrUnknownThid;
    if (priority < 0x08 || priority > 0x77) return kErrIllegalPriority;
    if (t->state == ThreadState::Dormant) return kErrDormant;
    t->priority = priority;
    if (t->state == ThreadState::Ready) make_ready(t); // re-queue at the back of the new level
    reschedule();
    return kOk;
}

namespace {

s32 wait_impl(WaitType type, Uid id, u32 timeout_ptr, bool timed, u64 timeout_us, bool callbacks) {
    Thread* t = g_current;
    if (!t || g_interrupt_depth) {
        LOG_ERROR("kernel", "a blocking call was made outside a thread context");
        return s32(kErrIllegalContext);
    }
    t->wait_type = type;
    t->wait_id = id;
    t->wait_timeout_ptr = timeout_ptr;
    t->wait_callbacks = callbacks;
    t->wait_result = 0;
    t->wait_event = 0;
    ++t->wait_serial;
    if (timed) t->wait_event = schedule_event(timeout_us, timeout_event, (u64(u32(t->uid)) << 32) | t->wait_serial);

    while (true) {
        if (callbacks && !t->pending_callbacks.empty()) {
            run_pending_callbacks();
            if (t->wait_type == WaitType::None) break; // the wait was satisfied meanwhile
        }
        t->state = ThreadState::Waiting;
        suspend_current();
        if (t->wait_type == WaitType::None) break;
        // Otherwise the thread was resumed only to run callbacks: loop and wait again.
    }
    return t->wait_result;
}

} // namespace

s32 wait_current(WaitType type, Uid id, u32 timeout_ptr, bool callbacks) {
    const bool timed = timeout_ptr && mem::valid(timeout_ptr, 4);
    return wait_impl(type, id, timed ? timeout_ptr : 0, timed, timed ? mem::r32(timeout_ptr) : 0, callbacks);
}

s32 wait_current_for(WaitType type, Uid id, u64 timeout_us, bool callbacks) {
    return wait_impl(type, id, 0, true, timeout_us, callbacks);
}

bool wake_thread(Thread* t, s32 result) {
    if (t->wait_type == WaitType::None) return false;
    if (t->wait_event) {
        cancel_event(t->wait_event);
        t->wait_event = 0;
    }
    t->wait_type = WaitType::None;
    t->wait_result = result;
    if (t->state == ThreadState::Waiting) make_ready(t);
    return true;
}

void reschedule() {
    if (g_interrupt_depth || !g_current || g_current->state != ThreadState::Running) return;
    Thread* best = pick_ready();
    if (!best || best->priority >= g_current->priority) return;
    make_ready(g_current, true); // a preempted thread keeps its place at the head of its level
    suspend_current();
}

void yield_current() {
    if (g_interrupt_depth || !g_current) return;
    Thread* best = pick_ready();
    if (!best || best->priority > g_current->priority) return;
    make_ready(g_current);
    suspend_current();
}

void on_slice_end(Cpu* cpu) {
    sync_clock();
    if (g_interrupt_depth) { // handlers run to completion; just keep the clock moving
        begin_slice(cpu);
        return;
    }
    process_events();
    if (g_stop && g_current) { // an event asked to quit: park this thread and return to the scheduler loop
        g_active_cpu = nullptr;
        g_current = nullptr;
        SwitchToFiber(g_idle_fiber);
    }
    reschedule();
    begin_slice(cpu);
}

void run() {
    while (!g_stop) {
        reap();
        if (Thread* next = pick_ready()) {
            remove_ready(next);
            next->state = ThreadState::Running;
            g_current = next;
            SwitchToFiber(next->fiber);
            g_current = nullptr;
            g_active_cpu = nullptr;
            continue;
        }
        if (g_events.empty()) {
            LOG_ERROR("kernel", "every thread is waiting and nothing is scheduled: the guest is deadlocked");
            dump_state();
            g_exit_code = 3;
            break;
        }
        g_cycles = std::max(g_cycles, g_events.begin()->first);
        process_events();
    }
    reap();
}

void request_stop(int code) {
    g_stop = true;
    g_exit_code = code;
    if (g_current && !g_interrupt_depth && GetCurrentFiber() != g_idle_fiber) {
        // Called from a guest thread: leave it suspended forever and return to the scheduler loop.
        sync_clock();
        g_active_cpu = nullptr;
        g_current = nullptr;
        SwitchToFiber(g_idle_fiber);
    }
}

bool stop_requested() { return g_stop; }
int exit_code() { return g_exit_code; }

// --- guest calls from host code ---------------------------------------------------------------------------------

u32 call_guest_on_thread(Thread* t, u32 entry, u32 a0, u32 a1, u32 a2) {
    Cpu saved = t->cpu;
    Cpu& c = t->cpu;
    c.r[kRegA0] = a0;
    c.r[kRegA1] = a1;
    c.r[kRegA2] = a2;
    c.r[kRegRa] = kReturnMagic;
    c.r[kRegSp] = (c.r[kRegSp] - 0x100) & ~15u;
    c.r[kRegGp] = g_module.gp;
    c.sec_entry = 0;
    lookup_guest_fn(entry)(&c, mem::g_base);
    u32 result = c.r[kRegV0];
    sync_clock();
    saved.downcount = c.downcount;
    saved.shadow_sp = c.shadow_sp;
    t->cpu = saved;
    return result;
}

u32 call_guest_interrupt(u32 entry, u32 a0, u32 a1, u32 a2) {
    sync_clock();
    Cpu* outer = g_active_cpu;
    const Cpu saved = g_intr_cpu; // supports nesting
    const bool nested = g_interrupt_depth != 0;

    Cpu& c = g_intr_cpu;
    if (!nested) {
        c.reset();
        c.shadow_base = c.shadow_sp = g_intr_shadow.data();
        c.r[kRegSp] = g_intr_stack_top - 64;
    } else {
        c.r[kRegSp] = (c.r[kRegSp] - 0x100) & ~15u;
    }
    c.r[kRegA0] = a0;
    c.r[kRegA1] = a1;
    c.r[kRegA2] = a2;
    c.r[kRegGp] = g_module.gp;
    c.r[kRegRa] = kReturnMagic;
    c.sec_entry = 0;

    ++g_interrupt_depth;
    begin_slice(&c);
    lookup_guest_fn(entry)(&c, mem::g_base);
    sync_clock();
    --g_interrupt_depth;

    u32 result = c.r[kRegV0];
    if (nested) g_intr_cpu = saved;
    g_active_cpu = nullptr;
    if (outer) begin_slice(outer);
    return result;
}

// --- callbacks ------------------------------------------------------------------------------------------------

Uid create_callback(const std::string& name, u32 entry, u32 common_arg) {
    auto cb = std::make_unique<Callback>();
    cb->type = ObjType::Callback;
    cb->name = name;
    cb->entry = entry;
    cb->common = common_arg;
    cb->owner = g_current ? g_current->uid : 0;
    return add_object(std::move(cb));
}

u32 delete_callback(Uid uid) {
    Callback* cb = get<Callback>(uid);
    if (!cb) return kErrUnknownCbid;
    if (Thread* owner = get<Thread>(cb->owner)) {
        auto& pending = owner->pending_callbacks;
        pending.erase(std::remove(pending.begin(), pending.end(), uid), pending.end());
    }
    destroy_object(uid);
    return kOk;
}

u32 notify_callback(Uid uid, u32 arg) {
    Callback* cb = get<Callback>(uid);
    if (!cb) return kErrUnknownCbid;
    Thread* owner = get<Thread>(cb->owner);
    if (!owner) return kErrUnknownThid;
    ++cb->notify_count;
    cb->notify_arg = arg;
    auto& pending = owner->pending_callbacks;
    if (std::find(pending.begin(), pending.end(), uid) == pending.end()) pending.push_back(uid);
    // A thread in a callback-aware wait wakes up just long enough to run it.
    if (owner->state == ThreadState::Waiting && owner->wait_callbacks) make_ready(owner);
    return kOk;
}

bool run_pending_callbacks() {
    Thread* t = g_current;
    if (!t || g_interrupt_depth) return false;
    bool ran = false;
    while (!t->pending_callbacks.empty()) {
        Uid uid = t->pending_callbacks.front();
        t->pending_callbacks.erase(t->pending_callbacks.begin());
        Callback* cb = get<Callback>(uid);
        if (!cb) continue;
        u32 count = cb->notify_count, arg = cb->notify_arg, entry = cb->entry, common = cb->common;
        cb->notify_count = 0;
        cb->notify_arg = 0;
        LOG_CH("thread", "callback %d '%s' (%u, %08x)", uid, cb->name.c_str(), count, arg);
        u32 result = call_guest_on_thread(t, entry, count, arg, common);
        if (result != 0) delete_callback(uid); // a callback that returns non-zero unregisters itself
        ran = true;
    }
    return ran;
}

// --- interrupts -----------------------------------------------------------------------------------------------

u32 register_sub_interrupt(u32 intr, u32 sub, u32 handler, u32 arg) {
    SubHandler& h = g_sub_handlers[{intr, sub}];
    h.handler = handler;
    h.arg = arg;
    h.enabled = false;
    return kOk;
}

u32 release_sub_interrupt(u32 intr, u32 sub) {
    g_sub_handlers.erase({intr, sub});
    return kOk;
}

u32 enable_sub_interrupt(u32 intr, u32 sub, bool enable) {
    auto it = g_sub_handlers.find({intr, sub});
    if (it == g_sub_handlers.end()) {
        if (!enable) return kOk;
        it = g_sub_handlers.insert({{intr, sub}, SubHandler{}}).first;
    }
    it->second.enabled = enable;
    return kOk;
}

void trigger_interrupt(u32 intr) {
    // Copy first: a handler may register or release handlers.
    std::vector<std::pair<u32, SubHandler>> handlers;
    for (const auto& [key, h] : g_sub_handlers)
        if (key.first == intr && h.enabled && h.handler) handlers.push_back({key.second, h});
    for (const auto& [sub, h] : handlers) call_guest_interrupt(h.handler, sub, h.arg);
}

// --- start-up and diagnostics -----------------------------------------------------------------------------------

ModuleInfo& module() { return g_module; }

bool init(std::string* error) {
    g_idle_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!g_idle_fiber) {
        if (error) *error = "cannot convert the main thread to a fiber";
        return false;
    }
    g_intr_shadow.resize(kShadowDepth);
    g_intr_cpu.reset();
    u32 stack = alloc_kernel("interrupt stack", 0x8000);
    g_intr_stack_top = stack + 0x8000;
    return true;
}

std::string describe_current() {
    Thread* t = g_current;
    if (!t) return "no guest thread";
    std::string out = format("thread %d '%s' ra %08x sp %08x", t->uid, t->name.c_str(), t->cpu.r[kRegRa], t->cpu.r[kRegSp]);
#if defined(P2EP_DEV)
    if (t->cpu.shadow_base && t->cpu.shadow_sp > t->cpu.shadow_base) {
        out += ", return addresses";
        const size_t depth = size_t(t->cpu.shadow_sp - t->cpu.shadow_base);
        for (size_t k = depth; k > 0 && depth - k < 12; --k) append_format(out, " %08x", t->cpu.shadow_base[k - 1]);
    }
#endif
    return out;
}

void dump_state() {
    static const char* const states[] = {"dormant", "ready", "running", "waiting", "dead"};
    LOG_ERROR("kernel", "guest time %.3f s, %zu thread(s) ready", double(g_cycles) / double(kCyclesPerUs) / 1e6,
              g_ready.size());
    for (auto& [uid, object] : g_objects) {
        if (object->type != ObjType::Thread) continue;
        Thread* t = static_cast<Thread*>(object.get());
        LOG_ERROR("kernel", "  thread %d '%s'%s: %s, priority %d, wait type %d on %d, ra %08x sp %08x", uid,
                  t->name.c_str(), t == g_current ? " (current)" : "", states[size_t(t->state)], t->priority,
                  int(t->wait_type), t->wait_id, t->cpu.r[kRegRa], t->cpu.r[kRegSp]);
#if defined(P2EP_DEV)
        if (t->cpu.shadow_base && t->cpu.shadow_sp > t->cpu.shadow_base) {
            std::string chain;
            size_t depth = size_t(t->cpu.shadow_sp - t->cpu.shadow_base);
            for (size_t k = depth; k > 0 && depth - k < 24; --k) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), " %08x", t->cpu.shadow_base[k - 1]);
                chain += buf;
            }
            LOG_ERROR("kernel", "    return addresses (innermost first):%s", chain.c_str());
        }
#endif
    }
}

} // namespace p2::kernel
