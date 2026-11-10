#pragma once
#include "reader.h"
typedef enum {
    OPEN_FILE,
    VALIDATE_SCALAR,
    COUNT_VALUES,
    INDEX_LINES,
    READ_PAGE,
    FIND_TEXT,
    LOCATE_HIT,
    TRANSFER_NODE
} JobType;
typedef enum {
    COPY_NAME = 1,
    COPY_VALUE,
    COPY_PATH,
    COPY_MINIFIED,
    COPY_FORMATTED,
    EXPORT_MINIFIED,
    EXPORT_FORMATTED,
    COPY_JSON,
    EXPORT_JSON
} NodeAction;
/* Immutable once published. Deltas keep dense queries compact on large files. */
typedef struct MatchBlock {
    struct MatchBlock *next;
    size_t first_index, base, last;
    unsigned count, used;
    unsigned char bytes[1024];
} MatchBlock;
typedef struct {
    MatchBlock *block;
    unsigned offset, ordinal;
    size_t position;
} MatchCursor;
bool match_next(MatchCursor *cursor, Match *out);
typedef struct Job {
    struct Job *next;
    JobType type;
    HWND window;
    HANDLE thread;
    Document *doc;
    unsigned generation;
    volatile LONG cancel, published;
    size_t received;
    wchar_t *filename;
    Node parent, root, page[J_PAGE];
    Record first[J_PAGE], *records;
    size_t nrecords, record_cap;
    size_t pos, ordinal, hit, resume;
    bool more, whole_document;
    bool count_root_page;
    size_t value_count;
    char query[2048], error[768];
    SearchMode mode;
    SRWLOCK match_lock;
    MatchBlock *building, *pending, *tail;
    size_t matches;
    LARGE_INTEGER find_finished;
    Path path;
    size_t record_start;
    NodeAction action;
    HGLOBAL clipboard;
} Job;
void prepare_transfer(Job *job, Reader *r);
bool job_launch(Job *job);
void job_free(Job *job);
