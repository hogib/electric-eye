#pragma once
#include <stdbool.h>

// The two read-only commands. `name` NULL means every running instance.

// Live dashboard (top.c). `once` prints one snapshot without the
// interactive screen and exits.
int hellion_top(const char *name, bool once);

// Health check (check.c): 0 healthy, 1 degraded, 2 down.
int hellion_check(const char *name);
