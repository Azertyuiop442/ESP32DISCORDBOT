#ifndef DISCORD_ETF_H
#define DISCORD_ETF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wire contract: standard Erlang external term format, the plain
 * term_to_binary encoding the official Discord reference
 * implementation discord/erlpack produces. Per-tag payload shapes:
 *   97  SMALL_INTEGER_EXT  tag + 1 unsigned value byte (0-255)
 *   98  INTEGER_EXT        tag + 4 bytes, signed big-endian
 *   99  FLOAT_EXT          tag + legacy 31-byte ASCII float
 *   100 ATOM_EXT           tag + 2-byte big-endian length (max 255) + text
 *   101 REFERENCE_EXT      not decoded
 *   104 SMALL_TUPLE_EXT    tag + 1-byte arity + elements
 *   105 LARGE_TUPLE_EXT    tag + 4-byte big-endian arity + elements
 *   106 NIL_EXT            tag, no payload
 *   107 STRING_EXT         tag + 2-byte big-endian length + bytes
 *   108 LIST_EXT           tag + 4-byte big-endian count + elements + NIL tail
 *   109 BINARY_EXT         tag + 4-byte big-endian length + bytes
 *   115 NEW_FLOAT_EXT      tag + 8-byte IEEE big-endian double
 *   116 SMALL_MAP_EXT      tag + 1-byte pair count + pairs
 *   117 MAP_EXT            tag + 4-byte big-endian pair count + pairs
 *   118 SMALL_ATOM_UTF8_EXT tag + 1-byte length + text
 *   119 ATOM_UTF8_EXT      tag + 2-byte big-endian length + text
 * Atoms (100, 118, 119) surface as STRING fields because gateway
 * atoms such as true and false are compared as strings. Floats
 * (99, 115) have no field kind: their bytes are consumed so the
 * stream stays in sync and a NIL field is returned, since gateway
 * payloads the framework reads contain no floats.
 */
typedef enum {
    DISCORD_ETF_SMALL_INTEGER = 97,
    DISCORD_ETF_INTEGER = 98,
    DISCORD_ETF_FLOAT = 99,
    DISCORD_ETF_ATOM = 100,
    DISCORD_ETF_REFERENCE = 101,
    DISCORD_ETF_SMALL_TUPLE = 104,
    DISCORD_ETF_LARGE_TUPLE = 105,
    DISCORD_ETF_NIL = 106,
    DISCORD_ETF_STRING = 107,
    DISCORD_ETF_LIST = 108,
    DISCORD_ETF_BINARY = 109,
    DISCORD_ETF_SMALL_BIG = 110,
    DISCORD_ETF_LARGE_BIG = 111,
    DISCORD_ETF_NEW_FLOAT = 115,
    DISCORD_ETF_SMALL_MAP = 116,
    DISCORD_ETF_MAP = 117,
    DISCORD_ETF_SMALL_ATOM_UTF8 = 118,
    DISCORD_ETF_ATOM_UTF8 = 119
} DiscordEtfTag;

typedef enum {
    DISCORD_ETF_OK = 0,
    DISCORD_ETF_ERR_TRUNCATED = -1,
    DISCORD_ETF_ERR_BAD_TAG = -2,
    DISCORD_ETF_ERR_DEPTH = -3,
    DISCORD_ETF_ERR_CONTAINER = -4
} DiscordEtfStatus;

typedef enum {
    DISCORD_ETF_FIELD_END = 0,
    DISCORD_ETF_FIELD_INT = 1,
    DISCORD_ETF_FIELD_STRING = 2,
    DISCORD_ETF_FIELD_BINARY = 3,
    DISCORD_ETF_FIELD_NIL = 4
} DiscordEtfFieldKind;

typedef struct {
    DiscordEtfFieldKind kind;
    DiscordSlice slice;
    int64_t integer;
} DiscordEtfField;

#define DISCORD_ETF_MAX_DEPTH 32

typedef struct {
    const uint8_t *base;
    size_t size;
    size_t pos;
    int depth;
    int max_depth;
    int32_t pairs_left;
    int32_t pairs_stack[DISCORD_ETF_MAX_DEPTH];
    DiscordSlice last_key;
} DiscordEtfReader;

void discord_etf_reader_init(DiscordEtfReader *r, const void *data, size_t size, int max_depth);

DiscordEtfStatus discord_etf_begin_map(DiscordEtfReader *r);
DiscordEtfStatus discord_etf_end_map(DiscordEtfReader *r);
DiscordEtfStatus discord_etf_next_map_key(DiscordEtfReader *r);
DiscordEtfStatus discord_etf_next_map_field(DiscordEtfReader *r, DiscordEtfField *out);
DiscordEtfStatus discord_etf_skip_value(DiscordEtfReader *r);

bool discord_etf_slice_equals(const DiscordSlice *s, const char *lit);
uint64_t discord_etf_to_id(const DiscordSlice *s);

#ifdef __cplusplus
}
#endif

#endif
