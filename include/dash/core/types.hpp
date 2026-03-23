#pragma once

#include <cstddef>
#include <string>

namespace dash::core {

enum class BuiltinTypeKind {
    Void,
    Bool,
    Int,
    UInt,
    Double,
    Char,
    String,
    Class,
    Array,
    Unknown,
};

struct TypeRef {
    BuiltinTypeKind kind {BuiltinTypeKind::Unknown};
    std::string name;
    BuiltinTypeKind elementKind {BuiltinTypeKind::Unknown};
    std::string elementName;
    bool hasArraySize {false};
    std::size_t arraySize {0};
    std::size_t pointerDepth {0};

    [[nodiscard]] bool isVoid() const noexcept { return kind == BuiltinTypeKind::Void; }
    [[nodiscard]] bool isBool() const noexcept { return kind == BuiltinTypeKind::Bool; }
    [[nodiscard]] bool isInt() const noexcept { return kind == BuiltinTypeKind::Int; }
    [[nodiscard]] bool isUInt() const noexcept { return kind == BuiltinTypeKind::UInt; }
    [[nodiscard]] bool isDouble() const noexcept { return kind == BuiltinTypeKind::Double; }
    [[nodiscard]] bool isChar() const noexcept { return kind == BuiltinTypeKind::Char; }
    [[nodiscard]] bool isString() const noexcept { return kind == BuiltinTypeKind::String; }
    [[nodiscard]] bool isNumeric() const noexcept { return pointerDepth == 0 && (isInt() || isUInt() || isDouble() || isChar()); }
    [[nodiscard]] bool isClass() const noexcept { return pointerDepth == 0 && kind == BuiltinTypeKind::Class; }
    [[nodiscard]] bool isArray() const noexcept { return pointerDepth == 0 && kind == BuiltinTypeKind::Array; }
    [[nodiscard]] bool isPointer() const noexcept { return pointerDepth > 0; }

    [[nodiscard]] TypeRef arrayElementType() const noexcept {
        TypeRef out{};
        out.kind = elementKind;
        out.name = elementName;
        return out;
    }

    [[nodiscard]] TypeRef pointeeType() const noexcept {
        TypeRef out = *this;
        if (out.pointerDepth > 0) {
            --out.pointerDepth;
        }
        return out;
    }
};

inline bool operator==(const TypeRef& lhs, const TypeRef& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.elementKind == rhs.elementKind &&
           lhs.elementName == rhs.elementName && lhs.hasArraySize == rhs.hasArraySize && lhs.arraySize == rhs.arraySize && lhs.pointerDepth == rhs.pointerDepth;
}

inline bool operator!=(const TypeRef& lhs, const TypeRef& rhs) noexcept {
    return !(lhs == rhs);
}

[[nodiscard]] inline std::string toString(const TypeRef& type) {
    std::string base;
    switch (type.kind) {
        case BuiltinTypeKind::Void: base = "void"; break;
        case BuiltinTypeKind::Bool: base = "bool"; break;
        case BuiltinTypeKind::Int: base = "int"; break;
        case BuiltinTypeKind::UInt: base = "uint"; break;
        case BuiltinTypeKind::Double: base = "double"; break;
        case BuiltinTypeKind::Char: base = "char"; break;
        case BuiltinTypeKind::String: base = "string"; break;
        case BuiltinTypeKind::Class: base = type.name; break;
        case BuiltinTypeKind::Array: {
            TypeRef elem{}; elem.kind = type.elementKind; elem.name = type.elementName;
            base = toString(elem) + "[" + (type.hasArraySize ? std::to_string(type.arraySize) : std::string{}) + "]";
            break;
        }
        case BuiltinTypeKind::Unknown: base = "<unknown>"; break;
    }
    for (std::size_t i = 0; i < type.pointerDepth; ++i) {
        base += "*";
    }
    return base;
}

[[nodiscard]] inline TypeRef parseBuiltinType(const std::string& name) {
    if (name == "void") { TypeRef t{}; t.kind = BuiltinTypeKind::Void; return t; }
    if (name == "bool") { TypeRef t{}; t.kind = BuiltinTypeKind::Bool; return t; }
    if (name == "int") { TypeRef t{}; t.kind = BuiltinTypeKind::Int; return t; }
    if (name == "uint") { TypeRef t{}; t.kind = BuiltinTypeKind::UInt; return t; }
    if (name == "double") { TypeRef t{}; t.kind = BuiltinTypeKind::Double; return t; }
    if (name == "char") { TypeRef t{}; t.kind = BuiltinTypeKind::Char; return t; }
    if (name == "string") { TypeRef t{}; t.kind = BuiltinTypeKind::String; return t; }
    return TypeRef{};
}

[[nodiscard]] inline bool isImplicitlyConvertible(const TypeRef& from, const TypeRef& to) {
    if (from == to) {
        return true;
    }

    if (from.isPointer() || to.isPointer()) {
        if (from.isPointer() && to.isPointer()) {
            return from.pointerDepth == to.pointerDepth && from.kind == to.kind && from.name == to.name;
        }
        if (from.isPointer() && (to.isInt() || to.isUInt())) {
            return true;
        }
        if (to.isPointer() && (from.isInt() || from.isUInt())) {
            return true;
        }
        return false;
    }

    if (from.isArray() && to.isArray()) {
        const bool sameElement = from.elementKind == to.elementKind && from.elementName == to.elementName;
        if (!sameElement) {
            return false;
        }
        if (to.hasArraySize && from.hasArraySize && from.arraySize > to.arraySize) {
            return false;
        }
        return true;
    }

    if ((from.isInt() || from.isUInt() || from.isChar()) && to.isDouble()) {
        return true;
    }

    if ((from.isInt() || from.isChar()) && to.isUInt()) {
        return true;
    }

    if ((from.isUInt() || from.isChar()) && to.isInt()) {
        return true;
    }

    if ((from.isInt() || from.isUInt()) && to.isChar()) {
        return true;
    }

    if (from.isChar() && (to.isChar() || to.isInt() || to.isUInt())) {
        return true;
    }

    return false;
}

[[nodiscard]] inline TypeRef usualArithmeticType(const TypeRef& lhs, const TypeRef& rhs) {
    if (!lhs.isNumeric() || !rhs.isNumeric()) {
        return TypeRef{};
    }
    if (lhs.isDouble() || rhs.isDouble()) {
        { TypeRef t{}; t.kind = BuiltinTypeKind::Double; return t; }
    }
    if (lhs.isUInt() || rhs.isUInt()) {
        { TypeRef t{}; t.kind = BuiltinTypeKind::UInt; return t; }
    }
    { TypeRef t{}; t.kind = BuiltinTypeKind::Int; return t; }
}

} // namespace dash::core
