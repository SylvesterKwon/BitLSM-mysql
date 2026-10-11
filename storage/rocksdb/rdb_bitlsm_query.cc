// Copyright (c) 2026 Dohyun Kwon

#include "./rdb_bitlsm_query.h"

#include "./rdb_bitlsm_extractor.h"

#include <cstdint>
#include <string>
#include <unordered_map>

#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_row.h"
#include "sql/key.h"
#include "sql/sql_list.h"

#include "my_time.h"

namespace myrocks {

namespace {

using bit_lsm::AttrSpec;
using bit_lsm::BitLSMOptions;
using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;
using bit_lsm::OrClause;
using bit_lsm::QueryCondition;

// Which variant alternative an attribute's comparand uses. Must agree with the
// encoding rdb_datadic.cc::bitlsm_derive_enc binned the rows with. SKIP marks a
// type no literal can be encoded for; conditions on it are omitted.
enum class ValKind { SKIP, I64, U64, DBL, STR, DATE };

struct AttrMeta {
  uint32_t attr_idx;  // position in the index's user-defined key parts
  ValKind kind;
  bit_lsm::IndexType index_type;  // kEquality attrs accept EQUAL only
};

// Map a Field to (AttrSpec, ValKind). An unexpected type still gets a
// placeholder AttrSpec so attr_idx alignment is preserved, but is marked SKIP.
static void field_to_attr(const Field *f, AttrSpec *spec, ValKind *kind,
                          bool declared_ordered, bool declared_unordered) {
  using bit_lsm::IndexType;
  using bit_lsm::PhysicalType;
  const bool nullable = f->is_nullable();
  switch (f->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      const bool uns = f->is_unsigned();
      uint16_t w = static_cast<uint16_t>(f->pack_length());
      if (w == 3) w = 4;  // INT24 -> nearest valid AttrSpec width
      *spec = AttrSpec(
          rdb_bitlsm_index_type_for(false, declared_ordered,
                                    declared_unordered),
          uns ? PhysicalType::kUint : PhysicalType::kInt, w, nullable);
      *kind = uns ? ValKind::U64 : ValKind::I64;
      return;
    }
    case MYSQL_TYPE_FLOAT:
      *spec = AttrSpec(rdb_bitlsm_index_type_for(false, declared_ordered,
                                                 declared_unordered),
                       PhysicalType::kFloat, 4, nullable);
      *kind = ValKind::DBL;
      return;
    case MYSQL_TYPE_DOUBLE:
      *spec = AttrSpec(rdb_bitlsm_index_type_for(false, declared_ordered,
                                                 declared_unordered),
                       PhysicalType::kFloat, 8, nullable);
      *kind = ValKind::DBL;
      return;
    case MYSQL_TYPE_STRING:  // CHAR/BINARY, fixed width (binary collation)
      *spec = AttrSpec(rdb_bitlsm_index_type_for(true, declared_ordered,
                                                 declared_unordered),
                       PhysicalType::kBinary,
                       static_cast<uint16_t>(f->pack_length()), nullable);
      *kind = ValKind::STR;
      return;
    case MYSQL_TYPE_VARCHAR:  // VARCHAR/VARBINARY (binary collation)
      *spec = AttrSpec(rdb_bitlsm_index_type_for(true, declared_ordered,
                                                 declared_unordered),
                       PhysicalType::kVarBinary, 0, nullable);
      *kind = ValKind::STR;
      return;
    case MYSQL_TYPE_NEWDATE:  // DATE (Field_newdate: 3 bytes, LE packed)
      // Width 4 is filler: AttrSpec cannot express 3, and nothing on this path
      // reads the width (the extractor walks the row with its own plan).
      *spec = AttrSpec(rdb_bitlsm_index_type_for(false, declared_ordered,
                                                 declared_unordered),
                       PhysicalType::kUint, 4, nullable);
      *kind = ValKind::DATE;
      return;
    default:
      *spec = AttrSpec(IndexType::kRange, PhysicalType::kInt, 8, nullable);
      *kind = ValKind::SKIP;
      return;
  }
}

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
// is on the right-hand side (const OP field), which reverses the direction.
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
// false when no comparand can be emitted safely (NULL, a negative value for an
// unsigned attr, an unrepresentable DATE); the caller omits the condition.
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
      if (!it->unsigned_flag && v < 0) return false;  // no okey for it
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
      // A comparand with a time part is compared as DATETIME, where the DATE
      // column widens to midnight: `d < '2020-01-01 10:00:00'` matches
      // d = 2020-01-01. Truncating it to a date would prune that row.
      if (non_zero_time(ltime)) return false;
      // A zero month or day ('2020-06-00') parses here but fails in the
      // server's comparator, which then compares against packed 0. Any
      // comparand we emit would prune rows the server counts as matching.
      if (ltime.month == 0 || ltime.day == 0) return false;
      // Pack with the same function Field_newdate::store_internal uses, so the
      // bytes match what the extractor reads back from the row.
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

// `field OP const` or `const OP field` -> one QueryCondition.
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
  // Validate rejects a non-EQUAL op on a kEquality attr; omit it instead.
  if (meta.index_type == bit_lsm::IndexType::kEquality &&
      op != CompareOp::EQUAL)
    return false;

  if (!extract_value(cst, meta.kind, &cond->value)) return false;
  cond->attr_idx = meta.attr_idx;
  cond->op = op;
  return true;
}

// Positive `field IN (c1, c2, ...)` -> EQUAL terms appended to *clause.
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

// Translate a positive `field BETWEEN lo AND hi` (kRange attrs only) into the
// clause_groups `>= lo` and `<= hi`. Both bounds are extracted before either
// group is pushed. Returns false on NOT BETWEEN or any unrepresentable shape.
static bool make_between_groups(Item_func *f, const Ctx &ctx, BitLSMQuery *q) {
  if (static_cast<Item_func_opt_neg *>(f)->negated) return false;
  if (f->argument_count() != 3) return false;

  AttrMeta meta{};
  if (index_field_of(f->arguments()[0], ctx, &meta) == nullptr) return false;
  if (meta.kind == ValKind::SKIP ||
      meta.index_type == bit_lsm::IndexType::kEquality)
    return false;

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

// Translate a positive row-constructor IN by distributing it per column, a safe
// weakening of the DNF the row form denotes:
//   (a,b) IN ((1,2),(3,4))  ==  (a=1 AND b=2) OR (a=3 AND b=4)
//                           <=  (a=1 OR a=3) AND (b=2 OR b=4)
// Each column becomes one clause_group of EQUALs; a column whose values do not
// all translate is dropped alone. Returns false only on an unusable shape.
static bool make_row_in_groups(Item_func_in *in, const Ctx &ctx,
                               BitLSMQuery *q) {
  if (in->negated) return false;  // NOT IN is not a disjunction of EQUALs
  const uint n = in->argument_count();
  if (n < 2) return false;
  Item_row *const pred =
      static_cast<Item_row *>(in->arguments()[0]->real_item());
  const uint cols = pred->cols();
  if (cols == 0) return false;

  // Every RHS element must be a row of the same arity.
  for (uint i = 1; i < n; i++) {
    Item *const e = in->arguments()[i]->real_item();
    if (e->type() != Item::ROW_ITEM) return false;
    if (static_cast<Item_row *>(e)->cols() != cols) return false;
  }

  bool any = false;
  for (uint j = 0; j < cols; j++) {
    AttrMeta meta{};
    if (index_field_of(pred->element_index(j), ctx, &meta) == nullptr) continue;
    if (meta.kind == ValKind::SKIP) continue;

    // Build the column's clause completely before publishing it; a partial OR
    // clause would strengthen the condition.
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
    if (!ok || clause.empty()) continue;  // drop this column only
    q->clause_groups.push_back(std::move(clause));
    any = true;
  }
  return any;
}

// Append the OR-terms of a purely disjunctive item (a comparison, an IN-list,
// or an OR of those) to *clause. Returns false on any other shape, e.g. an AND
// nested inside an OR, so the caller drops the whole clause.
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
        // Item_equal inside an OR arm (planning-time shape). Each member alone
        // is a superset of the arm, so emitting any subset only widens the OR.
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
// are flattened; an unrepresentable conjunct is dropped.
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
  // Item_equal (from optimize_cond) appears only in planning-time conditions.
  // It is an AND of `member = const`, so any subset of members may be emitted.
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
  // A row-constructor IN yields one clause_group per column, so it cannot go
  // through collect_disjunction. A scalar IN falls through to it.
  if (it->type() == Item::FUNC_ITEM &&
      static_cast<Item_func *>(it)->functype() == Item_func::IN_FUNC) {
    Item_func_in *const in = static_cast<Item_func_in *>(it);
    if (in->arguments()[0]->real_item()->type() == Item::ROW_ITEM) {
      make_row_in_groups(in, ctx, q);
      return;
    }
  }
  // BETWEEN expands to two clause_groups, so it is handled here too.
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
  // Options and the field_index -> attr map, both from the index Field types.
  Ctx ctx;
  out_options->attr_specs.clear();
  out_options->attr_specs.reserve(key_info.user_defined_key_parts);
  for (uint i = 0; i < key_info.user_defined_key_parts; i++) {
    const Field *f = key_info.key_part[i].field;
    AttrSpec spec(bit_lsm::IndexType::kRange, bit_lsm::PhysicalType::kInt, 8);
    ValKind kind;
    field_to_attr(f, &spec, &kind,
                  (key_info.m_bitlsm_ordered_mask >> i) & 1U,
                  (key_info.m_bitlsm_unordered_mask >> i) & 1U);
    out_options->attr_specs.push_back(spec);
    ctx.by_field.emplace(
        f->field_index(),
        AttrMeta{static_cast<uint32_t>(i), kind, spec.index_type});
  }
  out_options->attr_num =
      static_cast<uint32_t>(out_options->attr_specs.size());
  out_options->rho = 0.001;     // matches setup_bitlsm_index default
  out_options->read_seqno = 0;  // unused by Validate/ToString

  out_query->clause_groups.clear();
  if (cond != nullptr) collect_conjunction(cond, ctx, out_query);

  // On validation failure degrade to an empty (no-pruning) query.
  const rocksdb::Status s = out_query->Validate(*out_options);
  if (!s.ok()) {
    out_query->clause_groups.clear();
    return false;
  }
  return true;
}

}  // namespace myrocks
