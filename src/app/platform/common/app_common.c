#include "app.h"

#include <locale.h>
#include <libintl.h>
#include <stdlib.h>

#include "util/i18n.h"
#include "util/path.h"
#include "app_launch.h"

#define USE_OPENURL SDL_VERSION_ATLEAST(2, 0, 14)

#if !USE_OPENURL

#include <stdio.h>
#include <stdlib.h>

#endif

void app_open_url(const char *url) {
#if USE_OPENURL
    SDL_OpenURL(url);
#elif OS_LINUX
    char command[8192];
    snprintf(command, sizeof(command), "xdg-open '%s'", url);
    system(command);
#elif OS_DARWIN
    char command[8192];
    snprintf(command, sizeof(command), "open '%s'", url);
    system(command);
#endif
}

void app_init_locale() {
    const char *textdomaindir = getenv("TEXTDOMAINDIR");
#ifdef OS_WINDOWS
    /* A downloaded portable build has no machine-wide gettext prefix. Resolve
     * the installed catalogs from the executable, not the launcher's working
     * directory. Keep TEXTDOMAINDIR as an explicit developer/user override. */
    char *portable_textdomaindir = NULL;
    if (textdomaindir == NULL) {
        char *base = path_assets();
        char *share = base != NULL ? path_join(base, "share") : NULL;
        portable_textdomaindir = share != NULL ? path_join(share, "locale") : NULL;
        free(base);
        free(share);
        textdomaindir = portable_textdomaindir;
    }
#endif
    if (textdomaindir) {
        bindtextdomain("moonlight-tv", textdomaindir);
        bind_textdomain_codeset("moonlight-tv", "UTF-8");
    }
#ifdef OS_WINDOWS
    free(portable_textdomaindir);
#endif
    textdomain("moonlight-tv");
    i18n_setlocale("");
}

app_launch_params_t *app_handle_launch(app_t *app, int argc, char *argv[]) {
    // Do nothing
    return NULL;
}
