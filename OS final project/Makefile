CC      = clang
STD     = -std=c11
WARN    = -Wall -Wextra
OPT     = -O2
DEFS    = -D_POSIX_C_SOURCE=200809L
INCS    = -Isrc

# Detect libcurl and libxml2 via pkg-config (brew install pkg-config curl libxml2)
# Falls back to Homebrew keg-only paths, then macOS system SDK.
SDK         := $(shell xcrun --show-sdk-path 2>/dev/null)
BREW_CURL   := $(shell brew --prefix curl    2>/dev/null)
BREW_XML2   := $(shell brew --prefix libxml2 2>/dev/null)

PKGCFG_CF       := $(shell pkg-config --cflags libcurl libxml-2.0 2>/dev/null)
PKGCFG_LIBS     := $(shell pkg-config --libs   libcurl libxml-2.0 2>/dev/null)
PKGCFG_LIBS_XML := $(shell pkg-config --libs   libxml-2.0 2>/dev/null)

ifneq ($(strip $(PKGCFG_CF)),)
  EXT_CFLAGS      := $(PKGCFG_CF)
  EXT_LIBS        := $(PKGCFG_LIBS)
  EXT_LIBS_XML    := $(PKGCFG_LIBS_XML)
else ifneq ($(wildcard $(BREW_CURL)/lib/libcurl.dylib $(BREW_CURL)/lib/libcurl.a),)
  EXT_CFLAGS      := -I$(BREW_CURL)/include -I$(BREW_XML2)/include/libxml2
  EXT_LIBS        := -L$(BREW_CURL)/lib -lcurl -L$(BREW_XML2)/lib -lxml2
  EXT_LIBS_XML    := -L$(BREW_XML2)/lib -lxml2
else
  EXT_CFLAGS      := $(if $(SDK),-I$(SDK)/usr/include/libxml2)
  EXT_LIBS        := -lcurl -lxml2
  EXT_LIBS_XML    := -lxml2
endif

CFLAGS       = $(STD) $(WARN) $(OPT) $(DEFS) $(INCS) $(EXT_CFLAGS)
LIBS_CRAWLER = $(EXT_LIBS) -lpthread
LIBS_INDEXER = $(EXT_LIBS_XML) -lpthread
LIBS_QUERY   =

BUILD   = build
SRC_CRW = src/crawler
SRC_IDX = src/indexer
SRC_QRY = src/query

CRAWLER_SRCS = $(SRC_CRW)/main.c       \
               $(SRC_CRW)/queue.c      \
               $(SRC_CRW)/visited.c    \
               $(SRC_CRW)/fetch.c      \
               $(SRC_CRW)/parse.c      \
               $(SRC_CRW)/ipc_client.c

INDEXER_SRCS = $(SRC_IDX)/main.c        \
               $(SRC_IDX)/ipc_server.c  \
               $(SRC_IDX)/tokenizer.c   \
               $(SRC_IDX)/index.c

QUERY_SRCS   = $(SRC_QRY)/main.c         \
               $(SRC_QRY)/index_reader.c

.PHONY: all clean dirs run help

all: dirs crawler indexer query

dirs:
	@mkdir -p $(BUILD) data/pages index

crawler: $(CRAWLER_SRCS) src/common/ipc_proto.h
	$(CC) $(CFLAGS) -o $(BUILD)/crawler $(CRAWLER_SRCS) $(LIBS_CRAWLER)
	@echo "Built: $(BUILD)/crawler"

indexer: $(INDEXER_SRCS) src/common/ipc_proto.h
	$(CC) $(CFLAGS) -o $(BUILD)/indexer $(INDEXER_SRCS) $(LIBS_INDEXER)
	@echo "Built: $(BUILD)/indexer"

query: $(QUERY_SRCS)
	$(CC) $(CFLAGS) -o $(BUILD)/query $(QUERY_SRCS) $(LIBS_QUERY)
	@echo "Built: $(BUILD)/query"

clean:
	rm -rf $(BUILD)/ data/ index/
	@echo "Cleaned."

# End-to-end demo:
#   1. Start indexer in background
#   2. Run crawler on example.com
#   3. Query the resulting index
run: all
	@echo "=== Starting indexer ==="
	$(BUILD)/indexer --ipc /tmp/crawl.sock --out . &
	@sleep 0.5
	@echo "=== Starting crawler ==="
	$(BUILD)/crawler --seed https://example.com --max-depth 2 \
	    --max-pages 50 -t 4 --out . --ipc /tmp/crawl.sock
	@sleep 1
	@echo "=== Querying index ==="
	$(BUILD)/query --index . web example domain

help:
	@echo "Targets: all  clean  run  help"
	@echo ""
	@echo "  make all    — build crawler, indexer, query into build/"
	@echo "  make clean  — remove build/, data/, index/"
	@echo "  make run    — build + run end-to-end demo on example.com"
	@echo ""
	@echo "Manual usage:"
	@echo "  $(BUILD)/indexer --ipc /tmp/crawl.sock --out <dir>"
	@echo "  $(BUILD)/crawler --seed <url> --max-depth <D> --max-pages <N> -t <T> --out <dir> --ipc /tmp/crawl.sock"
	@echo "  $(BUILD)/query   --index <dir> <term1> [term2 ...]"
