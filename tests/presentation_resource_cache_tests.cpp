#include "TestHarness.hpp"

#include "v2/render/PresentationResourceCache.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

struct Resource {
  explicit Resource() { ++live; }
  ~Resource() { --live; }

  static inline std::size_t live = 0;
};

PresentationKey key(std::string output, std::uint64_t generation = 1) {
  return {
      .identity =
          {
              .owner = "client:test",
              .targetId = "bar",
          },
      .output = std::move(output),
      .outputGeneration = generation,
      .stage = RenderStage::PostWindows,
  };
}

} // namespace

int main() {
  return hfg::test::run({
      Case{"same presentation reuses one resource", [] {
             PresentationResourceCache<Resource> cache;
             const auto presentation = key("DP-1");
             const auto first = cache.resourceFor(presentation);
             const auto second = cache.resourceFor(presentation);

             require(first == second,
                     "same presentation did not reuse its resource");
             require(cache.size() == 1U,
                     "same presentation created duplicate resources");
           }},
      Case{"different outputs keep independent resources", [] {
             PresentationResourceCache<Resource> cache;
             const auto first = cache.resourceFor(key("DP-1"));
             const auto second = cache.resourceFor(key("DP-2"));

             require(first != second,
                     "different outputs shared a presentation resource");
             require(cache.size() == 2U,
                     "different outputs did not retain independent resources");
           }},
      Case{"new output generation gets a fresh resource", [] {
             PresentationResourceCache<Resource> cache;
             const auto first = cache.resourceFor(key("DP-1", 1));
             const auto second = cache.resourceFor(key("DP-1", 2));

             require(first != second,
                     "new output generation reused stale resources");
           }},
      Case{"retired resources survive queued users then release", [] {
             PresentationResourceCache<Resource> cache;
             auto retired = cache.resourceFor(key("DP-1"));
             (void)cache.resourceFor(key("DP-2"));

             cache.retain({key("DP-2")});
             require(cache.size() == 1U,
                     "retired presentation remained in the cache");
             require(Resource::live == 2U,
                     "queued user lost its retired presentation resource");

             retired.reset();
             require(Resource::live == 1U,
                     "retired resource outlived its final queued user");
           }},
      Case{"clear releases all cached resources", [] {
             PresentationResourceCache<Resource> cache;
             (void)cache.resourceFor(key("DP-1"));
             (void)cache.resourceFor(key("DP-2"));

             cache.clear();
             require(cache.size() == 0U, "clear retained cache entries");
             require(Resource::live == 0U,
                     "clear retained presentation resources");
           }},
  });
}
