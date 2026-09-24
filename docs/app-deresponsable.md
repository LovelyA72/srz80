# Application and project ownership

`App` is the desktop shell. It arranges views, shows dialogs, owns GUI
resources, and coordinates startup and shutdown. Project state and lifecycle
decisions belong to `ProjectSession`, which can be tested without SDL or ImGui.

## Ownership

| Owner | Responsibility |
| --- | --- |
| `App` | Window and renderer, frame composition, dialog presentation, audio reactions, tool hosting, and GUI resource lifetime |
| `ProjectSession` | Project document, canonical path, revisions, card associations, lifecycle operations, and operation results |
| `ProjectWorkspace` | Open source documents, saved baselines, cursors, and undo/redo history |
| Project persistence | Serialization, staging, file reads, and file writes using owned data |
| `SimulationController` | Engine lifetime, worker transport, runtime commands, generation validation, and authoritative results |
| GUI adapters | Native dialogs, tool callbacks, selection invalidation, recent projects, and clipboard work |

The session uses the `ProjectRuntime` interface implemented by
`SimulationController`; it never calls the engine directly. Session and
persistence code have no SDL or ImGui dependency.

## Lifecycle operations

New, Open, Save, Save As, state load/save, and Quit use the same operation
interface. Only one lifecycle operation may be active at a time. Each operation
has an identity, and runtime work also carries the expected project generation.
Late or mismatched results are ignored rather than treated as rollback.

Before the first successful New or Open, the session is unloaded. It has a
runtime generation but no editable project. Save, state, and machine mutations
are rejected, while New and Open proceed without an unsaved-work prompt.

New and Open prepare a candidate document without touching the active one. The
session installs it only after runtime replacement succeeds and returns the new
generation and card associations. Failure preserves the current project, path,
workspace, and unsaved edits. Both actions use the same installation path, which
clears stale selections, inspections, dialogs, and editor references.

Native file-dialog callbacks retain a shared mailbox plus operation and project
epochs. They never retain `App` or `ProjectSession` pointers. Shutdown stops new
work and drains session-owned file and runtime jobs before tools and the
controller are destroyed.

## Runtime and document consistency

Project revision and runtime generation are separate:

- generation identifies a rack lifetime;
- revision identifies edits within that lifetime.

Card insertion, parking, reactivation, removal, reordering, clock changes, and
configuration changes are submitted through the session. The document changes
only after the matching runtime command succeeds. Controls may display a pending
draft, but rejection leaves committed JSON unchanged.

Reordering uses a validated permutation of existing handles. It preserves the
rack generation, card instances, memory, parked slots, bus routing, clocks, and
run state. Configuration retains its rebuild and reset behavior.

Unavailable cards still have engine-owned inert handles and authoritative rack
positions. Their original JSON, `removed` field, and opaque plugin data are
preserved. Missing-resource errors belong to runtime snapshots and are not saved
into the project. Reopening retries the saved resource path. Execution-state
capture and restore reject racks containing unavailable cards.

## Saving projects

A project owns the directory containing its manifest and subdirectories. New
projects and Save As write `project.json` at the selected root. Save As copies
the current project directory into staging before moving it into place. Existing
targets and targets inside the current project are rejected.

Saving has three distinct phases:

1. Collect machine JSON, workspace files, plugin data, and tool contributions.
2. Write owned snapshots on a filesystem worker.
3. Commit paths, saved revisions, and workspace relocation after success.

Opaque card data is collected on the simulation worker. Tool callbacks remain on
the GUI thread. Writes use a reserved sibling staging directory and replace the
project manifest last. A failed save retains the active path and dirty baselines,
and cleanup touches only staging artifacts created by that operation.

Saving revision N never marks revision N+1 clean. Edits made after collection
remain dirty. Multi-file writes and native tool callbacks do not form a single
transaction, so partial source writes are reported instead of presented as a
rollback. Tool relocation callbacks run after the disk commit; callback failure
is a GUI notification error, not a failed save.

The manifest's top-level `name` is its display name. Blank names fall back to the
root folder for `project.json`, or the manifest stem for legacy files. On the first
Save, an unnamed project takes its name from the chosen folder; Save As preserves
an existing name.

## External file changes

Open source files are checked asynchronously with bounded reads. Results apply
only when the generation, document ID, path, and disk baseline still match.

- A clean file reloads and drops its stale per-file history.
- A file with local edits keeps those edits and gains an external-change marker.
- Deleted, unreadable, binary, or oversized files never replace the editor buffer.

Save checks captured disk contents before the batch and again before each file
replacement. A conflict pauses the operation for Overwrite, Reload, or Cancel.
Overwrite updates the expected disk version and checks again on retry. Reload or
Cancel also stops a deferred New, Open, or Quit.

These checks cannot provide atomic compare-and-replace against arbitrary external
applications or undo side effects from native tool callbacks.

## Execution state

Execution state is separate from the editable project. State files retain the
project/execution/project-path wrapper and have their own path.

Restore loads the project and execution data into a fresh candidate engine. A
parse or plugin-load failure leaves the active engine and generation intact.
Success installs the returned card associations, pauses the restored machine,
invalidates runtime views, and preserves open source documents. External side
effects performed by native plugins are outside this transaction.

## Thread and lifetime rules

- Session state changes on the GUI thread.
- File workers receive and return owned values.
- Engine and card callbacks run on the simulation worker.
- Tool callbacks, SDL audio, and ImGui run on the GUI thread.
- Dropping a future is not cancellation.
- Tools are destroyed before ImGui teardown.

Tests use controllable runtime completions and file results for lifecycle cases,
plus real-controller coverage for replacement associations, repeated card types,
parked cards, live reordering, state restore, clock/configuration commits, and
shutdown. UI interaction remains a manual check.
