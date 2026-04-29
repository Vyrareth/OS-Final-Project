#ifndef IPC_H
#define IPC_H

/*
 * ipc.h — defines the message format shared between the crawler and indexer.
 * Both programs include this file so they agree on the exact structure
 * of every message sent over the UNIX domain socket.
 */

#include <stdint.h>  /* needed for uint32_t — a 32-bit unsigned integer */

/* Maximum number of characters allowed in a URL */
#define IPC_URL_MAX   2048

/* Maximum number of characters allowed in a file path */
#define IPC_PATH_MAX   512

/*
 * CrawlMsg — the message the crawler sends to the indexer for every page downloaded.
 *
 * Wire format: the crawler writes sizeof(CrawlMsg) bytes in one send() call.
 * The indexer reads sizeof(CrawlMsg) bytes per message in a loop.
 * Fixed size means no need for length headers or delimiters — simple and fast.
 *
 * Fields:
 *   docid    — unique ID number assigned to this page (0, 1, 2, 3...)
 *   depth    — how many links away from the seed URL this page was found
 *   url      — the full web address of the page (e.g. "https://wikipedia.org/Linux")
 *   filepath — where the HTML file was saved on disk (e.g. "data/pages/0.html")
 */
typedef struct {
    uint32_t docid;                /* unique page number assigned by the crawler */
    uint32_t depth;                /* crawl depth (0 = seed page, 1 = one click away) */
    char     url[IPC_URL_MAX];     /* null-terminated URL string (max 2048 chars) */
    char     filepath[IPC_PATH_MAX]; /* null-terminated file path string (max 512 chars) */
} CrawlMsg;

/*
 * IPC_SENTINEL_DOCID — special docid value meaning "crawling is done".
 * The crawler sends one final CrawlMsg with docid = UINT32_MAX (4294967295)
 * to tell the indexer there are no more pages coming.
 * The indexer sees this and flushes the index to disk then exits.
 */
#define IPC_SENTINEL_DOCID UINT32_MAX

#endif /* IPC_H */
