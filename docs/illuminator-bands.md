# What to expect in each band

A blah2 node works with whatever transmitter it is pointed at. The band of that
transmitter decides what the node can see: how far, how finely in range and
speed, which targets, and which settings need changing. This page summarises
the four bands RETINA nodes use.

Most figures follow from the signal and blah2's processing. They are what to
expect, not measurements: FM in particular has not yet been tried on a working
node. Settings are named as they appear in retina-gui; the `config.yml` keys
are in [config/README.md](../config/README.md).

## At a glance

| | FM | VHF (Band III) | UHF | Cellphone |
| --- | --- | --- | --- | --- |
| Frequencies | 88-108 MHz | 174-240 MHz | 470-790 MHz | 700-2100 MHz |
| Typical transmitter | FM radio | DAB radio, some VHF TV | DVB-T television | LTE base stations |
| Signal width | up to ~200 kHz, varies with the programme | 1.536 MHz (DAB) | 7.6 MHz | 5-20 MHz |
| How much blah2 captures | all of it | all of it (DAB) | a 1.536 MHz slice | a 1.536 MHz slice |
| Sample Rate | **250 kHz** | 2 MHz | 2 MHz | 2 MHz |
| RF Notch (77-115 MHz) | **Off** | either | either | either |
| DAB Notch (155-235 MHz) | either | **Off** | either | either |
| One delay bin | 1.2 km | 150 m | 150 m | 150 m |
| Range resolution | ~2-6 km, varies | ~200 m | ~200 m | ~200 m |
| One Doppler bin (2 Hz) | 6 m/s | 2.8 m/s at 213 MHz | 1.2 m/s at 500 MHz | 0.75 m/s at 800 MHz, 0.33 at 1800 |
| Fastest aircraft Doppler | ~170 Hz | ~360 Hz | ~830-1320 Hz | ~1170-3500 Hz |
| Default window reaches | 480 km | 60 km | 60 km | 60 km |
| Best suited to | long-range aircraft | aircraft | aircraft, the shipped design point | short range: drones, vehicles |

Notes on the rows:

- **Range resolution** is about c divided by the bandwidth captured. A delay bin
  finer than that adds no detail.
- **One Doppler bin** is the speed one 2 Hz bin represents, as bistatic range
  rate (Hz × wavelength).
- **Fastest aircraft Doppler** assumes 250 m/s and the worst geometry: at most
  2 × speed / wavelength. The fleet's Doppler span is ±1000 Hz, so anything
  faster than that folds back into the map as a ghost 7-15 dB down.
- **Default window** is `delayMax` 400 bins of bistatic range. Maximum Delay can
  go up to 600 bins in the GUI.

## Before you start: what works today

The current release (v0.8.0) cannot run below 1 MHz: 500 kHz and below
crash-loop blah2 (ClickUp 123zgecjc4k). Until the fix is released:

- **FM nodes must run at 1 or 2 MHz.** This works but captures neighbouring
  stations too, and gives up FM's main advantage (see below).
- **Targets fade with range on every band.** The current release loses
  power with delay, about 2.2 dB at 60 km at 2 MHz, and cuts off completely at
  about 150 km. The fix removes both.

Everything else on this page applies to the current release as well.

## FM (88-108 MHz)

FM transmitters are powerful and VHF travels far, so FM is the long-range
illuminator. An FM channel is narrow, so it needs far less sample rate and CPU
than the other bands.

**Set:**
- **Sample Rate 250 kHz**, once the fix is released. It covers a whole channel
  (±125 kHz), and the SDR's 200 kHz filter matches it. 125 kHz and below clip
  the outer part of a stereo FM signal. Capturing at 2 MHz costs no sensitivity
  but adds noise and the neighbouring stations, which act as extra transmitters
  at other sites.
- **RF Notch off.** It blocks 77-115 MHz, which is the FM band itself.
  Recalibrate gain afterwards: the notch changes the signal level a lot.
- **Max Reported Doppler about 300 Hz.** No aircraft gets near ±1000 Hz at
  100 MHz. False alarms spread evenly over the map, so this removes roughly 70%
  of them.

**Settings in bins change meaning.** At 250 kHz a delay bin is 1.2 km, 8 times
the 2 MHz bin they were tuned for:

| Setting | At 2 MHz | At 250 kHz |
| --- | --- | --- |
| Minimum Delay (5 bins) | 750 m hidden | 6 km hidden |
| Centroid window (6 bins) | ±900 m | ±7.2 km: nearby aircraft can merge |
| CFAR guard / training (2 / 6 bins) | 300 m / 900 m | 2.4 km / 7.2 km |
| Maximum Delay (400 bins) | 60 km | 480 km |

FM resolves only to about 2 km, so 1.2 km bins are not too coarse. Minimum Delay
and the centroid window probably want lowering for FM. That is for whoever tunes
the first FM node to settle.

**Expect:**
- **Resolution follows the programme.** Music gives the most bandwidth. Speech
  and silence collapse it, and range resolution and sidelobes get worse until
  the programme picks up.
- **Possible fixed ghosts** near strong returns, from the 19 kHz stereo pilot
  (around 16 km) and the 57 kHz RDS subcarrier (around 5 km). blah2 does not
  filter these yet.
- **Coarse speed:** 6 m/s per Doppler bin.
- **About half the processing** of a 2 MHz node: 187 ms per CPI against
  358 ms, measured on owl-ded9.

## VHF Band III (174-240 MHz)

Mostly DAB digital radio, plus VHF TV in some countries. A DAB block is
1.536 MHz wide, which is exactly what blah2 captures at 2 MHz, so the node uses
the whole signal.

**Set:**
- **Sample Rate 2 MHz.**
- **DAB Notch off.** It blocks 155-235 MHz, which includes the illuminator.
  Recalibrate gain afterwards.
- Max Reported Doppler about 500 Hz is enough for aircraft.

**Expect:**
- **Steady performance.** DAB is OFDM: its spectrum is flat and noise-like
  whatever is being broadcast, so the ambiguity function does not change with
  the programme the way FM's does.
- **Duplicate targets from single-frequency networks.** DAB transmitters in a
  network share one frequency. Each one the node receives is a separate
  illuminator, so one aircraft can appear several times at different ranges.
- **Range resolution about 200 m**, speed 2.8 m/s per Doppler bin at 213 MHz.

## UHF (470-790 MHz)

DVB-T television. The shipped defaults are designed around this band: the
figures in `config/README.md` assume a 503 MHz transmitter, and the ±1000 Hz
Doppler span is sized for aircraft at UHF.

**Set:**
- **Sample Rate 2 MHz.** Neither notch affects this band.
- Keep the ±1000 Hz span and Max Reported Doppler at its default.

**Expect:**
- **Part of the signal.** A DVB-T channel is 7.6 MHz wide and blah2 captures a
  1.536 MHz slice of it, about a fifth of the power. Range resolution is still
  about 200 m.
- **Duplicate targets from single-frequency networks**, as for DAB. DVB-T
  networks commonly share frequencies.
- **Possible fixed sidelobes** from DVB-T's built-in pilot carriers.
- **Fine speed:** 1.2 m/s per Doppler bin at 500 MHz.
- **The fastest aircraft can fold at the top of the band.** Above about
  600 MHz, an aircraft at 250 m/s in the worst geometry exceeds ±1000 Hz and
  appears as a weak ghost on the opposite side of the map.

## Cellphone (700-2100 MHz)

LTE base stations, tuned to their downlink. The RSPduo tunes up to 2 GHz, so the
2.6 GHz LTE and 3.5 GHz 5G bands are out of reach.

**Set:**
- **Sample Rate 2 MHz.** Neither notch affects this band.
- **Tune to a downlink.** The uplink comes from phones and is no use as an
  illuminator.

**Expect:**
- **Short range only.** A base station transmits tens of watts per sector, far
  less than a broadcast tower. Expect a few km, not tens.
- **Slow targets: drones, vehicles, people.** At 1800 MHz an aircraft can reach
  3000 Hz, far outside ±1000 Hz, so aircraft fold into ghosts. A drone at
  20 m/s stays within about 240 Hz.
- **Very fine speed:** 0.33 m/s per Doppler bin at 1800 MHz.
- **The signal changes with traffic.** An idle cell transmits mostly its
  reference and sync signals, and power rises and falls with load. Its strong
  periodic structure gives fixed sidelobes.
- **Many transmitters on one frequency.** Neighbouring cells normally reuse the
  same frequency, so expect duplicate targets, as with single-frequency
  broadcast networks.
- **Part of the signal.** An LTE carrier is 5-20 MHz wide and blah2 captures a
  1.536 MHz slice.

## Settings that are fleet policy

The Doppler span is set for the whole fleet at ±1000 Hz. A `user.yml` override
is dropped. Under the fix this no longer limits range on any band; a wider span
than the band needs only costs some CPU. To ignore fast-moving false alarms on a
low band, use Max Reported Doppler instead.
