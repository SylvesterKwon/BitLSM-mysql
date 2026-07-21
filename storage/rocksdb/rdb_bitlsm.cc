/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include "./rdb_bitlsm.h"

/* rocksdb-internal headers, needed only for the estimator_attach casts (the
   same DBImpl / ColumnFamilyHandleImpl casts BitLSMIterator and the standalone
   BitLSM ctor already exercise). Contained to this TU. */
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"

namespace myrocks {

Rdb_bitlsm_registry &Rdb_bitlsm_registry::instance() {
  static Rdb_bitlsm_registry inst;
  return inst;
}

bool Rdb_bitlsm_registry::bind(const std::string &cf_name,
                               std::vector<bit_lsm::AttrRole> roles,
                               std::shared_ptr<bit_lsm::SABIFactory> factory) {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_map.find(cf_name);
  if (it != m_map.end()) {
    if (it->second.roles != roles) {
      // D5 violation: CF already hosts a different bitlsm schema.
      return false;
    }
    // Same-schema rebind (table reopen): refresh the factory but keep the
    // running estimator -- replacing the Entry would join and restart its
    // worker on every reopen.
    it->second.factory = std::move(factory);
    return true;
  }
  m_map.emplace(cf_name,
                Entry{std::move(roles), std::move(factory), nullptr});
  return true;
}

void Rdb_bitlsm_registry::estimator_attach(
    const std::string &cf_name, rocksdb::DB *base_db,
    rocksdb::ColumnFamilyHandle *cfh, bit_lsm::SABISchema schema,
    const bit_lsm::BitLSMOptions &options) {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_map.find(cf_name);
  if (it == m_map.end()) return;     // bind() first (caller order)
  if (it->second.estimator) return;  // reopen: keep the running worker
  it->second.estimator = std::make_unique<bit_lsm::CardinalityEstimator>(
      static_cast<rocksdb::DBImpl *>(base_db),
      static_cast<rocksdb::ColumnFamilyHandleImpl *>(cfh)->cfd(),
      std::move(schema), options);
}

bit_lsm::CardinalityEstimator *Rdb_bitlsm_registry::estimator_get(
    const std::string &cf_name) const {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_map.find(cf_name);
  return it == m_map.end() ? nullptr : it->second.estimator.get();
}

void Rdb_bitlsm_registry::estimator_notify(const std::string &cf_name) {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_map.find(cf_name);
  if (it != m_map.end() && it->second.estimator) {
    it->second.estimator->NotifyChange();
  }
}

void Rdb_bitlsm_registry::estimator_destroy(const std::string &cf_name) {
  std::unique_ptr<bit_lsm::CardinalityEstimator> dead;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_map.find(cf_name);
    if (it != m_map.end()) dead = std::move(it->second.estimator);
  }
  // ~CardinalityEstimator joins the worker; run outside the registry lock so
  // a concurrent flush-completion notify cannot deadlock against the join.
}

void Rdb_bitlsm_registry::estimator_shutdown() {
  std::vector<std::unique_ptr<bit_lsm::CardinalityEstimator>> dead;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &kv : m_map) {
      if (kv.second.estimator) dead.push_back(std::move(kv.second.estimator));
    }
  }
  // Workers join here, outside the lock (same reasoning as estimator_destroy).
}

void Rdb_bitlsm_registry::estimator_refresh_all() {
  // Collect raw pointers under the lock, run the blocking refreshes outside
  // it: TEST_Refresh waits on the worker, and a concurrent flush completion
  // takes the registry lock in estimator_notify.
  std::vector<bit_lsm::CardinalityEstimator *> targets;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &kv : m_map) {
      if (kv.second.estimator) targets.push_back(kv.second.estimator.get());
    }
  }
  for (auto *est : targets) est->TEST_Refresh();
}

void Rdb_bitlsm_stats_listener::OnFlushCompleted(
    rocksdb::DB *, const rocksdb::FlushJobInfo &info) {
  Rdb_bitlsm_registry::instance().estimator_notify(info.cf_name);
}

void Rdb_bitlsm_stats_listener::OnCompactionCompleted(
    rocksdb::DB *, const rocksdb::CompactionJobInfo &info) {
  Rdb_bitlsm_registry::instance().estimator_notify(info.cf_name);
}

std::shared_ptr<bit_lsm::SABIFactory> Rdb_bitlsm_registry::get(
    const std::string &cf_name) const {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_map.find(cf_name);
  return it == m_map.end() ? nullptr : it->second.factory;
}

rocksdb::UserDefinedIndexBuilder *Rdb_bitlsm_udi_factory::NewBuilder() const {
  auto f = Rdb_bitlsm_registry::instance().get(m_cf_name);
  // null -> RocksDB does not install the UDI wrapper (zero per-row cost).
  return f ? f->NewBuilder() : nullptr;
}

namespace {
// Process-global schema-less SABIFactory for the read path. A default-
// constructed SABIFactory is reader-only: v5 SABI blobs carry their own attr
// roles in the directory, so it needs no schema binding to open an SST. Const
// read methods make it safe to share across CFs and threads. NewBuilder() is
// never called on it (the build path uses the registry-bound factory).
const bit_lsm::SABIFactory &bitlsm_reader_factory() {
  static const bit_lsm::SABIFactory f;
  return f;
}
}  // namespace

std::unique_ptr<rocksdb::UserDefinedIndexReader>
Rdb_bitlsm_udi_factory::NewReader(rocksdb::Slice &index_block) const {
  return bitlsm_reader_factory().NewReader(index_block);
}

rocksdb::Status Rdb_bitlsm_udi_factory::NewReader(
    const rocksdb::UserDefinedIndexOption &option, rocksdb::Slice &index_block,
    std::unique_ptr<rocksdb::UserDefinedIndexReader> &reader) const {
  // Validates the v5 footer/directory (rejects a missing/unsupported version,
  // i.e. old v4 SSTs, loudly) then builds a self-describing reader -- no
  // registry lookup, so DB open no longer races CF binding.
  return bitlsm_reader_factory().NewReader(option, index_block, reader);
}

}  // namespace myrocks
