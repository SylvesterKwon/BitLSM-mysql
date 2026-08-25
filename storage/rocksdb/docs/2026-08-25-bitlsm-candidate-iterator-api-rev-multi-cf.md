# [개정] Candidate Iterator API — 멀티 CF 도입에 따른 CF 주입 방식 변경 (2026-08-25)

> 대상 문서: `plans/2026-07-12-bitlsm-candidate-iterator-api.md`
> 원인: BitLSM 멀티 CF 지원 머지 (BitLSM PR #57, master `dfcc089bc`)

## 한 줄

하위호환 옵션 3개 중 **(A) CF 주입**의 시그니처가 바뀌었다: `BitLSM::NewIterator`는 더 이상 raw `rocksdb::ColumnFamilyHandle*`를 받지 않고, **BitLSM이 발급한 `bit_lsm::ColumnFamilyHandle*`**를 받는다. (B) ResultMode, (C) 스냅샷 주입은 무변경.

## 바뀐 것

```cpp
// 이전 (원 스펙)
db.NewIterator(query, /*cfh=*/raw_rocksdb_cfh, ResultMode::Candidate, snap);

// 이후 (BitLSM master)
bit_lsm::ColumnFamilyHandle* cf = nullptr;
db.CreateColumnFamily("my_table_cf", per_cf_options, &cf);   // 또는
db.GetColumnFamily("my_table_cf");                            // 또는 오픈 시 descriptor 목록
db.NewIterator(cf, query, ResultMode::Candidate, snap);       // CF 지정
db.NewIterator(query, ResultMode::Candidate, snap);           // 생략 = default CF
```

- CF는 이제 BitLSM이 정식으로 소유/관리한다: `BitLSM(path, ropts, topts, vector<ColumnFamilyDescriptor>)` 오픈 또는 런타임 `CreateColumnFamily(name, BitLSMOptions, &handle)`. CF마다 **독립 스키마**(`BitLSMOptions`)를 가지며, 스캔은 항상 **해당 CF의 스키마로 디코딩**된다 (원 스펙의 "DB 전역 스키마 가정" 제거됨).
- MyRocks의 테이블-당-CF 레이아웃은 이 API로 직접 표현 가능 — 임시방편이던 raw 핸들 주입의 원 사용처가 정식 경로로 대체된 것.
- estimator도 CF 단위 옵션이 됐다: 핸들 경유 `cf->Estimator()` / `db.EstimateSelectivity(cf, q)`. M5 estimation API의 `enable_estimator=true` 요구는 CF별로 적용.

## 정말 raw rocksdb 핸들이 필요한 경우

저수준 시임은 남아 있다: `BitLSMIterator(db, rocksdb_cfh, options, query, result_mode, snapshot)` 생성자를 직접 호출하면 원 스펙 (A)와 동일하게 임의 rocksdb CF + 명시 스키마로 이터레이터를 만들 수 있다. 단, 스키마(`options`)를 호출자가 정확히 공급할 책임이 생긴다.

## 원 스펙 문서에서 무효가 된 부분

- §(A) "접근할 CF를 `cfh`에서 유도 (null → default)" — `NewIterator`의 `cfh` 파라미터 자체가 제거됨. null-자리채움 관례(`/*cfh=*/nullptr`)도 소멸.
- `cf_handles_[0]` 언급 (bit_lsm.cpp:92 등 라인 참조) — 내부가 name→handle 레지스트리로 재작성되어 라인/구조 참조 전부 stale.
- "옵션 모두 생략 시 standalone 동작 100% 동일" 계약은 **유지됨** (default CF 편의 오버로드).
