//! Deterministic byte-input queues.
//!
//! An endpoint is a named, bounded FIFO owned by the host.  Records are kept
//! sorted by `(time, injection sequence)`; a due-record transition schedules a
//! single wakeup so a card can be notified without polling, and cancelling the
//! subscription or reaching the records cancels that wakeup exactly.

use core::cell::Cell;
use core::ffi::c_void;
use std::rc::Rc;

use crate::core::{Core, Handle, InputRecord};
use crate::ffi::*;

/// Chronological insertion point for a batch, compared on the record time.
fn insertion_index(records: &[InputRecord], timestamp: u64) -> usize {
    records
        .iter()
        .position(|record| record.first > timestamp)
        .unwrap_or(records.len())
}

impl Core {
    pub fn register_input(
        &self,
        owner: Handle,
        name: &str,
        capacity: u32,
        result: *mut Handle,
    ) -> SrhStatus {
        if result.is_null() || name.is_empty() || !self.alive(owner) {
            return SRH_INVALID;
        }
        if self
            .inputs
            .borrow()
            .iter()
            .any(|endpoint| endpoint.active.get() && endpoint.name == name)
        {
            return SRH_CONFLICT;
        }
        let capacity = if capacity == 0 { 1024 } else { capacity };
        let id = self.next();
        self.inputs
            .borrow_mut()
            .push(Rc::new(crate::core::InputEndpoint {
                id,
                owner,
                name: name.to_string(),
                capacity,
                records: core::cell::RefCell::new(std::collections::VecDeque::new()),
                active: Cell::new(true),
                subscription: Cell::new(0),
                wake: Cell::new(0),
                callback: Cell::new(None),
                context: Cell::new(core::ptr::null_mut()),
                notified: Cell::new(false),
            }));
        // Safety: the caller supplied a valid output pointer.
        unsafe { *result = id };
        SRH_OK
    }

    /// Finds the live endpoint with `handle` whose owner is alive.
    pub fn live_endpoint(&self, handle: Handle) -> Option<Rc<crate::core::InputEndpoint>> {
        self.inputs
            .borrow()
            .iter()
            .find(|endpoint| {
                endpoint.id == handle && endpoint.active.get() && self.alive(endpoint.owner)
            })
            .cloned()
    }

    pub fn input_due(&self, endpoint: Handle, count: *mut u64) -> SrhStatus {
        if count.is_null() {
            return SRH_INVALID;
        }
        let Some(entry) = self.live_endpoint(endpoint) else {
            return SRH_NOT_FOUND;
        };
        let now = self.now.get();
        let due = entry
            .records
            .borrow()
            .iter()
            .filter(|record| record.first <= now)
            .count() as u64;
        // Safety: the caller supplied a valid output pointer.
        unsafe { *count = due };
        SRH_OK
    }

    pub fn input_pop(&self, endpoint: Handle, timestamp: *mut u64, value: *mut u8) -> SrhStatus {
        if timestamp.is_null() || value.is_null() {
            return SRH_INVALID;
        }
        let Some(entry) = self.live_endpoint(endpoint) else {
            return SRH_NOT_FOUND;
        };
        {
            let mut records = entry.records.borrow_mut();
            match records.front() {
                None => return SRH_UNAVAILABLE,
                Some(record) if record.first > self.now.get() => return SRH_UNAVAILABLE,
                Some(record) => {
                    // Safety: the caller supplied valid output pointers.
                    unsafe {
                        *timestamp = record.first;
                        *value = record.second;
                    }
                    records.pop_front();
                }
            }
        }
        self.arm_input(&entry);
        SRH_OK
    }

    pub fn enqueue_input(&self, endpoint: &str, timestamp: u64, value: u8) -> Result<(), String> {
        let status = self.enqueue_input_batch(endpoint, timestamp, &[value], 0, 0);
        if status != SRH_OK {
            return Err(format!("Input batch rejected: {endpoint}"));
        }
        Ok(())
    }

    pub fn enqueue_input_batch(
        &self,
        name: &str,
        timestamp: u64,
        bytes: &[u8],
        source: u64,
        expected_owner: Handle,
    ) -> SrhStatus {
        if name.is_empty() || bytes.is_empty() || bytes.len() > 65536 {
            return SRH_INVALID;
        }
        let entry = {
            let inputs = self.inputs.borrow();
            match inputs.iter().find(|endpoint| {
                endpoint.active.get() && self.alive(endpoint.owner) && endpoint.name == name
            }) {
                Some(endpoint) => Rc::clone(endpoint),
                None => return SRH_NOT_FOUND,
            }
        };
        if expected_owner != 0 && entry.owner != expected_owner {
            return SRH_CONFLICT;
        }
        if bytes.len() as u64 > entry.capacity as u64 - entry.records.borrow().len() as u64
            || bytes.len() as u64 > u64::MAX - self.input_order.get()
        {
            return SRH_UNAVAILABLE;
        }
        // Allocate before committing: neither a capacity nor an allocation
        // failure may insert a prefix of the batch.
        let existing: Vec<InputRecord> = entry.records.borrow().iter().copied().collect();
        let position = insertion_index(&existing, timestamp);
        let mut records: Vec<InputRecord> = Vec::with_capacity(existing.len() + bytes.len());
        records.extend_from_slice(&existing[..position]);
        for byte in bytes {
            records.push(InputRecord {
                first: timestamp,
                second: *byte,
                source,
            });
        }
        records.extend_from_slice(&existing[position..]);
        *entry.records.borrow_mut() = records.into();
        self.input_order
            .set(self.input_order.get() + bytes.len() as u64);
        self.arm_input(&entry);
        SRH_OK
    }

    pub fn cancel_input_source(&self, source: u64) {
        if source == 0 {
            return;
        }
        // Rearming cancels wakeups. Defer any pending collection until the
        // endpoint-list borrow has ended, including full owner cleanup.
        self.enter_frame();
        for entry in self.inputs.borrow().iter() {
            entry
                .records
                .borrow_mut()
                .retain(|record| record.source != source);
            self.arm_input(entry);
        }
        self.leave_frame();
    }

    pub fn subscribe_input_due(
        &self,
        owner: Handle,
        endpoint: Handle,
        callback: SrhCallback,
        context: *mut c_void,
        result: *mut Handle,
    ) -> SrhStatus {
        if result.is_null() || !self.alive(owner) {
            return SRH_INVALID;
        }
        for entry in self.inputs.borrow().iter() {
            if !entry.active.get() || entry.id != endpoint || entry.owner != owner {
                continue;
            }
            if entry.subscription.get() != 0 {
                return SRH_CONFLICT;
            }
            let subscription = self.next();
            entry.subscription.set(subscription);
            entry.callback.set(Some(callback));
            entry.context.set(context);
            entry.notified.set(false);
            // Safety: the caller supplied a valid output pointer.
            unsafe { *result = subscription };
            self.arm_input(entry);
            return SRH_OK;
        }
        SRH_NOT_FOUND
    }

    /// (Re)arms the single wakeup that reports a due record to the subscriber.
    pub fn arm_input(&self, entry: &Rc<crate::core::InputEndpoint>) {
        let due = entry
            .records
            .borrow()
            .front()
            .map(|record| record.first <= self.now.get())
            .unwrap_or(false);
        if !due {
            entry.notified.set(false);
        }
        let existing_wake = entry.wake.get();
        if existing_wake != 0 {
            self.cancel_event(existing_wake);
            entry.wake.set(0);
        }
        if !entry.active.get()
            || !self.alive(entry.owner)
            || entry.callback.get().is_none()
            || entry.records.borrow().is_empty()
            || entry.notified.get()
        {
            return;
        }
        let delay = if due {
            0
        } else {
            entry
                .records
                .borrow()
                .front()
                .expect("records are not empty")
                .first
                - self.now.get()
        };
        let context = Rc::as_ptr(entry) as *mut c_void;
        let mut wake = 0u64;
        let status = self.schedule(
            entry.owner,
            delay,
            input_wake,
            context,
            &mut wake as *mut Handle,
        );
        if status == SRH_OK {
            entry.wake.set(wake);
        }
    }

    pub fn refresh_inputs(&self) {
        for entry in self.inputs.borrow().iter() {
            self.arm_input(entry);
        }
    }

    /// Installs the persistent deterministic input script.  Records are sorted
    /// by time (stable, preserving the original sequence) and split into
    /// batches so installation stays linear in batch count.
    pub fn set_input_records(&self, records: Vec<(u64, String, Vec<u8>)>) -> Result<(), String> {
        let mut records = records;
        records.sort_by_key(|entry| entry.0);
        for (timestamp, endpoint, bytes) in records {
            let mut offset = 0usize;
            while offset < bytes.len() {
                let end = (offset + 65536).min(bytes.len());
                if self.enqueue_input_batch(&endpoint, timestamp, &bytes[offset..end], 0, 0)
                    != SRH_OK
                {
                    return Err(format!("Project input rejected: {endpoint}"));
                }
                offset = end;
            }
        }
        Ok(())
    }

    pub fn input_endpoints(&self) -> Vec<String> {
        self.inputs
            .borrow()
            .iter()
            .filter(|endpoint| endpoint.active.get() && self.alive(endpoint.owner))
            .map(|endpoint| endpoint.name.clone())
            .collect()
    }

    pub fn input_records(&self, persistent_only: bool) -> Vec<(u64, String, u8)> {
        let mut result = Vec::new();
        for endpoint in self.inputs.borrow().iter() {
            if !endpoint.active.get() || !self.alive(endpoint.owner) {
                continue;
            }
            for record in endpoint.records.borrow().iter() {
                if !persistent_only || record.source == 0 {
                    result.push((record.first, endpoint.name.clone(), record.second));
                }
            }
        }
        result
    }
}

/// Scheduler callback for a due-record notification.  The context is an
/// `InputEndpoint` allocation owned by the input registry.
unsafe extern "C" fn input_wake(context: *mut c_void) -> SrhStatus {
    if context.is_null() {
        return SRH_OK;
    }
    // Safety: the scheduler only invokes this callback with the endpoint
    // pointer that armed it, which the input registry keeps alive.
    let entry = unsafe { &*(context as *const crate::core::InputEndpoint) };
    entry.wake.set(0);
    let Some(active_core) = crate::core::active_core_ref() else {
        return SRH_OK;
    };
    if !entry.active.get() || !active_core.alive(entry.owner) || entry.callback.get().is_none() {
        return SRH_OK;
    }
    let due = entry
        .records
        .borrow()
        .front()
        .map(|record| record.first <= active_core.now.get())
        .unwrap_or(false);
    if !due {
        return SRH_OK;
    }
    entry.notified.set(true);
    match entry.callback.get() {
        // Safety: the card registered this callback and is still alive, and the
        // callback pointer came from the card ABI.
        Some(callback) => unsafe { callback(entry.context.get()) },
        None => SRH_OK,
    }
}
