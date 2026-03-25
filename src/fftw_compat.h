/**
 * @file fftw_compat.h
 * @brief Single-precision FFTW3 API mapped onto KissFFT.
 *
 * This shim is a redirect to fft_shim.h, which contains the full
 * implementation with renamed internal types.  Including fftw_compat.h
 * or fft_shim.h is equivalent; JS8.cpp uses fftw_compat.h.
 *
 * Supported FFTW entry points (only the subset used by JS8.cpp):
 *   fftwf_plan_dft_1d    — complex-to-complex (forward and backward)
 *   fftwf_plan_dft_r2c_1d — real-to-complex (forward only)
 *   fftwf_execute        — execute a previously created plan
 *   fftwf_destroy_plan   — free plan resources
 *
 * Compatibility guarantees:
 *   - fftwf_complex and kiss_fft_cpx share the same {float r; float i;} layout.
 *   - KissFFT does not apply any normalisation on inverse transforms; neither
 *     does FFTW with FFTW_BACKWARD.  Callers that need 1/N scaling must do it.
 *   - In-place transforms (in == out pointer) work correctly.
 *   - All FFTW planning flags (FFTW_ESTIMATE, FFTW_MEASURE, …) are ignored.
 */
#pragma once

// Delegate to fft_shim.h which provides the actual implementation.
// The two files are interchangeable; fft_shim.h uses more descriptive
// type names (FftwPlanImpl vs _fftwf_plan_s).
#include "fft_shim.h"
