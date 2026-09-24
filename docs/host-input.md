# Host keyboard and mouse input

The Video panel routes input to cards selected under Project Settings → Input.
Each surface has independent Keyboard, Mouse, and Mouse mode settings. A combined
card may be selected in both fields. None disables that stream. Missing or
incompatible destinations remain visible and are never replaced automatically.
Apply commits routing; Cancel discards the draft. Save persists applied settings.

Click a visible video image to activate it; that first click is not sent to the
card. Keyboard events use physical keys, including modifiers, keypad and function
keys. Captured keys do not activate host shortcuts or ImGui navigation. Text/IME
composition is not converted to physical key events.

Relative mode hides and grabs the cursor and sends movement counts independent of
video zoom. Ctrl+Alt releases input (either Ctrl and either Alt, excluding an
AltGr event marked by SDL as Mode). The final key-down completing this reserved
chord is consumed; modifiers already delivered receive releases. Keys held before
activation are ignored until physically released. OS-reserved shortcuts remain
subject to the platform's keyboard-grab implementation.

Absolute mode leaves the cursor unconstrained. Coordinates are native video
pixels with top-left origin, mapped from the displayed image rectangle, not the
panel bounds. Clipped or covered content is not interactive. Entering sends an
absolute position; leaving ends guest mouse drags and sends LEAVE, so releasing a
button outside the application cannot leave it held. Clicking outside returns
keyboard focus to the host. Ctrl+Alt also releases keyboard focus in this mode.
Shader output is mapped to the original surface dimensions; arbitrary nonlinear
shader distortion is not inverted.

Capture ends on focus loss, modal dialogs, hidden video, pause/stop/reset, project
replacement, or a changed/missing destination. It is never restored automatically.

## Card contract

Include `srz80/input.h`. Register a provider with protocol `srz80.input.v1` through
`host.providers.v1`. Its JSON snapshot contains:

```json
{
  "schema": 1,
  "keyboard": {"endpoint": "controller.keys"},
  "mouse": {"endpoint": "controller.mouse", "relative": true, "absolute": true}
}
```

Either stream may be omitted. Endpoint names must be nonempty, at most 127 bytes,
and registered by the provider's owning card through `host.input.v1`. Keyboard
and mouse must have different endpoint names because their framing differs. Cards
should provide at least 65536 bytes of endpoint capacity, drain due input fully
at each notification, and parse complete packets before returning. Discovery uses
only this capability contract. The host does not know plugin
identifiers, probe card names, recognize device-specific provider schemas, or
translate device-specific protocols. Any card can opt in by advertising these
standard streams and interpreting their events. Existing cards must explicitly
adopt this contract before they appear in routing selectors.

Keyboard packets are two bytes: HID Keyboard/Keypad usage, then flags (bit 0 down,
bit 1 explicit host repeat). Release clears down. Unknown usages must be ignored.
Cards own keyboard layout, guest protocol conversion, and repeat policy; a card
that implements its own typematic behavior may ignore explicit host repeats.

Mouse packets are exactly 20 bytes, little endian, with no native struct padding:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | u8 type | 1 relative, 2 absolute, 3 button, 4 wheel, 5 leave |
| 1 | u8 down | 1 for button press, 0 otherwise |
| 2 | u16 button | 1 left, 2 middle, 3 right, 4 back, 5 forward; 0 otherwise |
| 4 | i32 x | Horizontal counts, pixel coordinate, or wheel units |
| 8 | i32 y | Vertical counts, pixel coordinate, or wheel units |
| 12 | u32 width | Native video width, only for absolute events |
| 16 | u32 height | Native video height, only for absolute events |

Relative positive x/y mean right/down. Absolute x/y are within `[0,width)` and
`[0,height)`. Wheel positive x/y mean right/up, with 120 units per notch; fractional
units accumulate in the host. Unused fields are zero. LEAVE ends pointer presence;
the next absolute packet re-enters. Relative capture cleanup may also send LEAVE.
Card reset clears held keys/buttons and any partial packet state. Input releases
must update held state even when a guest-facing FIFO is full; FIFO overflow must
not leave a key or button stuck.

## Delivery and persistence

Both streams use the simulation worker and deterministic engine queues. Events
within each stream retain their order and receive the simulation time when the
worker accepts the batch. There is no cross-endpoint atomicity guarantee. Adjacent
relative deltas and absolute positions may be coalesced without crossing button/wheel events.

Each stream allows one in-flight request and at most 64 KiB of pending/submitted
bytes. Backpressure retries whole batches. Reaching the host's buffer limit ends capture
and starts releasing input instead of silently dropping transitions. Release cancels
that source's pending input and submits releases for potentially delivered state.
Releases use a separate ephemeral source so a later capture cancellation cannot
remove earlier cleanup. The top source IDs `UINT64_MAX-1`, `UINT64_MAX-2` and those
IDs XOR `1<<62` are reserved for host input. Cleanup is accepted while paused or
stopped and consumed at the next simulation scheduler boundary. Generation and
owner checks prevent an old stream from delivering to a replacement rack/card.

Project `input_routes` entries contain `video_card`, surface registration ordinal,
`keyboard`, `mouse`, and `relative`. Card references are opaque `host_input_id`
strings stored on card records when routing is first applied. A persisted
`input_next_card_id` prevents ID reuse after deletion. Reordering and parking do
not change IDs. Surface ordinal is registration order within its card; plugins
must preserve that order across loads. Missing references are retained. Runtime
handles, capture state, held keys, and pending live events are not routing state.

The keyboard tool remains installed during this stage. Its migration, text-file
typing replacement, and production mouse-card implementations are separate work.
