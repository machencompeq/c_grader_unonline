#include <stdio.h>
#include <string.h>

#include "exporter.h"
#include "util.h"

/*
 * CSV 欄位含逗號、引號、換行時要用引號包起來。
 * 文字欄位 (資料夾名稱、姓名、備註) 若以 = + - @ 開頭，Excel 會當成公式執行，前面加 ' 避免
 * (' 必須放在引號裡面，否則整列格式會壞掉)。
 */
static void write_field(FILE *f, const char *s)
{
    int formula = s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@';

    if (!formula && strpbrk(s, ",\"\r\n") == NULL) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    if (formula)
        fputc('\'', f);
    for (; *s != '\0'; s++) {
        if (*s == '"')
            fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

int export_csv(const char *path, const Grader *g, const StudentResult *results, int count,
               char *err, size_t err_size)
{
    FILE *f = fopen_utf8(path, "wb");
    int i, j;

    if (f == NULL) {
        snprintf(err, err_size, "無法寫入 CSV：%s (檔案是否正被 Excel 開啟？)", path);
        return 0;
    }

    fputs("\xEF\xBB\xBF", f); /* UTF-8 BOM */
    fputs("StudentID,Name,Folder,CompileStatus,Passed,Failed,Difference,Penalty,FixChars,FixPenalty,Score", f);
    for (j = 0; j < g->tests.count; j++) {
        fputc(',', f);
        write_field(f, g->tests.items[j].name);
    }
    fputs(",Remarks\r\n", f);

    for (i = 0; i < count; i++) {
        const StudentResult *r = &results[i];
        char score[64], penalty[64], fix_penalty[64];
        StrBuf remarks = {0};

        format_score(score, sizeof(score), r->score);
        format_score(penalty, sizeof(penalty), r->total_deduction);
        format_score(fix_penalty, sizeof(fix_penalty), r->fix_penalty);

        /* 學號、姓名來自 名單.csv；資料夾名稱保留，方便對照 */
        write_field(f, r->sid[0] != '\0' ? r->sid : r->id);
        fputc(',', f);
        write_field(f, r->name);
        fputc(',', f);
        write_field(f, r->id);
        fprintf(f, ",%s,%d,%d,", student_status_name(r->status), r->passed, r->failed);
        if (student_ran_tests(r->status))
            fprintf(f, "%ld,%s,", r->total_diff, penalty);
        else
            fprintf(f, "-1,-1,");
        if (r->status == STUDENT_CE_FIXED)
            fprintf(f, "%ld,%s,%s", r->fix_chars, fix_penalty, score);
        else
            fprintf(f, ",,%s", score);

        for (j = 0; j < r->test_count; j++) {
            const TestResult *t = &r->tests[j];
            char test_score[64];
            format_score(test_score, sizeof(test_score), t->score);
            fprintf(f, ",%s", test_score);
            if (t->status != TEST_PASS && t->status != TEST_WRONG && t->status != TEST_NOT_RUN)
                sb_appendf(&remarks, "%s%s:%s", remarks.len > 0 ? " " : "",
                           g->tests.items[j].name, test_status_name(t->status));
            if (t->approximate)
                sb_appendf(&remarks, "%s%s:差異數為近似值", remarks.len > 0 ? " " : "",
                           g->tests.items[j].name);
        }
        if (r->note[0] != '\0')
            sb_appendf(&remarks, "%s%s", remarks.len > 0 ? " " : "", r->note);
        fputc(',', f);
        write_field(f, remarks.data != NULL ? remarks.data : "");
        fputs("\r\n", f);
        sb_free(&remarks);
    }

    fclose(f);
    return 1;
}
