#pragma once
#include <windows.h>
#include "commands.h"
typedef struct {
    wchar_t *path[RECENT_LIMIT];
} RecentFiles;
void recent_load(RecentFiles *r);
void recent_add(RecentFiles *r, const wchar_t *path);
void recent_menu(const RecentFiles *r, HMENU menu);
void recent_clear(RecentFiles *r);
