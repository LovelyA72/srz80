//! Project JSON validation and loading.
//!
//! Loading is a fallible construction followed by installation: the whole
//! document is validated before the new rack becomes observable. Unavailable
//! image files and libraries produce inert slots; other failures leave the
//! caller's engine untouched. Original project data belongs to the session.

use std::path::{Path, PathBuf};

use crate::core::{Core, Resolver, RunState, TimeMode};
use crate::ffi::*;

/// A JSON number that may also be encoded as a decimal or hexadecimal string.
/// Mirrors the C++ `number` helper, including its rejection of negatives and
/// of anything that is not fully consumed by the parse.
pub fn number(value: &serde_json::Value) -> Result<u64, String> {
    if let Some(unsigned) = value.as_u64() {
        return Ok(unsigned);
    }
    if let Some(signed) = value.as_i64() {
        if signed >= 0 {
            return Ok(signed as u64);
        }
    }
    if let Some(text) = value.as_str() {
        if text.is_empty() || text.starts_with('-') {
            return Err("Invalid unsigned value".to_string());
        }
        if let Some(parsed) = parse_radix(text) {
            return Ok(parsed);
        }
    }
    Err("Expected unsigned integer or hexadecimal string".to_string())
}

/// `std::stoull(text, &end, 0)`: base 8 for a leading zero, base 16 for `0x`,
/// base 10 otherwise, and only when the entire string is consumed.
fn parse_radix(text: &str) -> Option<u64> {
    let (digits, radix) =
        if let Some(rest) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
            (rest, 16)
        } else if text.len() > 1 && text.starts_with('0') {
            (&text[1..], 8)
        } else {
            (text, 10)
        };
    if digits.is_empty() {
        return None;
    }
    u64::from_str_radix(digits, radix).ok()
}

fn hex_bytes(text: &str) -> Result<Vec<u8>, String> {
    if text.len() % 2 != 0 {
        return Err("Hex string must have even length".to_string());
    }
    if text.len() > 4096 * 2 {
        return Err("Input record too large".to_string());
    }
    crate::plugin_data::hex_decode(text).map_err(|_| "Malformed hex byte".to_string())
}

/// Loads a project file from disk.
pub fn load_project(
    project: &Path,
    plugins: &Path,
    initial_state: RunState,
    host_config: &crate::config::Config,
    audio_sample_rate: u32,
) -> Result<Box<Core>, String> {
    let text = std::fs::read_to_string(project)
        .map_err(|_| format!("Cannot open project: {}", project.display()))?;
    let document: serde_json::Value =
        serde_json::from_str(&text).map_err(|error| error.to_string())?;
    let directory = project.parent().filter(|path| !path.as_os_str().is_empty())
        .map(Path::to_path_buf)
        .unwrap_or_else(|| std::env::current_dir().unwrap_or_default());
    build_project(&document, &directory, plugins, initial_state, host_config, audio_sample_rate)
        .map_err(|message| format!("Failed loading project: {message}"))
}

/// Loads a project document that is already in memory.
pub fn load_project_json(
    json: &str,
    project_dir: &Path,
    plugins: &Path,
    initial_state: RunState,
    host_config: &crate::config::Config,
    audio_sample_rate: u32,
) -> Result<Box<Core>, String> {
    let document: serde_json::Value =
        serde_json::from_str(json).map_err(|error| error.to_string())?;
    build_project(&document, project_dir, plugins, initial_state, host_config, audio_sample_rate)
        .map_err(|message| format!("Failed loading project: {message}"))
}

fn build_project(
    document: &serde_json::Value,
    project_dir: &Path,
    plugins: &Path,
    initial_state: RunState,
    host_config: &crate::config::Config,
    audio_sample_rate: u32,
) -> Result<Box<Core>, String> {
    let version = number(
        document
            .get("version")
            .ok_or_else(|| "missing field 'version'".to_string())?,
    )?;
    if version != 1 && version != 2 {
        return Err("Unsupported project version".to_string());
    }
    let mut core = Box::new(Core::new_with_audio_rate(initial_state, audio_sample_rate));
    if !project_dir.as_os_str().is_empty() {
        core.project_root = Some(std::fs::canonicalize(project_dir)
            .map_err(|error| format!("Cannot resolve project directory: {error}"))?);
    }
    *core.config.borrow_mut() = host_config.clone();
    // The host tables must exist before a plugin can be resolved or created.
    core.initialize_host();

    let field = |name: &str| -> Result<&serde_json::Value, String> {
        document
            .get(name)
            .ok_or_else(|| format!("missing field '{name}'"))
    };

    core.rng
        .set(document.get("seed").map(number).transpose()?.unwrap_or(1));
    let mode = document
        .get("time_mode")
        .and_then(|value| value.as_str())
        .unwrap_or("project")
        .to_string();
    let mode = match mode.as_str() {
        "project" => TimeMode::Project,
        "fixed" => TimeMode::Fixed,
        "system" => TimeMode::System,
        _ => return Err("Unknown time mode".to_string()),
    };
    core.mode.set(mode);
    core.epoch.set(
        document
            .get("epoch_ns")
            .map(number)
            .transpose()?
            .unwrap_or(0),
    );

    for space in field("spaces")?
        .as_array()
        .ok_or_else(|| "field 'spaces' must be an array".to_string())?
    {
        let fallback = space.get("unclaimed").map(number).transpose()?.unwrap_or(0);
        if fallback > 255 {
            return Err("Unclaimed byte exceeds 255".to_string());
        }
        let resolver = space
            .get("resolver")
            .and_then(|value| value.as_str())
            .unwrap_or("priority")
            .to_string();
        let resolver = match resolver.as_str() {
            "priority" => Resolver::Priority,
            "or" => Resolver::BitOr,
            _ => return Err("Unknown bus resolver".to_string()),
        };
        let name = space
            .get("name")
            .and_then(|value| value.as_str())
            .ok_or_else(|| "address space needs a name".to_string())?
            .to_string();
        let maximum = number(
            space
                .get("maximum")
                .ok_or_else(|| "address space needs a maximum".to_string())?,
        )?;
        let random = space
            .get("random")
            .and_then(|value| value.as_bool())
            .unwrap_or(false);
        core.space(name, maximum, fallback as u8, random, resolver);
    }

    let clocks = field("clocks")?
        .as_array()
        .ok_or_else(|| "field 'clocks' must be an array".to_string())?;
    if clocks.len() != 3 {
        return Err("Exactly three clocks required".to_string());
    }
    for (index, clock) in clocks.iter().enumerate() {
        let hz = number(clock)?;
        if hz > 50_000_000 {
            return Err("Clock exceeds 50 MHz".to_string());
        }
        core.frequency(index as u32, hz as u32);
    }

    for card in field("cards")?
        .as_array()
        .ok_or_else(|| "field 'cards' must be an array".to_string())?
    {
        let plugin = card
            .get("plugin")
            .and_then(|value| value.as_str())
            .ok_or_else(|| "card needs a plugin".to_string())?
            .to_string();
        if plugin.is_empty()
            || !plugin
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
        {
            return Err(format!("Invalid plugin identifier: {plugin}"));
        }
        let mut name = String::new();
        if let Some(value) = card.get("name") {
            let text = value
                .as_str()
                .ok_or_else(|| "Card name must be a string".to_string())?;
            if text.len() > 255 {
                return Err("Card name exceeds 255 bytes".to_string());
            }
            name = text.to_string();
        }
        let space_name = card
            .get("space")
            .and_then(|value| value.as_str())
            .ok_or_else(|| "card needs a space".to_string())?;
        let space = core.find_space(space_name);
        if space == 0 {
            return Err(format!("Unknown card address space: {space_name}"));
        }
        let clock = card.get("clock").map(number).transpose()?.unwrap_or(0);
        if clock >= 3 {
            return Err("Unknown clock".to_string());
        }
        let priority = card
            .get("priority")
            .and_then(|value| value.as_i64())
            .unwrap_or(0);
        if priority < i32::MIN as i64 || priority > i32::MAX as i64 {
            return Err("Priority out of range".to_string());
        }
        let mut config_json = "{}".to_string();
        if let Some(value) = card.get("config") {
            if !value.is_object() {
                return Err("Card config must be a JSON object".to_string());
            }
            config_json = value.to_string();
        }

        let mut image_paths: Vec<String> = Vec::new();
        if let Some(value) = card.get("image") {
            image_paths.push(
                value
                    .as_str()
                    .ok_or_else(|| "card image must be a string".to_string())?
                    .to_string(),
            );
        }
        if let Some(value) = card.get("images") {
            if !image_paths.is_empty() {
                return Err("Card images must be an array".to_string());
            }
            let entries = value
                .as_array()
                .ok_or_else(|| "Card images must be an array".to_string())?;
            for entry in entries {
                image_paths.push(
                    entry
                        .as_str()
                        .ok_or_else(|| "card image must be a string".to_string())?
                        .to_string(),
                );
            }
        }
        let mut load_error = String::new();
        let mut images: Vec<Vec<u8>> = Vec::with_capacity(image_paths.len());
        for path in image_paths {
            if path.is_empty() {
                images.push(Vec::new());
                continue;
            }
            let resolved = {
                let candidate = PathBuf::from(&path);
                if candidate.is_absolute() {
                    candidate
                } else {
                    project_dir.join(candidate)
                }
            };
            let bytes = std::fs::read(&resolved).unwrap_or_else(|error| {
                load_error = format!("Cannot read ROM: {}: {error}", resolved.display());
                Vec::new()
            });
            images.push(bytes);
        }

        let mut config = SrhConfig {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhConfig>() as u32,
            space,
            base: card.get("base").map(number).transpose()?.unwrap_or(0),
            size: card.get("size").map(number).transpose()?.unwrap_or(0),
            reset_vector: card
                .get("reset_vector")
                .map(number)
                .transpose()?
                .unwrap_or(0),
            priority: priority as i32,
            clock: clock as u32,
            image: std::ptr::null(),
            image_size: 0,
            config_json: config_json.as_ptr() as *const core::ffi::c_char,
            config_json_size: config_json.len() as u64,
            images: std::ptr::null(),
            image_count: 0,
            error_message: std::ptr::null_mut(),
            error_message_capacity: 0,
        };

        let parts: Vec<SrhImagePart> = images
            .iter()
            .map(|bytes| SrhImagePart {
                abi_version: SRH_ABI,
                struct_size: core::mem::size_of::<SrhImagePart>() as u32,
                data: bytes.as_ptr(),
                size: bytes.len() as u64,
            })
            .collect();
        if card.get("image").is_some() {
            if let Some(first) = images.first() {
                config.image = first.as_ptr();
                config.image_size = first.len() as u64;
            }
        }
        if !parts.is_empty() {
            if parts.len() > u32::MAX as usize {
                return Err("Too many card image parts".to_string());
            }
            config.images = parts.as_ptr();
            config.image_count = parts.len() as u32;
        }

        let plugin_path = crate::plugin::resolve_plugin(&core, plugins, &plugin);
        if load_error.is_empty() && plugin_path.as_os_str().is_empty() {
            load_error = format!("Cannot find or load plugin with ID: {plugin}");
        }
        // Validate the transport container even when its owning plugin is absent.
        if let Some(chunk) = card.get("plugin_data") {
            if !chunk.is_object() || chunk.get("encoding").and_then(|v| v.as_str()) != Some("hex") {
                return Err("Invalid plugin data container".into());
            }
            let data = chunk
                .get("data")
                .and_then(|v| v.as_str())
                .ok_or("Invalid plugin data container")?;
            if data.len() > 32 * 1024 * 1024 {
                return Err("Invalid plugin data size".into());
            }
            crate::plugin_data::hex_decode(data)?;
        }
        let unavailable = !load_error.is_empty();
        let id = if unavailable {
            core.unavailable_card(plugin, priority as i32, load_error)
        } else {
            core.load(&plugin_path, &config)?
        };
        // `parts` must stay alive until the card was created.
        drop(parts);
        if let Some(chunk) = card.get("plugin_data").filter(|_| !unavailable) {
            let encoding = chunk.get("encoding").and_then(|value| value.as_str());
            let data = chunk.get("data").and_then(|value| value.as_str());
            match (encoding, data) {
                (Some("hex"), Some(data)) if chunk.is_object() => {
                    core.load_plugin_data(id, data)?;
                }
                _ => return Err("Invalid plugin data container".to_string()),
            }
        }
        if core.set_card_name(id, name) != SRH_OK {
            return Err("Cannot set card name".to_string());
        }
        if card
            .get("removed")
            .and_then(|value| value.as_bool())
            .unwrap_or(false)
        {
            core.park(id);
        }
    }

    if version >= 2 {
        if let Some(inputs) = document.get("inputs") {
            let entries = inputs
                .as_array()
                .ok_or_else(|| "field 'inputs' must be an array".to_string())?;
            let mut records = Vec::with_capacity(entries.len());
            for input in entries {
                let time_ns = number(
                    input
                        .get("time_ns")
                        .ok_or_else(|| "input needs a time".to_string())?,
                )?;
                let endpoint = input
                    .get("endpoint")
                    .and_then(|value| value.as_str())
                    .ok_or_else(|| "input needs an endpoint".to_string())?
                    .to_string();
                let bytes_hex = input
                    .get("bytes_hex")
                    .and_then(|value| value.as_str())
                    .ok_or_else(|| "input needs bytes_hex".to_string())?;
                records.push((time_ns, endpoint, hex_bytes(bytes_hex)?));
            }
            if core
                .all_cards()
                .iter()
                .any(|card| !card.load_error.is_empty())
            {
                // Missing cards cannot register endpoints. The session keeps all
                // original input records; only available endpoints run here.
                let endpoints = core.input_endpoints();
                records.retain(|(_, endpoint, _)| endpoints.contains(endpoint));
            }
            core.set_input_records(records)?;
        }
    }

    core.reset(true);
    Ok(core)
}

/// A fresh default machine: the two documented address spaces and no cards.
pub fn new_project(audio_sample_rate: u32) -> Result<Box<Core>, String> {
    let core = Box::new(Core::new_with_audio_rate(RunState::Stopped, audio_sample_rate));
    core.initialize_host();
    core.space(
        "cpu0.memory".to_string(),
        0xFFFF,
        0,
        false,
        Resolver::Priority,
    );
    core.space("cpu0.io".to_string(), 0xFF, 0xFF, false, Resolver::Priority);
    core.frequency(0, 0);
    core.frequency(1, 0);
    core.frequency(2, 0);
    core.reset(true);
    Ok(core)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn numbers_accept_plain_and_hexadecimal_forms() {
        assert_eq!(number(&json!(255)).unwrap(), 255);
        assert_eq!(number(&json!("0xFFFF")).unwrap(), 0xFFFF);
        assert_eq!(number(&json!("42")).unwrap(), 42);
        assert_eq!(number(&json!("010")).unwrap(), 8);
        assert!(number(&json!("-1")).is_err());
        assert!(number(&json!("12z")).is_err());
        assert!(number(&json!(1.5)).is_err());
        assert!(number(&json!(true)).is_err());
    }
}
