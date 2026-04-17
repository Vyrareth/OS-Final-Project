#ifndef IPC_H
#define IPC_H

#include <stdint.h>

/* Maximum field lengths — keep the struct fixed-size for simple framing */
#define IPC_URL_MAX      2048
#define IPC_PATH_MAX      512

/*
 * CrawlMsg — sent by the crawler to the indexer for every successfully
 * fetched and saved page.
 *
 * Wire format: write sizeof(CrawlMsg) bytes in one send() call.
 * The receiver does a full read of sizeof(CrawlMsg) bytes per message.
 * A sentinel message with docid == UINT32_MAX signals end-of-stream.
 */
typedef struct {
    uint32_t docid;               /* Unique document ID, assigned by crawler */
    uint32_t depth;               /* Crawl depth at which this page was found */
    char     url[IPC_URL_MAX];    /* Null-terminated canonical URL */
    char     filepath[IPC_PATH_MAX]; /* Null-terminated path to saved file */
} CrawlMsg;

/* Sentinel docid value — crawler sends this once when it is done crawling */
#define IPC_SENTINEL_DOCID UINT32_MAX

#endif /* IPC_H */