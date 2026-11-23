#include "g6/gdk/game_save.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "g6/gdk/thread.h"
#include "g6/log.h"

// XGameSave on the per-user data directory (docs/gdk.md, "XGameSave"). Each configuration ID
// has a folder, and each container a folder in it holding its blobs and a manifest that names
// them. A submitted update writes its blobs to new files and then replaces the manifest in one
// rename, so a container is always either before or after an update, never between.

namespace fs = std::filesystem;

namespace g6::gdk {

namespace {

constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80070490);    // ERROR_NOT_FOUND
constexpr HRESULT kFileCorrupt = static_cast<HRESULT>(0x80070570); // ERROR_FILE_CORRUPT

// Whether `key` is reported for the first time.
bool first_report(std::string key) {
    static std::mutex mutex;
    static std::set<std::string> keys;
    const std::scoped_lock lock(mutex);
    return keys.insert(std::move(key)).second;
}

// ---- Names ----

char fold(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string folded(std::string_view name) {
    std::string out(name);
    std::ranges::transform(out, out.begin(), fold);
    return out;
}

// Names match without case, as on the console.
bool same_name(std::string_view first, std::string_view second) {
    return std::ranges::equal(first, second, {}, fold, fold);
}

bool has_prefix(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && same_name(name.substr(0, prefix.size()), prefix);
}

bool is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// The public rules: at most 256 characters. Up to the last '/', letters, digits, '_', and '/';
// after it a non-empty name of letters, digits, '_', '.', and '-' that doesn't end in '.' or
// hold "..".
bool valid_container_name(const char* name) {
    if (name == nullptr) {
        return false;
    }
    const std::string_view text(name);
    if (text.empty() || text.size() > kGameSaveMaxContainerName) {
        return false;
    }
    const size_t slash = text.rfind('/');
    const std::string_view path = slash == std::string_view::npos ? "" : text.substr(0, slash);
    const std::string_view file = slash == std::string_view::npos ? text : text.substr(slash + 1);
    const bool path_ok =
        std::ranges::all_of(path, [](char c) { return is_alnum(c) || c == '_' || c == '/'; });
    const bool file_ok = std::ranges::all_of(
        file, [](char c) { return is_alnum(c) || c == '_' || c == '.' || c == '-'; });
    return path_ok && file_ok && !file.empty() && file.back() != '.' &&
           file.find("..") == std::string_view::npos;
}

bool valid_blob_name(const char* name) {
    return name != nullptr && name[0] != '\0';
}

uint64_t fnv1a(std::string_view text) {
    uint64_t hash = 0xcbf29ce484222325;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3;
    }
    return hash;
}

constexpr std::string_view kHex = "0123456789abcdef";
constexpr size_t kLongestComponent = 100;
constexpr size_t kKeptBeforeHash = 80;

// The host folder name for a container or configuration: a kind letter and '-', then the name
// folded to lower case with every byte outside [a-z0-9_.-] escaped as %xx. The prefix keeps it
// clear of "." and of device names such as NUL; folding makes names that match without case
// share a folder on any volume. A long result keeps its start and ends in '~' and a hash of the
// whole name. The real name is in the manifest, so the folder name is never read back.
std::wstring component(char kind, std::string_view name) {
    std::string out{kind, '-'};
    for (const char c : folded(name)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out += c;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 15];
        }
    }
    if (out.back() == '.') { // Windows drops a trailing dot
        out.back() = '%';
        out += "2e";
    }
    if (out.size() > kLongestComponent) {
        out.resize(kKeptBeforeHash);
        out += fmt::format("~{:016x}", fnv1a(folded(name)));
    }
    return {out.begin(), out.end()};
}

// A manifest field: bytes up to space, '%', and DEL escaped as %xx.
std::string escape_field(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == '%' || byte == 0x7f) {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 15];
        } else {
            out += c;
        }
    }
    return out;
}

std::optional<std::string> unescape_field(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) {
            return std::nullopt;
        }
        unsigned value = 0;
        const auto [end, error] =
            std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
        if (error != std::errc{} || end != text.data() + i + 3) {
            return std::nullopt;
        }
        out += static_cast<char>(value);
        i += 2;
    }
    return out;
}

// Names handed back to the guest by reads, kept for the life of the process.
const char* intern(const std::string& name) {
    static std::mutex mutex;
    static std::set<std::string, std::less<>> names;
    const std::scoped_lock lock(mutex);
    return names.insert(name).first->c_str();
}

// ---- Files ----

// A path for the log, in UTF-8 (path::string() throws for characters the code page lacks).
std::string shown(const fs::path& path) {
    const std::wstring& text = path.native();
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

// The ASCII file name of a folder entry, or none.
std::optional<std::string> ascii_name(const fs::path& path) {
    const std::wstring leaf = path.filename().native();
    if (!std::ranges::all_of(leaf, [](wchar_t c) { return c > 0 && c < 0x80; })) {
        return std::nullopt;
    }
    std::string name;
    for (const wchar_t c : leaf) {
        name += static_cast<char>(c);
    }
    return name;
}

HRESULT last_error() {
    const DWORD error = GetLastError();
    if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) {
        return kGameSaveOutOfLocalStorage;
    }
    return HRESULT_FROM_WIN32(error);
}

HRESULT result_of(const std::error_code& error) {
    if (error.value() == ERROR_DISK_FULL || error.value() == ERROR_HANDLE_DISK_FULL) {
        return kGameSaveOutOfLocalStorage;
    }
    return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
}

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = delete;
    Handle& operator=(Handle&&) = delete;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

// Writes the whole file and flushes it to the disk before returning.
HRESULT write_file(const fs::path& path, const uint8_t* data, size_t size) {
    const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        return last_error();
    }
    size_t done = 0;
    while (done < size) {
        const auto chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1U << 30));
        DWORD written = 0;
        if (WriteFile(file.value, data + done, chunk, &written, nullptr) == FALSE) {
            return last_error();
        }
        if (written == 0) {
            return E_FAIL;
        }
        done += written;
    }
    return FlushFileBuffers(file.value) != FALSE ? S_OK : last_error();
}

// Reads a file that must be exactly `size` bytes.
HRESULT read_file(const fs::path& path, uint64_t size, std::vector<uint8_t>& data) {
    const Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? kFileCorrupt : last_error();
    }
    LARGE_INTEGER actual{};
    if (GetFileSizeEx(file.value, &actual) == FALSE) {
        return last_error();
    }
    if (std::cmp_not_equal(actual.QuadPart, size)) {
        return kFileCorrupt;
    }
    data.resize(size);
    size_t done = 0;
    while (done < size) {
        const auto chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1U << 30));
        DWORD read = 0;
        if (ReadFile(file.value, data.data() + done, chunk, &read, nullptr) == FALSE) {
            return last_error();
        }
        if (read == 0) {
            return kFileCorrupt;
        }
        done += read;
    }
    return S_OK;
}

// ---- Containers on disk ----

constexpr std::wstring_view kManifest = L"manifest.txt";
constexpr std::wstring_view kManifestNext = L"manifest.tmp";
constexpr std::string_view kManifestHeader = "g6recomp container 1";

struct BlobEntry {
    std::string name;
    std::string file; // "<number>.blob" in the container's folder
    uint64_t size = 0;
};

struct Manifest {
    std::string name;
    std::string display_name;
    int64_t modified = 0;
    std::vector<BlobEntry> blobs;

    uint64_t total() const {
        uint64_t sum = 0;
        for (const BlobEntry& blob : blobs) {
            sum += blob.size;
        }
        return sum;
    }
};

// A blob file name we wrote: digits, then ".blob". Anything else in a manifest is refused, so a
// damaged one can't point outside its folder.
bool is_blob_file(std::string_view file) {
    constexpr std::string_view kSuffix = ".blob";
    if (!file.ends_with(kSuffix) || file.size() == kSuffix.size()) {
        return false;
    }
    return std::ranges::all_of(file.substr(0, file.size() - kSuffix.size()),
                               [](char c) { return c >= '0' && c <= '9'; });
}

enum class Stored : uint8_t { Missing, Ok, Corrupt };

std::optional<Manifest> parse_manifest(std::ifstream& in) {
    std::string line;
    if (!std::getline(in, line) || line != kManifestHeader) {
        return std::nullopt;
    }
    Manifest manifest;
    while (std::getline(in, line)) {
        const size_t space = line.find(' ');
        const std::string_view key = std::string_view(line).substr(0, space);
        const std::string_view rest = space == std::string::npos
                                          ? std::string_view{}
                                          : std::string_view(line).substr(space + 1);
        if (key == "name" || key == "display") {
            std::optional<std::string> value = unescape_field(rest);
            if (!value) {
                return std::nullopt;
            }
            (key == "name" ? manifest.name : manifest.display_name) = std::move(*value);
        } else if (key == "modified") {
            const auto [end, error] =
                std::from_chars(rest.data(), rest.data() + rest.size(), manifest.modified);
            if (error != std::errc{} || end != rest.data() + rest.size()) {
                return std::nullopt;
            }
        } else if (key == "blob") {
            // blob <file> <size> <name>
            const size_t first = rest.find(' ');
            const size_t second =
                first == std::string_view::npos ? first : rest.find(' ', first + 1);
            if (second == std::string_view::npos) {
                return std::nullopt;
            }
            BlobEntry blob;
            blob.file = std::string(rest.substr(0, first));
            const std::string_view size = rest.substr(first + 1, second - first - 1);
            const auto [end, error] =
                std::from_chars(size.data(), size.data() + size.size(), blob.size);
            std::optional<std::string> name = unescape_field(rest.substr(second + 1));
            if (error != std::errc{} || end != size.data() + size.size() || !name ||
                name->empty() || !is_blob_file(blob.file)) {
                return std::nullopt;
            }
            blob.name = std::move(*name);
            manifest.blobs.push_back(std::move(blob));
        } else {
            return std::nullopt;
        }
    }
    if (manifest.name.empty()) {
        return std::nullopt;
    }
    return manifest;
}

Stored read_manifest(const fs::path& folder, Manifest& manifest) {
    const fs::path path = folder / kManifest;
    std::error_code error;
    if (!fs::exists(path, error)) {
        return Stored::Missing;
    }
    std::ifstream in(path, std::ios::binary);
    std::optional<Manifest> parsed = in ? parse_manifest(in) : std::nullopt;
    if (!parsed) {
        if (first_report("corrupt " + shown(path))) {
            LOG_ERROR(gdk, "XGameSave: {} can't be read; the container is left alone", shown(path));
        }
        return Stored::Corrupt;
    }
    manifest = std::move(*parsed);
    return Stored::Ok;
}

HRESULT write_manifest(const fs::path& folder, const Manifest& manifest) {
    std::string text = fmt::format("{}\nname {}\ndisplay {}\nmodified {}\n", kManifestHeader,
                                   escape_field(manifest.name), escape_field(manifest.display_name),
                                   manifest.modified);
    for (const BlobEntry& blob : manifest.blobs) {
        text += fmt::format("blob {} {} {}\n", blob.file, blob.size, escape_field(blob.name));
    }
    const fs::path next = folder / kManifestNext;
    const HRESULT written =
        write_file(next, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    if (FAILED(written)) {
        return written;
    }
    if (MoveFileExW(next.c_str(), (folder / kManifest).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        return last_error();
    }
    return S_OK;
}

// Every disk operation runs under this lock, so updates and deletes apply one at a time.
std::mutex& disk_lock() {
    static std::mutex mutex;
    return mutex;
}

struct ProviderState {
    fs::path root; // <saves>\<configuration>
    std::atomic<bool> open{true};
};

using Provider = std::shared_ptr<ProviderState>;

fs::path container_folder(const ProviderState& provider, std::string_view name) {
    return provider.root / component('c', name);
}

// The provider's containers with a readable manifest, in name order. Disk lock held.
std::vector<Manifest> load_containers(const ProviderState& provider) {
    std::vector<Manifest> found;
    std::error_code error;
    fs::directory_iterator entry(provider.root, error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        std::error_code ignored;
        if (!entry->is_directory(ignored) ||
            !entry->path().filename().native().starts_with(L"c-")) {
            continue;
        }
        Manifest manifest;
        if (read_manifest(entry->path(), manifest) == Stored::Ok) {
            found.push_back(std::move(manifest));
        }
    }
    std::ranges::sort(found, {}, [](const Manifest& manifest) { return folded(manifest.name); });
    return found;
}

uint64_t used_bytes(const ProviderState& provider) {
    uint64_t used = 0;
    for (const Manifest& manifest : load_containers(provider)) {
        used += manifest.total();
    }
    return used;
}

int64_t remaining_quota(const ProviderState& provider) {
    const std::scoped_lock lock(disk_lock());
    const uint64_t used = used_bytes(provider);
    return used >= static_cast<uint64_t>(kGameSaveQuota)
               ? 0
               : kGameSaveQuota - static_cast<int64_t>(used);
}

// One past the highest blob file number in the folder.
uint64_t next_file_number(const fs::path& folder) {
    uint64_t next = 1;
    std::error_code error;
    fs::directory_iterator entry(folder, error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        const std::optional<std::string> name = ascii_name(entry->path());
        if (name && is_blob_file(*name)) {
            uint64_t number = 0;
            std::from_chars(name->data(), name->data() + name->size() - 5, number);
            next = std::max(next, number + 1);
        }
    }
    return next;
}

// Removes the folder's files that the manifest doesn't name: replaced blobs, and anything left
// by an update that never finished.
void remove_unreferenced(const fs::path& folder, const Manifest& manifest) {
    std::set<std::wstring> keep{std::wstring(kManifest)};
    for (const BlobEntry& blob : manifest.blobs) {
        keep.emplace(blob.file.begin(), blob.file.end());
    }
    std::vector<fs::path> stale;
    std::error_code error;
    fs::directory_iterator entry(folder, error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        std::error_code ignored;
        if (entry->is_regular_file(ignored) && !keep.contains(entry->path().filename().native())) {
            stale.push_back(entry->path());
        }
    }
    for (const fs::path& path : stale) {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
}

// Writes or deletes one blob; the last change to a name wins.
struct Change {
    std::string name;
    std::optional<std::vector<uint8_t>> data; // none: delete
};

HRESULT commit(const ProviderState& provider, const std::string& container,
               const std::optional<std::string>& display_name, const std::vector<Change>& changes) {
    const std::scoped_lock lock(disk_lock());
    const fs::path folder = container_folder(provider, container);
    Manifest manifest;
    const Stored stored = read_manifest(folder, manifest);
    if (stored == Stored::Corrupt) {
        return kFileCorrupt;
    }
    const uint64_t before = stored == Stored::Ok ? manifest.total() : 0;
    if (stored == Stored::Missing) {
        manifest.name = container;
    }
    if (display_name) {
        manifest.display_name = *display_name;
    }

    // The blobs after the update, those to write marked by an empty file name.
    std::map<std::string, const Change*> last;
    for (const Change& change : changes) {
        last[folded(change.name)] = &change;
    }
    std::vector<BlobEntry> blobs = std::move(manifest.blobs);
    std::map<size_t, const std::vector<uint8_t>*> writes;
    for (const auto& [key, change] : last) {
        auto found = std::ranges::find_if(
            blobs, [&](const BlobEntry& blob) { return same_name(blob.name, change->name); });
        if (!change->data) {
            if (found != blobs.end()) {
                blobs.erase(found);
            }
            continue;
        }
        if (found == blobs.end()) {
            blobs.push_back({.name = {}, .file = {}, .size = 0});
            found = std::prev(blobs.end());
        }
        found->name = change->name; // the latest spelling
        found->size = change->data->size();
        found->file.clear();
    }
    uint64_t after = 0;
    for (size_t i = 0; i < blobs.size(); ++i) {
        after += blobs[i].size;
        if (blobs[i].file.empty()) {
            writes[i] = &*last.at(folded(blobs[i].name))->data;
        }
    }
    if (used_bytes(provider) - before + after > static_cast<uint64_t>(kGameSaveQuota)) {
        return kGameSaveQuotaExceeded;
    }

    std::error_code error;
    fs::create_directories(folder, error);
    if (error) {
        return result_of(error);
    }
    // New blobs go to new files, so the old manifest stays whole until it is replaced. If the
    // update fails before that, only the new files are removed.
    std::vector<fs::path> created;
    const auto discard = [&created] {
        for (const fs::path& path : created) {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    };
    uint64_t number = next_file_number(folder);
    for (const auto& [index, data] : writes) {
        blobs[index].file = fmt::format("{}.blob", number++);
        created.push_back(folder / blobs[index].file);
        const HRESULT written = write_file(created.back(), data->data(), data->size());
        if (FAILED(written)) {
            discard();
            return written;
        }
    }
    manifest.blobs = std::move(blobs);
    manifest.modified = static_cast<int64_t>(std::time(nullptr));
    const HRESULT written = write_manifest(folder, manifest);
    if (FAILED(written)) {
        discard();
        return written;
    }
    remove_unreferenced(folder, manifest);
    return S_OK;
}

// Removes a deleted container's folder, or one left by an earlier run.
void remove_folder(const fs::path& folder) {
    std::error_code error;
    fs::remove_all(folder, error);
    if (error && first_report("remove " + shown(folder))) {
        LOG_WARN(gdk, "XGameSave: {} couldn't be removed ({}); it is tried again next run",
                 shown(folder), error.message());
    }
}

HRESULT delete_container(const ProviderState& provider, const std::string& name) {
    static std::atomic<uint64_t> deleted{0};
    const std::scoped_lock lock(disk_lock());
    const fs::path folder = container_folder(provider, name);
    std::error_code error;
    if (!fs::exists(folder, error)) {
        return S_OK;
    }
    // Moving it aside removes the container in one step; its files go after.
    const fs::path aside = provider.root / (L"!deleted-" + std::to_wstring(GetCurrentProcessId()) +
                                            L"-" + std::to_wstring(++deleted));
    if (MoveFileExW(folder.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
        return last_error();
    }
    remove_folder(aside);
    return S_OK;
}

struct Loaded {
    const char* name;
    std::vector<uint8_t> data;
};

// The named blobs, or all of them in name order.
HRESULT load_blobs(const ProviderState& provider, const std::string& container,
                   const std::optional<std::vector<std::string>>& names,
                   std::vector<Loaded>& loaded) {
    const std::scoped_lock lock(disk_lock());
    const fs::path folder = container_folder(provider, container);
    Manifest manifest;
    const Stored stored = read_manifest(folder, manifest);
    if (stored == Stored::Corrupt) {
        return kFileCorrupt;
    }
    std::vector<const BlobEntry*> chosen;
    if (!names) {
        for (const BlobEntry& blob : manifest.blobs) {
            chosen.push_back(&blob);
        }
        std::ranges::sort(chosen, {}, [](const BlobEntry* blob) { return folded(blob->name); });
    } else {
        for (const std::string& name : *names) {
            const auto found = std::ranges::find_if(
                manifest.blobs, [&](const BlobEntry& blob) { return same_name(blob.name, name); });
            if (found == manifest.blobs.end()) {
                return kGameSaveBlobNotFound;
            }
            chosen.push_back(&*found);
        }
    }
    for (const BlobEntry* blob : chosen) {
        Loaded entry{.name = intern(blob->name), .data = {}};
        const HRESULT read = read_file(folder / blob->file, blob->size, entry.data);
        if (FAILED(read)) {
            LOG_ERROR(gdk, "XGameSave: blob {} of {} can't be read ({:#x})", blob->name, container,
                      static_cast<uint32_t>(read));
            return read;
        }
        loaded.push_back(std::move(entry));
    }
    return S_OK;
}

size_t layout_size(const std::vector<Loaded>& blobs) {
    size_t size = blobs.size() * sizeof(XGameSaveBlob);
    for (const Loaded& blob : blobs) {
        size += blob.data.size();
    }
    return size;
}

// The records, then each blob's data in order.
void lay_out(const std::vector<Loaded>& blobs, void* buffer) {
    auto* records = static_cast<XGameSaveBlob*>(buffer);
    auto* next = static_cast<uint8_t*>(buffer) + blobs.size() * sizeof(XGameSaveBlob);
    for (size_t i = 0; i < blobs.size(); ++i) {
        records[i].info.name = blobs[i].name;
        records[i].info.size = static_cast<uint32_t>(blobs[i].data.size());
        records[i].data = next;
        if (!blobs[i].data.empty()) {
            std::memcpy(next, blobs[i].data.data(), blobs[i].data.size());
        }
        next += blobs[i].data.size();
    }
}

} // namespace

// ---- Handles ----

struct XGameSaveProvider {
    Provider state;
};

struct XGameSaveContainer {
    Provider provider;
    std::string name;
};

struct XGameSaveUpdate {
    Provider provider;
    std::string container;
    std::optional<std::string> display_name;
    std::mutex mutex;
    std::vector<Change> changes;
    size_t bytes = 0;
    bool submitted = false;
};

namespace {

enum class Kind : uint8_t { Provider, Container, Update };

struct Handles {
    std::mutex mutex;
    std::unordered_map<const void*, Kind> live;
};

Handles& handles() {
    static Handles instance;
    return instance;
}

template <typename T>
T* add_handle(std::unique_ptr<T> object, Kind kind) {
    T* raw = object.release();
    Handles& all = handles();
    const std::scoped_lock lock(all.mutex);
    all.live.emplace(raw, kind);
    return raw;
}

// E_INVALIDARG for null, kGameSaveHandleExpired for a handle that is closed or of another kind.
HRESULT check(const void* handle, Kind kind) {
    if (handle == nullptr) {
        return E_INVALIDARG;
    }
    Handles& all = handles();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.live.find(handle);
    return found != all.live.end() && found->second == kind ? S_OK : kGameSaveHandleExpired;
}

bool remove_handle(const void* handle, Kind kind) {
    Handles& all = handles();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.live.find(handle);
    if (found == all.live.end() || found->second != kind) {
        return false;
    }
    all.live.erase(found);
    return true;
}

HRESULT check_provider(XGameSaveProviderHandle provider) {
    return check(provider, Kind::Provider);
}

HRESULT check_container(XGameSaveContainerHandle container) {
    const HRESULT checked = check(container, Kind::Container);
    if (FAILED(checked)) {
        return checked;
    }
    return container->provider->open ? S_OK : kGameSaveHandleExpired;
}

HRESULT check_update(XGameSaveUpdateHandle update) {
    const HRESULT checked = check(update, Kind::Update);
    if (FAILED(checked)) {
        return checked;
    }
    return update->provider->open ? S_OK : kGameSaveHandleExpired;
}

// The synchronous forms that touch the disk refuse a time-sensitive thread.
HRESULT check_blocking(const char* function) {
    if (!XThreadIsTimeSensitive()) {
        return S_OK;
    }
    if (first_report(std::string("blocking ") + function)) {
        LOG_WARN(gdk, "{} called on a time-sensitive thread: E_GS_ASYNC_FUNCTION_REQUIRED",
                 function);
    }
    return kGameSaveAsyncFunctionRequired;
}

// ---- Providers ----

struct Settings {
    std::mutex mutex;
    fs::path root;
};

Settings& settings() {
    static Settings instance;
    return instance;
}

fs::path save_root() {
    Settings& all = settings();
    const std::scoped_lock lock(all.mutex);
    return all.root;
}

HRESULT check_provider_request(XUserHandle user, const char* configuration_id) {
    if (!is_offline_user(user)) {
        return E_INVALIDARG;
    }
    if (configuration_id == nullptr || configuration_id[0] == '\0') {
        return kGameSaveNoServiceConfiguration;
    }
    return S_OK;
}

// Opens the configuration's folder, and removes containers a crash left half deleted.
HRESULT open_provider(const std::string& configuration_id, Provider& provider) {
    const fs::path root = save_root();
    if (root.empty()) {
        LOG_ERROR(gdk, "XGameSave: no save directory is set; failing with E_GS_NO_ACCESS");
        return kGameSaveNoAccess;
    }
    auto state = std::make_shared<ProviderState>();
    state->root = root / component('s', configuration_id);
    const std::scoped_lock lock(disk_lock());
    std::error_code error;
    fs::create_directories(state->root, error);
    if (error) {
        LOG_ERROR(gdk, "XGameSave: {} can't be created ({})", shown(state->root), error.message());
        return result_of(error);
    }
    std::vector<fs::path> aside;
    fs::directory_iterator entry(state->root, error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        if (entry->path().filename().native().starts_with(L"!deleted-")) {
            aside.push_back(entry->path());
        }
    }
    for (const fs::path& folder : aside) {
        remove_folder(folder);
    }
    if (first_report("provider " + configuration_id)) {
        LOG_INFO(gdk, "XGameSave: configuration {} saves to {}", configuration_id,
                 shown(state->root));
    }
    provider = std::move(state);
    return S_OK;
}

XGameSaveProviderHandle new_provider(Provider state) {
    return add_handle(std::make_unique<XGameSaveProvider>(XGameSaveProvider{std::move(state)}),
                      Kind::Provider);
}

// ---- Asynchronous calls ----

// A call whose work runs on the queue's work port. `work` returns the status and sets the size
// of the result; `fetch` writes the result into the caller's buffer.
struct Job {
    std::function<HRESULT(size_t& size)> work;
    std::function<void(void* buffer)> fetch;
};

HRESULT CALLBACK job_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* job = static_cast<Job*>(data->context);
    switch (op) {
    case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork: {
        size_t size = 0;
        const HRESULT result = job->work(size);
        XAsyncComplete(data->async, result, SUCCEEDED(result) ? size : 0);
        return S_OK;
    }
    case XAsyncOp::GetResult:
        if (job->fetch) {
            job->fetch(data->buffer);
        }
        return S_OK;
    case XAsyncOp::Cancel: return S_OK;
    case XAsyncOp::Cleanup: delete job; return S_OK;
    }
    return S_OK;
}

template <auto Function>
const void* identity() {
    return reinterpret_cast<const void*>(Function);
}

template <auto Function>
HRESULT begin_job(XAsyncBlock* async, const char* name, std::unique_ptr<Job> job) {
    Job* raw = job.release();
    const HRESULT begun = XAsyncBegin(async, raw, identity<Function>(), name, &job_provider);
    if (FAILED(begun)) {
        delete raw; // a call that never began has no cleanup step
    }
    return begun;
}

} // namespace

void set_game_save_directory(const fs::path& root) {
    Settings& all = settings();
    const std::scoped_lock lock(all.mutex);
    all.root = root;
}

HRESULT XGameSaveInitializeProvider(XUserHandle requesting_user, const char* configuration_id,
                                    bool /*sync_on_demand*/, XGameSaveProviderHandle* provider) {
    HRESULT result = check_provider_request(requesting_user, configuration_id);
    if (SUCCEEDED(result) && provider == nullptr) {
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result)) {
        result = check_blocking("XGameSaveInitializeProvider");
    }
    Provider state;
    if (SUCCEEDED(result)) {
        result = open_provider(configuration_id, state);
    }
    if (SUCCEEDED(result)) {
        *provider = new_provider(std::move(state));
    }
    return result;
}

HRESULT XGameSaveInitializeProviderAsync(XUserHandle requesting_user, const char* configuration_id,
                                         bool /*sync_on_demand*/, XAsyncBlock* async) {
    const HRESULT checked = check_provider_request(requesting_user, configuration_id);
    if (FAILED(checked)) {
        return checked;
    }
    auto opened = std::make_shared<Provider>();
    auto job = std::make_unique<Job>();
    job->work = [configuration = std::string(configuration_id), opened](size_t& size) {
        size = sizeof(XGameSaveProviderHandle);
        return open_provider(configuration, *opened);
    };
    job->fetch = [opened](void* buffer) {
        *static_cast<XGameSaveProviderHandle*>(buffer) = new_provider(*opened);
    };
    return begin_job<&XGameSaveInitializeProviderAsync>(async, "XGameSaveInitializeProviderAsync",
                                                        std::move(job));
}

HRESULT XGameSaveInitializeProviderResult(XAsyncBlock* async, XGameSaveProviderHandle* provider) {
    if (provider == nullptr) {
        return E_INVALIDARG; // the result stays to fetch
    }
    return XAsyncGetResult(async, identity<&XGameSaveInitializeProviderAsync>(),
                           sizeof(XGameSaveProviderHandle), static_cast<void*>(provider), nullptr);
}

void XGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    if (provider == nullptr || !remove_handle(provider, Kind::Provider)) {
        return;
    }
    provider->state->open = false;
    delete provider;
}

HRESULT XGameSaveGetRemainingQuota(XGameSaveProviderHandle provider, int64_t* quota) {
    HRESULT result = check_provider(provider);
    if (SUCCEEDED(result) && quota == nullptr) {
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result)) {
        result = check_blocking("XGameSaveGetRemainingQuota");
    }
    if (SUCCEEDED(result)) {
        *quota = remaining_quota(*provider->state);
    }
    return result;
}

HRESULT XGameSaveGetRemainingQuotaAsync(XGameSaveProviderHandle provider, XAsyncBlock* async) {
    const HRESULT checked = check_provider(provider);
    if (FAILED(checked)) {
        return checked;
    }
    auto quota = std::make_shared<int64_t>(0);
    auto job = std::make_unique<Job>();
    job->work = [state = provider->state, quota](size_t& size) {
        *quota = remaining_quota(*state);
        size = sizeof(int64_t);
        return S_OK;
    };
    job->fetch = [quota](void* buffer) { std::memcpy(buffer, quota.get(), sizeof(int64_t)); };
    return begin_job<&XGameSaveGetRemainingQuotaAsync>(async, "XGameSaveGetRemainingQuotaAsync",
                                                       std::move(job));
}

HRESULT XGameSaveGetRemainingQuotaResult(XAsyncBlock* async, int64_t* quota) {
    if (quota == nullptr) {
        return E_INVALIDARG;
    }
    return XAsyncGetResult(async, identity<&XGameSaveGetRemainingQuotaAsync>(), sizeof(int64_t),
                           quota, nullptr);
}

HRESULT XGameSaveDeleteContainer(XGameSaveProviderHandle provider, const char* container_name) {
    HRESULT result = check_provider(provider);
    if (SUCCEEDED(result) && !valid_container_name(container_name)) {
        result = kGameSaveInvalidContainerName;
    }
    if (SUCCEEDED(result)) {
        result = check_blocking("XGameSaveDeleteContainer");
    }
    return SUCCEEDED(result) ? delete_container(*provider->state, container_name) : result;
}

HRESULT XGameSaveDeleteContainerAsync(XGameSaveProviderHandle provider, const char* container_name,
                                      XAsyncBlock* async) {
    const HRESULT checked = check_provider(provider);
    if (FAILED(checked)) {
        return checked;
    }
    if (!valid_container_name(container_name)) {
        return kGameSaveInvalidContainerName;
    }
    auto job = std::make_unique<Job>();
    job->work = [state = provider->state, name = std::string(container_name)](size_t& size) {
        size = 0;
        return delete_container(*state, name);
    };
    return begin_job<&XGameSaveDeleteContainerAsync>(async, "XGameSaveDeleteContainerAsync",
                                                     std::move(job));
}

HRESULT XGameSaveDeleteContainerResult(XAsyncBlock* async) {
    return XAsyncGetResult(async, identity<&XGameSaveDeleteContainerAsync>(), 0, nullptr, nullptr);
}

namespace {

// Calls `callback` for each container whose name starts with `prefix`, outside the disk lock,
// until it returns false.
HRESULT enumerate_containers(XGameSaveProviderHandle provider, std::string_view prefix,
                             void* context, XGameSaveContainerInfoCallback* callback) {
    const HRESULT checked = check_provider(provider);
    if (FAILED(checked)) {
        return checked;
    }
    if (callback == nullptr) {
        return E_INVALIDARG;
    }
    std::vector<Manifest> containers;
    {
        const std::scoped_lock lock(disk_lock());
        containers = load_containers(*provider->state);
    }
    for (const Manifest& manifest : containers) {
        if (!has_prefix(manifest.name, prefix)) {
            continue;
        }
        const XGameSaveContainerInfo info{
            .name = manifest.name.c_str(),
            .display_name = manifest.display_name.c_str(),
            .blob_count = static_cast<uint32_t>(manifest.blobs.size()),
            .total_size = manifest.total(),
            .last_modified_time = manifest.modified,
            .needs_sync = false,
        };
        if (!callback(&info, context)) {
            break;
        }
    }
    return S_OK;
}

HRESULT enumerate_blobs(XGameSaveContainerHandle container, std::string_view prefix, void* context,
                        XGameSaveBlobInfoCallback* callback) {
    const HRESULT checked = check_container(container);
    if (FAILED(checked)) {
        return checked;
    }
    if (callback == nullptr) {
        return E_INVALIDARG;
    }
    Manifest manifest;
    {
        const std::scoped_lock lock(disk_lock());
        if (read_manifest(container_folder(*container->provider, container->name), manifest) ==
            Stored::Corrupt) {
            return kFileCorrupt;
        }
    }
    std::ranges::sort(manifest.blobs, {}, [](const BlobEntry& blob) { return folded(blob.name); });
    for (const BlobEntry& blob : manifest.blobs) {
        if (!has_prefix(blob.name, prefix)) {
            continue;
        }
        const XGameSaveBlobInfo info{.name = blob.name.c_str(),
                                     .size = static_cast<uint32_t>(blob.size)};
        if (!callback(&info, context)) {
            break;
        }
    }
    return S_OK;
}

} // namespace

HRESULT XGameSaveGetContainerInfo(XGameSaveProviderHandle provider, const char* container_name,
                                  void* context, XGameSaveContainerInfoCallback* callback) {
    const HRESULT checked = check_provider(provider);
    if (FAILED(checked)) {
        return checked;
    }
    if (!valid_container_name(container_name)) {
        return kGameSaveInvalidContainerName;
    }
    if (callback == nullptr) {
        return E_INVALIDARG;
    }
    Manifest manifest;
    Stored stored = Stored::Missing;
    {
        const std::scoped_lock lock(disk_lock());
        stored = read_manifest(container_folder(*provider->state, container_name), manifest);
    }
    if (stored != Stored::Ok) {
        return stored == Stored::Missing ? kNotFound : kFileCorrupt;
    }
    const XGameSaveContainerInfo info{
        .name = manifest.name.c_str(),
        .display_name = manifest.display_name.c_str(),
        .blob_count = static_cast<uint32_t>(manifest.blobs.size()),
        .total_size = manifest.total(),
        .last_modified_time = manifest.modified,
        .needs_sync = false,
    };
    callback(&info, context);
    return S_OK;
}

HRESULT XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle provider, void* context,
                                        XGameSaveContainerInfoCallback* callback) {
    return enumerate_containers(provider, {}, context, callback);
}

HRESULT XGameSaveEnumerateContainerInfoByName(XGameSaveProviderHandle provider,
                                              const char* container_name_prefix, void* context,
                                              XGameSaveContainerInfoCallback* callback) {
    return enumerate_containers(
        provider, container_name_prefix != nullptr ? container_name_prefix : "", context, callback);
}

HRESULT XGameSaveCreateContainer(XGameSaveProviderHandle provider, const char* container_name,
                                 XGameSaveContainerHandle* container) {
    const HRESULT checked = check_provider(provider);
    if (FAILED(checked)) {
        return checked;
    }
    if (!valid_container_name(container_name)) {
        return kGameSaveInvalidContainerName;
    }
    if (container == nullptr) {
        return E_INVALIDARG;
    }
    *container = add_handle(
        std::make_unique<XGameSaveContainer>(XGameSaveContainer{provider->state, container_name}),
        Kind::Container);
    return S_OK;
}

void XGameSaveCloseContainer(XGameSaveContainerHandle container) {
    if (container != nullptr && remove_handle(container, Kind::Container)) {
        delete container;
    }
}

HRESULT XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle container, void* context,
                                   XGameSaveBlobInfoCallback* callback) {
    return enumerate_blobs(container, {}, context, callback);
}

HRESULT XGameSaveEnumerateBlobInfoByName(XGameSaveContainerHandle container,
                                         const char* blob_name_prefix, void* context,
                                         XGameSaveBlobInfoCallback* callback) {
    return enumerate_blobs(container, blob_name_prefix != nullptr ? blob_name_prefix : "", context,
                           callback);
}

namespace {

std::optional<std::vector<std::string>> requested_names(const char** blob_names, uint32_t count) {
    if (blob_names == nullptr) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    names.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        names.emplace_back(blob_names[i] != nullptr ? blob_names[i] : "");
    }
    return names;
}

} // namespace

HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle container, const char** blob_names,
                              uint32_t* count_of_blobs, size_t blobs_size,
                              XGameSaveBlob* blob_data) {
    HRESULT result = check_container(container);
    if (SUCCEEDED(result) && count_of_blobs == nullptr) {
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result)) {
        result = check_blocking("XGameSaveReadBlobData");
    }
    std::vector<Loaded> loaded;
    if (SUCCEEDED(result)) {
        result = load_blobs(*container->provider, container->name,
                            requested_names(blob_names, *count_of_blobs), loaded);
    }
    if (SUCCEEDED(result)) {
        const size_t needed = layout_size(loaded);
        if (needed > blobs_size || (needed != 0 && blob_data == nullptr)) {
            result = kGameSaveProvidedBufferTooSmall;
        } else {
            lay_out(loaded, blob_data);
            *count_of_blobs = static_cast<uint32_t>(loaded.size());
        }
    }
    return result;
}

HRESULT XGameSaveReadBlobDataAsync(XGameSaveContainerHandle container, const char** blob_names,
                                   uint32_t count_of_blobs, XAsyncBlock* async) {
    const HRESULT checked = check_container(container);
    if (FAILED(checked)) {
        return checked;
    }
    auto loaded = std::make_shared<std::vector<Loaded>>();
    auto job = std::make_unique<Job>();
    job->work = [state = container->provider, name = container->name,
                 names = requested_names(blob_names, count_of_blobs), loaded](size_t& size) {
        const HRESULT result = load_blobs(*state, name, names, *loaded);
        size = layout_size(*loaded);
        return result;
    };
    job->fetch = [loaded](void* buffer) { lay_out(*loaded, buffer); };
    return begin_job<&XGameSaveReadBlobDataAsync>(async, "XGameSaveReadBlobDataAsync",
                                                  std::move(job));
}

HRESULT XGameSaveReadBlobDataResult(XAsyncBlock* async, size_t blobs_size, XGameSaveBlob* blob_data,
                                    uint32_t* count_of_blobs) {
    if (count_of_blobs == nullptr) {
        return E_INVALIDARG;
    }
    size_t used = 0;
    const HRESULT result = XAsyncGetResult(async, identity<&XGameSaveReadBlobDataAsync>(),
                                           blobs_size, blob_data, &used);
    if (SUCCEEDED(result)) {
        // The records come first, and the first blob's data right after the last record.
        *count_of_blobs = used == 0
                              ? 0
                              : static_cast<uint32_t>(
                                    (blob_data[0].data - reinterpret_cast<uint8_t*>(blob_data)) /
                                    sizeof(XGameSaveBlob));
    }
    return result;
}

HRESULT XGameSaveCreateUpdate(XGameSaveContainerHandle container,
                              const char* container_display_name, XGameSaveUpdateHandle* update) {
    const HRESULT checked = check_container(container);
    if (FAILED(checked)) {
        return checked;
    }
    if (update == nullptr) {
        return E_INVALIDARG;
    }
    auto created = std::make_unique<XGameSaveUpdate>();
    created->provider = container->provider;
    created->container = container->name;
    if (container_display_name != nullptr) {
        created->display_name = container_display_name;
    }
    *update = add_handle(std::move(created), Kind::Update);
    return S_OK;
}

void XGameSaveCloseUpdate(XGameSaveUpdateHandle update) {
    if (update != nullptr && remove_handle(update, Kind::Update)) {
        delete update;
    }
}

namespace {

HRESULT add_change(XGameSaveUpdateHandle update, const char* blob_name,
                   std::optional<std::vector<uint8_t>> data) {
    const HRESULT checked = check_update(update);
    if (FAILED(checked)) {
        return checked;
    }
    if (!valid_blob_name(blob_name)) {
        return E_INVALIDARG;
    }
    const std::scoped_lock lock(update->mutex);
    if (update->submitted) {
        return kGameSaveHandleExpired;
    }
    const size_t size = data ? data->size() : 0;
    if (size > kGameSaveMaxUpdateSize - update->bytes) {
        return kGameSaveUpdateTooBig;
    }
    update->bytes += size;
    update->changes.push_back({.name = blob_name, .data = std::move(data)});
    return S_OK;
}

// Takes the update's changes for its one submit.
HRESULT take_changes(XGameSaveUpdateHandle update, std::vector<Change>& changes) {
    const HRESULT checked = check_update(update);
    if (FAILED(checked)) {
        return checked;
    }
    const std::scoped_lock lock(update->mutex);
    if (update->submitted) {
        return kGameSaveHandleExpired;
    }
    update->submitted = true;
    changes = std::move(update->changes);
    update->changes.clear();
    return S_OK;
}

} // namespace

HRESULT XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle update, const char* blob_name,
                                 const uint8_t* data, size_t byte_count) {
    if (data == nullptr && byte_count != 0) {
        return E_INVALIDARG;
    }
    if (byte_count > kGameSaveMaxUpdateSize) {
        return kGameSaveUpdateTooBig;
    }
    std::vector<uint8_t> copy(data, data + byte_count);
    return add_change(update, blob_name, std::move(copy));
}

HRESULT XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle update, const char* blob_name) {
    return add_change(update, blob_name, std::nullopt);
}

HRESULT XGameSaveSubmitUpdate(XGameSaveUpdateHandle update) {
    const HRESULT checked = check_update(update);
    if (FAILED(checked)) {
        return checked;
    }
    const HRESULT blocking = check_blocking("XGameSaveSubmitUpdate");
    if (FAILED(blocking)) {
        return blocking;
    }
    std::vector<Change> changes;
    const HRESULT taken = take_changes(update, changes);
    if (FAILED(taken)) {
        return taken;
    }
    return commit(*update->provider, update->container, update->display_name, changes);
}

HRESULT XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle update, XAsyncBlock* async) {
    auto changes = std::make_shared<std::vector<Change>>();
    const HRESULT taken = take_changes(update, *changes);
    if (FAILED(taken)) {
        return taken;
    }
    auto job = std::make_unique<Job>();
    job->work = [state = update->provider, container = update->container,
                 display_name = update->display_name, changes](size_t& size) {
        size = 0;
        return commit(*state, container, display_name, *changes);
    };
    const HRESULT begun =
        begin_job<&XGameSaveSubmitUpdateAsync>(async, "XGameSaveSubmitUpdateAsync", std::move(job));
    if (FAILED(begun)) {
        // Never begun, so the update may be submitted again.
        const std::scoped_lock lock(update->mutex);
        update->submitted = false;
        update->changes = std::move(*changes);
    }
    return begun;
}

HRESULT XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    return XAsyncGetResult(async, identity<&XGameSaveSubmitUpdateAsync>(), 0, nullptr, nullptr);
}

namespace {

HRESULT files_not_provided(const char* function) {
    if (first_report(std::string("files ") + function)) {
        LOG_WARN(gdk, "{}: XGameSaveFiles isn't provided; returning E_NOTIMPL", function);
    }
    return E_NOTIMPL;
}

} // namespace

HRESULT XGameSaveFilesGetFolderWithUiAsync(XUserHandle /*requesting_user*/,
                                           const char* /*configuration_id*/,
                                           XAsyncBlock* /*async*/) {
    return files_not_provided("XGameSaveFilesGetFolderWithUiAsync");
}

HRESULT XGameSaveFilesGetFolderWithUiResult(XAsyncBlock* /*async*/, size_t /*folder_size*/,
                                            char* /*folder_result*/) {
    return files_not_provided("XGameSaveFilesGetFolderWithUiResult");
}

HRESULT XGameSaveFilesGetRemainingQuota(XUserHandle /*requesting_user*/,
                                        const char* /*configuration_id*/,
                                        int64_t* /*remaining_quota*/) {
    return files_not_provided("XGameSaveFilesGetRemainingQuota");
}

} // namespace g6::gdk
