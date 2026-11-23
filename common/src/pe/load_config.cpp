#include <fmt/format.h>

#include "g6/pe/directories.h"

namespace g6::pe {
namespace {

// Field offsets in IMAGE_LOAD_CONFIG_DIRECTORY64. A field exists only when the structure's own
// Size field covers it; older linkers emit shorter structures.
constexpr uint32_t kSecurityCookie = 0x58;
constexpr uint32_t kGuardCfCheck = 0x70;
constexpr uint32_t kGuardCfDispatch = 0x78;
constexpr uint32_t kGuardCfFunctionTable = 0x80;
constexpr uint32_t kGuardCfFunctionCount = 0x88;
constexpr uint32_t kGuardFlags = 0x90;
constexpr uint32_t kGuardEhContinuationTable = 0x108;
constexpr uint32_t kGuardEhContinuationCount = 0x110;

// GuardFlags bits 28..31 give the number of metadata bytes after each 4-byte table RVA.
constexpr uint32_t kGuardTableStrideMask = 0xF0000000u;
constexpr uint32_t kGuardTableStrideShift = 28;
constexpr uint64_t kMaxGuardEntries = 1u << 24;

std::vector<uint32_t> read_guard_table(const Image& image, uint64_t table_va, uint64_t count,
                                       uint32_t stride) {
    std::vector<uint32_t> rvas;
    if (table_va == 0 || count == 0) {
        return rvas;
    }
    if (count > kMaxGuardEntries) {
        throw FormatError(fmt::format("guard table has an implausible {} entries", count));
    }
    const uint32_t table = image.va_to_rva(table_va);
    rvas.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        rvas.push_back(image.read<uint32_t>(static_cast<uint32_t>(table + i * stride)));
    }
    return rvas;
}

} // namespace

std::optional<LoadConfig> read_load_config(const Image& image) {
    const DataDirectory dir = image.directory(Directory::LoadConfig);
    if (!dir.present()) {
        return std::nullopt;
    }
    LoadConfig config;
    config.size = image.read<uint32_t>(dir.rva);
    const auto has = [&](uint32_t offset, uint32_t width) { return config.size >= offset + width; };
    const auto read64 = [&](uint32_t offset) {
        return has(offset, 8) ? image.read<uint64_t>(dir.rva + offset) : 0;
    };

    config.security_cookie_va = read64(kSecurityCookie);
    config.guard_cf_check_va = read64(kGuardCfCheck);
    config.guard_cf_dispatch_va = read64(kGuardCfDispatch);
    if (has(kGuardFlags, 4)) {
        config.guard_flags = image.read<uint32_t>(dir.rva + kGuardFlags);
    }
    const uint32_t stride =
        4 + ((config.guard_flags & kGuardTableStrideMask) >> kGuardTableStrideShift);

    config.guard_cf_functions = read_guard_table(image, read64(kGuardCfFunctionTable),
                                                 read64(kGuardCfFunctionCount), stride);
    config.guard_eh_continuations = read_guard_table(image, read64(kGuardEhContinuationTable),
                                                     read64(kGuardEhContinuationCount), stride);
    return config;
}

} // namespace g6::pe
