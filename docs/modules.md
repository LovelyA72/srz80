# Module boundaries

SRZ80 separates the engine, application shell, and plugin contracts into the
following modules in the public host repository.

| Module | Responsibility | Main boundary |
| --- | --- | --- |
| `engine/srz80engine` | Rust engine: rack, cards, bus, clocks, events, signals, debugger, inputs, audio, video, configuration, project loading, and execution state | [`engine.h`](../sdk/include/srz80/engine.h) |
| `sdk/` | Stable C contracts for the engine, cards, tools, signals, providers, and input | [`abi.h`](../sdk/include/srz80/abi.h), [`tool.h`](../sdk/include/srz80/tool.h) |
| `srz80_simulation` | Worker and engine ownership, commands, replies, generation checks, snapshots, pacing, and PCM transport | `SimulationController`, `UiSnapshot`, `PcmQueue` |
| `srz80_project` | Project lifecycle, persistence, workspace files, external-change checks, and text encoding | `ProjectSession`, `ProjectWorkspace` |
| `ui/` | SDL3 / Dear ImGui application, panels, settings, tools, file dialogs, editor, audio output, and shader presentation | [`main.cpp`](../ui/main.cpp), [`gui.cpp`](../ui/gui.cpp) |
| `app/` | Headless project runner, scripts, assertions, and captures | [`headless.cpp`](../app/headless.cpp) |

## Dependency direction

The Rust engine exposes a C ABI and has no SDL or ImGui dependency. It loads card
libraries through the SDK and never links a card implementation. The desktop and
headless applications both use the engine ABI.

`srz80_simulation` owns the worker thread and converts engine state into immutable
UI snapshots. It does not depend on SDL or ImGui. `srz80_project` owns the editable
document and filesystem operations; runtime changes go through the
`ProjectRuntime` interface.

The UI owns platform and presentation concerns. GUI tools use the SDK tool ABI
and the host's exact Dear ImGui context. Card plugins remain UI-independent.

The engine's master audio chain processes unclipped stereo frames in the order
gain → compressor → soft clip. Each effect implements `SignalEffect` and owns
its processing state; `EffectChain` runs and resets the ordered effects. DC
correction precedes the chain, and master volume and 16-bit quantization follow
it. Playback, master meters, and headless captures use the resulting PCM.

Project Settings → Audio edits these effects. Project JSON stores gain in
`audio.gain_db`, the clip toggle in `audio.software_clipping`, and compressor
values in `audio.compressor`. Missing values default to unity gain, a disabled
compressor, and enabled soft clip. Compressor keys and ranges are documented
in [`engine.h`](../sdk/include/srz80/engine.h).

Card and tool implementations are distributed separately and loaded from
`plugins/` and `tools/` beside the executables. They are not part of this source
tree.

See [Application and project ownership](app-deresponsable.md) for the lifecycle
boundary and [Runtime structure review](threading-review.md) for thread ownership
and remaining constraints.
