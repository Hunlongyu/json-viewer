#include "view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "failed line %d: %s\n", __LINE__, #x);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void drain(View *v) {
    ULONGLONG start = GetTickCount64();
    do {
        view_tick(v);
        CHECK(GetTickCount64() - start < 20000);
        if (v->jobs)
            Sleep(1);
    } while (v->jobs);
}
static void selected(View *v, const char *label, const char *value) {
    char name[100], text[200];
    CHECK(v->rows > 0);
    view_text(v, view_row(v, v->cursor), name, sizeof name, text, sizeof text);
    if(!strstr(name,label) || !strstr(text,value))
        fprintf(stderr,"expected %s / %s, selected %s / %s at row %d\n",label,value,name,text,v->cursor);
    CHECK(strstr(name, label) && strstr(text, value));
}
static void transfer_check(View *v, Node node, NodeAction action, const wchar_t *expected) {
    Job j = {
        .doc = v->doc, .parent = node, .root = v->root, .action = action, .record_start = J_NONE};
    Reader r = doc_reader(v->doc, &j.cancel);
    prepare_transfer(&j, &r);
    CHECK(!r.message && !j.error[0] && j.clipboard);
    wchar_t *text = GlobalLock(j.clipboard);
    CHECK(text && !wcscmp(text, expected));
    GlobalUnlock(j.clipboard);
    GlobalFree(j.clipboard);
}
static void check_roots(const wchar_t *file) {
    const char *source[] = {"{}", "[]", "123", "[1,{\"x\":2}]", "{\"x\":{},\"x\":[]}"};
    size_t counts[] = {0, 0, 0, 3, 2};
    int rows[] = {0, 1, 1, 1, 2};
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        FILE *f = _wfopen(file, L"wb");
        CHECK(f);
        fputs(source[i], f);
        CHECK(!fclose(f));
        View v = {.focus = J_NONE, .last_hit = J_NONE};
        view_open(&v, file);
        drain(&v);
        CHECK(v.count_state == COUNT_READY && v.value_count == counts[i] && v.rows == rows[i]);
        if (i == 3) {
            selected(&v, "Root", "[");
            view_toggle(&v, 1);
            drain(&v);
            CHECK(v.rows == 3);
        }
        view_close(&v);
    }
    FILE *f = _wfopen(file, L"wb");
    CHECK(f);
    fputc('{', f);
    for (int i = 0; i <= 2 * J_PAGE; i++)
        fprintf(f, "%s\"k%d\":[0,1]", i ? "," : "", i);
    fputc('}', f);
    CHECK(!fclose(f));
    View v = {.focus = J_NONE, .last_hit = J_NONE};
    view_open(&v, file);
    drain(&v);
    CHECK(v.rows == J_PAGE + 1 && v.count_state == COUNT_READY &&
          v.value_count == 3 * (2 * J_PAGE + 1));
    selected(&v, "k0", "[");
    v.cursor = J_PAGE;
    view_toggle(&v, 1);
    drain(&v);
    selected(&v, "k128", "[");
    CHECK(v.rows == 2 * J_PAGE + 1 && v.value_count == 3 * (2 * J_PAGE + 1));
    view_close(&v);
}
int wmain(int argc, wchar_t **argv) {
    CHECK(argc == 1 || argc == 2);
    wchar_t temp[MAX_PATH], file[MAX_PATH], log[MAX_PATH], large[32768];
    CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"cjs", 0, file));
    check_roots(file);
    CHECK(GetTempFileNameW(temp, L"cjl", 0, log));
    wchar_t lines[MAX_PATH + 8];
    swprintf(lines, MAX_PATH + 8, L"%ls.jsonl", log);
    CHECK(MoveFileW(log, lines));
    FILE *out = _wfopen(file, L"wb");
    CHECK(out);
    fputs("{\"items\":[", out);
    for (int i = 0; i < 1000; i++)
        fputs("0,", out);
    fputs("{\"dup\":\"first\",\"dup\":\"\\u4e2d\\u6587\"}],\"tail\":true}", out);
    CHECK(fclose(out) == 0);
    out = _wfopen(lines, L"wb");
    CHECK(out);
    fputs("{\"broken\":\"bad\n\n{\"field\":\"\\u4e2d\\u6587\"}\r\n", out);
    CHECK(!fclose(out));
    View v = {.focus = J_NONE, .last_hit = J_NONE};
    view_open(&v, file);
    drain(&v);
    CHECK(v.rows == 2 && v.count_state == COUNT_READY && v.value_count == 1005);
    v.cursor = 0;
    selected(&v, "items", "[");
    view_toggle(&v, 1);
    drain(&v);
    v.cursor = 1 + J_PAGE;
    CHECK(view_row(&v, v.cursor).kind == NEXT_PAGE);
    view_toggle(&v, 1);
    drain(&v);
    v.cursor = 1 + J_PAGE;
    selected(&v, "[128]", "0");
    view_search(&v, "0", KEYS_AND_VALUES, false);
    drain(&v);
    CHECK(v.total_hits == 1000);
    view_previous(&v);
    drain(&v);
    selected(&v, "[999]", "0");
    CHECK(v.hit_count == v.total_hits);
    view_search(&v, "中文", VALUE_TEXT, false);
    drain(&v);
    selected(&v, "dup", "中文");
    CHECK(v.hit_count == 1 && v.total_hits == 1 && v.search_selection);
    Reader r = doc_reader(v.doc, NULL);
    size_t offsets[20], shown;
    char display[20];
    unsigned char marks[20] = {0};
    Node matched = view_row(&v, v.cursor).node;
    jstring_preview(&r, matched.start, matched.bound, display, sizeof display, offsets, &shown);
    view_mark(&v, offsets, shown, marks);
    CHECK(shown == 6 && !memcmp(marks, "\x01\x01\x01\x01\x01\x01", 6));
    Node leaf = view_row(&v, v.cursor).node;
    char path_label[128];
    view_path(&v, view_row(&v, v.cursor), path_label, sizeof path_label);
    CHECK(!strcmp(path_label, "$.items[1000].dup"));
    transfer_check(&v, leaf, COPY_NAME, L"dup");
    transfer_check(&v, leaf, COPY_VALUE, L"中文");
    transfer_check(&v, leaf, COPY_JSON, L"\"\\u4e2d\\u6587\"");
    transfer_check(&v, leaf, COPY_PATH, L"$.items[1000].dup");
    Node object = view_row(&v, v.cursor).parent->node;
    transfer_check(&v, object, COPY_MINIFIED, L"{\"dup\":\"first\",\"dup\":\"\\u4e2d\\u6587\"}");
    transfer_check(&v, object, COPY_FORMATTED,
                   L"{\r\n  \"dup\": \"first\",\r\n  \"dup\": \"\\u4e2d\\u6587\"\r\n}");
    {
        char json[4100];
        wchar_t expected[4097];
        json[0] = '"';
        for (size_t i = 0; i < 4096; i++) {
            json[i + 1] = 'x';
            expected[i] = L'x';
        }
        json[4097] = '"';
        expected[4096] = 0;
        Reader reader = {.bytes = (const unsigned char *)json, .size = 4098, .error = J_NONE};
        Node string;
        CHECK(jroot(&reader, 0, 4098, &string));
        Job copy_string = {.root = string, .parent = string, .action = COPY_VALUE};
        prepare_transfer(&copy_string, &reader);
        CHECK(copy_string.clipboard && !copy_string.error[0]);
        const wchar_t *full = GlobalLock(copy_string.clipboard);
        CHECK(full && !wcscmp(full, expected));
        GlobalUnlock(copy_string.clipboard);
        GlobalFree(copy_string.clipboard);
    }
    wchar_t output[MAX_PATH];
    CHECK(GetTempFileNameW(temp, L"exp", 0, output));
    view_transfer(&v, (Row){.kind = DATA_ROW, .node = object}, EXPORT_FORMATTED, output);
    drain(&v);
    FILE *exported = _wfopen(output, L"rb");
    CHECK(exported);
    char copy[200] = {0};
    CHECK(fread(copy, 1, sizeof copy - 1, exported) > 0);
    fclose(exported);
    CHECK(!strcmp(copy, "{\r\n  \"dup\": \"first\",\r\n  \"dup\": \"\\u4e2d\\u6587\"\r\n}"));
    CHECK(DeleteFileW(output));
    view_transfer(&v, (Row){.kind = DATA_ROW, .node = object}, EXPORT_MINIFIED, file);
    drain(&v);
    CHECK(strstr(v.status, "other than"));
    view_search(&v, "dup", KEY_TEXT, true);
    view_search(&v, "dup", KEY_TEXT, true);
    drain(&v);
    CHECK(v.hit_count == 2 && v.total_hits == 2);
    selected(&v, "dup", "中文");
    view_search(&v, "dup", KEY_TEXT, false);
    drain(&v);
    selected(&v, "dup", "first");
    view_search(&v, "dup", KEY_TEXT, true);
    drain(&v);
    selected(&v, "dup", "中文");
    view_open(&v, lines);
    drain(&v);
    CHECK(v.nrecords == 2 && v.records[1].line == 3);
    view_search(&v, "中文", VALUE_TEXT, false);
    drain(&v);
    selected(&v, "field", "中文");
    if (argc == 2)
        swprintf(large, 32768, L"%ls\\sdk-events_x.jsonl", argv[1]);
    view_open(&v, argc == 2 ? large : lines);
    while (!v.doc) {
        view_tick(&v);
        CHECK(v.jobs || v.doc);
        Sleep(1);
    }
    view_search(&v, "absent-in-large-file", RAW_TEXT, false);
    view_cancel(&v);
    view_search(&v, "local_index", RAW_TEXT, false);
    view_open(&v, file);
    drain(&v);
    CHECK(v.doc && !v.doc->lines && v.rows == 2 && v.hit_count == 0 && v.value_count == 1005 &&
          !strcmp(v.status, "Ready"));
    view_close(&v);
    HANDLE writer = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
    CHECK(writer != INVALID_HANDLE_VALUE);
    char error[400];
    Document *snap = doc_open(file, NULL, error, sizeof error);
    CHECK(snap && snap->snapshot && !doc_changed(snap));
    CHECK(SetEndOfFile(writer));
    CloseHandle(writer);
    CHECK(doc_changed(snap) && snap->data[0] == '{');
    doc_release(snap);
    writer = CreateFileW(file, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(writer != INVALID_HANDLE_VALUE);
    CHECK(!doc_open(file, NULL, error, sizeof error) && strstr(error, "32"));
    CloseHandle(writer);
    CHECK(DeleteFileW(file));
    CHECK(DeleteFileW(lines));
    puts("C view passed paging, sparse bookmarks, duplicate keys, corrupt JSONL, cancellation, "
         "snapshot and sharing tests");
    return 0;
}
