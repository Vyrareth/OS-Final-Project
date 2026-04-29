Create the following directory layout from your project root. Every source file you write will live inside one of these folders.
```bash
mkdir -p project/{src/{crawler,indexer,query,common},include,data/{pages,index}}
cd project
touch Makefile README.md
```
**What each directory is for:**
- `src/crawler/` — all `.c` files belonging to the crawler process
- `src/indexer/` — all `.c` files belonging to the indexer process
- `src/query/` — all `.c` files belonging to the query tool
- `src/common/` — shared code used by two or more programs (logging, IPC message format, string utilities)
- `include/` — shared `.h` header files
- `data/pages/` — where the crawler will save downloaded HTML files at runtime
- `data/index/` — where the indexer will write its on-disk index files at runtime
Create placeholder source files so the Makefile has something to compile against immediately:

```bash
touch src/crawler/main.c src/crawler/queue.c src/crawler/queue.h
touch src/crawler/visited.c src/crawler/visited.h
touch src/crawler/fetch.c src/crawler/fetch.h
touch src/crawler/parse.c src/crawler/parse.h
touch src/crawler/threadpool.c src/crawler/threadpool.h
touch src/indexer/main.c src/indexer/indexer.c src/indexer/indexer.h
touch src/query/main.c
touch src/common/log.c src/common/log.h
touch src/common/ipc.h
```

### Step 2: Write the Makefile

Write the Makefile before writing any source code. This keeps you honest about your build dependencies and lets you compile incrementally as you add files.

Create `Makefile` in your project root:

```makefile
CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -I./src
LDFLAGS = -lpthread -lcurl

# Source files per binary
CRAWLER_SRCS = src/crawler/main.c \
               src/crawler/queue.c \
               src/crawler/visited.c \
               src/crawler/fetch.c \
               src/crawler/parse.c \
               src/crawler/threadpool.c \
               src/common/log.c

INDEXER_SRCS = src/indexer/main.c \
               src/indexer/indexer.c \
               src/common/log.c

QUERY_SRCS   = src/query/main.c \
               src/common/log.c

# Object files (place in build/ to keep src/ clean)
CRAWLER_OBJS = $(patsubst src/%.c, build/%.o, $(CRAWLER_SRCS))
INDEXER_OBJS = $(patsubst src/%.c, build/%.o, $(INDEXER_SRCS))
QUERY_OBJS   = $(patsubst src/%.c, build/%.o, $(QUERY_SRCS))

.PHONY: all clean run dirs

all: dirs crawler indexer query

dirs:
	mkdir -p build/crawler build/indexer build/query build/common

crawler: $(CRAWLER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

indexer: $(INDEXER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

query: $(QUERY_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Generic pattern rule: src/foo/bar.c -> build/foo/bar.o
build/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/ crawler indexer query data/pages/* data/index/*

# Demo run — start indexer in background, run crawler, then query
run: all
	mkdir -p data/pages data/index
	./indexer --ipc /tmp/crawl.sock --out data/index &
	sleep 1
	./crawler --seed https://en.wikipedia.org/wiki/Linux \
	          --max-depth 2 --max-pages 50 -t 4 \
	          --out data --ipc /tmp/crawl.sock
	./query --index data/index linux kernel threads
```

**Verify the Makefile works** (even with empty source files):

```bash
make all        # Should compile with warnings about empty files — that is fine
make clean      # Should remove build/ and binaries
```

---

## Phase 2: Shared / Common Utilities

---

### Step 3: Define the IPC Message Protocol

The crawler and indexer are separate processes. They need an agreed-upon message format to send document metadata over a UNIX domain socket. Define this format once in a shared header so both programs include the same definition.

Create `src/common/ipc.h`:

```c
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
```

**Why fixed-size?** It eliminates framing complexity. Each `send()`/`recv()` call transfers exactly `sizeof(CrawlMsg)` bytes. No length prefix, no delimiter scanning. The tradeoff is wasted bytes for short URLs, which is acceptable here.

**Why UNIX domain socket over pipe?**
- Bidirectional (useful if you add acknowledgement messages later)
- Named path (`/tmp/crawl.sock`) is easier to coordinate than anonymous pipe file descriptors across `fork()`
- Behaves like a stream socket — familiar API

---

### Step 4: Write the Logging Module

Every module in all three programs will call the logger. Implement it once in `src/common/`.

Create `src/common/log.h`:

```c
#ifndef LOG_H
#define LOG_H

typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

/*
 * log_init  — call once at program startup (pass a filename or NULL for stderr)
 * log_msg   — thread-safe; can be called from any thread at any time
 * log_close — flush and close the log file
 */
void log_init(const char *filepath);
void log_msg(LogLevel level, const char *fmt, ...);
void log_close(void);

/* Convenience macros */
#define LOG_INFO(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif /* LOG_H */
```

Create `src/common/log.c`:

```c
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

static FILE           *log_fp   = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *filepath) {
    pthread_mutex_lock(&log_lock);
    log_fp = filepath ? fopen(filepath, "a") : stderr;
    if (!log_fp) log_fp = stderr;
    pthread_mutex_unlock(&log_lock);
}

void log_msg(LogLevel level, const char *fmt, ...) {
    const char *labels[] = { "INFO", "WARN", "ERROR" };
    time_t      now      = time(NULL);
    char        tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&now));

    va_list ap;
    va_start(ap, fmt);

    pthread_mutex_lock(&log_lock);
    FILE *fp = log_fp ? log_fp : stderr;
    fprintf(fp, "[%s][%s] ", tbuf, labels[level]);
    vfprintf(fp, fmt, ap);
    fprintf(fp, "\n");
    fflush(fp);
    pthread_mutex_unlock(&log_lock);

    va_end(ap);
}

void log_close(void) {
    pthread_mutex_lock(&log_lock);
    if (log_fp && log_fp != stderr) fclose(log_fp);
    log_fp = NULL;
    pthread_mutex_unlock(&log_lock);
}
```

**Test the logger in isolation** by writing a small `test_log.c`, compiling it with `gcc -pthread src/common/log.c test_log.c -o test_log`, and spawning a few pthreads that all call `LOG_INFO`. Confirm that output lines are not interleaved.

---

## Phase 3: Build the Crawler

---

### Step 5: CLI Argument Parsing

The crawler's `main()` must parse its command-line arguments before doing anything else. Parse with `getopt_long()` from `<getopt.h>`.

In `src/crawler/main.c`, define a config struct and parse into it:

```c
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *seed_url;      /* --seed */
    int   max_depth;     /* --max-depth */
    int   max_pages;     /* --max-pages */
    int   num_threads;   /* -t */
    char *out_dir;       /* --out */
    char *ipc_path;      /* --ipc */
} CrawlerConfig;

static void print_usage(const char *prog) {
    fprintf(stderr,
        "USAGE: %s --seed <url> --max-depth <D> --max-pages <N>"
        " -t <threads> --out <dir> --ipc <path>\n", prog);
}

static CrawlerConfig parse_args(int argc, char **argv) {
    CrawlerConfig cfg = { NULL, 3, 100, 4, NULL, NULL };

    static struct option opts[] = {
        { "seed",      required_argument, 0, 's' },
        { "max-depth", required_argument, 0, 'd' },
        { "max-pages", required_argument, 0, 'n' },
        { "out",       required_argument, 0, 'o' },
        { "ipc",       required_argument, 0, 'i' },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:h", opts, NULL)) != -1) {
        switch (c) {
            case 's': cfg.seed_url     = optarg;         break;
            case 'd': cfg.max_depth    = atoi(optarg);   break;
            case 'n': cfg.max_pages    = atoi(optarg);   break;
            case 't': cfg.num_threads  = atoi(optarg);   break;
            case 'o': cfg.out_dir      = optarg;         break;
            case 'i': cfg.ipc_path     = optarg;         break;
            case 'h': print_usage(argv[0]); exit(0);
            default:  print_usage(argv[0]); exit(1);
        }
    }

    /* Validate required arguments */
    if (!cfg.seed_url || !cfg.out_dir || !cfg.ipc_path) {
        print_usage(argv[0]);
        exit(1);
    }
    return cfg;
}
```

Do the same pattern for `indexer/main.c` (flags: `--ipc`, `--out`) and `query/main.c` (flags: `--index`, plus remaining `argv` as query terms).

---

### Step 6: Build the Bounded, Thread-Safe URL Queue

This is the most critical data structure. Worker threads pop URLs to fetch; the main thread and worker threads push newly discovered URLs. The queue must block producers when full (backpressure) and block consumers when empty.

Create `src/crawler/queue.h`:

```c
#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <pthread.h>

typedef struct {
    char          **items;          /* Ring buffer of URL strings (heap-allocated) */
    int             capacity;       /* Maximum number of items */
    int             head;           /* Index of next item to pop */
    int             tail;           /* Index where next item will be pushed */
    int             count;          /* Current number of items */
    int             max_depth_seen; /* Track peak depth for summary */
    int             shutdown;       /* Set to 1 to wake all blocked threads */
    pthread_mutex_t lock;
    pthread_cond_t  not_full;       /* Signaled when an item is removed */
    pthread_cond_t  not_empty;      /* Signaled when an item is added */

    /* Statistics */
    int             max_count_ever; /* High-water mark of count */
} URLQueue;

URLQueue *queue_create(int capacity);
void      queue_destroy(URLQueue *q);

/*
 * queue_push  — blocks if full; returns 0 on success, -1 if shutdown.
 * queue_pop   — blocks if empty; returns heap-allocated string or NULL if shutdown.
 * queue_shutdown — wakes all blocked threads so they can exit cleanly.
 */
int   queue_push(URLQueue *q, const char *url);
char *queue_pop(URLQueue *q);
void  queue_shutdown(URLQueue *q);

#endif /* QUEUE_H */
```

Create `src/crawler/queue.c`:

```c
#include "queue.h"
#include <stdlib.h>
#include <string.h>

URLQueue *queue_create(int capacity) {
    URLQueue *q    = calloc(1, sizeof(URLQueue));
    q->items       = calloc(capacity, sizeof(char *));
    q->capacity    = capacity;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_full,  NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void queue_destroy(URLQueue *q) {
    for (int i = 0; i < q->count; i++)
        free(q->items[(q->head + i) % q->capacity]);
    free(q->items);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    free(q);
}

int queue_push(URLQueue *q, const char *url) {
    pthread_mutex_lock(&q->lock);
    while (q->count == q->capacity && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->lock);

    if (q->shutdown) { pthread_mutex_unlock(&q->lock); return -1; }

    q->items[q->tail] = strdup(url);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    if (q->count > q->max_count_ever) q->max_count_ever = q->count;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

char *queue_pop(URLQueue *q) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0) { pthread_mutex_unlock(&q->lock); return NULL; }

    char *url   = q->items[q->head];
    q->items[q->head] = NULL;
    q->head     = (q->head + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return url;   /* caller must free() */
}

void queue_shutdown(URLQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}
```

**Unit test this before moving on.** Write a test that spawns 4 producer threads and 4 consumer threads pushing 10,000 URLs each through a queue of capacity 64. Confirm every URL is received exactly once and no deadlock occurs.

---

### Step 7: Build the Thread-Safe Visited Set

You need to track which URLs have already been fetched so you never crawl the same page twice. The check-and-insert operation must be **atomic** — it is a classic TOCTOU bug if you check membership and then insert in two separate lock acquisitions.

Create `src/crawler/visited.h`:

```c
#ifndef VISITED_H
#define VISITED_H

#include <pthread.h>

#define VISITED_BUCKETS 65536   /* Must be a power of 2 */

typedef struct VisitedNode {
    char               *url;
    struct VisitedNode *next;
} VisitedNode;

typedef struct {
    VisitedNode    *buckets[VISITED_BUCKETS];
    pthread_mutex_t lock;
    int             count;
} VisitedSet;

VisitedSet *visited_create(void);
void        visited_destroy(VisitedSet *vs);

/*
 * visited_check_and_insert
 *   Returns 1 if the URL was ALREADY seen (skip it).
 *   Returns 0 if the URL was NEW and has now been inserted (process it).
 * The check and insert happen under the same lock acquisition.
 */
int visited_check_and_insert(VisitedSet *vs, const char *url);

#endif /* VISITED_H */
```

Create `src/crawler/visited.c`:

```c
#include "visited.h"
#include <stdlib.h>
#include <string.h>

/* djb2 hash */
static unsigned long hash_url(const char *url) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*url++))
        h = ((h << 5) + h) + c;
    return h;
}

VisitedSet *visited_create(void) {
    VisitedSet *vs = calloc(1, sizeof(VisitedSet));
    pthread_mutex_init(&vs->lock, NULL);
    return vs;
}

void visited_destroy(VisitedSet *vs) {
    for (int i = 0; i < VISITED_BUCKETS; i++) {
        VisitedNode *n = vs->buckets[i];
        while (n) { VisitedNode *tmp = n->next; free(n->url); free(n); n = tmp; }
    }
    pthread_mutex_destroy(&vs->lock);
    free(vs);
}

int visited_check_and_insert(VisitedSet *vs, const char *url) {
    unsigned long idx = hash_url(url) & (VISITED_BUCKETS - 1);

    pthread_mutex_lock(&vs->lock);
    for (VisitedNode *n = vs->buckets[idx]; n; n = n->next) {
        if (strcmp(n->url, url) == 0) {
            pthread_mutex_unlock(&vs->lock);
            return 1;   /* Already seen */
        }
    }
    /* Not found — insert */
    VisitedNode *node = malloc(sizeof(VisitedNode));
    node->url         = strdup(url);
    node->next        = vs->buckets[idx];
    vs->buckets[idx]  = node;
    vs->count++;
    pthread_mutex_unlock(&vs->lock);
    return 0;   /* Freshly inserted */
}
```

---

### Step 8: Build the Thread Pool

The thread pool consists of `N` worker threads that are created once at startup and reused for the lifetime of the crawl. There is no thread-per-URL.

Create `src/crawler/threadpool.h`:

```c
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "queue.h"
#include "visited.h"
#include "../common/ipc.h"
#include <pthread.h>
#include <stdint.h>

/* Arguments passed to every worker thread */
typedef struct {
    URLQueue   *url_queue;
    VisitedSet *visited;
    int         max_depth;
    int         ipc_fd;        /* Connected UNIX socket fd to indexer */
    char       *out_dir;

    /* Shared atomic counters (protect with a mutex or use __atomic builtins) */
    int        *pages_fetched;
    int        *pages_failed;
    int         max_pages;
    pthread_mutex_t *counter_lock;
} WorkerArgs;

typedef struct {
    pthread_t  *threads;
    int         num_threads;
    WorkerArgs  args;          /* Shared args struct passed to all threads */
} ThreadPool;

ThreadPool *threadpool_create(int n, WorkerArgs args);
void        threadpool_wait_and_destroy(ThreadPool *tp);

/* The actual worker function — implemented in fetch.c or threadpool.c */
void *worker_thread(void *arg);

#endif /* THREADPOOL_H */
```

In `threadpool_create()`, call `pthread_create(&tp->threads[i], NULL, worker_thread, &tp->args)` for each thread. In `threadpool_wait_and_destroy()`, call `pthread_join()` on each thread, then free the pool.

The `worker_thread` function (implemented in Step 9 after you have fetch/parse) will:

```
loop:
    url = queue_pop(url_queue)   // blocks until URL available or shutdown
    if url == NULL: break        // shutdown signal received
    fetch the page
    parse links, enqueue new URLs
    save page to disk
    send IPC message to indexer
    free(url)
```

---

### Step 9: Implement Fetching with libcurl

Each worker thread has its own `CURL *` handle (not shared — libcurl handles are not thread-safe). Initialize one handle per thread at thread startup and reuse it for every fetch that thread performs.

Create `src/crawler/fetch.h`:

```c
#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>

typedef struct {
    char  *data;    /* Heap-allocated response body */
    size_t size;    /* Number of bytes in data */
} FetchResult;

/*
 * fetch_url — downloads the URL into a heap buffer.
 *   Returns 0 on success (caller must free result->data).
 *   Returns -1 on any error (timeout, HTTP error, etc.) — logs the failure.
 */
int fetch_url(void *curl_handle, const char *url, FetchResult *result);

/* Write callback used internally by libcurl */
size_t fetch_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata);

#endif /* FETCH_H */
```

Create `src/crawler/fetch.c`:

```c
#include "fetch.h"
#include "../common/log.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

size_t fetch_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FetchResult *res  = userdata;
    size_t       total = size * nmemb;
    res->data         = realloc(res->data, res->size + total + 1);
    memcpy(res->data + res->size, ptr, total);
    res->size        += total;
    res->data[res->size] = '\0';
    return total;
}

int fetch_url(void *handle, const char *url, FetchResult *result) {
    CURL *curl = handle;
    result->data = malloc(1);
    result->size = 0;

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  fetch_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);   /* 10s connect */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);   /* 30s total */
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "MyCrawler/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG_WARN("curl fetch failed for %s: %s", url, curl_easy_strerror(res));
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
```

In each worker thread, at thread start:
```c
CURL *curl = curl_easy_init();
// ... do all fetches with this handle ...
curl_easy_cleanup(curl);
```

Call `curl_global_init(CURL_GLOBAL_ALL)` once in `main()` before creating threads, and `curl_global_cleanup()` after all threads have joined.

---

### Step 10: HTML Parsing — Extract Links and Text

After fetching a page, you need to find all outgoing `<a href="...">` links and extract the visible text (for the indexer to tokenize).

**Approach A (simple, sufficient):** Scan the HTML with `strstr()` for `href=` and extract the quoted value. Handles ~95% of real pages.

**Approach B (robust):** Use `libxml2`'s HTML parser. Add `-lxml2` to `LDFLAGS` and `#include <libxml/HTMLparser.h>`.

For this project, Approach A is acceptable. Create `src/crawler/parse.c`:

```c
#include "parse.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/*
 * extract_links — scans html for href="..." and href='...' patterns.
 *   Calls callback(url, user_data) for each raw href found.
 *   The callback receives a heap-allocated string; caller frees it.
 */
void extract_links(const char *html, LinkCallback callback, void *user_data) {
    const char *p = html;
    while ((p = strcasestr(p, "href=")) != NULL) {
        p += 5;   /* skip "href=" */
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        if (!quote) continue;

        const char *start = p;
        while (*p && *p != quote) p++;
        if (*p != quote) break;

        size_t len = p - start;
        char  *href = strndup(start, len);
        callback(href, user_data);
        free(href);
        p++;
    }
}
```

**URL normalization** — implement `normalize_url(base_url, href)` in the same file. This function must:

1. If `href` starts with `http://` or `https://`, use it as-is.
2. If `href` starts with `//`, prepend the scheme from `base_url`.
3. If `href` starts with `/`, prepend `scheme://host` from `base_url`.
4. If `href` is relative (no leading `/`), resolve against the base URL's directory.
5. Strip `#fragment` suffixes.
6. Return `NULL` for `mailto:`, `javascript:`, `ftp:`, and other non-HTTP schemes.

Use `libcurl`'s `CURLU` API for robust URL parsing:

```c
#include <curl/curl.h>

char *normalize_url(const char *base, const char *href) {
    CURLU *cu = curl_url();
    curl_url_set(cu, CURLUPART_URL, base, 0);
    curl_url_set(cu, CURLUPART_URL, href, CURLU_ALLOW_SPACE);

    char *scheme = NULL, *full = NULL;
    curl_url_get(cu, CURLUPART_SCHEME, &scheme, 0);
    if (!scheme || (strcmp(scheme,"http")!=0 && strcmp(scheme,"https")!=0)) {
        curl_free(scheme); curl_url_cleanup(cu); return NULL;
    }
    curl_url_set(cu, CURLUPART_FRAGMENT, NULL, 0);  /* strip fragment */
    curl_url_get(cu, CURLUPART_URL, &full, 0);

    char *result = strdup(full);
    curl_free(scheme); curl_free(full);
    curl_url_cleanup(cu);
    return result;
}
```

---

### Step 11: Persist Pages to Disk

After a successful fetch, save the HTML (or extracted text) to `<out_dir>/pages/<docid>.html`.

**DocID generation** — use a shared atomic counter. Increment it safely across threads:

```c
/* In a shared location, e.g. crawler state struct */
static int          next_docid   = 0;
static pthread_mutex_t docid_lock = PTHREAD_MUTEX_INITIALIZER;

int allocate_docid(void) {
    pthread_mutex_lock(&docid_lock);
    int id = next_docid++;
    pthread_mutex_unlock(&docid_lock);
    return id;
}
```

Or use GCC atomics: `int id = __atomic_fetch_add(&next_docid, 1, __ATOMIC_SEQ_CST);`

**Page limit enforcement** — decrement `pages_remaining` atomically *before* fetching. If it reaches zero, call `queue_shutdown()` to stop all threads. Do this check inside the worker loop, not after:

```c
/* Inside worker_thread, before fetching: */
pthread_mutex_lock(args->counter_lock);
if (*args->pages_fetched >= args->max_pages) {
    pthread_mutex_unlock(args->counter_lock);
    queue_shutdown(args->url_queue);
    break;
}
pthread_mutex_unlock(args->counter_lock);
```

**Saving to disk:**

```c
#include <stdio.h>

int save_page(const char *out_dir, int docid, const char *html) {
    char path[512];
    snprintf(path, sizeof(path), "%s/pages/%d.html", out_dir, docid);
    FILE *fp = fopen(path, "w");
    if (!fp) { LOG_ERROR("Cannot open %s for writing", path); return -1; }
    fputs(html, fp);
    fclose(fp);
    return 0;
}
```

---

### Step 12: Send IPC Messages to the Indexer

After saving a page, the worker thread sends a `CrawlMsg` to the indexer. Because multiple threads share the socket file descriptor, protect sends with a mutex.

```c
/* In crawler state, visible to all workers */
static pthread_mutex_t ipc_lock = PTHREAD_MUTEX_INITIALIZER;

int send_ipc_msg(int sockfd, const CrawlMsg *msg) {
    pthread_mutex_lock(&ipc_lock);
    ssize_t sent = 0, total = sizeof(CrawlMsg);
    const char *buf = (const char *)msg;
    while (sent < total) {
        ssize_t n = send(sockfd, buf + sent, total - sent, 0);
        if (n <= 0) { pthread_mutex_unlock(&ipc_lock); return -1; }
        sent += n;
    }
    pthread_mutex_unlock(&ipc_lock);
    return 0;
}
```

Fill the message in the worker:

```c
CrawlMsg msg = {0};
msg.docid  = docid;
msg.depth  = current_depth;
strncpy(msg.url,      url,      IPC_URL_MAX  - 1);
strncpy(msg.filepath, filepath, IPC_PATH_MAX - 1);
send_ipc_msg(ipc_fd, &msg);
```

**Connecting to the indexer** — in `crawler main()`, connect before spawning threads:

```c
#include <sys/socket.h>
#include <sys/un.h>

int connect_to_indexer(const char *ipc_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, ipc_path, sizeof(addr.sun_path)-1);

    /* Retry loop — indexer might not be ready yet */
    for (int i = 0; i < 10; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        sleep(1);
    }
    fprintf(stderr, "Failed to connect to indexer at %s\n", ipc_path);
    exit(1);
}
```

---

### Step 13: Graceful Shutdown and Summary

**Stop conditions (all must be race-free):**

1. `--max-pages` reached → call `queue_shutdown()` from inside the worker that hits the limit.
2. URL frontier exhausted → detect with an idle-thread counter: when `active_workers == 0 && queue.count == 0`, call `queue_shutdown()`.
3. Ctrl+C → install a `SIGINT` handler that sets a global `volatile sig_atomic_t shutdown_flag = 1`, then calls `queue_shutdown()`.

**Idle-thread detection** — track active workers with a shared counter:

```c
pthread_mutex_lock(&idle_lock);
active_workers--;
if (active_workers == 0 && queue->count == 0) {
    queue_shutdown(queue);
}
pthread_mutex_unlock(&idle_lock);
```

Increment `active_workers` when a thread picks up a URL, decrement when it finishes processing.

**Final summary** — after `pthread_join()` on all threads:

```c
printf("=== Crawl Summary ===\n");
printf("Pages fetched:   %d\n", pages_fetched);
printf("Pages skipped:   %d\n", pages_skipped);
printf("Pages failed:    %d\n", pages_failed);
printf("Max queue depth: %d\n", url_queue->max_count_ever);
printf("Runtime:         %.2f seconds\n", elapsed_seconds);
```

Then send the IPC sentinel and close the socket:

```c
CrawlMsg sentinel = { .docid = IPC_SENTINEL_DOCID };
send_ipc_msg(ipc_fd, &sentinel);
close(ipc_fd);
```

---

## Phase 4: Build the Indexer

---

### Step 14: Accept the IPC Connection and Receive Messages

The indexer starts first and waits for the crawler to connect. Implement this in `src/indexer/main.c`.

```c
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int create_server_socket(const char *ipc_path) {
    unlink(ipc_path);   /* Remove stale socket file if any */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, ipc_path, sizeof(addr.sun_path)-1);
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(fd, 1);
    return fd;
}

/* In main(): */
int server_fd = create_server_socket(cfg.ipc_path);
printf("Indexer listening on %s ...\n", cfg.ipc_path);
int client_fd = accept(server_fd, NULL, NULL);   /* Blocks until crawler connects */
```

**Receive loop:**

```c
#include "../common/ipc.h"

CrawlMsg msg;
while (1) {
    ssize_t n = recv_full(client_fd, &msg, sizeof(msg));
    if (n <= 0) break;                              /* Crawler closed the connection */
    if (msg.docid == IPC_SENTINEL_DOCID) break;    /* Explicit end-of-stream signal */
    process_document(&msg);
}
```

Implement `recv_full()` as a loop that keeps calling `recv()` until all bytes arrive (handle partial reads):

```c
ssize_t recv_full(int fd, void *buf, size_t len) {
    size_t  got = 0;
    char   *p   = buf;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return n;
        got += n;
    }
    return got;
}
```

---

### Step 15: Read Saved Pages and Tokenize

When `process_document()` is called, it reads the saved HTML file and tokenizes it into words.

```c
#include <ctype.h>
#include <string.h>

/*
 * tokenize — lowercases and splits text into words.
 *   Calls callback(word, user_data) for each word found.
 *   Words consist only of [a-z0-9] characters after lowercasing.
 */
void tokenize(const char *text, TokenCallback callback, void *user_data) {
    const char *p = text;
    char word[256];
    int  wlen = 0;

    while (*p) {
        if (isalnum((unsigned char)*p)) {
            if (wlen < (int)sizeof(word) - 1)
                word[wlen++] = tolower((unsigned char)*p);
        } else {
            if (wlen >= 3) {       /* Skip very short tokens */
                word[wlen] = '\0';
                if (!is_stopword(word))
                    callback(word, user_data);
            }
            wlen = 0;
        }
        p++;
    }
    if (wlen >= 3) { word[wlen] = '\0'; callback(word, user_data); }
}
```

A minimal stop word list (hard-code an array of strings):

```c
static const char *STOPWORDS[] = {
    "the","and","for","are","but","not","you","all",
    "can","had","her","was","one","our","out","his",
    NULL
};

int is_stopword(const char *word) {
    for (int i = 0; STOPWORDS[i]; i++)
        if (strcmp(word, STOPWORDS[i]) == 0) return 1;
    return 0;
}
```

---

### Step 16: Build the In-Memory Inverted Index

Use a hash map from `term → PostingsList`. In C, implement a simple chained hash map:

```c
#define INDEX_BUCKETS 131072   /* Power of 2 */

typedef struct Posting {
    uint32_t       docid;
    struct Posting *next;
} Posting;

typedef struct IndexEntry {
    char              *term;
    Posting           *postings;     /* Linked list of docids */
    int                df;           /* Document frequency */
    struct IndexEntry *next;         /* Hash chain */
} IndexEntry;

typedef struct {
    IndexEntry *buckets[INDEX_BUCKETS];
    int         num_terms;
} InvertedIndex;
```

Implement:
- `index_insert(InvertedIndex *idx, const char *term, uint32_t docid)` — hash the term, walk the chain to find or create an `IndexEntry`, append a `Posting`.
- Avoid duplicate docids per term: before appending, check if the tail posting already has this docid.

In C++, you can use `std::unordered_map<std::string, std::vector<uint32_t>>` instead, which is much simpler to implement and sufficient for this project.

---

### Step 17: Maintain the Document Map

Every time a `CrawlMsg` arrives, append one line to `index/docs.tsv`:

```c
void append_doc_map(const char *index_dir, const CrawlMsg *msg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/docs.tsv", index_dir);
    FILE *fp = fopen(path, "a");   /* Append mode */
    if (!fp) { LOG_ERROR("Cannot open docs.tsv"); return; }
    fprintf(fp, "%u\t%s\t%s\t%u\n",
            msg->docid, msg->url, msg->filepath, msg->depth);
    fclose(fp);
}
```

Opening in append mode and closing after each write is safe enough for this project. If performance matters, keep the file open for the duration and `fflush()` periodically.

---

### Step 18: Flush the Index to Disk on Shutdown

When the receive loop ends (sentinel or connection closed), write the full in-memory index to disk.

**Format for `index/postings.bin`** — write raw `uint32_t` docids:

```c
void write_index(InvertedIndex *idx, const char *index_dir) {
    char dict_path[512], post_path[512];
    snprintf(dict_path, sizeof(dict_path), "%s/dict.tsv",     index_dir);
    snprintf(post_path, sizeof(post_path), "%s/postings.bin", index_dir);

    FILE *dict_fp = fopen(dict_path, "w");
    FILE *post_fp = fopen(post_path, "wb");

    long offset = 0;
    for (int i = 0; i < INDEX_BUCKETS; i++) {
        for (IndexEntry *e = idx->buckets[i]; e; e = e->next) {
            /* Write term metadata to dict.tsv */
            fprintf(dict_fp, "%s\t%ld\t%d\n", e->term, offset, e->df);

            /* Write postings list to postings.bin */
            for (Posting *p = e->postings; p; p = p->next) {
                fwrite(&p->docid, sizeof(uint32_t), 1, post_fp);
                offset += sizeof(uint32_t);
            }
        }
    }
    fclose(dict_fp);
    fclose(post_fp);
    LOG_INFO("Index flushed: %d terms", idx->num_terms);
}
```

---

## Phase 5: Build the Query Tool

---

### Step 19: Load the Index from Disk

The query tool runs independently — it loads the index files on startup and answers queries.

```c
typedef struct {
    char    *term;
    long     offset;   /* Byte offset in postings.bin */
    int      df;       /* Number of postings (document frequency) */
} DictEntry;

typedef struct {
    DictEntry *entries;
    int        count;
    /* doc map: docid -> url (use a dynamic array or hash map) */
    char     **doc_urls;
    int        doc_count;
    FILE      *postings_fp;
} Index;
```

Loading `dict.tsv`:
```c
Index *index_load(const char *index_dir) {
    /* Read dict.tsv line by line with fgets + sscanf */
    /* Read docs.tsv to populate doc_urls[] */
    /* fopen postings.bin for random-access reads */
}
```

For a fast term lookup, sort the `DictEntry` array by term after loading and use `bsearch()`.

---

### Step 20: Execute AND Queries

For each query term, retrieve its postings list. Intersect all lists.

```c
uint32_t *load_postings(Index *idx, const DictEntry *entry) {
    uint32_t *list = malloc(entry->df * sizeof(uint32_t));
    fseek(idx->postings_fp, entry->offset, SEEK_SET);
    fread(list, sizeof(uint32_t), entry->df, idx->postings_fp);
    return list;   /* Caller frees */
}

/*
 * intersect — takes two SORTED arrays and returns their intersection.
 * Use two-pointer technique: O(m + n).
 * If postings are not stored sorted, sort them after loading.
 */
```

Main query flow in `src/query/main.c`:

```c
/* Parse --index and remaining argv as query terms */
/* Load index */
/* For each term, look it up in dict; if missing, print "No results" and exit */
/* Load all postings lists */
/* Intersect them all */
/* Print results: "Found N matching documents" then docid + URL per line */
/* If intersection is empty: "No documents matched all query terms." */
```

---

## Phase 6: Testing & Integration

---

### Step 21: Unit Test Each Component in Isolation

Write small standalone test programs for each component **before** integration:

**Queue test** — spawn 8 threads (4 producers, 4 consumers), push 100,000 items through a capacity-64 queue, assert count received equals count sent.

**Visited set test** — spawn 16 threads each inserting 10,000 URLs, assert no duplicate returns of `0` for the same URL.

**Tokenizer test** — feed known HTML strings, assert specific words appear in output, assert stop words do not.

**Index flush/load test** — insert 1,000 terms with known postings, flush to disk, reload with the query tool, assert all terms and postings match.

Run with ThreadSanitizer:
```bash
gcc -fsanitize=thread -g -pthread src/crawler/queue.c test_queue.c -o test_queue
./test_queue
```

Run with Valgrind:
```bash
valgrind --error-exitcode=1 --leak-check=full ./test_queue
```

---

### Step 22: End-to-End Integration Test

Run the exact sequence from the project spec:

```bash
# Terminal 1
./indexer --ipc /tmp/crawl.sock --out data/index

# Terminal 2 (after indexer is listening)
./crawler --seed https://en.wikipedia.org/wiki/Linux \
          --max-depth 2 --max-pages 50 -t 4 \
          --out data --ipc /tmp/crawl.sock

# Terminal 3 (after crawl finishes)
./query --index data/index operating systems threads
```

Verify:
- `data/pages/` contains `.html` files named by docid
- `data/index/docs.tsv` has one line per fetched page
- `data/index/dict.tsv` and `data/index/postings.bin` exist and are non-empty
- Query returns reasonable results
- No crashes, no zombie processes

---

### Step 23: Stress Test for Race Conditions

Run with a high thread count to expose races:

```bash
./crawler --seed https://en.wikipedia.org/wiki/Computer_science \
          --max-depth 3 --max-pages 500 -t 16 \
          --out data --ipc /tmp/crawl.sock
```

Compile with ThreadSanitizer for the full binary:
```bash
# In Makefile, add to CFLAGS:
CFLAGS += -fsanitize=thread
LDFLAGS += -fsanitize=thread
```

Run and inspect TSan output. Fix all reported data races before submitting. Common races to watch for:
- Unprotected reads of `pages_fetched` or `pages_failed`
- Writing to a shared file without a lock
- Reading `queue->count` outside the queue's lock

---

## Phase 7: Polish & Deliverables

---

### Step 24: Write README.md

Your README must cover the following sections. Graders read this to understand your design decisions.

```markdown
# Multithreaded Web Crawler Pipeline

## Build & Run
\`\`\`bash
make all
./indexer --ipc /tmp/crawl.sock --out data/index
./crawler --seed https://example.com --max-depth 3 --max-pages 500 -t 8 \
          --out data --ipc /tmp/crawl.sock
./query --index data/index term1 term2
\`\`\`

## URL Queue Design
Bounded ring buffer of capacity N, protected by a mutex and two condition
variables (`not_full`, `not_empty`). Producers block when the queue is full;
consumers block when empty. A `shutdown` flag is set atomically under the
lock; `pthread_cond_broadcast` wakes all blocked threads so they can exit.

## Visited Set Design
Open-addressing hash set with 65,536 buckets using djb2 hashing. A single
mutex protects the entire set. The `check_and_insert` operation is performed
under one lock acquisition to make it atomic, preventing TOCTOU races.

## IPC Protocol
UNIX domain stream socket. The crawler connects to the indexer's bound
socket path. Each message is a fixed-size `CrawlMsg` struct (docid, depth,
url, filepath). Fixed sizing eliminates framing complexity. A sentinel
message (docid = UINT32_MAX) signals end-of-stream.

## Index Format
- `pages/<docid>.html`  — raw downloaded HTML
- `index/docs.tsv`      — docid TAB url TAB filepath TAB depth (one line per doc)
- `index/dict.tsv`      — term TAB byte_offset TAB df (sorted alphabetically)
- `index/postings.bin`  — raw uint32_t docid values, concatenated per term

The query tool seeks to `byte_offset` in `postings.bin` and reads `df`
uint32_t values for a given term's postings list.

## Stretch Features
(List any extras you implemented, e.g. TF-IDF scoring, robots.txt
compliance, rate limiting per domain, parallel indexing, compressed postings)
```

---

### Step 25: Final Checklist

Go through this before submitting:

- [ ] `make all` compiles cleanly with `-Wall -Wextra` and zero warnings
- [ ] `make clean` removes all build artifacts, binaries, and generated data
- [ ] `make run` runs a complete demo end-to-end without manual intervention
- [ ] Signal handler for `SIGINT` (Ctrl+C) triggers graceful shutdown — no hung processes
- [ ] `valgrind --leak-check=full ./crawler ...` reports zero memory leaks
- [ ] ThreadSanitizer reports zero data races on a 16-thread run
- [ ] `./crawler -h` prints the usage string from the spec exactly
- [ ] Query correctly prints "No documents matched all query terms." for unknown terms
- [ ] `data/index/` persists correctly — you can kill and restart the query tool and get the same results
- [ ] README.md explains all four required design areas: queue, visited set, IPC, index format
- [ ] All source files have header comment blocks with your name and a brief description

---

## Quick Reference: Data Flow Summary

```
main()
  ect_to_indexer(ipc_path)         # UNIX socket connect
  → visited_check_and_insert(seed_url)   # Mark seed as visited
  → queue_push(url_queue, seed_url)      # Seed the queue
  → threadpool_create(N, args)           # Spawn N worker threads

worker_thread() [runs N times in parallel]:
  loop:
    url ← queue_pop()                    # Blocks if empty
    if url == NULL: exit thread
    check pages_fetched >= max_pages → queue_shutdown() + break
    html ← fetch_url(curl, url)
    if failed: log + continue
    docid ← allocate_docid()
    save_page(out_dir, docid, html)
    links ← extract_links(html)
    for each link:
        norm ← normalize_url(url, link)
        if norm && depth+1 <= max_depth && !visited(norm):
            queue_push(norm)
    send_ipc_msg(ipc_fd, {docid, url, filepath, depth})
    update counters

main() continues:
  → pthread_join(all threads)
  → send IPC sentinel
  → close(ipc_fd)
  → print summary
```
