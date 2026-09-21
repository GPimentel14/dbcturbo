# Test fixture

`sids.dbc` is a 5 KB DBC fixture used only by the automated tests.  It is the
compressed form of the `sids.dbf` example distributed with R's `foreign`
package and was obtained from the AGPL-3 `read.dbc` package by Daniela
Petruzalek.  It is included so that the package tests are fast, reproducible,
and independent of network access.

The production-scale DATASUS files used for manual benchmarks are deliberately
not distributed in this package.
