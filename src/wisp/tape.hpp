// SPDX-License-Identifier: AGPL-3.0-or-later
// Logical heap images inspired by mbrock/wisp core/tape.zig, revision
// e1c9a96f6c543c858aeafb0dac26bafd649f82a7. Not the Zig tape byte format.
#pragma once

#include "wisp/eval.hpp"

#include <iosfwd>

namespace wisp {

/// A restored machine with stable addresses and a persistent entry root.
/// Destruction unlinks roots before destroying their heap. A vector/list
/// entry can retain several application roots. Native tasks are recreated
/// by the caller, never restored from the tape.
struct image
{
    heap storage;
    evaluator machine;
    root entry;

private:
    friend struct tape_codec;

    image()
        : machine(storage, nullptr)
        , entry(storage)
    {
    }
};

namespace tape {

struct error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

inline constexpr std::size_t default_limit = 64 * 1024 * 1024;

/// Encode at a safepoint between evaluator calls. No collection, mutation,
/// or implicit I/O. Includes whole pools/tables (including garbage), pins,
/// evaluator roots/state, and entry; other host root registrations are not
/// saved. Any ext row is rejected. Collect explicitly first if desired.
/// The format is versioned, little-endian, schema-checked, and relocates
/// builtin IDs by name. SHA-256 detects corruption, not authenticity.
/// Version 2 adds segmented control state; version 1 tapes are rejected.
std::vector<std::byte> encode(const evaluator &, word entry = nil);

/// Restore into a separate owner or throw error; never changes a live
/// machine. Validates references, private package indexes, cached roots,
/// and acyclic segmented control links. Mutable syntax, uses, environments
/// and continuation payloads keep their runtime meaning, including
/// conditions on use; they need not describe a successful program to be
/// checkpointable. Load trusted checkpoints only: this is not a sandbox for
/// hostile executable images. Limits input bytes, not execution time or
/// total resident memory. Allocation failures propagate normally.
std::unique_ptr<image>
decode(std::span<const std::byte>, std::size_t limit = default_limit);

/// Blocking host stream I/O; use binary file streams. Reads exactly one
/// whole tape through EOF. Checks stream failures, but does not flush,
/// close, fsync, or atomically replace a file. Hosts own that policy.
void write(std::ostream &, const evaluator &, word entry = nil);
std::unique_ptr<image>
read(std::istream &, std::size_t limit = default_limit);

} // namespace tape
} // namespace wisp
