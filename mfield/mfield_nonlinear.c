#define OLD_FDF     0

typedef struct
{
  mfield_workspace *w;
} mfield_nonlinear_params;

static int mfield_init_nonlinear(mfield_workspace *w);
static int mfield_calc_f(const gsl_vector *x, void *params, gsl_vector *f);
static int mfield_calc_df(const gsl_vector *x, void *params, gsl_matrix *J);
static int mfield_calc_f2(const gsl_vector *x, void *params, gsl_vector *f);
static int mfield_calc_df2(const gsl_vector *x, const gsl_vector *y, void *params,
                           gsl_vector *JTy, gsl_matrix *JTJ);
static inline int mfield_jacobian_row(const double t, const size_t flags, const double weight,
                                      gsl_vector * dB_int, const double y, const size_t extidx,
                                      const double dB_ext, const size_t euler_idx, const double B_nec_alpha,
                                      const double B_nec_beta, const double B_nec_gamma,
                                      gsl_matrix *JTJ, gsl_vector *JTf, const mfield_workspace *w);
static inline int mfield_jacobian_row_F(const double t, const double y, const double weight,
                                        gsl_vector * dX, gsl_vector * dY, gsl_vector * dZ,
                                        const double B_model[4], const size_t extidx, const double dB_ext[3],
                                        gsl_vector *J_int, gsl_matrix *JTJ, gsl_vector *JTf,
                                        const mfield_workspace *w);
static int mfield_nonlinear_matrices(gsl_matrix *dX, gsl_matrix *dY,
                                     gsl_matrix *dZ, mfield_workspace *w);
static int mfield_nonlinear_matrices2(mfield_workspace *w);
static int mfield_nonlinear_vector_precompute(const gsl_vector *weights, mfield_workspace *w);
static int mfield_vector_green(const double t, const double weight, const gsl_vector *g,
                               gsl_vector *G, mfield_workspace *w);
static int mfield_read_matrix_block(const size_t nblock, mfield_workspace *w);
static double mfield_nonlinear_model_int(const double t, const gsl_vector *v,
                                         const gsl_vector *g, mfield_workspace *w);
static int mfield_nonlinear_model_ext(const double r, const double theta,
                                      const double phi, const gsl_vector *g,
                                      double dB[3], mfield_workspace *w);
static int mfield_nonlinear_histogram(const gsl_vector *c,
                                      mfield_workspace *w);
static int mfield_nonlinear_regularize(gsl_vector *diag,
                                       mfield_workspace *w);
static void mfield_nonlinear_callback(const size_t iter, void *params,
                                      const gsl_multifit_nlinear_workspace *multifit_p);
static void mfield_nonlinear_callback2(const size_t iter, void *params,
                                       const gsl_multilarge_nlinear_workspace *multifit_p);

/*
mfield_calc_nonlinear()
  Solve linear least squares system, using previously stored
satellite data

Inputs: c    - (input/output)
               on input, initial guess for coefficient vector
               on output, final coefficients
               units of nT, nT/year, nT/year^2
        w    - workspace

Notes:
1) On input, w->data_workspace_p must be filled with satellite data
2a) mfield_init() must be called first to initialize various parameters,
    including weights
2b) this includes mfield_init_nonlinear(), called from mfield_init()
3) on output, coefficients are stored in w->c with following units:
4) static coefficients have units of nT
5) SV coefficients have units of nT/dimensionless_time
6) SA coefficients have units of nT/dimensionless_time^2
7) call mfield_coeffs() to convert coefficients to physical
   time units
*/

int
mfield_calc_nonlinear(gsl_vector *c, mfield_workspace *w)
{
  int s = 0;
  const size_t max_iter = 50;     /* maximum iterations */
  const double xtol = 1.0e-4;
  const double gtol = 1.5e-3;
  const double ftol = 1.0e-6;
  int info;
  const size_t p = w->p;          /* number of coefficients */
  const size_t n = w->nres;       /* number of residuals */
  gsl_multifit_nlinear_fdf fdf;
  gsl_multilarge_nlinear_fdf fdf2;
  gsl_vector *f;
  struct timeval tv0, tv1;
  double res0;                    /* initial residual */
  FILE *fp_res;
  char resfile[2048];

  fdf.f = mfield_calc_f;
  fdf.df = mfield_calc_df;
  fdf.fvv = NULL;
  fdf.n = n;
  fdf.p = p;
  fdf.params = w;

  fdf2.f = mfield_calc_f2;
  fdf2.df = mfield_calc_df2;
  fdf2.fvv = NULL;
  fdf2.n = n;
  fdf2.p = p;
  fdf2.params = w;

  printv_octave(c, "c0");

  /* convert input vector from physical to dimensionless time units */
  mfield_coeffs(-1, c, c, w);

#if OLD_FDF
  /*
   * build and print residual histograms with previous coefficients
   * and previous wts_final vector
   */
  mfield_nonlinear_histogram(c, w);
#endif

  sprintf(resfile, "res.nlin.iter%zu.txt", w->niter);
  fp_res = fopen(resfile, "w");
  mfield_print_residuals(1, fp_res, NULL, NULL, NULL);

  /* compute robust weights with coefficients from previous iteration */
  if (w->niter > 0)
    {
      size_t i;

      /* compute f = Y_model - y_data with previous coefficients */
#if OLD_FDF
      mfield_calc_f(c, w, w->fvec);
#else
      /* compute residuals in w->fvec */
      mfield_calc_f2(c, w, w->fvec);
#endif

      gsl_vector_memcpy(w->wfvec, w->fvec);

      /* compute weighted residuals r = sqrt(W) (Y - y) */
      for (i = 0; i < n; ++i)
        {
          double wi = gsl_vector_get(w->wts_spatial, i);
          double *ri = gsl_vector_ptr(w->wfvec, i);

          *ri *= sqrt(wi);
        }

      /* compute robust weights */
#if 0
      gsl_multifit_robust_weights(w->wfvec, w->wts_final, w->robust_workspace_p);
#else
      gsl_multifit_robust_weights(w->fvec, w->wts_final, w->robust_workspace_p);
#endif

      mfield_print_residuals(0, fp_res, w->fvec, w->wts_final, w->wts_spatial);

      /* compute final weights = wts_robust .* wts_spatial */
      gsl_vector_mul(w->wts_final, w->wts_spatial);
    }
  else
    {
      gsl_vector_memcpy(w->wts_final, w->wts_spatial);
    }

  fprintf(stderr, "mfield_calc_nonlinear: wrote residuals to %s\n",
          resfile);
  fclose(fp_res);

#if MFIELD_SYNTH_DATA || MFIELD_EMAG2 || MFIELD_NOWEIGHTS
  gsl_vector_set_all(w->wts_final, 1.0);
#endif

#if MFIELD_REGULARIZE && !MFIELD_SYNTH_DATA
  fprintf(stderr, "mfield_calc_nonlinear: regularizing least squares system...");
  mfield_nonlinear_regularize(w->lambda_diag, w);
  fprintf(stderr, "done\n");
#else
  gsl_vector_set_all(w->lambda_diag, 0.0);
#endif

#if !OLD_FDF

  fprintf(stderr, "mfield_calc_nonlinear: precomputing vector J_int^T W J_int...");
  gettimeofday(&tv0, NULL);
  mfield_nonlinear_vector_precompute(w->wts_final, w);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  fprintf(stderr, "mfield_calc_nonlinear: initializing multilarge...");
  gettimeofday(&tv0, NULL);
  gsl_multilarge_nlinear_init(c, &fdf2, w->nlinear_workspace_p);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  /* compute initial residual */
  f = gsl_multilarge_nlinear_residual(w->nlinear_workspace_p);
  res0 = gsl_blas_dnrm2(f);

  fprintf(stderr, "mfield_calc_nonlinear: computing nonlinear least squares solution...");
  gettimeofday(&tv0, NULL);
  s = gsl_multilarge_nlinear_driver(max_iter, xtol, gtol, ftol,
                                    mfield_nonlinear_callback2, (void *) w,
                                    &info, w->nlinear_workspace_p);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  if (s == GSL_SUCCESS)
    {
      fprintf(stderr, "mfield_calc_nonlinear: number of iterations: %zu\n",
              gsl_multilarge_nlinear_niter(w->nlinear_workspace_p));
      fprintf(stderr, "mfield_calc_nonlinear: function evaluations: %zu\n",
              fdf2.nevalf);
      fprintf(stderr, "mfield_calc_nonlinear: Jacobian evaluations: %zu\n",
              fdf2.nevaldf);
      fprintf(stderr, "mfield_calc_nonlinear: reason for stopping: %d\n", info);
      fprintf(stderr, "mfield_calc_nonlinear: initial residual: %.12e\n", res0);
      fprintf(stderr, "mfield_calc_nonlinear: final residual: %.12e\n",
              gsl_blas_dnrm2(f));
    }
  else
    {
      fprintf(stderr, "mfield_calc_nonlinear: failed: %s\n",
              gsl_strerror(s));
    }

  /* store final coefficients in physical units */
  {
    gsl_vector *x_final = gsl_multilarge_nlinear_position(w->nlinear_workspace_p);

    gsl_vector_memcpy(w->c, x_final);
    mfield_coeffs(1, w->c, c, w);

    printv_octave(c, "cfinal");
  }

#else

  fprintf(stderr, "mfield_calc_nonlinear: initializing multifit...");
  gettimeofday(&tv0, NULL);
  gsl_multifit_nlinear_winit(&fdf, c, w->wts_final, w->multifit_nlinear_p);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  /* compute initial residual */
  f = gsl_multifit_nlinear_residual(w->multifit_nlinear_p);
  res0 = gsl_blas_dnrm2(f);

  fprintf(stderr, "mfield_calc_nonlinear: computing nonlinear least squares solution...");
  gettimeofday(&tv0, NULL);
  s = gsl_multifit_nlinear_driver(max_iter, xtol, gtol, ftol,
                                  mfield_nonlinear_callback, (void *) w,
                                  &info, w->multifit_nlinear_p);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  if (s == GSL_SUCCESS)
    {
      fprintf(stderr, "mfield_calc_nonlinear: NITER = %zu\n",
              gsl_multifit_nlinear_niter(w->multifit_nlinear_p));
      fprintf(stderr, "mfield_calc_nonlinear: NFEV  = %zu\n", fdf.nevalf);
      fprintf(stderr, "mfield_calc_nonlinear: NJEV  = %zu\n", fdf.nevaldf);
      fprintf(stderr, "mfield_calc_nonlinear: NAEV  = %zu\n", fdf.nevalfvv);
      fprintf(stderr, "mfield_calc_nonlinear: reason for stopping: %d\n", info);
      fprintf(stderr, "mfield_calc_nonlinear: initial |f(x)|: %.12e\n", res0);
      fprintf(stderr, "mfield_calc_nonlinear: final   |f(x)|: %.12e\n",
              gsl_blas_dnrm2(f));
    }
  else
    {
      fprintf(stderr, "mfield_calc_nonlinear: multifit failed: %s\n",
              gsl_strerror(s));
    }

  /* store final coefficients in physical units */
  {
    gsl_vector *x_final = gsl_multifit_nlinear_position(w->multifit_nlinear_p);

    gsl_vector_memcpy(w->c, x_final);
    mfield_coeffs(1, w->c, c, w);

    printv_octave(c, "cfinal");
  }
#endif

  w->niter++;

  return s;
} /* mfield_calc_nonlinear() */

/*
mfield_init_nonlinear()
  This function is called from mfield_init() to count
total number of residuals and allocate nonlinear least
squares workspaces

Notes:
1) weight_calc() must be called prior to this function to
compute spatial weights
*/

static int
mfield_init_nonlinear(mfield_workspace *w)
{
  int s = 0;
  const size_t p = w->p;        /* number of coefficients */
  const size_t nnm = w->nnm_mf; /* number of (n,m) harmonics */
  size_t ndata = 0;             /* number of distinct data points */
  size_t nres = 0;              /* total number of residuals */
  size_t nres_scal = 0;         /* number of scalar residuals */
  size_t nres_vec = 0;          /* number of vector residuals */
  struct timeval tv0, tv1;
  size_t i, j;

  /* count total number of residuals */
  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

      for (j = 0; j < mptr->n; ++j)
        {
          /* check if data point is discarded due to time interval */
          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          if (mptr->flags[j] & MAGDATA_FLG_X)
            ++nres_vec;
          if (mptr->flags[j] & MAGDATA_FLG_Y)
            ++nres_vec;
          if (mptr->flags[j] & MAGDATA_FLG_Z)
            ++nres_vec;

          /* don't increase nres if only fitting Euler angles */
          if (MAGDATA_ExistScalar(mptr->flags[j]) &&
              MAGDATA_FitMF(mptr->flags[j]))
            ++nres_scal;

          ++ndata;
        }
    }

  nres = nres_scal + nres_vec;

  w->nres_vec = nres_vec;
  w->nres_scal = nres_scal;
  w->nres = nres;
  w->ndata = ndata;

  fprintf(stderr, "mfield_init_nonlinear: %zu distinct data points\n", ndata);
  fprintf(stderr, "mfield_init_nonlinear: %zu scalar residuals\n", w->nres_scal);
  fprintf(stderr, "mfield_init_nonlinear: %zu vector residuals\n", w->nres_vec);
  fprintf(stderr, "mfield_init_nonlinear: %zu total residuals\n", w->nres);
  fprintf(stderr, "mfield_init_nonlinear: %zu total parameters\n", p);

  /* precomputing these matrices make computing the residuals faster */
#if OLD_FDF
  w->mat_dX = gsl_matrix_alloc(ndata, nnm);
  w->mat_dY = gsl_matrix_alloc(ndata, nnm);
  w->mat_dZ = gsl_matrix_alloc(ndata, nnm);
  if (!w->mat_dX || !w->mat_dY || !w->mat_dZ)
    {
      GSL_ERROR("error allocating dX, dY, dZ", GSL_ENOMEM);
    }

  fprintf(stderr, "mfield_init_nonlinear: building matrices for nonlinear fit...");
  gettimeofday(&tv0, NULL);
  mfield_nonlinear_matrices(w->mat_dX, w->mat_dY, w->mat_dZ, w);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  /* allocate fit workspace */
  {
    const gsl_multifit_nlinear_type *T = gsl_multifit_nlinear_lm;
    gsl_multifit_nlinear_parameters fdf_params =
      gsl_multifit_nlinear_default_parameters();

    fdf_params.solver = gsl_multifit_nlinear_solver_normal;
    fdf_params.accel = 0;
    fdf_params.h_fvv = 0.5;
    w->multifit_nlinear_p = gsl_multifit_nlinear_alloc(T, &fdf_params, nres, p);
  }
#else

  /* allocate fit workspace */
  {
    const gsl_multilarge_nlinear_type *T = gsl_multilarge_nlinear_lm;
    gsl_multilarge_nlinear_parameters fdf_params =
      gsl_multilarge_nlinear_default_parameters();

    w->nlinear_workspace_p = gsl_multilarge_nlinear_alloc(T, &fdf_params, nres, p);
  }

  fprintf(stderr, "mfield_init_nonlinear: writing matrices for nonlinear fit...");
  gettimeofday(&tv0, NULL);
  mfield_nonlinear_matrices2(w);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));
#endif

  w->lambda_diag = gsl_vector_calloc(p);

  w->wts_spatial = gsl_vector_alloc(nres);
  w->wts_final = gsl_vector_alloc(nres);

  /* to save memory, allocate p-by-p workspace since a full n-by-p isn't needed */
  w->robust_workspace_p = gsl_multifit_robust_alloc(gsl_multifit_robust_huber, p, p);

  w->fvec = gsl_vector_alloc(nres);
  w->wfvec = gsl_vector_alloc(nres);

  gsl_vector_set_all(w->wts_final, 1.0);

  /* calculate spatial weights */
  {
    size_t idx = 0;
    size_t j;

    for (i = 0; i < w->nsat; ++i)
      {
        magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

        for (j = 0; j < mptr->n; ++j)
          {
            double wt; /* spatial weight */

            if (MAGDATA_Discarded(mptr->flags[j]))
              continue;

            track_weight_get(mptr->phi[j], mptr->theta[j], &wt, w->weight_workspace_p);

            if (mptr->flags[j] & MAGDATA_FLG_X)
              gsl_vector_set(w->wts_spatial, idx++, MFIELD_WEIGHT_X * wt);

            if (mptr->flags[j] & MAGDATA_FLG_Y)
              gsl_vector_set(w->wts_spatial, idx++, MFIELD_WEIGHT_Y * wt);

            if (mptr->flags[j] & MAGDATA_FLG_Z)
              gsl_vector_set(w->wts_spatial, idx++, MFIELD_WEIGHT_Z * wt);

            if (MAGDATA_ExistScalar(mptr->flags[j]) && MAGDATA_FitMF(mptr->flags[j]))
              gsl_vector_set(w->wts_spatial, idx++, MFIELD_WEIGHT_F * wt);
          }
      }
  }

  return s;
} /* mfield_init_nonlinear() */

/*
mfield_calc_f()
  Construct residual vector f using coefficients x

Inputs: x      - model coefficients
        params - parameters
        f      - (output) residual vector
                 if set to NULL, the residual histograms
                 w->hf and w->hz are updated with residuals

Notes:
1) For the histograms, w->wts_final must be initialized prior
to calling this function
*/

static int
mfield_calc_f(const gsl_vector *x, void *params, gsl_vector *f)
{
  int s = GSL_SUCCESS;
  mfield_workspace *w = (mfield_workspace *) params;
  gsl_matrix *dX = w->mat_dX;
  gsl_matrix *dY = w->mat_dY;
  gsl_matrix *dZ = w->mat_dZ;
  size_t i, j;
  size_t ridx = 0; /* index of residual */
  size_t didx = 0; /* index of data point */

  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

      for (j = 0; j < mptr->n; ++j)
        {
          size_t k;
          double t = mptr->ts[j]; /* use scaled time */
          gsl_vector_view vx, vy, vz;
          double B_int[3];     /* internal field model */
          double B_extcorr[3]; /* external field correction model */
          double B_model[3];   /* a priori model (crustal/external) */
          double B_total[3];   /* internal + external */
          double B_obs[3];     /* observation vector NEC frame */

#if MFIELD_FIT_EXTFIELD
          size_t extidx = mfield_extidx(mptr->t[j], w);
          double extcoeff = gsl_vector_get(x, extidx);
          double dB_ext[3];
#endif

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          vx = gsl_matrix_row(dX, didx);
          vy = gsl_matrix_row(dY, didx);
          vz = gsl_matrix_row(dZ, didx);

          /* compute internal field model */
          B_int[0] = mfield_nonlinear_model_int(t, &vx.vector, x, w);
          B_int[1] = mfield_nonlinear_model_int(t, &vy.vector, x, w);
          B_int[2] = mfield_nonlinear_model_int(t, &vz.vector, x, w);

          /* load apriori model of external (and possibly crustal) field */
          B_model[0] = mptr->Bx_model[j];
          B_model[1] = mptr->By_model[j];
          B_model[2] = mptr->Bz_model[j];

#if MFIELD_FIT_EXTFIELD
          /* compute external field model correction */
          mfield_nonlinear_model_ext(mptr->r[j], mptr->theta[j], mptr->phi[j],
                                     x, dB_ext, w);

          /* add correction to POMME field */
          B_extcorr[0] = extcoeff * dB_ext[0];
          B_extcorr[1] = extcoeff * dB_ext[1];
          B_extcorr[2] = extcoeff * dB_ext[2];
#else
          B_extcorr[0] = B_extcorr[1] = B_extcorr[2] = 0.0;
#endif

          /* compute total modeled field (internal + external) */
          for (k = 0; k < 3; ++k)
            B_total[k] = B_int[k] + B_model[k] + B_extcorr[k];

#if MFIELD_FIT_EULER
          if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
            {
              /*
               * get the Euler angles for this satellite and time period,
               * and apply rotation
               */
              size_t euler_idx = mfield_euler_idx(i, mptr->t[j], w);
              double alpha = gsl_vector_get(x, euler_idx);
              double beta = gsl_vector_get(x, euler_idx + 1);
              double gamma = gsl_vector_get(x, euler_idx + 2);
              double *q = &(mptr->q[4*j]);
              double B_vfm[3];

              B_vfm[0] = mptr->Bx_vfm[j];
              B_vfm[1] = mptr->By_vfm[j];
              B_vfm[2] = mptr->Bz_vfm[j];

              /* rotate VFM vector to NEC */
              euler_vfm2nec(EULER_FLG_ZYX, alpha, beta, gamma, q, B_vfm, B_obs);
            }
          else
#endif
            {
              /* use supplied NEC vector */
              B_obs[0] = mptr->Bx_nec[j];
              B_obs[1] = mptr->By_nec[j];
              B_obs[2] = mptr->Bz_nec[j];
            }

          if (mptr->flags[j] & MAGDATA_FLG_X)
            {
              /* set residual vector */
              if (f)
                gsl_vector_set(f, ridx, B_total[0] - B_obs[0]);

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Y)
            {
              /* set residual vector */
              if (f)
                gsl_vector_set(f, ridx, B_total[1] - B_obs[1]);

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Z)
            {
              /* set residual vector */
              if (f)
                gsl_vector_set(f, ridx, B_total[2] - B_obs[2]);
              else
                {
                  double wt = gsl_vector_get(w->wts_final, ridx);
                  wt = sqrt(wt);
                  wt = 1.0;
                  gsl_histogram_increment(w->hz, wt * (B_obs[2] - B_total[2]));
                }

              ++ridx;
            }

          if (MAGDATA_ExistScalar(mptr->flags[j]) &&
              MAGDATA_FitMF(mptr->flags[j]))
            {
              double F = gsl_hypot3(B_total[0], B_total[1], B_total[2]);
              double F_obs = mptr->F[j];

              if (f)
                gsl_vector_set(f, ridx, F - F_obs);
              else
                {
                  double wt = gsl_vector_get(w->wts_final, ridx);
                  wt = sqrt(wt);
                  wt = 1.0;
                  gsl_histogram_increment(w->hf, wt * (F_obs - F));
                }

              ++ridx;
            }

          ++didx;
        } /* for (j = 0; j < mptr->n; ++j) */
    }

  if (f)
    assert(ridx == f->size);

  return s;
} /* mfield_calc_f() */

static int
mfield_calc_df(const gsl_vector *x, void *params, gsl_matrix *J)
{
  int s = GSL_SUCCESS;
  mfield_workspace *w = (mfield_workspace *) params;
  gsl_matrix *dX = w->mat_dX;
  gsl_matrix *dY = w->mat_dY;
  gsl_matrix *dZ = w->mat_dZ;
  size_t i, j;
  size_t ridx = 0; /* index of residual */
  size_t didx = 0; /* index of data point */

  gsl_matrix_set_zero(J);

  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

      for (j = 0; j < mptr->n; ++j)
        {
          double t = mptr->ts[j]; /* use scaled time */

#if MFIELD_FIT_EULER
          size_t euler_idx;
          double B_vfm[3], B_nec_alpha[3], B_nec_beta[3], B_nec_gamma[3];
#endif

#if MFIELD_FIT_EXTFIELD
          size_t extidx = mfield_extidx(mptr->t[j], w);
          double extcoeff = gsl_vector_get(x, extidx);
          double dB_ext[3];

          /* compute external field model */
          mfield_nonlinear_model_ext(mptr->r[j], mptr->theta[j], mptr->phi[j],
                                     x, dB_ext, w);
#endif

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

#if MFIELD_FIT_EULER
          /* compute Euler angle derivatives of B vector */
          if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
            {
              const double *q = &(mptr->q[4*j]);
              double alpha, beta, gamma;

              euler_idx = mfield_euler_idx(i, mptr->t[j], w);
              alpha = gsl_vector_get(x, euler_idx);
              beta = gsl_vector_get(x, euler_idx + 1);
              gamma = gsl_vector_get(x, euler_idx + 2);

              /* get vector in VFM frame */
              B_vfm[0] = mptr->Bx_vfm[j];
              B_vfm[1] = mptr->By_vfm[j];
              B_vfm[2] = mptr->Bz_vfm[j];

              /* compute alpha derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_ALPHA, alpha, beta, gamma, q, B_vfm, B_nec_alpha);

              /* compute beta derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_BETA, alpha, beta, gamma, q, B_vfm, B_nec_beta);

              /* compute gamma derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_GAMMA, alpha, beta, gamma, q, B_vfm, B_nec_gamma);
            }
#endif

          if (mptr->flags[j] & MAGDATA_FLG_X)
            {
              /* check if fitting MF to this data point */
              if (MAGDATA_FitMF(mptr->flags[j]))
                {
                  gsl_vector_view Jv, vx;

                  /* main field portion */
                  vx = gsl_matrix_subrow(dX, didx, 0, w->nnm_mf);
                  Jv = gsl_matrix_subrow(J, ridx, 0, w->nnm_mf);
                  gsl_vector_memcpy(&Jv.vector, &vx.vector);

#if MFIELD_FIT_SECVAR
                  /* secular variation portion */
                  vx = gsl_matrix_subrow(dX, didx, 0, w->nnm_sv);
                  Jv = gsl_matrix_subrow(J, ridx, w->sv_offset, w->nnm_sv);
                  gsl_vector_memcpy(&Jv.vector, &vx.vector);
                  gsl_vector_scale(&Jv.vector, t);
#endif

#if MFIELD_FIT_SECACC
                  /* secular acceleration portion */
                  vx = gsl_matrix_subrow(dX, didx, 0, w->nnm_sa);
                  Jv = gsl_matrix_subrow(J, ridx, w->sa_offset, w->nnm_sa);
                  gsl_vector_memcpy(&Jv.vector, &vx.vector);
                  gsl_vector_scale(&Jv.vector, 0.5 * t * t);
#endif

#if MFIELD_FIT_EXTFIELD
                  gsl_matrix_set(J, ridx, extidx, dB_ext[0]);
#endif
                }

#if MFIELD_FIT_EULER
              /* check if fitting Euler angles to this data point */
              if (MAGDATA_FitEuler(mptr->flags[j]))
                {
                  gsl_matrix_set(J, ridx, euler_idx, -B_nec_alpha[0]);
                  gsl_matrix_set(J, ridx, euler_idx + 1, -B_nec_beta[0]);
                  gsl_matrix_set(J, ridx, euler_idx + 2, -B_nec_gamma[0]);
                }
#endif

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Y)
            {
              /* check if fitting MF to this data point */
              if (MAGDATA_FitMF(mptr->flags[j]))
                {
                  gsl_vector_view Jv, vy;

                  /* main field portion */
                  vy = gsl_matrix_subrow(dY, didx, 0, w->nnm_mf);
                  Jv = gsl_matrix_subrow(J, ridx, 0, w->nnm_mf);
                  gsl_vector_memcpy(&Jv.vector, &vy.vector);

#if MFIELD_FIT_SECVAR
                  /* secular variation portion */
                  vy = gsl_matrix_subrow(dY, didx, 0, w->nnm_sv);
                  Jv = gsl_matrix_subrow(J, ridx, w->sv_offset, w->nnm_sv);
                  gsl_vector_memcpy(&Jv.vector, &vy.vector);
                  gsl_vector_scale(&Jv.vector, t);
#endif

#if MFIELD_FIT_SECACC
                  /* secular acceleration portion */
                  vy = gsl_matrix_subrow(dY, didx, 0, w->nnm_sa);
                  Jv = gsl_matrix_subrow(J, ridx, w->sa_offset, w->nnm_sa);
                  gsl_vector_memcpy(&Jv.vector, &vy.vector);
                  gsl_vector_scale(&Jv.vector, 0.5 * t * t);
#endif

#if MFIELD_FIT_EXTFIELD
                  gsl_matrix_set(J, ridx, extidx, dB_ext[1]);
#endif
                }

#if MFIELD_FIT_EULER
              /* check if fitting Euler angles to this data point */
              if (MAGDATA_FitEuler(mptr->flags[j]))
                {
                  gsl_matrix_set(J, ridx, euler_idx, -B_nec_alpha[1]);
                  gsl_matrix_set(J, ridx, euler_idx + 1, -B_nec_beta[1]);
                  gsl_matrix_set(J, ridx, euler_idx + 2, -B_nec_gamma[1]);
                }
#endif

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Z)
            {
              /* check if fitting MF to this data point */
              if (MAGDATA_FitMF(mptr->flags[j]))
                {
                  gsl_vector_view Jv, vz;

                  /* main field portion */
                  vz = gsl_matrix_subrow(dZ, didx, 0, w->nnm_mf);
                  Jv = gsl_matrix_subrow(J, ridx, 0, w->nnm_mf);
                  gsl_vector_memcpy(&Jv.vector, &vz.vector);

#if MFIELD_FIT_SECVAR
                  /* secular variation portion */
                  vz = gsl_matrix_subrow(dZ, didx, 0, w->nnm_sv);
                  Jv = gsl_matrix_subrow(J, ridx, w->sv_offset, w->nnm_sv);
                  gsl_vector_memcpy(&Jv.vector, &vz.vector);
                  gsl_vector_scale(&Jv.vector, t);
#endif

#if MFIELD_FIT_SECACC
                  /* secular acceleration portion */
                  vz = gsl_matrix_subrow(dZ, didx, 0, w->nnm_sa);
                  Jv = gsl_matrix_subrow(J, ridx, w->sa_offset, w->nnm_sa);
                  gsl_vector_memcpy(&Jv.vector, &vz.vector);
                  gsl_vector_scale(&Jv.vector, 0.5 * t * t);
#endif

#if MFIELD_FIT_EXTFIELD
                  gsl_matrix_set(J, ridx, extidx, dB_ext[2]);
#endif
                }

#if MFIELD_FIT_EULER
              /* check if fitting Euler angles to this data point */
              if (MAGDATA_FitEuler(mptr->flags[j]))
                {
                  gsl_matrix_set(J, ridx, euler_idx, -B_nec_alpha[2]);
                  gsl_matrix_set(J, ridx, euler_idx + 1, -B_nec_beta[2]);
                  gsl_matrix_set(J, ridx, euler_idx + 2, -B_nec_gamma[2]);
                }
#endif

              ++ridx;
            }

          if (MAGDATA_ExistScalar(mptr->flags[j]) &&
              MAGDATA_FitMF(mptr->flags[j]))
            {
              gsl_vector_view Jv = gsl_matrix_row(J, ridx++);
              gsl_vector_view vx = gsl_matrix_row(dX, didx);
              gsl_vector_view vy = gsl_matrix_row(dY, didx);
              gsl_vector_view vz = gsl_matrix_row(dZ, didx);
              double X, Y, Z, F;
              size_t k;

              /* compute internal X, Y, Z */
              X = mfield_nonlinear_model_int(t, &vx.vector, x, w);
              Y = mfield_nonlinear_model_int(t, &vy.vector, x, w);
              Z = mfield_nonlinear_model_int(t, &vz.vector, x, w);

              /* add apriori (external and crustal) field */
              X += mptr->Bx_model[j];
              Y += mptr->By_model[j];
              Z += mptr->Bz_model[j];

#if MFIELD_FIT_EXTFIELD
              /* add external field correction */
              X += extcoeff * dB_ext[0];
              Y += extcoeff * dB_ext[1];
              Z += extcoeff * dB_ext[2];
#endif

              F = gsl_hypot3(X, Y, Z);

              /* compute (X dX + Y dY + Z dZ) */
              for (k = 0; k < w->nnm_mf; ++k)
                {
                  double dXk = gsl_vector_get(&vx.vector, k);
                  double dYk = gsl_vector_get(&vy.vector, k);
                  double dZk = gsl_vector_get(&vz.vector, k);
                  double val = X * dXk + Y * dYk + Z * dZk;

                  mfield_set_mf(&Jv.vector, k, val, w);
                  mfield_set_sv(&Jv.vector, k, t * val, w);
                  mfield_set_sa(&Jv.vector, k, 0.5 * t * t * val, w);
                }

#if MFIELD_FIT_EXTFIELD
              gsl_vector_set(&Jv.vector, extidx, X * dB_ext[0] + Y * dB_ext[1] + Z * dB_ext[2]);
#endif

              /* scale by 1/F */
              gsl_vector_scale(&Jv.vector, 1.0 / F);
            }

          ++didx;
        }
    }

  assert(ridx == J->size1);

#if 0
  print_octave(J, "J");
  exit(1);
#endif

  return s;
} /* mfield_calc_df() */

/*
mfield_calc_f2()
  Calculate residual vector

Inputs: x      - model parameters
        params - parameters
        f      - (output) residual f(x)

Notes:
1) On output, w->fvec contains full residual vector
*/

static int
mfield_calc_f2(const gsl_vector *x, void *params, gsl_vector *f)
{
  mfield_workspace *w = (mfield_workspace *) params;
  double B_extcorr[3] = { 0.0, 0.0, 0.0 }; /* external field correction model */
#if MFIELD_FIT_EXTFIELD
  double dB_ext[3] = { 0.0, 0.0, 0.0 };
  size_t extidx = 0;
  double extcoeff = 0.0;
#endif
  size_t euler_idx = 0;
  double B_nec_alpha[3], B_nec_beta[3], B_nec_gamma[3];
  size_t i, j, k;
  size_t ridx = 0;   /* index of residual in [0:nres-1] */
  size_t didx = 0;   /* index of data point in [0:ndata-1] */
  size_t dbidx = 0;  /* index of data point in current block, [0:data_block-1] */
  size_t nblock = 0; /* number of row blocks processed */
  size_t rowidx = 0; /* row index for block accumulation of scalar residual J_int */

  /*
   * read first block of internal Green's functions from disk,
   * stored in w->block_{dX,dY,dZ}
   */
  mfield_read_matrix_block(nblock, w);

  /* loop over satellites */
  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

      /* loop over data for individual satellite */
      for (j = 0; j < mptr->n; ++j)
        {
          gsl_vector_view vx, vy, vz;
          double t = mptr->ts[j]; /* use scaled time */
          double B_int[3];        /* internal field model */
          double B_model[3];      /* a priori model (crustal/external) */
          double B_total[4];      /* internal + external */
          double B_obs[3];        /* observation vector NEC frame */
#if MFIELD_FIT_EULER
          double B_vfm[3];        /* observation vector VFM frame */
#endif

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          vx = gsl_matrix_row(w->block_dX, dbidx);
          vy = gsl_matrix_row(w->block_dY, dbidx);
          vz = gsl_matrix_row(w->block_dZ, dbidx);

          /* compute internal field model */
          B_int[0] = mfield_nonlinear_model_int(t, &vx.vector, x, w);
          B_int[1] = mfield_nonlinear_model_int(t, &vy.vector, x, w);
          B_int[2] = mfield_nonlinear_model_int(t, &vz.vector, x, w);

          /* load apriori model of external (and possibly crustal) field */
          B_model[0] = mptr->Bx_model[j];
          B_model[1] = mptr->By_model[j];
          B_model[2] = mptr->Bz_model[j];

#if MFIELD_FIT_EXTFIELD
          extidx = mfield_extidx(mptr->t[j], w);
          extcoeff = gsl_vector_get(x, extidx);

          /* compute external field model */
          mfield_nonlinear_model_ext(mptr->r[j], mptr->theta[j], mptr->phi[j],
                                     x, dB_ext, w);

          /* add correction to external field model */
          for (k = 0; k < 3; ++k)
            B_extcorr[k] = extcoeff * dB_ext[k];
#endif

          /* compute total modeled field (internal + external) */
          for (k = 0; k < 3; ++k)
            B_total[k] = B_int[k] + B_model[k] + B_extcorr[k];

#if MFIELD_FIT_EULER
          /* compute Euler angle derivatives of B vector */
          if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
            {
              const double *q = &(mptr->q[4*j]);
              double alpha, beta, gamma;

              euler_idx = mfield_euler_idx(i, mptr->t[j], w);
              alpha = gsl_vector_get(x, euler_idx);
              beta = gsl_vector_get(x, euler_idx + 1);
              gamma = gsl_vector_get(x, euler_idx + 2);

              /* get vector in VFM frame */
              B_vfm[0] = mptr->Bx_vfm[j];
              B_vfm[1] = mptr->By_vfm[j];
              B_vfm[2] = mptr->Bz_vfm[j];

              /* compute alpha derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_ALPHA, alpha, beta, gamma, q, B_vfm, B_nec_alpha);

              /* compute beta derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_BETA, alpha, beta, gamma, q, B_vfm, B_nec_beta);

              /* compute gamma derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_GAMMA, alpha, beta, gamma, q, B_vfm, B_nec_gamma);

              /* compute observation vector in NEC frame */
              euler_vfm2nec(EULER_FLG_ZYX, alpha, beta, gamma, q, B_vfm, B_obs);
            }
          else
#endif
            {
              /* use supplied NEC vector */
              B_obs[0] = mptr->Bx_nec[j];
              B_obs[1] = mptr->By_nec[j];
              B_obs[2] = mptr->Bz_nec[j];
            }

          if (mptr->flags[j] & MAGDATA_FLG_X)
            {
              double fj = B_total[0] - B_obs[0];
              double wj = gsl_vector_get(w->wts_final, ridx);

              /* set residual vector X component */
              gsl_vector_set(w->fvec, ridx, fj);
              gsl_vector_set(f, ridx, fj * sqrt(wj));

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Y)
            {
              double fj = B_total[1] - B_obs[1];
              double wj = gsl_vector_get(w->wts_final, ridx);

              /* set residual vector Y component */
              gsl_vector_set(w->fvec, ridx, fj);
              gsl_vector_set(f, ridx, fj * sqrt(wj));

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Z)
            {
              double fj = B_total[2] - B_obs[2];
              double wj = gsl_vector_get(w->wts_final, ridx);

              /* set residual vector Z component */
              gsl_vector_set(w->fvec, ridx, fj);
              gsl_vector_set(f, ridx, fj * sqrt(wj));

              ++ridx;
            }

          if (MAGDATA_ExistScalar(mptr->flags[j]) &&
              MAGDATA_FitMF(mptr->flags[j]))
            {
              double F_obs = mptr->F[j];
              double fj;
              double wj = gsl_vector_get(w->wts_final, ridx);

              B_total[3] = gsl_hypot3(B_total[0], B_total[1], B_total[2]);
              fj = B_total[3] - F_obs;

              /* set scalar residual */
              gsl_vector_set(w->fvec, ridx, fj);
              gsl_vector_set(f, ridx, fj * sqrt(wj));

              ++ridx;
            }

          if (++dbidx == w->data_block)
            {
              /* reset for new block of observations */
              dbidx = 0;

              /* read next block of internal Green's functions from disk */
              ++nblock;
              mfield_read_matrix_block(nblock, w);
            }

          if (rowidx == w->data_block)
            {
              /* reset for new block of rows */
              rowidx = 0;
            }

          ++didx;
        }
    }

  assert(ridx == w->nres);
  assert(didx == w->ndata);

  return GSL_SUCCESS;
} /* mfield_calc_f2() */

/*
mfield_calc_fdf()
  Accumulate Jacobian and residual vector (J,f) into nonlinear LS
solver
*/

static int
mfield_calc_df2(const gsl_vector *x, const gsl_vector *y, void *params,
                gsl_vector *JTy, gsl_matrix *JTJ)
{
  mfield_workspace *w = (mfield_workspace *) params;
  double dB_ext[3] = { 0.0, 0.0, 0.0 };
  double B_extcorr[3] = { 0.0, 0.0, 0.0 }; /* external field correction model */
  size_t extidx = 0;
  double extcoeff = 0.0;
  size_t euler_idx = 0;
  double B_nec_alpha[3], B_nec_beta[3], B_nec_gamma[3];
  size_t i, j, k;
  size_t ridx = 0;   /* index of residual in [0:nres-1] */
  size_t didx = 0;   /* index of data point in [0:ndata-1] */
  size_t dbidx = 0;  /* index of data point in current block, [0:data_block-1] */
  size_t nblock = 0; /* number of row blocks processed */
  size_t rowidx = 0; /* row index for block accumulation of scalar residual J_int */

  /* internal field portion of J^T J */
  gsl_matrix_view JTJ_int;

  /* avoid unused variable warnings */
  (void)extcoeff;

  if (JTy)
    gsl_vector_set_zero(JTy);

  if (JTJ)
    {
      gsl_matrix_set_zero(JTJ);

      /* copy previously computed vector internal field portion of J^T J
       * (doesn't depend on x) */
      JTJ_int = gsl_matrix_submatrix(JTJ, 0, 0, w->p_int, w->p_int);
      gsl_matrix_tricpy('L', 1, &JTJ_int.matrix, w->JTJ_vec);
    }

  /*
   * read first block of internal Green's functions from disk,
   * stored in w->block_{dX,dY,dZ}
   */
  mfield_read_matrix_block(nblock, w);

  /* loop over satellites */
  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

      /* loop over data for individual satellite */
      for (j = 0; j < mptr->n; ++j)
        {
          gsl_vector_view vx, vy, vz;
          double t = mptr->ts[j]; /* use scaled time */
          double B_int[3];        /* internal field model */
          double B_model[3];      /* a priori model (crustal/external) */
          double B_total[4];      /* internal + external */
          double B_obs[3];        /* observation vector NEC frame */
#if MFIELD_FIT_EULER
          double B_vfm[3];        /* observation vector VFM frame */
#endif

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          vx = gsl_matrix_row(w->block_dX, dbidx);
          vy = gsl_matrix_row(w->block_dY, dbidx);
          vz = gsl_matrix_row(w->block_dZ, dbidx);

          /* compute internal field model */
          B_int[0] = mfield_nonlinear_model_int(t, &vx.vector, x, w);
          B_int[1] = mfield_nonlinear_model_int(t, &vy.vector, x, w);
          B_int[2] = mfield_nonlinear_model_int(t, &vz.vector, x, w);

          /* load apriori model of external (and possibly crustal) field */
          B_model[0] = mptr->Bx_model[j];
          B_model[1] = mptr->By_model[j];
          B_model[2] = mptr->Bz_model[j];

#if MFIELD_FIT_EXTFIELD
          extidx = mfield_extidx(mptr->t[j], w);
          extcoeff = gsl_vector_get(x, extidx);

          /* compute external field model */
          mfield_nonlinear_model_ext(mptr->r[j], mptr->theta[j], mptr->phi[j],
                                     x, dB_ext, w);

          /* add correction to external field model */
          for (k = 0; k < 3; ++k)
            B_extcorr[k] = extcoeff * dB_ext[k];
#endif

          /* compute total modeled field (internal + external) */
          for (k = 0; k < 3; ++k)
            B_total[k] = B_int[k] + B_model[k] + B_extcorr[k];

#if MFIELD_FIT_EULER
          /* compute Euler angle derivatives of B vector */
          if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
            {
              const double *q = &(mptr->q[4*j]);
              double alpha, beta, gamma;

              euler_idx = mfield_euler_idx(i, mptr->t[j], w);
              alpha = gsl_vector_get(x, euler_idx);
              beta = gsl_vector_get(x, euler_idx + 1);
              gamma = gsl_vector_get(x, euler_idx + 2);

              /* get vector in VFM frame */
              B_vfm[0] = mptr->Bx_vfm[j];
              B_vfm[1] = mptr->By_vfm[j];
              B_vfm[2] = mptr->Bz_vfm[j];

              /* compute alpha derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_ALPHA, alpha, beta, gamma, q, B_vfm, B_nec_alpha);

              /* compute beta derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_BETA, alpha, beta, gamma, q, B_vfm, B_nec_beta);

              /* compute gamma derivative of: R_q R_3 B_vfm */
              euler_vfm2nec(EULER_FLG_ZYX|EULER_FLG_DERIV_GAMMA, alpha, beta, gamma, q, B_vfm, B_nec_gamma);

              /* compute observation vector in NEC frame */
              euler_vfm2nec(EULER_FLG_ZYX, alpha, beta, gamma, q, B_vfm, B_obs);
            }
          else
#endif
            {
              /* use supplied NEC vector */
              B_obs[0] = mptr->Bx_nec[j];
              B_obs[1] = mptr->By_nec[j];
              B_obs[2] = mptr->Bz_nec[j];
            }

          if (mptr->flags[j] & MAGDATA_FLG_X)
            {
              double wj = gsl_vector_get(w->wts_final, ridx);
              double yj = gsl_vector_get(y, ridx);

              if (JTJ)
                {
                  mfield_jacobian_row(t, mptr->flags[j], wj, &vx.vector, yj,
                                      extidx, dB_ext[0], euler_idx, B_nec_alpha[0],
                                      B_nec_beta[0], B_nec_gamma[0], JTJ, JTy, w);
                }

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Y)
            {
              double wj = gsl_vector_get(w->wts_final, ridx);
              double yj = gsl_vector_get(y, ridx);

              if (JTJ)
                {
                  mfield_jacobian_row(t, mptr->flags[j], wj, &vy.vector, yj,
                                      extidx, dB_ext[1], euler_idx, B_nec_alpha[1],
                                      B_nec_beta[1], B_nec_gamma[1], JTJ, JTy, w);
                }

              ++ridx;
            }

          if (mptr->flags[j] & MAGDATA_FLG_Z)
            {
              double wj = gsl_vector_get(w->wts_final, ridx);
              double yj = gsl_vector_get(y, ridx);

              if (JTJ)
                {
                  mfield_jacobian_row(t, mptr->flags[j], wj, &vz.vector, yj,
                                      extidx, dB_ext[2], euler_idx, B_nec_alpha[2],
                                      B_nec_beta[2], B_nec_gamma[2], JTJ, JTy, w);
                }

              ++ridx;
            }

          if (MAGDATA_ExistScalar(mptr->flags[j]) &&
              MAGDATA_FitMF(mptr->flags[j]))
            {
              double wj = gsl_vector_get(w->wts_final, ridx);
              double yj = gsl_vector_get(y, ridx);

              B_total[3] = gsl_hypot3(B_total[0], B_total[1], B_total[2]);

              if (JTJ)
                {
                  gsl_vector_view Jv = gsl_matrix_subrow(w->block_J, rowidx++, 0, w->p_int);
                  mfield_jacobian_row_F(t, yj, wj, &vx.vector, &vy.vector, &vz.vector,
                                        B_total, extidx, dB_ext, &Jv.vector, JTJ, JTy, w);
                }

              ++ridx;
            }

          if (++dbidx == w->data_block)
            {
              /* reset for new block of observations */
              dbidx = 0;

              /* read next block of internal Green's functions from disk */
              ++nblock;
              mfield_read_matrix_block(nblock, w);
            }

          if (rowidx == w->data_block)
            {
              /* accumulate scalar J_int^T J_int into J^T J; it is much faster to do this
               * with blocks and dsyrk() rather than individual rows with dsyr() */
              gsl_matrix_view Jm = gsl_matrix_submatrix(w->block_J, 0, 0, rowidx, w->p_int);

              gsl_blas_dsyrk(CblasLower, CblasTrans, 1.0, &Jm.matrix, 1.0, &JTJ_int.matrix);

              /* reset for new block of rows */
              rowidx = 0;
            }

          ++didx;
        }
    }

  /* accumulate any last rows of scalar internal field Green's functions */
  if (rowidx > 0)
    {
      gsl_matrix_view Jm = gsl_matrix_submatrix(w->block_J, 0, 0, rowidx, w->p_int);
      gsl_blas_dsyrk(CblasLower, CblasTrans, 1.0, &Jm.matrix, 1.0, &JTJ_int.matrix);
    }

#if 0
  gsl_matrix_transpose_tricpy('L', 0, JTJ, JTJ);
  print_octave(JTJ, "JTJ");
  printv_octave(JTy, "JTy");
  printv_octave(y, "y");
  printv_octave(w->wts_final, "w");
  exit(1);
#endif

  assert(ridx == w->nres);
  assert(didx == w->ndata);

  return GSL_SUCCESS;
} /* mfield_calc_df2() */

/*
mfield_jacobian_row()
  Update the J^T J matrix and J^T y vector with a new row
of the Jacobian matrix, corresponding to a vector residual.
The internal field portion of J^T J does not need to be
computed, since it is independent of the model parameters
and is pre-computed. Only the Euler and external field
portion of J^T J must be updated. All portions of the
vector J^T y are updated.

Inputs: t           - scaled timestamp
        flags       - MAGDATA_FLG_xxx flags for this data point
        weight      - weight for this data point
        dB_int      - Green's functions for desired vector component of
                      internal SH expansion, nnm_mf-by-1
        y           - element of y vector corresponding to this row,
                      for computing J^T y
        extidx      - index of external field coefficient in [0,next-1]
        dB_ext      - external field Green's function corresponding
                      to desired vector component
        euler_idx   - index of Euler angles
        B_nec_alpha - Green's function for alpha Euler angle
        B_nec_beta  - Green's function for beta Euler angle
        B_nec_gamma - Green's function for gamma Euler angle
        JTJ         - (output) J^T J matrix
        JTy         - (output) J^T y vector
        w           - workspace
*/

static inline int
mfield_jacobian_row(const double t, const size_t flags, const double weight,
                    gsl_vector * dB_int, const double y, const size_t extidx,
                    const double dB_ext, const size_t euler_idx, const double B_nec_alpha,
                    const double B_nec_beta, const double B_nec_gamma,
                    gsl_matrix *JTJ, gsl_vector *JTy, const mfield_workspace *w)
{
  const double sWy = sqrt(weight) * y;
  gsl_vector_view g_mf = gsl_vector_subvector(dB_int, 0, w->nnm_mf);
#if MFIELD_FIT_SECVAR
  gsl_vector_view g_sv = gsl_vector_subvector(dB_int, 0, w->nnm_sv);
#endif
#if MFIELD_FIT_SECACC
  gsl_vector_view g_sa = gsl_vector_subvector(dB_int, 0, w->nnm_sa);
#endif

  /* check if fitting MF to this data point */
  if (MAGDATA_FitMF(flags))
    {
      gsl_vector_view v;

      /* update J^T y */
      v = gsl_vector_subvector(JTy, 0, w->nnm_mf);
      gsl_blas_daxpy(sWy, &g_mf.vector, &v.vector);

#if MFIELD_FIT_SECVAR
      v = gsl_vector_subvector(JTy, w->sv_offset, w->nnm_sv);
      gsl_blas_daxpy(t * sWy, &g_sv.vector, &v.vector);
#endif

#if MFIELD_FIT_SECACC
      v = gsl_vector_subvector(JTy, w->sa_offset, w->nnm_sa);
      gsl_blas_daxpy(0.5 * t * t * sWy, &g_sa.vector, &v.vector);
#endif

#if MFIELD_FIT_EXTFIELD
      {
        double *ptr33 = gsl_matrix_ptr(JTJ, extidx, extidx);
        double *ptr = gsl_vector_ptr(JTy, extidx);

        /* update J^T y */
        *ptr += dB_ext * sWy;

        /* update (J^T J)_33 */
        *ptr33 += dB_ext * dB_ext * weight;

        /* update (J^T J)_31 = J_ext^T J_int */
        v = gsl_matrix_subrow(JTJ, extidx, 0, w->nnm_mf);
        gsl_blas_daxpy(dB_ext * weight, &g_mf.vector, &v.vector);

#if MFIELD_FIT_SECVAR
        v = gsl_matrix_subrow(JTJ, extidx, w->sv_offset, w->nnm_sv);
        gsl_blas_daxpy(t * dB_ext * weight, &g_sv.vector, &v.vector);
#endif

#if MFIELD_FIT_SECACC
        v = gsl_matrix_subrow(JTJ, extidx, w->sa_offset, w->nnm_sa);
        gsl_blas_daxpy(0.5 * t * t * dB_ext * weight, &g_sa.vector, &v.vector);
#endif
      }
#endif /* MFIELD_FIT_EXTFIELD */
    }

#if MFIELD_FIT_EULER
  /* check if fitting Euler angles to this data point */
  if (MAGDATA_FitEuler(flags))
    {
      double x_data[3];
      gsl_vector_view vJTy = gsl_vector_subvector(JTy, euler_idx, 3);
      gsl_vector_view v = gsl_vector_view_array(x_data, 3);
      gsl_matrix_view m;

      x_data[0] = -B_nec_alpha;
      x_data[1] = -B_nec_beta;
      x_data[2] = -B_nec_gamma;

      /* update J^T y */
      gsl_blas_daxpy(sWy, &v.vector, &vJTy.vector);

      /* update (J^T J)_22 */
      m = gsl_matrix_submatrix(JTJ, euler_idx, euler_idx, 3, 3);
      gsl_blas_dsyr(CblasLower, weight, &v.vector, &m.matrix);

      if (MAGDATA_FitMF(flags))
        {
          /* update (J^T J)_21 */

          m = gsl_matrix_submatrix(JTJ, euler_idx, 0, 3, w->nnm_mf);
          gsl_blas_dger(weight, &v.vector, &g_mf.vector, &m.matrix);

#if MFIELD_FIT_SECVAR
          m = gsl_matrix_submatrix(JTJ, euler_idx, w->sv_offset, 3, w->nnm_sv);
          gsl_blas_dger(t * weight, &v.vector, &g_sv.vector, &m.matrix);
#endif

#if MFIELD_FIT_SECACC
          m = gsl_matrix_submatrix(JTJ, euler_idx, w->sa_offset, 3, w->nnm_sa);
          gsl_blas_dger(0.5 * t * t * weight, &v.vector, &g_sa.vector, &m.matrix);
#endif

#if MFIELD_FIT_EXTFIELD
          /* update (J^T J)_32 */
          {
            gsl_vector_view v32 = gsl_matrix_subrow(JTJ, extidx, euler_idx, 3);
            gsl_blas_daxpy(dB_ext * weight, &v.vector, &v32.vector);
          }
#endif
        }
    }
#endif

  return GSL_SUCCESS;
}

/*
mfield_jacobian_row_F()
  Construct a row of the Jacobian matrix corresponding to
a scalar measurement and update J^T J matrix and J^T y
vector

Inputs: t           - scaled timestamp
        flags       - MAGDATA_FLG_xxx flags for this data point
        weight      - weight for this data point
        y           - element of y vector corresponding to this row
                      for updating J^T y
        dX          - Green's functions for X component
        dY          - Green's functions for Y component
        dZ          - Green's functions for Z component
        B_model     - total model vector
                      B_model[0] = X model
                      B_model[1] = Y model
                      B_model[2] = Z model
                      B_model[3] = F model
        extidx      - index of external field coefficient
        dB_ext      - external field vector Green's functions
        J_int       - (output) row of Jacobian (weighted) for internal
                               Green's functions, p_int-by-1
        JTJ         - (output) updated J^T W J matrix
        JTy         - (output) updated J^T sqrt(W) y vector
        w           - workspace
*/

static inline int
mfield_jacobian_row_F(const double t, const double y, const double weight,
                      gsl_vector * dX, gsl_vector * dY, gsl_vector * dZ,
                      const double B_model[4], const size_t extidx, const double dB_ext[3],
                      gsl_vector *J_int, gsl_matrix *JTJ, gsl_vector *JTy,
                      const mfield_workspace *w)
{
  const double sqrt_weight = sqrt(weight);
  size_t k;
  double b[3];

  /* compute unit vector in model direction */
  for (k = 0; k < 3; ++k)
    b[k] = B_model[k] / B_model[3];

  /* compute (X dX + Y dY + Z dZ) */
  for (k = 0; k < w->nnm_mf; ++k)
    {
      double dXk = gsl_vector_get(dX, k);
      double dYk = gsl_vector_get(dY, k);
      double dZk = gsl_vector_get(dZ, k);
      double val = sqrt_weight * (b[0] * dXk +
                                  b[1] * dYk +
                                  b[2] * dZk);

      mfield_set_mf(J_int, k, val, w);
      mfield_set_sv(J_int, k, t * val, w);
      mfield_set_sa(J_int, k, 0.5 * t * t * val, w);
    }

  /* update J^T y */
  {
    gsl_vector_view vJTy = gsl_vector_subvector(JTy, 0, w->p_int);
    gsl_blas_daxpy(y, J_int, &vJTy.vector);
  }

#if MFIELD_FIT_EXTFIELD
  {
    double *ptr = gsl_vector_ptr(JTy, extidx);
    gsl_vector_view v31 = gsl_matrix_subrow(JTJ, extidx, 0, w->p_int);
    double *ptr33 = gsl_matrix_ptr(JTJ, extidx, extidx);
    double val = sqrt_weight * (b[0] * dB_ext[0] +
                                b[1] * dB_ext[1] +
                                b[2] * dB_ext[2]);

    /* update J^T y */
    *ptr += val * y;

    /* update (J^T J)_33 */
    *ptr33 += val * val;

    /* update (J^T J)_31 */
    gsl_blas_daxpy(val, J_int, &v31.vector);
  }
#endif

  return GSL_SUCCESS;
}

/*
mfield_nonlinear_matrices()
  Precompute matrices to evaluate residual vector and Jacobian
quickly in calc_f and calc_df

The ndata-by-nnm matrices computed are:

[dX] = dX/dg
[dY] = dY/dg
[dZ] = dZ/dg

Inputs: dX    - (output) dX/dg (main field)
        dY    - (output) dY/dg (main field)
        dZ    - (output) dZ/dg (main field)
*/

static int
mfield_nonlinear_matrices(gsl_matrix *dX, gsl_matrix *dY,
                          gsl_matrix *dZ, mfield_workspace *w)
{
  int s = GSL_SUCCESS;
  size_t i, j;
  size_t idx = 0;

  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);
      size_t n;
      int m;

      for (j = 0; j < mptr->n; ++j)
        {
          double r = mptr->r[j];
          double theta = mptr->theta[j];
          double phi = mptr->phi[j];

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          /* compute basis functions for spherical harmonic expansions */
          mfield_green(r, theta, phi, w);

          for (n = 1; n <= w->nmax_mf; ++n)
            {
              int ni = (int) n;

              for (m = -ni; m <= ni; ++m)
                {
                  size_t cidx = mfield_coeff_nmidx(n, m);

                  gsl_matrix_set(dX, idx, cidx, w->dX[cidx]);
                  gsl_matrix_set(dY, idx, cidx, w->dY[cidx]);
                  gsl_matrix_set(dZ, idx, cidx, w->dZ[cidx]);
                }
            }

          ++idx;
        } /* for (j = 0; j < mptr->n; ++j) */
    } /* for (i = 0; i < w->nsat; ++i) */

  assert(idx == dX->size1);

  return s;
} /* mfield_nonlinear_matrices() */

/*
mfield_nonlinear_matrices2()
  Precompute matrices to evaluate residual vector and Jacobian
quickly in calc_fdf

The ndata-by-nnm_mf matrices computed are:

[dX] = dX/dg
[dY] = dY/dg
[dZ] = dZ/dg

Inputs: w - workspace

Notes:
1) w->block_{dX,dY,dZ} are used to construct row blocks
of matrices, which are then written to disk
*/

static int
mfield_nonlinear_matrices2(mfield_workspace *w)
{
  int s = GSL_SUCCESS;
  size_t i, j;
  size_t idx = 0;
  size_t didx = 0;
  double percent_done = 0.2;

  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);
      size_t n;
      int m;

      for (j = 0; j < mptr->n; ++j)
        {
          double r = mptr->r[j];
          double theta = mptr->theta[j];
          double phi = mptr->phi[j];

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          if ((double) didx++ / (double) w->ndata > percent_done)
            {
              fprintf(stderr, "%g%%...", percent_done * 100.0);
              percent_done += 0.2;
            }

          /* compute basis functions for spherical harmonic expansions */
          mfield_green(r, theta, phi, w);

          for (n = 1; n <= w->nmax_mf; ++n)
            {
              int ni = (int) n;

              for (m = -ni; m <= ni; ++m)
                {
                  size_t cidx = mfield_coeff_nmidx(n, m);

                  gsl_matrix_set(w->block_dX, idx, cidx, w->dX[cidx]);
                  gsl_matrix_set(w->block_dY, idx, cidx, w->dY[cidx]);
                  gsl_matrix_set(w->block_dZ, idx, cidx, w->dZ[cidx]);
                }
            }

          if (++idx == w->data_block)
            {
              /* write this block of rows to disk */
              gsl_matrix_fwrite(w->fp_dX, w->block_dX);
              gsl_matrix_fwrite(w->fp_dY, w->block_dY);
              gsl_matrix_fwrite(w->fp_dZ, w->block_dZ);
              idx = 0;
            }
        } /* for (j = 0; j < mptr->n; ++j) */
    } /* for (i = 0; i < w->nsat; ++i) */

  /* check for final partial block and write to disk */
  if (idx > 0)
    {
      gsl_matrix_view m;

      m = gsl_matrix_submatrix(w->block_dX, 0, 0, idx, w->nnm_mf);
      gsl_matrix_fwrite(w->fp_dX, &m.matrix);

      m = gsl_matrix_submatrix(w->block_dY, 0, 0, idx, w->nnm_mf);
      gsl_matrix_fwrite(w->fp_dY, &m.matrix);

      m = gsl_matrix_submatrix(w->block_dZ, 0, 0, idx, w->nnm_mf);
      gsl_matrix_fwrite(w->fp_dZ, &m.matrix);
    }

  return s;
} /* mfield_nonlinear_matrices() */

/*
mfield_nonlinear_vector_precompute()
  Precompute J_int^T W J_int for vector measurements, since
this submatrix is independent of the model parameters and
only needs to be computed once per iteration.

Inputs: w - workspace

Notes:
1) w->JTJ_vec is updated with J_int^T W J_int for vector
residuals
*/

static int
mfield_nonlinear_vector_precompute(const gsl_vector *weights, mfield_workspace *w)
{
  int s = GSL_SUCCESS;
  size_t i, j;
  size_t idx = 0;
  size_t ridx = 0;   /* index of residual in [0:nres-1] */
  size_t dbidx = 0;  /* index of data point in current block, [0:data_block-1] */
  size_t nblock = 0; /* number of row blocks processed */

  gsl_matrix_set_zero(w->JTJ_vec);

  /* check for quick return */
  if (w->nres_vec == 0)
    return GSL_SUCCESS;

  /*
   * read first block of internal Green's functions from disk,
   * stored in w->block_{dX,dY,dZ}
   */
  mfield_read_matrix_block(nblock, w);

  for (i = 0; i < w->nsat; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

      for (j = 0; j < mptr->n; ++j)
        {
          double t = mptr->ts[j];
          gsl_vector_view vx, vy, vz;

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          vx = gsl_matrix_row(w->block_dX, dbidx);
          vy = gsl_matrix_row(w->block_dY, dbidx);
          vz = gsl_matrix_row(w->block_dZ, dbidx);

          if (mptr->flags[j] & MAGDATA_FLG_X)
            {
              double wj = gsl_vector_get(weights, ridx++);
              if (MAGDATA_FitMF(mptr->flags[j]))
                {
                  gsl_vector_view v = gsl_matrix_subrow(w->block_J, idx++, 0, w->p_int);
                  mfield_vector_green(t, wj, &vx.vector, &v.vector, w);
                }
            }

          if (mptr->flags[j] & MAGDATA_FLG_Y)
            {
              double wj = gsl_vector_get(weights, ridx++);
              if (MAGDATA_FitMF(mptr->flags[j]))
                {
                  gsl_vector_view v = gsl_matrix_subrow(w->block_J, idx++, 0, w->p_int);
                  mfield_vector_green(t, wj, &vy.vector, &v.vector, w);
                }
            }

          if (mptr->flags[j] & MAGDATA_FLG_Z)
            {
              double wj = gsl_vector_get(weights, ridx++);
              if (MAGDATA_FitMF(mptr->flags[j]))
                {
                  gsl_vector_view v = gsl_matrix_subrow(w->block_J, idx++, 0, w->p_int);
                  mfield_vector_green(t, wj, &vz.vector, &v.vector, w);
                }
            }

          if (MAGDATA_ExistScalar(mptr->flags[j]) &&
              MAGDATA_FitMF(mptr->flags[j]))
            {
              /* update ridx */
              ++ridx;
            }

          if (idx >= w->data_block - 3)
            {
              /* accumulate vector Green's functions into JTJ_vec */
              gsl_matrix_view m = gsl_matrix_submatrix(w->block_J, 0, 0, idx, w->p_int);
              gsl_blas_dsyrk(CblasLower, CblasTrans, 1.0, &m.matrix, 1.0, w->JTJ_vec);
              idx = 0;
            }

          if (++dbidx == w->data_block)
            {
              /* reset for new block of observations */
              dbidx = 0;

              /* read next block of internal Green's functions from disk */
              ++nblock;
              mfield_read_matrix_block(nblock, w);
            }
        } /* for (j = 0; j < mptr->n; ++j) */
    } /* for (i = 0; i < w->nsat; ++i) */

  if (idx > 0)
    {
      /* accumulate final Green's functions into JTJ_vec */
      gsl_matrix_view m = gsl_matrix_submatrix(w->block_J, 0, 0, idx, w->p_int);
      gsl_blas_dsyrk(CblasLower, CblasTrans, 1.0, &m.matrix, 1.0, w->JTJ_vec);
    }

  assert(ridx == w->nres);

  return s;
} /* mfield_nonlinear_vector_precompute() */

/*
mfield_vector_green()
  Function to compute sqrt(w) [ J_mf J_sv J_sa ] for a given set of
vector Green's functions

Inputs: t      - scaled timestamp
        weight - weight for this measurement
        g      - vector Green's functions J_mf, size nnm_mf
        G      - (output) combined vector G = sqrt(w) [ g ; t*g ; 0.5*t*t*g ],
                 size w->p_int
        w      - workspace
*/

static int
mfield_vector_green(const double t, const double weight, const gsl_vector *g,
                    gsl_vector *G, mfield_workspace *w)
{
  const double sqrt_weight = sqrt(weight);
  size_t i;

  /* form G */
  for (i = 0; i < w->nnm_mf; ++i)
    {
      double gi = sqrt_weight * gsl_vector_get(g, i);

      mfield_set_mf(G, i, gi, w);
      mfield_set_sv(G, i, t * gi, w);
      mfield_set_sa(G, i, 0.5 * t * t * gi, w);
    }

  return GSL_SUCCESS;
}

/*
mfield_read_matrix_block()
  Read a block of rows from the dX, dY, dZ matrices
previously stored to disk

Inputs: nblock - which block to read, [0:max_block-1], where
                 max_block = ndata / data_block
        w      - workspace

Return: success/error

Notes:
1) This function is designed to be called in-order with successive blocks,
nblock = 0, 1, 2, ...

2) The current block is stored in w->block_{dX,dY,dZ}
*/

static int
mfield_read_matrix_block(const size_t nblock, mfield_workspace *w)
{
  /* number of rows left in file */
  const size_t nleft = w->ndata - nblock * w->data_block;

  /* number of rows in current block */
  const size_t nrows = GSL_MIN(nleft, w->data_block);

  /* check for quick return */
  if (nrows == 0)
    return GSL_SUCCESS;

  gsl_matrix_view mx = gsl_matrix_submatrix(w->block_dX, 0, 0, nrows, w->nnm_mf);
  gsl_matrix_view my = gsl_matrix_submatrix(w->block_dY, 0, 0, nrows, w->nnm_mf);
  gsl_matrix_view mz = gsl_matrix_submatrix(w->block_dZ, 0, 0, nrows, w->nnm_mf);

  if (nblock == 0)
    {
      /* rewind file pointers to beginning of file */
      fseek(w->fp_dX, 0L, SEEK_SET);
      fseek(w->fp_dY, 0L, SEEK_SET);
      fseek(w->fp_dZ, 0L, SEEK_SET);
    }

  gsl_matrix_fread(w->fp_dX, &mx.matrix);
  gsl_matrix_fread(w->fp_dY, &my.matrix);
  gsl_matrix_fread(w->fp_dZ, &mz.matrix);

  return GSL_SUCCESS;
}

/*
mfield_nonlinear_model_int()
  Evaluate internal field model for a given coefficient vector

Inputs: t - scaled time
        v - vector of basis functions (dX/dg,dY/dg,dZ/dg)
        g - model coefficients
        w - workspace

Return: model = v . g_mf + t*(v . g_sv) + 1/2*t^2*(v . g_sa)
*/

static double
mfield_nonlinear_model_int(const double t, const gsl_vector *v,
                           const gsl_vector *g, mfield_workspace *w)
{
  gsl_vector_const_view gmf = gsl_vector_const_subvector(g, 0, w->nnm_mf);
  gsl_vector_const_view vmf = gsl_vector_const_subvector(v, 0, w->nnm_mf);
  double mf, sv = 0.0, sa = 0.0, val;

  /* compute v . x_mf */
  gsl_blas_ddot(&vmf.vector, &gmf.vector, &mf);

#if MFIELD_FIT_SECVAR
  {
    /* compute v . x_sv */
    gsl_vector_const_view gsv = gsl_vector_const_subvector(g, w->sv_offset, w->nnm_sv);
    gsl_vector_const_view vsv = gsl_vector_const_subvector(v, 0, w->nnm_sv);
    gsl_blas_ddot(&vsv.vector, &gsv.vector, &sv);
  }
#endif

#if MFIELD_FIT_SECACC
  {
    /* compute v . x_sa */
    gsl_vector_const_view gsa = gsl_vector_const_subvector(g, w->sa_offset, w->nnm_sa);
    gsl_vector_const_view vsa = gsl_vector_const_subvector(v, 0, w->nnm_sa);
    gsl_blas_ddot(&vsa.vector, &gsa.vector, &sa);
  }
#endif

  val = mf + t * sv + 0.5 * t * t * sa;

  return val;
}

/*
mfield_nonlinear_model_ext()
  Compute external field model:

r_k * (0.7 * external_dipole + 0.3 * internal_dipole)

where the dipole coefficients are aligned with the
direction of the main field dipole. r_k is the strength
of the ring current for a given day (k = doy) but is not
incorporated into the output of this function. This function
outputs the coefficient of r_k, ie, the "green's function" part.

Inputs: r     - radius (km)
        theta - colatitude (radians)
        phi   - longitude (radians)
        g     - model coefficients
        dB    - (output) external field model green's functions
                dB[0] = X component of external field
                dB[1] = Y component of external field
                dB[2] = Z component of external field
        w     - workspace
*/

static int
mfield_nonlinear_model_ext(const double r, const double theta,
                           const double phi, const gsl_vector *g,
                           double dB[3], mfield_workspace *w)
{
#if MFIELD_FIT_EXTFIELD

  int s = 0;
  double g10 = gsl_vector_get(g, mfield_coeff_nmidx(1, 0));
  double g11 = gsl_vector_get(g, mfield_coeff_nmidx(1, 1));
  double h11 = gsl_vector_get(g, mfield_coeff_nmidx(1, -1));
  double g1 = gsl_hypot3(g10, g11, h11);
  double q[3];
  mfield_green_workspace *green_p = mfield_green_alloc(1, w->R);

  /* construct unit vector along internal dipole direction */
  q[mfield_coeff_nmidx(1, 0)] = g10 / g1;
  q[mfield_coeff_nmidx(1, 1)] = g11 / g1;
  q[mfield_coeff_nmidx(1, -1)] = h11 / g1;

  /* compute internal and external dipole field components */
  mfield_green_calc(r, theta, phi, green_p);
  mfield_green_ext(r, theta, phi, green_p);

  /* add external and induced sums */
  dB[0] = 0.7*vec_dot(q, green_p->dX_ext) + 0.3*vec_dot(q, green_p->dX);
  dB[1] = 0.7*vec_dot(q, green_p->dY_ext) + 0.3*vec_dot(q, green_p->dY);
  dB[2] = 0.7*vec_dot(q, green_p->dZ_ext) + 0.3*vec_dot(q, green_p->dZ);

  mfield_green_free(green_p);

  return s;

#else
  
  dB[0] = dB[1] = dB[2] = 0.0;
  return 0;

#endif

} /* mfield_nonlinear_model_ext() */

/*
mfield_nonlinear_histogram()
  Print residual histogram

Inputs: c - scaled/dimensionless coefficient vector
        w - workspace

Notes:
1) w->wts_final must be initialized prior to calling this function
*/

static int
mfield_nonlinear_histogram(const gsl_vector *c, mfield_workspace *w)
{
  int s = 0;
  FILE *fp;
  char filename[2048];

  /* reset histograms */
  gsl_histogram_reset(w->hf);
  gsl_histogram_reset(w->hz);

  /* loop through data and construct residual histograms */
  mfield_calc_f(c, w, NULL);

  /* scale histograms */
  gsl_histogram_scale(w->hf, 1.0 / gsl_histogram_sum(w->hf));
  gsl_histogram_scale(w->hz, 1.0 / gsl_histogram_sum(w->hz));

  /* print histograms to file */

  sprintf(filename, "reshistF.nlin.iter%zu.dat", w->niter);
  fprintf(stderr, "mfield_nonlinear_histogram: writing %s...", filename);
  fp = fopen(filename, "w");
  mfield_print_histogram(fp, w->hf);
  fclose(fp);
  fprintf(stderr, "done\n");

  sprintf(filename, "reshistZ.nlin.iter%zu.dat", w->niter);
  fprintf(stderr, "mfield_nonlinear_histogram: writing %s...", filename);
  fp = fopen(filename, "w");
  mfield_print_histogram(fp, w->hz);
  fclose(fp);
  fprintf(stderr, "done\n");

  return s;
} /* mfield_nonlinear_histogram() */

/*
mfield_nonlinear_regularize()
  Construct diag = diag(L) for regularized fit using frozen
flux assumption of Gubbins, 1983
*/

static int
mfield_nonlinear_regularize(gsl_vector *diag, mfield_workspace *w)
{
  int s = 0;
  const size_t nmin = 9;
  const size_t nmax = w->nmax_mf;
  const double c = 3485.0;       /* Earth core radius */
  const double a = MFIELD_RE_KM; /* Earth surface radius */
  const double ratio = a / c;
  size_t n;
  int m;
  double lambda_mf = w->lambda_mf;
  double lambda_sv = w->lambda_sv;
  double lambda_sa = w->lambda_sa;

  for (n = 1; n <= nmax; ++n)
    {
      int ni = (int) n;
      double term = (n + 1.0) / sqrt(2.0*n + 1.0) * pow(ratio, n + 2.0);

      for (m = -ni; m <= ni; ++m)
        {
          size_t cidx = mfield_coeff_nmidx(n, m);

          mfield_set_mf(diag, cidx, lambda_mf, w);

          if (n >= nmin)
            {
              mfield_set_sv(diag, cidx, lambda_sv, w);
              mfield_set_sa(diag, cidx, lambda_sa * term, w);
            }
          else
            {
              mfield_set_sv(diag, cidx, lambda_mf, w);
              mfield_set_sa(diag, cidx, lambda_mf, w);
            }
        }
    }

  return s;
} /* mfield_nonlinear_regularize() */

static void
mfield_nonlinear_callback(const size_t iter, void *params,
                          const gsl_multifit_nlinear_workspace *multifit_p)
{
  mfield_workspace *w = (mfield_workspace *) params;
  gsl_vector *x = gsl_multifit_nlinear_position(multifit_p);
  gsl_vector *f = gsl_multifit_nlinear_residual(multifit_p);
  double avratio = gsl_multifit_nlinear_avratio(multifit_p);
  double rcond;

  /* print out state every 5 iterations */
  if (iter % 5 != 0 && iter != 1)
    return;

  fprintf(stderr, "iteration %zu:\n", iter);

  fprintf(stderr, "\t dipole: %12.4f %12.4f %12.4f [nT]\n",
          gsl_vector_get(x, mfield_coeff_nmidx(1, 0)),
          gsl_vector_get(x, mfield_coeff_nmidx(1, 1)),
          gsl_vector_get(x, mfield_coeff_nmidx(1, -1)));

#if MFIELD_FIT_EULER
  {
    size_t i;

    for (i = 0; i < w->nsat; ++i)
      {
        magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

        if (mptr->n == 0)
          continue;

        if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
          {
            double t0 = w->data_workspace_p->t0[i];
            size_t euler_idx = mfield_euler_idx(i, t0, w);

            fprintf(stderr, "\t euler : %12.4f %12.4f %12.4f [deg]\n",
                    gsl_vector_get(x, euler_idx) * 180.0 / M_PI,
                    gsl_vector_get(x, euler_idx + 1) * 180.0 / M_PI,
                    gsl_vector_get(x, euler_idx + 2) * 180.0 / M_PI);
          }
      }
  }
#endif

  fprintf(stderr, "\t |a|/|v|:    %12g\n", avratio);
  fprintf(stderr, "\t ||f(x)||:   %12g\n", gsl_blas_dnrm2(f));

  gsl_multifit_nlinear_rcond(&rcond, multifit_p);
  fprintf(stderr, "\t cond(J(x)): %12g\n", 1.0 / rcond);
}

static void
mfield_nonlinear_callback2(const size_t iter, void *params,
                           const gsl_multilarge_nlinear_workspace *multilarge_p)
{
  mfield_workspace *w = (mfield_workspace *) params;
  gsl_vector *x = gsl_multilarge_nlinear_position(multilarge_p);
  gsl_vector *f = gsl_multilarge_nlinear_residual(multilarge_p);
  double rcond;

  /* print out state every 5 iterations */
  if (iter % 5 != 0 && iter != 1)
    return;

  fprintf(stderr, "iteration %zu:\n", iter);

  fprintf(stderr, "\t dipole: %12.4f %12.4f %12.4f [nT]\n",
          gsl_vector_get(x, mfield_coeff_nmidx(1, 0)),
          gsl_vector_get(x, mfield_coeff_nmidx(1, 1)),
          gsl_vector_get(x, mfield_coeff_nmidx(1, -1)));

#if MFIELD_FIT_EULER
  {
    size_t i;

    for (i = 0; i < w->nsat; ++i)
      {
        magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);

        if (mptr->n == 0)
          continue;

        if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
          {
            double t0 = w->data_workspace_p->t0[i];
            size_t euler_idx = mfield_euler_idx(i, t0, w);

            fprintf(stderr, "\t euler : %12.4f %12.4f %12.4f [deg]\n",
                    gsl_vector_get(x, euler_idx) * 180.0 / M_PI,
                    gsl_vector_get(x, euler_idx + 1) * 180.0 / M_PI,
                    gsl_vector_get(x, euler_idx + 2) * 180.0 / M_PI);
          }
      }
  }
#endif

  fprintf(stderr, "\t ||f(x)||:   %12g\n", gsl_blas_dnrm2(f));

  gsl_multilarge_nlinear_rcond(&rcond, multilarge_p);
  fprintf(stderr, "\t cond(J(x)): %12g\n", 1.0 / rcond);
}
