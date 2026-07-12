/* Copyright (c) Meta Platforms, Inc. and affiliates.

   Unit test for the BitLSM row-value decoder (M3a-4). Exercises
   Rdb_bitlsm_extractor::ExtractAll over hand-built MyRocks value blobs and
   plans, so it depends only on rdb_bitlsm_extractor.h + bit_lsm_encoding.h --
   no TABLE or Field access, no RocksDB storage-engine link. */

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "rdb_bitlsm_extractor.h"
#include "bit_lsm_encoding.h"

namespace {

using myrocks::Rdb_bitlsm_attr_plan;
using myrocks::Rdb_bitlsm_extractor;
using Enc = Rdb_bitlsm_attr_plan::Enc;
using Shape = Rdb_bitlsm_attr_plan::Shape;
using bit_lsm::EncodedAttr;

// --- little helpers to assemble raw value/key blobs ------------------------

void put_u8(std::string *s, uint8_t v) { s->push_back(static_cast<char>(v)); }

// append `len` little-endian bytes of an unsigned value
void put_le(std::string *s, uint64_t v, unsigned len) {
  for (unsigned i = 0; i < len; ++i)
    s->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void put_f64(std::string *s, double d) {
  char b[8];
  std::memcpy(b, &d, 8);
  s->append(b, 8);
}

void put_f32(std::string *s, float f) {
  char b[4];
  std::memcpy(b, &f, 4);
  s->append(b, 4);
}

// 4-byte big-endian index-number prefix + arbitrary pk-key tail
std::string make_key(uint32_t index_number, std::string_view tail = "pk") {
  std::string k;
  put_u8(&k, (index_number >> 24) & 0xff);
  put_u8(&k, (index_number >> 16) & 0xff);
  put_u8(&k, (index_number >> 8) & 0xff);
  put_u8(&k, index_number & 0xff);
  k.append(tail);
  return k;
}

std::vector<EncodedAttr> run(const Rdb_bitlsm_attr_plan &plan,
                             std::string_view key, std::string_view value) {
  auto shared = std::make_shared<const Rdb_bitlsm_attr_plan>(plan);
  Rdb_bitlsm_extractor extractor(shared);
  std::vector<EncodedAttr> out(plan.attr_num);
  extractor.ExtractAll(key, value, out.data());
  return out;
}

Rdb_bitlsm_attr_plan::WalkEntry fixed(uint32_t len, bool nullable,
                                      uint32_t null_off, uint8_t null_mask,
                                      bool is_target, uint32_t attr_index,
                                      Enc enc) {
  Rdb_bitlsm_attr_plan::WalkEntry e{};
  e.shape = Shape::FIXED;
  e.len = len;
  e.nullable = nullable;
  e.null_offset = null_off;
  e.null_mask = null_mask;
  e.is_target = is_target;
  e.attr_index = attr_index;
  e.enc = enc;
  return e;
}

Rdb_bitlsm_attr_plan::WalkEntry varlen(uint32_t prefix_bytes, bool nullable,
                                       uint32_t null_off, uint8_t null_mask,
                                       bool is_target, uint32_t attr_index,
                                       Enc enc) {
  Rdb_bitlsm_attr_plan::WalkEntry e{};
  e.shape = Shape::VARLEN;
  e.len = prefix_bytes;
  e.nullable = nullable;
  e.null_offset = null_off;
  e.null_mask = null_mask;
  e.is_target = is_target;
  e.attr_index = attr_index;
  e.enc = enc;
  return e;
}

// --- tests ------------------------------------------------------------------

// One row exercising: signed int, a variable-length NON-target field before
// later targets, unsigned int, double, DATE(3B), VARBINARY, and a NULL
// attribute at the end. Also confirms the CR-1 key filter passes for a matching
// index number.
TEST(BitlsmDecoder, MixedRowExtractsAllTypes) {
  const uint32_t kIdxNo = 42;
  Rdb_bitlsm_attr_plan plan;
  plan.attr_num = 6;
  plan.ttl_bytes = 0;
  plan.null_bytes_len = 1;  // three nullable fields -> 1 byte
  plan.maybe_unpack_info = false;
  plan.pk_index_number = kIdxNo;
  plan.walk = {
      // attr0: signed INT, nullable (present), null bit 0x01
      fixed(4, true, 0, 0x01, true, 0, Enc::INT_SIGNED),
      // non-target VARBINARY(1B prefix), nullable (present), null bit 0x02
      varlen(1, true, 0, 0x02, false, 0, Enc::BINARY_STR),
      // attr1: unsigned INT, not null
      fixed(4, false, 0, 0, true, 1, Enc::INT_UNSIGNED),
      // attr2: DOUBLE
      fixed(8, false, 0, 0, true, 2, Enc::FLOAT64),
      // attr3: DATE (3B)
      fixed(3, false, 0, 0, true, 3, Enc::DATE3),
      // attr4: VARBINARY target (1B prefix)
      varlen(1, false, 0, 0, true, 4, Enc::BINARY_STR),
      // attr5: signed INT, nullable and NULL in this row (null bit 0x04)
      fixed(4, true, 0, 0x04, true, 5, Enc::INT_SIGNED),
  };

  std::string v;
  put_u8(&v, 0x04);            // null bitmap: only attr5's bit set
  put_le(&v, uint32_t(-5), 4);  // attr0 = -5 (two's complement int32 LE)
  put_u8(&v, 3);              // non-target varlen: length 3
  v.append("abc");           // ...data (skipped)
  put_le(&v, 300, 4);        // attr1 = 300
  put_f64(&v, 2.5);          // attr2 = 2.5
  put_le(&v, 0x332211, 3);   // attr3 packed date
  put_u8(&v, 2);             // attr4 varlen length 2
  v.append("hi");            // ...data
  // attr5 is NULL -> no bytes

  auto out = run(plan, make_key(kIdxNo), v);

  ASSERT_EQ(out.size(), 6u);
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[0]));
  EXPECT_EQ(std::get<uint64_t>(out[0]), bit_lsm::I64ToOkey(-5));
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[1]));
  EXPECT_EQ(std::get<uint64_t>(out[1]), bit_lsm::U64ToOkey(300));
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[2]));
  EXPECT_EQ(std::get<uint64_t>(out[2]), bit_lsm::F64ToOkey(2.5));
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[3]));
  EXPECT_EQ(std::get<uint64_t>(out[3]), bit_lsm::U64ToOkey(0x332211));
  ASSERT_TRUE(std::holds_alternative<std::string_view>(out[4]));
  EXPECT_EQ(std::get<std::string_view>(out[4]), std::string_view("hi"));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(out[5]));  // NULL
}

// CR-1: a row whose key prefix does not match this table's PK index number must
// yield all-monostate (it belongs to a foreign index sharing the CF). A too-
// short key is treated the same way.
TEST(BitlsmDecoder, ForeignRowAndShortKeyAreAllNull) {
  Rdb_bitlsm_attr_plan plan;
  plan.attr_num = 1;
  plan.null_bytes_len = 0;
  plan.pk_index_number = 42;
  plan.walk = {fixed(4, false, 0, 0, true, 0, Enc::INT_SIGNED)};

  std::string v;
  put_le(&v, uint32_t(-5), 4);

  // Matching index number -> extracts normally (sanity anchor).
  {
    auto out = run(plan, make_key(42), v);
    ASSERT_TRUE(std::holds_alternative<uint64_t>(out[0]));
    EXPECT_EQ(std::get<uint64_t>(out[0]), bit_lsm::I64ToOkey(-5));
  }
  // Different index number -> foreign row -> monostate.
  {
    auto out = run(plan, make_key(99), v);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(out[0]));
  }
  // Key shorter than 4 bytes -> monostate.
  {
    auto out = run(plan, std::string_view("\x00\x00", 2), v);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(out[0]));
  }
}

// Sign extension for sub-8-byte widths (INT24, TINY) plus okey monotonicity for
// signed integers.
TEST(BitlsmDecoder, SignExtensionAndSignedMonotonicity) {
  // INT24 = -3, then a TINY = -1, both signed.
  Rdb_bitlsm_attr_plan plan;
  plan.attr_num = 2;
  plan.null_bytes_len = 0;
  plan.pk_index_number = 7;
  plan.walk = {fixed(3, false, 0, 0, true, 0, Enc::INT_SIGNED),
               fixed(1, false, 0, 0, true, 1, Enc::INT_SIGNED)};

  std::string v;
  put_le(&v, uint32_t(-3) & 0xffffff, 3);  // int24 -3 == 0xFFFFFD
  put_le(&v, uint32_t(-1) & 0xff, 1);      // int8 -1 == 0xFF

  auto out = run(plan, make_key(7), v);
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[0]));
  EXPECT_EQ(std::get<uint64_t>(out[0]), bit_lsm::I64ToOkey(-3));
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[1]));
  EXPECT_EQ(std::get<uint64_t>(out[1]), bit_lsm::I64ToOkey(-1));

  // Order preservation: -3 < 0 < 5 must hold in okey space.
  EXPECT_LT(bit_lsm::I64ToOkey(-3), bit_lsm::I64ToOkey(0));
  EXPECT_LT(bit_lsm::I64ToOkey(0), bit_lsm::I64ToOkey(5));
}

// FLOAT (4B) decode and okey monotonicity for floating point (including across
// the sign boundary).
TEST(BitlsmDecoder, Float32AndFloatMonotonicity) {
  Rdb_bitlsm_attr_plan plan;
  plan.attr_num = 1;
  plan.null_bytes_len = 0;
  plan.pk_index_number = 7;
  plan.walk = {fixed(4, false, 0, 0, true, 0, Enc::FLOAT32)};

  std::string v;
  put_f32(&v, 1.5f);

  auto out = run(plan, make_key(7), v);
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[0]));
  EXPECT_EQ(std::get<uint64_t>(out[0]), bit_lsm::F64ToOkey(static_cast<double>(1.5f)));

  EXPECT_LT(bit_lsm::F64ToOkey(-2.0), bit_lsm::F64ToOkey(0.0));
  EXPECT_LT(bit_lsm::F64ToOkey(0.0), bit_lsm::F64ToOkey(3.5));
}

// Unsigned-integer decode plus the TTL prefix + unpack_info block skip. The
// null bitmap sits AFTER the 8-byte TTL; the unpack block is [tag][u16 BE len].
TEST(BitlsmDecoder, TtlAndUnpackInfoSkip) {
  Rdb_bitlsm_attr_plan plan;
  plan.attr_num = 1;
  plan.ttl_bytes = 8;             // PK has TTL
  plan.null_bytes_len = 1;        // one nullable field, present
  plan.maybe_unpack_info = true;  // an unpack_info block precedes the fields
  plan.pk_index_number = 7;
  plan.walk = {fixed(4, true, 0, 0x01, true, 0, Enc::INT_UNSIGNED)};

  std::string v;
  put_le(&v, 0, 8);      // 8-byte TTL (contents irrelevant)
  put_u8(&v, 0x00);      // null bitmap: attr present
  // unpack_info block: tag(0x02) + u16 big-endian total-length(5) + 2 payload
  put_u8(&v, 0x02);
  put_u8(&v, 0x00);
  put_u8(&v, 0x05);      // total block length = 5 bytes
  put_u8(&v, 0xAA);
  put_u8(&v, 0xBB);      // 5 - 3(header) = 2 payload bytes
  put_le(&v, 4000000000u, 4);  // attr0 unsigned, > INT_MAX

  auto out = run(plan, make_key(7), v);
  ASSERT_TRUE(std::holds_alternative<uint64_t>(out[0]));
  EXPECT_EQ(std::get<uint64_t>(out[0]), bit_lsm::U64ToOkey(4000000000u));
}

}  // namespace
