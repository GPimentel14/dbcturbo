/*
 * r_bridge.c — R <-> C interface for dbcturbo (.Call() API).
 *
 * Entry points (registered in init.c):
 *   C_dbc2dbf          SEXP(SEXP, SEXP)
 *   C_dbc_to_csv       SEXP(SEXP, SEXP, SEXP, SEXP, SEXP, SEXP)
 *   C_dbc_read_native  SEXP(SEXP, SEXP, SEXP, SEXP)
 *   C_dbc_validate     SEXP(SEXP)
 *   C_dbc_inspect      SEXP(SEXP)
 *
 * R objects allocated here are protected before further allocation; errors are
 * reported to R with Rf_error().
 */

#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Utils.h>   /* R_tmpnam(), R_TempDir */
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>

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
    char err_buf[512];
    err_buf[0] = '\0';
    dbc_error_t rc = dbc_to_csv_stream(in_path, out_path,
                                       NULL,
                                       batch, encoding, selected_fields, nselected_fields,
                                       progress_cb_bridge, &pb,
                                       err_buf, sizeof(err_buf));
    if (rc != DBC_OK) throw_dbc_error(rc, err_buf);

    return Rf_ScalarLogical(TRUE);
}

/* ── C_dbc_read_native ─────────────────────────────────────────────────────
 * Small-file fast path: decode DBF records directly into character vectors.
 * Encoding and DBF type conversion remain in R so this has the same public
 * semantics as the CSV engines, without creating DBF or CSV temporary files.
 * ───────────────────────────────────────────────────────────────────────── */
/* ── Fast string interning cache (per-column, GC-safe) ─────────────────────
 * Every SEXP cached here is immediately stored in the protected column STRSXP,
 * so it stays alive for the lifetime of the column without extra GC roots.
 * Caches are cleared whenever a new chunk column vector is allocated. */
#define STR_CACHE_SLOTS   32
#define STR_CACHE_MASK    (STR_CACHE_SLOTS - 1)
#define STR_CACHE_MAX_LEN 24

typedef struct {
    uint8_t len;
    unsigned char bytes[STR_CACHE_MAX_LEN];
    SEXP sexp;
} str_cache_entry_t;

typedef struct {
    str_cache_entry_t slots[STR_CACHE_SLOTS];
} str_col_cache_t;

static inline unsigned hash_bytes(const unsigned char *p, int len)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return (unsigned)(h ^ (h >> 5)) & STR_CACHE_MASK;
}

static inline SEXP intern_cell_string(const unsigned char *p, int len,
                                      dbc_encoding_t enc,
                                      str_col_cache_t *cache)
{
    str_cache_entry_t *slot = NULL;
    if (cache && len <= STR_CACHE_MAX_LEN) {
        slot = &cache->slots[hash_bytes(p, len)];
        if (slot->len == (uint8_t)len && memcmp(slot->bytes, p, (size_t)len) == 0)
            return slot->sexp;
    }

    int all_ascii = 1;
    if (enc != DBC_ENC_UTF8) {
        for (int i = 0; i < len; i++) {
            if (p[i] > 0x7F) { all_ascii = 0; break; }
        }
    }

    SEXP s;
    if (all_ascii || enc == DBC_ENC_UTF8) {
        s = Rf_mkCharLenCE((const char *)p, len, CE_UTF8);
    } else {
        /* Field width <= 255, so UTF-8 expansion (<= 3x) always fits. */
        char utf8[3 * 256];
        size_t n = dbc_transcode_to_utf8(p, len, utf8, enc);
        s = Rf_mkCharLenCE(utf8, (int)n, CE_UTF8);
    }

    if (slot) {
        slot->len = (uint8_t)len;
        memcpy(slot->bytes, p, (size_t)len);
        slot->sexp = s;
    }
    return s;
}

static inline int parse_fast_int(const unsigned char *p, int len, int *out)
{
    int i = 0;
    int neg = 0;
    if (p[0] == '-') { neg = 1; i = 1; }
    else if (p[0] == '+') { i = 1; }
    if (i >= len) return 0;
    int64_t acc = 0;
    for (; i < len; i++) {
        unsigned d = (unsigned)(p[i] - '0');
        if (d > 9U) return 0;
        acc = acc * 10 + (int64_t)d;
        if (acc > (int64_t)INT_MAX + 1) return 0;
    }
    int64_t val = neg ? -acc : acc;
    if (val <= (int64_t)INT_MIN || val > (int64_t)INT_MAX) return 0;
    *out = (int)val;
    return 1;
}

static R_xlen_t parse_n_max(SEXP r_n_max, uint32_t nrecords)
{
    if (r_n_max == R_NilValue) return (R_xlen_t)nrecords;
    double v = Rf_asReal(r_n_max);
    if (ISNAN(v) || v < 0)
        Rf_error("'n_max' must be a non-negative number");
    if (!R_FINITE(v) || v >= (double)nrecords)
        return (R_xlen_t)nrecords;
    if (v > (double)INT_MAX)
        return (R_xlen_t)INT_MAX;
    return (R_xlen_t)v;
}

typedef struct {
    SEXP columns;
    SEXP *col_sexp;
    str_col_cache_t *str_caches;
    const size_t *offsets;
    const uint8_t *widths;
    int nselected;
    R_xlen_t out_row;
    R_xlen_t max_rows;
    dbc_encoding_t enc;
} native_read_ctx_t;

/* Read the source encoding from an R character scalar. NULL/NA falls back to
 * latin1 (modern DATASUS default). */
static dbc_encoding_t parse_encoding(SEXP r_encoding)
{
    const char *name = NULL;
    if (Rf_isString(r_encoding) && Rf_length(r_encoding) > 0 &&
        STRING_ELT(r_encoding, 0) != NA_STRING) {
        const char *s = CHAR(STRING_ELT(r_encoding, 0));
        if (s[0] != '\0') name = s;
    }
    return dbc_encoding_from_name(name);
}

static int native_record_cb(const unsigned char *record, int64_t index, void *user_data)
{
    (void)index;
    native_read_ctx_t *ctx = (native_read_ctx_t *)user_data;
    if (record[0] == 0x2A) return 0; /* deleted DBF record */
    if (ctx->out_row >= ctx->max_rows) return DBC_CB_STOP;

    R_xlen_t row = ctx->out_row;
    for (int j = 0; j < ctx->nselected; j++) {
        const unsigned char *cell = record + 1 + ctx->offsets[j];
        int len = (int)ctx->widths[j];
        int start = 0;
        while (start < len && (cell[start] == ' ' || cell[start] == '\0')) start++;
        while (len > start && (cell[len - 1] == ' ' || cell[len - 1] == '\0')) len--;
        if (start >= len) {
            SET_STRING_ELT(ctx->col_sexp[j], row, NA_STRING);
        } else {
            SET_STRING_ELT(ctx->col_sexp[j], row,
                           intern_cell_string(cell + start, len - start,
                                              ctx->enc, &ctx->str_caches[j]));
        }
    }
    ctx->out_row++;
    return ctx->out_row >= ctx->max_rows ? DBC_CB_STOP : 0;
}

static SEXP C_dbc_read_native_character(SEXP r_input, SEXP r_selected_fields,
                                        SEXP r_encoding, SEXP r_n_max)
{
    const char *in_path = require_scalar_string(r_input, "file");
    char field_names[DBC_MAX_FIELDS][DBF_FIELD_NAME_LEN];
    char field_types[DBC_MAX_FIELDS];
    uint8_t field_widths[DBC_MAX_FIELDS];
    uint8_t field_decs[DBC_MAX_FIELDS];
    int nfields = 0;
    uint32_t nrecords = 0;
    char err_buf[512] = {0};

    dbc_error_t rc = dbc_inspect(in_path, field_names, field_types, field_widths,
                                 field_decs, &nfields, &nrecords, DBC_MAX_FIELDS,
                                 err_buf, sizeof(err_buf));
    if (rc != DBC_OK) throw_dbc_error(rc, err_buf);
    R_xlen_t alloc_rows = parse_n_max(r_n_max, nrecords);
    if (alloc_rows > INT_MAX)
        Rf_error("native engine cannot allocate more than %d records", INT_MAX);

    int nselected = nfields;
    const int *requested = NULL;
    if (r_selected_fields != R_NilValue) {
        if (!Rf_isInteger(r_selected_fields) || Rf_length(r_selected_fields) < 1)
            Rf_error("'cols' must be a non-empty integer vector or NULL");
        nselected = Rf_length(r_selected_fields);
        requested = INTEGER(r_selected_fields);
    }

    int *selected = (int *)R_alloc((size_t)nselected, sizeof(int));
    for (int j = 0; j < nselected; j++) {
        int field = requested ? requested[j] : j;
        if (field < 0 || field >= nfields)
            Rf_error("selected field index is out of bounds");
        selected[j] = field;
    }

    size_t *field_offsets = (size_t *)R_alloc((size_t)nfields, sizeof(size_t));
    size_t data_width = 0;
    for (int i = 0; i < nfields; i++) {
        field_offsets[i] = data_width;
        data_width += (size_t)field_widths[i];
    }
    if (data_width + 1 > UINT16_MAX)
        Rf_error("DBF record width is too large for the native engine");

    size_t *selected_offsets = (size_t *)R_alloc((size_t)nselected, sizeof(size_t));
    uint8_t *selected_widths = (uint8_t *)R_alloc((size_t)nselected, sizeof(uint8_t));
    SEXP *col_sexp = (SEXP *)R_alloc((size_t)nselected, sizeof(SEXP));
    str_col_cache_t *str_caches = (str_col_cache_t *)R_alloc((size_t)nselected, sizeof(str_col_cache_t));
    memset(str_caches, 0, (size_t)nselected * sizeof(str_col_cache_t));

    SEXP columns = PROTECT(Rf_allocVector(VECSXP, nselected));
    SEXP names = PROTECT(Rf_allocVector(STRSXP, nselected));
    for (int j = 0; j < nselected; j++) {
        selected_offsets[j] = field_offsets[selected[j]];
        selected_widths[j] = field_widths[selected[j]];
        SEXP col = Rf_allocVector(STRSXP, alloc_rows);
        SET_VECTOR_ELT(columns, j, col);
        col_sexp[j] = col;
        SET_STRING_ELT(names, j, Rf_mkChar(field_names[selected[j]]));
    }

    native_read_ctx_t ctx;
    ctx.columns = columns;
    ctx.col_sexp = col_sexp;
    ctx.str_caches = str_caches;
    ctx.offsets = selected_offsets;
    ctx.widths = selected_widths;
    ctx.nselected = nselected;
    ctx.out_row = 0;
    ctx.max_rows = alloc_rows;
    ctx.enc = parse_encoding(r_encoding);

    if (alloc_rows > 0) {
        rc = dbc_read_records(in_path, (uint16_t)(data_width + 1), nrecords,
                              native_record_cb, &ctx, err_buf, sizeof(err_buf));
        if (rc != DBC_OK) {
            UNPROTECT(2);
            throw_dbc_error(rc, err_buf);
        }
    }

    if (ctx.out_row != alloc_rows) {
        for (int j = 0; j < nselected; j++)
            SET_VECTOR_ELT(columns, j,
                           Rf_xlengthgets(VECTOR_ELT(columns, j), ctx.out_row));
    }
    Rf_setAttrib(columns, R_NamesSymbol, names);
    Rf_setAttrib(columns, R_ClassSymbol, Rf_mkString("data.frame"));
    SEXP row_names = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(row_names)[0] = NA_INTEGER;
    INTEGER(row_names)[1] = -(int)ctx.out_row;
    Rf_setAttrib(columns, R_RowNamesSymbol, row_names);

    UNPROTECT(3);
    return columns;
}

typedef enum {
    TYPED_CHARACTER = 0, TYPED_INTEGER, TYPED_REAL, TYPED_LOGICAL, TYPED_DATE
} typed_mode_t;

typedef struct {
    SEXP columns;
    SEXP *col_sexp;
    int **col_int;
    double **col_real;
    str_col_cache_t *str_caches;
    const size_t *offsets;
    const uint8_t *widths;
    typed_mode_t *modes;
    int nselected;
    R_xlen_t nrecords;
    R_xlen_t row;
    R_xlen_t max_rows;
    dbc_encoding_t enc;
} typed_read_ctx_t;

static void bind_typed_columns(typed_read_ctx_t *reader, SEXP columns)
{
    reader->columns = columns;
    for (int j = 0; j < reader->nselected; j++) {
        SEXP col = VECTOR_ELT(columns, j);
        reader->col_sexp[j] = col;
        reader->col_int[j] = NULL;
        reader->col_real[j] = NULL;
        if (reader->modes[j] == TYPED_INTEGER)
            reader->col_int[j] = INTEGER(col);
        else if (reader->modes[j] == TYPED_LOGICAL)
            reader->col_int[j] = LOGICAL(col);
        else if (reader->modes[j] == TYPED_REAL || reader->modes[j] == TYPED_DATE)
            reader->col_real[j] = REAL(col);
    }
    if (reader->str_caches)
        memset(reader->str_caches, 0, (size_t)reader->nselected * sizeof(str_col_cache_t));
}

static int leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int64_t civil_days(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    unsigned doy = (153U * adjusted_month + 2U) / 5U + day - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int parse_date(const unsigned char *value, int length, double *result)
{
    if (length != 8) return 0;
    int all_zero = 1;
    for (int i = 0; i < 8; i++) {
        if (value[i] < '0' || value[i] > '9') return 0;
        if (value[i] != '0') all_zero = 0;
    }
    if (all_zero) return 0;

    int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100
             + (value[2] - '0') * 10 + value[3] - '0';
    unsigned month = (unsigned)((value[4] - '0') * 10 + value[5] - '0');
    unsigned day = (unsigned)((value[6] - '0') * 10 + value[7] - '0');
    static const unsigned days_in_month[12] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month < 1 || month > 12) return 0;
    unsigned maximum = days_in_month[month - 1];
    if (month == 2 && leap_year(year)) maximum = 29;
    if (day < 1 || day > maximum) return 0;
    *result = (double)civil_days(year, month, day);
    return 1;
}

static void promote_integer(typed_read_ctx_t *ctx, int j)
{
    SEXP old = ctx->col_sexp[j];
    const int *old_ptr = INTEGER(old);
    SEXP replacement = PROTECT(Rf_allocVector(REALSXP, ctx->nrecords));
    double *new_ptr = REAL(replacement);
    for (R_xlen_t i = 0; i < ctx->row; i++) {
        int value = old_ptr[i];
        new_ptr[i] = value == NA_INTEGER ? NA_REAL : (double)value;
    }
    SET_VECTOR_ELT(ctx->columns, j, replacement);
    ctx->col_sexp[j] = replacement;
    ctx->col_int[j] = NULL;
    ctx->col_real[j] = new_ptr;
    ctx->modes[j] = TYPED_REAL;
    UNPROTECT(1);
}

static int typed_record_cb(const unsigned char *record, int64_t index,
                           void *user_data)
{
    (void)index;
    typed_read_ctx_t *ctx = (typed_read_ctx_t *)user_data;
    if (record[0] == 0x2A) return 0;
    if (ctx->max_rows >= 0 && ctx->row >= ctx->max_rows) return DBC_CB_STOP;

    R_xlen_t row = ctx->row;
    for (int j = 0; j < ctx->nselected; j++) {
        const unsigned char *cell = record + 1 + ctx->offsets[j];
        int end = (int)ctx->widths[j];
        int start = 0;
        while (start < end && (cell[start] == ' ' || cell[start] == '\0')) start++;
        while (end > start && (cell[end - 1] == ' ' || cell[end - 1] == '\0')) end--;
        int length = end - start;
        typed_mode_t mode = ctx->modes[j];

        if (length == 0) {
            if (mode == TYPED_CHARACTER)
                SET_STRING_ELT(ctx->col_sexp[j], row, NA_STRING);
            else if (mode == TYPED_INTEGER || mode == TYPED_LOGICAL)
                ctx->col_int[j][row] = NA_INTEGER;
            else
                ctx->col_real[j][row] = NA_REAL;
            continue;
        }

        if (mode == TYPED_CHARACTER) {
            SET_STRING_ELT(ctx->col_sexp[j], row,
                           intern_cell_string(cell + start, length,
                                              ctx->enc, &ctx->str_caches[j]));
        } else if (mode == TYPED_INTEGER) {
            int ival;
            if (parse_fast_int(cell + start, length, &ival)) {
                ctx->col_int[j][row] = ival;
            } else {
                char number[256];
                memcpy(number, cell + start, (size_t)length);
                number[length] = '\0';
                char *tail = NULL;
                double value = strtod(number, &tail);
                int valid = tail != number && *tail == '\0';
                if (!valid) {
                    ctx->col_int[j][row] = NA_INTEGER;
                } else if (isfinite(value) &&
                           value > (double)INT_MIN && value <= (double)INT_MAX) {
                    ctx->col_int[j][row] = (int)value;
                } else {
                    promote_integer(ctx, j);
                    ctx->col_real[j][row] = value;
                }
            }
        } else if (mode == TYPED_REAL) {
            char number[256];
            memcpy(number, cell + start, (size_t)length);
            number[length] = '\0';
            char *tail = NULL;
            double value = strtod(number, &tail);
            ctx->col_real[j][row] = (tail != number && *tail == '\0') ? value : NA_REAL;
        } else if (mode == TYPED_LOGICAL) {
            int value = toupper((unsigned char)cell[start]);
            if (value == 'T' || value == 'Y' || value == 'S' || value == '1')
                ctx->col_int[j][row] = TRUE;
            else if (value == 'F' || value == 'N' || value == '0')
                ctx->col_int[j][row] = FALSE;
            else
                ctx->col_int[j][row] = NA_LOGICAL;
        } else {
            double value;
            ctx->col_real[j][row] =
                parse_date(cell + start, length, &value) ? value : NA_REAL;
        }
    }
    ctx->row++;
    return (ctx->max_rows >= 0 && ctx->row >= ctx->max_rows) ? DBC_CB_STOP : 0;
}

SEXP C_dbc_read_native(SEXP r_input, SEXP r_selected_fields,
                       SEXP r_coerce_types, SEXP r_encoding, SEXP r_n_max)
{
    int coerce_types = Rf_asLogical(r_coerce_types);
    if (coerce_types == NA_LOGICAL)
        Rf_error("'coerce_types' must be TRUE or FALSE");
    if (!coerce_types)
        return C_dbc_read_native_character(r_input, r_selected_fields, r_encoding, r_n_max);

    const char *path = require_scalar_string(r_input, "file");
    char names_buffer[DBC_MAX_FIELDS][DBF_FIELD_NAME_LEN];
    char types[DBC_MAX_FIELDS];
    uint8_t widths[DBC_MAX_FIELDS], decimals[DBC_MAX_FIELDS];
    int nfields = 0;
    uint32_t nrecords = 0;
    char error[512] = {0};
    dbc_error_t rc = dbc_inspect(path, names_buffer, types, widths, decimals,
                                 &nfields, &nrecords, DBC_MAX_FIELDS,
                                 error, sizeof(error));
    if (rc != DBC_OK) throw_dbc_error(rc, error);
    R_xlen_t alloc_rows = parse_n_max(r_n_max, nrecords);
    if (alloc_rows > INT_MAX)
        Rf_error("native engine cannot allocate more than %d records", INT_MAX);

    int nselected = nfields;
    const int *requested = NULL;
    if (r_selected_fields != R_NilValue) {
        if (!Rf_isInteger(r_selected_fields) || Rf_length(r_selected_fields) < 1)
            Rf_error("'cols' must be a non-empty integer vector or NULL");
        nselected = Rf_length(r_selected_fields);
        requested = INTEGER(r_selected_fields);
    }

    int *selected = (int *)R_alloc((size_t)nselected, sizeof(int));
    typed_mode_t *modes =
        (typed_mode_t *)R_alloc((size_t)nselected, sizeof(typed_mode_t));
    for (int j = 0; j < nselected; j++) {
        int field = requested ? requested[j] : j;
        if (field < 0 || field >= nfields)
            Rf_error("selected field index is out of bounds");
        selected[j] = field;
        modes[j] = TYPED_CHARACTER;
        if (types[field] == 'N')
            modes[j] = decimals[field] ? TYPED_REAL : TYPED_INTEGER;
        else if (types[field] == 'L')
            modes[j] = TYPED_LOGICAL;
        else if (types[field] == 'D')
            modes[j] = TYPED_DATE;
    }

    size_t *offsets = (size_t *)R_alloc((size_t)nfields, sizeof(size_t));
    size_t record_size = 1;
    for (int i = 0; i < nfields; i++) {
        offsets[i] = record_size - 1;
        record_size += widths[i];
    }
    if (record_size > UINT16_MAX)
        Rf_error("DBF record width is too large for the native engine");

    size_t *selected_offsets =
        (size_t *)R_alloc((size_t)nselected, sizeof(size_t));
    uint8_t *selected_widths =
        (uint8_t *)R_alloc((size_t)nselected, sizeof(uint8_t));
    for (int j = 0; j < nselected; j++) {
        selected_offsets[j] = offsets[selected[j]];
        selected_widths[j] = widths[selected[j]];
    }
    SEXP columns = PROTECT(Rf_allocVector(VECSXP, nselected));
    SEXP names = PROTECT(Rf_allocVector(STRSXP, nselected));
    for (int j = 0; j < nselected; j++) {
        SEXPTYPE storage = STRSXP;
        if (modes[j] == TYPED_INTEGER) storage = INTSXP;
        else if (modes[j] == TYPED_REAL || modes[j] == TYPED_DATE)
            storage = REALSXP;
        else if (modes[j] == TYPED_LOGICAL) storage = LGLSXP;

        SEXP column = PROTECT(Rf_allocVector(storage, alloc_rows));
        if (modes[j] == TYPED_DATE) {
            SEXP date_class = PROTECT(Rf_mkString("Date"));
            Rf_setAttrib(column, R_ClassSymbol, date_class);
            UNPROTECT(1);
        }
        SET_VECTOR_ELT(columns, j, column);
        UNPROTECT(1);
        SET_STRING_ELT(names, j, Rf_mkChar(names_buffer[selected[j]]));
    }

    SEXP *col_sexp = (SEXP *)R_alloc((size_t)nselected, sizeof(SEXP));
    int **col_int = (int **)R_alloc((size_t)nselected, sizeof(int *));
    double **col_real = (double **)R_alloc((size_t)nselected, sizeof(double *));
    str_col_cache_t *str_caches =
        (str_col_cache_t *)R_alloc((size_t)nselected, sizeof(str_col_cache_t));

    typed_read_ctx_t ctx;
    ctx.columns = columns;
    ctx.col_sexp = col_sexp;
    ctx.col_int = col_int;
    ctx.col_real = col_real;
    ctx.str_caches = str_caches;
    ctx.offsets = selected_offsets;
    ctx.widths = selected_widths;
    ctx.modes = modes;
    ctx.nselected = nselected;
    ctx.nrecords = alloc_rows;
    ctx.row = 0;
    ctx.max_rows = alloc_rows;
    ctx.enc = parse_encoding(r_encoding);
    bind_typed_columns(&ctx, columns);

    if (alloc_rows > 0) {
        rc = dbc_read_records(path, (uint16_t)record_size, nrecords,
                              typed_record_cb, &ctx, error, sizeof(error));
        if (rc != DBC_OK) {
            UNPROTECT(2);
            throw_dbc_error(rc, error);
        }
    }

    if (ctx.row != alloc_rows) {
        for (int j = 0; j < nselected; j++) {
            SEXP resized = PROTECT(
                Rf_xlengthgets(VECTOR_ELT(columns, j), ctx.row));
            SET_VECTOR_ELT(columns, j, resized);
            if (modes[j] == TYPED_DATE) {
                SEXP date_class = PROTECT(Rf_mkString("Date"));
                Rf_setAttrib(resized, R_ClassSymbol, date_class);
                UNPROTECT(1);
            }
            UNPROTECT(1);
        }
    }
    Rf_setAttrib(columns, R_NamesSymbol, names);
    SEXP data_frame_class = PROTECT(Rf_mkString("data.frame"));
    Rf_setAttrib(columns, R_ClassSymbol, data_frame_class);
    UNPROTECT(1);
    SEXP row_names = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(row_names)[0] = NA_INTEGER;
    INTEGER(row_names)[1] = -(int)ctx.row;
    Rf_setAttrib(columns, R_RowNamesSymbol, row_names);
    UNPROTECT(3);
    return columns;
}

typedef struct {
    typed_read_ctx_t reader;
    SEXP names;
    SEXP callback;
    R_xlen_t capacity;
    double total_rows;
} chunk_read_ctx_t;

static SEXP allocate_chunk_columns(chunk_read_ctx_t *ctx)
{
    SEXP columns = PROTECT(Rf_allocVector(VECSXP, ctx->reader.nselected));
    for (int j = 0; j < ctx->reader.nselected; j++) {
        SEXPTYPE storage = STRSXP;
        if (ctx->reader.modes[j] == TYPED_INTEGER) storage = INTSXP;
        else if (ctx->reader.modes[j] == TYPED_REAL ||
                 ctx->reader.modes[j] == TYPED_DATE) storage = REALSXP;
        else if (ctx->reader.modes[j] == TYPED_LOGICAL) storage = LGLSXP;

        SEXP column = PROTECT(Rf_allocVector(storage, ctx->capacity));
        if (ctx->reader.modes[j] == TYPED_DATE) {
            SEXP date_class = PROTECT(Rf_mkString("Date"));
            Rf_setAttrib(column, R_ClassSymbol, date_class);
            UNPROTECT(1);
        }
        SET_VECTOR_ELT(columns, j, column);
        UNPROTECT(1);
    }
    R_PreserveObject(columns);
    bind_typed_columns(&ctx->reader, columns);
    UNPROTECT(1);
    return columns;
}

static void emit_chunk(chunk_read_ctx_t *ctx, int allocate_next)
{
    SEXP columns = ctx->reader.columns;
    R_xlen_t rows = ctx->reader.row;
    if (rows < ctx->capacity) {
        for (int j = 0; j < ctx->reader.nselected; j++) {
            SEXP resized =
                PROTECT(Rf_xlengthgets(VECTOR_ELT(columns, j), rows));
            SET_VECTOR_ELT(columns, j, resized);
            if (ctx->reader.modes[j] == TYPED_DATE) {
                SEXP date_class = PROTECT(Rf_mkString("Date"));
                Rf_setAttrib(resized, R_ClassSymbol, date_class);
                UNPROTECT(1);
            }
            UNPROTECT(1);
        }
    }

    Rf_setAttrib(columns, R_NamesSymbol, ctx->names);
    SEXP data_frame_class = PROTECT(Rf_mkString("data.frame"));
    Rf_setAttrib(columns, R_ClassSymbol, data_frame_class);
    UNPROTECT(1);
    SEXP row_names = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(row_names)[0] = NA_INTEGER;
    INTEGER(row_names)[1] = -(int)rows;
    Rf_setAttrib(columns, R_RowNamesSymbol, row_names);
    UNPROTECT(1);

    SEXP call = PROTECT(Rf_lang2(ctx->callback, columns));
    Rf_eval(call, R_GlobalEnv);
    UNPROTECT(1);
    ctx->total_rows += (double)rows;

    R_ReleaseObject(columns);
    ctx->reader.row = 0;
    if (allocate_next)
        ctx->reader.columns = allocate_chunk_columns(ctx);
    else
        ctx->reader.columns = R_NilValue;
}

static int chunk_record_cb(const unsigned char *record, int64_t index,
                           void *user_data)
{
    chunk_read_ctx_t *ctx = (chunk_read_ctx_t *)user_data;
    if (record[0] == 0x2A) return 0;
    if (ctx->reader.columns == R_NilValue)
        ctx->reader.columns = allocate_chunk_columns(ctx);
    int rc = typed_record_cb(record, index, &ctx->reader);
    if (rc != 0) return rc;
    if (ctx->reader.row == ctx->capacity)
        emit_chunk(ctx, 0);
    return 0;
}

SEXP C_dbc_read_chunks(SEXP r_input, SEXP r_selected_fields,
                       SEXP r_coerce_types, SEXP r_encoding,
                       SEXP r_chunk_size, SEXP r_callback)
{
    const char *path = require_scalar_string(r_input, "file");
    int coerce_types = Rf_asLogical(r_coerce_types);
    if (coerce_types == NA_LOGICAL)
        Rf_error("'coerce_types' must be TRUE or FALSE");
    if (!Rf_isFunction(r_callback))
        Rf_error("'callback' must be a function");

    double requested_size = Rf_asReal(r_chunk_size);
    if (!R_FINITE(requested_size) || requested_size < 1 ||
        requested_size > INT_MAX)
        Rf_error("'chunk_size' must be between 1 and %d", INT_MAX);
    R_xlen_t chunk_size = (R_xlen_t)requested_size;

    char field_names[DBC_MAX_FIELDS][DBF_FIELD_NAME_LEN];
    char field_types[DBC_MAX_FIELDS];
    uint8_t field_widths[DBC_MAX_FIELDS], field_decs[DBC_MAX_FIELDS];
    int nfields = 0;
    uint32_t nrecords = 0;
    char error[512] = {0};
    dbc_error_t rc = dbc_inspect(path, field_names, field_types, field_widths,
                                 field_decs, &nfields, &nrecords,
                                 DBC_MAX_FIELDS, error, sizeof(error));
    if (rc != DBC_OK) throw_dbc_error(rc, error);

    int nselected = nfields;
    const int *requested = NULL;
    if (r_selected_fields != R_NilValue) {
        if (!Rf_isInteger(r_selected_fields) ||
            Rf_length(r_selected_fields) < 1)
            Rf_error("'cols' must be a non-empty integer vector or NULL");
        nselected = Rf_length(r_selected_fields);
        requested = INTEGER(r_selected_fields);
    }

    int *selected = (int *)R_alloc((size_t)nselected, sizeof(int));
    typed_mode_t *modes =
        (typed_mode_t *)R_alloc((size_t)nselected, sizeof(typed_mode_t));
    for (int j = 0; j < nselected; j++) {
        int field = requested ? requested[j] : j;
        if (field < 0 || field >= nfields)
            Rf_error("selected field index is out of bounds");
        selected[j] = field;
        modes[j] = TYPED_CHARACTER;
        if (coerce_types && field_types[field] == 'N')
            modes[j] = field_decs[field] || field_widths[field] > 9
                       ? TYPED_REAL : TYPED_INTEGER;
        else if (coerce_types && field_types[field] == 'L')
            modes[j] = TYPED_LOGICAL;
        else if (coerce_types && field_types[field] == 'D')
            modes[j] = TYPED_DATE;
    }

    size_t *all_offsets = (size_t *)R_alloc((size_t)nfields, sizeof(size_t));
    size_t record_size = 1;
    for (int i = 0; i < nfields; i++) {
        all_offsets[i] = record_size - 1;
        record_size += field_widths[i];
    }
    if (record_size > UINT16_MAX)
        Rf_error("DBF record width is too large");

    size_t *offsets =
        (size_t *)R_alloc((size_t)nselected, sizeof(size_t));
    uint8_t *widths =
        (uint8_t *)R_alloc((size_t)nselected, sizeof(uint8_t));
    SEXP names = PROTECT(Rf_allocVector(STRSXP, nselected));
    for (int j = 0; j < nselected; j++) {
        offsets[j] = all_offsets[selected[j]];
        widths[j] = field_widths[selected[j]];
        SET_STRING_ELT(names, j, Rf_mkChar(field_names[selected[j]]));
    }

    if (nrecords > 0 && (R_xlen_t)nrecords < chunk_size)
        chunk_size = (R_xlen_t)nrecords;

    SEXP *col_sexp = (SEXP *)R_alloc((size_t)nselected, sizeof(SEXP));
    int **col_int = (int **)R_alloc((size_t)nselected, sizeof(int *));
    double **col_real = (double **)R_alloc((size_t)nselected, sizeof(double *));
    str_col_cache_t *str_caches =
        (str_col_cache_t *)R_alloc((size_t)nselected, sizeof(str_col_cache_t));

    chunk_read_ctx_t ctx;
    ctx.names = names;
    ctx.callback = r_callback;
    ctx.capacity = chunk_size;
    ctx.total_rows = 0;
    ctx.reader.columns = R_NilValue;
    ctx.reader.col_sexp = col_sexp;
    ctx.reader.col_int = col_int;
    ctx.reader.col_real = col_real;
    ctx.reader.str_caches = str_caches;
    ctx.reader.offsets = offsets;
    ctx.reader.widths = widths;
    ctx.reader.modes = modes;
    ctx.reader.nselected = nselected;
    ctx.reader.nrecords = chunk_size;
    ctx.reader.row = 0;
    ctx.reader.max_rows = -1;
    ctx.reader.enc = parse_encoding(r_encoding);

    rc = dbc_read_records(path, (uint16_t)record_size, nrecords,
                          chunk_record_cb, &ctx, error, sizeof(error));
    if (rc != DBC_OK) {
        if (ctx.reader.columns != R_NilValue)
            R_ReleaseObject(ctx.reader.columns);
        UNPROTECT(1);
        throw_dbc_error(rc, error);
    }
    if (ctx.reader.row > 0)
        emit_chunk(&ctx, 0);
    else if (ctx.reader.columns != R_NilValue)
        R_ReleaseObject(ctx.reader.columns);

    UNPROTECT(1);
    return Rf_ScalarReal(ctx.total_rows);
}

static int validate_record_cb(const unsigned char *record, int64_t index,
                              void *user_data)
{
    (void)record; (void)index; (void)user_data;
    return 0;
}

SEXP C_dbc_validate(SEXP r_input)
{
    const char *path = require_scalar_string(r_input, "file");
    char names_buffer[DBC_MAX_FIELDS][DBF_FIELD_NAME_LEN];
    char types[DBC_MAX_FIELDS];
    uint8_t widths[DBC_MAX_FIELDS], decimals[DBC_MAX_FIELDS];
    int nfields = 0;
    uint32_t nrecords = 0;
    char error[512] = {0};
    dbc_error_t rc = dbc_inspect(path, names_buffer, types, widths, decimals,
                                 &nfields, &nrecords, DBC_MAX_FIELDS,
                                 error, sizeof(error));
    if (rc != DBC_OK) throw_dbc_error(rc, error);

    size_t record_size = 1;
    for (int i = 0; i < nfields; i++) record_size += widths[i];
    if (record_size > UINT16_MAX) Rf_error("DBF record width is too large");
    rc = dbc_read_records(path, (uint16_t)record_size, nrecords,
                          validate_record_cb, NULL, error, sizeof(error));
    if (rc != DBC_OK) throw_dbc_error(rc, error);
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
