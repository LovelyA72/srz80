//! Execution-state save and restore.
//!
//! The document is versioned JSON that mirrors the C++ engine byte for byte in
//! shape: handles are remapped by rack order on restore because saved ids are
//! only stable within one rack, PCM is transient and discarded, and a restore
//! that fails before installation leaves the active rack untouched.

use std::collections::BTreeMap;

use serde_json::{json, Value};

use crate::core::{Breakpoint, Core, Handle, RunState, TimeMode, Trace};
use crate::ffi::*;
use crate::plugin_data::{hex_decode, hex_encode};

fn clock_json(clock: &crate::core::Clock) -> Value {
    json!({
        "hz": clock.hz,
        "ticks": clock.ticks,
        "phase": clock.phase,
        "order": clock.order,
    })
}

fn trace_json(trace: &Trace) -> Value {
    json!({
        "sequence": trace.sequence,
        "parent": trace.parent,
        "time": trace.time,
        "ticks": [trace.ticks[0], trace.ticks[1], trace.ticks[2]],
        "master": trace.master,
        "space": trace.space,
        "address": trace.address,
        "operation": trace.operation,
        "depth": trace.depth,
        "kind": trace.kind,
        "instruction": trace.instruction,
        "value": trace.value,
        "result": trace.result,
        "responders": trace.responders,
    })
}

impl Core {
    /// Serializes the execution state.  A card that cannot report its state
    /// aborts the save instead of writing a partial document.
    pub fn save_state(&self, include_trace: bool) -> Result<String, String> {
        if self
            .all_cards()
            .iter()
            .any(|card| !card.load_error.is_empty())
        {
            return Err("Cannot save execution state with unavailable cards".into());
        }
        let mut document = serde_json::Map::new();
        document.insert("version".to_string(), json!(1));
        document.insert("seed".to_string(), json!(self.rng.get()));
        document.insert(
            "time_mode".to_string(),
            json!(match self.mode.get() {
                TimeMode::Project => "project",
                TimeMode::Fixed => "fixed",
                TimeMode::System => "system",
            }),
        );
        document.insert("epoch_ns".to_string(), json!(self.epoch.get()));
        document.insert("now".to_string(), json!(self.now.get()));
        document.insert("paused".to_string(), json!(self.paused()));
        document.insert("run_state".to_string(), json!(self.run_state.get().name()));
        document.insert(
            "stop_reason".to_string(),
            json!(self.stop_reason.borrow().clone()),
        );
        document.insert("next".to_string(), json!(self.next.get()));
        document.insert("order".to_string(), json!(self.order.get()));
        document.insert("sequence".to_string(), json!(self.sequence.get()));
        document.insert("input_order".to_string(), json!(self.input_order.get()));
        document.insert(
            "clocks".to_string(),
            Value::Array(
                (0..crate::core::CLOCK_COUNT)
                    .map(|index| clock_json(&self.clocks[index].get()))
                    .collect(),
            ),
        );
        document.insert(
            "breakpoints".to_string(),
            Value::Array(
                self.breakpoints
                    .borrow()
                    .iter()
                    .map(|point| {
                        json!({
                            "id": point.id,
                            "card": point.card,
                            "space": point.space,
                            "first": point.first,
                            "last": point.last,
                            "operations": point.operations,
                            "enabled": point.enabled,
                        })
                    })
                    .collect(),
            ),
        );
        document.insert(
            "stopped_boundary".to_string(),
            self.stopped_boundary
                .get()
                .map(|boundary| json!(boundary))
                .unwrap_or(Value::Null),
        );
        document.insert(
            "skip_boundary".to_string(),
            self.skip_boundary
                .get()
                .map(|boundary| json!(boundary))
                .unwrap_or(Value::Null),
        );

        let mut cards = Vec::new();
        for id in self.rack_order.borrow().iter() {
            let card = match self.cards.borrow().get(id) {
                Some(card) => std::rc::Rc::clone(card),
                None => continue,
            };
            if !card.is_active() && !card.is_parked() {
                continue;
            }
            let mut state = String::new();
            // Safety: `api` is live for as long as the card exists.
            let callback = unsafe { (*card.api).save_state };
            if let Some(callback) = callback {
                let mut required = 0u64;
                // Safety: the instance is live and its plugin supplied this
                // callback.
                if unsafe { callback(card.instance(), std::ptr::null_mut(), &mut required) }
                    != SRH_OK
                {
                    return Err(format!("Card state save query failed: {}", card.type_));
                }
                if required != 0 {
                    let mut bytes = vec![0u8; required as usize];
                    // Safety: `bytes` holds `required` bytes.
                    let status =
                        unsafe { callback(card.instance(), bytes.as_mut_ptr(), &mut required) };
                    if status != SRH_OK {
                        return Err(format!("Card state save failed: {}", card.type_));
                    }
                    bytes.truncate(required as usize);
                    state = hex_encode(&bytes);
                }
            }
            cards.push(json!({
                "plugin": card.type_,
                "state": state,
                "id": card.id,
                "parked": card.is_parked(),
            }));
        }
        document.insert("cards".to_string(), Value::Array(cards));
        document.insert(
            "instruction_seq".to_string(),
            Value::Array(
                self.instruction_seq
                    .borrow()
                    .iter()
                    .map(|(card, sequence)| json!({"card": card, "sequence": sequence}))
                    .collect(),
            ),
        );
        document.insert(
            "inputs".to_string(),
            Value::Array(
                self.input_records(true)
                    .into_iter()
                    .map(|(timestamp, endpoint, value)| {
                        json!({"time_ns": timestamp, "endpoint": endpoint, "value": value})
                    })
                    .collect(),
            ),
        );

        if include_trace {
            document.insert("dropped".to_string(), json!(self.dropped.get()));
            document.insert(
                "trace".to_string(),
                Value::Array(self.trace.borrow().iter().map(trace_json).collect()),
            );
        }
        Ok(Value::Object(document).to_string())
    }

    /// Restores an execution state into the existing rack.
    pub fn load_state(&self, json_text: &str) -> Result<(), String> {
        if self
            .all_cards()
            .iter()
            .any(|card| !card.load_error.is_empty())
        {
            return Err("Cannot restore execution state with unavailable cards".into());
        }
        let parsed: Value = serde_json::from_str(json_text).map_err(|error| error.to_string())?;
        let field = |name: &str| -> Result<&Value, String> {
            parsed
                .get(name)
                .ok_or_else(|| format!("missing field '{name}'"))
        };
        if field("version")?.as_i64() != Some(1) {
            return Err("Unsupported execution state version".to_string());
        }
        let saved_cards = field("cards")?
            .as_array()
            .ok_or_else(|| "field 'cards' must be an array".to_string())?;
        if saved_cards.len() != self.cards.borrow().len() {
            return Err("Execution state card count does not match rack".to_string());
        }
        // Saved handles are stable only within one engine instance.  A state is
        // restored into a freshly rebuilt rack, so cards are matched by order
        // and the saved ids are remapped for breakpoints and boundaries.
        let rack: Vec<Handle> = self.rack_order.borrow().clone();
        let mut remap: BTreeMap<Handle, Handle> = BTreeMap::new();
        for (index, saved) in saved_cards.iter().enumerate() {
            let saved_id = saved
                .get("id")
                .and_then(|value| value.as_u64())
                .ok_or_else(|| "card entry needs an id".to_string())?;
            remap.insert(saved_id, rack[index]);
        }
        let remap_card = |saved_id: Handle| -> Handle {
            if saved_id == 0 {
                return 0;
            }
            remap.get(&saved_id).copied().unwrap_or(saved_id)
        };

        for (index, id) in rack.iter().enumerate() {
            let card = match self.cards.borrow().get(id) {
                Some(card) => std::rc::Rc::clone(card),
                None => continue,
            };
            let saved = &saved_cards[index];
            let plugin = saved
                .get("plugin")
                .and_then(|value| value.as_str())
                .ok_or_else(|| "card entry needs a plugin".to_string())?;
            if plugin != card.type_ {
                return Err("Execution state card type mismatch".to_string());
            }
            let parked = saved
                .get("parked")
                .and_then(|value| value.as_bool())
                .unwrap_or(false);
            card.active.set(!parked);
            card.parked.set(parked);
            let state_text = saved
                .get("state")
                .and_then(|value| value.as_str())
                .ok_or_else(|| "card entry needs a state".to_string())?;
            let bytes =
                hex_decode(state_text).map_err(|_| "Malformed card state hex".to_string())?;
            // Safety: `api` is live for as long as the card exists.
            let callback = unsafe { (*card.api).load_state };
            if !bytes.is_empty() && callback.is_none() {
                return Err(format!("Plugin has no state loader: {}", card.type_));
            }
            if let Some(callback) = callback {
                // Safety: the instance is live and the buffer outlives the call.
                let status = unsafe {
                    callback(
                        card.instance(),
                        if bytes.is_empty() {
                            std::ptr::null()
                        } else {
                            bytes.as_ptr()
                        },
                        bytes.len() as u64,
                    )
                };
                if status != SRH_OK {
                    return Err(format!("Card state load failed: {}", card.type_));
                }
            }
        }
        // Keep the active-card set consistent with the restored park flags.
        {
            let mut active = self.active_cards.borrow_mut();
            active.clear();
            for id in &rack {
                let parked = self
                    .cards
                    .borrow()
                    .get(id)
                    .map(|card| card.is_parked())
                    .unwrap_or(false);
                if !parked {
                    active.insert(*id);
                }
            }
        }

        self.rng.set(field("seed")?.as_u64().unwrap_or(0));
        self.mode
            .set(match field("time_mode")?.as_str().unwrap_or("project") {
                "project" => TimeMode::Project,
                "fixed" => TimeMode::Fixed,
                "system" => TimeMode::System,
                other => return Err(format!("Unknown time mode in execution state: {other}")),
            });
        self.epoch.set(field("epoch_ns")?.as_u64().unwrap_or(0));
        self.now.set(field("now")?.as_u64().unwrap_or(0));
        *self.stop_reason.borrow_mut() = field("stop_reason")?
            .as_str()
            .ok_or_else(|| "field 'stop_reason' must be a string".to_string())?
            .to_string();
        if let Some(state) = parsed.get("run_state") {
            self.run_state.set(match state.as_str().unwrap_or("") {
                "stopped" => RunState::Stopped,
                "paused" => RunState::Paused,
                "running" => RunState::Running,
                other => return Err(format!("Unknown run state in execution state: {other}")),
            });
        } else if !field("paused")?.as_bool().unwrap_or(false) {
            self.run_state.set(RunState::Running);
        } else {
            // Version-1 snapshots encoded stopped state only in the display
            // reason.
            self.run_state
                .set(if *self.stop_reason.borrow() == "Stopped" {
                    RunState::Stopped
                } else {
                    RunState::Paused
                });
        }
        // A restore never reuses a handle allocated since capture, including
        // the input wakeups that were just discarded.
        self.next
            .set(self.next.get().max(field("next")?.as_u64().unwrap_or(0)));
        self.order.set(field("order")?.as_u64().unwrap_or(0));
        self.sequence.set(field("sequence")?.as_u64().unwrap_or(0));

        let clocks = field("clocks")?
            .as_array()
            .ok_or_else(|| "field 'clocks' must be an array".to_string())?;
        if clocks.len() != 3 {
            return Err("Execution state requires three clocks".to_string());
        }
        for (index, clock) in clocks.iter().enumerate() {
            self.clocks[index].set(crate::core::Clock {
                hz: clock
                    .get("hz")
                    .and_then(|value| value.as_u64())
                    .unwrap_or(0) as u32,
                ticks: clock
                    .get("ticks")
                    .and_then(|value| value.as_u64())
                    .unwrap_or(0),
                phase: clock
                    .get("phase")
                    .and_then(|value| value.as_u64())
                    .unwrap_or(0),
                order: clock
                    .get("order")
                    .and_then(|value| value.as_u64())
                    .unwrap_or(0),
            });
        }

        {
            let mut breakpoints = self.breakpoints.borrow_mut();
            breakpoints.clear();
            for entry in field("breakpoints")?
                .as_array()
                .ok_or_else(|| "field 'breakpoints' must be an array".to_string())?
            {
                breakpoints.push(Breakpoint {
                    id: entry
                        .get("id")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    card: remap_card(
                        entry
                            .get("card")
                            .and_then(|value| value.as_u64())
                            .unwrap_or(0),
                    ),
                    space: entry
                        .get("space")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    first: entry
                        .get("first")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    last: entry
                        .get("last")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    operations: entry
                        .get("operations")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0) as u32,
                    enabled: entry
                        .get("enabled")
                        .and_then(|value| value.as_bool())
                        .unwrap_or(false),
                });
            }
        }
        let read_boundary = |value: &Value| -> Option<[u64; 3]> {
            let entries = value.as_array()?;
            if entries.len() != 3 {
                return None;
            }
            Some([
                remap_card(entries[0].as_u64().unwrap_or(0)),
                entries[1].as_u64().unwrap_or(0),
                entries[2].as_u64().unwrap_or(0),
            ])
        };
        self.stopped_boundary
            .set(read_boundary(field("stopped_boundary")?));
        self.skip_boundary
            .set(read_boundary(field("skip_boundary")?));

        {
            let mut sequences = self.instruction_seq.borrow_mut();
            sequences.clear();
            for entry in field("instruction_seq")?
                .as_array()
                .ok_or_else(|| "field 'instruction_seq' must be an array".to_string())?
            {
                let card = remap_card(
                    entry
                        .get("card")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                );
                if card != 0 {
                    sequences.insert(
                        card,
                        entry
                            .get("sequence")
                            .and_then(|value| value.as_u64())
                            .unwrap_or(0),
                    );
                }
            }
        }

        for input in self.inputs.borrow().iter() {
            let wake = input.wake.get();
            if wake != 0 {
                self.cancel(wake);
            }
            input.wake.set(0);
            input.notified.set(false);
            input.records.borrow_mut().clear();
        }
        let mut records = Vec::new();
        for input in field("inputs")?
            .as_array()
            .ok_or_else(|| "field 'inputs' must be an array".to_string())?
        {
            let value = input
                .get("value")
                .and_then(|value| value.as_u64())
                .unwrap_or(0) as u8;
            records.push((
                input
                    .get("time_ns")
                    .and_then(|value| value.as_u64())
                    .unwrap_or(0),
                input
                    .get("endpoint")
                    .and_then(|value| value.as_str())
                    .unwrap_or("")
                    .to_string(),
                vec![value],
            ));
        }
        self.input_order.set(1);
        self.set_input_records(records)?;
        self.input_order
            .set(field("input_order")?.as_u64().unwrap_or(1));

        if parsed.get("trace").is_some() {
            let mut trace = self.trace.borrow_mut();
            trace.clear();
            for entry in parsed
                .get("trace")
                .and_then(|value| value.as_array())
                .cloned()
                .unwrap_or_default()
            {
                let mut record = Trace {
                    sequence: entry
                        .get("sequence")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    parent: entry
                        .get("parent")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    time: entry
                        .get("time")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    master: entry
                        .get("master")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    space: entry
                        .get("space")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    address: entry
                        .get("address")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    operation: entry
                        .get("operation")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0) as u32,
                    depth: entry
                        .get("depth")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0) as u32,
                    kind: entry
                        .get("kind")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0) as u32,
                    instruction: entry
                        .get("instruction")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0),
                    value: entry
                        .get("value")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0) as u8,
                    result: entry
                        .get("result")
                        .and_then(|value| value.as_i64())
                        .unwrap_or(0) as SrhStatus,
                    ..Default::default()
                };
                if let Some(ticks) = entry.get("ticks").and_then(|value| value.as_array()) {
                    for (index, tick) in ticks.iter().take(3).enumerate() {
                        record.ticks[index] = tick.as_u64().unwrap_or(0);
                    }
                }
                if let Some(responders) = entry.get("responders").and_then(|value| value.as_array())
                {
                    record.responders = responders
                        .iter()
                        .map(|value| value.as_u64().unwrap_or(0))
                        .collect();
                }
                trace.push_back(record);
            }
            self.dropped.set(
                parsed
                    .get("dropped")
                    .and_then(|value| value.as_u64())
                    .unwrap_or(0),
            );
        }
        // PCM is transient output, not execution state: discard it and restart
        // source frame accounting from the restored time.
        self.audio_reset();
        Ok(())
    }

    /// Sorted trace snapshot; the C++ engine returns records in sequence order.
    pub fn trace_records(&self) -> Vec<Trace> {
        let mut result: Vec<Trace> = self.trace.borrow().iter().cloned().collect();
        result.sort_by_key(|trace| trace.sequence);
        result
    }
}
