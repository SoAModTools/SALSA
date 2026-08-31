#pragma once

#include "SalsaCore/Foundation/Diagnostic.h"

#include <cassert>
#include <optional>
#include <utility>
#include <vector>

namespace salsa::core {

template <typename T>
class Result final {
public:
    [[nodiscard]] static Result success(T value, std::vector<Diagnostic> diagnostics = {}) {
        assert(!hasErrors(diagnostics));
        return Result(std::optional<T>{ std::move(value) }, std::move(diagnostics));
    }

    [[nodiscard]] static Result failure(std::vector<Diagnostic> diagnostics) {
        assert(hasErrors(diagnostics));
        return Result(std::nullopt, std::move(diagnostics));
    }

    [[nodiscard]] static Result failure(Diagnostic diagnostic) {
        std::vector<Diagnostic> diagnostics{};
        diagnostics.push_back(std::move(diagnostic));
        return failure(std::move(diagnostics));
    }

    [[nodiscard]] bool hasValue() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] bool ok() const noexcept {
        return hasValue() && !hasErrors(diagnostics_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }

    [[nodiscard]] T& value() & {
        assert(value_.has_value());
        return *value_;
    }

    [[nodiscard]] const T& value() const& {
        assert(value_.has_value());
        return *value_;
    }

    [[nodiscard]] T takeValue() && {
        assert(value_.has_value());
        return std::move(*value_);
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    Result(std::optional<T> value, std::vector<Diagnostic> diagnostics)
        : value_(std::move(value)), diagnostics_(std::move(diagnostics)) {}

    std::optional<T> value_{};
    std::vector<Diagnostic> diagnostics_{};
};

template <>
class Result<void> final {
public:
    [[nodiscard]] static Result success(std::vector<Diagnostic> diagnostics = {}) {
        assert(!hasErrors(diagnostics));
        return Result(true, std::move(diagnostics));
    }

    [[nodiscard]] static Result failure(std::vector<Diagnostic> diagnostics) {
        assert(hasErrors(diagnostics));
        return Result(false, std::move(diagnostics));
    }

    [[nodiscard]] static Result failure(Diagnostic diagnostic) {
        std::vector<Diagnostic> diagnostics{};
        diagnostics.push_back(std::move(diagnostic));
        return failure(std::move(diagnostics));
    }

    [[nodiscard]] bool ok() const noexcept {
        return success_ && !hasErrors(diagnostics_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    Result(bool success, std::vector<Diagnostic> diagnostics)
        : success_(success), diagnostics_(std::move(diagnostics)) {}

    bool success_ = false;
    std::vector<Diagnostic> diagnostics_{};
};

}  // namespace salsa::core
