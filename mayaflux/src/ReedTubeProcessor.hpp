// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ReedTube.hpp"

#include <MayaFlux/Buffers/AudioBuffer.hpp>
#include <MayaFlux/Buffers/BufferProcessor.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

/**
 * @brief Runs a ReedTube once per sample and writes its output into an AudioBuffer.
 *
 * The model's state lives here, so attach one processor to one buffer.
 *
 * Every setter only stores an atomic, so any thread may call it: a fader on
 * the graphics thread, an input node's hook on the input thread. The audio
 * side reads the targets once at the start of each block and applies them
 * there, so the model itself is only ever touched by the audio thread. The
 * blowing pressure glides over about 10 ms, so a change never clicks.
 */
class ReedTubeProcessor : public MayaFlux::Buffers::BufferProcessor {
public:
    /// Highest blowing ratio accepted. At 1 the reed is shut at rest.
    static constexpr double k_max_blowing = 0.95;

    explicit ReedTubeProcessor(const Aerophone::ReedTubeParams& params)
        : m_model(params)
        , m_glide(1.0 - std::exp(-1.0 / (0.01 * params.sample_rate)))
        , m_closing_pressure(params.closing_pressure)
        , m_length(params.length)
    {
    }

    void processing_function(const std::shared_ptr<MayaFlux::Buffers::Buffer>& buffer) override
    {
        auto audio = std::dynamic_pointer_cast<MayaFlux::Buffers::AudioBuffer>(buffer);
        if (!audio)
            return;

        apply_targets();

        const double target = m_blowing.load(std::memory_order_relaxed) * m_model.params().closing_pressure;
        const double gain = m_gain.load(std::memory_order_relaxed);

        for (double& sample : audio->get_data()) {
            m_pressure += m_glide * (target - m_pressure);
            sample = gain * m_model.tick(m_pressure);
        }
    }

    [[nodiscard]] bool is_compatible_with(const std::shared_ptr<MayaFlux::Buffers::Buffer>& buffer) const override
    {
        return std::dynamic_pointer_cast<MayaFlux::Buffers::AudioBuffer>(buffer) != nullptr;
    }

    /**
     * @brief Blowing pressure as gamma = p_mouth / p_M, clamped to [0, 0.95].
     *
     * A lossless cylinder starts to sound near 1/3. Expressing it as a ratio
     * keeps the playing regime when the closing pressure changes.
     */
    void set_blowing(double gamma)
    {
        m_blowing.store(std::clamp(gamma, 0.0, k_max_blowing), std::memory_order_relaxed);
    }

    /// Reed closing pressure p_M, Pa.
    void set_closing_pressure(double pascals) { m_closing_pressure.store(pascals, std::memory_order_relaxed); }

    /// Bore length, m. The grid follows at the next block, keeping the note going.
    void set_length(double metres) { m_length.store(metres, std::memory_order_relaxed); }

    /// Output gain applied to the radiated pressure (Pa at 1 m).
    void set_gain(double gain) { m_gain.store(gain, std::memory_order_relaxed); }

    [[nodiscard]] double blowing() const { return m_blowing.load(std::memory_order_relaxed); }

private:
    void apply_targets()
    {
        const double closing = m_closing_pressure.load(std::memory_order_relaxed);
        if (closing != m_model.params().closing_pressure)
            m_model.set_closing_pressure(closing);

        const double length = m_length.load(std::memory_order_relaxed);
        if (length != m_model.params().length)
            m_model.set_length(length);
    }

    Aerophone::ReedTube m_model;
    double m_glide;
    double m_pressure { 0.0 };
    std::atomic<double> m_blowing { 0.0 };
    std::atomic<double> m_closing_pressure;
    std::atomic<double> m_length;
    std::atomic<double> m_gain { 0.1 };
};
