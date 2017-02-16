/*
 * mfield.h
 */

#ifndef INCLUDED_mfield_h
#define INCLUDED_mfield_h

#include <satdata/satdata.h>

#include <gsl/gsl_math.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_multifit_nlinear.h>
#include <gsl/gsl_multilarge_nlinear.h>
#include <gsl/gsl_histogram.h>
#include <gsl/gsl_eigen.h>

#include "mfield_data.h"
#include "mfield_green.h"

#include "green.h"
#include "track_weight.h"

#define MFIELD_SYNTH_DATA      0

/* define to fit secular variation coefficients */
#define MFIELD_FIT_SECVAR      1

/* define to fit secular acceleration coefficients */
#define MFIELD_FIT_SECACC      1

#if !MFIELD_FIT_SECVAR
#define MFIELD_FIT_SECACC      0
#endif

/* fit Euler angles to data */
#define MFIELD_FIT_EULER       1

/* fit external field model to data */
#define MFIELD_FIT_EXTFIELD    0

/* epoch to define SV and SA terms in fit */
#define MFIELD_EPOCH          (2016.0)

#define MFIELD_RE_KM          (6371.2)

/*
 * approximate matrix size in bytes for precomputing J^T J; each
 * thread gets its own matrix (1 GB)
 */
#define MFIELD_MATRIX_SIZE    (1e9)

/* define if fitting to the EMAG2 grid */
#define MFIELD_EMAG2          0

/* define for no weighting in fit */
#define MFIELD_NOWEIGHTS      0

typedef struct
{
  double epoch;                         /* model epoch (decimal year) */
  double R;                             /* reference radius (km) */
  size_t nmax_mf;                       /* MF nmax */
  size_t nmax_sv;                       /* SV nmax */
  size_t nmax_sa;                       /* SA nmax */
  size_t nsat;                          /* number of satellites */
  double euler_period;                  /* time period for Euler angles (decimal days) */
  mfield_data_workspace *mfield_data_p; /* satellite data */
} mfield_parameters;

typedef struct
{
  size_t nsat;      /* number of different satellites */
  size_t nmax_mf;   /* maximum internal spherical harmonic degree for MF */
  size_t nmax_sv;   /* maximum internal spherical harmonic degree for SV */
  size_t nmax_sa;   /* maximum internal spherical harmonic degree for SA */
  size_t nmax_ext;  /* maximum external spherical harmonic degree */

  mfield_parameters params;

  size_t nnm_mf;    /* number of (n,m) coefficients in model for MF */
  size_t nnm_sv;    /* number of (n,m) coefficients in model for SV */
  size_t nnm_sa;    /* number of (n,m) coefficients in model for SA */
  size_t neuler;    /* number of Euler angles in model */
  size_t next;      /* number of external coefficients in model */

  size_t *nbins_euler;  /* number of Euler bins for each satellite */
  size_t *offset_euler; /* start index of each satellite's Euler angles in coefficient vector */

  int ext_fdayi[3 * 366 + 30]; /* sorted array of daily timestamps with data for that day */

  size_t p;         /* number of model coefficients */
  size_t p_int;     /* number of model coefficients for internal field only */

  size_t nobs_cnt;

  double *t;        /* data timestamps minus epoch (t - t0) in units of years */
  double t_mu;      /* time array mean (years) */
  double t_sigma;   /* time array stddev (years) */
  double epoch;     /* time epoch t0 (years) */

  double t0_data;   /* time of first data input (CDF_EPOCH) */

  double R;         /* reference radius (km) */

  double *cosmphi;  /* array of cos(m phi) */
  double *sinmphi;  /* array of sin(m phi) */

  double *Plm;      /* associated legendres */
  double *dPlm;     /* associated legendre derivatives */

  double *dX;       /* basis functions for X */
  double *dY;       /* basis functions for Y */
  double *dZ;       /* basis functions for Z */

  /*
   * The model coefficients are partitioned as follows:
   *
   * c = [ MF | SV | SA | Euler | External ]
   */
  gsl_vector *c;       /* model coefficients */
  gsl_vector *c_copy;  /* model coefficients in physical units */

  gsl_matrix *covar;   /* coefficient covariance matrix */

  size_t niter;        /* number of robust LS iterations */

  size_t sv_offset;    /* offset of SV coefficients in 'c' */
  size_t sa_offset;    /* offset of SA coefficients in 'c' */
  size_t euler_offset; /* offset of Euler angles in 'c' */
  size_t ext_offset;   /* offset of external coefficients in 'c' */

  gsl_vector *diag; /* diag(D) where D is regularization matrix */

  gsl_histogram *hf; /* histogram of F residuals */
  gsl_histogram *hz; /* histogram of Z residuals */

  /* nonlinear least squares parameters */
  gsl_vector *wts_spatial; /* spatial weights, nres-by-1 */
  gsl_vector *wts_final;   /* final weights (robust x spatial), nres-by-1 */
  gsl_multifit_nlinear_workspace *multifit_nlinear_p;
  gsl_multilarge_nlinear_workspace *nlinear_workspace_p;
  size_t ndata;            /* number of unique data points in LS system */
  size_t nres;             /* number of residuals to minimize */
  size_t nres_vec;         /* number of vector residuals to minimize */
  size_t data_block;       /* maximum observations to accumulate at once in LS system */
  gsl_vector *lambda_diag; /* diag(L) regularization matrix */
  gsl_vector *LTL;         /* L^T L regularization matrix */
  double lambda_mf;        /* main field damping */
  double lambda_sv;        /* SV damping */
  double lambda_sa;        /* SA damping */

  /*
   * The Jacobian is organized as follows:
   *
   * J = [ J_mf    | J_sv    | J_sa    | J_euler(x) | J_ext(x) ] vector
   *     [ J_mf(x) | J_sv(x) | J_sa(x) |     0      | J_ext(x) ] scalar
   *
   * J_mf, J_sv, and J_sa are constant for vector
   * residuals, and depend on the model parameters x
   * for scalar residuals. J_euler is 0 for scalar
   * residuals and depends on x for vector.
   * J_ext depends on x for both vector and scalar residuals.
   * J_euler and J_ext have significant sparse structure.
   *
   * For each iteration, we need to compute J^T J. This is
   * organized as:
   *
   * J^T J = [ JTJ_11 |    x   |    x   ]
   *         [ JTJ_21 | JTJ_22 |    x   ]
   *         [ JTJ_31 | JTJ_32 | JTJ_33 ]
   *
   * where we only need to compute the lower triangle since
   * the matrix is symmetric
   */
  gsl_matrix *JTJ_vec;     /* J_mf^T J_mf for vector measurements, p_int-by-p_int */

  size_t max_threads;      /* maximum number of threads/processors available */
  gsl_matrix *omp_dX;      /* dX/dg max_threads-by-nnm_mf */
  gsl_matrix *omp_dY;      /* dY/dg max_threads-by-nnm_mf */
  gsl_matrix *omp_dZ;      /* dZ/dg max_threads-by-nnm_mf */
  gsl_matrix *omp_dX_grad; /* gradient dX/dg max_threads-by-nnm_mf */
  gsl_matrix *omp_dY_grad; /* gradient dY/dg max_threads-by-nnm_mf */
  gsl_matrix *omp_dZ_grad; /* gradient dZ/dg max_threads-by-nnm_mf */
  gsl_matrix **omp_J;      /* max_threads matrices, each 4*data_block-by-p_int */
  size_t *omp_rowidx;      /* row indices for omp_J */
  gsl_matrix **omp_GTG;    /* max_threads matrices, each nnm_mf-by-nnm_mf */
  gsl_matrix **omp_JTJ;    /* max_threads matrices, each p_int-by-p_int */
  green_workspace **green_array_p; /* array of green workspaces, size max_threads */

  gsl_vector *fvec;        /* residual vector for robust weights */
  gsl_vector *wfvec;       /* weighted residual vector */
  gsl_multifit_robust_workspace *robust_workspace_p;

  mfield_green_workspace *green_workspace_p;
  mfield_data_workspace *data_workspace_p;
  track_weight_workspace *weight_workspace_p;
  green_workspace *green_workspace_p2;
  gsl_eigen_symm_workspace *eigen_workspace_p;
} mfield_workspace;

#define MFIELD_EULER_DERIV_ALPHA       (1 << 0)
#define MFIELD_EULER_DERIV_BETA        (1 << 1)
#define MFIELD_EULER_DERIV_GAMMA       (1 << 2)

/*
 * Prototypes
 */

mfield_workspace *mfield_alloc(const mfield_parameters *params);
void mfield_free(mfield_workspace *w);
int mfield_set_damping(const double lambda_sv, const double lambda_sa,
                       mfield_workspace *w);
mfield_workspace *mfield_copy(const mfield_workspace *w);
int mfield_init(mfield_workspace *w);
int mfield_calc_linear(gsl_vector *c, mfield_workspace *w);
int mfield_calc_nonlinear(gsl_vector *c, mfield_workspace *w);
int mfield_reset(mfield_workspace *w);
int mfield_coeffs(const int dir, const gsl_vector *gin, gsl_vector *gout,
                  const mfield_workspace *w);
int mfield_eval(const double t, const double r, const double theta, const double phi,
                double B[4], mfield_workspace *w);
int mfield_eval_dBdt(const double t, const double r, const double theta,
                     const double phi, double dBdt[4], mfield_workspace *w);
int mfield_eval_dgdt(const double t, const double r, const double theta,
                     const double phi, const gsl_vector *dg,
                     const gsl_vector *ddg, double dBdt[4],
                     mfield_workspace *w);
int mfield_eval_ext(const double t, const double r, const double theta, const double phi,
                    double B[4], mfield_workspace *w);
int mfield_eval_ext_coeff(const double r, const double theta, const double phi,
                          const double extcoeff, double B[4], mfield_workspace *w);
int mfield_eval_g_ext(const double t, const double r, const double theta, const double phi,
                      const double E_st, const double I_st,
                      const gsl_vector *g, const gsl_vector *dg,
                      double B[4], mfield_workspace *w);
int mfield_eval_static(const double r, const double theta, const double phi,
                       const gsl_vector *g, double B[4], mfield_workspace *w);
int mfield_calc_uncertainties(mfield_workspace *w);
int mfield_calc_evals(gsl_vector *evals, mfield_workspace *w);
double mfield_spectrum(const size_t n, const mfield_workspace *w);
double mfield_spectrum_sv(const size_t n, const mfield_workspace *w);
double mfield_spectrum_sa(const size_t n, const mfield_workspace *w);
int mfield_write(const char *filename, mfield_workspace *w);
mfield_workspace *mfield_read(const char *filename);
int mfield_write_ascii(const char *filename, const double epoch,
                       const int write_delta, mfield_workspace *w);
int mfield_new_epoch(const double new_epoch, mfield_workspace *w);
size_t mfield_coeff_nmidx(const size_t n, const int m);
size_t mfield_extidx(const double t, const mfield_workspace *w);
double mfield_get_mf(const gsl_vector *c, const size_t idx, const mfield_workspace *w);
double mfield_get_sv(const gsl_vector *c, const size_t idx, const mfield_workspace *w);
double mfield_get_sa(const gsl_vector *c, const size_t idx, const mfield_workspace *w);
int mfield_set_mf(gsl_vector *c, const size_t idx, const double x,
                  const mfield_workspace *w);
int mfield_set_sv(gsl_vector *c, const size_t idx, const double x,
                  const mfield_workspace *w);
int mfield_set_sa(gsl_vector *c, const size_t idx, const double x,
                  const mfield_workspace *w);

/* mfield_euler.c */
size_t mfield_euler_idx(const size_t sat_idx, const double t, const mfield_workspace *w);
int mfield_euler_print(const char *filename, const size_t sat_idx,
                       const mfield_workspace *w);

/* mfield_fill.c */
int mfield_fill(const char *coeffile, satdata_mag *data);

#endif /* INCLUDED_mfield_h */
