#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "failed line %d: %s\n", __LINE__, #expr);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static Reader input(const char *s) {
    return (Reader){.bytes = (const unsigned char *)s, .size = strlen(s), .error = J_NONE};
}
static void fixtures(void) {
    Reader r = input("{\"a\":1,\"a\":\"\\u4e2d\\u6587\",\"arr\":[{},true,null,-2.7E+3],\"face\":"
                     "\"\\ud83d\\ude03\"}");
    Node root, field;
    Path path = {0};
    size_t resume = 0;
    CHECK(jroot(&r, 0, r.size, &root));
    CHECK(jskip(&r, root) == r.size);
    CHECK(jchild(&r, root, 1, 0, &field) == 1);
    CHECK(jchild(&r, root, jskip(&r, field), 1, &field) == 1 && field.ordinal == 1);
    size_t hit = jsearch(&r, 0, r.size, "中文", VALUE_TEXT, &resume);
    CHECK(hit == field.start && jlocate(&r, root, hit, &path) && path.count == 2 &&
          path.nodes[1].ordinal == 1);
    CHECK(jsearch(&r, 0, r.size, "😃", VALUE_TEXT, &resume) != J_NONE);
    CHECK(jsearch(&r, 0, r.size, "a", KEY_TEXT, &resume) == 1);
    CHECK(jsearch(&r, resume, r.size, "a", KEY_TEXT, &resume) == 7);
    CHECK(jsearch(&r, 0, r.size, "false", RAW_TEXT, &resume) == J_NONE);
    free(path.nodes);
    const char *bad[] = {"[1,]",          "{\"a\":1", "[00]", "[\"\xff\"]",
                         "[\"\\ud834\"]", "[truex]",  "[\"x"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        r = input(bad[i]);
        CHECK(jroot(&r, 0, r.size, &root));
        CHECK(jskip(&r, root) == J_NONE && r.message);
    }
    r = input("\r\n {broken}\r\n \n{\"after\":1}");
    size_t at = 0, line = 0;
    Record rec;
    CHECK(next_record(&r, &at, &line, &rec) && rec.line == 2);
    CHECK(next_record(&r, &at, &line, &rec) && rec.line == 4 && rec.end == r.size);
    CHECK(!next_record(&r, &at, &line, &rec));
    size_t depth = 12000;
    char *nested = malloc(2 * depth + 2);
    CHECK(nested);
    memset(nested, '[', depth);
    nested[depth] = '0';
    memset(nested + depth + 1, ']', depth);
    nested[2 * depth + 1] = 0;
    r = input(nested);
    CHECK(jroot(&r, 0, r.size, &root) && jskip(&r, root) == r.size);
    free(nested);
}
/* Navigation needs a path stack; boundary scanning uses a compact delimiter
   stack. Exercise the two grammar paths on the same damaged/truncated input. */
static void check_scanners(char *text) {
    Reader direct = input(text), walk = direct;
    Node n;
    Path path = {0};
    if (!jroot(&direct, 0, direct.size, &n))
        return;
    bool complete = jskip(&direct, n) != J_NONE;
    CHECK(jlocate(&walk, n, walk.size, &path) == complete);
    free(path.nodes);
}
static void scanner_agreement(void) {
    char text[] = "{\"first\":[{\"escaped\":\"\\u4e2d\\n\"},true],\"last\":[[],{},null,-12.5e-3]}";
    const char replacements[] = "{},[]:a\"\\0";
    size_t length = strlen(text);
    for (size_t i = 0; i < length; i++) {
        char original = text[i];
        for (size_t j = 0; j < sizeof replacements; j++) {
            text[i] = replacements[j];
            check_scanners(text);
        }
        text[i] = original;
    }
}
static void search_options(void) {
    Reader r = input("{\"Ab.C\":\"\\u4e2d\\u6587\",\"item2\":23,\"item35\":35}");
    size_t next = 0;
    CHECK(jsearch(&r, 0, r.size, "ab.c", IGNORE_CASE, &next) == 2);
    CHECK(jsearch(&r, 0, r.size, "ab.c", RAW_TEXT, &next) == J_NONE);
    CHECK(jsearch(&r, 0, r.size, "abxc", IGNORE_CASE, &next) == J_NONE);
    size_t a = jsearch(&r, 0, r.size, "ITEM\\d+", IGNORE_CASE | REGEX_SEARCH, &next);
    CHECK(a != J_NONE &&
          jsearch(&r, next, r.size, "ITEM\\d+", IGNORE_CASE | REGEX_SEARCH, &next) > a);
    CHECK(jsearch(&r, 0, r.size, "^中.$", VALUE_TEXT | REGEX_SEARCH, &next) != J_NONE);
    CHECK(jsearch(&r, 0, r.size, "^itEM\\d+$", KEY_TEXT | IGNORE_CASE | REGEX_SEARCH, &next) !=
          J_NONE);
    CHECK(jsearch(&r, 0, r.size, "$", REGEX_SEARCH, &next) == r.size && next == r.size + 1);
    CHECK(jsearch(&r, next, r.size, "$", REGEX_SEARCH, &next) == J_NONE);
    CHECK(jsearch(&r, 0, r.size, "[", REGEX_SEARCH, &next) == J_NONE && r.pattern_error);
    r = input("\"Äb\"");
    CHECK(jsearch(&r, 0, r.size, "äB", IGNORE_CASE, &next) == 1);
    size_t len = 262144 + 10;
    char *cross = malloc(len + 1);
    CHECK(cross);
    memset(cross, 'x', len);
    memcpy(cross + 262142, "Ab.c", 4);
    cross[len] = 0;
    r = input(cross);
    CHECK(jsearch(&r, 0, r.size, "aB.C", IGNORE_CASE, &next) == 262142);
    free(cross);
}
typedef struct {
    Match items[16];
    size_t count;
} Hits;
static bool collect(void *context, Match m) {
    Hits *hits = context;
    CHECK(hits->count < 16);
    hits->items[hits->count++] = m;
    return true;
}
static void match_ranges(void) {
    Reader r = input("{\"pWsH\":\"pwsh-\\u0070wsh\",\"empty\":\"\"}");
    Hits hits = {0};
    jmatches(&r, 0, r.size, "pwsh", KEYS_AND_VALUES | IGNORE_CASE, false, collect, &hits);
    CHECK(hits.count == 3 && hits.items[0].start == 2 && hits.items[0].end == 6);
    CHECK(hits.items[1].start == 9 && hits.items[1].end == 13);
    CHECK(hits.items[2].start == 14 && hits.items[2].end == 23);
    hits.count = 0;
    jmatches(&r, 0, r.size, "^$", VALUE_TEXT | REGEX_SEARCH, false, collect, &hits);
    CHECK(hits.count == 1 && hits.items[0].start == hits.items[0].end);
    hits.count = 0;
    jmatches(&r, 0, r.size, "(?<=-)p.*h$", VALUE_TEXT | REGEX_SEARCH, false, collect, &hits);
    CHECK(hits.count == 1 && hits.items[0].start == 14 && hits.items[0].end == 23);
    r = input("{\"k\":\"\u212A\"}");
    hits.count = 0;
    jmatches(&r, 0, r.size, "k", KEYS_AND_VALUES | IGNORE_CASE, false, collect, &hits);
    CHECK(hits.count == 2 && hits.items[1].end - hits.items[1].start == 3);
}
static void sample(const wchar_t *dir, const wchar_t *file, size_t rows) {
    wchar_t path[32768];
    swprintf(path, 32768, L"%ls\\%ls", dir, file);
    char error[500];
    Document *doc = doc_open(path, NULL, error, sizeof error);
    CHECK(doc);
    Reader r = doc_reader(doc, NULL);
    size_t resume = 0;
    if (!rows) {
        Node root, child;
        char label[128];
        CHECK(jroot(&r, doc->start, r.size, &root));
        CHECK(jchild(&r, root, root.start + 1, 0, &child) == 1);
        jlabel(&r, child, label, sizeof label);
        CHECK(!strcmp(label, "Id"));
        size_t end = jskip(&r, root);
        CHECK(end != J_NONE && jwhite(&r, end, r.size) == doc->size);
        CHECK(jsearch(&r, doc->start, r.size, "Id", KEY_TEXT, &resume) == child.key);
    } else {
        Record rec, last = {0};
        size_t count = 0, p = doc->start, line = 0;
        while (next_record(&r, &p, &line, &rec)) {
            count++;
            last = rec;
        }
        CHECK(count == rows);
        Node end;
        CHECK(jroot(&r, last.start, last.end, &end));
        CHECK(jskip(&r, end) != J_NONE);
    }
    doc_release(doc);
}
/* Validate every value, including every nonempty JSONL record. Keep only the
   traversal stack, as in navigation. The timed interval includes open/map. */
static void benchmark(const wchar_t *path) {
    LARGE_INTEGER hz, begin, end;
    QueryPerformanceFrequency(&hz);
    for (int run = 0; run <= 5; ++run) {
        char error[256];
        Node root;
        Record rec;
        QueryPerformanceCounter(&begin);
        Document *doc = doc_open(path, NULL, error, sizeof error);
        CHECK(doc);
        Reader r = doc_reader(doc, NULL);
        size_t at = doc->start, line = 0, count = 0;
        do {
            size_t first = at, bound = r.size;
            if (doc->lines) {
                if (!next_record(&r, &at, &line, &rec))
                    break;
                first = rec.content;
                bound = rec.end;
            }
            CHECK(jroot(&r, first, bound, &root));
            size_t pos = jskip(&r, root);
            CHECK(pos != J_NONE && jwhite(&r, pos, bound) == bound);
            ++count;
        } while (doc->lines);
        QueryPerformanceCounter(&end);
        if (run)
            printf("{\"run\":%d,\"bytes\":%zu,\"documents\":%zu,\"full_ms\":%.3f}\n", run,
                   doc->size, count, 1000.0 * (end.QuadPart - begin.QuadPart) / hz.QuadPart);
        doc_release(doc);
    }
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && !wcscmp(argv[1], L"--bench")) {
        benchmark(argv[2]);
        return 0;
    }
    CHECK(argc == 1 || argc == 2);
    fixtures();
    scanner_agreement();
    search_options();
    match_ranges();
    if (argc == 1) {
        puts("C reader passed synthetic fixtures");
        return 0;
    }
    sample(argv[1], L"23MB.json", 0);
    sample(argv[1], L"30MB.json", 0);
    sample(argv[1], L"sdk-events_s.jsonl", 24325);
    sample(argv[1], L"sdk-events_x.jsonl", 50232);
    puts("C reader passed fixtures and four repository samples");
    return 0;
}
