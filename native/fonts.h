#pragma once
#include <windows.h>
#include <cui/cui.h>

typedef struct FontEntry FontEntry;
typedef struct {
    cui_ctx *ui;
    cui_font font, fallback;
    int size, weight;
    bool italic;
    wchar_t family[LF_FACESIZE];
    wchar_t portable_ini[MAX_PATH], user_ini[MAX_PATH];
    FontEntry *loaded;
} TreeFonts;

void fonts_init(TreeFonts *fonts, cui_ctx *ui, cui_font fallback);
void fonts_dispose(TreeFonts *fonts);
bool fonts_choose(TreeFonts *fonts, HWND owner);
