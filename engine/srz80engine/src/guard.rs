//! Panic containment for every entry point.
//!
//! No Rust failure may unwind into C++ callers or C card code.  Diagnostic
//! builds use these helpers to translate a panic into a status (or fallback)
//! and record it on the engine.  Shipping builds use `panic = "abort"` and the
//! direct helper variants, eliminating unwind recovery from the DLL.

use crate::ffi::SrhStatus;
#[cfg(any(test, debug_assertions))]
use crate::ffi::SRH_ERROR;

/// Runs a body with a fallback.  No engine is touched, so callers that have no
/// engine to record a failure on can still contain a panic.
#[cfg(any(test, debug_assertions))]
pub fn guard_or<F, R>(fallback: R, body: F) -> R
where
    F: FnOnce() -> R,
{
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)).unwrap_or(fallback)
}

#[cfg(not(any(test, debug_assertions)))]
pub fn guard_or<F, R>(_fallback: R, body: F) -> R
where
    F: FnOnce() -> R,
{
    body()
}

/// Records a structured failure on the engine and returns the status unchanged.
/// A null engine is accepted so a caller can report a failure before it has an
/// engine to attach it to.
pub fn fail(
    engine: *mut crate::engine::EngineHandle,
    status: SrhStatus,
    message: String,
) -> SrhStatus {
    if !engine.is_null() {
        // Safety: an engine pointer is only ever passed from the caller that
        // owns it, on its own thread.
        unsafe { (*engine).last_error.borrow_mut().clone_from(&message) };
    }
    status
}

#[cfg(any(test, debug_assertions))]
pub fn panic_message(payload: &Box<dyn core::any::Any + Send>) -> String {
    if let Some(text) = payload.downcast_ref::<&str>() {
        (*text).to_string()
    } else if let Some(text) = payload.downcast_ref::<String>() {
        text.clone()
    } else {
        "Unexpected engine failure".to_string()
    }
}

/// A body that returns a status.  A panic becomes `SRH_ERROR` with its message
/// recorded on the engine.
#[cfg(any(test, debug_assertions))]
pub fn guarded<F>(engine: *mut crate::engine::EngineHandle, body: F) -> SrhStatus
where
    F: FnOnce() -> SrhStatus,
{
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        Ok(status) => status,
        Err(payload) => {
            let message = panic_message(&payload);
            fail(engine, SRH_ERROR, message)
        }
    }
}

#[cfg(not(any(test, debug_assertions)))]
pub fn guarded<F>(_engine: *mut crate::engine::EngineHandle, body: F) -> SrhStatus
where
    F: FnOnce() -> SrhStatus,
{
    body()
}

/// A fire-and-forget body: a failure becomes the engine's last error.
#[cfg(any(test, debug_assertions))]
pub fn guarded_void<F>(engine: *mut crate::engine::EngineHandle, body: F)
where
    F: FnOnce(),
{
    if let Err(payload) = std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        let message = panic_message(&payload);
        fail(engine, SRH_ERROR, message);
    }
}

#[cfg(not(any(test, debug_assertions)))]
pub fn guarded_void<F>(_engine: *mut crate::engine::EngineHandle, body: F)
where
    F: FnOnce(),
{
    body()
}

/// A value-producing body with a fallback on failure.
#[cfg(any(test, debug_assertions))]
pub fn guarded_value<F, R>(engine: *mut crate::engine::EngineHandle, fallback: R, body: F) -> R
where
    F: FnOnce() -> R,
{
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(payload) => {
            let message = panic_message(&payload);
            fail(engine, SRH_ERROR, message);
            fallback
        }
    }
}

#[cfg(not(any(test, debug_assertions)))]
pub fn guarded_value<F, R>(_engine: *mut crate::engine::EngineHandle, _fallback: R, body: F) -> R
where
    F: FnOnce() -> R,
{
    body()
}
