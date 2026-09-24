//! Opaque monotonic handles.
//!
//! Handles are never reused: generation safety comes from a strictly
//! increasing counter rather than a recycling table.

pub type Handle = u64;
