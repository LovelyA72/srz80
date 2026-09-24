//! Platform paths used by the engine: the executable directory that anchors
//! `config.ini` and the default plugin folder, and the native plugin
//! extension.

use std::path::{Path, PathBuf};

pub fn plugin_extension() -> &'static str {
    if cfg!(windows) {
        ".dll"
    } else {
        ".so"
    }
}

/// The directory of the running executable.  Plugin discovery, project-relative
/// assets and the host configuration path all resolve against it, not against
/// the current working directory.
pub fn executable_directory() -> PathBuf {
    match std::env::current_exe() {
        Ok(path) => path.parent().map(Path::to_path_buf).unwrap_or_default(),
        Err(_) => PathBuf::new(),
    }
}

pub fn default_plugins_directory() -> PathBuf {
    executable_directory().join("plugins")
}
