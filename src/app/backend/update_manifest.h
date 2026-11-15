#pragma once

/* What the Project Hub's updates.json says about a newer version (backend/update_check.c fetches it).
 * Pure logic, no I/O, so tests/app/test_update_manifest.c can drive it.
 *
 * updates.json, schema 1 (the release script writes it; docs/update-notice.md is the reference):
 *
 *   { "schema": 1,
 *     "eclipse": {
 *       "download_url": "https://github.com/PreceptorOfMagic/Eclipse/releases/latest",
 *       "how_to_update": { "webos": "...", "windows": "...", "linux": "...", "android": "..." },
 *       "releases": [
 *         { "version": "1.3.7", "date": "2026-11-20",
 *           "notify": true,                      optional, default true
 *           "platforms": ["windows", "linux"],   optional, default every platform
 *           "highlights": ["...", "..."],
 *           "notes_url": "https://..." } ] },
 *     "umbra": { ... } }
 *
 * A release with "notify": false is SILENT: on its own it never shows the update notice. When a later
 * release does notify, the silent ones between the installed version and it are listed with it, since
 * installing the later one brings their changes too. "platforms" limits a release, silent or not, to
 * those platforms. Unknown keys are ignored, so later schema additions do not break older clients. */

#include <stdbool.h>
#include <stddef.h>

typedef struct update_release_t {
    char *version;
    char *date;
    char *notes_url;
    char **highlights;
    size_t highlights_count;
} update_release_t;

typedef struct update_info_t {
    bool available;            /* a NOTIFYING release newer than the installed version applies here */
    char *latest_version;      /* that release's version */
    update_release_t *releases;   /* every applicable release above the installed one, up to latest_version,
                                   * newest first */
    size_t releases_count;
    char *how_to_update;       /* the product's text for this platform, or NULL */
    char *download_url;
} update_info_t;

/* Compares dotted versions numerically ("1.10.0" > "1.9.2"). A pre-release suffix ("1.4.0-beta.1")
 * sorts below the same version without one. Returns <0, 0 or >0. */
int update_version_compare(const char *a, const char *b);

/* False when the document is not a usable manifest (out is then cleared). True otherwise, with
 * out->available saying whether there is anything to show. */
bool update_manifest_evaluate(const char *json, size_t len, const char *product, const char *platform,
                              const char *installed_version, update_info_t *out);

void update_info_clear(update_info_t *info);
