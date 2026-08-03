/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include "./rdb_bitlsm_descriptor.h"

#include <gtest/gtest.h>

namespace myrocks {
namespace {

Rdb_bitlsm_descriptor make_desc() {
  Rdb_bitlsm_descriptor d;
  d.schema.roles = {bit_lsm::ORDERED, bit_lsm::UNORDERED, bit_lsm::ORDERED};
  d.schema.rho = 0.01;

  d.plan.attr_num = 3;
  d.plan.ttl_bytes = 8;
  d.plan.null_bytes_len = 2;
  d.plan.maybe_unpack_info = true;
  d.plan.pk_index_number = 261;

  Rdb_bitlsm_attr_plan::WalkEntry e0{};
  e0.shape = Rdb_bitlsm_attr_plan::Shape::FIXED;
  e0.len = 4;
  e0.nullable = true;
  e0.null_offset = 0;
  e0.null_mask = 0x2;
  e0.is_target = true;
  e0.attr_index = 0;
  e0.enc = Rdb_bitlsm_attr_plan::Enc::INT_SIGNED;

  Rdb_bitlsm_attr_plan::WalkEntry e1{};
  e1.shape = Rdb_bitlsm_attr_plan::Shape::VARLEN;
  e1.len = 1;
  e1.nullable = false;
  e1.is_target = true;
  e1.attr_index = 1;
  e1.enc = Rdb_bitlsm_attr_plan::Enc::BINARY_STR;

  Rdb_bitlsm_attr_plan::WalkEntry e2{};
  e2.shape = Rdb_bitlsm_attr_plan::Shape::FIXED;
  e2.len = 3;
  e2.nullable = true;
  e2.null_offset = 1;
  e2.null_mask = 0x80;
  e2.is_target = true;
  e2.attr_index = 2;
  e2.enc = Rdb_bitlsm_attr_plan::Enc::DATE3;

  d.plan.walk = {e0, e1, e2};
  return d;
}

TEST(RdbBitlsmDescriptor, RoundTrip) {
  const Rdb_bitlsm_descriptor in = make_desc();
  const std::string blob = rdb_bitlsm_serialize_descriptor(in);

  Rdb_bitlsm_descriptor out;
  ASSERT_TRUE(rdb_bitlsm_deserialize_descriptor(blob, &out));

  EXPECT_EQ(out.schema.roles, in.schema.roles);
  EXPECT_DOUBLE_EQ(out.schema.rho, in.schema.rho);
  EXPECT_EQ(out.plan.attr_num, in.plan.attr_num);
  EXPECT_EQ(out.plan.ttl_bytes, in.plan.ttl_bytes);
  EXPECT_EQ(out.plan.null_bytes_len, in.plan.null_bytes_len);
  EXPECT_EQ(out.plan.maybe_unpack_info, in.plan.maybe_unpack_info);
  EXPECT_EQ(out.plan.pk_index_number, in.plan.pk_index_number);
  ASSERT_EQ(out.plan.walk.size(), in.plan.walk.size());
  for (size_t i = 0; i < in.plan.walk.size(); ++i) {
    const auto &a = in.plan.walk[i];
    const auto &b = out.plan.walk[i];
    EXPECT_EQ(static_cast<int>(b.shape), static_cast<int>(a.shape)) << i;
    EXPECT_EQ(b.len, a.len) << i;
    EXPECT_EQ(b.nullable, a.nullable) << i;
    EXPECT_EQ(b.null_offset, a.null_offset) << i;
    EXPECT_EQ(b.null_mask, a.null_mask) << i;
    EXPECT_EQ(b.is_target, a.is_target) << i;
    EXPECT_EQ(b.attr_index, a.attr_index) << i;
    EXPECT_EQ(static_cast<int>(b.enc), static_cast<int>(a.enc)) << i;
  }
}

// Two descriptors that differ in ANY field must serialize differently: the
// open-time verification compares serialized bytes and nothing else.
TEST(RdbBitlsmDescriptor, ByteComparisonDetectsSchemaChange) {
  const std::string a = rdb_bitlsm_serialize_descriptor(make_desc());

  Rdb_bitlsm_descriptor null_mask_changed = make_desc();
  null_mask_changed.plan.walk[0].null_mask = 0x4;
  EXPECT_NE(a, rdb_bitlsm_serialize_descriptor(null_mask_changed));

  Rdb_bitlsm_descriptor role_changed = make_desc();
  role_changed.schema.roles[1] = bit_lsm::ORDERED;
  EXPECT_NE(a, rdb_bitlsm_serialize_descriptor(role_changed));

  Rdb_bitlsm_descriptor rho_changed = make_desc();
  rho_changed.schema.rho = 0.1;
  EXPECT_NE(a, rdb_bitlsm_serialize_descriptor(rho_changed));

  Rdb_bitlsm_descriptor field_added = make_desc();
  field_added.plan.walk.push_back(field_added.plan.walk.back());
  EXPECT_NE(a, rdb_bitlsm_serialize_descriptor(field_added));
}

TEST(RdbBitlsmDescriptor, RejectsUnknownVersion) {
  std::string blob = rdb_bitlsm_serialize_descriptor(make_desc());
  blob[0] = static_cast<char>(0xEE);  // corrupt the version field
  Rdb_bitlsm_descriptor out;
  EXPECT_FALSE(rdb_bitlsm_deserialize_descriptor(blob, &out));
}

TEST(RdbBitlsmDescriptor, RejectsTruncated) {
  const std::string blob = rdb_bitlsm_serialize_descriptor(make_desc());
  Rdb_bitlsm_descriptor out;
  for (size_t cut : {size_t{0}, size_t{1}, blob.size() / 2, blob.size() - 1}) {
    EXPECT_FALSE(rdb_bitlsm_deserialize_descriptor(
        std::string_view(blob.data(), cut), &out))
        << "cut=" << cut;
  }
}

TEST(RdbBitlsmDescriptor, RejectsTrailingGarbage) {
  std::string blob = rdb_bitlsm_serialize_descriptor(make_desc());
  blob.push_back('\0');
  Rdb_bitlsm_descriptor out;
  EXPECT_FALSE(rdb_bitlsm_deserialize_descriptor(blob, &out));
}

TEST(RdbBitlsmDescriptor, RejectsOutOfRangeEnums) {
  const std::string good = rdb_bitlsm_serialize_descriptor(make_desc());

  // Role byte sits right after [version u16][role_count u32].
  std::string bad_role = good;
  bad_role[2 + 4] = static_cast<char>(0x7F);
  Rdb_bitlsm_descriptor out;
  EXPECT_FALSE(rdb_bitlsm_deserialize_descriptor(bad_role, &out));

  // The last byte of the blob is the final walk entry's Enc.
  std::string bad_enc = good;
  bad_enc.back() = static_cast<char>(0x7F);
  EXPECT_FALSE(rdb_bitlsm_deserialize_descriptor(bad_enc, &out));
}

}  // namespace
}  // namespace myrocks
