#include "hstream/src/libs/stitching/CrossCorrelation.h"

#include <fftw3.h>

namespace hm {
namespace stitching {

/**
 * @brief Computes the full cross-correlation of two 1D signals.
 *
 * The cross-correlation is defined as
 * \f[
 *    (x \star y)[l] = \sum_{n} x[n] \, y[n+l]
 * \f]
 * where the sum is taken over all indices for which both x[n] and y[n+l] are defined.
 * The output vector has size N1 + N2 - 1, where N1 = x.size() and N2 = y.size().
 * The zero lag (l = 0) is located at index N1 - 1.
 *
 * This function is equivalent to calling scipy.signal.correlate(in1, in2, mode="full")
 * on the first channel (i.e. 1D arrays) of your audio.
 *
 * @param x The first input signal.
 * @param y The second input signal.
 * @return std::vector<float> The full cross-correlation array.
 */
std::vector<float> full_correlate(const std::vector<float>& x, const std::vector<float>& y) {
  // Lengths of the input signals.
  int N1 = static_cast<int>(x.size());
  int N2 = static_cast<int>(y.size());
  // The length of the full cross-correlation result.
  int L = N1 + N2 - 1;
  std::vector<float> correlation(L, 0.0);

  // The index corresponding to zero lag.
  int zero_index = N1 - 1;

// Loop over all lags from -(N1-1) to (N2-1)
#pragma omp parallel for
  for (int lag = -N1 + 1; lag <= N2 - 1; ++lag) {
    float sum = 0.0;
    // Determine the valid range for summation:
    // i must be in [0, N1) and i+lag must be in [0, N2).
    int i_start = std::max(0, -lag);
    int i_end = std::min(N1, N2 - lag);
    for (int i = i_start; i < i_end; ++i) {
      sum += x[i] * y[i + lag];
    }
    // Store the computed sum in the correlation array.
    correlation[lag + zero_index] = sum;
  }
  return correlation;
}

// full_correlate_fft computes the full cross‐correlation of x and y using FFTW.
// The direct cross-correlation (in time domain) is defined for lags from –(N1–1) to N2–1,
// with zero lag at index N1–1 in the output vector.
// This function zero pads both x and y to length L = N1 + N2 – 1, computes
// r = ifft( conj(fft(x)) * fft(y) ), normalizes the result, and then rotates the output.
std::vector<float> full_correlate_fft(const std::vector<float>& x, const std::vector<float>& y) {
  int N1 = x.size();
  int N2 = y.size();
  int L = N1 + N2 - 1; // length of full cross-correlation

  // Zero-pad x and y to length L.
  std::vector<float> x_pad(L, 0.0f), y_pad(L, 0.0f);
  std::copy(x.begin(), x.end(), x_pad.begin());
  std::copy(y.begin(), y.end(), y_pad.begin());

  // FFTW real-to-complex transform yields L/2+1 complex numbers.
  int ncomplex = L / 2 + 1;
  fftwf_complex* X = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * ncomplex);
  fftwf_complex* Y = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * ncomplex);
  fftwf_complex* C = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * ncomplex);

  // Create FFTW plans.
  fftwf_plan plan_forward_x = fftwf_plan_dft_r2c_1d(L, x_pad.data(), X, FFTW_ESTIMATE);
  fftwf_plan plan_forward_y = fftwf_plan_dft_r2c_1d(L, y_pad.data(), Y, FFTW_ESTIMATE);
  fftwf_plan plan_backward = fftwf_plan_dft_c2r_1d(L, C, x_pad.data(), FFTW_ESTIMATE);

  // Execute forward FFTs.
  fftwf_execute(plan_forward_x);
  fftwf_execute(plan_forward_y);

  // Compute element-wise product: C[k] = conj(X[k]) * Y[k].
  // (This produces the FFT of the cross-correlation.)
  for (int k = 0; k < ncomplex; ++k) {
    float xr = X[k][0], xi = X[k][1];
    float yr = Y[k][0], yi = Y[k][1];
    // Conjugate of X is (xr, -xi).
    C[k][0] = xr * yr + xi * yi; // real part
    C[k][1] = xr * yi - xi * yr; // imaginary part
  }

  // Execute inverse FFT. The result is placed in x_pad.
  fftwf_execute(plan_backward);

  // Normalize the inverse FFT (FFTW does not scale the inverse transform).
  std::vector<float> corr(L);
  for (int i = 0; i < L; i++) {
    corr[i] = x_pad[i] / L;
  }

  // Clean up FFTW resources.
  fftwf_destroy_plan(plan_forward_x);
  fftwf_destroy_plan(plan_forward_y);
  fftwf_destroy_plan(plan_backward);
  fftwf_free(X);
  fftwf_free(Y);
  fftwf_free(C);

  // The FFT-based correlation computed above produces an array r such that:
  //   r[0] corresponds to lag = 0,
  //   r[1] corresponds to lag = 1, etc.
  // For a full (linear) correlation we want the output arranged so that:
  //   index N1-1 corresponds to lag = 0,
  //   indices before that correspond to negative lags,
  //   and indices after correspond to positive lags.
  // Perform a circular shift by N1 - 1.
  std::vector<float> corr_shifted(L);
  int shift = N1 - 1;
  for (int i = 0; i < L; i++) {
    corr_shifted[i] = corr[(i + shift) % L];
  }
  return corr_shifted;
}

} // namespace stitching
} // namespace hm
