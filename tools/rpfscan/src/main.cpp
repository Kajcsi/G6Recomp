// rpfscan command-line front end: reads the game's RPF9 archives in place and prints what their
// data areas hold (docs/rpf.md). It never decrypts a table of contents and never extracts a file;
// --out writes only an index. Everything it prints or writes is derived from the user's files, so
// it stays on their machine.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "g6/log.h"
#include "g6/mapped_file.h"
#include "g6/rpf9.h"
#include "g6/text.h"

namespace {

using namespace g6;
namespace fs = std::filesystem;

int usage() {
    fmt::print(
        stderr,
        "usage: rpfscan <command> <file or folder> [options]\n"
        "\n"
        "commands:\n"
        "  info        print an archive's header\n"
        "  scan        carve each archive's data area into zlib streams and files, and\n"
        "              print a census; a folder is searched for *.rpf\n"
        "              --threads <n>   threads per archive (default: one per processor)\n"
        "              --out <dir>     also write <archive>.tsv, an index of the files and\n"
        "                              gaps found; derived from the game, so keep it out of\n"
        "                              the repo\n");
    return 2;
}

double gib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

template <typename Key>
std::string join_counts(const std::map<Key, size_t>& counts, auto&& describe, size_t shown = 12) {
    std::vector<std::pair<size_t, Key>> by_count;
    by_count.reserve(counts.size());
    for (const auto& [key, count] : counts) {
        by_count.emplace_back(count, key);
    }
    std::ranges::stable_sort(by_count,
                             [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text;
    for (size_t i = 0; i < std::min(shown, by_count.size()); ++i) {
        text += fmt::format("{}{} {}", text.empty() ? "" : ", ", describe(by_count[i].second),
                            by_count[i].first);
    }
    if (by_count.size() > shown) {
        text += fmt::format(", {} more", by_count.size() - shown);
    }
    return text.empty() ? "none" : text;
}

// The first four bytes of a gap as text when they are printable, else as hex.
std::string head_text(std::span<const uint8_t> bytes) {
    if (bytes.size() < 4) {
        return "short";
    }
    if (std::all_of(bytes.begin(), bytes.begin() + 4, [](uint8_t c) { return c == 0; })) {
        return "zeros";
    }
    if (std::all_of(bytes.begin(), bytes.begin() + 4, [](uint8_t c) { return std::isprint(c); })) {
        return fmt::format("'{}'",
                           std::string_view(reinterpret_cast<const char*>(bytes.data()), 4));
    }
    return fmt::format("{:02x}{:02x}{:02x}{:02x}", bytes[0], bytes[1], bytes[2], bytes[3]);
}

std::string entry_kind(const rpf9::Entry& entry) {
    const std::string_view name = rpf9::kind_name(entry.head.kind);
    if (entry.head.kind == rpf9::Kind::kResource) {
        return fmt::format("{} {:#x}", name, entry.head.tag);
    }
    return std::string(name);
}

int cmd_info(const fs::path& file) {
    const MappedFile archive(file);
    const std::optional<rpf9::Header> header = rpf9::read_header(archive.bytes());
    if (!header) {
        LOG_ERROR(rpfscan, "{} is not an RPF9 archive", g6::utf8(file.native()));
        return 1;
    }
    fmt::print("file             {} ({} bytes)\n", g6::utf8(file.filename().native()),
               archive.bytes().size());
    fmt::print("table of contents {} bytes; data from {:#x}\n", header->toc_size,
               rpf9::data_start(*header));
    fmt::print("word 24          {} (reported as the entry count)\n", header->entry_count);
    fmt::print("names size       {}\n", header->names_size);
    fmt::print("words            08 {:#x}, 18 {:#x}, 1c {:#x}, 20 {:#x}, 22 {:#x}, 2c {:#x}, "
               "30 {:#x}\n",
               header->word_08, header->word_18, header->word_1c, header->word_20, header->word_22,
               header->flags, header->word_30);
    fmt::print("toc hash         {:016x}\n", header->toc_hash);
    return 0;
}

struct Totals {
    uint64_t bytes = 0;
    uint64_t compressed = 0;
    uint64_t inflated = 0;
    uint64_t gap_bytes = 0;
    size_t streams = 0;
    size_t entries = 0;
    size_t word_24 = 0;
    std::map<std::string, size_t> kinds;
};

void write_index(const fs::path& path, std::span<const uint8_t> archive, const rpf9::Carve& carve) {
    std::ofstream out(path);
    out << "offset\tstreams\tcompressed\tsize\tkind\tfirst_u32\n";
    for (const rpf9::Entry& entry : carve.entries) {
        out << fmt::format("{:#x}\t{}\t{}\t{}\t{}\t{:#x}\n", entry.offset, entry.streams,
                           entry.compressed, entry.size, entry_kind(entry), entry.head.tag);
    }
    for (const rpf9::Gap& gap : carve.gaps) {
        out << fmt::format("{:#x}\t0\t{}\t{}\tgap {}\t0\n", gap.offset, gap.size, gap.size,
                           head_text(archive.subspan(gap.offset)));
    }
    if (!out) {
        throw std::runtime_error(fmt::format("can't write {}", g6::utf8(path.native())));
    }
}

void scan_one(const fs::path& file, unsigned threads, const fs::path& out_dir, Totals& totals) {
    const MappedFile mapped(file);
    const std::span<const uint8_t> archive = mapped.bytes();
    const auto start = std::chrono::steady_clock::now();
    const rpf9::Carve carve = rpf9::carve(archive, threads);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    uint64_t compressed = 0;
    uint64_t inflated = 0;
    std::map<std::string, size_t> kinds;
    for (const rpf9::Entry& entry : carve.entries) {
        compressed += entry.compressed;
        inflated += entry.size;
        ++kinds[entry_kind(entry)];
        ++totals.kinds[entry_kind(entry)];
    }
    uint64_t gap_bytes = 0;
    std::map<std::string, size_t> gap_heads;
    for (const rpf9::Gap& gap : carve.gaps) {
        gap_bytes += gap.size;
        ++gap_heads[head_text(archive.subspan(gap.offset))];
    }
    const uint64_t data = archive.size() - carve.data_start;
    fmt::print(
        "{}: {:.2f} GiB, word 24 {}; carved {} streams into {} files "
        "({:.2f} GiB -> {:.2f} GiB); {} gaps, {:.2f} GiB ({:.1f}% of the data area); {:.0f} s\n",
        g6::utf8(file.filename().native()), gib(archive.size()), carve.header.entry_count,
        carve.streams.size(), carve.entries.size(), gib(compressed), gib(inflated),
        carve.gaps.size(), gib(gap_bytes),
        data == 0 ? 0.0 : 100.0 * static_cast<double>(gap_bytes) / static_cast<double>(data),
        seconds);
    fmt::print("  files: {}\n", join_counts(kinds, [](const std::string& kind) { return kind; }));
    fmt::print("  gaps start with: {}\n",
               join_counts(gap_heads, [](const std::string& head) { return head; }));
    std::fflush(stdout);

    totals.bytes += archive.size();
    totals.compressed += compressed;
    totals.inflated += inflated;
    totals.gap_bytes += gap_bytes;
    totals.streams += carve.streams.size();
    totals.entries += carve.entries.size();
    totals.word_24 += carve.header.entry_count;
    if (!out_dir.empty()) {
        fs::create_directories(out_dir);
        write_index(out_dir / (file.stem().native() + L".tsv"), archive, carve);
    }
}

int cmd_scan(const fs::path& target, std::span<const std::string_view> flags) {
    unsigned threads = 0;
    fs::path out_dir;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (flags[i] == "--threads" && i + 1 < flags.size()) {
            threads = static_cast<unsigned>(std::stoul(std::string(flags[++i])));
        } else if (flags[i] == "--out" && i + 1 < flags.size()) {
            out_dir = fs::path(flags[++i]);
        } else {
            LOG_ERROR(rpfscan, "bad option {}", flags[i]);
            return usage();
        }
    }
    std::vector<fs::path> files;
    if (fs::is_directory(target)) {
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(target)) {
            if (entry.is_regular_file() && entry.path().extension() == ".rpf") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
    } else {
        files.push_back(target);
    }
    Totals totals;
    size_t failed = 0;
    for (const fs::path& file : files) {
        try {
            scan_one(file, threads, out_dir, totals);
        } catch (const std::exception& error) {
            LOG_ERROR(rpfscan, "{}: {}", g6::utf8(file.native()), error.what());
            ++failed;
        }
    }
    if (files.size() > 1) {
        fmt::print("all {} archives: {:.2f} GiB; word 24 sums to {}; carved {} "
                   "streams into {} files ({:.2f} GiB -> {:.2f} GiB); gaps {:.2f} GiB\n",
                   files.size(), gib(totals.bytes), totals.word_24, totals.streams, totals.entries,
                   gib(totals.compressed), gib(totals.inflated), gib(totals.gap_bytes));
        fmt::print("  files: {}\n",
                   join_counts(totals.kinds, [](const std::string& kind) { return kind; }, 24));
    }
    return failed == 0 ? 0 : 1;
}

int run(int argc, wchar_t** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::string command = fs::path(argv[1]).string();
    const fs::path target(argv[2]);
    std::vector<std::string> flag_storage;
    for (int i = 3; i < argc; ++i) {
        flag_storage.push_back(fs::path(argv[i]).string());
    }
    const std::vector<std::string_view> flags(flag_storage.begin(), flag_storage.end());
    if (command == "info" && flags.empty()) {
        return cmd_info(target);
    }
    if (command == "scan") {
        return cmd_scan(target, flags);
    }
    return usage();
}

} // namespace

int wmain(int argc, wchar_t** argv) { // NOLINT(misc-use-internal-linkage): process entry point
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        LOG_ERROR(rpfscan, "{}", error.what());
    }
    return 1;
}
