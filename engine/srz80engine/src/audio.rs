//! Deterministic audio mixing and PCM transport.
//!
//! Cards render native-rate audio into host-owned interleaved S16 stereo
//! buffers on the engine thread.  The engine resamples every source to the
//! host rate, mixes them, applies the master volume,
//! the optional DC-offset high-pass and the optional soft clipper, then pushes
//! frames into a bounded queue that the audio device drains from its own
//! thread.

use core::cell::Cell;
use core::ffi::c_void;
use std::collections::VecDeque;
use std::rc::Rc;

use crate::core::{AudioDiagnostics, AudioSourceInfo, Core, Handle, BILLION};
use crate::ffi::*;

// Preserve unity gain at center. The asymmetric endpoints both reach silence.
fn pan_sample(sample: i64, pan: i32, channel: usize) -> i64 {
    match (pan, channel) {
        (1..=63, 0) => sample * (63 - pan) as i64 / 63,
        (-64..=-1, 1) => sample * (64 + pan) as i64 / 64,
        _ => sample,
    }
}

const MAX_RENDER_FRAMES: u32 = 1024;
const RESAMPLE_NONE: u32 = 0;
const RESAMPLE_LINEAR: u32 = 1;
const RESAMPLE_BOXCAR: u32 = 2;
const RESAMPLE_COSINE: u32 = 3;
const RESAMPLE_SINC: u32 = 4;
const RESAMPLE_HISTORY_FRAMES: usize = 80;
const SINC_RADIUS: isize = 8;

fn saturated(value: i64) -> i16 {
    value.clamp(i16::MIN as i64, i16::MAX as i64) as i16
}

fn next_audio_due(now: u64, remainder: u64, sample_rate: u32) -> u64 {
    let needed = BILLION - remainder;
    let wait = core::cmp::max(1, (needed + sample_rate as u64 - 1) / sample_rate as u64);
    now.saturating_add(wait)
}

impl Core {
    pub fn audio_sample_rate(&self) -> u32 {
        self.audio_sample_rate
    }

    pub fn audio_channels(&self) -> u32 {
        self.audio_channels
    }

    /// Changes the canonical mix rate. Callers serialize this with simulation
    /// execution. Registered sources keep their declared native rates.
    pub fn audio_set_sample_rate(&mut self, sample_rate: u32) -> bool {
        if !(8_000..=384_000).contains(&sample_rate) {
            return false;
        }
        if self.audio_sample_rate == sample_rate {
            return true;
        }
        self.audio_sample_rate = sample_rate;
        self.audio_reset();
        true
    }

    pub fn audio_resampling(&self) -> u32 {
        self.audio_resampling.get()
    }

    pub fn audio_set_resampling(&self, method: u32) -> bool {
        if method > RESAMPLE_SINC {
            return false;
        }
        if self.audio_resampling.replace(method) != method {
            self.audio_reset();
        }
        true
    }

    pub fn audio_register_source(
        &self,
        owner: Handle,
        sample_rate: u32,
        channels: u32,
        format: SrhAudioFormat,
        name: &str,
        render: SrhAudioRender,
        render_context: *mut c_void,
        result: *mut Handle,
    ) -> SrhStatus {
        if !self.alive(owner)
            || sample_rate == 0
            || channels != self.audio_channels
            || format != SRH_AUDIO_S16_STEREO
            || result.is_null()
            || name.len() > 256
        {
            return SRH_INVALID;
        }
        if self.audio_sources.borrow().iter().any(|source| {
            source.active.get()
                && source.owner == owner
                && source.render as usize == render as usize
                && source.context == render_context
        }) {
            return SRH_CONFLICT;
        }
        let source = Rc::new(crate::core::AudioSource {
            id: self.next(),
            owner,
            sample_rate,
            channels,
            format,
            name: name.to_string(),
            render,
            context: render_context,
            scratch: core::cell::RefCell::new(vec![
                0i16;
                MAX_RENDER_FRAMES as usize
                    * self.audio_channels as usize
            ]),
            resample_buffer: core::cell::RefCell::new(VecDeque::new()),
            resample_history: core::cell::RefCell::new(VecDeque::new()),
            resample_phase: Cell::new(0),
            resample_next_frame: Cell::new(0),
            errors: Cell::new(0),
            volume_percent: Cell::new(100),
            pan: Cell::new(0),
            muted: Cell::new(false),
            active: Cell::new(true),
            level_peak: Cell::new(0),
        });
        let id = source.id;
        Rc::make_mut(&mut self.audio_sources.borrow_mut()).push(source);
        // Safety: the caller supplied a valid output pointer.
        unsafe { *result = id };
        SRH_OK
    }

    /// Renders every frame that `now` has advanced past.
    pub fn audio_advance(&self) {
        if self.now.get() < self.audio_last_ns.get() {
            self.audio_reset();
            return;
        }
        if self.now.get() < self.audio_next_due_ns.get() {
            return;
        }
        let delta = self.now.get() - self.audio_last_ns.get();
        self.audio_last_ns.set(self.now.get());
        let whole_seconds = delta / BILLION;
        let fraction = delta % BILLION;
        let mut frames = whole_seconds * self.audio_sample_rate as u64;
        let fractional_product =
            fraction * self.audio_sample_rate as u64 + self.audio_remainder.get();
        frames += fractional_product / BILLION;
        self.audio_remainder.set(fractional_product % BILLION);

        while frames != 0 {
            let count = core::cmp::min(frames, MAX_RENDER_FRAMES as u64) as u32;
            let samples = count as usize * self.audio_channels as usize;
            // Own the scratch across callbacks without keeping a RefCell
            // borrow alive. Nested rendering can acquire independent scratch.
            let mut mixed = self.audio_mix_scratch.take();
            mixed.resize(samples, 0);
            mixed[..samples].fill(0);
            // Copy-on-write registration keeps this snapshot stable without
            // allocating a vector on every (often single-frame) render.
            let sources = Rc::clone(&self.audio_sources.borrow());
            for source in sources.iter() {
                if !source.active.get() || !self.alive(source.owner) {
                    continue;
                }
                if source.sample_rate != self.audio_sample_rate {
                    self.audio_mix_resampled_source(source, count, &mut mixed);
                    continue;
                }
                // The scratch buffer is host-owned.  Its raw pointer is taken
                // before the callback so no `RefCell` borrow is live while
                // plugin code runs.
                let scratch_pointer = {
                    let mut scratch = source.scratch.borrow_mut();
                    for sample in scratch[..samples].iter_mut() {
                        *sample = 0;
                    }
                    scratch.as_mut_ptr()
                };
                // Safety: the source belongs to a live owner and `scratch`
                // holds at least `samples` samples for the whole call.
                let status = unsafe {
                    (source.render)(
                        source.context,
                        self.audio_next_frame.get(),
                        count,
                        scratch_pointer,
                    )
                };
                if status != SRH_OK {
                    // Per-source render errors are tracked next to the source.
                    // the aggregate lives in an interior-mutable counter.
                    source.errors.set(source.errors.get() + 1);
                    self.audio_source_errors
                        .set(self.audio_source_errors.get() + 1);
                    continue;
                }
                let scratch = source.scratch.borrow();
                for index in 0..samples {
                    let contribution = if source.muted.get() {
                        0
                    } else {
                        (scratch[index] as i64 * source.volume_percent.get() as i64) / 100
                    };
                    let contribution = pan_sample(contribution, source.pan.get(), index % 2);
                    source.level_peak.set(source.level_peak.get().max(contribution.unsigned_abs() as u32));
                    mixed[index] += contribution;
                }
            }
            {
                let mut queue = self.audio_queue.borrow_mut();
                for index in 0..samples {
                    let channel = index % self.audio_channels as usize;
                    let mut sample = mixed[index];
                    if self.audio_dc_correction.get() {
                        // A deliberately small, deterministic one-pole high-pass
                        // removes static bias without adding a second thread.
                        let state = self.audio_dc_state[channel].get();
                        let next = state + (sample as f64 - state) / 1024.0;
                        self.audio_dc_state[channel].set(next);
                        sample -= next as i64;
                    }
                    sample = (sample * self.audio_master_volume.get() as i64) / 100;
                    if self.audio_clipping.get() {
                        const KNEE: i64 = 30000;
                        if sample > KNEE {
                            sample = KNEE + (sample - KNEE) / 4;
                        } else if sample < -KNEE {
                            sample = -KNEE + (sample + KNEE) / 4;
                        }
                    }
                    let output = saturated(sample);
                    self.audio_master_peaks[channel].set(
                        self.audio_master_peaks[channel].get().max((output as i32).unsigned_abs()));
                    queue.push_back(output);
                }
                let capacity_samples = self.audio_queue_capacity.get() * self.audio_channels as u64;
                while queue.len() as u64 > capacity_samples {
                    for _ in 0..self.audio_channels {
                        queue.pop_front();
                    }
                    self.audio_dropped_frames
                        .set(self.audio_dropped_frames.get() + 1);
                }
            }
            self.audio_next_frame
                .set(self.audio_next_frame.get() + count as u64);
            *self.audio_mix_scratch.borrow_mut() = mixed;
            frames -= count as u64;
        }
        self.audio_next_due_ns.set(next_audio_due(
            self.now.get(),
            self.audio_remainder.get(),
            self.audio_sample_rate,
        ));
    }

    /// Append `frames` contiguous native-rate frames to a source's resampling
    /// buffer. Callback failures append silence so that all source timelines
    /// continue advancing together.
    fn audio_render_native(&self, source: &crate::core::AudioSource, mut frames: usize) {
        while frames != 0 {
            let count = frames.min(MAX_RENDER_FRAMES as usize);
            let samples = count * self.audio_channels as usize;
            let scratch_pointer = {
                let mut scratch = source.scratch.borrow_mut();
                scratch.resize(samples, 0);
                scratch[..samples].fill(0);
                scratch.as_mut_ptr()
            };
            let start = source.resample_next_frame.get();
            // Safety: the source belongs to a live owner and scratch contains
            // `count` interleaved stereo frames for the duration of the call.
            let status = unsafe {
                (source.render)(source.context, start, count as u32, scratch_pointer)
            };
            if status != SRH_OK {
                source.errors.set(source.errors.get() + 1);
                self.audio_source_errors
                    .set(self.audio_source_errors.get() + 1);
            }
            {
                let scratch = source.scratch.borrow();
                let mut buffer = source.resample_buffer.borrow_mut();
                for frame in 0..count {
                    buffer.push_back(if status == SRH_OK {
                        [scratch[frame * 2], scratch[frame * 2 + 1]]
                    } else {
                        [0, 0]
                    });
                }
            }
            source
                .resample_next_frame
                .set(start.saturating_add(count as u64));
            frames -= count;
        }
    }

    fn audio_mix_resampled_source(
        &self,
        source: &crate::core::AudioSource,
        host_frames: u32,
        mixed: &mut [i64],
    ) {
        let source_rate = source.sample_rate as u64;
        let host_rate = self.audio_sample_rate as u64;
        let phase = source.resample_phase.get();
        let last_position = phase + (host_frames.saturating_sub(1) as u64) * source_rate;
        let consumed = (phase + host_frames as u64 * source_rate) / host_rate;
        let lookahead = match self.audio_resampling.get() {
            RESAMPLE_LINEAR | RESAMPLE_COSINE => 1,
            RESAMPLE_SINC => SINC_RADIUS as u64,
            _ => 0,
        };
        // Retaining `consumed` establishes the next batch's front frame.
        let required_index = (last_position / host_rate + lookahead).max(consumed) as usize;
        let required_len = required_index + 1;
        let missing = required_len.saturating_sub(source.resample_buffer.borrow().len());
        self.audio_render_native(source, missing);

        if !source.muted.get() {
            let buffer = source.resample_buffer.borrow();
            let history = source.resample_history.borrow();
            for frame in 0..host_frames as usize {
                let position = phase + frame as u64 * source_rate;
                let base = (position / host_rate) as usize;
                let fraction = position % host_rate;
                for channel in 0..2 {
                    let sample = self.audio_resample_sample(
                        &history,
                        &buffer,
                        base,
                        fraction,
                        host_rate,
                        channel,
                    );
                    let contribution = sample * source.volume_percent.get() as i64 / 100;
                    let contribution = pan_sample(contribution, source.pan.get(), channel);
                    source.level_peak.set(source.level_peak.get().max(contribution.unsigned_abs() as u32));
                    mixed[frame * 2 + channel] += contribution;
                }
            }
        }

        {
            let mut buffer = source.resample_buffer.borrow_mut();
            let mut history = source.resample_history.borrow_mut();
            for _ in 0..consumed {
                if let Some(frame) = buffer.pop_front() {
                    history.push_back(frame);
                    if history.len() > RESAMPLE_HISTORY_FRAMES {
                        history.pop_front();
                    }
                }
            }
        }
        source
            .resample_phase
            .set((phase + host_frames as u64 * source_rate) % host_rate);
    }

    fn audio_resample_sample(
        &self,
        history: &VecDeque<[i16; 2]>,
        buffer: &VecDeque<[i16; 2]>,
        base: usize,
        fraction: u64,
        host_rate: u64,
        channel: usize,
    ) -> i64 {
        let sample_at = |offset: isize| -> i64 {
            let index = base as isize + offset;
            if index >= 0 {
                return buffer[index as usize][channel] as i64;
            }
            let history_index = history.len() as isize + index;
            if history_index >= 0 {
                history[history_index as usize][channel] as i64
            } else {
                buffer[0][channel] as i64
            }
        };
        match self.audio_resampling.get() {
            RESAMPLE_NONE => sample_at(0),
            RESAMPLE_LINEAR => {
                (sample_at(0) * (host_rate - fraction) as i64
                    + sample_at(1) * fraction as i64)
                    / host_rate as i64
            }
            RESAMPLE_BOXCAR => {
                let sum: i64 = (0..RESAMPLE_HISTORY_FRAMES)
                    .map(|age| sample_at(-(age as isize)))
                    .sum();
                sum / RESAMPLE_HISTORY_FRAMES as i64
            }
            RESAMPLE_COSINE => {
                let position = fraction as f64 / host_rate as f64;
                let weight = (1.0 - (core::f64::consts::PI * position).cos()) * 0.5;
                (sample_at(0) as f64 * (1.0 - weight) + sample_at(1) as f64 * weight)
                    .round() as i64
            }
            RESAMPLE_SINC => {
                let position = fraction as f64 / host_rate as f64;
                let mut weighted = 0.0;
                let mut weights = 0.0;
                for offset in (-SINC_RADIUS + 1)..=SINC_RADIUS {
                    let distance = offset as f64 - position;
                    let sinc = if distance.abs() < f64::EPSILON {
                        1.0
                    } else {
                        let angle = core::f64::consts::PI * distance;
                        angle.sin() / angle
                    };
                    let window = if distance.abs() < f64::EPSILON {
                        1.0
                    } else if distance.abs() >= SINC_RADIUS as f64 {
                        0.0
                    } else {
                        let angle = core::f64::consts::PI * distance / SINC_RADIUS as f64;
                        angle.sin() / angle
                    };
                    let weight = sinc * window;
                    weighted += sample_at(offset) as f64 * weight;
                    weights += weight;
                }
                if weights.abs() < f64::EPSILON {
                    sample_at(0)
                } else {
                    (weighted / weights).round() as i64
                }
            }
            _ => sample_at(0),
        }
    }

    pub fn audio_read(&self, interleaved: *mut i16, frames: u32) -> u32 {
        if interleaved.is_null() && frames != 0 {
            return 0;
        }
        let mut queue = self.audio_queue.borrow_mut();
        let available = (queue.len() / self.audio_channels as usize) as u32;
        let count = frames.min(available);
        for frame in 0..count {
            for channel in 0..self.audio_channels {
                // Safety: the caller provided `frames` interleaved frames.
                unsafe {
                    *interleaved
                        .add(frame as usize * self.audio_channels as usize + channel as usize) =
                        queue.pop_front().unwrap_or(0);
                }
            }
        }
        if count < frames {
            for index in (count as usize * self.audio_channels as usize)
                ..(frames as usize * self.audio_channels as usize)
            {
                // Safety: the caller provided `frames` interleaved frames.
                unsafe { *interleaved.add(index) = 0 };
            }
            self.audio_underflow_frames
                .set(self.audio_underflow_frames.get() + (frames - count) as u64);
        }
        count
    }

    pub fn audio_diagnostics(&self) -> AudioDiagnostics {
        let queue = self.audio_queue.borrow();
        AudioDiagnostics {
            queued_frames: (queue.len() / self.audio_channels as usize) as u64,
            queue_capacity_frames: self.audio_queue_capacity.get(),
            dropped_frames: self.audio_dropped_frames.get(),
            underflow_frames: self.audio_underflow_frames.get(),
            source_errors: self.audio_source_errors.get(),
        }
    }

    pub fn audio_set_queue_capacity(&self, frames: u64) {
        let frames = if frames == 0 { 1 } else { frames };
        let maximum = 10 * 60 * self.audio_sample_rate as u64;
        self.audio_queue_capacity.set(frames.min(maximum));
        let mut queue = self.audio_queue.borrow_mut();
        while queue.len() as u64 > self.audio_queue_capacity.get() * self.audio_channels as u64 {
            for _ in 0..self.audio_channels {
                queue.pop_front();
            }
            self.audio_dropped_frames
                .set(self.audio_dropped_frames.get() + 1);
        }
    }

    pub fn audio_sources(&self) -> Vec<AudioSourceInfo> {
        self.audio_sources
            .borrow()
            .iter()
            .map(|source| AudioSourceInfo {
                id: source.id,
                owner: source.owner,
                name: source.name.clone(),
                volume_percent: source.volume_percent.get(),
                muted: source.muted.get(),
                active: source.active.get() && self.alive(source.owner),
                level_peak: source.level_peak.replace(0),
            })
            .collect()
    }

    pub fn audio_set_source_volume(&self, source: Handle, percent: u32) -> bool {
        let sources = self.audio_sources.borrow();
        let Some(entry) = sources.iter().find(|entry| entry.id == source) else {
            return false;
        };
        entry.volume_percent.set(percent.min(150));
        true
    }

    pub fn audio_source_pan(&self, source: Handle) -> i32 {
        self.audio_sources.borrow().iter().find(|entry| entry.id == source)
            .map_or(0, |entry| entry.pan.get())
    }

    pub fn audio_set_source_pan(&self, source: Handle, pan: i32) -> bool {
        let sources = self.audio_sources.borrow();
        let Some(entry) = sources.iter().find(|entry| entry.id == source) else {
            return false;
        };
        entry.pan.set(pan.clamp(-64, 63));
        true
    }

    pub fn audio_set_source_muted(&self, source: Handle, muted: bool) -> bool {
        let sources = self.audio_sources.borrow();
        let Some(entry) = sources.iter().find(|entry| entry.id == source) else {
            return false;
        };
        entry.muted.set(muted);
        true
    }

    pub fn audio_master_volume(&self) -> u32 {
        self.audio_master_volume.get()
    }

    pub fn audio_master_levels(&self) -> [u32; 2] {
        [self.audio_master_peaks[0].replace(0), self.audio_master_peaks[1].replace(0)]
    }

    pub fn audio_set_master_volume(&self, percent: u32) {
        self.audio_master_volume.set(percent.min(100));
    }

    pub fn audio_dc_offset_correction(&self) -> bool {
        self.audio_dc_correction.get()
    }

    pub fn audio_set_dc_offset_correction(&self, enabled: bool) {
        self.audio_dc_correction.set(enabled);
        self.audio_dc_state[0].set(0.0);
        self.audio_dc_state[1].set(0.0);
    }

    pub fn audio_software_clipping(&self) -> bool {
        self.audio_clipping.get()
    }

    pub fn audio_set_software_clipping(&self, enabled: bool) {
        self.audio_clipping.set(enabled);
    }

    pub fn audio_reset(&self) {
        for subscription in self.audio_input.borrow_mut().queues.values_mut() {
            subscription.clear();
        }
        self.audio_queue.borrow_mut().clear();
        self.audio_next_frame.set(0);
        self.audio_last_ns.set(self.now.get());
        self.audio_remainder.set(0);
        self.audio_next_due_ns
            .set(next_audio_due(self.now.get(), 0, self.audio_sample_rate));
        self.audio_dropped_frames.set(0);
        self.audio_underflow_frames.set(0);
        self.audio_source_errors.set(0);
        self.audio_dc_state[0].set(0.0);
        self.audio_dc_state[1].set(0.0);
        self.audio_master_peaks[0].set(0);
        self.audio_master_peaks[1].set(0);
        for source in self.audio_sources.borrow().iter() {
            source.level_peak.set(0);
            source.errors.set(0);
            source.resample_buffer.borrow_mut().clear();
            source.resample_history.borrow_mut().clear();
            source.resample_phase.set(0);
            source.resample_next_frame.set(0);
        }
    }
}
