// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ReedTube.hpp
 * @brief Single-reed cylindrical bore, 1D finite-difference time-domain model.
 *
 * Plain C++ with no MayaFlux includes, so the same code runs offline (for
 * checks against the Python reference) and inside a BufferProcessor.
 *
 * Grid: staggered. Pressure p sits at the N + 1 points x = l h (l = 0..N),
 * volume velocity u at the N half points x = (l + 1/2) h. With S the bore
 * cross-section, the scheme discretises
 *
 *     dp/dt = -(rho c^2 / S) du/dx
 *     du/dt = -(S / rho)     dp/dx
 *
 * and is stable for lambda = c k / h <= 1 (k = 1 / sample rate). The grid is
 * chosen so lambda is as close to 1 as the bore length allows.
 *
 * Mouthpiece end (l = 0): a quasi-static single reed. The channel opening is
 * H (1 - dp / p_M), zero once dp >= p_M (the reed beats shut), and the flow is
 * Bernoulli: U = w * opening * sign(dp) * sqrt(2 |dp| / rho), with
 * dp = p_mouth - p(0). The reed and the bore are solved together each sample
 * (one scalar equation, safeguarded Newton), so there is no one-sample delay
 * in the coupling.
 *
 * Open end (l = N): a radiation load made of an inertance (end correction
 * 0.6133 a) in parallel with a resistance. Low frequencies reflect, high
 * frequencies radiate. The resistance is set so the low-frequency radiation
 * resistance matches Levine and Schwinger.
 *
 * Output: radiated pressure at 1 m, rho / (4 pi) * dU_end / dt, in pascals.
 *
 * Not modelled yet: wall (viscothermal) losses, reed mass and damping, a
 * conical bore, tone holes.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

namespace Aerophone {

struct ReedTubeParams {
    double sample_rate { 48000.0 }; ///< Hz
    double length { 0.6 }; ///< Bore length, m. Lowest note is about c / (4 * length).
    double radius { 0.0075 }; ///< Bore radius, m.
    double sound_speed { 343.0 }; ///< m/s
    double air_density { 1.2 }; ///< kg/m^3
    double reed_opening { 4.0e-4 }; ///< Rest opening H of the reed channel, m.
    double reed_width { 0.012 }; ///< Width w of the reed channel, m.
    double closing_pressure { 3000.0 }; ///< p_M: pressure difference that shuts the reed, Pa.
    double max_length { 1.5 }; ///< Longest bore set_length() accepts, m. Storage is reserved for it.
};

class ReedTube {
public:
    explicit ReedTube(const ReedTubeParams& params = {}) { configure(params); }

    void configure(const ReedTubeParams& params)
    {
        m_params = params;
        m_params.max_length = std::max(params.max_length, params.length);
        update_grid();

        const auto most = points_for(m_params.max_length) + 1;
        m_p.reserve(most);
        m_u.reserve(most);
        m_scratch.reserve(most);

        reset();
    }

    /**
     * @brief Change the bore length while it plays.
     *
     * The grid is rebuilt for the new length. When the number of points
     * changes, pressure and flow are interpolated onto the new grid by
     * relative position, so the note slides rather than restarting. Storage
     * was reserved in configure(), so this does not allocate. The length is
     * clamped to [2 * minimum spacing, max_length].
     */
    void set_length(double length)
    {
        const double shortest = 2.0 * m_params.sound_speed / m_params.sample_rate;
        length = std::clamp(length, shortest, m_params.max_length);
        if (length == m_params.length)
            return;

        const size_t old_points = m_points;
        m_params.length = length;
        update_grid();

        if (m_points != old_points) {
            resample(m_p, old_points, m_points, 0.0);
            resample(m_u, old_points - 1, m_points - 1, 0.5);
        }
    }

    /// Change the reed's closing pressure p_M while it plays. Clamped to at least 1 Pa.
    void set_closing_pressure(double pascals)
    {
        m_params.closing_pressure = std::max(pascals, 1.0);
    }

    void reset()
    {
        m_p.assign(m_points + 1, 0.0);
        m_u.assign(m_points, 0.0);
        m_u_inertance = 0.0;
        m_u_end_prev = 0.0;
        m_delta = 0.0;
        m_reed_flow = 0.0;
    }

    /**
     * @brief Advance one sample.
     * @param mouth_pressure Blowing pressure, Pa (gauge).
     * @return Radiated pressure at 1 m, Pa.
     */
    double tick(double mouth_pressure)
    {
        const size_t n = m_points;

        for (size_t l = 0; l < n; ++l)
            m_u[l] -= m_u_coef * (m_p[l + 1] - m_p[l]);

        const double g = m_p_edge_coef;

        // Reed and mouthpiece together, with the reed flow taken at the new
        // p(0) (backward Euler at this boundary: dissipative, no Nyquist
        // ringing). The half-cell update reads p0_new = a + g U(dp), where
        // dp = mouth_pressure - p0_new.
        const double a = m_p[0] - g * m_u[0];
        m_delta = solve_reed(mouth_pressure - a, g);
        m_reed_flow = reed_flow(m_delta);
        const double p0_new = mouth_pressure - m_delta;

        for (size_t l = 1; l < n; ++l)
            m_p[l] -= m_p_coef * (m_u[l] - m_u[l - 1]);

        m_p[0] = p0_new;

        // Open end, trapezoidal in the end pressure p_bar = (old + new) / 2:
        // inertance L du_L/dt = p_bar, resistance u_R = p_bar / R. Both are
        // linear, so the end cell is solved directly and cannot add energy.
        const double p_end = m_p[n];
        const double alpha = m_k / (2.0 * m_inertance) + 1.0 / m_resistance;
        const double p_bar = (2.0 * p_end - g * (m_u_inertance - m_u[n - 1])) / (2.0 + g * alpha);
        const double u_end = m_u_inertance + alpha * p_bar;
        m_u_inertance += (m_k / m_inertance) * p_bar;
        m_p[n] = 2.0 * p_bar - p_end;

        const double out = m_params.air_density / (4.0 * std::numbers::pi)
            * (u_end - m_u_end_prev) * m_params.sample_rate;
        m_u_end_prev = u_end;
        return out;
    }

    [[nodiscard]] const ReedTubeParams& params() const { return m_params; }
    [[nodiscard]] size_t grid_points() const { return m_points; }
    [[nodiscard]] double grid_spacing() const { return m_h; }
    [[nodiscard]] double courant() const { return m_lambda; }

    /// Quarter-wave estimate with the open-end correction: c / (4 (L + 0.6133 a)).
    [[nodiscard]] static double nominal_f0(const ReedTubeParams& p)
    {
        return p.sound_speed / (4.0 * (p.length + 0.6133 * p.radius));
    }

    /// Reed parameter zeta = Z_c w H sqrt(2 / (rho p_M)). Plays roughly for 0.1 to 0.6.
    [[nodiscard]] static double zeta(const ReedTubeParams& p)
    {
        const double area = std::numbers::pi * p.radius * p.radius;
        const double impedance = p.air_density * p.sound_speed / area;
        return impedance * p.reed_width * p.reed_opening * std::sqrt(2.0 / (p.air_density * p.closing_pressure));
    }

    /// Grid cells N the scheme uses for @p p, as grid_points() reports (N + 1 pressure values).
    [[nodiscard]] static size_t grid_points(const ReedTubeParams& p)
    {
        return std::max<size_t>(2, static_cast<size_t>(std::floor(p.length * p.sample_rate / p.sound_speed)));
    }

    [[nodiscard]] double nominal_f0() const { return nominal_f0(m_params); }
    [[nodiscard]] double zeta() const { return zeta(m_params); }

    [[nodiscard]] double mouthpiece_pressure() const { return m_p[0]; }
    [[nodiscard]] double reed_flow() const { return m_reed_flow; }
    [[nodiscard]] double pressure_difference() const { return m_delta; }
    [[nodiscard]] const std::vector<double>& pressure() const { return m_p; }

private:
    [[nodiscard]] size_t points_for(double length) const
    {
        return std::max<size_t>(2, static_cast<size_t>(std::floor(length * m_params.sample_rate / m_params.sound_speed)));
    }

    /// Grid and coefficients from m_params. Leaves the state alone.
    void update_grid()
    {
        const double k = 1.0 / m_params.sample_rate;
        const double c = m_params.sound_speed;
        const double rho = m_params.air_density;

        m_points = points_for(m_params.length);
        m_h = m_params.length / static_cast<double>(m_points);
        m_lambda = c * k / m_h;

        m_area = std::numbers::pi * m_params.radius * m_params.radius;
        m_u_coef = m_area * k / (rho * m_h);
        m_p_coef = rho * c * c * k / (m_area * m_h);
        m_p_edge_coef = 2.0 * m_p_coef; // half cell at each end

        const double impedance = rho * c / m_area;
        const double end_correction = 0.6133 * m_params.radius;
        m_inertance = rho * end_correction / m_area;
        m_resistance = 4.0 * 0.6133 * 0.6133 * impedance;

        m_reed_flow_coef = m_params.reed_width * m_params.reed_opening * std::sqrt(2.0 / rho);
        m_k = k;
    }

    /**
     * @brief Interpolate a grid field from old_last + 1 to new_last + 1 values.
     *
     * Value i sits at (i + offset) / spacing along the bore: offset 0 for
     * pressure points, 0.5 for the flow half points.
     */
    void resample(std::vector<double>& field, size_t old_last, size_t new_last, double offset)
    {
        m_scratch.assign(field.begin(), field.end());
        field.resize(new_last + 1);
        const double old_cells = static_cast<double>(old_last) + 2.0 * offset;
        const double new_cells = static_cast<double>(new_last) + 2.0 * offset;
        for (size_t i = 0; i <= new_last; ++i) {
            const double at = std::clamp(
                (static_cast<double>(i) + offset) * old_cells / new_cells - offset,
                0.0, static_cast<double>(old_last));
            const auto lo = static_cast<size_t>(at);
            const size_t hi = std::min(lo + 1, old_last);
            const double t = at - static_cast<double>(lo);
            field[i] = m_scratch[lo] + t * (m_scratch[hi] - m_scratch[lo]);
        }
    }

    [[nodiscard]] double reed_flow(double dp) const
    {
        const double opening = 1.0 - dp / m_params.closing_pressure;
        if (opening <= 0.0)
            return 0.0;
        const double root = std::sqrt(std::abs(dp));
        return (dp >= 0.0 ? 1.0 : -1.0) * m_reed_flow_coef * opening * root;
    }

    [[nodiscard]] double reed_flow_slope(double dp) const
    {
        const double pm = m_params.closing_pressure;
        if (dp >= pm)
            return 0.0;
        const double root = std::sqrt(std::abs(dp));
        if (root == 0.0)
            return std::numeric_limits<double>::infinity();
        if (dp > 0.0)
            return m_reed_flow_coef * ((1.0 - dp / pm) / (2.0 * root) - root / pm);
        return m_reed_flow_coef * (root / pm + (1.0 - dp / pm) / (2.0 * root));
    }

    /**
     * @brief Solve dp + b U(dp) = q for dp.
     *
     * With b = 2 Z_c lambda the left side is increasing while
     * 2 zeta lambda < 1, so the root is unique there. Above p_M the reed is shut and dp = q. Otherwise the root is
     * bracketed in [min(q, 0) - 1, p_M] and found by Newton steps that fall
     * back to bisection when they leave the bracket.
     */
    [[nodiscard]] double solve_reed(double q, double b) const
    {
        const double pm = m_params.closing_pressure;
        if (q >= pm)
            return q;

        double lo = std::min(q, 0.0) - 1.0;
        double hi = pm;
        double x = std::clamp(m_delta, lo, hi);
        const double tol = 1e-9 * pm;

        for (int it = 0; it < 60; ++it) {
            const double fx = x + b * reed_flow(x) - q;
            if (std::abs(fx) < tol)
                break;
            (fx < 0.0 ? lo : hi) = x;
            if (hi - lo < tol)
                break;

            const double slope = 1.0 + b * reed_flow_slope(x);
            double next = (std::isfinite(slope) && slope > 0.0) ? x - fx / slope : lo - 1.0;
            if (!(next > lo && next < hi))
                next = 0.5 * (lo + hi);
            x = next;
        }
        return x;
    }

    ReedTubeParams m_params;
    size_t m_points { 0 };
    double m_h { 0.0 };
    double m_k { 0.0 };
    double m_lambda { 0.0 };
    double m_area { 0.0 };
    double m_u_coef { 0.0 };
    double m_p_coef { 0.0 };
    double m_p_edge_coef { 0.0 };
    double m_inertance { 0.0 };
    double m_resistance { 0.0 };
    double m_reed_flow_coef { 0.0 };

    std::vector<double> m_p;
    std::vector<double> m_u;
    std::vector<double> m_scratch;
    double m_u_inertance { 0.0 };
    double m_u_end_prev { 0.0 };
    double m_delta { 0.0 };
    double m_reed_flow { 0.0 };
};

} // namespace Aerophone
