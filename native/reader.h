#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define J_NONE SIZE_MAX
#define J_PAGE 128
typedef struct {
    const unsigned char *bytes;
    size_t size, error;
    const char *message;
    volatile LONG *cancel;
    bool pattern_error;
    char diagnostic[192];
} Reader;
typedef struct {
    size_t start, bound, key, key_end, ordinal;
    unsigned char kind;
} Node;
typedef struct {
    size_t start, end, line, content;
} Record;
typedef struct {
    Node *nodes;
    size_t count, capacity;
} Path;
typedef enum {
    RAW_TEXT,
    KEY_TEXT,
    VALUE_TEXT,
    KEYS_AND_VALUES,
    SCOPE_MASK = 3,
    IGNORE_CASE = 4,
    REGEX_SEARCH = 8
} SearchMode;
typedef struct {
    LONG refs;
    HANDLE file, mapping;
    const unsigned char *data;
    size_t size, start;
    bool lines, snapshot;
    BY_HANDLE_FILE_INFORMATION stamp;
    wchar_t *path;
} Document;

Document *doc_open(const wchar_t *path, volatile LONG *cancel, char *error, size_t cap);
Document *doc_clipboard(char *error, size_t cap);
void doc_release(Document *doc);
void doc_retain(Document *doc);
bool doc_changed(Document *doc);
bool reserve_items(void **ptr, size_t *capacity, size_t need, size_t stride);
static inline bool stopped(volatile LONG *cancel) {
    return cancel && ReadNoFence(cancel) != 0;
}
Reader doc_reader(Document *doc, volatile LONG *cancel);
bool jroot(Reader *r, size_t start, size_t bound, Node *out);
bool container(Node n);
bool jpath_identifier(const unsigned char *key, size_t len);
size_t jwhite(Reader *r, size_t pos, size_t end);
/* Returns 1 for a child, 0 for a closing delimiter, -1 for an error. */
int jchild(Reader *r, Node parent, size_t at, size_t ordinal, Node *out);
size_t jskip(Reader *r, Node node);
/* Add the number of values in a valid subtree, including its own root. */
size_t jskip_count(Reader *r, Node node, size_t *total);
char *jstring(Reader *r, size_t start, size_t end, size_t *len);
bool jlocate(Reader *r, Node root, size_t offset, Path *path);
size_t jsearch(Reader *r, size_t start, size_t end, const char *query, SearchMode mode,
               size_t *resume);
typedef struct {
    size_t start, end;
} Match;
typedef bool (*MatchVisitor)(void *context, Match match);
void jmatches(Reader *r, size_t start, size_t end, const char *query, SearchMode mode, bool lines,
              MatchVisitor visit, void *context);
void jstring_preview(Reader *r, size_t start, size_t end, char *out, size_t cap, size_t *source,
                     size_t *mapped);
void jlabel(Reader *r, Node n, char *out, size_t cap);
void jpreview(Reader *r, Node n, char *out, size_t cap);
bool next_record(Reader *r, size_t *at, size_t *line, Record *out);
