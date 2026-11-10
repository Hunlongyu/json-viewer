#include "tree_ui.h"
#include "cui_platform.h"
#include <stdio.h>
#include <string.h>

enum { ROW_HEIGHT = 21, INDENT = 16, FONT_SIZE = 12, LEVELS = 63 };
typedef struct {
    char label[300], value[400];
    unsigned char label_marks[300], value_marks[400];
    bool search_selection;
    int depth, index;
    const int *cursor;
    uint64_t guides, accent, endpoints;
    bool striped, expandable, open, last;
    bool flat_root;
} TreeLine;
/* A match beyond the displayed prefix gets an excerpt starting at its decoded
   scalar. Search offsets already point to scalar boundaries in keys/values mode. */
static void mark_string(cui_ctx *ui, float width, View *v, Reader *reader, size_t start, size_t end,
                        size_t focus, char *text, size_t cap, unsigned char *marks) {
    size_t source[400], mapped;
    jstring_preview(reader, start, end, text, cap, source, &mapped);
    bool clipped = false;
    if (focus != J_NONE && focus > start && focus + 1 < end && (v->mode & SCOPE_MASK) != RAW_TEXT) {
        size_t prefix = 0;
        while (prefix < mapped && source[prefix] < focus)
            ++prefix;
        clipped = prefix == mapped
                      ? reader->bytes[source[mapped]] != '"'
                      : cui_measure_text(ui, CUI_FONT_BODY, FONT_SIZE, text, (int)prefix).width >
                            width - 30;
    }
    if (clipped) {
        memcpy(text, "…", 3);
        jstring_preview(reader, focus - 1, end, text + 3, cap - 3, source, &mapped);
        marks += 3;
    }
    view_mark(v, source, mapped, marks);
}
static float joint(int level) {
    return 6.5f + INDENT * level;
}
static float paint_text(cui_ctx *ui, cui_vec2 pos, cui_color color, char *text,
                        const unsigned char *marks, bool dark) {
    size_t length = strlen(text);
    cui_color highlight = dark ? CUI_RGB(0xe6cf83) : CUI_RGB(0xffed9e);
    for (size_t i = 0; i < length;) {
        if (!marks[i]) {
            ++i;
            continue;
        }
        size_t start = i++;
        while (i < length && marks[i])
            ++i;
        float left = cui_measure_text(ui, CUI_FONT_BODY, FONT_SIZE, text, (int)start).width;
        float right = cui_measure_text(ui, CUI_FONT_BODY, FONT_SIZE, text, (int)i).width;
        cui_draw_rect(ui, (cui_rect){pos.x + left, pos.y, right - left, 15}, CUI_CORNERS(0),
                      highlight);
    }
    cui_draw_text(ui, CUI_FONT_BODY, FONT_SIZE, pos, color, text, -1);
    if (dark)
        for (size_t i = 0; i < length;) {
            if (!marks[i]) {
                ++i;
                continue;
            }
            size_t start = i++;
            while (i < length && marks[i])
                ++i;
            float left = cui_measure_text(ui, CUI_FONT_BODY, FONT_SIZE, text, (int)start).width;
            cui_draw_text(ui, CUI_FONT_BODY, FONT_SIZE, (cui_vec2){pos.x + left, pos.y},
                          CUI_RGB(0x302b1b), text + start, (int)(i - start));
        }
    return cui_measure_text(ui, CUI_FONT_BODY, FONT_SIZE, text, -1).width;
}
static void paint_line(cui_ctx *ui, cui_rect r, void *data) {
    TreeLine *line = data;
    bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
    bool selected = line->index == *line->cursor;
    cui_color bg = selected && !line->search_selection
                       ? (dark ? CUI_RGB(0x254966) : CUI_RGB(0xcce9ff))
                   : line->striped ? (dark ? CUI_RGB(0x26292d) : CUI_RGB(0xf7f8fa))
                                   : (dark ? CUI_RGB(0x202226) : CUI_RGB(0xffffff));
    cui_color guide = dark ? CUI_RGB(0x586370) : CUI_RGB(0xc6ccd4);
    cui_color active = dark ? CUI_RGB(0x76baff) : CUI_RGB(0x2279c0);
    cui_color key = dark ? CUI_RGB(0xe0e6ed) : CUI_RGB(0x253447);
    cui_color value = dark ? CUI_RGB(0x8ec1ff) : CUI_RGB(0x175ca4);
    cui_push_clip(ui, r);
    cui_draw_rect(ui, r, CUI_CORNERS(0), bg);
    float scale = ui->scale, pixel = 1.f / scale;
    float mid = floorf((r.y + ROW_HEIGHT / 2.f) * scale) * pixel;
    float x = floorf((r.x + joint(line->depth)) * scale) * pixel;
    float top = roundf(r.y * scale) * pixel, bottom_edge = roundf((r.y + r.h) * scale) * pixel;
    for (int d = line->flat_root ? 0 : 1; d <= line->depth; d++) {
        uint64_t bit = UINT64_C(1) << d;
        if (d != line->depth && !(line->guides & bit))
            continue;
        float gx = floorf((r.x + joint(d)) * scale) * pixel;
        bool bright = (line->accent & bit) != 0;
        float bottom = d == line->depth && line->last ? mid + pixel : bottom_edge;
        cui_draw_rect(ui, (cui_rect){gx, top, pixel, bottom - top}, CUI_CORNERS(0), guide);
        if (bright)
            cui_draw_rect(
                ui,
                (cui_rect){gx, top, pixel, ((line->endpoints & bit) ? mid + pixel : bottom) - top},
                CUI_CORNERS(0), active);
    }
    if (line->depth || line->flat_root)
        cui_draw_rect(ui, (cui_rect){x, mid, 8, pixel}, CUI_CORNERS(0), selected ? active : guide);
    if (line->expandable) {
        cui_color ink = selected ? active : key;
        float half = roundf(4 * scale) * pixel, arm = roundf(2 * scale) * pixel;
        float side = 2 * half + pixel;
        cui_draw_rect(ui, (cui_rect){x - half, mid - half, side, side}, CUI_CORNERS(0),
                      selected ? active : guide);
        cui_draw_rect(
            ui,
            (cui_rect){x - half + pixel, mid - half + pixel, side - 2 * pixel, side - 2 * pixel},
            CUI_CORNERS(0), bg);
        cui_draw_rect(ui, (cui_rect){x - arm, mid, 2 * arm + pixel, pixel}, CUI_CORNERS(0), ink);
        if (!line->open)
            cui_draw_rect(ui, (cui_rect){x, mid - arm, pixel, 2 * arm + pixel}, CUI_CORNERS(0),
                          ink);
    }
    cui_vec2 pos = {x + 11, r.y + 3};
    pos.x += paint_text(ui, pos, key, line->label, line->label_marks, dark);
    if (pos.x < r.x + r.w)
        paint_text(ui, pos, value, line->value, line->value_marks, dark);
    cui_pop_clip(ui);
}
void preview_tree(cui_ctx *ui, View *v, TreeClick *click, float width, bool *ensure_visible,
                  bool *search_focus, POINT *context) {
    int first = 0, last = 0, clicked = -1;
    bool toggle = false;
    Row selected = view_row(v, v->cursor);
    size_t lower[LEVELS + 1] = {0}, upper[LEVELS + 1] = {0};
    Node node = selected.node;
    Branch *parent = selected.parent;
    bool flat_root = !v->doc->lines && v->root.kind == '{';
    for (int d = selected.depth; d >= (flat_root ? 0 : 1); d--) {
        if (d <= LEVELS) {
            lower[d] = parent ? parent->node.start : 0;
            upper[d] = node.start;
        }
        if (!parent)
            break;
        node = parent->node;
        parent = parent->parent;
    }
    int cap = min(LEVELS, max(1, (int)(width - 90) / INDENT));
    cui_virtual_list_begin(ui, "document", v->rows, ROW_HEIGHT, CUI_FILL, &first, &last);
    for (int i = first; i < last; i++) {
        Row row = view_row(v, i);
        char label[256], value[320];
        TreeLine line = {0};
        view_text(v, row, label, sizeof label, value, sizeof value);
        snprintf(line.label, sizeof line.label, "%s%s", label, row.kind == NEXT_PAGE ? "" : " : ");
        line.open = view_expanded(v, row);
        if (container(row.node)) {
            if (!line.open)
                strcpy(line.value, row.node.kind == '{' ? "{…}" : "[…]");
        } else if (row.node.kind == '"')
            snprintf(line.value, sizeof line.value, "\"%s\"", value);
        else
            snprintf(line.value, sizeof line.value, "%s", value);
        line.search_selection = v->search_selection;
        if (v->nblocks && (row.kind == DATA_ROW || row.kind == RECORD_ROW)) {
            Reader reader = doc_reader(v->doc, NULL);
            size_t focus = line.search_selection && row.node.start == v->located_node
                               ? v->located_hit
                               : J_NONE;
            float room = width - joint(min(row.depth, cap)) - 24;
            if (row.node.key != J_NONE && row.record == J_NONE) {
                mark_string(ui, room, v, &reader, row.node.key, row.node.key_end,
                            focus < row.node.key_end ? focus : J_NONE, label, sizeof label,
                            line.label_marks);
                snprintf(line.label, sizeof line.label, "%s : ", label);
            }
            if (row.node.kind == '"') {
                if (focus != J_NONE) {
                    float label_width =
                        cui_measure_text(ui, CUI_FONT_BODY, FONT_SIZE, line.label, -1).width;
                    if (focus > row.node.start && label_width > room - 45) {
                        strcpy(line.label, "… : ");
                        memset(line.label_marks, 0, sizeof line.label_marks);
                        label_width =
                            cui_measure_text(ui, CUI_FONT_BODY, FONT_SIZE, line.label, -1).width;
                    }
                    room -= label_width;
                }
                mark_string(ui, room, v, &reader, row.node.start, row.node.bound,
                            focus > row.node.start ? focus : J_NONE, value, sizeof value,
                            line.value_marks + 1);
                snprintf(line.value, sizeof line.value, "\"%s\"", value);
            } else if (!container(row.node) && row.node.kind != '!') {
                size_t source[400], mapped = strlen(line.value);
                for (size_t k = 0; k <= mapped; ++k)
                    source[k] = row.node.start + k;
                view_mark(v, source, mapped, line.value_marks);
            }
        }
        line.depth = min(row.depth, cap);
        line.flat_root = flat_root;
        line.guides = row.guides;
        line.last = row.last;
        line.expandable = row.kind == RECORDS_ROOT || container(row.node);
        line.index = i;
        line.cursor = &v->cursor;
        line.striped = (i & 1) != 0;
        for (int d = flat_root ? 0 : 1; d <= line.depth; d++)
            if (row.node.start > lower[d] && row.node.start <= upper[d]) {
                line.accent |= UINT64_C(1) << d;
                if (row.node.start == upper[d])
                    line.endpoints |= UINT64_C(1) << d;
            }
        cui_push_id_int(ui, i);
        cui_canvas_result hit =
            cui_canvas(ui, "node", CUI_FILL, CUI_PX(ROW_HEIGHT), paint_line, &line, sizeof line);
        if (hit.clicked) {
            clicked = i;
            ULONGLONG now = GetTickCount64();
            POINT point = {(LONG)roundf((hit.rect.x + hit.mouse.x) * ui->scale),
                           (LONG)roundf((hit.rect.y + hit.mouse.y) * ui->scale)};
            bool twice = click->when && now - click->when <= GetDoubleClickTime() &&
                         click->generation == v->generation && click->kind == row.kind &&
                         click->offset == row.node.start &&
                         labs(point.x - click->point.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                         labs(point.y - click->point.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
            bool glyph = fabsf(hit.mouse.x - joint(line.depth)) <= 8;
            toggle = row.kind == NEXT_PAGE || (line.expandable && (twice ? !click->glyph : glyph));
            *click =
                (TreeClick){twice ? 0 : now, v->generation, row.kind, row.node.start, point, glyph};
        }
        if (hit.hovered && ui->mpressed[CUI_MOUSE_RIGHT]) {
            click->when = 0;
            v->search_selection = false;
            v->cursor = i;
            *search_focus = false;
            cui_request_redraw(ui);
        }
        if (hit.hovered && ui->mreleased[CUI_MOUSE_RIGHT] && row.kind != NEXT_PAGE) {
            v->search_selection = false;
            v->cursor = i;
            *search_focus = false;
            float scale = GetDpiForWindow(v->window) / 96.f;
            cui_vec2 mouse = cui_mouse_pos(ui);
            *context = (POINT){(LONG)(mouse.x * scale), (LONG)(mouse.y * scale)};
            ClientToScreen(v->window, context);
            PostMessageW(v->window, WM_APP + 2, 0, 0);
        }
        cui_pop_id(ui);
    }
    cui_virtual_list_end(ui);
    if (clicked >= 0) {
        v->search_selection = false;
        v->cursor = clicked;
        if (toggle)
            view_toggle(v, 0);
        *search_focus = false;
        cui_request_redraw(ui);
    }
    if (*ensure_visible && v->rows) {
        if (v->cursor < first + 2 || v->cursor >= last - 2) {
            int top = v->cursor - max(0, (last - first) / 2);
            cui_scroll_to(ui, "document", (float)max(0, top) * ROW_HEIGHT);
            cui_request_redraw(ui);
        }
        *ensure_visible = false;
    }
}
