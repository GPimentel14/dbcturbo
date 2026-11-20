# dbcturbo 0.3.0

## New features

* `read_dbc_chunks()` streams a DBC file in memory-bounded data-frame chunks
  passed to a user `callback`, with optional column selection (`cols`),
  character encoding, and native type coercion.

* `dbc_validate()` checks the structural integrity of a DBC file (embedded DBF
  header, field descriptors, and file boundaries) and, with `deep = TRUE`,
  decompresses the full payload to verify record counts, record markers, stream
  termination, and absence of unexpected trailing data.

## Performance and internal improvements

* `read_dbc()` gains an `n_max` argument to stop decompression early after
  reading the requested number of active records.

* `read_dbc()` now performs type coercion and UTF-8 character transcoding
  directly in C in the native engine, removing the extra R-side `iconv()` and
  coercion passes. `native_threshold` now defaults to `Inf` so `engine = "auto"`
  uses the native engine for all file sizes unless extra CSV-reader arguments
  are supplied via `...`.

* `dbc_to_parquet()` now streams typed chunks directly from `read_dbc_chunks()`
  into `arrow::ParquetFileWriter` using an explicit DBF-derived Arrow schema,
  avoiding the intermediate temporary CSV file. It also gains a `cols`
  parameter for column selection, and the default `batch_size` is now `100000L`.

* Default source `encoding` is now `"latin1"` (matching modern DATASUS files);
  pass `encoding = "CP850"` for older DOS-era DATASUS files.

* `dbc_to_csv()` now uses a 256 KB in-memory output buffer and bulk copies for
  plain ASCII cells, reducing per-byte I/O overhead.

* `dbc_inspect()` and `dbc_read_records()` perform stricter header alignment,
  truncation, record marker, and trailing-data checks.

# dbcturbo 0.2.0

## New features

* `read_dbc(engine = "native")` decodes DBC records directly into R columns
  without DBF or CSV temporary files. In `engine = "auto"`, this native path
  is selected for files up to 50 MiB by default; set `native_threshold` to
  control the cutoff.

* `dbc_to_csv()` now writes CSV rows directly from the decompression stream,
  removing the full-sized temporary DBF previously required during conversion.

* `dbc_batch_to_csv()` gains `cols` for field selection and `workers` for
  file-level parallel conversion on macOS and Linux.

# dbcturbo 0.1.0

## New features

* `dbc_batch_to_csv()` converts every DBC file in a directory, optionally
  recursively, while preserving the input subdirectory layout.

* `dbc_to_csv()` and `read_dbc()` gain a `cols` parameter to select fields in
  the C writer, avoiding creation of unselected columns in the temporary CSV.

* `read_dbc()` gains `engine = "auto" | "data.table" | "base"` so users can
  choose the CSV reader explicitly for large files.

* `read_dbc()` gains a `coerce_types = TRUE` parameter that automatically
  converts columns to their native R types based on the DBF field metadata:
  - `D` (date) fields → `Date` (DBF format `YYYYMMDD`)
  - `N` fields with decimals > 0 → `numeric`
  - `N` fields with decimals == 0 → `integer` when values fit in R integers,
    otherwise `numeric`
  - `L` (logical) fields → `logical`
  - `C` (character) fields → `character` (unchanged)
  Blank or zero-filled values become `NA` of the appropriate type.
