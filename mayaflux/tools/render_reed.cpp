// SPDX-License-Identifier: GPL-3.0-or-later
// Offline render of the reed-tube model: prints the grid and the played pitch,
// and writes a mono 16-bit WAV. No MayaFlux needed.
//
//   g++ -std=c++23 -O2 -I src tools/render_reed.cpp -o build/render_reed
//   ./build/render_reed [seconds] [mouth_pressure_Pa] [out.wav]

#include "ReedTube.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace {

void write_wav(const char* path, const std::vector<double>& x, double rate, double scale)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<uint32_t>(x.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate) * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    for (double v : x) {
        const double s = std::clamp(v * scale, -1.0, 1.0);
        u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(s * 32767.0))));
    }
}

/// Period by autocorrelation over a window, searching 40 Hz to 2 kHz.
double estimate_f0(const std::vector<double>& x, size_t start, size_t len, double rate)
{
    const auto lag_min = static_cast<size_t>(rate / 2000.0);
    const auto lag_max = static_cast<size_t>(rate / 40.0);
    std::vector<double> acc(lag_max + 1, 0.0);
    double best = 0.0;
    for (size_t lag = lag_min; lag <= lag_max; ++lag) {
        for (size_t i = 0; i < len; ++i)
            acc[lag] += x[start + i] * x[start + i + lag];
        best = std::max(best, acc[lag]);
    }
    // First local peak within 10% of the best, so multiples of the period lose.
    for (size_t lag = lag_min + 1; lag < lag_max; ++lag) {
        if (acc[lag] >= 0.9 * best && acc[lag] >= acc[lag - 1] && acc[lag] >= acc[lag + 1])
            return rate / static_cast<double>(lag);
    }
    return 0.0;
}

}

int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    Aerophone::ReedTubeParams params;
    const double mouth = argc > 2 ? std::atof(argv[2]) : 0.5 * params.closing_pressure;
    const char* out_path = argc > 3 ? argv[3] : "build/reed.wav";

    Aerophone::ReedTube tube(params);
    const auto total = static_cast<size_t>(seconds * params.sample_rate);
    const auto attack = static_cast<size_t>(0.03 * params.sample_rate);

    std::vector<double> out(total);
    bool finite = true;
    for (size_t i = 0; i < total; ++i) {
        const double ramp = i < attack ? static_cast<double>(i) / static_cast<double>(attack) : 1.0;
        out[i] = tube.tick(mouth * ramp);
        finite = finite && std::isfinite(out[i]);
    }

    const size_t tail = std::min<size_t>(total / 2, static_cast<size_t>(0.5 * params.sample_rate));
    const size_t tail_start = total - tail;
    double peak = 0.0;
    double sum2 = 0.0;
    for (size_t i = tail_start; i < total; ++i) {
        peak = std::max(peak, std::abs(out[i]));
        sum2 += out[i] * out[i];
    }
    const double rms = std::sqrt(sum2 / static_cast<double>(tail));
    const size_t lag_max = static_cast<size_t>(params.sample_rate / 40.0);
    const double f0 = tail > 2 * lag_max ? estimate_f0(out, tail_start, tail - lag_max, params.sample_rate) : 0.0;

    std::printf("grid points %zu, h %.2f mm, lambda %.4f, zeta %.3f\n",
        tube.grid_points(), tube.grid_spacing() * 1e3, tube.courant(), tube.zeta());
    std::printf("mouth pressure %.0f Pa (gamma %.2f)\n", mouth, mouth / params.closing_pressure);
    std::printf("last %.2f s: rms %.4g Pa, peak %.4g Pa, finite %s\n",
        static_cast<double>(tail) / params.sample_rate, rms, peak, finite ? "yes" : "NO");
    std::printf("f0 %.1f Hz (quarter-wave estimate %.1f Hz)\n", f0, tube.nominal_f0());

    const double scale = peak > 0.0 ? 0.5 / peak : 1.0;
    write_wav(out_path, out, params.sample_rate, scale);
    std::printf("wrote %s (normalised to peak 0.5, gain %.3g)\n", out_path, scale);
    return finite ? 0 : 1;
}
