/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/* BitLSM headers (compiled into rocksdb_se from the BitLSM submodule). */
#include "bit_lsm_encoding.h"  // bit_lsm::AttrExtractor, EncodedAttr, SABISchema
#include "sabi.h"              // bit_lsm::SABIFactory

namespace myrocks {

// Process-global map cf_name -> bound SABIFactory. Written by
// Rdb_key_def::setup_bitlsm_index at table open; read by the per-CF UDI factory
// at SST build/open (cold paths). Guarded by an internal mutex.
class Rdb_bitlsm_registry {
 public:
  static Rdb_bitlsm_registry &instance();

  // Bind a CF to a SABIFactory. Returns false if cf_name is already bound to a
  // DIFFERENT schema (D5: <=1 bitlsm schema per CF) so the caller can fail the
  // open loudly. Same-schema rebind (reopen) is idempotent OK.
  bool bind(const std::string &cf_name, std::vector<bit_lsm::AttrRole> roles,
            std::shared_ptr<bit_lsm::SABIFactory> factory);

  // Returns nullptr if cf_name is not bound (non-bitlsm or not yet opened).
  std::shared_ptr<bit_lsm::SABIFactory> get(const std::string &cf_name) const;

 private:
  Rdb_bitlsm_registry() = default;
  struct Entry {
    std::vector<bit_lsm::AttrRole> roles;
    std::shared_ptr<bit_lsm::SABIFactory> factory;
  };
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, Entry> m_map;
};

// Per-CF UDI factory installed on every data CF's BlockBasedTableOptions. Knows
// only its cf_name; dispatches to the bound SABIFactory via the registry.
// Unbound -> null builder/reader -> RocksDB skips the UDI wrapper (zero
// per-row cost on non-bitlsm / not-yet-bound CFs).
class Rdb_bitlsm_udi_factory : public rocksdb::UserDefinedIndexFactory {
 public:
  explicit Rdb_bitlsm_udi_factory(std::string cf_name)
      : m_cf_name(std::move(cf_name)) {}
  const char *Name() const override { return "bitlsm.sabi"; }
  rocksdb::UserDefinedIndexBuilder *NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice &index_block) const override;

 private:
  std::string m_cf_name;
};

// No-op extractor: yields SQL NULL for every attribute. M3a-3b uses this so the
// SABI block is produced without the real row decoder (M3a-4 replaces it).
class Rdb_bitlsm_noop_extractor : public bit_lsm::AttrExtractor {
 public:
  explicit Rdb_bitlsm_noop_extractor(uint32_t attr_num)
      : m_attr_num(attr_num) {}
  void ExtractAll(std::string_view /*key*/, std::string_view /*value*/,
                  bit_lsm::EncodedAttr *out) override {
    for (uint32_t i = 0; i < m_attr_num; ++i) out[i] = std::monostate{};
  }

 private:
  uint32_t m_attr_num;
};

}  // namespace myrocks
