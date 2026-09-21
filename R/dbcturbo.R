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
#' @param cols Character vector or \code{NULL}. Names of fields to write.
#'   \code{NULL} (default) writes every field. Selection is applied by the C
#'   writer, so unselected fields are not written to the CSV.
#' @param encoding    Character string or \code{NULL}. Source encoding of
#'   character fields (e.g. \code{"CP850"}, \code{"latin1"}).
#'   \code{NULL} (default) assumes ASCII / UTF-8.
#' @param verbose     Logical. If \code{TRUE}, prints file metrics,
#'   a progress bar, and elapsed time upon completion. Default \code{FALSE}.
#' @param progress    Function or \code{NULL}. Custom callback called after each batch with
#'   two numeric arguments: \code{done} and \code{total} (record counts).
#'   Ctrl+C is honoured between batches.
#'
#' @param ...         Optional internal arguments.
#'
#' @return \code{TRUE} invisibly on success. On failure, stops with a
#'   descriptive error message.
#'
#' @examples
#' \dontrun{
#' # Simple usage: just provide input and output paths
#' dbc_to_csv("SINASC_RS_2024.dbc", "SINASC_RS_2024.csv")
#'
#' # Advanced usage: with custom batch size and verbose mode
#' dbc_to_csv(
#'   input_file  = "SINASC_RS_2024.dbc",
#'   output_file = "SINASC_RS_2024.csv",
#'   batch_size  = 10000L,
#'   verbose     = TRUE
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
                       cols       = NULL,
                       verbose    = FALSE,
                       progress   = NULL,
                       ...) {
  .assert_scalar_string(input_file,  "input_file")
  .assert_scalar_string(output_file, "output_file")

  batch_size <- .assert_positive_integer(batch_size, "batch_size")
  .assert_scalar_logical(verbose, "verbose")

  if (!is.null(encoding)) {
    if (!is.character(encoding) || length(encoding) != 1L ||
        is.na(encoding) || !nzchar(encoding))
      stop("'encoding' must be a non-empty character string or NULL")
    encoding <- encoding[[1L]]
  }

  if (!is.null(cols) && (!is.character(cols) || length(cols) == 0L || anyNA(cols)))
    stop("'cols' must be a non-empty character vector of field names, or NULL")

  if (!is.null(progress) && !is.function(progress))
    stop("'progress' must be a function or NULL")

  input_file  <- normalizePath(input_file,  mustWork = TRUE)
  output_file <- normalizePath(output_file, mustWork = FALSE)

  selected_fields <- NULL
  if (!is.null(cols)) {
    fields <- dbc_inspect(input_file)$fields$name
    unknown <- setdiff(cols, fields)
    if (length(unknown))
      stop("Unknown column(s): ", paste(unknown, collapse = ", "),
           "\nAvailable fields: ", paste(fields, collapse = ", "))
    selected_fields <- as.integer(match(cols, fields) - 1L)
  }

  extra_args <- list(...)
  disp_name  <- if (!is.null(extra_args$output_display)) extra_args$output_display else basename(output_file)

  if (isTRUE(verbose) && is.null(progress)) {
    finfo <- file.info(input_file)
    meta  <- tryCatch(dbc_inspect(input_file), error = function(e) NULL)
    nrec  <- if (!is.null(meta)) meta$nrecords else 0L
    ncols <- if (!is.null(meta)) nrow(meta$fields) else 0L

    cat(sprintf("\n\u2500\u2500 dbcturbo streaming engine \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"))
    cat(sprintf(" \u2022 Input:    %s (%s)\n", basename(input_file), .format_bytes(finfo$size)))
    if (nrec > 0L) {
      cat(sprintf(" \u2022 Records:  %s rows | %d columns\n", format(nrec, big.mark = ","), ncols))
    }
    cat(sprintf(" \u2022 Output:   %s\n", disp_name))

    t0 <- proc.time()
    pb <- utils::txtProgressBar(style = 3)
    progress <- function(done, total) {
      utils::setTxtProgressBar(pb, done / total)
      if (done >= total) {
        close(pb)
        elapsed <- (proc.time() - t0)[["elapsed"]]
        speed <- if (elapsed > 0.01 && total > 0) sprintf(" (%.0f rows/s)", total / elapsed) else ""
        cat(sprintf(" \u2714 Conversion completed in %.2fs%s\n\n", elapsed, speed))
      }
    }
  }

  invisible(.Call("C_dbc_to_csv",
                  input_file, output_file, batch_size, encoding, selected_fields,
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
#' @param verbose    Logical. Passed to \code{\link{dbc_to_csv}}.
#'   Default \code{FALSE}.
#' @param cols       Character vector or \code{NULL}. Names of the columns to
#'   keep in the returned object. \code{NULL} (default) returns all columns.
#'   Column names are validated against \code{\link{dbc_inspect}} metadata and
#'   selection is applied before the temporary CSV is written.
#' @param coerce_types Logical. If \code{TRUE} (default), columns are
#'   automatically converted to their native R types based on the DBF field
#'   metadata:
#'   \itemize{
#'     \item \code{D} (date) → \code{Date} (DBF stores dates as
#'       \code{YYYYMMDD} strings; blank or \code{"00000000"} become \code{NA}).
#'     \item \code{N} with decimals > 0 → \code{numeric}.
#'     \item \code{N} with decimals == 0 → \code{integer} when values fit in
#'     R integers, otherwise \code{numeric}.
#'     \item \code{L} (logical) → \code{logical}
#'       (\code{"T"}/\code{"Y"}/\code{"S"}/\code{"1"} → \code{TRUE};
#'       \code{"F"}/\code{"N"}/\code{"0"} → \code{FALSE}; others → \code{NA}).
#'     \item \code{C} (character) → unchanged.
#'   }
#' @param engine Character string selecting the CSV reader: \code{"auto"}
#'   (default) uses \pkg{data.table} when installed and otherwise R base;
#'   \code{"data.table"} requires \pkg{data.table}; \code{"base"} always
#'   uses \code{utils::read.csv()}.
#' @param ...        Additional arguments forwarded to the CSV reader.
#'
#' @return A \code{data.frame} or \code{data.table} (if \pkg{data.table}
#'   is installed).
#'
#' @examples
#' \dontrun{
#' # All columns, types coerced automatically
#' df <- read_dbc("DENGBR23.dbc")
#' class(df$DT_NOTIFIC)   # "Date"
#' class(df$NU_IDADE_N)   # "integer"
#'
#' # Only a subset of columns
#' df <- read_dbc("DENGBR23.dbc",
#'                cols = c("DT_NOTIFIC", "SG_UF_NOT", "CLASSI_FIN"))
#' ncol(df)  # 3
#' }
#'
#' @export
read_dbc <- function(file,
                     batch_size    = 4096L,
                     encoding      = "CP850",
                     verbose       = FALSE,
                     cols          = NULL,
                     coerce_types  = TRUE,
                     engine        = c("auto", "data.table", "base"),
                     ...) {

  .assert_scalar_logical(verbose, "verbose")
  .assert_scalar_logical(coerce_types, "coerce_types")
  engine <- match.arg(engine)

  # ── Validate cols ────────────────────────────────────────────────────────────
  if (!is.null(cols)) {
    if (!is.character(cols) || length(cols) == 0L)
      stop("'cols' must be a non-empty character vector of field names, or NULL")
  }

  # ── Get field metadata (needed for cols validation and/or type coercion) ─────
  meta <- NULL
  if (coerce_types || !is.null(cols)) {
    meta <- tryCatch(dbc_inspect(file), error = function(e) NULL)
  }

  # ── Validate column names against actual fields ───────────────────────────
  if (!is.null(cols) && !is.null(meta)) {
    bad <- setdiff(cols, meta$fields$name)
    if (length(bad) > 0L)
      stop("Unknown column(s): ", paste(bad, collapse = ", "),
           "\nAvailable fields: ", paste(meta$fields$name, collapse = ", "))
  }

  # ── Stream DBC → temp CSV ────────────────────────────────────────────────────
  tmp <- tempfile(fileext = ".csv")
  on.exit(unlink(tmp), add = TRUE)
  dbc_to_csv(file, tmp,
             batch_size = batch_size,
             encoding   = encoding,
             cols       = cols,
             verbose    = verbose)

  # ── Read CSV ─────────────────────────────────────────────────────────────────
  # Start from character data so DBF metadata controls type conversion below.
  extra_args <- list(...)
  if (!("colClasses" %in% names(extra_args))) {
    extra_args$colClasses <- "character"
  }

  use_data_table <- switch(
    engine,
    "auto" = requireNamespace("data.table", quietly = TRUE),
    "data.table" = {
      if (!requireNamespace("data.table", quietly = TRUE)) {
        stop("'engine = \"data.table\"' requires the 'data.table' package. ",
             "Install it with install.packages(\"data.table\").")
      }
      TRUE
    },
    "base" = FALSE
  )

  if (use_data_table) {
    read_args <- c(list(input = tmp, encoding = "UTF-8", select = cols), extra_args)
    df <- do.call(data.table::fread, read_args)
  } else {
    read_args <- c(list(file = tmp, fileEncoding = "UTF-8", stringsAsFactors = FALSE), extra_args)
    df <- do.call(utils::read.csv, read_args)
    if (!is.null(cols))
      df <- df[, intersect(cols, names(df)), drop = FALSE]
  }

  # ── Type coercion ─────────────────────────────────────────────────────────
  if (coerce_types && !is.null(meta)) {
    active_fields <- if (!is.null(cols)) {
      meta$fields[meta$fields$name %in% cols, ]
    } else {
      meta$fields
    }

    for (i in seq_len(nrow(active_fields))) {
      fname <- active_fields$name[i]
      ftype <- active_fields$type[i]
      fdec  <- active_fields$decimals[i]

      if (fname %in% names(df) && ftype != "C") {
        df[[fname]] <- .coerce_column(df[[fname]], ftype, fdec)
      }
    }
  }

  df
}


#' Convert all DBC files in a directory to CSV files
#'
#' Finds DBC files in a directory and converts each one with
#' \code{\link{dbc_to_csv}}. When \code{recursive = TRUE}, the relative
#' directory layout beneath \code{input_dir} is preserved in
#' \code{output_dir}. Existing output files are never replaced unless
#' \code{overwrite = TRUE}.
#'
#' @param input_dir Character string. Directory containing DBC files.
#' @param output_dir Character string. Destination directory for CSV files.
#'   Created when it does not exist.
#' @param pattern Regular expression used to select input files. Defaults to
#'   \code{"\\\\.dbc$"}, case-insensitively.
#' @param recursive Logical. Search subdirectories? Default \code{FALSE}.
#' @param batch_size Integer. Number of records per CSV write batch. Passed to
#'   \code{\link{dbc_to_csv}}.
#' @param encoding Character string or \code{NULL}. Passed to
#'   \code{\link{dbc_to_csv}}.
#' @param overwrite Logical. Replace existing CSV files? Default \code{FALSE}.
#' @param verbose Logical. Print per-file conversion progress? Default
#'   \code{FALSE}.
#'
#' @return A data frame with one row per converted file and columns
#'   \code{input_file} and \code{output_file}. For an empty input directory,
#'   returns a zero-row data frame with those columns.
#'
#' @examples
#' \dontrun{
#' result <- dbc_batch_to_csv("data/dbc", "data/csv", recursive = TRUE)
#' }
#' @export
dbc_batch_to_csv <- function(input_dir, output_dir,
                             pattern = "\\.dbc$", recursive = FALSE,
                             batch_size = 4096L, encoding = NULL,
                             overwrite = FALSE, verbose = FALSE) {
  .assert_scalar_string(input_dir, "input_dir")
  .assert_scalar_string(output_dir, "output_dir")
  .assert_scalar_string(pattern, "pattern")
  .assert_scalar_logical(recursive, "recursive")
  .assert_scalar_logical(overwrite, "overwrite")
  .assert_scalar_logical(verbose, "verbose")
  batch_size <- .assert_positive_integer(batch_size, "batch_size")

  if (!dir.exists(input_dir))
    stop("'input_dir' does not exist or is not a directory")
  if (!dir.exists(output_dir) && !dir.create(output_dir, recursive = TRUE))
    stop("Could not create 'output_dir': ", output_dir)

  input_dir <- normalizePath(input_dir, mustWork = TRUE)
  output_dir <- normalizePath(output_dir, mustWork = TRUE)
  input_files <- list.files(input_dir, pattern = pattern, full.names = TRUE,
                            recursive = recursive, ignore.case = TRUE)

  if (!length(input_files)) {
    return(data.frame(input_file = character(), output_file = character(),
                      stringsAsFactors = FALSE))
  }

  relative_paths <- substring(input_files, nchar(input_dir) + 2L)
  output_files <- file.path(
    output_dir,
    sub("\\.[dD][bB][cC]$", ".csv", relative_paths)
  )

  existing <- file.exists(output_files)
  if (any(existing) && !overwrite) {
    stop("Output file(s) already exist: ",
         paste(output_files[existing], collapse = ", "),
         ". Set 'overwrite = TRUE' to replace them.")
  }

  output_dirs <- unique(dirname(output_files))
  for (directory in output_dirs) {
    if (!dir.exists(directory) && !dir.create(directory, recursive = TRUE))
      stop("Could not create output directory: ", directory)
  }

  for (i in seq_along(input_files)) {
    dbc_to_csv(input_files[[i]], output_files[[i]], batch_size = batch_size,
               encoding = encoding, verbose = verbose)
  }

  data.frame(input_file = input_files, output_file = output_files,
             stringsAsFactors = FALSE)
}


#' Convert a DATASUS DBC file to Parquet format
#'
#' A high-level convenience wrapper that first writes the \code{.dbc} file to
#' a temporary CSV using the C engine, then converts that CSV into a
#' \code{.parquet} file using the \pkg{arrow} package. It therefore requires
#' temporary disk space for the CSV and is not an end-to-end streaming writer.
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
#' @param verbose     Logical. If \code{TRUE}, prints file metrics,
#'   a progress bar, and elapsed time upon completion. Default \code{FALSE}.
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
dbc_to_parquet <- function(input_file, output_file, batch_size = 8192L, encoding = "CP850", verbose = FALSE, progress = NULL) {
  .assert_scalar_string(input_file, "input_file")
  .assert_scalar_string(output_file, "output_file")
  batch_size <- .assert_positive_integer(batch_size, "batch_size")
  .assert_scalar_logical(verbose, "verbose")

  if (!is.null(encoding)) .assert_scalar_string(encoding, "encoding")
  if (!is.null(progress) && !is.function(progress))
    stop("'progress' must be a function or NULL")

  if (grepl("\\.csv$", output_file, ignore.case = TRUE)) {
    stop(
      "The output path ends in '.csv', but dbc_to_parquet() always writes binary Parquet format.\n",
      "  - To generate a CSV file use: dbc_to_csv(\"", basename(input_file), "\", \"", basename(output_file), "\")\n",
      "  - To generate a Parquet file change the extension: \"",
      sub("\\.csv$", ".parquet", output_file, ignore.case = TRUE), "\""
    )
  }

  if (!requireNamespace("arrow", quietly = TRUE)) {
    stop("The 'arrow' package is required to generate Parquet files.\n",
         "Please install it running: install.packages('arrow')")
  }

  input_file <- normalizePath(input_file, mustWork = TRUE)

  # 1. Create a fast temporary CSV
  tmp_csv <- tempfile(fileext = ".csv")
  on.exit(unlink(tmp_csv), add = TRUE)

  # 2. Extract data using our C streaming engine with progress callback
  dbc_to_csv(input_file, tmp_csv, batch_size = batch_size, encoding = encoding, verbose = verbose, progress = progress, output_display = basename(output_file))

  # 3. Read the temporary CSV with arrow and save as Parquet
  # read_csv_arrow is extremely fast and handles type inference automatically
  df_arrow <- arrow::read_csv_arrow(tmp_csv)
  arrow::write_parquet(df_arrow, output_file)

  invisible(TRUE)
}


# -- Internal helpers (not exported) -------------------------------------------
.format_bytes <- function(bytes) {
  if (is.na(bytes) || bytes <= 0) return("0 B")
  if (bytes < 1024) return(paste0(bytes, " B"))
  if (bytes < 1024^2) return(sprintf("%.1f KB", bytes / 1024))
  if (bytes < 1024^3) return(sprintf("%.1f MB", bytes / 1024^2))
  return(sprintf("%.2f GB", bytes / 1024^3))
}


# -- Type coercion helper (not exported) ---------------------------------------
# Converts a character vector read from CSV back to its native R type
# based on the DBF field type code and decimal count.
#
# DBF type codes:
#   C  character  → kept as-is (no conversion)
#   D  date       → Date  (YYYYMMDD format; blank/"00000000" → NA)
#   N  numeric    → numeric (decimals > 0) or integer (decimals == 0)
#   L  logical    → logical ("T"/"Y"/"S"/"1" → TRUE; "F"/"N"/"0" → FALSE)
#
.coerce_column <- function(x, type, decimals) {
  switch(type,
    "D" = {
      x <- trimws(x)
      # Treat blank, all-zero, or all-space values as NA
      x[!nzchar(x) | x == "00000000"] <- NA_character_
      as.Date(x, format = "%Y%m%d")
    },
    "N" = {
      x <- trimws(x)
      x[!nzchar(x)] <- NA_character_
      value <- suppressWarnings(as.numeric(x))
      if (decimals > 0L || any(!is.na(value) &
          (value < -.Machine$integer.max - 1 | value > .Machine$integer.max))) {
        value
      } else {
        as.integer(value)
      }
    },
    "L" = {
      x <- trimws(toupper(as.character(x)))
      result <- rep(NA, length(x))
      result[x %in% c("T", "Y", "S", "1")] <- TRUE
      result[x %in% c("F", "N", "0"     )] <- FALSE
      as.logical(result)
    },
    x   # "C" and unknown types: return unchanged
  )
}

# -- Internal validator (not exported) -----------------------------------------
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

.assert_scalar_logical <- function(x, name) {
  if (!is.logical(x) || length(x) != 1L || is.na(x))
    stop(sprintf("'%s' must be TRUE or FALSE", name))
  invisible(NULL)
}

.assert_positive_integer <- function(x, name) {
  if (!is.numeric(x) || length(x) != 1L || is.na(x) || !is.finite(x) ||
      x < 1 || x != floor(x) || x > .Machine$integer.max)
    stop(sprintf("'%s' must be a positive integer", name))
  as.integer(x)
}
