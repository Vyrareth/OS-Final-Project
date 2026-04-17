#include "indexer.h"
#include "../common/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── djb2 hash ───────────────────────────────────────────────────────── */
static unsigned long hash_term(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

/* ── Stop word list ──────────────────────────────────────────────────── */
static const char *STOPWORDS[] = {
    "the","and","for","are","but","not","you","all",
    "can","had","her","was","one","our","out","his",
    "that","this","with","have","from","they","will",
    "been","were","said","each","which","their","time",
    NULL
};

int is_stopword(const char *word) {
    for (int i = 0; STOPWORDS[i]; i++)
        if (strcmp(word, STOPWORDS[i]) == 0) return 1;
    return 0;
}

/* ── Index lifecycle ─────────────────────────────────────────────────── */
InvertedIndex *index_create(void) {
    InvertedIndex *idx = calloc(1, sizeof(InvertedIndex));
    return idx;
}

void index_destroy(InvertedIndex *idx) {
    for (int b = 0; b < INDEX_BUCKETS; b++) {
        IndexEntry *e = idx->buckets[b];
        while (e) {
            IndexEntry *enext = e->next;
            Posting    *p     = e->postings;
            while (p) { Posting *pnext = p->next; free(p); p = pnext; }
            free(e->term);
            free(e);
            e = enext;
        }
    }
    free(idx);
}

/* ── Insert term→docid (no duplicate docids per term) ───────────────── */
void index_insert(InvertedIndex *idx, const char *term, uint32_t docid) {
    unsigned long bkt = hash_term(term) & (INDEX_BUCKETS - 1);

    IndexEntry *e = idx->buckets[bkt];
    while (e) {
        if (strcmp(e->term, term) == 0) break;
        e = e->next;
    }
    if (!e) {
        e           = calloc(1, sizeof(IndexEntry));
        e->term     = strdup(term);
        e->next     = idx->buckets[bkt];
        idx->buckets[bkt] = e;
        idx->num_terms++;
    }

    /* No duplicate docids */
    for (Posting *p = e->postings; p; p = p->next)
        if (p->docid == docid) return;

    Posting *node = malloc(sizeof(Posting));
    node->docid   = docid;
    node->next    = e->postings;
    e->postings   = node;
    e->df++;
}

/* ── Tokenizer ───────────────────────────────────────────────────────── */
void tokenize(const char *text, TokenCallback callback, void *user_data) {
    char word[256];
    int  wlen   = 0;
    int  in_tag = 0;

    for (const char *p = text; *p; p++) {
        if (*p == '<')  { in_tag = 1; wlen = 0; continue; }
        if (*p == '>')  { in_tag = 0; wlen = 0; continue; }
        if (in_tag)     continue;

        if (isalpha((unsigned char)*p)) {
            if (wlen < (int)sizeof(word) - 1)
                word[wlen++] = (char)tolower((unsigned char)*p);
        } else {
            if (wlen >= 3) {
                word[wlen] = '\0';
                if (!is_stopword(word))
                    callback(word, user_data);
            }
            wlen = 0;
        }
    }
    if (wlen >= 3) {
        word[wlen] = '\0';
        if (!is_stopword(word)) callback(word, user_data);
    }
}

/* ── Flush to disk ───────────────────────────────────────────────────── *
 *
 * dict.tsv format:   term TAB byte_offset TAB df
 * postings.bin:      raw uint32_t docids concatenated per term
 *                    (df values come from dict.tsv; no length prefix)
 */
void write_index(InvertedIndex *idx, const char *index_dir) {
    char dict_path[512], post_path[512];
    snprintf(dict_path, sizeof(dict_path), "%s/dict.tsv",     index_dir);
    snprintf(post_path, sizeof(post_path), "%s/postings.bin", index_dir);

    FILE *dict_fp = fopen(dict_path, "w");
    FILE *post_fp = fopen(post_path, "wb");
    if (!dict_fp || !post_fp) {
        LOG_ERROR("Cannot open index files in %s", index_dir);
        if (dict_fp) fclose(dict_fp);
        if (post_fp) fclose(post_fp);
        return;
    }

    long offset = 0;
    for (int i = 0; i < INDEX_BUCKETS; i++) {
        for (IndexEntry *e = idx->buckets[i]; e; e = e->next) {
            fprintf(dict_fp, "%s\t%ld\t%d\n", e->term, offset, e->df);
            for (Posting *p = e->postings; p; p = p->next) {
                fwrite(&p->docid, sizeof(uint32_t), 1, post_fp);
                offset += (long)sizeof(uint32_t);
            }
        }
    }
    fclose(dict_fp);
    fclose(post_fp);
    LOG_INFO("Index flushed: %d terms to %s", idx->num_terms, index_dir);
}
