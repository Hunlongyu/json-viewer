#include "jobs.h"
#include <process.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void publish(Job *j, LONG count) {
    InterlockedExchange(&j->published, count);
    if (count <= 2 || !(count % 16))
        PostMessageW(j->window, WM_NULL, 0, 0);
}
static void check_root_end(Reader *r, size_t at, size_t bound) {
    size_t close = jwhite(r, at, bound);
    size_t tail = jwhite(r, close + 1, bound);
    if (tail < bound) {
        r->error = tail;
        r->message = "Unexpected content after the root value";
    }
}
static void page(Job *j, Reader *r) {
    LONG count = 0;
    while (count < J_PAGE && !stopped(&j->cancel)) {
        Node child;
        int step = jchild(r, j->parent, j->pos, j->ordinal, &child);
        if (step <= 0) {
            if (!step && j->parent.start == j->root.start)
                check_root_end(r, j->pos, j->parent.bound);
            return;
        }
        j->page[count++] = child;
        publish(j, count);
        size_t end = jskip_count(r, child, j->count_root_page ? &j->value_count : NULL);
        if (end == J_NONE)
            return;
        j->pos = end;
        ++j->ordinal;
    }
    size_t next = jwhite(r, j->pos, j->parent.bound);
    j->more = next < j->parent.bound && r->bytes[next] == ',';
    if (!j->more && !stopped(&j->cancel)) {
        Node unused;
        if (!jchild(r, j->parent, j->pos, j->ordinal, &unused) && j->parent.start == j->root.start)
            check_root_end(r, j->pos, j->parent.bound);
    }
}
/* A normal object's first page has already counted the subtrees it skipped.
   Continue from that cursor without rescanning them or allocating more rows. */
static void count_values(Job *j, Reader *r) {
    if (j->doc->lines) {
        size_t at = j->doc->start, line = 0;
        Record rec;
        while (next_record(r, &at, &line, &rec)) {
            Node root;
            size_t values = 0;
            if (!jroot(r, rec.content, rec.end, &root))
                return;
            size_t end = jskip_count(r, root, &values);
            if (end == J_NONE)
                return;
            if (jwhite(r, end, rec.end) != rec.end) {
                r->error = end;
                r->message = "Trailing record content";
                return;
            }
            j->value_count += values - 1;
        }
        return;
    }
    while (!stopped(&j->cancel)) {
        Node child;
        int step = jchild(r, j->root, j->pos, j->ordinal, &child);
        if (step <= 0) {
            if (!step)
                check_root_end(r, j->pos, j->root.bound);
            return;
        }
        j->pos = jskip_count(r, child, &j->value_count);
        if (j->pos == J_NONE)
            return;
        ++j->ordinal;
    }
}
static void index_lines(Job *j, Reader *r) {
    size_t p = j->doc->start, line = 0;
    Record rec;
    while (next_record(r, &p, &line, &rec)) {
        if (!reserve_items((void **)&j->records, &j->record_cap, j->nrecords + 1, sizeof rec)) {
            snprintf(j->error, sizeof j->error, "Out of memory building index");
            return;
        }
        j->records[j->nrecords++] = rec;
        if (j->nrecords <= J_PAGE) {
            j->first[j->nrecords - 1] = rec;
            publish(j, (LONG)j->nrecords);
        }
    }
}
static void locate(Job *j, Reader *r) {
    Node root = j->root;
    if (j->doc->lines) {
        size_t start = j->hit, end = j->hit;
        while (start > j->doc->start && r->bytes[start - 1] != '\n') {
            if (!(start & 65535) && stopped(&j->cancel))
                return;
            --start;
        }
        while (end < r->size && r->bytes[end] != '\n') {
            if (!(end & 65535) && stopped(&j->cancel))
                return;
            ++end;
        }
        j->record_start = start;
        if (!jroot(r, start, end, &root))
            return;
    }
    jlocate(r, root, j->hit, &j->path);
}
static unsigned put_delta(unsigned char *bytes, size_t n) {
    unsigned used = 0;
    while (n >= 128) {
        bytes[used++] = (unsigned char)(n | 128);
        n >>= 7;
    }
    bytes[used++] = (unsigned char)n;
    return used;
}
static size_t get_delta(MatchCursor *c) {
    size_t n = 0;
    for (unsigned shift = 0;; shift += 7) {
        unsigned byte = c->block->bytes[c->offset++];
        n |= (size_t)(byte & 127) << shift;
        if (byte < 128)
            return n;
    }
}
bool match_next(MatchCursor *c, Match *out) {
    if (c->ordinal >= c->block->count)
        return false;
    out->start = c->position + get_delta(c);
    out->end = out->start + get_delta(c);
    c->position = out->end;
    ++c->ordinal;
    return true;
}
static void flush_matches(Job *j) {
    MatchBlock *b = j->building;
    if (!b)
        return;
    bool first = !b->first_index;
    AcquireSRWLockExclusive(&j->match_lock);
    if (j->tail)
        j->tail->next = b;
    else
        j->pending = b;
    j->tail = b;
    ReleaseSRWLockExclusive(&j->match_lock);
    j->building = NULL;
    if (first)
        PostMessageW(j->window, WM_NULL, 0, 0);
}
static bool save_match(void *context, Match m) {
    Job *j = context;
    MatchBlock *b = j->building;
    if (b && b->used + 20 > sizeof b->bytes) {
        flush_matches(j);
        b = NULL;
    }
    if (!b) {
        j->building = b = calloc(1, sizeof *b);
        if (!b) {
            snprintf(j->error, sizeof j->error, "Out of memory storing matches");
            return false;
        }
        b->base = b->last = m.start;
        b->first_index = j->matches;
    }
    b->used += put_delta(b->bytes + b->used, m.start - b->last);
    b->used += put_delta(b->bytes + b->used, m.end - m.start);
    b->last = m.end;
    ++b->count;
    if (!j->matches++)
        flush_matches(j);
    return !stopped(&j->cancel);
}
static void find(Job *j, Reader *r) {
    jmatches(r, j->doc->start, r->size, j->query, j->mode, j->doc->lines, save_match, j);
    flush_matches(j);
    QueryPerformanceCounter(&j->find_finished);
}
static unsigned __stdcall run(void *arg) {
    Job *j = arg;
    if (j->type == OPEN_FILE)
        j->doc = j->filename ? doc_open(j->filename, &j->cancel, j->error, sizeof j->error)
                             : doc_clipboard(j->error, sizeof j->error);
    if (j->doc && !stopped(&j->cancel)) {
        Reader r = doc_reader(j->doc, &j->cancel);
        switch (j->type) {
        case OPEN_FILE:
            if (!j->doc->lines)
                jroot(&r, j->doc->start, r.size, &j->root);
            break;
        case VALIDATE_SCALAR: {
            size_t end = jskip(&r, j->root);
            if (end != J_NONE && jwhite(&r, end, r.size) < r.size) {
                r.error = end;
                r.message = "Unexpected content after the root value";
            }
            break;
        }
        case INDEX_LINES:
            index_lines(j, &r);
            break;
        case COUNT_VALUES:
            count_values(j, &r);
            break;
        case READ_PAGE:
            page(j, &r);
            break;
        case FIND_TEXT:
            find(j, &r);
            break;
        case LOCATE_HIT:
            locate(j, &r);
            break;
        case TRANSFER_NODE:
            prepare_transfer(j, &r);
            break;
        }
        if (r.pattern_error)
            snprintf(j->error, sizeof j->error, "%s", r.message);
        else if (r.message)
            snprintf(j->error, sizeof j->error, "byte %zu · %s", r.error, r.message);
    }
    PostMessageW(j->window, WM_NULL, 0, 0);
    return 0;
}
bool job_launch(Job *j) {
    j->thread = (HANDLE)_beginthreadex(NULL, 0, run, j, 0, NULL);
    return j->thread != NULL;
}
void job_free(Job *j) {
    if (j->thread)
        CloseHandle(j->thread);
    doc_release(j->doc);
    while (j->pending) {
        MatchBlock *next = j->pending->next;
        free(j->pending);
        j->pending = next;
    }
    free(j->building);
    free(j->filename);
    free(j->records);
    free(j->path.nodes);
    if (j->clipboard)
        GlobalFree(j->clipboard);
    free(j);
}
