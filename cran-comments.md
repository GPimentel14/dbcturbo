## Test environments
* Local: Ubuntu 24.04.4 LTS, R 4.6.1
* win-builder: Windows Server 2022, R Under development (unstable) (2026-09-20 r90574 ucrt)

## R CMD check results

* Local Ubuntu: 0 ERRORs | 0 WARNINGs | 0 NOTEs
* win-builder: 0 ERRORs | 0 WARNINGs | 0 NOTEs

### win-builder submission notice

* **New submission**: This is the initial submission of `dbcturbo` to CRAN.
  Win-builder reports this as an informational notice, not as a technical NOTE.

## Additional Notes
* This package provides a memory-efficient C-based streaming decompression engine for DATASUS `.dbc` files.
* The core PKWare Implode decompression algorithm is derived from `blast.c` by Mark Adler (properly acknowledged in `DESCRIPTION` as `ctb` and within `src/blast.c`).
