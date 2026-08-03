/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "bit_lsm_encoding.h"        // bit_lsm::SABISchema
#include "./rdb_bitlsm_extractor.h"  // Rdb_bitlsm_attr_plan

namespace myrocks {

// Everything the SST build path needs in order to emit SABI blocks for one CF,
// with no server-layer TABLE object in sight. Persisted in the MyRocks data
// dictionary (BITLSM_INDEX_INFO, keyed by the PK's GL_INDEX_ID) so the build
// binding can be re-established at DB open instead of at first table open --
// otherwise a flush/compaction that runs before anyone opens the table
// finalizes its SSTs without a SABI block.
struct Rdb_bitlsm_descriptor {
  bit_lsm::SABISchema schema;
  Rdb_bitlsm_attr_plan plan;
};

// Blob layout version. Bump on ANY layout change; a reader that sees a version
// it does not know refuses the blob, which leaves the CF expected-but-unbound
// and makes the build path fail loudly instead of dropping SABI silently.
constexpr uint16_t RDB_BITLSM_DESCRIPTOR_VERSION = 1;

// Fixed-width little-endian encoding: no padding, no dependence on host struct
// layout. Never throws.
std::string rdb_bitlsm_serialize_descriptor(const Rdb_bitlsm_descriptor &desc);

// Returns false on an unknown version, truncated input, out-of-range enum, or
// trailing garbage. *out is left unspecified on failure.
bool rdb_bitlsm_deserialize_descriptor(std::string_view blob,
                                       Rdb_bitlsm_descriptor *out);

}  // namespace myrocks
