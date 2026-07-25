#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace hfg::v2 {

enum class ErrorCode {
    InvalidJson,
    InvalidRequest,
    UnsupportedVersion,
    UnsupportedOperation,
    ResourceLimited,
    SessionNotFound,
    InvalidToken,
    StaleGeneration,
    InvalidMaterial,
    InvalidTarget,
    UnresolvedTarget,
    UnsupportedTarget,
    InternalError,
};

struct Error {
    ErrorCode   code = ErrorCode::InternalError;
    std::string path;
    std::string message;

    friend bool operator==(const Error&, const Error&) = default;
};

template <typename T>
class Result {
  public:
    static Result success(T value) {
        return Result(std::move(value));
    }

    static Result failure(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept {
        return std::holds_alternative<T>(m_value);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasValue();
    }

    [[nodiscard]] const T& value() const {
        return std::get<T>(m_value);
    }

    [[nodiscard]] T& value() {
        return std::get<T>(m_value);
    }

    [[nodiscard]] const Error& error() const {
        return std::get<Error>(m_value);
    }

  private:
    explicit Result(T value) : m_value(std::move(value)) {}
    explicit Result(Error error) : m_value(std::move(error)) {}

    std::variant<T, Error> m_value;
};

template <>
class Result<void> {
  public:
    static Result success() {
        return Result(std::nullopt);
    }

    static Result failure(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept {
        return !m_error.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasValue();
    }

    [[nodiscard]] const Error& error() const {
        return m_error.value();
    }

  private:
    explicit Result(std::optional<Error> error) : m_error(std::move(error)) {}
    explicit Result(Error error) : m_error(std::move(error)) {}

    std::optional<Error> m_error;
};

[[nodiscard]] constexpr std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidJson:          return "invalid-json";
        case ErrorCode::InvalidRequest:       return "invalid-request";
        case ErrorCode::UnsupportedVersion:   return "unsupported-version";
        case ErrorCode::UnsupportedOperation: return "unsupported-operation";
        case ErrorCode::ResourceLimited:      return "resource-limited";
        case ErrorCode::SessionNotFound:      return "session-not-found";
        case ErrorCode::InvalidToken:          return "invalid-token";
        case ErrorCode::StaleGeneration:       return "stale-generation";
        case ErrorCode::InvalidMaterial:       return "invalid-material";
        case ErrorCode::InvalidTarget:         return "invalid-target";
        case ErrorCode::UnresolvedTarget:      return "unresolved-target";
        case ErrorCode::UnsupportedTarget:     return "unsupported-target";
        case ErrorCode::InternalError:         return "internal-error";
    }
    return "internal-error";
}

} // namespace hfg::v2
