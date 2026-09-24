# Rust engine hot-path measurements

These measurements were recorded on September 10, 2026 with optimized x86_64
Windows GNU builds and identical release card libraries. The results apply to
the hot-path changes below, on the tested workloads and host.

## Problem and fix

The Rust port allocated a callback vector for every subscribed clock tick, cloned
the route vector for cached bus transactions, and cloned a space name for every
read. Hot paths also repeated hash lookups for integer card handles.

The updated engine:

- iterates retained event records inside a deferred-collection frame;
- shares immutable cached route slices;
- copies only scalar bus policy;
- uses an ordered set for live handles;
- keeps collection borrows out of card callbacks; and
- uses bounded stack storage for audio-mix scratch.

Dispatch still checks visibility, cancellation, and owner liveness immediately
before each subscriber call, including after an earlier callback changes the
rack.

## Results

The benchmark ran a 4 MHz Z80 over zero-filled RAM for 250 ms of simulated time,
with an optional idle SID audio source. It discarded one warm-up run and reported
the median of three. Bounded runs used repeated 2 ms controller budgets without
GUI rendering or pacing sleeps.

| Workload | Baseline Rust | Fixed Rust | C++ reference |
| --- | ---: | ---: | ---: |
| Z80, unbounded | 133.62 ms | 46.02 ms | 31.97 ms |
| Z80, 2 ms slices | 172.64 ms | 70.82 ms | 63.38 ms |
| Z80 + idle SID, unbounded | 147.55 ms | 53.78 ms | 39.27 ms |
| Z80 + idle SID, 2 ms slices | 159.92 ms | 76.24 ms | 65.02 ms |

The fixed engine was 2.1–2.4 times faster in bounded runs and 2.7–2.9 times
faster in unbounded runs. It remained slower than the C++ reference for this
workload. Wall time varies with machine load, and these numbers do not predict
performance for every card or rack.

The production change left simulated timing, buffering rules, card behavior, and
UI ownership unchanged. It gave audio delivery more headroom before the PCM
queue runs dry.

Validation at the time included 79 Rust tests, 10 applicable native tests, and a
142-observation comparison with the C++ reference. Audio coverage exercised
rendering, mixing, source errors, and queue limits. Physical playback still
required a listening check.

## Playback delivery

The same investigation found two host-side problems: SDL received samples only
once per GUI frame, and choosing an audio driver did not initialize that driver.

SDL now pulls from the controller's locked PCM queue through a stream callback.
The callback primes two device periods within queue capacity, handles reset
epochs, and is drained before the controller is released. It cannot access the
engine, cards, tools, or ImGui. Device creation and lifetime remain GUI-owned.
Startup selects the saved driver before enumerating devices, and changing drivers
closes the old stream before reinitializing SDL audio.

The simulation worker retains unfinished time across 2 ms execution slices so a
late OS wake can recover. The retained time is capped at 20 ms and cleared on pause
or rack replacement.

Controller-level tests covered reset, close, invalid-driver recovery, and delayed
slices with SDL's dummy driver. Native checks passed with WASAPI at a requested
256-frame buffer (441 actual frames) and DirectSound at 1024 frames. DirectSound
at 256 frames accumulated PCM on the test machine and remains a known limit.

These checks cover initialization and delivery. They do not establish memory
safety for the engine or native plugins, or rule out other causes of audible
crackling.
