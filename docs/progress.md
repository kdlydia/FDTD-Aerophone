# Progress

Where things stand on 25 Nov 2026.

## Working now

The instrument in `mayaflux/` plays in real time. It is MayaFlux's
`DuctNetwork`: a 0.6 m cylindrical bore on an 83-cell grid, a reed at the
mouth, wall losses and a radiating open end, all at 48 kHz. The window has
six faders (blowing, reed closing pressure, bore length, breath noise, lip,
gain), a level meter, the pressure along the bore and the levels of the
first eight harmonics. The whole app, audio and graphics, uses about 12% of
one core on an i7-1185G7 laptop.

A second window makes multiphonics: a second bore at an interval to the first
(a dial from 1.25 to 2.9 times its pitch), blown the same way, mixed in and
shaped into sum and difference tones. It also has a burst button, and both
windows have a mute. See [model.md](model.md).

[model.md](model.md) has the scheme. [demo.mp4](demo.mp4) and
[walkthrough.mp4](walkthrough.mp4) show it playing.

Measured from the app's output:

| length | measured | c / (4(L + 0.6133a)) |
|---|---|---|
| 0.3 m | 281.2 Hz | 281.5 Hz |
| 0.6 m | 141.7 Hz | 141.8 Hz |
| 1.2 m | 71.1 Hz | 71.2 Hz |

At the default settings H1, H3, H5 and H7 are within 1 dB of each other.
The even harmonics are much weaker, as in a clarinet: H2 is 37 dB down and
H8 27 dB down. The 0.6 m case was checked again on 25 Nov with the 24 Nov
MayaFlux nightly and gave the same numbers. The others are from 22 Nov.

## Against the proposal

| proposal | state |
|---|---|
| Python/NumPy reference | not written; `DuctNetwork` could be the reference instead |
| cylindrical bore on CPU | done, real time |
| conical bore | `DuctNetwork` takes any bore profile; the instrument doesn't use it yet |
| GPU port | not started |
| fixed-point SystemVerilog on the XC7A100T | not started |
| CPU/GPU/FPGA benchmarks | not started |
| playable controls | done as on-screen faders; no MIDI yet |
| multiphonics (stretch) | a first version: two bores mixed and shaped, see model.md |

## Proposed next

1. Verilog for the Artix, checked in Verilator against `DuctNetwork`'s
   output on the same grid. Two parts won't map straight to hardware: the
   reed is solved with Newton steps every sample (the reed with its own
   resonance needs one square root instead), and the wall losses solve along
   the whole bore every sample.
2. A small UART protocol, so the app can drive the board over serial with
   the same faders and we can listen to both side by side.
3. Benchmark FPGA against CPU on the same grid, in place of the GPU port.
4. A conical bore in the instrument.

## Open questions

See the end of [model.md](model.md): a second, louder state with a strong
third harmonic that showed up once at the default settings, and two
problems of the earlier model that haven't been rechecked.
