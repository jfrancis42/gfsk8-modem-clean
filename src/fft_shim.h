#pragma once
/**
 * @file fft_shim.h
 * @brief Maps the single-precision FFTW3 API onto KissFFT.
 *
 * Only the subset actually called by the JS8 decoder is implemented.
 * Types are layout-compatible: fftwf_complex == kiss_fft_cpx == {float r, i}.
 *
 * Key behavioural notes:
 *   - KissFFT does not normalize on inverse transforms; neither does FFTW
 *     FFTW_BACKWARD.  Callers that need normalization must do it themselves.
 *   - In-place transforms (in == out) are supported by KissFFT.
 *   - Planning flags (FFTW_ESTIMATE etc.) are silently ignored.
 */

#include <kissfft/kiss_fft.h>
#include <kissfft/kiss_fftr.h>
#include <cassert>
#include <cstdlib>

// ── Type aliases ──────────────────────────────────────────────────────────────

// fftwf_complex and kiss_fft_cpx both have layout {float r; float i;}.
typedef kiss_fft_cpx fftwf_complex;

// Direction flags matching FFTW conventions.
static constexpr int FFTW_FORWARD  = -1;
static constexpr int FFTW_BACKWARD = +1;

// Planning flags — all no-ops under KissFFT.
static constexpr unsigned FFTW_ESTIMATE         = 0u;
static constexpr unsigned FFTW_ESTIMATE_PATIENT = 0u;
static constexpr unsigned FFTW_PATIENT          = 0u;
static constexpr unsigned FFTW_MEASURE          = 0u;

// ── Plan struct ───────────────────────────────────────────────────────────────

struct FftwPlanImpl {
    enum class Kind { C2C, R2C } kind;
    union {
        kiss_fft_cfg  c2c;
        kiss_fftr_cfg r2c;
    };
    void *in;
    void *out;
};
typedef FftwPlanImpl *fftwf_plan;

// ── Plan creation ─────────────────────────────────────────────────────────────

inline fftwf_plan fftwf_plan_dft_1d(int n,
                                     fftwf_complex *in, fftwf_complex *out,
                                     int sign, unsigned /*flags*/)
{
    int const inverse = (sign == FFTW_BACKWARD) ? 1 : 0;
    auto *p = new FftwPlanImpl;
    p->kind = FftwPlanImpl::Kind::C2C;
    p->c2c  = kiss_fft_alloc(n, inverse, nullptr, nullptr);
    p->in   = in;
    p->out  = out;
    return p;
}

inline fftwf_plan fftwf_plan_dft_r2c_1d(int n,
                                          float *in, fftwf_complex *out,
                                          unsigned /*flags*/)
{
    auto *p = new FftwPlanImpl;
    p->kind = FftwPlanImpl::Kind::R2C;
    p->r2c  = kiss_fftr_alloc(n, 0 /*forward*/, nullptr, nullptr);
    p->in   = in;
    p->out  = out;
    return p;
}

// ── Execution ─────────────────────────────────────────────────────────────────

inline void fftwf_execute(fftwf_plan p)
{
    if (!p) return;
    if (p->kind == FftwPlanImpl::Kind::C2C) {
        kiss_fft(p->c2c,
                 static_cast<const kiss_fft_cpx *>(p->in),
                 static_cast<kiss_fft_cpx *>(p->out));
    } else {
        kiss_fftr(p->r2c,
                  static_cast<const kiss_fft_scalar *>(p->in),
                  static_cast<kiss_fft_cpx *>(p->out));
    }
}

// ── Destruction ───────────────────────────────────────────────────────────────

inline void fftwf_destroy_plan(fftwf_plan p)
{
    if (!p) return;
    if (p->kind == FftwPlanImpl::Kind::C2C)
        kiss_fft_free(p->c2c);
    else
        kiss_fft_free(p->r2c);
    delete p;
}
