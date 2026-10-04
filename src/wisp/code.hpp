// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "wisp/heap.hpp"
#include "wisp/word.hpp"

#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

namespace wisp {

// Compact executable records: [versioned opcode, operands...]. Using the
// record word pool leaves ordinary vectors self-evaluating. No descriptor,
// binding, or other analysis object belongs to this representation.
//
// Code records are valid by construction. Only MAKE-CODE creates them and
// tape decoding checks restored ones; RECORD refuses a code opcode and
// RECORD-SET! refuses a code record. Dispatch then trusts a code record's
// arity and operand kinds. NODES and NAMES vectors stay ordinary mutable
// vectors, so executors check their elements on use.
enum class code_op {
    constant,
    lexical_load,
    global_load,
    function_cell,
    lexical_store,
    global_store,
    branch,
    sequence,
    let,
    call,
    closure,
    source,
    function,
};

enum class operand_kind {
    value,
    address,
    symbol,
    name,
    node,
    nodes,
    names
};

constexpr std::string_view operand_name(operand_kind kind)
{
    switch (kind) {
    case operand_kind::value:
        return "VALUE";
    case operand_kind::address:
        return "ADDRESS";
    case operand_kind::symbol:
        return "SYMBOL";
    case operand_kind::name:
        return "NAME";
    case operand_kind::node:
        return "NODE";
    case operand_kind::nodes:
        return "NODES";
    case operand_kind::names:
        return "NAMES";
    }
    return {};
}

struct code_operation
{
    code_op op;
    std::string_view name;
    std::size_t count;
    std::array<operand_kind, 4> operands{};

    consteval code_operation(
        code_op op,
        std::string_view name,
        std::initializer_list<operand_kind> operands)
        : op(op)
        , name(name)
        , count(operands.size())
    {
        std::size_t i = 0;
        for (auto kind : operands)
            this->operands[i++] = kind;
    }
};

inline constexpr std::array code_operations{
    code_operation{code_op::constant, "CONSTANT", {operand_kind::value}},
    code_operation{
        code_op::lexical_load,
        "LEXICAL-LOAD",
        {operand_kind::address, operand_kind::address}},
    code_operation{
        code_op::global_load, "GLOBAL-LOAD", {operand_kind::symbol}},
    code_operation{
        code_op::function_cell, "FUNCTION-CELL", {operand_kind::symbol}},
    code_operation{
        code_op::lexical_store,
        "LEXICAL-STORE",
        {operand_kind::address, operand_kind::address, operand_kind::node}},
    code_operation{
        code_op::global_store,
        "GLOBAL-STORE",
        {operand_kind::symbol, operand_kind::node}},
    code_operation{
        code_op::branch,
        "BRANCH",
        {operand_kind::node, operand_kind::node, operand_kind::node}},
    code_operation{code_op::sequence, "SEQUENCE", {operand_kind::nodes}},
    code_operation{
        code_op::let,
        "LET",
        {operand_kind::names, operand_kind::nodes, operand_kind::node}},
    code_operation{
        code_op::call, "CALL", {operand_kind::symbol, operand_kind::nodes}},
    code_operation{code_op::closure, "CLOSURE", {operand_kind::node}},
    code_operation{code_op::source, "SOURCE", {operand_kind::value}},
    code_operation{
        code_op::function,
        "FUNCTION",
        {operand_kind::name,
         operand_kind::value,
         operand_kind::node,
         operand_kind::value}},
};

static_assert([] {
    for (std::size_t i = 0; i < code_operations.size(); ++i)
        if (std::size_t(code_operations[i].op) != i)
            return false;
    return true;
}());

inline constexpr int code_version = 1;

constexpr word code_opcode(code_op op)
{
    return fixnum((code_version << 8) + int(op));
}

constexpr std::optional<code_op> code_operation_of(word opcode)
{
    if (tag_of(opcode) != tag::integer)
        return std::nullopt;
    const auto index = integer(opcode) - (code_version << 8);
    if (index < 0 || std::size_t(index) >= code_operations.size())
        return std::nullopt;
    return code_op(index);
}

// The operation of a code record, or nothing for any other value.
inline std::optional<code_op> code_record_op(const heap & h, word x)
{
    if (tag_of(x) != tag::rec)
        return std::nullopt;
    const auto xs = h.words<tag::rec>(x);
    return xs.empty() ? std::nullopt : code_operation_of(xs[0]);
}

// Whether OPERANDS fit OP's schema. A NODE operand need only be a code
// record: every code record was checked when it was made or restored.
inline bool code_operands_valid(
    const heap & h, code_op op, std::span<const word> operands)
{
    const auto & shape = code_operations[std::size_t(op)];
    if (operands.size() != shape.count)
        return false;
    for (std::size_t i = 0; i < shape.count; ++i) {
        const auto x = operands[i];
        switch (shape.operands[i]) {
        case operand_kind::value:
            break;
        case operand_kind::address:
            if (tag_of(x) != tag::integer || integer(x) < 0)
                return false;
            break;
        case operand_kind::symbol:
            if (tag_of(x) != tag::sym)
                return false;
            break;
        case operand_kind::name:
            if (x != nil && tag_of(x) != tag::sym)
                return false;
            break;
        case operand_kind::node:
            if (!code_record_op(h, x))
                return false;
            break;
        case operand_kind::nodes:
        case operand_kind::names:
            if (tag_of(x) != tag::v32)
                return false;
            break;
        }
    }
    return true;
}

} // namespace wisp
