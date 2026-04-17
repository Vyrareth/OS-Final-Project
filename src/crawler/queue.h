#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

/*
 * URLQueue — bounded, thread-safe ring buffer of (url, depth) pairs.
 *
 * Design:
 *  - Mutex + two condition variables (not_full, not_empty) for backpressure.
 *  - idle_workers counter: when idle_workers == num_workers AND count == 0,
 *    the frontier is exhausted and the queue shuts itself down.
 *  - queue_shutdown() wakes all blocked threads for a forced stop.
 */

typedef struct {
    char *url;    /* heap-allocated — caller frees after queue_pop */
    int   depth;
} QueueItem;

typedef struct {
    QueueItem      *items;
    int             capacity;
    int             head;
    int             tail;
    int             count;
    int             max_count_ever; /* high-water mark for summary */
    int             num_workers;    /* total threads that call queue_pop */
    int             idle_workers;   /* threads currently blocked in pop  */
    int             shutdown;
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} URLQueue;

URLQueue *queue_create(int capacity, int num_workers);
void      queue_destroy(URLQueue *q);

/* Returns 0 on success, -1 if the queue is shut down. */
int   queue_push(URLQueue *q, const char *url, int depth);

/*
 * Blocks until a URL is available or the queue shuts down.
 * On success: sets *url_out (caller must free) and *depth_out, returns 0.
 * On shutdown: returns -1.
 */
int   queue_pop(URLQueue *q, char **url_out, int *depth_out);

void  queue_shutdown(URLQueue *q);
int   queue_max_size(URLQueue *q);

#endif /* QUEUE_H */
