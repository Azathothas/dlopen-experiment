/* Does the ICD actually run out of memory, or is VK_ERROR_OUT_OF_HOST_MEMORY
 * standing in for something else? Interpose the allocator family and report
 * every NULL return. Also count dl_iterate_phdr, which Mesa uses to find its
 * own build-id for the pipeline-cache UUID. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stddef.h>

#define VIS __attribute__((visibility("default")))
static int nulls, iters;

VIS void *malloc(size_t n) {
    static void *(*real)(size_t);
    if (!real) real = dlsym(RTLD_NEXT, "malloc");
    void *p = real(n);
    if (!p && n) { fprintf(stderr, "  [alloc] malloc(%zu) -> NULL\n", n); nulls++; }
    return p;
}
VIS void *calloc(size_t a, size_t b) {
    static void *(*real)(size_t, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "calloc");
    void *p = real(a, b);
    if (!p && a && b) { fprintf(stderr, "  [alloc] calloc(%zu,%zu) -> NULL\n", a, b); nulls++; }
    return p;
}
VIS void *realloc(void *q, size_t n) {
    static void *(*real)(void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "realloc");
    void *p = real(q, n);
    if (!p && n) { fprintf(stderr, "  [alloc] realloc(%zu) -> NULL\n", n); nulls++; }
    return p;
}
VIS int posix_memalign(void **out, size_t al, size_t n) {
    static int (*real)(void **, size_t, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "posix_memalign");
    int r = real(out, al, n);
    if (r) { fprintf(stderr, "  [alloc] posix_memalign(%zu,%zu) -> %d\n", al, n, r); nulls++; }
    return r;
}
VIS void *aligned_alloc(size_t al, size_t n) {
    static void *(*real)(size_t, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "aligned_alloc");
    void *p = real(al, n);
    if (!p) { fprintf(stderr, "  [alloc] aligned_alloc(%zu,%zu) -> NULL\n", al, n); nulls++; }
    return p;
}
VIS int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *d) {
    static int (*real)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    if (!real) real = dlsym(RTLD_NEXT, "dl_iterate_phdr");
    iters++;
    return real(cb, d);
}
__attribute__((destructor)) static void report(void) {
    fprintf(stderr, "  [alloc] SUMMARY: %d NULL allocations, %d dl_iterate_phdr calls\n",
            nulls, iters);
}
