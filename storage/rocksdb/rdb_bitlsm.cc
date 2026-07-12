/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include "./rdb_bitlsm.h"

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
  if (it != m_map.end() && it->second.roles != roles) {
    // D5 violation: CF already hosts a different bitlsm schema.
    return false;
  }
  m_map[cf_name] = Entry{std::move(roles), std::move(factory)};
  return true;
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
