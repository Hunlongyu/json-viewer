#pragma once
#include "jobs.h"
typedef struct Branch {
    struct Branch *parent;
    Node node, pinned;
    Node *items;
    size_t count, capacity, next, ordinal, record;
    bool open, pending, more, has_pin;
    char error[256];
} Branch;
typedef enum { DATA_ROW, RECORDS_ROOT, RECORD_ROW, NEXT_PAGE } RowKind;
typedef struct {
    RowKind kind;
    Node node;
    Branch *parent;
    size_t record;
    int depth;
    uint64_t guides;
    bool last;
} Row;
typedef struct {
    int start, count;
    Row row;
} Span;
typedef struct {
    HWND window;
    Document *doc;
    unsigned generation;
    Job *jobs;
    Node root;
    Record *records;
    size_t nrecords, record_cap;
    Branch **branches;
    size_t nbranches, branch_cap;
    Span *spans;
    size_t nspans, span_cap;
    int rows, cursor;
    bool dirty, lines_open, indexed, waiting_hit;
    enum { COUNT_PENDING, COUNT_READY, COUNT_INVALID } count_state;
    size_t value_count;
    size_t last_hit, located_hit, located_node, hit_count, focus, total_hits, requested_hit;
    MatchBlock **blocks;
    size_t nblocks, block_cap;
    bool searching, search_selection;
    LARGE_INTEGER find_started, find_finished;
    char search_error[192];
    char query[2048], status[800];
    SearchMode mode;
    wchar_t *filename;
} View;
void view_open(View *v, const wchar_t *path);
void view_tick(View *v);
void view_close(View *v);
Row view_row(View *v, int index);
void view_path(const View *v, Row row, char *out, size_t cap);
bool view_expanded(View *v, Row row);
void view_toggle(View *v, int action); /* -1 collapse, 0 toggle, +1 expand */
void view_collapse_all(View *v);
/* Zero restarts the query. Each submission selects the next match. */
void view_search(View *v, const char *query, SearchMode mode, unsigned submissions);
void view_previous(View *v);
void view_search_label(const View *v, char *text, size_t cap);
void view_mark(const View *v, const size_t *source, size_t length, unsigned char *marks);
void view_transfer(View *v, Row row, NodeAction action, const wchar_t *destination);
void view_cancel(View *v);
void view_locate(View *v);
void view_text(View *v, Row row, char *label, size_t labelcap, char *value, size_t valuecap);
