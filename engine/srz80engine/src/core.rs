//! Engine state: handles, spaces, signals, cards, registrations and host host
//! tables.
//!
//! The engine is single-threaded: one `Core` belongs to the thread that created
//! it and every card it loaded is driven from that thread.  The port therefore
//! keeps the C++ core's shape -- one owner of all state -- while using interior
//! mutability for everything a card callback can reach, so that a callback may
//! re-enter the engine (mapping, logging, driving a signal, querying time)
//! without creating an aliasing `&mut Core`.
//!
//! The interior-mutability fields are exactly the ones reachable from a card:
//! the card registry, routes, events, signals, inputs, video, audio, config
//! entries and providers.  A borrow of any of them is always released before a
//! plugin function is invoked; the code below copies the small amount of
//! dispatch state it needs out of the container first.

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::{c_char, c_void};
use std::cmp::Ordering;
use std::collections::{BTreeMap, BTreeSet, BinaryHeap, HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::rc::Rc;

use crate::ffi::*;
use crate::plugin::Library;
pub const CLOCK_COUNT: usize = 3;
pub const BILLION: u64 = 1_000_000_000;
pub const TRACE_CAPACITY: usize = 8192;
pub const STOPPED_BYTE: u32 = 5000;

/// `BinaryHeap` is a max-heap, so reverse the timer key to expose the earliest
/// `(due, order)` entry at the root. Event identity is deliberately excluded
/// from ordering: registration order is unique.
pub struct TimedEvent {
    pub event: Rc<Event>,
}

impl PartialEq for TimedEvent {
    fn eq(&self, other: &Self) -> bool {
        (self.event.due, self.event.order) == (other.event.due, other.event.order)
    }
}

impl Eq for TimedEvent {}

impl PartialOrd for TimedEvent {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for TimedEvent {
    fn cmp(&self, other: &Self) -> Ordering {
        (other.event.due, other.event.order).cmp(&(self.event.due, self.event.order))
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Resolver {
    Priority,
    BitOr,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TimeMode {
    Project,
    Fixed,
    System,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RunState {
    Stopped,
    Paused,
    Running,
}

impl RunState {
    pub fn as_abi(self) -> SrzRunState {
        match self {
            RunState::Stopped => SRZ_STOPPED,
            RunState::Running => SRZ_RUNNING,
            RunState::Paused => SRZ_PAUSED,
        }
    }

    pub fn from_abi(state: SrzRunState) -> RunState {
        match state {
            SRZ_STOPPED => RunState::Stopped,
            SRZ_RUNNING => RunState::Running,
            _ => RunState::Paused,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            RunState::Stopped => "stopped",
            RunState::Running => "running",
            RunState::Paused => "paused",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Space {
    pub id: Handle,
    pub name: String,
    pub maximum: u64,
    pub fallback: u8,
    pub random: bool,
    pub resolver: Resolver,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Clock {
    pub hz: u32,
    pub ticks: u64,
    pub phase: u64,
    pub order: u64,
}

/// A maximal address interval with the same ordered mapping candidates.
pub struct RouteRegion {
    pub space: Handle,
    pub first: u64,
    pub last: u64,
    pub mappings: Rc<[Rc<Mapping>]>,
}

#[derive(Default)]
pub struct RouteCache {
    pub regions: BTreeMap<(Handle, u64), Rc<RouteRegion>>,
    pub recent: Option<Rc<RouteRegion>>,
}

impl RouteCache {
    pub fn clear(&mut self) {
        self.regions.clear();
        self.recent = None;
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Trace {
    pub sequence: u64,
    pub parent: u64,
    pub time: u64,
    pub ticks: [u64; 3],
    pub master: Handle,
    pub space: Handle,
    pub address: u64,
    pub operation: u32,
    pub depth: u32,
    pub kind: u32,
    pub instruction: u64,
    pub value: u8,
    pub result: SrhStatus,
    pub responders: Vec<Handle>,
}

#[derive(Clone, Copy, Debug)]
pub struct Breakpoint {
    pub id: u64,
    pub card: Handle,
    pub space: Handle,
    pub first: u64,
    pub last: u64,
    pub operations: u32,
    pub enabled: bool,
}

#[derive(Clone, Debug)]
pub struct CardInfo {
    pub id: Handle,
    pub type_: String,
    pub name: String,
    pub priority: i32,
    pub active: bool,
    pub parked: bool,
    pub load_error: String,
}

#[derive(Clone, Copy, Debug)]
pub struct VideoSurface {
    pub id: Handle,
    pub owner: Handle,
    pub width: u32,
    pub height: u32,
    pub format: SrhVideoFormat,
    pub flags: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct AudioDiagnostics {
    pub queued_frames: u64,
    pub queue_capacity_frames: u64,
    pub dropped_frames: u64,
    pub underflow_frames: u64,
    pub source_errors: u64,
}

#[derive(Clone, Debug)]
pub struct AudioSourceInfo {
    pub id: Handle,
    pub owner: Handle,
    pub name: String,
    pub volume_percent: u32,
    pub muted: bool,
    pub active: bool,
    pub level_peak: u32,
}

#[derive(Clone, Debug)]
pub struct TextEndpointInfo {
    pub card: Handle,
    pub inputs: Vec<String>,
}

#[derive(Clone, Debug)]
pub struct Property {
    pub name: String,
    pub group: String,
    pub description: String,
    pub enum_labels: String,
    pub kind: u32,
    pub bits: u32,
    pub base: u32,
    pub ui_flags: u32,
    pub editable: bool,
    pub value: SrhValue,
}

#[derive(Clone)]
pub struct ConfigEntry {
    pub category: String,
    pub name: String,
    pub label: String,
    pub description: String,
    pub enum_labels: String,
    pub default_value: String,
    pub provider: String,
    pub type_: u32,
    pub context: *mut c_void,
    pub owner: Handle,
    pub get: Option<SrhConfigGet>,
    pub set: Option<SrhConfigSet>,
}

#[derive(Clone, Debug)]
pub struct ProviderData {
    pub name: String,
    pub protocol: String,
    pub data: String,
    pub flags: u32,
}

/// One loaded card.  `api` points into `library`, which stays alive for at
/// least as long as the card, and `instance` is freed through `api.destroy`
/// before the card record disappears.
///
/// Every card field a host callback can change is a `Cell`: the registry hands
/// out shared `Rc<Card>` clones, and a card calls back into the engine from
/// inside its own `create`/`reset`/property callbacks, so no `&mut Card` may
/// ever be created.
pub struct Card {
    pub id: Handle,
    /// Null for a card installed by a test that has no plugin behind it.
    pub library: *mut Library,
    pub api: *const SrhPlugin,
    pub instance: Cell<*mut c_void>,
    pub type_: String,
    pub name: RefCell<String>,
    pub priority: i32,
    pub active: Cell<bool>,
    pub parked: Cell<bool>,
    pub disasm: Cell<Option<SrhDisasm>>,
    pub disasm_context: Cell<*mut c_void>,
    pub disasm_enabled: Cell<bool>,
    pub disasm_revision: Cell<u64>,
    pub disasm_message: RefCell<String>,
    pub text: Cell<Option<SrhTextQuery>>,
    pub text_context: Cell<*mut c_void>,
    pub load_error: String,
}

impl Card {
    /// # Safety
    /// `self.api` is a live plugin table for the whole card lifetime.
    pub unsafe fn plugin(&self) -> &SrhPlugin {
        unsafe { &*self.api }
    }

    pub fn is_active(&self) -> bool {
        self.active.get()
    }

    pub fn is_parked(&self) -> bool {
        self.parked.get()
    }

    pub fn name(&self) -> String {
        self.name.borrow().clone()
    }

    pub fn instance(&self) -> *mut c_void {
        self.instance.get()
    }

    pub fn disassembler(&self) -> Option<(SrhDisasm, *mut c_void)> {
        self.disasm
            .get()
            .map(|function| (function, self.disasm_context.get()))
    }

    pub fn text_provider(&self) -> Option<(SrhTextQuery, *mut c_void)> {
        self.text
            .get()
            .map(|function| (function, self.text_context.get()))
    }
}

impl Drop for Card {
    fn drop(&mut self) {
        // The library outlives the instance: `destroy` belongs to the same
        // module and runs before `library` is released.
        let instance = self.instance.replace(std::ptr::null_mut());
        if instance.is_null() || self.library.is_null() {
            return;
        }
        // Safety: `instance` was produced by this plugin's `create`, and the
        // library is still loaded at this point.
        unsafe {
            if !self.api.is_null() {
                if let Some(destroy) = (*self.api).destroy {
                    destroy(instance);
                }
            }
        }
    }
}

impl Core {
    /// Installs a card with no plugin behind it, for tests that need a live
    /// owner handle without loading a shared library.
    ///
    /// The card never receives a callback, so it needs no host table; it exists
    /// to exercise handle lifetime, signal ownership, breakpoint association and
    /// rack ordering.
    #[cfg(test)]
    pub fn attach_test_card(&self, type_: &str) -> Handle {
        // A plugin table with no callbacks, promoted to a static so the card's
        // `api` pointer stays valid for as long as the card exists.
        const TEST_PLUGIN: SrhPlugin = SrhPlugin {
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
        };
        let id = self.next();
        let card = Rc::new(Card {
            id,
            library: std::ptr::null_mut(),
            api: &TEST_PLUGIN,
            instance: Cell::new(std::ptr::null_mut()),
            type_: type_.to_string(),
            name: RefCell::new(String::new()),
            priority: 0,
            active: Cell::new(true),
            parked: Cell::new(false),
            disasm: Cell::new(None),
            disasm_context: core::cell::Cell::new(std::ptr::null_mut()),
            disasm_enabled: core::cell::Cell::new(true),
            disasm_revision: core::cell::Cell::new(1),
            disasm_message: core::cell::RefCell::new(String::new()),
            text: Cell::new(None),
            text_context: Cell::new(std::ptr::null_mut()),
            load_error: String::new(),
        });
        self.cards.borrow_mut().insert(id, card);
        self.active_cards.borrow_mut().insert(id);
        self.rack_order.borrow_mut().push(id);
        id
    }
}

#[derive(Clone, Copy)]
pub struct MappingApi {
    pub space: Handle,
    pub first: u64,
    pub last: u64,
    pub priority: i32,
    pub context: *mut c_void,
    pub read: Option<SrhRead>,
    pub write: Option<SrhWrite>,
    pub peek: Option<SrhRead>,
    pub read_word: Option<SrhReadWord>,
}

pub struct Mapping {
    pub id: Handle,
    pub owner: Handle,
    pub api: MappingApi,
    pub active: Cell<bool>,
    pub visible: Cell<bool>,
}

pub struct Event {
    pub id: Handle,
    pub owner: Handle,
    pub due: u64,
    pub order: u64,
    pub clock: Cell<Option<u32>>,
    pub callback: SrhCallback,
    pub context: *mut c_void,
    pub active: Cell<bool>,
    pub visible: Cell<bool>,
}

pub struct Listener {
    pub id: Handle,
    pub owner: Handle,
    pub signal: Handle,
    pub callback: SrhSignalCallback,
    pub context: *mut c_void,
    pub active: Cell<bool>,
    pub visible: Cell<bool>,
}

pub struct ResumeListener {
    pub id: Handle,
    pub owner: Handle,
    pub callback: SrhCallback,
    pub context: *mut c_void,
    pub active: Cell<bool>,
}

pub struct Driver {
    pub owner: Handle,
    pub value: i32,
    pub priority: i32,
    pub order: u64,
}

pub struct Signal {
    pub id: Handle,
    pub name: String,
    pub idle: i32,
    pub value: i32,
    pub drivers: Vec<Driver>,
}

#[derive(Clone, Copy, Debug)]
pub struct InputRecord {
    pub first: u64,
    pub second: u8,
    pub source: u64,
}

pub struct InputEndpoint {
    pub id: Handle,
    pub owner: Handle,
    pub name: String,
    pub capacity: u32,
    pub records: RefCell<VecDeque<InputRecord>>,
    pub active: Cell<bool>,
    pub subscription: Cell<Handle>,
    pub wake: Cell<Handle>,
    pub callback: Cell<Option<SrhCallback>>,
    pub context: Cell<*mut c_void>,
    pub notified: Cell<bool>,
}

pub struct VideoRegistration {
    pub timing: Cell<Option<(SrhVideoTimingQuery, *mut c_void)>>,
    pub id: Handle,
    pub owner: Handle,
    pub width: u32,
    pub height: u32,
    pub format: SrhVideoFormat,
    pub flags: u32,
    pub query: SrhVideoQuery,
    pub context: *mut c_void,
    pub active: Cell<bool>,
}

pub struct AudioSource {
    pub id: Handle,
    pub owner: Handle,
    pub sample_rate: u32,
    pub channels: u32,
    pub format: SrhAudioFormat,
    pub name: String,
    pub render: SrhAudioRender,
    pub context: *mut c_void,
    pub scratch: RefCell<Vec<i16>>,
    /// Native-rate stereo frames retained across host mix batches.  The front
    /// is the native frame containing `resample_phase`.
    pub resample_buffer: RefCell<VecDeque<[i16; 2]>>,
    /// Native frames immediately preceding `resample_buffer`, retained for
    /// causal filters and the left side of the sinc kernel.
    pub resample_history: RefCell<VecDeque<[i16; 2]>>,
    /// Fractional native-frame position, expressed with the host sample rate
    /// as its denominator.
    pub resample_phase: Cell<u64>,
    /// First native frame not yet requested from the source callback.
    pub resample_next_frame: Cell<u64>,
    pub errors: Cell<u64>,
    pub volume_percent: Cell<u32>,
    pub pan: Cell<i32>,
    pub muted: Cell<bool>,
    pub active: Cell<bool>,
    pub level_peak: Cell<u32>,
}

/// Rack state.  See the module comment for the interior-mutability contract.
pub struct Core {
    pub project_root: Option<PathBuf>,
    pub next: Cell<Handle>,
    pub order: Cell<u64>,
    pub sequence: Cell<u64>,
    pub now: Cell<u64>,
    pub rng: Cell<u64>,
    pub epoch: Cell<u64>,
    pub dropped: Cell<u64>,
    pub input_order: Cell<u64>,
    pub depth: Cell<u32>,
    pub collecting: Cell<bool>,
    pub collection_pending: Cell<bool>,
    pub event_collection_pending: Cell<bool>,
    pub shutting_down: Cell<bool>,
    pub mode: Cell<TimeMode>,
    pub run_state: Cell<RunState>,
    pub stop_reason: RefCell<String>,
    pub clocks: [Cell<Clock>; CLOCK_COUNT],
    pub breakpoints: RefCell<Vec<Breakpoint>>,
    pub trace: RefCell<VecDeque<Trace>>,
    pub trace_stack: RefCell<Vec<u64>>,
    pub logs: RefCell<Vec<String>>,
    pub stopped_boundary: Cell<Option<[u64; 3]>>,
    pub skip_boundary: Cell<Option<[u64; 3]>>,
    pub pending_reset: Cell<Option<bool>>,
    /// Changes whenever clock/event topology or card liveness changes. Hot
    /// dispatch paths compare this scalar instead of rescanning registries.
    pub topology_generation: Cell<u64>,
    pub instruction_seq: RefCell<BTreeMap<Handle, u64>>,
    pub trace_kind: Cell<u32>,
    pub trace_capture: Cell<bool>,
    pub trace_capture_operations: Cell<u32>,

    pub spaces: RefCell<BTreeMap<Handle, Space>>,
    pub cards: RefCell<BTreeMap<Handle, Rc<Card>>>,
    pub rack_order: RefCell<Vec<Handle>>,
    // Small, monotonic handle set: avoid hashing on every tick and bus access.
    pub active_cards: RefCell<BTreeSet<Handle>>,
    pub libraries: RefCell<HashMap<PathBuf, Rc<Library>>>,
    pub mappings: RefCell<Vec<Rc<Mapping>>>,
    pub route_cache: RefCell<RouteCache>,
    pub events: RefCell<Vec<Rc<Event>>>,
    pub clock_events: [RefCell<Vec<Rc<Event>>>; CLOCK_COUNT],
    pub timed_events: RefCell<BinaryHeap<TimedEvent>>,
    /// Timers registered inside a callback become heap-visible only when the
    /// outermost dispatch frame ends.
    pub pending_timed_events: RefCell<Vec<Rc<Event>>>,
    /// Heap entries encountered while their owner is parked. Parking is
    /// reversible, unlike cancellation or removal, so these cannot be dropped.
    pub parked_timed_events: RefCell<Vec<TimedEvent>>,
    pub listeners: RefCell<Vec<Rc<Listener>>>,
    pub resume_listeners: RefCell<Vec<Rc<ResumeListener>>>,
    pub signals: RefCell<BTreeMap<Handle, Signal>>,
    pub inputs: RefCell<Vec<Rc<InputEndpoint>>>,
    pub videos: RefCell<Vec<Rc<VideoRegistration>>>,
    pub audio_sources: RefCell<Rc<Vec<Rc<AudioSource>>>>,
    pub providers: RefCell<BTreeMap<Handle, SrhDataProviderV1>>,

    pub config: RefCell<crate::config::Config>,
    pub config_path: RefCell<PathBuf>,
    pub config_entries: RefCell<Vec<ConfigEntry>>,

    pub audio_queue: RefCell<VecDeque<i16>>,
    pub audio_mix_scratch: RefCell<Vec<i64>>,
    pub audio_sample_rate: u32,
    pub audio_channels: u32,
    pub audio_input: RefCell<crate::audio_input::AudioInput>,
    pub audio_queue_capacity: Cell<u64>,
    pub audio_next_frame: Cell<u64>,
    pub audio_last_ns: Cell<u64>,
    pub audio_next_due_ns: Cell<u64>,
    pub audio_remainder: Cell<u64>,
    pub audio_dropped_frames: Cell<u64>,
    pub audio_underflow_frames: Cell<u64>,
    pub audio_source_errors: Cell<u64>,
    pub audio_resampling: Cell<u32>,
    pub audio_master_volume: Cell<u32>,
    pub audio_master_peaks: [Cell<u32>; 2],
    pub audio_dc_correction: Cell<bool>,
    pub audio_clipping: Cell<bool>,
    pub audio_dc_state: [Cell<f64>; 2],

    /// The versioned host tables handed to card plugins.  They are created
    /// once, after the core has reached its final address, so the callback
    /// context and every extension pointer stay stable for the engine
    /// lifetime.
    host: OnceCell<Box<crate::host::HostTables>>,
}

impl Core {
    pub fn new(initial_state: RunState) -> Core {
        Self::new_with_audio_rate(initial_state, 44_100)
    }

    pub fn new_with_audio_rate(initial_state: RunState, audio_sample_rate: u32) -> Core {
        let stop_reason = match initial_state {
            RunState::Stopped => "Stopped",
            RunState::Running => "Running",
            RunState::Paused => "Paused",
        };
        let config_path = crate::paths::executable_directory().join("config.ini");
        let core = Core {
            project_root: None,
            next: Cell::new(1),
            order: Cell::new(1),
            sequence: Cell::new(1),
            now: Cell::new(0),
            rng: Cell::new(1),
            epoch: Cell::new(0),
            dropped: Cell::new(0),
            input_order: Cell::new(1),
            depth: Cell::new(0),
            collecting: Cell::new(false),
            collection_pending: Cell::new(false),
            event_collection_pending: Cell::new(false),
            shutting_down: Cell::new(false),
            mode: Cell::new(TimeMode::Project),
            run_state: Cell::new(initial_state),
            stop_reason: RefCell::new(stop_reason.to_string()),
            clocks: [
                Cell::new(Clock::default()),
                Cell::new(Clock::default()),
                Cell::new(Clock::default()),
            ],
            breakpoints: RefCell::new(Vec::new()),
            trace: RefCell::new(VecDeque::new()),
            trace_stack: RefCell::new(Vec::new()),
            logs: RefCell::new(Vec::new()),
            stopped_boundary: Cell::new(None),
            skip_boundary: Cell::new(None),
            pending_reset: Cell::new(None),
            topology_generation: Cell::new(1),
            instruction_seq: RefCell::new(BTreeMap::new()),
            trace_kind: Cell::new(0),
            trace_capture: Cell::new(true),
            trace_capture_operations: Cell::new(SRH_READ | SRH_WRITE),
            spaces: RefCell::new(BTreeMap::new()),
            cards: RefCell::new(BTreeMap::new()),
            rack_order: RefCell::new(Vec::new()),
            active_cards: RefCell::new(BTreeSet::new()),
            libraries: RefCell::new(HashMap::new()),
            mappings: RefCell::new(Vec::new()),
            route_cache: RefCell::new(RouteCache::default()),
            events: RefCell::new(Vec::new()),
            clock_events: std::array::from_fn(|_| RefCell::new(Vec::new())),
            timed_events: RefCell::new(BinaryHeap::new()),
            pending_timed_events: RefCell::new(Vec::new()),
            parked_timed_events: RefCell::new(Vec::new()),
            listeners: RefCell::new(Vec::new()),
            resume_listeners: RefCell::new(Vec::new()),
            signals: RefCell::new(BTreeMap::new()),
            inputs: RefCell::new(Vec::new()),
            videos: RefCell::new(Vec::new()),
            audio_sources: RefCell::new(Rc::new(Vec::new())),
            providers: RefCell::new(BTreeMap::new()),
            config: RefCell::new(crate::config::Config::default()),
            config_path: RefCell::new(config_path),
            config_entries: RefCell::new(Vec::new()),
            audio_queue: RefCell::new(VecDeque::new()),
            audio_mix_scratch: RefCell::new(vec![0; 1024 * 2]),
            audio_sample_rate,
            audio_channels: 2,
            audio_input: RefCell::new(crate::audio_input::AudioInput::default()),
            audio_queue_capacity: Cell::new(44100),
            audio_next_frame: Cell::new(0),
            audio_last_ns: Cell::new(0),
            audio_next_due_ns: Cell::new(0),
            audio_remainder: Cell::new(0),
            audio_dropped_frames: Cell::new(0),
            audio_underflow_frames: Cell::new(0),
            audio_source_errors: Cell::new(0),
            audio_resampling: Cell::new(1),
            audio_master_volume: Cell::new(100),
            audio_master_peaks: [Cell::new(0), Cell::new(0)],
            audio_dc_correction: Cell::new(false),
            audio_clipping: Cell::new(false),
            audio_dc_state: [Cell::new(0.0), Cell::new(0.0)],
            host: OnceCell::new(),
        };
        for index in 0..CLOCK_COUNT {
            let order = core.order.get();
            core.order.set(order + 1);
            let mut clock = core.clocks[index].get();
            clock.order = order;
            core.clocks[index].set(clock);
        }
        core.signal("/RST".to_string(), STOPPED_BYTE as i32);
        core.signal("IRQ".to_string(), 0);
        core.signal("NMI".to_string(), 0);
        core
    }

    /// Allocates the next handle.  The counter never wraps, so a handle is
    /// never reused.
    pub fn next(&self) -> Handle {
        let value = self.next.get();
        if value == Handle::MAX {
            panic!("Handle space exhausted");
        }
        self.next.set(value + 1);
        value
    }

    pub fn alive(&self, handle: Handle) -> bool {
        handle != 0 && self.active_cards.borrow().contains(&handle)
    }

    pub fn topology_changed(&self) {
        self.topology_generation
            .set(self.topology_generation.get().wrapping_add(1));
    }

    pub fn space(
        &self,
        name: String,
        maximum: u64,
        fallback: u8,
        random: bool,
        resolver: Resolver,
    ) -> Handle {
        if name.is_empty() || self.find_space(&name) != 0 {
            panic!("Duplicate/empty address space");
        }
        let handle = self.next();
        self.spaces.borrow_mut().insert(
            handle,
            Space {
                id: handle,
                name,
                maximum,
                fallback,
                random,
                resolver,
            },
        );
        handle
    }

    pub fn find_space(&self, name: &str) -> Handle {
        self.spaces
            .borrow()
            .values()
            .find(|space| space.name == name)
            .map(|space| space.id)
            .unwrap_or(0)
    }

    /// Registers a signal.  An empty name or an existing name is rejected the
    /// way the C++ core's `std::invalid_argument` was: no signal is created and
    /// the caller receives `None`.
    pub fn try_signal(&self, name: String, idle: i32) -> Option<Handle> {
        if name.is_empty() || self.find_signal(&name) != 0 {
            return None;
        }
        let handle = self.next();
        self.signals.borrow_mut().insert(
            handle,
            Signal {
                id: handle,
                name,
                idle,
                value: idle,
                drivers: Vec::new(),
            },
        );
        Some(handle)
    }

    /// Registers a signal and treats a duplicate as an error, which is the
    /// right behaviour while validating a project document.
    pub fn signal(&self, name: String, idle: i32) -> Handle {
        match self.try_signal(name, idle) {
            Some(handle) => handle,
            None => panic!("Duplicate/empty signal"),
        }
    }

    pub fn find_signal(&self, name: &str) -> Handle {
        self.signals
            .borrow()
            .values()
            .find(|signal| signal.name == name)
            .map(|signal| signal.id)
            .unwrap_or(0)
    }

    // ------------------------------------------------------------------
    // Frame / collection
    // ------------------------------------------------------------------

    pub fn enter_frame(&self) {
        self.depth.set(self.depth.get() + 1);
    }

    pub fn leave_frame(&self) {
        let depth = self.depth.get();
        self.depth.set(depth - 1);
        if depth == 1 {
            self.collect();
        }
    }

    /// Retires removed registrations, then makes every surviving registration
    /// visible.  Nested frames defer this until the outermost call unwinds.
    pub fn collect(&self) {
        if self.depth.get() != 0 || self.collecting.get() {
            return;
        }
        // Timer churn must not sweep unrelated device registrations. Card
        // removal still requests a full sweep, including its owned events.
        if !self.collection_pending.get() {
            if self.event_collection_pending.replace(false) {
                self.events.borrow_mut().retain(|event| event.active.get());
                for clock in &self.clock_events {
                    clock.borrow_mut().retain(|event| event.active.get());
                }
                let pending: Vec<_> = self.pending_timed_events.borrow_mut().drain(..).collect();
                let mut timed_events = self.timed_events.borrow_mut();
                for event in pending {
                    if event.active.get() && self.alive(event.owner) {
                        timed_events.push(TimedEvent { event });
                    }
                }
                for event in self.events.borrow().iter() {
                    event.visible.set(true);
                }
            }
            if let Some(cold) = self.pending_reset.replace(None) {
                if !self.shutting_down.get() {
                    self.reset(cold);
                }
            }
            return;
        }
        self.collecting.set(true);
        self.event_collection_pending.set(false);
        let retired = |owner: Handle| -> bool {
            match self.cards.borrow().get(&owner) {
                None => true,
                Some(card) => !card.is_active() && !card.is_parked(),
            }
        };

        self.providers
            .borrow_mut()
            .retain(|owner, _| !retired(*owner));

        let old_mapping_count = self.mappings.borrow().len();
        self.mappings
            .borrow_mut()
            .retain(|mapping| mapping.active.get() && !retired(mapping.owner));
        let mappings_removed = self.mappings.borrow().len() != old_mapping_count;
        self.events
            .borrow_mut()
            .retain(|event| event.active.get() && !retired(event.owner));
        for clock in &self.clock_events {
            clock
                .borrow_mut()
                .retain(|event| event.active.get() && !retired(event.owner));
        }
        self.pending_timed_events
            .borrow_mut()
            .retain(|event| event.active.get() && !retired(event.owner));
        self.parked_timed_events
            .borrow_mut()
            .retain(|entry| entry.event.active.get() && !retired(entry.event.owner));
        self.listeners
            .borrow_mut()
            .retain(|listener| listener.active.get() && !retired(listener.owner));
        self.resume_listeners
            .borrow_mut()
            .retain(|listener| listener.active.get() && !retired(listener.owner));
        self.inputs
            .borrow_mut()
            .retain(|input| input.active.get() && !retired(input.owner));
        self.videos
            .borrow_mut()
            .retain(|video| video.active.get() && !retired(video.owner));
        self.audio_input.borrow_mut().queues.retain(|owner, _| !retired(*owner));
        Rc::make_mut(&mut self.audio_sources.borrow_mut())
            .retain(|source| source.active.get() && !retired(source.owner));

        let mut mappings_became_visible = false;
        for mapping in self.mappings.borrow().iter() {
            if !mapping.visible.get() {
                mappings_became_visible = true;
            }
            mapping.visible.set(true);
        }
        if mappings_removed || mappings_became_visible {
            self.route_cache.borrow_mut().clear();
        }
        for event in self.events.borrow().iter() {
            event.visible.set(true);
        }
        let pending: Vec<_> = self.pending_timed_events.borrow_mut().drain(..).collect();
        let mut timed_events = self.timed_events.borrow_mut();
        for event in pending {
            if event.active.get() && !retired(event.owner) {
                timed_events.push(TimedEvent { event });
            }
        }
        for listener in self.listeners.borrow().iter() {
            listener.visible.set(true);
        }

        // Erase before destroy: a nested query from a destructor must not
        // rediscover the card, and the removal callbacks have already run.
        let mut retired_cards: Vec<Rc<Card>> = Vec::new();
        {
            let mut cards = self.cards.borrow_mut();
            let mut rack = self.rack_order.borrow_mut();
            cards.retain(|id, card| {
                if !card.is_active() && !card.is_parked() {
                    rack.retain(|entry| entry != id);
                    retired_cards.push(Rc::clone(card));
                    false
                } else {
                    true
                }
            });
        }
        for card in retired_cards {
            self.config_entries
                .borrow_mut()
                .retain(|entry| entry.owner != card.id);
            // `Card::drop` calls the plugin's destroy before the library is
            // released.
            drop(card);
        }

        self.collection_pending.set(false);
        self.collecting.set(false);
        if let Some(cold) = self.pending_reset.replace(None) {
            if !self.shutting_down.get() {
                self.reset(cold);
            }
        }
    }
}

impl Drop for Core {
    fn drop(&mut self) {
        self.shutting_down.set(true);
        // Remove every card so the plugin `destroy` callbacks run while the
        // libraries are still loaded, then release the libraries.  No callback
        // may re-enter a partially dropped engine, so the registry is emptied
        // first and the collection machinery is bypassed.
        let cards: Vec<Rc<Card>> = std::mem::take(&mut *self.cards.borrow_mut())
            .into_values()
            .collect();
        self.active_cards.borrow_mut().clear();
        self.rack_order.borrow_mut().clear();
        drop(cards);
    }
}

pub use crate::handle::Handle;

impl Core {
    /// Creates the host tables.
    ///
    /// Must be called exactly once, after the core has reached the address it
    /// will keep for its whole life and before any plugin can see the host
    /// table.  Every table captures that address as its callback context, so
    /// the address must never change afterwards:
    /// `srz80_engine_replace` installs a candidate by exchanging the two
    /// handles' cores rather than by moving a core value.
    pub fn initialize_host(&self) {
        let core = self as *const Core as *mut c_void;
        if self.host.get().is_none() {
            let _ = self.host.set(Box::new(crate::host::HostTables::new(core)));
        }
        // The thread-local is what a scheduled input wakeup uses to reach its
        // engine, so it follows the most recently initialized core on this
        // thread.
        ACTIVE_CORE.with(|active| active.set(core));
    }

    /// The host table handed to `srz80_plugin_init`.
    pub fn host(&self) -> *const ShouryoHost {
        match self.host.get() {
            Some(tables) => tables.host(),
            None => core::ptr::null(),
        }
    }

    pub fn host_tables(&self) -> Option<&crate::host::HostTables> {
        self.host.get().map(|tables| &**tables)
    }

}

thread_local! {
    /// The engine that owns the current thread.  Every engine instance and the
    /// cards it loads belong to the thread that created the engine, and the ABI
    /// never shares an instance between threads, so one slot per thread is
    /// exact.  It exists so a scheduled input wakeup, which only receives an
    /// opaque context, can reach its engine without a self-referential pointer.
    static ACTIVE_CORE: Cell<*mut c_void> = const { Cell::new(core::ptr::null_mut()) };
}

/// Returns the engine created on this thread, or null when none is active.
///
/// # Safety
/// The returned pointer is only valid until that engine is destroyed.
pub unsafe fn active_core() -> *mut Core {
    ACTIVE_CORE.with(|slot| slot.get() as *mut Core)
}

/// Runs `body` with the engine that owns the current thread, if any.
pub fn with_active_core<R>(body: impl FnOnce(&Core) -> R) -> Option<R> {
    let Some(core) = active_core_ref() else {
        return None;
    };
    Some(body(core))
}

/// The engine that owns the current thread, if any.
pub fn active_core_ref<'a>() -> Option<&'a Core> {
    // Safety: the slot is only ever set to a live engine on this thread and is
    // cleared when that engine is destroyed.
    let pointer = unsafe { active_core() };
    if pointer.is_null() {
        None
    } else {
        Some(unsafe { &*pointer })
    }
}

/// Copies a NUL-terminated C string; `None` becomes the empty string.
///
/// # Safety
/// `pointer` must be null or point to a NUL-terminated string that stays valid
/// for the call.
pub unsafe fn cstr(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    unsafe { std::ffi::CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

/// Copies a borrowed UTF-8 slice exactly as the C++ `slice_text` helper does.
///
/// # Safety
/// `slice.data` must be null or point to `slice.size` readable bytes.
pub unsafe fn slice_text(slice: SrzSlice) -> String {
    if slice.data.is_null() || slice.size == 0 {
        return String::new();
    }
    let bytes = unsafe { std::slice::from_raw_parts(slice.data as *const u8, slice.size as usize) };
    String::from_utf8_lossy(bytes).into_owned()
}

/// # Safety
/// Same contract as [`slice_text`].
pub unsafe fn slice_path(slice: SrzSlice) -> PathBuf {
    PathBuf::from(unsafe { slice_text(slice) })
}

pub fn path_text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}
