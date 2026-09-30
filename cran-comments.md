## Resubmission

This is a resubmission. In this version I have:

* removed single quotation marks around file-format names in the Title and
  Description fields;
* replaced `\\dontrun{}` examples with runnable examples based on the small DBC
  file included in `inst/extdata`; the optional `arrow` example uses
  `\\donttest{}` and checks that the package is available; and
* made informational output suppressible with `message()` and retained the
  existing `verbose = FALSE` default.

## Test environments
* Local: Ubuntu 24.04.5 LTS, R 4.6.1
* win-builder: Windows Server 2022, R Under development (unstable) (2026-09-20 r90574 ucrt)

## R CMD check results

* Local Ubuntu: 0 ERRORs | 0 WARNINGs | 2 NOTEs
* win-builder: 0 ERRORs | 0 WARNINGs | 0 NOTEs

### win-builder submission notice

* **New submission**: This is the first version of `dbcturbo` intended for
  publication on CRAN.

* **Non-portable compilation flag**: the local R installation adds
  `-mno-omit-leaf-frame-pointer` through its own compiler configuration. The
  package does not set this flag; `src/Makevars` intentionally contains no
  custom compilation flags.
## Additional Notes
* This package provides a memory-efficient C-based streaming decompression engine for DATASUS `.dbc` files.
* The core PKWare Implode decompression algorithm is derived from `blast.c` by Mark Adler (properly acknowledged in `DESCRIPTION` as `ctb` and within `src/blast.c`).
