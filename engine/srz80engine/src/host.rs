//! The host tables handed to card plugins.
//!
//! Each callback receives the owning [`Core`] as its opaque context.  The
//! tables are `Box`ed so their addresses never move while a plugin holds a
//! borrowed extension pointer, and every callback contains a panic so a Rust
//! failure can never unwind into C++ card code.
//!
//! The `ShouryoHost` declaration order and the extension IDs are part of the
//! card ABI. `SRH_HEADER`/`SRH_INIT` equivalents are reproduced field for
//! field by `crate::ffi`.  Every callback is an `unsafe extern "C" fn` because
//! that is the pointer type the C headers declare.

use core::ffi::{c_char, c_int, c_void};
use core::mem::size_of;
use std::ffi::CStr;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use crate::core::{Core, Handle};
use crate::ffi::*;
use crate::guard::guarded;

pub struct HostTables {
    pub host: Box<ShouryoHost>,
    pub resources: Box<SrhHostResourcesV1>,
    pub project_files: Box<SrhHostProjectFilesV1>,
    pub lifecycle: Box<SrhHostLifecycleV1>,
    pub debug: Box<SrhHostDebugV1>,
    pub memory: Box<SrhHostMemoryV1>,
    pub input: Box<SrhHostInputV1>,
    pub signals: Box<SrhHostSignalsV1>,
    pub text: Box<SrhHostTextV1>,
    pub video: Box<SrhHostVideoV1>,
    pub audio: Box<SrhHostAudioV1>,
    pub audio_input: Box<SrhHostAudioInputV1>,
    pub config: Box<SrhHostConfigV1>,
    pub providers: Box<SrhHostProvidersV1>,
}

impl HostTables {
    /// Builds the tables around `context`, which must be the stable context
    /// slot owned by the core that will receive these callbacks.  Every table
    /// copies that cookie, so re-pointing the slot is how a moved core keeps
    /// observing the same tables.
    pub fn new(context: *mut c_void) -> HostTables {
        let resources = Box::new(SrhHostResourcesV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostResourcesV1>() as u32,
            context,
            lookup: Some(lookup_named),
        });
        let debug = Box::new(SrhHostDebugV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostDebugV1>() as u32,
            context,
            register_disasm: Some(register_disasm),
            boundary_ex: Some(boundary_ex),
            request_stop: Some(request_stop),
            set_trace_kind: Some(set_trace_kind),
            trace_enabled: Some(trace_enabled),
            boundary_required: Some(boundary_required),
            set_disassembly_enabled: Some(set_disassembly_enabled),
        });
        let memory = Box::new(SrhHostMemoryV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostMemoryV1>() as u32,
            context,
            read_word: Some(host_read_word),
        });
        let input = Box::new(SrhHostInputV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostInputV1>() as u32,
            context,
            register_input: Some(register_input),
            input_due: Some(input_due),
            input_pop: Some(input_pop),
            subscribe_due: Some(subscribe_input_due),
        });
        let signals = Box::new(SrhHostSignalsV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostSignalsV1>() as u32,
            context,
            release: Some(release_signal),
        });
        let text = Box::new(SrhHostTextV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostTextV1>() as u32,
            context,
            register_text: Some(register_text),
        });
        let video = Box::new(SrhHostVideoV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostVideoV1>() as u32,
            context,
            register_video: Some(register_video),
            register_video_ex: Some(register_video_ex),
            set_video_timing: Some(set_video_timing),
        });
        // Safety: `core` is the engine that owns these tables and has already
        // reached its final address.
        let (sample_rate, channels) = unsafe { audio_format(context) };
        let audio = Box::new(SrhHostAudioV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostAudioV1>() as u32,
            context,
            sample_rate,
            channels,
            format: SRH_AUDIO_S16_STEREO,
            register_source: Some(register_audio_source),
        });
        let config = Box::new(SrhHostConfigV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostConfigV1>() as u32,
            context,
            register_entry: Some(register_config_entry),
            unregister_context: Some(unregister_config_entries),
            get_value: Some(config_value),
            set_value: Some(config_set),
        });
        let providers = Box::new(SrhHostProvidersV1 {
            abi_version: SRH_ABI,
            struct_size: size_of::<SrhHostProvidersV1>() as u32,
            context,
            register_provider: Some(register_provider),
            simulation_time_ns: Some(provider_time_ns),
        });
        let host = Box::new(ShouryoHost {
            abi_version: SRH_ABI,
            struct_size: size_of::<ShouryoHost>() as u32,
            context,
            log: Some(host_log),
            map: Some(host_map),
            unmap: Some(host_unmap),
            read: Some(host_read),
            write: Some(host_write),
            subscribe_clock: Some(host_subscribe),
            schedule: Some(host_schedule),
            cancel: Some(host_cancel),
            boundary: Some(host_boundary),
            remove: Some(host_remove),
            alive: Some(host_alive),
            signal_find: Some(host_signal_find),
            signal_drive: Some(host_signal_drive),
            signal_subscribe: Some(host_signal_subscribe),
            signal_read: Some(host_signal_read),
            time_ns: Some(host_time_ns),
            random_u64: Some(host_random),
            query: Some(host_query),
        });
        HostTables {
            host,
            resources,
            project_files: Box::new(SrhHostProjectFilesV1 {
                abi_version: SRH_ABI,
                struct_size: size_of::<SrhHostProjectFilesV1>() as u32,
                context,
                project_root: Some(project_root),
                read_file: Some(project_read_file),
                write_file: Some(project_write_file),
            }),
            lifecycle: Box::new(SrhHostLifecycleV1 {
                abi_version: SRH_ABI,
                struct_size: size_of::<SrhHostLifecycleV1>() as u32,
                context,
                subscribe_resume: Some(subscribe_resume),
            }),
            debug,
            memory,
            input,
            signals,
            text,
            video,
            audio,
            audio_input: Box::new(SrhHostAudioInputV1 {
                abi_version: SRH_ABI,
                struct_size: size_of::<SrhHostAudioInputV1>() as u32,
                context,
                request: Some(crate::audio_input::request),
                read: Some(crate::audio_input::read),
            }),
            config,
            providers,
        }
    }

    pub fn host(&self) -> *const ShouryoHost {
        &*self.host
    }

    /// Finds an extension table by its stable id.
    pub fn find(&self, id: &str) -> *const c_void {
        match id {
            "host.resources.v1" => &*self.resources as *const _ as *const c_void,
            "host.project_files.v1" => &*self.project_files as *const _ as *const c_void,
            "host.lifecycle.v1" => &*self.lifecycle as *const _ as *const c_void,
            "host.debug.v1" => &*self.debug as *const _ as *const c_void,
            "host.memory.v1" => &*self.memory as *const _ as *const c_void,
            "host.signals.v1" => &*self.signals as *const _ as *const c_void,
            "host.input.v1" => &*self.input as *const _ as *const c_void,
            "host.text.v1" => &*self.text as *const _ as *const c_void,
            "host.video.v1" => &*self.video as *const _ as *const c_void,
            "host.audio_input.v1" => &*self.audio_input as *const _ as *const c_void,
            "host.audio.v1" => &*self.audio as *const _ as *const c_void,
            "host.providers.v1" => &*self.providers as *const _ as *const c_void,
            "host.config.v1" => &*self.config as *const _ as *const c_void,
            _ => core::ptr::null(),
        }
    }
}

const PROJECT_FILE_LIMIT: u64 = 16 * 1024 * 1024;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

unsafe extern "C" fn subscribe_resume(
    context: *mut c_void,
    owner: Handle,
    callback: SrhCallback,
    callback_context: *mut c_void,
    subscription: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.subscribe_resume(owner, callback, callback_context, subscription)
    })
}

fn project_file_path(root: &Path, name: &str) -> Result<PathBuf, SrhStatus> {
    if name.is_empty() || name.starts_with('/') || name.starts_with('\\') || name.contains(':') {
        return Err(SRH_INVALID);
    }
    let mut parts = Vec::new();
    for part in name.split(['/', '\\']) {
        match part {
            "" | "." => {},
            ".." => { if parts.pop().is_none() { return Err(SRH_INVALID); } },
            _ if part.contains('\0') => return Err(SRH_INVALID),
            _ => parts.push(part),
        }
    }
    if parts.is_empty() { return Err(SRH_INVALID); }
    let mut path = root.to_path_buf();
    for part in parts { path.push(part); }
    Ok(path)
}

fn project_path_arg(path: *const c_char) -> Result<String, SrhStatus> {
    if path.is_null() { return Err(SRH_INVALID); }
    unsafe { CStr::from_ptr(path) }.to_str().map(str::to_owned).map_err(|_| SRH_INVALID)
}

unsafe fn project_core<'a>(context: *mut c_void, owner: Handle) -> Result<&'a Core, SrhStatus> {
    let core = unsafe { core_from_host(context) }.ok_or(SRH_INVALID)?;
    if !core.alive(owner) { return Err(SRH_INVALID); }
    Ok(core)
}

fn project_checked_parent(root: &Path, path: &Path) -> Result<(), SrhStatus> {
    let parent = path.parent().ok_or(SRH_INVALID)?;
    let canonical = std::fs::canonicalize(parent).map_err(|error| {
        if error.kind() == std::io::ErrorKind::NotFound { SRH_NOT_FOUND } else { SRH_ERROR }
    })?;
    if !canonical.starts_with(root) { return Err(SRH_INVALID); }
    Ok(())
}

unsafe extern "C" fn project_root(context: *mut c_void, owner: Handle, buffer: *mut c_char, size: *mut u64) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let core = match unsafe { project_core(context, owner) } { Ok(core) => core, Err(status) => return status };
        let Some(root) = &core.project_root else { return SRH_UNAVAILABLE; };
        if size.is_null() { return SRH_INVALID; }
        let Some(path) = root.to_str() else { return SRH_ERROR; };
        let needed = path.len() as u64 + 1;
        let capacity = unsafe { *size };
        unsafe { *size = needed };
        if buffer.is_null() { return SRH_OK; }
        if capacity < needed { return SRH_INVALID; }
        unsafe {
            std::ptr::copy_nonoverlapping(path.as_ptr(), buffer.cast(), path.len());
            *buffer.add(path.len()) = 0;
        }
        SRH_OK
    })
}

unsafe extern "C" fn project_read_file(context: *mut c_void, owner: Handle, relative_path: *const c_char, buffer: *mut u8, size: *mut u64) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let core = match unsafe { project_core(context, owner) } { Ok(core) => core, Err(status) => return status };
        let Some(root) = &core.project_root else { return SRH_UNAVAILABLE; };
        if size.is_null() { return SRH_INVALID; }
        let name = match project_path_arg(relative_path) { Ok(name) => name, Err(status) => return status };
        let path = match project_file_path(root, &name) { Ok(path) => path, Err(status) => return status };
        let resolved = match std::fs::canonicalize(&path) {
            Ok(path) => path,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return SRH_NOT_FOUND,
            Err(_) => return SRH_ERROR,
        };
        if !resolved.starts_with(root) { return SRH_INVALID; }
        let metadata = match std::fs::metadata(&resolved) { Ok(meta) => meta, Err(_) => return SRH_ERROR };
        if !metadata.is_file() { return SRH_INVALID; }
        if metadata.len() > PROJECT_FILE_LIMIT { return SRH_INVALID; }
        let file = match std::fs::File::open(&resolved) { Ok(file) => file, Err(_) => return SRH_ERROR };
        let needed = metadata.len();
        let capacity = unsafe { *size };
        unsafe { *size = needed };
        if buffer.is_null() { return SRH_OK; }
        if capacity < needed { return SRH_INVALID; }
        let mut bytes = Vec::new();
        if file.take(PROJECT_FILE_LIMIT + 1).read_to_end(&mut bytes).is_err() { return SRH_ERROR; }
        if bytes.len() as u64 > PROJECT_FILE_LIMIT { return SRH_INVALID; }
        if bytes.len() as u64 > capacity { unsafe { *size = bytes.len() as u64 }; return SRH_INVALID; }
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer, bytes.len()); *size = bytes.len() as u64; }
        SRH_OK
    })
}

struct RemoveTemp(PathBuf);
impl Drop for RemoveTemp { fn drop(&mut self) { let _ = std::fs::remove_file(&self.0); } }

unsafe extern "C" fn project_write_file(context: *mut c_void, owner: Handle, relative_path: *const c_char, data: *const u8, size: u64) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let core = match unsafe { project_core(context, owner) } { Ok(core) => core, Err(status) => return status };
        let Some(root) = &core.project_root else { return SRH_UNAVAILABLE; };
        if size > PROJECT_FILE_LIMIT || (size > 0 && data.is_null()) { return SRH_INVALID; }
        let name = match project_path_arg(relative_path) { Ok(name) => name, Err(status) => return status };
        let path = match project_file_path(root, &name) { Ok(path) => path, Err(status) => return status };
        if path.parent() == Some(root.as_path()) && path.file_name().is_some_and(|name| name.to_string_lossy().eq_ignore_ascii_case("project.json")) { return SRH_INVALID; }
        if let Err(status) = project_checked_parent(root, &path) { return status; }
        match std::fs::symlink_metadata(&path) {
            Ok(meta) if meta.file_type().is_symlink() || !meta.is_file() => return SRH_INVALID,
            Ok(_) => {},
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {},
            Err(_) => return SRH_ERROR,
        }
        let parent = path.parent().unwrap();
        let mut temporary = None;
        for _ in 0..16 {
            let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let candidate = parent.join(format!(".srz80-{}-{sequence}.tmp", std::process::id()));
            match std::fs::OpenOptions::new().write(true).create_new(true).open(&candidate) {
                Ok(file) => { temporary = Some((candidate, file)); break; },
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {},
                Err(_) => return SRH_ERROR,
            }
        }
        let Some((temp_path, mut file)) = temporary else { return SRH_ERROR; };
        let cleanup = RemoveTemp(temp_path.clone());
        let bytes = if size == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(data, size as usize) } };
        if file.write_all(bytes).is_err() || file.sync_all().is_err() { return SRH_ERROR; }
        drop(file);
        if std::fs::rename(&temp_path, &path).is_err() { return SRH_ERROR; }
        drop(cleanup);
        SRH_OK
    })
}

/// Recovers the owning core from a callback context.
///
/// # Safety
/// `context` must be the pointer the engine passed to the plugin, or null.
unsafe fn core<'a>(context: *mut c_void) -> Option<&'a Core> {
    if context.is_null() {
        None
    } else {
        Some(unsafe { &*(context as *const Core) })
    }
}

/// Recovers the owning core from a host-table context.
///
/// The context is the core address captured when the tables were built.  A
/// candidate rack keeps its own address when it is installed into an active
/// engine -- `srz80_engine_replace` exchanges the two handles' cores instead of
/// moving a core value -- so the captured address stays valid for the whole
/// life of the rack.
///
/// # Safety
/// `context` must be null or the address of a live core built by this engine.
unsafe fn core_from_host<'a>(context: *mut c_void) -> Option<&'a Core> {
    if context.is_null() {
        return None;
    }
    unsafe { core(context) }
}

/// Reads the core out of a host-table context cookie.  Used by the card data
/// providers, which forward the engine's context to a plugin callback.
///
/// # Safety
/// Same contract as [`core_from_host`].
pub unsafe fn context_of<'a>(context: *mut c_void) -> Option<&'a Core> {
    unsafe { core_from_host(context) }
}

/// Audio mixer format reported through `host.audio.v1`.
///
/// # Safety
/// `context` must be null or the pointer the engine passed to a plugin.
unsafe fn audio_format(context: *mut c_void) -> (u32, u32) {
    match unsafe { core_from_host(context) } {
        Some(core) => (core.audio_sample_rate, core.audio_channels),
        None => (44100, 2),
    }
}

unsafe extern "C" fn host_log(
    context: *mut c_void,
    owner: Handle,
    message: *const c_char,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        // Safety: the engine owns `context`. The plugin borrowed the string
        // only for this call.
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if message.is_null() {
            return SRH_INVALID;
        }
        if owner != 0 && !core.alive(owner) {
            return SRH_INVALID;
        }
        let text = unsafe { crate::core::cstr(message) };
        core.log(format!("[card {owner}] {text}"));
        SRH_OK
    })
}

unsafe extern "C" fn host_map(
    context: *mut c_void,
    owner: Handle,
    mapping: *const SrhMapping,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        // Safety: `context` is the engine and `mapping` is borrowed for the
        // duration of the call.
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if mapping.is_null() {
            return SRH_INVALID;
        }
        // Safety: the plugin supplies one of its own mapping records. The
        // header check keeps an older layout from being read as a newer one.
        if !valid(mapping, size_of::<SrhMapping>()) {
            return SRH_INVALID;
        }
        core.mapping(owner, unsafe { &*mapping }, result)
    })
}

unsafe extern "C" fn host_unmap(context: *mut c_void, id: Handle) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.unmap(id)
    })
}

unsafe extern "C" fn host_read(
    context: *mut c_void,
    owner: Handle,
    space: Handle,
    address: u64,
    value: *mut u8,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if value.is_null() {
            return SRH_INVALID;
        }
        let mut byte = 0u8;
        let status = core.read(owner, space, address, &mut byte, false);
        if status == SRH_OK {
            // Safety: the caller supplied a writable output.
            unsafe { *value = byte };
        }
        status
    })
}

unsafe extern "C" fn host_write(
    context: *mut c_void,
    owner: Handle,
    space: Handle,
    address: u64,
    value: u8,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.write(owner, space, address, value)
    })
}

unsafe extern "C" fn host_subscribe(
    context: *mut c_void,
    owner: Handle,
    clock: u32,
    callback: SrhCallback,
    callback_context: *mut c_void,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.subscribe(owner, clock, callback, callback_context, result)
    })
}

unsafe extern "C" fn host_schedule(
    context: *mut c_void,
    owner: Handle,
    delay: u64,
    callback: SrhCallback,
    callback_context: *mut c_void,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.schedule(owner, delay, callback, callback_context, result)
    })
}

unsafe extern "C" fn host_cancel(context: *mut c_void, event: Handle) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.cancel(event)
    })
}

unsafe extern "C" fn host_boundary(
    context: *mut c_void,
    card: Handle,
    space: Handle,
    pc: u64,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.boundary(card, space, pc, 0, 0)
    })
}

unsafe extern "C" fn host_remove(context: *mut c_void, owner: Handle) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.remove(owner)
    })
}

unsafe extern "C" fn host_alive(context: *mut c_void, owner: Handle) -> SrhStatus {
    let Some(core) = (unsafe { core_from_host(context) }) else {
        return SRH_INVALID;
    };
    if core.alive(owner) {
        SRH_OK
    } else {
        SRH_NOT_FOUND
    }
}

unsafe extern "C" fn host_signal_find(
    context: *mut c_void,
    name: *const c_char,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if name.is_null() || result.is_null() {
            return SRH_INVALID;
        }
        let handle = core.find_signal(&unsafe { crate::core::cstr(name) });
        if handle == 0 {
            return SRH_NOT_FOUND;
        }
        // Safety: the caller supplied a writable output.
        unsafe { *result = handle };
        SRH_OK
    })
}

unsafe extern "C" fn host_signal_drive(
    context: *mut c_void,
    owner: Handle,
    signal: Handle,
    value: i32,
    priority: i32,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.drive(owner, signal, value, priority)
    })
}

unsafe extern "C" fn host_signal_subscribe(
    context: *mut c_void,
    owner: Handle,
    signal: Handle,
    callback: SrhSignalCallback,
    callback_context: *mut c_void,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.signal_subscribe(owner, signal, callback, callback_context, result)
    })
}

unsafe extern "C" fn host_signal_read(
    context: *mut c_void,
    signal: Handle,
    value: *mut i32,
) -> SrhStatus {
    let Some(core) = (unsafe { core_from_host(context) }) else {
        return SRH_INVALID;
    };
    if value.is_null() {
        return SRH_INVALID;
    }
    match core.sample(signal) {
        Ok(sampled) => {
            // Safety: the caller supplied a writable output.
            unsafe { *value = sampled };
            SRH_OK
        }
        Err(status) => status,
    }
}

unsafe extern "C" fn host_time_ns(context: *mut c_void, value: *mut u64) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if value.is_null() {
            return SRH_INVALID;
        }
        // Safety: the caller supplied a writable output.
        unsafe { *value = core.time_ns() };
        SRH_OK
    })
}

unsafe extern "C" fn host_random(context: *mut c_void, value: *mut u64) -> SrhStatus {
    let Some(core) = (unsafe { core_from_host(context) }) else {
        return SRH_INVALID;
    };
    if value.is_null() {
        return SRH_INVALID;
    }
    // Safety: the caller supplied a writable output.
    unsafe { *value = core.random() };
    SRH_OK
}

unsafe extern "C" fn host_query(
    context: *mut c_void,
    id: *const c_char,
    result: *mut *const c_void,
) -> SrhStatus {
    let Some(core) = (unsafe { core_from_host(context) }) else {
        return SRH_INVALID;
    };
    if id.is_null() || result.is_null() {
        return SRH_INVALID;
    }
    let name = unsafe { crate::core::cstr(id) };
    // The extension tables live with the core that owns this context. Their
    // addresses stay stable for the engine lifetime.
    let pointer = match core.host_tables() {
        Some(tables) => tables.find(&name),
        None => core::ptr::null(),
    };
    if pointer.is_null() {
        return SRH_NOT_FOUND;
    }
    // Safety: the caller supplied a writable output.
    unsafe { *result = pointer };
    SRH_OK
}

unsafe extern "C" fn lookup_named(
    context: *mut c_void,
    kind: *const c_char,
    name: *const c_char,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if kind.is_null() || name.is_null() || result.is_null() {
            return SRH_INVALID;
        }
        let kind = unsafe { crate::core::cstr(kind) };
        let name = unsafe { crate::core::cstr(name) };
        let handle = if kind == "space" {
            core.find_space(&name)
        } else if kind == "signal" {
            core.find_signal(&name)
        } else {
            return SRH_NOT_FOUND;
        };
        if handle == 0 {
            return SRH_NOT_FOUND;
        }
        // Safety: the caller supplied a writable output.
        unsafe { *result = handle };
        SRH_OK
    })
}

unsafe extern "C" fn register_disasm(
    context: *mut c_void,
    card: Handle,
    function: SrhDisasm,
    disasm_context: *mut c_void,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if !core.alive(card) {
            return SRH_INVALID;
        }
        let cards = core.cards.borrow();
        let Some(entry) = cards.get(&card) else {
            return SRH_NOT_FOUND;
        };
        entry.disasm_revision.set(entry.disasm_revision.get() + 1);
        entry.disasm.set(Some(function));
        entry.disasm_context.set(disasm_context);
        SRH_OK
    })
}

unsafe extern "C" fn boundary_ex(
    context: *mut c_void,
    card: Handle,
    space: Handle,
    pc: u64,
    instruction_bytes: u32,
    cycles: u32,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.boundary(card, space, pc, instruction_bytes, cycles)
    })
}

unsafe extern "C" fn request_stop(
    context: *mut c_void,
    card: Handle,
    reason: *const c_char,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if reason.is_null() {
            return SRH_INVALID;
        }
        core.request_stop(card, &unsafe { crate::core::cstr(reason) })
    })
}

unsafe extern "C" fn set_trace_kind(context: *mut c_void, card: Handle, kind: u32) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if card != 0 && !core.alive(card) {
            return SRH_NOT_FOUND;
        }
        core.trace_kind.set(kind);
        SRH_OK
    })
}

unsafe extern "C" fn trace_enabled(context: *mut c_void) -> c_int {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return 0;
        };
        if core.trace_capture.get() {
            1
        } else {
            0
        }
    })
}

unsafe extern "C" fn boundary_required(context: *mut c_void) -> c_int {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return 0;
        };
        if core.boundary_required() {
            1
        } else {
            0
        }
    })
}

unsafe extern "C" fn host_read_word(
    context: *mut c_void,
    master: Handle,
    space: Handle,
    address: u64,
    value: *mut u32,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if value.is_null() {
            return SRH_INVALID;
        }
        let mut word = 0u32;
        let status = core.read_word(master, space, address, &mut word);
        if status == SRH_OK {
            // Safety: the caller supplied a writable output.
            unsafe { *value = word };
        }
        status
    })
}

unsafe extern "C" fn release_signal(
    context: *mut c_void,
    owner: Handle,
    signal: Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.release_signal(owner, signal)
    })
}

unsafe extern "C" fn register_input(
    context: *mut c_void,
    owner: Handle,
    name: *const c_char,
    capacity: u32,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if name.is_null() {
            return SRH_INVALID;
        }
        core.register_input(owner, &unsafe { crate::core::cstr(name) }, capacity, result)
    })
}

unsafe extern "C" fn input_due(
    context: *mut c_void,
    endpoint: Handle,
    count: *mut u64,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.input_due(endpoint, count)
    })
}

unsafe extern "C" fn input_pop(
    context: *mut c_void,
    endpoint: Handle,
    timestamp: *mut u64,
    value: *mut u8,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.input_pop(endpoint, timestamp, value)
    })
}

unsafe extern "C" fn subscribe_input_due(
    context: *mut c_void,
    owner: Handle,
    endpoint: Handle,
    callback: SrhCallback,
    callback_context: *mut c_void,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.subscribe_input_due(owner, endpoint, callback, callback_context, result)
    })
}

unsafe extern "C" fn register_text(
    context: *mut c_void,
    card: Handle,
    query: SrhTextQuery,
    text_context: *mut c_void,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if !core.alive(card) {
            return SRH_INVALID;
        }
        let cards = core.cards.borrow();
        let Some(entry) = cards.get(&card) else {
            return SRH_NOT_FOUND;
        };
        entry.text.set(Some(query));
        entry.text_context.set(text_context);
        SRH_OK
    })
}

unsafe extern "C" fn register_video(
    context: *mut c_void,
    owner: Handle,
    width: u32,
    height: u32,
    format: SrhVideoFormat,
    query: SrhVideoQuery,
    video_context: *mut c_void,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.register_video(
            owner,
            width,
            height,
            format,
            0,
            query,
            video_context,
            result,
        )
    })
}

unsafe extern "C" fn set_video_timing(
    context: *mut c_void,
    surface: Handle,
    query: Option<SrhVideoTimingQuery>,
    timing_context: *mut c_void,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.set_video_timing(surface, query, timing_context)
    })
}

unsafe extern "C" fn register_video_ex(
    context: *mut c_void,
    owner: Handle,
    width: u32,
    height: u32,
    format: SrhVideoFormat,
    query: SrhVideoQuery,
    video_context: *mut c_void,
    flags: u32,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.register_video(
            owner,
            width,
            height,
            format,
            flags,
            query,
            video_context,
            result,
        )
    })
}

#[allow(clippy::too_many_arguments)]
unsafe extern "C" fn register_audio_source(
    context: *mut c_void,
    owner: Handle,
    sample_rate: u32,
    channels: u32,
    format: SrhAudioFormat,
    name: *const c_char,
    render: SrhAudioRender,
    render_context: *mut c_void,
    result: *mut Handle,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if name.is_null() {
            return SRH_INVALID;
        }
        core.audio_register_source(
            owner,
            sample_rate,
            channels,
            format,
            &unsafe { crate::core::cstr(name) },
            render,
            render_context,
            result,
        )
    })
}

unsafe extern "C" fn register_config_entry(
    context: *mut c_void,
    entry: *const SrhConfigEntry,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if !valid(entry, size_of::<SrhConfigEntry>()) {
            return SRH_INVALID;
        }
        if core.register_config_entry(unsafe { &*entry }) {
            SRH_OK
        } else {
            SRH_CONFLICT
        }
    })
}

unsafe extern "C" fn unregister_config_entries(
    context: *mut c_void,
    entry_context: *mut c_void,
) -> SrhStatus {
    let Some(core) = (unsafe { core_from_host(context) }) else {
        return SRH_INVALID;
    };
    core.unregister_config_entries(entry_context);
    SRH_OK
}

unsafe extern "C" fn config_value(
    context: *mut c_void,
    key: *const c_char,
    value: *mut c_char,
    capacity: u32,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if key.is_null() || value.is_null() || capacity == 0 {
            return SRH_INVALID;
        }
        let text = core.config_value(&unsafe { crate::core::cstr(key) }, "");
        let bytes = text.as_bytes();
        let amount = bytes.len().min(capacity as usize - 1);
        // Safety: the caller supplied a buffer of `capacity` bytes.
        unsafe {
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), value as *mut u8, amount);
            *value.add(amount) = 0;
        }
        SRH_OK
    })
}

unsafe extern "C" fn config_set(
    context: *mut c_void,
    key: *const c_char,
    value: *const c_char,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if key.is_null() || value.is_null() {
            return SRH_INVALID;
        }
        core.config_set(&unsafe { crate::core::cstr(key) }, &unsafe {
            crate::core::cstr(value)
        });
        SRH_OK
    })
}

unsafe extern "C" fn register_provider(
    context: *mut c_void,
    owner: Handle,
    provider: *const SrhDataProviderV1,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        core.register_provider(owner, provider)
    })
}

unsafe extern "C" fn provider_time_ns(context: *mut c_void, now: *mut u64) -> SrhStatus {
    let Some(core) = (unsafe { core_from_host(context) }) else {
        return SRH_INVALID;
    };
    if now.is_null() {
        return SRH_INVALID;
    }
    // Safety: the caller supplied a writable output.
    unsafe { *now = core.now.get() };
    SRH_OK
}
unsafe extern "C" fn set_disassembly_enabled(
    context: *mut c_void,
    card: Handle,
    enabled: u32,
    message: *const c_char,
) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { core_from_host(context) }) else {
            return SRH_INVALID;
        };
        if enabled > 1 {
            return SRH_INVALID;
        }
        let cards = core.cards.borrow();
        let Some(entry) = cards.get(&card) else {
            return SRH_NOT_FOUND;
        };
        if !entry.is_active() && !entry.is_parked() {
            return SRH_UNAVAILABLE;
        }
        let message = unsafe { crate::core::cstr(message) };
        if entry.disasm_enabled.get() != (enabled != 0) || *entry.disasm_message.borrow() != message
        {
            entry.disasm_enabled.set(enabled != 0);
            *entry.disasm_message.borrow_mut() = message;
            entry.disasm_revision.set(entry.disasm_revision.get() + 1);
        }
        SRH_OK
    })
}
