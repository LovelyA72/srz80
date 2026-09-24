# Z80 assembler

The Z80 assembler is a separately distributed GUI tool. Open it from
**Tools → Z80 assembler** after installing the tool plugin. It does not depend on
a Z80 card and can assemble while the rack is stopped, paused, or running.

## Editing

The default listing places addresses, emitted bytes, and source on each row. The
symbol list stays beside it, with the first and last emitted addresses above.

- Enter or double-click edits the selected row. Escape cancels the edit.
- F2 or Insert adds a row before the selection; **Insert after** adds one after it.
- F3 or Delete removes the selected row when no text field is active.
- Arrow and Page keys move through the listing.
- Clicking a symbol or diagnostic selects its definition or error.
- Ctrl+C/X/V copies, cuts, or pastes source lines outside text fields.
- Ctrl+Z/Y undo and redo instruction and structural changes.
- **Source text** opens a multiline editor with literal, case-sensitive Find and
  Replace.

After 200 ms without input, the tool rebuilds the whole document and refreshes
addresses and symbol references. F5 or Ctrl+Enter builds immediately. Addresses
and bytes stay hidden while the build is stale. Long data rows show a preview;
hover it to inspect all emitted bytes.

Editing does not patch rack memory. F6 performs **Load + Cold Reset**. **Load
(keep PC)** is available for debugger work, and F4 resumes the rack. These
shortcuts apply only while the assembler window is active.

## Loading

Loading requires:

- an explicitly stopped rack, not merely a paused one;
- a selected address space; and
- a successful build of the latest source and origin revision.

The host validates segment headers, ordering, ranges, and overlap before reset or
writes. A requested cold reset occurs once, immediately before the first write.
The host then writes each byte through the semantic bus.

A device callback may fail after earlier writes have taken effect. The tool
reports completed bytes and the failing segment and address; it cannot roll back
device side effects. A failed transaction may have reached multiple mapped
devices even when it is not counted as completed.

Loading never changes PC directly. Cold reset follows each CPU card's configured
reset behavior, not the source `ORG`. Run only resumes the rack. The tool does not
look up a processor, edit registers, or transfer control to an entry point.

## Source dialect

Instruction encoding and expressions follow the plugin's pinned libasm Z80
defaults, with SRZ80-owned labels and directives. This is not an SJAsmPlus, z88dk,
or X16 compatibility layer.

- Mnemonics, directives, and symbols are case-insensitive. Global labels start
  with a letter or underscore and require a colon.
- Constants use `size EQU 16` or `size: EQU 16`. Forward references work;
  duplicate, undefined, and circular symbols are errors.
- Numbers may be decimal, `1234h`, `101b`, `17o`, `17q`, `10d`, `0x1234`, or
  `0b101`. A leading zero selects C-style octal. `$` is the current location.
- Single quotes form character expressions. Double-quoted strings are accepted
  by `DB`, `DEFB`, and `DEFM`; backslashes remain literal.
- `DW` and `DEFW` emit little-endian words. Bytes accept −128 through 255; words
  accept −32768 through 65535.
- `ORG` changes location. Gaps emit nothing, disjoint backward origins are valid,
  and overlap or writes past `FFFF` are errors.
- `DS` and `DEFS` reserve space without emitting bytes. They do not accept a fill
  value.
- Semicolon comments, blank lines, tabs, and indentation are preserved.
- Base, CB, ED, DD, FD, and indexed Z80 forms are supported, including `AF'`.
- Arithmetic uses checked 32-bit integers. Floating-point values are rejected.
- Includes, macros, conditionals, CPU changes, and other upstream pseudo-ops are
  rejected; assembly never reads implicit files.

Each build starts with fresh encoder state and takes at most ten passes to resolve
symbols and segments. Diagnostics use one-based line and byte-column positions.
Failed builds cannot be loaded.

## Project files

The Project files panel sends `.asm`, `.s`, and `.z80` files to the assembler when
its handler is enabled under **Settings → Plugins**. The host workspace owns open
documents, cursors, dirty buffers, and undo/redo history. Save Project and Ctrl+S
write every dirty workspace document; the assembler does not write source files
directly.

The active source path is stored as `tool_state.z80_assembler` in the project.
Relative paths allow a project and its source to move together. If a source file
cannot be read, the tool keeps the current document and reports the error.

Hiding the window retains its document. Exiting the application discards unsaved
source, so save before closing. There is no draft autosave or crash recovery.
Native dialog callbacks retain shared result storage rather than application or
plugin pointers, which makes late callbacks safe during shutdown.
