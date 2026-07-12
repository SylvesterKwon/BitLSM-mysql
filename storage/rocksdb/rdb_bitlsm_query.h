/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

/* BitLSM query type (compiled into rocksdb_se from the BitLSM submodule). */
#include "bit_lsm_query.h"  // bit_lsm::BitLSMQuery, bit_lsm::BitLSMOptions

class Item;
struct KEY;

namespace myrocks {

// M3b-2: translate a pushed index condition (a MySQL Item tree) into a
// bit_lsm::BitLSMQuery over the columns of `key_info` (a BITLSM_INDEX). The
// result is a CONSERVATIVE weakening (a necessary condition) of `cond`: any row
// the query excludes, `cond` also excludes. Correctness of the read is
// guaranteed elsewhere (ICP re-evaluation); this query is only a pruning hint,
// so untranslatable sub-conditions are silently OMITTED, never mistranslated.
//
// Rules (see the M3b-2 design):
//   - AND: each conjunct becomes its own clause_group; unrepresentable
//     conjuncts are dropped (dropping a conjunct only weakens -> safe).
//   - OR:  becomes one OR-clause ONLY IF every disjunct is representable;
//     otherwise the whole OR is omitted (dropping a disjunct would strengthen).
//   - `field <cmp> const` on an index column -> one QueryCondition.
//   - `field IN (c1, c2, ...)` -> an OR-clause of EQUAL conditions.
//   - UNORDERED (binary string) attrs: only EQUAL is representable.
//   - functions, column-to-column, IS NULL, NOT IN, arithmetic, subqueries,
//     unsupported attr types (e.g. DATE): omitted.
//
// `*out_options` is (re)built from the index Field types so the comparand
// variant type matches each attribute (signed int -> int64, unsigned -> uint64,
// float/double -> double, binary string -> string). The assembled query is
// Validate()d against it; on validation failure the query is emptied (degrades
// to "no pruning", which is still a valid weakening).
//
// Returns true if a valid (possibly empty) query was produced; false if
// Validate() rejected it (in which case *out_query is emptied).
bool rdb_bitlsm_assemble_query(const KEY &key_info, Item *cond,
                               bit_lsm::BitLSMQuery *out_query,
                               bit_lsm::BitLSMOptions *out_options);

}  // namespace myrocks
