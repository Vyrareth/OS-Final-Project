#include "queue.h"
#include <stdlib.h>
#include <string.h>

URLQueue *queue_create(int capacity, int num_workers) {
    URLQueue *q     = calloc(1, sizeof(URLQueue));
    q->items        = calloc((size_t)capacity, sizeof(QueueItem));
    q->capacity     = capacity;
    q->num_workers  = num_workers;
    pthread_mutex_init(&q->lock,     NULL);
    pthread_cond_init(&q->not_full,  NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void queue_destroy(URLQueue *q) {
    /* Free any remaining items */
    for (int i = 0; i < q->count; i++)
        free(q->items[(q->head + i) % q->capacity].url);
    free(q->items);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    free(q);
}

int queue_push(URLQueue *q, const char *url, int depth) {
    pthread_mutex_lock(&q->lock);
    while (q->count == q->capacity && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->lock);

    if (q->shutdown) { pthread_mutex_unlock(&q->lock); return -1; }

    q->items[q->tail].url   = strdup(url);
    q->items[q->tail].depth = depth;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    if (q->count > q->max_count_ever) q->max_count_ever = q->count;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int queue_pop(URLQueue *q, char **url_out, int *depth_out) {
    pthread_mutex_lock(&q->lock);
    q->idle_workers++;  /* mark this thread as waiting */

    while (q->count == 0 && !q->shutdown) {
        /* All workers idle + queue empty → frontier is exhausted */
        if (q->idle_workers == q->num_workers) {
            q->shutdown = 1;
            pthread_cond_broadcast(&q->not_empty);
            break;
        }
        pthread_cond_wait(&q->not_empty, &q->lock);
    }

    q->idle_workers--;

    if (q->count == 0) {
        /* Shutdown with nothing left */
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    QueueItem item = q->items[q->head];
    q->items[q->head].url = NULL;
    q->head  = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);

    *url_out   = item.url;   /* caller must free() */
    *depth_out = item.depth;
    return 0;
}

void queue_shutdown(URLQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

int queue_max_size(URLQueue *q) {
    return q->max_count_ever;
}
