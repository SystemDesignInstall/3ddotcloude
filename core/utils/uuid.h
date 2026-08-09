#pragma once

// UUID generation and formatting (RFC 4122): v4 random generation, v5
// name-based generation, parse/format. UUIDs are the identity primitive across
// the platform: 16-byte BLOB in SQLite, RFC-4122 string in JSON
// (schemas/database/schema.sql conventions). Deterministic v5 identity is used
// for content-derived identifiers (PPS-0001 §5.4, RFC-0006).

#include <array>
#include <cstdint>
#include <string>

namespace spatial::core {

using Uuid = std::array<std::uint8_t, 16>;

// Generates a version-4 UUID. Thread-safe; uses the OS random source
// (BCryptGenRandom on Windows, getrandom on Linux) when available.
Uuid GenerateUuid();

// Generates a version-5 UUID (RFC 4122 §4.3): SHA-1 over the namespace bytes
// concatenated with the name. Deterministic: identical (namespace, name) always
// yield identical UUIDs across runs, machines, and processes.
Uuid GenerateUuidV5(const Uuid& namespace_uuid, const std::string& name);

// Parses a canonical RFC-4122 string ("8-4-4-4-12") into a 16-byte UUID.
// Throws ValidationError on malformed input.
Uuid ParseUuid(const std::string& s);

// Formats a UUID as the canonical lowercase RFC-4122 string.
std::string FormatUuid(const Uuid& uuid);

// True if every byte is zero.
bool IsNil(const Uuid& uuid);

}  // namespace spatial::core
