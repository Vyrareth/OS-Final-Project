#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <pthread.h>
#include <curl/curl.h>

#include "queue.h"
#include "visited.h"
#include "threadpool.h"
#include "../common/ipc.h"
#include "../common/log.h"

/* ── Config ──────────────────────────────────────────────────────────── */
typedef struct {
    char *seed_url;
    int   max_depth;
    int   max_pages;
    int   num_threads;
    char *out_dir;
    char *ipc_path;
} CrawlerConfig;

static void print_usage(const char *prog) {
    fprintf(stderr,
        "USAGE: %s --seed <url> --max-depth <D> --max-pages <N>"
        " -t <threads> --out <dir> --ipc <path>\n", prog);
}

static CrawlerConfig parse_args(int argc, char **argv) {
    CrawlerConfig cfg = { NULL, 3, 100, 4, NULL, NULL };

    static struct option opts[] = {
        { "seed",      required_argument, 0, 's' },
        { "max-depth", required_argument, 0, 'd' },
        { "max-pages", required_argument, 0, 'n' },
        { "out",       required_argument, 0, 'o' },
        { "ipc",       required_argument, 0, 'i' },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:h", opts, NULL)) != -1) {
        switch (c) {
            case 's': cfg.seed_url    = optarg;       break;
            case 'd': cfg.max_depth   = atoi(optarg); break;
            case 'n': cfg.max_pages   = atoi(optarg); break;
            case 't': cfg.num_threads = atoi(optarg); break;
            case 'o': cfg.out_dir     = optarg;       break;
            case 'i': cfg.ipc_path    = optarg;       break;
            case 'h': print_usage(argv[0]); exit(0);
            default:  print_usage(argv[0]); exit(1);
        }
    }

    if (!cfg.seed_url || !cfg.out_dir || !cfg.ipc_path) {
        print_usage(argv[0]); exit(1);
    }
    return cfg;
}

/* ── SIGINT handler ──────────────────────────────────────────────────── */
static URLQueue              *g_queue    = NULL;
static volatile sig_atomic_t  g_shutdown = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
    if (g_queue) queue_shutdown(g_queue);
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

/* ── Connect to indexer (retry up to 10 s) ───────────────────────────── */
static int connect_to_indexer(const char *ipc_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ipc_path, sizeof(addr.sun_path) - 1);

    for (int i = 0; i < 10; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            LOG_INFO("Connected to indexer at %s", ipc_path);
            return fd;
        }
        LOG_INFO("Waiting for indexer (%d/10)...", i + 1);
        sleep(1);
    }
    fprintf(stderr, "Failed to connect to indexer at %s\n", ipc_path);
    exit(1);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    CrawlerConfig cfg = parse_args(argc, argv);

    log_init(NULL);   /* log to stderr */

    /* Install SIGINT handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* Create output directories */
    char pages_dir[512];
    snprintf(pages_dir, sizeof(pages_dir), "%s/pages", cfg.out_dir);
    mkdir_p(pages_dir);

    /* Connect to indexer */
    int ipc_fd = connect_to_indexer(cfg.ipc_path);

    curl_global_init(CURL_GLOBAL_ALL);

    /* Init frontier + visited set */
    URLQueue   *q  = queue_create(4096, cfg.num_threads);
    VisitedSet *vs = visited_create();
    g_queue = q;

    /* Seed the frontier */
    visited_check_and_insert(vs, cfg.seed_url);
    queue_push(q, cfg.seed_url, 0);

    /* Shared counters */
    int pages_fetched = 0, pages_failed = 0, pages_skipped = 0, next_docid = 0;
    pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t ipc_lock     = PTHREAD_MUTEX_INITIALIZER;

    WorkerArgs args = {
        .url_queue     = q,
        .visited       = vs,
        .max_depth     = cfg.max_depth,
        .ipc_fd        = ipc_fd,
        .out_dir       = cfg.out_dir,
        .pages_fetched = &pages_fetched,
        .pages_failed  = &pages_failed,
        .pages_skipped = &pages_skipped,
        .max_pages     = cfg.max_pages,
        .next_docid    = &next_docid,
        .counter_lock  = &counter_lock,
        .ipc_lock      = &ipc_lock,
    };

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ThreadPool *tp = threadpool_create(cfg.num_threads, args);
    threadpool_wait_and_destroy(tp);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (double)(t1.tv_sec  - t0.tv_sec) +
                     (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    /* Send IPC sentinel, then close socket */
    CrawlMsg sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.docid = IPC_SENTINEL_DOCID;
    send(ipc_fd, &sentinel, sizeof(sentinel), 0);
    close(ipc_fd);

    printf("\n=== Crawl Summary ===\n");
    printf("Pages fetched  : %d\n", pages_fetched);
    printf("Pages failed   : %d\n", pages_failed);
    printf("Pages skipped  : %d\n", pages_skipped);
    printf("Max queue depth: %d\n", queue_max_size(q));
    printf("Runtime        : %.2f s\n", elapsed);

    queue_destroy(q);
    visited_destroy(vs);
    curl_global_cleanup();
    pthread_mutex_destroy(&counter_lock);
    pthread_mutex_destroy(&ipc_lock);
    log_close();
    return 0;
}
