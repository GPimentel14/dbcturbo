# dbcturbo 🚀

<!-- badges: start -->
[![R-CMD-check](https://img.shields.io/badge/R--CMD--check-passing-success.svg)](https://github.com/GPimentel14/dbcturbo)
[![CRAN status](https://www.r-pkg.org/badges/version/dbcturbo)](https://CRAN.R-project.org/package=dbcturbo)
[![License: AGPL-3](https://img.shields.io/badge/License-AGPL%203-blue.svg)](https://opensource.org/licenses/AGPL-3.0)
[![Memory Safe](https://img.shields.io/badge/RAM_Usage-O(1)-success.svg)](https://github.com/GPimentel14/dbcturbo)
<!-- badges: end -->

**dbcturbo** is a high-performance R package to read, convert, and stream DATASUS `.dbc` files.

Powered by a pure C99 engine, `dbcturbo` uses a streaming architecture (O(1) memory complexity) to process gigabytes of epidemiological data without overloading system RAM.

## Installation

```r
# Install from source
install.packages("dbcturbo", repos = NULL, type = "source")
```
*(C/C++ compilers are required: Rtools on Windows, or GCC/Clang on Linux/macOS).*

## Usage: Output Formats

`dbcturbo` offers multiple output functions. Choose the format that best fits your analytical pipeline.

### 1. Parquet (Recommended for Big Data)
Streams the `.dbc` file into a heavily compressed, columnar `.parquet` file. Ideal for large datasets spanning multiple years.

```r
library(dbcturbo)
dbc_to_parquet("DENGBR23.dbc", "dengue_2023.parquet")
```
*(Note: Requires the `arrow` package. Notice you only need to provide the input and output paths. Technical settings like `batch_size` and `encoding` are configured automatically by default for maximum performance!)*

### 2. CSV (Universal)
Exports data to a standard UTF-8 CSV file. Useful for cross-language compatibility.

```r
library(dbcturbo)
dbc_to_csv("DENGBR23.dbc", "dengue_2023.csv")
```

### 3. In-Memory Data.Frame
Loads the file directly into R's RAM as a `data.frame` or `data.table`. Recommended only for smaller files to prevent memory crashes.

```r
library(dbcturbo)
df <- read_dbc("DENGBR23.dbc")
head(df)
```

### 4. Fast Metadata Inspection
Reads the internal DBF header without decompressing the payload. Returns field structures and total record counts instantly.

```r
library(dbcturbo)
meta <- dbc_inspect("DENGBR23.dbc")
print(meta$nrecords)
print(meta$fields)
```

### 5. DBF (Legacy Support)
Decompresses the data into its original `.dbf` format for compatibility with legacy epidemiological software (e.g., EpiInfo, older GIS tools).

```r
library(dbcturbo)
dbc2dbf("DENGBR23.dbc", "dengue_2023.dbf")
```

## Citation

If you use `dbcturbo` in your research, please cite it:

```bibtex
@Manual{,
  title = {dbcturbo: High-Performance Streaming Reader for DATASUS DBC Files},
  author = {Gumercindo {Pimentel Peralta}},
  year = {2026},
  note = {R package version 0.1.0},
  url = {https://github.com/GPimentel14/dbcturbo},
}
```
