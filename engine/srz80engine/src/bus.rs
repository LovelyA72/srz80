//! Address mappings and bus dispatch.
//!
//! Reads resolve by priority (first responder wins) or, in a per-space byte-OR
//! space, by OR-ing every responder.  Writes fan out to every applicable
//! mapping.  A mapping registered during a dispatch becomes visible only at the
//! next dispatch boundary: [`Mapping::visible`] is cleared until the enclosing
//! frame unwinds, and the route cache is invalidated whenever the mapping set
//! changes.

use std::cell::Cell;
use std::rc::Rc;

use crate::core::{
    Core, Handle, Mapping, MappingApi, Resolver, RouteRegion, Trace, TRACE_CAPACITY,
};
use crate::ffi::*;

impl Core {
    pub fn mapping(&self, owner: Handle, mapping: &SrhMapping, result: *mut Handle) -> SrhStatus {
        if result.is_null() {
            return SRH_INVALID;
        }
        // Safety: the caller validated the mapping header before calling.
        if !valid(mapping, core::mem::size_of::<SrhMapping>()) {
            return SRH_INVALID;
        }
        if !self.alive(owner) {
            return SRH_INVALID;
        }
        if !self.spaces.borrow().contains_key(&mapping.space) {
            return SRH_INVALID;
        }
        if mapping.first > mapping.last {
            return SRH_INVALID;
        }
        let maximum = self.spaces.borrow()[&mapping.space].maximum;
        if mapping.last > maximum {
            return SRH_INVALID;
        }
        if mapping.read.is_none() && mapping.write.is_none() {
            return SRH_INVALID;
        }
        let id = self.next();
        let api = MappingApi {
            space: mapping.space,
            first: mapping.first,
            last: mapping.last,
            priority: mapping.priority,
            context: mapping.context,
            read: mapping.read,
            write: mapping.write,
            peek: mapping.peek,
            read_word: mapping.read_word,
        };
        self.mappings.borrow_mut().push(Rc::new(Mapping {
            id,
            owner,
            api,
            active: Cell::new(true),
            // A mapping created inside a dispatch stays invisible until the
            // enclosing frame unwinds, exactly like the C++ core.
            visible: Cell::new(self.depth.get() == 0),
        }));
        if self.depth.get() != 0 {
            self.collection_pending.set(true);
        } else {
            self.route_cache.borrow_mut().clear();
        }
        // Safety: the caller supplied a valid output pointer.
        unsafe { *result = id };
        SRH_OK
    }

    pub fn unmap(&self, id: Handle) -> SrhStatus {
        {
            let mappings = self.mappings.borrow();
            let Some(mapping) = mappings
                .iter()
                .find(|entry| entry.id == id && entry.active.get())
            else {
                return SRH_NOT_FOUND;
            };
            mapping.active.set(false);
        }
        self.collection_pending.set(true);
        self.collect();
        SRH_OK
    }

    /// Mapping handles for `space`/`address`, ordered by descending priority
    /// and then by ascending mapping id. Cached immutable shared slices avoid
    /// allocating on cache hits. The returned handle owns a snapshot so
    /// the caller may invoke plugin code while holding it.
    pub fn routes(&self, space: Handle, address: u64) -> Rc<[Rc<Mapping>]> {
        {
            let mut cache = self.route_cache.borrow_mut();
            if let Some(region) = &cache.recent {
                if region.space == space && region.first <= address && address <= region.last {
                    return Rc::clone(&region.mappings);
                }
            }
            if let Some((_, region)) = cache.regions.range(..=(space, address)).next_back() {
                if region.space == space && address <= region.last {
                    let region = Rc::clone(region);
                    let result = Rc::clone(&region.mappings);
                    cache.recent = Some(region);
                    return result;
                }
            }
        }
        let mut first = 0;
        let mut last = u64::MAX;
        let mut result: Vec<Rc<Mapping>> = self
            .mappings
            .borrow()
            .iter()
            .filter(|mapping| {
                if !mapping.active.get() || !mapping.visible.get() || mapping.api.space != space {
                    return false;
                }
                // Include every mapping boundary, even boundaries of mappings
                // which do not cover this address. No cached interval may span
                // a change in responders (including overlapping mappings).
                if address < mapping.api.first {
                    last = last.min(mapping.api.first - 1);
                    false
                } else if address > mapping.api.last {
                    first = first.max(mapping.api.last + 1);
                    false
                } else {
                    first = first.max(mapping.api.first);
                    last = last.min(mapping.api.last);
                    true
                }
            })
            .cloned()
            .collect();
        result.sort_by(|a, b| {
            if a.api.priority == b.api.priority {
                a.id.cmp(&b.id)
            } else {
                b.api.priority.cmp(&a.api.priority)
            }
        });
        let result: Rc<[Rc<Mapping>]> = result.into();
        let region = Rc::new(RouteRegion {
            space,
            first,
            last,
            mappings: Rc::clone(&result),
        });
        let mut cache = self.route_cache.borrow_mut();
        cache.regions.insert((space, first), Rc::clone(&region));
        cache.recent = Some(region);
        result
    }

    pub fn record(&self, trace: Trace) {
        let mut records = self.trace.borrow_mut();
        if records.len() == TRACE_CAPACITY {
            records.pop_front();
            self.dropped.set(self.dropped.get() + 1);
        }
        records.push_back(trace);
    }

    fn watch(&self, trace: &Trace) {
        let hit = self.breakpoints.borrow().iter().any(|point| {
            point.enabled
                && (point.operations & trace.operation) != 0
                && point.space == trace.space
                && trace.address >= point.first
                && trace.address <= point.last
        });
        if hit {
            self.stop_clocks();
            *self.stop_reason.borrow_mut() = format!(
                "Watchpoint after transaction #{} (master clocks stopped; simulation not paused)",
                trace.sequence
            );
        }
    }

    fn watch_simple(&self, space: Handle, address: u64, operation: u32, reason: &str) {
        let hit = self.breakpoints.borrow().iter().any(|point| {
            point.enabled
                && (point.operations & operation) != 0
                && point.space == space
                && address >= point.first
                && address <= point.last
        });
        if hit {
            self.stop_clocks();
            *self.stop_reason.borrow_mut() = reason.to_string();
        }
    }

    pub fn read(
        &self,
        master: Handle,
        space: Handle,
        address: u64,
        value: &mut u8,
        peek: bool,
    ) -> SrhStatus {
        if master != 0 && !self.alive(master) {
            return SRH_INVALID;
        }
        {
            let spaces = self.spaces.borrow();
            let Some(entry) = spaces.get(&space) else {
                return SRH_INVALID;
            };
            if address > entry.maximum {
                return SRH_INVALID;
            }
        }
        self.enter_frame();
        let result = self.read_inner(master, space, address, value, peek);
        self.leave_frame();
        result
    }

    fn read_inner(
        &self,
        master: Handle,
        space: Handle,
        address: u64,
        value: &mut u8,
        peek: bool,
    ) -> SrhStatus {
        // Copy only bus policy; cloning Space also allocates its name per read.
        let (resolver, random, fallback) = {
            let spaces = self.spaces.borrow();
            let entry = &spaces[&space];
            (entry.resolver, entry.random, entry.fallback)
        };
        let traced = (self.trace_capture.get()
            && (self.trace_capture_operations.get() & SRH_READ) != 0)
            || peek;

        if !traced {
            let mut driven = false;
            *value = 0;
            for mapping in self.routes(space, address).iter() {
                if !mapping.active.get() || !self.alive(mapping.owner) {
                    continue;
                }
                let Some(read) = mapping.api.read else {
                    continue;
                };
                let mut byte = 0u8;
                // Safety: `context` belongs to the mapping owner, which is
                // alive, and the callback was declared by that plugin.
                let status = unsafe { read(mapping.api.context, address, &mut byte) };
                if status == SRH_UNAVAILABLE {
                    continue;
                }
                if status != SRH_OK {
                    return status;
                }
                if !driven {
                    *value = byte;
                } else if resolver == Resolver::BitOr {
                    *value |= byte;
                }
                driven = true;
            }
            if !driven {
                *value = if random {
                    self.random() as u8
                } else {
                    fallback
                };
            }
            self.watch_simple(space, address, SRH_READ, "Watchpoint after read");
            return SRH_OK;
        }

        let mut trace = Trace {
            sequence: if peek { 0 } else { self.sequence.get() },
            parent: self.trace_stack.borrow().last().copied().unwrap_or(0),
            depth: self.trace_stack.borrow().len() as u32,
            time: self.now.get(),
            master,
            space,
            address,
            operation: SRH_READ,
            kind: self.trace_kind.get(),
            instruction: self
                .instruction_seq
                .borrow()
                .get(&master)
                .copied()
                .unwrap_or(0),
            ..Default::default()
        };
        if !peek {
            self.sequence.set(trace.sequence + 1);
        }
        for index in 0..crate::core::CLOCK_COUNT {
            trace.ticks[index] = self.clocks[index].get().ticks;
        }
        if !peek {
            self.trace_stack.borrow_mut().push(trace.sequence);
        }
        let mut driven = false;
        let mut resolved = SRH_OK;
        *value = 0;
        for mapping in self.routes(space, address).iter() {
            if !mapping.active.get() || !self.alive(mapping.owner) {
                continue;
            }
            let callback = if peek {
                mapping.api.peek
            } else {
                mapping.api.read
            };
            let Some(callback) = callback else {
                if peek {
                    resolved = SRH_UNAVAILABLE;
                    break;
                }
                continue;
            };
            let mut byte = 0u8;
            // Safety: see the untraced path above.
            let status = unsafe { callback(mapping.api.context, address, &mut byte) };
            if status == SRH_UNAVAILABLE {
                continue;
            }
            if status != SRH_OK {
                resolved = status;
                break;
            }
            trace.responders.push(mapping.owner);
            if !driven {
                *value = byte;
            } else if resolver == Resolver::BitOr {
                *value |= byte;
            }
            driven = true;
        }
        if !driven && resolved == SRH_OK {
            if peek && random {
                resolved = SRH_UNAVAILABLE;
            } else {
                *value = if random {
                    self.random() as u8
                } else {
                    fallback
                };
            }
        }
        trace.value = *value;
        trace.result = resolved;
        if !peek {
            self.trace_stack.borrow_mut().pop();
            self.watch(&trace);
            self.record(trace);
        }
        resolved
    }

    pub fn write(&self, master: Handle, space: Handle, address: u64, value: u8) -> SrhStatus {
        if master != 0 && !self.alive(master) {
            return SRH_INVALID;
        }
        {
            let spaces = self.spaces.borrow();
            let Some(entry) = spaces.get(&space) else {
                return SRH_INVALID;
            };
            if address > entry.maximum {
                return SRH_INVALID;
            }
        }
        self.enter_frame();
        let result = self.write_inner(master, space, address, value);
        self.leave_frame();
        result
    }

    fn write_inner(&self, master: Handle, space: Handle, address: u64, value: u8) -> SrhStatus {
        let traced =
            self.trace_capture.get() && (self.trace_capture_operations.get() & SRH_WRITE) != 0;
        if !traced {
            let mut result = SRH_OK;
            for mapping in self.routes(space, address).iter() {
                if !mapping.active.get() || !self.alive(mapping.owner) {
                    continue;
                }
                let Some(write) = mapping.api.write else {
                    continue;
                };
                // Safety: see `read`.
                let status = unsafe { write(mapping.api.context, address, value) };
                if status != SRH_OK {
                    result = status;
                }
            }
            self.watch_simple(space, address, SRH_WRITE, "Watchpoint after write");
            return result;
        }

        let mut trace = Trace {
            sequence: self.sequence.get(),
            parent: self.trace_stack.borrow().last().copied().unwrap_or(0),
            depth: self.trace_stack.borrow().len() as u32,
            time: self.now.get(),
            master,
            space,
            address,
            operation: SRH_WRITE,
            kind: self.trace_kind.get(),
            instruction: self
                .instruction_seq
                .borrow()
                .get(&master)
                .copied()
                .unwrap_or(0),
            value,
            ..Default::default()
        };
        self.sequence.set(trace.sequence + 1);
        for index in 0..crate::core::CLOCK_COUNT {
            trace.ticks[index] = self.clocks[index].get().ticks;
        }
        self.trace_stack.borrow_mut().push(trace.sequence);
        for mapping in self.routes(space, address).iter() {
            if !mapping.active.get() || !self.alive(mapping.owner) {
                continue;
            }
            let Some(write) = mapping.api.write else {
                continue;
            };
            // Safety: see `read`.
            let status = unsafe { write(mapping.api.context, address, value) };
            trace.responders.push(mapping.owner);
            if status != SRH_OK {
                trace.result = status;
            }
        }
        self.trace_stack.borrow_mut().pop();
        let result = trace.result;
        self.watch(&trace);
        self.record(trace);
        result
    }

    /// Contiguous, side-effect-free little-endian read.  It succeeds only when
    /// the byte-bus semantics can be preserved: one mapping covering all four
    /// bytes, no active tracing and no breakpoints.
    pub fn read_word(
        &self,
        master: Handle,
        space: Handle,
        address: u64,
        value: &mut u32,
    ) -> SrhStatus {
        if self.trace_capture.get()
            || (master != 0 && !self.alive(master))
            || !self.spaces.borrow().contains_key(&space)
            || address > u64::MAX - 3
            || !self.breakpoints.borrow().is_empty()
        {
            return SRH_UNAVAILABLE;
        }
        if self.spaces.borrow()[&space].resolver != Resolver::Priority {
            return SRH_UNAVAILABLE;
        }
        let first = self.routes(space, address);
        if first.len() != 1 {
            return SRH_UNAVAILABLE;
        }
        let mapping = Rc::clone(&first[0]);
        let Some(read_word) = mapping.api.read_word else {
            return SRH_UNAVAILABLE;
        };
        if !mapping.active.get() || !self.alive(mapping.owner) {
            return SRH_UNAVAILABLE;
        }
        // routes() just selected this maximal interval. Prove all four bytes
        // have the same candidates without doing three more bus lookups.
        if self
            .route_cache
            .borrow()
            .recent
            .as_ref()
            .map_or(true, |region| address + 3 > region.last)
        {
            return SRH_UNAVAILABLE;
        }
        self.enter_frame();
        let mut word = 0u32;
        // Safety: the mapping owner is alive and declared this callback.
        let status = unsafe { read_word(mapping.api.context, address, &mut word) };
        self.leave_frame();
        if status == SRH_OK {
            *value = word;
        }
        status
    }
}
