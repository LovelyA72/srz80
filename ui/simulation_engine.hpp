#pragma once
#include <srz80/engine.h>
#include <filesystem>
#include <string>

namespace srz80::ui {
// Shared engine-boundary helpers used by the simulation translation units.
// All of them run on the simulation thread and copy out of the engine, so no
// borrowed engine pointer or slice survives a call.

// Borrows `text` for the duration of one engine call.
SrzSlice engine_slice(const std::string &text);
// UTF-8 text for a filesystem path, matching the engine's path encoding.
std::string engine_path_text(const std::filesystem::path &path);
// Owns a NUL-terminated string the engine handed over.
std::string engine_text(const char *value);
// Message produced by the most recent failing call on this engine.
std::string engine_last_message(const SrzEngine *engine);
// Reads one host configuration value through the engine's two-phase text query.
std::string engine_config_value(const SrzEngine *engine, const std::string &key,
                                const std::string &fallback);
// Reads one plugin setting value.  Returns the engine status.
SrhStatus engine_config_entry(const SrzEngine *engine, const std::string &key, std::string &value);
} // namespace srz80::ui
