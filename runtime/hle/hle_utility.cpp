// sceUtility dialogs: save data, message dialog and game-data install.
//
// Save data lives in plain files under <user dir>/ms0/PSP/SAVEDATA/<game><save>/ next to a
// PARAM.SFO describing it. The game drives every dialog as a small state machine
// (init -> running -> finished -> shut down); the work itself is done when the dialog starts.

#include "lib/common/file.h"
#include "lib/psp/sfo.h"
#include "runtime/hle/hle.h"
#include "runtime/platform/host.h"
#include "runtime/ui/overlay.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <vector>

using namespace p2;
using namespace p2::kernel;

namespace {

enum : u32 { kStatusNone = 0, kStatusInit = 1, kStatusRunning = 2, kStatusFinished = 3, kStatusShutdown = 4 };

constexpr u32 kErrWrongStatus = 0x80110001; // dialog call in the wrong state
constexpr u32 kErrLoadNoData = 0x80110307;
constexpr u32 kErrLoadParam = 0x80110308;
constexpr u32 kErrSaveParam = 0x80110388;
constexpr u32 kErrRwNoData = 0x80110327;
constexpr u32 kErrSizesNoData = 0x801103C7;
constexpr u32 kErrDeleteNoData = 0x80110347;

struct Dialog {
    u32 status = kStatusNone;
    u32 param = 0;
    // What a dialog that waits for the user does on each update; returns true once it is done.
    // Dialogs without one finish on their first update.
    std::function<bool()> step;

    u32 start(u32 param_addr) {
        if (status != kStatusNone) return kErrWrongStatus;
        status = kStatusInit;
        param = param_addr;
        step = nullptr;
        return 0;
    }
    u32 update() {
        if (status == kStatusRunning && (!step || step())) {
            status = kStatusFinished;
            step = nullptr;
        }
        return 0;
    }
    u32 shutdown() {
        if (status == kStatusNone) return kErrWrongStatus;
        status = kStatusShutdown;
        return 0;
    }
    u32 get_status() {
        // Initialising and shutting down are transient: each is reported once, then the dialog
        // moves on by itself. Games only call update() once they see the running state.
        u32 s = status;
        if (s == kStatusInit) status = kStatusRunning;
        else if (s == kStatusShutdown) status = kStatusNone;
        return s;
    }
};

Dialog g_savedata, g_msg, g_install;

// --- save data parameter block --------------------------------------------------------------------------------

constexpr u32 kOffResult = 0x1C, kOffMode = 0x30, kOffGameName = 0x3C, kOffSaveName = 0x4C, kOffSaveNameList = 0x60,
              kOffFileName = 0x64, kOffDataBuf = 0x74, kOffDataBufSize = 0x78, kOffDataSize = 0x7C, kOffSfoTitle = 0x80,
              kOffSfoSaveTitle = 0x100, kOffSfoDetail = 0x180, kOffSfoParental = 0x580, kOffIcon0 = 0x584,
              kOffIcon1 = 0x594, kOffPic1 = 0x5A4, kOffSnd0 = 0x5B4, kOffFocus = 0x5C8,
              kOffMsFree = 0x5D0, kOffMsData = 0x5D4, kOffUtilityData = 0x5D8, kOffIdList = 0x5F4, kOffFileList = 0x5F8,
              kOffSizeInfo = 0x5FC;

std::string fixed_string(u32 addr, u32 max) {
    std::string s = mem::read_string(addr, max);
    return s;
}

std::filesystem::path save_root() { return host::memory_stick_dir() / "PSP" / "SAVEDATA"; }

bool safe_name(const std::string& name) {
    if (name.empty()) return false;
    for (char ch : name)
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '.' || static_cast<unsigned char>(ch) < 0x20) return false;
    return true;
}

std::string size_text(u64 bytes) {
    char buf[16];
    if (bytes >= (1u << 20)) std::snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / (1 << 20));
    else std::snprintf(buf, sizeof(buf), "%.0f KB", double(bytes) / 1024);
    return buf;
}

u64 directory_size(const std::filesystem::path& dir) {
    u64 total = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    return total;
}

void write_date(u32 addr, std::filesystem::file_time_type time) {
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(time - std::filesystem::file_time_type::clock::now() + system_clock::now());
    std::time_t t = system_clock::to_time_t(sys);
    std::tm local{};
    localtime_s(&local, &t);
    mem::w16(addr + 0, u16(local.tm_year + 1900));
    mem::w16(addr + 2, u16(local.tm_mon + 1));
    mem::w16(addr + 4, u16(local.tm_mday));
    mem::w16(addr + 6, u16(local.tm_hour));
    mem::w16(addr + 8, u16(local.tm_min));
    mem::w16(addr + 10, u16(local.tm_sec));
    mem::w32(addr + 12, 0);
}

// Writes one of the optional attachments (icon, picture, sound) if the game supplied it.
void save_attachment(u32 param, u32 offset, const std::filesystem::path& path) {
    u32 buf = mem::r32(param + offset), size = mem::r32(param + offset + 8);
    if (buf && size && mem::valid(buf, size)) write_file(path, mem::ptr(buf), size);
}

void load_attachment(u32 param, u32 offset, const std::filesystem::path& path) {
    u32 buf = mem::r32(param + offset), capacity = mem::r32(param + offset + 4);
    std::vector<u8> data;
    if (!buf || !capacity || !read_file(path, data)) return;
    u32 n = std::min<u32>(u32(data.size()), capacity);
    if (!mem::valid(buf, n)) return;
    std::memcpy(mem::ptr(buf), data.data(), n);
    mem::w32(param + offset + 8, u32(data.size()));
}

u32 do_save(u32 param, const std::filesystem::path& dir, const std::string& file_name) {
    u32 buf = mem::r32(param + kOffDataBuf), size = mem::r32(param + kOffDataSize);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!file_name.empty()) {
        if (size && !mem::valid(buf, size)) return kErrSaveParam;
        if (!write_file(dir / file_name, size ? mem::ptr(buf) : nullptr, size)) return kErrSaveParam;
    }
    psp::Sfo sfo;
    std::vector<u8> existing;
    if (read_file(dir / "PARAM.SFO", existing)) sfo.parse(existing.data(), existing.size());
    sfo.set_string("CATEGORY", "MS", 4);
    sfo.set_int("PARENTAL_LEVEL", mem::r8(param + kOffSfoParental));
    sfo.set_string("TITLE", fixed_string(param + kOffSfoTitle, 127), 128);
    sfo.set_string("SAVEDATA_TITLE", fixed_string(param + kOffSfoSaveTitle, 127), 128);
    sfo.set_string("SAVEDATA_DETAIL", fixed_string(param + kOffSfoDetail, 1023), 1024);
    sfo.set_string("SAVEDATA_DIRECTORY", dir.filename().string(), 64);
    std::vector<u8> bytes = sfo.serialize();
    write_file(dir / "PARAM.SFO", bytes.data(), bytes.size());
    save_attachment(param, kOffIcon0, dir / "ICON0.PNG");
    save_attachment(param, kOffIcon1, dir / "ICON1.PMF");
    save_attachment(param, kOffPic1, dir / "PIC1.PNG");
    save_attachment(param, kOffSnd0, dir / "SND0.AT3");
    return 0;
}

u32 do_load(u32 param, const std::filesystem::path& dir, const std::string& file_name, u32 missing_error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return missing_error;
    if (!file_name.empty()) {
        std::vector<u8> data;
        if (!read_file(dir / file_name, data)) return missing_error;
        u32 buf = mem::r32(param + kOffDataBuf), capacity = mem::r32(param + kOffDataBufSize);
        u32 n = std::min<u32>(u32(data.size()), capacity);
        if (n && !mem::valid(buf, n)) return kErrLoadParam;
        if (n) std::memcpy(mem::ptr(buf), data.data(), n);
        mem::w32(param + kOffDataSize, u32(data.size()));
    }
    std::vector<u8> sfo_bytes;
    psp::Sfo sfo;
    if (read_file(dir / "PARAM.SFO", sfo_bytes) && sfo.parse(sfo_bytes.data(), sfo_bytes.size())) {
        mem::write_string(param + kOffSfoTitle, sfo.get_string("TITLE"), 128);
        mem::write_string(param + kOffSfoSaveTitle, sfo.get_string("SAVEDATA_TITLE"), 128);
        mem::write_string(param + kOffSfoDetail, sfo.get_string("SAVEDATA_DETAIL"), 1024);
        mem::w8(param + kOffSfoParental, u8(sfo.get_int("PARENTAL_LEVEL")));
    }
    load_attachment(param, kOffIcon0, dir / "ICON0.PNG");
    load_attachment(param, kOffIcon1, dir / "ICON1.PMF");
    load_attachment(param, kOffPic1, dir / "PIC1.PNG");
    load_attachment(param, kOffSnd0, dir / "SND0.AT3");
    return 0;
}

void fill_free_space(u32 addr) {
    if (!addr || !mem::valid(addr, 20)) return;
    const u32 cluster = 32 * 1024;
    const u64 free_bytes = u64(1) << 30;
    mem::w32(addr + 0, cluster);
    mem::w32(addr + 4, u32(free_bytes / cluster));
    mem::w32(addr + 8, u32(free_bytes / 1024));
    mem::write_string(addr + 12, size_text(free_bytes), 8);
}

void fill_used_space(u32 addr, u64 bytes) {
    // { clusters, KB, text[8], KB rounded to 32 KB, text[8] }
    const u32 cluster = 32 * 1024;
    u64 rounded = align_up<u64>(bytes, cluster);
    mem::w32(addr + 0, u32(rounded / cluster));
    mem::w32(addr + 4, u32(align_up<u64>(bytes, 1024) / 1024));
    mem::write_string(addr + 8, size_text(bytes), 8);
    mem::w32(addr + 16, u32(rounded / 1024));
    mem::write_string(addr + 20, size_text(rounded), 8);
}

// `pick_slot`: nobody chose a slot for a list mode, so take a sensible one.
u32 run_savedata(u32 param, bool pick_slot) {
    const u32 struct_size = mem::r32(param);
    const u32 mode = mem::r32(param + kOffMode);
    const std::string game = fixed_string(param + kOffGameName, 13);
    std::string save = fixed_string(param + kOffSaveName, 20);
    const std::string file = fixed_string(param + kOffFileName, 13);
    const u32 name_list = mem::r32(param + kOffSaveNameList);

    // Without a selection screen, loads take the first slot that exists and saves take the first
    // slot offered.
    if (pick_slot && (mode == 4 || mode == 5 || mode == 6) && name_list && mem::valid(name_list, 20)) {
        std::error_code ec;
        std::string first;
        for (u32 i = 0; i < 64; ++i) {
            std::string candidate = fixed_string(name_list + i * 20, 20);
            if (candidate.empty()) break;
            if (first.empty()) first = candidate;
            if (mode != 5 && std::filesystem::is_directory(save_root() / (game + candidate), ec)) {
                first = candidate;
                break;
            }
        }
        if (!first.empty()) {
            save = first;
            mem::write_string(param + kOffSaveName, save, 20);
        }
    }

    LOG_INFO("save", "save-data request: mode %u, game '%s', save '%s', file '%s', %u bytes (block size %#x)", mode, game.c_str(),
             save.c_str(), file.c_str(), mem::r32(param + kOffDataSize), struct_size);
    if (!safe_name(game) || (!save.empty() && !safe_name(save))) return kErrLoadParam;
    if (!file.empty() && (file.find('/') != std::string::npos || file.find('\\') != std::string::npos || file.find("..") != std::string::npos))
        return kErrLoadParam;
    const std::filesystem::path dir = save_root() / (game + save);
    std::error_code ec;

    switch (mode) {
    case 0: // auto load
    case 2: // load
    case 4: // list load
        return do_load(param, dir, file, kErrLoadNoData);
    case 15: // read (secure)
    case 16: // read
        return do_load(param, dir, file, kErrRwNoData);
    case 1: // auto save
    case 3: // save
    case 5: // list save
    case 13: // make data (secure)
    case 14: // make data
    case 17: // write (secure)
    case 18: // write
        return do_save(param, dir, file);
    case 6: // list delete
    case 7: // delete all
    case 9: // auto delete
    case 10: // delete
        if (!std::filesystem::is_directory(dir, ec)) return kErrDeleteNoData;
        std::filesystem::remove_all(dir, ec);
        return 0;
    case 19: // erase one file (secure)
    case 20:
        if (!std::filesystem::remove(dir / file, ec)) return kErrRwNoData;
        return 0;
    case 8: { // sizes
        const bool exists = std::filesystem::is_directory(dir, ec);
        if (struct_size > kOffMsFree) fill_free_space(mem::r32(param + kOffMsFree));
        u32 ms_data = struct_size > kOffMsData ? mem::r32(param + kOffMsData) : 0;
        if (ms_data && mem::valid(ms_data, 64) && exists) fill_used_space(ms_data + 36, directory_size(dir));
        u32 utility = struct_size > kOffUtilityData ? mem::r32(param + kOffUtilityData) : 0;
        if (utility && mem::valid(utility, 28)) {
            // Space a new save would need: its data plus the attachments the game supplied.
            u64 needed = mem::r32(param + kOffDataSize);
            for (u32 off : {kOffIcon0, kOffIcon1, kOffPic1, kOffSnd0}) needed += mem::r32(param + off + 8);
            fill_used_space(utility, needed + 0x800);
        }
        return exists ? 0 : kErrSizesNoData;
    }
    case 11: { // list: which save directories of this game exist
        u32 list = struct_size > kOffIdList ? mem::r32(param + kOffIdList) : 0;
        if (!list || !mem::valid(list, 12)) return 0;
        u32 max = mem::r32(list), entries = mem::r32(list + 8), count = 0;
        for (const auto& e : std::filesystem::directory_iterator(save_root(), ec)) {
            std::string name = e.path().filename().string();
            if (!e.is_directory(ec) || name.compare(0, game.size(), game) != 0 || count >= max) continue;
            u32 entry = entries + count * 72;
            if (!mem::valid(entry, 72)) break;
            std::memset(mem::ptr(entry), 0, 72);
            mem::w32(entry, 0x11FF);
            write_date(entry + 4, e.last_write_time(ec));
            write_date(entry + 20, e.last_write_time(ec));
            write_date(entry + 36, e.last_write_time(ec));
            mem::write_string(entry + 52, name.substr(game.size()), 20);
            ++count;
        }
        mem::w32(list + 4, count);
        return 0;
    }
    case 12: { // files inside one save
        if (!std::filesystem::is_directory(dir, ec)) return kErrRwNoData;
        u32 list = struct_size > kOffFileList ? mem::r32(param + kOffFileList) : 0;
        if (!list || !mem::valid(list, 36)) return 0;
        u32 max_secure = mem::r32(list), max_normal = mem::r32(list + 4);
        u32 secure_entries = mem::r32(list + 24), normal_entries = mem::r32(list + 28);
        u32 secure = 0, normal = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string name = e.path().filename().string();
            const bool system = name == "PARAM.SFO" || name == "ICON0.PNG" || name == "ICON1.PMF" || name == "PIC1.PNG" || name == "SND0.AT3";
            if (system || name.size() > 12) continue;
            // Data files are reported as secure entries when the game provides room for them.
            u32* count = secure_entries && secure < max_secure ? &secure : &normal;
            u32 base = count == &secure ? secure_entries : normal_entries;
            if (count == &normal && (!normal_entries || normal >= max_normal)) continue;
            u32 entry = base + *count * 80;
            if (!mem::valid(entry, 80)) break;
            std::memset(mem::ptr(entry), 0, 80);
            mem::w32(entry, 0x21FF);
            mem::w64(entry + 8, e.file_size(ec));
            write_date(entry + 16, e.last_write_time(ec));
            write_date(entry + 32, e.last_write_time(ec));
            write_date(entry + 48, e.last_write_time(ec));
            mem::write_string(entry + 64, name, 16);
            ++*count;
        }
        mem::w32(list + 12, secure);
        mem::w32(list + 16, normal);
        mem::w32(list + 20, 0);
        return 0;
    }
    case 22: { // size check for a planned save
        u32 info = struct_size > kOffSizeInfo ? mem::r32(param + kOffSizeInfo) : 0;
        if (info && mem::valid(info, 64)) {
            const u32 cluster = 32 * 1024;
            mem::w32(info + 24, cluster);                       // sector size
            mem::w32(info + 28, u32((u64(1) << 30) / cluster)); // free sectors
            mem::w32(info + 32, u32((u64(1) << 30) / 1024));    // free KB
            mem::write_string(info + 36, size_text(u64(1) << 30), 8);
            mem::w32(info + 44, 0);                             // nothing more is needed
            mem::write_string(info + 48, "0 KB", 8);
            mem::w32(info + 56, 0);
        }
        return std::filesystem::is_directory(dir, ec) ? 0 : kErrRwNoData;
    }
    default:
        LOG_WARN("save", "save-data mode %u is not implemented", mode);
        return 0;
    }
}

// --- slot selection ---------------------------------------------------------------------------------------------
// The list modes let the player choose a slot. The overlay shows the list; the dialog stays in
// its running state until an answer arrives.

constexpr u32 kResultCancelled = 1;

struct SlotFlow {
    u32 param = 0, mode = 0;
    std::string game;
    std::vector<ui::SaveSlot> slots;
    enum { Listing, Confirming, Reporting } stage = Listing;
    int chosen = 0;
    u32 report_result = 0;
};

SlotFlow g_flow;

std::string time_text(std::filesystem::file_time_type time) {
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(time - std::filesystem::file_time_type::clock::now() + system_clock::now());
    std::time_t t = system_clock::to_time_t(sys);
    std::tm local{};
    localtime_s(&local, &t);
    return format("%04d-%02d-%02d  %02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
}

ui::SlotPurpose purpose_of(u32 mode) {
    return mode == 5 ? ui::SlotPurpose::Save : mode == 4 ? ui::SlotPurpose::Load : ui::SlotPurpose::Delete;
}

void list_slots(SlotFlow& flow, u32 name_list, const std::string& file) {
    flow.slots.clear();
    std::error_code ec;
    for (u32 i = 0; i < 64 && mem::valid(name_list + i * 20, 20); ++i) {
        ui::SaveSlot slot;
        slot.id = fixed_string(name_list + i * 20, 20);
        if (slot.id.empty()) break;
        if (!safe_name(slot.id)) continue;
        const std::filesystem::path dir = save_root() / (flow.game + slot.id);
        slot.exists = std::filesystem::is_directory(dir, ec);
        if (slot.exists) {
            std::vector<u8> bytes;
            psp::Sfo sfo;
            if (read_file(dir / "PARAM.SFO", bytes) && sfo.parse(bytes.data(), bytes.size())) {
                slot.title = sfo.get_string("SAVEDATA_TITLE");
                slot.detail = sfo.get_string("SAVEDATA_DETAIL");
                if (slot.title.empty()) slot.title = sfo.get_string("TITLE");
            }
            const std::filesystem::path stamp = !file.empty() && std::filesystem::exists(dir / file, ec) ? dir / file : dir;
            slot.when = time_text(std::filesystem::last_write_time(stamp, ec));
            slot.icon = dir / "ICON0.PNG";
        }
        flow.slots.push_back(std::move(slot));
    }
}

// Where the list opens, following the game's hint (first, last, first empty, ...).
int initial_focus(const SlotFlow& flow, u32 hint) {
    const int count = int(flow.slots.size());
    auto find = [&](bool exists, bool from_end) {
        for (int k = 0; k < count; ++k) {
            const int i = from_end ? count - 1 - k : k;
            if (flow.slots[size_t(i)].exists == exists) return i;
        }
        return 0;
    };
    switch (hint) {
    case 2: return std::max(0, count - 1);
    case 3: { // most recently written
        int best = find(true, false);
        for (int i = 0; i < count; ++i)
            if (flow.slots[size_t(i)].exists && flow.slots[size_t(i)].when > flow.slots[size_t(best)].when) best = i;
        return best;
    }
    case 7: return find(false, false);
    case 8: return find(false, true);
    default: return flow.mode == 5 ? 0 : find(true, false);
    }
}

void finish_savedata(u32 param, u32 result) {
    mem::w32(param + kOffResult, result);
    LOG_INFO("save", "save-data result %08x", result);
}

void perform_chosen(SlotFlow& flow) {
    mem::write_string(flow.param + kOffSaveName, flow.slots[size_t(flow.chosen)].id, 20);
    const u32 result = run_savedata(flow.param, false);
    finish_savedata(flow.param, result);
    if (result == 0 && flow.mode == 5) ui::notify("Saved.");
    else if (result == 0 && flow.mode == 6) ui::notify("Save data deleted.");
    else if (result != 0) ui::notify(format("The save data could not be %s.", flow.mode == 5 ? "written" : flow.mode == 4 ? "read" : "deleted"));
}

bool step_slot_flow() {
    SlotFlow& flow = g_flow;
    switch (flow.stage) {
    case SlotFlow::Listing: {
        const int answer = ui::slot_dialog_result();
        if (answer == ui::kPending) return false;
        if (answer == ui::kCancelled) {
            finish_savedata(flow.param, kResultCancelled);
            return true;
        }
        flow.chosen = answer;
        const bool exists = flow.slots[size_t(answer)].exists;
        if (flow.mode == 5 && exists) {
            ui::open_message("This slot already holds save data.\nOverwrite it?", true, true);
            flow.stage = SlotFlow::Confirming;
            return false;
        }
        if (flow.mode == 6) {
            ui::open_message("Delete this save data?", true, true);
            flow.stage = SlotFlow::Confirming;
            return false;
        }
        perform_chosen(flow);
        return true;
    }
    case SlotFlow::Confirming: {
        const int answer = ui::message_result();
        if (answer == ui::kPending) return false;
        if (answer == 1) {
            perform_chosen(flow);
            return true;
        }
        ui::open_slot_dialog(purpose_of(flow.mode), flow.slots, flow.chosen);
        flow.stage = SlotFlow::Listing;
        return false;
    }
    case SlotFlow::Reporting:
        if (ui::message_result() == ui::kPending) return false;
        finish_savedata(flow.param, flow.report_result);
        return true;
    }
    return true;
}

// Starts the interactive version of a list mode. False when the request cannot be shown as a list.
bool begin_slot_flow(u32 param) {
    SlotFlow& flow = g_flow;
    flow = SlotFlow{};
    flow.param = param;
    flow.mode = mem::r32(param + kOffMode);
    flow.game = fixed_string(param + kOffGameName, 13);
    const u32 name_list = mem::r32(param + kOffSaveNameList);
    if (!safe_name(flow.game) || !name_list || !mem::valid(name_list, 20)) return false;
    list_slots(flow, name_list, fixed_string(param + kOffFileName, 13));
    if (flow.slots.empty()) return false;

    const bool any = std::any_of(flow.slots.begin(), flow.slots.end(), [](const ui::SaveSlot& s) { return s.exists; });
    LOG_INFO("save", "save-data request: mode %u, game '%s', %zu slot(s) offered", flow.mode, flow.game.c_str(), flow.slots.size());
    if (flow.mode != 5 && !any) {
        ui::open_message("There is no save data.", false, false);
        flow.stage = SlotFlow::Reporting;
        flow.report_result = flow.mode == 4 ? kErrLoadNoData : kErrDeleteNoData;
    } else {
        const u32 hint = mem::r32(param) > kOffFocus ? mem::r32(param + kOffFocus) : 0;
        ui::open_slot_dialog(purpose_of(flow.mode), flow.slots, initial_focus(flow, hint));
    }
    g_savedata.step = step_slot_flow;
    return true;
}

} // namespace

// --- save data ------------------------------------------------------------------------------------------------

HLE(sceUtilitySavedataInitStart) {
    u32 param = ARG(0);
    if (!mem::valid(param, 0x5C8)) RET(kErrIllegalAddr);
    u32 started = g_savedata.start(param);
    if (started) RET(started);
    const u32 mode = mem::r32(param + kOffMode);
    if (ui::interactive() && (mode == 4 || mode == 5 || mode == 6) && begin_slot_flow(param)) RET(0);
    finish_savedata(param, run_savedata(param, true));
    RET(0);
}

HLE(sceUtilitySavedataGetStatus) { RET(g_savedata.get_status()); }
HLE(sceUtilitySavedataUpdate) { RET(g_savedata.update()); }
HLE(sceUtilitySavedataShutdownStart) { RET(g_savedata.shutdown()); }

// --- message dialog -------------------------------------------------------------------------------------------
// +0x30 result, +0x34 mode (0 error code, 1 text), +0x38 error value, +0x3C text[512], +0x23C options,
// +0x240 button pressed (1 yes, 2 no, 3 back).

HLE(sceUtilityMsgDialogInitStart) {
    u32 param = ARG(0);
    if (!mem::valid(param, 0x244)) RET(kErrIllegalAddr);
    u32 started = g_msg.start(param);
    if (started) RET(started);
    u32 mode = mem::r32(param + 0x34), options = mem::r32(param + 0x23C);
    const std::string text = mode == 0 ? format("An error has occurred. (%08X)", mem::r32(param + 0x38))
                                       : mem::read_string(param + 0x3C, 511);
    LOG_INFO("dialog", "system message: %s", text.c_str());
    const bool yes_no = mode != 0 && (options & 0x10) != 0, default_no = (options & 0x100) != 0;
    mem::w32(param + kOffResult, 0);
    if (ui::interactive()) {
        ui::open_message(text, yes_no, default_no);
        g_msg.step = [param, yes_no] {
            const int answer = ui::message_result();
            if (answer == ui::kPending) return false;
            mem::w32(param + 0x240, yes_no ? u32(answer) : (answer == 3 ? 3u : 0u));
            return true;
        };
    } else {
        // With nobody to ask, a yes/no question is answered with its default choice.
        mem::w32(param + 0x240, yes_no ? (default_no ? 2u : 1u) : 0u);
    }
    RET(0);
}

HLE(sceUtilityMsgDialogGetStatus) { RET(g_msg.get_status()); }
HLE(sceUtilityMsgDialogUpdate) { RET(g_msg.update()); }
HLE(sceUtilityMsgDialogShutdownStart) { RET(g_msg.shutdown()); }

// --- game-data install ----------------------------------------------------------------------------------------
// Copying game data to the memory stick only speeds up loading on the console; here the disc image
// is already as fast as storage gets, so the request completes at once without copying anything.

HLE(sceUtilityGamedataInstallInitStart) {
    u32 param = ARG(0);
    if (!mem::valid(param, 0x40)) RET(kErrIllegalAddr);
    u32 started = g_install.start(param);
    if (started) RET(started);
    LOG_INFO("dialog", "game-data install requested; nothing needs installing");
    mem::w32(param + kOffResult, 0);
    RET(0);
}

HLE(sceUtilityGamedataInstallGetStatus) { RET(g_install.get_status()); }
HLE(sceUtilityGamedataInstallUpdate) { RET(g_install.update()); }
HLE(sceUtilityGamedataInstallShutdownStart) { RET(g_install.shutdown()); }
HLE(sceUtilityGamedataInstallAbort) { RET(0); }
