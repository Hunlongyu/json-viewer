#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

bool reserve_items(void **ptr, size_t *capacity, size_t need, size_t stride) {
    if (need <= *capacity)
        return true;
    if (!stride || need > SIZE_MAX / stride)
        return false;
    size_t n = *capacity < 16 ? 16 : *capacity;
    while (n < need && n <= SIZE_MAX / stride / 2)
        n *= 2;
    if (n < need)
        n = need;
    void *p = realloc(*ptr, n * stride);
    if (!p)
        return false;
    *ptr = p;
    *capacity = n;
    return true;
}
static bool same_stamp(BY_HANDLE_FILE_INFORMATION a, BY_HANDLE_FILE_INFORMATION b) {
    return a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
           !CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) &&
           a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}
bool doc_changed(Document *d) {
    if (!d->snapshot)
        return false;
    HANDLE f = CreateFileW(d->path, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return true;
    BY_HANDLE_FILE_INFORMATION s;
    bool changed = !GetFileInformationByHandle(f, &s) || !same_stamp(s, d->stamp);
    CloseHandle(f);
    return changed;
}
void doc_retain(Document *d) {
    if (d)
        InterlockedIncrement(&d->refs);
}
void doc_release(Document *d) {
    if (!d || InterlockedDecrement(&d->refs))
        return;
    if (d->mapping) {
        UnmapViewOfFile(d->data);
        CloseHandle(d->mapping);
    }
    if (d->file != INVALID_HANDLE_VALUE)
        CloseHandle(d->file);
    free(d->path);
    free(d);
}
/* The source can have an active writer. Only map a verified private copy. */
static DWORD snapshot(Document *d, volatile LONG *cancel) {
    HANDLE src =
        CreateFileW(d->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (src == INVALID_HANDLE_VALUE)
        return GetLastError();
    DWORD error = 0;
    wchar_t dir[MAX_PATH + 1], tmp[MAX_PATH + 1];
    DWORD dirlen = GetTempPathW(MAX_PATH, dir);
    if (!GetFileInformationByHandle(src, &d->stamp) || !dirlen || dirlen >= MAX_PATH ||
        !GetTempFileNameW(dir, L"jvw", 0, tmp)) {
        error = GetLastError();
        CloseHandle(src);
        return error ? error : ERROR_PATH_NOT_FOUND;
    }
    d->file = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (d->file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        DeleteFileW(tmp);
    }
    unsigned char *buf = malloc(256 * 1024);
    if (!buf && !error)
        error = ERROR_OUTOFMEMORY;
    uint64_t left = ((uint64_t)d->stamp.nFileSizeHigh << 32) | d->stamp.nFileSizeLow;
    while (!error && left) {
        DWORD n = 0, w = 0, want = left > 256 * 1024 ? 256 * 1024 : (DWORD)left;
        if (stopped(cancel))
            error = ERROR_CANCELLED;
        else if (!ReadFile(src, buf, want, &n, NULL) || !WriteFile(d->file, buf, n, &w, NULL))
            error = GetLastError();
        else if (!n || w != n)
            error = ERROR_FILE_INVALID;
        else
            left -= n;
    }
    BY_HANDLE_FILE_INFORMATION after;
    if (!error && (!GetFileInformationByHandle(src, &after) || !same_stamp(after, d->stamp)))
        error = ERROR_FILE_INVALID;
    free(buf);
    CloseHandle(src);
    d->snapshot = !error;
    return error;
}
Document *doc_open(const wchar_t *path, volatile LONG *cancel, char *msg, size_t cap) {
    Document *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(msg, cap, "Out of memory");
        return NULL;
    }
    d->refs = 1;
    d->file = INVALID_HANDLE_VALUE;
    d->path = _wcsdup(path);
    DWORD error = d->path ? 0 : ERROR_OUTOFMEMORY;
    if (!error) {
        d->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (d->file == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION)
                error = snapshot(d, cancel);
        }
    }
    LARGE_INTEGER size;
    if (!error && !GetFileSizeEx(d->file, &size))
        error = GetLastError();
    if (!error) {
        d->size = (size_t)size.QuadPart;
        d->data = (const unsigned char *)"";
        if (d->size) {
            d->mapping = CreateFileMappingW(d->file, NULL, PAGE_READONLY, 0, 0, NULL);
            if (!d->mapping || !(d->data = MapViewOfFile(d->mapping, FILE_MAP_READ, 0, 0, 0)))
                error = GetLastError();
        }
    }
    if (error) {
        wchar_t w[180] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error,
                       MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), w, 180, NULL);
        char detail[560] = "";
        WideCharToMultiByte(CP_UTF8, 0, w, -1, detail, sizeof detail, NULL, NULL);
        snprintf(msg, cap, "Cannot read file (%lu) %s", error, detail);
        doc_release(d);
        return NULL;
    }
    const wchar_t *ext = wcsrchr(path, L'.');
    d->lines = ext && (!_wcsicmp(ext, L".jsonl") || !_wcsicmp(ext, L".ndjson"));
    d->start = (d->size >= 3 && !memcmp(d->data, "\xef\xbb\xbf", 3)) ? 3 : 0;
    return d;
}
Document *doc_clipboard(char *msg, size_t cap) {
    if (!OpenClipboard(NULL)) {
        snprintf(msg, cap, "The clipboard is busy.");
        return NULL;
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *text = h ? GlobalLock(h) : NULL;
    size_t limit = text ? GlobalSize(h) / sizeof(wchar_t) : 0;
    size_t chars = text ? wcsnlen(text, limit) : 0;
    int bytes = chars && chars < limit && chars <= INT_MAX
                    ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)chars, NULL, 0,
                                          NULL, NULL)
                    : 0;
    Document *d = bytes ? calloc(1, sizeof *d) : NULL;
    if (d) {
        d->refs = 1;
        d->file = INVALID_HANDLE_VALUE;
        d->size = (size_t)bytes;
        d->mapping =
            CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)bytes, NULL);
        char *data = d->mapping ? MapViewOfFile(d->mapping, FILE_MAP_WRITE, 0, 0, 0) : NULL;
        d->data = (unsigned char *)data;
        if (!data || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)chars, data,
                                          bytes, NULL, NULL)) {
            doc_release(d);
            d = NULL;
        } else if (bytes >= 3 && !memcmp(data, "\xef\xbb\xbf", 3))
            d->start = 3;
    }
    if (text)
        GlobalUnlock(h);
    CloseClipboard();
    if (!d)
        snprintf(msg, cap,
                 bytes ? "Out of memory reading clipboard"
                       : "The clipboard contains no readable text.");
    return d;
}
Reader doc_reader(Document *d, volatile LONG *cancel) {
    return (Reader){.bytes = d->data, .size = d->size, .error = J_NONE, .cancel = cancel};
}
