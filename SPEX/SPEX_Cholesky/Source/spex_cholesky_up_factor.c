//------------------------------------------------------------------------------
// SPEX_Cholesky/spex_cholesky_up_factor: Up-looking REF Cholesky factorization
//------------------------------------------------------------------------------

// SPEX_Cholesky: (c) 2020-2023, Christopher Lourenco, Jinhao Chen,
// Lorena Mejia Domenzain, Timothy A. Davis, and Erick Moreno-Centeno.
// All Rights Reserved.
// SPDX-License-Identifier: GPL-2.0-or-later or LGPL-3.0-or-later

//------------------------------------------------------------------------------

#define SPEX_FREE_WORKSPACE         \
{                                   \
    HERE2 ("1") ; \
    SPEX_matrix_free(&x, NULL);     \
    HERE2 ("2") ; \
    SPEX_FREE(xi);                  \
    HERE2 ("3") ; \
    SPEX_FREE(h);                   \
    HERE2 ("4") ; \
    SPEX_FREE(c);                   \
    HERE2 ("5") ; \
}

#define SPEX_FREE_ALL               \
{                                   \
    HERE2 ("a") ; \
    SPEX_matrix_free(&L, NULL);     \
    HERE2 ("b") ; \
    SPEX_matrix_free(&rhos, NULL);  \
    HERE2 ("c") ; \
    SPEX_FREE_WORKSPACE             \
    HERE2 ("d") ; \
}

#include "spex_cholesky_internal.h"

/* Purpose: This function performs the up-looking REF Cholesky factorization.
 * In order to compute the L matrix, it performs n iterations of a sparse REF
 * symmetric triangular solve function which, at each iteration, computes the
 * kth row of L.
 *
 * Importantly, this function assumes that A has already been permuted.
 *
 * Input arguments of the function:
 *
 * L_handle:    A handle to the L matrix. Null on input.
 *              On output, contains a pointer to the L matrix.
 *
 * rhos_handle: A handle to the sequence of pivots. NULL on input.
 *              On output it contains a pointer to the pivots matrix.
 *
 * S:           Symbolic analysis struct for Cholesky factorization.
 *              On input it contains information that is not used in this
 *              function such as the row/column permutation
 *              On output it contains the elimination tree and
 *              the number of nonzeros in L.
 *
 * A:           The user's permuted input matrix
 *
 * option:      Command options
 *
 */

SPEX_info spex_cholesky_up_factor
(
    // Output
    SPEX_matrix* L_handle,     // Lower triangular matrix. NULL on input.
    SPEX_matrix* rhos_handle,  // Sequence of pivots. NULL on input.
    // Input
    const SPEX_symbolic_analysis S, // Symbolic analysis struct containing the
                               // elimination tree of A, the column pointers of
                               // L, and the exact number of nonzeros of L.
    const SPEX_matrix A,       // Matrix to be factored
    const SPEX_options option  // command options
)
{
HERE

    //--------------------------------------------------------------------------
    // check inputs
    //--------------------------------------------------------------------------

    SPEX_info info;
    ASSERT (A != NULL);
    ASSERT (A->type == SPEX_MPZ);
    ASSERT (A->kind == SPEX_CSC);
    ASSERT (L_handle != NULL);
    ASSERT (rhos_handle != NULL);
    (*L_handle) = NULL ;
    (*rhos_handle) = NULL ;
HERE

    //--------------------------------------------------------------------------
    // Declare and initialize workspace
    //--------------------------------------------------------------------------

    SPEX_matrix L = NULL ;
    SPEX_matrix rhos = NULL ;
    int64_t *xi = NULL ;
    int64_t *h = NULL ;
    SPEX_matrix x = NULL ;
    int64_t *c = NULL;

    // Declare variables
    int64_t n = A->n, i, j, jnew, k;
    int64_t top = n ;
    int sgn, prev_sgn;
    size_t size;

    c = (int64_t*) SPEX_malloc(n*sizeof(int64_t));
HERE

    // h is the history vector utilized for the sparse REF
    // triangular solve algorithm. h serves as a global
    // vector which is repeatedly passed into the triangular
    // solve algorithm
    h = (int64_t*) SPEX_malloc(n*sizeof(int64_t));
HERE

    // xi serves as a global nonzero pattern vector. It stores
    // the pattern of nonzeros of the kth column of L
    // for the triangular solve.
    xi = (int64_t*) SPEX_malloc(2*n*sizeof(int64_t));
HERE

    if (!h || !xi || !c)
    {
        SPEX_FREE_WORKSPACE;
        return SPEX_OUT_OF_MEMORY;
    }

    // initialize workspace history array
    for (i = 0; i < n; i++)
    {
        h[i] = -1;
    }
HERE

    //--------------------------------------------------------------------------
    // Allocate and initialize the workspace x
    //--------------------------------------------------------------------------

    // SPEX utilizes arbitrary sized integers which can grow beyond the
    // default 64 bits allocated by GMP. If the integers frequently grow, GMP
    // can get bogged down by performing intermediate reallocations. Instead,
    // we utilize a larger estimate on the workspace x vector so that computing
    // the values in L and U do not require too many extra intermediate calls to
    // realloc.
    //
    // The bound given in the paper is that the number of bits is <= n log sigma
    // where sigma is the largest entry in A. Because this bound is extremely
    // pessimistic, instead of using this bound, we use a very rough estimate:
    // 64*max(2, log (n))
    //
    // Note that the estimate presented here is not an upper bound nor a lower
    // bound.  It is still possible that more bits will be required which is
    // correctly handled internally.
    int64_t estimate = 64 * SPEX_MAX (2, ceil (log2 ((double) n)));

    // Create x, a "global" dense mpz_t matrix of dimension n*1 (i.e., it is
    // used as workspace re-used at each iteration). The second boolean
    // parameter is set to false, indicating that the size of each mpz entry
    // will be initialized afterwards (and should not be initialized with the
    // default size)
    SPEX_CHECK (SPEX_matrix_allocate(&x, SPEX_DENSE, SPEX_MPZ, n, 1, n,
        false, /* do not initialize the entries of x: */ false, option));
HERE

    // Create rhos, a "global" dense mpz_t matrix of dimension n*1.
    // As indicated with the second boolean parameter true, the mpz entries in
    // rhos are initialized to the default size (unlike x).
    SPEX_CHECK (SPEX_matrix_allocate(&(rhos), SPEX_DENSE, SPEX_MPZ, n, 1, n,
        false, true, option));
HERE
    printf ("estimate: % " PRId64"\n", estimate) ;
    fflush (stdout) ;

    // initialize the entries of x
    for (i = 0; i < n; i++)
    {
        // Allocate memory for entries of x to be estimate bits
        SPEX_MPZ_INIT2(x->x.mpz[i], estimate);
    }
HERE

    //--------------------------------------------------------------------------
    // Declare memory for L
    //--------------------------------------------------------------------------

    // Since we are performing an up-looking factorization, we allocate
    // L without initializing each entry.
    // Note that, the inidividual (x) values of L are not allocated. Instead,
    // a more efficient method to allocate these values is done inside the
    // factorization to reduce memory usage.

    SPEX_CHECK(SPEX_matrix_allocate(&(L), SPEX_CSC, SPEX_MPZ, n, n, S->lnz,
                                    false, false, option));
HERE

    // Set the column pointers of L
    for (k = 0; k < n; k++)
    {
        L->p[k] = c[k] = S->cp[k];
    }
HERE


    //--------------------------------------------------------------------------
    // Perform the up-looking factorization
    //--------------------------------------------------------------------------

    //--------------------------------------------------------------------------
    // Iterations 0:n-1 (1:n in standard)
    //--------------------------------------------------------------------------
    SPEX_MPZ_SGN(&prev_sgn, x->x.mpz[0]);
HERE

    for (k = 0; k < n; k++)
    {
        // LDx = A(:,k)
HERE
        SPEX_CHECK(spex_cholesky_up_triangular_solve(&top, xi, x, L, A, k,
            S->parent, c, rhos, h));
HERE

        // If x[k] is nonzero choose it as pivot. Otherwise, the matrix is
        // not SPD (indeed, it may even be singular).
        SPEX_MPZ_SGN(&sgn, x->x.mpz[k]);
HERE
        if (sgn != 0)
        {
            SPEX_MPZ_SET(rhos->x.mpz[k], x->x.mpz[k]);
        }
        else
        {
            // A is not symmetric positive definite
HERE
            SPEX_FREE_ALL;
            return SPEX_NOTSPD;
        }
HERE

        //----------------------------------------------------------------------
        // Add the nonzeros (i.e. x) to L
        //----------------------------------------------------------------------
        int64_t p = 0;
        for (j = top; j < n; j++)
        {
HERE
            // Obtain the row index of x[j]
            jnew = xi[j];
            if (jnew == k) continue;

            // Determine the column where x[j] belongs to
            p = c[jnew]++;

            // Place the i index of this nonzero. Should always be k because at
            // iteration k, the up-looking algorithm computes row k of L
            L->i[p] = k;
HERE

            // Find the number of bits of x[j]
            size = mpz_sizeinbase(x->x.mpz[jnew],2);
HERE

            // GMP manual: Allocated size should be size+2
            SPEX_MPZ_INIT2(L->x.mpz[p], size+2);
HERE

            // Place the x value of this nonzero
            SPEX_MPZ_SET(L->x.mpz[p],x->x.mpz[jnew]);
HERE
        }
        // Now, place L(k,k)
        p = c[k]++;
        L->i[p] = k;
HERE
        size = mpz_sizeinbase(x->x.mpz[k], 2);
HERE
        SPEX_MPZ_INIT2(L->x.mpz[p], size+2);
HERE
        SPEX_MPZ_SET(L->x.mpz[p], x->x.mpz[k]);
HERE
    }
    // Finalize L->p
    L->p[n] = S->lnz;
HERE

    //--------------------------------------------------------------------------
    // Free memory and set output
    //--------------------------------------------------------------------------

    (*L_handle) = L;
    (*rhos_handle) = rhos;
    SPEX_FREE_WORKSPACE;
HERE
    return SPEX_OK;
}
