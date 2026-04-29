#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>  /* needed for size_t */

/*
 * FetchResult — holds the downloaded page content.
 * After a successful fetch, data points to the full HTML of the page.
 * The caller is responsible for freeing result->data when done.
 */
typedef struct {
    char  *data;   /* heap-allocated HTML content of the downloaded page */
    size_t size;   /* number of bytes in data (not counting the null terminator) */
} FetchResult;

/*
 * fetch_url — downloads a web page using libcurl.
 * handle     — a libcurl easy handle (one per worker thread, reused for every fetch)
 * url        — the web address to download
 * result     — struct where the downloaded content will be stored
 *
 * Returns  0 on success — result->data contains the HTML, caller must free it.
 * Returns -1 on any error (timeout, HTTP error, network failure) — logs the failure.
 */
int fetch_url(void *curl_handle, const char *url, FetchResult *result);

/*
 * fetch_write_callback — internal libcurl callback function.
 * Called automatically by libcurl every time it receives a chunk of data.
 * Appends each chunk to the growing buffer in result->data.
 * NOT meant to be called directly — only used by libcurl internally.
 */
size_t fetch_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata);

#endif /* FETCH_H */
