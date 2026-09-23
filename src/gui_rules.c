/*
 * gui_rules.c - 「編輯評分規則」視窗 (自訂區間扣分)。
 *
 *   ┌────────────┬────────────┬──────┐
 *   │ CHAR 最小  │ CHAR 最大  │ 扣分 │
 *   ├────────────┼────────────┼──────┤
 *   │ 0          │ 0          │ 0    │
 *   │ 21         │ MAX        │ 20   │
 *   └────────────┴────────────┴──────┘
 *   最小 [  ] 最大 [  ] 扣分 [  ]
 *   [新增] [修改] [刪除] [恢復預設]
 *   ☐ 要求區間連續
 *   (即時檢查結果)
 *   [儲存規則] [取消]
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "gui.h"
#include "util.h"

enum {
    ID_R_LIST = 200,
    ID_R_MIN,
    ID_R_MAX,
    ID_R_PENALTY,
    ID_R_ADD,
    ID_R_MODIFY,
    ID_R_DELETE,
    ID_R_DEFAULT,
    ID_R_CONTIGUOUS,
    ID_R_MESSAGE
};

static struct {
    HWND wnd, list, e_min, e_max, e_penalty, contiguous, message;
    ScoreRules rules;
    RulesCheck check;
    volatile int done;
    int saved;
} R;

static int compare_by_min(const void *a, const void *b)
{
    const RangeRule *x = a, *y = b;
    return (x->min > y->min) - (x->min < y->min);
}

static void format_rule_max(wchar_t *out, size_t size, long max)
{
    if (max == RANGE_MAX)
        swprintf(out, size, L"MAX");
    else
        swprintf(out, size, L"%ld", max);
}

static void format_rule_penalty(wchar_t *out, size_t size, double penalty)
{
    char text[32];
    wchar_t *w;
    format_penalty(text, sizeof(text), penalty);
    w = utf8_to_wide(text);
    swprintf(out, size, L"%ls", w != NULL ? w : L"");
    free(w);
}

/* 重新排序、填表格、重新檢查規則 */
static void refresh(int select)
{
    char msg[4096];
    wchar_t buf[64];
    int i;

    qsort(R.rules.ranges, R.rules.range_count, sizeof(RangeRule), compare_by_min);

    ListView_DeleteAllItems(R.list);
    for (i = 0; i < R.rules.range_count; i++) {
        const RangeRule *r = &R.rules.ranges[i];
        LVITEMW item;

        swprintf(buf, 64, L"%ld", r->min);
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = buf;
        ListView_InsertItem(R.list, &item);

        format_rule_max(buf, 64, r->max);
        ListView_SetItemText(R.list, i, 1, buf);
        format_rule_penalty(buf, 64, r->penalty);
        ListView_SetItemText(R.list, i, 2, buf);
    }
    if (select >= 0 && select < R.rules.range_count) {
        ListView_SetItemState(R.list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(R.list, select, FALSE);
    }

    R.rules.require_contiguous = SendMessageW(R.contiguous, BM_GETCHECK, 0, 0) == BST_CHECKED;
    R.check = score_validate(&R.rules, msg, sizeof(msg));
    if (R.check == RULES_OK)
        set_text_utf8(R.message, "✔ 規則正確，沒有重疊或空缺。");
    else
        set_text_utf8(R.message, msg);
    InvalidateRect(R.message, NULL, TRUE);
}

static int find_rule_index(long min, long max, double penalty)
{
    int i;
    for (i = 0; i < R.rules.range_count; i++)
        if (R.rules.ranges[i].min == min && R.rules.ranges[i].max == max && R.rules.ranges[i].penalty == penalty)
            return i;
    return -1;
}

static int trimmed_text(HWND edit, wchar_t *buf, int size)
{
    wchar_t *start = buf;
    size_t n;

    GetWindowTextW(edit, buf, size);
    while (*start == L' ')
        start++;
    memmove(buf, start, (wcslen(start) + 1) * sizeof(wchar_t));
    n = wcslen(buf);
    while (n > 0 && buf[n - 1] == L' ')
        buf[--n] = L'\0';
    return (int)n;
}

static int parse_long_text(const wchar_t *text, long *out)
{
    wchar_t *end;
    if (text[0] == L'\0')
        return 0;
    *out = wcstol(text, &end, 10);
    return *end == L'\0';
}

/* 從三個輸入框讀出一條規則；格式錯誤時顯示訊息並回傳 0 */
static int read_rule_from_inputs(RangeRule *rule)
{
    wchar_t buf[64];

    trimmed_text(R.e_min, buf, 64);
    if (!parse_long_text(buf, &rule->min)) {
        MessageBoxW(R.wnd, L"「最小 CHAR」請輸入整數，例如 0、3、21。", L"評分規則", MB_ICONWARNING);
        SetFocus(R.e_min);
        return 0;
    }

    if (trimmed_text(R.e_max, buf, 64) == 0 || _wcsicmp(buf, L"MAX") == 0) {
        rule->max = RANGE_MAX;
    } else if (!parse_long_text(buf, &rule->max)) {
        MessageBoxW(R.wnd, L"「最大 CHAR」請輸入整數，或輸入 MAX 表示沒有上限。", L"評分規則", MB_ICONWARNING);
        SetFocus(R.e_max);
        return 0;
    }

    {
        wchar_t pbuf[64];
        char *ptext;
        int ok;
        trimmed_text(R.e_penalty, pbuf, 64);
        ptext = wide_to_utf8(pbuf);
        ok = ptext != NULL && parse_penalty(ptext, &rule->penalty);
        free(ptext);
        if (!ok) {
            MessageBoxW(R.wnd, L"「扣分」請輸入數字 (例如 1 或 0.5)，或輸入 ALL 表示扣光該題分數。", L"評分規則",
                        MB_ICONWARNING);
            SetFocus(R.e_penalty);
            return 0;
        }
    }
    return 1;
}

static int selected_row(void)
{
    return ListView_GetNextItem(R.list, -1, LVNI_SELECTED);
}

static void fill_inputs_from_row(int row)
{
    wchar_t buf[64];
    const RangeRule *r;

    if (row < 0 || row >= R.rules.range_count)
        return;
    r = &R.rules.ranges[row];
    swprintf(buf, 64, L"%ld", r->min);
    SetWindowTextW(R.e_min, buf);
    format_rule_max(buf, 64, r->max);
    SetWindowTextW(R.e_max, buf);
    format_rule_penalty(buf, 64, r->penalty);
    SetWindowTextW(R.e_penalty, buf);
}

static void on_add(void)
{
    RangeRule rule;

    if (!read_rule_from_inputs(&rule))
        return;
    if (R.rules.range_count >= MAX_RANGE_RULES) {
        MessageBoxW(R.wnd, L"規則數量已達上限。", L"評分規則", MB_ICONWARNING);
        return;
    }
    R.rules.ranges[R.rules.range_count++] = rule;
    refresh(-1);
    refresh(find_rule_index(rule.min, rule.max, rule.penalty));
}

static void on_modify(void)
{
    RangeRule rule;
    int row = selected_row();

    if (row < 0) {
        MessageBoxW(R.wnd, L"請先在表格中選一條規則。", L"評分規則", MB_ICONINFORMATION);
        return;
    }
    if (!read_rule_from_inputs(&rule))
        return;
    R.rules.ranges[row] = rule;
    refresh(-1);
    refresh(find_rule_index(rule.min, rule.max, rule.penalty));
}

static void on_delete(void)
{
    int row = selected_row(), i;

    if (row < 0) {
        MessageBoxW(R.wnd, L"請先在表格中選一條規則。", L"評分規則", MB_ICONINFORMATION);
        return;
    }
    for (i = row; i < R.rules.range_count - 1; i++)
        R.rules.ranges[i] = R.rules.ranges[i + 1];
    R.rules.range_count--;
    refresh(row < R.rules.range_count ? row : R.rules.range_count - 1);
}

static void on_default(void)
{
    GradeConfig def;

    if (MessageBoxW(R.wnd, L"要把區間規則恢復成預設值嗎？\n\n0→0、1～2→1、3～5→2、6～10→5、11～20→10、21+→20",
                    L"評分規則", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    config_default(&def);
    memcpy(R.rules.ranges, def.rules.ranges, sizeof(R.rules.ranges));
    R.rules.range_count = def.rules.range_count;
    refresh(-1);
}

static void on_save(void)
{
    char msg[4096];
    wchar_t *wmsg, text[4600];

    R.rules.require_contiguous = SendMessageW(R.contiguous, BM_GETCHECK, 0, 0) == BST_CHECKED;
    R.check = score_validate(&R.rules, msg, sizeof(msg));
    wmsg = utf8_to_wide(msg);

    if (R.check == RULES_ERROR) {
        swprintf(text, 4600, L"規則有錯誤，請修正後再儲存：\n\n%ls", wmsg != NULL ? wmsg : L"");
        MessageBoxW(R.wnd, text, L"評分規則", MB_ICONERROR);
    } else if (R.check == RULES_WARNING) {
        swprintf(text, 4600, L"%ls\n仍要儲存嗎？", wmsg != NULL ? wmsg : L"");
        if (MessageBoxW(R.wnd, text, L"評分規則", MB_YESNO | MB_ICONWARNING) == IDYES) {
            R.saved = 1;
            R.done = 1;
        }
    } else {
        R.saved = 1;
        R.done = 1;
    }
    free(wmsg);
}

static LRESULT CALLBACK rules_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_R_ADD:        on_add(); break;
        case ID_R_MODIFY:     on_modify(); break;
        case ID_R_DELETE:     on_delete(); break;
        case ID_R_DEFAULT:    on_default(); break;
        case ID_R_CONTIGUOUS: refresh(selected_row()); break;
        case IDOK:            on_save(); break;
        case IDCANCEL:        R.done = 1; break;
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        LRESULT result;
        if (gui_theme_notify(hdr, &result))
            return result;
        if (hdr->idFrom == ID_R_LIST && hdr->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;
            if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
                fill_inputs_from_row(nm->iItem);
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == R.message) {
            LRESULT brush = gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);
            SetTextColor((HDC)wp, R.check == RULES_ERROR ? g_theme.danger
                                  : R.check == RULES_WARNING ? g_theme.warn : g_theme.success);
            return brush;
        }
        return gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);

    case WM_CLOSE:
        R.done = 1;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int rules_dialog(HWND owner, ScoreRules *rules)
{
    static int registered = 0;
    LVCOLUMNW col;
    HWND wnd;

    if (!registered) {
        register_window_class(L"CGraderRules", rules_proc);
        registered = 1;
    }

    memset(&R, 0, sizeof(R));
    R.rules = *rules;

    wnd = create_dialog_window(L"CGraderRules", L"評分規則 (自訂區間)", owner, 470, 520, 0);
    R.wnd = wnd;

    make_control(wnd, L"STATIC", L"差異數 (CHAR) 落在哪個區間，該 testcase 就扣幾分。每題最低 0 分。", SS_LEFT, 0,
                 12, 10, 446, 20, -1);

    R.list = make_control(wnd, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 0, 12, 34, 446,
                          210, ID_R_LIST);
    ListView_SetExtendedListViewStyle(R.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    gui_style_listview(R.list);
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = dpi(140);
    col.pszText = L"CHAR 最小";
    ListView_InsertColumn(R.list, 0, &col);
    col.pszText = L"CHAR 最大";
    ListView_InsertColumn(R.list, 1, &col);
    col.pszText = L"扣分";
    ListView_InsertColumn(R.list, 2, &col);

    make_control(wnd, L"STATIC", L"最小", SS_LEFT, 0, 12, 259, 34, 20, -1);
    R.e_min = make_control(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 48, 256, 70, 24,
                           ID_R_MIN);
    make_control(wnd, L"STATIC", L"最大", SS_LEFT, 0, 132, 259, 34, 20, -1);
    R.e_max = make_control(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 168, 256, 70, 24,
                           ID_R_MAX);
    make_control(wnd, L"STATIC", L"扣分", SS_LEFT, 0, 252, 259, 34, 20, -1);
    R.e_penalty = make_control(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 288, 256, 70,
                               24, ID_R_PENALTY);
    gui_mark_muted(make_control(wnd, L"STATIC", L"MAX = 無上限\nALL = 扣光該題", SS_LEFT, 0, 366, 252, 96, 40, -1));

    make_control(wnd, L"BUTTON", L"新增", BS_PUSHBUTTON | WS_TABSTOP, 0, 12, 292, 80, 28, ID_R_ADD);
    make_control(wnd, L"BUTTON", L"修改", BS_PUSHBUTTON | WS_TABSTOP, 0, 98, 292, 80, 28, ID_R_MODIFY);
    make_control(wnd, L"BUTTON", L"刪除", BS_PUSHBUTTON | WS_TABSTOP, 0, 184, 292, 80, 28, ID_R_DELETE);
    make_control(wnd, L"BUTTON", L"恢復預設", BS_PUSHBUTTON | WS_TABSTOP, 0, 270, 292, 90, 28, ID_R_DEFAULT);

    R.contiguous = make_control(wnd, L"BUTTON", L"要求區間連續 (不能有空缺)", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                                12, 330, 300, 22, ID_R_CONTIGUOUS);
    SendMessageW(R.contiguous, BM_SETCHECK, R.rules.require_contiguous ? BST_CHECKED : BST_UNCHECKED, 0);

    R.message = make_control(wnd, L"STATIC", L"", SS_LEFT, 0, 12, 358, 446, 110, ID_R_MESSAGE);

    gui_mark_primary(make_control(wnd, L"BUTTON", L"儲存規則", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 262, 480, 100, 30, IDOK));
    make_control(wnd, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 0, 368, 480, 90, 30, IDCANCEL);

    refresh(0);
    fill_inputs_from_row(0);
    SetFocus(R.list);
    run_modal(wnd, owner, &R.done);

    if (R.saved)
        *rules = R.rules;
    return R.saved;
}
