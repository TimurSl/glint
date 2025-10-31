#pragma once

#include <optional>
#include <utility>
#include <variant>

namespace glint {

template <typename E>
class Unexpected {
public:
    explicit Unexpected(E error) : error_(std::move(error)) {}

    [[nodiscard]] const E& value() const noexcept { return error_; }
    [[nodiscard]] E& value() noexcept { return error_; }

private:
    E error_;
};

template <typename E>
[[nodiscard]] Unexpected<E> unexpected(E error) {
    return Unexpected<E>(std::move(error));
}

template <typename T, typename E>
class Expected {
public:
    Expected(const T& value) : storage_(value), has_value_(true) {}
    Expected(T&& value) : storage_(std::move(value)), has_value_(true) {}
    Expected(const Unexpected<E>& error)
        : storage_(error.value()), has_value_(false) {}
    Expected(Unexpected<E>&& error)
        : storage_(std::move(error.value())), has_value_(false) {}

    [[nodiscard]] bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }

    [[nodiscard]] T& value() { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const { return std::get<T>(storage_); }

    [[nodiscard]] E& error() { return std::get<E>(storage_); }
    [[nodiscard]] const E& error() const { return std::get<E>(storage_); }

private:
    std::variant<T, E> storage_;
    bool has_value_{false};
};

template <typename E>
class Expected<void, E> {
public:
    Expected() noexcept : has_value_(true) {}
    Expected(const Unexpected<E>& error)
        : error_(error.value()), has_value_(false) {}
    Expected(Unexpected<E>&& error)
        : error_(std::move(error.value())), has_value_(false) {}

    [[nodiscard]] bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }

    void value() const noexcept {}

    [[nodiscard]] const E& error() const { return *error_; }
    [[nodiscard]] E& error() { return *error_; }

private:
    std::optional<E> error_{};
    bool has_value_{false};
};

} // namespace glint
