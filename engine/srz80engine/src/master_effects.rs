//! Master effects operate on unclipped, normalized stereo frames, in order.
//! Effects own their DSP state; configuration never allocates in the render path.

use std::any::Any;

use crate::ffi::{SrzAudioCompressor, SRZ80_ENGINE_ABI};

pub trait SignalEffect: Any {
    fn process(&mut self, frame: &mut [f64; 2]);
    fn reset(&mut self, sample_rate: u32);
    fn as_any_mut(&mut self) -> &mut dyn Any;
}

#[derive(Default)]
pub struct EffectChain {
    effects: Vec<Box<dyn SignalEffect>>,
}

impl EffectChain {
    pub fn push(&mut self, effect: impl SignalEffect) -> usize {
        let slot = self.effects.len();
        self.effects.push(Box::new(effect));
        slot
    }

    pub fn effect_mut<T: SignalEffect>(&mut self, slot: usize) -> Option<&mut T> {
        self.effects.get_mut(slot)?.as_any_mut().downcast_mut()
    }

    pub fn process(&mut self, frame: &mut [f64; 2]) {
        for effect in &mut self.effects {
            effect.process(frame);
        }
    }

    pub fn reset(&mut self, sample_rate: u32) {
        for effect in &mut self.effects {
            effect.reset(sample_rate);
        }
    }
}

impl Default for SrzAudioCompressor {
    fn default() -> Self {
        Self {
            abi_version: SRZ80_ENGINE_ABI,
            struct_size: std::mem::size_of::<Self>() as u32,
            enabled: 0,
            downward_threshold_db: -12.0,
            downward_ratio: 4.0,
            upward_threshold_db: -40.0,
            upward_ratio: 2.0,
            attack_ms: 10.0,
            release_ms: 100.0,
            knee_db: 6.0,
            max_boost_db: 12.0,
            makeup_db: 6.0,
        }
    }
}

impl SrzAudioCompressor {
    pub fn valid(&self) -> bool {
        let within =
            |value: f32, low: f32, high: f32| value.is_finite() && (low..=high).contains(&value);
        self.abi_version == SRZ80_ENGINE_ABI
            && self.struct_size as usize >= std::mem::size_of::<Self>()
            && self.enabled <= 1
            && within(self.downward_threshold_db, -96.0, 0.0)
            && within(self.upward_threshold_db, -96.0, 0.0)
            && within(self.downward_ratio, 1.0, 20.0)
            && within(self.upward_ratio, 1.0, 20.0)
            && within(self.attack_ms, 0.1, 1000.0)
            && within(self.release_ms, 1.0, 5000.0)
            && within(self.knee_db, 0.0, 24.0)
            && within(self.max_boost_db, 0.0, 36.0)
            && within(self.makeup_db, -24.0, 24.0)
    }
}

pub struct Compressor {
    pub settings: SrzAudioCompressor,
    sample_rate: u32,
    attack: f64,
    release: f64,
    envelope: f64,
    gain_db: f64,
}

impl Compressor {
    pub fn new(sample_rate: u32) -> Self {
        let mut effect = Self {
            settings: SrzAudioCompressor::default(),
            sample_rate,
            attack: 0.0,
            release: 0.0,
            envelope: 0.0,
            gain_db: 0.0,
        };
        effect.reset(sample_rate);
        effect
    }

    pub fn configure(&mut self, settings: SrzAudioCompressor) {
        let toggled = self.settings.enabled != settings.enabled;
        self.settings = settings;
        self.update_coefficients();
        if toggled {
            self.envelope = 0.0;
            self.gain_db = 0.0;
        }
    }

    fn update_coefficients(&mut self) {
        let coefficient = |ms: f32| (-1000.0 / (ms as f64 * self.sample_rate as f64)).exp();
        self.attack = coefficient(self.settings.attack_ms);
        self.release = coefficient(self.settings.release_ms);
    }

    // Soft-knee positive part. Both compression directions use the same curve.
    fn knee(distance: f64, width: f64) -> f64 {
        if width == 0.0 || distance >= width * 0.5 {
            distance.max(0.0)
        } else if distance <= -width * 0.5 {
            0.0
        } else {
            (distance + width * 0.5).powi(2) / (2.0 * width)
        }
    }

    fn target_gain(&self, level_db: f64) -> f64 {
        let s = &self.settings;
        let reduction = Self::knee(level_db - s.downward_threshold_db as f64, s.knee_db as f64)
            * (1.0 - 1.0 / s.downward_ratio as f64);
        let boost = (Self::knee(s.upward_threshold_db as f64 - level_db, s.knee_db as f64)
            * (1.0 - 1.0 / s.upward_ratio as f64))
            .min(s.max_boost_db as f64);
        s.makeup_db as f64 + boost - reduction
    }
}

impl SignalEffect for Compressor {
    fn process(&mut self, frame: &mut [f64; 2]) {
        if self.settings.enabled == 0 {
            return;
        }
        // Link channels with one peak detector and gain to preserve stereo balance.
        let peak = frame[0].abs().max(frame[1].abs());
        self.envelope = peak.max(self.envelope * self.release);
        let level_db = 20.0 * self.envelope.max(1e-12).log10();
        let target = self.target_gain(level_db);
        let coefficient = if target < self.gain_db {
            self.attack
        } else {
            self.release
        };
        self.gain_db = target + coefficient * (self.gain_db - target);
        let gain = 10.0_f64.powf(self.gain_db / 20.0);
        frame[0] *= gain;
        frame[1] *= gain;
    }

    fn reset(&mut self, sample_rate: u32) {
        self.sample_rate = sample_rate;
        self.update_coefficients();
        self.envelope = 0.0;
        self.gain_db = 0.0;
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

/// Project gain is cached at configuration time, avoiding pow() in the DSP loop.
pub struct Gain {
    pub tenths_db: i32,
    linear: f64,
}

impl Default for Gain {
    fn default() -> Self {
        Self {
            tenths_db: 0,
            linear: 1.0,
        }
    }
}

impl Gain {
    pub fn configure(&mut self, tenths_db: i32) {
        self.tenths_db = tenths_db.clamp(-360, 240);
        self.linear = 10.0_f64.powf(self.tenths_db as f64 / 200.0);
    }
}

impl SignalEffect for Gain {
    fn process(&mut self, frame: &mut [f64; 2]) {
        for sample in frame {
            *sample *= self.linear;
        }
    }
    fn reset(&mut self, _sample_rate: u32) {}
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

/// Existing soft clip curve, evaluated before the final integer quantization.
pub struct SoftClip {
    pub enabled: bool,
}

impl Default for SoftClip {
    fn default() -> Self {
        Self { enabled: true }
    }
}

impl SignalEffect for SoftClip {
    fn process(&mut self, frame: &mut [f64; 2]) {
        if !self.enabled {
            return;
        }
        const KNEE: f64 = 30000.0 / 32768.0;
        for sample in frame {
            if *sample > KNEE {
                *sample = KNEE + (*sample - KNEE) / 4.0;
            } else if *sample < -KNEE {
                *sample = -KNEE + (*sample + KNEE) / 4.0;
            }
        }
    }
    fn reset(&mut self, _sample_rate: u32) {}
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}
