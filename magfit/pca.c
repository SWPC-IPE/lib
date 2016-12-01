/*
 * pca.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <satdata/satdata.h>

#include <gsl/gsl_math.h>
#include <gsl/gsl_sf_legendre.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_test.h>
#include <gsl/gsl_statistics.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_blas.h>

#include "common.h"
#include "interp.h"
#include "pca.h"
#include "track.h"

#include "magfit.h"

/* mu_0 in units of: nT / (kA km^{-1}) */
#define SECS1D_MU_0                  (400.0 * M_PI)

/* relative weightings of different components */
#define PCA_WEIGHT_X                 (5.0)
#define PCA_WEIGHT_Y                 (1.0)
#define PCA_WEIGHT_Z                 (5.0)

/* assign higher weight to low-latitude data for better EEJ fit */
#define PCA_WEIGHT_EEJ               (3.0)

typedef struct
{
  size_t n;         /* total number of measurements in system */
  size_t p;         /* number of coefficients */
  size_t nmax;      /* maximum number of measurements in LS system */

  gsl_matrix *X;    /* LS matrix */
  gsl_vector *rhs;  /* rhs vector */
  gsl_vector *wts;  /* weight vector */
  gsl_matrix *cov;  /* covariance matrix */
  gsl_vector *c;    /* solution vector */

  gsl_multifit_linear_workspace *multifit_p;
  pca_workspace *pca_workspace_p;
} pca_state_t;

static void *pcafit_alloc(const void * params);
static void pcafit_free(void * vstate);
static int pcafit_reset(void * vstate);
static size_t pcafit_add_track(const track_data *tptr, const satdata_mag *data,
                            void * vstate);
static int pcafit_fit(void * vstate);
static int pcafit_eval_B(const double r, const double theta, const double phi,
                      double B[3], void * vstate);
static int pcafit_eval_J(const double r, const double theta, const double phi,
                      double J[3], void * vstate);

static int build_matrix_row(const double r, const double theta, const double phi,
                            gsl_vector *X, gsl_vector *Y, gsl_vector *Z,
                            pca_state_t *state);

/*
pcafit_alloc()
  Allocate pca workspace

Inputs: flags        - MAGFIT_SECS_FLG_xxx
        lmax         - maximum degree for Legendre functions in expansion
        R_iono       - radius of ionosphere (km)
        pole_spacing - along-orbit latitude spacing of SECS poles (degrees)

Return: pointer to workspace
*/

static void *
pcafit_alloc(const void * params)
{
  const magfit_parameters *mparams = (const magfit_parameters *) params;
  pca_state_t *state;

  state = calloc(1, sizeof(pca_state_t));
  if (!state)
    return 0;

  state->nmax = 30000;
  state->n = 0;
  state->p = mparams->pca_modes;

  state->X = gsl_matrix_alloc(state->nmax, state->p);
  state->c = gsl_vector_alloc(state->p);
  state->rhs = gsl_vector_alloc(state->nmax);
  state->wts = gsl_vector_alloc(state->nmax);
  state->cov = gsl_matrix_alloc(state->p, state->p);
  state->multifit_p = gsl_multifit_linear_alloc(state->nmax, state->p);

  state->pca_workspace_p = pca_alloc();

  fprintf(stderr, "pca_alloc: number of modes = %zu\n", state->p);

  return state;
}

static void
pcafit_free(void * vstate)
{
  pca_state_t *state = (pca_state_t *) vstate;

  if (state->X)
    gsl_matrix_free(state->X);

  if (state->c)
    gsl_vector_free(state->c);

  if (state->rhs)
    gsl_vector_free(state->rhs);

  if (state->wts)
    gsl_vector_free(state->wts);

  if (state->cov)
    gsl_matrix_free(state->cov);

  if (state->multifit_p)
    gsl_multifit_linear_free(state->multifit_p);

  if (state->pca_workspace_p)
    pca_free(state->pca_workspace_p);

  free(state);
}

/* reset workspace to work on new data set */
static int
pcafit_reset(void * vstate)
{
  pca_state_t *state = (pca_state_t *) vstate;
  state->n = 0;
  return 0;
}

/*
pcafit_add_track()
  Add satellite data from a single track to LS system

Inputs: tptr   - track pointer
        data   - satellite data
        vstate - state

Return: total data added so far

Notes:
1) state->n is updated with the number of total data added
*/

static size_t
pcafit_add_track(const track_data *tptr, const satdata_mag *data,
              void * vstate)
{
  pca_state_t *state = (pca_state_t *) vstate;
  size_t rowidx = state->n;
  size_t i;

  for (i = 0; i < tptr->n; ++i)
    {
      size_t didx = i + tptr->start_idx;
      double r = data->altitude[didx] + data->R;
      double theta = M_PI / 2.0 - data->latitude[didx] * M_PI / 180.0;
      double phi = data->longitude[didx] * M_PI / 180.0;
      double wi = 1.0;
      gsl_vector_view vx = gsl_matrix_row(state->X, rowidx);
      gsl_vector_view vy = gsl_matrix_row(state->X, rowidx + 1);
      gsl_vector_view vz = gsl_matrix_row(state->X, rowidx + 2);

      if (!SATDATA_AvailableData(data->flags[didx]))
        continue;

      /* fit only low-latitude data */
      if (fabs(data->qdlat[didx]) > MAGFIT_QDMAX)
        continue;

      /* upweight equatorial data */
      if (fabs(data->qdlat[didx]) <= 10.0)
        wi = PCA_WEIGHT_EEJ;

      /* set rhs vector */
      gsl_vector_set(state->rhs, rowidx, tptr->Bx[i]);
      gsl_vector_set(state->rhs, rowidx + 1, tptr->By[i]);
      gsl_vector_set(state->rhs, rowidx + 2, tptr->Bz[i]);

      /* set weight vector */
      gsl_vector_set(state->wts, rowidx, PCA_WEIGHT_X * wi);
      gsl_vector_set(state->wts, rowidx + 1,PCA_WEIGHT_Y *  wi);
      gsl_vector_set(state->wts, rowidx + 2,PCA_WEIGHT_Z *  wi);

      /* build 3 rows of the LS matrix */
      build_matrix_row(r, theta, phi, &vx.vector, &vy.vector, &vz.vector, state);

      rowidx += 3;
    }

  state->n = rowidx;

  return state->n;
}

/*
pcafit_fit()
  Fit model to previously added tracks

Inputs: vstate - state

Return: success/error

Notes:
1) Data must be added to workspace via pca_add_track()
*/

static int
pcafit_fit(void * vstate)
{
  pca_state_t *state = (pca_state_t *) vstate;
  const size_t npts = 200;
  const double tol = 1.0e-6;
  gsl_vector *reg_param = gsl_vector_alloc(npts);
  gsl_vector *rho = gsl_vector_alloc(npts);
  gsl_vector *eta = gsl_vector_alloc(npts);
  gsl_vector *G = gsl_vector_alloc(npts);
  gsl_matrix_view A = gsl_matrix_submatrix(state->X, 0, 0, state->n, state->p);
  gsl_vector_view b = gsl_vector_subvector(state->rhs, 0, state->n);
  gsl_vector_view wts = gsl_vector_subvector(state->wts, 0, state->n);
  double lambda_gcv, lambda_l, G_gcv;
  double rnorm, snorm;
  size_t i;
  const char *lambda_file = "lambda.dat";
  FILE *fp = fopen(lambda_file, "w");
  double s0; /* largest singular value */

  if (state->n < state->p)
    return -1;

  /* convert to standard form */
  gsl_multifit_linear_applyW(&A.matrix, &wts.vector, &b.vector, &A.matrix, &b.vector);

  /* compute SVD of A */
  gsl_multifit_linear_svd(&A.matrix, state->multifit_p);
  s0 = gsl_vector_get(state->multifit_p->S, 0);

  /* compute L-curve */
  gsl_multifit_linear_lcurve(&b.vector, reg_param, rho, eta, state->multifit_p);
  gsl_multifit_linear_lcorner(rho, eta, &i);
  lambda_l = gsl_vector_get(reg_param, i);

  /* compute GCV curve */
  gsl_multifit_linear_gcv(&b.vector, reg_param, G, &lambda_gcv, &G_gcv, state->multifit_p);

  /* the L-curve method often overdamps the system, not sure why */
  lambda_l *= 1.0e-2;
  lambda_l = GSL_MAX(lambda_l, 1.0e-3 * s0);

  /* solve regularized system with lambda_l */
  gsl_multifit_linear_solve(lambda_l, &A.matrix, &b.vector, state->c, &rnorm, &snorm, state->multifit_p);

  fprintf(stderr, "lambda_l = %.12e\n", lambda_l);
  fprintf(stderr, "lambda_gcv = %.12e\n", lambda_gcv);

  fprintf(stderr, "pcafit_fit: writing %s...", lambda_file);

  for (i = 0; i < npts; ++i)
    {
      fprintf(fp, "%e %e %e %e\n",
              gsl_vector_get(reg_param, i),
              gsl_vector_get(rho, i),
              gsl_vector_get(eta, i),
              gsl_vector_get(G, i));
    }

  fprintf(stderr, "done\n");

  fprintf(stderr, "rnorm = %.12e\n", rnorm);
  fprintf(stderr, "snorm = %.12e\n", snorm);

  fprintf(stderr, "cond(X) = %.12e\n", 1.0 / gsl_multifit_linear_rcond(state->multifit_p));

  gsl_vector_free(reg_param);
  gsl_vector_free(rho);
  gsl_vector_free(eta);
  gsl_vector_free(G);

  fclose(fp);

  return 0;
}

/*
pcafit_eval_B()
  Evaluate magnetic field at a given (r,theta) using
previously computed coefficients

Inputs: r      - radius (km)
        theta  - colatitude (radians)
        phi    - longitude (radians)
        B      - (output) magnetic field vector (nT)
        vstate - state

Notes:
1) state->c must contain fit coefficients
*/

static int
pcafit_eval_B(const double r, const double theta, const double phi,
           double B[3], void * vstate)
{
  int status;
  pca_state_t *state = (pca_state_t *) vstate;

  status = pca_B(state->c, r, theta, phi, B, state->pca_workspace_p);

  return status;
}

/*
pcafit_eval_J()
  Evaluate current density at a given (r,theta,phi) using
previously computed coefficients

Inputs: r      - radius (km)
        theta  - colatitude (radians)
        phi    - longitude (radians)
        J      - (output) current density vector [A/km]
        vstate - workspace

Notes:
1) state->c must contain coefficients
*/

static int
pcafit_eval_J(const double r, const double theta, const double phi,
              double J[3], void * vstate)
{
  int status;
  pca_state_t *state = (pca_state_t *) vstate;

  (void) r; /* unused parameter */

  status = pca_K(state->c, theta, phi, J, state->pca_workspace_p);

  return status;
}

static int
build_matrix_row(const double r, const double theta, const double phi,
                 gsl_vector *X, gsl_vector *Y, gsl_vector *Z,
                 pca_state_t *state)
{
  const size_t p = X->size;
  size_t i;

  for (i = 0; i < p; ++i)
    {
      double B[3];

      pca_pc_B(i, r, theta, phi, B, state->pca_workspace_p);

      gsl_vector_set(X, i, B[0]);
      gsl_vector_set(Y, i, B[1]);
      gsl_vector_set(Z, i, B[2]);
    }

  return 0;
}

static const magfit_type pca_type =
{
  "pca",
  pcafit_alloc,
  pcafit_reset,
  pcafit_add_track,
  pcafit_fit,
  pcafit_eval_B,
  pcafit_eval_J,
  pcafit_free
};

const magfit_type *magfit_pca = &pca_type;
