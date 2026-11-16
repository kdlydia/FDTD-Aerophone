// NOLINTBEGIN
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "MayaFlux/MayaFlux.hpp"

/**
 * @warning INCLUDES: check these before writing code.
 *
 * - MAYAFLUX_NO_CONVENIENCE
 *   Skips the everyday headers and namespace imports that MayaFlux.hpp
 *   normally pulls in. It only works if defined before the MayaFlux.hpp
 *   include above, or as a compile definition (-DMAYAFLUX_NO_CONVENIENCE).
 *
 * - SimulationIncludes.hpp (OFF by default)
 *   Fields, grids, volumes and the spatial cache. Uncomment the include below.
 *
 * - WorkflowIncludes.hpp (OFF by default)
 *   Yantra compute layer, granular and vision workflows. Uncomment the
 *   include below.
 *
 * The two group headers each have MAYAFLUX_*_NO_* macros to leave parts out.
 * Define them before the include. Options are listed in the headers.
 */

// #include "MayaFlux/SimulationIncludes.hpp"
// #include "MayaFlux/WorkflowIncludes.hpp"

/**
 * @brief Engine configuration, run once before Init().
 *
 * Settings are locked for the lifetime of the process; changes after Init()
 * log a warning and have no effect. Anything left unset keeps its default
 * (48 kHz, stereo, 512 frames, 60 FPS).
 *
 * Sections: Config::get_global_stream_info(), get_global_graphics_config(),
 * get_global_input_config(), get_global_network_config(), plus the journal
 * (logging) helpers in Config. A full setup can also be loaded with
 * Config::load_config_from_file("mayaflux.json").
 *
 * The launcher loads mayaflux.json from the project folder (or the path given
 * by --config) before this runs, so values set here win. Pass
 * --config-override to load the file after this instead.
 *
 * @code
 * auto& stream = MayaFlux::Config::get_global_stream_info();
 * stream.buffer_size = 128;
 * @endcode
 */
void settings()
{
}

/**
 * @note Global scope: anything you write in this file above compose()
 * (variables, helper functions, structs) is plain C++ for use inside
 * compose(). Nothing in it runs unless compose() calls it.
 */

#include "Gui.hpp"

#include <MayaFlux/Kinesis/Discrete/Spectral.hpp>
#include <MayaFlux/Kinesis/Discrete/Taper.hpp>
#include <MayaFlux/Transitive/Memory/RingBuffer.hpp>

namespace {

// Starting values. ReedOperator's default reed (12 mm wide, 0.4 mm open) is
// sized for this bore, the same reed ReedTube.hpp uses.
constexpr double k_length = 0.6; ///< m
constexpr double k_longest = 1.2; ///< Top of the length fader, m
constexpr double k_radius = 0.0075; ///< m
constexpr double k_closing_pressure = 3000.0; ///< p_M, Pa
constexpr double k_blowing = 0.6; ///< p_mouth / p_M. With wall losses it starts near 0.5.
constexpr double k_gain = 0.3; ///< The output is in Pa at 1 m and peaks near 1.4 Pa.
constexpr double k_sound_speed = 343.0; ///< DuctSpec's default, m/s
constexpr size_t k_harmonics = 8;

/// Quarter-wave pitch of the bore with its end correction, Hz.
double expected_f0(double length)
{
    return k_sound_speed / (4.0 * (length + 0.6133 * k_radius));
}

/// The output samples, from the audio thread (one writer) to the graphics thread (one reader).
using Samples = Memory::RingBuffer<double, Memory::FixedStorage<double, 16384>, Memory::LockFreePolicy, Memory::QueueAccess>;

/// What later steps (visuals, input) reach. Never destroyed, for the reason given at Gui::keep_alive().
struct Instrument {
    std::shared_ptr<DuctNetwork> duct;
    std::shared_ptr<Samples> output;
    std::shared_ptr<Examples::Gui::Panel> panel;
};

/**
 * Level in dB of the first k_harmonics harmonics of the last 4096 output
 * samples: the power of a Hann-windowed spectrum in a band one f0 wide around
 * each harmonic, as an rms level. Used on the graphics thread only.
 */
class Harmonics {
public:
    explicit Harmonics(double sample_rate)
        : m_rate(sample_rate)
    {
        for (double w : m_taper)
            m_taper_energy += w * w;
    }

    std::vector<double> levels(Samples& output, double f0)
    {
        std::vector<double> fresh;
        while (auto v = output.pop())
            fresh.push_back(*v);
        const auto n = static_cast<std::ptrdiff_t>(std::min(fresh.size(), m_recent.size()));
        std::shift_left(m_recent.begin(), m_recent.end(), n);
        std::copy(fresh.end() - n, fresh.end(), m_recent.end() - n);

        if (f0 != m_f0)
            bands(f0);

        const auto power = Kinesis::Discrete::spectral_projection(m_recent, k_window, k_window, m_weights, m_taper);
        std::vector<double> db(k_harmonics, -100.0);
        for (size_t k = 0; k < std::min(power.size(), db.size()); ++k)
            db[k] = 10.0 * std::log10(std::max(2.0 * power[k] / (k_window * m_taper_energy), 1e-10));
        return db;
    }

private:
    static constexpr size_t k_window = 4096;

    /// Weight 1 for every bin from (k - 1/2) f0 to (k + 1/2) f0, one row per harmonic k.
    void bands(double f0)
    {
        const size_t bins = k_window / 2 + 1;
        m_weights.assign(k_harmonics * bins, 0.0);
        for (size_t k = 1; k <= k_harmonics; ++k) {
            for (size_t b = 0; b < bins; ++b) {
                const double f = static_cast<double>(b) * m_rate / static_cast<double>(k_window);
                if (f >= (static_cast<double>(k) - 0.5) * f0 && f < (static_cast<double>(k) + 0.5) * f0)
                    m_weights[(k - 1) * bins + b] = 1.0;
            }
        }
        m_f0 = f0;
    }

    double m_rate;
    double m_f0 {};
    std::vector<double> m_recent = std::vector<double>(k_window);
    std::vector<double> m_taper = Kinesis::Discrete::hann(k_window);
    double m_taper_energy {};
    std::vector<double> m_weights;
};

Instrument& instrument()
{
    static auto* held = new Instrument();
    return *held;
}

/// The values the faders last set. Only the graphics thread touches it.
struct Played {
    double blowing { k_blowing };
    double closing_pressure { k_closing_pressure };
    double length { k_length };
};

/// One fader: its label, unit, range, start value, and what it sets.
struct Fader {
    std::string name;
    std::string unit;
    double lo;
    double hi;
    double initial;
    bool log;
    std::function<void(double)> set;
};

}

/**
 * The control window. Runs on the graphics thread (see compose()). Each fader
 * writes a typed control, which the duct applies at the start of its next
 * block, or changes the length, which keeps the note sounding.
 */
void build_controls(double sample_rate)
{
    auto& inst = instrument();
    inst.panel = std::make_shared<Examples::Gui::Panel>("Reed tube", Examples::Gui::k_panel_at, 940, 0.06F, 860);
    auto& panel = *inst.panel;
    auto duct = inst.duct;
    auto played = std::make_shared<Played>();

    auto status = panel.text("");
    auto describe = [played, status, sample_rate] {
        const auto cells = Kinesis::Acoustics::duct_intervals({ .length = played->length, .sample_rate = sample_rate });
        status(std::format("expected f0 {:.1f} Hz, {} cells", expected_f0(played->length), cells));
    };

    // Peak of the last block after the gain, in dB, falling back slowly.
    auto held = std::make_shared<float>(0.F);
    panel.meter("level", [duct, held] {
        double peak = 0.0;
        if (auto block = duct->get_audio_buffer())
            for (double v : *block)
                peak = std::max(peak, std::abs(v));
        const float now = Examples::Gui::db_unit(20.0 * std::log10(std::max(peak, 1e-6)));
        *held = std::max(now, *held - 0.01F);
        return *held;
    });

    // The pressure at each grid point from the last block, mouth on the left,
    // as a fraction of p_M. Blowing at p_M fills about 80% of the height. There
    // are bars for the longest bore, so a shorter one fills part of the width.
    const auto most = Kinesis::Acoustics::duct_intervals({ .length = k_longest, .sample_rate = sample_rate }) + 1;
    panel.bars("pressure along the bore, +-p_M (grid 0.2 m, p_M / 2)", 5.5F, most, [duct, played](std::vector<double>& out) {
        const auto field = duct->pressure_field();
        std::ranges::fill(out, 0.0);
        for (size_t i = 0; i < std::min(field.size(), out.size()); ++i)
            out[i] = field[i] / played->closing_pressure;
    }, { 7, 5 });

    // Each harmonic of the expected pitch over the last five seconds, after the gain.
    std::vector<std::string> names;
    for (size_t k = 1; k <= k_harmonics; ++k)
        names.push_back(std::format("H{}", k));
    auto harmonics = std::make_shared<Harmonics>(sample_rate);
    panel.lines("harmonics at the output, -100 to -10 dB, last 5 s (grid 10 dB, 1 s)", 5.5F, names,
        Portal::Forma::Plot::AxisRange {}.range(-100.F, -10.F), 5.0,
        [output = inst.output, harmonics, played] { return harmonics->levels(*output, expected_f0(played->length)); },
        { { 0.9F, 0.3F, 0.3F }, { 0.95F, 0.6F, 0.25F }, { 0.95F, 0.85F, 0.3F }, { 0.5F, 0.85F, 0.35F },
            { 0.3F, 0.8F, 0.8F }, { 0.35F, 0.55F, 0.95F }, { 0.65F, 0.45F, 0.95F }, { 0.9F, 0.45F, 0.75F } },
        { 6, 10 });
    panel.gap();

    const std::vector<Fader> faders {
        { "blowing (p_mouth / p_M)", "", 0.0, 1.2, k_blowing, false,
            [duct, played](double v) {
                played->blowing = v;
                duct->set({ .pressure = v * played->closing_pressure });
            } },
        { "reed closing pressure p_M", "Pa", 1000.0, 8000.0, k_closing_pressure, true,
            [duct, played](double v) {
                played->closing_pressure = v;
                duct->set({ .pressure = played->blowing * v, .closing_pressure = v });
            } },
        { "bore length", "m", 0.15, k_longest, k_length, true,
            [duct, played, describe](double v) {
                if (!duct->set_length(v))
                    return;
                played->length = v;
                describe();
            } },
        { "breath noise", "", 0.0, 0.3, 0.0, false,
            [duct](double v) { duct->set({ .noise = v }); } },
        { "lip", "", 0.0, 1.0, 0.0, false,
            [duct](double v) { duct->set({ .lip = v }); } },
        { "gain", "", 0.0, 1.0, k_gain, false,
            [duct](double v) { duct->set({ .scale = v }); } },
    };

    // Each fader reports once at start, which also fills in the status line.
    for (const auto& f : faders)
        (void)panel.fader(f.name, f.unit, f.lo, f.hi, f.initial, f.set, f.log);
}

/**
 * @brief Your code. Runs after the engine has started.
 *
 * A cylindrical single-reed bore, MayaFlux's DuctNetwork, on both output
 * channels, with one control window: blowing, reed closing pressure, bore
 * length, breath noise, lip, gain, a level meter, the pressure along the bore,
 * the level of each harmonic and the expected pitch.
 */
void compose()
{
    const auto sample_rate = static_cast<double>(Config::get_sample_rate());

    auto& inst = instrument();
    inst.duct = vega.DuctNetwork(k_length, k_radius, sample_rate);
    inst.duct->set({
        .scale = k_gain,
        .pressure = k_blowing * k_closing_pressure,
        .closing_pressure = k_closing_pressure,
    });

    // One network, processed once per cycle and sent to each channel it is on.
    if (Config::get_num_out_channels() > 1)
        inst.duct = inst.duct | Audio[{ 0, 1 }];
    else
        inst.duct = inst.duct | Audio[0];

    // Each cycle runs channel 0's buffers before the networks, so this takes
    // the block the duct made in the previous cycle and misses none.
    inst.output = std::make_shared<Samples>();
    (void)attach_quick_process([duct = inst.duct, output = inst.output](const std::shared_ptr<Buffers::AudioBuffer>&) {
        if (auto block = duct->get_audio_buffer())
            for (double v : *block)
                (void)output->push(v);
    }, { 0 });

    // compose() runs on the main thread, and WindowManager::create_window adds
    // to the window list without a lock while the graphics thread copies that
    // list every frame. A FRAME_ACCURATE task runs on the graphics thread, so
    // the window is made there.
    schedule_sequence({ { 0.05, [sample_rate] {
                             try {
                                 build_controls(sample_rate);
                             } catch (const std::exception& e) {
                                 MF_ERROR(Journal::Component::USER, Journal::Context::Runtime,
                                     "Reed tube controls could not be built: {}", e.what());
                             }
                         } } },
        "reed_controls", Vruta::ProcessingToken::FRAME_ACCURATE);
}
// NOLINTEND
