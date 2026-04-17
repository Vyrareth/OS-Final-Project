#include "threadpool.h"
#include "fetch.h"
#include "parse.h"
#include "../common/log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* ── Context passed to the link callback ─────────────────────────────── */
typedef struct {
    WorkerArgs *args;
    const char *base_url;
    int         next_depth;
} LinkCtx;

static void link_callback(char *href, void *user_data) {
    LinkCtx *ctx = user_data;
    /* Filter depth and already-visited URLs */
    if (ctx->next_depth > ctx->args->max_depth) { free(href); return; }
    if (visited_check_and_insert(ctx->args->visited, href) != 0) {
        free(href); return;   /* already seen */
    }
    queue_push(ctx->args->url_queue, href, ctx->next_depth);
    free(href);
}

/* ── Persist page HTML to <out_dir>/pages/<docid>.html ───────────────── */
static int save_page(const char *out_dir, int docid, const char *html) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/pages/%d.html", out_dir, docid);
    FILE *fp = fopen(path, "w");
    if (!fp) { LOG_ERROR("Cannot open %s for writing", path); return -1; }
    fputs(html, fp);
    fclose(fp);
    return 0;
}

/* ── Send a CrawlMsg over the shared socket (serialized) ─────────────── */
static int send_ipc_msg(int sockfd, const CrawlMsg *msg,
                        pthread_mutex_t *lock) {
    pthread_mutex_lock(lock);
    ssize_t sent  = 0;
    ssize_t total = (ssize_t)sizeof(CrawlMsg);
    const char *buf = (const char *)msg;
    while (sent < total) {
        ssize_t n = send(sockfd, buf + sent, (size_t)(total - sent), 0);
        if (n <= 0) { pthread_mutex_unlock(lock); return -1; }
        sent += n;
    }
    pthread_mutex_unlock(lock);
    return 0;
}

/* ── Worker thread ────────────────────────────────────────────────────── */
void *worker_thread(void *arg) {
    WorkerArgs *args = arg;

    CURL *curl = curl_easy_init();
    if (!curl) { LOG_ERROR("curl_easy_init failed"); return NULL; }

    char *url   = NULL;
    int   depth = 0;

    while (queue_pop(args->url_queue, &url, &depth) == 0) {

        /* ── Check page limit (race-free) ── */
        pthread_mutex_lock(args->counter_lock);
        if (*args->pages_fetched >= args->max_pages) {
            (*args->pages_skipped)++;
            pthread_mutex_unlock(args->counter_lock);
            queue_shutdown(args->url_queue);
            free(url); url = NULL;
            break;
        }
        int docid = (*args->next_docid)++;
        (*args->pages_fetched)++;
        pthread_mutex_unlock(args->counter_lock);

        /* ── Fetch ── */
        FetchResult result;
        if (fetch_url(curl, url, &result) < 0) {
            pthread_mutex_lock(args->counter_lock);
            (*args->pages_fetched)--;  /* give back reserved slot */
            (*args->pages_failed)++;
            pthread_mutex_unlock(args->counter_lock);
            free(url); url = NULL;
            continue;
        }

        /* ── Save to disk ── */
        if (save_page(args->out_dir, docid, result.data) < 0) {
            free(result.data);
            pthread_mutex_lock(args->counter_lock);
            (*args->pages_fetched)--;
            pthread_mutex_unlock(args->counter_lock);
            free(url); url = NULL;
            continue;
        }

        LOG_INFO("fetched depth=%d docid=%d %s", depth, docid, url);

        /* ── Extract and enqueue links ── */
        LinkCtx ctx = { args, url, depth + 1 };
        extract_links(result.data, url, link_callback, &ctx);
        free(result.data);

        /* ── Send IPC message to indexer ── */
        CrawlMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.docid = (uint32_t)docid;
        msg.depth = (uint32_t)depth;
        snprintf(msg.filepath, IPC_PATH_MAX,
                 "%s/pages/%d.html", args->out_dir, docid);
        strncpy(msg.url, url, IPC_URL_MAX - 1);

        if (send_ipc_msg(args->ipc_fd, &msg, args->ipc_lock) < 0)
            LOG_ERROR("IPC send failed for docid=%d", docid);

        free(url);
        url = NULL;
    }

    if (url) free(url);
    curl_easy_cleanup(curl);
    return NULL;
}

/* ── Thread pool lifecycle ────────────────────────────────────────────── */
ThreadPool *threadpool_create(int n, WorkerArgs args) {
    ThreadPool *tp  = malloc(sizeof(ThreadPool));
    tp->threads     = malloc(sizeof(pthread_t) * (size_t)n);
    tp->num_threads = n;
    tp->args        = args;

    for (int i = 0; i < n; i++)
        pthread_create(&tp->threads[i], NULL, worker_thread, &tp->args);

    return tp;
}

void threadpool_wait_and_destroy(ThreadPool *tp) {
    for (int i = 0; i < tp->num_threads; i++)
        pthread_join(tp->threads[i], NULL);
    free(tp->threads);
    free(tp);
}
