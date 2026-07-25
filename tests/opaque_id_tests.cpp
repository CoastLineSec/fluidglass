#include "TestHarness.hpp"

#include "v2/core/OpaqueId.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

int main() {
    return hfg::test::run({
        Case{"secure ids are fixed-width lowercase hexadecimal", [] {
            const auto id = secureOpaqueId();
            require(id.size() == 32, "opaque id width changed");
            require(std::ranges::all_of(id, [](const unsigned char character) {
                return std::isdigit(character) || (character >= 'a' && character <= 'f');
            }), "opaque id contains a non-hexadecimal character");
        }},
        Case{"secure ids do not repeat in a sample", [] {
            std::set<std::string> ids;
            for (std::size_t index = 0; index < 256; ++index)
                require(ids.insert(secureOpaqueId()).second, "secure random source repeated an opaque id");
        }},
    });
}
