// Copyright (c) 2026 Dohyun Kwon

#pragma once

/* BitLSM query type (compiled into rocksdb_se from the BitLSM submodule). */
#include "bit_lsm_query.h"

class Item;
struct KEY;

namespace myrocks {

// Translate a pushed index condition (a MySQL Item tree) into a
// bit_lsm::BitLSMQuery over the columns of `key_info` (a BITLSM_INDEX).
//
// The query is a conservative weakening of `cond`: any row it excludes, `cond`
// excludes too. ICP re-checks every candidate, so untranslatable parts are
// omitted, never mistranslated:
//   - AND: one clause_group per conjunct; an unrepresentable one is dropped.
//   - OR: one OR-clause if every disjunct is representable, else omitted whole.
//   - `field <cmp> const` -> one condition; `field IN (...)` -> an OR-clause of
//     EQUALs; `field BETWEEN lo AND hi` -> two clause_groups; a row-constructor
//     IN -> one clause_group of EQUALs per column.
//   - kEquality attrs accept EQUAL only. Everything else is omitted.
//
// `*out_options` is rebuilt from the index Field types. Returns false if
// Validate() rejected the query, in which case *out_query is emptied.
bool rdb_bitlsm_assemble_query(const KEY &key_info, Item *cond,
                               bit_lsm::BitLSMQuery *out_query,
                               bit_lsm::BitLSMOptions *out_options);

}  // namespace myrocks
