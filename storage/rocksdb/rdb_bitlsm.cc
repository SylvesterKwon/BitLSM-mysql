// Copyright (c) 2026 Dohyun Kwon

#include "./rdb_bitlsm.h"

#include "./rdb_bitlsm_descriptor.h"

/* rocksdb-internal headers, needed only for the estimator_attach casts. */
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"

namespace myrocks {

Rdb_bitlsm_registry &Rdb_bitlsm_registry::instance() {
  static Rdb_bitlsm_registry inst;
  return inst;
}

Rdb_bitlsm_schema_key rdb_bitlsm_schema_key_of(
    const bit_lsm::SABISchema &schema, const Rdb_bitlsm_attr_plan &plan) {
  Rdb_bitlsm_schema_key key;
  key.reserve(schema.index_types.size());
  for (const auto index_type : schema.index_types) {
    // Placeholder enc/len; the walk below fills in every attribute.
    key.push_back({index_type, Rdb_bitlsm_attr_plan::Enc::BINARY_STR, 0});
  }
  for (const auto &e : plan.walk) {
    if (e.is_target && e.attr_index < key.size()) {
      key[e.attr_index].enc = e.enc;
      key[e.attr_index].len = e.len;
    }
  }
  return key;
}

bool Rdb_bitlsm_registry::bind(const std::string &cf_name,
                               Rdb_bitlsm_schema_key schema_key,
                               std::shared_ptr<bit_lsm::SABIFactory> factory) {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_map.find(cf_name);
  if (it != m_map.end()) {
    if (it->second.schema_key != schema_key) {
      return false;  // CF already hosts a different bitlsm schema
    }
    // Same-schema rebind (table reopen): keep the running estimator.
    it->second.factory = std::move(factory);
    return true;
  }
  m_map.emplace(cf_name,
                Entry{std::move(schema_key), std::move(factory), nullptr});
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
  // ~CardinalityEstimator joins the worker. It runs outside the registry lock
  // so a concurrent flush-completion notify cannot deadlock against the join.
}

void Rdb_bitlsm_registry::estimator_shutdown() {
  std::vector<std::unique_ptr<bit_lsm::CardinalityEstimator>> dead;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &kv : m_map) {
      if (kv.second.estimator) dead.push_back(std::move(kv.second.estimator));
    }
  }
  // Workers join here, outside the lock (see estimator_destroy).
}

size_t Rdb_bitlsm_registry::estimator_refresh_all() {
  // Collect pointers under the lock, refresh outside it: TEST_Refresh waits on
  // the worker, and a concurrent flush completion takes the lock in
  // estimator_notify.
  std::vector<bit_lsm::CardinalityEstimator *> targets;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &kv : m_map) {
      if (kv.second.estimator) targets.push_back(kv.second.estimator.get());
    }
  }
  for (auto *est : targets) est->TEST_Refresh();
  return targets.size();
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

void Rdb_bitlsm_registry::mark_expected(const std::string &cf_name) {
  std::lock_guard<std::mutex> lk(m_mutex);
  m_map[cf_name].expected = true;
}

bool Rdb_bitlsm_registry::expects_sabi(const std::string &cf_name) const {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_map.find(cf_name);
  return it != m_map.end() && it->second.expected;
}

bool rdb_bitlsm_bind_persisted(const std::string &cf_name,
                               const std::string &blob) {
  Rdb_bitlsm_descriptor desc;
  if (!rdb_bitlsm_deserialize_descriptor(blob, &desc)) return false;

  auto plan =
      std::make_shared<const Rdb_bitlsm_attr_plan>(std::move(desc.plan));
  auto factory = std::make_shared<bit_lsm::SABIFactory>(
      desc.schema,
      [plan] { return std::make_unique<Rdb_bitlsm_extractor>(plan); });

  auto &registry = Rdb_bitlsm_registry::instance();
  if (!registry.bind(cf_name,
                     rdb_bitlsm_schema_key_of(desc.schema, *plan),
                     std::move(factory))) {
    return false;
  }
  registry.mark_expected(cf_name);
  return true;
}

rocksdb::UserDefinedIndexBuilder *Rdb_bitlsm_udi_factory::NewBuilder() const {
  auto f = Rdb_bitlsm_registry::instance().get(m_cf_name);
  return f ? f->NewBuilder() : nullptr;  // null: no UDI wrapper for this CF
}

rocksdb::Status Rdb_bitlsm_udi_factory::NewBuilder(
    const rocksdb::UserDefinedIndexOption & /*option*/,
    std::unique_ptr<rocksdb::UserDefinedIndexBuilder> &builder) const {
  auto &registry = Rdb_bitlsm_registry::instance();
  auto f = registry.get(m_cf_name);
  if (f) {
    builder.reset(f->NewBuilder());
    return rocksdb::Status::OK();
  }
  if (registry.expects_sabi(m_cf_name)) {
    // A persisted descriptor exists but no factory is bound. Writing the SST
    // would silently drop its bitmaps, so fail the flush/compaction instead.
    return rocksdb::Status::Corruption(
        "bitlsm: column family '" + m_cf_name +
        "' has a persisted SABI descriptor but no bound builder");
  }
  builder.reset();  // ordinary CF: no UDI wrapper
  return rocksdb::Status::OK();
}

namespace {
// Schema-less, reader-only SABIFactory shared by every CF and thread. SABI
// blobs are self-describing, so no schema binding is needed to open an SST.
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
  // Validates the SABI footer and builds a self-describing reader, with no
  // registry lookup, so DB open does not depend on CF-binding order.
  return bitlsm_reader_factory().NewReader(option, index_block, reader);
}

}  // namespace myrocks
