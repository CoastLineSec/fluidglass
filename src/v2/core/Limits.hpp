#pragma once

#include <cstddef>
#include <cstdint>

namespace hfg::v2 {

struct Limits {
    static constexpr std::size_t MAX_REQUEST_BYTES        = 256U * 1024U;
    static constexpr std::size_t MAX_JSON_NESTING         = 64U;
    static constexpr std::size_t MAX_IDENTIFIER_BYTES     = 128U;
    static constexpr std::size_t MAX_REGEX_BYTES          = 256U;
    static constexpr std::size_t MAX_SESSIONS             = 64U;
    static constexpr std::size_t MAX_TARGETS_PER_SESSION   = 256U;
    static constexpr std::size_t MAX_DYNAMIC_TARGETS       = 512U;
    static constexpr std::size_t MAX_MATERIALS_PER_OWNER   = 128U;
    static constexpr std::size_t MAX_RULES_PER_KIND        = 512U;
    static constexpr std::size_t MAX_COMPOUND_PARTS        = 32U;

    static constexpr std::uint64_t CLIENT_LEASE_MS         = 15'000U;
    static constexpr std::uint64_t PREVIEW_LEASE_MS        = 5'000U;
    static constexpr std::uint64_t SESSION_TOMBSTONE_MS    = 2'000U;
};

} // namespace hfg::v2
