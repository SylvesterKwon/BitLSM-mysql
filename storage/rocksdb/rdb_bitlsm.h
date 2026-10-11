// Copyright (c) 2026 Dohyun Kwon

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_estimator.h"
#include "bit_lsm_option.h"
#include "rocksdb/listener.h"
#include "sabi.h"

#include "./rdb_bitlsm_extractor.h"

namespace myrocks {

// Identity of one attribute for the one-schema-per-CF check. Index type alone
// is not enough (an INT and a VARBINARY attr can both be kRange), so the key
// also pins the extractor encoding and the byte width.
struct Rdb_bitlsm_attr_key {
  bit_lsm::IndexType index_type;
  Rdb_bitlsm_attr_plan::Enc enc;
  uint32_t len;  // FIXED: field byte count; VARLEN/BLOB: length-prefix size
  bool operator==(const Rdb_bitlsm_attr_key &) const = default;
};
using Rdb_bitlsm_schema_key = std::vector<Rdb_bitlsm_attr_key>;

Rdb_bitlsm_schema_key rdb_bitlsm_schema_key_of(
    const bit_lsm::SABISchema &schema, const Rdb_bitlsm_attr_plan &plan);

// Process-global map cf_name -> bound SABIFactory (plus the CF's cardinality
// estimator). Written at table open and DB open; read by the per-CF UDI
// factory at SST build. Guarded by an internal mutex.
class Rdb_bitlsm_registry {
 public:
  static Rdb_bitlsm_registry &instance();

  // Returns false if cf_name is already bound to a different schema (at most
  // one bitlsm schema per CF). Rebinding the same schema is idempotent.
  bool bind(const std::string &cf_name, Rdb_bitlsm_schema_key schema_key,
            std::shared_ptr<bit_lsm::SABIFactory> factory);

  // nullptr if cf_name is not bound.
  std::shared_ptr<bit_lsm::SABIFactory> get(const std::string &cf_name) const;

  // Attach the planning-time cardinality estimator to a bound CF. Idempotent:
  // an already-running estimator is kept. Call after bind().
  void estimator_attach(const std::string &cf_name, rocksdb::DB *base_db,
                        rocksdb::ColumnFamilyHandle *cfh,
                        bit_lsm::SABISchema schema,
                        const bit_lsm::BitLSMOptions &options);
  // nullptr when the CF has no estimator. Valid until estimator_destroy /
  // estimator_shutdown, which run only while no planning can be in flight.
  bit_lsm::CardinalityEstimator *estimator_get(const std::string &cf_name) const;
  // Wake the CF's refresh worker after a flush/compaction. No-op without one.
  void estimator_notify(const std::string &cf_name);
  // Join the worker before the CF handle dies; it references ColumnFamilyData.
  void estimator_destroy(const std::string &cf_name);
  // Join every worker before the DB closes.
  void estimator_shutdown();
  // Synchronously refresh every estimator's stats (blocks through the worker's
  // cooldown). Must not race estimator_destroy / estimator_shutdown. Returns
  // the number refreshed; 0 means nothing is attached yet.
  size_t estimator_refresh_all();

  // Record that a persisted SABI descriptor exists for cf_name even if it
  // failed to load, so an SST build on this CF fails instead of silently
  // omitting SABI. Call after bind(), or bind() would see a schema conflict.
  void mark_expected(const std::string &cf_name);
  bool expects_sabi(const std::string &cf_name) const;

 private:
  Rdb_bitlsm_registry() = default;
  struct Entry {
    Rdb_bitlsm_schema_key schema_key;
    std::shared_ptr<bit_lsm::SABIFactory> factory;
    std::unique_ptr<bit_lsm::CardinalityEstimator> estimator;  // may be null
    bool expected = false;  // a persisted descriptor exists for this CF
  };
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, Entry> m_map;
};

// DB-wide flush/compaction listener. RocksDB fixes the listener list at
// DB::Open, before any bitlsm CF is bound, so it is registered unconditionally
// and fans events out through the registry (a hash miss for other CFs).
class Rdb_bitlsm_stats_listener : public rocksdb::EventListener {
 public:
  void OnFlushCompleted(rocksdb::DB *,
                        const rocksdb::FlushJobInfo &info) override;
  void OnCompactionCompleted(rocksdb::DB *,
                             const rocksdb::CompactionJobInfo &info) override;
};

// Per-CF UDI factory installed on every data CF. Build path: a CF without a
// registry entry gets a null builder (RocksDB skips the UDI wrapper); an entry
// without a factory fails the build rather than writing an SST without SABI.
// Read path: registry-independent, since SABI blobs are self-describing and
// RocksDB opens SST readers at DB open, before any CF is bound.
class Rdb_bitlsm_udi_factory : public rocksdb::UserDefinedIndexFactory {
 public:
  explicit Rdb_bitlsm_udi_factory(std::string cf_name)
      : m_cf_name(std::move(cf_name)) {}
  const char *Name() const override { return "bitlsm.sabi"; }
  rocksdb::UserDefinedIndexBuilder *NewBuilder() const override;
  rocksdb::Status NewBuilder(
      const rocksdb::UserDefinedIndexOption &option,
      std::unique_ptr<rocksdb::UserDefinedIndexBuilder> &builder)
      const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice &index_block) const override;
  // Called at SST open; validates the SABI footer and self-describes.
  rocksdb::Status NewReader(
      const rocksdb::UserDefinedIndexOption &option, rocksdb::Slice &index_block,
      std::unique_ptr<rocksdb::UserDefinedIndexReader> &reader) const override;

 private:
  std::string m_cf_name;
};

// Rebuild a SABIFactory from a persisted descriptor blob and bind it to
// cf_name. Called at DB open before auto compactions resume. Returns false on
// an undecodable blob or a schema conflict on that CF.
bool rdb_bitlsm_bind_persisted(const std::string &cf_name,
                               const std::string &blob);

}  // namespace myrocks
