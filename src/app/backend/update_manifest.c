#include "update_manifest.h"

#include <stdlib.h>
#include <string.h>

#include "util/json_mini.h"

static char *dup_or_null(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

/* Reads one numeric component; *s advances past it and a following '.'. */
static long next_component(const char **s) {
    long v = 0;
    while (**s >= '0' && **s <= '9') {
        v = v * 10 + (**s - '0');
        if (v > 1000000000L) {
            v = 1000000000L;
        }
        (*s)++;
    }
    if (**s == '.') {
        (*s)++;
    }
    return v;
}

static bool at_numeric_end(const char *s) {
    return *s == '\0' || *s == '-' || *s == '+';
}

int update_version_compare(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return a == b ? 0 : (a == NULL ? -1 : 1);
    }
    if (*a == 'v') a++;
    if (*b == 'v') b++;
    while (!at_numeric_end(a) || !at_numeric_end(b)) {
        long x = at_numeric_end(a) ? 0 : next_component(&a);
        long y = at_numeric_end(b) ? 0 : next_component(&b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        /* Neither advanced (a stray character): stop rather than loop. */
        if (!at_numeric_end(a) && !(*a >= '0' && *a <= '9')) break;
        if (!at_numeric_end(b) && !(*b >= '0' && *b <= '9')) break;
    }
    bool pre_a = *a == '-', pre_b = *b == '-';
    if (pre_a != pre_b) {
        return pre_a ? -1 : 1;
    }
    if (pre_a) {
        return strcmp(a, b) < 0 ? -1 : (strcmp(a, b) > 0 ? 1 : 0);
    }
    return 0;
}

static bool applies_to(const json_value_t *release, const char *platform) {
    const json_value_t *platforms = json_mini_get(release, "platforms");
    if (platforms == NULL || platforms->type != JSON_ARRAY) {
        return true;
    }
    for (size_t i = 0; i < platforms->count; i++) {
        const json_value_t *p = platforms->items[i];
        if (p->type == JSON_STRING && platform != NULL && strcmp(p->string, platform) == 0) {
            return true;
        }
    }
    return false;
}

static void release_clear(update_release_t *r) {
    free(r->version);
    free(r->date);
    free(r->notes_url);
    for (size_t i = 0; i < r->highlights_count; i++) {
        free(r->highlights[i]);
    }
    free(r->highlights);
    memset(r, 0, sizeof(*r));
}

static bool release_copy(const json_value_t *src, update_release_t *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->version = dup_or_null(json_mini_get_string(src, "version"));
    dst->date = dup_or_null(json_mini_get_string(src, "date"));
    dst->notes_url = dup_or_null(json_mini_get_string(src, "notes_url"));
    const json_value_t *hl = json_mini_get(src, "highlights");
    if (hl != NULL && hl->type == JSON_ARRAY && hl->count > 0) {
        dst->highlights = calloc(hl->count, sizeof(char *));
        if (dst->highlights == NULL) {
            release_clear(dst);
            return false;
        }
        for (size_t i = 0; i < hl->count; i++) {
            if (hl->items[i]->type == JSON_STRING) {
                dst->highlights[dst->highlights_count++] = dup_or_null(hl->items[i]->string);
            }
        }
    }
    return dst->version != NULL;
}

static int release_cmp_desc(const void *a, const void *b) {
    const update_release_t *x = a, *y = b;
    return -update_version_compare(x->version, y->version);
}

bool update_manifest_evaluate(const char *json, size_t len, const char *product, const char *platform,
                              const char *installed_version, update_info_t *out) {
    memset(out, 0, sizeof(*out));
    json_value_t *root = json_mini_parse(json, len);
    if (root == NULL || root->type != JSON_OBJECT) {
        json_mini_free(root);
        return false;
    }
    const json_value_t *schema = json_mini_get(root, "schema");
    const json_value_t *prod = json_mini_get(root, product);
    const json_value_t *releases = json_mini_get(prod, "releases");
    /* A later schema number means a format this build cannot read safely: show nothing. */
    if (schema == NULL || schema->type != JSON_NUMBER || schema->number != 1 || releases == NULL ||
        releases->type != JSON_ARRAY) {
        json_mini_free(root);
        return false;
    }

    /* The newest notifying release above the installed one, for this platform. */
    const char *target = NULL;
    for (size_t i = 0; i < releases->count; i++) {
        const json_value_t *r = releases->items[i];
        const char *v = json_mini_get_string(r, "version");
        if (v == NULL || !applies_to(r, platform) || !json_mini_get_bool(r, "notify", true)) {
            continue;
        }
        if (update_version_compare(v, installed_version) > 0 &&
            (target == NULL || update_version_compare(v, target) > 0)) {
            target = v;
        }
    }
    if (target != NULL) {
        out->releases = calloc(releases->count, sizeof(update_release_t));
        if (out->releases == NULL) {
            json_mini_free(root);
            return false;
        }
        for (size_t i = 0; i < releases->count; i++) {
            const json_value_t *r = releases->items[i];
            const char *v = json_mini_get_string(r, "version");
            if (v == NULL || !applies_to(r, platform) || update_version_compare(v, installed_version) <= 0 ||
                update_version_compare(v, target) > 0) {
                continue;
            }
            if (release_copy(r, &out->releases[out->releases_count])) {
                out->releases_count++;
            }
        }
        qsort(out->releases, out->releases_count, sizeof(update_release_t), release_cmp_desc);
        out->available = true;
        out->latest_version = dup_or_null(target);
        out->how_to_update = dup_or_null(json_mini_get_string(json_mini_get(prod, "how_to_update"), platform));
        out->download_url = dup_or_null(json_mini_get_string(prod, "download_url"));
    }
    json_mini_free(root);
    return true;
}

void update_info_clear(update_info_t *info) {
    if (info == NULL) {
        return;
    }
    for (size_t i = 0; i < info->releases_count; i++) {
        release_clear(&info->releases[i]);
    }
    free(info->releases);
    free(info->latest_version);
    free(info->how_to_update);
    free(info->download_url);
    memset(info, 0, sizeof(*info));
}
