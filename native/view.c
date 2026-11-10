#include "view.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void status(View *v, const char *s) {
    snprintf(v->status, sizeof v->status, "%s", s);
}
static void cancel_type(View *v, JobType type) {
    for (Job *j = v->jobs; j; j = j->next)
        if (j->type == type)
            InterlockedExchange(&j->cancel, 1);
}
static Job *make_job(View *v, JobType type) {
    Job *j = calloc(1, sizeof *j);
    if (!j) {
        status(v, "Out of memory");
        return NULL;
    }
    j->window = v->window;
    j->type = type;
    j->generation = v->generation;
    j->hit = J_NONE;
    if (type != OPEN_FILE) {
        j->doc = v->doc;
        doc_retain(j->doc);
        j->root = v->root;
    }
    return j;
}
static bool start(View *v, Job *j) {
    if (!job_launch(j)) {
        status(v, "Could not start background task");
        job_free(j);
        return false;
    }
    j->next = v->jobs;
    v->jobs = j;
    return true;
}
static size_t branch_slot(View *v, size_t offset) {
    size_t a = 0, b = v->nbranches;
    while (a < b) {
        size_t m = a + (b - a) / 2;
        if (v->branches[m]->node.start < offset)
            a = m + 1;
        else
            b = m;
    }
    return a;
}
static Branch *lookup(View *v, size_t offset) {
    size_t i = branch_slot(v, offset);
    return i < v->nbranches && v->branches[i]->node.start == offset ? v->branches[i] : NULL;
}
static Branch *branch(View *v, Node n, size_t record) {
    size_t i = branch_slot(v, n.start);
    if (i < v->nbranches && v->branches[i]->node.start == n.start)
        return v->branches[i];
    if (!reserve_items((void **)&v->branches, &v->branch_cap, v->nbranches + 1,
                       sizeof *v->branches))
        return NULL;
    Branch *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->node = n;
    b->record = record;
    b->next = n.start + 1;
    b->more = true;
    memmove(v->branches + i + 1, v->branches + i, (v->nbranches - i) * sizeof *v->branches);
    v->branches[i] = b;
    v->nbranches++;
    return b;
}
static void load_page(View *v, Branch *b) {
    if (b->pending || !b->more || b->error[0])
        return;
    Job *j = make_job(v, READ_PAGE);
    if (!j)
        return;
    j->parent = b->node;
    j->count_root_page =
        !v->doc->lines && b->node.start == v->root.start && b->node.kind == '{' && b->ordinal == 0;
    j->pos = b->next;
    j->ordinal = b->ordinal;
    if (b->record != J_NONE)
        j->root = b->node;
    b->pending = start(v, j);
}
static void clear_matches(View *v) {
    for (size_t i = 0; i < v->nblocks; ++i)
        free(v->blocks[i]);
    v->nblocks = v->total_hits = v->hit_count = v->requested_hit = 0;
    v->searching = v->search_selection = false;
    v->search_error[0] = 0;
    v->last_hit = J_NONE;
}
static void reset(View *v) {
    ++v->generation;
    for (Job *j = v->jobs; j; j = j->next)
        InterlockedExchange(&j->cancel, 1);
    for (size_t i = 0; i < v->nbranches; i++) {
        free(v->branches[i]->items);
        free(v->branches[i]);
    }
    v->nbranches = v->nspans = v->nrecords = v->record_cap = 0;
    free(v->records);
    v->records = NULL;
    doc_release(v->doc);
    v->doc = NULL;
    free(v->filename);
    v->filename = NULL;
    v->rows = v->cursor = 0;
    v->value_count = 0;
    v->count_state = COUNT_PENDING;
    v->lines_open = v->indexed = v->waiting_hit = false;
    v->last_hit = v->focus = J_NONE;
    clear_matches(v);
    v->query[0] = 0;
    v->dirty = true;
}
void view_open(View *v, const wchar_t *path) {
    wchar_t *name = _wcsdup(path ? path : L"Clipboard");
    if (!name) {
        status(v, "Out of memory");
        return;
    }
    reset(v);
    v->filename = name;
    Job *j = make_job(v, OPEN_FILE);
    if (!j)
        return;
    if (path && !(j->filename = _wcsdup(name))) {
        job_free(j);
        status(v, "Out of memory");
        return;
    }
    status(v, "Opening file");
    start(v, j);
}
void view_close(View *v) {
    reset(v);
    while (v->jobs) {
        Job *j = v->jobs;
        v->jobs = j->next;
        WaitForSingleObject(j->thread, INFINITE);
        job_free(j);
    }
    free(v->spans);
    free(v->branches);
    free(v->blocks);
}
static void span(View *v, Row row, size_t count) {
    if (count > (size_t)(INT_MAX - v->rows))
        count = INT_MAX - v->rows;
    if (!count)
        return;
    if (!reserve_items((void **)&v->spans, &v->span_cap, v->nspans + 1, sizeof *v->spans)) {
        status(v, "Out of memory building visible rows");
        return;
    }
    v->spans[v->nspans++] = (Span){v->rows, (int)count, row};
    if (row.kind == DATA_ROW && row.node.start == v->focus) {
        v->cursor = v->rows;
        v->focus = J_NONE;
    }
    v->rows += (int)count;
}
typedef struct {
    Branch *b;
    size_t next;
    int depth;
    uint64_t guides;
} Projection;
/* Only materialised child pages get individual rows. Unopened JSONL records
   share a span and are resolved by index when the grid requests them. */
static void project_tree(View *v, Row row) {
    Projection *stack = NULL;
    size_t depth = 0, cap = 0;
    for (;;) {
        if (row.depth >= 0)
            span(v, row, 1);
        Branch *b = container(row.node) ? lookup(v, row.node.start) : NULL;
        if (b && b->open) {
            if (!reserve_items((void **)&stack, &cap, depth + 1, sizeof *stack)) {
                status(v, "Out of memory building visible rows");
                break;
            }
            uint64_t guides = row.guides;
            if (row.depth >= 0 && row.depth < 64 && !row.last &&
                (row.parent || row.record != J_NONE))
                guides |= UINT64_C(1) << row.depth;
            stack[depth++] = (Projection){b, 0, row.depth + 1, guides};
        }
        while (depth) {
            Projection *f = &stack[depth - 1];
            b = f->b;
            if (f->next < b->count || (b->has_pin && f->next == b->count)) {
                Node n = f->next < b->count ? b->items[f->next] : b->pinned;
                ++f->next;
                row = (Row){.kind = DATA_ROW,
                            .node = n,
                            .parent = b,
                            .record = J_NONE,
                            .depth = f->depth,
                            .guides = f->guides,
                            .last = f->next == b->count + (size_t)b->has_pin && !b->more};
                break;
            }
            /* Keep the requested page's row until its first result replaces it. */
            if (b->more && (!b->pending || (b->ordinal && b->count == b->ordinal)) && !b->error[0])
                span(v,
                     (Row){.kind = NEXT_PAGE,
                           .parent = b,
                           .depth = f->depth,
                           .guides = f->guides,
                           .last = true},
                     1);
            --depth;
        }
        if (!depth)
            break;
    }
    free(stack);
}
static void record_span(View *v, size_t first, size_t count) {
    span(v, (Row){.kind = RECORD_ROW, .depth = 1, .record = first}, count);
}
static void rebuild(View *v) {
    Row previous = {0};
    bool restore = v->focus == J_NONE && v->rows && v->doc;
    if (restore)
        previous = view_row(v, v->cursor);
    if (restore && previous.kind == DATA_ROW)
        v->focus = previous.node.start;
    v->rows = 0;
    v->nspans = 0;
    v->dirty = false;
    if (!v->doc)
        return;
    if (!v->doc->lines)
        project_tree(
            v, (Row){.node = v->root, .record = J_NONE, .depth = v->root.kind == '{' ? -1 : 0});
    else {
        span(v, (Row){.kind = RECORDS_ROOT}, 1);
        if (v->lines_open) {
            size_t first = 0;
            for (size_t i = 0; i < v->nbranches; i++) {
                Branch *b = v->branches[i];
                if (!b->open || b->record == J_NONE || b->record >= v->nrecords)
                    continue;
                record_span(v, first, b->record - first);
                project_tree(v, (Row){.kind = DATA_ROW,
                                      .node = b->node,
                                      .depth = 1,
                                      .record = b->record,
                                      .last = b->record + 1 == v->nrecords});
                first = b->record + 1;
            }
            record_span(v, first, v->nrecords - first);
        }
    }
    if (restore && previous.kind == RECORD_ROW) {
        for (size_t i = 0; i < v->nspans; i++) {
            Span s = v->spans[i];
            if (s.row.kind == RECORD_ROW && previous.record >= s.row.record &&
                previous.record - s.row.record < (size_t)s.count) {
                v->cursor = s.start + (int)(previous.record - s.row.record);
                break;
            }
            if (s.row.kind == DATA_ROW && s.row.record == previous.record) {
                v->cursor = s.start;
                break;
            }
        }
    }
    v->focus = J_NONE;
    if (v->cursor >= v->rows)
        v->cursor = v->rows ? v->rows - 1 : 0;
}
Row view_row(View *v, int at) {
    Row empty = {.record = J_NONE};
    if (at < 0 || at >= v->rows || !v->doc)
        return empty;
    size_t lo = 0, hi = v->nspans;
    while (lo + 1 < hi) {
        size_t m = (lo + hi) / 2;
        if (v->spans[m].start <= at)
            lo = m;
        else
            hi = m;
    }
    Span *s = &v->spans[lo];
    Row row = s->row;
    if (row.kind == RECORD_ROW) {
        row.record += at - s->start;
        row.last = row.record + 1 == v->nrecords;
        Record rec = v->records[row.record];
        Reader r = doc_reader(v->doc, NULL);
        if (!jroot(&r, rec.content, rec.end, &row.node))
            row.node = (Node){rec.start, rec.end, J_NONE, J_NONE, 0, '!'};
    }
    return row;
}
void view_path(const View *v, Row row, char *out, size_t cap) {
    if (!cap)
        return;
    if (cap < 4) {
        *out = 0;
        return;
    }
    if (!v->doc || !v->rows || row.kind == NEXT_PAGE) {
        *out = 0;
        return;
    }
    /* Walk only materialised ancestors. Build from the leaf backwards to keep
       the visible suffix of long paths without traversing the JSON again. */
    char *at = out + cap - 1;
    *at = 0;
    size_t record = row.record;
    Node node = row.node;
    for (Branch *b = row.parent; b; b = b->parent) {
        if (node.key == J_NONE) {
            char index[32];
            size_t len = (size_t)snprintf(index, sizeof index, "[%zu]", node.ordinal);
            if ((size_t)(at - out) < len + 32)
                goto clipped;
            at -= len;
            memcpy(at, index, len);
        } else {
            size_t len = node.key_end - node.key - 2;
            if ((size_t)(at - out) < len + 36)
                goto clipped;
            const unsigned char *key = v->doc->data + node.key + 1;
            if (jpath_identifier(key, len)) {
                at -= len;
                memcpy(at, key, len);
                *--at = '.';
            } else {
                *--at = ']';
                at -= len + 2;
                memcpy(at, key - 1, len + 2);
                *--at = '[';
            }
        }
        if (b->record != J_NONE)
            record = b->record;
        node = b->node;
    }
    if (v->doc->lines && record != J_NONE && row.kind != RECORDS_ROOT) {
        char index[32];
        size_t len = (size_t)snprintf(index, sizeof index, "[%zu]", record);
        if ((size_t)(at - out) < len + 1)
            goto clipped;
        at -= len;
        memcpy(at, index, len);
    }
    *--at = '$';
    memmove(out, at, (size_t)(out + cap - at));
    return;
clipped:
    at -= 3;
    memcpy(at, "…", 3);
    memmove(out, at, (size_t)(out + cap - at));
}
bool view_expanded(View *v, Row row) {
    if (row.kind == RECORDS_ROOT)
        return v->lines_open;
    Branch *b = lookup(v, row.node.start);
    return b && b->open;
}
void view_toggle(View *v, int action) {
    if (!v->rows)
        return;
    Row row = view_row(v, v->cursor);
    if (row.kind == NEXT_PAGE) {
        if (action >= 0)
            load_page(v, row.parent);
        else
            v->focus = row.parent->node.start;
    } else if (row.kind == RECORDS_ROOT)
        v->lines_open = action ? action > 0 : !v->lines_open;
    else {
        bool open = view_expanded(v, row);
        if (action < 0 && !open) {
            if (row.parent)
                v->focus = row.parent->node.start;
            else if (v->doc->lines)
                v->cursor = 0;
        } else if (container(row.node)) {
            Branch *b = branch(v, row.node, row.record);
            if (!b) {
                status(v, "Out of memory");
                return;
            }
            b->open = action ? action > 0 : !open;
            b->parent = row.parent;
            if (b->open && !b->count)
                load_page(v, b);
            if (b->error[0])
                status(v, b->error);
        }
    }
    v->dirty = true;
}
void view_collapse_all(View *v) {
    if (!v->doc)
        return;
    cancel_type(v, LOCATE_HIT);
    v->waiting_hit = v->lines_open = v->search_selection = false;
    for (size_t i = 0; i < v->nbranches; ++i) {
        Branch *b = v->branches[i];
        b->open = !v->doc->lines && v->root.kind == '{' && b->node.start == v->root.start;
    }
    v->focus = J_NONE;
    v->cursor = 0;
    v->dirty = true;
}
void view_cancel(View *v) {
    cancel_type(v, FIND_TEXT);
    cancel_type(v, LOCATE_HIT);
    v->waiting_hit = false;
    v->query[0] = 0;
    clear_matches(v);
    status(v, "Search cancelled");
}
static MatchCursor match_cursor(MatchBlock *b) {
    return (MatchCursor){.block = b, .position = b->base};
}
static void navigate(View *v, size_t index) {
    size_t lo = 0, hi = v->nblocks;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v->blocks[mid]->first_index <= index)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return;
    MatchCursor c = match_cursor(v->blocks[lo - 1]);
    Match m;
    while (match_next(&c, &m)) {
        if (c.block->first_index + c.ordinal == index + 1) {
            v->hit_count = index + 1;
            v->last_hit = m.start;
            view_locate(v);
            return;
        }
    }
}
/* source[i] is the file offset of the scalar rendered at preview byte i.
   Adjacent preview bytes may belong to the same escaped scalar. */
void view_mark(const View *v, const size_t *source, size_t length, unsigned char *marks) {
    if (!length)
        return;
    size_t lo = 0, hi = v->nblocks;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v->blocks[mid]->last <= source[0])
            lo = mid + 1;
        else
            hi = mid;
    }
    size_t at = 0;
    for (; lo < v->nblocks && v->blocks[lo]->base < source[length]; ++lo) {
        MatchCursor c = match_cursor(v->blocks[lo]);
        Match m;
        while (match_next(&c, &m)) {
            if (m.start >= source[length])
                return;
            if (m.end <= source[0] || m.start == m.end)
                continue;
            while (at < length && source[at + 1] <= m.start)
                ++at;
            while (at && source[at] == source[at - 1])
                --at;
            while (at < length && source[at] < m.end)
                marks[at++] = 1;
        }
    }
}
void view_search_label(const View *v, char *text, size_t cap) {
    if (!v->query[0])
        text[0] = 0;
    else if (v->search_error[0])
        snprintf(text, cap, "Find error");
    else if (v->searching)
        snprintf(text, cap, "%zu/…", v->hit_count);
    else
        snprintf(text, cap, "%zu/%zu", v->hit_count, v->total_hits);
}
void view_search(View *v, const char *query, SearchMode mode, unsigned submissions) {
    if (!v->doc || !query[0])
        return;
    if (submissions && !strcmp(query, v->query) && mode == v->mode && !v->search_error[0]) {
        size_t target = max(v->hit_count, v->requested_hit) + submissions;
        if (target <= v->total_hits)
            navigate(v, target - 1);
        else if (v->searching)
            v->requested_hit = target;
        else if (v->total_hits)
            navigate(v, (target - 1) % v->total_hits);
        return;
    }
    QueryPerformanceCounter(&v->find_started);
    view_cancel(v);
    v->mode = mode;
    snprintf(v->query, sizeof v->query, "%s", query);
    Job *j = make_job(v, FIND_TEXT);
    if (j) {
        snprintf(j->query, sizeof j->query, "%s", query);
        j->mode = mode;
        v->searching = start(v, j);
    }
    if (v->searching) {
        v->requested_hit = max(1u, submissions);
        status(v, "Searching");
    } else
        snprintf(v->search_error, sizeof v->search_error, "%.180s", v->status);
}
void view_transfer(View *v, Row row, NodeAction action, const wchar_t *destination) {
    if (!v->doc || row.kind == NEXT_PAGE)
        return;
    cancel_type(v, TRANSFER_NODE);
    Job *j = make_job(v, TRANSFER_NODE);
    if (!j)
        return;
    j->action = action;
    j->whole_document = row.kind == RECORDS_ROOT;
    j->parent = row.node;
    j->record_start = J_NONE;
    if (destination && !(j->filename = _wcsdup(destination))) {
        job_free(j);
        status(v, "Out of memory");
        return;
    }
    if (v->doc->lines && row.kind != RECORDS_ROOT) {
        size_t lo = 0, hi = v->nrecords;
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if (v->records[m].end <= row.node.start)
                lo = m + 1;
            else
                hi = m;
        }
        if (lo == v->nrecords) {
            job_free(j);
            return;
        }
        j->record_start = lo;
        Record rec = v->records[lo];
        Reader r = doc_reader(v->doc, NULL);
        if (!jroot(&r, rec.content, rec.end, &j->root))
            j->root = row.node;
    }
    start(v, j);
}
void view_previous(View *v) {
    v->requested_hit = 0;
    if (v->hit_count > 1)
        navigate(v, v->hit_count - 2);
    else if (!v->searching && v->total_hits)
        navigate(v, v->total_hits - 1);
}
void view_locate(View *v) {
    if (!v->doc || v->last_hit == J_NONE)
        return;
    cancel_type(v, LOCATE_HIT);
    if (v->doc->lines && !v->indexed &&
        (!v->nrecords || v->last_hit >= v->records[v->nrecords - 1].end)) {
        v->waiting_hit = true;
        status(v, "Match found; indexing records");
        return;
    }
    v->waiting_hit = false;
    Job *j = make_job(v, LOCATE_HIT);
    if (!j)
        return;
    j->hit = v->last_hit;
    start(v, j);
}
static void select_hit(View *v, Job *j) {
    v->search_selection = true;
    v->located_hit = j->hit;
    v->located_node = j->path.count ? j->path.nodes[j->path.count - 1].start : J_NONE;
    size_t record = J_NONE;
    if (v->doc->lines) {
        v->lines_open = true;
        size_t lo = 0, hi = v->nrecords;
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if (v->records[m].end <= j->hit)
                lo = m + 1;
            else
                hi = m;
        }
        if (lo < v->nrecords)
            record = lo;
        if (record != J_NONE) {
            rebuild(v);
            for (size_t i = 0; i < v->nspans; i++) {
                Span s = v->spans[i];
                if (s.row.kind == RECORD_ROW && record >= s.row.record &&
                    record - s.row.record < (size_t)s.count) {
                    v->cursor = s.start + (int)(record - s.row.record);
                    break;
                }
            }
        }
    }
    Branch *ancestor = NULL;
    for (size_t i = 0; i + 1 < j->path.count; i++) {
        Node n = j->path.nodes[i], child = j->path.nodes[i + 1];
        Branch *b = branch(v, n, i == 0 ? record : J_NONE);
        if (!b) {
            status(v, "Out of memory");
            return;
        }
        b->open = true;
        b->parent = ancestor;
        ancestor = b;
        b->has_pin = child.ordinal >= b->count;
        if (b->has_pin)
            b->pinned = child;
        if (!b->count)
            load_page(v, b);
    }
    if (j->path.count)
        v->focus = j->path.nodes[j->path.count - 1].start;
    v->dirty = true;
    if (j->error[0])
        snprintf(v->status, sizeof v->status, "Match at byte %zu · %s", j->hit, j->error);
    else
        snprintf(v->status, sizeof v->status, "Match %zu · byte %zu · %.450s", v->hit_count, j->hit,
                 v->query);
}
static void receive_matches(View *v, Job *j) {
    AcquireSRWLockExclusive(&j->match_lock);
    MatchBlock *b = j->pending;
    j->pending = j->tail = NULL;
    ReleaseSRWLockExclusive(&j->match_lock);
    bool first = !v->total_hits;
    while (b) {
        MatchBlock *next = b->next;
        if (!reserve_items((void **)&v->blocks, &v->block_cap, v->nblocks + 1, sizeof *v->blocks)) {
            do {
                next = b->next;
                free(b);
                b = next;
            } while (b);
            view_cancel(v);
            status(v, "Out of memory storing matches");
            return;
        }
        v->blocks[v->nblocks++] = b;
        v->total_hits += b->count;
        b = next;
    }
    if (v->requested_hit && v->requested_hit <= v->total_hits) {
        navigate(v, v->requested_hit - 1);
        v->requested_hit = 0;
    } else if (first && v->total_hits)
        navigate(v, 0);
}
static void partial(View *v, Job *j) {
    if (j->type == FIND_TEXT) {
        receive_matches(v, j);
        return;
    }
    size_t n = (size_t)InterlockedCompareExchange(&j->published, 0, 0);
    if (n <= j->received)
        return;
    if (j->type == READ_PAGE) {
        Branch *b = lookup(v, j->parent.start);
        if (!b)
            return;
        for (size_t i = j->received; i < n; i++) {
            if (!reserve_items((void **)&b->items, &b->capacity, b->count + 1, sizeof(Node))) {
                status(v, "Out of memory loading page");
                b->pending = false;
                InterlockedExchange(&j->cancel, 1);
                break;
            }
            b->items[b->count++] = j->page[i];
            if (b->has_pin && b->pinned.start == j->page[i].start)
                b->has_pin = false;
        }
    } else if (j->type == INDEX_LINES) {
        if (!reserve_items((void **)&v->records, &v->record_cap, n, sizeof(Record)))
            return;
        memcpy(v->records + j->received, j->first + j->received,
               (n - j->received) * sizeof(Record));
        v->nrecords = n;
    }
    j->received = n;
    v->dirty = true;
}
static void start_count(View *v, size_t pos, size_t ordinal, size_t count) {
    Job *job = make_job(v, COUNT_VALUES);
    if (job) {
        job->pos = pos;
        job->ordinal = ordinal;
        job->value_count = count;
        if (start(v, job))
            return;
    }
    v->count_state = COUNT_INVALID;
}
static void complete(View *v, Job *j) {
    if (j->type == READ_PAGE) {
        Branch *b = lookup(v, j->parent.start);
        if (!b)
            return;
        b->pending = false;
        b->next = j->pos;
        b->ordinal = j->ordinal;
        b->more = j->more;
        if (j->count_root_page) {
            if (j->error[0])
                v->count_state = COUNT_INVALID;
            else if (j->more)
                start_count(v, j->pos, j->ordinal, j->value_count);
            else {
                v->value_count = j->value_count;
                v->count_state = COUNT_READY;
            }
        }
        if (j->error[0]) {
            snprintf(b->error, sizeof b->error, "%.245s", j->error);
            if (b->record != J_NONE)
                snprintf(v->status, sizeof v->status, "Line %zu · %.700s",
                         v->records[b->record].line, j->error);
            else
                status(v, j->error);
        }
    } else if (j->type == COUNT_VALUES || j->type == VALIDATE_SCALAR) {
        v->count_state = j->error[0] ? COUNT_INVALID : COUNT_READY;
        v->value_count = j->value_count;
        if (j->type == VALIDATE_SCALAR && j->error[0])
            status(v, j->error);
    } else if (j->type == LOCATE_HIT)
        select_hit(v, j);
    else if (j->type == FIND_TEXT) {
        v->searching = false;
        v->find_finished = j->find_finished;
        if (v->requested_hit && v->total_hits && !j->error[0])
            navigate(v, (v->requested_hit - 1) % v->total_hits);
        v->requested_hit = 0;
        if (j->error[0]) {
            snprintf(v->search_error, sizeof v->search_error, "%.180s", j->error);
            status(v, j->error);
        } else if (!v->total_hits)
            status(v, "No matches");
    } else if (j->error[0])
        status(v, j->error);
    else if (j->type == TRANSFER_NODE) {
        if (j->clipboard) {
            if (!OpenClipboard(v->window)) {
                status(v, "Clipboard is busy");
                return;
            }
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, j->clipboard)) {
                j->clipboard = NULL;
                status(v, "Ready");
            } else
                status(v, "Could not copy to clipboard");
            CloseClipboard();
        } else
            status(v, "Ready");
    } else if (j->type == OPEN_FILE) {
        v->doc = j->doc;
        j->doc = NULL;
        v->root = j->root;
        status(v, v->doc->snapshot ? "Ready · snapshot" : "Ready");
        if (v->doc->lines) {
            Job *index = make_job(v, INDEX_LINES);
            if (index)
                start(v, index);
        } else if (v->root.kind == '{') {
            Branch *b = branch(v, v->root, J_NONE);
            if (b) {
                b->open = true;
                load_page(v, b);
            } else {
                v->count_state = COUNT_INVALID;
                status(v, "Out of memory");
            }
        } else if (v->root.kind == '[') {
            start_count(v, v->root.start + 1, 0, 0);
        } else {
            Job *check = make_job(v, VALIDATE_SCALAR);
            if (check)
                start(v, check);
        }
    } else if (j->type == INDEX_LINES) {
        free(v->records);
        v->records = j->records;
        j->records = NULL;
        v->nrecords = j->nrecords;
        v->record_cap = j->record_cap;
        v->indexed = true;
        start_count(v, 0, 0, 0);
        if (v->waiting_hit)
            view_locate(v);
    }
    v->dirty = true;
}
void view_tick(View *v) {
    Job **link = &v->jobs;
    while (*link) {
        Job *j = *link;
        bool done = WaitForSingleObject(j->thread, 0) == WAIT_OBJECT_0;
        bool live = j->generation == v->generation && !stopped(&j->cancel);
        if (done)
            *link = j->next;
        if (live)
            partial(v, j);
        if (done) {
            if (live && !stopped(&j->cancel))
                complete(v, j);
            job_free(j);
            link = &v->jobs;
        } else
            link = &j->next;
    }
    if (v->dirty)
        rebuild(v);
}
void view_text(View *v, Row row, char *label, size_t lc, char *value, size_t vc) {
    *label = *value = 0;
    if (row.kind == RECORDS_ROOT) {
        snprintf(label, lc, "JSONL");
        snprintf(value, vc, "%zu records%s", v->nrecords, v->indexed ? "" : " · indexing");
        return;
    }
    if (row.kind == NEXT_PAGE) {
        snprintf(label, lc, "Load next page");
        return;
    }
    Reader r = doc_reader(v->doc, NULL);
    if (row.record != J_NONE)
        snprintf(label, lc, "#%zu · line %zu", row.record + 1, v->records[row.record].line);
    else if (!row.parent && !row.depth)
        snprintf(label, lc, "Root");
    else
        jlabel(&r, row.node, label, lc);
    Branch *b = row.record != J_NONE ? lookup(v, row.node.start) : NULL;
    if (row.node.kind == '!' || (b && b->error[0])) {
        size_t len = row.node.bound - row.node.start;
        if (len > vc / 2)
            len = vc / 2;
        snprintf(value, vc, "Invalid record · %.*s", (int)len,
                 (const char *)r.bytes + row.node.start);
    } else {
        jpreview(&r, row.node, value, vc);
        if (container(row.node)) {
            Branch *opened = lookup(v, row.node.start);
            if (opened && opened->open)
                snprintf(value, vc, "%s · %zu items%s", row.node.kind == '{' ? "{ … }" : "[ … ]",
                         opened->count,
                         opened->pending ? " · loading"
                         : opened->more  ? " · more available"
                                         : "");
        }
    }
    if (r.message)
        snprintf(value, vc, "byte %zu · %s", r.error, r.message);
}
