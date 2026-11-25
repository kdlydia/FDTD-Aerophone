# The model

What the instrument computes. It is MayaFlux's `DuctNetwork` with its
defaults: a reed at the mouth and a radiating open end
([MayaFlux#187](https://github.com/MayaFlux/MayaFlux/issues/187)). The scheme
follows Bilbao, *Numerical Sound Synthesis* (2009).

## One sample

1. Update every u from the pressure differences.
2. Apply the wall losses to u.
3. Solve the reed and the mouth cell p(0).
4. Update the interior p(1) to p(N−1) from the new u.
5. Solve the open end and p(N).
6. Output the radiated pressure from the flow out of the open end.

## Bore

Cross-section S(x) = πr(x)², a cylinder here:

    dp/dt = -(ρc² / S) du/dx
    du/dt = -(S / ρ)   dp/dx  - f u + ν d²u/dx²

Staggered grid: pressure p at the N + 1 points x = lh, volume velocity u at
the N half points between them, with N = floor(L fs / c) and h = L / N:

    u(l) -= k S(l) / (ρh) · (p(l+1) - p(l))
    p(l) -= ρc²k / V(l) · (u(l) - u(l-1))

V(l) is the volume of the two half sections around point l, so the end cells
are half cells. The Courant number λ = ck/h stays just under 1: 0.953 at
0.15 m, 0.9885 at 0.6 m, 0.9945 at 1.2 m. The grid carries up to
fs·asin(λ)/π, 21.7 kHz at 0.6 m.

## Wall losses

The f and ν terms above. They are fitted to the viscothermal attenuation of a
7.5 mm tube at 150 Hz and 3 kHz, which gives f = 33.1 1/s and ν = 0.0385 m²/s.
The real loss grows as √frequency, so this is a two-point fit. They are
applied implicitly right after the u update, as a tridiagonal solve along the
whole bore, so they can't add energy.

## Reed

Quasi-static, so no mass. With Δp = p_mouth − p(0):

    opening = H (1 − Δp / p_M),   zero once Δp ≥ p_M
    U       = w · opening · sign(Δp) · sqrt(2|Δp| / ρ)

The flow rises up to Δp = p_M/3 and falls to zero at p_M. That falling part
is what lets the bore drive the reed into oscillation.

The reed and the mouth cell are solved together in the same sample, centred
in time:

    Δp + g U(Δp) = p_mouth − p(0) + g u(0),   g = ρc²k / (2 V(0))
    p(0) ← 2 (p_mouth − Δp) − p(0)

In s = sign(Δp)·sqrt|Δp| this is a cubic, smooth through zero. It is solved by
Newton steps inside a known bracket, starting from the last sample's root,
usually in 2 or 3 steps. It has one solution while g·wH·sqrt(2/ρ) < sqrt(p_M).
Over the faders' range the left side is at most 14.4 and the right side at
least 31.6, so it always does.

ζ = Z_c w H sqrt(2 / (ρ p_M)) with Z_c = ρc/S, which is 0.264 for the
defaults.

The blowing pressure is smoothed over 5 ms. The lip fader narrows the opening,
H × (1 − 0.2 lip), and stiffens the reed, p_M × (1 + 0.4 lip). The blowing
fader stays a fraction of the p_M fader, not of the stiffened value.

Breath noise is white noise through a one-pole low-pass at 3 kHz, added to the
flow into the bore. Its rms is the noise fader times the reed's flow, so it
goes silent when the reed shuts.

`DuctNetwork` can also give the reed a mass (`resonance` above 0). That
version is a closed form with one square root and no Newton steps. The
instrument doesn't use it.

## Open end

The second-order load of Bilbao and Chick: an inertance giving the 0.6133a end
correction, in parallel with Z_c in series with a resistance and a capacitance
in parallel. The values are Silva et al.'s Padé fit to Levine and Schwinger's
unflanged pipe. Low frequencies reflect and high ones radiate. It uses the
trapezoidal rule and is solved with the end cell in closed form, so it can't
add energy.

## Output

Radiated pressure at 1 m, ρ/(4π) · dU_end/dt, in Pa, times the gain fader.

## Defaults

| | |
|---|---|
| sample rate | 48 kHz |
| length L | 0.6 m, N = 83 (fader 0.15 to 1.2 m) |
| radius a | 7.5 mm |
| c, ρ | 343 m/s, 1.2 kg/m³ |
| rest opening H | 0.4 mm |
| reed width w | 12 mm |
| closing pressure p_M | 3000 Pa (fader 1000 to 8000) |
| blowing | 0.6 p_M (fader 0 to 1.2) |
| breath noise, lip | 0 (faders 0 to 0.3, 0 to 1) |

## Changing length while playing

The new grid is built off the audio thread and swapped in at the start of an
audio block. p and u are interpolated onto it by relative position, and the
reed and the open end keep their state, so the note slides instead of
restarting.

## Results

Measured from the instrument's output, 22 Nov. [walkthrough.mp4](walkthrough.mp4)
shows each of these.

| length | measured | c / (4(L + 0.6133a)) |
|---|---|---|
| 0.3 m | 281.2 Hz | 281.5 Hz |
| 0.6 m | 141.7 Hz | 141.8 Hz |
| 1.2 m | 71.1 Hz | 71.2 Hz |

- At the default settings H1, H3, H5 and H7 are within 1 dB of each other.
  The even harmonics are much weaker, as in a clarinet: H2 is 37 dB down and
  H8 27 dB down. At blowing 1.0, H2 falls to about 50 dB down.
- Below about 0.5 p_M of blowing the note dies. Going from 0.6 to 1.0 adds
  about 2 dB.
- p_M of 6000 Pa is 3.6 dB louder and 1500 Pa is 4.4 dB quieter, at the same
  pitch.
- Lip at 0.7 is 17 dB quieter and silent from about 0.9. Back below about 0.35
  the note restarts.
- Breath noise at 0.3 raises the noise between the harmonics from 54 to about
  26 dB under the tone.

## Open questions

1. Once, after a blowing ramp from 0.35 to 1.1 p_M, the note settled into a
   second state at the default settings: H3 19 dB over H1, 5 dB louder, until
   the note stopped. A slower ramp to 0.85 didn't bring it back. The reed
   solve has one solution there, so it isn't the solver picking another root.
   We don't know yet whether it's physical or comes from the grid.
2. The older model had a 750 Hz tone when blowing above p_M and an f0/2
   component at 0.2 m with p_M = 6000 Pa. Neither has been rechecked on
   `DuctNetwork`.
