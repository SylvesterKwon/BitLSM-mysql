/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "./rdb_bitlsm_query.h"

#include <cstdint>
#include <string>
#include <unordered_map>

/* MySQL server: Item tree, Field metadata, KEY. */
#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"  // Item_cond, Item_func_in
#include "sql/item_func.h"
#include "sql/item_row.h"  // Item_row (row-constructor IN predicand)
#include "sql/key.h"
#include "sql/sql_list.h"

/* my_date_to_binary / non_zero_time / TIME_FUZZY_DATE: the DATE comparand is
 * packed exactly the way Field_newdate stores the column. */
#include "my_time.h"

namespace myrocks {

namespace {

using bit_lsm::AttrSpec;
using bit_lsm::BitLSMOptions;
using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;
using bit_lsm::OrClause;
using bit_lsm::QueryCondition;

// Which variant alternative an attribute's comparand must use. Mirrors the
// role/encoding derivation in rdb_datadic.cc::bitlsm_derive_enc so the query's
// comparand type can never disagree with how rows were binned. SKIP marks an
// attribute the query translator does not know how to encode a literal for:
// conditions on it are conservatively omitted. After DATE support landed no
// type reachable through a valid BITLSM_INDEX maps to SKIP -- it is kept as a
// defensive default.
enum class ValKind { SKIP, I64, U64, DBL, STR, DATE };

// Per-attribute translation metadata, keyed by the table field index.
struct AttrMeta {
  uint32_t attr_idx;  // position in the index's user-defined key parts
  ValKind kind;
};

// Map a Field to (AttrSpec, ValKind). All types here are the ones the extractor
// accepts (the table would not exist otherwise). Anything unexpected still gets
// a placeholder AttrSpec so attr_idx alignment is preserved, but is marked SKIP
// so no condition is emitted for it.
static void field_to_attr(const Field *f, AttrSpec *spec, ValKind *kind) {
  using bit_lsm::ORDERED;
  using bit_lsm::UNORDERED;
  const bool nullable = f->is_nullable();
  switch (f->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      const bool uns = f->is_unsigned();
      uint8_t w = static_cast<uint8_t>(f->pack_length());
      if (w == 3) w = 4;  // INT24 -> nearest valid AttrSpec width
      *spec = AttrSpec(ORDERED, w, /*is_signed=*/!uns, /*is_float=*/false,
                       nullable);
      *kind = uns ? ValKind::U64 : ValKind::I64;
      return;
    }
    case MYSQL_TYPE_FLOAT:
      *spec = AttrSpec(ORDERED, 4, /*is_signed=*/true, /*is_float=*/true,
                       nullable);
      *kind = ValKind::DBL;
      return;
    case MYSQL_TYPE_DOUBLE:
      *spec = AttrSpec(ORDERED, 8, /*is_signed=*/true, /*is_float=*/true,
                       nullable);
      *kind = ValKind::DBL;
      return;
    case MYSQL_TYPE_STRING:   // CHAR/BINARY (binary collation)
    case MYSQL_TYPE_VARCHAR:  // VARCHAR/VARBINARY (binary collation)
      *spec = AttrSpec(UNORDERED, 0, /*is_signed=*/false, /*is_float=*/false,
                       nullable);
      *kind = ValKind::STR;
      return;
    case MYSQL_TYPE_NEWDATE:  // DATE (Field_newdate: 3 bytes, LE packed)
      // `width` decodes nothing on this path: the SABI schema persists only
      // roles (rdb_datadic.cc), and MyRocks drives the iterator in
      // ResultMode::Candidate, which leaves CompiledQuery inert and skips
      // per-row Eval (bit_lsm_iterator.cpp). BitLSMQuery::Validate only
      // branches on is_signed/is_float (both false here, matching the
      // extractor's unsigned, non-float U64ToOkey(read_le(..., 3))); it never
      // reads width. `width` is filler -- AttrSpec widths are 1/2/4/8, so
      // DATE's 3 is not expressible here and does not need to be.
      *spec = AttrSpec(ORDERED, 4, /*is_signed=*/false, /*is_float=*/false,
                       nullable);
      *kind = ValKind::DATE;
      return;
    default:
      *spec = AttrSpec(ORDERED, 8, /*is_signed=*/true, /*is_float=*/false,
                       nullable);
      *kind = ValKind::SKIP;
      return;
  }
}

// Shared context for one translation: field_index -> attr metadata.
struct Ctx {
  std::unordered_map<uint, AttrMeta> by_field;
};

// If `it` (after unwrapping refs) is an index-column field, return its Field and
// fill *meta; otherwise return nullptr.
static const Field *index_field_of(Item *it, const Ctx &ctx, AttrMeta *meta) {
  Item *r = it->real_item();
  if (r->type() != Item::FIELD_ITEM) return nullptr;
  const Field *f = static_cast<Item_field *>(r)->field;
  auto found = ctx.by_field.find(f->field_index());
  if (found == ctx.by_field.end()) return nullptr;
  *meta = found->second;
  return f;
}

// Map a comparison functype to a CompareOp; `flip` is true when the index field
// is on the RIGHT-hand side (const OP field), which reverses the direction.
static bool map_op(Item_func::Functype ft, bool flip, CompareOp *op) {
  switch (ft) {
    case Item_func::EQ_FUNC:
      *op = CompareOp::EQUAL;
      return true;
    case Item_func::LT_FUNC:
      *op = flip ? CompareOp::GREATER : CompareOp::LESS;
      return true;
    case Item_func::LE_FUNC:
      *op = flip ? CompareOp::GREATER_EQUAL : CompareOp::LESS_EQUAL;
      return true;
    case Item_func::GT_FUNC:
      *op = flip ? CompareOp::LESS : CompareOp::GREATER;
      return true;
    case Item_func::GE_FUNC:
      *op = flip ? CompareOp::LESS_EQUAL : CompareOp::GREATER_EQUAL;
      return true;
    default:
      return false;
  }
}

// Extract a constant literal from `it` into `*out`, coerced to `kind`. Returns
// false if the literal is NULL or (for an unsigned attr) negative -- in either
// case the condition is omitted rather than risk a non-weakening translation.
static bool extract_value(
    Item *it, ValKind kind,
    std::variant<int64_t, uint64_t, double, std::string> *out) {
  switch (kind) {
    case ValKind::I64: {
      const longlong v = it->val_int();
      if (it->null_value) return false;
      *out = static_cast<int64_t>(v);
      return true;
    }
    case ValKind::U64: {
      const longlong v = it->val_int();
      if (it->null_value) return false;
      // A negative comparand against an unsigned attr has no safe okey; omit.
      if (!it->unsigned_flag && v < 0) return false;
      *out = static_cast<uint64_t>(v);
      return true;
    }
    case ValKind::DBL: {
      const double d = it->val_real();
      if (it->null_value) return false;
      *out = d;
      return true;
    }
    case ValKind::STR: {
      String buf;
      String *s = it->val_str(&buf);
      if (s == nullptr || it->null_value) return false;
      *out = std::string(s->ptr(), s->length());
      return true;
    }
    case ValKind::DATE: {
      MYSQL_TIME ltime;
      if (it->get_date(&ltime, TIME_FUZZY_DATE)) return false;
      if (it->null_value) return false;
      // A comparand carrying a time part is compared as DATETIME: the DATE
      // column widens to midnight, so `d < '2020-01-01 10:00:00'` MATCHES
      // d = 2020-01-01. Truncating to the date part would emit
      // `okey < packed(2020-01-01)` and prune that row away -- a
      // STRENGTHENING, which loses rows. Omit instead.
      if (non_zero_time(ltime)) return false;
      // Zero month/day, e.g. '2020-06-00'. Item::get_date() reaches
      // str_to_datetime_with_warn(), which does NOT apply MODE_NO_ZERO_IN_DATE,
      // so it parses; the comparator's get_mysql_time_from_str() DOES apply it,
      // fails, and get_datetime_value() then silently compares against packed
      // 0 -- weaker than any real date. Emitting a comparand here would prune
      // rows the server counts as matching, i.e. NARROW. Omit instead.
      if (ltime.month == 0 || ltime.day == 0) return false;
      // Pack through the SAME function Field_newdate::store_internal uses
      // (sql/field.cc), so these are byte-for-byte the bytes the extractor
      // reads back. Writing the formula out by hand here would silently
      // diverge if MySQL ever changed the layout.
      uchar buf[3];
      my_date_to_binary(&ltime, buf);
      *out = static_cast<uint64_t>(buf[0]) |
             (static_cast<uint64_t>(buf[1]) << 8) |
             (static_cast<uint64_t>(buf[2]) << 16);
      return true;
    }
    case ValKind::SKIP:
    default:
      return false;
  }
}

// Try to translate a binary comparison (`field OP const` / `const OP field`)
// into one QueryCondition. Returns false (omit) on any unrepresentable shape.
static bool make_condition(Item_func *f, const Ctx &ctx, QueryCondition *cond) {
  if (f->argument_count() != 2) return false;
  Item *a0 = f->arguments()[0];
  Item *a1 = f->arguments()[1];

  AttrMeta meta{};
  Item *cst = nullptr;
  bool flip = false;
  if (index_field_of(a0, ctx, &meta) != nullptr) {
    cst = a1;
    flip = false;
  } else if (index_field_of(a1, ctx, &meta) != nullptr) {
    cst = a0;
    flip = true;
  } else {
    return false;  // neither side is an index column (or column-to-column)
  }
  if (!cst->const_item()) return false;  // e.g. column-to-column
  if (meta.kind == ValKind::SKIP) return false;

  CompareOp op;
  if (!map_op(f->functype(), flip, &op)) return false;
  // UNORDERED attrs are equality-only; a non-EQUAL op is not representable.
  if (meta.kind == ValKind::STR && op != CompareOp::EQUAL) return false;

  if (!extract_value(cst, meta.kind, &cond->value)) return false;
  cond->attr_idx = meta.attr_idx;
  cond->op = op;
  return true;
}

// Translate a positive `field IN (c1, c2, ...)` into EQUAL conditions appended
// to *clause (all on the same attr). Returns false (omit) otherwise.
static bool make_in_terms(Item_func_in *in, const Ctx &ctx, OrClause *clause) {
  if (in->negated) return false;  // NOT IN is a conjunction of !=, not an OR
  const uint n = in->argument_count();
  if (n < 2) return false;

  AttrMeta meta{};
  if (index_field_of(in->arguments()[0], ctx, &meta) == nullptr) return false;
  if (meta.kind == ValKind::SKIP) return false;

  for (uint i = 1; i < n; i++) {
    Item *v = in->arguments()[i];
    if (!v->const_item()) return false;
    QueryCondition c{};
    c.attr_idx = meta.attr_idx;
    c.op = CompareOp::EQUAL;
    if (!extract_value(v, meta.kind, &c.value)) return false;
    clause->push_back(std::move(c));
  }
  return true;
}

// A positive `field BETWEEN lo AND hi` is the conjunction `field >= lo AND
// field <= hi`: TWO independent clause_groups. Handled at the conjunction level
// (not in collect_disjunction, which builds a single OR clause) because it
// contributes two groups. ORDERED attrs only -- an UNORDERED/STR attr has no
// representable range. Both bounds are extracted BEFORE either group is pushed,
// so a partial (potentially non-weakening) translation is never emitted.
// Returns false (caller drops the whole conjunct -> safe weakening) on NOT
// BETWEEN or any unrepresentable shape.
static bool make_between_groups(Item_func *f, const Ctx &ctx, BitLSMQuery *q) {
  if (static_cast<Item_func_opt_neg *>(f)->negated) return false;
  if (f->argument_count() != 3) return false;

  AttrMeta meta{};
  if (index_field_of(f->arguments()[0], ctx, &meta) == nullptr) return false;
  if (meta.kind == ValKind::SKIP || meta.kind == ValKind::STR) return false;

  Item *lo = f->arguments()[1];
  Item *hi = f->arguments()[2];
  if (!lo->const_item() || !hi->const_item()) return false;

  QueryCondition c_lo{};
  c_lo.attr_idx = meta.attr_idx;
  c_lo.op = CompareOp::GREATER_EQUAL;
  if (!extract_value(lo, meta.kind, &c_lo.value)) return false;

  QueryCondition c_hi{};
  c_hi.attr_idx = meta.attr_idx;
  c_hi.op = CompareOp::LESS_EQUAL;
  if (!extract_value(hi, meta.kind, &c_hi.value)) return false;

  OrClause g_lo;
  g_lo.push_back(std::move(c_lo));
  OrClause g_hi;
  g_hi.push_back(std::move(c_hi));
  q->clause_groups.push_back(std::move(g_lo));
  q->clause_groups.push_back(std::move(g_hi));
  return true;
}

// Translate a positive row-constructor IN -- `(c0, c1, ...) IN ((v00, v01,
// ...), (v10, v11, ...), ...)` -- by DISTRIBUTING it per column:
//
//   (a,b) IN ((1,2),(3,4))
//     == (a=1 AND b=2) OR (a=3 AND b=4)   -- DNF; CNF cannot express it
//     <= (a=1 OR a=3) AND (b=2 OR b=4)    -- per column: a safe weakening
//
// Each column contributes ONE clause_group of EQUALs, so this lives at the
// conjunction level (like make_between_groups) rather than in
// collect_disjunction, which builds a single OR clause. The extra combinations
// the distributed form admits (a=1 AND b=4) cost fetches and are dropped by the
// ICP re-verify -- never a correctness problem.
//
// Unlike BETWEEN's two bounds, per-column clauses are INDEPENDENT: a column
// whose values do not all translate is dropped ALONE and the remaining groups
// are still a superset of the original condition. Returns false (nothing
// emitted) only when the SHAPE is unusable.
static bool make_row_in_groups(Item_func_in *in, const Ctx &ctx,
                               BitLSMQuery *q) {
  if (in->negated) return false;  // NOT IN is not a disjunction of EQUALs
  const uint n = in->argument_count();
  if (n < 2) return false;
  Item_row *const pred =
      static_cast<Item_row *>(in->arguments()[0]->real_item());
  const uint cols = pred->cols();
  if (cols == 0) return false;

  // Every RHS element must be a row of the same arity; otherwise values cannot
  // be lined up with columns at all.
  for (uint i = 1; i < n; i++) {
    Item *const e = in->arguments()[i]->real_item();
    if (e->type() != Item::ROW_ITEM) return false;
    if (static_cast<Item_row *>(e)->cols() != cols) return false;
  }

  bool any = false;
  for (uint j = 0; j < cols; j++) {
    AttrMeta meta{};
    // NOTE: the out-of-index case does not actually arrive here -- a row
    // constructor mixing indexed and non-indexed columns is rejected earlier by
    // uses_index_fields_only() and never pushed. Kept for safety; the reachable
    // partial-failure path is a value that fails to extract.
    if (index_field_of(pred->element_index(j), ctx, &meta) == nullptr) continue;
    if (meta.kind == ValKind::SKIP) continue;

    // Build the column's clause COMPLETELY before publishing it: a
    // half-translated OR clause would be a strengthening of this column's
    // constraint, which loses rows.
    OrClause clause;
    bool ok = true;
    for (uint i = 1; i < n; i++) {
      Item *const v =
          static_cast<Item_row *>(in->arguments()[i]->real_item())
              ->element_index(j);
      if (!v->const_item()) {
        ok = false;
        break;
      }
      QueryCondition c{};
      c.attr_idx = meta.attr_idx;
      c.op = CompareOp::EQUAL;
      if (!extract_value(v, meta.kind, &c.value)) {
        ok = false;
        break;
      }
      clause.push_back(std::move(c));
    }
    if (!ok || clause.empty()) continue;  // drop THIS column only
    q->clause_groups.push_back(std::move(clause));
    any = true;
  }
  return any;
}

// Append the OR-terms of a purely disjunctive, fully representable item to
// *clause. A single comparison is a degenerate 1-term disjunction; IN expands
// to n terms; OR is the union of its operands' terms (all-or-nothing). Any
// non-disjunctive shape (e.g. an AND nested inside an OR) returns false so the
// caller drops the whole clause -- preserving the weakening invariant.
static bool collect_disjunction(Item *it, const Ctx &ctx, OrClause *clause) {
  if (it->type() == Item::COND_ITEM) {
    Item_cond *c = static_cast<Item_cond *>(it);
    if (c->functype() != Item_func::COND_OR_FUNC) return false;
    List_iterator<Item> li(*c->argument_list());
    Item *sub;
    while ((sub = li++)) {
      if (!collect_disjunction(sub, ctx, clause)) return false;
    }
    return true;
  }
  if (it->type() == Item::FUNC_ITEM) {
    Item_func *f = static_cast<Item_func *>(it);
    switch (f->functype()) {
      case Item_func::EQ_FUNC:
      case Item_func::LT_FUNC:
      case Item_func::LE_FUNC:
      case Item_func::GT_FUNC:
      case Item_func::GE_FUNC: {
        QueryCondition cond{};
        if (!make_condition(f, ctx, &cond)) return false;
        clause->push_back(std::move(cond));
        return true;
      }
      case Item_func::IN_FUNC:
        return make_in_terms(static_cast<Item_func_in *>(f), ctx, clause);
      case Item_func::MULT_EQUAL_FUNC: {
        // A multiple-equality inside an OR arm (planning-time shape: see
        // collect_conjunction -- optimize_cond rewrites equalities in every
        // arm too, and the execution-time ICP tree has them substituted
        // back). Arm semantics: every member field equals the shared
        // constant. Emitting any member as an OR term only WIDENS the union
        // (each term is a superset of the arm), so the weakening invariant
        // holds even when some members are skipped.
        Item_equal *ie = static_cast<Item_equal *>(f);
        Item *cst = ie->const_arg();
        if (cst == nullptr || !cst->const_item()) return false;
        bool any = false;
        for (Item_field &fld : ie->get_fields()) {
          AttrMeta meta{};
          if (index_field_of(&fld, ctx, &meta) == nullptr) continue;
          if (meta.kind == ValKind::SKIP) continue;
          QueryCondition qc{};
          if (!extract_value(cst, meta.kind, &qc.value)) continue;
          qc.attr_idx = meta.attr_idx;
          qc.op = CompareOp::EQUAL;
          clause->push_back(std::move(qc));
          any = true;
        }
        return any;  // no representable member -> drop the whole clause
      }
      default:
        return false;
    }
  }
  return false;
}

// Walk the AND-spine: each conjunct becomes its own clause_group. Nested ANDs
// are flattened; a conjunct that is not representable as a pure disjunction is
// silently dropped (dropping a conjunct only weakens the query).
static void collect_conjunction(Item *it, const Ctx &ctx, BitLSMQuery *q) {
  if (it->type() == Item::COND_ITEM) {
    Item_cond *c = static_cast<Item_cond *>(it);
    if (c->functype() == Item_func::COND_AND_FUNC) {
      List_iterator<Item> li(*c->argument_list());
      Item *sub;
      while ((sub = li++)) collect_conjunction(sub, ctx, q);
      return;
    }
    // A top-level OR falls through to collect_disjunction below.
  }
  // A multiple-equality node (Item_equal, built by optimize_cond). PLANNING-
  // time conditions carry these; the execution-time ICP tree has plain
  // field=const again because substitute_for_best_equal_field runs after
  // range analysis. Semantics: every member field equals the shared constant
  // = an AND of equalities, so translating any subset is a weakening.
  // Without a constant (pure column-to-column chain) nothing is
  // representable.
  if (it->type() == Item::FUNC_ITEM &&
      static_cast<Item_func *>(it)->functype() == Item_func::MULT_EQUAL_FUNC) {
    Item_equal *ie = static_cast<Item_equal *>(it);
    Item *cst = ie->const_arg();
    if (cst != nullptr && cst->const_item()) {
      for (Item_field &fld : ie->get_fields()) {
        AttrMeta meta{};
        if (index_field_of(&fld, ctx, &meta) == nullptr) continue;
        if (meta.kind == ValKind::SKIP) continue;
        QueryCondition qc{};
        if (!extract_value(cst, meta.kind, &qc.value)) continue;
        qc.attr_idx = meta.attr_idx;
        qc.op = CompareOp::EQUAL;
        OrClause oc;
        oc.push_back(std::move(qc));
        q->clause_groups.push_back(std::move(oc));
      }
    }
    return;
  }
  // A row-constructor IN -- `(a,b) IN ((1,2),(3,4))` -- distributes into one
  // clause_group per column (see make_row_in_groups), so like BETWEEN it
  // cannot go through collect_disjunction. A SCALAR IN keeps falling through
  // to collect_disjunction, which turns it into a single OR clause.
  if (it->type() == Item::FUNC_ITEM &&
      static_cast<Item_func *>(it)->functype() == Item_func::IN_FUNC) {
    Item_func_in *const in = static_cast<Item_func_in *>(it);
    if (in->arguments()[0]->real_item()->type() == Item::ROW_ITEM) {
      make_row_in_groups(in, ctx, q);
      return;
    }
  }
  // `field BETWEEN lo AND hi` expands to two clause_groups (>= lo, <= hi).
  // collect_disjunction can't represent it (a conjunction is not a single OR
  // term), so handle it here; an unrepresentable BETWEEN is dropped (a dropped
  // conjunct only weakens -> safe).
  if (it->type() == Item::FUNC_ITEM &&
      static_cast<Item_func *>(it)->functype() == Item_func::BETWEEN) {
    make_between_groups(static_cast<Item_func *>(it), ctx, q);
    return;
  }
  OrClause clause;
  if (collect_disjunction(it, ctx, &clause) && !clause.empty()) {
    q->clause_groups.push_back(std::move(clause));
  }
}

}  // namespace

bool rdb_bitlsm_assemble_query(const KEY &key_info, Item *cond,
                               BitLSMQuery *out_query,
                               BitLSMOptions *out_options) {
  // (1) Build the schema/options from the index Field types (single source of
  // truth for role + comparand variant), and the field_index -> attr map.
  Ctx ctx;
  out_options->attr_specs.clear();
  out_options->attr_specs.reserve(key_info.user_defined_key_parts);
  for (uint i = 0; i < key_info.user_defined_key_parts; i++) {
    const Field *f = key_info.key_part[i].field;
    AttrSpec spec;
    ValKind kind;
    field_to_attr(f, &spec, &kind);
    out_options->attr_specs.push_back(spec);
    ctx.by_field.emplace(f->field_index(),
                         AttrMeta{static_cast<uint32_t>(i), kind});
  }
  out_options->attr_num =
      static_cast<uint32_t>(out_options->attr_specs.size());
  out_options->rho = 0.1;       // matches setup_bitlsm_index default
  out_options->read_seqno = 0;  // unused by Validate/ToString

  // (2) Walk the pushed condition into a CNF BitLSMQuery.
  out_query->clause_groups.clear();
  if (cond != nullptr) collect_conjunction(cond, ctx, out_query);

  // (3) Validate; on failure degrade to an empty (no-pruning) query, which is
  // still a valid weakening.
  const rocksdb::Status s = out_query->Validate(*out_options);
  if (!s.ok()) {
    out_query->clause_groups.clear();
    return false;
  }
  return true;
}

}  // namespace myrocks
