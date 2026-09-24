//! Small key=value INI store.
//!
//! Values are UTF-8 strings; typed interpretation belongs to the UI.  The
//! parser accepts ordinary `key=value` lines, ignores blank lines and lines
//! without a separator, and trims ASCII whitespace -- it deliberately does not
//! interpret section headers, so the GUI keeps its serialized ImGui settings
//! as one base64 value.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

#[derive(Default, Clone)]
pub struct Config {
    path: PathBuf,
    values: BTreeMap<String, String>,
}

fn trim(text: &str) -> &str {
    text.trim_matches(|c: char| c.is_ascii_whitespace())
}

impl Config {
    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn load(&mut self, path: &Path) {
        self.path = path.to_path_buf();
        self.values.clear();
        let text = match std::fs::read_to_string(path) {
            Ok(text) => text,
            Err(_) => return,
        };
        self.load_string(&text);
    }

    pub fn load_string(&mut self, text: &str) {
        self.values.clear();
        for line in text.split('\n') {
            let line = line.strip_suffix('\r').unwrap_or(line);
            let Some(equals) = line.find('=') else {
                continue;
            };
            let key = trim(&line[..equals]);
            let value = trim(&line[equals + 1..]);
            if !key.is_empty() {
                self.values.insert(key.to_string(), value.to_string());
            }
        }
    }

    /// Persistence is best effort: a write failure is never reported.
    pub fn save(&self, path: &Path) {
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                let _ = std::fs::create_dir_all(parent);
            }
        }
        let _ = std::fs::write(path, self.to_string());
    }

    pub fn has(&self, key: &str) -> bool {
        self.values.contains_key(key)
    }

    pub fn value(&self, key: &str, fallback: &str) -> String {
        self.values
            .get(key)
            .cloned()
            .unwrap_or_else(|| fallback.to_string())
    }

    pub fn set(&mut self, key: &str, value: &str) {
        self.values.insert(key.to_string(), value.to_string());
    }

    pub fn to_string(&self) -> String {
        let mut result = String::new();
        for (key, value) in &self.values {
            result.push_str(key);
            result.push('=');
            result.push_str(value);
            result.push('\n');
        }
        result
    }
}

const BASE64_CHARS: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn base64_encode(input: &[u8]) -> String {
    let mut output = String::with_capacity((input.len() + 2) / 3 * 4);
    let mut index = 0;
    while index + 2 < input.len() {
        let n =
            (input[index] as u32) << 16 | (input[index + 1] as u32) << 8 | input[index + 2] as u32;
        output.push(BASE64_CHARS[((n >> 18) & 63) as usize] as char);
        output.push(BASE64_CHARS[((n >> 12) & 63) as usize] as char);
        output.push(BASE64_CHARS[((n >> 6) & 63) as usize] as char);
        output.push(BASE64_CHARS[(n & 63) as usize] as char);
        index += 3;
    }
    match input.len() - index {
        1 => {
            let n = (input[index] as u32) << 16;
            output.push(BASE64_CHARS[((n >> 18) & 63) as usize] as char);
            output.push(BASE64_CHARS[((n >> 12) & 63) as usize] as char);
            output.push('=');
            output.push('=');
        }
        2 => {
            let n = (input[index] as u32) << 16 | (input[index + 1] as u32) << 8;
            output.push(BASE64_CHARS[((n >> 18) & 63) as usize] as char);
            output.push(BASE64_CHARS[((n >> 12) & 63) as usize] as char);
            output.push(BASE64_CHARS[((n >> 6) & 63) as usize] as char);
            output.push('=');
        }
        _ => {}
    }
    output
}

pub fn base64_decode(input: &str) -> Vec<u8> {
    let mut table = [255u8; 256];
    for (index, byte) in BASE64_CHARS.iter().enumerate() {
        table[*byte as usize] = index as u8;
    }
    let mut output = Vec::new();
    let mut accumulator: u32 = 0;
    let mut bits = 0;
    for byte in input.bytes() {
        if byte == b'=' {
            break;
        }
        if byte.is_ascii_whitespace() {
            continue;
        }
        let value = table[byte as usize];
        if value == 255 {
            continue;
        }
        accumulator = (accumulator << 6) | value as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            output.push(((accumulator >> bits) & 0xFF) as u8);
        }
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ini_round_trip() {
        let mut config = Config::default();
        config.load_string("alpha = 1\n\n# comment line\nbeta=hello world\r\n");
        assert_eq!(config.value("alpha", ""), "1");
        assert_eq!(config.value("beta", ""), "hello world");
        assert_eq!(config.value("missing", "fallback"), "fallback");
        assert_eq!(config.to_string(), "alpha=1\nbeta=hello world\n");
        config.set("beta", "changed");
        assert_eq!(config.value("beta", ""), "changed");
    }

    #[test]
    fn base64_round_trip() {
        for sample in [&b""[..], b"a", b"ab", b"abc", b"\x00\xff\x10hello world"] {
            let encoded = base64_encode(sample);
            assert_eq!(base64_decode(&encoded), sample);
        }
        assert_eq!(base64_decode("aGVsbG8="), b"hello");
    }
}
