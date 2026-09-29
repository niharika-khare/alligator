#include "alligator.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>

/* page size resolved once in main, used for tier-aware size calculations */
static long page_sz;

/* largest request that fits each tier (request + 16-byte header must fit the slab):
 * small = 16 pages, medium = 1024 pages, large = 32 * 1024 pages. Set in main. */
static size_t sml_max, mid_max, lrg_max;

/* pass / fail counters for the summary */
static int n_pass, n_fail;
static const char *failed[64];

/* ---------- types used across tests ------------------------------------ */

typedef struct { int x, y; double val; } Vec2;

typedef struct {
    char  label[24];
    Vec2  origin;
    Vec2  dims;
    int   id;
    float scale;
} Rect;

typedef struct Node {
    int          data;
    struct Node *next;
} Node;

typedef struct {
    Rect   bounds;        /* nested Rect, which itself nests Vec2 */
    Node  *head;          /* pointer to a separately allocated list */
    int    count;
    char   name[32];
} Canvas;

/* ---------- helpers ----------------------------------------------------- */

static void check (int expr, const char *msg) {
    printf ("  [%s] %s\n", expr ? "ok  " : "FAIL", msg);
    if (expr) n_pass++;
    else { if (n_fail < 64) failed[n_fail] = msg; n_fail++; }
}

/* 1 if the page holding p is still mapped (msync fails with ENOMEM on unmapped memory) */
static int is_mapped (void *p) {
    void *pg = (void *)((uintptr_t)p & ~((uintptr_t)page_sz - 1));
    return msync (pg, (size_t)page_sz, MS_ASYNC) == 0;
}

static void next () {
    printf ("  press enter to continue\n\n");
    getc (stdin);
}

/* fork a child, suppress its stderr, run fn() in it, expect SIGABRT */
static void expect_abort (void (*fn)(), const char *desc) {
    pid_t pid = fork ();
    if (pid == 0) {
        close (STDERR_FILENO);
        fn ();
        _exit (0);
    }
    int st;
    waitpid (pid, &st, 0);
    check (WIFSIGNALED (st) && WTERMSIG (st) == SIGABRT, desc);
}

/* ---------- test cases -------------------------------------------------- */

/* 1. int array — small tier */
static void test_int_array () {
    printf ("--- 1. int array (small tier) ---\n");

    int *arr = malloc (128 * sizeof (int));
    check (arr != NULL, "malloc(512 bytes) non-NULL");

    for (int i = 0; i < 128; i++) arr[i] = i * i;

    int ok = 1;
    for (int i = 0; i < 128; i++) ok &= (arr[i] == i * i);
    check (ok, "128 int values read back correctly");

    free (arr);
    check (1, "free completed");
    next ();
}

/* 2. deeply nested: Canvas has a Rect (which has Vec2s) and a Node list */
static void test_deep_nested () {
    printf ("--- 2. deeply nested struct (Canvas -> Rect -> Vec2, Node*) ---\n");

    Canvas *c = malloc (sizeof (Canvas));
    check (c != NULL, "malloc(sizeof Canvas) non-NULL");

    strncpy (c->name, "main_canvas", sizeof (c->name) - 1);
    c->bounds.id        = 1;
    c->bounds.origin.x  = 5;
    c->bounds.dims.val  = 3.0;
    c->count            = 3;

    /* build a small node list into the Canvas */
    c->head = NULL;
    Node *prev = NULL;
    for (int i = 0; i < 3; i++) {
        Node *n = malloc (sizeof (Node));
        check (n != NULL, "Node allocation succeeded");
        n->data = i * 10;
        n->next = NULL;
        if (!c->head) c->head = n;
        else prev->next = n;
        prev = n;
    }

    check (strcmp (c->name, "main_canvas") == 0, "Canvas name correct");
    check (c->bounds.origin.x == 5,              "nested Rect->Vec2 field correct");
    check (c->bounds.dims.val == 3.0,             "nested Rect->Vec2 double correct");

    int ok = 1, i = 0;
    for (Node *n = c->head; n; n = n->next) ok &= (n->data == i++ * 10);
    check (ok && i == 3, "embedded node list values correct");

    /* free list first, then canvas */
    Node *cur = c->head;
    while (cur) { Node *tmp = cur->next; free (cur); cur = tmp; }
    free (c);
    check (1, "all freed");
    next ();
}

/* 3. many independent small allocations - no overlap */
static void test_independence () {
    printf ("--- 3. 80 independent allocs, verify no overlap ---\n");

    const int N = 80;
    char *ptrs[80];

    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc (48);
        check (ptrs[i] != NULL, "alloc non-NULL");
        memset (ptrs[i], i & 0xFF, 48);
    }

    int ok = 1;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < 48; j++)
            ok &= ((unsigned char)ptrs[i][j] == (unsigned char)(i & 0xFF));
    check (ok, "all 80 blocks retained distinct tag values");

    for (int i = 0; i < N; i++) free (ptrs[i]);
    check (1, "all 80 blocks freed");
    next ();
}

/* 4. coalescing: free adjacent blocks, alloc a block larger than any one piece */
static void test_coalescing () {
    printf ("--- 4. coalescing adjacent free blocks ---\n");

    int *a = malloc (80);
    int *b = malloc (80);
    int *c = malloc (80);
    check (a && b && c, "three 80-byte blocks allocated");

    free (a); free (b); free (c);
    check (1, "all three freed");

    char *big = malloc (200);
    check (big != NULL, "malloc(200) succeeded after coalescing");
    if (big) {
        memset (big, 0xAB, 200);
        int ok = 1;
        for (int i = 0; i < 200; i++) ok &= ((unsigned char)big[i] == 0xAB);
        check (ok, "coalesced block fully writable");
        free (big);
    }
    next ();
}

/* 5. slab reclamation: fill several small slabs, free all, allocate again.
 *    One empty slab per tier is kept (reserved); the other empty slabs are unmapped. */
static void test_slab_reclamation () {
    printf ("--- 5. slab reclamation, reserved slab and reuse ---\n");

    const int N = 48;                     /* ~7 blocks per small slab -> ~7 slabs */
    size_t sz = sml_max / 8;
    char *ptrs[48];
    int ok = 1;
    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc (sz);
        ok &= (ptrs[i] != NULL);
        if (ptrs[i]) memset (ptrs[i], i, sz);
    }
    check (ok, "48 blocks of (small max / 8) allocated across several small slabs");

    for (int i = 0; i < N; i++) free (ptrs[i]);
    check (1, "all freed");

    int mapped = 0;
    for (int i = 0; i < N; i++) mapped += is_mapped (ptrs[i]);
    check (mapped < N,      "surplus empty slabs returned to the OS (munmap)");
    check (mapped > 0,      "an empty slab is kept mapped (reserved) for reuse");
    check (mapped <= 2 * 8, "at most one empty slab retained (plus any slab still holding other data)");

    /* allocator must still work after reclamation */
    Rect *r = malloc (sizeof (Rect));
    check (r != NULL, "alloc after reclamation succeeded");
    if (r) {
        r->id = 99; r->dims.x = 640; r->dims.y = 480;
        check (r->id == 99 && r->dims.x == 640, "post-reclamation block usable");
        free (r);
    }
    next ();
}

/* 6. medium tier: size just above the small slab boundary */
static void test_medium_tier () {
    size_t sz = sml_max + 16;
    printf ("--- 6. medium tier allocation (%zu bytes) ---\n", sz);

    char *buf = malloc (sz);
    check (buf != NULL, "malloc(small max + 16) non-NULL");
    if (!buf) { next (); return; }

    memset (buf, 0x77, sz);
    int ok = 1;
    for (size_t i = 0; i < sz; i += 512) ok &= ((unsigned char)buf[i] == 0x77);
    check (ok, "medium-tier buffer spot-check passed");

    free (buf);
    check (1, "free completed");
    next ();
}

/* 7. large tier: int array > 1 million elements */
static void test_large_tier_int () {
    const int N  = 1 << 20;  /* 1M ints = 4MB: 16 bytes past the medium limit on 4KB pages, so large tier */
    size_t    sz = (size_t)N * sizeof (int);
    printf ("--- 7. large tier int[1M] (%zu bytes) ---\n", sz);

    int *arr = malloc (sz);
    check (arr != NULL, "malloc(4MB int array) non-NULL");
    if (!arr) { next (); return; }

    /* stride write/read — touching every 1024th element to keep runtime sane */
    for (int i = 0; i < N; i += 1024) arr[i] = i;
    int ok = 1;
    for (int i = 0; i < N; i += 1024) ok &= (arr[i] == i);
    check (ok, "strided read-back across 4MB int array correct");

    free (arr);
    check (1, "free completed");
    next ();
}

/* 8. large tier realloc grow */
static void test_large_tier_realloc () {
    size_t sz = (size_t)page_sz * 1500;
    printf ("--- 8. large tier realloc grow (%zu → %zu bytes) ---\n", sz, sz * 2);

    char *buf = malloc (sz);
    check (buf != NULL, "initial large-tier alloc non-NULL");
    if (!buf) { next (); return; }

    memset (buf, 0xAA, sz);

    char *grown = realloc (buf, sz * 2);
    check (grown != NULL, "realloc grow non-NULL");
    if (grown) {
        int ok = 1;
        for (size_t i = 0; i < sz; i += 512) ok &= ((unsigned char)grown[i] == 0xAA);
        check (ok, "original data preserved after large-tier grow");
        free (grown);
    }
    next ();
}

/* 9. realloc: NULL / grow / shrink */
static void test_realloc () {
    printf ("--- 9. realloc: NULL / grow / shrink ---\n");

    /* NULL → behaves like malloc */
    Rect *r = realloc (NULL, sizeof (Rect));
    check (r != NULL, "realloc(NULL, sizeof Rect) non-NULL");
    if (r) { r->id = 42; check (r->id == 42, "returned block writable"); free (r); }

    /* grow: original bytes preserved */
    char *buf = malloc (64);
    check (buf != NULL, "pre-grow alloc non-NULL");
    if (buf) {
        memset (buf, 0x55, 64);
        char *grown = realloc (buf, 512);
        check (grown != NULL, "realloc grow non-NULL");
        if (grown) {
            int ok = 1;
            for (int i = 0; i < 64; i++) ok &= ((unsigned char)grown[i] == 0x55);
            check (ok, "first 64 bytes preserved after grow");

            /* shrink: min(old, new) bytes preserved */
            char *shrunk = realloc (grown, 32);
            check (shrunk != NULL, "realloc shrink non-NULL");
            if (shrunk) {
                int ok2 = 1;
                for (int i = 0; i < 32; i++) ok2 &= ((unsigned char)shrunk[i] == 0x55);
                check (ok2, "first 32 bytes preserved after shrink");
                free (shrunk);
            }
        }
    }
    next ();
}

/* 10. free(NULL) is a no-op */
static void test_free_null () {
    printf ("--- 10. free(NULL) is a no-op ---\n");
    free (NULL);
    check (1, "returned without crash");
    next ();
}

/* 11. error: double free → SIGABRT */
static void do_double_free () {
    int *p = malloc (sizeof (int));
    *p = 1;
    free (p);
    free (p);
}

static void test_error_double_free () {
    printf ("--- 11. error: double free → SIGABRT ---\n");
    expect_abort (do_double_free, "double free triggered SIGABRT");
    next ();
}

/* 12. error: free of stack pointer → SIGABRT */
static void do_free_stack () {
    int x = 42;
    free (&x);
}

static void test_error_free_stack () {
    printf ("--- 12. error: free of stack pointer → SIGABRT ---\n");
    expect_abort (do_free_stack, "free of stack pointer triggered SIGABRT");
    next ();
}

/* 13. error: free of interior pointer (not block start) → SIGABRT */
static void do_free_interior () {
    char *p = malloc (64);
    free (p + 16);   /* offset into payload — magic check should fail */
}

static void test_error_free_interior () {
    printf ("--- 13. error: free of interior pointer → SIGABRT ---\n");
    expect_abort (do_free_interior, "interior pointer free triggered SIGABRT");
    next ();
}

/* 14. calloc: zeroed memory in every tier, including reused (dirty) memory */
static void test_calloc () {
    printf ("--- 14. calloc: zeroed memory ---\n");

    /* dirty a block, free it, calloc the same size: must come back zeroed */
    char *d = malloc (256);
    check (d != NULL, "dirty block allocated");
    if (d) { memset (d, 0xFF, 256); free (d); }

    size_t sizes[] = { 256, sml_max / 2, sml_max + 16, mid_max + 16 };
    const char *msgs[] = { "calloc small (reused memory) zeroed", "calloc small (large block) zeroed",
                           "calloc medium tier zeroed", "calloc large tier zeroed" };
    for (int k = 0; k < 4; k++) {
        unsigned char *p = calloc (1, sizes[k]);
        int ok = (p != NULL);
        for (size_t i = 0; ok && i < sizes[k]; i += (sizes[k] > 4096 ? 509 : 1)) ok &= (p[i] == 0);
        if (p) ok &= (p[sizes[k] - 1] == 0);
        check (ok, msgs[k]);
        free (p);
    }

    Vec2 *v = calloc (32, sizeof (Vec2));
    int ok = (v != NULL);
    for (int i = 0; ok && i < 32; i++) ok &= (v[i].x == 0 && v[i].y == 0 && v[i].val == 0.0);
    check (ok, "calloc(32, sizeof Vec2) all fields zero");
    free (v);

    void *z = calloc (0, 16);
    check (z != NULL, "calloc(0, 16) returns a valid pointer (like malloc(0))");
    free (z);
    next ();
}

/* 15. limits: impossible sizes must fail with NULL, not wrap around */
static void test_limits () {
    printf ("--- 15. limits: impossible sizes return NULL ---\n");

    void *m = malloc (SIZE_MAX);
    check (m == NULL, "malloc(SIZE_MAX) returns NULL");
    if (m) free (m);

    void *c = calloc (SIZE_MAX / 2 + 1, 2);      /* n * size overflows to 0 */
    check (c == NULL, "calloc with n * size overflow returns NULL");
    if (c) free (c);
    next ();
}

/* 16. reserved slab in the medium tier, and dedicated mmap for huge requests */
static void test_reserved_and_dedicated () {
    printf ("--- 16. reserved medium slab, dedicated mmap ---\n");

    /* each block needs its own medium slab; after freeing both, exactly one stays mapped */
    size_t sz = (mid_max / 4) * 3;
    char *a = malloc (sz);
    char *b = malloc (sz);
    check (a && b, "two 3/4-medium-slab blocks allocated (one slab each)");
    if (a && b) {
        a[0] = 1; b[0] = 2;
        free (a); free (b);
        check (is_mapped (a) + is_mapped (b) == 1, "one empty medium slab reserved, the other unmapped");

        char *c = malloc (sz);
        check (c != NULL && is_mapped (c), "next medium alloc reuses the reserved slab");
        free (c);
    }

    /* larger than the large slab: gets its own mapping, returned on free */
    size_t big = lrg_max + page_sz;
    char *h = malloc (big);
    check (h != NULL, "dedicated mmap allocation (> large slab) non-NULL");
    if (h) {
        h[0] = 0x5A; h[big - 1] = 0x5B;
        check (h[0] == 0x5A && h[big - 1] == 0x5B, "first and last byte writable");
        free (h);
        check (!is_mapped (h), "dedicated block unmapped on free");
    }
    next ();
}

/* 17. slab registry: many live slabs at once, every pointer found on free */
static void test_slab_registry () {
    printf ("--- 17. slab registry: 40 live medium slabs ---\n");

    const int N = 40;
    size_t sz = (mid_max / 4) * 3;         /* one medium slab per block */
    char *p[40];
    int ok = 1;
    for (int i = 0; i < N; i++) {
        p[i] = malloc (sz);
        ok &= (p[i] != NULL);
        if (p[i]) { p[i][0] = (char)i; p[i][sz - 1] = (char)i; }
    }
    check (ok, "40 medium slabs live at the same time");

    ok = 1;
    for (int i = 0; i < N; i++) ok &= (p[i] && p[i][0] == (char)i && p[i][sz - 1] == (char)i);
    check (ok, "every block kept its own data");

    /* free in an interleaved order: odd indices, then even */
    for (int i = 1; i < N; i += 2) free (p[i]);
    for (int i = 0; i < N; i += 2) free (p[i]);
    int mapped = 0;
    for (int i = 0; i < N; i++) mapped += is_mapped (p[i]);
    check (mapped <= 1, "all slabs released except at most one reserved");

    char *again = malloc (sz);
    check (again != NULL, "registry slots reused: allocation after release works");
    free (again);
    next ();
}

/* 18. validation branches: every rejection path aborts, valid edge cases don't */
static char *freed_huge;

static void do_free_foreign ()     { char *m = mmap (NULL, page_sz, PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                                     free (m + 64); }
static void do_free_misaligned ()  { char *p = malloc (64); free (p + 8); }
/* medium-tier blocks: earlier tests leave only the empty reserved medium slab, so a, b, c
 * are carved back to back from it and b's neighbours are both allocated (no coalescing).
 * a and c are volatile so the compiler cannot drop those unused allocations. */
static void do_double_free_iso ()  { size_t n = sml_max + 16;
                                     char * volatile a = malloc (n), *b = malloc (n), * volatile c = malloc (n);
                                     free (b); free (b); (void)a; (void)c; }
static void do_double_free_huge () { free (freed_huge); }
static void do_realloc_stack ()    { int x; if (realloc (&x, 64)) {} }
static void do_realloc_freed ()    { size_t n = sml_max + 16;
                                     char * volatile a = malloc (n), *b = malloc (n), * volatile c = malloc (n);
                                     free (b); if (realloc (b, 128)) {} (void)a; (void)c; }

static void test_validation () {
    printf ("--- 18. validation: rejection paths → SIGABRT ---\n");

    expect_abort (do_free_foreign,    "free of memory from another mmap (not registered)");
    expect_abort (do_free_misaligned, "free of a misaligned pointer (p + 8)");
    expect_abort (do_double_free_iso, "double free of a block with allocated neighbours");

    freed_huge = malloc (lrg_max + page_sz);   /* dedicated mmap, removed from registry on free */
    free (freed_huge);
    expect_abort (do_double_free_huge, "double free of a dedicated mmap block (already unmapped)");

    expect_abort (do_realloc_stack,   "realloc of a stack pointer");
    expect_abort (do_realloc_freed,   "realloc of a freed block");

    /* valid edge cases must not abort */
    char *p = malloc (100);
    char *q = realloc (p, 0);
    check (q == NULL, "realloc(p, 0) frees p and returns NULL");

    char *r = malloc (64);
    char *s = realloc (r, 64);
    check (s == r, "realloc to the same size returns the same pointer");
    free (s);

    void *z = malloc (0);
    check (z != NULL && ((uintptr_t)z % 16) == 0, "malloc(0) returns a valid 16-byte aligned pointer");
    free (z);
    next ();
}

/* 19. stress: interleaved malloc / calloc / realloc / free across all tiers.
 *     Every block is tagged at both ends and checked before it is reused or freed.
 *     Also mixes in the edge cases: zero-size requests, realloc to 0, and requests
 *     above the size limit (must return NULL and leave any existing block untouched). */
static unsigned long long rng = 88172645463325252ULL;
static unsigned rnd () { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }

#define STRESS_EDGE 64      /* bytes written and checked at each end of a block */

static void fill_block (unsigned char *p, size_t n, unsigned char tag) {
    size_t k = n < STRESS_EDGE ? n : STRESS_EDGE;
    memset (p, tag, k);
    memset (p + n - k, tag, k);
}

static int check_block (unsigned char *p, size_t n, unsigned char tag) {
    size_t k = n < STRESS_EDGE ? n : STRESS_EDGE;
    for (size_t i = 0; i < k; i++)
        if (p[i] != tag || p[n - 1 - i] != tag) return 0;
    return 1;
}

static void test_stress_interleaved () {
    const int SLOTS = 512, OPS = 200000;
    printf ("--- 19. stress: %d interleaved ops over %d slots ---\n", OPS, SLOTS);

    static unsigned char *p[512];
    static size_t        len[512];
    static unsigned char tag[512];
    long errors = 0, misaligned = 0, failed_allocs = 0, not_zeroed = 0, bad_limit = 0;
    long n_malloc = 0, n_calloc = 0, n_realloc = 0, n_free = 0, n_limit = 0;

    for (int op = 0; op < OPS; op++) {
        int i = rnd () % SLOTS;
        unsigned r = rnd () % 1000;
        size_t sz = r < 700 ? rnd () % 512 :              /* mostly small (includes 0) */
                    r < 950 ? rnd () % sml_max :          /* small tier                */
                    r < 998 ? sml_max + rnd () % sml_max  /* medium tier               */
                            : mid_max + rnd () % mid_max; /* large tier                */
        unsigned kind = rnd () % 100;

        if (!p[i]) {
            if (kind < 2) {                               /* over the limit: must be NULL */
                void *x = kind == 0 ? malloc (SIZE_MAX - rnd () % 64) : calloc (SIZE_MAX / 2 + 1, 2);
                n_limit++;
                if (x) { bad_limit++; free (x); }
                continue;
            }
            if (kind < 50) {                              /* malloc */
                p[i] = malloc (sz); n_malloc++;
            } else {                                      /* calloc: n elements of 16 bytes */
                size_t n = (sz + 15) / 16;
                sz = n * 16;
                p[i] = calloc (n, 16); n_calloc++;
                if (p[i]) {
                    size_t k = sz < STRESS_EDGE ? sz : STRESS_EDGE;
                    for (size_t j = 0; j < k; j++)
                        if (p[i][j] || p[i][sz - 1 - j]) { not_zeroed++; break; }
                }
            }
            if (!p[i]) { failed_allocs++; continue; }
            if ((uintptr_t)p[i] % 16) misaligned++;
            len[i] = sz;
            tag[i] = (unsigned char)rnd ();
            fill_block (p[i], sz, tag[i]);

        } else {
            if (!check_block (p[i], len[i], tag[i])) errors++;

            if (kind < 30) {                              /* realloc (to 0 frees) */
                size_t keep = len[i] < sz ? len[i] : sz;
                unsigned char *q = realloc (p[i], sz); n_realloc++;
                if (sz == 0) { p[i] = NULL; continue; }
                if (!q) { failed_allocs++; continue; }    /* old block is still valid */
                if ((uintptr_t)q % 16) misaligned++;
                for (size_t j = 0; j < keep && j < STRESS_EDGE; j++)
                    if (q[j] != tag[i]) { errors++; break; }
                p[i] = q; len[i] = sz;
                tag[i] = (unsigned char)rnd ();
                fill_block (q, sz, tag[i]);

            } else if (kind < 32) {                       /* realloc over the limit */
                unsigned char *q = realloc (p[i], SIZE_MAX - 8); n_limit++;
                if (q) { bad_limit++; p[i] = q; len[i] = 0; }
                else if (!check_block (p[i], len[i], tag[i])) errors++;

            } else {                                      /* free */
                free (p[i]); p[i] = NULL; n_free++;
            }
        }
    }

    for (int i = 0; i < SLOTS; i++) {
        if (p[i] && !check_block (p[i], len[i], tag[i])) errors++;
        free (p[i]); p[i] = NULL;
    }

    printf ("  ops: malloc %ld, calloc %ld, realloc %ld, free %ld, over-limit %ld\n",
            n_malloc, n_calloc, n_realloc, n_free, n_limit);
    check (failed_allocs == 0, "no valid allocation failed");
    check (misaligned == 0,    "every pointer 16-byte aligned");
    check (not_zeroed == 0,    "calloc memory always zeroed (including reused blocks)");
    check (bad_limit == 0,     "over-limit malloc / calloc / realloc returned NULL, old block untouched");
    check (errors == 0,        "no block's contents were overwritten (both ends checked)");
    next ();
}

/* ---------- main ------------------------------------------------------- */

int main () {
    page_sz = sysconf (_SC_PAGESIZE);
    sml_max = 16   * (size_t)page_sz        - 16;
    mid_max = 1024 * (size_t)page_sz        - 16;
    lrg_max = 32   * 1024 * (size_t)page_sz - 16;

    printf ("\n");
    printf ("╔══════════════════════════════════════════╗\n");
    printf ("║         alligator test driver            ║\n");
    printf ("║         page size : %-5ld bytes          ║\n", page_sz);
    printf ("╚══════════════════════════════════════════╝\n\n");

    test_int_array ();
    test_deep_nested ();
    test_independence ();
    test_coalescing ();
    test_slab_reclamation ();
    test_medium_tier ();
    test_large_tier_int ();
    test_large_tier_realloc ();
    test_realloc ();
    test_free_null ();
    test_error_double_free ();
    test_error_free_stack ();
    test_error_free_interior ();
    test_calloc ();
    test_limits ();
    test_reserved_and_dedicated ();
    test_slab_registry ();
    test_validation ();
    test_stress_interleaved ();

    printf ("=== summary: %d checks, %d passed, %d failed ===\n", n_pass + n_fail, n_pass, n_fail);
    for (int i = 0; i < n_fail && i < 64; i++) printf ("  FAIL: %s\n", failed[i]);
    printf ("=== done ===\n");
    return n_fail ? 1 : 0;
}