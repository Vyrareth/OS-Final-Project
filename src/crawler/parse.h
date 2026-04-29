#ifndef PARSE_H
#define PARSE_H

/*
 * parse.h — declares functions for extracting links from HTML pages.
 * Used by worker threads after fetching a page to find new URLs to crawl.
 */

/*
 * LinkCallback — a function pointer type.
 * extract_links() calls this function for EACH link it finds in the HTML.
 * href     — a heap-allocated normalized absolute URL (e.g. "https://wikipedia.org/Linux")
 * user_data — extra data passed through from the caller (e.g. the queue pointer)
 * The callback takes ownership of href and must free() it when done.
 */
typedef void (*LinkCallback)(char *href, void *user_data);

/*
 * extract_links — scans HTML for all href="..." and href='...' patterns.
 * For each link found, normalizes it to an absolute URL and calls callback.
 *
 * html      — the full HTML content of a downloaded page
 * base_url  — the URL of the page (used to resolve relative links like "/wiki/Linux")
 * callback  — function called for each valid link found
 * user_data — passed through to the callback unchanged
 */
void extract_links(const char *html, const char *base_url,
                   LinkCallback callback, void *user_data);

/*
 * normalize_url — converts a raw href into a clean absolute URL.
 * Resolves relative URLs against the base URL.
 * Strips fragment (#section) from URLs.
 * Rejects non-HTTP/HTTPS links (javascript:, mailto:, data:, tel:, etc.)
 *
 * base — the URL of the current page
 * href — the raw href value found in the HTML (could be relative or absolute)
 *
 * Returns a heap-allocated absolute URL string, or NULL if invalid.
 * Caller must free() the returned string.
 *
 * Examples:
 *   base="https://wikipedia.org/wiki/Linux", href="/wiki/Kernel"
 *   → returns "https://wikipedia.org/wiki/Kernel"
 *
 *   base="https://wikipedia.org", href="javascript:void(0)"
 *   → returns NULL (not a valid web link)
 */
char *normalize_url(const char *base, const char *href);

#endif /* PARSE_H */
