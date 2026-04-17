#include "visited.h"
#include <stdlib.h>
#include <string.h>

/* djb2 hash */
static unsigned long hash_url(const char *url) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*url++))
        h = ((h << 5) + h) + (unsigned long)c;
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
        while (n) {
            VisitedNode *tmp = n->next;
            free(n->url);
            free(n);
            n = tmp;
        }
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
    VisitedNode *node = malloc(sizeof(VisitedNode));
    node->url        = strdup(url);
    node->next       = vs->buckets[idx];
    vs->buckets[idx] = node;
    vs->count++;
    pthread_mutex_unlock(&vs->lock);
    return 0;   /* Freshly inserted */
}
