//! Clocks, scheduled events, run states, the synchronous run loop and signal
//! resolution.
//!
//! Persistent subscribers are indexed by master clock and one-shot wakeups use
//! a min-heap. Dispatch order is deterministic: the smallest delta wins, ties
//! break on registration order, and a clock tick takes precedence over a timed
//! event that is due at the same simulated instant.

use core::cell::Cell;
use std::rc::Rc;
use std::time::{Duration, Instant};

use crate::core::{
    Core, Driver, Event, Handle, Listener, ResumeListener, RunState, TimeMode, TimedEvent, BILLION, CLOCK_COUNT,
};
use crate::ffi::*;

const MAX_FREQUENCY: u32 = 50_000_000;

// Reading the host clock at 10–50 MHz can cost more than the dispatch itself.
// Keep pause/topology/event-budget checks per dispatch, but amortize wall time
// checks over at most 32 dispatches. Native callbacks remain non-preemptible.
const WALL_CHECK_DISPATCHES: u32 = 32;

struct SliceDeadline {
    deadline: Option<Instant>,
    remaining: u32,
}

impl SliceDeadline {
    fn expired(&mut self) -> bool {
        let Some(deadline) = self.deadline else {
            return false;
        };
        if self.remaining != 0 {
            self.remaining -= 1;
            return false;
        }
        self.remaining = WALL_CHECK_DISPATCHES - 1;
        Instant::now() >= deadline
    }
}

fn checked_add(a: u64, b: u64) -> u64 {
    a.checked_add(b).expect("Simulation counter exhausted")
}

impl Core {
    pub fn subscribe(
        &self,
        owner: Handle,
        clock: u32,
        callback: SrhCallback,
        context: *mut core::ffi::c_void,
        result: *mut Handle,
    ) -> SrhStatus {
        if !self.alive(owner) || clock >= CLOCK_COUNT as u32 || result.is_null() {
            return SRH_INVALID;
        }
        let id = self.next();
        let order = self.order.get();
        self.order.set(order + 1);
        let event = Rc::new(Event {
            id,
            owner,
            due: 0,
            order,
            clock: Cell::new(Some(clock)),
            callback,
            context,
            active: Cell::new(true),
            visible: Cell::new(self.depth.get() == 0),
        });
        self.events.borrow_mut().push(Rc::clone(&event));
        self.clock_events[clock as usize].borrow_mut().push(event);
        self.topology_changed();
        if self.depth.get() != 0 {
            self.event_collection_pending.set(true);
        }
        // Safety: the caller supplied a valid output pointer.
        unsafe { *result = id };
        SRH_OK
    }

    pub fn schedule(
        &self,
        owner: Handle,
        delay: u64,
        callback: SrhCallback,
        context: *mut core::ffi::c_void,
        result: *mut Handle,
    ) -> SrhStatus {
        if !self.alive(owner) || result.is_null() || delay > u64::MAX - self.now.get() {
            return SRH_INVALID;
        }
        let id = self.next();
        let order = self.order.get();
        self.order.set(order + 1);
        let event = Rc::new(Event {
            id,
            owner,
            due: self.now.get() + delay,
            order,
            clock: Cell::new(None),
            callback,
            context,
            active: Cell::new(true),
            visible: Cell::new(self.depth.get() == 0),
        });
        self.events.borrow_mut().push(Rc::clone(&event));
        if event.visible.get() {
            self.timed_events.borrow_mut().push(TimedEvent { event });
        } else {
            self.pending_timed_events.borrow_mut().push(event);
        }
        self.topology_changed();
        if self.depth.get() != 0 {
            self.event_collection_pending.set(true);
        }
        // Safety: the caller supplied a valid output pointer.
        unsafe { *result = id };
        SRH_OK
    }

    pub fn cancel(&self, id: Handle) -> SrhStatus {
        if id == 0 {
            return SRH_NOT_FOUND;
        }
        {
            let listeners = self.resume_listeners.borrow();
            if let Some(listener) = listeners.iter().find(|entry| entry.id == id && entry.active.get()) {
                listener.active.set(false);
                self.collection_pending.set(true);
                return SRH_OK;
            }
        }
        {
            let inputs = self.inputs.borrow();
            for input in inputs.iter() {
                if input.subscription.get() == id {
                    let wake = input.wake.get();
                    input.subscription.set(0);
                    input.wake.set(0);
                    input.callback.set(None);
                    if wake != 0 {
                        self.cancel_event(wake);
                    }
                    return SRH_OK;
                }
            }
        }
        if self.cancel_event(id) {
            return SRH_OK;
        }
        {
            let listeners = self.listeners.borrow();
            let Some(listener) = listeners
                .iter()
                .find(|entry| entry.id == id && entry.active.get())
            else {
                return SRH_NOT_FOUND;
            };
            listener.active.set(false);
        }
        self.collection_pending.set(true);
        self.collect();
        SRH_OK
    }

    /// Cancels an event without consulting the input endpoints, so a caller
    /// iterating an endpoint list can cancel an endpoint wakeup safely.
    pub fn cancel_event(&self, id: Handle) -> bool {
        let deactivated = {
            let events = self.events.borrow();
            match events
                .iter()
                .find(|event| event.id == id && event.active.get())
            {
                Some(event) => {
                    event.active.set(false);
                    true
                }
                None => false,
            }
        };
        if deactivated {
            self.topology_changed();
            self.event_collection_pending.set(true);
            self.collect();
        }
        deactivated
    }

    pub fn frequency(&self, clock: u32, hz: u32) {
        if clock >= CLOCK_COUNT as u32 || hz > MAX_FREQUENCY {
            panic!("Clock frequency must be 0..50 MHz");
        }
        let mut value = self.clocks[clock as usize].get();
        if hz > 0 && value.hz == 0 {
            self.suppress_once();
        }
        value.hz = hz;
        let order = self.order.get();
        self.order.set(order + 1);
        value.order = order;
        self.clocks[clock as usize].set(value);
        self.topology_changed();
    }

    pub fn suppress_once(&self) {
        if let Some(boundary) = self.stopped_boundary.replace(None) {
            self.skip_boundary.set(Some(boundary));
        }
    }

    pub fn tick(&self, clock: u32) {
        self.enter_frame();
        {
            let mut value = self.clocks[clock as usize].get();
            value.ticks = checked_add(value.ticks, 1);
            self.clocks[clock as usize].set(value);
        }

        // The enclosing frame defers removal and keeps additions invisible.
        // Capture the original length, then release the borrow before each
        // callback so nested dispatch and registration remain legal. Retaining
        // one Rc keeps the record alive without allocating a callback snapshot.
        let count = self.clock_events[clock as usize].borrow().len();
        for index in 0..count {
            let event = Rc::clone(&self.clock_events[clock as usize].borrow()[index]);
            if !self.event_eligible(&event, Some(clock)) {
                continue;
            }
            // Safety: eligibility is checked immediately before invocation;
            // the frame retains the owning card until dispatch unwinds.
            let status = unsafe { (event.callback)(event.context) };
            if status != SRH_OK && status != SRH_STOP {
                self.run_state.set(RunState::Paused);
                *self.stop_reason.borrow_mut() = format!("Card clock callback failed ({status})");
            }
        }
        self.audio_advance();
        self.leave_frame();
    }

    fn event_eligible(&self, event: &Event, clock: Option<u32>) -> bool {
        if !event.active.get() || !event.visible.get() || !self.alive(event.owner) {
            return false;
        }
        match (clock, event.clock.get()) {
            (Some(expected), Some(actual)) => actual == expected,
            (None, None) => true,
            _ => false,
        }
    }

    /// Drops lazily cancelled or orphaned timers and returns the heap root.
    fn next_timed_event(&self) -> Option<Rc<Event>> {
        let mut timers = self.timed_events.borrow_mut();
        loop {
            let event = Rc::clone(&timers.peek()?.event);
            if event.active.get() && self.alive(event.owner) {
                return Some(event);
            }
            let entry = timers.pop().expect("peeked timer");
            let parked = self
                .cards
                .borrow()
                .get(&event.owner)
                .is_some_and(|card| card.is_parked());
            if event.active.get() && parked {
                self.parked_timed_events.borrow_mut().push(entry);
            }
        }
    }

    pub fn restore_parked_timers(&self, owner: Handle) {
        let mut parked = self.parked_timed_events.borrow_mut();
        let mut timers = self.timed_events.borrow_mut();
        let mut index = 0;
        while index < parked.len() {
            if parked[index].event.owner == owner {
                timers.push(parked.swap_remove(index));
            } else {
                index += 1;
            }
        }
    }

    pub fn log(&self, message: String) {
        let mut logs = self.logs.borrow_mut();
        if logs.len() == 1024 {
            logs.remove(0);
        }
        logs.push(message);
    }

    pub fn step(&self, clock: u32) -> bool {
        if clock >= CLOCK_COUNT as u32 || self.clocks[clock as usize].get().hz != 0 {
            return false;
        }
        self.suppress_once();
        self.tick(clock);
        true
    }

    pub fn stop(&self) {
        self.run_state.set(RunState::Stopped);
        *self.stop_reason.borrow_mut() = "Stopped".to_string();
        // Reset callbacks must observe the restarted timeline when
        // initializing timers and observations.
        self.now.set(0);
        self.reset(true);
        self.audio_reset();
    }

    pub fn resume(&self) {
        if self.run_state.get() == RunState::Running {
            return;
        }
        self.suppress_once();
        self.run_state.set(RunState::Running);
        *self.stop_reason.borrow_mut() = "Running".to_string();
        self.enter_frame();
        let listeners = self.resume_listeners.borrow().clone();
        for listener in listeners {
            if !listener.active.get() || !self.alive(listener.owner) {
                continue;
            }
            // Safety: the registration belongs to a live card and is called
            // on the simulation thread before the next event is dispatched.
            if unsafe { (listener.callback)(listener.context) } != SRH_OK {
                self.run_state.set(RunState::Paused);
                *self.stop_reason.borrow_mut() = "Card resume callback failed".to_string();
                break;
            }
        }
        self.leave_frame();
    }

    pub fn subscribe_resume(&self, owner: Handle, callback: SrhCallback,
                            context: *mut core::ffi::c_void, result: *mut Handle) -> SrhStatus {
        if !self.alive(owner) || result.is_null() {
            return SRH_INVALID;
        }
        let id = self.next();
        self.resume_listeners.borrow_mut().push(Rc::new(ResumeListener {
            id, owner, callback, context, active: Cell::new(true),
        }));
        // Safety: the caller supplied a writable result pointer.
        unsafe { *result = id };
        SRH_OK
    }

    pub fn pause(&self) {
        self.run_state.set(RunState::Paused);
        *self.stop_reason.borrow_mut() = "Paused".to_string();
    }

    pub fn stop_clocks(&self) {
        for index in 0..CLOCK_COUNT {
            self.frequency(index as u32, 0);
        }
    }

    pub fn reset(&self, cold: bool) {
        if self.depth.get() != 0 {
            self.pending_reset.set(Some(cold));
            return;
        }
        self.audio_reset();
        self.enter_frame();
        self.stopped_boundary.set(None);
        self.skip_boundary.set(None);
        if cold {
            // A cold power-on reset restarts the external oscillators while
            // simulated time stays monotonic.
            for index in 0..CLOCK_COUNT {
                let order = self.order.get();
                self.order.set(order + 1);
                self.clocks[index].set(crate::core::Clock {
                    hz: self.clocks[index].get().hz,
                    ticks: 0,
                    phase: 0,
                    order,
                });
            }
        }
        let rst = self.find_signal("/RST");
        let _ = self.drive(0, rst, 0, i32::MAX);
        let cards: Vec<Rc<crate::core::Card>> = self.cards.borrow().values().cloned().collect();
        for card in cards {
            if !card.is_active() {
                continue;
            }
            // Safety: the card is alive and its plugin is loaded.
            let status = unsafe {
                match (*card.api).reset {
                    Some(reset) => reset(card.instance(), if cold { 1 } else { 0 }),
                    None => SRH_OK,
                }
            };
            if status != SRH_OK {
                self.run_state.set(RunState::Paused);
                *self.stop_reason.borrow_mut() = "Card reset failed".to_string();
            }
        }
        let _ = self.drive(0, rst, STOPPED_LEVEL, i32::MAX);
        for input in self.inputs.borrow().iter() {
            let wake = input.wake.get();
            if wake != 0 {
                self.cancel_event(wake);
            }
            input.wake.set(0);
            input.notified.set(false);
        }
        self.refresh_inputs();
        self.leave_frame();
    }

    pub fn time_ns(&self) -> u64 {
        match self.mode.get() {
            TimeMode::Fixed => self.epoch.get(),
            TimeMode::System => {
                let now = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or(Duration::ZERO);
                now.as_nanos() as u64
            }
            TimeMode::Project => checked_add(self.epoch.get(), self.now.get()),
        }
    }

    /// SplitMix64: fixed, portable unsigned arithmetic, including a zero seed.
    pub fn random(&self) -> u64 {
        let mut z = self.rng.get().wrapping_add(0x9e37_79b9_7f4a_7c15);
        self.rng.set(z);
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }

    pub fn run(&self, event_budget: u64, until: u64) {
        self.run_impl(event_budget, until, None);
    }

    pub fn run_slice(&self, event_budget: u64, until: u64, wall_budget_ns: u64) {
        let maximum = 3600 * 1_000_000_000u64;
        let budget = wall_budget_ns.min(maximum);
        let deadline = Instant::now() + Duration::from_nanos(budget);
        self.run_impl(event_budget, until, Some(deadline));
    }

    fn run_impl(&self, mut budget: u64, until: u64, deadline: Option<Instant>) {
        let mut wall = SliceDeadline {
            deadline,
            remaining: 0,
        };
        if until < self.now.get() {
            panic!("Cannot run backwards");
        }
        if self.paused() {
            return;
        }
        // Fast path: one active master clock driven by one clock subscriber.
        // Any callback that changes the topology falls through to the general
        // scheduler on the next iteration.
        {
            let mut subscriber: Option<(u32, Rc<Event>)> = None;
            let mut subscriber_count = 0usize;
            for clock in 0..CLOCK_COUNT {
                for event in self.clock_events[clock].borrow().iter() {
                    if self.event_eligible(event, Some(clock as u32)) {
                        subscriber_count += 1;
                        if subscriber_count == 1 {
                            subscriber = Some((clock as u32, Rc::clone(event)));
                        }
                    }
                }
                if subscriber_count > 1 {
                    break;
                }
            }
            if subscriber_count == 1 {
                let (subscriber_clock, event) = subscriber.expect("one subscriber");
                let mut active_clock: Option<u32> = None;
                let mut active_clocks = 0u32;
                for index in 0..CLOCK_COUNT {
                    if self.clocks[index].get().hz != 0 {
                        active_clock = Some(index as u32);
                        active_clocks += 1;
                    }
                }
                if active_clocks == 1 && active_clock == Some(subscriber_clock) {
                    let clock = active_clock.expect("one active clock");
                    let generation = self.topology_generation.get();
                    let next_timer_due = self.next_timed_event().map(|timer| timer.due);
                    loop {
                        if budget == 0 || self.run_state.get() != RunState::Running {
                            break;
                        }
                        if self.topology_generation.get() != generation {
                            break;
                        }
                        if wall.expired() {
                            return;
                        }
                        budget -= 1;
                        let state = self.clocks[clock as usize].get();
                        let delta = if state.phase >= BILLION {
                            0
                        } else {
                            (BILLION - state.phase + state.hz as u64 - 1) / state.hz as u64
                        };
                        if next_timer_due
                            .is_some_and(|due| due.saturating_sub(self.now.get()) <= delta)
                        {
                            break;
                        }
                        if until != u64::MAX && (delta > until - self.now.get()) {
                            let remaining = until - self.now.get();
                            let mut advanced = state;
                            advanced.phase =
                                checked_add(advanced.phase, remaining * advanced.hz as u64);
                            self.clocks[clock as usize].set(advanced);
                            self.now.set(until);
                            self.audio_advance();
                            return;
                        }
                        self.now.set(checked_add(self.now.get(), delta));
                        let order = self.order.get();
                        self.order.set(order + 1);
                        let mut advanced = state;
                        advanced.phase =
                            checked_add(advanced.phase, delta * advanced.hz as u64) - BILLION;
                        advanced.order = order;
                        self.clocks[clock as usize].set(advanced);
                        self.enter_frame();
                        let mut tick_state = self.clocks[clock as usize].get();
                        tick_state.ticks = checked_add(tick_state.ticks, 1);
                        self.clocks[clock as usize].set(tick_state);
                        let status = unsafe { (event.callback)(event.context) };
                        if status != SRH_OK && status != SRH_STOP {
                            self.run_state.set(RunState::Paused);
                            *self.stop_reason.borrow_mut() =
                                format!("Card clock callback failed ({status})");
                        }
                        self.audio_advance();
                        self.leave_frame();
                    }
                    if self.run_state.get() != RunState::Running
                        || self.now.get() == until
                        || budget == 0
                    {
                        return;
                    }
                }
            }
        }

        while budget != 0 && self.run_state.get() == RunState::Running {
            if wall.expired() {
                return;
            }
            budget -= 1;
            let mut delta = u64::MAX;
            let mut chosen_order = u64::MAX;
            let mut chosen_clock: i64 = -1;
            for index in 0..CLOCK_COUNT {
                let clock = self.clocks[index].get();
                if clock.hz != 0 {
                    let candidate = if clock.phase >= BILLION {
                        0
                    } else {
                        (BILLION - clock.phase + clock.hz as u64 - 1) / clock.hz as u64
                    };
                    if candidate < delta || (candidate == delta && clock.order < chosen_order) {
                        delta = candidate;
                        chosen_order = clock.order;
                        chosen_clock = index as i64;
                    }
                }
            }
            let chosen_event = self.next_timed_event();
            if let Some(event) = &chosen_event {
                let candidate = event.due.saturating_sub(self.now.get());
                if candidate < delta || (candidate == delta && event.order < chosen_order) {
                    delta = candidate;
                    chosen_clock = -1;
                }
            }
            if until != u64::MAX && (delta == u64::MAX || delta > until - self.now.get()) {
                let remaining = until - self.now.get();
                for index in 0..CLOCK_COUNT {
                    let mut clock = self.clocks[index].get();
                    if clock.hz != 0 {
                        clock.phase = checked_add(clock.phase, remaining * clock.hz as u64);
                        self.clocks[index].set(clock);
                    }
                }
                self.now.set(until);
                self.audio_advance();
                return;
            }
            if delta == u64::MAX {
                return;
            }
            self.now.set(checked_add(self.now.get(), delta));
            // With any active clock, delta never exceeds that clock's next
            // tick, which is at most one second.
            for index in 0..CLOCK_COUNT {
                let mut clock = self.clocks[index].get();
                if clock.hz != 0 {
                    clock.phase = checked_add(clock.phase, delta * clock.hz as u64);
                    self.clocks[index].set(clock);
                }
            }
            if chosen_clock >= 0 {
                let index = chosen_clock as usize;
                let mut clock = self.clocks[index].get();
                clock.phase -= BILLION;
                let order = self.order.get();
                self.order.set(order + 1);
                clock.order = order;
                self.clocks[index].set(clock);
                self.tick(chosen_clock as u32);
            } else if let Some(event) = chosen_event {
                self.enter_frame();
                event.active.set(false);
                let popped = self.timed_events.borrow_mut().pop();
                debug_assert!(popped.is_some_and(|entry| entry.event.id == event.id));
                self.topology_changed();
                // A dispatched event is retired at the next dispatch boundary,
                // which is what keeps a callback from observing itself.
                self.event_collection_pending.set(true);
                // Safety: the callback belongs to a live owner.
                let status = unsafe { (event.callback)(event.context) };
                if status != SRH_OK && status != SRH_STOP {
                    self.run_state.set(RunState::Paused);
                    *self.stop_reason.borrow_mut() = "Scheduled callback failed".to_string();
                }
                self.audio_advance();
                self.leave_frame();
            }
        }
    }

    pub fn paused(&self) -> bool {
        self.run_state.get() != RunState::Running
    }

    pub fn boundary_required(&self) -> bool {
        if self.trace_capture.get() {
            return true;
        }
        self.breakpoints
            .borrow()
            .iter()
            .any(|point| point.enabled && point.operations == 0)
    }

    // ------------------------------------------------------------------
    // Signals
    // ------------------------------------------------------------------

    /// Recomputes the resolved value of `id` and notifies listeners on change.
    pub fn recompute(&self, id: Handle) {
        self.enter_frame();
        let resolution = {
            let signals = self.signals.borrow();
            let signal = signals.get(&id);
            let Some(signal) = signal else {
                self.leave_frame();
                return;
            };
            let mut value = signal.idle;
            let mut best: Option<&Driver> = None;
            for driver in &signal.drivers {
                let usable = driver.owner == 0 || self.alive(driver.owner);
                if !usable {
                    continue;
                }
                let better = match best {
                    None => true,
                    Some(current) => {
                        driver.priority > current.priority
                            || (driver.priority == current.priority && driver.order < current.order)
                    }
                };
                if better {
                    best = Some(driver);
                }
            }
            if let Some(driver) = best {
                value = driver.value;
            }
            if value == signal.value {
                None
            } else {
                Some(value)
            }
        };
        let Some(next) = resolution else {
            self.leave_frame();
            return;
        };
        {
            let mut signals = self.signals.borrow_mut();
            if let Some(signal) = signals.get_mut(&id) {
                signal.value = next;
            }
        }
        let listeners: Vec<Rc<Listener>> = self.listeners.borrow().clone();
        for listener in listeners {
            if listener.active.get()
                && listener.visible.get()
                && listener.signal == id
                && self.alive(listener.owner)
            {
                // Safety: the listener belongs to a live owner.
                let status = unsafe { (listener.callback)(listener.context, next) };
                if status != SRH_OK {
                    self.run_state.set(RunState::Paused);
                    *self.stop_reason.borrow_mut() = "Signal callback failed".to_string();
                }
            }
        }
        self.leave_frame();
    }

    pub fn drive(&self, owner: Handle, id: Handle, value: i32, priority: i32) -> SrhStatus {
        if (owner != 0 && !self.alive(owner)) || !self.signals.borrow().contains_key(&id) {
            return SRH_INVALID;
        }
        {
            let mut signals = self.signals.borrow_mut();
            let signal = signals.get_mut(&id).expect("signal was checked above");
            match signal
                .drivers
                .iter_mut()
                .find(|driver| driver.owner == owner)
            {
                Some(driver) => {
                    driver.value = value;
                    driver.priority = priority;
                }
                None => {
                    let order = self.order.get();
                    self.order.set(order + 1);
                    signal.drivers.push(Driver {
                        owner,
                        value,
                        priority,
                        order,
                    });
                }
            }
        }
        self.recompute(id);
        SRH_OK
    }

    pub fn release_signal(&self, owner: Handle, id: Handle) -> SrhStatus {
        if (owner != 0 && !self.alive(owner)) || !self.signals.borrow().contains_key(&id) {
            return SRH_INVALID;
        }
        {
            let mut signals = self.signals.borrow_mut();
            if let Some(signal) = signals.get_mut(&id) {
                signal.drivers.retain(|driver| driver.owner != owner);
            }
        }
        self.recompute(id);
        SRH_OK
    }

    pub fn signal_subscribe(
        &self,
        owner: Handle,
        id: Handle,
        callback: SrhSignalCallback,
        context: *mut core::ffi::c_void,
        result: *mut Handle,
    ) -> SrhStatus {
        if !self.alive(owner) || !self.signals.borrow().contains_key(&id) || result.is_null() {
            return SRH_INVALID;
        }
        let handle = self.next();
        self.listeners.borrow_mut().push(Rc::new(Listener {
            id: handle,
            owner,
            signal: id,
            callback,
            context,
            active: Cell::new(true),
            visible: Cell::new(self.depth.get() == 0),
        }));
        if self.depth.get() != 0 {
            self.collection_pending.set(true);
        }
        // Safety: the caller supplied a valid output pointer.
        unsafe { *result = handle };
        SRH_OK
    }

    pub fn sample(&self, id: Handle) -> Result<i32, SrhStatus> {
        match self.signals.borrow().get(&id) {
            Some(signal) => Ok(signal.value),
            None => Err(SRH_NOT_FOUND),
        }
    }

    /// Removes an owner's drivers from every signal it touched, used when a
    /// card disappears.
    pub fn drop_owner_signals(&self, owner: Handle) {
        let changed: Vec<Handle> = {
            let mut signals = self.signals.borrow_mut();
            let mut changed = Vec::new();
            for (id, signal) in signals.iter_mut() {
                let before = signal.drivers.len();
                signal.drivers.retain(|driver| driver.owner != owner);
                if signal.drivers.len() != before {
                    changed.push(*id);
                }
            }
            changed
        };
        for id in changed {
            self.recompute(id);
        }
    }

    /// Signals an owner currently drives, used when a card is parked so its
    /// released drivers expose the next driver or the idle level.
    pub fn owner_signal_ids(&self, owner: Handle) -> Vec<Handle> {
        self.signals
            .borrow()
            .iter()
            .filter(|(_, signal)| signal.drivers.iter().any(|driver| driver.owner == owner))
            .map(|(id, _)| *id)
            .collect()
    }
}

/// The idle level of `/RST` used by the default machine.
pub const STOPPED_LEVEL: i32 = 5000;

#[cfg(test)]
mod deadline_tests {
    use super::*;

    #[test]
    fn expired_deadline_is_observed_within_one_dispatch_batch() {
        let mut wall = SliceDeadline {
            deadline: Some(Instant::now() + Duration::from_secs(3600)),
            remaining: 0,
        };
        assert!(!wall.expired());
        // Expire immediately after a check, the worst point in a batch.
        wall.deadline = Some(Instant::now());
        for _ in 1..WALL_CHECK_DISPATCHES {
            assert!(!wall.expired());
        }
        assert!(wall.expired());
    }
}
