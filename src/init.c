/* init.c
 * R package DLL initialisation.
 * Registers all .Call() entry points for R's C API.
 */

#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

/* Forward declarations */
SEXP C_dbc2dbf(SEXP, SEXP);
SEXP C_dbc_to_csv(SEXP, SEXP, SEXP, SEXP, SEXP);
SEXP C_dbc_inspect(SEXP);

static const R_CallMethodDef CallEntries[] = {
    {"C_dbc2dbf",    (DL_FUNC) &C_dbc2dbf,    2},
    {"C_dbc_to_csv", (DL_FUNC) &C_dbc_to_csv, 5},
    {"C_dbc_inspect",(DL_FUNC) &C_dbc_inspect, 1},
    {NULL, NULL, 0}
};

void R_init_dbcturbo(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE); /* only allow registered entry points */
    /* R_forceSymbols not set: allows both .Call("C_name") and native refs */
}
