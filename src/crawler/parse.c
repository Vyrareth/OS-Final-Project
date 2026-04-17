#include "parse.h"
#include "../common/log.h"
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── Portable case-insensitive scan for "href=" ─────────────────────── */
static const char *find_href(const char *s) {
    for (; *s; s++) {
        if ((s[0] | 32) == 'h' && (s[1] | 32) == 'r' &&
            (s[2] | 32) == 'e' && (s[3] | 32) == 'f' && s[4] == '=')
            return s;
    }
    return NULL;
}

/*
 * normalize_url — use libcurl's CURLU API to resolve href against base,
 * strip the fragment, and verify the scheme is http or https.
 */
char *normalize_url(const char *base, const char *href) {
    if (!href || !href[0]) return NULL;

    /* Quick filter: skip non-web schemes before touching CURLU */
    if (strncasecmp(href, "javascript:", 11) == 0) return NULL;
    if (strncasecmp(href, "mailto:",      7) == 0) return NULL;
    if (strncasecmp(href, "data:",        5) == 0) return NULL;
    if (strncasecmp(href, "tel:",         4) == 0) return NULL;

    CURLU *cu = curl_url();
    if (!cu) return NULL;

    /* Set the base URL first, then overlay the href */
    if (base) curl_url_set(cu, CURLUPART_URL, base, 0);
    CURLUcode rc = curl_url_set(cu, CURLUPART_URL, href, CURLU_ALLOW_SPACE);
    if (rc != CURLUE_OK) { curl_url_cleanup(cu); return NULL; }

    /* Accept only http and https */
    char *scheme = NULL;
    curl_url_get(cu, CURLUPART_SCHEME, &scheme, 0);
    if (!scheme ||
        (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0)) {
        curl_free(scheme);
        curl_url_cleanup(cu);
        return NULL;
    }
    curl_free(scheme);

    /* Strip fragment */
    curl_url_set(cu, CURLUPART_FRAGMENT, NULL, 0);

    char *full = NULL;
    curl_url_get(cu, CURLUPART_URL, &full, 0);
    char *result = full ? strdup(full) : NULL;

    curl_free(full);
    curl_url_cleanup(cu);
    return result;
}

/*
 * extract_links — scans html for all href="..." and href='...' values,
 * normalizes each against base_url, and calls callback(url, user_data).
 */
void extract_links(const char *html, const char *base_url,
                   LinkCallback callback, void *user_data) {
    const char *p = html;
    while ((p = find_href(p)) != NULL) {
        p += 5;  /* skip "href=" */

        char quote = 0;
        if (*p == '"' || *p == '\'') quote = *p++;
        if (!quote) continue;  /* bare href= without quotes — skip */

        const char *start = p;
        while (*p && *p != quote) p++;
        if (*p != quote) break;

        size_t len = (size_t)(p - start);
        p++;  /* move past closing quote */

        if (len == 0 || len >= 4096) continue;

        char raw[4096];
        memcpy(raw, start, len);
        raw[len] = '\0';

        char *norm = normalize_url(base_url, raw);
        if (norm) callback(norm, user_data);
    }
}
