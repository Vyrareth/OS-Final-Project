#include "fetch.h"           /* our own header — FetchResult struct and function declarations */
#include "../common/log.h"   /* for LOG_WARN — to log errors without crashing */
#include <curl/curl.h>       /* libcurl — the library that handles HTTP downloads */
#include <stdlib.h>          /* for malloc, realloc, free */
#include <string.h>          /* for memcpy — copying bytes of data */

/*
 * fetch_write_callback — called by libcurl every time it receives a chunk of data.
 *
 * When libcurl downloads a page, it doesn't get all the data at once.
 * It receives it in small chunks. This function is called for EACH chunk.
 * We append each chunk to our growing buffer so at the end we have the full page.
 *
 * Parameters:
 *   ptr      — pointer to the chunk of data libcurl just received
 *   size     — always 1 (size of each element in bytes)
 *   nmemb    — number of elements in this chunk
 *   userdata — our FetchResult struct where we store the data
 *
 * Returns the number of bytes we processed (must equal size*nmemb or curl stops)
 */
size_t fetch_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FetchResult *res   = userdata;          /* cast userdata back to our FetchResult */
    size_t       total = size * nmemb;      /* total bytes in this chunk */

    /* grow our buffer to fit the new chunk plus a null terminator */
    char *tmp = realloc(res->data, res->size + total + 1);
    if (!tmp) return 0; /* return 0 tells libcurl something went wrong */

    res->data = tmp;                            /* update pointer to new larger buffer */
    memcpy(res->data + res->size, ptr, total);  /* copy new chunk to end of buffer */
    res->size              += total;            /* update total size */
    res->data[res->size]    = '\0';             /* add null terminator so it's a valid string */
    return total;                               /* tell libcurl we processed all bytes */
}

/*
 * fetch_url — downloads a web page and stores the HTML in result.
 *
 * Each worker thread has its own CURL handle (created in threadpool.c).
 * The handle is reused for every URL that thread fetches — more efficient
 * than creating a new handle for each request.
 *
 * Parameters:
 *   handle — the libcurl easy handle for this worker thread
 *   url    — the web address to download (e.g. "https://wikipedia.org/Linux")
 *   result — where to store the downloaded HTML content
 *
 * Returns  0 on success.
 * Returns -1 on failure (logs the error, frees data, sets data to NULL).
 */
int fetch_url(void *handle, const char *url, FetchResult *result) {
    CURL *curl   = handle;       /* cast void* back to CURL* (libcurl handle type) */
    result->data = malloc(1);    /* start with 1 byte — will grow as data arrives */
    result->size = 0;            /* no data yet */
    if (!result->data) return -1; /* malloc failed — out of memory */
    result->data[0] = '\0';      /* make it an empty string to start */

    /* --- Configure libcurl options --- */

    curl_easy_setopt(curl, CURLOPT_URL, url);
    /* tells libcurl which URL to download */

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_callback);
    /* tells libcurl to call our fetch_write_callback for each chunk of data */

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, result);
    /* passes our FetchResult as the userdata to fetch_write_callback */

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* follow redirects (e.g. http → https, or old URL → new URL) */

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    /* give up connecting after 10 seconds — prevents hanging forever */

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    /* give up the entire download after 30 seconds */

    curl_easy_setopt(curl, CURLOPT_USERAGENT, "WebCrawler/1.0");
    /* identify ourselves to the web server — some servers block requests without this */

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    /* don't verify SSL certificate — simplifies crawling various websites */

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    /* don't verify SSL hostname — same reason as above */

    /* --- Perform the download --- */
    CURLcode res = curl_easy_perform(curl);
    /* this actually sends the HTTP request and downloads the page */
    /* libcurl calls fetch_write_callback for each chunk it receives */

    /* --- Check for libcurl errors --- */
    if (res != CURLE_OK) {
        /* libcurl failed — network error, timeout, DNS failure, etc. */
        LOG_WARN("curl error for %s: %s", url, curl_easy_strerror(res));
        /* curl_easy_strerror converts error code to human readable message */
        free(result->data);   /* free the buffer we allocated */
        result->data = NULL;  /* set to NULL so caller knows it failed */
        return -1;            /* signal failure */
    }

    /* --- Check HTTP response code --- */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    /* get the HTTP status code (200 = OK, 404 = Not Found, 500 = Server Error) */

    if (http_code < 200 || http_code >= 300) {
        /* anything outside 200-299 is an error */
        /* 404 = page not found, 403 = forbidden, 500 = server error */
        LOG_WARN("HTTP %ld for %s", http_code, url);
        /* this is what printed: [WARN] HTTP 404 for https://... in your output */
        free(result->data);   /* free the buffer */
        result->data = NULL;  /* set to NULL */
        return -1;            /* signal failure */
    }

    return 0; /* success — result->data contains the full HTML of the page */
}
