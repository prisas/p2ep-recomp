#include "runtime/kernel/kernel.h"

#include "lib/common/log.h"
#include "runtime/mem/memory.h"

#include <algorithm>

namespace p2::kernel {

namespace {

constexpr u32 kGrain = 0x100; // partition blocks are multiples of 256 bytes

struct Block {
    u32 addr;
    u32 size;
    bool used;
    std::string name;
};

// First-fit allocator over one address range, kept as a sorted list of adjacent blocks.
class Partition {
public:
    Partition(u32 start, u32 end) { blocks_.push_back({start, end - start, false, {}}); }

    u32 alloc(const std::string& name, AllocType type, u32 size, u32 param) {
        size = align_up<u32>(size ? size : 1, kGrain);
        switch (type) {
        case AllocType::Low:
        case AllocType::LowAligned: {
            u32 align = type == AllocType::LowAligned && param > kGrain ? param : kGrain;
            for (size_t i = 0; i < blocks_.size(); ++i) {
                const Block& b = blocks_[i];
                if (b.used) continue;
                u32 addr = align_up<u32>(b.addr, align);
                if (addr - b.addr + size <= b.size && addr >= b.addr) return carve(i, addr, size, name);
            }
            return 0;
        }
        case AllocType::High:
        case AllocType::HighAligned: {
            u32 align = type == AllocType::HighAligned && param > kGrain ? param : kGrain;
            for (size_t i = blocks_.size(); i-- > 0;) {
                const Block& b = blocks_[i];
                if (b.used || b.size < size) continue;
                u32 addr = align_down<u32>(b.addr + b.size - size, align);
                if (addr >= b.addr) return carve(i, addr, size, name);
            }
            return 0;
        }
        case AllocType::Addr: {
            u32 addr = param;
            if (addr & (kGrain - 1)) return 0;
            for (size_t i = 0; i < blocks_.size(); ++i) {
                const Block& b = blocks_[i];
                if (!b.used && addr >= b.addr && u64(addr) + size <= u64(b.addr) + b.size) return carve(i, addr, size, name);
            }
            return 0;
        }
        }
        return 0;
    }

    bool free(u32 addr) {
        for (size_t i = 0; i < blocks_.size(); ++i) {
            if (!blocks_[i].used || blocks_[i].addr != addr) continue;
            blocks_[i].used = false;
            blocks_[i].name.clear();
            if (i + 1 < blocks_.size() && !blocks_[i + 1].used) {
                blocks_[i].size += blocks_[i + 1].size;
                blocks_.erase(blocks_.begin() + ptrdiff_t(i) + 1);
            }
            if (i > 0 && !blocks_[i - 1].used) {
                blocks_[i - 1].size += blocks_[i].size;
                blocks_.erase(blocks_.begin() + ptrdiff_t(i));
            }
            return true;
        }
        return false;
    }

    u32 max_free() const {
        u32 best = 0;
        for (const Block& b : blocks_)
            if (!b.used) best = std::max(best, b.size);
        return best;
    }

    u32 total_free() const {
        u32 total = 0;
        for (const Block& b : blocks_)
            if (!b.used) total += b.size;
        return total;
    }

private:
    u32 carve(size_t index, u32 addr, u32 size, const std::string& name) {
        Block b = blocks_[index];
        std::vector<Block> parts;
        if (addr > b.addr) parts.push_back({b.addr, addr - b.addr, false, {}});
        parts.push_back({addr, size, true, name});
        u32 end = addr + size, block_end = b.addr + b.size;
        if (end < block_end) parts.push_back({end, block_end - end, false, {}});
        blocks_.erase(blocks_.begin() + ptrdiff_t(index));
        blocks_.insert(blocks_.begin() + ptrdiff_t(index), parts.begin(), parts.end());
        return addr;
    }

    std::vector<Block> blocks_;
};

Partition& user_partition() {
    static Partition partition(mem::kUserBase, mem::kUserEnd);
    return partition;
}

Partition& kernel_partition() {
    // The first 64 KiB stay unused so that small bogus pointers into kernel RAM are easy to spot.
    static Partition partition(mem::kKernelBase + 0x10000, mem::kKernelEnd);
    return partition;
}

} // namespace

u32 alloc_user(const std::string& name, AllocType type, u32 size, u32 addr_or_align) {
    u32 addr = user_partition().alloc(name, type, size, addr_or_align);
    LOG_CH("mem", "alloc '%s' type %u size %08x -> %08x", name.c_str(), u32(type), size, addr);
    return addr;
}

u32 alloc_kernel(const std::string& name, u32 size) {
    u32 addr = kernel_partition().alloc(name, AllocType::Low, size, 0);
    if (!addr) P2_FATAL("out of kernel memory allocating '%s' (%u bytes)", name.c_str(), size);
    return addr;
}

bool free_user(u32 addr) { return user_partition().free(addr); }
u32 user_max_free() { return user_partition().max_free(); }
u32 user_total_free() { return user_partition().total_free(); }

void reserve_user(u32 start, u32 end, const std::string& name) {
    start = align_down<u32>(start, kGrain);
    end = align_up<u32>(end, kGrain);
    if (!user_partition().alloc(name, AllocType::Addr, end - start, start))
        P2_FATAL("cannot reserve guest memory %08x-%08x for '%s'", start, end, name.c_str());
}

} // namespace p2::kernel
