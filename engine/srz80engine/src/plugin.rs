//! Card plugin libraries: loading, symbol resolution and descriptor probing.
//!
//! A library stays loaded for at least as long as any card instance created
//! from it, so a card's `destroy` callback always runs before its module is
//! unloaded.

use core::ffi::{c_char, c_void};
use std::path::{Path, PathBuf};
use std::rc::Rc;

use crate::core::{cstr, path_text};
use crate::ffi::*;

/// A loaded card library plus its `srz80_plugin_init` entry point.
pub struct Library {
    _module: libloading::Library,
    pub init: SrhPluginInit,
    pub path: PathBuf,
}

impl Library {
    pub fn open(path: &Path) -> Result<Rc<Library>, String> {
        // Safety: the caller offers a path that is expected to be a card
        // plugin.  Loading an arbitrary library runs its initializers.
        let module = unsafe { libloading::Library::new(path) }
            .map_err(|error| format!("Cannot load plugin {}: {error}", path_text(path)))?;
        let init: SrhPluginInit = unsafe {
            // Safety: the symbol is copied out as a plain function pointer. The
            // module stays loaded for as long as `init` is reachable.
            match module.get::<SrhPluginInit>(b"srz80_plugin_init\0") {
                Ok(symbol) => *symbol,
                Err(_) => {
                    return Err(format!(
                        "Plugin has no srz80_plugin_init: {}",
                        path_text(path)
                    ))
                }
            }
        };
        Ok(Rc::new(Library {
            _module: module,
            init,
            path: path.to_path_buf(),
        }))
    }
}

pub fn plugin_id(api: *const SrhPlugin) -> String {
    // Safety: the caller validated `api` first.
    unsafe {
        if api.is_null() || (*api).id.is_null() {
            return String::new();
        }
        cstr((*api).id)
    }
}

/// The required ABI functions.  A plugin missing any of them is rejected rather
/// than loaded with a partial table.
pub fn plugin_complete(api: *const SrhPlugin) -> bool {
    // Safety: the caller validated `api` first.
    unsafe {
        !api.is_null()
            && !(*api).id.is_null()
            && (*api).create.is_some()
            && (*api).destroy.is_some()
            && (*api).reset.is_some()
            && (*api).property_count.is_some()
            && (*api).property_info.is_some()
            && (*api).property_get.is_some()
            && (*api).property_set.is_some()
    }
}

/// Builds the versioned host table handed to `srz80_plugin_init` and to every
/// card `create` call.  The context is the owning [`crate::core::Core`], so a
/// card callback re-enters the engine through this thread only.
pub fn host_tables(core: *mut c_void) -> crate::host::HostTables {
    crate::host::HostTables::new(core)
}

/// Probes a library for its plugin id without creating a card.  Returns the
/// API pointer only while `library` is alive.
pub fn probe(library: &Library, host: *const ShouryoHost) -> *const SrhPlugin {
    let api = unsafe {
        // Safety: `init` came from this module and the host table is a live,
        // fully initialized structure.
        (library.init)(host)
    };
    if valid(api, core::mem::size_of::<SrhPlugin>()) {
        api
    } else {
        std::ptr::null()
    }
}

/// Resolves a plugin id inside `directory` by loading each regular file in
/// sorted order.  Candidates that cannot be loaded, or that do not safely
/// provide plugin metadata, are skipped.
pub fn resolve_plugin(core: &crate::core::Core, directory: &Path, id: &str) -> PathBuf {
    if !directory.exists() {
        return PathBuf::new();
    }
    let mut candidates: Vec<PathBuf> = std::fs::read_dir(directory)
        .map(|entries| {
            entries
                .flatten()
                .filter(|entry| entry.path().is_file())
                .map(|entry| entry.path())
                .collect()
        })
        .unwrap_or_default();
    candidates.sort();

    let host = core.host();
    for path in candidates {
        let Ok(library) = Library::open(&path) else {
            continue;
        };
        let api = probe(&library, host);
        let found = plugin_id(api);
        if !found.is_empty() && found == id {
            return path;
        }
    }
    PathBuf::new()
}

/// Sorted list of plugin libraries in `directory`.
pub fn discover(directory: &Path) -> Vec<PathBuf> {
    if !directory.exists() {
        return Vec::new();
    }
    let mut result: Vec<PathBuf> = std::fs::read_dir(directory)
        .map(|entries| {
            entries
                .flatten()
                .filter(|entry| {
                    entry.path().is_file()
                        && entry.path().extension().and_then(|value| value.to_str())
                            == Some(&crate::paths::plugin_extension()[1..])
                })
                .map(|entry| entry.path())
                .collect()
        })
        .unwrap_or_default();
    result.sort();
    result
}

/// Copies a NUL-terminated label from a card descriptor, replacing an empty or
/// missing label with the default slot name.
///
/// # Safety
/// `label` must be null or NUL-terminated.
pub unsafe fn image_slot_label(label: *const c_char, index: u32) -> String {
    let text = unsafe { cstr(label) };
    if text.is_empty() {
        format!("Image {}", index + 1)
    } else {
        text
    }
}
