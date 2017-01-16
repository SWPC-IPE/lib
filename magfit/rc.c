/*
 * rc.c
 *
 * Fit simple ring current model to magnetic vector data. Model is
 *
 * B_model = k * (0.7 * B_ext + 0.3 * B_int)
 *
 * where B_int and B_ext are internal/external dipole fields aligned with
 * main field dipole axis, and k is a parameter to be determined from
 * each track
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
#include "green.h"
#include "track.h"

#include "magfit.h"

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
  green_workspace *green_workspace_p;
} rc_state_t;

static void *rc_alloc(const void * params);
static void rc_free(void * vstate);
static int rc_reset(void * vstate);
static size_t rc_ncoeff(void * vstate);
static int rc_add_datum(const double r, const double theta, const double phi,
                            const double qdlat, const double B[3], void * vstate);
static int rc_fit(double * rnorm, double * snorm, void * vstate);
static int rc_eval_B(const double r, const double theta, const double phi,
                         double B[3], void * vstate);
static int rc_eval_J(const double r, const double theta, const double phi,
                         double J[3], void * vstate);
static double rc_eval_chi(const double theta, const double phi, void * vstate);

static int build_matrix_row(const double r, const double theta, const double phi,
                            gsl_vector *X, gsl_vector *Y, gsl_vector *Z,
                            rc_state_t *state);

/*
rc_alloc()
  Allocate rc workspace

Inputs: params - parameters

Return: pointer to workspace
*/

static void *
rc_alloc(const void * params)
{
  rc_state_t *state;

  (void) params;

  state = calloc(1, sizeof(rc_state_t));
  if (!state)
    return 0;

  state->nmax = 20000;
  state->n = 0;
  state->p = 1;

  state->green_workspace_p = green_alloc(1, 1, R_EARTH_KM);

  state->X = gsl_matrix_alloc(state->nmax, state->p);
  state->c = gsl_vector_alloc(state->p);
  state->rhs = gsl_vector_alloc(state->nmax);
  state->wts = gsl_vector_alloc(state->nmax);
  state->cov = gsl_matrix_alloc(state->p, state->p);
  state->multifit_p = gsl_multifit_linear_alloc(state->nmax, state->p);

  return state;
}

static void
rc_free(void * vstate)
{
  rc_state_t *state = (rc_state_t *) vstate;

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

  if (state->green_workspace_p)
    green_free(state->green_workspace_p);

  free(state);
}

/* reset workspace to work on new data set */
static int
rc_reset(void * vstate)
{
  rc_state_t *state = (rc_state_t *) vstate;
  state->n = 0;
  return 0;
}

static size_t
rc_ncoeff(void * vstate)
{
  rc_state_t *state = (rc_state_t *) vstate;
  return state->p;
}

/*
rc_add_datum()
  Add single vector measurement to LS system

Inputs: r      - radius (km)
        theta  - colatitude (radians)
        phi    - longitude (radians)
        qdlat  - QD latitude (degrees)
        B      - magnetic field vector NEC (nT)
        vstate - state

Return: success/error

Notes:
1) state->n is updated with the number of total data added
*/

static int
rc_add_datum(const double r, const double theta, const double phi,
                   const double qdlat, const double B[3], void * vstate)
{
  rc_state_t *state = (rc_state_t *) vstate;
  size_t rowidx = state->n;
  double wi = 1.0;
  gsl_vector_view vx = gsl_matrix_row(state->X, rowidx);
  gsl_vector_view vy = gsl_matrix_row(state->X, rowidx + 1);
  gsl_vector_view vz = gsl_matrix_row(state->X, rowidx + 2);

  (void) qdlat;

  /* set rhs vector */
  gsl_vector_set(state->rhs, rowidx, B[0]);
  gsl_vector_set(state->rhs, rowidx + 1, B[1]);
  gsl_vector_set(state->rhs, rowidx + 2, B[2]);

  /* set weight vector */
  gsl_vector_set(state->wts, rowidx, wi);
  gsl_vector_set(state->wts, rowidx + 1, wi);
  gsl_vector_set(state->wts, rowidx + 2, wi);

  /* build 3 rows of the LS matrix */
  build_matrix_row(r, theta, phi, &vx.vector, &vy.vector, &vz.vector, state);
  rowidx += 3;

  state->n = rowidx;

  return GSL_SUCCESS;
}

/*
rc_fit()
  Fit model to previously added tracks

Inputs: rnorm  - residual norm || y - A x ||
        snorm  - solution norm || x ||
        vstate - state

Return: success/error

Notes:
1) Data must be added to workspace via rc_add_datum()
*/

static int
rc_fit(double * rnorm, double * snorm, void * vstate)
{
  rc_state_t *state = (rc_state_t *) vstate;
  gsl_matrix_view A = gsl_matrix_submatrix(state->X, 0, 0, state->n, state->p);
  gsl_vector_view b = gsl_vector_subvector(state->rhs, 0, state->n);
  gsl_vector_view wts = gsl_vector_subvector(state->wts, 0, state->n);
  double chisq;

  if (state->n < state->p)
    return -1;

  /* solve system */
  gsl_multifit_wlinear(&A.matrix, &wts.vector, &b.vector, state->c, state->cov, &chisq, state->multifit_p);

  *rnorm = sqrt(chisq);
  *snorm = gsl_blas_dnrm2(state->c);

  return 0;
}

/*
rc_eval_B()
  Evaluate magnetic field at a given (r,theta,phi) using
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
rc_eval_B(const double r, const double theta, const double phi,
                double B[3], void * vstate)
{
  int status = GSL_SUCCESS;
  rc_state_t *state = (rc_state_t *) vstate;
  gsl_vector_view vx = gsl_matrix_row(state->X, 0);
  gsl_vector_view vy = gsl_matrix_row(state->X, 1);
  gsl_vector_view vz = gsl_matrix_row(state->X, 2);

  build_matrix_row(r, theta, phi, &vx.vector, &vy.vector, &vz.vector, state);

  gsl_blas_ddot(&vx.vector, state->c, &B[0]);
  gsl_blas_ddot(&vy.vector, state->c, &B[1]);
  gsl_blas_ddot(&vz.vector, state->c, &B[2]);

  return status;
}

/*
rc_eval_J()
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
rc_eval_J(const double r, const double theta, const double phi,
          double J[3], void * vstate)
{
  (void) r; /* unused parameter */
  (void) theta; /* unused parameter */
  (void) phi; /* unused parameter */
  (void) vstate; /* unused parameter */

  /*XXX*/
  J[0] = 0.0;
  J[1] = 0.0;
  J[2] = 0.0;

  return GSL_SUCCESS;
}

/*
rc_eval_chi()
  Evaluate current stream function at a given (theta,phi) using
previously computed coefficients

Inputs: theta  - colatitude (radians)
        phi    - longitude (radians)
        vstate - workspace

Return: current stream function chi in kA/nT

Notes:
1) state->c must contain coefficients
*/

static double
rc_eval_chi(const double theta, const double phi, void * vstate)
{
  rc_state_t *state = (rc_state_t *) vstate;
  double chi;

  chi = green_eval_chi_int(R_EARTH_KM + 110.0, theta, phi, state->c, state->green_workspace_p);

  return chi;
}

static int
build_matrix_row(const double r, const double theta, const double phi,
                 gsl_vector *X, gsl_vector *Y, gsl_vector *Z,
                 rc_state_t *state)
{
  int s = 0;
  green_workspace *w = state->green_workspace_p;
  const double g10 = -29439.0536; /* XXX */
  const double g11 = -1492.8298;
  const double h11 = 4783.2326;
  const double g1 = gsl_hypot3(g10, g11, h11);
  double q[3];
  double dX[3], dY[3], dZ[3];
  double dX_ext[3], dY_ext[3], dZ_ext[3];

  /* unit vector along internal dipole direction */
  q[green_nmidx(1, 0, w)] = g10 / g1;
  q[green_nmidx(1, 1, w)] = g11 / g1;
  q[green_nmidx(1, -1, w)] = h11 / g1;

  /* compute internal/external green's functions */
  green_calc_int(r, theta, phi, dX, dY, dZ, w);
  green_calc_ext(r, theta, phi, dX_ext, dY_ext, dZ_ext, w);

  gsl_vector_set(X, 0, 0.7 * vec_dot(q, dX_ext) + 0.3 * vec_dot(q, dX));
  gsl_vector_set(Y, 0, 0.7 * vec_dot(q, dY_ext) + 0.3 * vec_dot(q, dY));
  gsl_vector_set(Z, 0, 0.7 * vec_dot(q, dZ_ext) + 0.3 * vec_dot(q, dZ));

  return s;
}

static const magfit_type rc_type =
{
  "rc",
  rc_alloc,
  rc_reset,
  rc_ncoeff,
  rc_add_datum,
  rc_fit,
  rc_eval_B,
  rc_eval_J,
  rc_eval_chi,
  rc_free
};

const magfit_type *magfit_rc = &rc_type;