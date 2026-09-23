/*
 * report.h - 產生 HTML 比對報告 (用瀏覽器開啟)。
 *
 *   <題目資料夾>/reports/index.html      全班總表 (點學號進入個人報告)
 *   <題目資料夾>/reports/<學號>.html     每位學生一份：
 *        成績摘要、自動處理說明、各題結果、
 *        每題的輸入 / 標準答案 / 學生輸出 (逐字標出差異位置)、
 *        學生原始碼、編譯訊息
 */
#ifndef REPORT_H
#define REPORT_H

#include <stddef.h>

#include "grader.h"
#include "util.h"

/*
 * 兩種報告：
 *   老師版 (reports\老師版\)：全班總表 index.html + 每人一份，隱藏測資也完整顯示
 *   學生版 (reports\學生版\)：每人一份、可直接發給學生；隱藏測資只顯示結果，沒有全班總表
 */
typedef enum { REPORT_TEACHER, REPORT_STUDENT } ReportAudience;

void report_default_dir(const Grader *g, ReportAudience audience, char *out, size_t size);
void report_file_name(const StudentResult *r, char *out, size_t size); /* "<資料夾名稱>.html" */

int report_write_student(const char *report_dir, const Grader *g, const StudentResult *r, ReportAudience audience,
                         char *err, size_t err_size);
/* 一位學生的老師版 + 學生版；全班總表寫在老師版資料夾 */
int report_write_both(const Grader *g, const StudentResult *r, char *err, size_t err_size);
int report_write_teacher_index(const Grader *g, const StudentResult *results, int count, char *err, size_t err_size);

/* 全班總表 (只有老師版) */
int report_write_index(const char *report_dir, const Grader *g, const StudentResult *results, int count,
                       char *err, size_t err_size);

/* 原始碼清單 (含行號，Big5 自動轉換)，給「查看原始碼」用；回傳 malloc 的字串 */
char *source_listing_text(const StringList *sources);

/* 目前設定的文字說明 (報告與畫面共用)，例如「比對：Strict｜忽略行尾空白…」 */
void describe_settings(const GradeConfig *cfg, StrBuf *out);

#endif
