/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "./rdb_bitlsm_read.h"

#include <exception>

/* Full BitLSMIterator definition (pulls in the rocksdb-internal headers this
 * translation unit is here to contain). */
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
    // Candidate mode: yield bitmap-pruned candidate keys only (no per-row
    // verify, no authoritative fetch). Requires the injected snapshot; the
    // consumer re-fetches at the same view. value() is never called (it throws
    // in Candidate mode) -- we consume only key().
    bit_lsm::BitLSMIterator it(base_db, pk_cfh, options, query,
                               bit_lsm::ResultMode::Candidate, snapshot);
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
      const rocksdb::Slice k = it.key();
      out_keys->emplace_back(k.data(), k.size());
    }
    return true;
  } catch (const std::exception &) {
    // D17 fail-loud: an invalid argument (e.g. a null snapshot) or a corrupt
    // SABI surfacing as an exception must not yield a partial candidate set.
    // The caller turns this into an HA_ERR. (A SABI-less SST that null-derefs
    // inside BitLSM would crash before reaching here; bitlsm-CF SSTs always
    // carry a SABI via the UDI factory, so that path is not expected.)
    return false;
  }
}

}  // namespace myrocks
