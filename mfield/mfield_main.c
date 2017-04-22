/*
 * mfield_main.c
 *
 * Usage:
 * ./mfield [flags]
 *
 * Flags:
 *   -c coef_output_file
 *   -n max_iterations
 *   -e epoch_decimal_year
 *   -v lambda_sv
 *   -a lambda_sa
 *   -p euler_period_days
 *   -r residual_file
 *   -l Lcurve_data_file
 *
 * After each iteration, the file 'res.#.dat' is written
 * where # is the iteration number. This file contains the
 * residuals of a sample of the DMSP dataset.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <math.h>
#include <assert.h>
#include <omp.h>
#include <libconfig.h>

#include <gsl/gsl_math.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_histogram.h>
#include <gsl/gsl_sf_legendre.h>
#include <gsl/gsl_statistics.h>
#include <gsl/gsl_rstat.h>

#include <satdata/satdata.h>

#include "common.h"
#include "euler.h"
#include "oct.h"
#include "magdata.h"
#include "mfield.h"
#include "mfield_synth.h"
#include "msynth.h"
#include "track.h"

#define MAX_BUFFER           2048

#if 0

/*
parse_input()
  Read in index file for a given satellite, perform data
selection and downsampling
*/

satdata_mag *
parse_input(const size_t sat_idx)
{
  satdata_mag *data = NULL;
  const char *idxfile = index_files[sat_idx];
  size_t i;

  fprintf(stderr, "parse_input: reading %s...", idxfile);

  if (sat_idx >= IDX_SWA && sat_idx <= IDX_SWC)
    data = satdata_swarm_read_idx(idxfile, 0);
  else if (sat_idx == IDX_CHAMP)
    data = satdata_champ_read_idx(idxfile, 0);
  else if (sat_idx == IDX_OERSTED)
    data = satdata_oersted_read_idx(idxfile, 0);
  else if (sat_idx >= IDX_F15 && sat_idx <= IDX_F18)
    data = satdata_dmsp_read_idx(idxfile, 0);

  if (!data)
    return NULL;

  fprintf(stderr, "done (%zu points read)\n", data->n);

  if (sat_idx >= IDX_SWA && sat_idx <= IDX_SWC)
    {
      size_t nrms;
      track_workspace *track_p = track_alloc();
      double thresh[] = { 20.0, 25.0, 15.0, 15.0 };

      satdata_swarm_filter_instrument(1, data);

      /* filter by track rms */

      track_init(data, NULL, track_p);

      nrms = track_flag_rms("swarm_rms.dat", thresh, data, track_p);
      fprintf(stderr, "parse_input: flagged (%zu/%zu) (%.1f%%) points due to high rms\n",
              nrms, data->n, (double) nrms / (double) data->n * 100.0);

      track_free(track_p);

      satdata_filter_wmm(1, data);
    }

  fprintf(stderr, "parse_input: downsampling data by factor %d...", DOWNSAMPLE);

  for (i = 0; i < data->n; ++i)
    {
      if (i % DOWNSAMPLE != 0)
        data->flags[i] |= SATDATA_FLG_OUTLIER;
    }

  fprintf(stderr, "done\n");

  /* flag local time */
  if (!(sat_idx >= IDX_F15 && sat_idx <= IDX_F18))
    {
      size_t nlt;
      const double lt_min = 5.0;
      double lt_max = 22.0;
      const double euler_lt_min = 6.0;
      const double euler_lt_max = 18.0;

      /* in the first half of 2013, Oersted is in a ~10am/10pm orbit */
      if (sat_idx == IDX_OERSTED)
        lt_max = 20.0;

      fprintf(stderr, "parse_input: flagging points inside LT window [%g,%g], euler [%g,%g]...",
              lt_min, lt_max, euler_lt_min, euler_lt_max);

      nlt = flag_local_time(lt_min, lt_max, euler_lt_min, euler_lt_max, data);

      fprintf(stderr, "done (%zu/%zu data flagged)\n", nlt, data->n);
    }

  {
    size_t nflagged = satdata_nflagged(data);
    fprintf(stderr, "parse_input: total flagged points: %zu/%zu (%.1f%%) (%zu remaining)\n",
            nflagged, data->n, (double)nflagged / (double)data->n * 100.0,
            data->n - nflagged);
  }

  return data;
} /* parse_input() */

#endif /* 0 */

/*
initial_guess()
  Construct initial guess for main field coefficients. These
are based on the relevant IGRF coefficients, extrapolated forward
to the desired epoch using the SV coefficients. Initial SA coefficients
are set to 0.
*/

int
initial_guess(gsl_vector *c, mfield_workspace *w)
{
  gsl_vector_set_zero(c);
  return 0; /*XXX*/

  {
    msynth_workspace *msynth_p = msynth_igrf_read(MSYNTH_IGRF_FILE);
    const size_t nmax = GSL_MIN(w->nmax_mf, msynth_p->nmax);
    const double t = w->epoch;                       /* desired epoch */
    const double t0 = msynth_get_epoch(t, msynth_p); /* IGRF epoch */
    const double dt = t - t0;
    size_t n;
    int m;

    for (n = 1; n <= nmax; ++n)
      {
        int ni = (int) n;

        for (m = -ni; m <= ni; ++m)
          {
            size_t midx = msynth_nmidx(n, m, msynth_p);
            size_t cidx = mfield_coeff_nmidx(n, m);
            double gnm = msynth_get_mf(t, midx, msynth_p);
            double dgnm = msynth_get_sv(t, midx, msynth_p);

            /*
             * use SV prediction to update main field coefficients for new
             * epoch
             */
            mfield_set_mf(c, cidx, gnm + dt * dgnm, w);
            mfield_set_sv(c, cidx, dgnm, w);
            mfield_set_sa(c, cidx, 0.0, w);
          }
      }

    msynth_free(msynth_p);
  }

  return 0;
} /* initial_guess() */

int
print_spectrum(const char *filename, mfield_workspace *w)
{
  const double c = 3485.0;               /* Earth core radius */
  const double ratio = MFIELD_RE_KM / c; /* a / c */
  size_t n;
  FILE *fp = fopen(filename, "w");

  n = 1;
  fprintf(fp, "# Field %zu: spherical harmonic degree n\n", n++);
  fprintf(fp, "# Field %zu: MF power R_n at Earth surface\n", n++);
  fprintf(fp, "# Field %zu: SV power R_n at Earth surface\n", n++);
  fprintf(fp, "# Field %zu: SA power R_n at Earth surface\n", n++);
  fprintf(fp, "# Field %zu: MF power R_n at CMB\n", n++);
  fprintf(fp, "# Field %zu: SV power R_n at CMB\n", n++);
  fprintf(fp, "# Field %zu: SA power R_n at CMB\n", n++);

  fprintf(stderr, "print_spectrum: writing spectrum to %s...", filename);
  for (n = 1; n <= w->nmax_mf; ++n)
    {
      double gn = mfield_spectrum(n, w);
      double dgn = mfield_spectrum_sv(n, w);
      double ddgn = mfield_spectrum_sa(n, w);
      double rterm = pow(ratio, 2.0*n + 4.0);

      fprintf(fp, "%zu %.12e %.12e %.12e %.12e %.12e %.12e\n",
              n,
              gn,
              dgn,
              ddgn,
              rterm * gn,
              rterm * dgn,
              rterm * ddgn);
    }
  fprintf(stderr, "done\n");

  fclose(fp);

  return 0;
} /* print_spectrum() */

int
mfield_print_residual(const char *prefix, const size_t iter, const gsl_vector *c, mfield_workspace *w)
{
  int s = 0;
  char buf[2048];
  FILE *fp[12];
  const size_t n = 12; /* number of components to write to disk */
  const char *fmtstr = "%ld %f %.4f %.4f %.4f %.4f %.3f %.4f\n";
  size_t i;
  mfield_data_workspace *data_p = w->data_workspace_p;
  size_t idx = 0;
  gsl_vector *f = mfield_residual(c, w);
  gsl_vector *wts_spatial = w->wts_spatial;

  for (i = 0; i < data_p->nsources; ++i)
    {
      magdata *mptr = mfield_data_ptr(i, data_p);
      size_t j, k;

      sprintf(buf, "%s/res%zu_X_iter%zu.dat", prefix, i, iter);
      fp[0] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_Y_iter%zu.dat", prefix, i, iter);
      fp[1] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_Z_iter%zu.dat", prefix, i, iter);
      fp[2] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_F_iter%zu.dat", prefix, i, iter);
      fp[3] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DX_NS_iter%zu.dat", prefix, i, iter);
      fp[4] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DY_NS_iter%zu.dat", prefix, i, iter);
      fp[5] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DZ_NS_iter%zu.dat", prefix, i, iter);
      fp[6] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DF_NS_iter%zu.dat", prefix, i, iter);
      fp[7] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DX_EW_iter%zu.dat", prefix, i, iter);
      fp[8] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DY_EW_iter%zu.dat", prefix, i, iter);
      fp[9] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DZ_EW_iter%zu.dat", prefix, i, iter);
      fp[10] = fopen(buf, "w");

      sprintf(buf, "%s/res%zu_DF_EW_iter%zu.dat", prefix, i, iter);
      fp[11] = fopen(buf, "w");

      fprintf(fp[0], "# X vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[1], "# Y vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[2], "# Z vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[3], "# F scalar residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[4], "# DX gradient (N/S) vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[5], "# DY gradient (N/S) vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[6], "# DZ gradient (N/S) vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[7], "# DZ gradient (N/S) scalar residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[8], "# DX gradient (E/W) vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[9], "# DY gradient (E/W) vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[10], "# DZ gradient (E/W) vector residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);
      fprintf(fp[11], "# DF gradient (E/W) scalar residuals for MF modeling (satellite %zu, iteration %zu)\n", i, iter);

      for (j = 0; j < n; ++j)
        {
          k = 1;
          fprintf(fp[j], "# Field %zu: timestamp (UT seconds since 1970-01-01)\n", k++);
          fprintf(fp[j], "# Field %zu: time (decimal year)\n", k++);
          fprintf(fp[j], "# Field %zu: longitude (degrees)\n", k++);
          fprintf(fp[j], "# Field %zu: geocentric latitude (degrees)\n", k++);
          fprintf(fp[j], "# Field %zu: QD latitude (degrees)\n", k++);
          fprintf(fp[j], "# Field %zu: geocentric radius (km)\n", k++);
          fprintf(fp[j], "# Field %zu: total weight factor\n", k++);
        }

      fprintf(fp[0], "# Field %zu: X vector residual (nT)\n", k);
      fprintf(fp[1], "# Field %zu: Y vector residual (nT)\n", k);
      fprintf(fp[2], "# Field %zu: Z vector residual (nT)\n", k);
      fprintf(fp[3], "# Field %zu: F vector residual (nT)\n", k);
      fprintf(fp[4], "# Field %zu: DX gradient (N/S) vector residual (nT)\n", k);
      fprintf(fp[5], "# Field %zu: DY gradient (N/S) vector residual (nT)\n", k);
      fprintf(fp[6], "# Field %zu: DZ gradient (N/S) vector residual (nT)\n", k);
      fprintf(fp[7], "# Field %zu: DF gradient (N/S) scalar residual (nT)\n", k);
      fprintf(fp[8], "# Field %zu: DX gradient (E/W) vector residual (nT)\n", k);
      fprintf(fp[9], "# Field %zu: DY gradient (E/W) vector residual (nT)\n", k);
      fprintf(fp[10], "# Field %zu: DZ gradient (E/W) vector residual (nT)\n", k);
      fprintf(fp[11], "# Field %zu: DF gradient (E/W) scalar residual (nT)\n", k);
      ++k;

      for (j = 0; j < mptr->n; ++j)
        {
          double t = satdata_epoch2year(mptr->t[j]);
          time_t unix_time = satdata_epoch2timet(mptr->t[j]);
          double phi = wrap180(mptr->phi[j] * 180.0 / M_PI);
          double lat = 90.0 - mptr->theta[j] * 180.0 / M_PI;
          double qdlat = mptr->qdlat[j];
          double r = mptr->r[j];

          if (MAGDATA_Discarded(mptr->flags[j]) || !MAGDATA_FitMF(mptr->flags[j]))
            continue;

          if (MAGDATA_ExistX(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[0], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistY(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[1], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistZ(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[2], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistScalar(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[3], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDX_NS(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[4], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDY_NS(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[5], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDZ_NS(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[6], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDF_NS(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[7], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDX_EW(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[8], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDY_EW(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[9], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDZ_EW(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[10], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }

          if (MAGDATA_ExistDF_EW(mptr->flags[j]))
            {
              double wj = gsl_vector_get(wts_spatial, idx);
              double resj = gsl_vector_get(f, idx++);
              fprintf(fp[11], fmtstr, unix_time, t, phi, lat, qdlat, r, wj, resj);
            }
        }
    }

  assert(idx == w->nres);

  for (i = 0; i < n; ++i)
    fclose(fp[i]);

  return s;
}

/*
print_residuals()
  Output residuals for each satellite, using data stored
in w->data_workspace_p and coefficients stored in w->c
*/

int
print_residuals(const char *filename, mfield_workspace *w)
{
  const double qdlat_cutoff = 55.0; /* cutoff latitude for high/low statistics */
  size_t i, j, k;
  gsl_rstat_workspace *rstat_x = gsl_rstat_alloc();
  gsl_rstat_workspace *rstat_y = gsl_rstat_alloc();
  gsl_rstat_workspace *rstat_z = gsl_rstat_alloc();
  gsl_rstat_workspace *rstat_f = gsl_rstat_alloc();
  gsl_rstat_workspace *rstat_lowz = gsl_rstat_alloc();
  gsl_rstat_workspace *rstat_highz = gsl_rstat_alloc();
  gsl_rstat_workspace *rstat_lowf = gsl_rstat_alloc();
  gsl_rstat_workspace *rstat_highf = gsl_rstat_alloc();

  for (i = 0; i < w->nsat; ++i)
    {
      const size_t nbins = 500;
      const double a = -100.0;
      const double b = 100.0;
      magdata *mptr = mfield_data_ptr(i, w->data_workspace_p);
      FILE *fp_res, *fp_hist;
      char fileres[2048];
      char filehist[2048];
      gsl_histogram *hf, *hz;

      if (mptr->n == 0)
        continue;

      hf = gsl_histogram_alloc(nbins);
      hz = gsl_histogram_alloc(nbins);
      gsl_histogram_set_ranges_uniform(hf, a, b);
      gsl_histogram_set_ranges_uniform(hz, a, b);

      gsl_rstat_reset(rstat_x);
      gsl_rstat_reset(rstat_y);
      gsl_rstat_reset(rstat_z);
      gsl_rstat_reset(rstat_f);
      gsl_rstat_reset(rstat_lowz);
      gsl_rstat_reset(rstat_highz);
      gsl_rstat_reset(rstat_lowf);
      gsl_rstat_reset(rstat_highf);

      sprintf(fileres, "%s.sat%zu", filename, i);
      fp_res = fopen(fileres, "w");
      if (!fp_res)
        {
          fprintf(stderr, "print_residuals: unable to open %s: %s\n",
                  fileres, strerror(errno));
          return GSL_FAILURE;
        }

      sprintf(filehist, "%s.sat%zu.hist", filename, i);
      fp_hist = fopen(filehist, "w");
      if (!fp_hist)
        {
          fprintf(stderr, "print_residuals: unable to open %s: %s\n",
                  filehist, strerror(errno));
          return GSL_FAILURE;
        }

      k = 1;
      fprintf(fp_res, "# Field %zu: time (years)\n", k++);
      fprintf(fp_res, "# Field %zu: local time (hours)\n", k++);
      fprintf(fp_res, "# Field %zu: season (day of year)\n", k++);
      fprintf(fp_res, "# Field %zu: altitude (km)\n", k++);
      fprintf(fp_res, "# Field %zu: longitude (deg)\n", k++);
      fprintf(fp_res, "# Field %zu: latitude (deg)\n", k++);
      fprintf(fp_res, "# Field %zu: QD latitude (deg)\n", k++);
      fprintf(fp_res, "# Field %zu: scalar residual (nT)\n", k++);
      fprintf(fp_res, "# Field %zu: X residual (nT)\n", k++);
      fprintf(fp_res, "# Field %zu: Y residual (nT)\n", k++);
      fprintf(fp_res, "# Field %zu: Z residual (nT)\n", k++);
      fprintf(fp_res, "# Field %zu: NEC X component (nT)\n", k++);
      fprintf(fp_res, "# Field %zu: NEC Y component (nT)\n", k++);
      fprintf(fp_res, "# Field %zu: NEC Z component (nT)\n", k++);
      fprintf(fp_res, "# Field %zu: satellite direction (+1 north -1 south)\n", k++);
      fprintf(fp_res, "# Field %zu: scalar data used in MF fitting (1 or 0)\n", k++);
      fprintf(fp_res, "# Field %zu: X data used in MF fitting (1 or 0)\n", k++);
      fprintf(fp_res, "# Field %zu: Y data used in MF fitting (1 or 0)\n", k++);
      fprintf(fp_res, "# Field %zu: Z data used in MF fitting (1 or 0)\n", k++);
      fprintf(fp_res, "# Field %zu: vector data used in Euler angle fitting (1 or 0)\n", k++);
      fprintf(fp_res, "# Field %zu: along-track gradient available (1 or 0)\n", k++);
      fprintf(fp_res, "# Field %zu: east-west gradient available (1 or 0)\n", k++);

      fprintf(stderr, "Writing residuals to %s...", fileres);

      for (j = 0; j < mptr->n; ++j)
        {
          double t = satdata_epoch2year(mptr->t[j]);
          double r = mptr->r[j];
          double theta = mptr->theta[j];
          double phi = mptr->phi[j];
          time_t unix_time = satdata_epoch2timet(mptr->t[j]);
          double lt = get_localtime(unix_time, phi);
          double B_nec[3], B_int[4], B_model[4];
          double B_ext[4] = { 0.0, 0.0, 0.0, 0.0 };
          double res[4];
          int fit_scal = MAGDATA_ExistScalar(mptr->flags[j]) && MAGDATA_FitMF(mptr->flags[j]);
          int fit_X = MAGDATA_ExistX(mptr->flags[j]) && MAGDATA_FitMF(mptr->flags[j]);
          int fit_Y = MAGDATA_ExistY(mptr->flags[j]) && MAGDATA_FitMF(mptr->flags[j]);
          int fit_Z = MAGDATA_ExistZ(mptr->flags[j]) && MAGDATA_FitMF(mptr->flags[j]);

          if (MAGDATA_Discarded(mptr->flags[j]))
            continue;

          mfield_eval(mptr->t[j], r, theta, phi, B_int, w);

          /* external field is fitted only to data points used for main field modeling */
          if ((MAGDATA_ExistX(mptr->flags[j]) || MAGDATA_ExistY(mptr->flags[j]) ||
               MAGDATA_ExistZ(mptr->flags[j]) || MAGDATA_ExistScalar(mptr->flags[j])) &&
              (MAGDATA_FitMF(mptr->flags[j])))
            {
              mfield_eval_ext(mptr->t[j], r, theta, phi, B_ext, w);
            }

          /* add apriori external/crustal fields to computed external field */
          B_ext[0] += mptr->Bx_model[j];
          B_ext[1] += mptr->By_model[j];
          B_ext[2] += mptr->Bz_model[j];

#if MFIELD_FIT_EULER
          if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
            {
              size_t euler_idx = mfield_euler_idx(i, mptr->t[j], w);
              double alpha = gsl_vector_get(w->c, euler_idx);
              double beta = gsl_vector_get(w->c, euler_idx + 1);
              double gamma = gsl_vector_get(w->c, euler_idx + 2);
              double *q = &(mptr->q[4*j]);

              B_nec[0] = mptr->Bx_vfm[j];
              B_nec[1] = mptr->By_vfm[j];
              B_nec[2] = mptr->Bz_vfm[j];

              /* rotate to NEC with computed Euler angles */
              euler_vfm2nec(EULER_FLG_ZYX, alpha, beta, gamma, q, B_nec, B_nec);
            }
          else
#endif
            {
              B_nec[0] = mptr->Bx_nec[j];
              B_nec[1] = mptr->By_nec[j];
              B_nec[2] = mptr->Bz_nec[j];
            }

          /* compute total modeled field */
          for (k = 0; k < 3; ++k)
            B_model[k] = B_int[k] + B_ext[k];

          B_model[3] = gsl_hypot3(B_model[0], B_model[1], B_model[2]);

          /* compute vector residuals in NEC frame */
          for (k = 0; k < 3; ++k)
            res[k] = B_nec[k] - B_model[k];

          /* compute scalar residual */
          res[3] = mptr->F[j] - B_model[3];

          if (fit_X)
            gsl_rstat_add(res[0], rstat_x);

          if (fit_Y)
            gsl_rstat_add(res[1], rstat_y);

          if (fit_Z)
            {
              gsl_rstat_add(res[2], rstat_z);
              gsl_histogram_increment(hz, res[2]);

              if (fabs(mptr->qdlat[j]) <= qdlat_cutoff)
                gsl_rstat_add(res[2], rstat_lowz);
              else
                gsl_rstat_add(res[2], rstat_highz);
            }

          if (fit_scal)
            {
              gsl_rstat_add(res[3], rstat_f);
              gsl_histogram_increment(hf, res[3]);

              if (fabs(mptr->qdlat[j]) <= qdlat_cutoff)
                gsl_rstat_add(res[3], rstat_lowf);
              else
                gsl_rstat_add(res[3], rstat_highf);
            }

          fprintf(fp_res, "%12.6f %6.3f %6.2f %7.3f %8.4f %8.4f %9.4f %8.2f %8.2f %8.2f %8.2f %9.2f %9.2f %9.2f %d %d %d %d %d %d %d %d\n",
                  t,
                  lt,
                  get_season(unix_time),
                  mptr->r[j] - 6371.2,
                  wrap180(phi * 180.0 / M_PI),
                  90.0 - theta * 180.0 / M_PI,
                  mptr->qdlat[j],
                  res[3],
                  res[0],
                  res[1],
                  res[2],
                  B_nec[0],
                  B_nec[1],
                  B_nec[2],
                  mptr->satdir[j],
                  fit_scal,
                  fit_X,
                  fit_Y,
                  fit_Z,
                  mptr->flags[j] & MAGDATA_FLG_FIT_EULER ? 1 : 0,
                  mptr->flags[j] & MAGDATA_FLG_DZ_NS ? 1 : 0,
                  mptr->flags[j] & MAGDATA_FLG_DZ_EW ? 1 : 0);
        }

      fprintf(stderr, "done\n");

      fprintf(stderr, "=== FIT STATISTICS SATELLITE %zu ===\n", i);

      fprintf(stderr, "%8s %10s %12s %12s %12s\n",
              "", "N", "mean (nT)", "sigma (nT)", "rms (nT)");

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "X",
              gsl_rstat_n(rstat_x),
              gsl_rstat_mean(rstat_x),
              gsl_rstat_sd(rstat_x),
              gsl_rstat_rms(rstat_x));

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "Y",
              gsl_rstat_n(rstat_y),
              gsl_rstat_mean(rstat_y),
              gsl_rstat_sd(rstat_y),
              gsl_rstat_rms(rstat_y));

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "Z",
              gsl_rstat_n(rstat_z),
              gsl_rstat_mean(rstat_z),
              gsl_rstat_sd(rstat_z),
              gsl_rstat_rms(rstat_z));

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "F",
              gsl_rstat_n(rstat_f),
              gsl_rstat_mean(rstat_f),
              gsl_rstat_sd(rstat_f),
              gsl_rstat_rms(rstat_f));

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "low Z",
              gsl_rstat_n(rstat_lowz),
              gsl_rstat_mean(rstat_lowz),
              gsl_rstat_sd(rstat_lowz),
              gsl_rstat_rms(rstat_lowz));

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "high Z",
              gsl_rstat_n(rstat_highz),
              gsl_rstat_mean(rstat_highz),
              gsl_rstat_sd(rstat_highz),
              gsl_rstat_rms(rstat_highz));

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "low F",
              gsl_rstat_n(rstat_lowf),
              gsl_rstat_mean(rstat_lowf),
              gsl_rstat_sd(rstat_lowf),
              gsl_rstat_rms(rstat_lowf));

      fprintf(stderr, "%8s %10zu %12.2f %12.2f %12.2f\n",
              "high F",
              gsl_rstat_n(rstat_highf),
              gsl_rstat_mean(rstat_highf),
              gsl_rstat_sd(rstat_highf),
              gsl_rstat_rms(rstat_highf));

      gsl_histogram_scale(hf, 1.0 / gsl_histogram_sum(hf));
      gsl_histogram_scale(hz, 1.0 / gsl_histogram_sum(hz));

      fprintf(stderr, "Writing histogram to %s...", filehist);
      for (k = 0; k < nbins; ++k)
        {
          double lower, upper;
          gsl_histogram_get_range(hf, k, &lower, &upper);
          fprintf(fp_hist, "%g %g %f %f\n",
                  lower, upper,
                  gsl_histogram_get(hf, k),
                  gsl_histogram_get(hz, k));
        }
      fprintf(stderr, "done\n");

      gsl_histogram_free(hf);
      gsl_histogram_free(hz);

      fclose(fp_res);
      fclose(fp_hist);
    }

  gsl_rstat_free(rstat_x);
  gsl_rstat_free(rstat_y);
  gsl_rstat_free(rstat_z);
  gsl_rstat_free(rstat_f);
  gsl_rstat_free(rstat_lowf);
  gsl_rstat_free(rstat_highf);

  return GSL_SUCCESS;
} /* print_residuals() */

static int
parse_config_file(const char *filename, mfield_parameters *mfield_params,
                  mfield_data_parameters *data_params)
{
  int s;
  config_t cfg;
  double fval;
  int ival;

  config_init(&cfg);

  s = config_read_file(&cfg, filename);
  if (s != CONFIG_TRUE)
    {
      fprintf(stderr, "parse_config_file: %s:%d - %s\n",
              config_error_file(&cfg),
              config_error_line(&cfg),
              config_error_text(&cfg));
      config_destroy(&cfg);
      return -1;
    }

  if (config_lookup_int(&cfg, "nmax_mf", &ival))
    mfield_params->nmax_mf = (size_t) ival;
  if (config_lookup_int(&cfg, "nmax_sv", &ival))
    mfield_params->nmax_sv = (size_t) ival;
  if (config_lookup_int(&cfg, "nmax_sa", &ival))
    mfield_params->nmax_sa = (size_t) ival;

  if (config_lookup_float(&cfg, "epoch", &fval))
    mfield_params->epoch = fval;
  if (config_lookup_float(&cfg, "R", &fval))
    mfield_params->R = fval;
  if (config_lookup_float(&cfg, "euler_period", &fval))
    mfield_params->euler_period = fval;

  if (config_lookup_int(&cfg, "max_iter", &ival))
    mfield_params->max_iter = (size_t) ival;
  if (config_lookup_float(&cfg, "qdlat_cutoff", &fval))
    data_params->qdlat_cutoff = fval;
  if (config_lookup_int(&cfg, "fit_sv", &ival))
    mfield_params->fit_sv = ival;
  if (config_lookup_int(&cfg, "fit_sa", &ival))
    mfield_params->fit_sa = ival;
  if (config_lookup_int(&cfg, "fit_euler", &ival))
    mfield_params->fit_euler = ival;
  if (config_lookup_int(&cfg, "fit_ext", &ival))
    mfield_params->fit_ext = ival;

  if (config_lookup_int(&cfg, "scale_time", &ival))
    mfield_params->scale_time = ival;
  if (config_lookup_int(&cfg, "use_weights", &ival))
    mfield_params->use_weights = ival;
  if (config_lookup_int(&cfg, "regularize", &ival))
    mfield_params->regularize = ival;

  if (config_lookup_float(&cfg, "weight_X", &fval))
    mfield_params->weight_X = fval;
  if (config_lookup_float(&cfg, "weight_Y", &fval))
    mfield_params->weight_Y = fval;
  if (config_lookup_float(&cfg, "weight_Z", &fval))
    mfield_params->weight_Z = fval;
  if (config_lookup_float(&cfg, "weight_F", &fval))
    mfield_params->weight_F = fval;
  if (config_lookup_float(&cfg, "weight_DX", &fval))
    mfield_params->weight_DX = fval;
  if (config_lookup_float(&cfg, "weight_DY", &fval))
    mfield_params->weight_DY = fval;
  if (config_lookup_float(&cfg, "weight_DZ", &fval))
    mfield_params->weight_DZ = fval;

  if (config_lookup_int(&cfg, "fit_X", &ival))
    data_params->fit_X = ival;
  if (config_lookup_int(&cfg, "fit_Y", &ival))
    data_params->fit_Y = ival;
  if (config_lookup_int(&cfg, "fit_Z", &ival))
    data_params->fit_Z = ival;
  if (config_lookup_int(&cfg, "fit_F", &ival))
    data_params->fit_F = ival;
  if (config_lookup_int(&cfg, "fit_DX_NS", &ival))
    data_params->fit_DX_NS = ival;
  if (config_lookup_int(&cfg, "fit_DY_NS", &ival))
    data_params->fit_DY_NS = ival;
  if (config_lookup_int(&cfg, "fit_DZ_NS", &ival))
    data_params->fit_DZ_NS = ival;
  if (config_lookup_int(&cfg, "fit_DF_NS", &ival))
    data_params->fit_DF_NS = ival;
  if (config_lookup_int(&cfg, "fit_DX_EW", &ival))
    data_params->fit_DX_EW = ival;
  if (config_lookup_int(&cfg, "fit_DY_EW", &ival))
    data_params->fit_DY_EW = ival;
  if (config_lookup_int(&cfg, "fit_DZ_EW", &ival))
    data_params->fit_DZ_EW = ival;
  if (config_lookup_int(&cfg, "fit_DF_EW", &ival))
    data_params->fit_DF_EW = ival;

  if (config_lookup_int(&cfg, "fit_Z_highlat", &ival))
    data_params->fit_Z_highlat = ival;
  if (config_lookup_int(&cfg, "fit_F_highlat", &ival))
    data_params->fit_F_highlat = ival;
  if (config_lookup_int(&cfg, "fit_DZ_NS_highlat", &ival))
    data_params->fit_DZ_NS_highlat = ival;
  if (config_lookup_int(&cfg, "fit_DF_NS_highlat", &ival))
    data_params->fit_DF_NS_highlat = ival;
  if (config_lookup_int(&cfg, "fit_DZ_EW_highlat", &ival))
    data_params->fit_DZ_EW_highlat = ival;
  if (config_lookup_int(&cfg, "fit_DF_EW_highlat", &ival))
    data_params->fit_DF_EW_highlat = ival;

  if (config_lookup_int(&cfg, "synth_data", &ival))
    mfield_params->synth_data = ival;
  if (config_lookup_int(&cfg, "synth_noise", &ival))
    mfield_params->synth_noise = ival;
  if (config_lookup_int(&cfg, "synth_nmin", &ival))
    mfield_params->synth_nmin = (size_t) ival;

  config_destroy(&cfg);

  return 0;
}

void
print_help(char *argv[])
{
  fprintf(stderr, "Usage: %s [options] sat1.dat sat2.dat ...\n", argv[0]);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "\t --maxit | -n num_iterations     - number of robust iterations\n");
  fprintf(stderr, "\t --output_file | -o file         - coefficient output file (ASCII)\n");
  fprintf(stderr, "\t --epoch | -e epoch              - model epoch in decimal years\n");
  fprintf(stderr, "\t --lambda_sv | -v lambda_sv      - SV damping parameter\n");
  fprintf(stderr, "\t --lambda_sa | -a lambda_sa      - SA damping parameter\n");
  fprintf(stderr, "\t --euler | -p period             - Euler bin size in days\n");
  fprintf(stderr, "\t --print_residuals | -r          - write residuals at each iteration\n");
  fprintf(stderr, "\t --lcurve_file | -l file         - L-curve data file\n");
  fprintf(stderr, "\t --tmin | -b min_time            - minimum data period time in decimal years\n");
  fprintf(stderr, "\t --tmax | -c max_time            - maximum data period time in decimal years\n");
  fprintf(stderr, "\t --print_data | -d               - print data used for MF modeling to output directory\n");
  fprintf(stderr, "\t --print_map | -m                - print spatial data map files to output directory\n");
  fprintf(stderr, "\t --config_file | -C file         - configuration file\n");
} /* print_help() */

int
main(int argc, char *argv[])
{
  char *outfile = NULL;
  char *Lfile = NULL;
  char *datamap_prefix = "output";
  char *data_prefix = "output";
  char *residual_prefix = "output";
  char *config_file = "MF.cfg"; /* default config file */
  mfield_workspace *mfield_workspace_p;
  mfield_parameters mfield_params;
  mfield_data_workspace *mfield_data_p;
  mfield_data_parameters data_params;
  gsl_vector *coeffs; /* model coefficients */
  size_t iter = 0;
  size_t maxit = 0;
  double lambda_sv = 0.0;     /* coefficient damping */
  double lambda_sa = 0.0;
  double epoch = -1.0;        /* model epoch */
  double euler_period = -1.0; /* set to 0 for single set of angles */
  double tmin = -1.0;         /* minimum time for data in years */
  double tmax = -1.0;         /* maximum time for data in years */
  int nsat = 0;               /* number of satellites */
  int print_data = 0;         /* print data for MF modeling */
  int print_map = 0;          /* print data maps */
  int print_residuals = 0;    /* print residuals at each iteration */
  struct timeval tv0, tv1;
  char buf[MAX_BUFFER];

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
          { "print_residuals", no_argument, NULL, 'r' },
          { "output_file", required_argument, NULL, 'o' },
          { "epoch", required_argument, NULL, 'e' },
          { "lambda_sv", required_argument, NULL, 'v' },
          { "lambda_sa", required_argument, NULL, 'a' },
          { "lcurve_file", required_argument, NULL, 'l' },
          { "maxit", required_argument, NULL, 'n' },
          { "tmin", required_argument, NULL, 'b' },
          { "tmax", required_argument, NULL, 'c' },
          { "euler", required_argument, NULL, 'p' },
          { "print_data", required_argument, NULL, 'd' },
          { "print_map", required_argument, NULL, 'm' },
          { "config_file", required_argument, NULL, 'C' },
          { 0, 0, 0, 0 }
        };

      c = getopt_long(argc, argv, "a:b:c:C:de:l:mn:o:p:rv:", long_options, &option_index);
      if (c == -1)
        break;

      switch (c)
        {
          case 'b':
            tmin = atof(optarg);
            break;

          case 'c':
            tmax = atof(optarg);
            break;

          case 'C':
            config_file = optarg;
            break;

          case 'd':
            print_data = 1;
            break;

          case 'm':
            print_map = 1;
            break;

          case 'o':
            outfile = optarg;
            break;

          case 'n':
            maxit = (size_t) atoi(optarg);
            break;

          case 'e':
            epoch = atof(optarg);
            break;

          case 'v':
            lambda_sv = atof(optarg);
            break;

          case 'a':
            lambda_sa = atof(optarg);
            break;

          case 'p':
            euler_period = atof(optarg);
            break;

          case 'r':
            print_residuals = 1;
            break;

          case 'l':
            Lfile = optarg;
            break;

          default:
            print_help(argv);
            exit(1);
            break;
        }
    }

  nsat = argc - optind;
  if (nsat == 0)
    {
      print_help(argv);
      exit(1);
    }

  /* parse configuration file */
  fprintf(stderr, "main: parsing configuration file %s...", config_file);
  parse_config_file(config_file, &mfield_params, &data_params);
  fprintf(stderr, "done\n");

  /* check if any command-line arguments should override config file values */
  if (epoch > 0.0)
    mfield_params.epoch = epoch;
  if (euler_period > 0.0)
    mfield_params.euler_period = euler_period;
  if (maxit > 0)
    mfield_params.max_iter = maxit;

  fprintf(stderr, "main: epoch = %.2f\n", mfield_params.epoch);
  fprintf(stderr, "main: radius = %g [km]\n", mfield_params.R);
  fprintf(stderr, "main: MF nmax = %zu\n", mfield_params.nmax_mf);
#if MFIELD_FIT_SECVAR
  fprintf(stderr, "main: SV nmax = %zu\n", mfield_params.nmax_sv);
  fprintf(stderr, "main: SV damping = %g\n", lambda_sv);
#endif
#if MFIELD_FIT_SECACC
  fprintf(stderr, "main: SA nmax = %zu\n", mfield_params.nmax_sa);
  fprintf(stderr, "main: SA damping = %g\n", lambda_sa);
#endif
  fprintf(stderr, "main: euler period = %g [days]\n", mfield_params.euler_period);
  fprintf(stderr, "main: tmin = %g\n", tmin);
  fprintf(stderr, "main: tmax = %g\n", tmax);
  fprintf(stderr, "main: number of robust iterations = %zu\n", mfield_params.max_iter);
  fprintf(stderr, "main: number of satellites = %d\n", nsat);
  fprintf(stderr, "main: number of threads = %d\n", omp_get_max_threads());
  fprintf(stderr, "main: print_residuals = %d\n", print_residuals);
  if (outfile)
    fprintf(stderr, "main: output coefficient file = %s\n", outfile);
  if (Lfile)
    fprintf(stderr, "main: L-curve output file = %s\n", Lfile);

  /* allocate data workspace */
  data_params.epoch = mfield_params.epoch;
  mfield_data_p = mfield_data_alloc(nsat, &data_params);

  {
    int satnum = 0;

    while (optind < argc)
      {
        magdata **mdata = &(mfield_data_p->mdata[satnum]);

        assert(satnum++ < nsat);

        fprintf(stderr, "main: reading %s...", argv[optind]);
        gettimeofday(&tv0, NULL);
        *mdata = magdata_read(argv[optind], NULL);
        gettimeofday(&tv1, NULL);

        if (!(*mdata))
          exit(1);

        fprintf(stderr, "done (%zu data total, %g seconds)\n",
                (*mdata)->n, time_diff(tv0, tv1));

        magdata_init(*mdata);
        magdata_calc(*mdata);

        ++optind;
      }
  }

  {
    size_t nflag;

    /* flag any datapoints outside of [tmin,tmax] */
    fprintf(stderr, "main: flagging points outside of time [%g,%g]...", tmin, tmax);
    nflag = mfield_data_filter_time(tmin, tmax, mfield_data_p);
    fprintf(stderr, "done (%zu data flagged)\n", nflag);

#if !MFIELD_FIT_EULER
    fprintf(stderr, "main: flagging Euler-only data points...");
    nflag = mfield_data_filter_euler(mfield_data_p);
    fprintf(stderr, "done (%zu data flagged)\n", nflag);
#endif

    fprintf(stderr, "main: flagging non-fitted components...");
    nflag = mfield_data_filter_comp(mfield_data_p);
    fprintf(stderr, "done (%zu data flagged)\n", nflag);

#if 0
    /* XXX: discard most of EMAG grid for testing */
    {
      size_t i, j;
      magdata *mptr = mfield_data_ptr(0, mfield_data_p);

      for (j = 0; j < mptr->n; ++j)
        {
          if (j % 5 != 0)
            mptr->flags[j] |= MAGDATA_FLG_DISCARD;
        }
    }
#endif
  }

  fprintf(stderr, "main: data epoch = %.2f\n", mfield_data_epoch(mfield_data_p));
  fprintf(stderr, "main: data tmin  = %.2f\n", satdata_epoch2year(mfield_data_p->t0_data));
  fprintf(stderr, "main: data tmax  = %.2f\n", satdata_epoch2year(mfield_data_p->t1_data));

  if (print_map)
    {
      /* print spatial coverage maps for each satellite */
      mfield_data_map(datamap_prefix, mfield_data_p);
    }

  mfield_params.nsat = nsat;
  mfield_params.mfield_data_p = mfield_data_p;

  /* allocate mfield workspace */
  mfield_workspace_p = mfield_alloc(&mfield_params);

  if (mfield_params.synth_data)
    {
      fprintf(stderr, "main: replacing with synthetic data...");
      gettimeofday(&tv0, NULL);
      mfield_synth_replace(mfield_workspace_p);
      gettimeofday(&tv1, NULL);
      fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));
    }

  /* initialize model parameters */
  mfield_init(mfield_workspace_p);

  /* coefficient damping parameters */
  mfield_set_damping(lambda_sv, lambda_sa, mfield_workspace_p);

  /* print out dataset if requested - do this after mfield_init() so
   * spatial weights are computed */
  if (print_data)
    {
      /* print data used for MF modeling for each satellite */
      fprintf(stderr, "main: printing data for MF modeling to %s...", data_prefix);
      mfield_data_print(data_prefix, mfield_workspace_p->wts_spatial, mfield_data_p);
      fprintf(stderr, "done\n");
    }

  /* construct initial guess vector from IGRF */
  coeffs = gsl_vector_alloc(mfield_workspace_p->p);
  fprintf(stderr, "main: constructing initial coefficient vector...");
  initial_guess(coeffs, mfield_workspace_p);
  fprintf(stderr, "done\n");

  gettimeofday(&tv0, NULL);

  while (iter++ < mfield_params.max_iter)
    {
      fprintf(stderr, "main: ROBUST ITERATION %zu/%zu\n", iter, mfield_params.max_iter);

      mfield_calc_nonlinear(coeffs, mfield_workspace_p);

      /* output coefficients for this iteration */
      sprintf(buf, "coef.txt.iter%zu", iter);
      fprintf(stderr, "main: writing coefficient file %s...", buf);
      mfield_write_ascii(buf, mfield_workspace_p->epoch, 0, mfield_workspace_p);
      fprintf(stderr, "done\n");

      /* output spectrum for this iteration */
      sprintf(buf, "mfield.s.iter%zu", iter);
      print_spectrum(buf, mfield_workspace_p);

      if (print_residuals)
        {
          fprintf(stderr, "main: printing residuals to %s...", residual_prefix);
          mfield_print_residual(residual_prefix, iter, coeffs, mfield_workspace_p);
          fprintf(stderr, "done\n");
        }

      /* reset workspace for a new iteration */
      mfield_reset(mfield_workspace_p);
    }

  gettimeofday(&tv1, NULL);

  fprintf(stderr, "main: total time for inversion: %.2f seconds\n", time_diff(tv0, tv1));

  /* calculate errors in coefficients */
  fprintf(stderr, "main: calculating coefficient uncertainties...");
  gettimeofday(&tv0, NULL);
  mfield_calc_uncertainties(mfield_workspace_p);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  sprintf(buf, "mfield_coeffs.txt");
  fprintf(stderr, "main: writing coefficients to %s...", buf);
  mfield_write_ascii(buf, mfield_workspace_p->epoch, 1, mfield_workspace_p);
  fprintf(stderr, "done\n");

  {
    gsl_vector *evals = gsl_vector_alloc(mfield_workspace_p->p);
    FILE *fp;

    fprintf(stderr, "main: calculating eigenvalues of J^T J...");
    gettimeofday(&tv0, NULL);
    mfield_calc_evals(evals, mfield_workspace_p);
    gettimeofday(&tv1, NULL);
    fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

    sprintf(buf, "mfield_evals.txt");
    fprintf(stderr, "main: writing eigenvalues to %s...", buf);
    fp = fopen(buf, "w");
    gsl_vector_fprintf(fp, evals, "%.12e");
    fclose(fp);
    fprintf(stderr, "done\n");

    gsl_vector_free(evals);
  }

  /* L-curve data */
  if (Lfile)
    {
      gsl_vector *f = gsl_multifit_nlinear_residual(mfield_workspace_p->multifit_nlinear_p);
      double xnorm = gsl_blas_dnrm2(mfield_workspace_p->c);
      double fnorm = gsl_blas_dnrm2(f);
      FILE *fp = fopen(Lfile, "a");

      if (!fp)
        {
          fprintf(stderr, "main: unable to open %s: %s\n",
                  Lfile, strerror(errno));
        }
      else
        {
          fprintf(fp, "%.12e %.12e %f %f\n",
                  log(fnorm),
                  log(xnorm),
                  lambda_sv,
                  lambda_sa);

          fclose(fp);
        }
    }

  if (outfile)
    {
      fprintf(stderr, "main: writing ASCII coefficients to %s...", outfile);
      /*mfield_write(outfile, mfield_workspace_p);*/
      mfield_write_ascii(outfile, mfield_workspace_p->epoch, 0, mfield_workspace_p);
      fprintf(stderr, "done\n");
    }

  print_spectrum("mfield.s", mfield_workspace_p);

  /* print coefficients */
  {
    size_t n;
    int m;

#if MFIELD_FIT_EXTFIELD
    char *ext_file = "coeffs.ext";
    FILE *fp = fopen(ext_file, "w");

    fprintf(stderr, "main: printing external coefficients to %s...", ext_file);
    
    for (n = 0; n < mfield_workspace_p->next; ++n)
      {
        size_t idx = mfield_workspace_p->ext_offset + n;
        double k = gsl_vector_get(coeffs, idx);

        fprintf(fp, "%d %g\n", mfield_workspace_p->ext_fdayi[n], k);
      }

    fprintf(stderr, "done\n");

    fclose(fp);
#endif

#if MFIELD_FIT_EULER
    /* print Euler angles */
    for (n = 0; n < mfield_workspace_p->nsat; ++n)
      {
        magdata *mptr = mfield_data_ptr(n, mfield_workspace_p->data_workspace_p);

        if (mptr->global_flags & MAGDATA_GLOBFLG_EULER)
          {
            double t0 = mfield_workspace_p->data_workspace_p->t0[n];
            size_t euler_idx = mfield_euler_idx(n, t0, mfield_workspace_p);
            double alpha = gsl_vector_get(coeffs, euler_idx);
            double beta = gsl_vector_get(coeffs, euler_idx + 1);
            double gamma = gsl_vector_get(coeffs, euler_idx + 2);
            char filename[2048];

            fprintf(stderr, "main: satellite %zu: alpha = %f beta = %f gamma = %f [deg]\n",
                    n,
                    wrap180(alpha * 180.0 / M_PI),
                    wrap180(beta * 180.0 / M_PI),
                    wrap180(gamma * 180.0 / M_PI));

            sprintf(filename, "euler.%zu", n);
            fprintf(stderr, "main: satellite %zu: printing Euler angles to %s...", n, filename);
            mfield_euler_print(filename, n, mfield_workspace_p);
            fprintf(stderr, "done\n");
          }
      }
#endif

    fprintf(stderr, "main: printing internal coefficients up to degree 3\n");
    for (n = 1; n <= GSL_MIN(3, mfield_params.nmax_mf); ++n)
      {
        int ni = (int) n;
        for (m = -ni; m <= ni; ++m)
          {
            int mabs = abs(m);
            size_t cidx = mfield_coeff_nmidx(n, m);
            char c = (m < 0) ? 'h' : 'g';
            double gnm = mfield_get_mf(coeffs, cidx, mfield_workspace_p);
            double dgnm = mfield_get_sv(coeffs, cidx, mfield_workspace_p);
            double ddgnm = mfield_get_sa(coeffs, cidx, mfield_workspace_p);

            fprintf(stderr, "%c(%d,%d) = %12g (%12g,%12g)\n", c, ni, mabs,
                    gnm, dgnm, ddgnm);
          }
      }
  }

  /* print residual norm between synthetic and computed coefficients */
  if (mfield_params.synth_data)
    {
      gsl_vector *g_synth = gsl_vector_alloc(mfield_workspace_p->p_int);
      gsl_vector_view g = gsl_vector_subvector(coeffs, 0, mfield_workspace_p->p_int);
      double norm;

      /* synthetic internal coefficients */
      mfield_synth_g(g_synth, mfield_workspace_p);

      /* subtract model internal coefficients */
      gsl_vector_sub(g_synth, &g.vector);

      norm = gsl_blas_dnrm2(g_synth);

      fprintf(stderr, "main: || g_synth - g || = %.12e [nT]\n", norm);

      gsl_vector_free(g_synth);
    }

  mfield_free(mfield_workspace_p);
  mfield_data_free(mfield_data_p);
  gsl_vector_free(coeffs);

  return 0;
} /* main() */
