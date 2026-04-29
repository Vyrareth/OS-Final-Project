#ifndef THREADPOOL_H
#define THREADPOOL_H

/*
 * threadpool.h — declares the worker thread pool and the shared arguments
 * passed to every worker thread. The thread pool is the heart of the crawler —
 * it manages N worker threads that all share the same URL queue and visited set.
 */

#include "queue.h"        /* for URLQueue — the shared URL queue */
#include "visited.h"      /* for VisitedSet — tracks which URLs were already crawled */
#include "../common/ipc.h" /* for CrawlMsg — the message sent to the indexer */
#include <pthread.h>       /* for pthread_t, pthread_mutex_t */
#include <stdint.h>        /* for uint32_t */

/*
 * WorkerArgs — all the shared data that every worker thread needs.
 * One copy of this struct is created in main.c and passed to ALL threads.
 * Every thread reads from and writes to the same WorkerArgs — that's why
 * we need mutexes to protect the counters and the IPC socket.
 */
typedef struct {
    URLQueue        *url_queue;     /* shared queue — threads pop URLs from here */
    VisitedSet      *visited;       /* shared visited set — prevents duplicate crawls */
    int              max_depth;     /* maximum crawl depth (e.g. --max-depth 1) */
    int              ipc_fd;        /* file descriptor for the UNIX socket to indexer */
    char            *out_dir;       /* output directory where pages are saved */

    /* Page counters — shared by all threads, protected by counter_lock */
    int             *pages_fetched;  /* total pages successfully downloaded */
    int             *pages_failed;   /* total pages that failed (404, timeout, etc.) */
    int             *pages_skipped;  /* total pages skipped (already visited or limit hit) */
    int              max_pages;      /* maximum pages to crawl (e.g. --max-pages 50) */
    int             *next_docid;     /* next unique ID to assign to a page (0, 1, 2...) */
    pthread_mutex_t *counter_lock;   /* mutex protecting all the counters above */

    /* IPC serialization — protects the socket so only one thread sends at a time */
    pthread_mutex_t *ipc_lock;       /* mutex protecting the IPC socket */
} WorkerArgs;

/*
 * ThreadPool — holds all the worker threads and their shared arguments.
 * Created once in main.c, destroyed after all threads finish.
 */
typedef struct {
    pthread_t  *threads;     /* array of thread IDs (one per worker) */
    int         num_threads; /* total number of worker threads (e.g. 8) */
    WorkerArgs  args;        /* shared arguments passed to all threads */
} ThreadPool;

/*
 * threadpool_create — creates N worker threads and starts them running.
 * n    — number of worker threads to create (e.g. 8)
 * args — the shared data all threads will use
 * Returns a heap-allocated ThreadPool pointer.
 */
ThreadPool *threadpool_create(int n, WorkerArgs args);

/*
 * threadpool_wait_and_destroy — waits for ALL threads to finish then frees memory.
 * Uses pthread_join to wait for each thread.
 * Called in main.c after queue_shutdown() to clean up.
 */
void threadpool_wait_and_destroy(ThreadPool *tp);

/*
 * worker_thread — the function each worker thread runs.
 * Each thread loops: pop URL → fetch → save → extract links → send IPC → repeat
 * Stops when queue_pop returns -1 (queue is shut down and empty).
 * arg — pointer to the shared WorkerArgs struct
 */
void *worker_thread(void *arg);

#endif /* THREADPOOL_H */
