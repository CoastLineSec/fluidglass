#pragma once

#include "v2/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace hfg::v2 {

enum class OutputTransform {
    Normal,
    Rotate90,
    Rotate180,
    Rotate270,
    Flipped,
    Flipped90,
    Flipped180,
    Flipped270,
};

struct OutputSnapshot {
    std::string     name;
    std::uint64_t   objectToken = 0;
    std::uint64_t   modeToken = 0;
    std::uint32_t   bufferWidth = 0;
    std::uint32_t   bufferHeight = 0;
    double          logicalX = 0.0;
    double          logicalY = 0.0;
    double          logicalWidth = 0.0;
    double          logicalHeight = 0.0;
    double          scale = 1.0;
    OutputTransform transform = OutputTransform::Normal;
    std::uint32_t   renderFormat = 0;
    std::uint64_t   colorStateToken = 0;

    friend bool operator==(const OutputSnapshot&, const OutputSnapshot&) = default;
};

struct OutputGeneration {
    OutputSnapshot snapshot;
    std::uint64_t  generation = 0;

    friend bool operator==(const OutputGeneration&, const OutputGeneration&) = default;
};

struct OutputGenerationUpdate {
    OutputGeneration                current;
    std::optional<OutputGeneration> retired;
    bool                            changed = false;

    friend bool operator==(const OutputGenerationUpdate&, const OutputGenerationUpdate&) = default;
};

class OutputGenerationTracker {
  public:
    [[nodiscard]] Result<OutputGenerationUpdate> update(OutputSnapshot snapshot);
    [[nodiscard]] std::optional<OutputGeneration> remove(std::string_view name);
    [[nodiscard]] std::optional<OutputGeneration> current(std::string_view name) const;
    [[nodiscard]] std::size_t activeCount() const noexcept;

  private:
    std::map<std::string, OutputGeneration, std::less<>> m_current;
    std::map<std::string, std::uint64_t, std::less<>>    m_lastGeneration;
};

} // namespace hfg::v2
