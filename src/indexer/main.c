#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>

#include "indexer.h"
#include "../common/ipc.h"
#include "../common/log.h"

/* ── recv_full — loop until all len bytes arrive ─────────────────────── */
static ssize_t recv_full(int fd, void *buf, size_t len) {
    size_t  got = 0;
    char   *p   = buf;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return n;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* ── Append one line to docs.tsv ─────────────────────────────────────── */
static void append_doc_map(const char *index_dir, const CrawlMsg *msg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/docs.tsv", index_dir);
    FILE *fp = fopen(path, "a");
    if (!fp) { LOG_ERROR("Cannot open docs.tsv: %s", strerror(errno)); return; }
    fprintf(fp, "%u\t%s\t%s\t%u\n",
            msg->docid, msg->url, msg->filepath, msg->depth);
    fclose(fp);
}

/* ── Context for tokenize callback ──────────────────────────────────── */
typedef struct { InvertedIndex *idx; uint32_t docid; } IndexCtx;

static void token_callback(const char *word, void *user_data) {
    IndexCtx *ctx = user_data;
    index_insert(ctx->idx, word, ctx->docid);
}

/* ── Read saved HTML and add to index ───────────────────────────────── */
static void process_document(InvertedIndex *idx, const CrawlMsg *msg) {
    FILE *f = fopen(msg->filepath, "r");
    if (!f) {
        LOG_WARN("Cannot open %s: %s", msg->filepath, strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    IndexCtx ctx = { idx, msg->docid };
    tokenize(buf, token_callback, &ctx);
    free(buf);

    LOG_INFO("Indexed docid=%-5u  %s", msg->docid, msg->url);
}

/* ── Create UNIX domain server socket ───────────────────────────────── */
static int create_server_socket(const char *ipc_path) {
    unlink(ipc_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ipc_path, sizeof(addr.sun_path) - 1);

    if (bind(fd,   (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind");   exit(1); }
    if (listen(fd, 1) < 0)                                       { perror("listen"); exit(1); }
    return fd;
}

/* ── Recursive mkdir ─────────────────────────────────────────────────── */
static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    char ipc_path[512] = "/tmp/crawl.sock";
    char out_dir[512]  = "data/index";

    static struct option opts[] = {
        { "ipc",  required_argument, 0, 'i' },
        { "out",  required_argument, 0, 'o' },
        { "help", no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        if (c == 'i') strncpy(ipc_path, optarg, sizeof(ipc_path) - 1);
        if (c == 'o') strncpy(out_dir,  optarg, sizeof(out_dir)  - 1);
        if (c == 'h') {
            fprintf(stderr, "USAGE: ./indexer --ipc <socket-path> --out <dir>\n");
            exit(0);
        }
    }

    log_init(NULL);
    mkdir_p(out_dir);

    int server_fd = create_server_socket(ipc_path);
    LOG_INFO("Indexer listening on %s", ipc_path);

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) { perror("accept"); return 1; }
    LOG_INFO("Crawler connected");

    InvertedIndex *idx = index_create();
    CrawlMsg msg;

    while (1) {
        ssize_t n = recv_full(client_fd, &msg, sizeof(msg));
        if (n <= 0) break;
        if (msg.docid == IPC_SENTINEL_DOCID) {
            LOG_INFO("Done signal received");
            break;
        }
        append_doc_map(out_dir, &msg);
        process_document(idx, &msg);
    }

    close(client_fd);
    close(server_fd);
    unlink(ipc_path);

    write_index(idx, out_dir);
    index_destroy(idx);
    log_close();
    return 0;
}
