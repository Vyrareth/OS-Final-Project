#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "queue.h"
#include "visited.h"
#include "../common/ipc.h"
#include <pthread.h>
#include <stdint.h>

/* Arguments shared by all worker threads */
typedef struct {
    URLQueue        *url_queue;
    VisitedSet      *visited;
    int              max_depth;
    int              ipc_fd;       /* Connected UNIX socket fd to indexer */
    char            *out_dir;

    /* Page counters — all protected by counter_lock */
    int             *pages_fetched;   /* reserved+saved slots */
    int             *pages_failed;
    int             *pages_skipped;
    int              max_pages;
    int             *next_docid;
    pthread_mutex_t *counter_lock;

    /* IPC serialization */
    pthread_mutex_t *ipc_lock;
} WorkerArgs;

typedef struct {
    pthread_t  *threads;
    int         num_threads;
    WorkerArgs  args;
} ThreadPool;

ThreadPool *threadpool_create(int n, WorkerArgs args);
void        threadpool_wait_and_destroy(ThreadPool *tp);

/* Worker function — implemented in threadpool.c */
void *worker_thread(void *arg);

#endif /* THREADPOOL_H */
