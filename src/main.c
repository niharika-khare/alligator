#include "alligator.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* page size resolved once in main, used for tier-aware size calculations */
static long page_sz;

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

    int *arr = mm_alloc (128 * sizeof (int));
    check (arr != NULL, "mm_alloc(512 bytes) non-NULL");

    for (int i = 0; i < 128; i++) arr[i] = i * i;

    int ok = 1;
    for (int i = 0; i < 128; i++) ok &= (arr[i] == i * i);
    check (ok, "128 int values read back correctly");

    mm_free (arr);
    check (1, "mm_free completed");
    next ();
}

/* 2. flat struct */
static void test_struct_vec2 () {
    printf ("--- 2. Vec2 struct allocation ---\n");

    Vec2 *v = mm_alloc (sizeof (Vec2));
    check (v != NULL, "mm_alloc(sizeof Vec2) non-NULL");

    v->x = 10; v->y = -3; v->val = 2.718;

    check (v->x == 10 && v->y == -3, "int fields correct");
    check (v->val > 2.71 && v->val < 2.72, "double field correct");

    mm_free (v);
    check (1, "mm_free completed");
    next ();
}

/* 3. nested struct (Rect contains two Vec2) */
static void test_nested_struct () {
    printf ("--- 3. nested struct (Rect -> Vec2) ---\n");

    Rect *r = mm_alloc (sizeof (Rect));
    check (r != NULL, "mm_alloc(sizeof Rect) non-NULL");

    strncpy (r->label, "viewport", sizeof (r->label) - 1);
    r->origin.x = 0;  r->origin.y = 0;  r->origin.val = 0.0;
    r->dims.x   = 1920; r->dims.y = 1080; r->dims.val = 1.78;
    r->id       = 7;
    r->scale    = 2.5f;

    check (strcmp (r->label, "viewport") == 0,      "label correct");
    check (r->dims.x == 1920 && r->dims.y == 1080,  "nested Vec2 dims correct");
    check (r->id == 7,                               "id correct");
    check (r->scale > 2.4f && r->scale < 2.6f,      "float scale correct");

    mm_free (r);
    check (1, "mm_free completed");
    next ();
}

/* 4. deeply nested: Canvas has a Rect (which has Vec2s) and a Node list */
static void test_deep_nested () {
    printf ("--- 4. deeply nested struct (Canvas -> Rect -> Vec2, Node*) ---\n");

    Canvas *c = mm_alloc (sizeof (Canvas));
    check (c != NULL, "mm_alloc(sizeof Canvas) non-NULL");

    strncpy (c->name, "main_canvas", sizeof (c->name) - 1);
    c->bounds.id        = 1;
    c->bounds.origin.x  = 5;
    c->bounds.dims.val  = 3.0;
    c->count            = 3;

    /* build a small node list into the Canvas */
    c->head = NULL;
    Node *prev = NULL;
    for (int i = 0; i < 3; i++) {
        Node *n = mm_alloc (sizeof (Node));
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
    while (cur) { Node *tmp = cur->next; mm_free (cur); cur = tmp; }
    mm_free (c);
    check (1, "all freed");
    next ();
}

/* 5. array of structs */
static void test_struct_array () {
    printf ("--- 5. array of structs (Vec2[32]) ---\n");

    const int N = 32;
    Vec2 *arr = mm_alloc (N * sizeof (Vec2));
    check (arr != NULL, "mm_alloc(32 * sizeof Vec2) non-NULL");

    for (int i = 0; i < N; i++) { arr[i].x = i; arr[i].y = -i; arr[i].val = i * 0.1; }

    int ok = 1;
    for (int i = 0; i < N; i++)
        ok &= (arr[i].x == i && arr[i].y == -i && arr[i].val > (i*0.1 - 0.001));
    check (ok, "all 32 Vec2 values correct");

    mm_free (arr);
    check (1, "mm_free completed");
    next ();
}

/* 6. many independent small allocations — no overlap */
static void test_independence () {
    printf ("--- 6. 80 independent allocs, verify no overlap ---\n");

    const int N = 80;
    char *ptrs[80];

    for (int i = 0; i < N; i++) {
        ptrs[i] = mm_alloc (48);
        check (ptrs[i] != NULL, "alloc non-NULL");
        memset (ptrs[i], i & 0xFF, 48);
    }

    int ok = 1;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < 48; j++)
            ok &= ((unsigned char)ptrs[i][j] == (unsigned char)(i & 0xFF));
    check (ok, "all 80 blocks retained distinct tag values");

    for (int i = 0; i < N; i++) mm_free (ptrs[i]);
    check (1, "all 80 blocks freed");
    next ();
}

/* 7. coalescing: free adjacent blocks, alloc a block larger than any one piece */
static void test_coalescing () {
    printf ("--- 7. coalescing adjacent free blocks ---\n");

    int *a = mm_alloc (80);
    int *b = mm_alloc (80);
    int *c = mm_alloc (80);
    check (a && b && c, "three 80-byte blocks allocated");

    mm_free (a); mm_free (b); mm_free (c);
    check (1, "all three freed");

    char *big = mm_alloc (200);
    check (big != NULL, "mm_alloc(200) succeeded after coalescing");
    if (big) {
        memset (big, 0xAB, 200);
        int ok = 1;
        for (int i = 0; i < 200; i++) ok &= ((unsigned char)big[i] == 0xAB);
        check (ok, "coalesced block fully writable");
        mm_free (big);
    }
    next ();
}

/* 8. slab reclamation: fill slab, free all, allocate again */
static void test_slab_reclamation () {
    printf ("--- 8. slab reclamation and reuse ---\n");

    const int N = 20;
    Vec2 *ptrs[20];
    for (int i = 0; i < N; i++) {
        ptrs[i] = mm_alloc (sizeof (Vec2));
        check (ptrs[i] != NULL, "Vec2 alloc non-NULL");
        ptrs[i]->x = i; ptrs[i]->y = i * 2;
    }

    for (int i = 0; i < N; i++) mm_free (ptrs[i]);
    check (1, "all freed — slab may have been reclaimed");

    /* allocator must still work after reclamation */
    Rect *r = mm_alloc (sizeof (Rect));
    check (r != NULL, "alloc after reclamation succeeded");
    if (r) {
        r->id = 99; r->dims.x = 640; r->dims.y = 480;
        check (r->id == 99 && r->dims.x == 640, "post-reclamation block usable");
        mm_free (r);
    }
    next ();
}

/* 9. medium tier: size just above the small slab boundary */
static void test_medium_tier () {
    size_t sz = (size_t)page_sz * 2;
    printf ("--- 9. medium tier allocation (%zu bytes) ---\n", sz);

    char *buf = mm_alloc (sz);
    check (buf != NULL, "mm_alloc(page_sz * 2) non-NULL");
    if (!buf) { next (); return; }

    memset (buf, 0x77, sz);
    int ok = 1;
    for (size_t i = 0; i < sz; i += 512) ok &= ((unsigned char)buf[i] == 0x77);
    check (ok, "medium-tier buffer spot-check passed");

    mm_free (buf);
    check (1, "mm_free completed");
    next ();
}

/* 10. medium tier: array of Rects totalling > one small slab */
static void test_medium_tier_structs () {
    size_t sz = (size_t)page_sz * 3;
    printf ("--- 10. medium tier struct array (~%zu bytes of Rect) ---\n", sz);

    int n = (int)(sz / sizeof (Rect));
    Rect *arr = mm_alloc ((size_t)n * sizeof (Rect));
    check (arr != NULL, "large Rect array allocation non-NULL");
    if (!arr) { next (); return; }

    for (int i = 0; i < n; i++) {
        arr[i].id = i;
        arr[i].dims.x = i * 2;
        arr[i].dims.y = i * 3;
    }

    int ok = 1;
    for (int i = 0; i < n; i++)
        ok &= (arr[i].id == i && arr[i].dims.x == i*2 && arr[i].dims.y == i*3);
    check (ok, "all Rect fields correct in medium-tier block");

    mm_free (arr);
    check (1, "mm_free completed");
    next ();
}

/* 11. large tier: single allocation above mid threshold */
static void test_large_tier () {
    size_t sz = (size_t)page_sz * 2048;  /* 8 MB on 4KB pages, well into large tier */
    printf ("--- 11. large tier allocation (%zu bytes) ---\n", sz);

    char *buf = mm_alloc (sz);
    check (buf != NULL, "mm_alloc(page_sz * 2048) non-NULL");
    if (!buf) { next (); return; }

    buf[0]      = 0x11;
    buf[sz / 2] = 0x22;
    buf[sz - 1] = 0x33;
    check (buf[0] == 0x11 && buf[sz/2] == 0x22 && buf[sz-1] == 0x33,
           "first, mid, and last byte of large-tier block writable");

    mm_free (buf);
    check (1, "mm_free completed");
    next ();
}

/* 12. large tier: int array > 1 million elements */
static void test_large_tier_int () {
    const int N  = 1 << 20;  /* 1M ints = 4MB, comfortably in large tier on 4KB pages */
    size_t    sz = (size_t)N * sizeof (int);
    printf ("--- 12. large tier int[1M] (%zu bytes) ---\n", sz);

    int *arr = mm_alloc (sz);
    check (arr != NULL, "mm_alloc(4MB int array) non-NULL");
    if (!arr) { next (); return; }

    /* stride write/read — touching every 1024th element to keep runtime sane */
    for (int i = 0; i < N; i += 1024) arr[i] = i;
    int ok = 1;
    for (int i = 0; i < N; i += 1024) ok &= (arr[i] == i);
    check (ok, "strided read-back across 4MB int array correct");

    mm_free (arr);
    check (1, "mm_free completed");
    next ();
}

/* 13. large tier realloc grow */
static void test_large_tier_realloc () {
    size_t sz = (size_t)page_sz * 1500;
    printf ("--- 13. large tier realloc grow (%zu → %zu bytes) ---\n", sz, sz * 2);

    char *buf = mm_alloc (sz);
    check (buf != NULL, "initial large-tier alloc non-NULL");
    if (!buf) { next (); return; }

    memset (buf, 0xAA, sz);

    char *grown = mm_realloc (buf, sz * 2);
    check (grown != NULL, "mm_realloc grow non-NULL");
    if (grown) {
        int ok = 1;
        for (size_t i = 0; i < sz; i += 512) ok &= ((unsigned char)grown[i] == 0xAA);
        check (ok, "original data preserved after large-tier grow");
        mm_free (grown);
    }
    next ();
}

/* 14. realloc: NULL / grow / shrink */
static void test_realloc () {
    printf ("--- 14. realloc: NULL / grow / shrink ---\n");

    /* NULL → behaves like mm_alloc */
    Rect *r = mm_realloc (NULL, sizeof (Rect));
    check (r != NULL, "mm_realloc(NULL, sizeof Rect) non-NULL");
    if (r) { r->id = 42; check (r->id == 42, "returned block writable"); mm_free (r); }

    /* grow: original bytes preserved */
    char *buf = mm_alloc (64);
    check (buf != NULL, "pre-grow alloc non-NULL");
    if (buf) {
        memset (buf, 0x55, 64);
        char *grown = mm_realloc (buf, 512);
        check (grown != NULL, "mm_realloc grow non-NULL");
        if (grown) {
            int ok = 1;
            for (int i = 0; i < 64; i++) ok &= ((unsigned char)grown[i] == 0x55);
            check (ok, "first 64 bytes preserved after grow");

            /* shrink: min(old, new) bytes preserved */
            char *shrunk = mm_realloc (grown, 32);
            check (shrunk != NULL, "mm_realloc shrink non-NULL");
            if (shrunk) {
                int ok2 = 1;
                for (int i = 0; i < 32; i++) ok2 &= ((unsigned char)shrunk[i] == 0x55);
                check (ok2, "first 32 bytes preserved after shrink");
                mm_free (shrunk);
            }
        }
    }
    next ();
}

/* 15. mm_free(NULL) is a no-op */
static void test_free_null () {
    printf ("--- 15. mm_free(NULL) is a no-op ---\n");
    mm_free (NULL);
    check (1, "returned without crash");
    next ();
}

/* 16. error: double free → SIGABRT */
static void do_double_free () {
    int *p = mm_alloc (sizeof (int));
    *p = 1;
    mm_free (p);
    mm_free (p);
}

static void test_error_double_free () {
    printf ("--- 16. error: double free → SIGABRT ---\n");
    expect_abort (do_double_free, "double free triggered SIGABRT");
    next ();
}

/* 17. error: free of stack pointer → SIGABRT */
static void do_free_stack () {
    int x = 42;
    mm_free (&x);
}

static void test_error_free_stack () {
    printf ("--- 17. error: free of stack pointer → SIGABRT ---\n");
    expect_abort (do_free_stack, "free of stack pointer triggered SIGABRT");
    next ();
}

/* 18. error: free of interior pointer (not block start) → SIGABRT */
static void do_free_interior () {
    char *p = mm_alloc (64);
    mm_free (p + 16);   /* offset into payload — magic check should fail */
}

static void test_error_free_interior () {
    printf ("--- 18. error: free of interior pointer → SIGABRT ---\n");
    expect_abort (do_free_interior, "interior pointer free triggered SIGABRT");
    next ();
}

/* ---------- main ------------------------------------------------------- */

int main () {
    page_sz = sysconf (_SC_PAGESIZE);

    printf ("\n");
    printf ("╔══════════════════════════════════════════╗\n");
    printf ("║         alligator test driver            ║\n");
    printf ("║         page size : %-5ld bytes          ║\n", page_sz);
    printf ("╚══════════════════════════════════════════╝\n\n");

    test_int_array ();
    test_struct_vec2 ();
    test_nested_struct ();
    test_deep_nested ();
    test_struct_array ();
    test_independence ();
    test_coalescing ();
    test_slab_reclamation ();
    test_medium_tier ();
    test_medium_tier_structs ();
    test_large_tier ();
    test_large_tier_int ();
    test_large_tier_realloc ();
    test_realloc ();
    test_free_null ();
    test_error_double_free ();
    test_error_free_stack ();
    test_error_free_interior ();

    printf ("=== done ===\n");
    return 0;
}