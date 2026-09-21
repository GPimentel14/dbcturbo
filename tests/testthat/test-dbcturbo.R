test_that("dbc_inspect returns a valid metadata list", {
  sample_dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
  skip_if(!nzchar(sample_dbc), "Sample DBC file not found in inst/files/")

  meta <- dbc_inspect(sample_dbc)

  expect_type(meta, "list")
  expect_named(meta, c("fields", "nrecords"))
  expect_s3_class(meta$fields, "data.frame")
  expect_true(nrow(meta$fields) > 0)
  expect_true(meta$nrecords > 0)

  # Field data.frame columns
  expect_named(meta$fields, c("name", "type", "width", "decimals"))

  # Types must be one of the valid DBF type characters
  valid_types <- c("C", "N", "D", "L", "M")
  expect_true(all(meta$fields$type %in% valid_types))
})

test_that("dbc2dbf produces a non-empty .dbf file", {
  sample_dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
  skip_if(!nzchar(sample_dbc), "Sample DBC file not found")

  out_dbf <- tempfile(fileext = ".dbf")
  on.exit(unlink(out_dbf), add = TRUE)

  result <- dbc2dbf(sample_dbc, out_dbf)
  expect_true(result)
  expect_true(file.exists(out_dbf))
  expect_gt(file.size(out_dbf), 0)
})

test_that("dbc_to_csv produces a readable CSV", {
  sample_dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
  skip_if(!nzchar(sample_dbc), "Sample DBC file not found")

  out_csv <- tempfile(fileext = ".csv")
  on.exit(unlink(out_csv), add = TRUE)

  result <- dbc_to_csv(sample_dbc, out_csv, batch_size = 100L)
  expect_true(result)
  expect_true(file.exists(out_csv))

  lines <- readLines(out_csv, n = 5, encoding = "UTF-8")
  expect_gte(length(lines), 2)   # at least header + 1 data row
})

test_that("read_dbc returns a data.frame with rows and columns", {
  sample_dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
  skip_if(!nzchar(sample_dbc), "Sample DBC file not found")

  df <- read_dbc(sample_dbc)
  expect_true(is.data.frame(df) || inherits(df, "data.table"))
  expect_gt(nrow(df), 0)
  expect_gt(ncol(df), 0)
})

test_that("dbc_inspect errors gracefully on missing file", {
  expect_error(dbc_inspect("/no/such/file.dbc"))
})

test_that("dbc_to_parquet validates input arguments", {
  expect_error(dbc_to_parquet(123L, tempfile()), regexp = "length-1 character")
  expect_error(dbc_to_parquet("file.dbc", 123L), regexp = "length-1 character")
  expect_error(dbc_to_parquet("", tempfile()), regexp = "empty string")
})

test_that("dbc_to_parquet rejects .csv output extension", {
  expect_error(
    dbc_to_parquet("sample.dbc", "output.csv"),
    regexp = "The output path ends in '\\.csv'"
  )
})
