//! The engine handle and the exported engine ABI.
//!
//! Every export validates its inputs and reports ordinary failures through a
//! status plus `srz80_engine_last_error`. Diagnostic builds contain panics;
//! shipping builds abort on one, so it cannot unwind through the ABI. Engine
//! instances are single-threaded: one handle belongs to the thread that
//! created it and every card it loads is driven from that thread.

use core::cell::RefCell;
use core::ffi::{c_char, c_void};
use std::path::PathBuf;

use crate::core::{Core, RunState, TimeMode};
use crate::ffi::*;
use crate::guard::{fail, guard_or, guarded, guarded_value, guarded_void};
use crate::result::ResultArena;

/// One engine instance: the rack plus the host configuration that outlives it.
pub struct EngineHandle {
    /// Owned boxed core; null for a candidate that has not loaded anything yet.
    pub core: *mut Core,
    pub plugins: PathBuf,
    pub last_error: RefCell<String>,
    pub candidate: bool,
    pub audio_sample_rate: u32,
    initial_config: crate::config::Config,
}

impl EngineHandle {
    fn new(core: *mut Core, plugins: PathBuf, candidate: bool) -> Box<EngineHandle> {
        Box::new(EngineHandle {
            core,
            plugins,
            last_error: RefCell::new(String::new()),
            candidate,
            audio_sample_rate: 44_100,
            initial_config: crate::config::Config::default(),
        })
    }

    fn config_snapshot(&self) -> crate::config::Config {
        // The snapshot owns values only; no active-card callbacks cross racks.
        unsafe { self.core() }
            .map(|core| core.config.borrow().clone())
            .unwrap_or_else(|| self.initial_config.clone())
    }

    /// # Safety
    /// `self.core` must be null or a live boxed core owned by this handle.
    pub unsafe fn core(&self) -> Option<&Core> {
        if self.core.is_null() {
            None
        } else {
            Some(unsafe { &*self.core })
        }
    }

    /// Installs a freshly built rack, releasing the previous one.
    ///
    /// # Safety
    /// `next` must be null or a live boxed core.
    pub unsafe fn install(&mut self, next: *mut Core) {
        if !self.core.is_null() {
            drop(unsafe { Box::from_raw(self.core) });
        }
        self.core = next;
    }
}

impl Drop for EngineHandle {
    fn drop(&mut self) {
        if !self.core.is_null() {
            // Safety: the handle owns this core and nothing else references it
            // once destruction begins.
            unsafe { drop(Box::from_raw(self.core)) };
            self.core = std::ptr::null_mut();
        }
    }
}

/// Copies an engine-produced UTF-8 string into a caller-owned buffer.  A null
/// buffer or zero capacity only queries the length.
pub fn store_text(text: &str, buffer: *mut c_char, capacity: u64) -> u64 {
    if !buffer.is_null() && capacity != 0 {
        let copied = (text.len() as u64).min(capacity - 1);
        unsafe {
            core::ptr::copy_nonoverlapping(text.as_ptr(), buffer as *mut u8, copied as usize);
            *buffer.add(copied as usize) = 0;
        }
    }
    text.len() as u64
}

/// Runs `body` with the active rack, reporting the documented failures.
fn with_rack<F>(engine: *mut EngineHandle, body: F) -> SrhStatus
where
    F: FnOnce(&Core) -> SrhStatus,
{
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    guarded(engine, || body(core))
}

/// Runs `body` with the active rack and a result arena.
fn with_arena<F>(engine: *const EngineHandle, result: *mut ResultArena, body: F) -> SrhStatus
where
    F: FnOnce(&Core, &mut ResultArena) -> SrhStatus,
{
    if engine.is_null() || result.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine`/`result` are owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_INVALID,
    };
    let arena = unsafe { &mut *result };
    guarded(engine as *mut EngineHandle, || body(core, arena))
}

fn fill_card(card: &crate::core::CardInfo, arena: &mut ResultArena) -> SrzCardInfo {
    let type_ = arena.intern(&card.type_);
    let name = arena.intern(&card.name);
    SrzCardInfo {
        abi_version: SRZ80_ENGINE_ABI,
        struct_size: core::mem::size_of::<SrzCardInfo>() as u32,
        id: card.id,
        type_,
        name,
        priority: card.priority,
        active: card.active as u32,
        parked: card.parked as u32,
        load_error: arena.intern(&card.load_error),
    }
}

fn fill_trace(trace: &crate::core::Trace, arena: &mut ResultArena) -> SrzTrace {
    let responder_offset = arena.handles.len() as u32;
    let responder_count = trace.responders.len() as u32;
    arena.handles.extend_from_slice(&trace.responders);
    SrzTrace {
        abi_version: SRZ80_ENGINE_ABI,
        struct_size: core::mem::size_of::<SrzTrace>() as u32,
        sequence: trace.sequence,
        parent: trace.parent,
        time: trace.time,
        ticks: trace.ticks,
        master: trace.master,
        space: trace.space,
        address: trace.address,
        operation: trace.operation,
        depth: trace.depth,
        kind: trace.kind,
        instruction: trace.instruction,
        value: trace.value,
        result: trace.result,
        responder_offset,
        responder_count,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_abi_version() -> u32 {
    SRZ80_ENGINE_ABI
}

#[no_mangle]
pub extern "C" fn srz80_engine_version() -> *const c_char {
    concat!(env!("SRZ80_ENGINE_VERSION"), "\0").as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn srz80_engine_capabilities(_engine: *const EngineHandle) -> u32 {
    SRZ_CAP_ALL
}

#[no_mangle]
pub extern "C" fn srz80_engine_create(plugins_directory: SrzSlice) -> *mut EngineHandle {
    guard_or(std::ptr::null_mut(), || {
        // Safety: the caller supplied a borrowed slice for this call.
        let plugins = unsafe {
            if plugins_directory.size != 0 {
                crate::core::slice_path(plugins_directory)
            } else {
                crate::paths::default_plugins_directory()
            }
        };
        let core = Box::into_raw(Box::new(Core::new(RunState::Stopped)));
        // Safety: the core is at its final address and stays owned by the
        // returned handle.
        unsafe { (*core).initialize_host() };
        #[cfg(any(test, debug_assertions))]
        crate::install_diagnostic_panic_hook();
        Box::into_raw(EngineHandle::new(core, plugins, false))
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_candidate_create(engine: *const EngineHandle) -> *mut EngineHandle {
    if engine.is_null() {
        return std::ptr::null_mut();
    }
    guard_or(std::ptr::null_mut(), || {
        // Safety: `engine` is non-null and owned by the caller's thread.
        let plugins = unsafe { (*engine).plugins.clone() };
        let mut candidate = EngineHandle::new(std::ptr::null_mut(), plugins, true);
        candidate.initial_config = unsafe { (*engine).config_snapshot() };
        candidate.audio_sample_rate = unsafe { (*engine).audio_sample_rate };
        Box::into_raw(candidate)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_replace(
    engine: *mut EngineHandle,
    candidate: *mut EngineHandle,
) -> SrhStatus {
    if engine.is_null() || candidate.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Replacement needs an active engine and a candidate".to_string(),
        );
    }
    if engine == candidate {
        return fail(
            engine,
            SRH_INVALID,
            "Cannot replace an engine with itself".to_string(),
        );
    }
    // Safety: both handles are owned by the caller and used on this thread.
    let (engine_ref, candidate_ref) = unsafe { (&mut *engine, &mut *candidate) };
    if !candidate_ref.candidate {
        return fail(
            engine,
            SRH_INVALID,
            "Replacement needs an active engine and a candidate".to_string(),
        );
    }
    if candidate_ref.core.is_null() {
        return fail(
            engine,
            SRH_UNAVAILABLE,
            "Candidate has no loaded project".to_string(),
        );
    }
    guarded(engine, || {
        // The candidate's rack was fully constructed before this point, so the
        // installation only exchanges ownership.  A failure above leaves the
        // active engine and its cards untouched.
        //
        // The swap happens at the handle level rather than by moving the core
        // value: the candidate's core was built at its own address and its host
        // tables, card instances and card contexts all captured that address.
        // Moving it would invalidate them, so the active engine takes over the
        // candidate's pointer and the candidate takes the retired rack, which
        // is then released with the candidate handle below.
        let retired = engine_ref.core;
        engine_ref.core = candidate_ref.core;
        candidate_ref.core = retired;
        engine_ref.plugins = candidate_ref.plugins.clone();
        engine_ref.audio_sample_rate = candidate_ref.audio_sample_rate;
        // Safety: the caller handed the candidate over; releasing it here
        // matches the C++ implementation, which consumes the candidate and with
        // it the retired rack.
        unsafe { drop(Box::from_raw(candidate)) };
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_destroy(engine: *mut EngineHandle) {
    if engine.is_null() {
        return;
    }
    guard_or((), || {
        // Safety: the caller hands ownership back exactly once.
        unsafe { drop(Box::from_raw(engine)) };
    });
}

#[no_mangle]
pub extern "C" fn srz80_engine_last_error(
    engine: *const EngineHandle,
    buffer: *mut c_char,
    capacity: u64,
) -> u64 {
    if engine.is_null() {
        return store_text("", buffer, capacity);
    }
    // Safety: `engine` is non-null and owned by the caller's thread.
    let text = unsafe { (*engine).last_error.borrow().clone() };
    store_text(&text, buffer, capacity)
}

#[no_mangle]
pub extern "C" fn srz80_engine_result_create(_engine: *mut EngineHandle) -> *mut ResultArena {
    guard_or(std::ptr::null_mut(), || {
        Box::into_raw(Box::new(ResultArena::new()))
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_result_destroy(
    _engine: *mut EngineHandle,
    result: *mut ResultArena,
) {
    if result.is_null() {
        return;
    }
    guard_or((), || {
        // Safety: the caller hands ownership back exactly once.
        unsafe { drop(Box::from_raw(result)) };
    });
}

#[no_mangle]
pub extern "C" fn srz80_engine_result_clear(result: *mut ResultArena) {
    if result.is_null() {
        return;
    }
    // Safety: `result` is a live arena owned by the caller.
    let arena = unsafe { &mut *result };
    guard_or((), || arena.clear());
}

macro_rules! result_list {
    ($name:ident, $field:ident, $ty:ty) => {
        #[no_mangle]
        pub extern "C" fn $name(result: *const ResultArena, count: *mut u32) -> *const $ty {
            if count.is_null() {
                return std::ptr::null();
            }
            let arena = if result.is_null() {
                // Safety: a null result reports an empty list.
                unsafe { *count = 0 };
                return std::ptr::null();
            } else {
                // Safety: `result` is a live arena owned by the caller.
                unsafe { &*result }
            };
            if arena.$field.is_empty() {
                unsafe { *count = 0 };
                return std::ptr::null();
            }
            unsafe { *count = arena.$field.len() as u32 };
            arena.$field.as_ptr()
        }
    };
}

result_list!(srz80_engine_result_cards, cards, SrzCardInfo);
result_list!(srz80_engine_result_all_cards, all_cards, SrzCardInfo);
result_list!(
    srz80_engine_result_removed_cards,
    removed_cards,
    SrzCardInfo
);
result_list!(srz80_engine_result_spaces, spaces, SrzSpace);
result_list!(srz80_engine_result_breakpoints, breakpoints, SrzBreakpoint);
result_list!(srz80_engine_result_trace, trace, SrzTrace);
result_list!(srz80_engine_result_handles, handles, SrhHandle);
result_list!(srz80_engine_result_logs, logs, *const c_char);
result_list!(srz80_engine_result_properties, properties, SrzProperty);
result_list!(
    srz80_engine_result_config_entries,
    config_entries,
    SrzConfigEntry
);
result_list!(
    srz80_engine_result_text_endpoints,
    text_endpoints,
    SrzTextEndpoint
);
result_list!(srz80_engine_result_input_names, input_names, *const c_char);
result_list!(
    srz80_engine_result_video_surfaces,
    video_surfaces,
    SrzVideoSurface
);
result_list!(
    srz80_engine_result_audio_sources,
    audio_sources,
    SrzAudioSource
);
result_list!(srz80_engine_result_providers, providers, SrzProviderData);
result_list!(srz80_engine_result_plugin_data, plugin_data, SrzPluginData);
result_list!(srz80_engine_result_paths, paths, *const c_char);
result_list!(srz80_engine_result_endpoints, endpoints, *const c_char);
result_list!(
    srz80_engine_result_input_records,
    input_records,
    SrzInputRecord
);
result_list!(srz80_engine_result_image_slots, image_slots, *const c_char);

#[no_mangle]
pub extern "C" fn srz80_engine_result_plugin_descriptor(
    result: *const ResultArena,
) -> *const SrzPluginDescriptor {
    if result.is_null() {
        return std::ptr::null();
    }
    // Safety: `result` is a live arena owned by the caller.
    let arena = unsafe { &*result };
    if !arena.has_descriptor {
        return std::ptr::null();
    }
    &arena.descriptor
}

#[no_mangle]
pub extern "C" fn srz80_engine_new_project(engine: *mut EngineHandle) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let handle = unsafe { &mut *engine };
    guarded(engine, || match crate::project::new_project(handle.audio_sample_rate) {
        Ok(fresh) => {
            *fresh.config.borrow_mut() = handle.config_snapshot();
            // Safety: the fresh core is live and becomes the active rack.
            unsafe { handle.install(Box::into_raw(fresh)) };
            SRH_OK
        }
        Err(message) => fail(engine, SRH_ERROR, message),
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_load_project(
    engine: *mut EngineHandle,
    project_path: SrzSlice,
    plugins_directory: SrzSlice,
    initial_state: SrzRunState,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread; the slices are borrowed
    // for this call only.
    let handle = unsafe { &mut *engine };
    let path = unsafe { crate::core::slice_path(project_path) };
    let plugins = unsafe {
        if plugins_directory.size != 0 {
            crate::core::slice_path(plugins_directory)
        } else {
            handle.plugins.clone()
        }
    };
    if path.as_os_str().is_empty() {
        return fail(engine, SRH_INVALID, "Project path is empty".to_string());
    }
    guarded(engine, || {
        match crate::project::load_project(
            &path,
            &plugins,
            RunState::from_abi(initial_state),
            &handle.config_snapshot(),
            handle.audio_sample_rate,
        ) {
            Ok(replacement) => {
                // Safety: the replacement core is live and becomes the rack.
                unsafe { handle.install(Box::into_raw(replacement)) };
                SRH_OK
            }
            Err(message) => fail(engine, SRH_ERROR, message),
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_load_project_json(
    engine: *mut EngineHandle,
    json: SrzSlice,
    project_directory: SrzSlice,
    plugins_directory: SrzSlice,
    initial_state: SrzRunState,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread; the slices are borrowed
    // for this call only.
    let handle = unsafe { &mut *engine };
    let document = unsafe { crate::core::slice_text(json) };
    let directory = unsafe { crate::core::slice_path(project_directory) };
    let plugins = unsafe {
        if plugins_directory.size != 0 {
            crate::core::slice_path(plugins_directory)
        } else {
            handle.plugins.clone()
        }
    };
    guarded(engine, || {
        match crate::project::load_project_json(
            &document,
            &directory,
            &plugins,
            RunState::from_abi(initial_state),
            &handle.config_snapshot(),
            handle.audio_sample_rate,
        ) {
            Ok(replacement) => {
                // Safety: the replacement core is live and becomes the rack.
                unsafe { handle.install(Box::into_raw(replacement)) };
                SRH_OK
            }
            Err(message) => fail(engine, SRH_ERROR, message),
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_load_state(engine: *mut EngineHandle, json: SrzSlice) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    let document = unsafe { crate::core::slice_text(json) };
    guarded(engine, || match core.load_state(&document) {
        Ok(()) => SRH_OK,
        Err(message) => fail(engine, SRH_ERROR, message),
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_save_state(
    engine: *mut EngineHandle,
    include_trace: u32,
    buffer: *mut c_char,
    capacity: u64,
    size: *mut u64,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if size.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "save_state needs a size output".to_string(),
        );
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    guarded(engine, || match core.save_state(include_trace != 0) {
        Ok(document) => {
            // Safety: the caller supplied a valid size output.
            unsafe { *size = document.len() as u64 };
            if buffer.is_null() {
                return SRH_OK;
            }
            if capacity < document.len() as u64 + 1 {
                return SRH_UNAVAILABLE;
            }
            // Safety: the caller supplied a buffer of at least `capacity` bytes.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    document.as_ptr(),
                    buffer as *mut u8,
                    document.len(),
                );
                *buffer.add(document.len()) = 0;
            }
            SRH_OK
        }
        Err(message) => fail(engine, SRH_ERROR, message),
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_plugin_project_data(
    engine: *mut EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if result.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "plugin_project_data needs a result".to_string(),
        );
    }
    // Safety: `engine` and `result` are owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    let arena = unsafe { &mut *result };
    guarded(engine, || match core.save_plugin_data() {
        Ok(data) => {
            arena.plugin_data.clear();
            for (owner, hex) in data {
                let size = hex.len() as u64;
                let hex = arena.intern(&hex);
                arena.plugin_data.push(SrzPluginData {
                    abi_version: SRZ80_ENGINE_ABI,
                    struct_size: core::mem::size_of::<SrzPluginData>() as u32,
                    owner,
                    hex,
                    size,
                });
            }
            SRH_OK
        }
        Err(message) => fail(engine, SRH_ERROR, message),
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_load_plugin_data(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    hex: SrzSlice,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    let chunk = unsafe { crate::core::slice_text(hex) };
    guarded(engine, || match core.load_plugin_data(owner, &chunk) {
        Ok(()) => SRH_OK,
        Err(message) => fail(engine, SRH_ERROR, message),
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_add_card(
    engine: *mut EngineHandle,
    request: *const SrzCardRequest,
    card: *mut SrhHandle,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if request.is_null() || card.is_null() {
        return fail(engine, SRH_INVALID, "Invalid card request ABI".to_string());
    }
    // Safety: the caller supplied a request record; the header is checked
    // before any tail field is read.
    let request_ref = unsafe { &*request };
    if request_ref.abi_version != SRZ80_ENGINE_ABI
        || (request_ref.struct_size as usize) < core::mem::size_of::<SrzCardRequest>()
    {
        return fail(engine, SRH_INVALID, "Invalid card request ABI".to_string());
    }
    if request_ref.images.is_null() && request_ref.image_count != 0 {
        return fail(
            engine,
            SRH_INVALID,
            "Card request image parts are missing".to_string(),
        );
    }
    // Safety: `engine` is owned by the caller's thread.
    let handle = unsafe { &*engine };
    let core = match unsafe { handle.core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    guarded(engine, || {
        let config_json = if request_ref.config_json.size != 0 {
            unsafe { crate::core::slice_text(request_ref.config_json) }
        } else {
            "{}".to_string()
        };
        let mut config = SrhConfig {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhConfig>() as u32,
            space: request_ref.space,
            base: request_ref.base,
            size: request_ref.size,
            reset_vector: request_ref.reset_vector,
            priority: request_ref.priority,
            clock: request_ref.clock,
            image: std::ptr::null(),
            image_size: 0,
            config_json: config_json.as_ptr() as *const c_char,
            config_json_size: config_json.len() as u64,
            images: std::ptr::null(),
            image_count: 0,
            error_message: std::ptr::null_mut(),
            error_message_capacity: 0,
        };
        let mut parts: Vec<SrhImagePart> = Vec::with_capacity(request_ref.image_count as usize);
        for index in 0..request_ref.image_count {
            // Safety: the caller declared `image_count` parts.
            let source = unsafe { &*request_ref.images.add(index as usize) };
            if source.abi_version != SRZ80_ENGINE_ABI
                || (source.struct_size as usize) < core::mem::size_of::<SrzImagePart>()
                || (source.data.is_null() && source.size != 0)
            {
                return fail(engine, SRH_INVALID, "Invalid card image part".to_string());
            }
            parts.push(SrhImagePart {
                abi_version: SRH_ABI,
                struct_size: core::mem::size_of::<SrhImagePart>() as u32,
                data: source.data,
                size: source.size,
            });
        }
        if parts.len() == 1 {
            config.image = parts[0].data;
            config.image_size = parts[0].size;
        }
        if !parts.is_empty() {
            config.images = parts.as_ptr();
            config.image_count = parts.len() as u32;
        }
        let directory = if request_ref.plugin_directory.size != 0 {
            unsafe { crate::core::slice_path(request_ref.plugin_directory) }
        } else {
            handle.plugins.clone()
        };
        let type_ = unsafe { crate::core::slice_text(request_ref.type_) };
        let plugin = crate::plugin::resolve_plugin(core, &directory, &type_);
        if plugin.as_os_str().is_empty() {
            return fail(
                engine,
                SRH_NOT_FOUND,
                format!("Cannot find plugin with ID: {type_}"),
            );
        }
        match core.load(&plugin, &config) {
            Ok(id) => {
                // Safety: the caller supplied a valid output pointer.
                unsafe { *card = id };
                SRH_OK
            }
            Err(message) => fail(engine, SRH_ERROR, message),
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_park(engine: *mut EngineHandle, card: SrhHandle) -> SrhStatus {
    with_rack(engine, |core| core.park(card))
}

#[no_mangle]
pub extern "C" fn srz80_engine_plug(engine: *mut EngineHandle, card: SrhHandle) -> SrhStatus {
    with_rack(engine, |core| core.plug(card))
}

#[no_mangle]
pub extern "C" fn srz80_engine_put_away(engine: *mut EngineHandle, card: SrhHandle) -> SrhStatus {
    with_rack(engine, |core| core.put_away(card))
}

#[no_mangle]
pub extern "C" fn srz80_engine_remove(engine: *mut EngineHandle, card: SrhHandle) -> SrhStatus {
    with_rack(engine, |core| core.remove(card))
}

#[no_mangle]
pub extern "C" fn srz80_engine_reorder_cards(
    engine: *mut EngineHandle,
    order: *const SrhHandle,
    count: u32,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if order.is_null() && count != 0 {
        return fail(engine, SRH_INVALID, "Missing rack order".to_string());
    }
    // Safety: the caller declared `count` handles.
    let handles = if count == 0 {
        Vec::new()
    } else {
        unsafe { core::slice::from_raw_parts(order, count as usize).to_vec() }
    };
    with_rack(engine, |core| core.reorder_cards(handles))
}

#[no_mangle]
pub extern "C" fn srz80_engine_card_alive(
    engine: *const EngineHandle,
    card: SrhHandle,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => {
            if core.alive(card) {
                SRH_OK
            } else {
                SRH_NOT_FOUND
            }
        }
        None => SRH_UNAVAILABLE,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_set_card_name(
    engine: *mut EngineHandle,
    card: SrhHandle,
    name: SrzSlice,
) -> SrhStatus {
    // Safety: the slice is borrowed for this call only.
    let name = unsafe { crate::core::slice_text(name) };
    with_rack(engine, |core| core.set_card_name(card, name))
}

#[no_mangle]
pub extern "C" fn srz80_engine_set_card_clock(
    engine: *mut EngineHandle,
    card: SrhHandle,
    clock: u32,
) -> SrhStatus {
    with_rack(engine, |core| core.set_card_clock(card, clock))
}

#[no_mangle]
pub extern "C" fn srz80_engine_cards(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.cards.clear();
        for card in core.cards() {
            let entry = fill_card(&card, arena);
            arena.cards.push(entry);
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_all_cards(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.all_cards.clear();
        for card in core.all_cards() {
            let entry = fill_card(&card, arena);
            arena.all_cards.push(entry);
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_removed_cards(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.removed_cards.clear();
        for card in core.removed_cards() {
            let entry = fill_card(&card, arena);
            arena.removed_cards.push(entry);
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_spaces(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.spaces.clear();
        for space in core.spaces.borrow().values() {
            let name = arena.intern(&space.name);
            arena.spaces.push(SrzSpace {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzSpace>() as u32,
                id: space.id,
                name,
                maximum: space.maximum,
                fallback: space.fallback,
                reserved: [0; 3],
                random: space.random as u32,
                resolver: if space.resolver == crate::core::Resolver::BitOr {
                    SRZ_RESOLVER_BIT_OR
                } else {
                    SRZ_RESOLVER_PRIORITY
                },
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_find_space(
    engine: *const EngineHandle,
    name: SrzSlice,
    space: *mut SrhHandle,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_INVALID,
    };
    if space.is_null() {
        return SRH_INVALID;
    }
    let name = unsafe { crate::core::slice_text(name) };
    guarded(engine as *mut EngineHandle, || {
        let id = core.find_space(&name);
        if id == 0 {
            return SRH_NOT_FOUND;
        }
        // Safety: the caller supplied a valid output pointer.
        unsafe { *space = id };
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_clocks(
    engine: *const EngineHandle,
    clocks: *mut SrzClock,
    capacity: u32,
    count: *mut u32,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_INVALID,
    };
    if clocks.is_null() || capacity < 3 {
        return SRH_INVALID;
    }
    for index in 0..3usize {
        let clock = core.clocks[index].get();
        // Safety: `capacity` is at least three records.
        unsafe {
            *clocks.add(index) = SrzClock {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzClock>() as u32,
                hz: clock.hz,
                reserved: 0,
                ticks: clock.ticks,
                phase: clock.phase,
                order: clock.order,
            };
        }
    }
    if !count.is_null() {
        // Safety: the caller supplied a valid output pointer.
        unsafe { *count = 3 };
    }
    SRH_OK
}

#[no_mangle]
pub extern "C" fn srz80_engine_resolve_plugin(
    engine: *const EngineHandle,
    directory: SrzSlice,
    type_: SrzSlice,
    buffer: *mut c_char,
    capacity: u64,
) -> u64 {
    if engine.is_null() {
        return store_text("", buffer, capacity);
    }
    // Safety: `engine` is owned by the caller's thread.
    let handle = unsafe { &*engine };
    let core = match unsafe { handle.core() } {
        Some(core) => core,
        None => return store_text("", buffer, capacity),
    };
    let folder = unsafe {
        if directory.size != 0 {
            crate::core::slice_path(directory)
        } else {
            handle.plugins.clone()
        }
    };
    let type_ = unsafe { crate::core::slice_text(type_) };
    let path = crate::plugin::resolve_plugin(core, &folder, &type_);
    store_text(&crate::core::path_text(&path), buffer, capacity)
}

#[no_mangle]
pub extern "C" fn srz80_engine_discover_plugins(
    engine: *const EngineHandle,
    directory: SrzSlice,
    result: *mut ResultArena,
) -> SrhStatus {
    if engine.is_null() || result.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine`/`result` are owned by the caller's thread.
    let handle = unsafe { &*engine };
    // Discovery only touches the directory, but the ABI requires a created
    // engine with a discovered plugin directory.
    if unsafe { handle.core() }.is_none() {
        return SRH_INVALID;
    }
    let folder = unsafe {
        if directory.size != 0 {
            crate::core::slice_path(directory)
        } else {
            handle.plugins.clone()
        }
    };
    let arena = unsafe { &mut *result };
    guarded(engine as *mut EngineHandle, || {
        arena.paths.clear();
        for path in crate::plugin::discover(&folder) {
            let text = crate::core::path_text(&path);
            let pointer = arena.intern(&text);
            arena.paths.push(pointer);
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_inspect_plugin(
    engine: *const EngineHandle,
    path: SrzSlice,
    result: *mut ResultArena,
) -> SrhStatus {
    if result.is_null() {
        return SRH_INVALID;
    }
    let engine_mutable = engine as *mut EngineHandle;
    // Safety: the caller supplied an arena owned on this thread.
    let arena = unsafe { &mut *result };
    let file = unsafe { crate::core::slice_path(path) };
    if file.as_os_str().is_empty() {
        return fail(
            engine_mutable,
            SRH_INVALID,
            "Plugin path is empty".to_string(),
        );
    }
    guarded(engine_mutable, || {
        let library = match crate::plugin::Library::open(&file) {
            Ok(library) => library,
            Err(message) => return fail(engine_mutable, SRH_NOT_FOUND, message),
        };
        // The descriptor is probed with a bare ABI-versioned host table: the
        // plugin library is not in the rack and must not observe engine state.
        let mut probe = ShouryoHost {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<ShouryoHost>() as u32,
            ..Default::default()
        };
        probe.context = std::ptr::null_mut();
        // Safety: `init` came from this library and `probe` is a live table.
        let api = unsafe { (library.init)(&probe) };
        if !crate::ffi::valid(api, core::mem::size_of::<SrhPlugin>()) {
            return fail(
                engine_mutable,
                SRH_INVALID,
                format!(
                    "Plugin ABI version/size mismatch: {}",
                    crate::core::path_text(&file)
                ),
            );
        }
        if crate::plugin::plugin_id(api).is_empty() {
            return fail(
                engine_mutable,
                SRH_INVALID,
                format!("Plugin reports no id: {}", crate::core::path_text(&file)),
            );
        }

        let path_text = crate::core::path_text(&file);
        let id = crate::plugin::plugin_id(api);
        let mut descriptor = SrzPluginDescriptor {
            abi_version: SRZ80_ENGINE_ABI,
            struct_size: core::mem::size_of::<SrzPluginDescriptor>() as u32,
            path: arena.intern(&path_text),
            id: arena.intern(&id),
            category: arena.intern("Misc"),
            name: arena.intern(&id),
            description: arena.intern(""),
            default_config_json: arena.intern("{}"),
            io_space_config_key: arena.intern(""),
            base_config_key: arena.intern(""),
            default_base: 0,
            default_size: 256,
            default_reset_vector: 0,
            default_priority: 0,
            default_clock: 0,
            flags: 0,
            image_slot_offset: 0,
            image_slot_count: 0,
        };
        // Safety: `api` was validated above.
        let has_descriptor = unsafe {
            has_field(
                api,
                core::mem::offset_of!(SrhPlugin, card_descriptor),
                core::mem::size_of::<*const SrhCardDescriptor>(),
            ) && !(*api).card_descriptor.is_null()
        };
        if has_descriptor {
            // Safety: the optional field is present and non-null.
            let source = unsafe { &*(*api).card_descriptor };
            if source.abi_version != SRH_ABI {
                return fail(
                    engine_mutable,
                    SRH_INVALID,
                    "Invalid card descriptor".to_string(),
                );
            }
            // A descriptor must reach the last field this build reads.  A
            // shorter one is a plugin built against an older layout, so the
            // tail fields are left at their defaults rather than read.
            let required = core::mem::offset_of!(SrhCardDescriptor, base_config_key)
                + core::mem::size_of::<*const c_char>();
            if (source.struct_size as usize) < required {
                return fail(
                    engine_mutable,
                    SRH_INVALID,
                    "Invalid card descriptor".to_string(),
                );
            }
            if !source.category.is_null() && unsafe { *source.category } != 0 {
                let text = unsafe { crate::core::cstr(source.category) };
                descriptor.category = arena.intern(&text);
            }
            if !source.name.is_null() && unsafe { *source.name } != 0 {
                let text = unsafe { crate::core::cstr(source.name) };
                descriptor.name = arena.intern(&text);
            }
            if !source.description.is_null() {
                let text = unsafe { crate::core::cstr(source.description) };
                descriptor.description = arena.intern(&text);
            }
            descriptor.default_base = source.default_base;
            descriptor.default_size = source.default_size;
            descriptor.default_reset_vector = source.default_reset_vector;
            descriptor.default_priority = source.default_priority;
            descriptor.default_clock = source.default_clock;
            descriptor.flags = source.flags;
            if !source.default_config_json.is_null() {
                let text = unsafe { crate::core::cstr(source.default_config_json) };
                descriptor.default_config_json = arena.intern(&text);
            }
            if !source.io_space_config_key.is_null() {
                let text = unsafe { crate::core::cstr(source.io_space_config_key) };
                descriptor.io_space_config_key = arena.intern(&text);
            }
            // Safety: the optional tail field is guarded by the structure size.
            let has_base_key = unsafe {
                has_field(
                    source as *const SrhCardDescriptor,
                    core::mem::offset_of!(SrhCardDescriptor, base_config_key),
                    core::mem::size_of::<*const c_char>(),
                )
            };
            if has_base_key && !source.base_config_key.is_null() {
                let text = unsafe { crate::core::cstr(source.base_config_key) };
                descriptor.base_config_key = arena.intern(&text);
            }
            let has_slots = unsafe {
                has_field(
                    source as *const SrhCardDescriptor,
                    core::mem::offset_of!(SrhCardDescriptor, image_slot_count),
                    core::mem::size_of::<u32>(),
                )
            };
            if has_slots && source.image_slot_count != 0 {
                if source.image_slots.is_null() {
                    return fail(
                        engine_mutable,
                        SRH_INVALID,
                        "Invalid card image-slot metadata".to_string(),
                    );
                }
                descriptor.image_slot_offset = arena.image_slots.len() as u32;
                for index in 0..source.image_slot_count {
                    // Safety: the descriptor declared `image_slot_count` slots.
                    let slot = unsafe { &*source.image_slots.add(index as usize) };
                    if slot.abi_version != SRH_ABI
                        || (slot.struct_size as usize)
                            < core::mem::offset_of!(SrhImageSlotDescriptor, label)
                                + core::mem::size_of::<*const c_char>()
                    {
                        return fail(
                            engine_mutable,
                            SRH_INVALID,
                            "Invalid card image-slot descriptor".to_string(),
                        );
                    }
                    let label = unsafe { crate::plugin::image_slot_label(slot.label, index) };
                    let pointer = arena.intern(&label);
                    arena.image_slots.push(pointer);
                }
                descriptor.image_slot_count = source.image_slot_count;
            }
        }
        if (descriptor.flags & SRH_CARD_REQUIRES_IMAGE) != 0 && descriptor.image_slot_count == 0 {
            descriptor.image_slot_offset = arena.image_slots.len() as u32;
            let pointer = arena.intern("ROM file");
            arena.image_slots.push(pointer);
            descriptor.image_slot_count = 1;
        }
        arena.descriptor = descriptor;
        arena.has_descriptor = true;
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_run_state(engine: *const EngineHandle) -> SrzRunState {
    if engine.is_null() {
        return SRZ_STOPPED;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.run_state.get().as_abi(),
        None => SRZ_STOPPED,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_pause(engine: *mut EngineHandle) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.pause());
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_resume(engine: *mut EngineHandle) -> SrhStatus {
    with_rack(engine, |core| {
        core.resume();
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_stop(engine: *mut EngineHandle) -> SrhStatus {
    with_rack(engine, |core| {
        core.stop();
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_reset(engine: *mut EngineHandle, cold: u32) -> SrhStatus {
    with_rack(engine, |core| {
        core.reset(cold != 0);
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_step(engine: *mut EngineHandle, clock: u32) -> SrhStatus {
    with_rack(engine, |core| {
        if core.step(clock) {
            SRH_OK
        } else {
            SRH_UNAVAILABLE
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_stop_clocks(engine: *mut EngineHandle) -> SrhStatus {
    with_rack(engine, |core| {
        core.stop_clocks();
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_frequency(
    engine: *mut EngineHandle,
    clock: u32,
    hz: u32,
) -> SrhStatus {
    with_rack(engine, |core| {
        core.frequency(clock, hz);
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_run(
    engine: *mut EngineHandle,
    event_budget: u64,
    until: u64,
) -> SrhStatus {
    with_rack(engine, |core| {
        core.run(event_budget, until);
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_run_slice(
    engine: *mut EngineHandle,
    event_budget: u64,
    until: u64,
    wall_budget_ns: u64,
) -> SrhStatus {
    with_rack(engine, |core| {
        core.run_slice(event_budget, until, wall_budget_ns);
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_now(engine: *const EngineHandle) -> u64 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.now.get(),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_time_ns(engine: *const EngineHandle) -> u64 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.time_ns(),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_time_mode(engine: *const EngineHandle) -> SrzTimeMode {
    if engine.is_null() {
        return SRZ_TIME_PROJECT;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => match core.mode.get() {
            TimeMode::Fixed => SRZ_TIME_FIXED,
            TimeMode::System => SRZ_TIME_SYSTEM,
            TimeMode::Project => SRZ_TIME_PROJECT,
        },
        None => SRZ_TIME_PROJECT,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_set_time_mode(
    engine: *mut EngineHandle,
    mode: SrzTimeMode,
    epoch: u64,
) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        let converted = match mode {
            SRZ_TIME_FIXED => TimeMode::Fixed,
            SRZ_TIME_SYSTEM => TimeMode::System,
            _ => TimeMode::Project,
        };
        core.mode.set(converted);
        core.epoch.set(epoch);
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_seed(engine: *mut EngineHandle, value: u64) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        core.rng.set(value);
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_random(engine: *mut EngineHandle, value: *mut u64) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if value.is_null() {
        return fail(engine, SRH_INVALID, "random needs an output".to_string());
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    // Safety: the caller supplied a valid output pointer.
    unsafe { *value = core.random() };
    SRH_OK
}

#[no_mangle]
pub extern "C" fn srz80_engine_stop_reason(
    engine: *const EngineHandle,
    buffer: *mut c_char,
    capacity: u64,
) -> u64 {
    if engine.is_null() {
        return store_text("", buffer, capacity);
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => {
            let reason = core.stop_reason.borrow().clone();
            store_text(&reason, buffer, capacity)
        }
        None => store_text("", buffer, capacity),
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_boundary_required(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.boundary_required() as u32,
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_subscribe(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    clock: u32,
    callback: SrhCallback,
    context: *mut c_void,
    result: *mut SrhHandle,
) -> SrhStatus {
    if result.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Clock subscription needs a callback and an output".to_string(),
        );
    }
    with_rack(engine, |core| {
        core.subscribe(owner, clock, callback, context, result)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_schedule(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    delay: u64,
    callback: SrhCallback,
    context: *mut c_void,
    result: *mut SrhHandle,
) -> SrhStatus {
    if result.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Scheduled event needs a callback and an output".to_string(),
        );
    }
    with_rack(engine, |core| {
        core.schedule(owner, delay, callback, context, result)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_cancel(engine: *mut EngineHandle, event: SrhHandle) -> SrhStatus {
    with_rack(engine, |core| core.cancel(event))
}

#[no_mangle]
pub extern "C" fn srz80_engine_read(
    engine: *mut EngineHandle,
    master: SrhHandle,
    space: SrhHandle,
    address: u64,
    value: *mut u8,
    peek: u32,
) -> SrhStatus {
    if value.is_null() {
        return fail(engine, SRH_INVALID, "Bus read needs an output".to_string());
    }
    with_rack(engine, |core| {
        let mut byte = 0u8;
        let status = core.read(master, space, address, &mut byte, peek != 0);
        if status == SRH_OK {
            // Safety: the caller supplied a valid output pointer.
            unsafe { *value = byte };
        }
        status
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_write(
    engine: *mut EngineHandle,
    master: SrhHandle,
    space: SrhHandle,
    address: u64,
    value: u8,
) -> SrhStatus {
    with_rack(engine, |core| core.write(master, space, address, value))
}

#[no_mangle]
pub extern "C" fn srz80_engine_read_word(
    engine: *mut EngineHandle,
    master: SrhHandle,
    space: SrhHandle,
    address: u64,
    value: *mut u32,
) -> SrhStatus {
    if value.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Bus word read needs an output".to_string(),
        );
    }
    with_rack(engine, |core| {
        let mut word = 0u32;
        let status = core.read_word(master, space, address, &mut word);
        if status == SRH_OK {
            // Safety: the caller supplied a valid output pointer.
            unsafe { *value = word };
        }
        status
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_add_breakpoint(
    engine: *mut EngineHandle,
    card: SrhHandle,
    space: SrhHandle,
    first: u64,
    last: u64,
    operations: u32,
) -> u64 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return 0,
    };
    guarded_value(engine, 0, || {
        match core.add_breakpoint(card, space, first, last, operations) {
            Ok(id) => id,
            Err(message) => {
                fail(engine, SRH_ERROR, message);
                0
            }
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_remove_breakpoint(engine: *mut EngineHandle, id: u64) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    guarded(engine, || {
        let before = core.breakpoints.borrow().len();
        core.breakpoints.borrow_mut().retain(|point| point.id != id);
        if core.breakpoints.borrow().len() == before {
            return fail(
                engine,
                SRH_NOT_FOUND,
                "Breakpoint is not registered".to_string(),
            );
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_set_breakpoint_enabled(
    engine: *mut EngineHandle,
    id: u64,
    enabled: u32,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    guarded(engine, || {
        let mut breakpoints = core.breakpoints.borrow_mut();
        let Some(point) = breakpoints.iter_mut().find(|point| point.id == id) else {
            return fail(
                engine,
                SRH_NOT_FOUND,
                "Breakpoint is not registered".to_string(),
            );
        };
        point.enabled = enabled != 0;
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_breakpoints(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.breakpoints.clear();
        for point in core.breakpoints.borrow().iter() {
            arena.breakpoints.push(SrzBreakpoint {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzBreakpoint>() as u32,
                id: point.id,
                card: point.card,
                space: point.space,
                first: point.first,
                last: point.last,
                operations: point.operations,
                enabled: point.enabled as u32,
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_boundary(
    engine: *mut EngineHandle,
    card: SrhHandle,
    space: SrhHandle,
    pc: u64,
) -> SrhStatus {
    with_rack(engine, |core| core.boundary(card, space, pc, 0, 0))
}

#[no_mangle]
pub extern "C" fn srz80_engine_boundary_ex(
    engine: *mut EngineHandle,
    card: SrhHandle,
    space: SrhHandle,
    pc: u64,
    instruction_bytes: u32,
    cycles: u32,
) -> SrhStatus {
    with_rack(engine, |core| {
        core.boundary(card, space, pc, instruction_bytes, cycles)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_request_stop(
    engine: *mut EngineHandle,
    card: SrhHandle,
    reason: SrzSlice,
) -> SrhStatus {
    // Safety: the slice is borrowed for this call only.
    let reason = unsafe { crate::core::slice_text(reason) };
    with_rack(engine, |core| core.request_stop(card, &reason))
}

#[no_mangle]
pub extern "C" fn srz80_engine_disassembly_availability(
    engine: *mut EngineHandle,
    card: SrhHandle,
    out: *mut SrzDisassemblyAvailability,
) -> SrhStatus {
    if out.is_null() {
        return SRH_INVALID;
    }
    with_rack(engine, |core| {
        let cards = core.cards.borrow();
        let Some(entry) = cards.get(&card) else {
            return SRH_NOT_FOUND;
        };
        let mut record = SrzDisassemblyAvailability {
            abi_version: SRZ80_ENGINE_ABI,
            struct_size: core::mem::size_of::<SrzDisassemblyAvailability>() as u32,
            state: if !entry.is_active() {
                0
            } else if !entry.disasm_enabled.get() {
                2
            } else if entry.disasm.get().is_some() {
                1
            } else {
                0
            },
            reserved: 0,
            revision: entry.disasm_revision.get(),
            message: [0; 256],
        };
        for (dst, src) in record
            .message
            .iter_mut()
            .take(255)
            .zip(entry.disasm_message.borrow().as_bytes())
        {
            *dst = *src as c_char;
        }
        unsafe {
            *out = record;
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_disassemble(
    engine: *mut EngineHandle,
    card: SrhHandle,
    space: SrhHandle,
    pc: u64,
    out: *mut SrzDisassembly,
) -> SrhStatus {
    if out.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Disassembly needs an output".to_string(),
        );
    }
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => {
            return fail(
                engine,
                SRH_UNAVAILABLE,
                "Engine has no loaded rack".to_string(),
            )
        }
    };
    guarded(engine, || {
        if core
            .cards
            .borrow()
            .get(&card)
            .is_some_and(|entry| !entry.disasm_enabled.get())
        {
            unsafe {
                *out = SrzDisassembly::default();
            }
            return SRH_UNAVAILABLE;
        }
        let revision = core
            .cards
            .borrow()
            .get(&card)
            .map(|entry| entry.disasm_revision.get());
        let disassembly = core.disassemble(card, space, pc);
        if core
            .cards
            .borrow()
            .get(&card)
            .map(|entry| entry.disasm_revision.get())
            != revision
        {
            unsafe {
                *out = SrzDisassembly::default();
            }
            return SRH_UNAVAILABLE;
        }
        let mut record = SrzDisassembly {
            abi_version: SRZ80_ENGINE_ABI,
            struct_size: core::mem::size_of::<SrzDisassembly>() as u32,
            ok: disassembly.ok as u32,
            instruction_bytes: disassembly.instruction_bytes,
            cycles: disassembly.cycles,
            byte_count: disassembly.bytes.len().min(32) as u32,
            bytes: [0; 32],
            text: [0; 256],
        };
        record.bytes[..record.byte_count as usize]
            .copy_from_slice(&disassembly.bytes[..record.byte_count as usize]);
        let text = disassembly.text.as_bytes();
        let copied = text.len().min(255);
        for (index, byte) in text[..copied].iter().enumerate() {
            record.text[index] = *byte as c_char;
        }
        // Safety: the caller supplied a valid output pointer.
        unsafe { *out = record };
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_trace(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.trace.clear();
        arena.handles.clear();
        for record in core.trace_records() {
            let entry = fill_trace(&record, arena);
            arena.trace.push(entry);
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_clear_trace(engine: *mut EngineHandle) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || {
            core.trace.borrow_mut().clear();
            core.dropped.set(0);
        });
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_set_trace_capture(
    engine: *mut EngineHandle,
    enabled: u32,
    operations: u32,
) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || {
            core.trace_capture.set(enabled != 0);
            core.trace_capture_operations
                .set(operations & (SRH_READ | SRH_WRITE));
        });
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_trace_capture(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.trace_capture.get() as u32,
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_dropped(engine: *const EngineHandle) -> u64 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.dropped.get(),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_signal(
    engine: *mut EngineHandle,
    name: SrzSlice,
    idle: i32,
) -> SrhHandle {
    if engine.is_null() {
        return 0;
    }
    // Safety: the slice is borrowed for this call only.
    let name = unsafe { crate::core::slice_text(name) };
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return 0,
    };
    guarded_value(engine, 0, || core.try_signal(name, idle).unwrap_or(0))
}

#[no_mangle]
pub extern "C" fn srz80_engine_find_signal(
    engine: *const EngineHandle,
    name: SrzSlice,
) -> SrhHandle {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return 0,
    };
    let name = unsafe { crate::core::slice_text(name) };
    guarded_value(engine as *mut EngineHandle, 0, || core.find_signal(&name))
}

#[no_mangle]
pub extern "C" fn srz80_engine_drive_signal(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    signal: SrhHandle,
    value: i32,
    priority: i32,
) -> SrhStatus {
    with_rack(engine, |core| core.drive(owner, signal, value, priority))
}

#[no_mangle]
pub extern "C" fn srz80_engine_release_signal(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    signal: SrhHandle,
) -> SrhStatus {
    with_rack(engine, |core| core.release_signal(owner, signal))
}

#[no_mangle]
pub extern "C" fn srz80_engine_sample_signal(
    engine: *const EngineHandle,
    signal: SrhHandle,
    value: *mut i32,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_UNAVAILABLE;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    if value.is_null() {
        return SRH_INVALID;
    }
    match core.sample(signal) {
        Ok(sampled) => {
            // Safety: the caller supplied a valid output pointer.
            unsafe { *value = sampled };
            SRH_OK
        }
        Err(status) => status,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_subscribe_signal(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    signal: SrhHandle,
    callback: SrhSignalCallback,
    context: *mut c_void,
    result: *mut SrhHandle,
) -> SrhStatus {
    if result.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Signal subscription needs a callback and an output".to_string(),
        );
    }
    with_rack(engine, |core| {
        core.signal_subscribe(owner, signal, callback, context, result)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_register_input(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    name: SrzSlice,
    capacity: u32,
    endpoint: *mut SrhHandle,
) -> SrhStatus {
    if endpoint.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Input registration needs an output".to_string(),
        );
    }
    // Safety: the slice is borrowed for this call only.
    let name = unsafe { crate::core::slice_text(name) };
    with_rack(engine, |core| {
        core.register_input(owner, &name, capacity, endpoint)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_subscribe_input_due(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    endpoint: SrhHandle,
    callback: SrhCallback,
    context: *mut c_void,
    result: *mut SrhHandle,
) -> SrhStatus {
    if result.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Input subscription needs a callback and an output".to_string(),
        );
    }
    with_rack(engine, |core| {
        core.subscribe_input_due(owner, endpoint, callback, context, result)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_enqueue_input(
    engine: *mut EngineHandle,
    endpoint: SrzSlice,
    timestamp: u64,
    value: u8,
) -> SrhStatus {
    // Safety: the slice is borrowed for this call only.
    let endpoint = unsafe { crate::core::slice_text(endpoint) };
    with_rack(engine, |core| {
        match core.enqueue_input(&endpoint, timestamp, value) {
            Ok(()) => SRH_OK,
            Err(message) => fail(engine, SRH_ERROR, message),
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_enqueue_input_batch(
    engine: *mut EngineHandle,
    endpoint: SrzSlice,
    timestamp: u64,
    bytes: *const u8,
    size: u64,
    source: u64,
    expected_owner: SrhHandle,
) -> SrhStatus {
    if bytes.is_null() && size != 0 {
        return fail(
            engine,
            SRH_INVALID,
            "Input batch bytes are missing".to_string(),
        );
    }
    // Safety: the slice is borrowed for this call only.
    let endpoint = unsafe { crate::core::slice_text(endpoint) };
    // Safety: the caller declared `size` bytes.
    let payload = if size == 0 {
        Vec::new()
    } else {
        unsafe { core::slice::from_raw_parts(bytes, size as usize).to_vec() }
    };
    with_rack(engine, |core| {
        core.enqueue_input_batch(&endpoint, timestamp, &payload, source, expected_owner)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_cancel_input_source(engine: *mut EngineHandle, source: u64) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.cancel_input_source(source));
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_input_due(
    engine: *const EngineHandle,
    endpoint: SrhHandle,
    count: *mut u64,
) -> SrhStatus {
    if engine.is_null() || count.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.input_due(endpoint, count),
        None => SRH_INVALID,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_input_pop(
    engine: *mut EngineHandle,
    endpoint: SrhHandle,
    timestamp: *mut u64,
    value: *mut u8,
) -> SrhStatus {
    if timestamp.is_null() || value.is_null() {
        return fail(engine, SRH_INVALID, "Input pop needs outputs".to_string());
    }
    with_rack(engine, |core| core.input_pop(endpoint, timestamp, value))
}

#[no_mangle]
pub extern "C" fn srz80_engine_input_endpoints(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.endpoints.clear();
        for name in core.input_endpoints() {
            let pointer = arena.intern(&name);
            arena.endpoints.push(pointer);
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_set_input_records(
    engine: *mut EngineHandle,
    records: *const SrzInputRecord,
    count: u32,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if records.is_null() && count != 0 {
        return fail(engine, SRH_INVALID, "Input records are missing".to_string());
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    guarded(engine, || {
        let mut converted = Vec::with_capacity(count as usize);
        for index in 0..count {
            // Safety: the caller declared `count` records.
            let record = unsafe { &*records.add(index as usize) };
            if record.abi_version != SRZ80_ENGINE_ABI
                || (record.struct_size as usize) < core::mem::size_of::<SrzInputRecord>()
            {
                return fail(engine, SRH_INVALID, "Invalid input record ABI".to_string());
            }
            let endpoint = unsafe { crate::core::cstr(record.endpoint) };
            converted.push((record.timestamp, endpoint, vec![record.value]));
        }
        match core.set_input_records(converted) {
            Ok(()) => SRH_OK,
            Err(message) => fail(engine, SRH_ERROR, message),
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_input_records(
    engine: *const EngineHandle,
    persistent_only: u32,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.input_records.clear();
        for (timestamp, endpoint, value) in core.input_records(persistent_only != 0) {
            let pointer = arena.intern(&endpoint);
            arena.input_records.push(SrzInputRecord {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzInputRecord>() as u32,
                timestamp,
                endpoint: pointer,
                value,
                reserved: [0; 7],
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_text_query(
    engine: *const EngineHandle,
    card: SrhHandle,
    offset: u64,
    buffer: *mut u8,
    size: *mut u32,
    total: *mut u32,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_UNAVAILABLE;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    if size.is_null() || total.is_null() || (buffer.is_null() && unsafe { *size } != 0) {
        return SRH_INVALID;
    }
    let capacity = unsafe { *size };
    guarded(engine as *mut EngineHandle, || {
        let (chunk, endpoint_total) = match core.text_query(card, offset, capacity as usize) {
            Ok(value) => value,
            Err(status) => return status,
        };
        let copied = (chunk.len() as u32).min(capacity);
        if copied != 0 && !buffer.is_null() {
            // Safety: the caller supplied a buffer of `capacity` bytes.
            unsafe {
                core::ptr::copy_nonoverlapping(chunk.as_ptr(), buffer, copied as usize);
            }
        }
        // Safety: the caller supplied valid output pointers.
        unsafe {
            *size = copied;
            *total = endpoint_total;
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_text_endpoints(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.text_endpoints.clear();
        for endpoint in core.text_endpoints() {
            let input_offset = arena.input_names.len() as u32;
            let input_count = endpoint.inputs.len() as u32;
            for name in &endpoint.inputs {
                let pointer = arena.intern(name);
                arena.input_names.push(pointer);
            }
            arena.text_endpoints.push(SrzTextEndpoint {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzTextEndpoint>() as u32,
                card: endpoint.card,
                input_offset,
                input_count,
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_video_surfaces(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.video_surfaces.clear();
        for surface in core.video_surfaces() {
            arena.video_surfaces.push(SrzVideoSurface {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzVideoSurface>() as u32,
                id: surface.id,
                owner: surface.owner,
                width: surface.width,
                height: surface.height,
                format: surface.format,
                flags: surface.flags,
                reserved: 0,
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_video_timing(
    engine: *mut EngineHandle,
    surface: SrhHandle,
    timing: *mut SrhVideoTiming,
) -> SrhStatus {
    with_rack(engine, |core| core.video_timing(surface, timing))
}

#[no_mangle]
pub extern "C" fn srz80_engine_video_read(
    engine: *mut EngineHandle,
    surface: SrhHandle,
    offset: u64,
    buffer: *mut u8,
    size: *mut u32,
    total: *mut u32,
) -> SrhStatus {
    if size.is_null() || total.is_null() || (buffer.is_null() && unsafe { *size } != 0) {
        return fail(
            engine,
            SRH_INVALID,
            "Video read needs buffer metadata".to_string(),
        );
    }
    with_rack(engine, |core| {
        core.video_read(surface, offset, buffer, size, total)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_sample_rate(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_sample_rate(),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_sample_rate(
    engine: *mut EngineHandle,
    sample_rate: u32,
) -> SrhStatus {
    if engine.is_null() || !(8_000..=384_000).contains(&sample_rate) {
        return SRH_INVALID;
    }
    // Safety: the caller owns the engine on this thread.
    let handle = unsafe { &mut *engine };
    guarded(engine, || {
        handle.audio_sample_rate = sample_rate;
        // Candidates do not own a core until a project is loaded. Their stored
        // rate is applied while constructing that rack, before plugins run.
        let Some(core) = (unsafe { handle.core.as_mut() }) else {
            return SRH_OK;
        };
        if core.audio_set_sample_rate(sample_rate) {
            SRH_OK
        } else {
            SRH_INVALID
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_resampling(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: the caller owns the engine on this thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_resampling(),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_resampling(
    engine: *mut EngineHandle,
    method: u32,
) -> SrhStatus {
    if engine.is_null() || method > 4 {
        return SRH_INVALID;
    }
    // Safety: the caller owns the engine on this thread.
    let Some(core) = (unsafe { (*engine).core() }) else {
        return SRH_INVALID;
    };
    guarded(engine, || {
        if core.audio_set_resampling(method) {
            SRH_OK
        } else {
            SRH_INVALID
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_channels(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_channels(),
        None => 0,
    }
}

#[allow(clippy::too_many_arguments)]
#[no_mangle]
pub extern "C" fn srz80_engine_audio_register_source(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    sample_rate: u32,
    channels: u32,
    format: SrhAudioFormat,
    name: SrzSlice,
    render: SrhAudioRender,
    render_context: *mut c_void,
    source: *mut SrhHandle,
) -> SrhStatus {
    if source.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Audio registration needs a render callback and an output".to_string(),
        );
    }
    // Safety: the slice is borrowed for this call only.
    let name = unsafe { crate::core::slice_text(name) };
    with_rack(engine, |core| {
        core.audio_register_source(
            owner,
            sample_rate,
            channels,
            format,
            &name,
            render,
            render_context,
            source,
        )
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_read(
    engine: *mut EngineHandle,
    interleaved: *mut i16,
    frames: u32,
) -> u32 {
    if engine.is_null() || interleaved.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_read(interleaved, frames),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_diagnostics(
    engine: *const EngineHandle,
    out: *mut SrzAudioDiagnostics,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_UNAVAILABLE;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    if out.is_null() {
        return SRH_INVALID;
    }
    let diagnostics = core.audio_diagnostics();
    // Safety: the caller supplied a valid output pointer.
    unsafe {
        *out = SrzAudioDiagnostics {
            abi_version: SRZ80_ENGINE_ABI,
            struct_size: core::mem::size_of::<SrzAudioDiagnostics>() as u32,
            queued_frames: diagnostics.queued_frames,
            queue_capacity_frames: diagnostics.queue_capacity_frames,
            dropped_frames: diagnostics.dropped_frames,
            underflow_frames: diagnostics.underflow_frames,
            source_errors: diagnostics.source_errors,
        };
    }
    SRH_OK
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_queue_capacity(engine: *mut EngineHandle, frames: u64) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.audio_set_queue_capacity(frames));
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_sources(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.audio_sources.clear();
        for source in core.audio_sources() {
            let name = arena.intern(&source.name);
            arena.audio_sources.push(SrzAudioSource {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzAudioSource>() as u32,
                id: source.id,
                owner: source.owner,
                name,
                volume_percent: source.volume_percent,
                muted: source.muted as u32,
                active: source.active as u32,
                level_peak: source.level_peak,
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_source_volume(
    engine: *mut EngineHandle,
    source: SrhHandle,
    percent: u32,
) -> SrhStatus {
    with_rack(engine, |core| {
        if core.audio_set_source_volume(source, percent) {
            SRH_OK
        } else {
            SRH_NOT_FOUND
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_source_pan(engine: *const EngineHandle, source: SrhHandle) -> i32 {
    if engine.is_null() { return 0; }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_source_pan(source),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_source_pan(
    engine: *mut EngineHandle,
    source: SrhHandle,
    pan: i32,
) -> SrhStatus {
    with_rack(engine, |core| {
        if core.audio_set_source_pan(source, pan) { SRH_OK } else { SRH_NOT_FOUND }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_source_muted(
    engine: *mut EngineHandle,
    source: SrhHandle,
    muted: u32,
) -> SrhStatus {
    with_rack(engine, |core| {
        if core.audio_set_source_muted(source, muted != 0) {
            SRH_OK
        } else {
            SRH_NOT_FOUND
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_master_volume(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_master_volume(),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_master_levels(
    engine: *const EngineHandle, left: *mut u32, right: *mut u32,
) -> SrhStatus {
    if engine.is_null() || left.is_null() || right.is_null() {
        return SRH_INVALID;
    }
    // Safety: the caller owns the engine thread and supplied writable outputs.
    let Some(core) = (unsafe { (*engine).core() }) else { return SRH_UNAVAILABLE; };
    let levels = core.audio_master_levels();
    unsafe { *left = levels[0]; *right = levels[1]; }
    SRH_OK
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_master_volume(engine: *mut EngineHandle, percent: u32) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.audio_set_master_volume(percent));
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_dc_offset_correction(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_dc_offset_correction() as u32,
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_dc_offset_correction(
    engine: *mut EngineHandle,
    enabled: u32,
) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.audio_set_dc_offset_correction(enabled != 0));
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_software_clipping(engine: *const EngineHandle) -> u32 {
    if engine.is_null() {
        return 0;
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => core.audio_software_clipping() as u32,
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_set_software_clipping(
    engine: *mut EngineHandle,
    enabled: u32,
) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.audio_set_software_clipping(enabled != 0));
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_properties(
    engine: *mut EngineHandle,
    card: SrhHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    if result.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Property query needs a result".to_string(),
        );
    }
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine`/`result` are owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    let arena = unsafe { &mut *result };
    guarded(engine, || {
        arena.properties.clear();
        for property in core.properties(card) {
            let name = arena.intern(&property.name);
            let group = arena.intern(&property.group);
            let description = arena.intern(&property.description);
            let enum_labels = arena.intern(&property.enum_labels);
            arena.properties.push(SrzProperty {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzProperty>() as u32,
                name,
                group,
                description,
                enum_labels,
                kind: property.kind,
                bits: property.bits,
                base: property.base,
                ui_flags: property.ui_flags,
                editable: property.editable as u32,
                value: property.value,
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_edit_property(
    engine: *mut EngineHandle,
    card: SrhHandle,
    index: u32,
    value: *const SrhValue,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if value.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Invalid property value ABI".to_string(),
        );
    }
    // Safety: the caller supplied a value record; its header is validated by
    // the core before any field is used.
    let value_ref = unsafe { &*value };
    if !valid(value, core::mem::size_of::<SrhValue>()) {
        return fail(
            engine,
            SRH_INVALID,
            "Invalid property value ABI".to_string(),
        );
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    guarded(engine, || core.edit_property(card, index, value_ref))
}

#[no_mangle]
pub extern "C" fn srz80_engine_load_config(engine: *mut EngineHandle) -> SrhStatus {
    with_rack(engine, |core| {
        let path = core.config_path.borrow().clone();
        core.config.borrow_mut().load(&path);
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_save_config(engine: *const EngineHandle) -> SrhStatus {
    if engine.is_null() {
        return SRH_UNAVAILABLE;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    guarded(engine as *mut EngineHandle, || {
        let path = core.config_path.borrow().clone();
        core.config.borrow().save(&path);
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_config_path(
    engine: *const EngineHandle,
    buffer: *mut c_char,
    capacity: u64,
) -> u64 {
    if engine.is_null() {
        return store_text("", buffer, capacity);
    }
    // Safety: `engine` is owned by the caller's thread.
    match unsafe { (*engine).core() } {
        Some(core) => {
            let text = crate::core::path_text(&core.config_path.borrow());
            store_text(&text, buffer, capacity)
        }
        None => store_text("", buffer, capacity),
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_config_value(
    engine: *const EngineHandle,
    key: SrzSlice,
    fallback: SrzSlice,
    buffer: *mut c_char,
    capacity: u64,
) -> u64 {
    if engine.is_null() {
        return store_text("", buffer, capacity);
    }
    // Safety: `engine` is owned by the caller's thread; the slices are borrowed
    // for this call only.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return store_text("", buffer, capacity),
    };
    let key = unsafe { crate::core::slice_text(key) };
    let fallback = unsafe { crate::core::slice_text(fallback) };
    guarded_value(engine as *mut EngineHandle, 0, || {
        let value = core.config.borrow().value(&key, &fallback);
        store_text(&value, buffer, capacity)
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_config_set(
    engine: *mut EngineHandle,
    key: SrzSlice,
    value: SrzSlice,
) -> SrhStatus {
    // Safety: the slices are borrowed for this call only.
    let key = unsafe { crate::core::slice_text(key) };
    let value = unsafe { crate::core::slice_text(value) };
    with_rack(engine, |core| {
        core.config.borrow_mut().set(&key, &value);
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_config_entries(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.config_entries.clear();
        for entry in core.config_entries.borrow().iter() {
            let category = arena.intern(&entry.category);
            let name = arena.intern(&entry.name);
            let label = arena.intern(&entry.label);
            let description = arena.intern(&entry.description);
            let enum_labels = arena.intern(&entry.enum_labels);
            let default_value = arena.intern(&entry.default_value);
            let provider = arena.intern(&entry.provider);
            arena.config_entries.push(SrzConfigEntry {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzConfigEntry>() as u32,
                category,
                name,
                label,
                description,
                enum_labels,
                default_value,
                provider,
                type_: entry.type_,
                owner: entry.owner,
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_config_entry_get(
    engine: *const EngineHandle,
    key: SrzSlice,
    buffer: *mut c_char,
    capacity: u64,
    size: *mut u64,
) -> SrhStatus {
    if engine.is_null() || size.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_INVALID,
    };
    let key = unsafe { crate::core::slice_text(key) };
    guarded(engine as *mut EngineHandle, || {
        match core.config_entry_get(&key) {
            Ok(value) => {
                // Safety: the caller supplied a valid size output.
                unsafe { *size = value.len() as u64 };
                store_text(&value, buffer, capacity);
                SRH_OK
            }
            Err(status) => {
                unsafe { *size = 0 };
                status
            }
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_config_entry_set(
    engine: *mut EngineHandle,
    key: SrzSlice,
    value: SrzSlice,
) -> SrhStatus {
    // Safety: the slices are borrowed for this call only.
    let key = unsafe { crate::core::slice_text(key) };
    let value = unsafe { crate::core::slice_text(value) };
    with_rack(engine, |core| core.config_entry_set(&key, &value))
}

#[no_mangle]
pub extern "C" fn srz80_engine_register_config_entry(
    engine: *mut EngineHandle,
    entry: *const SrhConfigEntry,
) -> SrhStatus {
    if engine.is_null() {
        return SRH_INVALID;
    }
    if !valid(entry, core::mem::size_of::<SrhConfigEntry>()) {
        return fail(
            engine,
            SRH_INVALID,
            "Invalid configuration entry ABI".to_string(),
        );
    }
    // Safety: `engine` is owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    // Safety: the entry header was validated above.
    let entry_ref = unsafe { &*entry };
    guarded(engine, || {
        if core.register_config_entry(entry_ref) {
            SRH_OK
        } else {
            SRH_INVALID
        }
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_unregister_config_entries(
    engine: *mut EngineHandle,
    context: *mut c_void,
) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.unregister_config_entries(context));
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_log(engine: *mut EngineHandle, message: SrzSlice) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread; the slice is borrowed
    // for this call only.
    if let Some(core) = unsafe { (*engine).core() } {
        let message = unsafe { crate::core::slice_text(message) };
        guarded_void(engine, || core.log(message));
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_clear_logs(engine: *mut EngineHandle) {
    if engine.is_null() {
        return;
    }
    // Safety: `engine` is owned by the caller's thread.
    if let Some(core) = unsafe { (*engine).core() } {
        guarded_void(engine, || core.logs.borrow_mut().clear());
    }
}

#[no_mangle]
pub extern "C" fn srz80_engine_logs(
    engine: *const EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    with_arena(engine, result, |core, arena| {
        arena.logs.clear();
        for message in core.logs.borrow().iter() {
            let pointer = arena.intern(message);
            arena.logs.push(pointer);
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_provider_data(
    engine: *mut EngineHandle,
    result: *mut ResultArena,
) -> SrhStatus {
    if result.is_null() {
        return fail(
            engine,
            SRH_INVALID,
            "Provider query needs a result".to_string(),
        );
    }
    if engine.is_null() {
        return SRH_INVALID;
    }
    // Safety: `engine`/`result` are owned by the caller's thread.
    let core = match unsafe { (*engine).core() } {
        Some(core) => core,
        None => return SRH_UNAVAILABLE,
    };
    let arena = unsafe { &mut *result };
    guarded(engine, || {
        arena.providers.clear();
        for (owner, data) in core.provider_data() {
            let name = arena.intern(&data.name);
            let protocol = arena.intern(&data.protocol);
            let payload = arena.intern(&data.data);
            arena.providers.push(SrzProviderData {
                abi_version: SRZ80_ENGINE_ABI,
                struct_size: core::mem::size_of::<SrzProviderData>() as u32,
                owner,
                name,
                protocol,
                data: payload,
                data_size: data.data.len() as u64,
                flags: data.flags,
                reserved: 0,
            });
        }
        SRH_OK
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_provider_command(
    engine: *mut EngineHandle,
    owner: SrhHandle,
    kind: u32,
    revision: u64,
    payload: SrzSlice,
) -> SrhStatus {
    // Safety: the slice is borrowed for this call only.
    let payload = unsafe { crate::core::slice_text(payload) };
    with_rack(engine, |core| {
        core.provider_command(owner, kind, revision, &payload)
    })
}

/// Frontend capture bridge. Must be called on the engine owning thread.
#[no_mangle]
pub extern "C" fn srz80_engine_audio_input_requested(engine: *mut EngineHandle) -> u32 {
    crate::guard::guarded_value(engine, 0, || {
    if engine.is_null() { return 0; }
    let Some(core) = (unsafe { (*engine).core() }) else { return 0; };
    let mut input = core.audio_input.borrow_mut();
    input.queues.retain(|owner, _| core.alive(*owner));
    (!input.queues.is_empty()) as u32
    })
}

#[no_mangle]
pub extern "C" fn srz80_engine_audio_input_push(engine: *mut EngineHandle,
    samples: *const f32, frames: u32, rate: u32, channels: u32) {
    guarded_void(engine, || {
    if engine.is_null() || frames > 8192 || channels > 8 ||
        (frames != 0 && (samples.is_null() || channels == 0)) { return; }
    let Some(core) = (unsafe { (*engine).core() }) else { return; };
    let valid = (8000..=384000).contains(&rate) && channels != 0;
    let (rate, channels) = if valid { (rate, channels) } else { (0, 0) };
    let mut input = core.audio_input.borrow_mut();
    if input.rate != rate || input.channels != channels || !valid {
        for queue in input.queues.values_mut() { queue.clear(); }
    }
    input.rate = rate;
    input.channels = channels;
    if !valid || frames == 0 { return; }
    let data = unsafe { std::slice::from_raw_parts(samples, frames as usize * channels as usize) };
    for subscription in input.queues.values_mut() {
        subscription.push(data, rate, channels as usize);
    }
    });
}
