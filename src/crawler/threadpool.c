#include "threadpool.h"      /* our own header — WorkerArgs, ThreadPool structs */
#include "fetch.h"           /* for fetch_url() — downloads web pages */
#include "parse.h"           /* for extract_links() — finds links in HTML */
#include "../common/log.h"   /* for LOG_INFO, LOG_ERROR — logging */
#include <curl/curl.h>       /* for curl_easy_init, curl_easy_cleanup */
#include <stdio.h>           /* for fopen, fputs, fclose, snprintf */
#include <stdlib.h>          /* for malloc, free */
#include <string.h>          /* for memset, strncpy, snprintf */
#include <unistd.h>          /* for POSIX file operations */
#include <sys/socket.h>      /* for send() — sending data over socket */

/*
 * LinkCtx — context passed to the link callback function.
 * When extract_links finds a URL, it calls link_callback with this struct.
 * Contains everything the callback needs to process the link.
 */
typedef struct {
    WorkerArgs *args;       /* shared worker arguments (queue, visited set, etc.) */
    const char *base_url;   /* the URL of the page we just crawled */
    int         next_depth; /* depth of the new links (current depth + 1) */
} LinkCtx;

/*
 * link_callback — called by extract_links() for each link found in the HTML.
 * This is where new URLs get added to the queue for other threads to crawl.
 *
 * Steps:
 *   1. Check if depth limit is exceeded → skip if too deep
 *   2. Check if URL was already visited → skip if already seen
 *   3. Add URL to the queue for a worker thread to crawl
 *
 * href      — the normalized URL found in the HTML (we own this, must free it)
 * user_data — our LinkCtx struct cast to void*
 */
static void link_callback(char *href, void *user_data) {
    LinkCtx *ctx = user_data; /* cast back to LinkCtx */

    /* Skip if this link is deeper than --max-depth allows */
    if (ctx->next_depth > ctx->args->max_depth) {
        free(href); /* we own href, must free it */
        return;
    }

    /* Check if URL was already visited — returns 0 if NEW, non-zero if SEEN */
    if (visited_check_and_insert(ctx->args->visited, href) != 0) {
        free(href); /* already seen — skip it */
        return;
    }

    /* URL is new and within depth — add to queue for a worker to crawl */
    queue_push(ctx->args->url_queue, href, ctx->next_depth);
    free(href); /* queue makes its own copy via strdup, so we free ours */
}

/*
 * save_page — saves the downloaded HTML to disk.
 * Creates a file at <out_dir>/pages/<docid>.html
 *
 * out_dir — the output directory (e.g. "data")
 * docid   — the unique page ID (e.g. 5 → "data/pages/5.html")
 * html    — the HTML content to write
 *
 * Returns  0 on success.
 * Returns -1 if the file could not be opened for writing.
 */
static int save_page(const char *out_dir, int docid, const char *html) {
    char path[1024];
    /* build the file path: "data/pages/5.html" */
    snprintf(path, sizeof(path), "%s/pages/%d.html", out_dir, docid);

    FILE *fp = fopen(path, "w"); /* open for writing */
    if (!fp) {
        LOG_ERROR("Cannot open %s for writing", path);
        return -1; /* failed to open file */
    }

    fputs(html, fp); /* write the HTML content */
    fclose(fp);      /* close the file */
    return 0;        /* success */
}

/*
 * send_ipc_msg — sends a CrawlMsg to the indexer over the UNIX socket.
 * Uses ipc_lock to ensure only ONE thread sends at a time.
 * Without the lock, two threads sending simultaneously would mix up messages.
 *
 * Uses a loop to handle partial sends — send() may not send all bytes at once.
 *
 * sockfd — the socket file descriptor connected to the indexer
 * msg    — the CrawlMsg to send (docid, depth, url, filepath)
 * lock   — mutex to ensure only one thread sends at a time
 *
 * Returns  0 on success.
 * Returns -1 if send() failed.
 */
static int send_ipc_msg(int sockfd, const CrawlMsg *msg,
                        pthread_mutex_t *lock) {
    pthread_mutex_lock(lock); /* lock — only this thread can send now */

    ssize_t sent  = 0;
    ssize_t total = (ssize_t)sizeof(CrawlMsg); /* total bytes to send */
    const char *buf = (const char *)msg;        /* treat struct as byte array */

    /* loop until all bytes are sent (send() may send partial data) */
    while (sent < total) {
        ssize_t n = send(sockfd, buf + sent, (size_t)(total - sent), 0);
        if (n <= 0) {
            pthread_mutex_unlock(lock); /* always unlock before returning */
            return -1; /* send failed */
        }
        sent += n; /* move forward by bytes sent */
    }

    pthread_mutex_unlock(lock); /* unlock — other threads can send now */
    return 0; /* success */
}

/*
 * worker_thread — the main function each worker thread runs.
 *
 * This is the core of the crawler. Each worker thread:
 *   1. Gets its own CURL handle (not shared — libcurl handles aren't thread-safe)
 *   2. Loops forever doing: pop URL → fetch → save → extract links → send IPC
 *   3. Stops when queue_pop returns -1 (no more URLs to crawl)
 *   4. Cleans up its CURL handle before exiting
 *
 * arg — pointer to the shared WorkerArgs struct (same for all threads)
 */
void *worker_thread(void *arg) {
    WorkerArgs *args = arg; /* cast void* back to WorkerArgs* */

    /* create a CURL handle for THIS thread — each thread needs its own */
    /* CURL handles are NOT thread-safe so we never share them */
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("curl_easy_init failed");
        return NULL; /* can't work without curl — exit thread */
    }

    char *url   = NULL; /* will hold the URL popped from the queue */
    int   depth = 0;    /* will hold the depth of the URL */

    /* main worker loop — keep going until queue is shut down and empty */
    while (queue_pop(args->url_queue, &url, &depth) == 0) {

        /* ── Step 1: Check if we've hit the page limit ── */
        pthread_mutex_lock(args->counter_lock); /* lock before reading/writing counters */

        if (*args->pages_fetched >= args->max_pages) {
            /* page limit reached — skip this URL and shut down */
            (*args->pages_skipped)++;            /* count as skipped */
            pthread_mutex_unlock(args->counter_lock);
            queue_shutdown(args->url_queue);     /* wake up all other waiting threads */
            free(url); url = NULL;
            break; /* exit the worker loop */
        }

        /* reserve a unique docid for this page */
        int docid = (*args->next_docid)++; /* get current value then increment */
        (*args->pages_fetched)++;           /* count this page as fetched */
        pthread_mutex_unlock(args->counter_lock); /* unlock counters */

        /* ── Step 2: Download the page ── */
        FetchResult result;
        if (fetch_url(curl, url, &result) < 0) {
            /* download failed — give back the reserved slot and count as failed */
            pthread_mutex_lock(args->counter_lock);
            (*args->pages_fetched)--; /* give back the slot we reserved */
            (*args->pages_failed)++;  /* count as failed */
            pthread_mutex_unlock(args->counter_lock);
            free(url); url = NULL;
            continue; /* skip to next URL */
        }

        /* ── Step 3: Save the HTML to disk ── */
        if (save_page(args->out_dir, docid, result.data) < 0) {
            /* couldn't save to disk — free data and give back slot */
            free(result.data);
            pthread_mutex_lock(args->counter_lock);
            (*args->pages_fetched)--; /* give back the slot */
            pthread_mutex_unlock(args->counter_lock);
            free(url); url = NULL;
            continue; /* skip to next URL */
        }

        /* log success — this is what you saw in the output */
        /* [INFO] fetched depth=0 docid=0 https://en.wikipedia.org/wiki/Linux */
        LOG_INFO("fetched depth=%d docid=%d %s", depth, docid, url);

        /* ── Step 4: Extract links and add them to the queue ── */
        /* create context for the link callback */
        LinkCtx ctx = { args, url, depth + 1 }; /* next depth = current + 1 */
        /* scan the HTML for all href="..." links and call link_callback for each */
        extract_links(result.data, url, link_callback, &ctx);
        free(result.data); /* done with HTML — free it */

        /* ── Step 5: Send metadata to the indexer via IPC ── */
        CrawlMsg msg;
        memset(&msg, 0, sizeof(msg)); /* zero out all fields first */
        msg.docid = (uint32_t)docid;  /* set the unique page ID */
        msg.depth = (uint32_t)depth;  /* set the crawl depth */

        /* set the filepath where we saved the HTML */
        snprintf(msg.filepath, IPC_PATH_MAX, "%s/pages/%d.html", args->out_dir, docid);

        /* set the URL (copy at most IPC_URL_MAX-1 chars to prevent overflow) */
        strncpy(msg.url, url, IPC_URL_MAX - 1);

        /* send the message to the indexer — protected by ipc_lock */
        if (send_ipc_msg(args->ipc_fd, &msg, args->ipc_lock) < 0)
            LOG_ERROR("IPC send failed for docid=%d", docid);

        free(url);  /* done with this URL — free it */
        url = NULL; /* set to NULL to prevent double-free */
    }

    /* cleanup — free URL if we exited loop while holding one */
    if (url) free(url);

    /* destroy this thread's CURL handle */
    curl_easy_cleanup(curl);

    return NULL; /* thread exits */
}

/*
 * threadpool_create — creates N worker threads and starts them all running.
 * Allocates the ThreadPool struct and the array of thread IDs.
 * Calls pthread_create for each thread, passing the shared WorkerArgs.
 *
 * n    — number of worker threads to create
 * args — the shared WorkerArgs all threads will use
 */
ThreadPool *threadpool_create(int n, WorkerArgs args) {
    ThreadPool *tp  = malloc(sizeof(ThreadPool));  /* allocate the thread pool */
    tp->threads     = malloc(sizeof(pthread_t) * (size_t)n); /* allocate thread ID array */
    tp->num_threads = n;    /* store number of threads */
    tp->args        = args; /* store the shared arguments */

    /* create each worker thread */
    for (int i = 0; i < n; i++)
        /* pthread_create starts a new thread running worker_thread */
        /* &tp->args — pass the shared WorkerArgs to every thread */
        pthread_create(&tp->threads[i], NULL, worker_thread, &tp->args);

    return tp; /* return the pool — caller uses threadpool_wait_and_destroy later */
}

/*
 * threadpool_wait_and_destroy — waits for all threads to finish then frees memory.
 * pthread_join blocks until thread i has exited.
 * Must wait for ALL threads before freeing shared data (queue, visited set, etc.)
 * Called in main.c after queue_shutdown() signals threads to stop.
 */
void threadpool_wait_and_destroy(ThreadPool *tp) {
    /* wait for each thread to finish */
    for (int i = 0; i < tp->num_threads; i++)
        pthread_join(tp->threads[i], NULL); /* block until thread i exits */

    free(tp->threads); /* free the thread ID array */
    free(tp);          /* free the thread pool struct */
}
