#pragma once

// `hellion bench` (bench.c): samples what eeye, hellion and their child
// processes cost for `seconds` (default 30), one line a second and a
// summary at the end. `name` limits the pipeline figures to one hellion
// instance (NULL: every one running here); `csv_path` keeps every sample.
// Exit 0, or 2 when the CSV file cannot be written.
int hellion_bench(const char *name, int seconds, const char *csv_path);
