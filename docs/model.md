# The model

What `mayaflux/src/ReedTube.hpp` computes. The scheme follows Bilbao,
*Numerical Sound Synthesis* (2009).

## Bore

A cylinder of length L and radius a, cross-section S = πa²:

    dp/dt = -(ρc² / S) du/dx
    du/dt = -(S / ρ)   dp/dx

Staggered grid: pressure p at the N + 1 points x = lh, volume velocity u at
the N half points between them, with N = floor(L fs / c) and h = L / N. That
keeps the Courant number λ = ck/h just under 1 (0.9885 at 0.6 m, 48 kHz).
Each sample updates u from the pressure differences, then the interior p from
the new u. The end cells are half cells.

## Reed

Quasi-static, so no mass. With Δp = p_mouth − p(0):

    opening = H (1 − Δp / p_M),   zero once Δp ≥ p_M
    U       = w · opening · sign(Δp) · sqrt(2|Δp| / ρ)

The flow rises up to Δp = p_M/3 and falls to zero at p_M. That falling part
is what lets the bore drive the reed into oscillation.

The reed and the mouthpiece cell are solved together every sample, as one
scalar equation (Newton with a bisection fallback), so there's no one-sample
delay in the coupling. The flow is evaluated at the new p(0) (backward Euler).
That damps a little but avoids ringing at Nyquist.

ζ = Z_c w H sqrt(2 / (ρ p_M)) with Z_c = ρc/S, which is 0.264 for the
defaults.

## Open end

An inertance with end correction 0.6133a, in parallel with a resistance
R = 4 · 0.6133² · Z_c (Levine and Schwinger at low frequency). Updated with
the trapezoidal rule and solved directly, so it can't add energy. An explicit
update of the inertance blew up within the first thousand samples.

## Output

Radiated pressure at 1 m, ρ/(4π) · dU_end/dt, in Pa.

## Defaults

| | |
|---|---|
| sample rate | 48 kHz |
| length L | 0.6 m (fader 0.15 to 1.2 m) |
| radius a | 7.5 mm |
| c, ρ | 343 m/s, 1.2 kg/m³ |
| rest opening H | 0.4 mm |
| reed width w | 12 mm |
| closing pressure p_M | 3000 Pa |

## Changing length while playing

`set_length` rebuilds the grid. If N changes, p and u are interpolated onto
the new grid by relative position, so the note slides instead of restarting.
Storage for the longest bore is reserved up front, so nothing allocates on the
audio thread.

## Results so far

At 0.6 m, 800 Pa of blowing pressure dies out. From 1200 to 2600 Pa it plays
141.6 to 142.0 Hz, and the quarter-wave estimate c / (4(L + 0.6133a)) is
141.8 Hz. The spectrum has only odd harmonics with the 5th strongest, as
you'd expect from a lossless cylinder. Sliding from 0.6 to 0.3 m lands on
281 Hz. 60 s of audio takes about half a second to render on one laptop core.

## Known problems

1. Blowing above p_M gives a tone near 750 Hz instead of silence. The blowing
   fader stops at 0.95 p_M for now.
2. At 0.2 m with p_M = 6000 Pa there's a strong f0/2 component. Could be real
   period doubling, or the grid is too coarse at 27 cells.
3. No wall losses yet, so it sounds buzzy.

## Next

MayaFlux now has a `DuctNetwork` that runs the same kind of scheme inside the
engine ([MayaFlux#187](https://github.com/MayaFlux/MayaFlux/issues/187)). It
adds wall losses, arbitrary bore profiles (cones), a second-order radiation
load and an optional reed with its own dynamics. It plays 141.7 Hz at 0.6 m
with losses. The instrument will move onto it once it's in a nightly.
`ReedTube.hpp` stays as the reference.
