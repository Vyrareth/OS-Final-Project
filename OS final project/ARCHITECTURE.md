# Multithreaded Web Crawler Pipeline — Architecture

## Overview

Three-process pipeline: **Crawler** → **Indexer** → **Query**

```
URLs → Fetch HTML → Parse links + extract text → Save document → Send metadata via IPC → Build inverted index → Query
```

---

## Process 1: Crawler (`crawler.c`)

### Responsibilities
- Seed a bounded, thread-safe URL frontier (work queue)
- Run a fixed-size pthread worker pool (no thread-per-URL)
- Fetch pages with libcurl; persist HTML/text to disk
- Parse HTML for outgoing links; normalize/filter to HTTP(S); enqueue new URLs
- Send document metadata `(docid, url, filepath, depth)` to indexer via IPC

### Internal Components

#### URL Queue (`queue.h / queue.c`)
- Bounded circular buffer protected by `pthread_mutex_t` + two `pthread_cond_t` (not_full, not_empty)
- Producers block when full (backpressure); consumers block when empty
- Supports a poison-pill / shutdown wakeup

#### Visited Set (`visited.h / visited.c`)
- Hash set of seen URLs, protected by a single `pthread_rwlock_t`
- Check + insert are done atomically under write lock to prevent duplicate crawls

#### Thread Pool (`threadpool.h / threadpool.c`)
- Fixed N worker threads created at startup, reused until shutdown
- Each thread pulls a URL from the queue, fetches, parses, persists, then sends IPC message

#### Fetcher (`fetcher.h / fetcher.c`)
- libcurl easy handle per thread (or shared with locking)
- Configurable timeouts; handles HTTP errors and I/O failures gracefully; logs failures

#### HTML Parser (`parser.h / parser.c`)
- Extracts `<a href>` links and visible text
- Normalizes URLs (absolute, strip fragments), filters non-HTTP(S)
- Enqueues new URLs if depth < max-depth and queue not shut down

#### Stop Conditions
- Atomic counters for pages fetched and pages in-flight
- Race-free checks against `--max-pages` and `--max-depth`
- Frontier exhaustion detected when queue empty + all workers idle

### CLI
```
./crawler --seed <url> --max-depth <D> --max-pages <N> -t <threads> --out <dir> --ipc <path>
```

---

## Process 2: Indexer (`indexer.c`)

### Responsibilities
- Listen on IPC endpoint; receive metadata messages from crawler
- Read saved page files, tokenize into words
- Update persistent inverted index on disk
- Maintain document map: `docid → url + filepath`
- Flush all structures to disk on clean exit

### Internal Components

#### IPC Receiver
- Listens on a UNIX domain socket or named FIFO at `<path>`
- Reads fixed-format binary or line-delimited messages:  
  `docid\turl\tfilepath\tdepth\n`
- Spawns or queues indexing work per received document

#### Tokenizer
- Reads saved file, lowercases, strips punctuation, splits on whitespace
- Emits a stream of `(term, docid)` pairs

#### Inverted Index (on-disk)
- **`index/dict.tsv`** — dictionary: `term\toffset\tdf`  
  (`offset` = byte offset into postings file, `df` = document frequency)
- **`index/postings.bin`** — postings lists, one per term, stored contiguously  
  Each posting: `docid` (4 bytes) | optional `tf` (4 bytes)
- In-memory accumulator flushed/merged to disk periodically and on shutdown

#### Document Map
- **`index/docs.tsv`** — one row per document:  
  `docid\turl\tfilepath\tdepth`

### CLI
```
./indexer --ipc <path> --out <dir>
```

---

## Process 3: Query (`query.c`)

### Responsibilities
- Load `dict.tsv` and `docs.tsv` into memory
- For each query term, seek into `postings.bin` and retrieve the postings list
- AND across all terms (intersection of docid sets)
- Print matching `docid url` pairs

### CLI
```
./query --index <dir> <term1> [term2 ...]
```

### Sample Output
```
Found 3 matching documents (AND across terms):
42   https://en.wikipedia.org/wiki/Operating_systems
87   https://en.wikipedia.org/wiki/Thread_(computing)
105  https://en.wikipedia.org/wiki/POSIX_Threads
```

---

## IPC Protocol

- **Transport**: UNIX domain socket (SOCK_STREAM) at the path given by `--ipc`
- **Message format** (newline-terminated, tab-separated):
  ```
  <docid>\t<url>\t<filepath>\t<depth>\n
  ```
- Indexer starts first and binds the socket; crawler connects after startup
- Crawler sends an EOF / sentinel message (`docid = -1`) to signal shutdown

---

## On-Disk Data Layout

```
<out>/
  pages/
    <docid>.html        # raw HTML (or extracted text) per crawled page
  index/
    docs.tsv            # docid TAB url TAB filepath TAB depth
    dict.tsv            # term TAB byte_offset TAB df
    postings.bin        # packed postings lists (docid uint32 per entry)
```

---

## Concurrency & Synchronization Summary

| Resource        | Mechanism                          |
|-----------------|------------------------------------|
| URL queue       | mutex + cond_not_full + cond_not_empty |
| Visited set     | pthread_rwlock_t                   |
| Pages counter   | atomic_int (stdatomic.h)           |
| IPC send        | mutex (if multiple threads send)   |
| Index flush     | single indexer thread (no sharing) |

---

## Build & Makefile Targets

```makefile
all:    crawler indexer query
clean:  rm -f crawler indexer query *.o
run:    # start indexer, then crawler, then example query
```

Dependencies: `libcurl`, `pthread`, POSIX sockets.

---

## Logging & Summary

At exit, both crawler and indexer print:
- Pages fetched / skipped / failed
- Max queue depth observed
- Total runtime (wall clock)
- IPC messages sent/received
