# FDTD-Aerophone

A real-time finite-difference time-domain (FDTD) model of a single-reed
aerophone. Final project for 21M.589.

The bore is a 1D grid of pressure and volume flow. A reed at the mouthpiece
lets air in by Bernoulli's law and beats shut when the pressure difference
gets too large, and the open end radiates. Details in
[docs/model.md](docs/model.md).

## Layout

- `mayaflux/`: the model in C++ and a playable instrument built on
  [MayaFlux](https://github.com/MayaFlux/MayaFlux)
- `docs/`: notes on the model, references

The Python reference and the FPGA design will go in `python/` and `fpga/`.

## Building the instrument

Needs the MayaFlux 1.0.0-dev nightly from 15 Nov 2026 or later, CMake 3.31+,
Ninja and a Vulkan driver. Tested on Arch.

```sh
cd mayaflux
cmake --preset release
cmake --build build
./build/aerophone
```

The faders set blowing pressure, reed closing pressure, bore length and gain.
Ctrl-C quits.

The model itself, `mayaflux/src/ReedTube.hpp`, doesn't depend on MayaFlux. To
render a WAV without it:

```sh
cd mayaflux && mkdir -p build
g++ -std=c++23 -O2 -I src tools/render_reed.cpp -o build/render_reed
./build/render_reed 2 1500 build/reed.wav   # seconds, mouth pressure in Pa, output file
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
