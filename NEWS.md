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
