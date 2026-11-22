# FDTD-Aerophone

A real-time finite-difference time-domain (FDTD) model of a single-reed
aerophone. Final project for 21M.589.

The bore is a 1D grid of pressure and volume flow. A reed at the mouthpiece
lets air in by Bernoulli's law and beats shut when the pressure difference
gets too large, and the open end radiates. Details in
[docs/model.md](docs/model.md).

## Layout

- `mayaflux/`: the playable instrument, built on the `DuctNetwork` of
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

No MayaFlux? [docs/demo.mp4](docs/demo.mp4) is 30 s of it playing while the
faders move: blowing up and down, bore length 0.6 to 0.3 to 1.0 m, lip, reed
closing pressure, then breath noise.
[docs/walkthrough.mp4](docs/walkthrough.mp4) is a minute long and moves one
fader at a time, with captions giving the pitch and harmonic levels measured
from the recording.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
