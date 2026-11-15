#include "json_mini.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct parser_t {
    const char *p;
    const char *end;
    int depth;
} parser_t;

static json_value_t *parse_value(parser_t *ps);

static void skip_ws(parser_t *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static bool literal(parser_t *ps, const char *word) {
    size_t n = strlen(word);
    if ((size_t) (ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) {
        return false;
    }
    ps->p += n;
    return true;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

static size_t put_utf8(char *out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char) cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

/* At an opening quote. The decoded string is never longer than its source, so one allocation fits. */
static char *parse_string_raw(parser_t *ps) {
    if (ps->p >= ps->end || *ps->p != '"') {
        return NULL;
    }
    ps->p++;
    const char *start = ps->p;
    char *out = malloc((size_t) (ps->end - start) + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t n = 0;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char) *ps->p++;
        if (c == '"') {
            out[n] = '\0';
            return out;
        }
        if (c < 0x20) {
            break;   /* control characters must be escaped */
        }
        if (c != '\\') {
            out[n++] = (char) c;
            continue;
        }
        if (ps->p >= ps->end) {
            break;
        }
        char e = *ps->p++;
        switch (e) {
            case '"': out[n++] = '"'; break;
            case '\\': out[n++] = '\\'; break;
            case '/': out[n++] = '/'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case 'u': {
                if (ps->end - ps->p < 4) {
                    goto fail;
                }
                int hi = hex4(ps->p);
                if (hi < 0) {
                    goto fail;
                }
                ps->p += 4;
                uint32_t cp = (uint32_t) hi;
                if (hi >= 0xD800 && hi <= 0xDBFF) {
                    if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                        goto fail;
                    }
                    int lo = hex4(ps->p + 2);
                    if (lo < 0xDC00 || lo > 0xDFFF) {
                        goto fail;
                    }
                    ps->p += 6;
                    cp = 0x10000 + (((uint32_t) hi - 0xD800) << 10) + ((uint32_t) lo - 0xDC00);
                } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                    goto fail;
                }
                /* \uXXXX is 6 source bytes (12 for a pair) and at most 4 output bytes: still fits. */
                n += put_utf8(out + n, cp);
                break;
            }
            default:
                goto fail;
        }
    }
    fail:
    free(out);
    return NULL;
}

static json_value_t *new_value(json_type_t type) {
    json_value_t *v = calloc(1, sizeof(json_value_t));
    if (v != NULL) {
        v->type = type;
    }
    return v;
}

static bool append(json_value_t *container, char *key, json_value_t *item) {
    json_value_t **items = realloc(container->items, (container->count + 1) * sizeof(json_value_t *));
    if (items == NULL) {
        return false;
    }
    container->items = items;
    if (container->type == JSON_OBJECT) {
        char **keys = realloc(container->keys, (container->count + 1) * sizeof(char *));
        if (keys == NULL) {
            return false;
        }
        container->keys = keys;
        keys[container->count] = key;
    }
    items[container->count++] = item;
    return true;
}

static json_value_t *parse_number(parser_t *ps) {
    const char *s = ps->p;
    if (ps->p < ps->end && *ps->p == '-') {
        ps->p++;
    }
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
        return NULL;
    }
    if (*ps->p == '0') {
        ps->p++;
    } else {
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return NULL;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return NULL;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    char buf[64];
    size_t len = (size_t) (ps->p - s);
    if (len >= sizeof(buf)) {
        return NULL;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    json_value_t *v = new_value(JSON_NUMBER);
    if (v != NULL) {
        v->number = strtod(buf, NULL);
    }
    return v;
}

static json_value_t *parse_container(parser_t *ps, bool object) {
    if (++ps->depth > JSON_MINI_MAX_DEPTH) {
        return NULL;
    }
    ps->p++;   /* '[' or '{' */
    json_value_t *c = new_value(object ? JSON_OBJECT : JSON_ARRAY);
    if (c == NULL) {
        return NULL;
    }
    char close = object ? '}' : ']';
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return c;
    }
    for (;;) {
        char *key = NULL;
        if (object) {
            skip_ws(ps);
            key = parse_string_raw(ps);
            if (key == NULL) {
                goto fail;
            }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                goto fail;
            }
            ps->p++;
        }
        json_value_t *item = parse_value(ps);
        if (item == NULL || !append(c, key, item)) {
            free(key);
            json_mini_free(item);
            goto fail;
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            ps->depth--;
            return c;
        }
        goto fail;
    }
    fail:
    json_mini_free(c);
    return NULL;
}

static json_value_t *parse_value(parser_t *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) {
        return NULL;
    }
    switch (*ps->p) {
        case '{':
            return parse_container(ps, true);
        case '[':
            return parse_container(ps, false);
        case '"': {
            char *s = parse_string_raw(ps);
            if (s == NULL) {
                return NULL;
            }
            json_value_t *v = new_value(JSON_STRING);
            if (v == NULL) {
                free(s);
                return NULL;
            }
            v->string = s;
            return v;
        }
        case 't':
        case 'f': {
            bool t = *ps->p == 't';
            if (!literal(ps, t ? "true" : "false")) {
                return NULL;
            }
            json_value_t *v = new_value(JSON_BOOL);
            if (v != NULL) {
                v->boolean = t;
            }
            return v;
        }
        case 'n':
            return literal(ps, "null") ? new_value(JSON_NULL) : NULL;
        default:
            return parse_number(ps);
    }
}

json_value_t *json_mini_parse(const char *text, size_t len) {
    if (text == NULL || len > JSON_MINI_MAX_INPUT) {
        return NULL;
    }
    parser_t ps = {.p = text, .end = text + len, .depth = 0};
    /* A UTF-8 byte order mark is tolerated. */
    if (len >= 3 && (unsigned char) text[0] == 0xEF && (unsigned char) text[1] == 0xBB &&
        (unsigned char) text[2] == 0xBF) {
        ps.p += 3;
    }
    json_value_t *v = parse_value(&ps);
    if (v == NULL) {
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        json_mini_free(v);
        return NULL;
    }
    return v;
}

void json_mini_free(json_value_t *value) {
    if (value == NULL) {
        return;
    }
    for (size_t i = 0; i < value->count; i++) {
        json_mini_free(value->items[i]);
        if (value->keys != NULL) {
            free(value->keys[i]);
        }
    }
    free(value->items);
    free(value->keys);
    free(value->string);
    free(value);
}

const json_value_t *json_mini_get(const json_value_t *obj, const char *key) {
    if (obj == NULL || obj->type != JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            return obj->items[i];
        }
    }
    return NULL;
}

const char *json_mini_get_string(const json_value_t *obj, const char *key) {
    const json_value_t *v = json_mini_get(obj, key);
    return v != NULL && v->type == JSON_STRING ? v->string : NULL;
}

bool json_mini_get_bool(const json_value_t *obj, const char *key, bool fallback) {
    const json_value_t *v = json_mini_get(obj, key);
    return v != NULL && v->type == JSON_BOOL ? v->boolean : fallback;
}
