/*
 * r_bridge.c — R <-> C interface for dbcturbo (.Call() API).
 *
 * Entry points (registered in init.c):
 *   C_dbc2dbf     SEXP(SEXP, SEXP)
 *   C_dbc_to_csv  SEXP(SEXP, SEXP, SEXP, SEXP, SEXP, SEXP)
 *   C_dbc_inspect SEXP(SEXP)
 *
 * R objects allocated here are protected before further allocation; errors are
 * reported to R with Rf_error().
 */

#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Utils.h>   /* R_tmpnam(), R_TempDir */
#include <stdint.h>

#include "dbc_engine.h"

#define DBC_MAX_FIELDS 512

/* ── check_string ─────────────────────────────────────────────────────────────
 * Validates that an SEXP is a length-1 character string, not NA, and not empty.
 * Returns the C string pointer, or NULL on validation failure.
 * ─────────────────────────────────────────────────────────────────────────── */
static const char *require_scalar_string(SEXP x, const char *param_name)
{
    if (!Rf_isString(x) || Rf_length(x) < 1 || STRING_ELT(x, 0) == NA_STRING)
        Rf_error("'%s' must be a non-empty, non-NA character string", param_name);
    const char *s = CHAR(STRING_ELT(x, 0));
    if (s[0] == '\0')
        Rf_error("'%s' must not be an empty string", param_name);
    return s;
}

/* ── progress_cb_bridge ───────────────────────────────────────────────────────
 * Callback passed to the C engine. Constructs an R function call dynamically
 * and evaluates it to report progress. Handles user interrupts (Ctrl+C).
 *
 * Safe pattern: PROTECT each scalar individually, then build the call.
 * The call cell produced by Rf_lang3 is itself rooted via PROTECT.
 * After Rf_eval the entire expression is dispensable; UNPROTECT(3).
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    SEXP r_fn;
    int  active;
} progress_bridge_t;

static void progress_cb_bridge(int64_t done, int64_t total, void *ctx)
{
    progress_bridge_t *pb = (progress_bridge_t *)ctx;
    R_CheckUserInterrupt();
    if (!pb->active) return;

    SEXP s_done  = PROTECT(Rf_ScalarReal((double)done));
    SEXP s_total = PROTECT(Rf_ScalarReal((double)total));
    SEXP call    = PROTECT(Rf_lang3(pb->r_fn, s_done, s_total));
    Rf_eval(call, R_GlobalEnv);
    UNPROTECT(3);
}

/* ── Error helper ─────────────────────────────────────────────────────────── */

static void throw_dbc_error(dbc_error_t rc, const char *msg)
{
    if (msg && msg[0])
        Rf_error("dbcturbo error: %s", msg);
    else
        Rf_error("dbcturbo error: unknown failure (code %d)", (int)rc);
}

/* ── C_dbc2dbf ───────────────────────────────────────────────────────────── */

SEXP C_dbc2dbf(SEXP r_input, SEXP r_output)
{
    const char *in_path  = require_scalar_string(r_input,  "input_file");
    const char *out_path = require_scalar_string(r_output, "output_file");

    char err_buf[512];
    err_buf[0] = '\0';
    dbc_error_t rc = dbc2dbf_stream(in_path, out_path, err_buf, sizeof(err_buf));
    if (rc != DBC_OK) throw_dbc_error(rc, err_buf);

    return Rf_ScalarLogical(TRUE);
}

/* ── C_dbc_to_csv ─────────────────────────────────────────────────────────── */

SEXP C_dbc_to_csv(SEXP r_input, SEXP r_output, SEXP r_batch,
                  SEXP r_encoding, SEXP r_selected_fields, SEXP r_progress_fn)
{
    const char *in_path  = require_scalar_string(r_input,  "input_file");
    const char *out_path = require_scalar_string(r_output, "output_file");

    int batch = Rf_isInteger(r_batch) ? INTEGER(r_batch)[0]
                                      : (int)Rf_asReal(r_batch);
    if (batch <= 0) batch = 4096;

    const char *encoding = NULL;
    if (Rf_isString(r_encoding) && Rf_length(r_encoding) > 0
        && STRING_ELT(r_encoding, 0) != NA_STRING) {
        const char *enc = CHAR(STRING_ELT(r_encoding, 0));
        if (enc[0] != '\0') encoding = enc;
    }

    const int *selected_fields = NULL;
    int nselected_fields = 0;
    if (r_selected_fields != R_NilValue) {
        if (!Rf_isInteger(r_selected_fields))
            Rf_error("'cols' must be an integer vector or NULL");
        nselected_fields = Rf_length(r_selected_fields);
        if (nselected_fields < 1)
            Rf_error("'cols' must select at least one field");
        selected_fields = INTEGER(r_selected_fields);
    }

    progress_bridge_t pb;
    pb.r_fn  = r_progress_fn;
    pb.active = (r_progress_fn != R_NilValue && Rf_isFunction(r_progress_fn));

    /* Generate a unique temp path in R's temp directory.
     * R_tmpnam() is cross-platform and always points to a user-writable
     * directory — avoids the C:\ permission problem of tmpfile() on Windows
     * and the POSIX-only limitation of mkstemp().
     * We get R's tempdir via Rf_eval() to avoid Rembedded.h dependency.
     * The returned string is malloc'd; freed via R_free_tmpnam() after use. */
    SEXP r_tempdir = PROTECT(Rf_eval(Rf_lang1(Rf_install("tempdir")), R_GlobalEnv));
    const char *tdir = CHAR(STRING_ELT(r_tempdir, 0));
    char *rtmp = R_tmpnam("dbcturbo_", tdir);
    UNPROTECT(1);

    char err_buf[512];
    err_buf[0] = '\0';
    dbc_error_t rc = dbc_to_csv_stream(in_path, out_path,
                                       rtmp,
                                       batch, encoding, selected_fields, nselected_fields,
                                       progress_cb_bridge, &pb,
                                       err_buf, sizeof(err_buf));
    R_free_tmpnam(rtmp);
    if (rc != DBC_OK) throw_dbc_error(rc, err_buf);

    return Rf_ScalarLogical(TRUE);
}

/* ── C_dbc_inspect ────────────────────────────────────────────────────────── */

SEXP C_dbc_inspect(SEXP r_input)
{
    const char *in_path = require_scalar_string(r_input, "input_file");

    char     field_names[DBC_MAX_FIELDS][DBF_FIELD_NAME_LEN];
    char     field_types[DBC_MAX_FIELDS];
    uint8_t  field_widths[DBC_MAX_FIELDS];
    uint8_t  field_decs[DBC_MAX_FIELDS];
    int      nfields  = 0;
    uint32_t nrecords = 0;

    char err_buf[512];
    err_buf[0] = '\0';
    dbc_error_t rc = dbc_inspect(in_path, field_names, field_types,
                                 field_widths, field_decs,
                                 &nfields, &nrecords,
                                 DBC_MAX_FIELDS, err_buf, sizeof(err_buf));
    if (rc != DBC_OK) throw_dbc_error(rc, err_buf);

    /* Keep all allocated R objects protected until the result is complete. */
    SEXP s_name  = PROTECT(Rf_allocVector(STRSXP, nfields)); /* [1] */
    SEXP s_type  = PROTECT(Rf_allocVector(STRSXP, nfields)); /* [2] */
    SEXP s_width = PROTECT(Rf_allocVector(INTSXP, nfields)); /* [3] */
    SEXP s_decs  = PROTECT(Rf_allocVector(INTSXP, nfields)); /* [4] */

    for (int i = 0; i < nfields; i++) {
        SET_STRING_ELT(s_name, i, Rf_mkChar(field_names[i]));
        char type_str[2] = {field_types[i], '\0'};
        SET_STRING_ELT(s_type, i, Rf_mkChar(type_str));
        INTEGER(s_width)[i] = (int)field_widths[i];
        INTEGER(s_decs)[i]  = (int)field_decs[i];
    }

    SEXP fields_df = PROTECT(Rf_allocVector(VECSXP, 4)); /* [5] */
    SET_VECTOR_ELT(fields_df, 0, s_name);
    SET_VECTOR_ELT(fields_df, 1, s_type);
    SET_VECTOR_ELT(fields_df, 2, s_width);
    SET_VECTOR_ELT(fields_df, 3, s_decs);

    SEXP col_names = PROTECT(Rf_allocVector(STRSXP, 4)); /* [6] */
    SET_STRING_ELT(col_names, 0, Rf_mkChar("name"));
    SET_STRING_ELT(col_names, 1, Rf_mkChar("type"));
    SET_STRING_ELT(col_names, 2, Rf_mkChar("width"));
    SET_STRING_ELT(col_names, 3, Rf_mkChar("decimals"));
    Rf_setAttrib(fields_df, R_NamesSymbol, col_names);

    SEXP row_names = PROTECT(Rf_allocVector(INTSXP, 2)); /* [7] */
    INTEGER(row_names)[0] = NA_INTEGER;
    INTEGER(row_names)[1] = -nfields;
    Rf_setAttrib(fields_df, R_RowNamesSymbol, row_names);
    Rf_setAttrib(fields_df, R_ClassSymbol, Rf_mkString("data.frame"));

    SEXP result = PROTECT(Rf_allocVector(VECSXP, 2)); /* [8] */
    SET_VECTOR_ELT(result, 0, fields_df);
    SET_VECTOR_ELT(result, 1, Rf_ScalarInteger((int)nrecords));

    SEXP res_names = PROTECT(Rf_allocVector(STRSXP, 2)); /* [9] */
    SET_STRING_ELT(res_names, 0, Rf_mkChar("fields"));
    SET_STRING_ELT(res_names, 1, Rf_mkChar("nrecords"));
    Rf_setAttrib(result, R_NamesSymbol, res_names);

    UNPROTECT(9);
    return result;
}
