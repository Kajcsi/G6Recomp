#include "g6/pe/directories.h"

namespace g6::pe {
namespace {
constexpr uint32_t kMaxTlsCallbacks = 4096;
}

std::optional<TlsDirectory> read_tls(const Image& image) {
    const DataDirectory dir = image.directory(Directory::Tls);
    if (!dir.present()) {
        return std::nullopt;
    }
    TlsDirectory tls;
    tls.raw_data_start_va = image.read<uint64_t>(dir.rva + 0);
    tls.raw_data_end_va = image.read<uint64_t>(dir.rva + 8);
    tls.index_va = image.read<uint64_t>(dir.rva + 16);
    tls.callbacks_va = image.read<uint64_t>(dir.rva + 24);
    tls.zero_fill_size = image.read<uint32_t>(dir.rva + 32);
    tls.characteristics = image.read<uint32_t>(dir.rva + 36);

    if (tls.callbacks_va != 0) {
        const uint32_t array = image.va_to_rva(tls.callbacks_va);
        for (uint32_t i = 0;; ++i) {
            if (i >= kMaxTlsCallbacks) {
                throw FormatError("TLS callback array is not terminated");
            }
            const auto callback = image.read<uint64_t>(array + i * 8);
            if (callback == 0) {
                break;
            }
            tls.callback_rvas.push_back(image.va_to_rva(callback));
        }
    }
    return tls;
}

} // namespace g6::pe
