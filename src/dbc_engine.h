/* dbc_engine.h - Public C interface for the dbcturbo streaming engine */

#ifndef DBC_ENGINE_H
#define DBC_ENGINE_H

#include <stdint.h>
#include <stdio.h>

#define DBF_FIELD_NAME_LEN   11
#define DBF_HEADER_RECORD_LEN 32
#define DBF_TERMINATOR       0x0D

typedef struct {
    char     name[DBF_FIELD_NAME_LEN];
    char     type;
    uint32_t reserved1;
    uint8_t  length;
    uint8_t  decimal_count;
    uint8_t  reserved2[14];
} dbf_field_t;

typedef struct {
    uint8_t  version;
    uint8_t  last_update[3];
    uint32_t record_count;
    uint16_t header_size;
    uint16_t record_size;
    uint8_t  reserved[20];
} dbf_header_t;

typedef void (*dbc_progress_cb)(long records_done, long records_total, void *user_data);

typedef enum {
    DBC_OK              =  0,
    DBC_ERR_OPEN_IN     = -1,
    DBC_ERR_OPEN_OUT    = -2,
    DBC_ERR_SEEK        = -3,
    DBC_ERR_READ_HDR    = -4,
    DBC_ERR_MEM         = -5,
    DBC_ERR_WRITE_HDR   = -6,
    DBC_ERR_DECOMP      = -7,
    DBC_ERR_WRITE_OUT   = -8,
    DBC_ERR_INVALID_DBF = -9,
} dbc_error_t;

#ifdef __cplusplus
extern "C" {
#endif

dbc_error_t dbc2dbf_stream(const char *input_path, const char *output_path,
                           char *error_buf, size_t error_buf_sz);

dbc_error_t dbc_to_csv_stream(const char *input_path, const char *output_path,
                              int batch_size, const char *encoding,
                              dbc_progress_cb progress_cb, void *user_data,
                              char *error_buf, size_t error_buf_sz);

dbc_error_t dbc_inspect(const char *input_path,
                        char field_names[][DBF_FIELD_NAME_LEN],
                        char *field_types, uint8_t *field_widths,
                        uint8_t *field_decs, int *nfields, uint32_t *nrecords,
                        int max_fields, char *error_buf, size_t error_buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* DBC_ENGINE_H */
