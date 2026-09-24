# Third-party software

SRZ80 uses pinned dependencies. CMake verifies downloaded archives against the
SHA-256 hashes in [cmake/Dependencies.cmake](../cmake/Dependencies.cmake), and
Cargo records Rust crates and checksums in
[engine/Cargo.lock](../engine/Cargo.lock).

## CMake dependencies

| Dependency | Version | License | Used by |
| --- | --- | --- | --- |
| [SDL](https://github.com/libsdl-org/SDL/tree/release-3.4.0) | 3.4.0 | zlib | desktop app |
| [Dear ImGui](https://github.com/ocornut/imgui/tree/v1.92.9b-docking) | 1.92.9b docking | MIT | desktop app |
| [ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit/tree/v1.92.9) | 1.92.9 | MIT | source editor |
| [nlohmann/json](https://github.com/nlohmann/json/tree/v3.12.0) | 3.12.0 | MIT | project and state data |

CMake downloads these archives during configuration. For an offline build,
extract matching releases and set:

- `FETCHCONTENT_SOURCE_DIR_SDL3`
- `FETCHCONTENT_SOURCE_DIR_IMGUI`
- `FETCHCONTENT_SOURCE_DIR_IMGUI_COLOR_TEXT_EDIT`
- `FETCHCONTENT_SOURCE_DIR_JSON`

SDL and Dear ImGui are skipped when `SRZ80_BUILD_GUI=OFF`. SDL is linked
statically into the desktop app.

## Rust dependencies

The engine uses `libloading` 0.8.6 to open native plugins and `serde_json`
1.0.145 to read and write JSON. Their transitive dependencies are fixed in
`engine/Cargo.lock`.

## Distribution

Keep each dependency's original notice with source and binary distributions.
Windows builds also stage the GCC, libstdc++, and winpthreads runtime DLLs needed
by the executables; the MSYS2/GCC distribution terms apply to those files.

Card and tool plugins are not part of this repository. Their dependencies and
notices must travel with their own source and binary distributions.
