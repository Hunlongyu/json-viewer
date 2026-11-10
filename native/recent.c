#include "recent.h"
#include <stdlib.h>
#include <wchar.h>

static const wchar_t key[] = L"Software\\Hunlongyu\\JSON Viewer\\Recent";
void recent_clear(RecentFiles *r) {
    for (unsigned i = 0; i < RECENT_LIMIT; ++i) {
        free(r->path[i]);
        r->path[i] = NULL;
    }
}
void recent_load(RecentFiles *r) {
    recent_clear(r);
    for (unsigned i = 0; i < RECENT_LIMIT; ++i) {
        wchar_t name[] = {(wchar_t)(L'0' + i), 0};
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, NULL, NULL, &bytes) ||
            bytes < sizeof(wchar_t) || bytes > 65536)
            break;
        wchar_t *p = malloc(bytes);
        if (!p || RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, NULL, p, &bytes)) {
            free(p);
            break;
        }
        r->path[i] = p;
    }
}
void recent_add(RecentFiles *r, const wchar_t *path) {
    wchar_t *absolute = _wfullpath(NULL, path, 0);
    if (!absolute)
        return;
    recent_load(r);
    unsigned i = 0;
    while (i + 1 < RECENT_LIMIT && r->path[i] && _wcsicmp(r->path[i], absolute))
        ++i;
    free(r->path[i]);
    while (i) {
        r->path[i] = r->path[i - 1];
        --i;
    }
    r->path[0] = absolute;
    for (i = 0; i < RECENT_LIMIT && r->path[i]; ++i) {
        wchar_t name[] = {(wchar_t)(L'0' + i), 0};
        RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, r->path[i],
                        (DWORD)((wcslen(r->path[i]) + 1) * sizeof(wchar_t)));
    }
}
void recent_menu(const RecentFiles *r, HMENU menu) {
    while (GetMenuItemCount(menu) > 0)
        DeleteMenu(menu, 0, MF_BYPOSITION);
    if (!r->path[0])
        AppendMenuW(menu, MF_STRING | MF_GRAYED, CMD_RECENT, L"(Empty)");
    for (unsigned i = 0; i < RECENT_LIMIT && r->path[i]; ++i) {
        wchar_t *title = malloc((2 * wcslen(r->path[i]) + 1) * sizeof *title);
        if (!title)
            break;
        wchar_t *out = title;
        for (const wchar_t *p = r->path[i]; *p; ++p) {
            if (*p == L'&')
                *out++ = L'&';
            *out++ = *p;
        }
        *out = 0;
        AppendMenuW(menu, MF_STRING, CMD_RECENT + i, title);
        free(title);
    }
}
