#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comparator.h"
#include "config.h"
#include "feedback.h"
#include "report.h"

/* 標示差異位置需要 n x m 的表格；超過這個大小就不標示 (差異數仍照常計算) */
#define HIGHLIGHT_CELL_LIMIT 8000000.0
#define INPUT_SHOW_LIMIT (16 * 1024)

/* ---------------- 小工具 ---------------- */

static void html_escape(StrBuf *sb, const char *s, size_t len)
{
    size_t i, start = 0;
    for (i = 0; i < len; i++) {
        const char *rep = NULL;
        switch (s[i]) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\r': rep = ""; break;
        case '\0': rep = "\xE2\x90\x80"; break; /* ␀ */
        }
        if (rep != NULL) {
            sb_append_len(sb, s + start, i - start);
            sb_append(sb, rep);
            start = i + 1;
        }
    }
    sb_append_len(sb, s + start, len - start);
}

static void html_text(StrBuf *sb, const char *s)
{
    html_escape(sb, s, strlen(s));
}

/* ---------------- 逐字差異標示 ---------------- */

typedef struct {
    uint32_t *ch;
    unsigned char *mark; /* 1 = 這個字元與另一邊不同 */
    size_t len;
} MarkedText;

/* UTF-8 -> 字元陣列，與 comparator 用同一個解碼器 (\r\n 視為 \n、不合法 byte 各算一個字元) */
static void decode(const char *s, size_t n, MarkedText *t)
{
    t->ch = malloc((n + 1) * sizeof(uint32_t));
    t->mark = calloc(n + 1, 1);
    t->len = 0;
    if (t->ch == NULL || t->mark == NULL)
        return;
    t->len = compare_decode_utf8((const unsigned char *)s, n, t->ch);
}

static void marked_free(MarkedText *t)
{
    free(t->ch);
    free(t->mark);
    t->ch = NULL;
    t->mark = NULL;
}

/* ---------------- 差異腳本 (diff-match-patch 風格) ---------------- */

/* 一段差異：0 = 相同、-1 = 只在標準答案 (學生少了/寫錯)、+1 = 只在學生輸出 (多出來/寫錯) */
typedef struct {
    signed char op;
    size_t len;
} DiffOp;

typedef struct {
    DiffOp *items;
    size_t count, cap;
} DiffList;

static void diff_push(DiffList *d, int op, size_t len)
{
    if (len == 0)
        return;
    if (d->count > 0 && d->items[d->count - 1].op == op) {
        d->items[d->count - 1].len += len;
        return;
    }
    if (d->count == d->cap) {
        size_t cap = d->cap > 0 ? d->cap * 2 : 64;
        DiffOp *p = realloc(d->items, cap * sizeof(DiffOp));
        if (p == NULL)
            return;
        d->items = p;
        d->cap = cap;
    }
    d->items[d->count].op = (signed char)op;
    d->items[d->count].len = len;
    d->count++;
}

static void diff_free(DiffList *d)
{
    free(d->items);
    d->items = NULL;
    d->count = d->cap = 0;
}

/* 把「刪、插、刪、插…」交錯的一段整理成「一段刪除 + 一段插入」，相同段合併 (同 diff-match-patch 的 cleanupMerge) */
static void diff_normalize(DiffList *d)
{
    DiffList out = {0};
    size_t i, del = 0, ins = 0;

    for (i = 0; i <= d->count; i++) {
        if (i == d->count || d->items[i].op == 0) {
            diff_push(&out, -1, del);
            diff_push(&out, 1, ins);
            del = ins = 0;
            if (i < d->count)
                diff_push(&out, 0, d->items[i].len);
        } else if (d->items[i].op < 0) {
            del += d->items[i].len;
        } else {
            ins += d->items[i].len;
        }
    }
    diff_free(d);
    *d = out;
}

/*
 * 語意清理 (同 diff-match-patch 的 cleanupSemantic)：夾在兩段修改中間、又比前後修改都短的「相同」小碎片，
 * 併進修改裡；例如 Avg vs Average 原本是「Av + 插 era + g + 插 e」，清理後是「Av + 刪 g + 插 erage」，
 * 老師一眼就看出是整個字不一樣，而不是散落的單字。
 */
static void diff_cleanup_semantic(DiffList *d)
{
    int changed = 1, rounds = 0;

    while (changed && rounds++ < 64) {
        size_t k;
        changed = 0;
        for (k = 1; k + 1 < d->count && !changed; k++) {
            size_t before = 0, after = 0, i, eq_len = d->items[k].len;
            DiffList out = {0};
            if (d->items[k].op != 0 || eq_len > 12)
                continue;
            for (i = k; i-- > 0 && d->items[i].op != 0;)
                if (d->items[i].len > before)
                    before = d->items[i].len;
            for (i = k + 1; i < d->count && d->items[i].op != 0; i++)
                if (d->items[i].len > after)
                    after = d->items[i].len;
            if (before == 0 || after == 0 || eq_len > before || eq_len > after)
                continue;
            /* 這段相同文字改成「刪掉再插入」，normalize 會把它併進前後的修改 */
            for (i = 0; i < d->count; i++) {
                if (i == k) {
                    diff_push(&out, -1, eq_len);
                    diff_push(&out, 1, eq_len);
                } else {
                    diff_push(&out, d->items[i].op, d->items[i].len);
                }
            }
            diff_free(d);
            *d = out;
            diff_normalize(d);
            changed = 1;
        }
    }
}

/*
 * 用編輯距離的回溯算出差異腳本 (相同 / 學生少了 / 學生多了)，並做語意清理。
 * 回傳 0 表示太長沒有算 (差異數仍照常計算)。
 */
static int diff_compute(const MarkedText *a, const MarkedText *b, DiffList *out)
{
    size_t pre = 0, suf = 0, n, m, i, j;
    int32_t *d;
    DiffList rev = {0};

    memset(out, 0, sizeof(*out));
    while (pre < a->len && pre < b->len && a->ch[pre] == b->ch[pre])
        pre++;
    while (suf < a->len - pre && suf < b->len - pre && a->ch[a->len - 1 - suf] == b->ch[b->len - 1 - suf])
        suf++;
    n = a->len - pre - suf;
    m = b->len - pre - suf;

    if (n > 0 && m > 0) {
        if ((double)(n + 1) * (double)(m + 1) > HIGHLIGHT_CELL_LIMIT)
            return 0;
        d = malloc((n + 1) * (m + 1) * sizeof(int32_t));
        if (d == NULL)
            return 0;
#define D(x, y) d[(x) * (m + 1) + (y)]
        for (i = 0; i <= n; i++)
            D(i, 0) = (int32_t)i;
        for (j = 0; j <= m; j++)
            D(0, j) = (int32_t)j;
        for (i = 1; i <= n; i++) {
            for (j = 1; j <= m; j++) {
                int32_t best = D(i - 1, j - 1) + (a->ch[pre + i - 1] != b->ch[pre + j - 1]);
                if (D(i - 1, j) + 1 < best)
                    best = D(i - 1, j) + 1;
                if (D(i, j - 1) + 1 < best)
                    best = D(i, j - 1) + 1;
                D(i, j) = best;
            }
        }
        /* 從右下角回溯 (得到的是反向順序) */
        i = n;
        j = m;
        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && a->ch[pre + i - 1] == b->ch[pre + j - 1] && D(i, j) == D(i - 1, j - 1)) {
                diff_push(&rev, 0, 1);
                i--;
                j--;
            } else if (i > 0 && j > 0 && D(i, j) == D(i - 1, j - 1) + 1) {
                diff_push(&rev, 1, 1); /* 反向：先插後刪，正向就是先刪後插 */
                diff_push(&rev, -1, 1);
                i--;
                j--;
            } else if (i > 0 && D(i, j) == D(i - 1, j) + 1) {
                diff_push(&rev, -1, 1);
                i--;
            } else {
                diff_push(&rev, 1, 1);
                j--;
            }
        }
#undef D
        free(d);
    }

    diff_push(out, 0, pre);
    if (n > 0 && m > 0) {
        for (i = rev.count; i-- > 0;)
            diff_push(out, rev.items[i].op, rev.items[i].len);
    } else {
        diff_push(out, -1, n);
        diff_push(out, 1, m);
    }
    diff_push(out, 0, suf);
    diff_free(&rev);
    diff_normalize(out);
    diff_cleanup_semantic(out);
    return 1;
}

/* 依差異腳本標記兩邊的字元 (給左右對照用) */
static void diff_apply_marks(const DiffList *d, MarkedText *a, MarkedText *b)
{
    size_t k, i = 0, j = 0, t;

    for (k = 0; k < d->count; k++) {
        const DiffOp *op = &d->items[k];
        if (op->op == 0) {
            i += op->len;
            j += op->len;
        } else if (op->op < 0) {
            for (t = 0; t < op->len && i < a->len; t++)
                a->mark[i++] = 1;
        } else {
            for (t = 0; t < op->len && j < b->len; t++)
                b->mark[j++] = 1;
        }
    }
}

static void append_codepoint(StrBuf *sb, uint32_t c)
{
    char buf[8];
    int n;

    if (c >= COMPARE_INVALID_BYTE && c <= COMPARE_INVALID_BYTE + 0xFF)
        c = 0xFFFD; /* 不合法的 byte：顯示成 � */

    if (c < 0x80) {
        buf[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        buf[0] = (char)(0xC0 | (c >> 6));
        buf[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        buf[0] = (char)(0xE0 | (c >> 12));
        buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (c >> 18));
        buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    buf[n] = '\0';
    switch (c) {
    case '&': sb_append(sb, "&amp;"); return;
    case '<': sb_append(sb, "&lt;"); return;
    case '>': sb_append(sb, "&gt;"); return;
    case 0:   sb_append(sb, "\xE2\x90\x80"); return;
    }
    sb_append(sb, buf);
}

/* 輸出一段文字，被標示的字元包在 <mark class=cls>，空白/換行顯示成符號才看得到 */
static void render_marked(StrBuf *sb, const MarkedText *t, const char *cls)
{
    size_t i;
    int open = 0;

    for (i = 0; i < t->len; i++) {
        uint32_t c = t->ch[i];
        if (t->mark[i] && !open) {
            sb_appendf(sb, "<mark class=\"%s\">", cls);
            open = 1;
        } else if (!t->mark[i] && open) {
            sb_append(sb, "</mark>");
            open = 0;
        }
        if (t->mark[i]) {
            if (c == ' ') {
                sb_append(sb, "\xC2\xB7"); /* · */
                continue;
            }
            if (c == '\t') {
                sb_append(sb, "\xE2\x86\x92"); /* → */
                continue;
            }
            if (c == '\n') {
                sb_append(sb, "\xE2\x86\xB5</mark>\n"); /* ↵ */
                open = 0;
                continue;
            }
        }
        append_codepoint(sb, c);
    }
    if (open)
        sb_append(sb, "</mark>");
}

/* 一段被標示的文字：空白/Tab/換行顯示成符號；換行後把標籤關掉再重開，行的結構才看得到 */
static void render_segment(StrBuf *sb, const uint32_t *ch, size_t len, const char *tag)
{
    size_t i;
    int open = 0;

    for (i = 0; i < len; i++) {
        if (!open) {
            sb_appendf(sb, "<%s>", tag);
            open = 1;
        }
        if (ch[i] == ' ')
            sb_append(sb, "\xC2\xB7");
        else if (ch[i] == '\t')
            sb_append(sb, "\xE2\x86\x92");
        else if (ch[i] == '\n') {
            sb_appendf(sb, "\xE2\x86\xB5</%s>\n", tag);
            open = 0;
        } else
            append_codepoint(sb, ch[i]);
    }
    if (open)
        sb_appendf(sb, "</%s>", tag);
}

/* 合併檢視 (diff-match-patch 的 prettyHtml)：一份文字裡同時看到刪除線 (學生少了) 與綠底 (學生多了) */
static void render_inline(StrBuf *sb, const MarkedText *a, const MarkedText *b, const DiffList *d)
{
    size_t k, i = 0, j = 0, t;

    for (k = 0; k < d->count; k++) {
        const DiffOp *op = &d->items[k];
        if (op->op == 0) {
            for (t = 0; t < op->len; t++)
                append_codepoint(sb, a->ch[i + t]);
            i += op->len;
            j += op->len;
        } else if (op->op < 0) {
            render_segment(sb, a->ch + i, op->len, "del");
            i += op->len;
        } else {
            render_segment(sb, b->ch + j, op->len, "ins");
            j += op->len;
        }
    }
}

/*
 * 兩段文字的完整比對呈現：先是合併檢視，再是左右對照 (同一份差異腳本)。
 * a = 標準答案 / 原始碼，b = 學生輸出 / 修正後。b_missing = 1 表示學生沒有輸出。回傳 1 = 有標示。
 */
static int render_diff_views(StrBuf *sb, MarkedText *a, MarkedText *b, const char *label_a, const char *label_b,
                             const char *note_a, const char *note_b, int b_missing, int identical)
{
    DiffList d = {0};
    int marked = 0;

    if (!identical && a->ch != NULL && b->ch != NULL && !b_missing)
        marked = diff_compute(a, b, &d);
    if (marked)
        diff_apply_marks(&d, a, b);

    if (marked && !identical) {
        sb_append(sb, "<details open><summary>合併檢視 (一份文字看完所有差異)</summary><pre class=inline>");
        render_inline(sb, a, b, &d);
        sb_append(sb, "</pre></details>");
    }
    sb_appendf(sb, "<div class=grid><div><b>%s</b><pre>", label_a);
    if (a->ch != NULL)
        render_marked(sb, a, "miss");
    if (note_a != NULL)
        sb_append(sb, note_a);
    sb_appendf(sb, "</pre></div><div><b>%s</b><pre>", label_b);
    if (b_missing)
        sb_append(sb, "(沒有輸出)");
    else if (b->ch != NULL)
        render_marked(sb, b, "extra");
    if (note_b != NULL)
        sb_append(sb, note_b);
    sb_append(sb, "</pre></div></div>");
    if (!marked && !identical && !b_missing)
        sb_append(sb, "<p class=muted>輸出太長，未標示差異位置 (差異數仍已計算)。</p>");
    diff_free(&d);
    return marked;
}

/* ---------------- 設定說明 ---------------- */

void describe_settings(const GradeConfig *cfg, StrBuf *out)
{
    const CompareOptions *c = &cfg->compare;
    const ScoreRules *r = &cfg->rules;
    char num[64], pen[64];
    int i;

    format_score(num, sizeof(num), cfg->full_score);
    sb_appendf(out, "滿分 %s (每個 testcase 等權重)｜比對：%s", num,
               c->mode == COMPARE_STRICT ? "Strict 逐字"
               : c->mode == COMPARE_TOKEN ? "Token" : "Ignore Whitespace");
    if (c->ignore_trailing_space)
        sb_append(out, "、忽略行尾空白與結尾換行");
    if (c->fullwidth_as_halfwidth)
        sb_append(out, "、全形半形視為相同");
    if (c->ignore_case)
        sb_append(out, "、忽略大小寫");
    if (c->ignore_blank_lines)
        sb_append(out, "、忽略空白行");
    if (c->ignore_lines_with[0] != '\0')
        sb_appendf(out, "、忽略含「%s」的行", c->ignore_lines_with);

    sb_append(out, "｜扣分 (每題各自計算，最低 0 分)：");
    switch (r->mode) {
    case SCORE_PER_CHAR:
        sb_appendf(out, "每 1 CHAR 扣 %g 分", r->per_char_penalty);
        break;
    case SCORE_PER_N_CHAR:
        sb_appendf(out, "每 %ld CHAR 扣 %g 分", r->n_chars, r->per_n_penalty);
        break;
    case SCORE_RANGE:
        for (i = 0; i < r->range_count; i++) {
            const RangeRule *x = &r->ranges[i];
            if (x->penalty >= SCORE_PENALTY_ALL)
                snprintf(pen, sizeof(pen), "扣光");
            else
                snprintf(pen, sizeof(pen), "扣%g", x->penalty);
            if (x->max == RANGE_MAX)
                sb_appendf(out, "%s%ld+→%s", i ? "、" : "", x->min, pen);
            else if (x->min == x->max)
                sb_appendf(out, "%s%ld→%s", i ? "、" : "", x->min, pen);
            else
                sb_appendf(out, "%s%ld～%ld→%s", i ? "、" : "", x->min, x->max, pen);
        }
        break;
    }
    if (cfg->runtime_error_zero)
        sb_append(out, "｜程式當掉的題目 0 分");
    if (cfg->ai_fix) {
        sb_appendf(out, "｜編譯失敗：本機 AI 最小修正後繼續批改，每修正 1 CHAR 扣 %g 分、最少扣滿分的 %g%%",
                   cfg->ai_fix_penalty_per_char, cfg->ai_fix_penalty_min_pct);
        if (cfg->ai_fix_penalty_max > 0)
            sb_appendf(out, "、最多扣 %g 分", cfg->ai_fix_penalty_max);
        if (cfg->ai_fix_max_chars > 0)
            sb_appendf(out, "；AI 修改超過 %d 字不採用", cfg->ai_fix_max_chars);
    }
    sb_appendf(out, "｜編譯失敗保底：滿分的 %g%%", cfg->ce_score_floor_pct);
}

/* ---------------- HTML ---------------- */

static const char *STYLE =
    "<style>"
    ":root{color-scheme:light}"
    "body{font-family:'Microsoft JhengHei UI','Noto Sans TC','Segoe UI',sans-serif;margin:0;padding:24px;"
    "background:#f3f5f8;color:#1d2330;line-height:1.55}"
    "main{max-width:1200px;margin:0 auto}"
    "h1{font-size:24px;margin:0 0 6px;letter-spacing:.2px}"
    "h2{font-size:17px;margin:30px 0 10px;padding-bottom:6px;border-bottom:2px solid #d8dde6;color:#2b3446}"
    "h3{font-size:15px;margin:0}"
    ".card{background:#fff;border:1px solid #dfe4ec;border-radius:10px;padding:18px 20px;margin:12px 0;"
    "box-shadow:0 1px 2px rgba(20,30,50,.04)}"
    ".big{font-size:42px;font-weight:700;letter-spacing:-.5px}.muted{color:#5b6475;font-size:13px}"
    ".pill{display:inline-block;padding:1px 9px;border-radius:999px;font-weight:600;font-size:12.5px;"
    "vertical-align:middle;white-space:nowrap}"
    ".AC{background:#dcf5dc;color:#17651f}.WA{background:#fdecc8;color:#8a5300}"
    ".RE,.TLE,.OLE,.CE,.NS{background:#fde0e0;color:#a31515}.CEF{background:#dbe7ff;color:#1a47a3}"
    ".note{background:#fff8db;border:1px solid #f0d77b;border-radius:8px;padding:8px 12px;margin:8px 0}"
    ".fb{background:#eef6ff;border:1px solid #b9d6fb;border-left:4px solid #3b82f6;border-radius:8px;"
    "padding:8px 14px;margin:8px 0}.fb b{color:#1e4f9a}.fb ul{margin:4px 0 2px;padding-left:20px}"
    ".fb li{margin:2px 0}"
    "table{border-collapse:separate;border-spacing:0;width:100%;background:#fff;border:1px solid #dfe4ec;"
    "border-radius:10px;overflow:hidden}"
    "th,td{border-bottom:1px solid #e6eaf0;padding:7px 10px;text-align:left;vertical-align:top;font-size:14px}"
    "th{background:#eef1f6;cursor:pointer;user-select:none;position:sticky;top:0;font-weight:600;"
    "color:#2b3446}td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}"
    "tbody tr:hover td{background:#f6f8fb}"
    "tr.row-ok td{background:#f1fbf1}tr.row-bad td{background:#fff1f1}tr.row-warn td{background:#fffbea}"
    "tr.row-fixed td{background:#eef4ff}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
    "@media(max-width:800px){.grid{grid-template-columns:1fr}}"
    "pre{background:#fbfbfd;border:1px solid #dfe4ec;border-radius:8px;padding:10px 12px;margin:4px 0;"
    "overflow:auto;max-height:480px;font-family:Consolas,'Cascadia Mono','MingLiU',monospace;font-size:13px;"
    "white-space:pre;line-height:1.45}"
    "pre.src{max-height:640px}"
    "mark.miss{background:#ffc9c9;color:#8a0000;text-decoration:line-through}"
    "pre.inline{max-height:560px;line-height:1.6}"
    "pre.inline del{background:#ffd3d3;color:#8a0000;text-decoration:line-through;border-radius:3px;padding:0 1px}"
    "pre.inline ins{background:#c9f2c9;color:#0b5a0b;text-decoration:none;border-radius:3px;padding:0 1px}"
    "mark.extra{background:#c4f0c4;color:#0b5a0b}"
    ".legend mark{padding:0 4px}"
    "pre.code{counter-reset:line}pre.code span{display:block}"
    "pre.code span::before{counter-increment:line;content:counter(line);display:inline-block;width:3em;"
    "margin-right:1em;color:#98a0ae;text-align:right}"
    "details summary{cursor:pointer;color:#2b3446;font-weight:600;margin:6px 0}"
    ".kv{display:flex;flex-wrap:wrap;gap:6px 22px;margin:6px 0 2px;font-size:14px}"
    ".kv b{font-weight:600}"
    "a{color:#1f5fbf}"
    "</style>";

static int write_html(const char *path, const StrBuf *sb, char *err, size_t err_size)
{
    if (!write_file(path, sb->data != NULL ? sb->data : "", sb->len)) {
        snprintf(err, err_size, "無法寫入報告：%s", path);
        return 0;
    }
    return 1;
}

static const char *pill_class(TestStatus s)
{
    return test_status_name(s)[0] == '-' ? "NS" : test_status_name(s);
}

/* 顯示名稱：「張敦品 115403001 (zhang_dun_pin)」；名單沒填就只顯示資料夾名稱 */
static void student_label(StrBuf *sb, const StudentResult *r)
{
    if (r->name[0] != '\0') {
        html_text(sb, r->name);
        if (r->sid[0] != '\0') {
            sb_append(sb, " ");
            html_text(sb, r->sid);
        }
        sb_append(sb, " <span class=muted>(");
        html_text(sb, r->id);
        sb_append(sb, ")</span>");
    } else {
        html_text(sb, r->id);
    }
}

void report_default_dir(const Grader *g, ReportAudience audience, char *out, size_t size)
{
    char base[GRADER_PATH_MAX];
    path_join(base, sizeof(base), g->problem_dir, "reports");
    path_join(out, size, base, audience == REPORT_STUDENT ? "學生版" : "老師版");
}

void report_file_name(const StudentResult *r, char *out, size_t size)
{
    snprintf(out, size, "%s.html", r->id);
}

/* 「哪裡錯了」方塊：text 是 feedback_* 產生的純文字，每行一條 */
static void append_feedback_box(StrBuf *sb, const char *title, const char *text)
{
    const char *line = text;

    if (text == NULL || text[0] == '\0')
        return;
    sb_append(sb, "<div class=fb><b>");
    html_text(sb, title);
    sb_append(sb, "</b><ul>");
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (len > 0) {
            sb_append(sb, "<li>");
            html_escape(sb, line, len);
            sb_append(sb, "</li>");
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    sb_append(sb, "</ul></div>");
}

static void append_compile_feedback(StrBuf *sb, const StudentResult *r)
{
    StrBuf text = {0};
    feedback_compile_errors(r->compile_log, &text);
    append_feedback_box(sb, "編譯錯誤說明 (白話)", text.data);
    sb_free(&text);
}

static void append_source_file(StrBuf *sb, const char *path)
{
    size_t len = 0;
    char *text = read_file(path, &len);
    size_t i, start = 0;

    const char *name = strrchr(path, '\\');
    sb_append(sb, "<h3>");
    html_text(sb, name != NULL ? name + 1 : path);
    sb_append(sb, "</h3>");
    if (text == NULL) {
        sb_append(sb, "<p class=muted>(無法讀取檔案)</p>");
        return;
    }
    text = text_to_utf8(text, &len);
    sb_append(sb, "<pre class=code>");
    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (i == len && start == len)
                break;
            sb_append(sb, "<span>");
            html_escape(sb, text + start, i - start);
            sb_append(sb, "</span>");
            start = i + 1;
        }
    }
    sb_append(sb, "</pre>");
    free(text);
}

/* 編譯失敗的 AI 修正：原始碼 vs 修正後 左右對照 (沿用輸出比對的標示器) */
static void append_fix_section(StrBuf *sb, const StudentResult *r)
{
    char pen[32];
    int i;

    format_score(pen, sizeof(pen), r->fix_penalty);

    sb_append(sb, "<h2>編譯錯誤的 AI 自動修正</h2><div class=card>");
    append_compile_feedback(sb, r);
    if (r->status == STUDENT_CE_FIXED) {
        sb_appendf(sb, "<p>程式無法編譯。批改工具把原始碼與 gcc 錯誤訊息交給本機 AI (%s) 做「最小修改」，"
                       "修正 <b>%ld</b> 個字元%s後可以編譯，<b>修正扣分 %s</b>%s；"
                       "之後用修正後的程式執行所有測資，各題的輸出扣分照常計算。</p>",
                   r->fix_tool, r->fix_chars, r->fix_approximate ? " (近似值)" : "", pen,
                   r->ce_floor_applied ? " (已套用編譯失敗保底分)" : "");
    } else if (r->fix_rejected) {
        sb_appendf(sb, "<p>程式無法編譯。AI 找到的修正改了 <b>%ld</b> 個字，超過上限，可能動到程式邏輯，所以<b>不採用</b>；"
                       "成績為編譯失敗保底分，<b>請老師確認</b>。以下是 AI 的修正，供參考。</p>",
                   r->fix_chars);
    } else {
        sb_appendf(sb, "<p>程式無法編譯。本機 AI (%s) 嘗試修正 %d 次後仍然無法編譯，成績為編譯失敗保底分。"
                       "以下是最後一次的嘗試，供老師參考。</p>",
                   r->fix_tool, r->fix_attempts);
    }
    sb_append(sb, "<details open><summary>原始 gcc 錯誤訊息</summary><pre>");
    html_text(sb, r->compile_log != NULL ? r->compile_log : "");
    sb_append(sb, "</pre></details>");

    sb_append(sb, "<p class='muted legend'>標示說明：<mark class=miss>紅色刪除線</mark> = 原始碼中被 AI 刪除或改掉的字；"
                  "<mark class=extra>綠色</mark> = AI 新增或改成的字。空白顯示為 ·、Tab 為 →、換行為 ↵。</p>");
    for (i = 0; i < r->fixed_file_count && i < r->sources.count; i++) {
        size_t orig_len = 0;
        char *orig;
        MarkedText a, b;
        const char *name = strrchr(r->sources.items[i], '\\');

        if (r->fixed_files[i] == NULL)
            continue;
        orig = read_file(r->sources.items[i], &orig_len);
        if (orig != NULL)
            orig = text_to_utf8(orig, &orig_len);
        if (r->sources.count > 1) {
            sb_append(sb, "<h3>");
            html_text(sb, name != NULL ? name + 1 : r->sources.items[i]);
            sb_append(sb, "</h3>");
        }
        decode(orig != NULL ? orig : "", orig != NULL ? orig_len : 0, &a);
        decode(r->fixed_files[i], strlen(r->fixed_files[i]), &b);
        if (!render_diff_views(sb, &a, &b, "學生原始碼", "AI 修正後", NULL, NULL, 0, 0))
            sb_append(sb, "<p class=muted>程式太長，未標示差異位置。</p>");
        marked_free(&a);
        marked_free(&b);
        free(orig);
    }
    if (r->fix_log != NULL && r->fix_log[0] != '\0') {
        sb_appendf(sb, "<details><summary>%s</summary><pre>",
                   r->status == STUDENT_CE_FIXED ? "修正後程式的 gcc 訊息 (警告)" : "修正後仍然失敗的 gcc 錯誤訊息");
        html_text(sb, r->fix_log);
        sb_append(sb, "</pre></details>");
    }
    sb_append(sb, "</div>");
}

int report_write_student(const char *report_dir, const Grader *g, const StudentResult *r, ReportAudience audience,
                         char *err, size_t err_size)
{
    StrBuf sb = {0};
    char path[GRADER_PATH_MAX], name[300], score[32], full[32], deduction[32];
    int i, ok, for_student = audience == REPORT_STUDENT;

    make_dir(report_dir);
    report_file_name(r, name, sizeof(name));
    path_join(path, sizeof(path), report_dir, name);

    format_score(score, sizeof(score), r->score);
    format_score(full, sizeof(full), g->cfg.full_score);
    format_score(deduction, sizeof(deduction), r->total_deduction);

    sb_append(&sb, "<!doctype html><html lang=zh-Hant><head><meta charset=utf-8>"
                   "<meta name=viewport content='width=device-width,initial-scale=1'><title>");
    html_text(&sb, r->name[0] != '\0' ? r->name : r->id);
    sb_append(&sb, for_student ? " 批改結果</title>" : " 比對報告 (老師版)</title>");
    sb_append(&sb, STYLE);
    sb_append(&sb, "</head><body><main>");
    /* 學生版逐人發放：沒有全班總表連結 */
    if (!for_student)
        sb_append(&sb, "<p><a href=index.html>← 回全班總表</a>　<span class=muted>老師版：含隱藏測資的完整內容</span></p>");

    /* 摘要 */
    sb_append(&sb, "<div class=card><h1>");
    student_label(&sb, r);
    sb_append(&sb, "</h1>");
    sb_appendf(&sb, "<div><span class=big>%s</span> <span class=muted>/ %s 分</span></div>", score, full);
    if (student_ran_tests(r->status)) {
        double output_deduction = 0;
        char out_ded[32], fix_pen[32];
        for (i = 0; i < r->test_count; i++)
            output_deduction += r->tests[i].deduction;
        format_score(out_ded, sizeof(out_ded), output_deduction);
        format_score(fix_pen, sizeof(fix_pen), r->fix_penalty);

        sb_append(&sb, "<div class=kv><span>編譯：");
        if (r->status == STUDENT_CE_FIXED)
            sb_appendf(&sb, "<span class='pill CEF'>CE → AI 最小修正 %ld 字元</span></span><span>修正扣分 <b>%s</b></span>",
                       r->fix_chars, fix_pen);
        else
            sb_append(&sb, "<span class='pill AC'>OK</span></span>");
        sb_appendf(&sb, "<span>通過 <b>%d / %d</b> 題</span><span>總差異數 <b>", r->passed, r->test_count);
        if (r->total_diff >= 0)
            sb_appendf(&sb, "%ld</b> CHAR</span>", r->total_diff);
        else
            sb_append(&sb, "-</b></span>");
        if (r->status == STUDENT_CE_FIXED)
            sb_appendf(&sb, "<span>輸出扣分 <b>%s</b></span><span>總扣分 <b>%s</b> (輸出 + 修正，最多扣到滿分)</span></div>",
                       out_ded, deduction);
        else
            sb_appendf(&sb, "<span>總扣分 <b>%s</b></span></div>", deduction);
    } else if (r->status == STUDENT_COMPILE_ERROR) {
        sb_appendf(&sb, "<p>編譯：<span class='pill CE'>Compile Error 編譯失敗</span>　程式沒有執行，"
                        "成績為編譯失敗保底分 %s 分 (錯誤說明在下方)</p>", score);
    } else {
        sb_append(&sb, "<p><span class='pill NS'>沒有 .c 檔</span>　資料夾裡找不到任何 C 原始碼，0 分</p>");
    }
    if (r->note[0] != '\0') {
        sb_append(&sb, "<div class=note>批改工具自動處理：");
        html_text(&sb, r->note);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "<p class=muted>評分設定：");
    {
        StrBuf settings = {0};
        describe_settings(&g->cfg, &settings);
        html_text(&sb, settings.data);
        sb_free(&settings);
    }
    sb_append(&sb, "</p></div>");

    /* AI 修正對照 (修正成功，或嘗試過但失敗)；沒有 AI 修正的 CE 只給白話說明 */
    if (student_has_fix(r)) {
        append_fix_section(&sb, r);
    } else if (r->status == STUDENT_COMPILE_ERROR) {
        sb_append(&sb, "<h2>哪裡錯了</h2>");
        append_compile_feedback(&sb, r);
    }

    /* 各題摘要 */
    if (student_ran_tests(r->status)) {
        sb_append(&sb, "<h2>各題結果</h2><table><tr><th>Testcase</th><th>結果</th><th class=num>差異數 (CHAR)</th>"
                       "<th class=num>扣分</th><th class=num>得分</th><th class=num>執行時間</th></tr>");
        for (i = 0; i < r->test_count; i++) {
            const TestResult *t = &r->tests[i];
            char s1[32], s2[32], s3[32];
            format_score(s1, sizeof(s1), t->score);
            format_score(s2, sizeof(s2), t->full);
            format_score(s3, sizeof(s3), t->deduction);
            sb_appendf(&sb, "<tr><td><a href='#t%d'>", i);
            html_text(&sb, g->tests.items[i].name);
            sb_append(&sb, "</a>");
            if (g->tests.items[i].hidden)
                sb_append(&sb, " <span class=muted>(隱藏)</span>");
            else if (g->tests.items[i].desc[0] != '\0') {
                sb_append(&sb, " <span class=muted>");
                html_text(&sb, g->tests.items[i].desc);
                sb_append(&sb, "</span>");
            }
            sb_appendf(&sb, "</td><td><span class='pill %s'>%s</span> %s</td>", pill_class(t->status),
                       test_status_name(t->status), test_status_text(t->status));
            if (t->diff >= 0)
                sb_appendf(&sb, "<td class=num>%ld%s</td>", t->diff, t->approximate ? " (近似)" : "");
            else
                sb_append(&sb, "<td class=num>-</td>");
            sb_appendf(&sb, "<td class=num>%s</td><td class=num>%s / %s</td><td class=num>%d ms</td></tr>", s3, s1,
                       s2, t->elapsed_ms);
        }
        sb_append(&sb, "</table>");

        /* 每題詳細 */
        sb_append(&sb, "<h2>逐題比對</h2>"
                       "<p class='muted legend'>標示說明：<mark class=miss>紅色刪除線</mark> = 標準答案有、學生少了或寫錯；"
                       "<mark class=extra>綠色</mark> = 學生多出來或寫錯。空白顯示為 ·、Tab 為 →、換行為 ↵。");
        if (g->cfg.compare.mode != COMPARE_STRICT || g->cfg.compare.ignore_trailing_space ||
            g->cfg.compare.fullwidth_as_halfwidth || g->cfg.compare.ignore_case ||
            g->cfg.compare.ignore_blank_lines || g->cfg.compare.ignore_lines_with[0] != '\0')
            sb_append(&sb, "<br>注意：標示的是「逐字」差異；差異數與扣分已套用比對選項 (例如忽略行尾空白)，"
                           "所以有些標示處並不扣分。");
        sb_append(&sb, "</p>");

        for (i = 0; i < r->test_count; i++) {
            const TestResult *t = &r->tests[i];
            size_t in_len = 0, exp_len = g->expected_len[i];
            char *input = read_file(g->tests.items[i].in_path, &in_len);
            MarkedText e, a;

            if (exp_len > OUTPUT_PREVIEW_LIMIT)
                exp_len = OUTPUT_PREVIEW_LIMIT;

            sb_appendf(&sb, "<div class=card id='t%d'><h3>", i);
            html_text(&sb, g->tests.items[i].name);
            sb_appendf(&sb, "　<span class='pill %s'>%s</span></h3><p>", pill_class(t->status),
                       test_status_name(t->status));
            if (t->diff >= 0)
                sb_appendf(&sb, "差異數 %ld CHAR%s　", t->diff, t->approximate ? " (輸出過長，近似值)" : "");
            {
                char s1[32], s2[32], s3[32];
                format_score(s1, sizeof(s1), t->score);
                format_score(s2, sizeof(s2), t->full);
                format_score(s3, sizeof(s3), t->deduction);
                sb_appendf(&sb, "扣分 %s　得分 %s / %s", s3, s1, s2);
            }
            if (t->status == TEST_RUNTIME_ERROR)
                sb_appendf(&sb, "<br>程式異常結束 (結束碼 0x%08lX)。%s", t->exit_code,
                           g->cfg.runtime_error_zero ? "此題 0 分。" : "當掉前印出的內容仍照常比對。");
            else if (t->status == TEST_TIMEOUT)
                sb_appendf(&sb, "<br>超過 %g 秒被強制結束，此題 0 分。", g->cfg.timeout_ms / 1000.0);
            else if (t->status == TEST_OUTPUT_LIMIT)
                sb_append(&sb, "<br>輸出超過大小限制被強制結束，此題 0 分。");
            sb_append(&sb, "</p>");

            /* 哪裡錯了 (白話)：隱藏測資的學生版只給錯誤類型，不透露內容 */
            if (t->diff > 0 && t->actual != NULL) {
                StrBuf fb = {0};
                feedback_output(g->expected[i], exp_len, t->actual, t->actual_len,
                                g->tests.items[i].hidden && for_student, &fb);
                append_feedback_box(&sb, "哪裡錯了", fb.data);
                sb_free(&fb);
            } else if (t->actual == NULL && (t->status == TEST_WRONG || t->status == TEST_RUNTIME_ERROR)) {
                append_feedback_box(&sb, "哪裡錯了", "程式沒有印出任何東西\n");
            }

            /* 隱藏測資：學生版只顯示結果；老師版照常顯示全部內容 */
            if (g->tests.items[i].hidden) {
                if (for_student) {
                    sb_append(&sb, "<p class=muted>這是隱藏測資，不公開輸入與答案。</p></div>");
                    free(input);
                    continue;
                }
                sb_append(&sb, "<p class=note>隱藏測資 (學生版報告不會顯示以下內容)</p>");
            }
            if (g->tests.items[i].desc[0] != '\0') {
                sb_append(&sb, "<p class=muted>說明：");
                html_text(&sb, g->tests.items[i].desc);
                sb_append(&sb, "</p>");
            }

            sb_append(&sb, "<details><summary>輸入 (stdin)</summary><pre>");
            if (input != NULL)
                html_escape(&sb, input, in_len > INPUT_SHOW_LIMIT ? INPUT_SHOW_LIMIT : in_len);
            sb_append(&sb, "</pre></details>");
            free(input);

            decode(g->expected[i], exp_len, &e);
            decode(t->actual != NULL ? t->actual : "", t->actual != NULL ? t->actual_len : 0, &a);
            render_diff_views(&sb, &e, &a, "標準答案 (Expected)", "學生輸出 (Actual)",
                              g->expected_len[i] > OUTPUT_PREVIEW_LIMIT ? "\n…(太長，只顯示前 64 KB)" : NULL,
                              t->actual_truncated ? "\n…(輸出太長，只顯示前 64 KB)" : NULL, t->actual == NULL,
                              t->diff == 0);
            marked_free(&e);
            marked_free(&a);
            sb_append(&sb, "</div>");
        }
    }

    /* 原始碼與編譯訊息 */
    if (r->sources.count > 0) {
        sb_append(&sb, "<h2>學生原始碼</h2><div class=card>");
        for (i = 0; i < r->sources.count; i++)
            append_source_file(&sb, r->sources.items[i]);
        sb_append(&sb, "</div>");
    }
    if (!student_has_fix(r) && r->compile_log != NULL && r->compile_log[0] != '\0') {
        sb_appendf(&sb, "<h2>%s</h2><pre>", r->status == STUDENT_COMPILE_ERROR ? "編譯錯誤 (gcc)" : "編譯警告 (gcc)");
        html_text(&sb, r->compile_log);
        sb_append(&sb, "</pre>");
    }

    sb_append(&sb, "</main></body></html>");
    ok = write_html(path, &sb, err, err_size);
    sb_free(&sb);
    return ok;
}

int report_write_index(const char *report_dir, const Grader *g, const StudentResult *results, int count,
                       char *err, size_t err_size)
{
    StrBuf sb = {0};
    char path[GRADER_PATH_MAX], full[32], avg[32];
    double total = 0;
    int i, j, ok, all_pass = 0, failed = 0, fixed = 0;

    make_dir(report_dir);
    path_join(path, sizeof(path), report_dir, "index.html");

    for (i = 0; i < count; i++) {
        total += results[i].score;
        if (!student_ran_tests(results[i].status))
            failed++;
        else if (results[i].passed == results[i].test_count)
            all_pass++;
        if (results[i].status == STUDENT_CE_FIXED)
            fixed++;
    }
    format_score(full, sizeof(full), g->cfg.full_score);
    format_score(avg, sizeof(avg), count > 0 ? total / count : 0);

    sb_append(&sb, "<!doctype html><html lang=zh-Hant><head><meta charset=utf-8>"
                   "<meta name=viewport content='width=device-width,initial-scale=1'><title>全班批改總表</title>");
    sb_append(&sb, STYLE);
    sb_append(&sb, "</head><body><main><h1>全班批改總表</h1><p class=muted>題目資料夾：");
    html_text(&sb, g->problem_dir);
    sb_append(&sb, "<br>學生資料夾：");
    html_text(&sb, g->students_dir);
    sb_append(&sb, "</p><div class=card>");
    sb_appendf(&sb, "<div class=kv><span><b>%d</b> 位學生</span><span>平均 <b>%s</b> / %s 分</span>"
                    "<span>全部通過 <b>%d</b> 位</span><span>編譯失敗/沒有檔案 <b>%d</b> 位</span>",
               count, avg, full, all_pass, failed);
    if (fixed > 0)
        sb_appendf(&sb, "<span>AI 修正編譯錯誤 <b>%d</b> 位</span>", fixed);
    sb_append(&sb, "</div><p class=muted>評分設定：");
    {
        StrBuf settings = {0};
        describe_settings(&g->cfg, &settings);
        html_text(&sb, settings.data);
        sb_free(&settings);
    }
    sb_appendf(&sb, "<br>testcase 共 %d 個：", g->tests.count);
    for (j = 0; j < g->tests.count; j++) {
        if (j > 0)
            sb_append(&sb, "、");
        html_text(&sb, g->tests.items[j].name);
    }
    sb_append(&sb, "　(點表頭可排序，點學號看個人比對報告)</p></div>");

    sb_append(&sb, "<table id=list><thead><tr><th>學號</th><th>狀態</th><th class=num>通過</th>"
                   "<th class=num>差異數</th><th class=num>扣分</th><th class=num>成績</th>");
    for (j = 0; j < g->tests.count; j++) {
        sb_append(&sb, "<th class=num>");
        html_text(&sb, g->tests.items[j].name);
        sb_append(&sb, "</th>");
    }
    sb_append(&sb, "<th>備註</th></tr></thead><tbody>");

    for (i = 0; i < count; i++) {
        const StudentResult *r = &results[i];
        char name[300], score[32], deduction[32];
        const char *row_class = "";
        int problem = 0;

        for (j = 0; j < r->test_count; j++)
            if (r->tests[j].status == TEST_RUNTIME_ERROR || r->tests[j].status == TEST_TIMEOUT ||
                r->tests[j].status == TEST_OUTPUT_LIMIT)
                problem = 1;
        if (!student_ran_tests(r->status))
            row_class = "row-bad";
        else if (r->status == STUDENT_CE_FIXED)
            row_class = "row-fixed";
        else if (problem)
            row_class = "row-warn";
        else if (r->passed == r->test_count)
            row_class = "row-ok";

        report_file_name(r, name, sizeof(name));
        format_score(score, sizeof(score), r->score);
        format_score(deduction, sizeof(deduction), r->total_deduction);

        sb_appendf(&sb, "<tr class='%s'><td><a href=\"", row_class);
        html_text(&sb, name);
        sb_append(&sb, "\">");
        student_label(&sb, r);
        sb_append(&sb, "</a></td>");
        if (r->status == STUDENT_OK)
            sb_append(&sb, "<td>OK</td>");
        else if (r->status == STUDENT_CE_FIXED)
            sb_appendf(&sb, "<td><span class='pill CEF'>CE→AI 修正 %ld</span></td>", r->fix_chars);
        else if (r->status == STUDENT_COMPILE_ERROR)
            sb_append(&sb, "<td><span class='pill CE'>編譯失敗</span></td>");
        else
            sb_append(&sb, "<td><span class='pill NS'>沒有 .c</span></td>");
        sb_appendf(&sb, "<td class=num>%d/%d</td>", r->passed, r->test_count);
        if (student_ran_tests(r->status) && r->total_diff >= 0)
            sb_appendf(&sb, "<td class=num>%ld</td><td class=num>%s</td>", r->total_diff, deduction);
        else if (student_ran_tests(r->status))
            sb_appendf(&sb, "<td class=num>-</td><td class=num>%s</td>", deduction);
        else
            sb_append(&sb, "<td class=num>-</td><td class=num>-</td>");
        sb_appendf(&sb, "<td class=num><b>%s</b></td>", score);
        for (j = 0; j < r->test_count; j++) {
            char s[32];
            format_score(s, sizeof(s), r->tests[j].score);
            if (student_ran_tests(r->status) && r->tests[j].status != TEST_PASS && r->tests[j].status != TEST_WRONG)
                sb_appendf(&sb, "<td class=num>%s <span class='pill %s'>%s</span></td>", s,
                           pill_class(r->tests[j].status), test_status_name(r->tests[j].status));
            else
                sb_appendf(&sb, "<td class=num>%s</td>", s);
        }
        sb_append(&sb, "<td>");
        html_text(&sb, r->note);
        sb_append(&sb, "</td></tr>");
    }
    sb_append(&sb, "</tbody></table>");

    /* 點表頭排序 */
    sb_append(&sb,
              "<script>document.querySelectorAll('#list th').forEach(function(th,col){var asc=true;"
              "th.title='點一下排序';th.onclick=function(){var tb=document.querySelector('#list tbody');"
              "var rows=Array.prototype.slice.call(tb.rows);rows.sort(function(a,b){"
              "var x=a.cells[col].innerText.trim(),y=b.cells[col].innerText.trim();"
              "var nx=parseFloat(x),ny=parseFloat(y);var r=(!isNaN(nx)&&!isNaN(ny))?nx-ny:x.localeCompare(y,'zh-Hant');"
              "return asc?r:-r;});asc=!asc;rows.forEach(function(r){tb.appendChild(r);});};});</script>");
    sb_append(&sb, "</main></body></html>");

    ok = write_html(path, &sb, err, err_size);
    sb_free(&sb);
    return ok;
}

char *source_listing_text(const StringList *sources)
{
    StrBuf sb = {0};
    int i;

    for (i = 0; i < sources->count; i++) {
        size_t len = 0, pos = 0;
        char *text = read_file(sources->items[i], &len);
        int line = 1;

        sb_appendf(&sb, "%s==================== %s ====================\n", i > 0 ? "\n" : "", sources->items[i]);
        if (text == NULL) {
            sb_append(&sb, "(無法讀取檔案)\n");
            continue;
        }
        if (!is_valid_utf8(text, len))
            sb_append(&sb, "(此檔案不是 UTF-8，已視為 Big5 轉換顯示)\n");
        text = text_to_utf8(text, &len);
        while (pos < len) {
            size_t end = pos;
            while (end < len && text[end] != '\n')
                end++;
            sb_appendf(&sb, "%4d | ", line++);
            sb_append_len(&sb, text + pos, end > pos && text[end - 1] == '\r' ? end - pos - 1 : end - pos);
            sb_append(&sb, "\n");
            pos = end + 1;
        }
        free(text);
    }
    if (sb.data == NULL)
        sb_append(&sb, "(找不到任何 .c 檔)");
    return sb.data;
}

int report_write_both(const Grader *g, const StudentResult *r, char *err, size_t err_size)
{
    char dir[GRADER_PATH_MAX];
    int ok;

    report_default_dir(g, REPORT_TEACHER, dir, sizeof(dir));
    ok = report_write_student(dir, g, r, REPORT_TEACHER, err, err_size);
    report_default_dir(g, REPORT_STUDENT, dir, sizeof(dir));
    return report_write_student(dir, g, r, REPORT_STUDENT, err, err_size) && ok;
}

int report_write_teacher_index(const Grader *g, const StudentResult *results, int count, char *err, size_t err_size)
{
    char dir[GRADER_PATH_MAX];
    report_default_dir(g, REPORT_TEACHER, dir, sizeof(dir));
    return report_write_index(dir, g, results, count, err, err_size);
}
