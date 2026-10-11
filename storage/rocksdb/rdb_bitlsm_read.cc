// Copyright (c) 2026 Dohyun Kwon

#include "./rdb_bitlsm_read.h"

#include <exception>

#include "bit_lsm_iterator.h"
#include "rocksdb/slice.h"

namespace myrocks {

bool rdb_bitlsm_collect_candidates(rocksdb::DB *base_db,
                                   rocksdb::ColumnFamilyHandle *pk_cfh,
                                   const bit_lsm::BitLSMOptions &options,
                                   const bit_lsm::BitLSMQuery &query,
                                   const rocksdb::Snapshot *snapshot,
                                   std::vector<std::string> *out_keys) {
  try {
    // Candidate mode yields pruned keys only; value() throws in this mode.
    bit_lsm::BitLSMIterator it(base_db, pk_cfh, options, query,
                               bit_lsm::ResultMode::Candidate, snapshot);
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
      const rocksdb::Slice k = it.key();
      out_keys->emplace_back(k.data(), k.size());
    }
    return true;
  } catch (const std::exception &) {
    // An invalid argument (e.g. a null snapshot) or a corrupt SABI must not
    // yield a partial candidate set; the caller turns this into an error.
    return false;
  }
}

}  // namespace myrocks
