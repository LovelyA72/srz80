//! The query result arena.
//!
//! Records and interned UTF-8 strings produced by a fill call live here, and
//! every accessor hands out a pointer into this arena.  A pointer stays valid
//! until the next fill or clear call on the same result, or until the result is
//! destroyed -- exactly the contract `srz80_engine.h` documents.  Strings are
//! interned into `Box<CString>` allocations, whose addresses never move when
//! the arena grows.

use core::ffi::c_char;
use std::collections::HashMap;
use std::ffi::CString;

use crate::ffi::*;

#[derive(Default)]
pub struct ResultArena {
    pub strings: Vec<Box<CString>>,
    interned: HashMap<String, *const c_char>,

    pub cards: Vec<SrzCardInfo>,
    pub all_cards: Vec<SrzCardInfo>,
    pub removed_cards: Vec<SrzCardInfo>,
    pub spaces: Vec<SrzSpace>,
    pub breakpoints: Vec<SrzBreakpoint>,
    pub trace: Vec<SrzTrace>,
    pub handles: Vec<SrhHandle>,
    pub logs: Vec<*const c_char>,
    pub properties: Vec<SrzProperty>,
    pub config_entries: Vec<SrzConfigEntry>,
    pub text_endpoints: Vec<SrzTextEndpoint>,
    pub input_names: Vec<*const c_char>,
    pub video_surfaces: Vec<SrzVideoSurface>,
    pub audio_sources: Vec<SrzAudioSource>,
    pub providers: Vec<SrzProviderData>,
    pub plugin_data: Vec<SrzPluginData>,
    pub paths: Vec<*const c_char>,
    pub endpoints: Vec<*const c_char>,
    pub input_records: Vec<SrzInputRecord>,
    pub image_slots: Vec<*const c_char>,
    pub descriptor: SrzPluginDescriptor,
    pub has_descriptor: bool,
}

impl ResultArena {
    pub fn new() -> ResultArena {
        ResultArena::default()
    }

    /// Interns a string and returns a pointer that stays valid for the
    /// lifetime of this arena.  A repeated value reuses its allocation, so
    /// filling the same result repeatedly does not grow the arena.
    pub fn intern(&mut self, text: &str) -> *const c_char {
        if let Some(pointer) = self.interned.get(text) {
            return *pointer;
        }
        let owned = match CString::new(text) {
            Ok(owned) => owned,
            // An interior NUL cannot be represented in the ABI, so those bytes
            // are dropped.
            Err(_) => CString::new(text.bytes().filter(|byte| *byte != 0).collect::<Vec<u8>>())
                .expect("interior NULs were removed"),
        };
        let pointer = owned.as_ptr();
        self.strings.push(Box::new(owned));
        self.interned.insert(text.to_string(), pointer);
        pointer
    }

    /// Invalidates every record and list previously produced by a fill call.
    /// The retained string storage is kept so subsequent fills reuse it.
    pub fn clear(&mut self) {
        self.cards.clear();
        self.all_cards.clear();
        self.removed_cards.clear();
        self.spaces.clear();
        self.breakpoints.clear();
        self.trace.clear();
        self.handles.clear();
        self.logs.clear();
        self.properties.clear();
        self.config_entries.clear();
        self.text_endpoints.clear();
        self.input_names.clear();
        self.video_surfaces.clear();
        self.audio_sources.clear();
        self.providers.clear();
        self.plugin_data.clear();
        self.paths.clear();
        self.endpoints.clear();
        self.input_records.clear();
        self.image_slots.clear();
        self.descriptor = SrzPluginDescriptor::default();
        self.has_descriptor = false;
    }
}
