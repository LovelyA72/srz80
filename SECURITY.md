# Security policy

## Plugins are trusted code

SRZ80 loads card plugins and GUI tools as native in-process shared libraries
through `dlopen`/`LoadLibrary`. There is no sandbox, no capability boundary, and
no process isolation. A plugin can execute arbitrary code in the host process,
read and write host memory, and access the filesystem with the user's
permissions.

SRZ80 deliberately gives plugins full trust. Installing a plugin is equivalent
to installing a program. Treat a card or tool library like an executable from
the same source.

In practice:

- Only load plugins you trust, preferably ones whose source you can inspect.
  `srz80_engine_discover_plugins` and the GUI's tool discovery enumerate
  libraries from the executable-relative `plugins/` and `tools/` directories.
  The engine loads whatever it finds there.
- A project file names plugins and ROM images. An untrusted project can choose
  which plugin libraries to load, so opening one carries the same risk as
  running an untrusted program.
- The version, size, and ABI validation at the engine and card boundaries
  (`sdk/include/srz80/engine.h`, `sdk/include/srz80/abi.h`) protects against a
  plugin built against an incompatible ABI producing unreadable structures. It
  does not provide a security boundary or contain a hostile plugin.
- Card callbacks run synchronously on the engine's owning thread. A plugin that
  panics, aborts, hangs, or corrupts memory can take down the host process. The
  engine contains Rust panics and C++ exceptions so they cannot unwind across the
  ABI, but it does not attempt to recover from a plugin that misbehaves.

## Supported versions

SRZ80 is pre-1.0. Security fixes land on the default branch; there are no
maintained release branches yet.

## Reporting a vulnerability

Report security-relevant bugs privately to the LovelyA72. Do not use GitHub issues.

When you report, include:

- the affected commit or version,
- the platform and build configuration,
- a description of the impact, and
- a minimal reproduction if you have one.

I will acknowledge the report, confirm the impact, and coordinate the fix and
disclosure timeline with the reporter.

## Out of scope

- A malicious or buggy plugin, a malicious project file, or an untrusted ROM
  image causing harm. See the threat model above; these are expected to have full
  process access.
- Denial of service that requires already running untrusted native code in the
  process.
- Vulnerabilities in vendored third-party code. Report those upstream, and here
  if the pinned version in [third_party/README.md](third_party/README.md) is
  affected.
