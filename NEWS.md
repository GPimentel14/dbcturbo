# dbcturbo 0.1.0

* Initial release of `dbcturbo`.
* Implemented pure C99 streaming decompression engine based on PKWare Implode algorithm (`blast.c`).
* Added `dbc2dbf()` for direct file decompression to DBF format without loading payload to RAM.
* Added `dbc_to_csv()` with streaming chunk writing, progress callbacks, and UTF-8 BOM encoding for seamless Excel compatibility.
* Added `dbc_to_parquet()` for direct conversion to Apache Parquet columnar format via the `arrow` package.
* Added `dbc_inspect()` for instant DBF header metadata inspection without decompressing data records.
* Added `read_dbc()` as a high-level wrapper returning `data.frame` or `data.table`.
