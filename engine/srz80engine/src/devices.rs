//! Card lifecycle, debugger surfaces and device registrations.
//!
//! Cards are loaded from the plugin directory by id, keep their library alive
//! for their whole lifetime, and are destroyed inside a dispatch frame so a
//! plugin may unmap its registrations from its own destructor.

use core::ffi::{c_void, CStr};
use std::path::Path;
use std::rc::Rc;

use crate::core::{Card, Core, Handle, RunState};
use crate::ffi::*;
use crate::plugin::Library;

impl Core {
    /// A rack slot with no native instance or bus registrations. Placement is
    /// independent of availability; the session retains its original document.
    pub fn unavailable_card(&self, type_: String, priority: i32, error: String) -> Handle {
        const INERT: SrhPlugin = SrhPlugin {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhPlugin>() as u32,
            id: std::ptr::null(),
            create: None,
            destroy: None,
            reset: None,
            property_count: None,
            property_info: None,
            property_get: None,
            property_set: None,
            save_state: None,
            load_state: None,
            card_descriptor: std::ptr::null(),
            save_project_data: None,
            load_project_data: None,
        };
        let id = self.next();
        self.cards.borrow_mut().insert(
            id,
            Rc::new(Card {
                id,
                library: std::ptr::null_mut(),
                api: &INERT,
                instance: core::cell::Cell::new(std::ptr::null_mut()),
                type_,
                name: core::cell::RefCell::new(String::new()),
                priority,
                // Internal lifecycle placement; the public active flag also checks
                // availability. This handle never enters the live-owner set.
                active: core::cell::Cell::new(true),
                parked: core::cell::Cell::new(false),
                disasm: core::cell::Cell::new(None),
                disasm_context: core::cell::Cell::new(std::ptr::null_mut()),
                disasm_enabled: core::cell::Cell::new(true),
                disasm_revision: core::cell::Cell::new(1),
                disasm_message: core::cell::RefCell::new(String::new()),
                text: core::cell::Cell::new(None),
                text_context: core::cell::Cell::new(std::ptr::null_mut()),
                load_error: error,
            }),
        );
        self.rack_order.borrow_mut().push(id);
        id
    }
    /// Loads one card.  Mirrors `Core::load`: the library is cached by
    /// canonical path, the plugin table is validated, and a card whose `create`
    /// fails is removed and reported as an error.
    pub fn load(&self, path: &Path, config: &SrhConfig) -> Result<Handle, String> {
        if self.depth.get() != 0 {
            return Err("Load cards between dispatches".to_string());
        }
        // Safety: the caller supplied a fully initialized config record.
        if !valid(config, core::mem::size_of::<SrhConfig>()) {
            return Err("Invalid card configuration ABI".to_string());
        }
        let canonical = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
        // A library stays loaded for at least as long as any card created from
        // it, so the cache hands out a shared handle instead of reloading.  The
        // lookup borrow is confined to its own statement so the insert below
        // cannot overlap it.
        let existing = {
            let libraries = self.libraries.borrow();
            libraries.get(&canonical).cloned()
        };
        let library = match existing {
            Some(existing) => existing,
            None => {
                let loaded = Library::open(&canonical)?;
                self.libraries
                    .borrow_mut()
                    .insert(canonical.clone(), Rc::clone(&loaded));
                loaded
            }
        };

        let host = self.host();
        // Safety: `init` came from this library and the host table is live.
        let api = unsafe { (library.init)(host) };
        if !crate::ffi::valid(api, core::mem::size_of::<SrhPlugin>()) {
            return Err(format!(
                "Plugin ABI version/size mismatch: {}",
                canonical.display()
            ));
        }
        if !crate::plugin::plugin_complete(api) {
            return Err(format!(
                "Plugin missing required ABI functions: {}",
                canonical.display()
            ));
        }
        // Safety: `api` was validated above and stays valid while `library`
        // is loaded, which the card holds for its lifetime.  The tail fields
        // are optional and are only read when the declared structure size
        // contains them.
        let saves_project = unsafe {
            has_field(
                api,
                core::mem::offset_of!(SrhPlugin, save_project_data),
                core::mem::size_of::<Option<SrhSaveState>>(),
            ) && (*api).save_project_data.is_some()
        };
        let loads_project = unsafe {
            has_field(
                api,
                core::mem::offset_of!(SrhPlugin, load_project_data),
                core::mem::size_of::<Option<SrhLoadState>>(),
            ) && (*api).load_project_data.is_some()
        };
        if saves_project != loads_project {
            return Err(format!(
                "Plugin must supply both project-data callbacks: {}",
                canonical.display()
            ));
        }
        let id = self.next();
        let type_ = crate::plugin::plugin_id(api);
        // The card keeps the library loaded for its whole life: the concrete
        // handle is leaked into a stable pointer and released when the card is
        // destroyed.
        let card = Rc::new(Card {
            id,
            library: Rc::into_raw(library) as *mut Library,
            api,
            instance: core::cell::Cell::new(std::ptr::null_mut()),
            type_,
            name: core::cell::RefCell::new(String::new()),
            priority: config.priority,
            active: core::cell::Cell::new(true),
            parked: core::cell::Cell::new(false),
            disasm: core::cell::Cell::new(None),
            disasm_context: core::cell::Cell::new(std::ptr::null_mut()),
            disasm_enabled: core::cell::Cell::new(true),
            disasm_revision: core::cell::Cell::new(1),
            disasm_message: core::cell::RefCell::new(String::new()),
            text: core::cell::Cell::new(None),
            text_context: core::cell::Cell::new(std::ptr::null_mut()),
            load_error: String::new(),
        });
        self.cards.borrow_mut().insert(id, Rc::clone(&card));
        self.active_cards.borrow_mut().insert(id);
        self.rack_order.borrow_mut().push(id);

        let mut instance: *mut c_void = std::ptr::null_mut();
        let mut error_message = [0u8; 512];
        let mut create_config = *config;
        create_config.error_message = error_message.as_mut_ptr().cast();
        create_config.error_message_capacity = error_message.len() as u32;
        self.enter_frame();
        let status = unsafe {
            let create = (*api).create.expect("validated plugin has create");
            create(host, id, &create_config, &mut instance)
        };
        card.instance.set(instance);
        let failed = status != SRH_OK || instance.is_null() || !card.is_active();
        if failed {
            let _ = self.remove(id);
        }
        self.leave_frame();
        if failed {
            let nul = error_message
                .iter()
                .position(|byte| *byte == 0)
                .unwrap_or(error_message.len());
            let card_reason = String::from_utf8_lossy(&error_message[..nul]);
            let card_reason = card_reason.trim();
            let reason = if !card_reason.is_empty() {
                card_reason.to_string()
            } else if status == SRH_OK && instance.is_null() {
                "Card did not return an instance".to_string()
            } else if status == SRH_OK {
                "Card removed itself during creation".to_string()
            } else {
                status_reason(status).to_string()
            };
            let reported_status = if status == SRH_OK { SRH_ERROR } else { status };
            return Err(format!(
                "Could not load {}: {reason} (error {reported_status})",
                card.type_
            ));
        }
        Ok(id)
    }

    pub fn set_card_name(&self, card: Handle, name: String) -> SrhStatus {
        let cards = self.cards.borrow();
        let Some(entry) = cards.get(&card) else {
            return SRH_NOT_FOUND;
        };
        if name.len() > 255 {
            return SRH_INVALID;
        }
        *entry.name.borrow_mut() = name;
        SRH_OK
    }

    pub fn set_card_clock(&self, card: Handle, clock: u32) -> SrhStatus {
        if clock >= crate::core::CLOCK_COUNT as u32 || !self.alive(card) {
            return SRH_INVALID;
        }
        let mut moved = Vec::new();
        for events in &self.clock_events {
            let mut events = events.borrow_mut();
            let mut index = 0;
            while index < events.len() {
                if events[index].owner == card {
                    moved.push(events.remove(index));
                } else {
                    index += 1;
                }
            }
        }
        for event in moved {
            event.clock.set(Some(clock));
            let mut destination = self.clock_events[clock as usize].borrow_mut();
            let index = destination.partition_point(|existing| existing.order < event.order);
            destination.insert(index, event);
        }
        self.topology_changed();
        SRH_OK
    }

    pub fn cards(&self) -> Vec<crate::core::CardInfo> {
        self.all_cards()
            .into_iter()
            .filter(|card| card.active || (!card.parked && !card.load_error.is_empty()))
            .collect()
    }

    pub fn all_cards(&self) -> Vec<crate::core::CardInfo> {
        let cards = self.cards.borrow();
        self.rack_order
            .borrow()
            .iter()
            .filter_map(|id| cards.get(id))
            .map(|card| crate::core::CardInfo {
                id: card.id,
                type_: card.type_.clone(),
                name: card.name(),
                priority: card.priority,
                active: card.is_active() && card.load_error.is_empty(),
                parked: card.is_parked(),
                load_error: card.load_error.clone(),
            })
            .collect()
    }

    pub fn removed_cards(&self) -> Vec<crate::core::CardInfo> {
        self.all_cards()
            .into_iter()
            .filter(|card| card.parked)
            .collect()
    }

    pub fn reorder_cards(&self, order: Vec<Handle>) -> SrhStatus {
        if self.depth.get() != 0 {
            return SRH_CONFLICT;
        }
        let cards = self.cards.borrow();
        if order.len() != cards.len() {
            return SRH_INVALID;
        }
        let mut sorted = order.clone();
        sorted.sort();
        let mut expected: Vec<Handle> = cards.keys().copied().collect();
        expected.sort();
        if sorted != expected {
            return SRH_INVALID;
        }
        drop(cards);
        // No callbacks, resets or routing changes: only the rack sequence
        // changes, so instances, handles, clocks, memory and routing survive.
        *self.rack_order.borrow_mut() = order;
        SRH_OK
    }

    pub fn park(&self, owner: Handle) -> SrhStatus {
        {
            let cards = self.cards.borrow();
            let Some(card) = cards.get(&owner) else {
                return SRH_NOT_FOUND;
            };
            if !card.is_active() {
                return SRH_NOT_FOUND;
            }
        }
        self.enter_frame();
        {
            let cards = self.cards.borrow();
            let card = cards.get(&owner).expect("checked above");
            card.active.set(false);
            card.parked.set(true);
            self.topology_changed();
        }
        for input in self.inputs.borrow().iter() {
            if input.owner == owner {
                let wake = input.wake.get();
                if wake != 0 {
                    self.cancel_event(wake);
                }
                input.wake.set(0);
                input.notified.set(false);
            }
        }
        self.active_cards.borrow_mut().remove(&owner);
        let changed = self.owner_signal_ids(owner);
        for id in changed {
            self.recompute(id);
        }
        self.leave_frame();
        SRH_OK
    }

    pub fn plug(&self, owner: Handle) -> SrhStatus {
        {
            let cards = self.cards.borrow();
            let Some(card) = cards.get(&owner) else {
                return SRH_NOT_FOUND;
            };
            if !card.is_parked() || card.is_active() {
                return SRH_NOT_FOUND;
            }
        }
        self.enter_frame();
        {
            let cards = self.cards.borrow();
            let card = cards.get(&owner).expect("checked above");
            card.active.set(true);
            card.parked.set(false);
            self.topology_changed();
        }
        if self.cards.borrow()[&owner].load_error.is_empty() {
            self.active_cards.borrow_mut().insert(owner);
            self.restore_parked_timers(owner);
        }
        self.refresh_inputs();
        let changed = self.owner_signal_ids(owner);
        for id in changed {
            self.recompute(id);
        }
        self.leave_frame();
        SRH_OK
    }

    pub fn put_away(&self, owner: Handle) -> SrhStatus {
        self.remove(owner)
    }

    pub fn remove(&self, owner: Handle) -> SrhStatus {
        {
            let cards = self.cards.borrow();
            let Some(card) = cards.get(&owner) else {
                return SRH_NOT_FOUND;
            };
            if !card.is_active() && !card.is_parked() {
                return SRH_NOT_FOUND;
            }
        }
        self.enter_frame();
        {
            let cards = self.cards.borrow();
            let card = cards.get(&owner).expect("checked above");
            card.active.set(false);
            card.parked.set(false);
        }
        self.active_cards.borrow_mut().remove(&owner);
        self.topology_changed();
        self.collection_pending.set(true);
        for mapping in self.mappings.borrow().iter() {
            if mapping.owner == owner {
                mapping.active.set(false);
            }
        }
        for event in self.events.borrow().iter() {
            if event.owner == owner {
                event.active.set(false);
            }
        }
        for listener in self.listeners.borrow().iter() {
            if listener.owner == owner {
                listener.active.set(false);
            }
        }
        for input in self.inputs.borrow().iter() {
            if input.owner == owner {
                input.active.set(false);
            }
        }
        for video in self.videos.borrow().iter() {
            if video.owner == owner {
                video.active.set(false);
            }
        }
        for source in self.audio_sources.borrow().iter() {
            if source.owner == owner {
                source.active.set(false);
            }
        }
        self.breakpoints
            .borrow_mut()
            .retain(|point| point.card != owner);
        if let Some(boundary) = self.stopped_boundary.get() {
            if boundary[0] == owner {
                self.stopped_boundary.set(None);
            }
        }
        if let Some(boundary) = self.skip_boundary.get() {
            if boundary[0] == owner {
                self.skip_boundary.set(None);
            }
        }
        self.drop_owner_signals(owner);
        self.leave_frame();
        SRH_OK
    }

    // ------------------------------------------------------------------
    // Debugger
    // ------------------------------------------------------------------

    pub fn add_breakpoint(
        &self,
        card: Handle,
        space: Handle,
        first: u64,
        last: u64,
        operations: u32,
    ) -> Result<u64, String> {
        if !self.spaces.borrow().contains_key(&space)
            || first > last
            || last > self.spaces.borrow()[&space].maximum
            || operations > 3
            || (operations == 0 && !self.alive(card))
        {
            return Err("Invalid breakpoint".to_string());
        }
        let id = self.next();
        self.breakpoints.borrow_mut().push(crate::core::Breakpoint {
            id,
            card,
            space,
            first,
            last,
            operations,
            enabled: true,
        });
        Ok(id)
    }

    pub fn boundary(
        &self,
        card: Handle,
        space: Handle,
        pc: u64,
        _instruction_bytes: u32,
        _cycles: u32,
    ) -> SrhStatus {
        if !self.alive(card) {
            return SRH_NOT_FOUND;
        }
        if self.trace_capture.get() {
            let mut sequences = self.instruction_seq.borrow_mut();
            let entry = sequences.entry(card).or_insert(0);
            *entry += 1;
        }
        self.trace_kind.set(0);
        let location = [card, space, pc];
        if self.skip_boundary.get() == Some(location) {
            self.skip_boundary.set(None);
            return SRH_OK;
        }
        let hit = self.breakpoints.borrow().iter().any(|point| {
            point.enabled
                && point.operations == 0
                && point.card == card
                && point.space == space
                && pc >= point.first
                && pc <= point.last
        });
        if hit {
            self.stopped_boundary.set(Some(location));
            self.stop_clocks();
            *self.stop_reason.borrow_mut() = format!(
                "Execution breakpoint before PC {pc} (master clocks stopped; simulation not paused)"
            );
            return SRH_STOP;
        }
        SRH_OK
    }

    pub fn request_stop(&self, card: Handle, reason: &str) -> SrhStatus {
        if !self.alive(card) {
            return SRH_NOT_FOUND;
        }
        self.run_state.set(RunState::Paused);
        *self.stop_reason.borrow_mut() = format!("Card {card} requested stop: {reason}");
        SRH_OK
    }

    pub fn properties(&self, id: Handle) -> Vec<crate::core::Property> {
        if !self.alive(id) {
            return Vec::new();
        }
        let card = match self.cards.borrow().get(&id) {
            Some(card) => Rc::clone(card),
            None => return Vec::new(),
        };
        self.enter_frame();
        let mut result = Vec::new();
        let count = unsafe {
            match (*card.api).property_count {
                Some(count) => count(card.instance()),
                None => 0,
            }
        };
        if count > 1024 {
            self.leave_frame();
            panic!("Excessive plugin property count");
        }
        for index in 0..count {
            if !card.is_active() {
                break;
            }
            let mut property = SrhProperty {
                abi_version: SRH_ABI,
                struct_size: core::mem::size_of::<SrhProperty>() as u32,
                ..Default::default()
            };
            let mut value = SrhValue {
                abi_version: SRH_ABI,
                struct_size: core::mem::size_of::<SrhValue>() as u32,
                ..Default::default()
            };
            // Safety: the card is alive and its plugin is loaded.
            let info = unsafe {
                match (*card.api).property_info {
                    Some(info) => info(card.instance(), index, &mut property),
                    None => SRH_ERROR,
                }
            };
            if info != SRH_OK || !card.is_active() {
                break;
            }
            let get = if property.ui_flags & SRH_PROPERTY_UNAVAILABLE != 0 {
                SRH_OK
            } else {
                unsafe {
                    match (*card.api).property_get {
                        Some(get) => get(card.instance(), index, &mut value),
                        None => SRH_ERROR,
                    }
                }
            };
            if get != SRH_OK {
                break;
            }
            value.text[127] = 0;
            result.push(crate::core::Property {
                name: unsafe { crate::core::cstr(property.name) },
                group: unsafe { crate::core::cstr(property.group) },
                description: unsafe { crate::core::cstr(property.description) },
                enum_labels: unsafe { crate::core::cstr(property.enum_labels) },
                kind: property.kind,
                bits: property.bits,
                base: property.base,
                ui_flags: property.ui_flags,
                editable: property.editable != 0
                    && property.ui_flags & SRH_PROPERTY_UNAVAILABLE == 0,
                value,
            });
        }
        self.leave_frame();
        result
    }

    pub fn edit_property(&self, id: Handle, index: u32, value: &SrhValue) -> SrhStatus {
        if !self.alive(id) || !valid(value, core::mem::size_of::<SrhValue>()) {
            return SRH_INVALID;
        }
        let card = match self.cards.borrow().get(&id) {
            Some(card) => Rc::clone(card),
            None => return SRH_NOT_FOUND,
        };
        self.enter_frame();
        let mut property = SrhProperty {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhProperty>() as u32,
            ..Default::default()
        };
        // Safety: the card is alive and its plugin is loaded.
        let info = unsafe {
            match (*card.api).property_info {
                Some(info) => info(card.instance(), index, &mut property),
                None => SRH_ERROR,
            }
        };
        if info != SRH_OK
            || property.editable == 0
            || property.ui_flags & SRH_PROPERTY_UNAVAILABLE != 0
            || !card.is_active()
        {
            self.leave_frame();
            return SRH_INVALID;
        }
        // Regular properties stay paused/stopped-only; live-debug properties
        // may be changed while running so tools can interact in real time.
        if self.run_state.get() == RunState::Running
            && (property.ui_flags & SRH_PROPERTY_LIVE_EDIT) == 0
        {
            self.leave_frame();
            return SRH_INVALID;
        }
        let result = unsafe {
            match (*card.api).property_set {
                Some(set) => set(card.instance(), index, value),
                None => SRH_ERROR,
            }
        };
        if result == SRH_OK {
            self.stopped_boundary.set(None);
            self.skip_boundary.set(None);
        }
        self.leave_frame();
        result
    }

    pub fn disassemble(&self, card: Handle, space: Handle, pc: u64) -> Disassembly {
        let entry = self.cards.borrow().get(&card).cloned();
        let Some(card) = entry else {
            return Disassembly::default();
        };
        let Some((disasm, context)) = card.disassembler() else {
            return Disassembly::default();
        };
        if !card.is_active() || !card.disasm_enabled.get() {
            return Disassembly::default();
        }
        let revision = card.disasm_revision.get();
        let mut bytes = Vec::new();
        for index in 0..16u64 {
            let mut byte = 0u8;
            if pc > u64::MAX - index || self.read(0, space, pc + index, &mut byte, true) != SRH_OK {
                break;
            }
            if !card.is_active()
                || !card.disasm_enabled.get()
                || card.disasm_revision.get() != revision
            {
                return Disassembly::default();
            }
            bytes.push(byte);
        }
        if bytes.is_empty() {
            return Disassembly::default();
        }
        let mut instruction_bytes = 0u32;
        let mut cycles = 0u32;
        let mut text = [0i8; 256];
        // Safety: the card is alive and registered this disassembler.
        let status = unsafe {
            disasm(
                context,
                card.id,
                space,
                pc,
                bytes.as_ptr(),
                bytes.len() as u32,
                &mut instruction_bytes,
                &mut cycles,
                text.as_mut_ptr(),
                text.len() as u32,
            )
        };
        if status != SRH_OK {
            return Disassembly::default();
        }
        let text = unsafe { CStr::from_ptr(text.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        Disassembly {
            ok: true,
            bytes,
            instruction_bytes,
            cycles,
            text,
        }
    }

    /// Reads a text endpoint window. `capacity` is the caller's requested chunk
    /// size and the returned `u32` is the endpoint's full length, which may
    /// exceed the chunk: callers page by advancing `offset` until they reach it.
    pub fn text_query(
        &self,
        card: Handle,
        offset: u64,
        capacity: usize,
    ) -> Result<(Vec<u8>, u32), SrhStatus> {
        let entry = self.cards.borrow().get(&card).cloned();
        let Some(card) = entry else {
            return Err(SRH_NOT_FOUND);
        };
        let Some((query, context)) = card.text_provider() else {
            return Err(SRH_NOT_FOUND);
        };
        if !card.is_active() {
            return Err(SRH_NOT_FOUND);
        }
        // A zero-capacity probe still needs a valid pointer, but the callback
        // must never see a capacity larger than the caller can receive.
        let mut buffer = vec![0u8; capacity.max(1)];
        let mut size = capacity as u32;
        let mut total = 0u32;
        // Safety: the card is alive and registered this query callback.
        let status = unsafe { query(context, offset, buffer.as_mut_ptr(), &mut size, &mut total) };
        if status != SRH_OK {
            return Err(status);
        }
        buffer.truncate((size as usize).min(capacity));
        Ok((buffer, total))
    }

    pub fn text_endpoints(&self) -> Vec<crate::core::TextEndpointInfo> {
        let cards = self.cards.borrow();
        let inputs = self.inputs.borrow();
        let mut result = Vec::new();
        for (id, card) in cards.iter() {
            if !card.is_active() || card.text.get().is_none() {
                continue;
            }
            let names = inputs
                .iter()
                .filter(|input| input.active.get() && input.owner == *id)
                .map(|input| input.name.clone())
                .collect();
            result.push(crate::core::TextEndpointInfo {
                card: *id,
                inputs: names,
            });
        }
        result
    }

    // ------------------------------------------------------------------
    // Video
    // ------------------------------------------------------------------

    pub fn video_surfaces(&self) -> Vec<crate::core::VideoSurface> {
        self.videos
            .borrow()
            .iter()
            .filter(|video| video.active.get() && self.alive(video.owner))
            .map(|video| crate::core::VideoSurface {
                id: video.id,
                owner: video.owner,
                width: video.width,
                height: video.height,
                format: video.format,
                flags: video.flags,
            })
            .collect()
    }

    pub fn register_video(
        &self,
        owner: Handle,
        width: u32,
        height: u32,
        format: SrhVideoFormat,
        flags: u32,
        query: SrhVideoQuery,
        context: *mut c_void,
        result: *mut Handle,
    ) -> SrhStatus {
        if result.is_null()
            || !self.alive(owner)
            || width == 0
            || height == 0
            || format != SRH_VIDEO_RGBA8
            || flags & !SRH_VIDEO_ALLOW_SHADER != 0
            || (width as u64) * (height as u64) > (u32::MAX / 4) as u64
        {
            return SRH_INVALID;
        }
        let id = self.next();
        self.videos
            .borrow_mut()
            .push(Rc::new(crate::core::VideoRegistration {
                timing: core::cell::Cell::new(None),
                id,
                owner,
                width,
                height,
                format,
                flags,
                query,
                context,
                active: core::cell::Cell::new(true),
            }));
        // Safety: the caller supplied a valid output pointer.
        unsafe { *result = id };
        SRH_OK
    }

    pub fn set_video_timing(
        &self,
        surface: Handle,
        query: Option<SrhVideoTimingQuery>,
        context: *mut c_void,
    ) -> SrhStatus {
        let Some(query) = query else {
            return SRH_INVALID;
        };
        let entry = self
            .videos
            .borrow()
            .iter()
            .find(|v| v.id == surface && v.active.get() && self.alive(v.owner))
            .cloned();
        let Some(video) = entry else {
            return SRH_NOT_FOUND;
        };
        video.timing.set(Some((query, context)));
        SRH_OK
    }

    pub fn video_timing(&self, surface: Handle, output: *mut SrhVideoTiming) -> SrhStatus {
        if output.is_null()
            || unsafe {
                (*output).abi_version != SRH_ABI
                    || (*output).struct_size < core::mem::size_of::<SrhVideoTiming>() as u32
            }
        {
            return SRH_INVALID;
        }
        let entry = self
            .videos
            .borrow()
            .iter()
            .find(|v| v.id == surface && v.active.get() && self.alive(v.owner))
            .cloned();
        let Some(video) = entry else {
            return SRH_NOT_FOUND;
        };
        let Some((query, context)) = video.timing.get() else {
            return SRH_UNAVAILABLE;
        };
        let mut timing = SrhVideoTiming {
            abi_version: SRH_ABI,
            struct_size: core::mem::size_of::<SrhVideoTiming>() as u32,
            frame_number: 0,
            scanline: 0,
            line_count: 0,
        };
        self.enter_frame();
        let status = unsafe { query(context, &mut timing) };
        self.leave_frame();
        if status != SRH_OK {
            return status;
        }
        if timing.abi_version != SRH_ABI
            || timing.struct_size < core::mem::size_of::<SrhVideoTiming>() as u32
            || timing.line_count == 0
            || timing.scanline >= timing.line_count
        {
            return SRH_INVALID;
        }
        unsafe {
            *output = timing;
        }
        SRH_OK
    }

    pub fn video_read(
        &self,
        surface: Handle,
        offset: u64,
        buffer: *mut u8,
        size: *mut u32,
        total: *mut u32,
    ) -> SrhStatus {
        if size.is_null() || total.is_null() || (buffer.is_null() && unsafe { *size } != 0) {
            return SRH_INVALID;
        }
        let entry = self
            .videos
            .borrow()
            .iter()
            .find(|video| video.id == surface && video.active.get())
            .cloned();
        let Some(video) = entry else {
            return SRH_NOT_FOUND;
        };
        if !self.alive(video.owner) {
            return SRH_NOT_FOUND;
        }
        self.enter_frame();
        let capacity = unsafe { *size };
        // Safety: the caller provided a buffer of `capacity` bytes.
        let status = unsafe { (video.query)(video.context, offset, buffer, size, total) };
        let result = if status == SRH_OK
            && (unsafe { *size } > capacity
                || unsafe { *total } != video.width.wrapping_mul(video.height).wrapping_mul(4))
        {
            SRH_INVALID
        } else {
            status
        };
        self.leave_frame();
        result
    }
}

fn status_reason(status: SrhStatus) -> &'static str {
    match status {
        SRH_ERROR => "Card initialization failed",
        SRH_INVALID => "Invalid card configuration",
        SRH_NOT_FOUND => "Required resource not found",
        SRH_UNAVAILABLE => "Required resource unavailable",
        SRH_STOP => "Card initialization stopped",
        SRH_CONFLICT => "Resource conflict",
        _ => "Unknown card error",
    }
}

/// The outcome of a card disassembly request.
#[derive(Default)]
pub struct Disassembly {
    pub ok: bool,
    pub bytes: Vec<u8>,
    pub instruction_bytes: u32,
    pub cycles: u32,
    pub text: String,
}

impl Core {
    // ------------------------------------------------------------------
    // Host configuration
    // ------------------------------------------------------------------

    /// Registers plugin-owned setting metadata.  The engine copies the
    /// metadata and keeps the plugin's callbacks; `context` identifies the
    /// registration for removal.
    ///
    /// # Safety
    /// `entry` must satisfy [`valid`]; the pointer fields inside it are read
    /// through `cstr`, and the callbacks are invoked only while the owning
    /// plugin is loaded.
    pub fn register_config_entry(&self, entry: &SrhConfigEntry) -> bool {
        if !valid(entry, core::mem::size_of::<SrhConfigEntry>())
            || entry.name.is_null()
            || unsafe { *entry.name } == 0
            || entry.label.is_null()
            || entry.get.is_none()
            || entry.set.is_none()
            || entry.kind > Srh_CONFIG_PATH
        {
            return false;
        }
        let name = unsafe { crate::core::cstr(entry.name) };
        let label = unsafe { crate::core::cstr(entry.label) };
        let category = match unsafe { crate::core::cstr(entry.category) } {
            text if text.is_empty() => "General".to_string(),
            text => text,
        };
        let description = unsafe { crate::core::cstr(entry.description) };
        let enum_labels = unsafe { crate::core::cstr(entry.enum_labels) };
        let default_value = unsafe { crate::core::cstr(entry.default_value) };
        let provider = self
            .cards
            .borrow()
            .get(&entry.owner)
            .map(|card| card.type_.clone())
            .unwrap_or_default();

        let same_schema = |existing: &crate::core::ConfigEntry| {
            existing.name == name
                && existing.category == category
                && existing.provider == provider
                && existing.label == label
                && existing.description == description
                && existing.type_ == entry.kind
                && existing.enum_labels == enum_labels
                && existing.default_value == default_value
        };
        let duplicate = self
            .config_entries
            .borrow()
            .iter()
            .find(|existing| existing.name == name)
            .map(same_schema);
        // Multiple instances of one card may expose the same plugin-wide
        // setting.  Keep one registration per instance so removing either card
        // cannot make the setting disappear while another instance is alive.
        if duplicate == Some(false) {
            return false;
        }
        if !self.config.borrow().has(&name) && !default_value.is_empty() {
            self.config.borrow_mut().set(&name, &default_value);
        }
        self.config_entries
            .borrow_mut()
            .push(crate::core::ConfigEntry {
                category,
                name,
                label,
                description,
                enum_labels,
                default_value,
                provider,
                type_: entry.kind,
                context: entry.context,
                owner: entry.owner,
                get: entry.get,
                set: entry.set,
            });
        true
    }

    pub fn unregister_config_entries(&self, context: *mut core::ffi::c_void) {
        self.config_entries
            .borrow_mut()
            .retain(|entry| entry.context != context);
    }

    /// Reads one registered setting through its owning plugin's callback.
    pub fn config_entry_get(&self, name: &str) -> Result<String, SrhStatus> {
        let entry = self
            .config_entries
            .borrow()
            .iter()
            .find(|entry| entry.name == name)
            .cloned();
        let Some(entry) = entry else {
            return Err(SRH_NOT_FOUND);
        };
        let Some(get) = entry.get else {
            return Err(SRH_NOT_FOUND);
        };
        let mut buffer = [0 as core::ffi::c_char; 4096];
        // Safety: the callback belongs to a loaded plugin and the buffer is
        // host-owned and valid for the call.
        let status = unsafe { get(entry.context, buffer.as_mut_ptr(), buffer.len() as u32) };
        if status != SRH_OK {
            return Err(status);
        }
        Ok(unsafe { crate::core::cstr(buffer.as_ptr()) })
    }

    /// Writes one registered setting through every instance's callback and
    /// then records the new canonical value.
    pub fn config_entry_set(&self, name: &str, value: &str) -> SrhStatus {
        let entries: Vec<crate::core::ConfigEntry> = self
            .config_entries
            .borrow()
            .iter()
            .filter(|entry| entry.name == name)
            .cloned()
            .collect();
        if entries.is_empty() {
            return SRH_NOT_FOUND;
        }
        let text = match std::ffi::CString::new(value) {
            Ok(text) => text,
            Err(_) => return SRH_INVALID,
        };
        for entry in entries {
            let Some(set) = entry.set else {
                continue;
            };
            // Safety: the callback belongs to a loaded plugin and the string
            // outlives the call.
            let status = unsafe { set(entry.context, text.as_ptr()) };
            if status != SRH_OK {
                return status;
            }
        }
        self.config.borrow_mut().set(name, value);
        SRH_OK
    }

    pub fn config_value(&self, key: &str, fallback: &str) -> String {
        self.config.borrow().value(key, fallback)
    }

    pub fn config_set(&self, key: &str, value: &str) {
        self.config.borrow_mut().set(key, value);
    }
}
