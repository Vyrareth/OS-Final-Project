#include "parse.h"           /* our own header — LinkCallback and function declarations */
#include "../common/log.h"   /* for logging errors */
#include <curl/curl.h>       /* for CURLU — libcurl's URL parsing API */
#include <string.h>          /* for strncasecmp, strcmp, memcpy */
#include <stdlib.h>          /* for malloc, free, strdup */
#include <ctype.h>           /* for character checking functions */

/*
 * find_href — scans a string looking for the text "href=" (case-insensitive).
 * Returns a pointer to where "href=" starts, or NULL if not found.
 *
 * Why case-insensitive?
 * HTML is case-insensitive — "HREF=", "Href=", "href=" are all valid.
 * We use bitwise OR with 32 (| 32) to convert uppercase to lowercase:
 *   'H' = 72, 72 | 32 = 104 = 'h'
 *   'h' = 104, 104 | 32 = 104 = 'h' (unchanged)
 * This is faster than calling tolower() for every character.
 */
static const char *find_href(const char *s) {
    for (; *s; s++) {  /* scan through every character */
        /* check if current 5 characters spell "href=" case-insensitively */
        if ((s[0] | 32) == 'h' &&  /* H or h */
            (s[1] | 32) == 'r' &&  /* R or r */
            (s[2] | 32) == 'e' &&  /* E or e */
            (s[3] | 32) == 'f' &&  /* F or f */
            s[4] == '=')           /* must be exactly = */
            return s;  /* found "href=" — return pointer to it */
    }
    return NULL;  /* reached end of string without finding "href=" */
}

/*
 * normalize_url — converts a raw href into a clean absolute http/https URL.
 *
 * Uses libcurl's CURLU API which handles all the complex URL parsing:
 *   - Resolves relative URLs: "/wiki/Linux" → "https://wikipedia.org/wiki/Linux"
 *   - Handles protocol-relative URLs: "//wikipedia.org" → "https://wikipedia.org"
 *   - Strips fragments: "https://wikipedia.org/Linux#History" → "https://wikipedia.org/Linux"
 *   - Rejects non-web schemes: "javascript:", "mailto:", "data:", "tel:"
 *
 * Returns heap-allocated URL string on success, NULL on failure.
 * Caller must free() the returned string.
 */
char *normalize_url(const char *base, const char *href) {
    /* reject empty hrefs */
    if (!href || !href[0]) return NULL;

    /* quick filter — reject known non-web schemes before doing any URL parsing */
    if (strncasecmp(href, "javascript:", 11) == 0) return NULL; /* javascript code */
    if (strncasecmp(href, "mailto:",      7) == 0) return NULL; /* email links */
    if (strncasecmp(href, "data:",        5) == 0) return NULL; /* inline data */
    if (strncasecmp(href, "tel:",         4) == 0) return NULL; /* phone numbers */

    /* create a new CURLU handle — libcurl's URL parser */
    CURLU *cu = curl_url();
    if (!cu) return NULL; /* failed to create handle — out of memory */

    /* set the base URL first so relative URLs can be resolved against it */
    if (base) curl_url_set(cu, CURLUPART_URL, base, 0);

    /* overlay the href on top of the base URL */
    /* CURLU_ALLOW_SPACE — don't fail if URL has spaces */
    CURLUcode rc = curl_url_set(cu, CURLUPART_URL, href, CURLU_ALLOW_SPACE);
    if (rc != CURLUE_OK) {
        /* href could not be parsed as a URL */
        curl_url_cleanup(cu);
        return NULL;
    }

    /* check the scheme — only accept http and https */
    char *scheme = NULL;
    curl_url_get(cu, CURLUPART_SCHEME, &scheme, 0);
    if (!scheme ||
        (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0)) {
        /* scheme is not http or https — reject it (e.g. ftp://, file://) */
        curl_free(scheme);
        curl_url_cleanup(cu);
        return NULL;
    }
    curl_free(scheme); /* free the scheme string we got from libcurl */

    /* strip the fragment part (#section) — we don't want anchors in our URLs */
    /* e.g. "https://wikipedia.org/Linux#History" → "https://wikipedia.org/Linux" */
    curl_url_set(cu, CURLUPART_FRAGMENT, NULL, 0);

    /* get the final normalized URL string */
    char *full = NULL;
    curl_url_get(cu, CURLUPART_URL, &full, 0);

    /* copy to our own heap memory (libcurl owns 'full', we need our own copy) */
    char *result = full ? strdup(full) : NULL;

    /* free libcurl's copies */
    curl_free(full);
    curl_url_cleanup(cu);

    return result; /* caller must free() this */
}

/*
 * extract_links — scans HTML for all href="..." and href='...' patterns.
 * For each valid link found, normalizes it and calls the callback function.
 *
 * How it works:
 *   1. Find the next "href=" in the HTML
 *   2. Read the quoted value (between " " or ' ')
 *   3. Normalize it to an absolute URL
 *   4. Call the callback with the URL
 *   5. Repeat from step 1 until end of HTML
 */
void extract_links(const char *html, const char *base_url,
                   LinkCallback callback, void *user_data) {
    const char *p = html; /* pointer that walks through the HTML */

    /* keep scanning until find_href returns NULL (no more hrefs found) */
    while ((p = find_href(p)) != NULL) {
        p += 5;  /* skip past "href=" (5 characters) to get to the quote */

        /* check for opening quote — must be " or ' */
        char quote = 0;
        if (*p == '"' || *p == '\'') quote = *p++; /* save quote type, advance past it */
        if (!quote) continue;  /* no quote found — skip this href */

        /* find the end of the href value (the matching closing quote) */
        const char *start = p;          /* remember where the URL starts */
        while (*p && *p != quote) p++;  /* scan until we hit the closing quote */
        if (*p != quote) break;         /* no closing quote — end of HTML, stop */

        /* calculate the length of the raw href value */
        size_t len = (size_t)(p - start);
        p++;  /* move past the closing quote for next iteration */

        /* skip empty hrefs or suspiciously long ones */
        if (len == 0 || len >= 4096) continue;

        /* copy the raw href into a local buffer and null-terminate it */
        char raw[4096];
        memcpy(raw, start, len); /* copy exactly len bytes */
        raw[len] = '\0';         /* add null terminator */

        /* normalize the raw href to an absolute http/https URL */
        /* e.g. "/wiki/Linux" → "https://en.wikipedia.org/wiki/Linux" */
        char *norm = normalize_url(base_url, raw);

        /* if normalization succeeded, call the callback with the URL */
        if (norm) callback(norm, user_data);
        /* callback takes ownership of norm and must free() it */
    }
}
