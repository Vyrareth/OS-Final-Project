# Multithreaded Web Crawler Pipeline

Three programs written in C: **crawler**, **indexer**, and **query**.

## Dependencies

- `libcurl` with CURLU support (curl ≥ 7.62.0)
  - macOS (Homebrew): `brew install curl`
  - Ubuntu/Debian: `sudo apt install libcurl4-openssl-dev`

## Build & Run

```bash
make all       # compile all three programs into build/ and link binaries
make clean     # remove build/, binaries, and generated data files
make run       # end-to-end demo crawl (Wikipedia/Linux, 50 pages, 4 threads)
```

Manual run sequence — always start the indexer first:

```bash
./indexer --ipc /tmp/crawl.sock --out data/index
./crawler --seed https://en.wikipedia.org/wiki/Linux \
          --max-depth 2 --max-pages 200 -t 8 \
          --out data --ipc /tmp/crawl.sock
./query --index data/index linux kernel threads
```

## URL Queue Design

`src/crawler/queue.c` implements a **bounded ring buffer** of `(url, depth)` pairs.

- **Capacity**: 4096 items (configurable at creation).
- **Synchronization**: one mutex + two condition variables (`not_full`, `not_empty`).
- **Backpressure**: `queue_push()` blocks the caller when the ring is full; it wakes when a consumer removes an item.
- **Frontier exhaustion detection**: an `idle_workers` counter is incremented inside `queue_pop()` before a thread waits, and decremented when it acquires an item. When `idle_workers == num_workers` **and** `count == 0`, all workers are stuck waiting with nothing left to do — the queue sets `shutdown = 1` and broadcasts to let all threads exit cleanly. This check happens under the queue lock, making it race-free.
- **Forced stop**: `queue_shutdown()` sets the flag and broadcasts both condition variables; used when `--max-pages` is reached or `SIGINT` fires.
- **High-water mark**: `max_count_ever` tracks the peak queue depth for the final summary.

## Visited Set Design

`src/crawler/visited.c` implements a **chaining hash table** with 65,536 buckets (power of 2 for fast masking).

- Hash function: **djb2** (`h = h * 33 ^ c`).
- Single mutex protects the entire table.
- `visited_check_and_insert()` performs the check and insert **in one lock acquisition** — this prevents TOCTOU races where two threads could both see a URL as unseen and enqueue it twice.
- Returns `1` if already seen (skip), `0` if freshly inserted (process).

## IPC Protocol

Crawler and indexer communicate over a **UNIX domain stream socket** (`SOCK_STREAM`).

- The indexer binds and listens on the socket path (`--ipc`); the crawler connects after startup.
- Each message is a fixed-size `CrawlMsg` struct (2,568 bytes): `docid`, `depth`, `url[2048]`, `filepath[512]`.
- Fixed size eliminates framing complexity: one `send()` per message, one `recv_full()` loop per message.
- The crawler sends one message per successfully saved page.
- When crawling is complete, the crawler sends a **sentinel** with `docid = UINT32_MAX`, signalling the indexer to flush and exit.
- Multiple worker threads share the socket fd; a mutex (`ipc_lock`) serializes writes.

## Index Format

All files live in the directory passed to `--out` (indexer) / `--index` (query):

| File | Format | Description |
|------|--------|-------------|
| `pages/<docid>.html` | raw HTML | Downloaded page content |
| `docs.tsv` | `docid TAB url TAB filepath TAB depth` | Document map, one line per page |
| `dict.tsv` | `term TAB byte_offset TAB df` | Dictionary: term → location in postings.bin |
| `postings.bin` | binary `uint32_t` values | Docid lists concatenated per term (no length prefix; `df` from dict.tsv gives the count) |

The query tool sorts `dict.tsv` entries in memory after loading and uses `bsearch()` for O(log n) term lookup. It then `fseek()`s to `byte_offset` in `postings.bin` and reads `df` uint32 values. Postings are sorted before AND-intersection using the two-pointer technique (O(m+n)).

## Source Layout

```
src/
  common/   ipc.h         — CrawlMsg struct and IPC_SENTINEL_DOCID
            log.h / log.c — thread-safe logger (mutex-protected)
  crawler/  main.c        — CLI (getopt_long), SIGINT handler, main loop
            queue.h/c     — bounded URL queue (ring buffer)
            visited.h/c   — visited-URL hash set (djb2, atomic check+insert)
            fetch.h/c     — libcurl page fetcher (one handle per thread)
            parse.h/c     — href extraction + normalize_url (CURLU API)
            threadpool.h/c— fixed-size pthread pool + worker_thread
  indexer/  main.c        — IPC server, receive loop, doc map append
            indexer.h/c   — InvertedIndex, tokenizer, stop words, flush
  query/    main.c        — load index, bsearch dict, two-pointer AND
```
