//! Optional wall-clock capture transport. All access is on the engine thread.
use std::collections::{HashMap, VecDeque};
use core::ffi::c_void;
use crate::{core::Handle, ffi::*, guard::guarded};

#[derive(Default)]
pub struct AudioInput {
    pub queues: HashMap<Handle, CaptureSubscription>,
    pub rate: u32,
    pub channels: u32,
}

pub unsafe extern "C" fn request(context: *mut c_void, owner: Handle, rate: u32) -> SrhStatus {
    guarded(std::ptr::null_mut(), || {
        let Some(core) = (unsafe { crate::host::context_of(context) }) else { return SRH_INVALID; };
        if !core.alive(owner) || (rate != 0 && !(8000..=384000).contains(&rate)) { return SRH_INVALID; }
        let mut input = core.audio_input.borrow_mut();
        if rate != 0 {
            if input.queues.get(&owner).map(|s| s.rate) != Some(rate) {
                input.queues.insert(owner, CaptureSubscription::new(rate));
            }
        }
        else { input.queues.remove(&owner); }
        SRH_OK
    })
}

pub unsafe extern "C" fn read(context: *mut c_void, owner: Handle, samples: *mut f32,
    capacity: u32, rate: *mut u32, channels: *mut u32) -> u32 {
    crate::guard::guard_or(0, || {
    // A read never calls plugin code. Reject invalid pointers before touching state.
    if rate.is_null() || channels.is_null() || (capacity != 0 && samples.is_null()) { return 0; }
    unsafe { *rate = 0; *channels = 0; }
    let Some(core) = (unsafe { crate::host::context_of(context) }) else { return 0; };
    if !core.alive(owner) { return 0; }
    let mut input = core.audio_input.borrow_mut();
    let count = input.channels;
    unsafe { *channels = count; }
    if count == 0 { return 0; }
    let Some(subscription) = input.queues.get_mut(&owner) else { return 0; };
    unsafe { *rate = subscription.rate; }
    let queue = &mut subscription.queue;
    let frames = (capacity / count).min((queue.len() / count as usize) as u32);
    for i in 0..frames as usize * count as usize {
        unsafe { *samples.add(i) = queue.pop_front().unwrap_or(0.0); }
    }
    frames
    })
}

/// Streaming nearest-exact conversion: source index floor((n + 0.5) *
/// source_rate / rate). Integer phase is retained across input blocks, with
/// no interpolation, filtering or floating-point timing drift.
pub struct CaptureSubscription {
    pub rate: u32,
    pub queue: VecDeque<f32>,
    phase: u64,
    initialized: bool,
}
impl CaptureSubscription {
    fn new(rate: u32) -> Self {
        Self { rate, queue: VecDeque::new(), phase: 0, initialized: false }
    }
    pub fn clear(&mut self) {
        self.queue.clear();
        self.phase = 0;
        self.initialized = false;
    }
    pub fn push(&mut self, data: &[f32], source_rate: u32, channels: usize) {
        if !self.initialized {
            self.phase = source_rate as u64;
            self.initialized = true;
        }
        let denominator = 2 * self.rate as u64;
        let frames = data.len() / channels;
        while self.phase / denominator < frames as u64 {
            let frame = (self.phase / denominator) as usize;
            self.queue.extend(data[frame * channels..(frame + 1) * channels].iter()
                .map(|x| if x.is_finite() { x.clamp(-1.0, 1.0) } else { 0.0 }));
            self.phase += 2 * source_rate as u64;
        }
        self.phase -= frames as u64 * denominator;
        let capacity = (self.rate as usize / 4).max(8192) * channels;
        let excess = self.queue.len().saturating_sub(capacity);
        self.queue.drain(..excess);
    }
}
