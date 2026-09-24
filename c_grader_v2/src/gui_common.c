#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "gui.h"
#include "util.h"

HINSTANCE g_inst;
HFONT g_font;
HFONT g_bold_font;
HFONT g_mono_font;
HFONT g_title_font;
static int g_dpi = 96;

/* ---------------- 深色科技風主題 ---------------- */

const GuiTheme g_theme = {
    RGB(13, 19, 30),     /* bg */
    RGB(22, 31, 46),     /* field */
    RGB(34, 47, 68),     /* field_hot */
    RGB(44, 60, 86),     /* border */
    RGB(17, 25, 38),     /* header */
    RGB(226, 233, 243),  /* text */
    RGB(139, 155, 178),  /* muted */
    RGB(56, 189, 248),   /* accent */
    RGB(103, 208, 255),  /* accent_hot */
    RGB(20, 150, 210),   /* accent_dark */
    RGB(8, 15, 26),      /* on_accent */
    RGB(52, 211, 153),   /* success */
    RGB(251, 191, 36),   /* warn */
    RGB(251, 113, 133),  /* danger */
    RGB(129, 140, 248),  /* info */
    RGB(20, 52, 40),     /* row_ok */
    RGB(70, 26, 38),     /* row_bad */
    RGB(66, 52, 18),     /* row_warn */
    RGB(28, 45, 78),     /* row_fixed */
    RGB(19, 27, 40),     /* row_alt */
};

static HBRUSH br_bg, br_field, br_header;
static HPEN pen_border, pen_accent;

static HFONT make_font(const wchar_t *face, int points, int weight, DWORD pitch)
{
    return CreateFontW(-MulDiv(points, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, pitch, face);
}

void gui_common_init(HINSTANCE instance)
{
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    g_inst = instance;

    g_font = make_font(L"Microsoft JhengHei UI", 10, FW_NORMAL, DEFAULT_PITCH);
    g_bold_font = make_font(L"Microsoft JhengHei UI", 10, FW_BOLD, DEFAULT_PITCH);
    g_title_font = make_font(L"Microsoft JhengHei UI", 11, FW_SEMIBOLD, DEFAULT_PITCH);
    g_mono_font = make_font(L"Consolas", 10, FW_NORMAL, FIXED_PITCH);

    br_bg = CreateSolidBrush(g_theme.bg);
    br_field = CreateSolidBrush(g_theme.field);
    br_header = CreateSolidBrush(g_theme.header);
    pen_border = CreatePen(PS_SOLID, 1, g_theme.border);
    pen_accent = CreatePen(PS_SOLID, 1, g_theme.accent);
}

int dpi(int px)
{
    return MulDiv(px, g_dpi, 96);
}

static int is_class(HWND h, const wchar_t *cls)
{
    wchar_t name[64];
    return h != NULL && GetClassNameW(h, name, 64) > 0 && _wcsicmp(name, cls) == 0;
}

void gui_mark_primary(HWND button)
{
    SetPropW(button, L"cg_primary", (HANDLE)1);
}

void gui_mark_muted(HWND label)
{
    SetPropW(label, L"cg_muted", (HANDLE)1);
    InvalidateRect(label, NULL, TRUE);
}

LRESULT gui_theme_ctlcolor(UINT msg, HDC dc, HWND control)
{
    /*
     * Edit / ListBox 必須用不透明背景 (OPAQUE)：Edit 刪字、捲動時只重畫那一行文字，
     * 透明背景不會蓋掉舊字，按 Backspace 就會留下殘影。
     */
    switch (msg) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkMode(dc, OPAQUE);
        SetTextColor(dc, g_theme.text);
        SetBkColor(dc, g_theme.field);
        return (LRESULT)br_field;
    case WM_CTLCOLORSTATIC:
        if (is_class(control, L"EDIT")) { /* 唯讀 Edit 也走這裡 */
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, g_theme.text);
            SetBkColor(dc, g_theme.field);
            return (LRESULT)br_field;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetPropW(control, L"cg_muted") != NULL ? g_theme.muted : g_theme.text);
        SetBkColor(dc, g_theme.bg);
        return (LRESULT)br_bg;
    default:
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_theme.text);
        SetBkColor(dc, g_theme.bg);
        return (LRESULT)br_bg;
    }
}

LRESULT gui_control_color(HDC dc)
{
    return gui_theme_ctlcolor(WM_CTLCOLORSTATIC, dc, NULL);
}

static void rounded(HDC dc, const RECT *rc, int radius, COLORREF fill, COLORREF line)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

/* 白色勾勾：box 是核取方塊的矩形 */
static void draw_check_mark(HDC dc, const RECT *box, COLORREF color)
{
    HPEN p = CreatePen(PS_SOLID, dpi(2), color);
    HGDIOBJ op = SelectObject(dc, p);
    int w = box->right - box->left, h = box->bottom - box->top;
    POINT pts[3] = {{box->left + w * 27 / 100, box->top + h * 52 / 100},
                    {box->left + w * 45 / 100, box->top + h * 72 / 100},
                    {box->left + w * 76 / 100, box->top + h * 30 / 100}};
    Polyline(dc, pts, 3);
    SelectObject(dc, op);
    DeleteObject(p);
}

/* 按鈕類 (push / checkbox / radio / groupbox) 的自繪 */
static LRESULT draw_button(NMCUSTOMDRAW *cd)
{
    HWND h = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT rc = cd->rc;
    DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
    UINT type = style & BS_TYPEMASK;
    int disabled = (cd->uItemState & CDIS_DISABLED) || !IsWindowEnabled(h);
    int hot = (cd->uItemState & CDIS_HOT) != 0, pressed = (cd->uItemState & CDIS_SELECTED) != 0;
    int focus = (cd->uItemState & CDIS_FOCUS) != 0;
    wchar_t text[512];
    HFONT font = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = SelectObject(dc, font != NULL ? font : g_font);
    COLORREF text_color = disabled ? g_theme.muted : g_theme.text;

    if (cd->dwDrawStage != CDDS_PREPAINT) {
        SelectObject(dc, old_font);
        return CDRF_DODEFAULT;
    }
    GetWindowTextW(h, text, 512);
    SetBkMode(dc, TRANSPARENT);
    if (rc.right <= rc.left || rc.bottom <= rc.top) /* 群組框的 NM_CUSTOMDRAW 不會給矩形 */
        GetClientRect(h, &rc);
    FillRect(dc, &rc, br_bg);

    if (type == BS_GROUPBOX) {
        /* 圓角框線 + 強調色標題 (標題壓在框線上) */
        RECT frame = rc, title;
        SIZE sz;
        frame.top += dpi(8);
        rounded(dc, &frame, dpi(10), g_theme.bg, g_theme.border);
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
        title.left = rc.left + dpi(12);
        title.top = rc.top;
        title.right = title.left + sz.cx + dpi(12);
        title.bottom = title.top + sz.cy + dpi(2);
        FillRect(dc, &title, br_bg);
        SetTextColor(dc, g_theme.accent);
        title.left += dpi(6);
        DrawTextW(dc, text, -1, &title, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    } else if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE ||
               type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {
        int checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int size = dpi(15), radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
        RECT box, label = rc;
        COLORREF fill = checked ? (disabled ? g_theme.accent_dark : g_theme.accent) : (hot ? g_theme.field_hot : g_theme.field);
        COLORREF line = checked ? fill : (hot ? g_theme.accent : g_theme.border);

        box.left = rc.left + dpi(1);
        box.top = (rc.top + rc.bottom - size) / 2;
        box.right = box.left + size;
        box.bottom = box.top + size;
        if (radio) {
            HBRUSH b = CreateSolidBrush(hot && !checked ? g_theme.field_hot : g_theme.field);
            HPEN p = CreatePen(PS_SOLID, 1, checked || hot ? g_theme.accent : g_theme.border);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            Ellipse(dc, box.left, box.top, box.right, box.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(p);
            if (checked) {
                HBRUSH dot = CreateSolidBrush(disabled ? g_theme.accent_dark : g_theme.accent);
                HPEN np = CreatePen(PS_NULL, 0, 0);
                int inset = dpi(4);
                ob = SelectObject(dc, dot);
                op = SelectObject(dc, np);
                Ellipse(dc, box.left + inset, box.top + inset, box.right - inset, box.bottom - inset);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(dot);
                DeleteObject(np);
            }
        } else {
            rounded(dc, &box, dpi(4), fill, line);
            if (checked)
                draw_check_mark(dc, &box, g_theme.on_accent);
        }
        label.left = box.right + dpi(7);
        SetTextColor(dc, text_color);
        DrawTextW(dc, text, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (focus && !(cd->uItemState & CDIS_SHOWKEYBOARDCUES) == 0) {
            RECT fr = label;
            SIZE sz;
            GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
            fr.right = fr.left + sz.cx + dpi(4);
            fr.left -= dpi(2);
            fr.top = (rc.top + rc.bottom - sz.cy) / 2 - dpi(1);
            fr.bottom = fr.top + sz.cy + dpi(2);
            SetTextColor(dc, g_theme.muted);
            DrawFocusRect(dc, &fr);
        }
    } else {
        /* 一般按鈕：圓角；主要按鈕用強調色 */
        int primary = GetPropW(h, L"cg_primary") != NULL || type == BS_DEFPUSHBUTTON;
        COLORREF fill, line;
        if (disabled) {
            fill = g_theme.field;
            line = g_theme.border;
            text_color = g_theme.muted;
        } else if (primary) {
            fill = pressed ? g_theme.accent_dark : hot ? g_theme.accent_hot : g_theme.accent;
            line = fill;
            text_color = g_theme.on_accent;
        } else {
            fill = pressed ? g_theme.bg : hot ? g_theme.field_hot : g_theme.field;
            line = hot || focus ? g_theme.accent : g_theme.border;
        }
        rounded(dc, &rc, dpi(8), fill, line);
        SetTextColor(dc, text_color);
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old_font);
    return CDRF_SKIPDEFAULT;
}

void gui_draw_panel(HDC dc, const RECT *rc, const wchar_t *title)
{
    RECT frame = *rc, tr;
    SIZE sz;
    HGDIOBJ old_font = SelectObject(dc, g_bold_font);

    SetBkMode(dc, TRANSPARENT);
    frame.top += dpi(9);
    rounded(dc, &frame, dpi(10), g_theme.bg, g_theme.border);
    GetTextExtentPoint32W(dc, title, (int)wcslen(title), &sz);
    tr.left = rc->left + dpi(12);
    tr.top = rc->top;
    tr.right = tr.left + sz.cx + dpi(12);
    tr.bottom = tr.top + sz.cy + dpi(2);
    FillRect(dc, &tr, br_bg);
    tr.left += dpi(6);
    SetTextColor(dc, g_theme.accent);
    DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
}

int gui_theme_notify(NMHDR *hdr, LRESULT *result)
{
    if (hdr->code == NM_CUSTOMDRAW && is_class(hdr->hwndFrom, L"Button")) {
        *result = draw_button((NMCUSTOMDRAW *)hdr);
        return 1;
    }
    return 0;
}

/* 狀態列：SBT_OWNERDRAW 的文字 */
int gui_theme_drawitem(DRAWITEMSTRUCT *dis)
{
    RECT rc = dis->rcItem;
    const wchar_t *text = (const wchar_t *)dis->itemData;

    if (!is_class(dis->hwndItem, STATUSCLASSNAMEW))
        return 0;
    FillRect(dis->hDC, &rc, br_bg);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, g_theme.muted);
    SelectObject(dis->hDC, g_font);
    rc.left += dpi(8);
    DrawTextW(dis->hDC, text != NULL ? text : L"", -1, &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    return 1;
}

void gui_status_text(HWND status, const wchar_t *text)
{
    static wchar_t buffer[2048]; /* 狀態列自繪時要用到，必須一直存在 */
    wcsncpy(buffer, text != NULL ? text : L"", 2047);
    buffer[2047] = L'\0';
    SendMessageW(status, SB_SETTEXTW, SBT_OWNERDRAW, (LPARAM)buffer);
}

/* 表頭自繪：深色底、淺色字、排序箭頭 */
static LRESULT CALLBACK header_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        int i, n = Header_GetItemCount(h);
        HGDIOBJ old_font = SelectObject(dc, g_bold_font), old_pen = SelectObject(dc, pen_border);

        GetClientRect(h, &rc);
        FillRect(dc, &rc, br_header);
        SetBkMode(dc, TRANSPARENT);
        for (i = 0; i < n; i++) {
            HDITEMW item;
            wchar_t text[128];
            RECT r, tr;
            memset(&item, 0, sizeof(item));
            item.mask = HDI_TEXT | HDI_FORMAT;
            item.pszText = text;
            item.cchTextMax = 128;
            Header_GetItem(h, i, &item);
            Header_GetItemRect(h, i, &r);
            tr = r;
            tr.left += dpi(8);
            tr.right -= dpi(8);
            if (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
                /* 排序箭頭 */
                int cx = r.right - dpi(12), cy = (r.top + r.bottom) / 2, s = dpi(3);
                POINT pts[3];
                HBRUSH b = CreateSolidBrush(g_theme.accent);
                HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen_accent);
                if (item.fmt & HDF_SORTUP) {
                    pts[0].x = cx - s; pts[0].y = cy + s / 2;
                    pts[1].x = cx + s; pts[1].y = cy + s / 2;
                    pts[2].x = cx;     pts[2].y = cy - s;
                } else {
                    pts[0].x = cx - s; pts[0].y = cy - s / 2;
                    pts[1].x = cx + s; pts[1].y = cy - s / 2;
                    pts[2].x = cx;     pts[2].y = cy + s;
                }
                Polygon(dc, pts, 3);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(b);
                tr.right -= dpi(12);
            }
            SetTextColor(dc, g_theme.muted);
            DrawTextW(dc, text, -1, &tr,
                      ((item.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE |
                          DT_NOPREFIX | DT_END_ELLIPSIS);
            MoveToEx(dc, r.right - 1, r.top + dpi(6), NULL);
            LineTo(dc, r.right - 1, r.bottom - dpi(6));
        }
        MoveToEx(dc, rc.left, rc.bottom - 1, NULL);
        LineTo(dc, rc.right, rc.bottom - 1);
        SelectObject(dc, old_font);
        SelectObject(dc, old_pen);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(h, header_proc, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

void gui_style_listview(HWND list)
{
    HWND header = ListView_GetHeader(list);
    DWORD ex = ListView_GetExtendedListViewStyle(list);

    SetWindowTheme(list, L"DarkMode_Explorer", NULL); /* 深色捲軸與選取列 (Windows 10 1809+；舊版會忽略) */
    ListView_SetExtendedListViewStyle(list, (ex & ~LVS_EX_GRIDLINES) | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                LVS_EX_HEADERDRAGDROP);
    ListView_SetBkColor(list, g_theme.field);
    ListView_SetTextBkColor(list, g_theme.field);
    ListView_SetTextColor(list, g_theme.text);
    if (header != NULL)
        SetWindowSubclass(header, header_proc, 1, 0);
}

void gui_set_sort_arrow(HWND list, int column, int ascending)
{
    HWND header = ListView_GetHeader(list);
    int i, n = Header_GetItemCount(header);

    for (i = 0; i < n; i++) {
        HDITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = HDI_FORMAT;
        Header_GetItem(header, i, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == column)
            item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, i, &item);
    }
    InvalidateRect(header, NULL, TRUE);
}

static BOOL CALLBACK theme_child(HWND h, LPARAM lp)
{
    wchar_t cls[64];
    DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
    (void)lp;

    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"EDIT") == 0) {
        if (style & ES_MULTILINE)
            SetWindowTheme(h, L"DarkMode_Explorer", NULL); /* 深色捲軸 */
        else
            SetWindowTheme(h, L"", L"");
        SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(dpi(5), dpi(5)));
    } else if (_wcsicmp(cls, WC_COMBOBOXW) == 0) {
        SetWindowTheme(h, L"DarkMode_CFD", NULL);
    } else if (_wcsicmp(cls, PROGRESS_CLASSW) == 0) {
        SetWindowTheme(h, L"", L""); /* 沒有主題時才能自訂顏色 */
        SendMessageW(h, PBM_SETBARCOLOR, 0, (LPARAM)g_theme.accent);
        SendMessageW(h, PBM_SETBKCOLOR, 0, (LPARAM)g_theme.field);
    } else if (_wcsicmp(cls, STATUSCLASSNAMEW) == 0) {
        SetWindowTheme(h, L"", L"");
        SendMessageW(h, SB_SETBKCOLOR, 0, (LPARAM)g_theme.bg);
    } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
        gui_style_listview(h);
    }
    return TRUE;
}

void gui_theme_window(HWND wnd)
{
    BOOL dark = TRUE;
    /* 深色標題列：DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 2004+)，舊版用 19 */
    if (FAILED(DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(wnd, 19, &dark, sizeof(dark));
    EnumChildWindows(wnd, theme_child, 0);
}

HWND make_control(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex_style,
                  int x, int y, int w, int h, int id)
{
    HWND control;

    if (_wcsicmp(cls, L"EDIT") == 0 && (ex_style & WS_EX_CLIENTEDGE)) { /* 3D 凹框在深色下很突兀：改細框線 */
        ex_style &= ~WS_EX_CLIENTEDGE;
        style |= WS_BORDER;
    }
    control = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, dpi(x), dpi(y), dpi(w),
                              dpi(h), parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    return control;
}

void move_control(HWND control, int x, int y, int w, int h)
{
    MoveWindow(control, dpi(x), dpi(y), dpi(w < 0 ? 0 : w), dpi(h < 0 ? 0 : h), TRUE);
}

void register_window_class(const wchar_t *name, WNDPROC proc)
{
    WNDCLASSEXW wc;
    HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(1)); /* res\app.ico */

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = icon != NULL ? icon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hIconSm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hbrBackground = br_bg;
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

HWND create_dialog_window(const wchar_t *cls, const wchar_t *title, HWND owner, int client_w, int client_h,
                          int resizable)
{
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT rc = {0, 0, dpi(client_w), dpi(client_h)};
    RECT owner_rc;
    int w, h, x, y;

    if (resizable)
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    /* 放在主視窗正中間 */
    GetWindowRect(owner, &owner_rc);
    x = owner_rc.left + (owner_rc.right - owner_rc.left - w) / 2;
    y = owner_rc.top + (owner_rc.bottom - owner_rc.top - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    return CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title, style, x, y, w, h, owner, NULL, g_inst, NULL);
}

void run_modal(HWND dialog, HWND owner, volatile int *done)
{
    MSG msg;

    gui_theme_window(dialog);
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    while (!*done) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got <= 0) {
            PostQuitMessage((int)msg.wParam); /* 讓外層的訊息迴圈也結束 */
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE); /* 先恢復主視窗，再關 dialog，焦點才會回到主視窗 */
    DestroyWindow(dialog);
    SetForegroundWindow(owner);
}

void set_text_utf8(HWND control, const char *text)
{
    wchar_t *w = utf8_to_wide(text != NULL ? text : "");
    SetWindowTextW(control, w != NULL ? w : L"");
    free(w);
}

wchar_t *make_display_text(const char *text, size_t len, int show_ws)
{
    wchar_t *w, *out;
    int n, i;
    size_t k = 0;

    if (text == NULL || len == 0)
        return _wcsdup(L"");

    n = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
    w = malloc((n + 1) * sizeof(wchar_t));
    out = malloc((n * 3 + 1) * sizeof(wchar_t));
    if (w == NULL || out == NULL) {
        free(w);
        free(out);
        return _wcsdup(L"(記憶體不足)");
    }
    MultiByteToWideChar(CP_UTF8, 0, text, (int)len, w, n);

    for (i = 0; i < n; i++) {
        wchar_t c = w[i];
        if (c == L'\r' && i + 1 < n && w[i + 1] == L'\n')
            continue;
        if (c == L'\n') {
            if (show_ws)
                out[k++] = L'↵';
            out[k++] = L'\r';
            out[k++] = L'\n';
        } else if (c == L'\0') {
            out[k++] = 0x2400; /* ␀ */
        } else if (c == L'\r') {
            out[k++] = 0x240D; /* 單獨的 \r 顯示成 ␍ */
        } else if (show_ws && c == L' ') {
            out[k++] = L'·';
        } else if (show_ws && c == L'\t') {
            out[k++] = L'→';
        } else {
            out[k++] = c;
        }
    }
    out[k] = L'\0';
    free(w);
    return out;
}

int read_number(HWND edit, double *out)
{
    wchar_t buf[64], *start = buf, *end;
    size_t n;

    GetWindowTextW(edit, buf, 64);
    while (*start == L' ')
        start++;
    n = wcslen(start);
    while (n > 0 && start[n - 1] == L' ')
        start[--n] = L'\0';
    if (n == 0)
        return 0;
    *out = wcstod(start, &end);
    return *end == L'\0';
}

/* ---------------- 工具提示 ---------------- */

HWND gui_create_tooltip(HWND parent)
{
    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, NULL, g_inst,
                               NULL);
    if (tip != NULL) {
        SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, dpi(420)); /* 太長就換行 */
        SendMessageW(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
        SendMessageW(tip, WM_SETFONT, (WPARAM)g_font, TRUE);
        SetWindowTheme(tip, L"DarkMode_Explorer", NULL);
    }
    return tip;
}

void gui_add_tooltip(HWND tooltip, HWND control, const wchar_t *text)
{
    TOOLINFOW ti;

    if (tooltip == NULL || control == NULL)
        return;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(control);
    ti.uId = (UINT_PTR)control;
    ti.lpszText = (wchar_t *)text;
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

/* ---------------- 文字視窗 ---------------- */

enum { ID_TEXT_EDIT = 10 };

static struct {
    HWND edit, close_btn;
    volatile int done;
} TW;

static void text_window_layout(HWND wnd)
{
    RECT rc;
    int w, h;
    GetClientRect(wnd, &rc);
    w = MulDiv(rc.right, 96, dpi(96));
    h = MulDiv(rc.bottom, 96, dpi(96));
    move_control(TW.edit, 10, 10, w - 20, h - 58);
    move_control(TW.close_btn, w - 100, h - 40, 90, 30);
}

static LRESULT CALLBACK text_window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT result;

    switch (msg) {
    case WM_SIZE:
        text_window_layout(wnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
            TW.done = 1;
        return 0;
    case WM_NOTIFY:
        if (gui_theme_notify((NMHDR *)lp, &result))
            return result;
        return 0;
    case WM_CLOSE:
        TW.done = 1;
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void show_text_window(HWND owner, const wchar_t *title, const char *text)
{
    static int registered = 0;
    HWND wnd;
    wchar_t *display;

    if (!registered) {
        register_window_class(L"CGraderText", text_window_proc);
        registered = 1;
    }

    TW.done = 0;
    wnd = create_dialog_window(L"CGraderText", title, owner, 760, 480, 1);
    TW.edit = make_control(wnd, L"EDIT", L"",
                           WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                               ES_AUTOHSCROLL | WS_TABSTOP,
                           WS_EX_CLIENTEDGE, 10, 10, 740, 422, ID_TEXT_EDIT);
    SendMessageW(TW.edit, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    SendMessageW(TW.edit, EM_SETLIMITTEXT, 0, 0);
    TW.close_btn = make_control(wnd, L"BUTTON", L"關閉", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 660, 440, 90, 30, IDOK);

    display = make_display_text(text, text != NULL ? strlen(text) : 0, 0);
    SetWindowTextW(TW.edit, display);
    free(display);

    text_window_layout(wnd);
    SetFocus(TW.close_btn);
    run_modal(wnd, owner, &TW.done);
}

/* ---------------- 貼上文字對話框 ---------------- */

static struct {
    HWND edit;
    volatile int done;
    int ok;
} InputBox;

static LRESULT CALLBACK input_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT result;

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            InputBox.ok = 1;
            InputBox.done = 1;
        } else if (LOWORD(wp) == IDCANCEL) {
            InputBox.done = 1;
        }
        return 0;
    case WM_NOTIFY:
        if (gui_theme_notify((NMHDR *)lp, &result))
            return result;
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);
    case WM_CLOSE:
        InputBox.done = 1;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

char *input_text_dialog(HWND owner, const wchar_t *title, const wchar_t *prompt)
{
    static int registered = 0;
    HWND wnd;
    char *text = NULL;

    if (!registered) {
        register_window_class(L"CGraderInput", input_proc);
        registered = 1;
    }
    memset(&InputBox, 0, sizeof(InputBox));
    wnd = create_dialog_window(L"CGraderInput", title, owner, 640, 520, 0);
    make_control(wnd, L"STATIC", prompt, SS_LEFT, 0, 10, 10, 620, 60, -1);
    InputBox.edit = make_control(wnd, L"EDIT", L"",
                           WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                               ES_WANTRETURN | WS_TABSTOP,
                           WS_EX_CLIENTEDGE, 10, 74, 620, 396, -1);
    SendMessageW(InputBox.edit, EM_SETLIMITTEXT, 0, 0);
    gui_mark_primary(make_control(wnd, L"BUTTON", L"確定", BS_PUSHBUTTON | WS_TABSTOP, 0, 444, 480, 90, 30, IDOK));
    make_control(wnd, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 0, 540, 480, 90, 30, IDCANCEL);
    gui_theme_window(wnd);
    SetFocus(InputBox.edit);

    /* run_modal 結束時會關閉視窗，所以在迴圈結束前 (按下確定的當下) 就要把文字讀出來 */
    EnableWindow(owner, FALSE);
    ShowWindow(wnd, SW_SHOW);
    while (!InputBox.done) {
        MSG m;
        if (GetMessageW(&m, NULL, 0, 0) <= 0) {
            PostQuitMessage(0);
            break;
        }
        if (!IsDialogMessageW(wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (InputBox.ok) {
        int n = GetWindowTextLengthW(InputBox.edit);
        wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
        if (w != NULL) {
            char *p, *q;
            GetWindowTextW(InputBox.edit, w, n + 1);
            text = wide_to_utf8(w);
            free(w);
            if (text != NULL) {
                for (p = q = text; *p != '\0'; p++)
                    if (*p != '\r')
                        *q++ = *p;
                *q = '\0';
            }
        }
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(wnd);
    SetForegroundWindow(owner);
    return text;
}

/* ---------------- 執行腳本 ---------------- */

int run_visible_command(HWND owner, const char *command_line)
{
    wchar_t *wcmd = utf8_to_wide(command_line);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = (DWORD)-1;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (wcmd == NULL || !CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        free(wcmd);
        return -1;
    }
    free(wcmd);

    /* 等待期間繼續處理視窗訊息 (畫面才不會變成「沒有回應」)，但不讓老師操作 */
    EnableWindow(owner, FALSE);
    for (;;) {
        MSG msg;
        DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)
            break;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return (int)exit_code;
}

int tool_script_path(const char *name, char *out, size_t size)
{
    char folder[GRADER_PATH_MAX], tools[GRADER_PATH_MAX];
    app_folder(folder, sizeof(folder));
    path_join(tools, sizeof(tools), folder, "tools");
    path_join(out, size, tools, name);
    return file_exists(out);
}
