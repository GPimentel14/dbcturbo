## Release 0.3.0

In this version I have:

* added `read_dbc_chunks()` for memory-bounded chunked streaming and
  `dbc_validate()` for header and full-stream structural integrity checks;
* moved type coercion, UTF-8 transcoding, and `n_max` early-stop decoding into
  the native C engine (`read_dbc()` now uses the native engine for all file
  sizes by default);
* converted `dbc_to_parquet()` to stream typed chunks directly to Parquet via
  `arrow::ParquetFileWriter` (removing the intermediate temporary CSV) and
  added `cols` selection; and
* changed the default source `encoding` to `"latin1"` (matching modern DATASUS
  files) while retaining `"CP850"` support for legacy files.

## Test environments
* Local: Ubuntu 26.04.1 LTS, R 4.6.2
* win-builder (R-release): Windows Server 2022, R 4.6.2 (2026-10-29 ucrt)
* win-builder (R-devel): Windows Server 2022, R Under development (unstable) (2026-11-17 r90926 ucrt)

## R CMD check results

* Local Ubuntu: 0 ERRORs | 0 WARNINGs | 1 NOTE (system compiler flag `-mno-omit-leaf-frame-pointer`)
* win-builder (R-release): 0 ERRORs | 0 WARNINGs | 0 NOTEs (Status: OK)
* win-builder (R-devel): 0 ERRORs | 0 WARNINGs | 0 NOTEs (Status: OK)

### Local compiler note

* **Non-portable compilation flag**: the local R installation adds
  `-mno-omit-leaf-frame-pointer` through its own compiler configuration. The
  package does not set this flag; `src/Makevars` intentionally contains no
  custom compilation flags.
## Additional Notes
* This package provides a memory-efficient C-based streaming decompression engine for DATASUS `.dbc` files.
* The core PKWare Implode decompression algorithm is derived from `blast.c` by Mark Adler (properly acknowledged in `DESCRIPTION` as `ctb` and within `src/blast.c`).
