//! Engine and card ABI declarations.
//!
//! Every structure here mirrors a C declaration in `<srz80/engine.h>`,
//! `<srz80/abi.h>`, `<srz80/providers.h>` or `<srz80/signals.h>`.  The layouts
//! are `#[repr(C)]` and the crate's tests assert the sizes and the offsets of
//! every tail field that the ABI guards with `struct_size`, so an accidental
//! field reorder fails the build instead of corrupting a caller.
// The `Srh_`-prefixed configuration constants keep their C header spelling so a
// reader can diff this file against `abi.h` directly.
#![allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code
)]

use core::ffi::{c_char, c_int, c_void};

pub const SRH_ABI: u32 = 1;
pub const SRZ80_ENGINE_ABI: u32 = 3;

pub const SRH_OK: SrhStatus = 0;
pub const SRH_ERROR: SrhStatus = 1;
pub const SRH_INVALID: SrhStatus = 2;
pub const SRH_NOT_FOUND: SrhStatus = 3;
pub const SRH_UNAVAILABLE: SrhStatus = 4;
pub const SRH_STOP: SrhStatus = 5;
pub const SRH_CONFLICT: SrhStatus = 6;

pub const SRH_READ: u32 = 1;
pub const SRH_WRITE: u32 = 2;
pub const SRH_PEEK: u32 = 4;

pub const SRH_UNSIGNED: u32 = 0;
pub const SRH_SIGNED: u32 = 1;
pub const SRH_BOOLEAN: u32 = 2;
pub const SRH_FIXED: u32 = 3;
pub const SRH_ENUM: u32 = 4;
pub const SRH_TEXT: u32 = 5;

pub const SRH_PROPERTY_HIDE_UI: u32 = 1;
pub const SRH_PROPERTY_LIVE_EDIT: u32 = 2;
pub const SRH_PROPERTY_RUNTIME: u32 = 4;
pub const SRH_PROPERTY_PERSISTENT: u32 = 8;
pub const SRH_PROPERTY_UNAVAILABLE: u32 = 16;

pub const SRH_CARD_REQUIRES_IMAGE: u32 = 1;
pub const SRH_CARD_REQUIRES_IO_SPACE: u32 = 2;
pub const SRH_CARD_SHOW_CLOCK: u32 = 4;

pub const SRH_VIDEO_RGBA8: u32 = 1;
pub const SRH_VIDEO_ALLOW_SHADER: u32 = 1;
pub const SRH_AUDIO_S16_STEREO: u32 = 1;

pub const Srh_CONFIG_BOOL: u32 = 0;
pub const Srh_CONFIG_INT: u32 = 1;
pub const Srh_CONFIG_FLOAT: u32 = 2;
pub const Srh_CONFIG_STRING: u32 = 3;
pub const Srh_CONFIG_ENUM: u32 = 4;
pub const Srh_CONFIG_PATH: u32 = 5;

pub const SRH_PROVIDER_MAX_BYTES: u64 = 1024 * 1024;
pub const SRH_PROVIDER_LIVE_CONFIG: u32 = 1;
pub const SRH_PROVIDER_INTERACTION: u32 = 0;
pub const SRH_PROVIDER_CONFIGURE: u32 = 1;

pub const SRZ_CAP_PROJECT: u32 = 1 << 0;
pub const SRZ_CAP_PROJECT_CANDIDATE: u32 = 1 << 1;
pub const SRZ_CAP_STATE: u32 = 1 << 2;
pub const SRZ_CAP_TRACE: u32 = 1 << 3;
pub const SRZ_CAP_DEBUGGER: u32 = 1 << 4;
pub const SRZ_CAP_SIGNALS: u32 = 1 << 5;
pub const SRZ_CAP_INPUT: u32 = 1 << 6;
pub const SRZ_CAP_TEXT: u32 = 1 << 7;
pub const SRZ_CAP_VIDEO: u32 = 1 << 8;
pub const SRZ_CAP_AUDIO: u32 = 1 << 9;
pub const SRZ_CAP_PROVIDERS: u32 = 1 << 10;
pub const SRZ_CAP_CONFIG: u32 = 1 << 11;
pub const SRZ_CAP_PLUGIN_DISCOVERY: u32 = 1 << 12;
pub const SRZ_CAP_PLUGIN_DATA: u32 = 1 << 13;
pub const SRZ_CAP_ALL: u32 = SRZ_CAP_PROJECT
    | SRZ_CAP_PROJECT_CANDIDATE
    | SRZ_CAP_STATE
    | SRZ_CAP_TRACE
    | SRZ_CAP_DEBUGGER
    | SRZ_CAP_SIGNALS
    | SRZ_CAP_INPUT
    | SRZ_CAP_TEXT
    | SRZ_CAP_VIDEO
    | SRZ_CAP_AUDIO
    | SRZ_CAP_PROVIDERS
    | SRZ_CAP_CONFIG
    | SRZ_CAP_PLUGIN_DISCOVERY
    | SRZ_CAP_PLUGIN_DATA;

pub const SRZ_STOPPED: u32 = 0;
pub const SRZ_PAUSED: u32 = 1;
pub const SRZ_RUNNING: u32 = 2;

pub const SRZ_TIME_PROJECT: u32 = 0;
pub const SRZ_TIME_FIXED: u32 = 1;
pub const SRZ_TIME_SYSTEM: u32 = 2;

pub const SRZ_RESOLVER_PRIORITY: u32 = 0;
pub const SRZ_RESOLVER_BIT_OR: u32 = 1;

pub type SrhHandle = u64;
pub type SrhStatus = i32;

pub type SrhAudioFormat = u32;
pub type SrhVideoFormat = u32;
pub type SrzRunState = u32;
pub type SrzTimeMode = u32;
pub type SrzResolver = u32;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SrzSlice {
    pub data: *const c_char,
    pub size: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SrhValue {
    pub abi_version: u32,
    pub struct_size: u32,
    pub unsigned_value: u64,
    pub signed_value: i64,
    pub text: [c_char; 128],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SrhImagePart {
    pub abi_version: u32,
    pub struct_size: u32,
    pub data: *const u8,
    pub size: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SrhMapping {
    pub abi_version: u32,
    pub struct_size: u32,
    pub space: SrhHandle,
    pub first: u64,
    pub last: u64,
    pub priority: i32,
    pub context: *mut c_void,
    pub read: Option<SrhRead>,
    pub write: Option<SrhWrite>,
    pub peek: Option<SrhRead>,
    pub read_word: Option<SrhReadWord>,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SrhConfig {
    pub abi_version: u32,
    pub struct_size: u32,
    pub space: SrhHandle,
    pub base: u64,
    pub size: u64,
    pub reset_vector: u64,
    pub priority: i32,
    pub clock: u32,
    pub image: *const u8,
    pub image_size: u64,
    pub config_json: *const c_char,
    pub config_json_size: u64,
    pub images: *const SrhImagePart,
    pub image_count: u32,
    pub error_message: *mut c_char,
    pub error_message_capacity: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SrhProperty {
    pub abi_version: u32,
    pub struct_size: u32,
    pub name: *const c_char,
    pub group: *const c_char,
    pub description: *const c_char,
    pub kind: u32,
    pub bits: u32,
    pub base: u32,
    pub editable: u32,
    pub enum_labels: *const c_char,
    pub ui_flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SrhConfigEntry {
    pub abi_version: u32,
    pub struct_size: u32,
    pub category: *const c_char,
    pub name: *const c_char,
    pub label: *const c_char,
    pub description: *const c_char,
    pub kind: u32,
    pub enum_labels: *const c_char,
    pub default_value: *const c_char,
    pub context: *mut c_void,
    pub owner: SrhHandle,
    pub get: Option<SrhConfigGet>,
    pub set: Option<SrhConfigSet>,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SrhImageSlotDescriptor {
    pub abi_version: u32,
    pub struct_size: u32,
    pub label: *const c_char,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SrhCardDescriptor {
    pub abi_version: u32,
    pub struct_size: u32,
    pub category: *const c_char,
    pub name: *const c_char,
    pub description: *const c_char,
    pub default_base: u64,
    pub default_size: u64,
    pub default_reset_vector: u64,
    pub default_priority: i32,
    pub default_clock: u32,
    pub flags: u32,
    pub default_config_json: *const c_char,
    pub io_space_config_key: *const c_char,
    pub base_config_key: *const c_char,
    pub image_slots: *const SrhImageSlotDescriptor,
    pub image_slot_count: u32,
}

pub type SrhPropertySet = unsafe extern "C" fn(*mut c_void, u32, *const SrhValue) -> SrhStatus;
pub type SrhRead = unsafe extern "C" fn(*mut c_void, u64, *mut u8) -> SrhStatus;
pub type SrhWrite = unsafe extern "C" fn(*mut c_void, u64, u8) -> SrhStatus;
pub type SrhReadWord = unsafe extern "C" fn(*mut c_void, u64, *mut u32) -> SrhStatus;
pub type SrhCallback = unsafe extern "C" fn(*mut c_void) -> SrhStatus;
pub type SrhSignalCallback = unsafe extern "C" fn(*mut c_void, i32) -> SrhStatus;
pub type SrhSaveState = unsafe extern "C" fn(*mut c_void, *mut u8, *mut u64) -> SrhStatus;
pub type SrhLoadState = unsafe extern "C" fn(*mut c_void, *const u8, u64) -> SrhStatus;
pub type SrhDebugFlag = unsafe extern "C" fn(*mut c_void) -> c_int;
pub type SrhDisasm = unsafe extern "C" fn(
    *mut c_void,
    SrhHandle,
    SrhHandle,
    u64,
    *const u8,
    u32,
    *mut u32,
    *mut u32,
    *mut c_char,
    u32,
) -> SrhStatus;
pub type SrhTextQuery =
    unsafe extern "C" fn(*mut c_void, u64, *mut u8, *mut u32, *mut u32) -> SrhStatus;
pub type SrhVideoQuery =
    unsafe extern "C" fn(*mut c_void, u64, *mut u8, *mut u32, *mut u32) -> SrhStatus;
pub type SrhAudioRender = unsafe extern "C" fn(*mut c_void, u64, u32, *mut i16) -> SrhStatus;
pub type SrhProviderSnapshot =
    unsafe extern "C" fn(*mut c_void, *mut c_char, *mut u64) -> SrhStatus;
pub type SrhProviderCommand =
    unsafe extern "C" fn(*mut c_void, u32, u64, *const c_char, u64) -> SrhStatus;
pub type SrhConfigGet = unsafe extern "C" fn(*mut c_void, *mut c_char, u32) -> SrhStatus;
pub type SrhConfigSet = unsafe extern "C" fn(*mut c_void, *const c_char) -> SrhStatus;
pub type SrhPluginInit = unsafe extern "C" fn(*const ShouryoHost) -> *const SrhPlugin;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShouryoHost {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub log: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *const c_char) -> SrhStatus>,
    pub map: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            *const SrhMapping,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
    pub unmap: Option<unsafe extern "C" fn(*mut c_void, SrhHandle) -> SrhStatus>,
    pub read:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhHandle, u64, *mut u8) -> SrhStatus>,
    pub write:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhHandle, u64, u8) -> SrhStatus>,
    pub subscribe_clock: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            u32,
            SrhCallback,
            *mut c_void,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
    pub schedule: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            u64,
            SrhCallback,
            *mut c_void,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
    pub cancel: Option<unsafe extern "C" fn(*mut c_void, SrhHandle) -> SrhStatus>,
    pub boundary: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhHandle, u64) -> SrhStatus>,
    pub remove: Option<unsafe extern "C" fn(*mut c_void, SrhHandle) -> SrhStatus>,
    pub alive: Option<unsafe extern "C" fn(*mut c_void, SrhHandle) -> SrhStatus>,
    pub signal_find:
        Option<unsafe extern "C" fn(*mut c_void, *const c_char, *mut SrhHandle) -> SrhStatus>,
    pub signal_drive:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhHandle, i32, i32) -> SrhStatus>,
    pub signal_subscribe: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            SrhHandle,
            SrhSignalCallback,
            *mut c_void,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
    pub signal_read: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *mut i32) -> SrhStatus>,
    pub time_ns: Option<unsafe extern "C" fn(*mut c_void, *mut u64) -> SrhStatus>,
    pub random_u64: Option<unsafe extern "C" fn(*mut c_void, *mut u64) -> SrhStatus>,
    pub query:
        Option<unsafe extern "C" fn(*mut c_void, *const c_char, *mut *const c_void) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhPlugin {
    pub abi_version: u32,
    pub struct_size: u32,
    pub id: *const c_char,
    pub create: Option<
        unsafe extern "C" fn(
            *const ShouryoHost,
            SrhHandle,
            *const SrhConfig,
            *mut *mut c_void,
        ) -> SrhStatus,
    >,
    pub destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    pub reset: Option<unsafe extern "C" fn(*mut c_void, u32) -> SrhStatus>,
    pub property_count: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    pub property_info:
        Option<unsafe extern "C" fn(*mut c_void, u32, *mut SrhProperty) -> SrhStatus>,
    pub property_get: Option<unsafe extern "C" fn(*mut c_void, u32, *mut SrhValue) -> SrhStatus>,
    pub property_set: Option<SrhPropertySet>,
    pub save_state: Option<SrhSaveState>,
    pub load_state: Option<SrhLoadState>,
    pub card_descriptor: *const SrhCardDescriptor,
    pub save_project_data: Option<SrhSaveState>,
    pub load_project_data: Option<SrhLoadState>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostResourcesV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub lookup: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const c_char,
            *const c_char,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostProjectFilesV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub project_root: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *mut c_char, *mut u64) -> SrhStatus>,
    pub read_file: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *const c_char, *mut u8, *mut u64) -> SrhStatus>,
    pub write_file: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *const c_char, *const u8, u64) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostLifecycleV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub subscribe_resume: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhCallback, *mut c_void, *mut SrhHandle) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostDebugV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub register_disasm:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhDisasm, *mut c_void) -> SrhStatus>,
    pub boundary_ex:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhHandle, u64, u32, u32) -> SrhStatus>,
    pub request_stop:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *const c_char) -> SrhStatus>,
    pub set_trace_kind: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, u32) -> SrhStatus>,
    pub trace_enabled: Option<SrhDebugFlag>,
    pub boundary_required: Option<SrhDebugFlag>,
    pub set_disassembly_enabled:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, u32, *const c_char) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostMemoryV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub read_word:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhHandle, u64, *mut u32) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostInputV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub register_input: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            *const c_char,
            u32,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
    pub input_due: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *mut u64) -> SrhStatus>,
    pub input_pop:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *mut u64, *mut u8) -> SrhStatus>,
    pub subscribe_due: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            SrhHandle,
            SrhCallback,
            *mut c_void,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostTextV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub register_text: Option<
        unsafe extern "C" fn(*mut c_void, SrhHandle, SrhTextQuery, *mut c_void) -> SrhStatus,
    >,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhVideoTiming {
    pub abi_version: u32,
    pub struct_size: u32,
    pub frame_number: u64,
    pub scanline: u32,
    pub line_count: u32,
}
pub type SrhVideoTimingQuery = unsafe extern "C" fn(*mut c_void, *mut SrhVideoTiming) -> SrhStatus;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostVideoV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub register_video: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            u32,
            u32,
            SrhVideoFormat,
            SrhVideoQuery,
            *mut c_void,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
    pub register_video_ex: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            u32,
            u32,
            SrhVideoFormat,
            SrhVideoQuery,
            *mut c_void,
            u32,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
    pub set_video_timing: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            Option<SrhVideoTimingQuery>,
            *mut c_void,
        ) -> SrhStatus,
    >,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostAudioInputV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub request: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, u32) -> SrhStatus>,
    pub read: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *mut f32, u32, *mut u32, *mut u32) -> u32>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostAudioV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub sample_rate: u32,
    pub channels: u32,
    pub format: SrhAudioFormat,
    pub register_source: Option<
        unsafe extern "C" fn(
            *mut c_void,
            SrhHandle,
            u32,
            u32,
            SrhAudioFormat,
            *const c_char,
            SrhAudioRender,
            *mut c_void,
            *mut SrhHandle,
        ) -> SrhStatus,
    >,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostConfigV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub register_entry:
        Option<unsafe extern "C" fn(*mut c_void, *const SrhConfigEntry) -> SrhStatus>,
    pub unregister_context: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> SrhStatus>,
    pub get_value:
        Option<unsafe extern "C" fn(*mut c_void, *const c_char, *mut c_char, u32) -> SrhStatus>,
    pub set_value:
        Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostProvidersV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub register_provider:
        Option<unsafe extern "C" fn(*mut c_void, SrhHandle, *const SrhDataProviderV1) -> SrhStatus>,
    pub simulation_time_ns: Option<unsafe extern "C" fn(*mut c_void, *mut u64) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhHostSignalsV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub release: Option<unsafe extern "C" fn(*mut c_void, SrhHandle, SrhHandle) -> SrhStatus>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrhDataProviderV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut c_void,
    pub snapshot: Option<SrhProviderSnapshot>,
    pub command: Option<SrhProviderCommand>,
    pub name: [c_char; 128],
    pub protocol: [c_char; 128],
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzCardInfo {
    pub abi_version: u32,
    pub struct_size: u32,
    pub id: SrhHandle,
    pub type_: *const c_char,
    pub name: *const c_char,
    pub priority: i32,
    pub active: u32,
    pub parked: u32,
    pub load_error: *const c_char,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzSpace {
    pub abi_version: u32,
    pub struct_size: u32,
    pub id: SrhHandle,
    pub name: *const c_char,
    pub maximum: u64,
    pub fallback: u8,
    pub reserved: [u8; 3],
    pub random: u32,
    pub resolver: SrzResolver,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzClock {
    pub abi_version: u32,
    pub struct_size: u32,
    pub hz: u32,
    pub reserved: u32,
    pub ticks: u64,
    pub phase: u64,
    pub order: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzTrace {
    pub abi_version: u32,
    pub struct_size: u32,
    pub sequence: u64,
    pub parent: u64,
    pub time: u64,
    pub ticks: [u64; 3],
    pub master: SrhHandle,
    pub space: SrhHandle,
    pub address: u64,
    pub operation: u32,
    pub depth: u32,
    pub kind: u32,
    pub instruction: u64,
    pub value: u8,
    pub result: SrhStatus,
    pub responder_offset: u32,
    pub responder_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzBreakpoint {
    pub abi_version: u32,
    pub struct_size: u32,
    pub id: u64,
    pub card: SrhHandle,
    pub space: SrhHandle,
    pub first: u64,
    pub last: u64,
    pub operations: u32,
    pub enabled: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzProperty {
    pub abi_version: u32,
    pub struct_size: u32,
    pub name: *const c_char,
    pub group: *const c_char,
    pub description: *const c_char,
    pub enum_labels: *const c_char,
    pub kind: u32,
    pub bits: u32,
    pub base: u32,
    pub ui_flags: u32,
    pub editable: u32,
    pub value: SrhValue,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzConfigEntry {
    pub abi_version: u32,
    pub struct_size: u32,
    pub category: *const c_char,
    pub name: *const c_char,
    pub label: *const c_char,
    pub description: *const c_char,
    pub enum_labels: *const c_char,
    pub default_value: *const c_char,
    pub provider: *const c_char,
    pub type_: u32,
    pub owner: SrhHandle,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzTextEndpoint {
    pub abi_version: u32,
    pub struct_size: u32,
    pub card: SrhHandle,
    pub input_offset: u32,
    pub input_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzVideoSurface {
    pub abi_version: u32,
    pub struct_size: u32,
    pub id: SrhHandle,
    pub owner: SrhHandle,
    pub width: u32,
    pub height: u32,
    pub format: SrhVideoFormat,
    pub flags: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzAudioSource {
    pub abi_version: u32,
    pub struct_size: u32,
    pub id: SrhHandle,
    pub owner: SrhHandle,
    pub name: *const c_char,
    pub volume_percent: u32,
    pub muted: u32,
    pub active: u32,
    pub level_peak: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzAudioDiagnostics {
    pub abi_version: u32,
    pub struct_size: u32,
    pub queued_frames: u64,
    pub queue_capacity_frames: u64,
    pub dropped_frames: u64,
    pub underflow_frames: u64,
    pub source_errors: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzProviderData {
    pub abi_version: u32,
    pub struct_size: u32,
    pub owner: SrhHandle,
    pub name: *const c_char,
    pub protocol: *const c_char,
    pub data: *const c_char,
    pub data_size: u64,
    pub flags: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzPluginData {
    pub abi_version: u32,
    pub struct_size: u32,
    pub owner: SrhHandle,
    pub hex: *const c_char,
    pub size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzInputRecord {
    pub abi_version: u32,
    pub struct_size: u32,
    pub timestamp: u64,
    pub endpoint: *const c_char,
    pub value: u8,
    pub reserved: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzPluginDescriptor {
    pub abi_version: u32,
    pub struct_size: u32,
    pub path: *const c_char,
    pub id: *const c_char,
    pub category: *const c_char,
    pub name: *const c_char,
    pub description: *const c_char,
    pub default_config_json: *const c_char,
    pub io_space_config_key: *const c_char,
    pub base_config_key: *const c_char,
    pub default_base: u64,
    pub default_size: u64,
    pub default_reset_vector: u64,
    pub default_priority: i32,
    pub default_clock: u32,
    pub flags: u32,
    pub image_slot_offset: u32,
    pub image_slot_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzDisassemblyAvailability {
    pub abi_version: u32,
    pub struct_size: u32,
    pub state: u32,
    pub reserved: u32,
    pub revision: u64,
    pub message: [c_char; 256],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzDisassembly {
    pub abi_version: u32,
    pub struct_size: u32,
    pub ok: u32,
    pub instruction_bytes: u32,
    pub cycles: u32,
    pub byte_count: u32,
    pub bytes: [u8; 32],
    pub text: [c_char; 256],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzError {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: SrhStatus,
    pub reserved: u32,
    pub sequence: u64,
    pub message: [c_char; 512],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzImagePart {
    pub abi_version: u32,
    pub struct_size: u32,
    pub data: *const u8,
    pub size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SrzCardRequest {
    pub abi_version: u32,
    pub struct_size: u32,
    pub space: SrhHandle,
    pub base: u64,
    pub size: u64,
    pub reset_vector: u64,
    pub priority: i32,
    pub clock: u32,
    pub type_: SrzSlice,
    pub config_json: SrzSlice,
    pub plugin_directory: SrzSlice,
    pub images: *const SrzImagePart,
    pub image_count: u32,
    pub reserved: u32,
}

// `Default` cannot be derived for the records that contain fixed-size text
// buffers (`[c_char; N]`, `[u8; N]`).  The C++ ABI leaves those arrays zeroed
// when a caller initializes a record with `SRH_INIT`, so a zeroed array is the
// correct default.

impl Default for SrhValue {
    fn default() -> Self {
        SrhValue {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhValue>() as u32,
            unsigned_value: 0,
            signed_value: 0,
            text: [0; 128],
        }
    }
}

impl Default for SrhPlugin {
    fn default() -> Self {
        SrhPlugin {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhPlugin>() as u32,
            id: core::ptr::null(),
            create: None,
            destroy: None,
            reset: None,
            property_count: None,
            property_info: None,
            property_get: None,
            property_set: None,
            save_state: None,
            load_state: None,
            card_descriptor: core::ptr::null(),
            save_project_data: None,
            load_project_data: None,
        }
    }
}

impl Default for ShouryoHost {
    fn default() -> Self {
        ShouryoHost {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<ShouryoHost>() as u32,
            context: core::ptr::null_mut(),
            log: None,
            map: None,
            unmap: None,
            read: None,
            write: None,
            subscribe_clock: None,
            schedule: None,
            cancel: None,
            boundary: None,
            remove: None,
            alive: None,
            signal_find: None,
            signal_drive: None,
            signal_subscribe: None,
            signal_read: None,
            time_ns: None,
            random_u64: None,
            query: None,
        }
    }
}

impl Default for SrzPluginDescriptor {
    fn default() -> Self {
        SrzPluginDescriptor {
            abi_version: SRZ80_ENGINE_ABI,
            struct_size: core::mem::size_of::<SrzPluginDescriptor>() as u32,
            path: core::ptr::null(),
            id: core::ptr::null(),
            category: core::ptr::null(),
            name: core::ptr::null(),
            description: core::ptr::null(),
            default_config_json: core::ptr::null(),
            io_space_config_key: core::ptr::null(),
            base_config_key: core::ptr::null(),
            default_base: 0,
            default_size: 0,
            default_reset_vector: 0,
            default_priority: 0,
            default_clock: 0,
            flags: 0,
            image_slot_offset: 0,
            image_slot_count: 0,
        }
    }
}

impl Default for SrzDisassembly {
    fn default() -> Self {
        SrzDisassembly {
            abi_version: SRZ80_ENGINE_ABI,
            struct_size: core::mem::size_of::<SrzDisassembly>() as u32,
            ok: 0,
            instruction_bytes: 0,
            cycles: 0,
            byte_count: 0,
            bytes: [0; 32],
            text: [0; 256],
        }
    }
}

impl Default for SrhMapping {
    fn default() -> Self {
        SrhMapping {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhMapping>() as u32,
            ..SrhMapping::zeroed()
        }
    }
}

impl SrhMapping {
    /// A zeroed body. The header fields are then filled by `Default`.
    const fn zeroed() -> SrhMapping {
        SrhMapping {
            abi_version: 0,
            struct_size: 0,
            space: 0,
            first: 0,
            last: 0,
            priority: 0,
            context: core::ptr::null_mut(),
            read: None,
            write: None,
            peek: None,
            read_word: None,
        }
    }
}

/// Mirrors `srz80::sdk::valid`: an ABI-1 structure must declare the current
/// version and be at least as large as the layout this build knows.
pub fn valid<T>(pointer: *const T, size: usize) -> bool {
    if pointer.is_null() {
        return false;
    }
    // Safety: the caller guarantees that a non-null pointer refers to at least
    // `size` readable bytes for the header, which every ABI structure starts
    // with.
    unsafe {
        let base = pointer as *const u8;
        let header = core::slice::from_raw_parts(base, 8);
        let abi_version = u32::from_ne_bytes([header[0], header[1], header[2], header[3]]);
        let struct_size = u32::from_ne_bytes([header[4], header[5], header[6], header[7]]);
        abi_version == SRH_ABI && struct_size as usize >= size
    }
}

/// `srz80::sdk::has_field`: an optional tail field is readable only when the
/// supplier declared a structure large enough to contain it.
///
/// Safety: `pointer` must be null or point to a readable structure with the
/// declared `struct_size`.
pub unsafe fn has_field<T>(pointer: *const T, offset: usize, field_size: usize) -> bool {
    if pointer.is_null() {
        return false;
    }
    let base = pointer as *const u8;
    let header = unsafe { core::slice::from_raw_parts(base, 8) };
    let struct_size = u32::from_ne_bytes([header[4], header[5], header[6], header[7]]);
    struct_size as usize >= offset + field_size
}
