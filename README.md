OS final project/
├── Makefile
├── README.md
├── ARCHITECTURE.md         ← already existed
├── build/                  ← compiled binaries go here
├── data/pages/             ← crawler saves HTML here
├── index/                  ← indexer writes dict/docs/postings here
└── src/
    ├── common/
    │   └── ipc_proto.h     ← shared IPC message format
    ├── crawler/
    │   ├── main.c          ← thread pool, CLI, shutdown logic
    │   ├── queue.c/h       ← bounded URL frontier
    │   ├── visited.c/h     ← thread-safe visited set
    │   ├── fetch.c/h       ← libcurl HTTP fetching
    │   ├── parse.c/h       ← HTML link extraction
    │   └── ipc_client.c/h  ← sends metadata to indexer
    ├── indexer/
    │   ├── main.c          ← IPC recv loop, flush on sentinel
    │   ├── ipc_server.c/h  ← UNIX socket server
    │   ├── tokenizer.c/h   ← HTML tag-strip + tokenize
    │   └── index.c/h       ← inverted index + disk flush
    └── query/
        ├── main.c          ← CLI, AND intersection, print results
        └── index_reader.c/h ← loads dict/postings/docs from disk
