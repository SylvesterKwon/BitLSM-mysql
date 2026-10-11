# BitLSM-mysql

[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

BitLSM-mysql is a MySQL fork that adds `BITLSM_INDEX`, a bitmap index for multi-attribute conjunctive queries on MyRocks tables. The bitmaps are built by BitLSM and stored inside the table's own SST files.

Built on top of [facebook/mysql-5.6](https://github.com/facebook/mysql-5.6) (MyRocks) and [BitLSM](https://github.com/SylvesterKwon/BitLSM).

## Installation

Requires GCC 11+ (C++20) and CMake 3.14+.

### 1. Initialize Git Submodules

```bash
git submodule update --init --recursive rocksdb
```

### 2. Install System Dependencies

```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake bison pkg-config libssl-dev libncurses-dev libzstd-dev
```

### 3. Build

```bash
cmake -B build -DDOWNLOAD_BOOST=1 -DWITH_BOOST=$HOME/boost
cmake --build build -j$(nproc)
```

## Quick Start

### Example

```sql
CREATE TABLE orders (
  id BIGINT PRIMARY KEY,
  shipdate DATE, discount INT, quantity INT,
  city VARBINARY(32), zip VARBINARY(5), store_id INT,
  INDEX bi (shipdate, discount, quantity, city,
            zip ORDERED, store_id UNORDERED) BITLSM_INDEX
) ENGINE=ROCKSDB;

-- Query it like any other table
SELECT * FROM orders WHERE discount >= 5 AND quantity < 24 AND city = 'Seoul';
```

Each indexed column is `ORDERED` (serves range and point predicates) or `UNORDERED` (point predicates only, for columns that are only ever compared with `=` or `IN`). Numbers and dates default to `ORDERED`, strings to `UNORDERED`; `zip ORDERED` and `store_id UNORDERED` override that.

Unlike a composite index, which serves only the leading prefixes of its column order, `bi` serves a conjunction over any subset of its columns.

### Core Concepts

- **Placement.** A `BITLSM_INDEX` writes no secondary-key entries. Its bitmaps are built from the primary key's column family at every flush and compaction and stored in each SST, so rows still in the memtable are not pruned. One BitLSM table per column family: give each its own `cfname` on the primary key.

- **Pruning.** `col <op> const`, `IN` and `BETWEEN` predicates (also under `OR`) prune through the bitmaps; anything else in the `WHERE` clause is evaluated on the fetched rows.

### API

`INDEX name (col [ORDERED|UNORDERED], ...) BITLSM_INDEX` — column types: integers, `FLOAT`, `DOUBLE`, `DATE`, and `VARCHAR`/`VARBINARY` with a binary collation.

### Limitations

The cardinality estimator combines predicates under independence and undershoots correlated conjunctions.

## Development

### Testing

The MyRocks test suite has a `bitlsm_*` group:

```bash
cd build/mysql-test
./mysql-test-run.pl --suite=rocksdb --do-test=bitlsm
```

### Code Style

C++ follows the repository's `.clang-format`.

### Updating BitLSM

The `rocksdb` submodule is the BitLSM repository; its RocksDB patch is applied at configure time. Move the submodule to the wanted commit and rebuild.

```bash
git -C rocksdb fetch origin && git -C rocksdb checkout <commit>
git -C rocksdb submodule update --init --recursive
```
