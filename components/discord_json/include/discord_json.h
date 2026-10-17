#ifndef DISCORD_JSON_H
#define DISCORD_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DC_JSON_OK = 0,
    DC_JSON_ERR_NOMEM = -1,
    DC_JSON_ERR_TYPE = -2,
    DC_JSON_ERR_DEPTH = -3,
    DC_JSON_ERR_ESCAPE = -4,
    DC_JSON_ERR_NUMBER = -5
} DcJsonStatus;

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    depth;
    bool   need_comma;
    bool   after_key;
    bool   overflowed;
} DcJson;

void dc_json_init(DcJson *w, char *buf, size_t cap);
DcJsonStatus dc_json_obj_begin(DcJson *w);
DcJsonStatus dc_json_obj_end(DcJson *w);
DcJsonStatus dc_json_arr_begin(DcJson *w);
DcJsonStatus dc_json_arr_end(DcJson *w);
DcJsonStatus dc_json_key(DcJson *w, const char *key);
DcJsonStatus dc_json_str(DcJson *w, const char *key, const char *value);
DcJsonStatus dc_json_strn(DcJson *w, const char *key, const char *value, size_t len);
DcJsonStatus dc_json_arr_str(DcJson *w, const char *value);
DcJsonStatus dc_json_arr_i64(DcJson *w, int64_t value);
DcJsonStatus dc_json_i64(DcJson *w, const char *key, int64_t value);
DcJsonStatus dc_json_u64(DcJson *w, const char *key, uint64_t value);
DcJsonStatus dc_json_bool(DcJson *w, const char *key, bool value);
DcJsonStatus dc_json_null(DcJson *w, const char *key);

size_t dc_json_len(const DcJson *w);
const char *dc_json_cstr(const DcJson *w);
bool dc_json_valid(const DcJson *w);
size_t dc_json_escape_len(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif