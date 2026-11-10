#pragma once
#include "view.h"
#include <cui/cui.h>
typedef struct {
    ULONGLONG when;
    unsigned generation;
    RowKind kind;
    size_t offset;
    POINT point;
    bool glyph;
} TreeClick;
void preview_tree(cui_ctx *ui, View *view, TreeClick *click, float width, bool *scroll,
                  bool *search_focus, POINT *context);
