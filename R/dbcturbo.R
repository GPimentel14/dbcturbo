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
#' dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
#' dbf <- tempfile(fileext = ".dbf")
#' dbc2dbf(dbc, dbf)
#' file.exists(dbf)
#' unlink(dbf)
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
#' dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
#' csv <- tempfile(fileext = ".csv")
#' dbc_to_csv(dbc, csv)
#' head(utils::read.csv(csv, fileEncoding = "UTF-8"))
#' unlink(csv)
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

    message(sprintf("\n\u2500\u2500 dbcturbo streaming engine \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"))
    message(sprintf(" \u2022 Input:    %s (%s)\n", basename(input_file), .format_bytes(finfo$size)))
    if (nrec > 0L) {
      message(sprintf(" \u2022 Records:  %s rows | %d columns\n", format(nrec, big.mark = ","), ncols))
    }
    message(sprintf(" \u2022 Output:   %s\n", disp_name))

    t0 <- proc.time()
    pb <- utils::txtProgressBar(style = 3)
    progress <- function(done, total) {
      utils::setTxtProgressBar(pb, done / total)
      if (done >= total) {
        close(pb)
        elapsed <- (proc.time() - t0)[["elapsed"]]
        speed <- if (elapsed > 0.01 && total > 0) sprintf(" (%.0f rows/s)", total / elapsed) else ""
        message(sprintf(" \u2714 Conversion completed in %.2fs%s\n\n", elapsed, speed))
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
#' dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
#' meta <- dbc_inspect(dbc)
#' meta$nrecords
#' head(meta$fields)
#'
#' @seealso \code{\link{dbc_to_csv}}, \code{\link{dbc2dbf}}
#' @export
dbc_inspect <- function(input_file) {
  .assert_scalar_string(input_file, "input_file")
  input_file <- normalizePath(input_file, mustWork = TRUE)
  .Call("C_dbc_inspect", input_file)
}

#' Validate the structural integrity of a DATASUS DBC file
#'
#' Checks the embedded DBF header, field descriptors, record width, and file
#' boundaries. With \code{deep = TRUE}, also decompresses the complete payload
#' and verifies the declared record count, record markers, stream termination,
#' and absence of unexpected trailing data. No data frame or temporary DBF file
#' is created.
#'
#' This is a structural and decompression check. It does not compare the
#' contents with an external reference dataset.
#'
#' @param file Character string. Path to the \code{.dbc} file.
#' @param deep Logical. If \code{TRUE} (default), decompress and validate every
#'   record. If \code{FALSE}, validate only the embedded DBF header.
#'
#' @return \code{TRUE} invisibly when the file passes validation. Invalid,
#'   truncated, or corrupt files raise an error.
#'
#' @examples
#' dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
#' dbc_validate(dbc)
#'
#' @seealso \code{\link{dbc_inspect}}, \code{\link{read_dbc}}
#' @export
dbc_validate <- function(file, deep = TRUE) {
  .assert_scalar_string(file, "file")
  .assert_scalar_logical(deep, "deep")
  file <- normalizePath(file, mustWork = TRUE)
  if (deep) invisible(.Call("C_dbc_validate", file))
  else {
    dbc_inspect(file)
    invisible(TRUE)
  }
}

#' Read a DATASUS DBC file into an R data frame
#'
#' High-level convenience wrapper. By default the native engine decodes the
#' whole file directly into R columns without temporary files, which is the
#' fastest option at every file size. Set \code{engine} to \code{"data.table"}
#' or \code{"base"}, or pass CSV-reader arguments through \code{...}, to use the
#' temporary-CSV route instead.
#'
#' The native engine transcodes character fields to UTF-8 in C as it decodes,
#' so its result is identical to the CSV engines without an extra R-side
#' \code{iconv()} pass.
#'
#' @param file       Character string. Path to the \code{.dbc} file.
#' @param batch_size Integer. Passed to \code{\link{dbc_to_csv}}.
#'   Default \code{4096L}.
#' @param encoding   Character string. Source encoding of character fields.
#'   Default \code{"latin1"} (standard for modern DATASUS files; use
#'   \code{"CP850"} for older DOS-era files).
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
#' @param engine Character string selecting the reader. \code{"auto"}
#'   (default) uses the native engine whenever no extra CSV-reader arguments are
#'   supplied; otherwise it falls back to \pkg{data.table} when installed and R
#'   base otherwise. \code{"native"} always uses direct native decoding;
#'   \code{"data.table"} requires \pkg{data.table}; \code{"base"} always uses
#'   \code{utils::read.csv()}.
#' @param native_threshold Non-negative number of bytes. In \code{"auto"} mode,
#'   files at or below this size use the native engine. Defaults to \code{Inf}
#'   (native for every size). Set to \code{0} to always use the CSV route.
#' @param n_max Non-negative number. Maximum number of active (non-deleted)
#'   records to read. Defaults to \code{Inf} (read all records). In the native
#'   engine, decompression stops immediately once \code{n_max} rows are read.
#' @param ...        Additional arguments forwarded to the CSV reader.
#'
#' @return A \code{data.frame} or \code{data.table} (if \pkg{data.table}
#'   is installed).
#'
#' @examples
#' dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
#' df <- read_dbc(dbc)
#' head(df)
#'
#' # Read a subset of fields.
#' fields <- dbc_inspect(dbc)$fields$name[1:3]
#' selected <- read_dbc(dbc, cols = fields)
#' names(selected)
#'
#' @export
read_dbc <- function(file,
                     batch_size    = 4096L,
                     encoding      = "latin1",
                     verbose       = FALSE,
                     cols          = NULL,
                     coerce_types  = TRUE,
                     engine        = c("auto", "native", "data.table", "base"),
                     native_threshold = Inf,
                     n_max         = Inf,
                     ...) {

  .assert_scalar_logical(verbose, "verbose")
  .assert_scalar_logical(coerce_types, "coerce_types")
  if (!is.null(encoding)) .assert_scalar_string(encoding, "encoding")
  if (!is.numeric(native_threshold) || length(native_threshold) != 1L ||
      is.na(native_threshold) || native_threshold < 0)
    stop("'native_threshold' must be a non-negative number of bytes (use Inf for no limit)")
  if (!is.numeric(n_max) || length(n_max) != 1L || is.na(n_max) || n_max < 0)
    stop("'n_max' must be a non-negative number (use Inf for all records)")
  engine <- match.arg(engine)
  .assert_scalar_string(file, "file")
  file <- normalizePath(file, mustWork = TRUE)
  extra_args <- list(...)

  # ── Validate cols ────────────────────────────────────────────────────────────
  if (!is.null(cols)) {
    if (!is.character(cols) || length(cols) == 0L)
      stop("'cols' must be a non-empty character vector of field names, or NULL")
  }

  # ── Get field metadata (needed for cols validation and/or type coercion) ─────
  use_native <- engine == "native" ||
    (engine == "auto" && native_threshold > 0 && !length(extra_args) &&
       file.info(file)$size <= native_threshold)

  if (use_native && length(extra_args))
    stop("additional CSV-reader arguments in '...' are not supported by the native engine")

  meta <- NULL
  if ((!use_native && coerce_types) || !is.null(cols)) {
    meta <- tryCatch(dbc_inspect(file), error = function(e) NULL)
  }

  # ── Validate column names against actual fields ───────────────────────────
  if (!is.null(cols) && !is.null(meta)) {
    bad <- setdiff(cols, meta$fields$name)
    if (length(bad) > 0L)
      stop("Unknown column(s): ", paste(bad, collapse = ", "),
           "\nAvailable fields: ", paste(meta$fields$name, collapse = ", "))
  }

  if (use_native) {
    selected_fields <- if (is.null(cols)) NULL else as.integer(match(cols, meta$fields$name) - 1L)
    # The C engine transcodes character fields to UTF-8 while decoding, so the
    # result already matches the CSV route without a second R-side iconv pass.
    df <- .Call("C_dbc_read_native", file, selected_fields, coerce_types, encoding, as.double(n_max))
  } else {
    # ── Stream DBC → temp CSV ──────────────────────────────────────────────────
    tmp <- tempfile(fileext = ".csv")
    on.exit(unlink(tmp), add = TRUE)
    dbc_to_csv(file, tmp,
               batch_size = batch_size,
               encoding   = encoding,
               cols       = cols,
               verbose    = verbose)

    # ── Read CSV ───────────────────────────────────────────────────────────────
    # Start from character data so DBF metadata controls type conversion below.
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
      "base" = FALSE,
      "native" = FALSE
    )

    if (use_data_table) {
      if (is.finite(n_max) && !("nrows" %in% names(extra_args)))
        extra_args$nrows <- as.integer(n_max)
      read_args <- c(list(input = tmp, encoding = "UTF-8", select = cols), extra_args)
      df <- do.call(data.table::fread, read_args)
    } else {
      if (is.finite(n_max) && !("nrows" %in% names(extra_args)))
        extra_args$nrows <- as.integer(n_max)
      read_args <- c(list(file = tmp, fileEncoding = "UTF-8", stringsAsFactors = FALSE), extra_args)
      df <- do.call(utils::read.csv, read_args)
      if (!is.null(cols))
        df <- df[, intersect(cols, names(df)), drop = FALSE]
    }
  }

  # ── Type coercion ─────────────────────────────────────────────────────────
  if (coerce_types && !is.null(meta) && !use_native) {
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


#' Read a DATASUS DBC file in memory-bounded chunks
#'
#' Streams a DBC file once and calls \code{callback} with each data-frame
#' chunk. A chunk is no longer retained by dbcturbo after the callback returns,
#' so memory use is approximately proportional to \code{chunk_size} unless the
#' callback stores the chunks.
#'
#' @param file Character string. Path to the DBC file.
#' @param callback Function called with one data frame per chunk.
#' @param chunk_size Positive integer. Maximum number of non-deleted records per
#'   chunk. Defaults to \code{100000L}.
#' @param encoding Source encoding for character fields. Defaults to
#'   \code{"latin1"}.
#' @param cols Character vector or \code{NULL}. Fields to include.
#' @param coerce_types Logical. Convert DBF numeric, logical, and date fields to
#'   native R types. Defaults to \code{TRUE}.
#'
#' @return Total number of delivered rows, invisibly.
#'
#' @examples
#' dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
#' rows <- 0L
#' read_dbc_chunks(dbc, chunk_size = 25L, callback = function(x) {
#'   rows <<- rows + nrow(x)
#' })
#' rows
#'
#' @seealso \code{\link{read_dbc}}, \code{\link{dbc_validate}}
#' @export
read_dbc_chunks <- function(file, callback, chunk_size = 100000L,
                            encoding = "latin1", cols = NULL,
                            coerce_types = TRUE) {
  .assert_scalar_string(file, "file")
  .assert_scalar_logical(coerce_types, "coerce_types")
  chunk_size <- .assert_positive_integer(chunk_size, "chunk_size")
  if (!is.function(callback))
    stop("'callback' must be a function")
  if (!is.null(encoding))
    .assert_scalar_string(encoding, "encoding")
  if (!is.null(cols) &&
      (!is.character(cols) || !length(cols) || anyNA(cols)))
    stop("'cols' must be a non-empty character vector of field names, or NULL")

  file <- normalizePath(file, mustWork = TRUE)
  selected_fields <- NULL
  if (!is.null(cols)) {
    fields <- dbc_inspect(file)$fields$name
    unknown <- setdiff(cols, fields)
    if (length(unknown))
      stop("Unknown column(s): ", paste(unknown, collapse = ", "))
    selected_fields <- as.integer(match(cols, fields) - 1L)
  }

  total <- .Call("C_dbc_read_chunks", file, selected_fields, coerce_types,
                 encoding, chunk_size, callback)
  invisible(total)
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
#' @param cols Character vector or \code{NULL}. Optional fields to write for
#'   every input file. Passed to \code{\link{dbc_to_csv}}.
#' @param overwrite Logical. Replace existing CSV files? Default \code{FALSE}.
#' @param verbose Logical. Print per-file conversion progress? Default
#'   \code{FALSE}.
#' @param workers Positive integer. Number of files to convert concurrently on
#'   macOS and Linux. Defaults to \code{1L}. Windows uses sequential execution
#'   because \code{parallel::mclapply()} is not available there.
#'
#' @return A data frame with one row per converted file and columns
#'   \code{input_file} and \code{output_file}. For an empty input directory,
#'   returns a zero-row data frame with those columns.
#'
#' @examples
#' input_dir <- tempfile("dbc-input-")
#' output_dir <- tempfile("dbc-output-")
#' dir.create(input_dir)
#' file.copy(system.file("extdata", "sids.dbc", package = "dbcturbo"),
#'           file.path(input_dir, "sids.dbc"))
#' result <- dbc_batch_to_csv(input_dir, output_dir)
#' result
#' unlink(c(input_dir, output_dir), recursive = TRUE)
#' @export
dbc_batch_to_csv <- function(input_dir, output_dir,
                             pattern = "\\.dbc$", recursive = FALSE,
                             batch_size = 4096L, encoding = NULL,
                             cols = NULL, overwrite = FALSE, verbose = FALSE,
                             workers = 1L) {
  .assert_scalar_string(input_dir, "input_dir")
  .assert_scalar_string(output_dir, "output_dir")
  .assert_scalar_string(pattern, "pattern")
  .assert_scalar_logical(recursive, "recursive")
  .assert_scalar_logical(overwrite, "overwrite")
  .assert_scalar_logical(verbose, "verbose")
  batch_size <- .assert_positive_integer(batch_size, "batch_size")
  workers <- .assert_positive_integer(workers, "workers")
  if (!is.null(cols) && (!is.character(cols) || !length(cols) || anyNA(cols)))
    stop("'cols' must be a non-empty character vector of field names, or NULL")

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

  convert_one <- function(i) {
    dbc_to_csv(input_files[[i]], output_files[[i]], batch_size = batch_size,
               encoding = encoding, cols = cols, verbose = verbose)
    NULL
  }

  if (workers > 1L) {
    if (.Platform$OS.type == "windows") {
      stop("'workers > 1' is not supported on Windows; use workers = 1L or a Windows-compatible parallel backend.")
    }
    result <- parallel::mclapply(seq_along(input_files), convert_one,
                                 mc.cores = min(workers, length(input_files)),
                                 mc.preschedule = FALSE)
    failed <- vapply(result, inherits, logical(1), what = "try-error")
    if (any(failed))
      stop("One or more DBC conversions failed: ",
           paste(input_files[failed], collapse = ", "))
  } else {
    for (i in seq_along(input_files)) convert_one(i)
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
#' @param batch_size  Integer. Number of rows per streamed chunk / row group.
#'   Default \code{100000L}.
#' @param encoding    Character string. Source encoding of character fields.
#'   Default \code{"latin1"}.
#' @param cols        Character vector or \code{NULL}. Names of columns to
#'   include in the Parquet file. \code{NULL} (default) writes all columns.
#' @param verbose     Logical. If \code{TRUE}, prints file metrics,
#'   a progress bar, and elapsed time upon completion. Default \code{FALSE}.
#' @param progress    Function or \code{NULL}. Optional callback for progress reporting.
#'
#' @return \code{TRUE} invisibly on success. Stops if the \pkg{arrow} package
#'   is not installed.
#'
#' @examples
#' \donttest{
#' if (requireNamespace("arrow", quietly = TRUE)) {
#'   dbc <- system.file("extdata", "sids.dbc", package = "dbcturbo")
#'   parquet <- tempfile(fileext = ".parquet")
#'   dbc_to_parquet(dbc, parquet)
#'   arrow::read_parquet(parquet)
#'   unlink(parquet)
#' }
#' }
#'
#' @export
dbc_to_parquet <- function(input_file, output_file, batch_size = 100000L, encoding = "latin1", verbose = FALSE, progress = NULL, cols = NULL) {
  .assert_scalar_string(input_file, "input_file")
  .assert_scalar_string(output_file, "output_file")
  batch_size <- .assert_positive_integer(batch_size, "batch_size")
  .assert_scalar_logical(verbose, "verbose")

  if (!is.null(encoding)) .assert_scalar_string(encoding, "encoding")
  if (!is.null(cols) && (!is.character(cols) || !length(cols) || anyNA(cols)))
    stop("'cols' must be a non-empty character vector of field names, or NULL")
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

  # Column types come from the DBF field descriptors and match the typed
  # chunks produced by the native reader.
  meta <- dbc_inspect(input_file)
  active_fields <- meta$fields
  if (!is.null(cols)) {
    unknown <- setdiff(cols, meta$fields$name)
    if (length(unknown))
      stop("Unknown column(s): ", paste(unknown, collapse = ", "))
    active_fields <- meta$fields[match(cols, meta$fields$name), , drop = FALSE]
  }
  schema <- .arrow_schema_from_fields(active_fields)

  output_dir <- dirname(output_file)
  if (!dir.exists(output_dir))
    stop("output directory does not exist: ", output_dir)
  tmp_output <- tempfile(paste0(".", basename(output_file), "-"),
                         tmpdir = output_dir, fileext = ".parquet")
  on.exit(if (file.exists(tmp_output)) unlink(tmp_output), add = TRUE)

  sink <- arrow::FileOutputStream$create(tmp_output)
  properties <- arrow::ParquetWriterProperties$create(schema$names)
  writer <- arrow::ParquetFileWriter$create(
    schema, sink, properties = properties
  )
  writer_open <- TRUE
  sink_open <- TRUE
  on.exit({
    if (writer_open) try(writer$Close(), silent = TRUE)
    if (sink_open) try(sink$close(), silent = TRUE)
  }, add = TRUE)

  written <- 0L
  started <- proc.time()[["elapsed"]]
  progress_bar <- NULL
  if (verbose) {
    progress_bar <- utils::txtProgressBar(min = 0, max = max(meta$nrecords, 1L), style = 3)
    on.exit(try(close(progress_bar), silent = TRUE), add = TRUE)
  }

  write_chunk <- function(chunk) {
    rows <- nrow(chunk)
    if (rows > 0L) {
      table <- arrow::Table$create(chunk, schema = schema)
      writer$WriteTable(table, rows)
      written <<- written + rows
    }
    if (!is.null(progress)) progress(written, meta$nrecords)
    if (!is.null(progress_bar))
      utils::setTxtProgressBar(progress_bar, min(written, meta$nrecords))
    invisible(NULL)
  }

  delivered <- read_dbc_chunks(
    input_file, callback = write_chunk, chunk_size = batch_size,
    encoding = encoding, cols = cols, coerce_types = TRUE
  )
  if (!identical(as.double(delivered), as.double(written)))
    stop("internal error: delivered and written row counts differ")

  writer$Close()
  writer_open <- FALSE
  sink$close()
  sink_open <- FALSE

  if (file.exists(output_file) && unlink(output_file) != 0L) {
    # Release any lingering memory-mapped file handles (e.g. arrow on Windows)
    gc(verbose = FALSE)
    if (unlink(output_file) != 0L)
      stop("cannot replace existing Parquet file at ", output_file)
  }
  if (!file.rename(tmp_output, output_file))
    stop("cannot move completed Parquet file to ", output_file)

  if (verbose) {
    elapsed <- proc.time()[["elapsed"]] - started
    message("Wrote ", format(written, big.mark = ","), " rows to ",
            basename(output_file), " in ", sprintf("%.2f", elapsed), " s")
  }
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


# -- Arrow schema from DBF metadata (not exported) -----------------------------
# Maps DBF field descriptors to an arrow::Schema so CSV parsing uses the DBF's
# declared types instead of unreliable block-wise inference. Character fields
# are kept as UTF-8 strings, preserving bytes exactly.
.arrow_schema_from_fields <- function(fields) {
  field_type <- function(type, decimals) {
    if (type == "N") {
      if (decimals > 0L) arrow::float64() else arrow::int64()
    } else if (type == "D") {
      arrow::date32()
    } else if (type == "L") {
      arrow::boolean()
    } else {
      arrow::string()
    }
  }
  do.call(arrow::schema,
          Map(function(nm, ty, dec) arrow::field(nm, field_type(ty, dec)),
              fields$name, fields$type, fields$decimals))
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
