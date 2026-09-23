#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scorer.h"
#include "util.h"

const char *score_mode_name(ScoreMode mode)
{
    switch (mode) {
    case SCORE_PER_CHAR:   return "per_char";
    case SCORE_PER_N_CHAR: return "per_n_char";
    case SCORE_RANGE:      return "range";
    }
    return "range";
}

int score_mode_from_name(const char *name, ScoreMode *mode)
{
    if (strcmp(name, "per_char") == 0)
        *mode = SCORE_PER_CHAR;
    else if (strcmp(name, "per_n_char") == 0)
        *mode = SCORE_PER_N_CHAR;
    else if (strcmp(name, "range") == 0)
        *mode = SCORE_RANGE;
    else
        return 0;
    return 1;
}

static int in_range(const RangeRule *r, long diff)
{
    return diff >= r->min && (r->max == RANGE_MAX || diff <= r->max);
}

double score_penalty(const ScoreRules *rules, long diff)
{
    int i;

    /* 完全相同永遠不扣分：即使區間表忘了寫 0～0 的規則，也不能把全對的學生扣光 */
    if (diff <= 0)
        return 0;

    switch (rules->mode) {
    case SCORE_PER_CHAR:
        return diff * rules->per_char_penalty;

    case SCORE_PER_N_CHAR:
        if (rules->n_chars <= 0)
            return 0;
        return (double)(diff / rules->n_chars) * rules->per_n_penalty; /* 整數除法 = floor */

    case SCORE_RANGE:
        for (i = 0; i < rules->range_count; i++)
            if (in_range(&rules->ranges[i], diff))
                return rules->ranges[i].penalty;
        return SCORE_PENALTY_ALL;
    }
    return 0;
}

double score_apply(double full, double penalty)
{
    double s = full - penalty;
    return s < 0 ? 0 : s;
}

static void format_max(char *out, size_t size, long max)
{
    if (max == RANGE_MAX)
        snprintf(out, size, "MAX");
    else
        snprintf(out, size, "%ld", max);
}

static int compare_rules_by_min(const void *a, const void *b)
{
    const RangeRule *x = a, *y = b;
    return (x->min > y->min) - (x->min < y->min);
}

RulesCheck score_validate(const ScoreRules *rules, char *message, size_t size)
{
    StrBuf msg = {0};
    RulesCheck result = RULES_OK;
    RangeRule sorted[MAX_RANGE_RULES];
    int i, j, n = rules->range_count;
    char a_max[32], b_max[32];

#define REPORT(level, ...)                         \
    do {                                           \
        sb_appendf(&msg, __VA_ARGS__);             \
        sb_append(&msg, "\n");                     \
        if ((level) > result) result = (level);    \
    } while (0)

    switch (rules->mode) {
    case SCORE_PER_CHAR:
        if (rules->per_char_penalty < 0)
            REPORT(RULES_ERROR, "⚠ 每 CHAR 扣分不能是負數");
        break;

    case SCORE_PER_N_CHAR:
        if (rules->n_chars <= 0)
            REPORT(RULES_ERROR, "⚠ N 必須大於 0");
        if (rules->per_n_penalty < 0)
            REPORT(RULES_ERROR, "⚠ 扣分不能是負數");
        break;

    case SCORE_RANGE:
        if (n == 0) {
            REPORT(RULES_ERROR, "⚠ 尚未設定任何評分區間");
            break;
        }
        for (i = 0; i < n; i++) {
            const RangeRule *r = &rules->ranges[i];
            format_max(a_max, sizeof(a_max), r->max);
            if (r->min < 0)
                REPORT(RULES_ERROR, "⚠ 最小值不能是負數 (%ld～%s)", r->min, a_max);
            if (r->max != RANGE_MAX && r->min > r->max)
                REPORT(RULES_ERROR, "⚠ 最小值不能大於最大值 (%ld～%s)", r->min, a_max);
            if (r->penalty < 0)
                REPORT(RULES_ERROR, "⚠ 扣分不能是負數 (%ld～%s)", r->min, a_max);
        }

        /* 重疊檢查：任兩個區間 */
        for (i = 0; i < n; i++) {
            for (j = i + 1; j < n; j++) {
                const RangeRule *a = &rules->ranges[i], *b = &rules->ranges[j];
                int a_end_ok = a->max == RANGE_MAX || a->max >= b->min;
                int b_end_ok = b->max == RANGE_MAX || b->max >= a->min;
                if (a_end_ok && b_end_ok) {
                    format_max(a_max, sizeof(a_max), a->max);
                    format_max(b_max, sizeof(b_max), b->max);
                    REPORT(RULES_ERROR, "⚠ 評分區間重疊：%ld～%s 與 %ld～%s 有重複範圍",
                           a->min, a_max, b->min, b_max);
                }
            }
        }
        if (result == RULES_ERROR)
            break;

        /* 空缺檢查：依最小值排序後看相鄰區間 (差異數 0 一律不扣分，所以從 1 開始檢查) */
        memcpy(sorted, rules->ranges, n * sizeof(RangeRule));
        qsort(sorted, n, sizeof(RangeRule), compare_rules_by_min);
        {
            RulesCheck gap_level = rules->require_contiguous ? RULES_ERROR : RULES_WARNING;
            if (sorted[0].min > 1)
                REPORT(gap_level, "⚠ 區間有空缺：1～%ld 沒有規則 (這些差異數會扣光該題分數)",
                       sorted[0].min - 1);
            for (i = 1; i < n; i++) {
                if (sorted[i].min > sorted[i - 1].max + 1)
                    REPORT(gap_level, "⚠ 區間有空缺：%ld～%ld 沒有規則 (這些差異數會扣光該題分數)",
                           sorted[i - 1].max + 1, sorted[i].min - 1);
            }
            if (sorted[n - 1].max != RANGE_MAX)
                REPORT(gap_level, "⚠ 最後一個區間沒有設定到 MAX：超過 %ld 的差異數會扣光該題分數",
                       sorted[n - 1].max);
        }
        break;
    }
#undef REPORT

    if (message != NULL && size > 0)
        snprintf(message, size, "%s", msg.data != NULL ? msg.data : "");
    sb_free(&msg);
    return result;
}
