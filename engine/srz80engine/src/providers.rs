//! Card data providers.
//!
//! A provider is an opaque, plugin-owned transport: the engine copies the
//! metadata at registration and never interprets the payload schema or the
//! revision numbers.  Every callback runs on the engine thread.

use crate::core::{Core, Handle, ProviderData, RunState};
use crate::ffi::*;

impl Core {
    pub fn register_provider(
        &self,
        owner: Handle,
        provider: *const SrhDataProviderV1,
    ) -> SrhStatus {
        if !self.alive(owner) || provider.is_null() {
            return SRH_INVALID;
        }
        // Safety: the caller supplied a provider record. The header and the
        // required tail fields are checked before any field is read.
        let source = unsafe { &*provider };
        if source.abi_version != SRH_ABI
            || !unsafe {
                has_field(
                    provider,
                    core::mem::offset_of!(SrhDataProviderV1, protocol),
                    core::mem::size_of::<*const core::ffi::c_char>(),
                )
            }
            || source.snapshot.is_none()
            || source.command.is_none()
        {
            return SRH_INVALID;
        }
        if !cstring_complete(&source.name) || !cstring_complete(&source.protocol) {
            return SRH_INVALID;
        }
        if self.providers.borrow().contains_key(&owner) {
            return SRH_CONFLICT;
        }
        let flags = unsafe {
            if has_field(
                provider,
                core::mem::offset_of!(SrhDataProviderV1, flags),
                core::mem::size_of::<u32>(),
            ) {
                source.flags
            } else {
                0
            }
        };
        if flags & !SRH_PROVIDER_LIVE_CONFIG != 0 {
            return SRH_INVALID;
        }
        let mut copy = *source;
        copy.flags = flags;
        self.providers.borrow_mut().insert(owner, copy);
        SRH_OK
    }

    pub fn provider_data(&self) -> Vec<(Handle, ProviderData)> {
        self.enter_frame();
        let owners: Vec<Handle> = self.providers.borrow().keys().copied().collect();
        let mut result = Vec::new();
        for owner in owners {
            if !self.alive(owner) {
                continue;
            }
            let provider = match self.providers.borrow().get(&owner) {
                Some(provider) => *provider,
                None => continue,
            };
            let mut data = ProviderData {
                name: fixed_text(&provider.name),
                protocol: fixed_text(&provider.protocol),
                data: String::new(),
                flags: provider.flags,
            };
            let Some(snapshot) = provider.snapshot else {
                continue;
            };
            let mut size = 0u64;
            // Safety: the provider owner is alive and registered this callback.
            if unsafe { snapshot(provider.context, core::ptr::null_mut(), &mut size) } != SRH_OK
                || size == 0
                || size > SRH_PROVIDER_MAX_BYTES
            {
                continue;
            }
            let mut buffer = vec![0i8; size as usize];
            let capacity = size;
            // Safety: `buffer` holds `size` bytes.
            let status = unsafe { snapshot(provider.context, buffer.as_mut_ptr(), &mut size) };
            if status != SRH_OK || size == 0 || size > capacity || buffer[size as usize - 1] != 0 {
                continue;
            }
            let bytes: Vec<u8> = buffer[..size as usize - 1]
                .iter()
                .map(|byte| *byte as u8)
                .collect();
            data.data = String::from_utf8_lossy(&bytes).into_owned();
            result.push((owner, data));
        }
        self.leave_frame();
        result
    }

    pub fn provider_command(
        &self,
        owner: Handle,
        kind: u32,
        revision: u64,
        payload: &str,
    ) -> SrhStatus {
        if kind > SRH_PROVIDER_CONFIGURE
            || payload.is_empty()
            || payload.len() as u64 >= SRH_PROVIDER_MAX_BYTES
            || payload.as_bytes().contains(&0)
        {
            return SRH_INVALID;
        }
        let provider = match self.providers.borrow().get(&owner) {
            Some(provider) => *provider,
            None => return SRH_NOT_FOUND,
        };
        if !self.alive(owner) {
            return SRH_NOT_FOUND;
        }
        if kind == SRH_PROVIDER_CONFIGURE
            && self.run_state.get() != RunState::Stopped
            && (provider.flags & SRH_PROVIDER_LIVE_CONFIG) == 0
        {
            return SRH_CONFLICT;
        }
        self.enter_frame();
        let mut text: Vec<u8> = payload.as_bytes().to_vec();
        text.push(0);
        // Safety: the provider owner is alive and registered this callback. The
        // payload buffer outlives the call.
        let status = unsafe {
            match provider.command {
                Some(command) => command(
                    provider.context,
                    kind,
                    revision,
                    text.as_ptr() as *const core::ffi::c_char,
                    (text.len() - 1) as u64,
                ),
                None => SRH_NOT_FOUND,
            }
        };
        self.leave_frame();
        status
    }
}

/// A fixed-size `char[N]` metadata field must hold a complete NUL-terminated
/// string. A missing terminator means the supplier was not a valid record.
fn cstring_complete(field: &[core::ffi::c_char]) -> bool {
    if field.is_empty() || field[0] == 0 {
        return false;
    }
    field.iter().any(|byte| *byte == 0)
}

fn fixed_text(field: &[core::ffi::c_char]) -> String {
    let end = field
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(field.len());
    let bytes: Vec<u8> = field[..end].iter().map(|byte| *byte as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}
