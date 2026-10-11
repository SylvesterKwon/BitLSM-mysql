// Copyright (c) 2026 Dohyun Kwon

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

/* Only the BitLSM okey/EncodedAttr contract, not sabi.h: this header is the
   per-row hot path and must stay free of RocksDB/CRoaring headers. */
#include "bit_lsm_encoding.h"

namespace myrocks {

// Index type of one BITLSM_INDEX key part: the ORDERED / UNORDERED keyword if
// given, else kRange for numerics and kEquality for binary strings. Build-time
// binning (rdb_datadic.cc) and the query-time comparand (rdb_bitlsm_query.cc)
// must both derive the type here; a disagreement mis-bins rows.
inline bit_lsm::IndexType rdb_bitlsm_index_type_for(bool is_binary,
                                                    bool declared_ordered,
                                                    bool declared_unordered) {
  if (declared_ordered) return bit_lsm::IndexType::kRange;
  if (declared_unordered) return bit_lsm::IndexType::kEquality;
  return is_binary ? bit_lsm::IndexType::kEquality : bit_lsm::IndexType::kRange;
}

// Immutable plan for extracting the BITLSM attribute columns from a MyRocks
// primary-key value blob. Built once at bind time from TABLE/Field metadata and
// shared read-only; the flush/compaction path never touches a TABLE or Field.
//
// Value blob layout (see Rdb_converter::encode_value_slice):
//   [TTL 8B]? [null bitmap : null_bytes_len B] [unpack_info block]?
//   [STORE_ALL field data, in table field order]
struct Rdb_bitlsm_attr_plan {
  // How a target field's bytes become an EncodedAttr. SABI compares attributes
  // by memcmp, so numeric fields map to a monotone okey emitted as 8 big-endian
  // bytes; binary fields are emitted as-is.
  enum class Enc : uint8_t {
    INT_SIGNED,    // little-endian, sign-extend `len` bytes -> I64ToOkey
    INT_UNSIGNED,  // little-endian `len` bytes -> U64ToOkey
    FLOAT32,       // 4B IEEE-754 LE -> double -> F64ToOkey
    FLOAT64,       // 8B IEEE-754 LE -> F64ToOkey
    DATE3,         // 3B LE packed NEWDATE (monotone in date order) -> U64ToOkey
    BINARY_STR,    // binary: std::string_view over the raw data bytes
  };

  // Physical shape of a STORE_ALL value field.
  enum class Shape : uint8_t {
    FIXED,   // `len` bytes verbatim (int/float/temporal/CHAR-BINARY)
    VARLEN,  // [len-prefix `len` bytes LE][data]  (VARCHAR/VARBINARY)
    BLOB,    // [len-prefix `len` bytes LE][data]  (BLOB/JSON)
  };

  struct WalkEntry {
    Shape shape;
    // FIXED: total field byte count. VARLEN/BLOB: number of length-prefix bytes.
    uint32_t len;
    bool nullable;
    uint32_t null_offset;  // byte index into the null bitmap
    uint8_t null_mask;     // bit within that byte
    bool is_target;        // true => this field is a BITLSM attribute
    uint32_t attr_index;   // valid iff is_target: slot in out[]
    Enc enc;               // valid iff is_target
  };

  uint32_t attr_num = 0;          // # of out[] slots (schema.attr_num())
  uint32_t ttl_bytes = 0;         // 0, or ROCKSDB_SIZEOF_TTL_RECORD if PK has TTL
  uint32_t null_bytes_len = 0;    // Nnull = ceil(#nullable table fields / 8)
  bool maybe_unpack_info = false;  // PK has an unpack_info block in the value
  uint32_t pk_index_number = 0;   // 4B big-endian key prefix of this table's PK
  // STORE_ALL, non-virtual fields in value order, truncated after the last
  // target.
  std::vector<WalkEntry> walk;
};

// Per-row attribute decoder, run on the flush/compaction thread. One linear
// walk of the value, no heap allocation: binary attrs view `value`, numeric
// attrs view per-attr scratch. Both views are valid only for the current call.
class Rdb_bitlsm_extractor : public bit_lsm::AttrExtractor {
 public:
  explicit Rdb_bitlsm_extractor(
      std::shared_ptr<const Rdb_bitlsm_attr_plan> plan)
      : m_plan(std::move(plan)),
        m_okey_scratch(
            static_cast<size_t>(m_plan->attr_num) * bit_lsm::kOkeyBytes,
            '\0') {}

  void ExtractAll(std::string_view key, std::string_view value,
                  bit_lsm::EncodedAttr *out) override {
    const Rdb_bitlsm_attr_plan &p = *m_plan;
    // Default every attribute to SQL NULL: the row then lands in no value bin.
    for (uint32_t i = 0; i < p.attr_num; ++i) out[i] = std::monostate{};

    // The PK data CF may also hold rows of other indexes. A row is ours iff
    // its 4-byte big-endian key prefix is this table's PK index number.
    if (key.size() < 4) return;
    const auto *k = reinterpret_cast<const unsigned char *>(key.data());
    const uint32_t idx = (static_cast<uint32_t>(k[0]) << 24) |
                         (static_cast<uint32_t>(k[1]) << 16) |
                         (static_cast<uint32_t>(k[2]) << 8) |
                         static_cast<uint32_t>(k[3]);
    if (idx != p.pk_index_number) return;

    const char *base = value.data();
    const size_t vlen = value.size();

    // [TTL?][null bitmap][unpack_info?][field data...]
    size_t cursor = static_cast<size_t>(p.ttl_bytes) + p.null_bytes_len;
    if (cursor > vlen) return;  // truncated / unexpected layout: bail safely
    const unsigned char *nullmap =
        reinterpret_cast<const unsigned char *>(base) + p.ttl_bytes;

    if (p.maybe_unpack_info) {
      // [tag u8][len u16 big-endian] ...; the u16 is the total block length.
      if (cursor + 3 > vlen) return;
      const uint16_t blk =
          (static_cast<uint16_t>(static_cast<uint8_t>(base[cursor + 1])) << 8) |
          static_cast<uint16_t>(static_cast<uint8_t>(base[cursor + 2]));
      cursor += blk;
      if (cursor > vlen) return;
    }

    for (const auto &e : p.walk) {
      if (e.nullable && (nullmap[e.null_offset] & e.null_mask)) {
        continue;  // NULL fields are not written to the value
      }
      size_t foff;
      size_t flen;
      if (e.shape == Rdb_bitlsm_attr_plan::Shape::FIXED) {
        foff = cursor;
        flen = e.len;
        cursor += flen;
      } else {  // VARLEN / BLOB: read the little-endian length prefix.
        const uint32_t pfx = e.len;
        if (cursor + pfx > vlen) return;
        uint32_t dlen = 0;
        for (uint32_t i = 0; i < pfx; ++i) {
          dlen |= static_cast<uint32_t>(static_cast<uint8_t>(base[cursor + i]))
                  << (8 * i);
        }
        foff = cursor + pfx;
        flen = dlen;
        cursor += static_cast<size_t>(pfx) + dlen;
      }
      if (cursor > vlen) return;  // bounds: field bytes exceed the value
      if (!e.is_target) continue;

      switch (e.enc) {
        case Rdb_bitlsm_attr_plan::Enc::INT_SIGNED:
          out[e.attr_index] = okey_bytes(
              e.attr_index,
              bit_lsm::I64ToOkey(sign_extend(
                  read_le(base + foff, static_cast<unsigned>(flen)),
                  static_cast<unsigned>(flen))));
          break;
        case Rdb_bitlsm_attr_plan::Enc::INT_UNSIGNED:
          out[e.attr_index] = okey_bytes(
              e.attr_index, bit_lsm::U64ToOkey(read_le(
                                base + foff, static_cast<unsigned>(flen))));
          break;
        case Rdb_bitlsm_attr_plan::Enc::FLOAT32: {
          float f;
          std::memcpy(&f, base + foff, 4);
          out[e.attr_index] =
              okey_bytes(e.attr_index,
                         bit_lsm::F64ToOkey(static_cast<double>(f)));
          break;
        }
        case Rdb_bitlsm_attr_plan::Enc::FLOAT64: {
          double d;
          std::memcpy(&d, base + foff, 8);
          out[e.attr_index] = okey_bytes(e.attr_index, bit_lsm::F64ToOkey(d));
          break;
        }
        case Rdb_bitlsm_attr_plan::Enc::DATE3:
          out[e.attr_index] = okey_bytes(
              e.attr_index, bit_lsm::U64ToOkey(read_le(base + foff, 3)));
          break;
        case Rdb_bitlsm_attr_plan::Enc::BINARY_STR:
          out[e.attr_index] = std::string_view(base + foff, flen);
          break;
      }
    }
  }

 private:
  // Write the okey as 8 big-endian bytes into this attribute's scratch slot.
  // One slot per attribute keeps every returned view valid for the whole call.
  inline std::string_view okey_bytes(uint32_t attr_index, uint64_t okey) {
    char *p = m_okey_scratch.data() +
              static_cast<size_t>(attr_index) * bit_lsm::kOkeyBytes;
    bit_lsm::OkeyToBytes(okey, p);
    return std::string_view(p, bit_lsm::kOkeyBytes);
  }

  // Read `len` (<= 8) little-endian bytes as an unsigned 64-bit value.
  static inline uint64_t read_le(const char *p, unsigned len) {
    uint64_t v = 0;
    for (unsigned i = 0; i < len; ++i) {
      v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    }
    return v;
  }

  // Sign-extend a `len`-byte (<= 8) value to int64.
  static inline int64_t sign_extend(uint64_t v, unsigned len) {
    if (len >= 8) return static_cast<int64_t>(v);
    const uint64_t sign = uint64_t{1} << (8 * len - 1);
    if (v & sign) v |= ~((uint64_t{1} << (8 * len)) - 1);
    return static_cast<int64_t>(v);
  }

  std::shared_ptr<const Rdb_bitlsm_attr_plan> m_plan;
  std::string m_okey_scratch;  // attr_num * kOkeyBytes, see okey_bytes()
};

}  // namespace myrocks
