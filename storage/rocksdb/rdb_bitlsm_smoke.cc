/* M2 smoke: prove the BitLSM bitmap API links and runs inside rocksdb_se.
   TEMPORARY -- replaced by Rdb_bitlsm_handler in M3. */
#include <memory>
#include <string>
#include <vector>

#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "sabi.h"

namespace myrocks {

bool rdb_bitlsm_smoke_check() {
  bit_lsm::BitLSMOptions opts;
  opts.attr_num = 2;
  // a0: kRange 8-byte float, a1: kEquality variable-length opaque bytes.
  opts.attr_specs = {
      bit_lsm::AttrSpec(bit_lsm::IndexType::kRange,
                        bit_lsm::PhysicalType::kFloat, 8),
      bit_lsm::AttrSpec(bit_lsm::IndexType::kEquality,
                        bit_lsm::PhysicalType::kVarBinary, 0)};
  opts.read_seqno = 0;
  opts.rho = 0.1;

  auto factory = std::make_shared<bit_lsm::SABIFactory>(opts);

  bit_lsm::BitLSMQuery query(std::vector<bit_lsm::OrClause>{
      bit_lsm::OrClause{{0, bit_lsm::CompareOp::GREATER_EQUAL, 10.5}},
      bit_lsm::OrClause{{1, bit_lsm::CompareOp::EQUAL, std::string("apple")}}});

  return factory != nullptr && query.clause_groups.size() == 2 &&
         query.Validate(opts).ok();
}

}  // namespace myrocks
