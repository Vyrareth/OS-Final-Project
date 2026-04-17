#include "fetch.h"
#include "../common/log.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

size_t fetch_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FetchResult *res   = userdata;
    size_t       total = size * nmemb;
    char        *tmp   = realloc(res->data, res->size + total + 1);
    if (!tmp) return 0;
    res->data = tmp;
    memcpy(res->data + res->size, ptr, total);
    res->size               += total;
    res->data[res->size]     = '\0';
    return total;
}

int fetch_url(void *handle, const char *url, FetchResult *result) {
    CURL *curl    = handle;
    result->data  = malloc(1);
    result->size  = 0;
    if (!result->data) return -1;
    result->data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  fetch_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "WebCrawler/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG_WARN("curl error for %s: %s", url, curl_easy_strerror(res));
        free(result->data);
        result->data = NULL;
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        LOG_WARN("HTTP %ld for %s", http_code, url);
        free(result->data);
        result->data = NULL;
        return -1;
    }
    return 0;
}
