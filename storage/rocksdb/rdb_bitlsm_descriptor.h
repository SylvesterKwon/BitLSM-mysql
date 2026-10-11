// Copyright (c) 2026 Dohyun Kwon

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "bit_lsm_encoding.h"
#include "./rdb_bitlsm_extractor.h"

namespace myrocks {

// Everything the SST build path needs to emit SABI blocks for one CF, with no
// server-layer TABLE object. Persisted in the data dictionary, keyed by the
// PK's GL_INDEX_ID, so the binding is restored at DB open before compactions.
struct Rdb_bitlsm_descriptor {
  bit_lsm::SABISchema schema;
  Rdb_bitlsm_attr_plan plan;
};

// Blob layout version. Bump on any layout change; a reader refuses an unknown
// version, which leaves the CF expected-but-unbound and fails its SST builds.
constexpr uint16_t RDB_BITLSM_DESCRIPTOR_VERSION = 1;

// Fixed-width little-endian encoding: no padding, no dependence on host struct
// layout. Never throws.
std::string rdb_bitlsm_serialize_descriptor(const Rdb_bitlsm_descriptor &desc);

// Returns false on an unknown version, truncated input, out-of-range enum, or
// trailing garbage. *out is unspecified on failure.
bool rdb_bitlsm_deserialize_descriptor(std::string_view blob,
                                       Rdb_bitlsm_descriptor *out);

}  // namespace myrocks
