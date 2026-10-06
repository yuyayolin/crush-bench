// aurora-bench integration glue for chibicc (added for the GB7 "Clang" workload).
//
// This header is included at the very end of chibicc.h, so it is the last
// thing every chibicc translation unit sees. It changes exactly two things:
//
//  1. Every heap allocation chibicc performs is redirected to a bump-pointer
//     arena owned by gb7_clang.cpp. chibicc is a batch compiler: it allocates
//     freely and never frees anything, relying on the OS to reclaim memory at
//     process exit. The benchmark compiles the same translation unit many
//     times inside one long-lived app process, so it must be able to rewind
//     the allocator between iterations; routing everything through one arena
//     turns that into a single pointer reset.
//     The arena is zero-filled, so calloc() semantics are preserved.
//
//  2. chibicc's fatal error path (error/error_at/error_tok in tokenize.c) calls
//     aurora_cc_on_error() instead of exit(1), so a rejected translation unit
//     is reported as a failed iteration instead of killing the host process.
//
// No algorithm, data structure or code path of chibicc is modified.
#ifndef AURORA_CHIBICC_GLUE_H
#define AURORA_CHIBICC_GLUE_H

#include <stddef.h>

/* Arena allocator, implemented in gb7_clang.cpp. */
void *aurora_cc_calloc(size_t nmemb, size_t size);
void *aurora_cc_realloc(void *ptr, size_t size);
char *aurora_cc_strdup(const char *s);
char *aurora_cc_strndup(const char *s, size_t n);

/* Fatal-error escape hatch, implemented in gb7_clang.cpp. Never returns.
   (__noreturn__ is spelled with underscores on purpose: chibicc.h includes
   <stdnoreturn.h>, whose "noreturn" macro would otherwise be expanded here.) */
void aurora_cc_on_error(void) __attribute__((__noreturn__));

#define calloc(nmemb, size) aurora_cc_calloc(nmemb, size)
#define realloc(ptr, size) aurora_cc_realloc(ptr, size)
#define strdup(s) aurora_cc_strdup(s)
#define strndup(s, n) aurora_cc_strndup(s, n)

#endif
