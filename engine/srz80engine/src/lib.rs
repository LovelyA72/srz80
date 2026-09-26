//! SRZ80 Rust engine.
//!
//! This crate is the Rust implementation of the SRZ80 core behind the stable
//! engine C ABI in `<srz80/engine.h>`.  It owns the rack -- spaces, cards, bus
//! routing, clocks, events, debugger state, inputs, video/text/audio/provider
//! registrations, project loading, execution state and host configuration --
//! and loads the unchanged C card plugins declared in `<srz80/abi.h>`.
//!
//! Layout:
//!
//! - [`ffi`] declares the C ABI structures and constants. `layout_tests`
//!   asserts every size and guarded tail offset against the C headers.
//! - [`core`] owns the engine state and the deferred collection rules.
//! - [`host`] builds the callback tables a card plugin receives.
//! - [`bus`], [`runtime`], [`devices`], [`input`], [`audio`], [`providers`]
//!   and [`plugin_data`] port the corresponding C++ translation units.
//! - [`project`] and [`state`] implement project JSON and execution state.
//! - [`engine`] exports the ABI and contains panics.
//!
//! Safety rules: `unsafe` is confined to the FFI boundary, dynamic library
//! loading and calls into card plugins.  Every export validates pointers,
//! lengths, enums, ABI versions and structure sizes, and no Rust panic may
//! unwind into C.

#![deny(unsafe_op_in_unsafe_fn)]

pub mod audio;
pub mod audio_input;
pub mod bus;
pub mod config;
pub mod core;
pub mod devices;
pub mod engine;
pub mod ffi;
pub mod guard;
pub mod handle;
pub mod host;
pub mod input;
pub mod paths;
pub mod plugin;
pub mod plugin_data;
pub mod project;
pub mod providers;
pub mod result;
pub mod runtime;
pub mod state;

#[cfg(test)]
mod layout_tests;


/// The engine ABI version this library implements.
pub const ABI_VERSION: u32 = ffi::SRZ80_ENGINE_ABI;

/// Installs a diagnostic panic hook the first time an engine is created.
///
/// The guard helpers turn a panic into a status, so without this a programming
/// error would be invisible to a C++ caller that only sees `SRH_ERROR`.  The
/// hook reports the panic on stderr and then delegates to the previous hook.
#[cfg(any(test, debug_assertions))]
pub fn install_diagnostic_panic_hook() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            eprintln!("srz80engine panic: {info}");
            previous(info);
        }));
    });
}
