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
#include "ReedTubeProcessor.hpp"

namespace {

/// What later steps (visuals, input) reach. Never destroyed, for the reason given at Gui::keep_alive().
struct Instrument {
    std::shared_ptr<Buffers::AudioBuffer> buffer;
    std::shared_ptr<ReedTubeProcessor> reed;
    std::shared_ptr<Examples::Gui::Panel> panel;
};

Instrument& instrument()
{
    static auto* held = new Instrument();
    return *held;
}

/// One fader: its label, unit, range, start value, and what it sets.
struct Control {
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
 * The control window. Runs on the graphics thread (see compose()).
 */
void build_controls(const Aerophone::ReedTubeParams& params)
{
    auto& inst = instrument();
    inst.panel = std::make_shared<Examples::Gui::Panel>("Reed tube", Examples::Gui::k_panel_at, 520, 0.11F, 520);
    auto& panel = *inst.panel;

    // Mirrors the faders on the graphics thread, only to describe the bore.
    auto shown = std::make_shared<Aerophone::ReedTubeParams>(params);
    auto status = panel.text("");
    auto describe = [shown, status] {
        status(std::format("expected f0 {:.1f} Hz, zeta {:.3f}, {} cells",
            Aerophone::ReedTube::nominal_f0(*shown), Aerophone::ReedTube::zeta(*shown),
            Aerophone::ReedTube::grid_points(*shown)));
    };
    panel.gap();

    auto reed = inst.reed;
    const std::vector<Control> controls {
        { "blowing (p_mouth / p_M)", "", 0.0, ReedTubeProcessor::k_max_blowing, 0.5, false,
            [reed](double v) { reed->set_blowing(v); } },
        { "reed closing pressure p_M", "Pa", 1000.0, 8000.0, params.closing_pressure, true,
            [reed, shown, describe](double v) { reed->set_closing_pressure(v); shown->closing_pressure = v; describe(); } },
        { "bore length", "m", 0.15, 1.2, params.length, true,
            [reed, shown, describe](double v) { reed->set_length(v); shown->length = v; describe(); } },
        { "gain", "", 0.0, 0.3, 0.1, false,
            [reed](double v) { reed->set_gain(v); } },
    };

    // Each fader reports once at start, which also fills in the status line.
    for (const auto& c : controls)
        (void)panel.fader(c.name, c.unit, c.lo, c.hi, c.initial, c.set, c.log);
}

/**
 * @brief Your code. Runs after the engine has started.
 *
 * Step 2: the cylindrical single-reed bore on both output channels, with one
 * control window: blowing, reed closing pressure, bore length and gain, and a
 * line showing the expected pitch.
 */
void compose()
{
    Aerophone::ReedTubeParams params;
    params.sample_rate = static_cast<double>(Config::get_sample_rate());

    auto& inst = instrument();
    inst.reed = std::make_shared<ReedTubeProcessor>(params);
    inst.reed->set_blowing(0.5);

    inst.buffer = vega.AudioBuffer(0, Config::get_buffer_size()) | Audio[0];
    inst.buffer->get_processing_chain()->add_processor(inst.reed, inst.buffer);
    if (Config::get_num_out_channels() > 1)
        supply_buffer_to_channel(inst.buffer, 1);

    // compose() runs on the main thread, and WindowManager::create_window adds
    // to the window list without a lock while the graphics thread copies that
    // list every frame. A FRAME_ACCURATE task runs on the graphics thread, so
    // the window is made there.
    schedule_sequence({ { 0.05, [params] {
                             try {
                                 build_controls(params);
                             } catch (const std::exception& e) {
                                 MF_ERROR(Journal::Component::USER, Journal::Context::Runtime,
                                     "Reed tube controls could not be built: {}", e.what());
                             }
                         } } },
        "reed_controls", Vruta::ProcessingToken::FRAME_ACCURATE);
}
// NOLINTEND
