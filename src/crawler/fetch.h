#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>

typedef struct {
    char  *data;   /* Heap-allocated response body; caller must free */
    size_t size;   /* Number of bytes in data (not counting NUL) */
} FetchResult;

/*
 * fetch_url — downloads url using the given libcurl easy handle.
 *   Returns  0 on success; result->data is heap-allocated, NUL-terminated.
 *   Returns -1 on any error (timeout, HTTP error, etc.) and logs the failure.
 *   The caller must free result->data on success.
 */
int fetch_url(void *curl_handle, const char *url, FetchResult *result);

/* libcurl write callback — not for direct use outside fetch.c */
size_t fetch_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata);

#endif /* FETCH_H */
