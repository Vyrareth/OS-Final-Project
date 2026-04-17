#ifndef IPC_H
#define IPC_H

#include <stdint.h>

/* Maximum field lengths — keep the struct fixed-size for simple framing */
#define IPC_URL_MAX   2048
#define IPC_PATH_MAX   512

/*
 * CrawlMsg — sent by the crawler to the indexer for every successfully
 * fetched and saved page.
 *
 * Wire format: write sizeof(CrawlMsg) bytes in one send() call.
 * The receiver loops on recv() until sizeof(CrawlMsg) bytes arrive.
 * A sentinel with docid == IPC_SENTINEL_DOCID signals end-of-stream.
 */
typedef struct {
    uint32_t docid;
    uint32_t depth;
    char     url[IPC_URL_MAX];
    char     filepath[IPC_PATH_MAX];
} CrawlMsg;

#define IPC_SENTINEL_DOCID UINT32_MAX

#endif /* IPC_H */
