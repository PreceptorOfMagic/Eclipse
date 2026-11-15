#pragma once

/* A small, strict JSON reader for the update manifest (backend/update_manifest.c).
 *
 * It builds a tree of the whole document. Strings are UTF-8 (\uXXXX escapes, surrogate pairs included,
 * are decoded); numbers are doubles. It is written for small trusted-format files of a few kilobytes,
 * not for speed: input over JSON_MINI_MAX_INPUT bytes or nested deeper than JSON_MINI_MAX_DEPTH is
 * refused, as is anything that is not exactly one JSON value (trailing text included). */

#include <stdbool.h>
#include <stddef.h>

#define JSON_MINI_MAX_INPUT (256 * 1024)
#define JSON_MINI_MAX_DEPTH 32

typedef enum json_type_t {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} json_type_t;

typedef struct json_value_t json_value_t;

struct json_value_t {
    json_type_t type;
    bool boolean;
    double number;
    char *string;             /* JSON_STRING */
    char **keys;              /* JSON_OBJECT: keys[i] names items[i] */
    json_value_t **items;     /* JSON_ARRAY / JSON_OBJECT */
    size_t count;
};

/* NULL on any error. */
json_value_t *json_mini_parse(const char *text, size_t len);

void json_mini_free(json_value_t *value);

/* NULL unless obj is an object holding key (the first such key wins). */
const json_value_t *json_mini_get(const json_value_t *obj, const char *key);

/* The string of obj[key], or NULL when absent or not a string. */
const char *json_mini_get_string(const json_value_t *obj, const char *key);

/* obj[key] as a boolean, or fallback when absent or not a boolean. */
bool json_mini_get_bool(const json_value_t *obj, const char *key, bool fallback);
