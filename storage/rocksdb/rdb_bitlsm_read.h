// Copyright (c) 2026 Dohyun Kwon

#pragma once

/* Thin wrapper over bit_lsm::BitLSMIterator in Candidate mode. It exists to
 * keep bit_lsm_iterator.h's rocksdb-internal headers out of ha_rocksdb.cc. */

#include <string>
#include <vector>

#include "bit_lsm_query.h"

namespace rocksdb {
class DB;
class ColumnFamilyHandle;
class Snapshot;
}  // namespace rocksdb

namespace myrocks {

// Append every bitmap-pruned candidate PK key of the committed data in `pk_cfh`
// at `snapshot` to *out_keys; the caller re-fetches and re-verifies each one.
// `snapshot` must be the read view the caller fetches with. Keys are not
// filtered by index id. Returns false if the iterator threw (invalid argument
// or corrupt SABI); the caller must then fail rather than use a partial set.
bool rdb_bitlsm_collect_candidates(rocksdb::DB *base_db,
                                   rocksdb::ColumnFamilyHandle *pk_cfh,
                                   const bit_lsm::BitLSMOptions &options,
                                   const bit_lsm::BitLSMQuery &query,
                                   const rocksdb::Snapshot *snapshot,
                                   std::vector<std::string> *out_keys);

}  // namespace myrocks
