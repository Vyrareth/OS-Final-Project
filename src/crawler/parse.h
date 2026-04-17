#ifndef PARSE_H
#define PARSE_H

/*
 * LinkCallback — called by extract_links() for each href found.
 *   href is a heap-allocated, normalized absolute URL.
 *   The callback takes ownership; it must free() href when done.
 */
typedef void (*LinkCallback)(char *href, void *user_data);

/*
 * extract_links — scans html for href="..." patterns and calls callback
 * with the normalized absolute URL for each one found.
 * base_url is used to resolve relative hrefs.
 */
void extract_links(const char *html, const char *base_url,
                   LinkCallback callback, void *user_data);

/*
 * normalize_url — resolves href relative to base and returns a
 * heap-allocated canonical http/https URL, or NULL if invalid/non-HTTP.
 * Uses libcurl's CURLU API for robust resolution.
 * Caller must free() the returned string.
 */
char *normalize_url(const char *base, const char *href);

#endif /* PARSE_H */
