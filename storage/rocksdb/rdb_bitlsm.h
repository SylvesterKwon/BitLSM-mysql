/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/* BitLSM headers (compiled into rocksdb_se from the BitLSM submodule). */
#include "bit_lsm_encoding.h"   // bit_lsm::AttrExtractor, EncodedAttr, SABISchema
#include "bit_lsm_estimator.h"  // bit_lsm::CardinalityEstimator (M5)
#include "bit_lsm_option.h"     // bit_lsm::BitLSMOptions
#include "rocksdb/listener.h"   // rocksdb::EventListener (stats refresh)
#include "sabi.h"               // bit_lsm::SABIFactory

/* Real per-row decoder + its bind-time plan (M3a-4). Kept in a separate,
   RocksDB-header-free unit so the hot path stays lightweight and unit-testable
   (see rdb_bitlsm_decoder_test.cc). */
#include "./rdb_bitlsm_extractor.h"  // Rdb_bitlsm_attr_plan, Rdb_bitlsm_extractor

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

  // M5 cardinality estimator (planning-time selectivity; lives on the bound
  // PK data CF, same place the SABI blobs do). Attach is idempotent across
  // table reopen: an already-running estimator is kept, never restarted.
  // Called from setup_bitlsm_index right after bind(), sysvar-gated.
  void estimator_attach(const std::string &cf_name, rocksdb::DB *base_db,
                        rocksdb::ColumnFamilyHandle *cfh,
                        bit_lsm::SABISchema schema,
                        const bit_lsm::BitLSMOptions &options);
  // nullptr when the CF has no estimator (unbound, sysvar off, or destroyed).
  // The pointer stays valid until estimator_destroy/estimator_shutdown -- both
  // run only while no planning can be in flight (manual CF drop, plugin
  // deinit), so callers may use it without extra pinning.
  bit_lsm::CardinalityEstimator *estimator_get(const std::string &cf_name) const;
  // Flush/compaction completed on cf_name -> wake its refresh worker (no-op
  // for CFs without an estimator).
  void estimator_notify(const std::string &cf_name);
  // Manual CF drop (rocksdb_delete_cf): join the worker BEFORE the CF handle
  // dies -- the worker references the ColumnFamilyData.
  void estimator_destroy(const std::string &cf_name);
  // Plugin deinit: join every worker before the DB closes.
  void estimator_shutdown();
  // Synchronously run one refresh pass on every estimator (blocks through
  // the worker's cooldown). Trigger sysvar rocksdb_bitlsm_estimator_refresh:
  // deterministic stats for MTR tests and benchmark harnesses. Callers must
  // not race estimator_destroy/estimator_shutdown (admin-op scope). Returns
  // the number of estimators refreshed -- 0 means nothing was bound yet
  // (estimators attach lazily at first table open), which callers should
  // surface rather than swallow.
  size_t estimator_refresh_all();

  // Marks cf_name as "a persisted SABI descriptor exists for this CF". Set at
  // DB open right after a successful bind, and also when a descriptor fails to
  // load, so the build path can tell an ordinary CF (no entry -> skip the UDI
  // wrapper) from a bitlsm CF whose builder is missing (-> fail the SST build).
  // Call AFTER bind(): this creates the entry if absent, and an entry with
  // empty roles would make a later bind() look like a D5 schema conflict.
  void mark_expected(const std::string &cf_name);
  bool expects_sabi(const std::string &cf_name) const;

 private:
  Rdb_bitlsm_registry() = default;
  struct Entry {
    std::vector<bit_lsm::AttrRole> roles;
    std::shared_ptr<bit_lsm::SABIFactory> factory;
    // M5: refresh worker + stats cache; empty until estimator_attach.
    std::unique_ptr<bit_lsm::CardinalityEstimator> estimator;
    // A BITLSM_INDEX_INFO record exists for this CF (set at DB open).
    bool expected = false;
  };
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, Entry> m_map;
};

// DB-wide flush/compaction listener registered unconditionally at
// rocksdb_init (RocksDB fixes the listener list at DB::Open, before any
// bitlsm CF is bound). Events fan out to the estimator of the event's CF via
// the registry -- a hash lookup + no-op for the overwhelmingly common
// non-bitlsm CFs. BitLSM's own single-target StatsRefreshListener is not used:
// the embedded server may host several bitlsm CFs.
class Rdb_bitlsm_stats_listener : public rocksdb::EventListener {
 public:
  void OnFlushCompleted(rocksdb::DB *,
                        const rocksdb::FlushJobInfo &info) override;
  void OnCompactionCompleted(rocksdb::DB *,
                             const rocksdb::CompactionJobInfo &info) override;
};

// Per-CF UDI factory installed on every data CF's BlockBasedTableOptions. Knows
// only its cf_name.
//
// Build path: dispatches to the bound SABIFactory via the registry. A CF the
// registry knows nothing about is an ordinary non-bitlsm CF -> null builder ->
// RocksDB skips the UDI wrapper (zero per-row cost). A CF that HAS a persisted
// descriptor but no factory is a broken invariant, not a fast path: the build
// fails with a non-OK Status rather than quietly finalizing an SST without its
// SABI block. Rdb_ddl_manager::populate binds every descriptor at DB open, so
// the binding no longer waits for someone to open the table.
//
// Read path: registry-independent. v5 SABI blobs are self-describing (the
// directory persists attr roles), so a reader opens an SST with no schema
// binding. This is essential: at DB open RocksDB creates SST readers BEFORE
// Rdb_key_def binds the CF, so a registry-dependent reader would return null
// and RocksDB would reject the SST as corrupt. NewReader delegates to a
// schema-less SABIFactory that validates the v5 footer (rejecting old v4 SSTs
// loudly) and self-describes.
class Rdb_bitlsm_udi_factory : public rocksdb::UserDefinedIndexFactory {
 public:
  explicit Rdb_bitlsm_udi_factory(std::string cf_name)
      : m_cf_name(std::move(cf_name)) {}
  const char *Name() const override { return "bitlsm.sabi"; }
  rocksdb::UserDefinedIndexBuilder *NewBuilder() const override;
  // RocksDB calls this overload at SST build. Three states:
  //   no entry           -> null builder + OK (ordinary non-bitlsm CF)
  //   entry with factory -> build SABI
  //   entry, no factory  -> non-OK; the SST build fails instead of silently
  //                         producing a SABI-less file. Unreachable once
  //                         Rdb_ddl_manager::populate has bound every CF with a
  //                         persisted descriptor -- an invariant check, not a
  //                         policy knob.
  rocksdb::Status NewBuilder(
      const rocksdb::UserDefinedIndexOption &option,
      std::unique_ptr<rocksdb::UserDefinedIndexBuilder> &builder)
      const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice &index_block) const override;
  // RocksDB calls this overload at SST open; it validates the v5 footer then
  // self-describes, so restart open no longer depends on CF-binding order.
  rocksdb::Status NewReader(
      const rocksdb::UserDefinedIndexOption &option, rocksdb::Slice &index_block,
      std::unique_ptr<rocksdb::UserDefinedIndexReader> &reader) const override;

 private:
  std::string m_cf_name;
};

// Rebuild a SABIFactory from a persisted descriptor blob (rdb_bitlsm_descriptor
// .h) and bind it to cf_name. Called from Rdb_ddl_manager::populate at DB open,
// before auto compactions are re-enabled, so every SST written afterwards
// carries its SABI block whether or not the table has been opened. Returns
// false on an undecodable blob or a D5 schema conflict on that CF.
bool rdb_bitlsm_bind_persisted(const std::string &cf_name,
                               const std::string &blob);

}  // namespace myrocks
