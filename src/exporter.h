/*
 * exporter.h - 把批改結果匯出成 CSV (UTF-8 含 BOM，Excel 直接開中文不會亂碼)。
 */
#ifndef EXPORTER_H
#define EXPORTER_H

#include <stddef.h>

#include "grader.h"

int export_csv(const char *path, const Grader *g, const StudentResult *results, int count,
               char *err, size_t err_size);

#endif
