#include "v2/core/OpaqueId.hpp"

#include <sys/random.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace hfg::v2 {

std::string secureOpaqueId() {
    std::array<std::uint8_t, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(
            bytes.data() + offset,
            bytes.size() - offset,
            0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        throw std::runtime_error("secure random source is unavailable");
    }

    static constexpr char HEX[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2U] = HEX[bytes[index] >> 4U];
        result[index * 2U + 1U] = HEX[bytes[index] & 0x0FU];
    }
    return result;
}

} // namespace hfg::v2
