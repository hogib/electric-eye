#pragma once
/*
 * C99 stand-ins for the handful of C11/C23 features this codebase used to
 * lean on, so it builds with the compilers old distros ship (GCC 4.7 and
 * up -- Debian 7 -- and any clang) rather than only with -std=c23.
 *
 * Atomics: there is no <stdatomic.h> here. Shared variables are declared
 * as their plain type and only ever touched through the GCC __atomic
 * builtins (__atomic_load_n, __atomic_store_n, __atomic_fetch_add, ...),
 * which GCC has had since 4.7 and clang since forever -- <stdatomic.h>
 * itself only arrived in GCC 4.9. Same memory model, same orderings
 * (__ATOMIC_RELAXED/ACQUIRE/RELEASE/SEQ_CST); the one thing lost is the
 * compiler refusing a plain, non-atomic access, so every such variable
 * says so where it is declared.
 */

// Compile-time assertion. A negative array size is a hard error in every
// C compiler; redeclaring the same extern array (all of type char[1]) any
// number of times is legal, so this works at file scope in a header
// included from many places. msg is unused, but the compiler's diagnostic
// quotes the macro call, so it still shows up next to the error.
#define STATIC_ASSERT(cond, msg)                                               \
  extern char static_assertion_failed[(cond) ? 1 : -1]
