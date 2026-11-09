/*
 * banker.c - FR5: Banker's safety algorithm with strict input validation.
 */
#include "banker.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* tokenizer (keeps line numbers for error messages)                   */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *fp;
    const char *name;
    char line[512];
    char *cur;              /* pointer for strtok_r; NULL once consumed */
    char *save;
    int lineno;
} Tok;

static const char *tok_next(Tok *t, char *err, size_t errlen)
{
    for (;;) {
        char *p = strtok_r(t->cur, " \t\r\n", &t->save);

        t->cur = NULL;
        if (p != NULL)
            return p;
        if (fgets(t->line, sizeof t->line, t->fp) == NULL)
            return NULL;
        t->lineno++;
        if (strchr(t->line, '\n') == NULL && !feof(t->fp)) {
            snprintf(err, errlen, "%s: line %d is too long", t->name,
                     t->lineno);
            return NULL;
        }
        for (char *c = t->line; *c; c++)
            if (*c == '#') {
                *c = '\0';
                break;
            }
        t->cur = t->line;
    }
}

#define TOK_ERR(t, ...) do {                                              \
        char why_[160];                                                   \
        snprintf(why_, sizeof why_, __VA_ARGS__);                         \
        snprintf(err, errlen, "%s: line %d: %s", (t)->name, (t)->lineno,  \
                 why_);                                                   \
        return -1;                                                        \
    } while (0)

static int expect_keyword(Tok *t, const char *kw, char *err, size_t errlen)
{
    const char *tok = tok_next(t, err, errlen);

    if (tok == NULL) {
        if (err[0] == '\0')
            snprintf(err, errlen, "%s: unexpected end of file, expected "
                     "'%s'", t->name, kw);
        return -1;
    }
    if (strcmp(tok, kw) != 0)
        TOK_ERR(t, "expected keyword '%s' but found '%s'", kw, tok);
    return 0;
}

static int read_int(Tok *t, const char *what, long lo, long hi, int *out,
                    char *err, size_t errlen)
{
    const char *tok = tok_next(t, err, errlen);
    long v;
    int rc;

    if (tok == NULL) {
        if (err[0] == '\0')
            snprintf(err, errlen, "%s: unexpected end of file while reading "
                     "%s (matrix too small or section missing)", t->name,
                     what);
        return -1;
    }
    rc = cc_parse_long(tok, LONG_MIN, LONG_MAX, &v);
    if (rc == -1)
        TOK_ERR(t, "'%s' is not an integer (while reading %s)", tok, what);
    if (v < lo || v > hi) {
        if (v < 0)
            TOK_ERR(t, "negative value %ld not allowed (%s)", v, what);
        TOK_ERR(t, "value %ld out of range %ld..%ld (%s)", v, lo, hi, what);
    }
    *out = (int)v;
    return 0;
}

static int read_matrix(Tok *t, const char *kw, int n, int m,
                       int dst[][BK_MAX_RES], char *err, size_t errlen)
{
    if (expect_keyword(t, kw, err, errlen) != 0)
        return -1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < m; j++) {
            char what[64];

            snprintf(what, sizeof what, "%s[P%d][%c]", kw, i, 'A' + j);
            if (read_int(t, what, 0, BK_MAX_VALUE, &dst[i][j], err,
                         errlen) != 0)
                return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* parsing and validation                                              */
/* ------------------------------------------------------------------ */

void banker_compute_need(BankerState *bs)
{
    for (int i = 0; i < bs->n; i++)
        for (int j = 0; j < bs->m; j++)
            bs->need[i][j] = bs->max[i][j] - bs->alloc[i][j];
}

int banker_validate(const BankerState *bs, char *err, size_t errlen)
{
    if (bs->n < 1 || bs->n > BK_MAX_PROC) {
        snprintf(err, errlen, "number of processes %d out of range (1..%d)",
                 bs->n, BK_MAX_PROC);
        return -1;
    }
    if (bs->m < 1 || bs->m > BK_MAX_RES) {
        snprintf(err, errlen, "number of resource types %d out of range "
                 "(1..%d)", bs->m, BK_MAX_RES);
        return -1;
    }
    for (int j = 0; j < bs->m; j++) {
        int total = bs->available[j];

        if (bs->available[j] < 0) {
            snprintf(err, errlen, "negative Available for resource %c",
                     'A' + j);
            return -1;
        }
        for (int i = 0; i < bs->n; i++) {
            if (bs->alloc[i][j] < 0 || bs->max[i][j] < 0) {
                snprintf(err, errlen, "negative value for P%d resource %c", i,
                         'A' + j);
                return -1;
            }
            if (bs->alloc[i][j] > bs->max[i][j]) {
                snprintf(err, errlen,
                         "invalid state: P%d holds %d of resource %c but its "
                         "declared Maximum is only %d (Allocation > Maximum)",
                         i, bs->alloc[i][j], 'A' + j, bs->max[i][j]);
                return -1;
            }
            total += bs->alloc[i][j];
        }
        for (int i = 0; i < bs->n; i++) {
            if (bs->max[i][j] > total) {
                snprintf(err, errlen,
                         "invalid state: P%d declares Maximum %d of resource "
                         "%c but only %d instances exist in total "
                         "(Available + Allocation)",
                         i, bs->max[i][j], 'A' + j, total);
                return -1;
            }
        }
    }
    return 0;
}

int banker_parse(BankerState *bs, FILE *fp, const char *name, char *err,
                 size_t errlen)
{
    Tok t = { .fp = fp, .name = name, .cur = NULL, .save = NULL,
              .lineno = 0 };
    BankerState tmp;

    /* Start from an empty line so the first strtok_r() call is well-defined
     * (a NULL string with a NULL save pointer would be undefined). */
    t.line[0] = '\0';
    t.cur = t.line;
    memset(&tmp, 0, sizeof tmp);
    err[0] = '\0';

    if (expect_keyword(&t, "processes", err, errlen) != 0 ||
        read_int(&t, "process count", 1, BK_MAX_PROC, &tmp.n, err,
                 errlen) != 0 ||
        expect_keyword(&t, "resources", err, errlen) != 0 ||
        read_int(&t, "resource count", 1, BK_MAX_RES, &tmp.m, err,
                 errlen) != 0 ||
        expect_keyword(&t, "available", err, errlen) != 0)
        return -1;
    for (int j = 0; j < tmp.m; j++) {
        char what[32];

        snprintf(what, sizeof what, "available[%c]", 'A' + j);
        if (read_int(&t, what, 0, BK_MAX_VALUE, &tmp.available[j], err,
                     errlen) != 0)
            return -1;
    }
    if (read_matrix(&t, "max", tmp.n, tmp.m, tmp.max, err, errlen) != 0 ||
        read_matrix(&t, "allocation", tmp.n, tmp.m, tmp.alloc, err,
                    errlen) != 0)
        return -1;

    if (tok_next(&t, err, errlen) != NULL) {
        TOK_ERR(&t, "unexpected extra data after the allocation matrix "
                "(more numbers than %d x %d?)", tmp.n, tmp.m);
    }
    if (err[0] != '\0')                 /* tokenizer reported an error */
        return -1;

    if (banker_validate(&tmp, err, errlen) != 0)
        return -1;
    banker_compute_need(&tmp);          /* Need = Max - Allocation */
    *bs = tmp;
    return 0;
}

int banker_load_file(BankerState *bs, const char *path, char *err,
                     size_t errlen)
{
    FILE *fp = fopen(path, "r");
    int rc;

    if (fp == NULL) {
        snprintf(err, errlen, "cannot open '%s': %s", path, strerror(errno));
        return -1;
    }
    rc = banker_parse(bs, fp, path, err, errlen);
    if (rc == 0 && ferror(fp)) {
        snprintf(err, errlen, "read error on '%s'", path);
        rc = -1;
    }
    fclose(fp);
    return rc;
}

/* ------------------------------------------------------------------ */
/* built-in demonstration states                                       */
/* ------------------------------------------------------------------ */

void banker_load_demo_safe(BankerState *bs)
{
    /* Classic 5-process / 3-resource textbook state. */
    static const int avail[3] = { 3, 3, 2 };
    static const int mx[5][3] = { {7,5,3}, {3,2,2}, {9,0,2}, {2,2,2}, {4,3,3} };
    static const int al[5][3] = { {0,1,0}, {2,0,0}, {3,0,2}, {2,1,1}, {0,0,2} };

    memset(bs, 0, sizeof *bs);
    bs->n = 5;
    bs->m = 3;
    memcpy(bs->available, avail, sizeof avail);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 3; j++) {
            bs->max[i][j] = mx[i][j];
            bs->alloc[i][j] = al[i][j];
        }
    banker_compute_need(bs);
}

void banker_load_demo_unsafe(BankerState *bs)
{
    /* 3 processes / 2 resources. P0 can finish, but afterwards neither P1
     * nor P2 can ever obtain its remaining need: no safe sequence. */
    static const int avail[2] = { 1, 1 };
    static const int mx[3][2] = { {3,2}, {7,5}, {9,6} };
    static const int al[3][2] = { {2,1}, {3,2}, {4,2} };

    memset(bs, 0, sizeof *bs);
    bs->n = 3;
    bs->m = 2;
    memcpy(bs->available, avail, sizeof avail);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 2; j++) {
            bs->max[i][j] = mx[i][j];
            bs->alloc[i][j] = al[i][j];
        }
    banker_compute_need(bs);
}

/* ------------------------------------------------------------------ */
/* the safety algorithm                                                */
/* ------------------------------------------------------------------ */

static void print_vec(const int *v, int m)
{
    putchar('[');
    for (int j = 0; j < m; j++)
        printf("%s%d", j ? " " : "", v[j]);
    putchar(']');
}

static bool vec_le(const int *a, const int *b, int m)
{
    for (int j = 0; j < m; j++)
        if (a[j] > b[j])
            return false;
    return true;
}

void banker_check_safety(const BankerState *bs, BankerResult *res, bool trace)
{
    int work[BK_MAX_RES];
    int step = 0;

    memset(res, 0, sizeof *res);
    memcpy(work, bs->available, sizeof work);       /* Work = Available */
    if (trace) {
        printf("Safety algorithm: Work = Available = ");
        print_vec(work, bs->m);
        puts("\nScan processes in index order; pick the first unfinished Pi "
             "with Need[i] <= Work;\nlet it finish (Work = Work + "
             "Allocation[i]); restart the scan.");
    }
    for (;;) {
        int pick = -1;

        for (int i = 0; i < bs->n && pick < 0; i++)
            if (!res->finished[i] && vec_le(bs->need[i], work, bs->m))
                pick = i;
        if (pick < 0)
            break;
        step++;
        if (trace) {
            printf("  Step %d: P%d  Need ", step, pick);
            print_vec(bs->need[pick], bs->m);
            printf(" <= Work ");
            print_vec(work, bs->m);
            printf("  -> P%d can finish\n", pick);
        }
        for (int j = 0; j < bs->m; j++)
            work[j] += bs->alloc[pick][j];
        res->finished[pick] = true;
        res->sequence[res->seq_len++] = pick;
        if (trace) {
            printf("          releases Allocation ");
            print_vec(bs->alloc[pick], bs->m);
            printf("  => Work = ");
            print_vec(work, bs->m);
            putchar('\n');
        }
    }
    res->safe = (res->seq_len == bs->n);

    if (!res->safe && trace) {
        puts("  No remaining process has Need <= Work:");
        for (int i = 0; i < bs->n; i++) {
            if (res->finished[i])
                continue;
            printf("    P%d needs ", i);
            print_vec(bs->need[i], bs->m);
            printf(" but Work is ");
            print_vec(work, bs->m);
            printf(" -> short of");
            for (int j = 0; j < bs->m; j++)
                if (bs->need[i][j] > work[j])
                    printf(" %c by %d", 'A' + j, bs->need[i][j] - work[j]);
            putchar('\n');
        }
    }
}

bool banker_verify_sequence(const BankerState *bs, const int *seq, int len)
{
    int work[BK_MAX_RES];
    bool used[BK_MAX_PROC] = { false };

    if (len != bs->n)
        return false;
    memcpy(work, bs->available, sizeof work);
    for (int k = 0; k < len; k++) {
        int p = seq[k];

        if (p < 0 || p >= bs->n || used[p])
            return false;
        used[p] = true;
        for (int j = 0; j < bs->m; j++) {
            /* recompute Need here instead of trusting bs->need */
            if (bs->max[p][j] - bs->alloc[p][j] > work[j])
                return false;
        }
        for (int j = 0; j < bs->m; j++)
            work[j] += bs->alloc[p][j];
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* reporting                                                           */
/* ------------------------------------------------------------------ */

void banker_print_state(const BankerState *bs)
{
    int total[BK_MAX_RES];

    printf("Processes: %d   Resource types: %d (", bs->n, bs->m);
    for (int j = 0; j < bs->m; j++)
        printf("%s%c", j ? " " : "", 'A' + j);
    puts(")");
    for (int j = 0; j < bs->m; j++) {
        total[j] = bs->available[j];
        for (int i = 0; i < bs->n; i++)
            total[j] += bs->alloc[i][j];
    }
    printf("Total instances (Available + sum of Allocation): ");
    for (int j = 0; j < bs->m; j++)
        printf("%c=%d ", 'A' + j, total[j]);
    puts("\nNeed = Maximum - Allocation (computed, not entered)\n");

    printf("  %-5s |", "");
    for (int g = 0; g < 3; g++) {
        for (int j = 0; j < bs->m; j++)
            printf(" %-3c", 'A' + j);
        printf(" |");
    }
    printf("\n  %-5s | %-*s| %-*s| %-*s|\n", "", bs->m * 4, "Allocation",
           bs->m * 4, "Maximum", bs->m * 4, "Need");
    for (int i = 0; i < bs->n; i++) {
        const int (*mat[3])[BK_MAX_RES] = { bs->alloc, bs->max, bs->need };

        printf("  P%-4d |", i);
        for (int g = 0; g < 3; g++) {
            for (int j = 0; j < bs->m; j++)
                printf(" %-3d", mat[g][i][j]);
            printf(" |");
        }
        putchar('\n');
    }
    printf("  Available: ");
    for (int j = 0; j < bs->m; j++)
        printf("%c=%d ", 'A' + j, bs->available[j]);
    putchar('\n');
}

int banker_run(const BankerState *bs)
{
    BankerResult res;

    banker_print_state(bs);
    putchar('\n');
    banker_check_safety(bs, &res, true);
    putchar('\n');

    if (res.safe) {
        printf("RESULT: SAFE\nSafe sequence: <");
        for (int k = 0; k < res.seq_len; k++)
            printf("%sP%d", k ? ", " : "", res.sequence[k]);
        puts(">");
        if (!banker_verify_sequence(bs, res.sequence, res.seq_len)) {
            cc_errf("internal error: computed sequence failed independent "
                    "re-verification");
            return -1;
        }
        puts("Independent re-verification of the sequence: PASSED");
        puts("Meaning: the processes can all run to completion one after "
             "another, so granting\nthe current allocation cannot lead to "
             "deadlock if they are scheduled in this order.");
    } else {
        printf("RESULT: UNSAFE\nNo safe sequence exists. Processes that "
               "can finish before the system gets stuck: ");
        if (res.seq_len == 0)
            printf("none");
        for (int k = 0; k < res.seq_len; k++)
            printf("%sP%d", k ? ", " : "", res.sequence[k]);
        puts("");
        puts("Meaning: the state is not guaranteed deadlock-free. The "
             "remaining processes could each\nhold resources while waiting "
             "for others, so a resource manager must not enter this state.");
    }
    return 0;
}
