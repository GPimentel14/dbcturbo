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

typedef struct {
    unsigned char *record;
    uint16_t record_size;
    size_t filled;
    uint32_t expected_records;
    uint32_t records_seen;
    dbc_record_cb record_cb;
    void *user_data;
    unsigned trailing_bytes;
    int callback_failed;
    int integrity_failed;
    int early_stop;
} record_stream_ctx_t;

static int blast_write_record_cb(void *ctx, unsigned char *buf, unsigned len)
{
    record_stream_ctx_t *c = (record_stream_ctx_t *)ctx;
    size_t pos = 0;

    while (pos < (size_t)len) {
        if (c->records_seen >= c->expected_records) {
            for (; pos < (size_t)len; pos++) {
                if (buf[pos] == 0x1A && c->trailing_bytes == 0)
                    c->trailing_bytes++;
                else
                    c->integrity_failed = 1;
            }
            break;
        }

        /* Fast zero-copy path when a full record lies contiguously in buf. */
        if (c->filled == 0 && (size_t)len - pos >= (size_t)c->record_size) {
            const unsigned char *rec = buf + pos;
            if (rec[0] != 0x20 && rec[0] != 0x2A) {
                c->integrity_failed = 1;
                return 1;
            }
            int cb_rc = c->record_cb(rec, (int64_t)c->records_seen,
                                     c->user_data);
            if (cb_rc != 0) {
                if (cb_rc == DBC_CB_STOP) c->early_stop = 1;
                else c->callback_failed = 1;
                return 1;
            }
            c->records_seen++;
            pos += (size_t)c->record_size;
            continue;
        }

        size_t needed = (size_t)c->record_size - c->filled;
        size_t available = (size_t)len - pos;
        size_t take = available < needed ? available : needed;
        memcpy(c->record + c->filled, buf + pos, take);
        c->filled += take;
        pos += take;

        if (c->filled == c->record_size) {
            if (c->record[0] != 0x20 && c->record[0] != 0x2A) {
                c->integrity_failed = 1;
                return 1;
            }
            int cb_rc = c->record_cb(c->record, (int64_t)c->records_seen,
                                     c->user_data);
            if (cb_rc != 0) {
                if (cb_rc == DBC_CB_STOP) c->early_stop = 1;
                else c->callback_failed = 1;
                return 1;
            }
            c->records_seen++;
            c->filled = 0;
        }
    }
    return 0;
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

/* ── dbc_read_records ───────────────────────────────────────────────────────
 * Streams decompressed DBF records directly to a callback. This is the common
 * primitive used by the native R reader; it never creates a DBF or CSV file.
 * ───────────────────────────────────────────────────────────────────────── */
dbc_error_t dbc_read_records(const char *input_path, uint16_t record_size,
                             uint32_t expected_records, dbc_record_cb record_cb,
                             void *user_data, char *err_buf, size_t err_sz)
{
    if (record_size == 0 || !record_cb) {
        fmt_error(err_buf, err_sz, "invalid record stream configuration");
        return DBC_ERR_INVALID_DBF;
    }

    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        fmt_error(err_buf, err_sz, "cannot open '%s': %s",
                  input_path, strerror(errno));
        return DBC_ERR_OPEN_IN;
    }

    uint16_t hdr_sz = 0;
    dbc_error_t rc = DBC_OK;
    unsigned char *record = NULL;

    if (read_dbf_header_size(fin, &hdr_sz, err_buf, err_sz) != 0) {
        rc = DBC_ERR_READ_HDR;
        goto cleanup;
    }
    if (fseek(fin, (long)hdr_sz + (long)CRC_OFFSET, SEEK_SET) != 0) {
        fmt_error(err_buf, err_sz, "fseek to payload failed: %s", strerror(errno));
        rc = DBC_ERR_SEEK;
        goto cleanup;
    }

    record = (unsigned char *)malloc(record_size);
    if (!record) {
        fmt_error(err_buf, err_sz, "malloc for record buffer failed");
        rc = DBC_ERR_MEM;
        goto cleanup;
    }

    record_stream_ctx_t out_ctx;
    out_ctx.record = record;
    out_ctx.record_size = record_size;
    out_ctx.filled = 0;
    out_ctx.expected_records = expected_records;
    out_ctx.records_seen = 0;
    out_ctx.record_cb = record_cb;
    out_ctx.user_data = user_data;
    out_ctx.trailing_bytes = 0;
    out_ctx.callback_failed = 0;
    out_ctx.integrity_failed = 0;
    out_ctx.early_stop = 0;

    blast_io_ctx_t in_ctx;
    in_ctx.fp = fin;
    int blast_rc = blast(blast_read_cb, &in_ctx, blast_write_record_cb, &out_ctx);
    if (out_ctx.early_stop) {
        rc = DBC_OK;
        goto cleanup;
    }
    if (blast_rc != 0) {
        if (out_ctx.integrity_failed) {
            fmt_error(err_buf, err_sz, "invalid DBF record marker");
            rc = DBC_ERR_INVALID_DBF;
        } else if (out_ctx.callback_failed) {
            fmt_error(err_buf, err_sz, "record callback failed");
            rc = DBC_ERR_DECOMP;
        } else {
            fmt_error(err_buf, err_sz, "decompression failed (code %d): %s",
                      blast_rc, blast_error_string(blast_rc));
            rc = DBC_ERR_DECOMP;
        }
        goto cleanup;
    }
    if (out_ctx.records_seen != expected_records || out_ctx.filled != 0) {
        fmt_error(err_buf, err_sz,
                  "decompressed %u complete records; expected %u",
                  (unsigned)out_ctx.records_seen, (unsigned)expected_records);
        rc = DBC_ERR_INVALID_DBF;
    } else if (out_ctx.integrity_failed) {
        fmt_error(err_buf, err_sz, "unexpected data after the final DBF record");
        rc = DBC_ERR_INVALID_DBF;
    }

cleanup:
    free(record);
    fclose(fin);
    return rc;
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

/* ── Buffered CSV output ─────────────────────────────────────────────────────
 * Writing one fputc() per byte dominated CSV conversion time. Every byte now
 * lands in a large in-memory buffer flushed with a single fwrite() when full,
 * which removes the per-byte stream-locking overhead.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    FILE          *fp;
    unsigned char *buf;
    size_t         cap;
    size_t         len;
    int            failed;
} csv_out_t;

static int csv_out_flush(csv_out_t *o)
{
    if (o->len) {
        if (fwrite(o->buf, 1, o->len, o->fp) != o->len)
            o->failed = 1;
        o->len = 0;
    }
    return o->failed ? -1 : 0;
}

static inline void csv_out_putc(csv_out_t *o, unsigned char c)
{
    if (o->len >= o->cap) csv_out_flush(o);
    o->buf[o->len++] = c;
}

/* Bulk write: one memcpy for the whole run instead of one call per byte.
 * Runs larger than the buffer are written straight through. */
static inline void csv_out_write(csv_out_t *o, const unsigned char *src, size_t n)
{
    if (n == 0) return;
    if (o->len + n > o->cap) {
        if (n >= o->cap) {
            csv_out_flush(o);
            if (fwrite(src, 1, n, o->fp) != n) o->failed = 1;
            return;
        }
        csv_out_flush(o);
    }
    memcpy(o->buf + o->len, src, n);
    o->len += n;
}

static inline void emit_utf8_char(csv_out_t *o, uint32_t cp)
{
    if (cp <= 0x7F) {
        csv_out_putc(o, (unsigned char)cp);
    } else if (cp <= 0x7FF) {
        csv_out_putc(o, (unsigned char)(0xC0 | ((cp >> 6) & 0x1F)));
        csv_out_putc(o, (unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        csv_out_putc(o, (unsigned char)(0xE0 | ((cp >> 12) & 0x0F)));
        csv_out_putc(o, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        csv_out_putc(o, (unsigned char)(0x80 | (cp & 0x3F)));
    }
}

static inline void emit_encoded_byte(csv_out_t *o, unsigned char b, dbc_encoding_t mode)
{
    if (b <= 0x7F) {
        csv_out_putc(o, b);
        return;
    }
    uint32_t cp = (mode == DBC_ENC_CP850) ? cp850_to_unicode[b]
                : (mode == DBC_ENC_LATIN1) ? cp1252_to_unicode[b]
                : (uint32_t)b;
    emit_utf8_char(o, cp);
}

/* Transcode a raw DBF cell straight into a flat buffer. Used by the native
 * R reader so encoding conversion happens once, in C, before interning. */
size_t dbc_transcode_to_utf8(const unsigned char *in, int len, char *out,
                             dbc_encoding_t enc)
{
    if (enc == DBC_ENC_UTF8) {
        if (len > 0) memcpy(out, in, (size_t)len);
        return (size_t)len;
    }
    size_t n = 0;
    for (int i = 0; i < len; i++) {
        unsigned char b = in[i];
        if (b <= 0x7F) {
            out[n++] = (char)b;
            continue;
        }
        uint32_t cp = (enc == DBC_ENC_LATIN1) ? cp1252_to_unicode[b]
                                              : cp850_to_unicode[b];
        if (cp <= 0x7F) {
            out[n++] = (char)cp;
        } else if (cp <= 0x7FF) {
            out[n++] = (char)(0xC0 | ((cp >> 6) & 0x1F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return n;
}

dbc_encoding_t dbc_encoding_from_name(const char *name)
{
    if (!name || !name[0]) return DBC_ENC_LATIN1;
    if (strcasecmp(name, "CP850") == 0 ||
        strcasecmp(name, "IBM850") == 0 ||
        strcasecmp(name, "850") == 0)
        return DBC_ENC_CP850;
    if (strcasecmp(name, "UTF-8") == 0 ||
        strcasecmp(name, "UTF8") == 0 ||
        strcasecmp(name, "ASCII") == 0)
        return DBC_ENC_UTF8;
    return DBC_ENC_LATIN1;
}

static void rtrim_inplace(char *str, int width)
{
    int i = width - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\0'))
        str[i--] = '\0';
}

static int write_csv_header(csv_out_t *o, const dbf_field_t *fields,
                            const int *selected_fields, int nselected_fields,
                            dbc_encoding_t enc_mode)
{
    for (int j = 0; j < nselected_fields; j++) {
        int i = selected_fields ? selected_fields[j] : j;
        if (j > 0) csv_out_putc(o, ',');
        csv_out_putc(o, '"');
        for (const unsigned char *p = (const unsigned char *)fields[i].name; *p; p++) {
            if (*p == '"') {
                csv_out_putc(o, '"');
                csv_out_putc(o, '"');
            } else {
                emit_encoded_byte(o, *p, enc_mode);
            }
        }
        csv_out_putc(o, '"');
    }
    csv_out_putc(o, '\n');
    return csv_out_flush(o);
}

static int write_csv_row(csv_out_t *o, const unsigned char *record,
                         const dbf_field_t *fields, const size_t *field_offsets,
                         const int *selected_fields, int nselected_fields,
                         dbc_encoding_t enc_mode)
{
    for (int j = 0; j < nselected_fields; j++) {
        int i = selected_fields ? selected_fields[j] : j;
        const unsigned char *cursor = record + 1 + field_offsets[i];
        if (j > 0) csv_out_putc(o, ',');

        /* field_length is uint8_t: 0-255.  cell[256] always fits. */
        int flen     = (int)(unsigned char)fields[i].length;
        int copy_len = flen < 255 ? flen : 255; /* guard: leave room for NUL */
        char cell[256];
        memcpy(cell, cursor, (size_t)copy_len);
        cell[copy_len] = '\0';
        rtrim_inplace(cell, copy_len);

        /* Fast path: a cell with no embedded quote (quoted fields) and no
         * byte needing conversion is copied in a single memcpy. Anything else
         * falls through to the byte loop below, which keeps identical output. */
        int quoted  = (fields[i].type == 'C' || fields[i].type == 'D');
        size_t len  = strlen(cell);
        int plain   = 1;
        for (size_t k = 0; k < len; k++) {
            unsigned char ch = (unsigned char)cell[k];
            if (quoted && ch == '"') { plain = 0; break; }
            if (ch > 0x7F && enc_mode != DBC_ENC_UTF8) { plain = 0; break; }
        }

        if (quoted) csv_out_putc(o, '"');
        if (plain && len >= 32) {
            csv_out_write(o, (const unsigned char *)cell, len);
        } else {
            for (const unsigned char *p = (const unsigned char *)cell; *p; p++) {
                if (quoted && *p == '"') {
                    csv_out_putc(o, '"');
                    csv_out_putc(o, '"'); /* RFC 4180 quote escaping */
                } else {
                    emit_encoded_byte(o, *p, enc_mode);
                }
            }
        }
        if (quoted) csv_out_putc(o, '"');
    }
    csv_out_putc(o, '\n');
    return o->failed ? -1 : 0;
}

/* ── dbc_to_csv_stream ──────────────────────────────────────────────────────
 * Core conversion function. Streams records from the uncompressed DBF payload
 * to an output CSV file in batches to maintain O(1) memory complexity.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    csv_out_t *out;
    const dbf_field_t *fields;
    const size_t *field_offsets;
    const int *selected_fields;
    int nselected_fields;
    dbc_encoding_t enc_mode;
    int batch_size;
    int64_t records_total;
    dbc_progress_cb progress_cb;
    void *user_data;
    int write_failed;
} csv_stream_ctx_t;

static int csv_record_cb(const unsigned char *record, int64_t index, void *user_data)
{
    csv_stream_ctx_t *ctx = (csv_stream_ctx_t *)user_data;
    if (record[0] != 0x2A &&
        write_csv_row(ctx->out, record, ctx->fields, ctx->field_offsets,
                      ctx->selected_fields, ctx->nselected_fields,
                      ctx->enc_mode) != 0) {
        ctx->write_failed = 1;
        return 1;
    }
    if (ctx->progress_cb &&
        (((index + 1) % ctx->batch_size) == 0 || index + 1 == ctx->records_total))
        ctx->progress_cb(index + 1, ctx->records_total, ctx->user_data);
    return 0;
}

/* Direct DBC -> CSV writer: decompresses records straight to the callback,
 * avoiding any full-sized DBF temporary file. */
dbc_error_t dbc_to_csv_stream(const char *input_path, const char *output_path,
                              const char *tmp_path,
                              int batch_size, const char *encoding,
                              const int *selected_fields, int nselected_fields,
                              dbc_progress_cb progress_cb, void *user_data,
                              char *err_buf, size_t err_sz)
{
    (void)tmp_path;
    dbc_encoding_t enc_mode = dbc_encoding_from_name(encoding);
    if (batch_size <= 0) batch_size = 4096;

    char field_names[512][DBF_FIELD_NAME_LEN];
    char field_types[512];
    uint8_t field_widths[512], field_decs[512];
    int nfields = 0;
    uint32_t nrecords = 0;
    dbc_error_t rc = dbc_inspect(input_path, field_names, field_types,
                                 field_widths, field_decs, &nfields, &nrecords,
                                 512, err_buf, err_sz);
    if (rc != DBC_OK) return rc;

    if (selected_fields) {
        if (nselected_fields < 1) {
            fmt_error(err_buf, err_sz, "at least one field must be selected");
            return DBC_ERR_INVALID_DBF;
        }
        for (int i = 0; i < nselected_fields; i++) {
            if (selected_fields[i] < 0 || selected_fields[i] >= nfields) {
                fmt_error(err_buf, err_sz, "selected field index is out of bounds");
                return DBC_ERR_INVALID_DBF;
            }
        }
    } else {
        nselected_fields = nfields;
    }

    dbf_field_t *fields = (dbf_field_t *)calloc((size_t)nfields, sizeof(dbf_field_t));
    size_t *field_offsets = (size_t *)malloc((size_t)nfields * sizeof(size_t));
    if (!fields || !field_offsets) {
        free(fields);
        free(field_offsets);
        fmt_error(err_buf, err_sz, "malloc for DBF metadata failed");
        return DBC_ERR_MEM;
    }

    size_t data_width = 0;
    for (int i = 0; i < nfields; i++) {
        memcpy(fields[i].name, field_names[i], DBF_FIELD_NAME_LEN - 1);
        fields[i].type = field_types[i];
        fields[i].length = field_widths[i];
        fields[i].decimal_count = field_decs[i];
        field_offsets[i] = data_width;
        data_width += (size_t)field_widths[i];
    }
    if (data_width + 1 > UINT16_MAX) {
        free(fields);
        free(field_offsets);
        fmt_error(err_buf, err_sz, "DBF record width is too large");
        return DBC_ERR_INVALID_DBF;
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        free(fields);
        free(field_offsets);
        fmt_error(err_buf, err_sz, "cannot create '%s': %s", output_path, strerror(errno));
        return DBC_ERR_OPEN_OUT;
    }

    /* Large output buffer: one fwrite() per ~256 KB replaces per-byte fputc. */
    const size_t out_cap = 256UL * 1024UL;
    unsigned char *out_buf = (unsigned char *)malloc(out_cap);
    if (!out_buf) {
        fclose(fout);
        free(fields);
        free(field_offsets);
        fmt_error(err_buf, err_sz, "malloc for CSV buffer failed");
        return DBC_ERR_MEM;
    }
    csv_out_t out;
    out.fp = fout;
    out.buf = out_buf;
    out.cap = out_cap;
    out.len = 0;
    out.failed = 0;

    /* UTF-8 BOM (EF BB BF) */
    csv_out_putc(&out, 0xEF);
    csv_out_putc(&out, 0xBB);
    csv_out_putc(&out, 0xBF);
    if (write_csv_header(&out, fields, selected_fields, nselected_fields, enc_mode) != 0) {
        fmt_error(err_buf, err_sz, "CSV header write failed: %s", strerror(errno));
        fclose(fout);
        free(out_buf);
        free(fields);
        free(field_offsets);
        return DBC_ERR_WRITE_OUT;
    }

    csv_stream_ctx_t ctx;
    ctx.out = &out;
    ctx.fields = fields;
    ctx.field_offsets = field_offsets;
    ctx.selected_fields = selected_fields;
    ctx.nselected_fields = nselected_fields;
    ctx.enc_mode = enc_mode;
    ctx.batch_size = batch_size;
    ctx.records_total = (int64_t)nrecords;
    ctx.progress_cb = progress_cb;
    ctx.user_data = user_data;
    ctx.write_failed = 0;

    rc = dbc_read_records(input_path, (uint16_t)(data_width + 1), nrecords,
                          csv_record_cb, &ctx, err_buf, err_sz);
    if (rc == DBC_ERR_DECOMP && ctx.write_failed) {
        fmt_error(err_buf, err_sz, "CSV row write failed: %s", strerror(errno));
        rc = DBC_ERR_WRITE_OUT;
    }
    if (csv_out_flush(&out) != 0 && rc == DBC_OK) {
        fmt_error(err_buf, err_sz, "CSV buffer flush failed: %s", strerror(errno));
        rc = DBC_ERR_WRITE_OUT;
    }
    if (fclose(fout) != 0 && rc == DBC_OK) {
        fmt_error(err_buf, err_sz, "CSV close failed: %s", strerror(errno));
        rc = DBC_ERR_WRITE_OUT;
    }
    free(out_buf);
    free(fields);
    free(field_offsets);
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
    if (((size_t)hdr_sz - sizeof(dbf_header_t) - 1U) %
        DBF_HEADER_RECORD_LEN != 0) {
        fmt_error(err_buf, err_sz,
                  "DBF header size %u is not aligned to field descriptors",
                  (unsigned)hdr_sz);
        fclose(fin);
        return DBC_ERR_INVALID_DBF;
    }
    if (fseek(fin, 0, SEEK_END) != 0) {
        fmt_error(err_buf, err_sz, "cannot determine DBC file size");
        fclose(fin);
        return DBC_ERR_SEEK;
    }
    long file_size = ftell(fin);
    if (file_size < 0 || file_size < (long)hdr_sz + CRC_OFFSET + 2L) {
        fmt_error(err_buf, err_sz, "DBC file is truncated before its payload");
        fclose(fin);
        return DBC_ERR_INVALID_DBF;
    }

    rewind(fin);
    dbf_header_t dbf_hdr;
    if (fread(&dbf_hdr, sizeof(dbf_header_t), 1, fin) != 1) {
        fmt_error(err_buf, err_sz, "cannot read DBF header");
        fclose(fin);
        return DBC_ERR_INVALID_DBF;
    }

    if (dbf_hdr.header_size != hdr_sz || dbf_hdr.record_size == 0) {
        fmt_error(err_buf, err_sz, "inconsistent DBF header dimensions");
        fclose(fin);
        return DBC_ERR_INVALID_DBF;
    }
    *nrecords = dbf_hdr.record_count;

    int n = ((int)hdr_sz - (int)sizeof(dbf_header_t) - 1)
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
