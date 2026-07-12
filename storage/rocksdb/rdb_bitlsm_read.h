/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

/* M3b-3: narrow façade over bit_lsm::BitLSMIterator (Candidate mode).
 *
 * bit_lsm_iterator.h drags in rocksdb-internal headers (db/db_impl/db_impl.h,
 * db/column_family.h, ...). Keeping the iterator construction behind this thin
 * function keeps those internals out of ha_rocksdb.cc, which already mixes the
 * MySQL server headers with the rocksdb public API. Only rocksdb public types
 * and bit_lsm public query types cross this boundary. */

#include <string>
#include <vector>

#include "bit_lsm_query.h"  // bit_lsm::BitLSMQuery, bit_lsm::BitLSMOptions

namespace rocksdb {
class DB;
class ColumnFamilyHandle;
class Snapshot;
}  // namespace rocksdb

namespace myrocks {

// Run a bit_lsm::BitLSMIterator in Candidate mode over `pk_cfh` at `snapshot`,
// appending every bitmap-pruned candidate PK (user) key to *out_keys. The
// candidates are the COMMITTED rows (memtable + immutable memtables + SSTs) that
// survive SABI block-skip at the injected read view; the caller re-fetches and
// re-verifies each authoritatively. `snapshot` MUST be the SAME read view the
// caller's authoritative fetch (rdb_tx_multi_get) uses -- a fresher snapshot
// would drop rows the caller must still see. Keys are NOT filtered by CF prefix
// here (a shared CF may hold foreign rows); the caller applies CR-1.
//
// Returns true on success. Returns false if the iterator threw (e.g. an invalid
// argument, or a corrupt SABI-less SST surfacing as an exception): the caller
// then fails loud with an HA_ERR rather than returning a partial candidate set.
bool rdb_bitlsm_collect_candidates(rocksdb::DB *base_db,
                                   rocksdb::ColumnFamilyHandle *pk_cfh,
                                   const bit_lsm::BitLSMOptions &options,
                                   const bit_lsm::BitLSMQuery &query,
                                   const rocksdb::Snapshot *snapshot,
                                   std::vector<std::string> *out_keys);

}  // namespace myrocks
