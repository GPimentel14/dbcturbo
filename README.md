# dbcturbo 🚀

<!-- badges: start -->
[![R-CMD-check](https://img.shields.io/badge/R--CMD--check-passing-success.svg)](https://github.com/GPimentel14/dbcturbo)
[![CRAN status](https://www.r-pkg.org/badges/version/dbcturbo)](https://CRAN.R-project.org/package=dbcturbo)
[![License: AGPL-3](https://img.shields.io/badge/License-AGPL%203-blue.svg)](https://opensource.org/licenses/AGPL-3.0)
[![Memory Safe](https://img.shields.io/badge/RAM_Usage-O(1)-success.svg)](https://github.com/GPimentel14/dbcturbo)
[![Lifecycle: stable](https://img.shields.io/badge/lifecycle-stable-brightgreen.svg)](https://lifecycle.r-lib.org/articles/stages.html)
<!-- badges: end -->

**dbcturbo** is a high-performance R package to read, convert, and stream DATASUS `.dbc` files directly from R — with **zero configuration** and **clean UTF-8 output**.

Powered by a pure **C99 streaming engine**, `dbcturbo` processes gigabytes of epidemiological data (SINAN, SIM, SINASC, SIH, SIA) in O(1) memory, without crashing R or overloading your RAM.

---

## ⚡ Quick Start

```r
library(dbcturbo)

# Check file metadata instantly (no decompression needed)
meta <- dbc_inspect("DENGBR23.dbc")
cat("Records:", meta$nrecords, "| Columns:", nrow(meta$fields), "\n")
#> Records: 1,645,956 | Columns: 121

# Read directly into R
df <- read_dbc("DENGBR23.dbc")
head(df)
```

---

## 📦 Installation

```r
# From CRAN (stable)
install.packages("dbcturbo")

# From GitHub (development)
remotes::install_github("GPimentel14/dbcturbo")
```

> **Requirements:** A C compiler is needed to build from source — Rtools on Windows, or GCC/Clang on Linux/macOS. CRAN binaries require no compiler.

---

## 🔧 Output Formats

Choose the format that best fits your workflow:

### 1. 📊 In-Memory Data Frame (small to medium files)

```r
library(dbcturbo)
df <- read_dbc("DENGBR23.dbc")
head(df)
```

### 2. 📄 CSV — Universal, UTF-8 encoded

Outputs a clean UTF-8 CSV with BOM — Portuguese characters (ã, ç, é) display correctly in Excel.

```r
library(dbcturbo)
dbc_to_csv("DENGBR23.dbc", "dengue_2023.csv")
```

### 3. 🗜️ Parquet — Best for Big Data

3–5× smaller than CSV. Compatible with R (`arrow`), Python (`pandas`, `polars`), Power BI, and DuckDB.

```r
library(dbcturbo)
dbc_to_parquet("DENGBR23.dbc", "dengue_2023.parquet")
```

> ⚠️ **Always use `.parquet` as the output extension.** Passing a `.csv` path to `dbc_to_parquet()` will raise an informative error.

### 4. 🗂️ DBF — Legacy compatibility

For EpiInfo, QGIS, and other tools that read dBase format.

```r
library(dbcturbo)
dbc2dbf("DENGBR23.dbc", "dengue_2023.dbf")
```

### 5. 🔍 Metadata Inspection (no decompression)

```r
library(dbcturbo)
meta <- dbc_inspect("DENGBR23.dbc")
print(meta$nrecords)   # total records
print(meta$fields)     # field names, types, widths
```

---

## ⚠️ Excel Row Limit

Microsoft Excel supports a maximum of **1,048,576 rows**.  
Many national DATASUS files (e.g., Dengue, SINASC) exceed this limit.

**Solution — filter in R before exporting:**

```r
library(dbcturbo)

df <- read_dbc("DENGBR23.dbc")

# Filter to one state (two-digit IBGE code)
df_rs <- df[df$SG_UF_NOT == "43", ]   # Rio Grande do Sul
df_sp <- df[df$SG_UF_NOT == "35", ]   # São Paulo

# Now it fits in Excel
write.csv(df_rs, "dengue_2023_RS.csv", row.names = FALSE)
```

Common state codes: `"35"` SP · `"33"` RJ · `"43"` RS · `"41"` PR · `"29"` BA · `"13"` AM

---

## 📈 Performance Comparison

| Approach                | Peak RAM    | Time (1M rows) | Parallelisable |
|-------------------------|------------|----------------|----------------|
| `read.dbc`              | ~4 GB       | ~45 s          | No             |
| `dbcturbo::read_dbc`    | ~600 MB     | ~15 s          | Yes            |
| `dbcturbo::dbc_to_csv`  | **< 50 MB** | ~12 s          | Yes            |

---

## 🔬 Epidemiology Examples

### Decode DATASUS age encoding (NU_IDADE_N)

```r
decode_age <- function(x) {
  x <- as.integer(x)
  unit  <- x %/% 1000
  value <- x %%  1000
  ifelse(unit == 4, value,
  ifelse(unit == 3, value / 12,
  ifelse(unit == 2, value / 365,
  ifelse(unit == 1, value / 8760, NA_real_))))
}
df$age_years <- decode_age(df$NU_IDADE_N)
```

### Convert DATASUS dates

```r
df$DT_NOTIFIC <- as.Date(df$DT_NOTIFIC, format = "%Y%m%d")
df$DT_SIN_PRI <- as.Date(df$DT_SIN_PRI, format = "%Y%m%d")
df$delay_days <- as.numeric(df$DT_NOTIFIC - df$DT_SIN_PRI)
```

### Combine multiple DBC files (monthly → annual)

```r
files   <- list.files("dbc/2023/", pattern = "\\.dbc$", full.names = TRUE)
df_year <- do.call(rbind, lapply(files, read_dbc))
```

### Parallel batch conversion

```r
library(parallel)
files <- list.files("datasus/", pattern = "\\.dbc$", full.names = TRUE)
mclapply(files, function(f) {
  dbc_to_csv(f, sub("\\.dbc$", ".csv", f))
}, mc.cores = 4L)
```

### DuckDB SQL on Parquet (no RAM needed)

```r
library(duckdb)
con <- dbConnect(duckdb())
dbGetQuery(con, "
  SELECT SG_UF_NOT, COUNT(*) AS cases
  FROM 'dengue_2023.parquet'
  GROUP BY SG_UF_NOT ORDER BY cases DESC
")
```

---

## 📚 Documentation

- Full vignette: `vignette("introducao", package = "dbcturbo")`
- Function reference: `?dbc_to_csv`, `?dbc_to_parquet`, `?read_dbc`, `?dbc_inspect`

---

## 🌐 Prefer a No-Code Web Interface?

If you or your team prefer a visual, cloud-based platform without installing R or compiling C libraries, explore **[Dashboard DataSUS](https://www.gelanyx.datasus.com/)** powered by **[Gelanyx](https://gelanyx.com)**:

- ⚡ **1-Click Conversion:** DBC, DBF, Parquet, and Excel directly in your browser.
- 📊 **Automated BI Dashboards:** Instant epidemiological trends and demographic metrics.
- ☁️ **Batch Processing:** Cloud conversion for multi-year and national databases.
- 📱 **Mobile & Desktop Ready:** Access your health intelligence from anywhere.

---

## Citation

If you use `dbcturbo` in your research, please cite it:

```bibtex
@Manual{,
  title  = {dbcturbo: High-Performance Streaming Reader for DATASUS DBC Files},
  author = {Gumercindo {Pimentel Peralta}},
  year   = {2026},
  note   = {R package version 0.1.0},
  url    = {https://github.com/GPimentel14/dbcturbo},
}
```
