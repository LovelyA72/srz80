# Simulation CLI

The `srz80` executable runs projects without a window or audio device.

```sh
srz80 project.json --run 2s --json result.json
srz80 project.json --run 500ms --screenshots captures --wav sound.wav --json result.json
srz80 project.json --script test.json --wav sound.wav --json -
```

Without `--run` or `--script`, the command inspects the project while paused.
`--plugins DIR` selects the card library directory; the default is `plugins/`
beside the executable. A project that requires an unavailable card fails to load.
The CLI does not capture or export bus traces.

`--json -` writes the report to stdout. Diagnostics go to stderr.

| Exit code | Meaning |
| ---: | --- |
| 0 | Success |
| 1 | Load, execution, assertion, or capture failure |
| 2 | Invalid arguments |

Reports and partial WAV files are finalized after ordinary failures.

Durations are positive integers followed by `ns`, `us`, `ms`, or `s`. A
command-line `--run` executes before script steps. Each run resumes the machine,
advances by the requested simulated duration, then pauses. An unexpected card or
debugger stop fails the run.

`--timeout 60s` sets the default wall-time budget from setup through execution.
The runner checks it between slices; it cannot preempt a native plugin callback.
Use an outer process timeout when a hard limit is required.

For repeatable results, use project or fixed time and keep the project seed,
plugins, and configuration unchanged. Add assertions to check firmware behavior;
running for the requested duration alone does not show that the firmware behaved
as expected.

## Scripts

```json
{
  "version": 1,
  "steps": [
    {"op": "run", "duration": "100ms"},
    {"op": "expect_text", "card": "uart0", "contains": "Ready"},
    {"op": "input", "endpoint": "uart0.rx", "text": "help\r", "after": "1ms"},
    {"op": "run", "duration": "100ms"},
    {"op": "write", "space": "cpu0.memory", "address": "0x8000", "bytes": [42]},
    {"op": "expect_memory", "space": "cpu0.memory", "address": "0x8000", "bytes": [42]},
    {"op": "screenshot", "card": "video0", "directory": "captures/after-help"}
  ]
}
```

Steps run in order and stop at the first failure. Names must match the project.
Run a load-only command with `--json -` to discover cards, spaces, properties,
inputs, video surfaces, audio sources, and clocks.

| Operation | Required fields | Behavior |
| --- | --- | --- |
| `run` | `duration` | Advance relative simulated time |
| `input` | `endpoint`, `text` or `bytes` | Queue UTF-8 text or bytes; `after` optionally delays delivery |
| `write` | `space`, `address`, `bytes` | Write consecutive bytes through the bus |
| `expect_memory` | `space`, `address`, `bytes` | Compare bytes using side-effect-free peeks |
| `expect_text` | `card`, `contains` | Require a substring in retained console text |
| `expect_property` | `card`, `property`, `equals` | Compare an exported property; `field` may be `unsigned`, `signed`, or `text` |
| `screenshot` | `directory` | Capture every surface, or restrict capture with `card` |

Numbers may be JSON unsigned integers or decimal/`0x` strings. Byte values are
0–255, with at most 1 MiB in one write or assertion. Transcript capture is
limited to 64 MiB. Inputs and writes occur while paused; the next run advances
the machine. Existing project input records remain scheduled.

## Captures

`--screenshots DIR` writes final surfaces as `surface-ID.bmp`. Script capture
directories are relative to the script; command-line directories are relative to
the current working directory. Reusing a path overwrites the file. Missing surfaces
or incomplete reads fail the command.

BMP output is the unscaled native surface without GUI effects or shaders. Files
use lossless 32-bit BGRX storage, discard alpha, and are limited to 256 MiB per
surface. The report records dimensions, surface and owner IDs, and simulated time.

`--wav FILE` records every run step into one stereo, 16-bit little-endian PCM WAV.
`--sample-rate HZ` accepts 8000–384000 Hz and defaults to 44100 Hz. Audio follows
simulated time, including silence; pauses and inspection add no frames. Mixer mute
and volume settings apply. Source errors or dropped capture frames fail the run.
Standard RIFF's approximate 4 GiB limit applies.

## JSON report

Every report contains `schema_version`, `ok`, and, on failure, `error`. Inspection
adds engine version, simulated and advanced time, stop reason, cards and
properties, spaces, retained console text, and engine logs. Discovery data is
reported under `inputs`, `video`, `audio_sources`, and `clocks`.

`steps` contains indexed pass/fail results. `artifacts` describes captures, while
`audio` records the rate, frame count, dropped frames, and source errors. Fields
may be absent after a load or inspection failure; `inspection_error` records a
secondary failure. Invalid UTF-8 is replaced during JSON serialization. Log text
is diagnostic and does not fail a run by itself.
