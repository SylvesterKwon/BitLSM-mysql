/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

/* Only the BitLSM okey/EncodedAttr contract -- deliberately NOT sabi.h. The
   per-row decoder is the hot path and must stay free of RocksDB/CRoaring
   headers so it can be unit-tested in isolation (rdb_bitlsm_decoder_test.cc). */
#include "bit_lsm_encoding.h"  // bit_lsm::AttrExtractor, EncodedAttr, *ToOkey

namespace myrocks {

// Immutable "cheat sheet" describing how to walk a MyRocks primary-key value
// blob and pull out the BITLSM attribute columns. Built ONCE at CF-bind time
// (table open, in Rdb_key_def::setup_bitlsm_index) from the TABLE/Field
// metadata, then shared read-only across every builder thread. The extractor
// touches only this snapshot -- never a TABLE*/Field on the flush/compaction
// path.
//
// Value blob layout it encodes (see Rdb_converter::encode_value_slice):
//   [TTL 8B]? [null bitmap : null_bytes_len B] [unpack_info block]?
//   [STORE_ALL field data, in table field order]
struct Rdb_bitlsm_attr_plan {
  // How a target field's bytes become an EncodedAttr. ORDERED roles map to a
  // monotone okey (uint64); the UNORDERED role emits opaque bytes.
  enum class Enc : uint8_t {
    INT_SIGNED,    // little-endian, sign-extend `len` bytes -> I64ToOkey
    INT_UNSIGNED,  // little-endian `len` bytes -> U64ToOkey
    FLOAT32,       // 4B IEEE-754 LE -> double -> F64ToOkey
    FLOAT64,       // 8B IEEE-754 LE -> F64ToOkey
    DATE3,         // 3B LE packed NEWDATE (monotone in date order) -> U64ToOkey
    BINARY_STR,    // UNORDERED: std::string_view over the raw data bytes
  };

  // Physical shape of a STORE_ALL value field: how to locate its bytes and
  // advance the walk cursor.
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
  // STORE_ALL, non-virtual fields in value order, truncated after the LAST
  // target (fields past it never need walking).
  std::vector<WalkEntry> walk;
};

// Real per-row decoder (replaces the M3a-3b all-monostate stub). Owns a shared,
// read-only pointer to the plan built at bind time.
//
// HOT PATH: exactly one virtual call (ExtractAll) per row on the
// flush/compaction thread; the body does a single linear walk of the value up
// to the last attribute plus a few arithmetic ops per attribute. No heap
// allocation, no TABLE*/Field access; UNORDERED attrs emit a std::string_view
// into `value` (valid only for the duration of this call).
class Rdb_bitlsm_extractor : public bit_lsm::AttrExtractor {
 public:
  explicit Rdb_bitlsm_extractor(
      std::shared_ptr<const Rdb_bitlsm_attr_plan> plan)
      : m_plan(std::move(plan)) {}

  void ExtractAll(std::string_view key, std::string_view value,
                  bit_lsm::EncodedAttr *out) override {
    const Rdb_bitlsm_attr_plan &p = *m_plan;
    // Default every attribute to SQL NULL: the row then lands in no value bin.
    for (uint32_t i = 0; i < p.attr_num; ++i) out[i] = std::monostate{};

    // CR-1 foreign-row filter. The SABI is built on the PK data CF, which can
    // also hold rows of other indexes sharing the CF. A row is ours iff its 4B
    // big-endian key prefix equals this table's PK index number; anything else
    // stays all-monostate.
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
      // [tag u8][len u16 big-endian(netbuf)] ...; total block length == that u16.
      if (cursor + 3 > vlen) return;
      const uint16_t blk =
          (static_cast<uint16_t>(static_cast<uint8_t>(base[cursor + 1])) << 8) |
          static_cast<uint16_t>(static_cast<uint8_t>(base[cursor + 2]));
      cursor += blk;
      if (cursor > vlen) return;
    }

    for (const auto &e : p.walk) {
      if (e.nullable && (nullmap[e.null_offset] & e.null_mask)) {
        // NULL: nothing written to the value; target (if any) stays monostate.
        continue;
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
          out[e.attr_index] = bit_lsm::I64ToOkey(
              sign_extend(read_le(base + foff, static_cast<unsigned>(flen)),
                          static_cast<unsigned>(flen)));
          break;
        case Rdb_bitlsm_attr_plan::Enc::INT_UNSIGNED:
          out[e.attr_index] = bit_lsm::U64ToOkey(
              read_le(base + foff, static_cast<unsigned>(flen)));
          break;
        case Rdb_bitlsm_attr_plan::Enc::FLOAT32: {
          float f;
          std::memcpy(&f, base + foff, 4);
          out[e.attr_index] = bit_lsm::F64ToOkey(static_cast<double>(f));
          break;
        }
        case Rdb_bitlsm_attr_plan::Enc::FLOAT64: {
          double d;
          std::memcpy(&d, base + foff, 8);
          out[e.attr_index] = bit_lsm::F64ToOkey(d);
          break;
        }
        case Rdb_bitlsm_attr_plan::Enc::DATE3:
          out[e.attr_index] = bit_lsm::U64ToOkey(read_le(base + foff, 3));
          break;
        case Rdb_bitlsm_attr_plan::Enc::BINARY_STR:
          out[e.attr_index] = std::string_view(base + foff, flen);
          break;
      }
    }
  }

 private:
  // Read `len` (<=8) little-endian bytes into an unsigned 64-bit value.
  static inline uint64_t read_le(const char *p, unsigned len) {
    uint64_t v = 0;
    for (unsigned i = 0; i < len; ++i) {
      v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    }
    return v;
  }

  // Sign-extend a `len`-byte (<=8) little-endian magnitude to int64.
  static inline int64_t sign_extend(uint64_t v, unsigned len) {
    if (len >= 8) return static_cast<int64_t>(v);
    const uint64_t sign = uint64_t{1} << (8 * len - 1);
    if (v & sign) v |= ~((uint64_t{1} << (8 * len)) - 1);
    return static_cast<int64_t>(v);
  }

  std::shared_ptr<const Rdb_bitlsm_attr_plan> m_plan;
};

}  // namespace myrocks
