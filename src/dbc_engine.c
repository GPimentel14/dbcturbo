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

static void fmt_error(char *buf, size_t buf_sz, const char *fmt, ...)
{
    if (!buf || buf_sz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, buf_sz, fmt, ap);
    va_end(ap);
    buf[buf_sz - 1] = '\0';
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
    /* Reject truncated or implausibly small DBF headers before allocation. */
    uint16_t sz = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    if (sz < sizeof(dbf_header_t) + DBF_HEADER_RECORD_LEN + 1U) {
        fmt_error(err_buf, err_sz,
                  "DBF header size %u is too small to be valid", (unsigned)sz);
        return -1;
    }
    *out_size = sz;
    return 0;
}

/* ── dbc2dbf_stream ───────────────────────────────────────────────────────── */
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

/* ── CSV helpers ──────────────────────────────────────────────────────────── */

/* ── Encoding Transcoding Tables ─────────────────────────────────────────── */

typedef enum {
    ENC_CP850 = 0,
    ENC_LATIN1 = 1,
    ENC_UTF8   = 2
} encoding_mode_t;

static const uint16_t cp850_to_unicode[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
    0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
    0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
    0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
    0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
    0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
    0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
    0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0
};

static const uint16_t cp1252_to_unicode[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF
};

static inline void emit_utf8_char(uint32_t cp, FILE *fout)
{
    if (cp <= 0x7F) {
        fputc((int)cp, fout);
    } else if (cp <= 0x7FF) {
        fputc((int)(0xC0 | ((cp >> 6) & 0x1F)), fout);
        fputc((int)(0x80 | (cp & 0x3F)), fout);
    } else {
        fputc((int)(0xE0 | ((cp >> 12) & 0x0F)), fout);
        fputc((int)(0x80 | ((cp >> 6) & 0x3F)), fout);
        fputc((int)(0x80 | (cp & 0x3F)), fout);
    }
}

static inline void emit_encoded_byte(unsigned char b, encoding_mode_t mode, FILE *fout)
{
    if (b <= 0x7F) {
        fputc((int)b, fout);
        return;
    }
    uint32_t cp = (mode == ENC_CP850) ? cp850_to_unicode[b]
                : (mode == ENC_LATIN1) ? cp1252_to_unicode[b]
                : (uint32_t)b;
    emit_utf8_char(cp, fout);
}

static void rtrim_inplace(char *str, int width)
{
    int i = width - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\0'))
        str[i--] = '\0';
}

static int write_csv_header(FILE *fout, const dbf_field_t *fields,
                            const int *selected_fields, int nselected_fields,
                            encoding_mode_t enc_mode)
{
    for (int j = 0; j < nselected_fields; j++) {
        int i = selected_fields ? selected_fields[j] : j;
        if (j > 0) fputc(',', fout);
        fputc('"', fout);
        for (const unsigned char *p = (const unsigned char *)fields[i].name; *p; p++) {
            if (*p == '"') {
                fputc('"', fout);
                fputc('"', fout);
            } else {
                emit_encoded_byte(*p, enc_mode, fout);
            }
        }
        fputc('"', fout);
    }
    fputc('\n', fout);
    return ferror(fout) ? -1 : 0;
}

static int write_csv_row(FILE *fout, const unsigned char *record,
                         const dbf_field_t *fields, const size_t *field_offsets,
                         const int *selected_fields, int nselected_fields,
                         encoding_mode_t enc_mode)
{
    for (int j = 0; j < nselected_fields; j++) {
        int i = selected_fields ? selected_fields[j] : j;
        const unsigned char *cursor = record + 1 + field_offsets[i];
        if (j > 0) fputc(',', fout);

        /* field_length is uint8_t: 0-255.  cell[256] always fits. */
        int flen     = (int)(unsigned char)fields[i].length;
        int copy_len = flen < 255 ? flen : 255; /* guard: leave room for NUL */
        char cell[256];
        memcpy(cell, cursor, (size_t)copy_len);
        cell[copy_len] = '\0';
        rtrim_inplace(cell, copy_len);

        if (fields[i].type == 'C' || fields[i].type == 'D') {
            fputc('"', fout);
            for (const unsigned char *p = (const unsigned char *)cell; *p; p++) {
                if (*p == '"') {
                    fputc('"', fout);
                    fputc('"', fout); /* RFC 4180 quote escaping */
                } else {
                    emit_encoded_byte(*p, enc_mode, fout);
                }
            }
            fputc('"', fout);
        } else {
            for (const unsigned char *p = (const unsigned char *)cell; *p; p++) {
                emit_encoded_byte(*p, enc_mode, fout);
            }
        }
    }
    fputc('\n', fout);
    return ferror(fout) ? -1 : 0;
}

/* ── dbc_to_csv_stream ──────────────────────────────────────────────────────
 * Core conversion function. Streams records from the uncompressed DBF payload
 * to an output CSV file in batches to maintain O(1) memory complexity.
 * ─────────────────────────────────────────────────────────────────────────── */
dbc_error_t dbc_to_csv_stream(const char *input_path, const char *output_path,
                              const char *tmp_path,
                              int batch_size, const char *encoding,
                              const int *selected_fields, int nselected_fields,
                              dbc_progress_cb progress_cb, void *user_data,
                              char *err_buf, size_t err_sz)
{
    encoding_mode_t enc_mode = ENC_CP850;
    if (encoding && encoding[0]) {
        if (strcasecmp(encoding, "latin1") == 0 ||
            strcasecmp(encoding, "ISO-8859-1") == 0 ||
            strcasecmp(encoding, "CP1252") == 0 ||
            strcasecmp(encoding, "windows-1252") == 0) {
            enc_mode = ENC_LATIN1;
        } else if (strcasecmp(encoding, "UTF-8") == 0 ||
                   strcasecmp(encoding, "ASCII") == 0) {
            enc_mode = ENC_UTF8;
        }
    }

    if (batch_size <= 0) batch_size = 4096;

    FILE          *fin       = NULL;
    FILE          *fout      = NULL;
    FILE          *ftmp      = NULL;
    uint8_t       *hdr_buf   = NULL;
    dbf_field_t   *fields    = NULL;
    size_t        *field_offsets = NULL;
    unsigned char *rec_batch = NULL;
    dbc_error_t    rc        = DBC_OK;

    fin = fopen(input_path, "rb");
    if (!fin) {
        fmt_error(err_buf, err_sz, "cannot open '%s': %s",
                  input_path, strerror(errno));
        return DBC_ERR_OPEN_IN;
    }

    /* Use caller-supplied tmp_path when available (e.g. R_tmpnam from r_bridge.c).
     * If NULL, fall back to output_path + ".tmp.dbf" for standalone C callers. */
    char tmp_path_buf[2048];
    const char *effective_tmp = tmp_path;
    if (!effective_tmp || effective_tmp[0] == '\0') {
        snprintf(tmp_path_buf, sizeof(tmp_path_buf), "%s.tmp.dbf", output_path);
        effective_tmp = tmp_path_buf;
    }
    ftmp = fopen(effective_tmp, "wb+");
    if (!ftmp) {
        fmt_error(err_buf, err_sz, "cannot create temp file '%s': %s",
                  effective_tmp, strerror(errno));
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

    if (selected_fields) {
        if (nselected_fields < 1) {
            fmt_error(err_buf, err_sz, "at least one field must be selected");
            rc = DBC_ERR_INVALID_DBF;
            goto cleanup;
        }
        for (int i = 0; i < nselected_fields; i++) {
            if (selected_fields[i] < 0 || selected_fields[i] >= nfields) {
                fmt_error(err_buf, err_sz, "selected field index is out of bounds");
                rc = DBC_ERR_INVALID_DBF;
                goto cleanup;
            }
        }
    } else {
        nselected_fields = nfields;
    }

    field_offsets = (size_t *)malloc((size_t)nfields * sizeof(size_t));
    if (!field_offsets) {
        fmt_error(err_buf, err_sz, "malloc for field offsets failed");
        rc = DBC_ERR_MEM;
        goto cleanup;
    }
    field_offsets[0] = 0;
    for (int i = 1; i < nfields; i++)
        field_offsets[i] = field_offsets[i - 1] + (size_t)fields[i - 1].length;

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
    if (write_csv_header(fout, fields, selected_fields, nselected_fields, enc_mode) != 0) {
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

    int64_t records_total   = (int64_t)dbf_hdr.record_count;
    int64_t records_written = 0;

    while (records_written < records_total) {
        size_t want = (size_t)(records_total - records_written);
        if ((int64_t)want > (int64_t)batch_size) want = (size_t)batch_size;

        size_t got = fread(rec_batch, rec_sz, want, ftmp);
        if (got == 0) break;

        for (size_t r = 0; r < got; r++) {
            const unsigned char *rec = rec_batch + r * (size_t)rec_sz;
            if (rec[0] == 0x2A) continue; /* '*' == deleted record */
            if (write_csv_row(fout, rec, fields, field_offsets,
                              selected_fields, nselected_fields, enc_mode) != 0) {
                fmt_error(err_buf, err_sz, "CSV row write failed: %s",
                          strerror(errno));
                rc = DBC_ERR_WRITE_OUT;
                goto cleanup;
            }
        }

        records_written += (int64_t)got;
        if (progress_cb)
            progress_cb(records_written, records_total, user_data);
    }

cleanup:
    free(hdr_buf);   /* NULL-safe */
    free(fields);    /* NULL-safe */
    free(field_offsets);
    free(rec_batch); /* NULL-safe */
    if (fin)  fclose(fin);
    if (fout) fclose(fout);
    if (ftmp) {
        fclose(ftmp);
        remove(effective_tmp);
    }
    return rc;
}

/* ── dbc_inspect ────────────────────────────────────────────────────────────
 * Reads the DBF header metadata WITHOUT decompressing the payload.
 *
 * This works because the DBC format stores the full DBF header verbatim at
 * the beginning of the file, before the compressed payload:
 *
 *   [Bytes 0-7]   DBF header (version, date, record count, header size)
 *   [Bytes 8-9]   uint16_t LE: total DBF header size  (read by read_dbf_header_size)
 *   [Bytes 10-N]  Field descriptors (32 bytes each) + 0x0D terminator
 *   [N+1..N+4]   CRC32 (ignored during decompression)
 *   [N+5..EOF]   PKWare Implode compressed payload
 *
 * So fread() on the raw DBC file directly gives us the header and field list.
 * No decompression is needed; dbc_to_csv_stream does the full decode.
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
