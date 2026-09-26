//! Plugin-owned project data.
//!
//! A card may store an opaque chunk in the project file.  The engine hex-encodes
//! the bytes for the JSON container and never interprets them. The owning card
//! validates and versions its own payload.

use crate::core::{Core, Handle};
use crate::ffi::*;

const MAX_CHUNK_BYTES: u64 = 16 * 1024 * 1024;

pub fn hex_encode(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789ABCDEF";
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(DIGITS[(byte >> 4) as usize] as char);
        out.push(DIGITS[(byte & 0x0F) as usize] as char);
    }
    out
}

pub fn hex_decode(text: &str) -> Result<Vec<u8>, String> {
    if text.len() % 2 != 0 {
        return Err("Hex string must have even length".to_string());
    }
    let nibble = |byte: u8| -> Result<u8, String> {
        match byte {
            b'0'..=b'9' => Ok(byte - b'0'),
            b'a'..=b'f' => Ok(byte - b'a' + 10),
            b'A'..=b'F' => Ok(byte - b'A' + 10),
            _ => Err("Malformed hex byte".to_string()),
        }
    };
    let bytes = text.as_bytes();
    let mut out = Vec::with_capacity(text.len() / 2);
    let mut index = 0;
    while index < bytes.len() {
        out.push((nibble(bytes[index])? << 4) | nibble(bytes[index + 1])?);
        index += 2;
    }
    Ok(out)
}

impl Core {
    /// Collects every card's opaque project chunk, keyed by card handle.
    pub fn save_plugin_data(&self) -> Result<Vec<(Handle, String)>, String> {
        self.enter_frame();
        let owners: Vec<(Handle, std::rc::Rc<crate::core::Card>)> = self
            .cards
            .borrow()
            .iter()
            .map(|(id, card)| (*id, std::rc::Rc::clone(card)))
            .collect();
        let mut result = Vec::new();
        for (owner, card) in owners {
            if card.instance().is_null() || (!card.is_active() && !card.is_parked()) {
                continue;
            }
            // Safety: `api` is a live table for as long as the card exists.
            let callback = unsafe {
                if !has_field(
                    card.api,
                    core::mem::offset_of!(SrhPlugin, save_project_data),
                    core::mem::size_of::<Option<SrhSaveState>>(),
                ) {
                    None
                } else {
                    (*card.api).save_project_data
                }
            };
            let Some(callback) = callback else {
                continue;
            };
            let mut size = 0u64;
            // Safety: the card instance is live and its plugin supplied this
            // callback.
            if unsafe { callback(card.instance(), std::ptr::null_mut(), &mut size) } != SRH_OK
                || size > MAX_CHUNK_BYTES
            {
                self.leave_frame();
                return Err(format!(
                    "Plugin project data size query failed: {}",
                    card.type_
                ));
            }
            let mut buffer = vec![0u8; if size == 0 { 1 } else { size as usize }];
            let capacity = size;
            // Safety: `buffer` holds `capacity` bytes.
            let status = unsafe { callback(card.instance(), buffer.as_mut_ptr(), &mut size) };
            if status != SRH_OK || size > capacity {
                self.leave_frame();
                return Err(format!("Plugin project data copy failed: {}", card.type_));
            }
            result.push((owner, hex_encode(&buffer[..size as usize])));
        }
        self.leave_frame();
        Ok(result)
    }

    pub fn load_plugin_data(&self, owner: Handle, hex: &str) -> Result<(), String> {
        if hex.len() as u64 > MAX_CHUNK_BYTES * 2 || hex.len() % 2 != 0 {
            return Err("Invalid plugin data size".to_string());
        }
        let card = match self.cards.borrow().get(&owner) {
            Some(card) if !card.instance().is_null() => std::rc::Rc::clone(card),
            _ => return Err("Plugin data owner missing".to_string()),
        };
        // Safety: `api` is live for the card lifetime.
        let callback = unsafe {
            if !has_field(
                card.api,
                core::mem::offset_of!(SrhPlugin, load_project_data),
                core::mem::size_of::<Option<SrhLoadState>>(),
            ) {
                None
            } else {
                (*card.api).load_project_data
            }
        };
        let Some(callback) = callback else {
            return Err(format!(
                "Plugin does not support project data: {}",
                card.type_
            ));
        };
        let bytes = hex_decode(hex).map_err(|_| "Malformed plugin data hex".to_string())?;
        self.enter_frame();
        // Safety: the card instance is live and the buffer outlives the call.
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
        self.leave_frame();
        if status != SRH_OK {
            return Err(format!("Plugin rejected project data: {}", card.type_));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_round_trip() {
        let bytes = [0u8, 1, 0xAB, 0xFF, 0x10];
        let text = hex_encode(&bytes);
        assert_eq!(text, "0001ABFF10");
        assert_eq!(hex_decode(&text).unwrap(), bytes);
        assert!(hex_decode("0").is_err());
        assert!(hex_decode("0G").is_err());
        assert!(hex_decode("").unwrap().is_empty());
    }
}
