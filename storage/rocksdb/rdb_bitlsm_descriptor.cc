/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include "./rdb_bitlsm_descriptor.h"

#include <cstring>

namespace myrocks {

namespace {

void put_u8(std::string *out, uint8_t v) {
  out->push_back(static_cast<char>(v));
}

void put_u16(std::string *out, uint16_t v) {
  for (int i = 0; i < 2; ++i) put_u8(out, static_cast<uint8_t>(v >> (8 * i)));
}

void put_u32(std::string *out, uint32_t v) {
  for (int i = 0; i < 4; ++i) put_u8(out, static_cast<uint8_t>(v >> (8 * i)));
}

void put_u64(std::string *out, uint64_t v) {
  for (int i = 0; i < 8; ++i) put_u8(out, static_cast<uint8_t>(v >> (8 * i)));
}

void put_double(std::string *out, double v) {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  put_u64(out, bits);
}

// Cursor over the blob. Every read is bounds-checked and sets m_bad on failure,
// so callers can run the whole parse and check once at the end.
class Reader {
 public:
  explicit Reader(std::string_view b) : m_buf(b) {}

  uint8_t u8() {
    if (m_pos + 1 > m_buf.size()) {
      m_bad = true;
      return 0;
    }
    return static_cast<uint8_t>(m_buf[m_pos++]);
  }
  uint16_t u16() {
    uint16_t v = 0;
    for (int i = 0; i < 2; ++i) v |= static_cast<uint16_t>(u8()) << (8 * i);
    return v;
  }
  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
    return v;
  }
  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
    return v;
  }
  double dbl() {
    const uint64_t bits = u64();
    double v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }

  bool bad() const { return m_bad; }
  bool at_end() const { return !m_bad && m_pos == m_buf.size(); }
  // Sanity-bound an element count before reserving: every element costs at
  // least one byte, so a count larger than what is left is corrupt.
  bool count_fits(uint32_t n) const {
    return !m_bad && n <= m_buf.size() - m_pos;
  }

 private:
  std::string_view m_buf;
  size_t m_pos = 0;
  bool m_bad = false;
};

}  // namespace

std::string rdb_bitlsm_serialize_descriptor(
    const Rdb_bitlsm_descriptor &desc) {
  std::string out;
  put_u16(&out, RDB_BITLSM_DESCRIPTOR_VERSION);

  // --- schema ---
  put_u32(&out, static_cast<uint32_t>(desc.schema.roles.size()));
  for (const auto role : desc.schema.roles) {
    put_u8(&out, static_cast<uint8_t>(role));
  }
  put_double(&out, desc.schema.rho);

  // --- plan header ---
  put_u32(&out, desc.plan.attr_num);
  put_u32(&out, desc.plan.ttl_bytes);
  put_u32(&out, desc.plan.null_bytes_len);
  put_u8(&out, desc.plan.maybe_unpack_info ? 1 : 0);
  put_u32(&out, desc.plan.pk_index_number);

  // --- walk list ---
  put_u32(&out, static_cast<uint32_t>(desc.plan.walk.size()));
  for (const auto &e : desc.plan.walk) {
    put_u8(&out, static_cast<uint8_t>(e.shape));
    put_u32(&out, e.len);
    put_u8(&out, e.nullable ? 1 : 0);
    put_u32(&out, e.null_offset);
    put_u8(&out, e.null_mask);
    put_u8(&out, e.is_target ? 1 : 0);
    put_u32(&out, e.attr_index);
    put_u8(&out, static_cast<uint8_t>(e.enc));
  }
  return out;
}

bool rdb_bitlsm_deserialize_descriptor(std::string_view blob,
                                       Rdb_bitlsm_descriptor *out) {
  Reader r(blob);
  if (r.u16() != RDB_BITLSM_DESCRIPTOR_VERSION || r.bad()) return false;

  const uint32_t role_count = r.u32();
  if (!r.count_fits(role_count)) return false;
  out->schema.roles.clear();
  out->schema.roles.reserve(role_count);
  for (uint32_t i = 0; i < role_count; ++i) {
    const uint8_t v = r.u8();
    if (v > static_cast<uint8_t>(bit_lsm::ORDERED)) return false;
    out->schema.roles.push_back(static_cast<bit_lsm::AttrRole>(v));
  }
  out->schema.rho = r.dbl();

  out->plan.attr_num = r.u32();
  out->plan.ttl_bytes = r.u32();
  out->plan.null_bytes_len = r.u32();
  out->plan.maybe_unpack_info = r.u8() != 0;
  out->plan.pk_index_number = r.u32();
  if (r.bad()) return false;

  const uint32_t walk_count = r.u32();
  if (!r.count_fits(walk_count)) return false;
  out->plan.walk.clear();
  out->plan.walk.reserve(walk_count);
  for (uint32_t i = 0; i < walk_count; ++i) {
    Rdb_bitlsm_attr_plan::WalkEntry e{};
    const uint8_t shape = r.u8();
    if (shape > static_cast<uint8_t>(Rdb_bitlsm_attr_plan::Shape::BLOB)) {
      return false;
    }
    e.shape = static_cast<Rdb_bitlsm_attr_plan::Shape>(shape);
    e.len = r.u32();
    e.nullable = r.u8() != 0;
    e.null_offset = r.u32();
    e.null_mask = r.u8();
    e.is_target = r.u8() != 0;
    e.attr_index = r.u32();
    const uint8_t enc = r.u8();
    if (enc > static_cast<uint8_t>(Rdb_bitlsm_attr_plan::Enc::BINARY_STR)) {
      return false;
    }
    e.enc = static_cast<Rdb_bitlsm_attr_plan::Enc>(enc);
    out->plan.walk.push_back(e);
  }

  // Trailing bytes mean writer and reader disagree about the layout.
  return r.at_end();
}

}  // namespace myrocks
