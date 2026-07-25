#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hfg::test {

struct Case {
    std::string           name;
    std::function<void()> body;
};

class Failure final : public std::exception {
  public:
    explicit Failure(std::string message) : m_message(std::move(message)) {}

    [[nodiscard]] const char* what() const noexcept override {
        return m_message.c_str();
    }

  private:
    std::string m_message;
};

inline void require(bool condition, std::string_view message) {
    if (!condition)
        throw Failure(std::string(message));
}

inline int run(const std::vector<Case>& cases) {
    std::size_t passed = 0;
    for (const auto& test : cases) {
        try {
            test.body();
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            std::cerr << "FAIL " << test.name << ": unknown exception\n";
        }
    }
    std::cout << passed << '/' << cases.size() << " tests passed\n";
    return passed == cases.size() ? 0 : 1;
}

} // namespace hfg::test
