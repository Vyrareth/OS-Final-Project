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
 * visited_check_and_insert — atomic check + insert under one lock acquisition.
 *   Returns 1 if the URL was ALREADY seen (skip it).
 *   Returns 0 if the URL is NEW and has been inserted (process it).
 */
int visited_check_and_insert(VisitedSet *vs, const char *url);

#endif /* VISITED_H */
