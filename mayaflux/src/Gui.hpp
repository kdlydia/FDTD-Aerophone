// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <MayaFlux/Portal/Forma/Forma.hpp>
#include <MayaFlux/Portal/Forma/Plot/Plot.hpp>
#include <MayaFlux/Portal/Forma/Primitives/Dropdown.hpp>

/**
 * Small layer over Portal::Forma.
 *
 * A control window (faders, level meters and live labels) and a plot window
 * (Portal::Forma::plot, which brings ticks, legend and title). A plot maps its
 * axes onto its whole window, so the two are kept apart rather than squeezed
 * into one.
 */
namespace Examples::Gui {

namespace F = Portal::Forma;
namespace P = Portal::Forma::Plot;
using Role = Kakshya::DataDimension::Role;
using Modality = Kakshya::DataModality;

/// Seconds of history the rolling plots show, sampled at the display rate.
constexpr double k_display_rate = 60.0;

/// Display work runs on the frame clock, not the audio scheduler.
constexpr auto k_frame = Vruta::ProcessingToken::FRAME_ACCURATE;

/// Objects that must outlive compose(): windows, elements, controls, taps.
///
/// Never destroyed. A static vector would be torn down after MayaFlux::End(), and destroying a TapSet
/// or a network then reaches into a BufferManager that no longer exists. The process exit reclaims it.
inline std::vector<std::shared_ptr<void>>& keep_alive()
{
    static auto* held = new std::vector<std::shared_ptr<void>>();
    return *held;
}

/**
 * Fixed-length rolling history for a plot series.
 *
 * Pushed and read on the scheduler thread: the same metro that pushes also
 * syncs the plot, so no locking is needed.
 */
class History {
public:
    explicit History(double seconds, double fill = 0.0)
        : m_data(static_cast<size_t>(seconds * k_display_rate), fill)
    {
    }

    void push(double value)
    {
        m_data[m_next] = value;
        m_next = (m_next + 1) % m_data.size();
    }

    [[nodiscard]] size_t size() const { return m_data.size(); }

    /// Oldest first, so the newest value sits at the right edge of the plot.
    void copy_to(std::vector<double>& out) const
    {
        const size_t n = std::min(out.size(), m_data.size());
        for (size_t i = 0; i < n; ++i)
            out[i] = m_data[(m_next + i) % m_data.size()];
    }

private:
    std::vector<double> m_data;
    size_t m_next { 0 };
};

/// A plot window: call sync() at the display rate to redraw.
class PlotWindow {
public:
    PlotWindow(std::string title, glm::ivec2 position, std::shared_ptr<Kakshya::PlotContainer> container, P::SeriesSpec spec,
        glm::uvec2 size = { 900, 520 })
    {
        auto [el, surface] = F::plot(std::move(title), size.x, size.y, std::move(container), std::move(spec));
        m_el.emplace(std::move(el));
        m_surface.emplace(std::move(surface));
        m_surface->window()->set_position(static_cast<uint32_t>(position.x), static_cast<uint32_t>(position.y));
    }

    void sync() { m_el->sync(); }

    [[nodiscard]] const std::shared_ptr<Core::Window>& window() const { return m_surface->window(); }

private:
    std::optional<F::Mapped<std::shared_ptr<Kakshya::PlotContainer>>> m_el;
    std::optional<F::Surface> m_surface;
};

/// Source for a rolling plot series.
inline auto rolling(const std::shared_ptr<History>& history)
{
    return [history](std::vector<double>& s) { history->copy_to(s); };
}

/**
 * A control window: rows of labelled faders and meters, top to bottom.
 */
class Panel {
public:
    Panel(const std::string& title, glm::ivec2 position, uint32_t height = 520, float row = 0.11F, uint32_t width = 420)
        : m_row(row)
        , m_height(height)
        , m_width(width)
    {
        auto window = create_window({ .title = title, .width = width, .height = height });
        window->show();
        window->set_position(static_cast<uint32_t>(position.x), static_cast<uint32_t>(position.y));

        m_surface.emplace(F::create_surface(F::SurfaceConfig { .window = window, .name = title }));

        auto render = m_surface->render_config();
        render.additional_textures.emplace_back("text", nullptr);
        m_text = F::create_buffer(std::move(render));
    }

    F::Surface& surface() { return *m_surface; }

    [[nodiscard]] const std::shared_ptr<Core::Window>& window() const { return m_surface->window(); }

    [[nodiscard]] float aspect() const { return static_cast<float>(m_width) / static_cast<float>(m_height); }

    /// A line of text. Returns a setter that replaces it later, from any thread.
    std::function<void(const std::string&)> text(const std::string& initial, glm::vec4 color = { 0.85F, 0.85F, 0.85F, 1.F })
    {
        const auto bounds = m_cursor.advance(m_row);
        const Portal::Text::PressParams params { .color = color, .render_bounds = text_pixels(bounds) };

        auto el = F::Element {}
                      .non_interactive()
                      .with_buffer(F::FormaTarget::slice_of(m_text, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST))
                      .with_text(initial, params, bounds);
        (void)m_surface->layer().add(el);

        return [el, params](const std::string& s) mutable { el.set_text(s, params); };
    }

    void gap() { (void)m_cursor.advance(m_row * 0.5F); }

    /// A labelled line of editable text. Click it to type. Read state->value.text on the frame thread.
    std::shared_ptr<F::MappedState<Portal::Text::EditableText>> field(const std::string& name, const std::string& initial)
    {
        (void)text(name);
        const auto bounds = m_cursor.advance(m_row);
        (void)m_cursor.advance(m_row * 0.3F);

        auto params = std::make_shared<Portal::Text::PressParams>(Portal::Text::PressParams {
            .color = { 0.92F, 0.92F, 0.92F, 1.F },
            .background = { 0.12F, 0.12F, 0.14F, 1.F },
            .render_bounds = { 1000, 48 },
        });
        auto tf = std::make_shared<F::TextField>();
        tf->place(F::FormaTarget::slice_of(m_text, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST), *m_surface, bounds, params, initial);
        keep_alive().push_back(tf);
        keep_alive().push_back(params);
        return tf->state;
    }

    /**
     * A labelled fader from @p lo to @p hi. With @p log the travel is
     * logarithmic, which suits times and frequencies. @p on_change runs on the
     * graphics thread whenever the value moves, and once at start. Returns the
     * fader's state, in [0, 1] along its travel, so code can move it.
     */
    std::shared_ptr<F::MappedState<float>> fader(const std::string& name, const std::string& unit, double lo, double hi, double initial,
        std::function<void(double)> on_change, bool log = false)
    {
        auto label = text(name);
        const auto track = m_cursor.advance(m_row * 0.8F);
        (void)m_cursor.advance(m_row * 0.3F);

        const auto to_value = [lo, hi, log](double t) {
            return log ? lo * std::pow(hi / lo, t) : lo + t * (hi - lo);
        };
        const double t0 = log ? std::log(initial / lo) / std::log(hi / lo) : (initial - lo) / (hi - lo);

        auto tend = std::make_shared<F::Tend<float>>(F::Tend<float> {
            .form = F::Geometry::horizontal_fader(track, 0.05F, glm::vec3(0.25F), glm::vec3(0.3F, 0.7F, 1.F)),
            .initial = static_cast<float>(std::clamp(t0, 0.0, 1.0)),
        });

        auto last = std::make_shared<double>(-1.0);
        tend->place(*m_surface).bridge().write([=](std::span<const float> s) {
            if (s.empty() || static_cast<double>(s[0]) == *last)
                return;
            *last = s[0];
            const double v = to_value(s[0]);
            label(std::format("{}  {:.4g} {}", name, v, unit));
            on_change(v);
        });

        keep_alive().push_back(tend);
        return tend->result->state;
    }

    /**
     * A labelled horizontal level meter, refreshed every frame from @p read,
     * which should return a value in [0, 1].
     */
    void meter(const std::string& name, std::function<float()> read, glm::vec3 color = { 0.2F, 0.8F, 0.4F })
    {
        (void)text(name);
        const auto bounds = m_cursor.advance(m_row * 0.7F);
        (void)m_cursor.advance(m_row * 0.3F);

        auto tend = std::make_shared<F::Tend<float>>(F::Tend<float> {
            .form = F::Geometry::level_meter(bounds, true, color),
            .initial = 0.F,
        });
        tend->place(*m_surface).bridge().bind(std::move(read));
        keep_alive().push_back(tend);
    }

    /**
     * A bar graph @p rows tall of @p count bars, a Forma plot. @p fill writes
     * the values into a vector of that size on the graphics thread every
     * frame. Bars grow from the middle and reach the edge at +-1. @p grid is
     * the number of vertical and horizontal grid lines, edges included.
     */
    void bars(const std::string& name, float rows, uint64_t count,
        std::function<void(std::vector<double>&)> fill, glm::uvec2 grid, glm::vec3 color = { 0.95F, 0.55F, 0.3F })
    {
        (void)text(name);
        const auto bounds = m_cursor.advance(m_row * rows);
        (void)m_cursor.advance(m_row * 0.3F);

        plot_area(bounds, grid);

        auto container = P::source().as(name, count, Role::SPATIAL_Y, Modality::SCALAR_F32).from(std::move(fill)).build();
        auto spec = P::series().y(Role::SPATIAL_Y, P::AxisRange {}.range(-1.F, 1.F), { color }).bounds(bounds).as_bars().done();
        auto plot = std::make_shared<F::Mapped<std::shared_ptr<Kakshya::PlotContainer>>>(
            P::place(*m_surface, F::slice(*m_surface, spec.topology), spec, std::move(container)));

        // place() draws once. A plot redraws when it is synced.
        schedule_metro(1.0 / k_display_rate, [plot] { plot->sync(); }, name + " bars", k_frame);
        keep_alive().push_back(plot);
    }

    /**
     * A plot @p rows tall of overlapping lines, one per entry of @p names, each
     * the last @p seconds of one value, under a legend in the lines' colours.
     * @p read runs on the graphics thread every frame and returns one value
     * per line, drawn against @p range. @p grid is the number of vertical and
     * horizontal grid lines, edges included.
     */
    void lines(const std::string& name, float rows, const std::vector<std::string>& names, P::AxisRange range, double seconds,
        std::function<std::vector<double>()> read, const std::vector<glm::vec3>& colors, glm::uvec2 grid)
    {
        (void)text(name);
        const auto key = m_cursor.advance(m_row);
        const auto bounds = m_cursor.advance(m_row * rows);
        (void)m_cursor.advance(m_row * 0.3F);

        plot_area(bounds, grid);

        // One plot per line. A waveform of several series joins the end of one
        // to the start of the next with a segment that still shows.
        using Plotted = F::Mapped<std::shared_ptr<Kakshya::PlotContainer>>;
        std::vector<std::shared_ptr<History>> histories;
        std::vector<std::shared_ptr<Plotted>> plots;
        const float cell = key.width() / static_cast<float>(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            const glm::vec3 color = colors[i % colors.size()];
            auto history = std::make_shared<History>(seconds, range.min);
            auto container = P::source().as(names[i], history->size(), Role::SPATIAL_Y, Modality::SCALAR_F32).from(rolling(history)).build();
            auto spec = P::series().y(Role::SPATIAL_Y, range, { color }).bounds(bounds).as_waveform().done();
            auto plot = std::make_shared<Plotted>(P::place(*m_surface, F::slice(*m_surface, spec.topology), spec, std::move(container)));
            histories.push_back(history);
            plots.push_back(plot);
            keep_alive().push_back(plot);

            const float x = key.min.x + cell * static_cast<float>(i);
            label(names[i], { .min = { x, key.min.y }, .max = { x + cell, key.max.y } }, glm::vec4(color, 1.F));
        }

        schedule_metro(1.0 / k_display_rate, [plots, histories, read = std::move(read)] {
            const auto values = read();
            for (size_t i = 0; i < std::min(values.size(), histories.size()); ++i)
                histories[i]->push(values[i]);
            for (const auto& plot : plots)
                plot->sync();
        }, name + " lines", k_frame);
    }

    /**
     * A drawable curve of @p initial.size() points in [0, 1], @p rows tall.
     * @p on_change runs on the graphics thread with every new curve. Returns
     * the state so code can redraw the curve with state->write().
     */
    std::shared_ptr<F::MappedState<std::vector<float>>> canvas(const std::string& name, std::vector<float> initial,
        float rows, std::function<void(std::span<const float>)> on_change, glm::vec3 color = { 0.9F, 0.75F, 0.3F })
    {
        (void)text(name);
        const auto bounds = m_cursor.advance(m_row * rows);
        (void)m_cursor.advance(m_row * 0.3F);

        auto tend = std::make_shared<F::Tend<std::vector<float>>>(F::Tend<std::vector<float>> {
            .form = F::Geometry::drawable_canvas(bounds, color, 2.F),
            .initial = std::move(initial),
        });

        auto last = std::make_shared<std::vector<float>>();
        tend->place(*m_surface).bridge().write([on_change = std::move(on_change), last](std::span<const float> s) {
            if (std::ranges::equal(s, *last))
                return;
            last->assign(s.begin(), s.end());
            on_change(s);
        });

        keep_alive().push_back(tend);
        return tend->result->state;
    }

    /**
     * A dropdown of @p options. Its list opens over the rows below it, so put
     * it where that is acceptable.
     */
    void choice(std::vector<std::string> options, size_t initial, std::function<void(size_t)> on_select)
    {
        auto dropdown = std::make_shared<F::Dropdown>();
        dropdown->options(std::move(options)).initial(initial).on_select(std::move(on_select)).place(*m_surface, m_cursor, m_row);
        keep_alive().push_back(dropdown);
    }

    /// A labelled on/off box. @p on_change runs on the graphics thread when it flips, and once at start.
    std::shared_ptr<F::MappedState<bool>> toggle(const std::string& name, bool initial, std::function<void(bool)> on_change)
    {
        (void)text(name);
        const auto row = m_cursor.advance(m_row * 0.7F);
        (void)m_cursor.advance(m_row * 0.3F);
        const Kinesis::AABB2D box { row.min, { row.min.x + 0.18F, row.max.y } };

        auto tend = std::make_shared<F::Tend<bool>>(F::Tend<bool> {
            .form = F::Geometry::toggle(box, glm::vec3(0.25F), glm::vec3(0.95F, 0.45F, 0.3F)),
            .initial = initial,
        });

        auto last = std::make_shared<int>(-1);
        tend->place(*m_surface).bridge().write([on_change = std::move(on_change), last](std::span<const float> s) {
            const int now = !s.empty() && s[0] > 0.5F ? 1 : 0;
            if (now == *last)
                return;
            *last = now;
            on_change(now == 1);
        });

        keep_alive().push_back(tend);
        return tend->result->state;
    }

    /// A 2D area to drag a point in, over a dark background.
    struct PickArea {
        std::shared_ptr<F::MappedState<glm::vec2>> state;
        Kinesis::AABB2D bounds;
    };

    /**
     * A point in [0, 1] x [0, 1] (y up) dragged inside an area @p rows tall.
     * @p on_change runs on the graphics thread with every move, including
     * moves written from code with state->write().
     */
    PickArea picker(const std::string& name, float rows, glm::vec2 initial, std::function<void(glm::vec2)> on_change,
        glm::vec3 color = { 0.95F, 0.45F, 0.3F })
    {
        (void)text(name);
        const auto bounds = m_cursor.advance(m_row * rows);
        (void)m_cursor.advance(m_row * 0.3F);

        rect(bounds, glm::vec3(0.1F, 0.1F, 0.12F));

        auto tend = std::make_shared<F::Tend<glm::vec2>>(F::Tend<glm::vec2> {
            .form = F::Geometry::position_picker(bounds, color, 14.F),
            .initial = initial,
        });

        tend->place(*m_surface).bridge().write([on_change = std::move(on_change)](std::span<const float> s) {
            if (s.size() >= 2)
                on_change({ s[0], s[1] });
        });

        keep_alive().push_back(tend);
        return { tend->result->state, bounds };
    }

    /// A short piece of text anywhere, @p height NDC tall, for marking things inside an area.
    void text_at(const std::string& s, glm::vec2 centre, float height, glm::vec4 color = { 0.6F, 0.6F, 0.65F, 1.F })
    {
        // A square texture keeps short labels in proportion whatever the window's shape.
        const float width = height * static_cast<float>(m_height) / static_cast<float>(m_width);
        const Kinesis::AABB2D region { centre - glm::vec2(width, height) * 0.5F, centre + glm::vec2(width, height) * 0.5F };
        const Portal::Text::PressParams params { .color = color, .render_bounds = { 48, 48 } };

        auto el = F::Element {}
                      .non_interactive()
                      .with_buffer(F::FormaTarget::slice_of(m_text, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST))
                      .with_text(s, params, region);
        (void)m_surface->layer().add(el);
    }

    /// A flat rectangle, drawn behind whatever is placed after it.
    void rect(Kinesis::AABB2D bounds, glm::vec3 color)
    {
        if (!m_shapes)
            m_shapes = F::create_buffer(m_surface->render_config());
        auto target = F::FormaTarget::slice_of(m_shapes, Portal::Graphics::PrimitiveTopology::TRIANGLE_STRIP);
        target.submit(Kinesis::filled_rect(bounds, glm::vec4(color, 1.F)));
        (void)m_surface->layer().add(F::Element {}.with_bounds(bounds).with_buffer(target).non_interactive());
    }

private:
    /// Texture size for text in @p bounds: 48 pixels tall and the bounds' shape, so glyphs keep their proportions.
    [[nodiscard]] glm::uvec2 text_pixels(Kinesis::AABB2D bounds) const
    {
        const float aspect = bounds.width() * static_cast<float>(m_width) / (bounds.height() * static_cast<float>(m_height));
        return { static_cast<uint32_t>(48.F * aspect), 48 };
    }

    /// A plot's background and grid. The surface's shared buffer draws in the
    /// order things are placed, so placing these before the data puts them behind it.
    void plot_area(Kinesis::AABB2D bounds, glm::uvec2 grid)
    {
        (void)P::place_rect(*m_surface, F::slice(*m_surface, Portal::Graphics::PrimitiveTopology::TRIANGLE_STRIP),
            { .bounds = bounds, .color = glm::vec3(0.1F, 0.1F, 0.12F) });

        auto target = F::slice(*m_surface, Portal::Graphics::PrimitiveTopology::LINE_LIST);
        target.submit(P::plot_grid(bounds, grid.x, grid.y, glm::vec3(0.25F)));
        (void)m_surface->layer().add(F::Element {}.with_bounds(bounds).with_buffer(target).non_interactive());
    }

    /// A fixed piece of text in @p region.
    void label(const std::string& s, Kinesis::AABB2D region, glm::vec4 color)
    {
        const Portal::Text::PressParams params { .color = color, .render_bounds = text_pixels(region) };
        (void)m_surface->layer().add(F::Element {}
                .non_interactive()
                .with_buffer(F::FormaTarget::slice_of(m_text, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST))
                .with_text(s, params, region));
    }

    float m_row { 0.11F };
    uint32_t m_height { 520 };
    uint32_t m_width { 420 };
    std::shared_ptr<Buffers::FormaBuffer> m_shapes;

    std::optional<F::Surface> m_surface;
    std::shared_ptr<Buffers::FormaBuffer> m_text;
    F::LayoutCursor m_cursor { 0.95F, -0.9F, 0.9F };
};

/// Map decibels in [floor_db, 0] onto [0, 1] for a meter.
inline float db_unit(double db, double floor_db = -60.0)
{
    return static_cast<float>(std::clamp((db - floor_db) / -floor_db, 0.0, 1.0));
}

/// Where the two windows of an example go.
constexpr glm::ivec2 k_plot_at { 40, 60 };
constexpr glm::ivec2 k_panel_at { 960, 60 };

} // namespace Examples::Gui
