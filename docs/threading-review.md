# Runtime structure and thread ownership

The engine owns the emulated machine and has no SDL or ImGui dependency. Card
instances run on the simulation worker. The headless runner calls the engine ABI
directly; the desktop app uses `srz80_simulation` to exchange commands and
immutable snapshots with the worker.

This document covers the runtime/UI boundary, not the internals of every card or
third-party dependency.

## Controller structure

| Source | Responsibility |
| --- | --- |
| `ui/simulation_controller.cpp` | Worker lifetime, pacing, transport, and error ownership |
| `ui/simulation_requests.cpp` | Public request construction and synchronous ABI adapters |
| `ui/simulation_commands.cpp` | Engine operations, generation checks, and snapshot invalidation |
| `ui/simulation_inspection_commands.cpp` | Memory, property, disassembly, and text operations |
| `ui/simulation_snapshot.cpp`, `ui/ui_snapshot.hpp` | Immutable publications and panel-interest cadence |
| `ui/pcm_queue.*` | Bounded stereo transport and reset epochs |
| `ui/inspection.cpp` | UI polling, cache acceptance, and clipboard completion |
| `ui/trace_view.hpp` | Cached row mapping for the bus monitor |
| `ui/audio_backend.*` | SDL device, stream, and PCM consumption |

The controller is one library shared by the GUI and controller tests. Runtime
creation and destruction occur on the worker thread.

## Data flow

UI requests carry the project generation visible when the user acted. The worker
rejects stale mutations and inspections. Successful mutations invalidate the
affected snapshots; the UI receives new immutable snapshots at the usual update
intervals.

Recurring inspections and clipboard reads are asynchronous. Control snapshots
share retained payloads instead of copying unchanged containers. The bus monitor
holds its immutable trace source and caches filtered row indices. Video textures
upload only when the publication sequence changes.

Commands use owned, named payloads for memory access, batch loads, properties,
disassembly, and text. The payload type determines the operation, which prevents
an opcode and data shape from disagreeing. Generation validation and promise
completion remain at the common worker boundary.

Some lifecycle, clock, mixer, configuration, and provider operations still use a
legacy command payload. They should move to typed payloads one domain at a time,
with the existing transport and tests kept intact.

## Audio

The worker writes stereo frames into a preallocated ring buffer. SDL pulls from
that queue through a stream callback, which handles reset epochs and cannot touch
the engine, plugins, tools, or ImGui. Device lifetime remains on the GUI thread.

Pacing measures cost per simulated time advanced. A bounded execution request
checks its wall deadline between plugin callbacks and retains limited unfinished
time so the next slice can recover from a late wake.

## Project lifecycle

`ProjectSession` owns the project document, revisions, card associations, and
lifecycle decisions. It retains asynchronous results instead of treating a
timed wait as cancellation. Card, clock, and property edits reach the document
only after the matching runtime command succeeds.

File workers receive owned snapshots. Tool state collection and relocation
callbacks stay on the GUI thread. Stable document IDs and weak handler references
prevent the workspace from retaining GUI pointers. Shutdown drains file and
runtime work before releasing tools and the controller.

See [Application and project ownership](app-deresponsable.md) for the complete
lifecycle and persistence contract.

## Remaining constraints

### Synchronous tool services

Some C ABI callbacks must return immediately to the calling tool. Batching avoids
per-byte round trips, but configuration, mutations, large loads, and provider
edits can still wait for the worker. A future versioned asynchronous service would
need explicit request IDs and results. A timeout cannot be treated as cancellation
or rollback of a native callback.

### Metadata discovery

The add-card catalogue opens libraries and calls `srz80_plugin_init` with a probe
host on the UI thread. Runtime instances are worker-owned, but not every call into
a card library occurs there. A metadata service with explicit ownership could make
these lifetime rules consistent and remove duplicated platform loader code.

### Cooperative deadlines

The scheduler checks its deadline between callbacks. A slow native callback,
state serializer, or large inspection can still occupy the worker. Large reads
could become chunked jobs with explicit consistency rules, but native plugin
callbacks cannot safely be interrupted.

### Native plugin trust

The engine ABI accepts caller-owned pointers and callbacks from trusted native
code. Thread separation limits ownership mistakes in the host; it is not a
sandbox or a memory-safety guarantee for plugins.
