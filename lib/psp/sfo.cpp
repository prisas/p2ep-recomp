#include "lib/psp/sfo.h"

#include <algorithm>

namespace p2::psp {

namespace {
constexpr u32 kMagic = 0x46535000; // "\0PSF"
constexpr u16 kFmtBinary = 0x0004, kFmtText = 0x0204, kFmtInt = 0x0404;
} // namespace

bool Sfo::parse(const u8* data, size_t size) {
    entries_.clear();
    if (size < 20 || load_le<u32>(data) != kMagic) return false;
    u32 key_table = load_le<u32>(data + 8);
    u32 data_table = load_le<u32>(data + 12);
    u32 count = load_le<u32>(data + 16);
    if (u64(20) + u64(count) * 16 > size || key_table > size || data_table > size) return false;

    for (u32 i = 0; i < count; ++i) {
        const u8* e = data + 20 + i * 16;
        u32 key_off = load_le<u16>(e);
        u16 fmt = load_le<u16>(e + 2);
        u32 len = load_le<u32>(e + 4);
        u32 max = load_le<u32>(e + 8);
        u32 data_off = load_le<u32>(e + 12);
        if (key_table + key_off >= size || u64(data_table) + data_off + len > size) return false;

        SfoEntry entry;
        const char* key = reinterpret_cast<const char*>(data + key_table + key_off);
        entry.key.assign(key, strnlen(key, size - (key_table + key_off)));
        entry.format = fmt;
        entry.max_size = max;
        const u8* value = data + data_table + data_off;
        if (fmt == kFmtInt && len >= 4) {
            entry.is_int = true;
            entry.int_value = load_le<u32>(value);
        } else if (fmt == kFmtText) {
            entry.str_value.assign(reinterpret_cast<const char*>(value),
                                   strnlen(reinterpret_cast<const char*>(value), len));
        } else {
            entry.raw.assign(value, value + len);
        }
        entries_.push_back(std::move(entry));
    }
    return true;
}

std::vector<u8> Sfo::serialize() const {
    // Keys must be sorted for the firmware's lookup; keep that property.
    std::vector<const SfoEntry*> sorted;
    for (const SfoEntry& e : entries_) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](const SfoEntry* a, const SfoEntry* b) { return a->key < b->key; });

    std::vector<u8> keys, values;
    std::vector<u8> index;
    for (const SfoEntry* e : sorted) {
        u32 len;
        std::vector<u8> value;
        if (e->is_int) {
            value.resize(4);
            store_le<u32>(value.data(), e->int_value);
            len = 4;
        } else if (e->format == kFmtText) {
            value.assign(e->str_value.begin(), e->str_value.end());
            value.push_back(0);
            len = u32(value.size());
        } else {
            value = e->raw;
            len = u32(value.size());
        }
        u32 max = std::max<u32>(align_up<u32>(std::max(e->max_size, len), 4), 4);
        if (value.size() > max) value.resize(max);
        value.resize(max, 0);

        u8 rec[16];
        store_le<u16>(rec, u16(keys.size()));
        store_le<u16>(rec + 2, e->is_int ? kFmtInt : e->format);
        store_le<u32>(rec + 4, std::min(len, max));
        store_le<u32>(rec + 8, max);
        store_le<u32>(rec + 12, u32(values.size()));
        index.insert(index.end(), rec, rec + 16);

        keys.insert(keys.end(), e->key.begin(), e->key.end());
        keys.push_back(0);
        values.insert(values.end(), value.begin(), value.end());
    }
    keys.resize(align_up<size_t>(keys.size(), 4), 0);

    std::vector<u8> out(20);
    store_le<u32>(out.data(), kMagic);
    store_le<u32>(out.data() + 4, 0x00000101);
    store_le<u32>(out.data() + 8, u32(20 + index.size()));
    store_le<u32>(out.data() + 12, u32(20 + index.size() + keys.size()));
    store_le<u32>(out.data() + 16, u32(sorted.size()));
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), values.begin(), values.end());
    return out;
}

const SfoEntry* Sfo::find(std::string_view key) const {
    for (const SfoEntry& e : entries_)
        if (e.key == key) return &e;
    return nullptr;
}

std::string Sfo::get_string(std::string_view key, std::string_view fallback) const {
    const SfoEntry* e = find(key);
    return e && !e->is_int ? e->str_value : std::string(fallback);
}

u32 Sfo::get_int(std::string_view key, u32 fallback) const {
    const SfoEntry* e = find(key);
    return e && e->is_int ? e->int_value : fallback;
}

SfoEntry& Sfo::upsert(std::string_view key) {
    for (SfoEntry& e : entries_)
        if (e.key == key) return e;
    entries_.push_back({});
    entries_.back().key = std::string(key);
    return entries_.back();
}

void Sfo::set_string(std::string_view key, std::string_view value, u32 max_size) {
    SfoEntry& e = upsert(key);
    e.is_int = false;
    e.format = kFmtText;
    e.str_value = std::string(value);
    e.raw.clear();
    e.max_size = max_size;
}

void Sfo::set_int(std::string_view key, u32 value) {
    SfoEntry& e = upsert(key);
    e.is_int = true;
    e.format = kFmtInt;
    e.int_value = value;
    e.max_size = 4;
}

void Sfo::set_raw(std::string_view key, const u8* data, size_t size, u32 max_size) {
    SfoEntry& e = upsert(key);
    e.is_int = false;
    e.format = kFmtBinary;
    e.raw.assign(data, data + size);
    e.max_size = max_size;
}

} // namespace p2::psp
