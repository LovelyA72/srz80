# Plugin project data

Cards may store an opaque data block in the project, similar to a plugin state
chunk in a DAW. The host preserves these bytes but never interprets them. A card
does not need a GUI tool or live-data provider to use project data.

`SrhPlugin` exposes two optional, size-guarded tail callbacks:

- `save_project_data(instance, buffer, size)` reports the required size when
  `buffer` is null, then copies bytes into host-owned storage. The input size is
  capacity and the output size is the number of bytes written.
- `load_project_data(instance, bytes, size)` restores the exact byte sequence
  after `create` and before the initial project reset. The plugin owns validation
  and must reject invalid input without partially changing its state.

A plugin must provide both callbacks or neither. Older function tables remain
valid because the host checks the table size before reading the tail fields. If a
project contains data for a plugin that cannot load it, project loading fails
instead of silently discarding the data.

The project stores a chunk as hexadecimal JSON:

```json
"plugin_data": {"encoding": "hex", "data": "00FF018078"}
```

Hex is only the container encoding. It can represent arbitrary bytes, including
zero bytes, and does not define the plugin's internal schema. Empty chunks are
valid. The host limits decoded chunks to 16 MiB and rejects unknown encodings,
malformed hex, inconsistent callback sizes, unsupported plugins, and callback
errors. Parked cards retain their data.

Saving a chunk runs on the simulation thread through a controller request, even
while the rack is running.

## Project data and execution state

Project chunks contain plugin-defined machine configuration. Execution-state
callbacks contain transient emulation state. The plugin decides what belongs in
each; the host treats both payloads as opaque.

For example, a routing card might store its component topology in project data
and its live switch or shift-register values in execution state. Saving a project
would preserve the layout without capturing the running machine's execution state.

## Live providers

The optional `host.providers.v1` service handles live displays and commands. A
provider publishes a name, protocol ID, and bounded UTF-8 payload. Tools select a
protocol and decode the data themselves; the host does not parse it. Provider
registration is independent of project persistence.
