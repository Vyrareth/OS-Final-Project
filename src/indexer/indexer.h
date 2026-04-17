#ifndef INDEXER_H
#define INDEXER_H

#include <stdint.h>

#define INDEX_BUCKETS 131072   /* Power of 2 */

typedef struct Posting {
    uint32_t        docid;
    struct Posting *next;
} Posting;

typedef struct IndexEntry {
    char              *term;
    Posting           *postings;   /* Linked list of docids */
    int                df;         /* Document frequency */
    struct IndexEntry *next;       /* Hash chain */
} IndexEntry;

typedef struct {
    IndexEntry *buckets[INDEX_BUCKETS];
    int         num_terms;
} InvertedIndex;

InvertedIndex *index_create(void);
void           index_destroy(InvertedIndex *idx);

/* Insert docid into term's postings list (no duplicates per doc). */
void index_insert(InvertedIndex *idx, const char *term, uint32_t docid);

/*
 * tokenize — strips HTML tags, lowercases, splits into words of length >= 3,
 * filters stop words, and calls callback(word, user_data) for each token.
 */
typedef void (*TokenCallback)(const char *word, void *user_data);
void tokenize(const char *text, TokenCallback callback, void *user_data);

int is_stopword(const char *word);

/*
 * write_index — flushes the in-memory index to disk:
 *   <index_dir>/dict.tsv      term TAB byte_offset TAB df
 *   <index_dir>/postings.bin  raw uint32_t docids, concatenated per term
 */
void write_index(InvertedIndex *idx, const char *index_dir);

#endif /* INDEXER_H */
