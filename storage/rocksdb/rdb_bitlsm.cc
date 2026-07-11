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

std::unique_ptr<rocksdb::UserDefinedIndexReader>
Rdb_bitlsm_udi_factory::NewReader(rocksdb::Slice &index_block) const {
  auto f = Rdb_bitlsm_registry::instance().get(m_cf_name);
  return f ? f->NewReader(index_block) : nullptr;
}

}  // namespace myrocks
