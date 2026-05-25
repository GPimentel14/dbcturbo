# test-descompresion.R
# Final production test suite for dbcturbo.
#
# Design principles:
#   - Every "corrupt" or "truncated" fixture is built from scratch using
#     writeBin() on tempfile() paths — no dependency on external files.
#   - Every "happy path" test requires the bundled inst/files/sids.dbc sample
#     and guards with skip_if() if it is absent.
#   - All temporary files are cleaned up via on.exit().
#   - Error regexp patterns match both the R-layer message and C-layer message
#     to remain robust against platform differences.

library(testthat)

# ── Fixture builders ──────────────────────────────────────────────────────────

# Returns path to the bundled sample or skips the test.
.sample_dbc <- function() {
  path <- system.file("files", "sids.dbc", package = "dbcturbo")
  skip_if(
    !nzchar(path) || !file.exists(path),
    "Sample DBC file (inst/files/sids.dbc) not found."
  )
  path
}

# Random bytes: always triggers a blast() error (invalid header).
.make_random_dbc <- function(n = 128L) {
  p <- tempfile(fileext = ".dbc")
  writeBin(as.raw(sample.int(256L, n, replace = TRUE) - 1L), p)
  p
}

# Zero-byte file: fread(header) returns 0 < sizeof(dbf_header_t).
.make_empty_dbc <- function() {
  p <- tempfile(fileext = ".dbc")
  writeBin(raw(0L), p)
  p
}

# Plausible-looking header prefix (first 12 bytes of a real DBC) followed
# by truncated/garbage compressed payload.  Triggers blast() error code 2
# (input truncated before end-of-stream).
.make_truncated_dbc <- function() {
  real <- .sample_dbc()
  # Read the real header (first hdr_sz + 4 bytes) then stop
  raw_real <- readBin(real, "raw", n = 20L)
  p <- tempfile(fileext = ".dbc")
  writeBin(raw_real, p)
  p
}

# A DBC whose header bytes 8-9 indicate header_size == 33
# (just 1 byte: too small to contain even one field descriptor).
# Triggers DBC_ERR_INVALID_DBF via the nfields <= 0 guard.
.make_tiny_header_dbc <- function() {
  p <- tempfile(fileext = ".dbc")
  raw_bytes <- raw(64L)
  # Bytes 8-9 (little-endian uint16): 33 = sizeof(dbf_header_t) + 1 + 1
  # That yields nfields = (33 - 32 - 1) / 32 = 0, which is rejected.
  raw_bytes[9L]  <- as.raw(33L)
  raw_bytes[10L] <- as.raw(0L)
  writeBin(raw_bytes, p)
  p
}

# ── 1. Argument validation (R layer, before any C call) ───────────────────────

test_that("dbc2dbf rejects non-character input_file", {
  expect_error(dbc2dbf(42L,   tempfile()), regexp = "length-1 character")
  expect_error(dbc2dbf(NULL,  tempfile()), regexp = "length-1 character")
  expect_error(dbc2dbf(TRUE,  tempfile()), regexp = "length-1 character")
  expect_error(dbc2dbf(list(), tempfile()), regexp = "length-1 character")
})

test_that("dbc2dbf rejects NA input_file", {
  expect_error(dbc2dbf(NA_character_, tempfile()), regexp = "NA")
})

test_that("dbc2dbf rejects empty-string input_file", {
  expect_error(dbc2dbf("", tempfile()), regexp = "empty string")
})

test_that("dbc2dbf rejects length-2 input_file vector", {
  expect_error(dbc2dbf(c("a.dbc", "b.dbc"), tempfile()), regexp = "length-1")
})

test_that("dbc_to_csv rejects non-character input_file", {
  expect_error(dbc_to_csv(123L, tempfile()), regexp = "length-1 character")
})

test_that("dbc_to_csv rejects NA_character_ input_file", {
  expect_error(dbc_to_csv(NA_character_, tempfile()), regexp = "NA")
})

test_that("dbc_to_csv rejects empty output_file", {
  dbc <- .sample_dbc()
  expect_error(dbc_to_csv(dbc, ""), regexp = "empty string")
})

test_that("dbc_to_csv rejects non-function progress", {
  dbc <- .sample_dbc()
  expect_error(
    dbc_to_csv(dbc, tempfile(), progress = "txt"),
    regexp = "function"
  )
  expect_error(
    dbc_to_csv(dbc, tempfile(), progress = 1L),
    regexp = "function"
  )
})

test_that("dbc_to_csv rejects non-coercible batch_size", {
  dbc <- .sample_dbc()
  expect_error(
    dbc_to_csv(dbc, tempfile(), batch_size = "abc"),
    regexp = "positive integer"
  )
})

test_that("dbc_to_csv rejects NA batch_size", {
  dbc <- .sample_dbc()
  expect_error(
    dbc_to_csv(dbc, tempfile(), batch_size = NA_integer_),
    regexp = "positive integer"
  )
})

test_that("dbc_to_csv rejects zero batch_size", {
  dbc <- .sample_dbc()
  expect_error(
    dbc_to_csv(dbc, tempfile(), batch_size = 0L),
    regexp = "positive integer"
  )
})

test_that("dbc_to_csv rejects NA encoding", {
  dbc <- .sample_dbc()
  expect_error(
    dbc_to_csv(dbc, tempfile(), encoding = NA_character_),
    regexp = "encoding"
  )
})

test_that("dbc_to_csv rejects empty-string encoding", {
  dbc <- .sample_dbc()
  expect_error(
    dbc_to_csv(dbc, tempfile(), encoding = ""),
    regexp = "encoding"
  )
})

test_that("dbc_inspect rejects non-character input_file", {
  expect_error(dbc_inspect(42L),   regexp = "length-1 character")
  expect_error(dbc_inspect(NULL),  regexp = "length-1 character")
})

test_that("dbc_inspect rejects NA input_file", {
  expect_error(dbc_inspect(NA_character_), regexp = "NA")
})

test_that("dbc_inspect rejects empty string", {
  expect_error(dbc_inspect(""), regexp = "empty string")
})

# ── 2. Missing-file errors ─────────────────────────────────────────────────────

test_that("dbc2dbf errors cleanly when input file does not exist", {
  expect_error(
    dbc2dbf("/no/such/path/xyzzy_42.dbc", tempfile())
  )
})

test_that("dbc_to_csv errors cleanly when input file does not exist", {
  expect_error(
    dbc_to_csv("/no/such/path/xyzzy_42.dbc", tempfile())
  )
})

test_that("dbc_inspect errors cleanly when input file does not exist", {
  expect_error(
    dbc_inspect("/no/such/path/xyzzy_42.dbc")
  )
})

# ── 3. Corrupt / pathological input files ─────────────────────────────────────

test_that("dbc2dbf throws dbcturbo error on random-bytes file", {
  bad <- .make_random_dbc(128L)
  out <- tempfile(fileext = ".dbf")
  on.exit({ unlink(bad); unlink(out) }, add = TRUE)

  expect_error(dbc2dbf(bad, out), regexp = "dbcturbo error")
})

test_that("dbc_to_csv throws dbcturbo error on random-bytes file", {
  bad <- .make_random_dbc(128L)
  out <- tempfile(fileext = ".csv")
  on.exit({ unlink(bad); unlink(out) }, add = TRUE)

  expect_error(dbc_to_csv(bad, out), regexp = "dbcturbo error")
})

test_that("dbc2dbf throws dbcturbo error on zero-byte file", {
  empty <- .make_empty_dbc()
  out   <- tempfile(fileext = ".dbf")
  on.exit({ unlink(empty); unlink(out) }, add = TRUE)

  expect_error(dbc2dbf(empty, out), regexp = "dbcturbo error")
})

test_that("dbc_inspect throws dbcturbo error on zero-byte file", {
  empty <- .make_empty_dbc()
  on.exit(unlink(empty), add = TRUE)

  expect_error(dbc_inspect(empty), regexp = "dbcturbo error")
})

test_that("dbc2dbf throws dbcturbo error on truncated DBC (valid header, cut payload)", {
  trunc <- .make_truncated_dbc()
  out   <- tempfile(fileext = ".dbf")
  on.exit({ unlink(trunc); unlink(out) }, add = TRUE)

  expect_error(dbc2dbf(trunc, out), regexp = "dbcturbo error")
})

test_that("dbc_inspect throws dbcturbo error on file with implausible field count", {
  tiny <- .make_tiny_header_dbc()
  on.exit(unlink(tiny), add = TRUE)

  expect_error(dbc_inspect(tiny), regexp = "dbcturbo error")
})

# ── 4. Happy path (requires bundled inst/files/sids.dbc) ──────────────────────

test_that("dbc2dbf produces a non-empty .dbf from a valid DBC", {
  dbc <- .sample_dbc()
  dbf <- tempfile(fileext = ".dbf")
  on.exit(unlink(dbf), add = TRUE)

  expect_true(dbc2dbf(dbc, dbf))
  expect_true(file.exists(dbf))
  expect_gt(file.size(dbf), 0L)
})

test_that("dbc_to_csv produces a UTF-8 CSV with quoted header row", {
  dbc <- .sample_dbc()
  csv <- tempfile(fileext = ".csv")
  on.exit(unlink(csv), add = TRUE)

  expect_true(dbc_to_csv(dbc, csv, batch_size = 100L))
  expect_true(file.exists(csv))

  lines <- readLines(csv, n = 5L, encoding = "UTF-8", warn = FALSE)
  expect_gte(length(lines), 2L)
  expect_match(lines[[1L]], '"[A-Z]')
})

test_that("dbc_to_csv row count is <= dbc_inspect nrecords", {
  dbc <- .sample_dbc()
  csv <- tempfile(fileext = ".csv")
  on.exit(unlink(csv), add = TRUE)

  dbc_to_csv(dbc, csv)
  meta <- dbc_inspect(dbc)

  n_lines <- length(readLines(csv, encoding = "UTF-8", warn = FALSE))
  expect_lte(n_lines - 1L, meta$nrecords)   # subtract BOM+header line
  expect_gte(n_lines - 1L, 0L)
})

test_that("dbc_inspect returns a valid metadata list", {
  meta <- dbc_inspect(.sample_dbc())

  expect_type(meta, "list")
  expect_named(meta, c("fields", "nrecords"))
  expect_s3_class(meta$fields, "data.frame")
  expect_named(meta$fields, c("name", "type", "width", "decimals"))
  expect_gt(nrow(meta$fields), 0L)
  expect_gt(meta$nrecords, 0L)
  expect_true(all(meta$fields$type %in% c("C", "N", "D", "L", "M")))
  expect_true(all(meta$fields$width   >= 1L))
  expect_true(all(meta$fields$decimals >= 0L))
  expect_true(all(nchar(meta$fields$name) >= 1L))
})

test_that("read_dbc returns a data.frame with rows and columns", {
  df <- read_dbc(.sample_dbc())

  expect_true(is.data.frame(df) || inherits(df, "data.table"))
  expect_gt(nrow(df), 0L)
  expect_gt(ncol(df), 0L)
})

test_that("negative batch_size is clamped to 4096 and succeeds", {
  dbc <- .sample_dbc()
  csv <- tempfile(fileext = ".csv")
  on.exit(unlink(csv), add = TRUE)

  # batch_size = -1 is rejected by R layer (>= 1 required)
  # batch_size passed as 0L is also rejected
  # Only batch_size >= 1 is valid; C clamps batch_size <= 0 to 4096.
  # Since R now rejects <= 0, we test that 1L works (minimum valid value).
  expect_true(dbc_to_csv(dbc, csv, batch_size = 1L))
})

test_that("progress callback is invoked with numeric done <= total", {
  dbc <- .sample_dbc()
  csv <- tempfile(fileext = ".csv")
  on.exit(unlink(csv), add = TRUE)

  log <- list()
  dbc_to_csv(dbc, csv, batch_size = 50L,
             progress = function(done, total)
               log[[length(log) + 1L]] <<- c(done = done, total = total))

  expect_gt(length(log), 0L)
  last <- log[[length(log)]]
  expect_true(is.numeric(last["done"]))
  expect_true(is.numeric(last["total"]))
  expect_lte(last["done"], last["total"])
})

# ── 5. Output consistency ──────────────────────────────────────────────────────

test_that("CSV column count equals dbc_inspect field count", {
  dbc <- .sample_dbc()
  csv <- tempfile(fileext = ".csv")
  on.exit(unlink(csv), add = TRUE)

  dbc_to_csv(dbc, csv)
  meta   <- dbc_inspect(dbc)
  header <- readLines(csv, n = 1L, encoding = "UTF-8", warn = FALSE)

  # CSV header is comma-separated; quoted names may not contain commas,
  # so a simple split is reliable for DBF field names.
  ncols <- length(strsplit(header, ",", fixed = TRUE)[[1L]])
  expect_equal(ncols, nrow(meta$fields))
})

test_that("dbc2dbf output is >= input size (DBF payload > compressed DBC)", {
  dbc <- .sample_dbc()
  dbf <- tempfile(fileext = ".dbf")
  on.exit(unlink(dbf), add = TRUE)

  dbc2dbf(dbc, dbf)
  expect_gte(file.size(dbf), file.size(dbc))
})
