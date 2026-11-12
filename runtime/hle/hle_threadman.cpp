// ThreadManForUser and the thread-related parts of Kernel_Library: threads, semaphores, event
// flags, lightweight mutexes, callbacks and the system clock.

#include "runtime/hle/hle.h"

#include <algorithm>

using namespace p2;
using namespace p2::kernel;

namespace {

struct Waiter {
    Uid thread;
    u32 serial;
};

// True while the recorded waiter is still blocked in the same wait on `object`.
Thread* live_waiter(const Waiter& w, WaitType type, Uid object) {
    Thread* t = get<Thread>(w.thread);
    if (!t || t->wait_type != type || t->wait_id != object || t->wait_serial != w.serial) return nullptr;
    return t;
}

void add_waiter(std::vector<Waiter>& list, Thread* t, bool by_priority) {
    Waiter w{t->uid, t->wait_serial + 1}; // wait_current() increments the serial when the wait begins
    if (by_priority) {
        auto pos = std::find_if(list.begin(), list.end(), [&](const Waiter& other) {
            Thread* o = get<Thread>(other.thread);
            return o && o->priority > t->priority;
        });
        list.insert(pos, w);
    } else {
        list.push_back(w);
    }
}

struct Sema final : Object {
    static constexpr ObjType kType = ObjType::Sema;
    u32 attr = 0;
    s32 init_count = 0;
    s32 count = 0;
    s32 max_count = 0;
    std::vector<Waiter> waiters;
};

struct EventFlag final : Object {
    static constexpr ObjType kType = ObjType::EventFlag;
    u32 attr = 0;
    u32 init_pattern = 0;
    u32 pattern = 0;
    std::vector<Waiter> waiters;
};

struct LwMutex final : Object {
    static constexpr ObjType kType = ObjType::LwMutex;
    u32 attr = 0;
    u32 work_area = 0;
    std::vector<Waiter> waiters;
};

constexpr u32 kAttrPriorityOrder = 0x100;
constexpr u32 kEvfMultiWait = 0x200;
constexpr u32 kLwMutexRecursive = 0x200;
constexpr u32 kEvfWaitOr = 0x01, kEvfWaitClearAll = 0x10, kEvfWaitClearMatched = 0x20;

// Wakes as many semaphore waiters as the count allows, in queue order.
void sema_dispatch(Sema* s) {
    for (size_t i = 0; i < s->waiters.size();) {
        Thread* t = live_waiter(s->waiters[i], WaitType::Sema, s->uid);
        if (!t) {
            s->waiters.erase(s->waiters.begin() + ptrdiff_t(i));
            continue;
        }
        s32 need = s32(t->wait_param[0]);
        if (s->count < need) break; // strict queue order: later waiters do not overtake
        s->count -= need;
        s->waiters.erase(s->waiters.begin() + ptrdiff_t(i));
        wake_thread(t, 0);
    }
}

bool evf_matches(u32 pattern, u32 bits, u32 mode) {
    return (mode & kEvfWaitOr) ? (pattern & bits) != 0 : (pattern & bits) == bits;
}

void evf_consume(EventFlag* e, u32 bits, u32 mode) {
    if (mode & kEvfWaitClearAll) e->pattern = 0;
    else if (mode & kEvfWaitClearMatched) e->pattern &= ~bits;
}

void evf_dispatch(EventFlag* e) {
    for (size_t i = 0; i < e->waiters.size();) {
        Thread* t = live_waiter(e->waiters[i], WaitType::EventFlag, e->uid);
        if (!t) {
            e->waiters.erase(e->waiters.begin() + ptrdiff_t(i));
            continue;
        }
        u32 bits = t->wait_param[0], mode = t->wait_param[1], out = t->wait_param[2];
        if (!evf_matches(e->pattern, bits, mode)) {
            ++i;
            continue;
        }
        if (out && mem::valid(out, 4)) mem::w32(out, e->pattern);
        evf_consume(e, bits, mode);
        e->waiters.erase(e->waiters.begin() + ptrdiff_t(i));
        wake_thread(t, 0);
    }
}

template <class T>
void wake_all_deleted(T* object, WaitType type) {
    for (const Waiter& w : object->waiters)
        if (Thread* t = live_waiter(w, type, object->uid)) wake_thread(t, s32(kErrWaitDelete));
    object->waiters.clear();
}

u32 delay_us(u32 requested) { return requested < 200 ? 210 : requested; }

} // namespace

// --- threads --------------------------------------------------------------------------------------------------

HLE(sceKernelCreateThread) {
    std::string name = mem::read_string(ARG(0), 31);
    u32 entry = ARG(1), stack_size = ARG(3), attr = ARG(4);
    s32 priority = s32(ARG(2));
    if (!mem::valid(entry, 4)) RET(kErrIllegalEntry);
    if (priority < 0x08 || priority > 0x77) RET(kErrIllegalPriority);
    if (stack_size < 0x200) RET(kErrIllegalStackSize);
    u32 error = 0;
    Uid uid = create_thread(name, entry, priority, stack_size, attr, &error);
    LOG_CH("thread", "create thread '%s' entry %08x priority %d stack %x attr %08x -> %d", name.c_str(), entry,
           priority, stack_size, attr, uid);
    RET(uid ? u32(uid) : error);
}

HLE(sceKernelStartThread) { RET(start_thread(Uid(ARG(0)), ARG(1), ARG(2))); }
HLE(sceKernelDeleteThread) { RET(delete_thread(Uid(ARG(0)))); }
HLE(sceKernelExitThread) { exit_current_thread(s32(ARG(0)), false); }
HLE(sceKernelExitDeleteThread) { exit_current_thread(s32(ARG(0)), true); }
HLE(sceKernelTerminateDeleteThread) { RET(terminate_thread(Uid(ARG(0)), true)); }

static u32 wait_thread_end(Cpu* c, bool callbacks) {
    Thread* target = get<Thread>(Uid(ARG(0)));
    if (!target) return kErrUnknownThid;
    if (target == current_thread()) return kErrIllegalThid;
    if (target->state == ThreadState::Dormant || target->state == ThreadState::Dead) return u32(target->exit_status);
    return u32(wait_current(WaitType::ThreadEnd, target->uid, ARG(1), callbacks));
}

HLE(sceKernelWaitThreadEnd) { RET(wait_thread_end(c, false)); }
HLE(sceKernelWaitThreadEndCB) { RET(wait_thread_end(c, true)); }

static u32 delay_thread(Cpu* c, bool callbacks) {
    if (!current_thread()) return kErrIllegalContext;
    wait_current_for(WaitType::Delay, 0, delay_us(ARG(0)), callbacks);
    return 0;
}

HLE(sceKernelDelayThread) { RET(delay_thread(c, false)); }
HLE(sceKernelDelayThreadCB) { RET(delay_thread(c, true)); }

HLE(sceKernelChangeCurrentThreadAttr) {
    if (Thread* t = current_thread()) t->attr = (t->attr & ~ARG(0)) | ARG(1);
    RET(0);
}

HLE(sceKernelChangeThreadPriority) { RET(change_priority(Uid(ARG(0)), s32(ARG(1)))); }

HLE(sceKernelGetThreadCurrentPriority) {
    Thread* t = current_thread();
    RET(t ? u32(t->priority) : 0);
}

HLE(sceKernelGetThreadId) {
    Thread* t = current_thread();
    RET(t ? u32(t->uid) : 0);
}

HLE(sceKernelReferThreadStatus) {
    Thread* t = ARG(0) == 0 ? current_thread() : get<Thread>(Uid(ARG(0)));
    u32 out = ARG(1);
    if (!t) RET(kErrUnknownThid);
    if (!mem::valid(out, 4)) RET(kErrIllegalAddr);
    u32 size = mem::r32(out);
    u8 info[104] = {};
    store_le<u32>(info, 104);
    std::memcpy(info + 4, t->name.c_str(), std::min<size_t>(t->name.size(), 31));
    store_le<u32>(info + 36, t->attr);
    u32 status = 16; // dormant
    switch (t->state) {
    case ThreadState::Running: status = 1; break;
    case ThreadState::Ready: status = 2; break;
    case ThreadState::Waiting: status = 4; break;
    default: break;
    }
    store_le<u32>(info + 40, status);
    store_le<u32>(info + 44, t->entry);
    store_le<u32>(info + 48, t->stack_base);
    store_le<u32>(info + 52, t->stack_size);
    store_le<u32>(info + 56, module().gp);
    store_le<u32>(info + 60, u32(t->init_priority));
    store_le<u32>(info + 64, u32(t->priority));
    store_le<u32>(info + 68, u32(t->wait_type));
    store_le<u32>(info + 72, u32(t->wait_id));
    store_le<u32>(info + 80, u32(t->exit_status));
    u32 copy = std::min<u32>(size ? size : 104, 104);
    if (!mem::valid(out, copy)) RET(kErrIllegalAddr);
    std::memcpy(mem::ptr(out), info, copy);
    if (copy >= 4) mem::w32(out, copy);
    RET(0);
}

// --- semaphores -----------------------------------------------------------------------------------------------

HLE(sceKernelCreateSema) {
    auto s = std::make_unique<Sema>();
    s->type = ObjType::Sema;
    s->name = mem::read_string(ARG(0), 31);
    s->attr = ARG(1);
    s->init_count = s->count = s32(ARG(2));
    s->max_count = s32(ARG(3));
    if (s->count < 0 || s->max_count <= 0 || s->count > s->max_count) RET(kErrIllegalCount);
    Uid uid = add_object(std::move(s));
    LOG_CH("thread", "create sema '%s' %d/%d -> %d", mem::read_string(ARG(0), 31).c_str(), s32(ARG(2)), s32(ARG(3)), uid);
    RET(uid);
}

HLE(sceKernelDeleteSema) {
    Sema* s = get<Sema>(Uid(ARG(0)));
    if (!s) RET(kErrUnknownSemid);
    wake_all_deleted(s, WaitType::Sema);
    destroy_object(s->uid);
    reschedule();
    RET(0);
}

HLE(sceKernelSignalSema) {
    Sema* s = get<Sema>(Uid(ARG(0)));
    s32 n = s32(ARG(1));
    if (!s) RET(kErrUnknownSemid);
    if (n <= 0 || s->count + n > s->max_count) RET(kErrSemaOvf);
    s->count += n;
    sema_dispatch(s);
    c->r[2] = 0;
    reschedule();
}

static u32 wait_sema(Cpu* c, bool callbacks) {
    Sema* s = get<Sema>(Uid(ARG(0)));
    s32 need = s32(ARG(1));
    if (!s) return kErrUnknownSemid;
    if (need <= 0 || need > s->max_count) return kErrIllegalCount;
    if (callbacks) run_pending_callbacks();
    if (s->count >= need && s->waiters.empty()) {
        s->count -= need;
        return 0;
    }
    Thread* t = current_thread();
    if (!t) return kErrCanNotWait;
    t->wait_param[0] = u32(need);
    add_waiter(s->waiters, t, (s->attr & kAttrPriorityOrder) != 0);
    return u32(wait_current(WaitType::Sema, s->uid, ARG(2), callbacks));
}

HLE(sceKernelWaitSema) { RET(wait_sema(c, false)); }
HLE(sceKernelWaitSemaCB) { RET(wait_sema(c, true)); }

// --- event flags ----------------------------------------------------------------------------------------------

HLE(sceKernelCreateEventFlag) {
    auto e = std::make_unique<EventFlag>();
    e->type = ObjType::EventFlag;
    e->name = mem::read_string(ARG(0), 31);
    e->attr = ARG(1);
    e->init_pattern = e->pattern = ARG(2);
    Uid uid = add_object(std::move(e));
    LOG_CH("thread", "create event flag '%s' attr %x pattern %08x -> %d", mem::read_string(ARG(0), 31).c_str(), ARG(1),
           ARG(2), uid);
    RET(uid);
}

HLE(sceKernelDeleteEventFlag) {
    EventFlag* e = get<EventFlag>(Uid(ARG(0)));
    if (!e) RET(kErrUnknownEvfid);
    wake_all_deleted(e, WaitType::EventFlag);
    destroy_object(e->uid);
    reschedule();
    RET(0);
}

HLE(sceKernelSetEventFlag) {
    EventFlag* e = get<EventFlag>(Uid(ARG(0)));
    if (!e) RET(kErrUnknownEvfid);
    e->pattern |= ARG(1);
    evf_dispatch(e);
    c->r[2] = 0;
    reschedule();
}

HLE(sceKernelClearEventFlag) {
    EventFlag* e = get<EventFlag>(Uid(ARG(0)));
    if (!e) RET(kErrUnknownEvfid);
    e->pattern &= ARG(1);
    RET(0);
}

HLE(sceKernelPollEventFlag) {
    EventFlag* e = get<EventFlag>(Uid(ARG(0)));
    u32 bits = ARG(1), mode = ARG(2), out = ARG(3);
    if (!e) RET(kErrUnknownEvfid);
    if (bits == 0) RET(kErrEvfIlpat);
    if (out && mem::valid(out, 4)) mem::w32(out, e->pattern);
    if (!evf_matches(e->pattern, bits, mode)) RET(kErrEvfCond);
    evf_consume(e, bits, mode);
    RET(0);
}

static u32 wait_event_flag(Cpu* c, bool callbacks) {
    EventFlag* e = get<EventFlag>(Uid(ARG(0)));
    u32 bits = ARG(1), mode = ARG(2), out = ARG(3), timeout = ARG(4);
    if (!e) return kErrUnknownEvfid;
    if (bits == 0) return kErrEvfIlpat;
    if (callbacks) run_pending_callbacks();
    if (evf_matches(e->pattern, bits, mode)) {
        if (out && mem::valid(out, 4)) mem::w32(out, e->pattern);
        evf_consume(e, bits, mode);
        return 0;
    }
    Thread* t = current_thread();
    if (!t) return kErrCanNotWait;
    if (!(e->attr & kEvfMultiWait)) {
        for (const Waiter& w : e->waiters)
            if (live_waiter(w, WaitType::EventFlag, e->uid)) return kErrEvfMulti;
    }
    t->wait_param[0] = bits;
    t->wait_param[1] = mode;
    t->wait_param[2] = out;
    add_waiter(e->waiters, t, (e->attr & kAttrPriorityOrder) != 0);
    s32 result = wait_current(WaitType::EventFlag, e->uid, timeout, callbacks);
    if (result != 0 && out && mem::valid(out, 4)) {
        // On timeout the caller still gets to see the pattern as it is now.
        if (EventFlag* still = get<EventFlag>(Uid(ARG(0)))) mem::w32(out, still->pattern);
    }
    return u32(result);
}

HLE(sceKernelWaitEventFlag) { RET(wait_event_flag(c, false)); }
HLE(sceKernelWaitEventFlagCB) { RET(wait_event_flag(c, true)); }

HLE(sceKernelReferEventFlagStatus) {
    EventFlag* e = get<EventFlag>(Uid(ARG(0)));
    u32 out = ARG(1);
    if (!e) RET(kErrUnknownEvfid);
    if (!mem::valid(out, 52)) RET(kErrIllegalAddr);
    u32 waiting = 0;
    for (const Waiter& w : e->waiters)
        if (live_waiter(w, WaitType::EventFlag, e->uid)) ++waiting;
    u8 info[52] = {};
    store_le<u32>(info, 52);
    std::memcpy(info + 4, e->name.c_str(), std::min<size_t>(e->name.size(), 31));
    store_le<u32>(info + 36, e->attr);
    store_le<u32>(info + 40, e->init_pattern);
    store_le<u32>(info + 44, e->pattern);
    store_le<u32>(info + 48, waiting);
    std::memcpy(mem::ptr(out), info, 52);
    RET(0);
}

// --- lightweight mutexes --------------------------------------------------------------------------------------
// The state lives in a 32-byte work area owned by the game:
//   +0 lock count, +4 owning thread, +8 attributes, +12 waiter count, +16 object id.

HLE(sceKernelCreateLwMutex) {
    u32 work = ARG(0), attr = ARG(2);
    s32 initial = s32(ARG(3));
    if (!mem::valid(work, 32)) RET(kErrIllegalAddr);
    if (initial < 0 || (initial > 1 && !(attr & kLwMutexRecursive))) RET(kErrIllegalCount);
    auto mtx = std::make_unique<LwMutex>();
    mtx->type = ObjType::LwMutex;
    mtx->name = mem::read_string(ARG(1), 31);
    mtx->attr = attr;
    mtx->work_area = work;
    Uid uid = add_object(std::move(mtx));
    Thread* t = current_thread();
    std::memset(mem::ptr(work), 0, 32);
    mem::w32(work + 0, u32(initial));
    mem::w32(work + 4, initial > 0 && t ? u32(t->uid) : 0);
    mem::w32(work + 8, attr);
    mem::w32(work + 16, u32(uid));
    RET(0);
}

HLE(sceKernelDeleteLwMutex) {
    u32 work = ARG(0);
    if (!mem::valid(work, 32)) RET(kErrIllegalAddr);
    LwMutex* mtx = get<LwMutex>(Uid(mem::r32(work + 16)));
    if (!mtx) RET(kErrLwMutexNotFound);
    wake_all_deleted(mtx, WaitType::LwMutex);
    destroy_object(mtx->uid);
    mem::w32(work + 4, 0);
    mem::w32(work + 16, 0);
    reschedule();
    RET(0);
}

static u32 lock_lw_mutex(Cpu* c, bool callbacks) {
    u32 work = ARG(0), timeout = ARG(2);
    s32 count = s32(ARG(1));
    if (!mem::valid(work, 32)) return kErrIllegalAddr;
    LwMutex* mtx = get<LwMutex>(Uid(mem::r32(work + 16)));
    Thread* t = current_thread();
    if (!mtx) return kErrLwMutexNotFound;
    if (!t) return kErrCanNotWait;
    if (count <= 0) return kErrIllegalCount;
    if (count > 1 && !(mtx->attr & kLwMutexRecursive)) return kErrIllegalCount;

    u32 owner = mem::r32(work + 4);
    if (owner == 0) {
        mem::w32(work + 0, u32(count));
        mem::w32(work + 4, u32(t->uid));
        return 0;
    }
    if (owner == u32(t->uid)) {
        if (!(mtx->attr & kLwMutexRecursive)) return kErrLwMutexRecursive;
        mem::w32(work + 0, mem::r32(work + 0) + u32(count));
        return 0;
    }
    t->wait_param[0] = u32(count);
    add_waiter(mtx->waiters, t, (mtx->attr & kAttrPriorityOrder) != 0);
    mem::w32(work + 12, mem::r32(work + 12) + 1);
    return u32(wait_current(WaitType::LwMutex, mtx->uid, timeout, callbacks));
}

HLE(sceKernelLockLwMutex) { RET(lock_lw_mutex(c, false)); }
HLE(sceKernelLockLwMutexCB) { RET(lock_lw_mutex(c, true)); }

HLE(sceKernelUnlockLwMutex) {
    u32 work = ARG(0);
    s32 count = s32(ARG(1));
    if (!mem::valid(work, 32)) RET(kErrIllegalAddr);
    LwMutex* mtx = get<LwMutex>(Uid(mem::r32(work + 16)));
    Thread* t = current_thread();
    if (!mtx) RET(kErrLwMutexNotFound);
    if (count <= 0) RET(kErrIllegalCount);
    if (!t || mem::r32(work + 4) != u32(t->uid)) RET(kErrLwMutexUnlocked);
    s32 held = s32(mem::r32(work + 0));
    if (held < count) RET(kErrLwMutexUnlockUdf);
    held -= count;
    mem::w32(work + 0, u32(held));
    if (held == 0) {
        mem::w32(work + 4, 0);
        // Hand the mutex straight to the first waiter still queued.
        while (!mtx->waiters.empty()) {
            Waiter w = mtx->waiters.front();
            mtx->waiters.erase(mtx->waiters.begin());
            Thread* next = live_waiter(w, WaitType::LwMutex, mtx->uid);
            if (!next) continue;
            mem::w32(work + 0, next->wait_param[0]);
            mem::w32(work + 4, u32(next->uid));
            u32 waiting = mem::r32(work + 12);
            mem::w32(work + 12, waiting ? waiting - 1 : 0);
            wake_thread(next, 0);
            break;
        }
    }
    c->r[2] = 0;
    reschedule();
}

// --- callbacks ------------------------------------------------------------------------------------------------

HLE(sceKernelCreateCallback) { RET(create_callback(mem::read_string(ARG(0), 31), ARG(1), ARG(2))); }
HLE(sceKernelDeleteCallback) { RET(delete_callback(Uid(ARG(0)))); }
HLE(sceKernelCheckCallback) { RET(run_pending_callbacks() ? 1 : 0); }

// --- clock ----------------------------------------------------------------------------------------------------

HLE(sceKernelGetSystemTimeLow) { RET(u32(now_us())); }
HLE(sceKernelGetSystemTimeWide) { RET64(now_us()); }
