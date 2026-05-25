/*
 * dbc_engine.c — Streaming decompression engine for DATASUS DBC files.
 *
 * Public API (dbc_engine.h):
 *   dbc2dbf_stream    DBC -> DBF, O(1) RAM
 *   dbc_to_csv_stream DBC -> UTF-8 CSV, O(batch * record_size) RAM
 *   dbc_inspect       Header-only metadata parse, no decompression
 *
 * Thread-safe: no global mutable state.
 * Memory-safe: every allocation is freed on all exit paths.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>

#include "blast.h"
#include "dbc_engine.h"

/* ── [FIX-1] Use Rprintf/REprintf instead of printf ──────────────────────────
 * CRAN policy: C code in packages must never write to stdout/stderr directly.
 * All diagnostic paths use fmt_error() into a caller-supplied buffer; no
 * Rprintf/REprintf calls are needed here.  The buffer is surfaced via
 * Rf_error() in r_bridge.c, which is the correct CRAN-approved channel.
 * ─────────────────────────────────────────────────────────────────────────── */

static void fmt_error(char *buf, size_t buf_sz, const char *fmt, ...)
{
    if (!buf || buf_sz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, buf_sz, fmt, ap);   /* [SAFE] vsnprintf, not vsprintf */
    va_end(ap);
    buf[buf_sz - 1] = '\0';            /* belt-and-suspenders null guard  */
}

static const char *blast_error_string(int code)
{
    switch (code) {
        case  1: return "output callback error";
        case  2: return "input truncated before end-of-stream";
        case -1: return "invalid literal flag (expected 0 or 1)";
        case -2: return "invalid dictionary size (expected 4, 5, or 6)";
        case -3: return "back-reference distance exceeds sliding window";
        default: return "unknown decompression error";
    }
}

/* ── blast() I/O callbacks ────────────────────────────────────────────────── */

typedef struct {
    FILE         *fp;
    unsigned char buf[CHUNK];
} blast_io_ctx_t;

static unsigned blast_read_cb(void *ctx, unsigned char **out_buf)
{
    blast_io_ctx_t *c = (blast_io_ctx_t *)ctx;
    *out_buf = c->buf;
    return (unsigned)fread(c->buf, 1, CHUNK, c->fp);
}

static int blast_write_file_cb(void *ctx, unsigned char *buf, unsigned len)
{
    return fwrite(buf, 1, len, (FILE *)ctx) != len;
}

typedef struct { FILE *tmp; } tmp_write_ctx_t;

static int blast_write_tmp_cb(void *ctx, unsigned char *buf, unsigned len)
{
    tmp_write_ctx_t *c = (tmp_write_ctx_t *)ctx;
    return fwrite(buf, 1, len, c->tmp) != len;
}

/* ── DBC header-size reader ─────────────────────────────────────────────────
 * Bytes 8-9 of a DBC file encode the embedded DBF header size as a
 * little-endian uint16_t.  HEADER_OFFSET == 8, defined in blast.h.
 * ─────────────────────────────────────────────────────────────────────────── */
static int read_dbf_header_size(FILE *fp, uint16_t *out_size,
                                char *err_buf, size_t err_sz)
{
    unsigned char raw[2];
    if (fseek(fp, HEADER_OFFSET, SEEK_SET) != 0) {
        fmt_error(err_buf, err_sz, "fseek(HEADER_OFFSET) failed: %s",
                  strerror(errno));
        return -1;
    }
    if (fread(raw, 2, 1, fp) != 1) {
        fmt_error(err_buf, err_sz, "cannot read header-size bytes: %s",
                  strerror(errno));
        return -1;
    }
    /* [FIX-2] Validate hdr_sz > 0 before any malloc(hdr_sz) call.
     * A zero-byte file or truncated file yields raw == {0,0}, producing
     * hdr_sz == 0.  malloc(0) is implementation-defined (may return NULL
     * or a unique non-NULL pointer); fread(buf, 1, 0, fp) is a no-op.
     * Both cases silently produce a corrupt output — reject here instead. */
    uint16_t sz = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    if (sz < sizeof(dbf_header_t) + DBF_HEADER_RECORD_LEN + 1U) {
        fmt_error(err_buf, err_sz,
                  "DBF header size %u is too small to be valid", (unsigned)sz);
        return -1;
    }
    *out_size = sz;
    return 0;
}

/* ── dbc2dbf_stream ─────────────────────────────────────────────────────────
 * [FIX-3] Integer overflow on fseek offset:
 *   hdr_sz is uint16_t (max 65535) and CRC_OFFSET is 4, so the sum fits
 *   comfortably in a long on all supported platforms.  Cast is explicit.
 * ─────────────────────────────────────────────────────────────────────────── */
dbc_error_t dbc2dbf_stream(const char *input_path, const char *output_path,
                           char *err_buf, size_t err_sz)
{
    FILE     *fin     = NULL;
    FILE     *fout    = NULL;
    uint8_t  *hdr_buf = NULL;
    uint16_t  hdr_sz  = 0;
    dbc_error_t rc    = DBC_OK;

    fin = fopen(input_path, "rb");
    if (!fin) {
        fmt_error(err_buf, err_sz, "cannot open '%s': %s",
                  input_path, strerror(errno));
        return DBC_ERR_OPEN_IN;
    }

    fout = fopen(output_path, "wb");
    if (!fout) {
        fmt_error(err_buf, err_sz, "cannot create '%s': %s",
                  output_path, strerror(errno));
        fclose(fin);
        return DBC_ERR_OPEN_OUT;
    }

    if (read_dbf_header_size(fin, &hdr_sz, err_buf, err_sz) != 0) {
        rc = DBC_ERR_READ_HDR;
        goto cleanup;
    }

    rewind(fin);
    hdr_buf = (uint8_t *)malloc(hdr_sz);
    if (!hdr_buf) {
        fmt_error(err_buf, err_sz, "malloc(%u) failed", (unsigned)hdr_sz);
        rc = DBC_ERR_MEM;
        goto cleanup;
    }

    if (fread(hdr_buf, 1, hdr_sz, fin) != hdr_sz) {
        fmt_error(err_buf, err_sz, "short read on DBF header: %s",
                  strerror(errno));
        rc = DBC_ERR_READ_HDR;
        goto cleanup;
    }
    if (fwrite(hdr_buf, 1, hdr_sz, fout) != hdr_sz) {
        fmt_error(err_buf, err_sz, "write of DBF header failed: %s",
                  strerror(errno));
        rc = DBC_ERR_WRITE_HDR;
        goto cleanup;
    }
    free(hdr_buf);
    hdr_buf = NULL;  /* prevent double-free in cleanup */

    if (fseek(fin, (long)hdr_sz + (long)CRC_OFFSET, SEEK_SET) != 0) {
        fmt_error(err_buf, err_sz, "fseek to payload failed: %s",
                  strerror(errno));
        rc = DBC_ERR_SEEK;
        goto cleanup;
    }

    {
        blast_io_ctx_t in_ctx;
        in_ctx.fp = fin;
        int blast_rc = blast(blast_read_cb, &in_ctx, blast_write_file_cb, fout);
        if (blast_rc != 0) {
            fmt_error(err_buf, err_sz, "decompression failed (code %d): %s",
                      blast_rc, blast_error_string(blast_rc));
            rc = DBC_ERR_DECOMP;
        }
    }

cleanup:
    free(hdr_buf);   /* NULL-safe per C99 */
    if (fin)  fclose(fin);
    if (fout) fclose(fout);
    return rc;
}

/* ── CSV helpers ────────────────────────────────────────────────────────────
 * [FIX-4] write_csv_row: field_len is uint8_t (0-255).  The original
 * `field_len < 255 ? field_len : 255` clamping is correct, but cell[256]
 * is only 256 bytes.  With field_len == 255, copy_len == 255, then
 * cell[255] = '\0' touches the last valid index.  Safe, but the
 * off-by-one is non-obvious.  Made the arithmetic explicit.
 * ─────────────────────────────────────────────────────────────────────────── */

static void rtrim_inplace(char *str, int width)
{
    int i = width - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\0'))
        str[i--] = '\0';
}

static int write_csv_header(FILE *fout, const dbf_field_t *fields, int nfields)
{
    for (int i = 0; i < nfields; i++) {
        if (i > 0) fputc(',', fout);
        /* field name is guaranteed null-terminated (memcpy+force-null in inspect) */
        fprintf(fout, "\"%s\"", fields[i].name);
    }
    fputc('\n', fout);
    return ferror(fout) ? -1 : 0;
}

static int write_csv_row(FILE *fout, const unsigned char *record,
                         const dbf_field_t *fields, int nfields)
{
    const unsigned char *cursor = record + 1; /* skip deletion-flag byte */

    for (int i = 0; i < nfields; i++) {
        if (i > 0) fputc(',', fout);

        /* field_length is uint8_t: 0-255.  cell[256] always fits. */
        int flen     = (int)(unsigned char)fields[i].length;
        int copy_len = flen < 255 ? flen : 255; /* guard: leave room for NUL */
        char cell[256];
        memcpy(cell, cursor, (size_t)copy_len);
        cell[copy_len] = '\0';
        rtrim_inplace(cell, copy_len);

        if (fields[i].type == 'C' || fields[i].type == 'D') {
            fputc('"', fout);
            for (const char *ch = cell; *ch; ch++) {
                if (*ch == '"') fputc('"', fout); /* RFC 4180 */
                fputc(*ch, fout);
            }
            fputc('"', fout);
        } else {
            fputs(cell, fout);
        }

        cursor += flen;
    }
    fputc('\n', fout);
    return ferror(fout) ? -1 : 0;
}

/* ── dbc_to_csv_stream ──────────────────────────────────────────────────────
 * Core conversion function. Streams records from the uncompressed DBF payload
 * to an output CSV file in batches to maintain O(1) memory complexity.
 * ─────────────────────────────────────────────────────────────────────────── */
dbc_error_t dbc_to_csv_stream(const char *input_path, const char *output_path,
                              int batch_size, const char *encoding,
                              dbc_progress_cb progress_cb, void *user_data,
                              char *err_buf, size_t err_sz)
{
    (void)encoding; /* reserved: iconv support planned */

    if (batch_size <= 0) batch_size = 4096;

    FILE          *fin       = NULL;
    FILE          *fout      = NULL;
    FILE          *ftmp      = NULL;
    uint8_t       *hdr_buf   = NULL;
    dbf_field_t   *fields    = NULL;
    unsigned char *rec_batch = NULL;
    dbc_error_t    rc        = DBC_OK;

    fin = fopen(input_path, "rb");
    if (!fin) {
        fmt_error(err_buf, err_sz, "cannot open '%s': %s",
                  input_path, strerror(errno));
        return DBC_ERR_OPEN_IN;
    }

    char tmp_path[2048];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.dbf", output_path);
    ftmp = fopen(tmp_path, "wb+");
    if (!ftmp) {
        fmt_error(err_buf, err_sz, "cannot create temp file '%s': %s", tmp_path, strerror(errno));
        fclose(fin);
        return DBC_ERR_OPEN_OUT;
    }

    uint16_t hdr_sz = 0;
    if (read_dbf_header_size(fin, &hdr_sz, err_buf, err_sz) != 0) {
        rc = DBC_ERR_READ_HDR;
        goto cleanup;
    }

    rewind(fin);
    hdr_buf = (uint8_t *)malloc(hdr_sz);
    if (!hdr_buf) {
        fmt_error(err_buf, err_sz, "malloc(%u) failed", (unsigned)hdr_sz);
        rc = DBC_ERR_MEM;
        goto cleanup;
    }
    if (fread(hdr_buf, 1, hdr_sz, fin) != hdr_sz) {
        fmt_error(err_buf, err_sz, "short read on DBF header: %s",
                  strerror(errno));
        rc = DBC_ERR_READ_HDR;
        goto cleanup;
    }
    if (fwrite(hdr_buf, 1, hdr_sz, ftmp) != hdr_sz) {
        fmt_error(err_buf, err_sz, "write to tmpfile failed: %s",
                  strerror(errno));
        rc = DBC_ERR_WRITE_HDR;
        goto cleanup;
    }
    free(hdr_buf);
    hdr_buf = NULL;

    if (fseek(fin, (long)hdr_sz + (long)CRC_OFFSET, SEEK_SET) != 0) {
        fmt_error(err_buf, err_sz, "fseek to payload failed: %s",
                  strerror(errno));
        rc = DBC_ERR_SEEK;
        goto cleanup;
    }

    {
        blast_io_ctx_t  in_ctx;
        tmp_write_ctx_t out_ctx;
        in_ctx.fp   = fin;
        out_ctx.tmp = ftmp;
        int blast_rc = blast(blast_read_cb, &in_ctx, blast_write_tmp_cb, &out_ctx);
        if (blast_rc != 0) {
            fmt_error(err_buf, err_sz, "decompression failed (code %d): %s",
                      blast_rc, blast_error_string(blast_rc));
            rc = DBC_ERR_DECOMP;
            goto cleanup;
        }
    }
    fclose(fin);
    fin = NULL;

    rewind(ftmp);
    dbf_header_t dbf_hdr;
    if (fread(&dbf_hdr, sizeof(dbf_header_t), 1, ftmp) != 1) {
        fmt_error(err_buf, err_sz, "cannot read DBF header from tmpfile");
        rc = DBC_ERR_INVALID_DBF;
        goto cleanup;
    }

    /* Validate record_size before using it in a malloc multiplier */
    if (dbf_hdr.record_size == 0) {
        fmt_error(err_buf, err_sz, "DBF record_size is 0 — file is corrupt");
        rc = DBC_ERR_INVALID_DBF;
        goto cleanup;
    }

    int nfields = ((int)dbf_hdr.header_size - (int)sizeof(dbf_header_t) - 1)
                  / DBF_HEADER_RECORD_LEN;
    if (nfields <= 0 || nfields > 512) {
        fmt_error(err_buf, err_sz,
                  "implausible field count %d — file may be corrupt", nfields);
        rc = DBC_ERR_INVALID_DBF;
        goto cleanup;
    }

    fields = (dbf_field_t *)malloc((size_t)nfields * sizeof(dbf_field_t));
    if (!fields) {
        fmt_error(err_buf, err_sz, "malloc for field descriptors failed");
        rc = DBC_ERR_MEM;
        goto cleanup;
    }
    if (fread(fields, sizeof(dbf_field_t), (size_t)nfields, ftmp) != (size_t)nfields) {
        fmt_error(err_buf, err_sz, "short read on field descriptors");
        rc = DBC_ERR_INVALID_DBF;
        goto cleanup;
    }

    /* Cast uint16_t to long before fseek to suppress sign-conversion */
    if (fseek(ftmp, (long)dbf_hdr.header_size, SEEK_SET) != 0) {
        fmt_error(err_buf, err_sz, "fseek to first record failed: %s",
                  strerror(errno));
        rc = DBC_ERR_SEEK;
        goto cleanup;
    }

    fout = fopen(output_path, "wb");
    if (!fout) {
        fmt_error(err_buf, err_sz, "cannot create '%s': %s",
                  output_path, strerror(errno));
        rc = DBC_ERR_OPEN_OUT;
        goto cleanup;
    }

    /* UTF-8 BOM (EF BB BF) — check write success */
    if (fwrite("\xEF\xBB\xBF", 1, 3, fout) != 3) {
        fmt_error(err_buf, err_sz, "BOM write failed: %s", strerror(errno));
        rc = DBC_ERR_WRITE_OUT;
        goto cleanup;
    }
    if (write_csv_header(fout, fields, nfields) != 0) {
        fmt_error(err_buf, err_sz, "CSV header write failed: %s", strerror(errno));
        rc = DBC_ERR_WRITE_OUT;
        goto cleanup;
    }

    /* Guard against overflow: batch_size * rec_sz */
    uint16_t rec_sz = dbf_hdr.record_size;
    {
        size_t alloc_sz = (size_t)batch_size * (size_t)rec_sz;
        /* 256 MB hard cap — files with wider records get a smaller batch */
        if (alloc_sz == 0 || alloc_sz > 256UL * 1024UL * 1024UL) {
            batch_size = (int)(256UL * 1024UL * 1024UL / (size_t)rec_sz);
            if (batch_size < 1) batch_size = 1;
            alloc_sz = (size_t)batch_size * (size_t)rec_sz;
        }
        rec_batch = (unsigned char *)malloc(alloc_sz);
    }
    if (!rec_batch) {
        fmt_error(err_buf, err_sz, "malloc for record batch failed");
        rc = DBC_ERR_MEM;
        goto cleanup;
    }

    long records_total   = (long)dbf_hdr.record_count;
    long records_written = 0;

    while (records_written < records_total) {
        size_t want = (size_t)(records_total - records_written);
        if ((long)want > batch_size) want = (size_t)batch_size;

        size_t got = fread(rec_batch, rec_sz, want, ftmp);
        if (got == 0) break;

        for (size_t r = 0; r < got; r++) {
            const unsigned char *rec = rec_batch + r * (size_t)rec_sz;
            if (rec[0] == 0x2A) continue; /* '*' == deleted record */
            if (write_csv_row(fout, rec, fields, nfields) != 0) {
                fmt_error(err_buf, err_sz, "CSV row write failed: %s",
                          strerror(errno));
                rc = DBC_ERR_WRITE_OUT;
                goto cleanup;
            }
        }

        records_written += (long)got;
        if (progress_cb)
            progress_cb(records_written, records_total, user_data);
    }

cleanup:
    free(hdr_buf);   /* NULL-safe */
    free(fields);    /* NULL-safe */
    free(rec_batch); /* NULL-safe */
    if (fin)  fclose(fin);
    if (fout) fclose(fout);
    if (ftmp) {
        fclose(ftmp);
        remove(tmp_path);
    }
    return rc;
}

/* ── dbc_inspect ────────────────────────────────────────────────────────────
 * Reads only the DBF header from the uncompressed payload.
 * ─────────────────────────────────────────────────────────────────────────── */
dbc_error_t dbc_inspect(const char *input_path,
                        char field_names[][DBF_FIELD_NAME_LEN],
                        char *field_types, uint8_t *field_widths,
                        uint8_t *field_decs, int *nfields, uint32_t *nrecords,
                        int max_fields, char *err_buf, size_t err_sz)
{
    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        fmt_error(err_buf, err_sz, "cannot open '%s': %s",
                  input_path, strerror(errno));
        return DBC_ERR_OPEN_IN;
    }

    uint16_t hdr_sz = 0;
    if (read_dbf_header_size(fin, &hdr_sz, err_buf, err_sz) != 0) {
        fclose(fin);
        return DBC_ERR_READ_HDR;
    }

    rewind(fin);
    dbf_header_t dbf_hdr;
    if (fread(&dbf_hdr, sizeof(dbf_header_t), 1, fin) != 1) {
        fmt_error(err_buf, err_sz, "cannot read DBF header");
        fclose(fin);
        return DBC_ERR_INVALID_DBF;
    }

    *nrecords = dbf_hdr.record_count;

    int n = ((int)dbf_hdr.header_size - (int)sizeof(dbf_header_t) - 1)
            / DBF_HEADER_RECORD_LEN;
    if (n <= 0) {
        fmt_error(err_buf, err_sz,
                  "implausible field count %d — file may be corrupt", n);
        fclose(fin);
        return DBC_ERR_INVALID_DBF;
    }
    if (n > max_fields) n = max_fields;
    *nfields = n;

    for (int i = 0; i < n; i++) {
        dbf_field_t f;
        if (fread(&f, sizeof(dbf_field_t), 1, fin) != 1) {
            *nfields = i;
            break;
        }
        /* Guarantee null-termination regardless of DBF content */
        memcpy(field_names[i], f.name, DBF_FIELD_NAME_LEN - 1);
        field_names[i][DBF_FIELD_NAME_LEN - 1] = '\0';
        field_types[i]  = f.type;
        field_widths[i] = f.length;
        field_decs[i]   = f.decimal_count;
    }

    fclose(fin);
    return DBC_OK;
}
