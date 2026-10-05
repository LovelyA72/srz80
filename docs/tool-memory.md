# Tool memory reads

`SrhToolHostV1` has optional `memory_read_request`, `memory_read_poll` and
`memory_read_release` services. Guard the tail fields with `has_field` and
check the function pointers. The older single-byte `peek` service still works.

Submit nonempty `SrhToolMemoryRange` entries sorted by space and address, with
no overlap within a space. A batch holds at most 64 ranges and 4096 bytes.
Use the current nonzero project generation and a stable tool-owned client
identity. The host copies the ranges before returning.

The GUI request ledger posts one `ReadMemoryBatch` command to the simulation
controller. The worker validates every space and range before reading anything,
then uses the engine's bus peek operation. Mapping resolution and unavailable
bytes follow the same rules as individual peeks. No guest-memory pointers cross
the tool boundary.

Poll without waiting. A null byte array, null status array and capacity zero
query completion and required size. Completed results stay available until
release. Allocate both arrays for `result.size` elements. Bytes concatenate the
submitted ranges in order. A successful batch can still contain failed byte
statuses. `generation` identifies the rack and `time_ns` identifies the worker
capture time. A queued request from an obsolete generation completes with
`SRH_CONFLICT`. Consumers must also discard replies if their selected view or
current generation changed after submission.

At most two requests per client and sixteen globally can remain outstanding.
Completed requests occupy a slot until released. Released requests still running
occupy a slot until completion. Release doesn't cancel a worker command. The
worker retains no plugin pointers or output buffers, so plugin destruction
can't invalidate a queued read. Release outstanding handles when a tool closes
or is destroyed. The GUI collects released replies even while tools are hidden.

One batch executes while simulation dispatch is quiescent. Several batches do
not form an atomic snapshot. Keep the old display during collection and validate
any layout assumptions before replacing it. Memory revision is not a general
CPU-write dirty counter.
