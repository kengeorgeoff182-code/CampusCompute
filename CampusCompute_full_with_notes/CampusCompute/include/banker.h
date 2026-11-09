/*
 * banker.h - FR5: Banker's safety algorithm.
 *
 * Input file format (whitespace separated, '#' starts a comment):
 *
 *     processes 5
 *     resources 3
 *     available 3 3 2
 *     max            # n rows of m integers
 *       7 5 3
 *       ...
 *     allocation     # n rows of m integers
 *       0 1 0
 *       ...
 *
 * NEED IS NEVER INPUT: Need[i][j] = Max[i][j] - Allocation[i][j].
 *
 * A configuration is rejected when it is malformed, has negative or
 * out-of-range numbers, when Allocation > Max for any cell, or when some
 * process declares a Max larger than the total number of instances that exist
 * (Available + sum of Allocation) -- such a process could never finish.
 */
#ifndef CC_BANKER_H
#define CC_BANKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define BK_MAX_PROC  10
#define BK_MAX_RES   10
#define BK_MAX_VALUE 10000

typedef struct {
    int n;                                  /* processes      */
    int m;                                  /* resource types */
    int available[BK_MAX_RES];
    int max[BK_MAX_PROC][BK_MAX_RES];
    int alloc[BK_MAX_PROC][BK_MAX_RES];
    int need[BK_MAX_PROC][BK_MAX_RES];      /* computed, never read from input */
} BankerState;

typedef struct {
    bool safe;
    int sequence[BK_MAX_PROC];              /* valid when safe */
    int seq_len;
    bool finished[BK_MAX_PROC];             /* which processes could finish */
} BankerResult;

/* Parse + validate from a file / stream; computes Need. 0 ok, -1 + err. */
int banker_load_file(BankerState *bs, const char *path, char *err,
                     size_t errlen);
int banker_parse(BankerState *bs, FILE *fp, const char *name, char *err,
                 size_t errlen);

/* Need = Max - Allocation. */
void banker_compute_need(BankerState *bs);

/* Semantic validation (Allocation <= Max, Max <= total instances). */
int banker_validate(const BankerState *bs, char *err, size_t errlen);

/* Built-in demonstration states (the program computes the verdicts). */
void banker_load_demo_safe(BankerState *bs);
void banker_load_demo_unsafe(BankerState *bs);

/*
 * The safety algorithm. Repeatedly scans processes in index order for an
 * unfinished one whose Need <= Work, "runs" it (Work += Allocation) and
 * restarts the scan, until none is left or none can proceed.
 * If 'trace' is set each step is printed.
 */
void banker_check_safety(const BankerState *bs, BankerResult *res,
                         bool trace);

/*
 * Independent re-simulation of a claimed safe sequence (separate code path
 * from banker_check_safety). Returns true if it is a valid safe sequence.
 */
bool banker_verify_sequence(const BankerState *bs, const int *seq, int len);

void banker_print_state(const BankerState *bs);

/* Print state, run the algorithm with a trace, explain the verdict.
 * Returns 0 if the analysis completed (safe or unsafe), -1 on internal error. */
int banker_run(const BankerState *bs);

#endif /* CC_BANKER_H */
