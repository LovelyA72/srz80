//! Layout regression tests for the ABI declarations.
//!
//! The expected numbers were produced by a C++ probe program that prints
//! `sizeof` and `offsetof` for the same declarations from `<srz80/engine.h>`,
//! `<srz80/abi.h>`, `<srz80/providers.h>` and `<srz80/signals.h>` on both
//! 64-bit MinGW and 64-bit Linux.  A mismatch means the Rust engine would read
//! or write the wrong bytes at the boundary.

use crate::ffi;
use core::mem::{align_of, offset_of, size_of};

#[test]
fn announced_abi_version_matches_the_crate_constant() {
    // The header is the contract; the crate constant and the exported query
    // must report the same revision, or a caller cannot trust either.
    assert_eq!(crate::ABI_VERSION, ffi::SRZ80_ENGINE_ABI);
    assert_eq!(
        crate::engine::srz80_engine_abi_version(),
        crate::ABI_VERSION
    );
}

#[test]
fn structure_sizes_match_the_c_headers() {
    assert_eq!(size_of::<ffi::SrzSlice>(), 16);
    assert_eq!(size_of::<ffi::SrzCardInfo>(), 56);
    assert_eq!(size_of::<ffi::SrzSpace>(), 48);
    assert_eq!(size_of::<ffi::SrzClock>(), 40);
    assert_eq!(size_of::<ffi::SrzTrace>(), 120);
    assert_eq!(size_of::<ffi::SrzBreakpoint>(), 56);
    assert_eq!(size_of::<ffi::SrzProperty>(), 216);
    assert_eq!(size_of::<ffi::SrzConfigEntry>(), 80);
    assert_eq!(size_of::<ffi::SrzTextEndpoint>(), 24);
    assert_eq!(size_of::<ffi::SrzVideoSurface>(), 48);
    assert_eq!(size_of::<ffi::SrzAudioSource>(), 48);
    assert_eq!(size_of::<ffi::SrzAudioDiagnostics>(), 48);
    assert_eq!(size_of::<ffi::SrzProviderData>(), 56);
    assert_eq!(size_of::<ffi::SrzPluginData>(), 32);
    assert_eq!(size_of::<ffi::SrzInputRecord>(), 32);
    assert_eq!(size_of::<ffi::SrzPluginDescriptor>(), 120);
    assert_eq!(size_of::<ffi::SrzDisassembly>(), 312);
    assert_eq!(size_of::<ffi::SrzDisassemblyAvailability>(), 280);
    assert_eq!(size_of::<ffi::SrzError>(), 536);
    assert_eq!(size_of::<ffi::SrzImagePart>(), 24);
    assert_eq!(size_of::<ffi::SrzCardRequest>(), 112);
    assert_eq!(size_of::<ffi::SrhValue>(), 152);
    assert_eq!(size_of::<ffi::SrhProperty>(), 64);
    assert_eq!(size_of::<ffi::SrhConfig>(), 112);
    assert_eq!(size_of::<ffi::SrhMapping>(), 80);
    assert_eq!(size_of::<ffi::SrhImagePart>(), 24);
    assert_eq!(size_of::<ffi::SrhConfigEntry>(), 96);
    assert_eq!(size_of::<ffi::SrhCardDescriptor>(), 112);
    assert_eq!(size_of::<ffi::SrhImageSlotDescriptor>(), 16);
    assert_eq!(size_of::<ffi::SrhPlugin>(), 112);
    assert_eq!(size_of::<ffi::ShouryoHost>(), 160);
    assert_eq!(size_of::<ffi::SrhHostResourcesV1>(), 24);
    assert_eq!(size_of::<ffi::SrhHostProjectFilesV1>(), 40);
    assert_eq!(size_of::<ffi::SrhHostDebugV1>(), 72);
    assert_eq!(size_of::<ffi::SrhHostMemoryV1>(), 24);
    assert_eq!(size_of::<ffi::SrhHostInputV1>(), 48);
    assert_eq!(size_of::<ffi::SrhHostTextV1>(), 24);
    assert_eq!(size_of::<ffi::SrhHostVideoV1>(), 40);
    assert_eq!(size_of::<ffi::SrhVideoTiming>(), 24);
    assert_eq!(size_of::<ffi::SrhHostAudioV1>(), 40);
    assert_eq!(size_of::<ffi::SrhHostConfigV1>(), 48);
    assert_eq!(size_of::<ffi::SrhHostProvidersV1>(), 32);
    assert_eq!(size_of::<ffi::SrhHostSignalsV1>(), 24);
    assert_eq!(size_of::<ffi::SrhDataProviderV1>(), 296);
}

#[test]
fn tail_field_offsets_match_the_c_headers() {
    assert_eq!(offset_of!(ffi::SrzCardInfo, load_error), 48);
    // The engine validates a caller-supplied request by structure size, so the
    // offsets it compares against must be the C ones.
    assert_eq!(offset_of!(ffi::SrzCardRequest, plugin_directory), 80);
    assert_eq!(offset_of!(ffi::SrzCardRequest, images), 96);
    assert_eq!(offset_of!(ffi::SrzCardRequest, image_count), 104);

    assert_eq!(offset_of!(ffi::SrhConfig, config_json), 64);
    assert_eq!(offset_of!(ffi::SrhConfig, config_json_size), 72);
    assert_eq!(offset_of!(ffi::SrhConfig, images), 80);
    assert_eq!(offset_of!(ffi::SrhConfig, image_count), 88);
    assert_eq!(offset_of!(ffi::SrhConfig, error_message), 96);
    assert_eq!(offset_of!(ffi::SrhConfig, error_message_capacity), 104);

    assert_eq!(offset_of!(ffi::SrhPlugin, save_state), 72);
    assert_eq!(offset_of!(ffi::SrhPlugin, load_state), 80);
    assert_eq!(offset_of!(ffi::SrhPlugin, card_descriptor), 88);
    assert_eq!(offset_of!(ffi::SrhPlugin, save_project_data), 96);
    assert_eq!(offset_of!(ffi::SrhPlugin, load_project_data), 104);

    assert_eq!(offset_of!(ffi::SrhCardDescriptor, base_config_key), 88);
    assert_eq!(offset_of!(ffi::SrhCardDescriptor, image_slots), 96);
    assert_eq!(offset_of!(ffi::SrhCardDescriptor, image_slot_count), 104);

    assert_eq!(offset_of!(ffi::ShouryoHost, query), 152);
    assert_eq!(offset_of!(ffi::SrhHostProjectFilesV1, project_root), 16);
    assert_eq!(offset_of!(ffi::SrhHostProjectFilesV1, read_file), 24);
    assert_eq!(offset_of!(ffi::SrhHostProjectFilesV1, write_file), 32);
    assert_eq!(offset_of!(ffi::SrhHostLifecycleV1, subscribe_resume), 16);
    assert_eq!(offset_of!(ffi::SrhHostDebugV1, trace_enabled), 48);
    assert_eq!(offset_of!(ffi::SrhHostDebugV1, boundary_required), 56);
    assert_eq!(offset_of!(ffi::SrhHostDebugV1, set_disassembly_enabled), 64);
    assert_eq!(offset_of!(ffi::SrhHostInputV1, subscribe_due), 40);
    assert_eq!(offset_of!(ffi::SrhDataProviderV1, flags), 288);

    assert_eq!(offset_of!(ffi::SrzTrace, result), 108);
    assert_eq!(offset_of!(ffi::SrzTrace, responder_offset), 112);
}

#[test]
fn headers_are_naturally_aligned() {
    // Every ABI structure starts with two u32 fields and otherwise contains
    // only 8-byte-aligned members on 64-bit hosts.
    assert_eq!(align_of::<ffi::SrzCardInfo>(), 8);
    assert_eq!(align_of::<ffi::SrhPlugin>(), 8);
    assert_eq!(align_of::<ffi::ShouryoHost>(), 8);
    assert_eq!(align_of::<ffi::SrzSlice>(), 8);
}

#[test]
fn has_field_respects_struct_size() {
    // A plugin built against the original ABI-1 layout declares exactly the
    // prefix ending after `property_set`; every later field is optional.
    let mut plugin = ffi::SrhPlugin {
        abi_version: ffi::SRH_ABI,
        struct_size: (offset_of!(ffi::SrhPlugin, property_set)
            + size_of::<Option<ffi::SrhPropertySet>>()) as u32,
        ..Default::default()
    };
    assert_eq!(plugin.struct_size, 72);
    let pointer = &plugin as *const ffi::SrhPlugin;
    // Safety: `plugin` is a live, fully initialized structure.
    unsafe {
        assert!(!ffi::has_field(
            pointer,
            offset_of!(ffi::SrhPlugin, save_state),
            8
        ));
        assert!(!ffi::has_field(
            pointer,
            offset_of!(ffi::SrhPlugin, card_descriptor),
            8
        ));
        assert!(!ffi::has_field(
            pointer,
            offset_of!(ffi::SrhPlugin, save_project_data),
            8
        ));
        assert!(!ffi::has_field(core::ptr::null::<ffi::SrhPlugin>(), 0, 8));
    }
    // A plugin that also declares the two optional state callbacks exposes
    // exactly those two tail fields.
    plugin.struct_size =
        (offset_of!(ffi::SrhPlugin, load_state) + size_of::<Option<ffi::SrhLoadState>>()) as u32;
    let pointer = &plugin as *const ffi::SrhPlugin;
    // Safety: `plugin` is a live, fully initialized structure.
    unsafe {
        assert!(ffi::has_field(
            pointer,
            offset_of!(ffi::SrhPlugin, save_state),
            8
        ));
        assert!(ffi::has_field(
            pointer,
            offset_of!(ffi::SrhPlugin, load_state),
            8
        ));
        assert!(!ffi::has_field(
            pointer,
            offset_of!(ffi::SrhPlugin, card_descriptor),
            8
        ));
    }
    plugin.struct_size = size_of::<ffi::SrhPlugin>() as u32;
    let pointer = &plugin as *const ffi::SrhPlugin;
    // Safety: `plugin` is a live, fully initialized structure.
    unsafe {
        assert!(ffi::has_field(
            pointer,
            offset_of!(ffi::SrhPlugin, save_project_data),
            8
        ));
    }
}
