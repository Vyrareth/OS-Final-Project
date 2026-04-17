CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -I./src
LDFLAGS = -lpthread -lcurl

CRAWLER_SRCS = src/crawler/main.c \
               src/crawler/queue.c \
               src/crawler/visited.c \
               src/crawler/fetch.c \
               src/crawler/parse.c \
               src/crawler/threadpool.c \
               src/common/log.c

INDEXER_SRCS = src/indexer/main.c \
               src/indexer/indexer.c \
               src/common/log.c

QUERY_SRCS   = src/query/main.c \
               src/common/log.c

CRAWLER_OBJS = $(patsubst src/%.c, build/%.o, $(CRAWLER_SRCS))
INDEXER_OBJS = $(patsubst src/%.c, build/%.o, $(INDEXER_SRCS))
QUERY_OBJS   = $(patsubst src/%.c, build/%.o, $(QUERY_SRCS))

.PHONY: all clean run dirs

all: dirs crawler indexer query

dirs:
	mkdir -p build/crawler build/indexer build/query build/common \
	         data/pages data/index

crawler: $(CRAWLER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

indexer: $(INDEXER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

query: $(QUERY_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Generic pattern rule: src/foo/bar.c -> build/foo/bar.o
build/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/ crawler indexer query data/pages/* data/index/* /tmp/crawl.sock

# Demo run — crawl example.com (depth 1, 10 pages, 4 threads), then query
run: all
	mkdir -p data/pages data/index
	rm -f /tmp/crawl.sock
	./indexer --ipc /tmp/crawl.sock --out data/index &
	sleep 1
	./crawler --seed https://en.wikipedia.org/wiki/Linux \
	          --max-depth 2 --max-pages 10 -t 4 \
	          --out data --ipc /tmp/crawl.sock
	./query --index data/index linux kernel threads
