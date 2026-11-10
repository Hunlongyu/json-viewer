#include "reader.h"
#include <emmintrin.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcre2.h>

static size_t fail(Reader *r, size_t at, const char *message) {
    r->error = at;
    r->message = message;
    return J_NONE;
}
static bool ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
size_t jwhite(Reader *r, size_t p, size_t end) {
    while (p < end && ws(r->bytes[p])) {
        if (!(p & 65535) && stopped(r->cancel))
            return end;
        ++p;
    }
    return p;
}
bool container(Node n) {
    return n.kind == '{' || n.kind == '[';
}
bool jpath_identifier(const unsigned char *key, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$' ||
              (i && c >= '0' && c <= '9')))
            return false;
    }
    return len != 0;
}
static bool digit(unsigned char c) {
    return c >= '0' && c <= '9';
}
bool jroot(Reader *r, size_t start, size_t bound, Node *n) {
    size_t p = jwhite(r, start, bound);
    if (p >= bound || (!digit(r->bytes[p]) && (!r->bytes[p] || !strchr("{[\"-tfn", r->bytes[p])))) {
        fail(r, p, "Expected a JSON value");
        return false;
    }
    *n = (Node){p, bound, J_NONE, J_NONE, 0, r->bytes[p]};
    return true;
}
static int hex4(const unsigned char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned x = s[i];
        if (x >= '0' && x <= '9')
            x -= '0';
        else if ((x | 32) >= 'a' && (x | 32) <= 'f')
            x = (x | 32) - 'a' + 10;
        else
            return -1;
        v = v * 16 + (int)x;
    }
    return v;
}
/* One decoded UTF-8 scalar. The caller has already ruled out the closing quote. */
static int piece(Reader *r, size_t *at, size_t end, unsigned char out[4]) {
    size_t p = *at;
    const unsigned char *s = r->bytes;
    unsigned c = s[p++];
    int n = 1;
    if (c == '\\') {
        if (p == end)
            goto bad;
        c = s[p++];
        switch (c) {
        case '"':
        case '\\':
        case '/':
            break;
        case 'b':
            c = 8;
            break;
        case 'f':
            c = 12;
            break;
        case 'n':
            c = 10;
            break;
        case 'r':
            c = 13;
            break;
        case 't':
            c = 9;
            break;
        case 'u': {
            int v;
            if (end - p < 4 || (v = hex4(s + p)) < 0)
                goto bad;
            p += 4;
            if (v >= 0xd800 && v <= 0xdbff) {
                if (end - p < 6 || s[p] != '\\' || s[p + 1] != 'u')
                    goto bad;
                int lo = hex4(s + p + 2);
                if (lo < 0xdc00 || lo > 0xdfff)
                    goto bad;
                v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00);
                p += 6;
            } else if (v >= 0xdc00 && v <= 0xdfff)
                goto bad;
            c = (unsigned)v;
            break;
        }
        default:
            goto bad;
        }
    } else if (c >= 128) {
        n = c < 224 ? 2 : c < 240 ? 3 : 4;
        if (c < 194 || c > 244 || end - p < (size_t)(n - 1))
            goto utf;
        unsigned next = s[p];
        if ((c == 224 && next < 160) || (c == 237 && next >= 160) || (c == 240 && next < 144) ||
            (c == 244 && next >= 144))
            goto utf;
        out[0] = (unsigned char)c;
        for (int i = 1; i < n; i++) {
            if ((s[p] & 192) != 128)
                goto utf;
            out[i] = s[p++];
        }
        *at = p;
        return n;
    } else if (c < 32)
        goto bad;
    if (c < 128)
        out[0] = (unsigned char)c;
    else if (c < 2048) {
        n = 2;
        out[0] = 192 | (c >> 6);
        out[1] = 128 | (c & 63);
    } else if (c < 65536) {
        n = 3;
        out[0] = 224 | (c >> 12);
        out[1] = 128 | ((c >> 6) & 63);
        out[2] = 128 | (c & 63);
    } else {
        n = 4;
        out[0] = 240 | (c >> 18);
        out[1] = 128 | ((c >> 12) & 63);
        out[2] = 128 | ((c >> 6) & 63);
        out[3] = 128 | (c & 63);
    }
    *at = p;
    return n;
utf:
    fail(r, *at, "Invalid UTF-8");
    return 0;
bad:
    fail(r, *at, "Invalid string or escape");
    return 0;
}
/* Skip ASCII runs without reading past a mapped page or the end of a JSONL record. */
static size_t quote_end(Reader *r, size_t start, size_t end) {
    size_t p = start + 1, check = p;
    while (p < end) {
        if (p >= check) {
            if (stopped(r->cancel))
                return J_NONE;
            check = p + 65536;
        }
        size_t limit = check < end ? check : end;
        while (limit - p >= 16) {
            __m128i x = _mm_loadu_si128((const __m128i *)(r->bytes + p));
            __m128i mark = _mm_or_si128(_mm_cmplt_epi8(x, _mm_set1_epi8(32)),
                                        _mm_or_si128(_mm_cmpeq_epi8(x, _mm_set1_epi8('"')),
                                                     _mm_cmpeq_epi8(x, _mm_set1_epi8('\\'))));
            unsigned mask = (unsigned)_mm_movemask_epi8(mark);
            if (mask) {
                unsigned long bit;
                _BitScanForward(&bit, mask);
                p += bit;
                break;
            }
            p += 16;
        }
        if (p == end)
            break;
        if (r->bytes[p] == '"')
            return p + 1;
        unsigned char tmp[4];
        if (!piece(r, &p, end, tmp))
            return J_NONE;
    }
    return fail(r, end, "Unterminated string");
}
static size_t primitive(Reader *r, Node n) {
    const unsigned char *s = r->bytes;
    size_t p = n.start, end = n.bound;
    if (n.kind == '"')
        return quote_end(r, p, end);
    const char *word = n.kind == 't'   ? "true"
                       : n.kind == 'f' ? "false"
                       : n.kind == 'n' ? "null"
                                       : NULL;
    if (word) {
        size_t len = strlen(word);
        return end - p >= len && !memcmp(s + p, word, len) ? p + len
                                                           : fail(r, p, "Invalid literal");
    }
    if (s[p] == '-')
        ++p;
    if (p >= end || !digit(s[p]))
        return fail(r, p, "Invalid number");
    if (s[p++] != '0')
        while (p < end && digit(s[p])) {
            if (!(p & 65535) && stopped(r->cancel))
                return J_NONE;
            ++p;
        }
    if (p < end && s[p] == '.') {
        ++p;
        if (p == end || !digit(s[p]))
            return fail(r, p, "Expected a fractional digit");
        while (p < end && digit(s[p])) {
            if (!(p & 65535) && stopped(r->cancel))
                return J_NONE;
            ++p;
        }
    }
    if (p < end && (s[p] == 'e' || s[p] == 'E')) {
        ++p;
        if (p < end && (s[p] == '+' || s[p] == '-'))
            ++p;
        if (p == end || !digit(s[p]))
            return fail(r, p, "Expected an exponent digit");
        while (p < end && digit(s[p])) {
            if (!(p & 65535) && stopped(r->cancel))
                return J_NONE;
            ++p;
        }
    }
    return p;
}
int jchild(Reader *r, Node parent, size_t at, size_t ordinal, Node *out) {
    size_t end = parent.bound, p = jwhite(r, at, end), key = J_NONE, key_end = J_NONE;
    unsigned char close = parent.kind == '{' ? '}' : ']';
    if (p < end && r->bytes[p] == close)
        return 0;
    if (ordinal) {
        if (p == end || r->bytes[p] != ',') {
            fail(r, p, "Expected a comma or closing delimiter");
            return -1;
        }
        p = jwhite(r, p + 1, end);
    }
    if (parent.kind == '{') {
        if (p == end || r->bytes[p] != '"') {
            fail(r, p, "Expected an object key");
            return -1;
        }
        key = p;
        key_end = quote_end(r, p, end);
        if (key_end == J_NONE)
            return -1;
        p = jwhite(r, key_end, end);
        if (p == end || r->bytes[p] != ':') {
            fail(r, p, "Expected a colon after the key");
            return -1;
        }
        ++p;
    }
    if (!jroot(r, p, end, out))
        return -1;
    out->key = key;
    out->key_end = key_end;
    out->ordinal = ordinal;
    return 1;
}
typedef struct {
    Node n;
    size_t next;
} Frame;
static bool set_path(Reader *r, Path *path, Frame *stack, size_t depth, Node n) {
    if (!reserve_items((void **)&path->nodes, &path->capacity, depth + 1, sizeof(Node))) {
        fail(r, n.start, "Out of memory");
        return false;
    }
    for (size_t i = 0; i < depth; i++)
        path->nodes[i] = stack[i].n;
    path->nodes[depth] = n;
    path->count = depth + 1;
    return true;
}
/* A heap stack keeps even deeply nested files off the C call stack. */
static size_t walk(Reader *r, Node root, size_t target, Path *path) {
    Frame *stack = NULL;
    size_t depth = 0, cap = 0, p = root.start;
    Node n = root;
    for (;;) {
        if (stopped(r->cancel)) {
            p = J_NONE;
            break;
        }
        if (path && target <= n.start) {
            p = set_path(r, path, stack, depth, n) ? n.start : J_NONE;
            break;
        }
        if (container(n)) {
            if (depth == cap && !reserve_items((void **)&stack, &cap, depth + 1, sizeof *stack)) {
                p = fail(r, n.start, "Out of memory");
                break;
            }
            stack[depth++] = (Frame){n, 0};
            p = n.start + 1;
        } else {
            p = primitive(r, n);
            if (p == J_NONE)
                break;
            if (path && target < p) {
                if (!set_path(r, path, stack, depth, n))
                    p = J_NONE;
                break;
            }
        }
        while (depth) {
            Frame *f = &stack[depth - 1];
            size_t from = p;
            int step = jchild(r, f->n, p, f->next, &n);
            if (step < 0) {
                p = J_NONE;
                goto finish;
            }
            if (step) {
                f->next++;
                break;
            }
            p = jwhite(r, from, f->n.bound) + 1;
            if (path && target < p) {
                if (!set_path(r, path, stack, depth - 1, f->n))
                    p = J_NONE;
                goto finish;
            }
            --depth;
        }
        if (!depth)
            break;
    }
finish:
    free(stack);
    return p;
}
size_t jskip_count(Reader *r, Node n, size_t *total) {
    if (!container(n)) {
        size_t end = primitive(r, n);
        if (end != J_NONE && total)
            ++*total;
        return end;
    }
    /* Boundary scans only need the expected closing delimiter. Keeping the
       full Node/ordinal stack is useful for locating a hit but expensive
       when skipping millions of tiny values. */
    unsigned char local[64], *stack = local;
    size_t capacity = sizeof local, depth = 0, p = n.start, end = n.bound;
    const unsigned char *s = r->bytes;
    size_t poll = p;
    size_t values = 0;
    for (;;) {
        if (p >= poll) {
            if (stopped(r->cancel)) {
                p = J_NONE;
                break;
            }
            poll = p + 65536;
        }
        if (p >= end) {
            p = fail(r, p, "Expected a JSON value");
            break;
        }
        ++values;
        unsigned char c = s[p];
        if (c == '{' || c == '[') {
            if (depth == capacity) {
                unsigned char *larger = malloc(capacity * 2);
                if (!larger) {
                    p = fail(r, p, "Out of memory");
                    break;
                }
                memcpy(larger, stack, depth);
                if (stack != local)
                    free(stack);
                stack = larger;
                capacity *= 2;
            }
            stack[depth++] = (c == '{' ? '}' : ']');
            p = jwhite(r, p + 1, end);
            if (p == end || s[p] != stack[depth - 1])
                goto member;
            ++p;
            --depth;
        } else if (c == '"') {
            p = quote_end(r, p, end);
            if (p == J_NONE)
                break;
        } else {
            if (!digit(c) && c != '-' && c != 't' && c != 'f' && c != 'n') {
                p = fail(r, p, "Expected a JSON value");
                break;
            }
            n.start = p;
            n.kind = c;
            p = primitive(r, n);
            if (p == J_NONE)
                break;
        }
        while (depth) {
            p = jwhite(r, p, end);
            if (p < end && s[p] == stack[depth - 1]) {
                ++p;
                --depth;
                continue;
            }
            if (p == end || s[p] != ',') {
                p = fail(r, p, "Expected a comma or closing delimiter");
                goto finished;
            }
            p = jwhite(r, p + 1, end);
            break;
        }
        if (!depth)
            break;
    member:
        if (stack[depth - 1] == '}') {
            if (p == end || s[p] != '"') {
                p = fail(r, p, "Expected an object key");
                break;
            }
            p = quote_end(r, p, end);
            if (p == J_NONE)
                break;
            p = jwhite(r, p, end);
            if (p == end || s[p] != ':') {
                p = fail(r, p, "Expected a colon after the key");
                break;
            }
            p = jwhite(r, p + 1, end);
        }
    }
finished:
    if (stack != local)
        free(stack);
    if (p != J_NONE && total)
        *total += values;
    return p;
}
size_t jskip(Reader *r, Node n) {
    return jskip_count(r, n, NULL);
}
bool jlocate(Reader *r, Node root, size_t offset, Path *path) {
    path->count = 0;
    if (walk(r, root, offset, path) == J_NONE)
        return false;
    return path->count || set_path(r, path, NULL, 0, root);
}
void jstring_preview(Reader *r, size_t p, size_t end, char *out, size_t cap, size_t *source,
                     size_t *mapped) {
    size_t used = 0;
    ++p;
    while (p < end && r->bytes[p] != '"' && used + 8 < cap) {
        size_t raw = p, before = used;
        unsigned char part[4];
        int n = piece(r, &p, end, part);
        if (!n)
            break;
        if (n == 1 && part[0] < 32) {
            const char *shown = part[0] == '\n'   ? "\\n"
                                : part[0] == '\t' ? "\\t"
                                : part[0] == '\r' ? "\\r"
                                                  : NULL;
            if (shown) {
                memcpy(out + used, shown, 2);
                used += 2;
            } else
                used += snprintf(out + used, cap - used, "\\u%04x", part[0]);
        } else {
            memcpy(out + used, part, n);
            used += n;
        }
        if (source)
            for (size_t i = before; i < used; ++i)
                source[i] = raw;
    }
    if (source) {
        source[used] = p;
        *mapped = used;
    }
    if (p < end && r->bytes[p] != '"' && used + 4 < cap) {
        memcpy(out + used, "…", 3);
        used += 3;
    }
    out[used] = 0;
}
void jlabel(Reader *r, Node n, char *out, size_t cap) {
    if (n.key != J_NONE)
        jstring_preview(r, n.key, n.key_end, out, cap, NULL, NULL);
    else
        snprintf(out, cap, "[%zu]", n.ordinal);
}
void jpreview(Reader *r, Node n, char *out, size_t cap) {
    if (container(n))
        snprintf(out, cap, "%s", n.kind == '{' ? "{ … }" : "[ … ]");
    else if (n.kind == '"')
        jstring_preview(r, n.start, n.bound, out, cap, NULL, NULL);
    else {
        size_t p = n.start;
        while (p < n.bound && p - n.start + 4 < cap && !ws(r->bytes[p]) &&
               !strchr(",]}", r->bytes[p]))
            ++p;
        size_t count = p - n.start;
        memcpy(out, r->bytes + n.start, count);
        out[count] = 0;
    }
}
static size_t find_bytes(Reader *r, size_t p, size_t end, const unsigned char *q, size_t len) {
    if (!len || end - p < len)
        return J_NONE;
    size_t stop = end - len + 1;
    while (p < stop && !stopped(r->cancel)) {
        size_t limit = stop - p > 65536 ? p + 65536 : stop;
        while (p < limit) {
            const unsigned char *s = memchr(r->bytes + p, q[0], limit - p);
            if (!s) {
                p = limit;
                break;
            }
            p = (size_t)(s - r->bytes);
            if (!memcmp(s, q, len))
                return p;
            ++p;
        }
    }
    return J_NONE;
}
/* Decode one complete quoted token; unlike a display preview this preserves
   embedded NUL and never clips the value. Escapes cannot expand the source. */
char *jstring(Reader *r, size_t start, size_t end, size_t *len) {
    if (end - start < 2)
        return NULL;
    char *s = malloc(end - start);
    if (!s) {
        fail(r, start, "Out of memory");
        return NULL;
    }
    size_t p = start + 1, n = 0, check = p;
    while (p < end - 1) {
        if (p >= check) {
            if (stopped(r->cancel)) {
                free(s);
                return NULL;
            }
            check = p + 65536;
        }
        unsigned char part[4];
        int used = piece(r, &p, end - 1, part);
        if (!used) {
            free(s);
            return NULL;
        }
        memcpy(s + n, part, used);
        n += used;
    }
    s[n] = 0;
    *len = n;
    return s;
}
typedef struct {
    pcre2_code *code;
    pcre2_match_data *data;
    pcre2_match_context *context;
} Pattern;
static int poll_regex(pcre2_callout_block *block, void *arg) {
    (void)block;
    return stopped(((Reader *)arg)->cancel) ? PCRE2_ERROR_CALLOUT : 0;
}
static size_t pattern_error(Reader *r, int error, size_t offset) {
    PCRE2_UCHAR msg[128] = "";
    pcre2_get_error_message(error, msg, sizeof msg);
    snprintf(r->diagnostic, sizeof r->diagnostic, "Find: %s (offset %zu)", (char *)msg, offset);
    r->pattern_error = true;
    return fail(r, offset, r->diagnostic);
}
static bool pattern_init(Pattern *pat, Reader *r, const char *query, SearchMode mode) {
    int err;
    PCRE2_SIZE at;
    uint32_t options = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF;
    if (mode & REGEX_SEARCH)
        options |= PCRE2_UCP | PCRE2_AUTO_CALLOUT | PCRE2_MULTILINE;
    else
        options |= PCRE2_LITERAL;
    if (mode & IGNORE_CASE)
        options |= PCRE2_CASELESS;
    pat->code = pcre2_compile((PCRE2_SPTR)query, PCRE2_ZERO_TERMINATED, options, &err, &at, NULL);
    if (!pat->code) {
        pattern_error(r, err, at);
        return false;
    }
    pat->data = pcre2_match_data_create_from_pattern(pat->code, NULL);
    pat->context = pcre2_match_context_create(NULL);
    if (!pat->context || !pat->data) {
        pattern_error(r, PCRE2_ERROR_NOMEMORY, 0);
        return false;
    }
    pcre2_set_callout(pat->context, poll_regex, r);
    return true;
}
static size_t pattern_find(Pattern *pat, Reader *r, const unsigned char *s, size_t len, size_t from,
                           size_t *next) {
    int rc = pcre2_match(pat->code, s, len, from, 0, pat->data, pat->context);
    if (rc < 0) {
        if (rc != PCRE2_ERROR_NOMATCH && !stopped(r->cancel))
            pattern_error(r, rc, from);
        return J_NONE;
    }
    PCRE2_SIZE *range = pcre2_get_ovector_pointer(pat->data);
    *next = range[1];
    if (range[0] == range[1]) {
        ++*next;
        while (*next < len && (s[*next] & 0xc0) == 0x80)
            ++*next;
    }
    return range[0];
}
typedef struct {
    Pattern pattern;
    const char *query;
    size_t length;
    SearchMode mode;
    MatchVisitor visit;
    void *context;
    size_t found, resume;
    bool regex, advanced, ascii_query;
} Scan;

/* Translate monotonically increasing decoded-byte boundaries back to the source.
   Only escaped tokens need this mapping. */
static size_t source_position(Reader *r, size_t wanted, size_t limit, size_t *raw,
                              size_t *decoded) {
    while (*decoded < wanted && *raw < limit) {
        unsigned char part[4];
        int n = piece(r, raw, limit, part);
        if (!n)
            break;
        *decoded += n;
    }
    return *raw;
}

static bool ascii_subject(const unsigned char *s, size_t n, volatile LONG *cancel) {
    size_t i = 0;
    for (; n - i >= 16; i += 16) {
        if (_mm_movemask_epi8(_mm_loadu_si128((const __m128i *)(s + i))))
            return false;
        if (!(i & 65535) && stopped(cancel))
            return false;
    }
    while (i < n)
        if (s[i++] & 128)
            return false;
    return true;
}
static unsigned char ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
static size_t find_folded(const unsigned char *s, size_t size, size_t from, const char *query,
                          size_t length, volatile LONG *cancel) {
    if (size - from < length)
        return J_NONE;
    const size_t stop = size - length + 1;
    unsigned char low = ascii_lower((unsigned char)query[0]);
    unsigned char high = low >= 'a' && low <= 'z' ? low - 32 : low;
    __m128i a = _mm_set1_epi8(low), b = _mm_set1_epi8(high);
    while (from < stop && !stopped(cancel)) {
        size_t candidates = min(stop - from, 16);
        unsigned mask = 0;
        if (candidates == 16) {
            __m128i x = _mm_loadu_si128((const __m128i *)(s + from));
            mask = (unsigned)_mm_movemask_epi8(
                _mm_or_si128(_mm_cmpeq_epi8(x, a), _mm_cmpeq_epi8(x, b)));
        } else
            for (unsigned k = 0; k < candidates; ++k)
                if (s[from + k] == low || s[from + k] == high)
                    mask |= 1u << k;
        while (mask) {
            unsigned long bit;
            _BitScanForward(&bit, mask);
            size_t at = from + bit, i = 1;
            while (i < length && ascii_lower(s[at + i]) == ascii_lower((unsigned char)query[i]))
                ++i;
            if (i == length)
                return at;
            mask &= mask - 1;
        }
        from += candidates;
    }
    return J_NONE;
}
static bool scan_subject(Scan *scan, Reader *r, const unsigned char *text, size_t size, size_t base,
                         size_t token, size_t token_end, bool escaped, size_t from) {
    Reader subject = {.bytes = text, .size = size, .cancel = r->cancel};
    size_t raw = base, decoded = 0;
    bool fold =
        scan->advanced && !scan->regex && scan->ascii_query && ascii_subject(text, size, r->cancel);
    if (fold && size < scan->length)
        return true;
    while (from <= size && !stopped(r->cancel)) {
        size_t first, end, next;
        if (!scan->advanced || fold) {
            first = fold ? find_folded(text, size, from, scan->query, scan->length, r->cancel)
                         : find_bytes(&subject, from, size, (const unsigned char *)scan->query,
                                      scan->length);
            if (first == J_NONE)
                break;
            next = end = first + scan->length;
        } else {
            /* Literal lookahead fits in a bounded UTF-8 window, including Unicode
               case folding. Keep regex subjects whole for anchors and lookaround. */
            size_t window = size;
            bool chunk = !scan->regex && size - from > 262144 + scan->length * 4 + 4;
            if (chunk)
                window = from + 262144 + scan->length * 4 + 4;
            if (scan->regex)
                first = pattern_find(&scan->pattern, r, text, size, from, &next);
            else {
                first = pattern_find(&scan->pattern, r, text + from, window - from, 0, &next);
                if (first != J_NONE) {
                    first += from;
                    next += from;
                }
            }
            if (first == J_NONE) {
                if (!chunk || r->message)
                    break;
                from += 262144;
                while (from < size && (text[from] & 0xc0) == 0x80)
                    ++from;
                continue;
            }
            size_t *range = pcre2_get_ovector_pointer(scan->pattern.data);
            end = first + (range[1] - range[0]);
        }
        Match m = {base + first, base + end};
        if (escaped) {
            m.start = source_position(r, first, token_end - 1, &raw, &decoded);
            m.end = source_position(r, end, token_end - 1, &raw, &decoded);
        }
        if (!scan->visit) {
            scan->found = token == J_NONE ? m.start : token;
            scan->resume = token == J_NONE ? base + next : token_end;
            return false;
        }
        if (!scan->visit(scan->context, m))
            return false;
        from = next;
    }
    return !r->message && !stopped(r->cancel);
}
static void scan_document(Scan *scan, Reader *r, size_t start, size_t end, bool lines) {
    if (!scan->length || start > end)
        return;
    scan->regex = (scan->mode & REGEX_SEARCH) != 0;
    scan->advanced = scan->regex;
    if (scan->mode & IGNORE_CASE) {
        scan->ascii_query = true;
        for (const unsigned char *c = (const unsigned char *)scan->query; *c; ++c) {
            if (*c >= 128)
                scan->ascii_query = false;
            if (*c >= 128 || ((*c | 32) >= 'a' && (*c | 32) <= 'z'))
                scan->advanced = true;
        }
    }
    if (scan->advanced && !pattern_init(&scan->pattern, r, scan->query, scan->mode))
        goto done;
    SearchMode scope = scan->mode & SCOPE_MASK;
    if (scope == RAW_TEXT) {
        size_t bom = end >= 3 && !memcmp(r->bytes, "\xef\xbb\xbf", 3) ? 3 : 0;
        scan_subject(scan, r, r->bytes + bom, end - bom, bom, J_NONE, end, false,
                     max(start, bom) - bom);
    } else
        for (size_t p = start; p < end && !stopped(r->cancel);) {
            unsigned char c = r->bytes[p];
            if (ws(c) || c == ':' || c == ',' || c == '{' || c == '[' || c == ']' || c == '}') {
                ++p;
                continue;
            }
            Node n;
            size_t next = jroot(r, p, end, &n) ? primitive(r, n) : J_NONE;
            if (next == J_NONE) {
                if (!lines || stopped(r->cancel))
                    break;
                /* Resume at the next record even if this token was malformed. */
                while (p < end && r->bytes[p++] != '\n') {
                    if (!(p & 65535) && stopped(r->cancel))
                        goto done;
                }
                r->error = J_NONE;
                r->message = NULL;
                continue;
            }
            size_t after = jwhite(r, next, end);
            bool key = c == '"' && after < end && r->bytes[after] == ':';
            if (scope == KEYS_AND_VALUES || key == (scope == KEY_TEXT)) {
                bool quoted = c == '"';
                size_t base = p + quoted, size = next - base - quoted;
                const unsigned char *text = r->bytes + base;
                char *decoded = NULL;
                if (quoted && memchr(text, '\\', size)) {
                    decoded = jstring(r, p, next, &size);
                    if (!decoded)
                        break;
                    text = (const unsigned char *)decoded;
                }
                bool more = scan_subject(scan, r, text, size, base, p, next, decoded != NULL, 0);
                free(decoded);
                if (!more)
                    break;
            }
            p = next;
        }
done:
    pcre2_match_context_free(scan->pattern.context);
    pcre2_match_data_free(scan->pattern.data);
    pcre2_code_free(scan->pattern.code);
}
void jmatches(Reader *r, size_t start, size_t end, const char *query, SearchMode mode, bool lines,
              MatchVisitor visit, void *context) {
    Scan scan = {
        .query = query, .length = strlen(query), .mode = mode, .visit = visit, .context = context};
    scan_document(&scan, r, start, end, lines);
}
size_t jsearch(Reader *r, size_t start, size_t end, const char *query, SearchMode mode,
               size_t *resume) {
    Scan scan = {.query = query, .length = strlen(query), .mode = mode, .found = J_NONE};
    scan_document(&scan, r, start, end, false);
    if (scan.found != J_NONE)
        *resume = scan.resume;
    return scan.found;
}
bool next_record(Reader *r, size_t *at, size_t *line, Record *out) {
    while (*at < r->size && !stopped(r->cancel)) {
        size_t p = *at;
        while (p < r->size) {
            size_t n = r->size - p > 262144 ? 262144 : r->size - p;
            const unsigned char *lf = memchr(r->bytes + p, '\n', n);
            if (lf) {
                p = (size_t)(lf - r->bytes);
                break;
            }
            p += n;
            if (stopped(r->cancel))
                return false;
        }
        size_t start = *at;
        *at = p + (p < r->size);
        ++*line;
        size_t content = jwhite(r, start, p);
        if (content < p) {
            *out = (Record){start, p, *line, content};
            return true;
        }
    }
    return false;
}
