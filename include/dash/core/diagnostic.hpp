#pragma once

#include <stdexcept>
#include <iostream>
#include <string>

#include "dash/core/source_location.hpp"

namespace dash::core {

class DiagnosticError : public std::runtime_error {
public:
    DiagnosticError(SourceLocation location, std::string message)
        : std::runtime_error(std::move(message)), location_(std::move(location)) {}

    [[nodiscard]] const SourceLocation& location() const noexcept {
        return location_;
    }

private:
    SourceLocation location_;
};


inline void emitWarning(const SourceLocation& location, const std::string& message) {
    std::cerr << location.file << ":" << location.line << ":" << location.column << ": warning: " << message << "\n";
}

[[noreturn]] inline void throwDiagnostic(const SourceLocation& location, const std::string& message) {
    throw DiagnosticError(location, message);
}

} // namespace dash::core
