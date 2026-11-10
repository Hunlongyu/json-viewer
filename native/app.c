#define COBJMACROS
#include "tree_ui.h"
#include "commands.h"
#include "recent.h"
#include "update.h"
#include <initguid.h>
#include <oleacc.h>
#include <cui/cui.h>
#include <cui/cui_app.h>
/* CUI 1.2 exposes the live-frame hook through cui_app_run, but that entry point
   owns the application handle. Our Win32 host needs the handle for drop/IME
   and accessibility, so use the same hook in this pinned backend. */
#include "cui_platform.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

typedef struct {
    View view;
    TreeClick tree_click;
    cui_app *app;
    HWND hwnd;
    IAccPropServices *access;
    Document *chinese;
    SearchMode mode;
    bool match_case, regex;
    POINT context;
    ULONGLONG last_check;
    char *description;
    char query[2048];
    cui_rect search_rect;
    bool search_focused, composing, ensure_visible, help, about, hide_status,
        clipboard_source;
    bool show_path;
    HACCEL accelerators;
    RecentFiles recent;
    ViewerUpdate update;
    LARGE_INTEGER open_started, clock_hz;
    double root_ms, load_ms;
} Application;

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n ? malloc((size_t)n * sizeof *w) : NULL;
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static void open_path(Application *a, const wchar_t *path) {
    QueryPerformanceCounter(&a->open_started);
    a->root_ms = 0;
    a->clipboard_source = !path;
    view_open(&a->view, path);
    if (!path)
        path = L"Clipboard";
    const char prefix[] = "JSON Viewer · ";
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    char *title = n ? malloc(sizeof prefix + (size_t)n) : NULL;
    if (title) {
        memcpy(title, prefix, sizeof prefix - 1);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, title + sizeof prefix - 1, n, NULL, NULL);
        cui_app_set_title(a->app, title);
        free(title);
    }
    a->query[0] = 0;
    a->search_focused = false;
    SetFocus(a->hwnd);
}
static void picker(Application *a) {
    char file[32768 * 4];
    cui_file_dialog_opts opts = {.kind = CUI_FILE_OPEN,
                                 .title = "Open file",
                                 .filter_name = "JSON / JSON Lines",
                                 .filter = "json;jsonl;ndjson"};
    if (cui_app_file_dialog(a->app, &opts, file, sizeof file)) {
        wchar_t *path = wide(file);
        if (path) {
            open_path(a, path);
            free(path);
        }
    }
}
static void search(Application *a, unsigned submissions) {
    view_search(&a->view, a->query,
                a->mode | (a->match_case ? 0 : IGNORE_CASE) | (a->regex ? REGEX_SEARCH : 0),
                submissions);
}
static void set_scope(Application *a, SearchMode scope) {
    const char *names[] = {"Document text", "Keys", "Values", "Keys and values"};
    a->mode = scope;
    view_cancel(&a->view);
    snprintf(a->view.status, sizeof a->view.status, "Search scope · %s", names[a->mode]);
    if (a->query[0])
        search(a, 0);
}
static void redraw(Application *a) {
    cui_request_redraw(cui_app_ui(a->app));
    PostMessageW(a->hwnd, WM_NULL, 0, 0);
}
/* The public search control exposes pointer and keyboard input. Route Ctrl+F
   through the same focus path as a click inside it. */
static void focus_search(Application *a) {
    cui_ctx *ui = cui_app_ui(a->app);
    cui_rect r = a->search_rect;
    if (r.w <= 0)
        return;
    cui_input_mouse_move(ui, r.x + r.w / 2, r.y + r.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    redraw(a);
}
static bool key(Application *a, WPARAM k) {
    View *v = &a->view;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (a->help || a->about || a->update.open)
        return false;
    if (k == VK_F1)
        a->help = true;
    else if (ctrl && k == 'O')
        picker(a);
    else if (ctrl && k == 'F')
        focus_search(a);
    else if (k == VK_F3 && (GetKeyState(VK_SHIFT) & 0x8000))
        view_previous(v);
    else if (k == VK_F3)
        search(a, 1);
    else if (k == VK_F4)
        set_scope(a, a->mode == KEYS_AND_VALUES ? KEY_TEXT
                     : a->mode == VALUE_TEXT    ? RAW_TEXT
                     : a->mode == RAW_TEXT      ? KEYS_AND_VALUES
                                                : VALUE_TEXT);
    else if (k == VK_ESCAPE)
        view_cancel(v);
    else if (a->search_focused || !v->rows)
        return false;
    else {
        a->ensure_visible = true;
        switch (k) {
        case VK_RIGHT:
            view_toggle(v, 1);
            break;
        case VK_LEFT:
            view_toggle(v, -1);
            break;
        case VK_HOME:
            v->cursor = 0;
            break;
        case VK_END:
            v->cursor = v->rows - 1;
            break;
        case VK_UP:
            if (v->cursor > 0)
                --v->cursor;
            break;
        case VK_DOWN:
            if (v->cursor < v->rows - 1)
                ++v->cursor;
            break;
        case VK_PRIOR:
            v->cursor = v->cursor > 16 ? v->cursor - 16 : 0;
            break;
        case VK_NEXT:
            v->cursor = v->rows - 1 - v->cursor > 16 ? v->cursor + 16 : v->rows - 1;
            break;
        default:
            return false;
        }
        v->search_selection = false;
    }
    redraw(a);
    return true;
}
static void transfer(Application *a, Row row, NodeAction action) {
    View *v = &a->view;
    if (action != EXPORT_MINIFIED && action != EXPORT_FORMATTED && action != EXPORT_JSON) {
        view_transfer(v, row, action, NULL);
        return;
    }
    unsigned generation = v->generation;
    bool lines = v->doc->lines && row.kind == RECORDS_ROOT;
    char file[32768 * 4];
    cui_file_dialog_opts opts = {.kind = CUI_FILE_SAVE,
                                 .title = "Export JSON",
                                 .filter_name = lines ? "JSON Lines" : "JSON",
                                 .filter = lines ? "jsonl" : "json",
                                 .default_name = lines ? "data.jsonl" : "value.json"};
    if (cui_app_file_dialog(a->app, &opts, file, sizeof file) && generation == v->generation) {
        wchar_t *path = wide(file);
        if (path) {
            view_transfer(v, row, action, path);
            free(path);
        }
    }
}
static void node_menu(Application *a) {
    View *v = &a->view;
    if (!v->doc || !v->rows)
        return;
    Row row = view_row(v, v->cursor);
    if (row.kind == NEXT_PAGE)
        return;
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    bool data = row.kind != RECORDS_ROOT;
    if (data && row.node.key != J_NONE)
        AppendMenuW(menu, MF_STRING, COPY_NAME, L"Copy Name");
    if (data)
        AppendMenuW(menu, MF_STRING, COPY_VALUE, L"Copy Value");
    if (data && container(row.node)) {
        HMENU form = CreatePopupMenu();
        AppendMenuW(form, MF_STRING, COPY_MINIFIED, L"Minified Value");
        AppendMenuW(form, MF_STRING, COPY_FORMATTED, L"Formatted Value");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)form, L"Copy Value As");
    }
    if (data)
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, COPY_PATH, L"Copy Path");
    if (data && container(row.node)) {
        HMENU form = CreatePopupMenu();
        AppendMenuW(form, MF_STRING, EXPORT_MINIFIED, L"Minified JSON…");
        AppendMenuW(form, MF_STRING, EXPORT_FORMATTED, L"Formatted JSON…");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)form, L"Export Value As");
    }
    unsigned generation = v->generation;
    int action = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, a->context.x, a->context.y,
                                  a->hwnd, NULL);
    DestroyMenu(menu);
    if (!action || generation != v->generation)
        return;
    transfer(a, row, (NodeAction)action);
    redraw(a);
}
static void command(Application *a, unsigned cmd);
static void menu_state(Application *a);
static LRESULT CALLBACK events(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id,
                               DWORD_PTR data) {
    Application *a = (Application *)data;
    if (msg == WM_INITMENU)
        menu_state(a);
    if (msg == WM_COMMAND && !l && LOWORD(w) >= CMD_NEW && LOWORD(w) < CMD_RECENT + RECENT_LIMIT) {
        if (!a->help && !a->about && !a->update.open)
            command(a, LOWORD(w));
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !a->composing && !a->help &&
        !a->about && !a->update.open &&
        !(a->search_focused && (w == 'C' || w == 'V'))) {
        MSG input = {.hwnd = hwnd, .message = msg, .wParam = w, .lParam = l};
        if (TranslateAcceleratorW(hwnd, a->accelerators, &input))
            return 0;
    }
    if (msg == WM_SYSCHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
        ((msg == WM_KEYDOWN || msg == WM_KEYUP) && w == VK_F10))
        return DefWindowProcW(hwnd, msg, w, l);
    if (msg == WM_APP + 2) {
        node_menu(a);
        return 0;
    }
    if (msg == WM_CONTEXTMENU && l == -1) {
        GetCursorPos(&a->context);
        node_menu(a);
        return 0;
    }
    if (msg == WM_APP + 1) {
        picker(a);
        return 0;
    }
    /* An exposed client area can contain pixels lost by the OS even when
       CUI's scene-damage tracker reports no changed geometry. */
    if (msg == WM_SIZE || msg == WM_PAINT || msg == WM_DPICHANGED || msg == WM_EXITSIZEMOVE)
        a->app->force_present = true;
    if (msg == WM_IME_STARTCOMPOSITION)
        a->composing = true;
    if (msg == WM_IME_ENDCOMPOSITION || msg == WM_KILLFOCUS)
        a->composing = false;
    if (msg == WM_KEYDOWN && !a->composing && key(a, w))
        return 0;
    if (msg == WM_DROPFILES) {
        HDROP drop = (HDROP)w;
        UINT len = DragQueryFileW(drop, 0, NULL, 0);
        wchar_t *p = calloc((size_t)len + 1, sizeof *p);
        if (p && DragQueryFileW(drop, 0, p, len + 1))
            open_path(a, p);
        free(p);
        DragFinish(drop);
        redraw(a);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, events, id);
    return DefSubclassProc(hwnd, msg, w, l);
}
static void count_label(const View *v, char *out, size_t cap) {
    if (v->count_state != COUNT_READY) {
        snprintf(out, cap, "Count: %s", v->count_state == COUNT_PENDING ? "…" : "—");
        return;
    }
    char digits[32];
    size_t n = (size_t)snprintf(digits, sizeof digits, "%zu", v->value_count);
    char grouped[48];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0)
            grouped[k++] = ',';
        grouped[k++] = digits[i];
    }
    grouped[k] = 0;
    snprintf(out, cap, "Count: %s", grouped);
}
static void timing_label(const Application *a, char *out, size_t cap) {
    const View *v = &a->view;
    if (v->query[0]) {
        if (v->search_error[0]) {
            snprintf(out, cap, "Find: —");
            return;
        }
        LARGE_INTEGER end = v->find_finished;
        if (v->searching)
            QueryPerformanceCounter(&end);
        double ms = (end.QuadPart - v->find_started.QuadPart) * 1000.0 / a->clock_hz.QuadPart;
        snprintf(out, cap, v->searching ? "Find: %.0f ms…" : "Find: %.2f ms", ms);
    } else if (v->count_state == COUNT_READY)
        snprintf(out, cap, "Load %.1f ms", a->load_ms);
    else
        snprintf(out, cap, "Load %s", v->count_state == COUNT_PENDING ? "…" : "—");
}
/* CUI draws the grid. Publish its current selection to assistive technology. */
static void accessible_selection(Application *a) {
    if (!a->access)
        return;
    View *v = &a->view;
    char value[512], all[2300];
    char label[384] = "";
    value[0] = 0;
    if (v->doc && v->rows) {
        Row row = view_row(v, v->cursor);
        view_text(v, row, label, sizeof label, value, sizeof value);
        if (row.kind == DATA_ROW || row.kind == RECORD_ROW) {
            snprintf(all, sizeof all, "%s    %s%s\r\n%s    byte %zu    %s", v->status,
                     v->doc->lines ? "JSONL  " : "", v->doc->snapshot ? "snapshot" : "", label,
                     row.node.start, value);
        } else
            snprintf(all, sizeof all, "%s\r\n%s    %s%s", v->status, label, value,
                     row.kind == RECORDS_ROOT && v->lines_open ? " · expanded" : "");
    } else
        snprintf(all, sizeof all, "%s", v->status);
    if (v->doc) {
        size_t used = strlen(all);
        char count[64], timing[64];
        count_label(v, count, sizeof count);
        timing_label(a, timing, sizeof timing);
        snprintf(all + used, sizeof all - used, "    %s    %s", count, timing);
    }
    if (a->query[0]) {
        char result[64];
        view_search_label(v, result, sizeof result);
        size_t used = strlen(all);
        snprintf(all + used, sizeof all - used, "    Find text [%s]    %s", a->query, result);
    }
    if (a->update.open) {
        size_t used = strlen(all);
        snprintf(all + used, sizeof all - used, "    Check For Update    %s",
                 a->update.state == UPDATE_MESSAGE ? a->update.message
                 : a->update.state == AVAILABLE    ? "An update is available."
                 : a->update.state == CHECKING     ? "Checking GitHub Releases…"
                 : a->update.state == DOWNLOADED   ? "Ready to restart and install."
                                                   : "Downloading…");
    }
    if (!a->description || strcmp(a->description, all)) {
        wchar_t *w = wide(all);
        if (w) {
            IAccPropServices_SetHwndPropStr(a->access, a->hwnd, OBJID_CLIENT, CHILDID_SELF,
                                            PROPID_ACC_VALUE, w);
            NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, a->hwnd, OBJID_CLIENT, CHILDID_SELF);
            free(w);
        }
        free(a->description);
        a->description = _strdup(all);
    }
}
typedef struct {
    unsigned id;
    NodeAction action;
    bool document;
} TransferCommand;
static const TransferCommand transfer_commands[] = {
    {CMD_DATA_COPY, COPY_JSON, true},
    {CMD_DATA_MIN, COPY_MINIFIED, true},
    {CMD_DATA_FORMAT, COPY_FORMATTED, true},
    {CMD_NAME, COPY_NAME},
    {CMD_VALUE, COPY_VALUE},
    {CMD_VALUE_MIN, COPY_MINIFIED},
    {CMD_VALUE_FORMAT, COPY_FORMATTED},
    {CMD_PATH, COPY_PATH},
    {CMD_DATA_EXPORT, EXPORT_JSON, true},
    {CMD_DATA_EXPORT_MIN, EXPORT_MINIFIED, true},
    {CMD_DATA_EXPORT_FORMAT, EXPORT_FORMATTED, true},
    {CMD_SEL_EXPORT, EXPORT_JSON},
    {CMD_SEL_EXPORT_MIN, EXPORT_MINIFIED},
    {CMD_SEL_EXPORT_FORMAT, EXPORT_FORMATTED}};
static bool can_transfer(const View *v, Row row, const TransferCommand *t) {
    if (!v->doc)
        return false;
    if (t->document)
        return !v->doc->lines || t->action == COPY_JSON || t->action == EXPORT_JSON;
    if (!v->rows || row.kind == NEXT_PAGE)
        return false;
    if (t->action == COPY_PATH)
        return true;
    if (row.kind == RECORDS_ROOT)
        return false;
    if (t->action == COPY_NAME)
        return row.node.key != J_NONE;
    if (t->action == COPY_FORMATTED || t->action == COPY_MINIFIED || t->action == EXPORT_MINIFIED ||
        t->action == EXPORT_FORMATTED)
        return row.node.kind != '!';
    return true;
}
static void enabled(HMENU m, unsigned cmd, bool enable) {
    EnableMenuItem(m, cmd, MF_BYCOMMAND | (enable ? MF_ENABLED : MF_GRAYED));
}
static void menu_state(Application *a) {
    View *v = &a->view;
    HMENU menu = GetMenu(a->hwnd);
    Row row = view_row(v, v->cursor);
    for (size_t i = 0; i < _countof(transfer_commands); ++i)
        enabled(menu, transfer_commands[i].id, can_transfer(v, row, &transfer_commands[i]));
    enabled(menu, CMD_CLIPBOARD, IsClipboardFormatAvailable(CF_UNICODETEXT));
    enabled(menu, CMD_REFRESH, v->filename != NULL);
    ModifyMenuW(menu, CMD_NODE_PATH, MF_BYCOMMAND | MF_STRING, CMD_NODE_PATH,
                a->show_path ? L"Hide Node Path" : L"Show Node Path");
    enabled(menu, CMD_FIND, v->doc != NULL);
    enabled(menu, CMD_NEXT, a->query[0] != 0 && v->doc);
    enabled(menu, CMD_PREVIOUS, v->total_hits > 1);
    for (unsigned id = CMD_EXPAND; id <= CMD_LAST; ++id)
        enabled(menu, id, v->rows > 0);
    enabled(menu, CMD_EXPAND,
            v->rows && (row.kind == NEXT_PAGE || row.kind == RECORDS_ROOT || container(row.node)));
    CheckMenuRadioItem(menu, CMD_SCOPE_RAW, CMD_SCOPE_BOTH, CMD_SCOPE_RAW + a->mode, MF_BYCOMMAND);
    ModifyMenuW(menu, CMD_STATUS, MF_BYCOMMAND | MF_STRING, CMD_STATUS,
                a->hide_status ? L"Show Status Bar" : L"Hide Status Bar");
}
static void shell_open(Application *a, const wchar_t *path) {
    if ((INT_PTR)ShellExecuteW(a->hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32)
        MessageBoxW(a->hwnd, L"Windows could not open the requested item.", L"JSON Viewer",
                    MB_OK | MB_ICONINFORMATION);
}
static void command(Application *a, unsigned cmd) {
    View *v = &a->view;
    Row row = view_row(v, v->cursor);
    if (cmd >= CMD_RECENT && cmd < CMD_RECENT + RECENT_LIMIT) {
        if (a->recent.path[cmd - CMD_RECENT])
            open_path(a, a->recent.path[cmd - CMD_RECENT]);
        return;
    }
    if (cmd >= CMD_SCOPE_RAW && cmd <= CMD_SCOPE_BOTH) {
        set_scope(a, (SearchMode)(cmd - CMD_SCOPE_RAW));
        redraw(a);
        return;
    }
    for (size_t i = 0; i < _countof(transfer_commands); ++i) {
        const TransferCommand *t = &transfer_commands[i];
        if (t->id != cmd)
            continue;
        if (can_transfer(v, row, t)) {
            if (t->document)
                row = (Row){.kind = RECORDS_ROOT, .node = v->root, .record = J_NONE};
            transfer(a, row, t->action);
        }
        return;
    }
    switch (cmd) {
    case CMD_NEW:
    case CMD_LEGAL: {
        wchar_t exe[32768];
        DWORD n = GetModuleFileNameW(NULL, exe, _countof(exe));
        if (!n || n >= _countof(exe))
            break;
        if (cmd == CMD_LEGAL) {
            wchar_t *leaf = wcsrchr(exe, L'\\');
            if (!leaf ||
                wcscpy_s(leaf + 1, _countof(exe) - (size_t)(leaf + 1 - exe), L"THIRD-PARTY.txt"))
                break;
        }
        shell_open(a, exe);
        break;
    }
    case CMD_OPEN:
        picker(a);
        break;
    case CMD_CLIPBOARD:
        open_path(a, NULL);
        break;
    case CMD_CLOSE:
        PostMessageW(a->hwnd, WM_CLOSE, 0, 0);
        break;
    case CMD_REFRESH:
        if (a->clipboard_source)
            open_path(a, NULL);
        else if (v->filename) {
            wchar_t *path = _wcsdup(v->filename);
            if (path) {
                open_path(a, path);
                free(path);
            }
        }
        break;
    case CMD_FIND:
        focus_search(a);
        break;
    case CMD_NEXT:
        search(a, 1);
        break;
    case CMD_PREVIOUS:
        view_previous(v);
        break;
    case CMD_NODE_PATH:
        a->show_path = !a->show_path;
        break;
    case CMD_STATUS:
        a->hide_status = !a->hide_status;
        break;
    case CMD_EXPAND:
        view_toggle(v, 1);
        break;
    case CMD_COLLAPSE:
        view_toggle(v, -1);
        break;
    case CMD_COLLAPSE_ALL:
        view_collapse_all(v);
        a->ensure_visible = true;
        break;
    case CMD_FIRST:
    case CMD_LAST:
        if (v->rows) {
            v->cursor = cmd == CMD_FIRST ? 0 : v->rows - 1;
            v->search_selection = false;
            a->ensure_visible = true;
        }
        break;
    case CMD_SHORTCUTS:
        a->help = true;
        break;
    case CMD_ABOUT:
        a->about = true;
        break;
    case CMD_UPDATE:
        update_check(&a->update, a->hwnd);
        break;
    case CMD_NOTES:
        shell_open(a, RELEASES_URL);
        break;
    }
    redraw(a);
}
static void status_bar(Application *a, cui_ctx *ui, float width) {
    View *v = &a->view;
    cui_status_bar(ui, "file-status") {
        if (!v->doc) {
            cui_status_bar_text(ui, v->filename ? "Open file" : "Ready");
        } else {
            char text[96], bytes[96];
            cui_status_bar_text(ui, v->doc->lines ? "JSONL" : "JSON");
            snprintf(bytes, sizeof bytes, "%zu byte", v->doc->size);
            snprintf(text, sizeof text, "%.2f MiB", (double)v->doc->size / 1048576.0);
            cui_status_bar_text(ui, text, .tooltip = bytes);
            count_label(v, text, sizeof text);
            cui_status_bar_text(ui, text,
                                .tooltip = v->count_state == COUNT_INVALID
                                               ? "Count unavailable for an invalid document"
                                               : "All JSON values below the root");
            if (v->doc->lines && width >= 470) {
                snprintf(text, sizeof text, "%zu records%s", v->nrecords,
                         v->indexed ? "" : " · indexing");
                cui_status_bar_text(ui, text);
            }
            cui_status_bar_spacer(ui);
            if (v->query[0] || width >= 470) {
                char detail[256];
                timing_label(a, text, sizeof text);
                if (v->query[0])
                    snprintf(detail, sizeof detail, "%s",
                             v->search_error[0]
                                 ? v->search_error
                                 : "Time from submitting the query to finishing the full search.");
                else
                    snprintf(detail, sizeof detail,
                             "From file open to full validation and count. Root ready in %.1f ms.",
                             a->root_ms);
                cui_status_bar_text(ui, text, .tooltip = detail);
            }
        }
    }
}
static void frame(Application *a) {
    cui_ctx *ui = cui_app_ui(a->app);
    View *v = &a->view;
    int before = v->cursor;
    size_t prior_hit = v->located_hit;
    bool was_search_selection = v->search_selection;
    bool opening = !v->doc, counting = v->count_state == COUNT_PENDING;
    view_tick(v);
    if (opening && v->doc) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        a->root_ms = (now.QuadPart - a->open_started.QuadPart) * 1000.0 / a->clock_hz.QuadPart;
        if (v->doc->path)
            recent_add(&a->recent, v->filename);
        recent_menu(&a->recent, GetSubMenu(GetSubMenu(GetMenu(a->hwnd), 0), 3));
    }
    if (counting && v->count_state == COUNT_READY) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        a->load_ms = (now.QuadPart - a->open_started.QuadPart) * 1000.0 / a->clock_hz.QuadPart;
    }
    if (v->cursor != before ||
        (v->search_selection && (!was_search_selection || prior_hit != v->located_hit)))
        a->ensure_visible = true;
    if (v->doc && v->doc->snapshot && GetTickCount64() - a->last_check > 800) {
        a->last_check = GetTickCount64();
        if (doc_changed(v->doc)) {
            QueryPerformanceCounter(&a->open_started);
            view_open(v, v->filename);
            snprintf(v->status, sizeof v->status, "Source changed; reopening");
        }
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_toolbar_begin(ui, "search-tools", NULL);
        bool old_case = a->match_case, old_regex = a->regex;
        cui_toolbar_button_ex(
            ui, NULL,
            &(cui_tool_opts){.label = "Aa", .tooltip = "Match case", .toggle = &a->match_case});
        cui_toolbar_button_ex(
            ui, NULL,
            &(cui_tool_opts){.label = ".*", .tooltip = "Regular expression", .toggle = &a->regex});
        if (old_case != a->match_case || old_regex != a->regex) {
            view_cancel(v);
            if (a->query[0])
                search(a, 0);
        }
        float available = cui_app_size(a->app).x;
        int change = cui_search(ui, "find", a->query, sizeof a->query, .placeholder = "Find",
                                .size = CUI_SIZE_SM, .height = 23,
                                .width = CUI_PX(min(280.0f, max(88.0f, available - 218.0f))));
        a->search_rect = cui_last_rect(ui);
        if (change & CUI_FOCUSED)
            a->search_focused = true;
        if (change & CUI_BLURRED)
            a->search_focused = false;
        if ((change & CUI_CHANGED) && v->query[0]) {
            view_cancel(v);
        }
        /* CUI reports submission once per frame, but counts individual key presses. */
        if (change & CUI_SUBMITTED)
            search(a, ui->kpressed[CUI_KEY_ENTER]);
        if (cui_toolbar_button_ex(ui, CUI_ICON_CHEVRON_LEFT,
                                  &(cui_tool_opts){.tooltip = "Previous",
                                                   .shortcut = "Shift+F3",
                                                   .disabled = v->total_hits < 2}))
            view_previous(v);
        if (cui_toolbar_button_ex(
                ui, CUI_ICON_CHEVRON_RIGHT,
                &(cui_tool_opts){.tooltip = "Next", .shortcut = "F3", .disabled = !a->query[0]}))
            search(a, 1);
        char result[64];
        view_search_label(v, result, sizeof result);
        if (result[0]) {
            cui_text(ui, result, .size = 12, .nowrap = true, .tabular = true);
            if (v->search_error[0])
                cui_tooltip(ui, v->search_error);
        }
        cui_toolbar_end(ui);
        if (v->filename && v->status[0] && strncmp(v->status, "Ready", strlen("Ready")) &&
            strncmp(v->status, "Search scope", strlen("Search scope")) &&
            strncmp(v->status, "Match ", 6) && strcmp(v->status, "Searching") &&
            strcmp(v->status, "No matches") && strcmp(v->status, v->search_error) &&
            strncmp(v->status, "Find:", 5) && strcmp(v->status, "Search cancelled"))
            cui_text(ui, v->status, .width = CUI_FILL, .nowrap = true,
                     .color = CUI_COLOR_TEXT_SECONDARY);
        if (v->doc)
            preview_tree(ui, v, &a->tree_click, cui_app_size(a->app).x, &a->ensure_visible,
                         &a->search_focused, &a->context);
        else {
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .justify = CUI_JUSTIFY_CENTER) {
                if (!v->filename &&
                    cui_empty_state_ex(
                        ui, "Open a JSON file",
                        &(cui_empty_opts){.icon = CUI_ICON_FOLDER_OPEN,
                                          .description = "Drop a JSON or JSONL file here",
                                          .action = "Choose file…"}))
                    PostMessageW(a->hwnd, WM_APP + 1, 0, 0);
            }
        }
        float width = cui_app_size(a->app).x;
        if (a->show_path && v->doc && v->rows) {
            char path[2048];
            view_path(v, view_row(v, v->cursor), path, sizeof path);
            cui_text(ui, path, .width = CUI_FILL, .nowrap = true, .size = 12);
        }
        if (!a->hide_status)
            status_bar(a, ui, width);
        if (cui_dialog_begin(ui, "About JSON Viewer", &a->about,
                             &(cui_dialog_opts){.width = 300})) {
            cui_text(ui, "JSON Viewer " VIEWER_VERSION);
            cui_text(ui, "Read-only JSON and JSON Lines viewer for Windows.", .width = CUI_FILL);
            cui_dialog_end(ui);
        }
        bool transferring = false;
        if (a->update.open)
            for (Job *j = v->jobs; j; j = j->next)
                if (j->type == TRANSFER_NODE)
                    transferring = true;
        update_draw(&a->update, ui, transferring);
        if (cui_dialog_begin(ui, "Keyboard Shortcuts", &a->help,
                             &(cui_dialog_opts){.width = width < 420 ? width - 28 : 390})) {
            cui_text(ui, "Ctrl+O    Open file");
            cui_text(ui, "Ctrl+F    Find      F4    Change search scope");
            cui_text(ui, "F3 / Shift+F3    Next / previous match");
            cui_text(ui, "← / →    Collapse / expand      Home / End    First / last row");
            cui_dialog_end(ui);
        }
    }
    accessible_selection(a);
    if (v->jobs)
        cui_request_wakeup(ui, 0.025f);
    else if (v->doc && v->doc->snapshot)
        cui_request_wakeup(ui, 0.8f);
}
static void live_frame(cui_ctx *ui, void *user) {
    (void)ui;
    frame((Application *)user);
}
static void system_font(Application *a) {
    wchar_t dir[MAX_PATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;
    const wchar_t *names[] = {L"msyh.ttc", L"msjh.ttc", L"simsun.ttc", L"msgothic.ttc",
                              L"malgun.ttf"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        wchar_t path[MAX_PATH + 40];
        swprintf(path, sizeof path / sizeof *path, L"%ls\\Fonts\\%ls", dir, names[i]);
        char error[100];
        Document *font = doc_open(path, NULL, error, sizeof error);
        if (!font)
            continue;
        cui_ctx *ui = cui_app_ui(a->app);
        cui_font f = cui_font_load_memory(ui, font->data, font->size, false);
        if (f >= 0) {
            for (int role = CUI_FONT_BODY; role <= CUI_FONT_MONO; role++)
                cui_font_add_fallback(ui, role, f);
            a->chinese = font;
            return;
        }
        doc_release(font);
    }
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev;
    (void)cmd;
    (void)show;
    Application a = {.mode = KEYS_AND_VALUES};
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int file_arg = 1;
    while (argv && file_arg < argc && !wcsncmp(argv[file_arg], L"--au-wait-pid=", 14))
        ++file_arg;
    if (file_arg > 1)
        update_after_restart(argc, argv);
    QueryPerformanceFrequency(&a.clock_hz);
    a.view.focus = a.view.last_hit = J_NONE;
    snprintf(a.view.status, sizeof a.view.status, "Open or drop a JSON / JSONL file");
    a.app = cui_app_create(&(cui_app_desc){.title = "JSON Viewer",
                                           .width = 600,
                                           .height = 800,
                                           .min_width = 320,
                                           .min_height = 240,
                                           .renderer = CUI_RENDERER_SOFTWARE});
    if (!a.app)
        return 1;
    cui_theme *theme = cui_theme_get(cui_app_ui(a.app));
    theme->radius_control = 3;
    theme->control_sm = 23;
    theme->control_md = 25;
    theme->text_body = 12;
    theme->text_sm = 11;
    a.hwnd = cui_app_native_window(a.app);
    RECT before_menu, after_menu;
    GetClientRect(a.hwnd, &before_menu);
    HMENU bar = LoadMenuW(instance, MAKEINTRESOURCEW(MAIN_MENU));
    if (!bar || !SetMenu(a.hwnd, bar)) {
        if (bar)
            DestroyMenu(bar);
        cui_app_destroy(a.app);
        if (argv)
            LocalFree(argv);
        return 1;
    }
    HICON small_icon = NULL, large_icon = NULL;
    if (SUCCEEDED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_JSON_VIEWER), LIM_SMALL, &small_icon)))
        SendMessageW(a.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    if (SUCCEEDED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_JSON_VIEWER), LIM_LARGE, &large_icon)))
        SendMessageW(a.hwnd, WM_SETICON, ICON_BIG, (LPARAM)large_icon);
    if (GetClientRect(a.hwnd, &after_menu) && before_menu.bottom > after_menu.bottom) {
        RECT frame_rect;
        GetWindowRect(a.hwnd, &frame_rect);
        SetWindowPos(a.hwnd, NULL, 0, 0, frame_rect.right - frame_rect.left,
                     frame_rect.bottom - frame_rect.top + before_menu.bottom - after_menu.bottom,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    a.accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(MAIN_KEYS));
    recent_load(&a.recent);
    recent_menu(&a.recent, GetSubMenu(GetSubMenu(bar, 0), 3));
    MONITORINFO monitor = {.cbSize = sizeof monitor};
    RECT outer;
    if (GetWindowRect(a.hwnd, &outer) &&
        GetMonitorInfoW(MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        RECT work = monitor.rcWork;
        int w = min(outer.right - outer.left, work.right - work.left);
        int h = min(outer.bottom - outer.top, work.bottom - work.top);
        int x = max(work.left, min(outer.left, work.right - w));
        int y = max(work.top, min(outer.top, work.bottom - h));
        if (x != outer.left || y != outer.top || w != outer.right - outer.left ||
            h != outer.bottom - outer.top)
            SetWindowPos(a.hwnd, NULL, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    a.view.window = a.hwnd;
    SetWindowSubclass(a.hwnd, events, 1, (DWORD_PTR)&a);
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER, &IID_IAccPropServices,
                     (void **)&a.access);
    DragAcceptFiles(a.hwnd, TRUE);
    system_font(&a);
    if (argv && file_arg < argc)
        open_path(&a, argv[file_arg]);
    if (argv)
        LocalFree(argv);
    a.app->frame_cb = live_frame;
    a.app->frame_user = &a;
    while (cui_app_frame(a.app))
        frame(&a);
    a.app->frame_cb = NULL;
    update_dispose(&a.update);
    recent_clear(&a.recent);
    view_close(&a.view);
    cui_app_destroy(a.app);
    if (small_icon)
        DestroyIcon(small_icon);
    if (large_icon)
        DestroyIcon(large_icon);
    doc_release(a.chinese);
    if (a.access)
        IAccPropServices_Release(a.access);
    if (SUCCEEDED(com))
        CoUninitialize();
    free(a.description);
    return 0;
}
