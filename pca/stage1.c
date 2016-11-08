/*
 * stage1.c
 *
 * 1. Read tiegcm data file(s)
 * 2. For each time step t_k, invert B(t_k) grid for SH coefficients k_{nm}(t_k)
 * 3. Store k_{nm}(t) SH coefficients in a nnm-by-nt matrix:
 *
 *      X_{ij} = k_i(t_j) where i = shidx(n,m)
 * 5. X matrix is output to a binary file
 *
 * ./stage1 [-o binary_output_matrix_file] tiegcm1.nc tiegcm2.nc ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include <assert.h>
#include <errno.h>

#include <lapacke/lapacke.h>

#include <gsl/gsl_math.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_blas.h>

#include "apex.h"
#include "common.h"
#include "geo.h"
#include "green.h"
#include "lapack_wrapper.h"
#include "magdata.h"
#include "poltor.h"

#include "io.h"
#include "pca.h"
#include "tiegcm.h"

#define USE_SYNTH_DATA              0

/*
print_residuals()
  Print model residuals for a given timestamp

Inputs: filename - where to store residuals
        tidx     - time index
        A        - model matrix
        c        - model coefficients
        data     - TIEGCM data
*/

double
print_residuals(const char *filename, const size_t tidx,
                const gsl_matrix *A, const gsl_vector *c,
                const tiegcm_data *data)
{
  FILE *fp;
  gsl_vector *b = gsl_vector_alloc(A->size1); /* model prediction */
  size_t bidx = 0;
  size_t ilon, ilat, j;
  double rnorm = 0.0;

  fp = fopen(filename, "w");
  if (!fp)
    {
      fprintf(stderr, "print_residuals: unable to open %s: %s\n",
              filename, strerror(errno));
      return 0.0;
    }

  /* compute b = A c */
  gsl_blas_dgemv(CblasNoTrans, 1.0, A, c, 0.0, b);

  j = 1;
  fprintf(fp, "# Grid for timestamp %ld\n", data->t[tidx]);
  fprintf(fp, "# Field %zu: geographic longitude (degrees)\n", j++);
  fprintf(fp, "# Field %zu: geodetic latitude (degrees)\n", j++);
  fprintf(fp, "# Field %zu: TIEGCM B_x (nT)\n", j++);
  fprintf(fp, "# Field %zu: TIEGCM B_y (nT)\n", j++);
  fprintf(fp, "# Field %zu: TIEGCM B_z (nT)\n", j++);
  fprintf(fp, "# Field %zu: Modeled B_x (nT)\n", j++);
  fprintf(fp, "# Field %zu: Modeled B_y (nT)\n", j++);
  fprintf(fp, "# Field %zu: Modeled B_z (nT)\n", j++);

  for (ilon = 0; ilon < data->nlon; ++ilon)
    {
      for (ilat = 0; ilat < data->nlat; ++ilat)
        {
          size_t idx = TIEGCM_BIDX(tidx, ilat, ilon, data);
          double B_model[3], B_data[3];

          B_data[0] = data->Bx[idx] * 1.0e9;
          B_data[1] = data->By[idx] * 1.0e9;
          B_data[2] = data->Bz[idx] * 1.0e9;

          for (j = 0; j < 3; ++j)
            {
              B_model[j] = gsl_vector_get(b, bidx++);

              /* update residual norm */
              rnorm = gsl_hypot(rnorm, B_data[j] - B_model[j]);
            }

          fprintf(fp, "%f %f %f %f %f %f %f %f\n",
                  data->glon[ilon],
                  data->glat[ilat],
                  data->Bx[idx] * 1.0e9,
                  data->By[idx] * 1.0e9,
                  data->Bz[idx] * 1.0e9,
                  B_model[0],
                  B_model[1],
                  B_model[2]);
        }

      fprintf(fp, "\n");
    }

  gsl_vector_free(b);
  fclose(fp);

  return rnorm;
}

magdata *
tiegcm_magdata(const size_t tidx, tiegcm_data *data)
{
  const size_t grid_size = data->nlon * data->nlat;
  magdata *mdata;
  magdata_datum datum;
  size_t ilat, ilon;
  apex_workspace *apex_p;

  mdata = magdata_alloc(grid_size, R_EARTH_KM);
  if (!mdata)
    return 0;

  apex_p = apex_alloc(2016);

  magdata_datum_init(&datum);

  datum.t = satdata_timet2epoch(data->t[tidx]);
  datum.flags = MAGDATA_FLG_X | MAGDATA_FLG_Y | MAGDATA_FLG_Z;

  fprintf(stderr, "tiegcm_magdata: building magdata structure for time index %zu...", tidx);

  for (ilon = 0; ilon < data->nlon; ++ilon)
    {
      double phi = data->glon[ilon] * M_PI / 180.0;

      for (ilat = 0; ilat < data->nlat; ++ilat)
        {
          double latd = data->glat[ilat] * M_PI / 180.0; /* geodetic latitude */
          double thetad = M_PI / 2.0 - latd;             /* geodetic colatitude */
          double r, latc, theta;                         /* geocentric radius, latitude and colatitude */
          double qdlat, alon, alat;
          size_t idx = TIEGCM_BIDX(tidx, ilat, ilon, data);

#if 0
          geodetic2geo(latd, 0.0, &latc, &r);
          theta = M_PI / 2.0 - latc;
#else
          theta = thetad;
          r = R_EARTH_KM;
#endif

          apex_transform_geodetic(thetad, phi, 0.0, &alon, &alat, &qdlat,
                                  NULL, NULL, NULL, apex_p);

          datum.r = r;
          datum.theta = theta;
          datum.phi = phi;
          datum.qdlat = qdlat;
          datum.B_nec[0] = data->Bx[idx] * 1.0e9;
          datum.B_nec[1] = data->By[idx] * 1.0e9;
          datum.B_nec[2] = data->Bz[idx] * 1.0e9;

          magdata_add(&datum, mdata);
        }
    }

  fprintf(stderr, "done\n");

  apex_free(apex_p);

  return mdata;
}

int
main_build_matrix(const magdata *mdata, green_workspace *green_p,
                  gsl_matrix *A)
{
  const size_t n = A->size1;
  const double eps = 1.0e-6;
  size_t rowidx = 0;
  size_t ilon, ilat;
  size_t i;

  for (i = 0; i < mdata->n; ++i)
    {
      double r = mdata->r[i];
      double theta = mdata->theta[i];
      double phi = mdata->phi[i];
      gsl_vector_view vx, vy, vz;

      if (theta < eps)
        theta = eps;
      if (theta > M_PI - eps)
        theta = M_PI - eps;

      vx = gsl_matrix_row(A, rowidx++);
      vy = gsl_matrix_row(A, rowidx++);
      vz = gsl_matrix_row(A, rowidx++);

      /* compute external Green's functions */
      green_calc_ext(r, theta, phi,
                     vx.vector.data,
                     vy.vector.data,
                     vz.vector.data,
                     green_p);
    }

  assert(rowidx == n);

  return 0;
}

/*
main_build_rhs()
  Construct RHS vector for a given time index
*/

int
main_build_rhs(const size_t tidx, const tiegcm_data *data,
               gsl_vector *b)
{
  const size_t n = b->size;
  size_t ilon, ilat;
  size_t rowidx = 0;

  for (ilon = 0; ilon < data->nlon; ++ilon)
    {
      for (ilat = 0; ilat < data->nlat; ++ilat)
        {
          size_t idx = TIEGCM_BIDX(tidx, ilat, ilon, data);

          gsl_vector_set(b, rowidx++, data->Bx[idx] * 1.0e9);
          gsl_vector_set(b, rowidx++, data->By[idx] * 1.0e9);
          gsl_vector_set(b, rowidx++, data->Bz[idx] * 1.0e9);
        }
    }

  assert(rowidx == n);

  return 0;
}

/*
convert_qnm()
  Convert k_{nm}(t) time series to q_{nm}(t)

Inputs: b       - radius of current shell (km)
        v       - vector of k_{nm}(t) for some time t, size nnm
        green_p - green workspace
*/

int
convert_qnm(const double b, gsl_vector * v, const green_workspace * green_p)
{
  const size_t nmax = green_p->nmax;
  const size_t mmax = green_p->mmax;
  const double ratio = b / green_p->R;
  double rterm = pow(ratio, -1.0); /* (b/R)^{n-2} */
  size_t n;

  for (n = 1; n <= nmax; ++n)
    {
      int M = (int) GSL_MIN(n, mmax);
      int m;
      double nfac = (2.0 * n + 1.0) / (n + 1.0);

      for (m = -M; m <= M; ++m)
        {
          size_t cidx = green_nmidx(n, m, green_p);
          double knm = gsl_vector_get(v, cidx);

          gsl_vector_set(v, cidx, nfac * rterm * knm);
        }

      /* (b/R)^{n-2} */
      rterm *= ratio;
    }

  return 0;
}

int
main_proc(const char *filename, const char *outfile_mat, tiegcm_data *data)
{
  int status;
  const char *res_file = "res.dat";
  const char *spectrum_file = "spectrum.s";
  const char *spectrum_azim_file = "spectrum_azim.s";
  const char *datamap_file = "datamap.dat";
  const size_t nmax = 60;
  const size_t mmax = GSL_MIN(nmax, 30);
  const double R = R_EARTH_KM;
  green_workspace *green_p = green_alloc(nmax, mmax, R);
  const size_t n = 3 * data->nlon * data->nlat; /* number of residuals */
  const size_t p = green_p->nnm;                /* number of external coefficients */
  const size_t nrhs = data->nt;                 /* number of right hand sides */
  gsl_matrix *A = gsl_matrix_alloc(n, p);       /* least squares matrix */
  gsl_matrix *B = gsl_matrix_alloc(n, nrhs);    /* right hand sides */
  gsl_matrix *X = gsl_matrix_alloc(p, nrhs);    /* solution vectors */
  gsl_vector *r = gsl_vector_alloc(n);          /* residual vector */
  magdata *mdata;
  size_t k;
  FILE *fp;
  struct timeval tv0, tv1;
  int rank;

  fprintf(stderr, "main_proc: %zu observations per grid\n", n);
  fprintf(stderr, "main_proc: %zu SH model coefficients\n", p);
  fprintf(stderr, "main_proc: %zu timestamps\n", nrhs);

  /* store spatial locations in magdata structure - grid points are
   * the same for all timestamps t_k */
  mdata = tiegcm_magdata(0, data);

  /* print data map */
  fprintf(stderr, "main_proc: writing data map to %s...", datamap_file);
  magdata_map(datamap_file, mdata);
  fprintf(stderr, "done\n");

  /* construct least squares matrix (common for all timestamps) */
  fprintf(stderr, "main_proc: building least squares matrix A...");
  gettimeofday(&tv0, NULL);
  status = main_build_matrix(mdata, green_p, A);
  if (status)
    return status;
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  fp = fopen(filename, "w");

  fprintf(stderr, "main_proc: building rhs vectors...");
  gettimeofday(&tv0, NULL);

  /* construct right hand side vectors */
  for (k = 0; k < data->nt; ++k)
    {
      gsl_vector_view b = gsl_matrix_column(B, k);

      /* construct rhs vector for time t_k */
      main_build_rhs(k, data, &b.vector);
    }

  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));

  /* solve least squares system for all rhs vectors */
  fprintf(stderr, "main_proc: solving LS system with QR decomposition of A...");
  gettimeofday(&tv0, NULL);
  status = lapack_lls(A, B, X, &rank);
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds, s = %d, rank = %d)\n",
          time_diff(tv0, tv1), status, rank);

  /* print spectrum of coefficients at time t_0 */
  {
    const size_t k = 0;
    gsl_vector_view x = gsl_matrix_column(X, k);

    fprintf(stderr, "main_proc: writing spectrum at t0 to %s...", spectrum_file);
    green_print_spectrum(spectrum_file, &x.vector, green_p);
    fprintf(stderr, "done\n");

    fprintf(stderr, "main_proc: writing azimuth spectrum at t0 to %s...", spectrum_azim_file);
    green_print_spectrum_azim(spectrum_azim_file, &x.vector, green_p);
    fprintf(stderr, "done\n");
  }

  /* print residuals at time t_0 */
  {
    const size_t k = 0;
    gsl_vector_view x = gsl_matrix_column(X, k);
    double rnorm;

    fprintf(stderr, "main_proc: writing residuals to %s...", res_file);
    rnorm = print_residuals(res_file, k, A, &x.vector, data);
    fprintf(stderr, "done (|| b - A x || = %.12e)\n", rnorm);
  }

#if 0
  /* convert k_{nm}(t) to q_{nm}(t) */
  fprintf(stderr, "main_proc: converting knm to qnm...");
  gettimeofday(&tv0, NULL);
  {
    for (k = 0; k < data->nt; ++k)
      {
        gsl_vector_view Xk = gsl_matrix_column(X, k);
        convert_qnm(b, &Xk.vector, green_p);
      }
  }
  gettimeofday(&tv1, NULL);
  fprintf(stderr, "done (%g seconds)\n", time_diff(tv0, tv1));
#endif

  k = 1;
  fprintf(fp, "# Field %zu: timestamp (UT seconds since 1970-01-01 00:00:00 UTC)\n", k++);
  fprintf(fp, "# Field %zu: k(1,0) (nT)\n", k++);
  fprintf(fp, "# Field %zu: k(1,1) (nT)\n", k++);
  fprintf(fp, "# Field %zu: k(2,0) (nT)\n", k++);
  fprintf(fp, "# Field %zu: k(2,1) (nT)\n", k++);
  fprintf(fp, "# Field %zu: k(2,2) (nT)\n", k++);

  for (k = 0; k < data->nt; ++k)
    {
      size_t N;
#if 0
      gsl_vector_view b = gsl_matrix_column(B, k);
      gsl_vector_view x = gsl_matrix_column(X, k);

      /* compute r = b - A x */
      gsl_vector_memcpy(r, &b.vector);
      gsl_blas_dgemv(CblasNoTrans, -1.0, A, &x.vector, 1.0, r);

      fprintf(stderr, "main_proc: residual for timestamp (%zu/%zu): %.12e [nT]\n",
              k + 1, data->nt, gsl_blas_dnrm2(r));
#endif

      fprintf(fp, "%ld ", data->t[k]);

      for (N = 1; N <= 2; ++N)
        {
          int M = (int) N;
          int m;

          for (m = 0; m <= M; ++m)
            {
              size_t cidx = green_nmidx(N, m, green_p);
              double knm = gsl_matrix_get(X, cidx, k);

              fprintf(fp, "%f ", knm);
            }
        }

      putc('\n', fp);
      fflush(fp);
    }

  /* write matrix of solution vectors to output file */
  fprintf(stderr, "main_proc: writing solution matrix to %s...", outfile_mat);
  pca_write_matrix(outfile_mat, X);
  fprintf(stderr, "done\n");

  fprintf(stderr, "main_proc: writing misc data to %s...", PCA_STAGE1_DATA);
  pca_write_data(PCA_STAGE1_DATA, nmax, mmax);
  fprintf(stderr, "done\n");

  green_free(green_p);
  gsl_matrix_free(A);
  gsl_matrix_free(B);
  gsl_matrix_free(X);

  fclose(fp);

  fprintf(stderr, "main_proc: wrote knm coefficients to %s\n", filename);

  return 0;
}

int
main(int argc, char *argv[])
{
  tiegcm_data *data = NULL;
  struct timeval tv0, tv1;
  char *outfile = "knm.txt";
  char *outfile_mat = PCA_STAGE1_KNM;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
          { 0, 0, 0, 0 }
        };

      c = getopt_long(argc, argv, "o:", long_options, &option_index);
      if (c == -1)
        break;

      switch (c)
        {
          case 'o':
            outfile_mat = optarg;
            break;

          default:
            break;
        }
    }

  if (optind >= argc)
    {
      fprintf(stderr, "Usage: %s [-o binary_matrix_output_file] file1.nc file2.nc ...\n",
              argv[0]);
      exit(1);
    }

  while (optind < argc)
    {
      fprintf(stderr, "main: reading %s...", argv[optind]);
      gettimeofday(&tv0, NULL);

      data = tiegcm_read(argv[optind], data);
      if (!data)
        {
          fprintf(stderr, "main: error reading %s\n", argv[optind]);
          exit(1);
        }

      gettimeofday(&tv1, NULL);
      fprintf(stderr, "done (%zu records read, %g seconds)\n", data->nt,
              time_diff(tv0, tv1));

      ++optind;
    }

  main_proc(outfile, outfile_mat, data);

  tiegcm_free(data);

  return 0;
}
