#include "jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <objbase.h>

typedef struct {
    Job *job;
    char *memory;
    size_t used, capacity;
    FILE *file;
} Output;
static bool put(Output *o, const void *data, size_t size) {
    if (stopped(&o->job->cancel))
        return false;
    if (o->file)
        return fwrite(data, 1, size, o->file) == size;
    if (size > INT_MAX - o->used ||
        !reserve_items((void **)&o->memory, &o->capacity, o->used + size + 1, 1))
        return false;
    memcpy(o->memory + o->used, data, size);
    o->used += size;
    o->memory[o->used] = 0;
    return true;
}
static bool newline(Output *o, size_t depth) {
    static const char spaces[] = "                                                                ";
    if (!put(o, "\r\n", 2))
        return false;
    for (size_t n = depth * 2; n;) {
        size_t k = n > sizeof spaces - 1 ? sizeof spaces - 1 : n;
        if (!put(o, spaces, k))
            return false;
        n -= k;
    }
    return true;
}
/* The scanner has validated this slice. Preserve tokens byte-for-byte,
   including duplicate keys, number spellings and whitespace inside strings. */
static bool format(Output *o, Reader *r, size_t p, size_t end, bool pretty) {
    const unsigned char *s = r->bytes;
    size_t depth = 0;
    unsigned char prev = 0;
    while (p < end && !stopped(r->cancel)) {
        unsigned char c = s[p];
        if (c <= 32) {
            ++p;
            continue;
        }
        if (c == '}' || c == ']')
            --depth;
        bool empty = (prev == '{' && c == '}') || (prev == '[' && c == ']');
        if (pretty && !empty &&
            (prev == '{' || prev == '[' || prev == ',' || c == '}' || c == ']')) {
            if (!newline(o, depth))
                return false;
        } else if (pretty && prev == ':' && !put(o, " ", 1))
            return false;
        size_t start = p++;
        if (c == '"') {
            while (p < end) {
                unsigned char b = s[p++];
                if (b == '"')
                    break;
                if (b == '\\')
                    ++p;
                if (p - start >= 65536) {
                    if (!put(o, s + start, p - start))
                        return false;
                    start = p;
                }
            }
        } else if (c == '{' || c == '[')
            ++depth;
        else if (!strchr(",:}]", c)) {
            while (p < end && s[p] > 32 && !strchr(",]}", s[p])) {
                ++p;
                if (p - start >= 65536) {
                    if (!put(o, s + start, p - start))
                        return false;
                    start = p;
                }
            }
        }
        if (!put(o, s + start, p - start))
            return false;
        prev = c;
    }
    return !stopped(r->cancel);
}
static bool path_text(Output *o, Reader *r) {
    Job *j = o->job;
    Path path = {0};
    bool ok = put(o, "$", 1);
    if (ok && j->record_start != J_NONE) {
        char s[40];
        int n = snprintf(s, sizeof s, "[%zu]", j->record_start);
        ok = put(o, s, n);
    }
    if (ok && j->parent.start != j->root.start)
        ok = jlocate(r, j->root, j->parent.start, &path);
    for (size_t i = 1; ok && i < path.count; i++) {
        Node n = path.nodes[i];
        if (n.key == J_NONE) {
            char s[40];
            int len = snprintf(s, sizeof s, "[%zu]", n.ordinal);
            ok = put(o, s, len);
            continue;
        }
        const unsigned char *key = r->bytes + n.key + 1;
        size_t len = n.key_end - n.key - 2;
        if (jpath_identifier(key, len))
            ok = put(o, ".", 1) && put(o, key, len);
        else
            ok = put(o, "[", 1) && put(o, key - 1, len + 2) && put(o, "]", 1);
    }
    free(path.nodes);
    return ok;
}
static bool content(Output *o, Reader *r) {
    Job *j = o->job;
    Node n = j->parent;
    if (j->action == COPY_PATH)
        return path_text(o, r);
    if (j->whole_document && (j->action == COPY_JSON || j->action == EXPORT_JSON)) {
        size_t pos = j->filename ? 0 : j->doc->start;
        while (pos < r->size) {
            size_t length = min(r->size - pos, (size_t)65536);
            if (!put(o, r->bytes + pos, length))
                return false;
            pos += length;
        }
        return true;
    }
    bool key = j->action == COPY_NAME;
    if (key && n.key == J_NONE)
        return false;
    size_t start = key ? n.key : n.start;
    size_t end = key ? n.key_end : n.kind == '!' ? n.bound : jskip(r, n);
    if (end == J_NONE)
        return false;
    if (key || (j->action == COPY_VALUE && n.kind == '"')) {
        size_t len = 0;
        char *s = jstring(r, start, end, &len);
        if (!s)
            return false;
        /* CF_UNICODETEXT cannot contain an interior NUL. Use the lossless
           quoted JSON representation for that particular string. */
        bool ok = memchr(s, 0, len) ? put(o, r->bytes + start, end - start) : put(o, s, len);
        free(s);
        return ok;
    }
    if (j->action == COPY_VALUE || j->action == COPY_JSON || j->action == EXPORT_JSON)
        return put(o, r->bytes + start, end - start);
    return format(o, r, start, end, j->action == COPY_FORMATTED || j->action == EXPORT_FORMATTED);
}
static bool same_file(HANDLE a, HANDLE b) {
    BY_HANDLE_FILE_INFORMATION x, y;
    return GetFileInformationByHandle(a, &x) && GetFileInformationByHandle(b, &y) &&
           x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
           x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}
static bool export_file(Output *o, Reader *r) {
    Job *j = o->job;
    HANDLE out = CreateFileW(j->filename, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, 0, NULL);
    if (out != INVALID_HANDLE_VALUE && j->doc->path) {
        HANDLE src = CreateFileW(j->doc->path, FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                 OPEN_EXISTING, 0, NULL);
        bool same =
            same_file(j->doc->file, out) || (src != INVALID_HANDLE_VALUE && same_file(src, out));
        if (src != INVALID_HANDLE_VALUE)
            CloseHandle(src);
        CloseHandle(out);
        if (same) {
            strcpy(j->error, "Choose a destination other than the open document");
            return false;
        }
    } else if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    GUID id;
    wchar_t suffix[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, suffix, 40))
        return false;
    size_t cap = wcslen(j->filename) + 48;
    wchar_t *tmp = malloc(cap * sizeof *tmp);
    if (!tmp)
        return false;
    swprintf(tmp, cap, L"%ls.%ls.tmp", j->filename, suffix);
    o->file = _wfopen(tmp, L"wbx");
    bool ok = false;
    if (o->file) {
        ok = content(o, r);
        if (fclose(o->file))
            ok = false;
        if (ok && !stopped(r->cancel))
            ok = MoveFileExW(tmp, j->filename,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        else
            ok = false;
        if (!ok)
            DeleteFileW(tmp);
    }
    free(tmp);
    return ok;
}
void prepare_transfer(Job *j, Reader *r) {
    Output out = {.job = j};
    bool ok = false;
    if (j->filename)
        ok = export_file(&out, r);
    else if (content(&out, r) && !stopped(r->cancel)) {
        int len = MultiByteToWideChar(CP_UTF8, 0, out.memory, (int)out.used, NULL, 0);
        if (len || !out.used) {
            j->clipboard = GlobalAlloc(GMEM_MOVEABLE, ((size_t)len + 1) * sizeof(wchar_t));
            wchar_t *w = j->clipboard ? GlobalLock(j->clipboard) : NULL;
            if (w) {
                ok = !len ||
                     MultiByteToWideChar(CP_UTF8, 0, out.memory, (int)out.used, w, len) == len;
                w[len] = 0;
                GlobalUnlock(j->clipboard);
            }
        }
    }
    free(out.memory);
    if (!ok && !j->error[0])
        snprintf(j->error, sizeof j->error, "%s failed", j->filename ? "Export" : "Copy");
}
