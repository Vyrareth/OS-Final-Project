#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/types.h>

#include "../common/ipc.h"

/* ══════════════════════════════════════════════════════════════════════
 * Dictionary
 * ══════════════════════════════════════════════════════════════════════ */
#define MAX_TERM_LEN 128

typedef struct {
    char term[MAX_TERM_LEN];
    long offset;   /* byte offset into postings.bin */
    int  df;       /* number of postings = number of docids to read */
} DictEntry;

static DictEntry *dict_entries = NULL;
static int        dict_count   = 0;
static int        dict_cap     = 0;

static int cmp_dict_entry(const void *a, const void *b) {
    return strcmp(((const DictEntry *)a)->term,
                  ((const DictEntry *)b)->term);
}

static void load_dict(const char *index_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/dict.tsv", index_dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return; }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char term[MAX_TERM_LEN];
        long offset;
        int  df;
        if (sscanf(line, "%127s\t%ld\t%d", term, &offset, &df) != 3) continue;

        if (dict_count >= dict_cap) {
            dict_cap = dict_cap ? dict_cap * 2 : 65536;
            dict_entries = realloc(dict_entries,
                                   sizeof(DictEntry) * (size_t)dict_cap);
        }
        strncpy(dict_entries[dict_count].term, term, MAX_TERM_LEN - 1);
        dict_entries[dict_count].term[MAX_TERM_LEN - 1] = '\0';
        dict_entries[dict_count].offset = offset;
        dict_entries[dict_count].df     = df;
        dict_count++;
    }
    fclose(f);

    /* Sort for bsearch() */
    if (dict_count > 0)
        qsort(dict_entries, (size_t)dict_count, sizeof(DictEntry), cmp_dict_entry);
}

static const DictEntry *dict_lookup(const char *term) {
    if (!dict_entries || dict_count == 0) return NULL;
    DictEntry key;
    strncpy(key.term, term, MAX_TERM_LEN - 1);
    key.term[MAX_TERM_LEN - 1] = '\0';
    return bsearch(&key, dict_entries, (size_t)dict_count,
                   sizeof(DictEntry), cmp_dict_entry);
}

/* ══════════════════════════════════════════════════════════════════════
 * Document map: docid → URL
 * ══════════════════════════════════════════════════════════════════════ */
#define MAX_DOCS 100000

static char doc_urls[MAX_DOCS][IPC_URL_MAX];

static void load_docs(const char *index_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/docs.tsv", index_dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return; }

    char line[IPC_URL_MAX + IPC_PATH_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        unsigned int docid, depth;
        char url[IPC_URL_MAX], fp[IPC_PATH_MAX];
        if (sscanf(line, "%u\t%2047[^\t]\t%511[^\t]\t%u",
                   &docid, url, fp, &depth) >= 2) {
            if (docid < MAX_DOCS)
                strncpy(doc_urls[docid], url, IPC_URL_MAX - 1);
        }
    }
    fclose(f);
}

/* ══════════════════════════════════════════════════════════════════════
 * Postings + AND intersection
 * ══════════════════════════════════════════════════════════════════════ */
static uint32_t *load_postings(FILE *pf, const DictEntry *entry) {
    if (fseek(pf, entry->offset, SEEK_SET) != 0) return NULL;
    uint32_t *list = malloc(sizeof(uint32_t) * (size_t)entry->df);
    if (!list) return NULL;
    if ((int)fread(list, sizeof(uint32_t), (size_t)entry->df, pf) != entry->df) {
        free(list); return NULL;
    }
    return list;
}

static int cmp_uint32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/*
 * Two-pointer AND intersection of two sorted arrays.
 * Returns heap-allocated result array; sets *out_n.
 */
static uint32_t *intersect(uint32_t *a, int na,
                            uint32_t *b, int nb,
                            int *out_n) {
    qsort(a, (size_t)na, sizeof(uint32_t), cmp_uint32);
    qsort(b, (size_t)nb, sizeof(uint32_t), cmp_uint32);

    uint32_t *result = malloc(sizeof(uint32_t) *
                              (size_t)(na < nb ? na : nb));
    int i = 0, j = 0, count = 0;
    while (i < na && j < nb) {
        if      (a[i] == b[j]) { result[count++] = a[i]; i++; j++; }
        else if (a[i]  < b[j]) { i++; }
        else                    { j++; }
    }
    *out_n = count;
    return result;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    char index_dir[512] = "data/index";
    int  term_start     = -1;

    static struct option opts[] = {
        { "index", required_argument, 0, 'x' },
        { 0, 0, 0, 0 }
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        if (c == 'x') strncpy(index_dir, optarg, sizeof(index_dir) - 1);
    }
    /* Remaining non-option arguments are query terms */
    term_start = optind;

    if (term_start >= argc) {
        fprintf(stderr, "USAGE: query --index <dir> <term1> [term2 ...]\n");
        return 1;
    }

    memset(doc_urls, 0, sizeof(doc_urls));
    load_docs(index_dir);
    load_dict(index_dir);

    char post_path[1024];
    snprintf(post_path, sizeof(post_path), "%s/postings.bin", index_dir);
    FILE *pf = fopen(post_path, "rb");
    if (!pf) { fprintf(stderr, "Cannot open %s\n", post_path); return 1; }

    /* Lowercase first term and find it */
    char first[MAX_TERM_LEN];
    strncpy(first, argv[term_start], MAX_TERM_LEN - 1);
    first[MAX_TERM_LEN - 1] = '\0';
    for (int i = 0; first[i]; i++)
        first[i] = (char)tolower((unsigned char)first[i]);

    const DictEntry *e = dict_lookup(first);
    if (!e) {
        printf("No documents matched all query terms.\n");
        fclose(pf); return 0;
    }

    int       n;
    uint32_t *result = load_postings(pf, e);
    if (!result) {
        printf("No documents matched all query terms.\n");
        fclose(pf); return 0;
    }
    n = e->df;

    /* AND with each remaining term */
    for (int i = term_start + 1; i < argc && n > 0; i++) {
        char term[MAX_TERM_LEN];
        strncpy(term, argv[i], MAX_TERM_LEN - 1);
        term[MAX_TERM_LEN - 1] = '\0';
        for (int j = 0; term[j]; j++)
            term[j] = (char)tolower((unsigned char)term[j]);

        const DictEntry *te = dict_lookup(term);
        if (!te) { free(result); result = NULL; n = 0; break; }

        uint32_t *postings = load_postings(pf, te);
        if (!postings) { free(result); result = NULL; n = 0; break; }

        int       new_n;
        uint32_t *new_result = intersect(result, n, postings, te->df, &new_n);
        free(result);
        free(postings);
        result = new_result;
        n      = new_n;
    }

    fclose(pf);

    if (!result || n == 0) {
        printf("No documents matched all query terms.\n");
        free(result);
        return 0;
    }

    printf("Found %d matching document%s (AND across terms):\n",
           n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {
        uint32_t docid = result[i];
        if (docid < MAX_DOCS && doc_urls[docid][0])
            printf("%u %s\n", docid, doc_urls[docid]);
        else
            printf("%u (unknown)\n", docid);
    }

    free(result);
    free(dict_entries);
    return 0;
}
