#' @useDynLib dbcturbo, .registration = TRUE
#' @keywords internal
"_PACKAGE"

#' Decompress a DATASUS DBC file to DBF format
#'
#' Calls the C streaming engine to decompress a \code{.dbc} file into a
#' standard \code{.dbf} file without loading the full payload into RAM.
#' The compressed payload is processed in 64 KB chunks using the PKWare
#' Implode algorithm (\code{blast.c}, Mark Adler).
#'
#' @param input_file  Character string. Path to the source \code{.dbc} file.
#'   Must exist and be readable.
#' @param output_file Character string. Path for the output \code{.dbf} file.
#'   Created or overwritten.
#'
#' @return \code{TRUE} invisibly on success. On failure, stops with a
#'   descriptive error message.
#'
#' @examples
#' \dontrun{
#' dbc2dbf("SINAN_LEPTO_RS_2024.dbc", "SINAN_LEPTO_RS_2024.dbf")
#' }
#'
#' @seealso \code{\link{dbc_to_csv}}, \code{\link{dbc_inspect}}
#' @export
dbc2dbf <- function(input_file, output_file) {
  .assert_scalar_string(input_file,  "input_file")
  .assert_scalar_string(output_file, "output_file")

  input_file  <- normalizePath(input_file,  mustWork = TRUE)
  output_file <- normalizePath(output_file, mustWork = FALSE)
  invisible(.Call("C_dbc2dbf", input_file, output_file))
}


#' Convert a DATASUS DBC file directly to CSV (streaming, low-RAM)
#'
#' Decompresses a \code{.dbc} file and writes a UTF-8 CSV to disk.
#' Records are processed in batches, so peak RAM usage is
#' \code{O(batch_size * record_width)} regardless of file size.
#'
#' The output CSV is prefixed with a UTF-8 BOM so that Microsoft Excel
#' opens it with correct encoding without additional configuration.
#'
#' @param input_file  Character string. Path to the source \code{.dbc} file.
#' @param output_file Character string. Path for the output \code{.csv} file.
#' @param batch_size  Integer \code{>= 1}. Number of records per write batch.
#'   Default \code{4096L}.
#' @param encoding    Character string or \code{NULL}. Source encoding of
#'   character fields (e.g. \code{"CP850"}, \code{"latin1"}).
#'   \code{NULL} (default) assumes ASCII / UTF-8.
#' @param progress    Function or \code{NULL}. Called after each batch with
#'   two numeric arguments: \code{done} and \code{total} (record counts).
#'   Ctrl+C is honoured between batches.
#'
#' @return \code{TRUE} invisibly on success. On failure, stops with a
#'   descriptive error message.
#'
#' @examples
#' \dontrun{
#' # Simple usage: just provide input and output paths
#' dbc_to_csv("SINASC_RS_2024.dbc", "SINASC_RS_2024.csv")
#'
#' # Advanced usage: with progress bar and custom batch size
#' dbc_to_csv(
#'   input_file  = "SINASC_RS_2024.dbc",
#'   output_file = "SINASC_RS_2024.csv",
#'   batch_size  = 10000L,
#'   progress    = function(done, total)
#'     cat(sprintf("\r  %.1f%%", 100 * done / total))
#' )
#' library(data.table)
#' dt <- fread("SINASC_RS_2024.csv")
#' }
#'
#' @seealso \code{\link{dbc2dbf}}, \code{\link{dbc_inspect}}
#' @export
dbc_to_csv <- function(input_file, output_file,
                       batch_size = 4096L,
                       encoding   = NULL,
                       progress   = NULL) {
  .assert_scalar_string(input_file,  "input_file")
  .assert_scalar_string(output_file, "output_file")

  batch_size <- suppressWarnings(as.integer(batch_size)[[1L]])
  if (is.na(batch_size) || batch_size < 1L)
    stop("'batch_size' must be a positive integer")

  if (!is.null(encoding)) {
    if (!is.character(encoding) || length(encoding) != 1L ||
        is.na(encoding) || !nzchar(encoding))
      stop("'encoding' must be a non-empty character string or NULL")
    encoding <- encoding[[1L]]
  }

  if (!is.null(progress) && !is.function(progress))
    stop("'progress' must be a function or NULL")

  if (is.null(progress) && interactive()) {
    pb <- utils::txtProgressBar(style = 3)
    progress <- function(done, total) {
      utils::setTxtProgressBar(pb, done / total)
      if (done >= total) close(pb)
    }
  }

  input_file  <- normalizePath(input_file,  mustWork = TRUE)
  output_file <- normalizePath(output_file, mustWork = FALSE)

  invisible(.Call("C_dbc_to_csv",
                  input_file, output_file, batch_size, encoding,
                  if (is.null(progress)) NULL else progress))
}


#' Inspect the metadata of a DATASUS DBC file without decompressing data
#'
#' Reads only the DBF header embedded in the \code{.dbc} file and returns
#' field metadata and the total record count. The compressed data payload
#' is never touched, making this function very fast even for large files.
#'
#' @param input_file Character string. Path to the \code{.dbc} file.
#'
#' @return A named list with:
#' \describe{
#'   \item{\code{fields}}{A \code{data.frame} with columns
#'     \code{name} (character), \code{type} (one-character string:
#'     \code{C}=text, \code{N}=numeric, \code{D}=date, \code{L}=logical),
#'     \code{width} (integer), \code{decimals} (integer).}
#'   \item{\code{nrecords}}{Integer. Total number of records.}
#' }
#'
#' @examples
#' \dontrun{
#' meta <- dbc_inspect("SIM_RS_2024.dbc")
#' cat(meta$nrecords, "records\n")
#' print(meta$fields)
#' }
#'
#' @seealso \code{\link{dbc_to_csv}}, \code{\link{dbc2dbf}}
#' @export
dbc_inspect <- function(input_file) {
  .assert_scalar_string(input_file, "input_file")
  input_file <- normalizePath(input_file, mustWork = TRUE)
  .Call("C_dbc_inspect", input_file)
}


#' Read a DATASUS DBC file into an R data frame
#'
#' High-level convenience wrapper that streams the \code{.dbc} file to a
#' temporary CSV via \code{\link{dbc_to_csv}} and then reads it with
#' \code{data.table::fread()} (if available) or \code{utils::read.csv()}.
#'
#' For files with more than one million records, use \code{\link{dbc_to_csv}}
#' directly and load the resulting CSV with \code{arrow::read_csv_arrow()} or
#' \code{data.table::fread()} for maximum performance.
#'
#' @param file       Character string. Path to the \code{.dbc} file.
#' @param batch_size Integer. Passed to \code{\link{dbc_to_csv}}.
#'   Default \code{4096L}.
#' @param encoding   Character string. Source encoding of character fields.
#'   Default \code{"CP850"} (standard DATASUS legacy encoding).
#' @param ...        Additional arguments forwarded to the CSV reader.
#'
#' @return A \code{data.frame} or \code{data.table} (if \pkg{data.table}
#'   is installed).
#'
#' @examples
#' \dontrun{
#' df <- read_dbc("SINAN_DENGUE_RS_2024.dbc")
#' head(df)
#' }
#'
#' @export
read_dbc <- function(file, batch_size = 4096L, encoding = "CP850", ...) {
  tmp <- tempfile(fileext = ".csv")
  on.exit(unlink(tmp), add = TRUE)

  dbc_to_csv(file, tmp, batch_size = batch_size, encoding = encoding)

  if (requireNamespace("data.table", quietly = TRUE)) {
    data.table::fread(tmp, encoding = "UTF-8", ...)
  } else {
    utils::read.csv(tmp, fileEncoding = "UTF-8", ...)
  }
}


#' Convert a DATASUS DBC file directly to Parquet format
#'
#' A high-level convenience wrapper that streams the \code{.dbc} file to a
#' temporary CSV using the highly optimized C engine, and then immediately
#' converts that temporary file into a heavily compressed \code{.parquet} file
#' using the \pkg{arrow} package.
#'
#' This is the recommended workflow for Big Data and epidemiological research,
#' as Parquet files are heavily compressed, columnar, and preserve types.
#'
#' @param input_file  Character string. Path to the source \code{.dbc} file.
#' @param output_file Character string. Path for the output \code{.parquet} file.
#' @param batch_size  Integer. Passed to \code{\link{dbc_to_csv}}.
#'   Default \code{8192L}.
#' @param encoding    Character string. Source encoding of character fields.
#'   Default \code{"CP850"}.
#' @param progress    Function or \code{NULL}. Optional callback for progress reporting.
#'
#' @return \code{TRUE} invisibly on success. Stops if the \pkg{arrow} package
#'   is not installed.
#'
#' @examples
#' \dontrun{
#' # You only need to provide the input and output paths.
#' # The technical settings (batch_size, encoding) are handled automatically.
#'
#' library(dbcturbo)
#' library(arrow)
#'
#' dbc_to_parquet(
#'   input_file = "/DENGBR24.dbc", 
#'   output_file = "/DENGBR24.parquet"
#' )
#'
#' datos <- read_parquet("/DENGBR24.parquet")
#' View(datos)
#' head(datos)
#' dim(datos) 
#' ncol(datos)
#' nrow(datos)
#' names(datos)
#' summary(datos)
#' }
#'
#' @export
dbc_to_parquet <- function(input_file, output_file, batch_size = 8192L, encoding = "CP850", progress = NULL) {
  if (!requireNamespace("arrow", quietly = TRUE)) {
    stop("The 'arrow' package is required to generate Parquet files.\n",
         "Please install it running: install.packages('arrow')")
  }

  .assert_scalar_string(input_file, "input_file")
  .assert_scalar_string(output_file, "output_file")
  input_file <- normalizePath(input_file, mustWork = TRUE)

  # 1. Create a fast temporary CSV
  tmp_csv <- tempfile(fileext = ".csv")
  on.exit(unlink(tmp_csv), add = TRUE)

  # 2. Extract data using our C streaming engine with progress callback
  dbc_to_csv(input_file, tmp_csv, batch_size = batch_size, encoding = encoding, progress = progress)

  # 3. Read the temporary CSV with arrow and save as Parquet
  # read_csv_arrow is extremely fast and handles type inference automatically
  df_arrow <- arrow::read_csv_arrow(tmp_csv)
  arrow::write_parquet(df_arrow, output_file)

  invisible(TRUE)
}


# ── Internal validator (not exported) ─────────────────────────────────────────
# Not exported (leading dot convention).
.assert_scalar_string <- function(x, name) {
  if (!is.character(x) || length(x) != 1L)
    stop(sprintf("'%s' must be a length-1 character string", name))
  if (is.na(x))
    stop(sprintf("'%s' must not be NA", name))
  if (!nzchar(x))
    stop(sprintf("'%s' must not be an empty string", name))
  invisible(NULL)
}
