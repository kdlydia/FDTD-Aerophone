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

Needs the MayaFlux 1.0.0-dev nightly from 16 Nov 2026 or later (for
`DuctNetwork`), CMake 3.31+,
Ninja and a Vulkan driver. Tested on Arch.

```sh
cd mayaflux
cmake --preset release
cmake --build build
./build/aerophone
```

The sound comes from MayaFlux's `DuctNetwork`. The faders set blowing
pressure, reed closing pressure, bore length, breath noise, lip and gain. Two
plots show the pressure along the bore and the level of the first eight
harmonics. Ctrl-C quits.

Our own version of the model, `mayaflux/src/ReedTube.hpp`, doesn't depend on
MayaFlux and is kept as the reference. To render a WAV with it:

```sh
cd mayaflux && mkdir -p build
g++ -std=c++23 -O2 -I src tools/render_reed.cpp -o build/render_reed
./build/render_reed 2 1500 build/reed.wav   # seconds, mouth pressure in Pa, output file
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
