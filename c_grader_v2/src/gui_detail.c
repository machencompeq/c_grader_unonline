/*
 * gui_detail.c - 學生詳細結果視窗。
 *
 *   學號 001　編譯 OK　通過 2/3　差異數 5　扣分 3　成績 97 / 100
 *   ┌─────────────────┐  test02　WA　Difference 1　扣分 1　得分 32.33 / 33.33
 *   │ test01 AC ...   │  第一個不同處：第 1 行第 6 個字
 *   │ test02 WA ...   │  Expected (標準答案)      Actual (學生輸出)
 *   └─────────────────┘  [                   ]  [                   ]
 *   [查看編譯訊息] [查看 AI 修正] [在瀏覽器開啟比對報告]                  [關閉]
 */
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "comparator.h"
#include "gui.h"
#include "util.h"

enum {
    ID_D_LIST = 300,
    ID_D_SHOW_WS,
    ID_D_COMPILE_LOG,
    ID_D_FIX,
    ID_D_OPEN_REPORT
};

static struct {
    HWND wnd, summary, list, test_info, show_ws, expected_label, actual_label, expected, actual, log_btn, fix_btn,
        report_btn, close_btn;
    const Grader *g;
    const StudentResult *r;
    char report_path[GRADER_PATH_MAX];
    int current;
    volatile int done;
} D;

static void layout(void)
{
    RECT rc;
    int w, h, right_x, right_w, half;

    GetClientRect(D.wnd, &rc);
    w = MulDiv(rc.right, 96, dpi(96));
    h = MulDiv(rc.bottom, 96, dpi(96));
    right_x = 316;
    right_w = w - right_x - 10;
    half = (right_w - 10) / 2;

    move_control(D.summary, 10, 10, w - 20, 22);
    move_control(D.list, 10, 40, 296, h - 90);
    move_control(D.test_info, right_x, 40, right_w, 44);
    move_control(D.show_ws, right_x, 88, 260, 22);
    move_control(D.expected_label, right_x, 114, half, 20);
    move_control(D.actual_label, right_x + half + 10, 114, half, 20);
    move_control(D.expected, right_x, 136, half, h - 186);
    move_control(D.actual, right_x + half + 10, 136, half, h - 186);
    move_control(D.log_btn, 10, h - 40, 120, 30);
    move_control(D.fix_btn, 138, h - 40, 120, 30);
    move_control(D.report_btn, 266, h - 40, 170, 30);
    move_control(D.close_btn, w - 100, h - 40, 90, 30);
}

static void show_test(int index)
{
    const TestResult *t;
    StrBuf info = {0};
    char full[32], score[32], deduction[32];
    const char *expected;
    size_t expected_len;
    wchar_t *text;
    int show_ws;
    long line, column;

    if (index < 0 || index >= D.r->test_count)
        return;
    D.current = index;
    t = &D.r->tests[index];
    show_ws = SendMessageW(D.show_ws, BM_GETCHECK, 0, 0) == BST_CHECKED;

    format_score(full, sizeof(full), t->full);
    format_score(score, sizeof(score), t->score);
    format_score(deduction, sizeof(deduction), t->deduction);

    sb_appendf(&info, "%s　%s　", D.g->tests.items[index].name, test_status_name(t->status));
    if (t->diff >= 0)
        sb_appendf(&info, "Difference：%ld%s　", t->diff, t->approximate ? " (近似)" : "");
    else
        sb_append(&info, "Difference：-　");
    sb_appendf(&info, "扣分：%s　得分：%s / %s　時間：%d ms\n", deduction, score, full, t->elapsed_ms);

    expected = D.g->expected[index];
    expected_len = D.g->expected_len[index];
    if (expected_len > OUTPUT_PREVIEW_LIMIT)
        expected_len = OUTPUT_PREVIEW_LIMIT;

    switch (t->status) {
    case TEST_RUNTIME_ERROR:
        sb_appendf(&info, "Runtime Error：程式異常結束 (結束碼 0x%08lX)。%s", t->exit_code,
                   D.g->cfg.runtime_error_zero ? "該題 0 分。" : "已印出的內容仍照常比對。");
        break;
    case TEST_TIMEOUT:
        sb_appendf(&info, "Time Limit Exceeded：超過 %g 秒被強制結束，該題 0 分。", D.g->cfg.timeout_ms / 1000.0);
        break;
    case TEST_OUTPUT_LIMIT:
        sb_append(&info, "Output Limit Exceeded：輸出太大被強制結束，該題 0 分。");
        break;
    default:
        if (t->diff == 0)
            sb_append(&info, "依目前比對設定，與標準答案相同");
        else if (t->actual != NULL && compare_first_difference(expected, expected_len, t->actual, t->actual_len,
                                                                &line, &column))
            sb_appendf(&info, "第一個不同處 (逐字)：第 %ld 行，第 %ld 個字　(差異位置標示請看 HTML 報告)", line,
                       column);
        break;
    }
    set_text_utf8(D.test_info, info.data);
    sb_free(&info);

    text = make_display_text(expected, expected_len, show_ws);
    SetWindowTextW(D.expected, text);
    free(text);

    if (t->actual == NULL) {
        SetWindowTextW(D.actual, L"(沒有輸出)");
    } else {
        text = make_display_text(t->actual, t->actual_len, show_ws);
        SetWindowTextW(D.actual, text);
        free(text);
        if (t->actual_truncated) {
            int n = GetWindowTextLengthW(D.actual);
            SendMessageW(D.actual, EM_SETSEL, n, n);
            SendMessageW(D.actual, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n\r\n…(輸出太長，只顯示前 64 KB)");
        }
    }
}

/* 編譯訊息：原始碼的 gcc 訊息 + (有 AI 修正時) 修正後程式的訊息 */
static void show_compile_log(HWND wnd)
{
    wchar_t title[300];
    wchar_t *id = utf8_to_wide(D.r->id);
    StrBuf sb = {0};

    swprintf(title, 300, L"%ls 編譯訊息", id != NULL ? id : L"");
    free(id);
    sb_append(&sb, D.r->compile_log != NULL && D.r->compile_log[0] != '\0' ? D.r->compile_log : "(沒有任何編譯訊息)");
    if (student_has_fix(D.r)) {
        sb_appendf(&sb, "\n\n==================== AI (%s) 修正後程式的 gcc 訊息 ====================\n", D.r->fix_tool);
        sb_append(&sb, D.r->fix_log != NULL && D.r->fix_log[0] != '\0' ? D.r->fix_log : "(沒有任何訊息，編譯成功)");
    }
    show_text_window(wnd, title, sb.data);
    sb_free(&sb);
}

/* AI 修正後的程式碼 (逐字差異標示請看 HTML 報告) */
static void show_fix(HWND wnd)
{
    wchar_t title[300];
    wchar_t *id = utf8_to_wide(D.r->id);
    StrBuf sb = {0};
    char pen[32];

    swprintf(title, 300, L"%ls - AI 修正後的程式碼", id != NULL ? id : L"");
    free(id);
    format_score(pen, sizeof(pen), D.r->fix_penalty);
    if (D.r->status == STUDENT_CE_FIXED)
        sb_appendf(&sb, "編譯失敗 → AI (%s) 最小修正 %ld 個字元 (嘗試 %d 次)，修正扣分 %s 分。\n"
                        "被修改的字元逐字標示請看 HTML 報告。\n\n",
                   D.r->fix_tool, D.r->fix_chars, D.r->fix_attempts, pen);
    else
        sb_appendf(&sb, "AI (%s) 嘗試修正 %d 次後仍無法編譯，維持 Compile Error。以下是最後一次的嘗試：\n\n",
                   D.r->fix_tool, D.r->fix_attempts);
    {
        char *fix = student_fix_text(D.r);
        sb_append(&sb, fix);
        free(fix);
    }
    show_text_window(wnd, title, sb.data);
    sb_free(&sb);
}

static LRESULT CALLBACK detail_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (D.close_btn != NULL)
            layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = dpi(800);
        mm->ptMinTrackSize.y = dpi(420);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_D_SHOW_WS:
            show_test(D.current);
            break;
        case ID_D_COMPILE_LOG:
            show_compile_log(wnd);
            break;
        case ID_D_FIX:
            show_fix(wnd);
            break;
        case ID_D_OPEN_REPORT: {
            wchar_t *path = utf8_to_wide(D.report_path);
            if (path != NULL)
                ShellExecuteW(wnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
            free(path);
            break;
        }
        case IDOK:
        case IDCANCEL:
            D.done = 1;
            break;
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        LRESULT result;
        if (gui_theme_notify(hdr, &result))
            return result;
        if (hdr->idFrom == ID_D_LIST && hdr->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;
            if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
                show_test(nm->iItem);
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);

    case WM_CLOSE:
        D.done = 1;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void detail_dialog(HWND owner, const Grader *g, const StudentResult *r, const char *report_path)
{
    static int registered = 0;
    wchar_t title[300], *id;
    char full[32], score[32], deduction[32], pen[32];
    StrBuf summary = {0};
    LVCOLUMNW col;
    HWND wnd;
    int i;

    if (!registered) {
        register_window_class(L"CGraderDetail", detail_proc);
        registered = 1;
    }
    memset(&D, 0, sizeof(D));
    D.g = g;
    D.r = r;

    id = utf8_to_wide(r->id);
    swprintf(title, 300, L"詳細結果 - %ls", id != NULL ? id : L"");
    free(id);
    wnd = create_dialog_window(L"CGraderDetail", title, owner, 1000, 620, 1);
    D.wnd = wnd;

    format_score(full, sizeof(full), g->cfg.full_score);
    format_score(score, sizeof(score), r->score);
    format_score(deduction, sizeof(deduction), r->total_deduction);
    format_score(pen, sizeof(pen), r->fix_penalty);
    if (r->name[0] != '\0')
        sb_appendf(&summary, "%s (%s)", r->name, r->id);
    else
        sb_append(&summary, r->id);
    sb_append(&summary, "　　編譯：");
    if (r->status == STUDENT_CE_FIXED)
        sb_appendf(&summary, "CE→AI 修正 %ld 字元 (扣 %s)", r->fix_chars, pen);
    else
        sb_append(&summary, student_status_text(r->status));
    sb_appendf(&summary, "　　通過：%d/%d　　差異數：", r->passed, r->test_count);
    if (r->total_diff >= 0)
        sb_appendf(&summary, "%ld", r->total_diff);
    else
        sb_append(&summary, "-");
    sb_appendf(&summary, "　　總扣分：%s　　成績：%s / %s　　(比對模式：%s)", deduction, score, full,
               compare_mode_name(g->cfg.compare.mode));

    D.summary = make_control(wnd, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, 10, 10, 980, 22, -1);
    SendMessageW(D.summary, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
    set_text_utf8(D.summary, summary.data);
    sb_free(&summary);

    D.list = make_control(wnd, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 0, 10, 40, 296,
                          530, ID_D_LIST);
    ListView_SetExtendedListViewStyle(D.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    gui_style_listview(D.list);
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = dpi(90);
    col.pszText = L"Testcase";
    ListView_InsertColumn(D.list, 0, &col);
    col.cx = dpi(50);
    col.pszText = L"結果";
    ListView_InsertColumn(D.list, 1, &col);
    col.cx = dpi(55);
    col.pszText = L"差異";
    ListView_InsertColumn(D.list, 2, &col);
    col.cx = dpi(95);
    col.pszText = L"得分";
    ListView_InsertColumn(D.list, 3, &col);

    for (i = 0; i < r->test_count; i++) {
        const TestResult *t = &r->tests[i];
        wchar_t *w;
        char buf[300], s1[32], s2[32];
        LVITEMW item;

        snprintf(buf, sizeof(buf), "%s%s", g->tests.items[i].name, g->tests.items[i].hidden ? " (隱藏)" : "");
        w = utf8_to_wide(buf);
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = w;
        ListView_InsertItem(D.list, &item);
        free(w);

        w = utf8_to_wide(test_status_name(t->status));
        ListView_SetItemText(D.list, i, 1, w);
        free(w);

        if (t->diff >= 0)
            snprintf(buf, sizeof(buf), "%ld", t->diff);
        else
            snprintf(buf, sizeof(buf), "-");
        w = utf8_to_wide(buf);
        ListView_SetItemText(D.list, i, 2, w);
        free(w);

        format_score(s1, sizeof(s1), t->score);
        format_score(s2, sizeof(s2), t->full);
        snprintf(buf, sizeof(buf), "%s / %s", s1, s2);
        w = utf8_to_wide(buf);
        ListView_SetItemText(D.list, i, 3, w);
        free(w);
    }

    D.test_info = make_control(wnd, L"STATIC", L"", SS_LEFT, 0, 316, 40, 674, 44, -1);
    D.show_ws = make_control(wnd, L"BUTTON", L"顯示空白與換行符號 ( · → ↵ )", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                             316, 88, 260, 22, ID_D_SHOW_WS);
    D.expected_label = make_control(wnd, L"STATIC", L"Expected (標準答案)", SS_LEFT, 0, 316, 114, 330, 20, -1);
    D.actual_label = make_control(wnd, L"STATIC", L"Actual (學生輸出)", SS_LEFT, 0, 656, 114, 330, 20, -1);
    SendMessageW(D.expected_label, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
    SendMessageW(D.actual_label, WM_SETFONT, (WPARAM)g_bold_font, TRUE);

    D.expected = make_control(wnd, L"EDIT", L"",
                              WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                                  ES_AUTOHSCROLL | WS_TABSTOP,
                              WS_EX_CLIENTEDGE, 316, 136, 330, 430, -1);
    D.actual = make_control(wnd, L"EDIT", L"",
                            WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                                ES_AUTOHSCROLL | WS_TABSTOP,
                            WS_EX_CLIENTEDGE, 656, 136, 330, 430, -1);
    SendMessageW(D.expected, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    SendMessageW(D.actual, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    SendMessageW(D.expected, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(D.actual, EM_SETLIMITTEXT, 0, 0);

    D.log_btn = make_control(wnd, L"BUTTON", L"查看編譯訊息", BS_PUSHBUTTON | WS_TABSTOP, 0, 10, 580, 120, 30,
                             ID_D_COMPILE_LOG);
    D.fix_btn = make_control(wnd, L"BUTTON", L"查看 AI 修正", BS_PUSHBUTTON | WS_TABSTOP, 0, 138, 580, 120, 30,
                             ID_D_FIX);
    EnableWindow(D.fix_btn, student_has_fix(r));
    D.report_btn = make_control(wnd, L"BUTTON", L"在瀏覽器開啟比對報告", BS_PUSHBUTTON | WS_TABSTOP, 0, 266, 580,
                                170, 30, ID_D_OPEN_REPORT);
    snprintf(D.report_path, sizeof(D.report_path), "%s", report_path != NULL ? report_path : "");
    EnableWindow(D.report_btn, D.report_path[0] != '\0');
    D.close_btn = make_control(wnd, L"BUTTON", L"關閉", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 900, 580, 90, 30, IDOK);

    layout();
    if (r->test_count > 0) {
        ListView_SetItemState(D.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        show_test(0);
    }
    SetFocus(D.list);
    run_modal(wnd, owner, &D.done);
}
