//! Subsystem tests driving `Core` directly, without a card plugin or the ABI:
//! rack, bus, clocks, scheduler, debugger, audio, video and providers.

use crate::core::{Core, Resolver, RunState, TimeMode, BILLION};
use crate::ffi::*;

fn core() -> Core {
    Core::new(RunState::Stopped)
}

#[test]
fn stopping_with_pending_midi_input_can_cancel_the_playback_source() {
    let core = core();
    let owner = core.attach_test_card("midi");
    let mut endpoint = 0;
    let mut subscription = 0;
    assert_eq!(
        core.register_input(owner, "midi", 64, &mut endpoint),
        SRH_OK
    );
    assert_eq!(
        core.subscribe_input_due(
            owner,
            endpoint,
            no_op,
            std::ptr::null_mut(),
            &mut subscription
        ),
        SRH_OK
    );
    assert_eq!(
        core.enqueue_input_batch("midi", 1_000_000, &[0x90, 60, 100], 42, owner),
        SRH_OK
    );
    core.resume();
    core.stop();
    core.cancel_input_source(42);
    assert!(core
        .live_endpoint(endpoint)
        .unwrap()
        .records
        .borrow()
        .is_empty());
    assert!(core.events.borrow().is_empty());
    assert_eq!(core.depth.get(), 0);
    assert!(!core.collecting.get());
}

struct SchedulerMutation {
    core: *const Core,
    owner: u64,
    subscription: u64,
    mode: u32,
    calls: u32,
}

unsafe extern "C" fn mutate_scheduler(context: *mut core::ffi::c_void) -> SrhStatus {
    // Safety: the test retains both context and core throughout run().
    let state = unsafe { &mut *context.cast::<SchedulerMutation>() };
    let core = unsafe { &*state.core };
    state.calls += 1;
    if state.calls == 1 {
        if state.mode == 0 {
            core.frequency(1, 1_000_000);
        } else {
            assert!(core.cancel_event(state.subscription));
            let mut replacement = 0;
            if state.mode == 1 {
                core.schedule(
                    state.owner,
                    500,
                    no_op,
                    std::ptr::null_mut(),
                    &mut replacement,
                );
            } else {
                core.subscribe(
                    state.owner,
                    1,
                    no_op,
                    std::ptr::null_mut(),
                    &mut replacement,
                );
            }
        }
    }
    SRH_OK
}

#[test]
fn scheduler_fast_path_revalidates_after_callback_mutations() {
    for mode in 0..3 {
        let core = core();
        let owner = core.attach_test_card("scheduler");
        core.frequency(0, 1_000_000);
        let mut state = SchedulerMutation {
            core: &core,
            owner,
            subscription: 0,
            mode,
            calls: 0,
        };
        let context = (&mut state as *mut SchedulerMutation).cast();
        assert_eq!(
            core.subscribe(owner, 0, mutate_scheduler, context, &mut state.subscription),
            SRH_OK
        );
        core.resume();
        core.run(3, u64::MAX);
        match mode {
            0 => {
                assert_eq!(core.now.get(), 2_000);
                assert_eq!(core.clocks[1].get().ticks, 1);
            }
            1 => {
                assert_eq!(core.now.get(), 2_000);
                assert!(
                    core.events.borrow().is_empty(),
                    "replacement timer must fire"
                );
            }
            _ => {
                // A replacement subscriber on a disabled clock must not be
                // dispatched as if it still belonged to clock zero.
                assert_eq!(state.calls, 1);
                assert_eq!(core.clocks[1].get().ticks, 0);
                assert_eq!(core.now.get(), 3_000);
            }
        }
    }
}

#[test]
fn event_only_collection_preserves_other_registrations_and_defers_visibility() {
    let core = core();
    let owner = core.attach_test_card("timer");
    core.enter_frame();
    let mut event = 0;
    assert_eq!(
        core.schedule(owner, 100, no_op, std::ptr::null_mut(), &mut event),
        SRH_OK
    );
    assert!(!core.events.borrow()[0].visible.get());
    core.enter_frame();
    core.leave_frame();
    assert!(!core.events.borrow()[0].visible.get());
    core.leave_frame();
    assert!(core.events.borrow()[0].visible.get());
    assert!(core.cancel_event(event));
    assert!(core.events.borrow().is_empty());
    assert!(core.alive(owner));
    assert!(!core.event_collection_pending.get());
    // A full sweep and event-only work may coexist at the same boundary.
    core.enter_frame();
    assert_eq!(
        core.schedule(owner, 100, no_op, std::ptr::null_mut(), &mut event),
        SRH_OK
    );
    assert_eq!(core.remove(owner), SRH_OK);
    core.leave_frame();
    assert!(core.events.borrow().is_empty());
    assert!(core.cards.borrow().is_empty());
}

#[test]
fn audio_single_frame_batches_reuse_scratch_and_clear_previous_mix() {
    let core = core();
    let owner = core.attach_test_card("audio");
    let amplitude: i16 = 1234;
    let mut source = 0;
    assert_eq!(
        core.audio_register_source(
            owner,
            44100,
            2,
            SRH_AUDIO_S16_STEREO,
            "tone",
            amplitude_source,
            (&amplitude as *const i16).cast_mut().cast(),
            &mut source
        ),
        SRH_OK
    );
    let scratch = core.audio_mix_scratch.borrow().as_ptr();
    for frame in 0..100 {
        core.audio_set_source_muted(source, frame % 2 != 0);
        core.now.set(((frame + 1) * BILLION + 44099) / 44100);
        core.audio_advance();
        let mut samples = [0i16; 2];
        assert_eq!(core.audio_read(samples.as_mut_ptr(), 1), 1);
        assert_eq!(samples, [if frame % 2 == 0 { amplitude } else { 0 }; 2]);
        assert_eq!(core.audio_mix_scratch.borrow().as_ptr(), scratch);
    }
}

/// A core with one 16-bit space, used by the bus tests.
fn machine() -> (Core, crate::handle::Handle) {
    let core = core();
    let space = core.space(
        "test.memory".to_string(),
        0xFFFF,
        7,
        false,
        Resolver::Priority,
    );
    (core, space)
}

#[test]
fn handles_are_monotonic_and_never_reused() {
    let core = core();
    let first = core.next();
    let second = core.next();
    let third = core.next();
    assert!(first < second && second < third);
    // The constructor consumes the first three handles for /RST, IRQ and NMI.
    assert_eq!(first, 4);
}

#[test]
fn spaces_and_signals_reject_duplicates() {
    let core = core();
    let first = core.space("a".to_string(), 0xFF, 0, false, Resolver::Priority);
    assert!(first != 0);
    assert_eq!(core.find_space("a"), first);
    assert_eq!(core.find_space("missing"), 0);
    assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        core.space("a".to_string(), 0xFF, 0, false, Resolver::Priority)
    }))
    .is_err());

    let signal = core.signal("S".to_string(), 0);
    assert!(signal != 0);
    assert_eq!(core.find_signal("S"), signal);
    // The ABI entry point reports a duplicate as "no handle" rather than
    // panicking, which is what a caller of `srz80_engine_signal` observes.
    assert_eq!(core.try_signal("S".to_string(), 0), None);
    assert_eq!(core.find_signal("S"), signal);
}

#[test]
fn the_default_machine_owns_the_documented_signals() {
    let core = core();
    assert!(core.find_signal("/RST") != 0);
    assert!(core.find_signal("IRQ") != 0);
    assert!(core.find_signal("NMI") != 0);
    assert_eq!(core.sample(core.find_signal("/RST")).unwrap(), 5000);
    assert_eq!(core.sample(core.find_signal("IRQ")).unwrap(), 0);
}

#[test]
fn signal_resolution_prefers_priority_then_registration_order() {
    let core = core();
    let signal = core.signal("S".to_string(), 0);
    // Two distinct drivers: a card handle of zero means "not a card", so the
    // engine accepts it, but the same owner twice replaces its own driver.
    let low = core.attach_test_card("low");
    let high = core.attach_test_card("high");
    assert_eq!(core.drive(low, signal, 3, 1), SRH_OK);
    assert_eq!(core.sample(signal).unwrap(), 3);
    // A higher priority wins even though it was driven later.
    assert_eq!(core.drive(high, signal, 5, 9), SRH_OK);
    assert_eq!(core.sample(signal).unwrap(), 5);
    // Releasing the strongest driver exposes the next one.
    assert_eq!(core.release_signal(high, signal), SRH_OK);
    assert_eq!(core.sample(signal).unwrap(), 3);
    assert_eq!(core.release_signal(low, signal), SRH_OK);
    assert_eq!(core.sample(signal).unwrap(), 0);
}

#[test]
fn one_owner_keeps_exactly_one_driver() {
    let core = core();
    let signal = core.signal("S".to_string(), 0);
    let card = core.attach_test_card("card");
    assert_eq!(core.drive(card, signal, 1, 0), SRH_OK);
    assert_eq!(core.drive(card, signal, 2, 0), SRH_OK);
    assert_eq!(core.sample(signal).unwrap(), 2);
    assert_eq!(core.signals.borrow()[&signal].drivers.len(), 1);
}

#[test]
fn an_unknown_signal_is_reported_as_not_found() {
    let core = core();
    assert_eq!(core.drive(0, 0xBEEF, 1, 0), SRH_INVALID);
    assert_eq!(core.release_signal(0, 0xBEEF), SRH_INVALID);
    assert_eq!(core.sample(0xBEEF), Err(SRH_NOT_FOUND));
}

#[test]
fn the_random_sequence_is_deterministic_and_seedable() {
    let core = core();
    core.rng.set(1);
    let first: Vec<u64> = (0..4).map(|_| core.random()).collect();
    core.rng.set(1);
    let second: Vec<u64> = (0..4).map(|_| core.random()).collect();
    assert_eq!(first, second);
    // SplitMix64 with a zero seed still produces non-zero output.
    core.rng.set(0);
    assert_ne!(core.random(), 0);
}

#[test]
fn frequencies_are_validated_and_clocks_start_stopped() {
    let core = core();
    for index in 0..3 {
        assert_eq!(core.clocks[index].get().hz, 0);
    }
    core.frequency(0, 1_000_000);
    assert_eq!(core.clocks[0].get().hz, 1_000_000);
    core.frequency(0, 0);
    assert_eq!(core.clocks[0].get().hz, 0);
    assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        core.frequency(0, 50_000_001)
    }))
    .is_err());
    assert!(
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| { core.frequency(3, 1) }))
            .is_err()
    );
}

#[test]
fn a_bounded_run_advances_the_master_clock_deterministically() {
    let core = core();
    core.frequency(0, 1_000_000);
    core.resume();
    core.run(100, u64::MAX);
    assert_eq!(core.clocks[0].get().ticks, 100);
    // One tick per microsecond at 1 MHz, so simulated time follows the ticks.
    assert_eq!(core.now.get(), 100_000);
}

#[test]
fn run_until_stops_exactly_at_the_requested_time() {
    let core = core();
    core.frequency(0, 1_000_000);
    core.resume();
    core.run(u64::MAX, 12_345);
    assert_eq!(core.now.get(), 12_345);
    assert_eq!(core.run_state.get(), RunState::Running);
}

#[test]
fn running_backwards_is_rejected() {
    let core = core();
    core.frequency(0, 1_000);
    core.resume();
    core.run(10, u64::MAX);
    let now = core.now.get();
    assert!(now > 0);
    assert!(
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| { core.run(10, now - 1) }))
            .is_err()
    );
}

#[test]
fn paused_and_stopped_are_distinct_states() {
    let core = core();
    assert_eq!(core.run_state.get(), RunState::Stopped);
    core.resume();
    assert_eq!(core.run_state.get(), RunState::Running);
    core.pause();
    assert_eq!(core.run_state.get(), RunState::Paused);
    assert_eq!(*core.stop_reason.borrow(), "Paused");
    // A paused engine does not advance when run.
    core.frequency(0, 1000);
    core.run(100, u64::MAX);
    assert_eq!(core.clocks[0].get().ticks, 0);
}

#[test]
fn stop_restarts_the_timeline_and_cold_resets_the_clocks() {
    let core = core();
    core.frequency(0, 1_000_000);
    core.resume();
    core.run(50, u64::MAX);
    assert!(core.now.get() > 0);
    core.stop();
    assert_eq!(core.run_state.get(), RunState::Stopped);
    assert_eq!(core.now.get(), 0);
    assert_eq!(core.clocks[0].get().ticks, 0);
    assert_eq!(core.clocks[0].get().phase, 0);
}

#[test]
fn step_only_works_while_the_clock_is_manually_driven() {
    let core = core();
    assert!(core.step(0));
    assert_eq!(core.clocks[0].get().ticks, 1);
    core.frequency(1, 1000);
    assert!(!core.step(1));
    assert_eq!(core.clocks[1].get().ticks, 0);
}

#[test]
fn time_modes_project_fixed_and_system() {
    let core = core();
    core.epoch.set(1_000);
    core.now.set(500);
    assert_eq!(core.time_ns(), 1_500);
    core.mode.set(TimeMode::Fixed);
    assert_eq!(core.time_ns(), 1_000);
    core.mode.set(TimeMode::System);
    assert!(core.time_ns() > 1_500);
}

#[test]
fn an_unclaimed_read_reports_the_space_fallback() {
    let (core, space) = machine();
    let mut value = 0u8;
    assert_eq!(core.read(0, space, 0x10, &mut value, false), SRH_OK);
    assert_eq!(value, 7);
}

#[test]
fn a_random_space_returns_a_random_byte_and_peeks_are_unavailable() {
    let core = core();
    let space = core.space("rand".to_string(), 0xFF, 0, true, Resolver::Priority);
    let mut value = 0u8;
    assert_eq!(core.read(0, space, 1, &mut value, false), SRH_OK);
    let mut peek = 0u8;
    assert_eq!(core.read(0, space, 1, &mut peek, true), SRH_UNAVAILABLE);
}

#[test]
fn reads_and_writes_validate_the_space_and_the_address() {
    let (core, space) = machine();
    let mut value = 0u8;
    assert_eq!(core.read(0, 0xDEAD, 0, &mut value, false), SRH_INVALID);
    assert_eq!(
        core.read(0, space, 0x1_0000, &mut value, false),
        SRH_INVALID
    );
    assert_eq!(core.write(0, 0xDEAD, 0, 1), SRH_INVALID);
    assert_eq!(core.write(0, space, 0x1_0000, 1), SRH_INVALID);
    // A master that is not a live card is rejected too.
    assert_eq!(core.read(0xBEEF, space, 0, &mut value, false), SRH_INVALID);
}

#[test]
fn trace_capture_records_a_write_with_its_address_and_value() {
    let (core, space) = machine();
    assert_eq!(core.write(0, space, 0x20, 0x5A), SRH_OK);
    let records = core.trace_records();
    assert_eq!(records.len(), 1);
    assert_eq!(records[0].operation, SRH_WRITE);
    assert_eq!(records[0].address, 0x20);
    assert_eq!(records[0].value, 0x5A);
    assert_eq!(records[0].space, space);
    assert_eq!(records[0].result, SRH_OK);
    // A peek is side-effect free and is not recorded.
    let mut value = 0u8;
    assert_eq!(core.read(0, space, 0x20, &mut value, true), SRH_OK);
    assert_eq!(core.trace_records().len(), 1);
}

#[test]
fn capture_can_be_limited_to_one_operation() {
    let (core, space) = machine();
    core.trace_capture_operations.set(SRH_WRITE);
    let mut value = 0u8;
    assert_eq!(core.read(0, space, 0x20, &mut value, false), SRH_OK);
    assert!(core.trace_records().is_empty());
    assert_eq!(core.write(0, space, 0x20, 1), SRH_OK);
    assert_eq!(core.trace_records().len(), 1);
}

#[test]
fn disabling_capture_stops_recording_but_keeps_the_records() {
    let (core, space) = machine();
    assert_eq!(core.write(0, space, 1, 1), SRH_OK);
    core.trace_capture.set(false);
    assert_eq!(core.write(0, space, 2, 2), SRH_OK);
    assert_eq!(core.trace_records().len(), 1);
    assert_eq!(core.write(0, space, 3, 3), SRH_OK);
}

#[test]
fn the_trace_ring_drops_the_oldest_record_and_counts_it() {
    let (core, space) = machine();
    for index in 0..(crate::core::TRACE_CAPACITY + 5) {
        assert_eq!(core.write(0, space, index as u64, index as u8), SRH_OK);
    }
    assert_eq!(core.trace_records().len(), crate::core::TRACE_CAPACITY);
    assert_eq!(core.dropped.get(), 5);
    core.trace.borrow_mut().clear();
    core.dropped.set(0);
}

#[test]
fn a_read_word_needs_one_contiguous_mapping_and_no_breakpoints() {
    let (core, space) = machine();
    let mut word = 0u32;
    // Without a mapping the engine cannot preserve byte-bus semantics.
    assert_eq!(core.read_word(0, space, 0, &mut word), SRH_UNAVAILABLE);
    // Any registered breakpoint disables the optimisation.
    let _ = core.add_breakpoint(0, space, 0x10, 0x1F, SRH_WRITE);
    assert_eq!(core.read_word(0, space, 0, &mut word), SRH_UNAVAILABLE);
}

#[test]
fn breakpoints_validate_their_range_and_operations() {
    let (core, space) = machine();
    assert!(core.add_breakpoint(0, space, 0x10, 0x1F, SRH_WRITE).is_ok());
    // first > last
    assert!(core.add_breakpoint(0, space, 0x20, 0x10, 0).is_err());
    // beyond the space
    assert!(core.add_breakpoint(0, space, 0, 0x1_0000, 0).is_err());
    // an unknown space
    assert!(core.add_breakpoint(0, 0xDEAD, 0, 1, 0).is_err());
    // more than read|write
    assert!(core.add_breakpoint(0, space, 0, 1, 4).is_err());
    // an execution breakpoint needs a live card
    assert!(core.add_breakpoint(0xBEEF, space, 0, 1, 0).is_err());
}

#[test]
fn a_watchpoint_stops_the_clocks_without_pausing() {
    let (core, space) = machine();
    core.frequency(0, 1_000_000);
    core.resume();
    let _ = core.add_breakpoint(0, space, 0x40, 0x4F, SRH_WRITE);
    assert_eq!(core.write(0, space, 0x40, 1), SRH_OK);
    assert_eq!(core.clocks[0].get().hz, 0);
    assert_eq!(core.run_state.get(), RunState::Running);
    assert!(core.stop_reason.borrow().contains("Watchpoint"));
}

#[test]
fn an_execution_breakpoint_reports_stop_and_can_be_skipped_once() {
    let (core, space) = machine();
    let card = core.attach_test_card("fake");
    let _ = core.add_breakpoint(card, space, 0x100, 0x1FF, 0);
    assert_eq!(core.boundary(card, space, 0x150, 0, 0), SRH_STOP);
    assert_eq!(core.stopped_boundary.get(), Some([card, space, 0x150]));
    // Resuming suppresses exactly one repeat of the same boundary.
    core.suppress_once();
    assert_eq!(core.boundary(card, space, 0x150, 0, 0), SRH_OK);
    assert_eq!(core.boundary(card, space, 0x150, 0, 0), SRH_STOP);
    assert_eq!(core.boundary(0xBEEF, space, 0x150, 0, 0), SRH_NOT_FOUND);
}

#[test]
fn boundary_required_follows_trace_capture_and_execution_breakpoints() {
    let (core, space) = machine();
    assert!(core.boundary_required());
    core.trace_capture.set(false);
    assert!(!core.boundary_required());
    let card = core.attach_test_card("fake");
    let _ = core.add_breakpoint(card, space, 0, 0xFF, 0);
    assert!(core.boundary_required());
    // A data watchpoint does not need an instruction boundary.
    core.breakpoints.borrow_mut().clear();
    let _ = core.add_breakpoint(card, space, 0, 0xFF, SRH_WRITE);
    assert!(!core.boundary_required());
}

#[test]
fn a_card_requested_stop_pauses_with_a_reason() {
    let (core, _space) = machine();
    let card = core.attach_test_card("fake");
    assert_eq!(core.request_stop(card, "halt"), SRH_OK);
    assert_eq!(core.run_state.get(), RunState::Paused);
    assert!(core.stop_reason.borrow().contains("halt"));
    assert_eq!(core.request_stop(0xBEEF, "halt"), SRH_NOT_FOUND);
}

#[test]
fn scheduled_events_fire_once_at_their_due_time() {
    let core = core();
    core.frequency(0, 1_000_000);
    core.resume();
    let card = core.attach_test_card("fake");
    let mut handle = 0u64;
    assert_eq!(
        core.schedule(card, 1_000_000, no_op, std::ptr::null_mut(), &mut handle),
        SRH_OK
    );
    assert!(handle != 0);
    // A budget far smaller than the delay leaves the event pending.
    core.run(10, u64::MAX);
    assert!(
        core.events.borrow().iter().any(|event| event.id == handle),
        "a timer fired before its due time at now={}",
        core.now.get()
    );
    // A budget large enough to pass the due time removes and runs it.
    core.run(u64::MAX, core.now.get() + 10_000_000);
    assert!(!core.events.borrow().iter().any(|event| event.id == handle));
}

#[test]
fn a_scheduled_event_never_fires_before_its_due_time() {
    let core = core();
    core.frequency(0, 1_000_000);
    core.resume();
    let card = core.attach_test_card("fake");
    let mut handle = 0u64;
    assert_eq!(
        core.schedule(card, 5_000, no_op, std::ptr::null_mut(), &mut handle),
        SRH_OK
    );
    // Run up to one nanosecond short of the due time.
    core.run(u64::MAX, 4_999);
    assert!(
        core.events.borrow().iter().any(|event| event.id == handle),
        "a timer fired before its due time at now={}",
        core.now.get()
    );
    assert!(core.now.get() <= 5_000);
}

#[test]
fn cancelling_an_event_is_exact_and_idempotent_reports_not_found() {
    let core = core();
    let card = core.attach_test_card("fake");
    let mut handle = 0u64;
    assert_eq!(
        core.schedule(card, 1_000_000, no_op, std::ptr::null_mut(), &mut handle),
        SRH_OK
    );
    assert_eq!(core.cancel(handle), SRH_OK);
    assert_eq!(core.cancel(handle), SRH_NOT_FOUND);
    assert_eq!(core.cancel(0), SRH_NOT_FOUND);
}

#[test]
fn scheduling_requires_a_live_owner() {
    let core = core();
    let mut handle = 0u64;
    assert_eq!(
        core.schedule(0xBEEF, 1, no_op, std::ptr::null_mut(), &mut handle),
        SRH_INVALID
    );
    assert_eq!(
        core.subscribe(0xBEEF, 0, no_op, std::ptr::null_mut(), &mut handle),
        SRH_INVALID
    );
    assert_eq!(
        core.subscribe(
            core.attach_test_card("fake"),
            3,
            no_op,
            std::ptr::null_mut(),
            &mut handle
        ),
        SRH_INVALID
    );
}

#[test]
fn reordering_requires_a_permutation_of_the_existing_rack() {
    let core = core();
    let first = core.attach_test_card("a");
    let second = core.attach_test_card("b");
    assert_eq!(core.reorder_cards(vec![second, first]), SRH_OK);
    assert_eq!(*core.rack_order.borrow(), vec![second, first]);
    assert_eq!(core.reorder_cards(vec![first]), SRH_INVALID);
    assert_eq!(core.reorder_cards(vec![first, 0xBEEF]), SRH_INVALID);
    // The order is presentation only: both cards are still alive.
    assert!(core.alive(first) && core.alive(second));
}

#[test]
fn parking_keeps_the_card_and_plugging_restores_it() {
    let core = core();
    let card = core.attach_test_card("a");
    assert!(core.alive(card));
    assert_eq!(core.park(card), SRH_OK);
    assert!(!core.alive(card));
    assert_eq!(core.cards().len(), 0);
    assert_eq!(core.all_cards().len(), 1);
    assert_eq!(core.removed_cards().len(), 1);
    assert_eq!(core.all_cards()[0].parked, true);
    // Parking twice is not a valid operation.
    assert_eq!(core.park(card), SRH_NOT_FOUND);
    assert_eq!(core.plug(card), SRH_OK);
    assert!(core.alive(card));
    assert_eq!(core.cards().len(), 1);
}

#[test]
fn removing_a_card_retires_it_at_the_next_collection() {
    let core = core();
    let card = core.attach_test_card("a");
    assert_eq!(core.remove(card), SRH_OK);
    assert!(!core.alive(card));
    // The registry entry disappears as soon as the enclosing frame unwinds.
    core.enter_frame();
    core.collection_pending.set(true);
    core.leave_frame();
    assert_eq!(core.all_cards().len(), 0);
    assert_eq!(core.rack_order.borrow().len(), 0);
    assert!(core.cards.borrow().is_empty());
    assert_eq!(core.remove(card), SRH_NOT_FOUND);
}

#[test]
fn execution_state_round_trips_through_the_engine() {
    let (core, space) = machine();
    core.frequency(0, 800_000);
    core.frequency(1, 0);
    core.frequency(2, 0);
    core.rng.set(4242);
    core.now.set(1_234);
    let _ = core.write(0, space, 0x10, 0xAB);
    core.trace.borrow_mut().clear();

    let document = core.save_state(true).expect("state save");
    assert!(document.contains("\"cards\""));
    assert!(document.contains("\"version\":1"));

    // Mutating the live rack then restoring rewinds the observable state.
    core.frequency(0, 100);
    core.now.set(999_999);
    core.load_state(&document).expect("state load");
    assert_eq!(core.now.get(), 1_234);
    assert_eq!(core.clocks[0].get().hz, 800_000);
    assert_eq!(core.rng.get(), 4242);
}

#[test]
fn a_state_document_with_the_wrong_version_is_rejected() {
    let (core, _space) = machine();
    let document = core.save_state(false).unwrap();
    let broken = document.replace("\"version\":1", "\"version\":9");
    assert!(core.load_state(&broken).is_err());
    let truncated = "{\"version\":1}";
    assert!(core.load_state(truncated).is_err());
    assert!(core.load_state("{").is_err());
}

#[test]
fn a_state_restore_requires_a_matching_card_count() {
    let (core, _space) = machine();
    let document = core.save_state(false).unwrap();
    core.attach_test_card("extra");
    assert!(core.load_state(&document).is_err());
}

#[test]
fn project_numbers_accept_the_documented_forms() {
    use crate::project::number;
    use serde_json::json;
    assert_eq!(number(&json!("0xFFFF")).unwrap(), 0xFFFF);
    assert_eq!(number(&json!(255)).unwrap(), 255);
    assert!(number(&json!("0x")).is_err());
    assert!(number(&json!("")).is_err());
}

#[test]
fn audio_master_volume_and_clipping_flags_round_trip() {
    let core = core();
    assert_eq!(core.audio_sample_rate(), 44100);
    assert_eq!(core.audio_channels(), 2);
    core.audio_set_master_volume(150);
    assert_eq!(core.audio_master_volume(), 100);
    core.audio_set_master_volume(37);
    assert_eq!(core.audio_master_volume(), 37);
    assert!(!core.audio_dc_offset_correction());
    core.audio_set_dc_offset_correction(true);
    assert!(core.audio_dc_offset_correction());
    core.audio_set_dc_offset_correction(false);
    assert!(!core.audio_dc_offset_correction());
    assert!(!core.audio_software_clipping());
    core.audio_set_software_clipping(true);
    assert!(core.audio_software_clipping());
}

#[test]
fn a_missing_audio_source_is_reported_not_found() {
    let core = core();
    assert!(!core.audio_set_source_volume(0xBEEF, 50));
    assert!(!core.audio_set_source_muted(0xBEEF, true));
    assert!(core.audio_sources().is_empty());
}

#[test]
fn the_audio_queue_capacity_is_bounded_and_reported() {
    let core = core();
    // A zero request is clamped to one frame rather than accepted.
    core.audio_set_queue_capacity(0);
    assert_eq!(core.audio_diagnostics().queue_capacity_frames, 1);
    // An excessive request is clamped to ten minutes of audio.
    let ten_minutes = 10 * 60 * core.audio_sample_rate() as u64;
    core.audio_set_queue_capacity(u64::MAX);
    assert_eq!(core.audio_diagnostics().queue_capacity_frames, ten_minutes);
    // A request below the ceiling is honoured exactly.
    core.audio_set_queue_capacity(441);
    assert_eq!(core.audio_diagnostics().queue_capacity_frames, 441);
}

#[test]
fn configuration_values_round_trip_with_fallbacks() {
    let core = core();
    core.config_set("probe.key", "value");
    assert_eq!(core.config_value("probe.key", ""), "value");
    assert_eq!(core.config_value("probe.missing", "fallback"), "fallback");
}

#[test]
fn a_missing_configuration_entry_reports_not_found() {
    let core = core();
    assert_eq!(core.config_entry_get("probe.missing"), Err(SRH_NOT_FOUND));
    assert_eq!(core.config_entry_set("probe.missing", "x"), SRH_NOT_FOUND);
}

#[test]
fn plugin_project_data_is_empty_without_a_card_that_reports_one() {
    let core = core();
    assert!(core.save_plugin_data().unwrap().is_empty());
    assert!(core.load_plugin_data(0xBEEF, "00").is_err());
    assert!(core.load_plugin_data(0, "0").is_err());
}

#[test]
fn the_engine_time_mode_owns_the_epoch() {
    let core = core();
    core.mode.set(TimeMode::Fixed);
    core.epoch.set(12_345);
    assert_eq!(core.time_ns(), 12_345);
    core.mode.set(TimeMode::Project);
    core.now.set(1);
    assert_eq!(core.time_ns(), 12_346);
}

#[test]
fn a_frequency_change_suppresses_one_pending_boundary() {
    let core = core();
    let location = [1u64, 2, 3];
    core.stopped_boundary.set(Some(location));
    core.suppress_once();
    assert_eq!(core.stopped_boundary.get(), None);
    assert_eq!(core.skip_boundary.get(), Some(location));
}

#[test]
fn a_nested_frame_defers_collection_until_it_unwinds() {
    let core = core();
    let card = core.attach_test_card("a");
    core.enter_frame();
    core.enter_frame();
    let _ = core.remove(card);
    core.collection_pending.set(true);
    core.leave_frame();
    // Still inside the outer frame, so nothing has been collected yet.
    assert_eq!(core.cards.borrow().len(), 1);
    core.leave_frame();
    assert!(core.cards.borrow().is_empty());
}

#[test]
fn a_full_trace_ring_reports_its_capacity() {
    assert_eq!(crate::core::TRACE_CAPACITY, 8192);
    assert_eq!(BILLION, 1_000_000_000);
}

/// Reads `table[offset % 8]`. The mapping context points at an 8-byte table.
unsafe extern "C" fn table_read(
    context: *mut core::ffi::c_void,
    offset: u64,
    value: *mut u8,
) -> SrhStatus {
    // Safety: the registration passes an 8-byte table as its context.
    let table = unsafe { core::slice::from_raw_parts(context as *const u8, 8) };
    // Safety: the bus supplies a valid output byte.
    unsafe { *value = table[offset as usize % 8] };
    SRH_OK
}

unsafe extern "C" fn table_write(
    context: *mut core::ffi::c_void,
    offset: u64,
    value: u8,
) -> SrhStatus {
    // Safety: the registration passes an 8-byte table as its context.
    let table = unsafe { core::slice::from_raw_parts_mut(context as *mut u8, 8) };
    table[offset as usize % 8] = value;
    SRH_OK
}

fn table_mapping(space: u64, table: *mut u8, first: u64, last: u64, priority: i32) -> SrhMapping {
    SrhMapping {
        abi_version: SRH_ABI,
        struct_size: core::mem::size_of::<SrhMapping>() as u32,
        space,
        first,
        last,
        priority,
        context: table as *mut core::ffi::c_void,
        read: Some(table_read),
        write: Some(table_write),
        peek: None,
        read_word: None,
    }
}

#[test]
fn a_card_mapping_serves_the_bus_and_priority_chooses_the_responder() {
    let (core, space) = machine();
    let card = core.attach_test_card("memory");

    let mut low = [1u8, 2, 3, 4, 5, 6, 7, 8];
    let mut high = [9u8, 9, 9, 9, 9, 9, 9, 9];
    let mut low_id = 0u64;
    let mut high_id = 0u64;
    assert_eq!(
        core.mapping(
            card,
            &table_mapping(space, low.as_mut_ptr(), 0x10, 0x1F, 1),
            &mut low_id
        ),
        SRH_OK
    );
    assert_eq!(
        core.mapping(
            card,
            &table_mapping(space, high.as_mut_ptr(), 0x10, 0x1F, 9),
            &mut high_id
        ),
        SRH_OK
    );
    assert_ne!(low_id, high_id);

    let mut value = 0u8;
    assert_eq!(core.read(0, space, 0x12, &mut value, false), SRH_OK);
    assert_eq!(value, 9, "the higher-priority mapping must answer first");
    // A write fans out to every applicable mapping, so both tables change.
    assert_eq!(core.write(0, space, 0x13, 0x5A), SRH_OK);
    assert_eq!(high[3], 0x5A);
    assert_eq!(low[3], 0x5A);

    // Retiring the stronger mapping exposes the weaker one again, which also
    // proves the route cache is invalidated.
    assert_eq!(core.unmap(high_id), SRH_OK);
    assert_eq!(core.read(0, space, 0x12, &mut value, false), SRH_OK);
    assert_eq!(value, 3);
    assert_eq!(core.unmap(high_id), SRH_NOT_FOUND);

    // Out-of-range addresses stay unclaimed.
    assert_eq!(core.read(0, space, 0x40, &mut value, false), SRH_OK);
    assert_eq!(value, 7);
}

#[test]
fn a_mapping_registered_inside_a_dispatch_appears_at_the_boundary() {
    let (core, space) = machine();
    let card = core.attach_test_card("memory");
    let mut table = [1u8, 2, 3, 4, 5, 6, 7, 8];

    // Reading before the mapping exists caches "no responder here".
    let mut value = 0u8;
    assert_eq!(core.read(0, space, 0x12, &mut value, false), SRH_OK);
    assert_eq!(value, 7);

    core.enter_frame();
    let mut id = 0u64;
    assert_eq!(
        core.mapping(
            card,
            &table_mapping(space, table.as_mut_ptr(), 0x10, 0x1F, 0),
            &mut id
        ),
        SRH_OK
    );
    // Inside the dispatch the new mapping is not visible yet.
    assert_eq!(core.read(0, space, 0x12, &mut value, false), SRH_OK);
    assert_eq!(value, 7, "a mapping must not become visible mid-dispatch");
    core.leave_frame();
    // The frame boundary publishes it.
    assert_eq!(core.read(0, space, 0x12, &mut value, false), SRH_OK);
    assert_eq!(value, 3);
}

#[test]
fn routing_intervals_respect_overlap_gaps_spaces_and_live_changes() {
    let core = core();
    let space = core.space("wide".into(), u64::MAX, 7, false, Resolver::Priority);
    let other = core.space("other".into(), u64::MAX, 8, false, Resolver::Priority);
    let owner = core.attach_test_card("memory");
    let mut low = [1u8; 8];
    let mut high = [2u8; 8];
    let mut base = 0;
    let mut overlay = 0;
    core.mapping(
        owner,
        &table_mapping(space, low.as_mut_ptr(), 10, 100, 0),
        &mut base,
    );
    core.mapping(
        owner,
        &table_mapping(space, high.as_mut_ptr(), 20, 30, 1),
        &mut overlay,
    );
    // Populate intervals in an order that exercises predecessor lookup as well
    // as the recent region, including unmapped intervals at both u64 edges.
    for (address, expected) in [
        (15, 1),
        (0, 7),
        (u64::MAX, 7),
        (30, 2),
        (31, 1),
        (19, 1),
        (20, 2),
        (100, 1),
        (101, 7),
    ] {
        let mut value = 0;
        assert_eq!(core.read(0, space, address, &mut value, false), SRH_OK);
        assert_eq!(value, expected);
        assert_eq!(core.read(0, other, address, &mut value, false), SRH_OK);
        assert_eq!(value, 8, "cache must distinguish spaces");
    }
    assert_eq!(core.park(owner), SRH_OK);
    let mut value = 0;
    core.read(0, space, 20, &mut value, false);
    assert_eq!(value, 7);
    assert_eq!(core.plug(owner), SRH_OK);
    core.read(0, space, 20, &mut value, false);
    assert_eq!(value, 2);
    core.enter_frame();
    assert_eq!(core.unmap(overlay), SRH_OK);
    core.read(0, space, 20, &mut value, false);
    assert_eq!(
        value, 1,
        "retired mappings must be skipped before collection"
    );
    core.leave_frame();
    core.read(0, space, 20, &mut value, false);
    assert_eq!(value, 1);
    let mut edge = 0;
    core.mapping(
        owner,
        &table_mapping(space, high.as_mut_ptr(), u64::MAX, u64::MAX, 0),
        &mut edge,
    );
    core.read(0, space, u64::MAX, &mut value, false);
    assert_eq!(
        value, 2,
        "new mapping must invalidate cached unmapped regions"
    );
}

#[test]
fn sequential_addresses_share_routes_instead_of_allocating_per_byte() {
    let core = core();
    let space = core.space("ram".into(), 0xffff, 0, false, Resolver::Priority);
    let owner = core.attach_test_card("ram");
    let mut table = [1u8; 8];
    let mut mapping = 0;
    core.mapping(
        owner,
        &table_mapping(space, table.as_mut_ptr(), 0, 0xffff, 0),
        &mut mapping,
    );
    let first = core.routes(space, 0);
    for address in 1..=0xffff {
        assert!(std::rc::Rc::ptr_eq(&first, &core.routes(space, address)));
    }
    assert_eq!(core.route_cache.borrow().regions.len(), 1);
}

#[test]
fn zero_wall_budget_does_not_dispatch_or_advance_time() {
    let core = core();
    let owner = core.attach_test_card("clock");
    core.frequency(0, 8_000_000);
    core.resume();
    for subscriber in [false, true] {
        if subscriber {
            let mut event = 0;
            core.subscribe(owner, 0, no_op, std::ptr::null_mut(), &mut event);
        }
        core.run_slice(1000, BILLION, 0);
        assert_eq!(core.now.get(), 0);
        assert_eq!(core.clocks[0].get().ticks, 0);
    }
}

unsafe extern "C" fn constant_word(
    _context: *mut core::ffi::c_void,
    _address: u64,
    value: *mut u32,
) -> SrhStatus {
    unsafe { *value = 0x01010101 };
    SRH_OK
}

#[test]
fn word_routes_reject_crossing_an_overlay_or_unmapped_edge() {
    let core = core();
    let space = core.space("memory".into(), 0xffff, 0, false, Resolver::Priority);
    let owner = core.attach_test_card("memory");
    let mut table = [1u8; 8];
    let mut mapping = table_mapping(space, table.as_mut_ptr(), 0, 31, 0);
    mapping.read_word = Some(constant_word);
    let mut base = 0;
    core.mapping(owner, &mapping, &mut base);
    core.trace_capture.set(false);
    let mut value = 0;
    assert_eq!(core.read_word(owner, space, 0, &mut value), SRH_OK);
    assert_eq!(value, 0x01010101);
    assert_eq!(core.read_word(owner, space, 28, &mut value), SRH_OK);
    assert_eq!(
        core.read_word(owner, space, 29, &mut value),
        SRH_UNAVAILABLE
    );
    let mut overlay = 0;
    core.mapping(
        owner,
        &table_mapping(space, table.as_mut_ptr(), 2, 2, 1),
        &mut overlay,
    );
    assert_eq!(core.read_word(owner, space, 0, &mut value), SRH_UNAVAILABLE);
    assert_eq!(core.read_word(owner, space, 3, &mut value), SRH_OK);
    core.unmap(overlay);
    assert_eq!(core.read_word(owner, space, 0, &mut value), SRH_OK);
}

const PROVIDER_SNAPSHOT: &str = "{\"channels\":2}";

unsafe extern "C" fn provider_snapshot(
    _context: *mut core::ffi::c_void,
    buffer: *mut core::ffi::c_char,
    size: *mut u64,
) -> SrhStatus {
    let needed = PROVIDER_SNAPSHOT.len() as u64 + 1;
    if buffer.is_null() {
        // Safety: the engine supplied a size output.
        unsafe { *size = needed };
        return SRH_OK;
    }
    // Safety: the engine promises a buffer of `*size` bytes.
    if unsafe { *size } < needed {
        return SRH_INVALID;
    }
    // Safety: the copy is bounded by the capacity checked above.
    unsafe {
        core::ptr::copy_nonoverlapping(
            PROVIDER_SNAPSHOT.as_ptr(),
            buffer as *mut u8,
            PROVIDER_SNAPSHOT.len(),
        );
        *buffer.add(PROVIDER_SNAPSHOT.len()) = 0;
        *size = needed;
    }
    SRH_OK
}

static LAST_COMMAND_KIND: core::sync::atomic::AtomicU32 =
    core::sync::atomic::AtomicU32::new(u32::MAX);
static LAST_COMMAND_REVISION: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);

unsafe extern "C" fn provider_command(
    _context: *mut core::ffi::c_void,
    kind: u32,
    revision: u64,
    _text: *const core::ffi::c_char,
    _size: u64,
) -> SrhStatus {
    LAST_COMMAND_KIND.store(kind, core::sync::atomic::Ordering::SeqCst);
    LAST_COMMAND_REVISION.store(revision, core::sync::atomic::Ordering::SeqCst);
    SRH_OK
}

fn fixed_text(text: &str) -> [core::ffi::c_char; 128] {
    let mut field = [0 as core::ffi::c_char; 128];
    for (index, byte) in text.bytes().enumerate() {
        if index + 1 >= field.len() {
            break;
        }
        field[index] = byte as core::ffi::c_char;
    }
    field
}

fn provider(flags: u32) -> SrhDataProviderV1 {
    SrhDataProviderV1 {
        abi_version: SRH_ABI,
        struct_size: core::mem::size_of::<SrhDataProviderV1>() as u32,
        context: core::ptr::null_mut(),
        snapshot: Some(provider_snapshot),
        command: Some(provider_command),
        name: fixed_text("probe"),
        protocol: fixed_text("srz80.probe.v1"),
        flags,
    }
}

#[test]
fn a_data_provider_publishes_a_snapshot_and_gates_live_configuration() {
    let core = core();
    let card = core.attach_test_card("probe");
    let registration = provider(0);
    assert_eq!(core.register_provider(card, &registration), SRH_OK);
    // One provider per owner.
    assert_eq!(core.register_provider(card, &registration), SRH_CONFLICT);

    let published = core.provider_data();
    assert_eq!(published.len(), 1);
    assert_eq!(published[0].0, card);
    assert_eq!(published[0].1.name, "probe");
    assert_eq!(published[0].1.protocol, "srz80.probe.v1");
    assert_eq!(published[0].1.data, PROVIDER_SNAPSHOT);

    // A stale header, a missing snapshot callback and an incomplete string are
    // all rejected.
    let other = core.attach_test_card("probe2");
    let mut broken = provider(0);
    broken.abi_version = SRH_ABI + 1;
    assert_eq!(core.register_provider(other, &broken), SRH_INVALID);
    let mut no_snapshot = provider(0);
    no_snapshot.snapshot = None;
    assert_eq!(core.register_provider(other, &no_snapshot), SRH_INVALID);
    let mut unnamed = provider(0);
    unnamed.name = [0 as core::ffi::c_char; 128];
    assert_eq!(core.register_provider(other, &unnamed), SRH_INVALID);
    // An unknown flag bit is rejected rather than ignored.
    let mut bad_flags = provider(0x8000_0000);
    assert_eq!(core.register_provider(other, &bad_flags), SRH_INVALID);
    bad_flags = provider(SRH_PROVIDER_LIVE_CONFIG);
    assert_eq!(core.register_provider(other, &bad_flags), SRH_OK);

    // Configuring a stopped rack is fine. Configuring a running one needs the
    // live-configuration capability, which only the second provider advertises.
    assert_eq!(
        core.provider_command(card, SRH_PROVIDER_CONFIGURE, 3, "{}"),
        SRH_OK
    );
    assert_eq!(
        LAST_COMMAND_KIND.load(core::sync::atomic::Ordering::SeqCst),
        SRH_PROVIDER_CONFIGURE
    );
    assert_eq!(
        LAST_COMMAND_REVISION.load(core::sync::atomic::Ordering::SeqCst),
        3
    );
    core.resume();
    assert_eq!(
        core.provider_command(card, SRH_PROVIDER_CONFIGURE, 4, "{}"),
        SRH_CONFLICT
    );
    assert_eq!(
        core.provider_command(other, SRH_PROVIDER_CONFIGURE, 4, "{}"),
        SRH_OK
    );
    // Interaction is not gated by the run state, and an unknown provider is not
    // found.  Malformed payloads are rejected before any callback runs.
    assert_eq!(
        core.provider_command(card, SRH_PROVIDER_INTERACTION, 1, "{}"),
        SRH_OK
    );
    assert_eq!(
        core.provider_command(0xBEEF, SRH_PROVIDER_INTERACTION, 1, "{}"),
        SRH_NOT_FOUND
    );
    assert_eq!(core.provider_command(card, 9, 1, "{}"), SRH_INVALID);
    assert_eq!(
        core.provider_command(card, SRH_PROVIDER_INTERACTION, 1, ""),
        SRH_INVALID
    );
    assert_eq!(
        core.provider_command(card, SRH_PROVIDER_INTERACTION, 1, "with\0nul"),
        SRH_INVALID
    );
}

#[test]
fn an_input_endpoint_releases_records_only_when_they_are_due() {
    let core = core();
    let card = core.attach_test_card("uart");
    let mut endpoint = 0u64;
    assert_eq!(
        core.register_input(card, "uart.rx", 4, &mut endpoint),
        SRH_OK
    );
    // The name is unique per rack, and an unknown owner is rejected.
    assert_eq!(
        core.register_input(card, "uart.rx", 4, &mut endpoint),
        SRH_CONFLICT
    );
    assert_eq!(
        core.register_input(0xBEEF, "other", 4, &mut endpoint),
        SRH_INVALID
    );

    assert!(core.enqueue_input("uart.rx", 100, 0x41).is_ok());
    let mut due = 0u64;
    assert_eq!(core.input_due(endpoint, &mut due), SRH_OK);
    assert_eq!(due, 0, "a record in the future is not due");
    let mut timestamp = 0u64;
    let mut value = 0u8;
    assert_eq!(
        core.input_pop(endpoint, &mut timestamp, &mut value),
        SRH_UNAVAILABLE
    );

    core.now.set(100);
    assert_eq!(core.input_due(endpoint, &mut due), SRH_OK);
    assert_eq!(due, 1);
    assert_eq!(core.input_pop(endpoint, &mut timestamp, &mut value), SRH_OK);
    assert_eq!((timestamp, value), (100, 0x41));
    // The endpoint is empty again.
    assert_eq!(
        core.input_pop(endpoint, &mut timestamp, &mut value),
        SRH_UNAVAILABLE
    );

    // Capacity bounds a batch, and the whole batch is refused rather than
    // partially accepted.
    assert_eq!(
        core.enqueue_input_batch("uart.rx", 200, &[1, 2, 3, 4, 5], 0, 0),
        SRH_UNAVAILABLE
    );
    assert_eq!(core.input_due(endpoint, &mut due), SRH_OK);
    assert_eq!(due, 0);
    assert_eq!(
        core.enqueue_input_batch("uart.rx", 200, &[1, 2, 3, 4], 0, 0),
        SRH_OK
    );
    // A known endpoint name pairs with the expected owner.
    assert_eq!(
        core.enqueue_input_batch("uart.rx", 300, &[9], 0, 0xBEEF),
        SRH_CONFLICT
    );
    // An unknown endpoint is reported as an error.
    assert!(core.enqueue_input("uart.tx", 1, 1).is_err());
    // Endpoint names are discoverable for the properties/console surface.
    assert_eq!(core.input_endpoints(), vec!["uart.rx".to_string()]);
}

/// A 2x2 RGBA8 surface served in one chunk per call.
const VIDEO_SURFACE: [u8; 16] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];

unsafe extern "C" fn video_query(
    _context: *mut core::ffi::c_void,
    offset: u64,
    buffer: *mut u8,
    size: *mut u32,
    total: *mut u32,
) -> SrhStatus {
    let start = offset as usize;
    let available = VIDEO_SURFACE.len().saturating_sub(start);
    // Safety: the engine supplied capacity and total outputs.
    let capacity = unsafe { *size } as usize;
    let copied = available.min(capacity);
    if !buffer.is_null() && copied != 0 {
        // Safety: `copied` bytes fit in the caller's buffer.
        unsafe {
            core::ptr::copy_nonoverlapping(VIDEO_SURFACE.as_ptr().add(start), buffer, copied)
        };
    }
    // Safety: the engine supplied capacity and total outputs.
    unsafe {
        *size = available as u32;
        *total = VIDEO_SURFACE.len() as u32;
    }
    SRH_OK
}

#[test]
fn a_video_surface_fills_caller_buffers_and_validates_them() {
    let core = core();
    let card = core.attach_test_card("video");
    let mut surface = 0u64;
    assert_eq!(
        core.register_video(
            card,
            2,
            2,
            SRH_VIDEO_RGBA8,
            SRH_VIDEO_ALLOW_SHADER,
            video_query,
            core::ptr::null_mut(),
            &mut surface
        ),
        SRH_OK
    );
    assert_eq!(core.video_surfaces().len(), 1);
    assert_eq!(core.video_surfaces()[0].width, 2);
    assert_eq!(core.video_surfaces()[0].height, 2);
    assert_eq!(core.video_surfaces()[0].flags, SRH_VIDEO_ALLOW_SHADER);

    // Registration validates the geometry and the pixel format.
    assert_eq!(
        core.register_video(
            card,
            0,
            2,
            SRH_VIDEO_RGBA8,
            0,
            video_query,
            core::ptr::null_mut(),
            &mut surface
        ),
        SRH_INVALID
    );
    assert_eq!(
        core.register_video(
            card,
            2,
            2,
            99,
            0,
            video_query,
            core::ptr::null_mut(),
            &mut surface
        ),
        SRH_INVALID
    );
    assert_eq!(
        core.register_video(
            card,
            2,
            2,
            SRH_VIDEO_RGBA8,
            2,
            video_query,
            core::ptr::null_mut(),
            &mut surface
        ),
        SRH_INVALID
    );

    let mut buffer = [0u8; 16];
    let mut size = buffer.len() as u32;
    let mut total = 0u32;
    assert_eq!(
        core.video_read(surface, 0, buffer.as_mut_ptr(), &mut size, &mut total),
        SRH_OK
    );
    assert_eq!(size, 16);
    assert_eq!(total, 16);
    assert_eq!(buffer, VIDEO_SURFACE);

    // A chunked read at an offset returns the tail.
    let mut tail = [0u8; 8];
    let mut tail_size = tail.len() as u32;
    assert_eq!(
        core.video_read(surface, 8, tail.as_mut_ptr(), &mut tail_size, &mut total),
        SRH_OK
    );
    assert_eq!(tail_size, 8);
    assert_eq!(tail, VIDEO_SURFACE[8..]);

    // A card that claims to have written more than the caller's capacity is
    // refused instead of overrunning the buffer.
    let mut small = [0u8; 4];
    let mut small_size = small.len() as u32;
    assert_eq!(
        core.video_read(surface, 0, small.as_mut_ptr(), &mut small_size, &mut total),
        SRH_INVALID
    );
    // An unknown surface is not found.
    assert_eq!(
        core.video_read(0xBEEF, 0, buffer.as_mut_ptr(), &mut size, &mut total),
        SRH_NOT_FOUND
    );
}

#[test]
fn the_result_arena_keeps_interned_string_pointers_stable() {
    let mut arena = crate::result::ResultArena::new();
    let first = arena.intern("card.ram");
    let second = arena.intern("card.rom");
    assert_ne!(first, second);
    // Interning the same text again reuses the allocation.
    assert_eq!(arena.intern("card.ram"), first);
    // Growing the arena must not move earlier allocations, because the C++
    // caller keeps them across accessor calls.
    for index in 0..128 {
        arena.intern(&format!("card.{index}"));
    }
    assert_eq!(arena.intern("card.ram"), first);
    assert_eq!(arena.intern("card.rom"), second);
    // A fill/clear cycle keeps the storage and invalidates only the records.
    arena.handles.push(1);
    arena.clear();
    assert!(arena.handles.is_empty());
    assert_eq!(arena.intern("card.rom"), second);
    // An interior NUL cannot be represented across the ABI, so it is dropped
    // rather than truncating the string.
    let pointer = arena.intern("a\0b");
    // Safety: `intern` returns a live NUL-terminated string.
    let text = unsafe { std::ffi::CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned();
    assert_eq!(text, "ab");
}

/// Renders a constant amplitude read from the context.
unsafe extern "C" fn amplitude_source(
    context: *mut core::ffi::c_void,
    _frame: u64,
    frames: u32,
    samples: *mut i16,
) -> SrhStatus {
    // Safety: the registration passes an `i16` as its context.
    let amplitude = unsafe { *(context as *const i16) };
    for index in 0..frames as usize * 2 {
        // Safety: the engine provides `frames * channels` samples.
        unsafe { *samples.add(index) = amplitude };
    }
    SRH_OK
}

unsafe extern "C" fn failing_source(
    _context: *mut core::ffi::c_void,
    _frame: u64,
    _frames: u32,
    _samples: *mut i16,
) -> SrhStatus {
    SRH_ERROR
}

fn render_frames(core: &Core, seconds: u64, frames: usize) -> Vec<i16> {
    core.audio_reset();
    core.now.set(seconds * BILLION);
    core.audio_advance();
    let mut buffer = vec![0i16; frames * 2];
    let read = core.audio_read(buffer.as_mut_ptr(), frames as u32);
    assert_eq!(read, frames as u32, "the mixer produced too few frames");
    buffer
}

#[test]
fn audio_mixing_applies_source_volume_mute_master_volume_and_clipping() {
    let core = core();
    let card = core.attach_test_card("audio");
    let quiet: i16 = 1000;
    let loud: i16 = 32_000;
    let mut quiet_source = 0u64;
    let mut loud_source = 0u64;
    assert_eq!(
        core.audio_register_source(
            card,
            44100,
            2,
            SRH_AUDIO_S16_STEREO,
            "quiet",
            amplitude_source,
            &quiet as *const i16 as *mut core::ffi::c_void,
            &mut quiet_source
        ),
        SRH_OK
    );
    assert_eq!(
        core.audio_register_source(
            card,
            44100,
            2,
            SRH_AUDIO_S16_STEREO,
            "loud",
            amplitude_source,
            &loud as *const i16 as *mut core::ffi::c_void,
            &mut loud_source
        ),
        SRH_OK
    );
    // The registration validates the sample rate, the channel count and the
    // format, and the same callback cannot be registered twice by one owner.
    assert_eq!(
        core.audio_register_source(
            card,
            48000,
            2,
            SRH_AUDIO_S16_STEREO,
            "rate",
            amplitude_source,
            core::ptr::null_mut(),
            &mut quiet_source
        ),
        SRH_INVALID
    );
    assert_eq!(
        core.audio_register_source(
            card,
            44100,
            1,
            SRH_AUDIO_S16_STEREO,
            "mono",
            amplitude_source,
            core::ptr::null_mut(),
            &mut quiet_source
        ),
        SRH_INVALID
    );
    assert_eq!(
        core.audio_register_source(
            card,
            44100,
            2,
            SRH_AUDIO_S16_STEREO,
            "dup",
            amplitude_source,
            &quiet as *const i16 as *mut core::ffi::c_void,
            &mut quiet_source
        ),
        SRH_CONFLICT
    );

    // Both sources at full volume sum their amplitudes.
    let mut buffer = render_frames(&core, 1, 4);
    assert_eq!(buffer[0], i16::MAX, "two loud sources saturate the mixer");

    // Source volume scales one source only.
    core.audio_set_source_volume(quiet_source, 50);
    core.audio_set_source_volume(loud_source, 0);
    buffer = render_frames(&core, 2, 4);
    assert_eq!(buffer[0], 500);

    // Muting removes the source from the mix entirely.
    core.audio_set_source_muted(quiet_source, true);
    buffer = render_frames(&core, 3, 4);
    assert_eq!(buffer[0], 0);
    assert!(core.audio_set_source_muted(quiet_source, false));

    // The master volume is applied after the sources.
    core.audio_set_source_volume(quiet_source, 100);
    core.audio_set_master_volume(25);
    buffer = render_frames(&core, 4, 4);
    assert_eq!(buffer[0], 250);
    core.audio_set_master_volume(100);

    // The soft clipper compresses everything above the knee by four.
    core.audio_set_software_clipping(true);
    core.audio_set_source_volume(quiet_source, 0);
    core.audio_set_source_volume(loud_source, 100);
    buffer = render_frames(&core, 5, 4);
    assert_eq!(buffer[0], 30_000 + (32_000 - 30_000) / 4);
    core.audio_set_software_clipping(false);
    buffer = render_frames(&core, 6, 4);
    assert_eq!(buffer[0], 32_000);
    // Values outside `i16` saturate instead of wrapping.
    core.audio_set_source_volume(loud_source, 100);
    core.audio_set_master_volume(100);
    let sources = core.audio_sources();
    assert_eq!(sources.len(), 2);
    assert!(sources.iter().all(|source| source.active));
    assert!(sources.iter().any(|source| source.muted == false));
}

#[test]
fn a_failing_audio_source_is_counted_and_contributes_silence() {
    let core = core();
    let card = core.attach_test_card("audio");
    let mut good = 0u64;
    let mut broken = 0u64;
    let amplitude: i16 = 500;
    assert_eq!(
        core.audio_register_source(
            card,
            44100,
            2,
            SRH_AUDIO_S16_STEREO,
            "good",
            amplitude_source,
            &amplitude as *const i16 as *mut core::ffi::c_void,
            &mut good
        ),
        SRH_OK
    );
    assert_eq!(
        core.audio_register_source(
            card,
            44100,
            2,
            SRH_AUDIO_S16_STEREO,
            "broken",
            failing_source,
            core::ptr::null_mut(),
            &mut broken
        ),
        SRH_OK
    );
    let buffer = render_frames(&core, 1, 4);
    // The healthy source still reaches the mix and the failure is reported.
    assert_eq!(buffer[0], 500);
    let diagnostics = core.audio_diagnostics();
    // A one-second render is split into bounded chunks, so the failing source is
    // counted once per chunk rather than once per call.
    assert!(diagnostics.source_errors >= 1);
    assert!(diagnostics.queued_frames > 0);
}

#[test]
fn the_audio_queue_drops_the_oldest_frames_when_it_overflows() {
    let core = core();
    let card = core.attach_test_card("audio");
    let amplitude: i16 = 100;
    let mut source = 0u64;
    assert_eq!(
        core.audio_register_source(
            card,
            44100,
            2,
            SRH_AUDIO_S16_STEREO,
            "constant",
            amplitude_source,
            &amplitude as *const i16 as *mut core::ffi::c_void,
            &mut source
        ),
        SRH_OK
    );
    core.audio_set_queue_capacity(4);
    // One second of audio is rendered without draining, so the bounded queue
    // keeps only its capacity and reports the frames it discarded.
    core.audio_reset();
    core.now.set(BILLION);
    core.audio_advance();
    let diagnostics = core.audio_diagnostics();
    assert_eq!(diagnostics.queue_capacity_frames, 4);
    assert_eq!(diagnostics.queued_frames, 4);
    assert!(
        diagnostics.dropped_frames > 0,
        "a one-second render must overflow a four-frame queue"
    );
    // Reading with a null buffer and no frames is a no-op rather than a crash,
    // and an empty request does not count as an underflow.
    assert_eq!(core.audio_read(core::ptr::null_mut(), 0), 0);
}

unsafe extern "C" fn no_op(_: *mut core::ffi::c_void) -> SrhStatus {
    SRH_OK
}

// Regression coverage for dispatch lifetimes and the allocation-free hot paths.
mod hot_path_regressions {
    use super::*;
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::cell::Cell;

    thread_local! {
        static COUNT: Cell<Option<usize>> = const { Cell::new(None) };
    }
    struct CountingAllocator;
    unsafe impl GlobalAlloc for CountingAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            let _ = COUNT.try_with(|count| {
                if let Some(n) = count.get() {
                    count.set(Some(n + 1));
                }
            });
            unsafe { System.alloc(layout) }
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            unsafe { System.dealloc(ptr, layout) }
        }
    }
    #[global_allocator]
    static ALLOCATOR: CountingAllocator = CountingAllocator;

    unsafe extern "C" fn noop(_: *mut core::ffi::c_void) -> SrhStatus {
        SRH_OK
    }

    #[test]
    fn warm_single_frame_audio_batches_do_not_allocate() {
        let core = core();
        let owner = core.attach_test_card("audio");
        let amplitude: i16 = 234;
        let mut source = 0;
        assert_eq!(
            core.audio_register_source(
                owner,
                44100,
                2,
                SRH_AUDIO_S16_STEREO,
                "tone",
                amplitude_source,
                (&amplitude as *const i16).cast_mut().cast(),
                &mut source
            ),
            SRH_OK
        );
        let mut samples = [0i16; 2];
        // Warm the PCM queue before measuring steady-state rendering.
        core.now.set(22676);
        core.audio_advance();
        core.audio_read(samples.as_mut_ptr(), 1);
        COUNT.with(|count| count.set(Some(0)));
        for frame in 2..1002 {
            core.now.set((frame * BILLION + 44099) / 44100);
            core.audio_advance();
            core.audio_read(samples.as_mut_ptr(), 1);
        }
        let allocations = COUNT.with(|count| count.replace(None).unwrap());
        assert_eq!(samples, [amplitude; 2]);
        assert_eq!(
            allocations, 0,
            "steady-state audio must not allocate per sample"
        );
    }

    struct AudioMutation<'a> {
        core: &'a Core,
        owner: u64,
        added: Cell<bool>,
        amplitude: i16,
    }

    unsafe extern "C" fn register_during_render(
        context: *mut core::ffi::c_void,
        _: u64,
        _: u32,
        _: *mut i16,
    ) -> SrhStatus {
        let state = unsafe { &*context.cast::<AudioMutation<'_>>() };
        if !state.added.replace(true) {
            let mut source = 0;
            assert_eq!(
                state.core.audio_register_source(
                    state.owner,
                    44100,
                    2,
                    SRH_AUDIO_S16_STEREO,
                    "new",
                    amplitude_source,
                    (&state.amplitude as *const i16).cast_mut().cast(),
                    &mut source
                ),
                SRH_OK
            );
        }
        SRH_OK
    }

    #[test]
    fn audio_registration_during_render_takes_effect_on_the_next_batch() {
        let core = core();
        let owner = core.attach_test_card("audio");
        let mut state = AudioMutation {
            core: &core,
            owner,
            added: Cell::new(false),
            amplitude: 789,
        };
        let mut source = 0;
        assert_eq!(
            core.audio_register_source(
                owner,
                44100,
                2,
                SRH_AUDIO_S16_STEREO,
                "register",
                register_during_render,
                (&mut state as *mut AudioMutation<'_>).cast(),
                &mut source
            ),
            SRH_OK
        );
        core.now.set(22676);
        core.audio_advance();
        let mut samples = [1i16; 2];
        assert_eq!(core.audio_read(samples.as_mut_ptr(), 1), 1);
        assert_eq!(samples, [0; 2]);
        core.now.set(45352);
        core.audio_advance();
        assert_eq!(core.audio_read(samples.as_mut_ptr(), 1), 1);
        assert_eq!(samples, [789; 2]);
    }

    #[test]
    fn warm_bus_and_single_subscriber_ticks_do_not_allocate() {
        let (core, space) = machine();
        let owner = core.attach_test_card("test");
        let mut table = [42u8; 8];
        let mut mapping = 0;
        assert_eq!(
            core.mapping(
                owner,
                &table_mapping(space, table.as_mut_ptr(), 0, 7, 0),
                &mut mapping
            ),
            SRH_OK
        );
        let mut subscription = 0;
        assert_eq!(
            core.subscribe(owner, 0, noop, std::ptr::null_mut(), &mut subscription),
            SRH_OK
        );
        // A pending timer must not reintroduce a snapshot allocation per tick.
        let mut timer = 0;
        assert_eq!(
            core.schedule(owner, 1_000_000, noop, std::ptr::null_mut(), &mut timer),
            SRH_OK
        );
        core.trace_capture.set(false);
        let mut byte = 0;
        assert_eq!(core.read(owner, space, 0, &mut byte, false), SRH_OK);
        COUNT.with(|count| count.set(Some(0)));
        for _ in 0..100 {
            core.tick(0);
            assert_eq!(core.read(owner, space, 0, &mut byte, false), SRH_OK);
            assert_eq!(core.write(owner, space, 0, byte), SRH_OK);
        }
        let allocations = COUNT.with(|count| count.replace(None).unwrap());
        assert_eq!(allocations, 0);
    }

    struct Mutation<'a> {
        core: &'a Core,
        owner: u64,
        cancel: Cell<u64>,
        new_calls: Cell<u32>,
        changed: Cell<bool>,
    }
    unsafe extern "C" fn count(ctx: *mut core::ffi::c_void) -> SrhStatus {
        let ctx = unsafe { &*(ctx as *const Mutation<'_>) };
        ctx.new_calls.set(ctx.new_calls.get() + 1);
        SRH_OK
    }
    unsafe extern "C" fn mutate(ctx: *mut core::ffi::c_void) -> SrhStatus {
        let state = unsafe { &*(ctx as *const Mutation<'_>) };
        if !state.changed.replace(true) {
            assert_eq!(state.core.cancel(state.cancel.get()), SRH_OK);
            let mut added = 0;
            assert_eq!(
                state.core.subscribe(state.owner, 0, count, ctx, &mut added),
                SRH_OK
            );
            // Neither the cancelled nor newly registered callback may run in
            // this nested tick before the outer dispatch boundary.
            state.core.tick(0);
        }
        SRH_OK
    }
    #[test]
    fn nested_ticks_respect_cancellation_and_registration_visibility() {
        let core = core();
        let owner = core.attach_test_card("test");
        let mut state = Mutation {
            core: &core,
            owner,
            cancel: Cell::new(0),
            new_calls: Cell::new(0),
            changed: Cell::new(false),
        };
        let ctx = &mut state as *mut _ as *mut core::ffi::c_void;
        let mut first = 0;
        let mut second = 0;
        assert_eq!(core.subscribe(owner, 0, mutate, ctx, &mut first), SRH_OK);
        assert_eq!(core.subscribe(owner, 0, count, ctx, &mut second), SRH_OK);
        state.cancel.set(second);
        core.tick(0);
        assert_eq!(state.new_calls.get(), 0);
        core.tick(0);
        assert_eq!(state.new_calls.get(), 1);
        assert_eq!(core.park(owner), SRH_OK);
        core.tick(0);
        assert_eq!(state.new_calls.get(), 1);
        assert_eq!(core.plug(owner), SRH_OK);
        core.tick(0);
        assert_eq!(state.new_calls.get(), 2);
    }

    #[test]
    fn cached_routes_survive_invalidation_without_retaining_a_cache_borrow() {
        let (core, space) = machine();
        let owner = core.attach_test_card("test");
        let mut table = [42u8; 8];
        let mut mapping = 0;
        assert_eq!(
            core.mapping(
                owner,
                &table_mapping(space, table.as_mut_ptr(), 0, 7, 0),
                &mut mapping
            ),
            SRH_OK
        );
        let snapshot = core.routes(space, 0);
        assert_eq!(core.unmap(mapping), SRH_OK);
        assert!(core.routes(space, 0).is_empty());
        assert_eq!(snapshot.len(), 1);
        assert!(!snapshot[0].active.get());
    }
}

#[test]
fn video_timing_is_optional_validated_and_only_committed_on_success() {
    unsafe extern "C" fn pixels(
        _: *mut core::ffi::c_void,
        _: u64,
        _: *mut u8,
        _: *mut u32,
        _: *mut u32,
    ) -> SrhStatus {
        SRH_OK
    }
    unsafe extern "C" fn timing(
        context: *mut core::ffi::c_void,
        out: *mut SrhVideoTiming,
    ) -> SrhStatus {
        let mode = unsafe { *context.cast::<u32>() };
        unsafe {
            (*out).frame_number = 99;
            (*out).line_count = if mode == 1 { 0 } else { 262 };
            (*out).scanline = 7;
        }
        if mode == 2 {
            SRH_ERROR
        } else {
            SRH_OK
        }
    }
    let core = core();
    let owner = core.attach_test_card("video");
    let mut surface = 0;
    assert_eq!(
        core.register_video(
            owner,
            1,
            1,
            SRH_VIDEO_RGBA8,
            SRH_VIDEO_ALLOW_SHADER,
            pixels,
            std::ptr::null_mut(),
            &mut surface
        ),
        SRH_OK
    );
    let mut out = SrhVideoTiming {
        abi_version: SRH_ABI,
        struct_size: std::mem::size_of::<SrhVideoTiming>() as u32,
        frame_number: 123,
        scanline: 0,
        line_count: 1,
    };
    assert_eq!(core.video_timing(surface, &mut out), SRH_UNAVAILABLE);
    assert_eq!(out.frame_number, 123);
    assert_eq!(
        core.set_video_timing(surface, None, std::ptr::null_mut()),
        SRH_INVALID
    );
    let mut mode = 0u32;
    assert_eq!(
        core.set_video_timing(surface, Some(timing), (&mut mode as *mut u32).cast()),
        SRH_OK
    );
    assert_eq!(core.video_timing(surface, &mut out), SRH_OK);
    assert_eq!(
        (out.frame_number, out.scanline, out.line_count),
        (99, 7, 262)
    );
    out.frame_number = 123;
    for (value, expected) in [(1, SRH_INVALID), (2, SRH_ERROR)] {
        mode = value;
        assert_eq!(core.video_timing(surface, &mut out), expected);
        assert_eq!(out.frame_number, 123);
    }
    assert_eq!(mode, 2);
    assert_eq!(core.video_timing(surface + 1, &mut out), SRH_NOT_FOUND);
    assert_eq!(core.depth.get(), 0);
}
