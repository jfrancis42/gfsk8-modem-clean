#pragma once
/**
 * @file tracker.h
 * @brief Kalman-style frequency and timing trackers for the JS8 decoder.
 *
 * FrequencyTracker: tracks residual carrier-frequency offset (Hz) using pilot
 *   tones from Costas arrays.  Applied per-sample to complex baseband data.
 *
 * TimingTracker: tracks residual symbol-timing offset (samples) using
 *   early/late energy measurements on pilot tones.
 *
 * Both use a first-order exponential smoother (single alpha coefficient) with
 * hard clamps on step size and total accumulated error.
 */

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace gfsk8 {

// ── FrequencyTracker ─────────────────────────────────────────────────────────

class FrequencyTracker {
public:
    /// Enable tracking with the given initial estimate and parameters.
    void reset(double initial_hz, double sample_rate_hz,
               double alpha = 0.15, double max_step_hz = 0.3,
               double max_error_hz = 5.0)
    {
        m_enabled     = true;
        m_est_hz      = initial_hz;
        m_fs          = sample_rate_hz;
        m_alpha       = alpha;
        m_max_step    = max_step_hz;
        m_max_error   = max_error_hz;
        m_sum_abs     = 0.0;
        m_updates     = 0;
    }

    void disable() noexcept { m_enabled = false; }

    [[nodiscard]] bool   enabled()       const noexcept { return m_enabled; }
    [[nodiscard]] double currentHz()     const noexcept { return m_est_hz; }
    [[nodiscard]] double averageStepHz() const noexcept
    {
        return m_updates > 0 ? m_sum_abs / static_cast<double>(m_updates) : 0.0;
    }

    /// Rotate each sample by the accumulated frequency correction.
    void apply(std::complex<float> *data, int count) const
    {
        if (!m_enabled || !data || count <= 0 || m_fs <= 0.0) return;
        double const dphi = 2.0 * std::numbers::pi * m_est_hz / m_fs;
        auto w = std::polar(1.0f, static_cast<float>(dphi));
        auto acc = std::complex<float>{1.0f, 0.0f};
        for (int i = 0; i < count; ++i) {
            acc *= w;
            data[i] *= acc;
        }
    }

    /// Feed a residual-Hz measurement (typically from a parabolic peak interpolation).
    void update(double residual_hz, double weight = 1.0)
    {
        if (!m_enabled || m_fs <= 0.0) return;
        if (!std::isfinite(residual_hz) || !std::isfinite(weight) || weight <= 0.0) return;
        if (std::abs(residual_hz) > m_max_error) return;
        residual_hz *= std::min(weight, 1.0);
        double const step = std::clamp(residual_hz, -m_max_step, m_max_step);
        m_est_hz  += m_alpha * step;
        m_sum_abs += std::abs(step);
        ++m_updates;
    }

private:
    bool   m_enabled   = false;
    double m_est_hz    = 0.0;
    double m_fs        = 0.0;
    double m_alpha     = 0.15;
    double m_max_step  = 0.3;
    double m_max_error = 5.0;
    double m_sum_abs   = 0.0;
    int    m_updates   = 0;
};

// ── TimingTracker ─────────────────────────────────────────────────────────────

class TimingTracker {
public:
    /// Enable tracking with the given initial estimate and parameters.
    void reset(double initial_samples, double alpha = 0.15,
               double max_step = 0.35, double max_total = 2.0)
    {
        m_enabled   = true;
        m_est       = initial_samples;
        m_alpha     = alpha;
        m_max_step  = max_step;
        m_max_total = max_total;
        m_sum_abs   = 0.0;
        m_updates   = 0;
    }

    void disable() noexcept { m_enabled = false; }

    [[nodiscard]] bool   enabled()            const noexcept { return m_enabled; }
    [[nodiscard]] double currentSamples()     const noexcept { return m_est; }
    [[nodiscard]] double averageStepSamples() const noexcept
    {
        return m_updates > 0 ? m_sum_abs / static_cast<double>(m_updates) : 0.0;
    }

    /// Feed a residual timing measurement in samples.
    void update(double residual_samples, double weight = 1.0)
    {
        if (!m_enabled) return;
        if (!std::isfinite(residual_samples) || !std::isfinite(weight) || weight <= 0.0) return;
        residual_samples *= std::min(weight, 1.0);
        double const step = std::clamp(residual_samples, -m_max_step, m_max_step);
        double const next = m_est + m_alpha * step;
        if (std::abs(next) > m_max_total) return;
        m_est     = next;
        m_sum_abs += std::abs(step);
        ++m_updates;
    }

private:
    bool   m_enabled   = false;
    double m_est       = 0.0;
    double m_alpha     = 0.15;
    double m_max_step  = 0.35;
    double m_max_total = 2.0;
    double m_sum_abs   = 0.0;
    int    m_updates   = 0;
};

} // namespace gfsk8
