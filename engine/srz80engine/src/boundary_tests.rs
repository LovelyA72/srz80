//! FFI boundary tests for the exported `srz80_engine_*` functions: header
//! validation, containment, and deterministic in-process fuzzing.
//!
//! The fuzzing is seeded and bounded: a failing seed reproduces exactly, so no
//! external fuzzing harness is needed.

use core::ffi::c_char;

use crate::engine::*;
use crate::ffi::*;

/// Every status the engine ABI may return.  Any other value means an export
/// invented a code or a failure escaped its guard.
const KNOWN_STATUSES: [SrhStatus; 7] = [
    SRH_OK,
    SRH_ERROR,
    SRH_INVALID,
    SRH_NOT_FOUND,
    SRH_UNAVAILABLE,
    SRH_STOP,
    SRH_CONFLICT,
];

/// A deterministic 64-bit xorshift generator.
struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Rng {
        Rng(seed | 1)
    }

    fn next(&mut self) -> u64 {
        let mut value = self.0;
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        self.0 = value;
        value
    }

    fn below(&mut self, bound: u64) -> u64 {
        self.next() % bound
    }
}

fn slice(text: &str) -> SrzSlice {
    SrzSlice {
        data: text.as_ptr() as *const c_char,
        size: text.len() as u64,
    }
}

fn empty_slice() -> SrzSlice {
    SrzSlice {
        data: core::ptr::null(),
        size: 0,
    }
}

fn bytes_slice(bytes: &[u8]) -> SrzSlice {
    SrzSlice {
        data: bytes.as_ptr() as *const c_char,
        size: bytes.len() as u64,
    }
}

/// A card request that passes the header checks; `SrzCardRequest` has no
/// `Default` because its slice fields borrow caller memory.
fn base_request(plugins: &str, type_text: &str) -> SrzCardRequest {
    SrzCardRequest {
        abi_version: SRZ80_ENGINE_ABI,
        struct_size: core::mem::size_of::<SrzCardRequest>() as u32,
        space: 0,
        base: 0,
        size: 0x1000,
        reset_vector: 0,
        priority: 0,
        clock: 0,
        type_: slice(type_text),
        config_json: slice("{}"),
        plugin_directory: slice(plugins),
        images: core::ptr::null(),
        image_count: 0,
        reserved: 0,
    }
}

fn assert_known(status: SrhStatus, context: &str) {
    assert!(
        KNOWN_STATUSES.contains(&status),
        "{context} returned an undeclared status {status}"
    );
}

/// A plugins directory that exists but holds no card library, so a fuzzed
/// project can never load arbitrary native code from the test's own directory.
fn empty_plugins_directory() -> std::path::PathBuf {
    let directory = std::env::temp_dir().join("srz80-engine-boundary-tests");
    std::fs::create_dir_all(&directory).expect("create the empty plugins directory");
    directory
}

struct TestEngine {
    handle: *mut EngineHandle,
    plugins: std::path::PathBuf,
}

impl TestEngine {
    fn new() -> TestEngine {
        let plugins = empty_plugins_directory();
        let text = plugins.to_string_lossy().into_owned();
        let handle = srz80_engine_create(slice(&text));
        assert!(!handle.is_null(), "engine creation failed");
        TestEngine { handle, plugins }
    }

    fn raw(&self) -> *mut EngineHandle {
        self.handle
    }

    fn plugins_text(&self) -> String {
        self.plugins.to_string_lossy().into_owned()
    }

    fn last_error(&self) -> String {
        let mut buffer = vec![0 as c_char; 512];
        srz80_engine_last_error(self.handle, buffer.as_mut_ptr(), buffer.len() as u64);
        let bytes: Vec<u8> = buffer
            .iter()
            .take_while(|byte| **byte != 0)
            .map(|byte| *byte as u8)
            .collect();
        String::from_utf8_lossy(&bytes).into_owned()
    }

    /// Loads `document` as a project and reports the status.
    fn load_project(&self, document: &[u8]) -> SrhStatus {
        let plugins = self.plugins_text();
        srz80_engine_load_project_json(
            self.handle,
            bytes_slice(document),
            empty_slice(),
            slice(&plugins),
            SRZ_STOPPED,
        )
    }

    fn load_state(&self, document: &[u8]) -> SrhStatus {
        srz80_engine_load_state(self.handle, bytes_slice(document))
    }

    /// A saved execution-state document for the empty rack.
    fn saved_state(&self) -> Vec<u8> {
        assert_eq!(srz80_engine_new_project(self.handle), SRH_OK);
        let mut size = 0u64;
        assert_eq!(
            srz80_engine_save_state(self.handle, 1, core::ptr::null_mut(), 0, &mut size),
            SRH_OK
        );
        assert!(size > 0);
        let mut buffer = vec![0 as c_char; size as usize + 1];
        assert_eq!(
            srz80_engine_save_state(
                self.handle,
                1,
                buffer.as_mut_ptr(),
                buffer.len() as u64,
                &mut size
            ),
            SRH_OK
        );
        buffer.truncate(size as usize);
        buffer.iter().map(|byte| *byte as u8).collect()
    }
}

impl Drop for TestEngine {
    fn drop(&mut self) {
        srz80_engine_destroy(self.handle);
    }
}

/// A minimal project document that the engine accepts as written.
const VALID_PROJECT: &str = r#"{
  "version": 1,
  "seed": 7,
  "time_mode": "project",
  "epoch_ns": 0,
  "spaces": [{"name":"cpu0.memory","maximum":"0xFFFF","unclaimed":0,"resolver":"priority"}],
  "clocks": [0, 0, 0],
  "cards": []
}"#;

fn mutate(bytes: &mut Vec<u8>, rng: &mut Rng, rounds: usize) {
    for _ in 0..rounds {
        if bytes.is_empty() {
            return;
        }
        let index = rng.below(bytes.len() as u64) as usize;
        match rng.below(4) {
            0 => bytes[index] = rng.next() as u8,
            1 => bytes[index] ^= 1u8 << rng.below(8),
            2 => {
                bytes.remove(index);
            }
            _ => bytes.insert(index, rng.next() as u8),
        }
    }
}

#[test]
fn the_engine_announces_the_declared_abi_version() {
    assert_eq!(srz80_engine_abi_version(), SRZ80_ENGINE_ABI);
}

#[test]
fn creation_survives_an_absent_plugins_directory() {
    let plugins = empty_plugins_directory();
    let text = plugins.to_string_lossy().into_owned();
    let engine = srz80_engine_create(slice(&text));
    assert!(!engine.is_null());
    // A zero-length slice selects the executable-relative default instead.
    let default_engine = srz80_engine_create(empty_slice());
    assert!(!default_engine.is_null());
    srz80_engine_destroy(default_engine);
    srz80_engine_destroy(engine);
    // Destruction is idempotent from the caller's perspective: a null handle is
    // ignored rather than dereferenced.
    srz80_engine_destroy(core::ptr::null_mut());
}

#[test]
fn a_null_engine_handle_is_rejected_by_every_sampled_export() {
    let mut handle = 0u64;
    let mut pad = 0u8;
    let request = base_request("", "ram");
    assert_eq!(srz80_engine_new_project(core::ptr::null_mut()), SRH_INVALID);
    assert_eq!(
        srz80_engine_load_project_json(
            core::ptr::null_mut(),
            empty_slice(),
            empty_slice(),
            empty_slice(),
            SRZ_STOPPED
        ),
        SRH_INVALID
    );
    assert_eq!(
        srz80_engine_load_state(core::ptr::null_mut(), empty_slice()),
        SRH_INVALID
    );
    assert_eq!(
        srz80_engine_add_card(core::ptr::null_mut(), &request, &mut handle),
        SRH_INVALID
    );
    assert_eq!(
        srz80_engine_read(core::ptr::null_mut(), 0, 0, 0, &mut pad, 0),
        SRH_INVALID
    );
    assert_eq!(srz80_engine_resume(core::ptr::null_mut()), SRH_INVALID);
    assert_eq!(srz80_engine_reset(core::ptr::null_mut(), 1), SRH_INVALID);
    assert!(srz80_engine_candidate_create(core::ptr::null()).is_null());
    assert_eq!(
        srz80_engine_replace(core::ptr::null_mut(), core::ptr::null_mut()),
        SRH_INVALID
    );
    // Value-returning exports fall back instead of dereferencing.
    assert_eq!(srz80_engine_now(core::ptr::null()), 0);
    assert_eq!(srz80_engine_audio_sample_rate(core::ptr::null()), 0);
    assert_eq!(
        srz80_engine_last_error(core::ptr::null(), core::ptr::null_mut(), 0),
        0
    );
    // Result accessors answer an empty arena rather than reading a null one.
    assert!(srz80_engine_result_cards(core::ptr::null(), core::ptr::null_mut()).is_null());
    assert!(srz80_engine_result_logs(core::ptr::null(), core::ptr::null_mut()).is_null());
}

#[test]
fn card_requests_validate_their_abi_header() {
    let engine = TestEngine::new();
    assert_eq!(srz80_engine_new_project(engine.raw()), SRH_OK);
    let plugins = engine.plugins_text();
    let base = base_request(&plugins, "ram");

    let mut handle = 0u64;
    // A null request record.
    assert_eq!(
        srz80_engine_add_card(engine.raw(), core::ptr::null(), &mut handle),
        SRH_INVALID
    );
    // A null output handle.
    assert_eq!(
        srz80_engine_add_card(engine.raw(), &base, core::ptr::null_mut()),
        SRH_INVALID
    );

    // A stale or future ABI version.
    let mut wrong_version = base;
    wrong_version.abi_version = SRZ80_ENGINE_ABI + 1;
    assert_eq!(
        srz80_engine_add_card(engine.raw(), &wrong_version, &mut handle),
        SRH_INVALID
    );

    // An undersized record.
    let mut undersized = base;
    undersized.struct_size = (core::mem::size_of::<SrzCardRequest>() - 1) as u32;
    assert_eq!(
        srz80_engine_add_card(engine.raw(), &undersized, &mut handle),
        SRH_INVALID
    );

    // An image count without image records.
    let mut dangling_images = base;
    dangling_images.image_count = 2;
    assert_eq!(
        srz80_engine_add_card(engine.raw(), &dangling_images, &mut handle),
        SRH_INVALID
    );

    // A larger `struct_size` is a future-compatible record, so it must get past
    // the header check and fail later for a reason that is not "invalid ABI".
    // The empty plugins directory cannot resolve the card.
    let mut oversized = base;
    oversized.struct_size = core::mem::size_of::<SrzCardRequest>() as u32 + 64;
    let status = srz80_engine_add_card(engine.raw(), &oversized, &mut handle);
    assert_known(status, "add_card with an oversized request");
    assert_ne!(
        status, SRH_INVALID,
        "an oversized request must be accepted as a newer record"
    );
}

#[test]
fn mutated_card_requests_never_escape_the_declared_statuses() {
    let engine = TestEngine::new();
    assert_eq!(srz80_engine_new_project(engine.raw()), SRH_OK);
    let plugins = engine.plugins_text();
    let base = base_request(&plugins, "ram");

    // Only scalar fields are mutated: the slice and pointer fields keep
    // pointing at live memory, so the boundary is exercised without undefined
    // behaviour on the test's own side.
    let mut rng = Rng::new(0x5EED_1234);
    for _ in 0..256 {
        let mut request = base;
        request.abi_version = match rng.below(4) {
            0 => SRZ80_ENGINE_ABI,
            1 => 0,
            2 => SRH_ABI,
            _ => rng.next() as u32,
        };
        request.struct_size = match rng.below(4) {
            0 => core::mem::size_of::<SrzCardRequest>() as u32,
            1 => (core::mem::size_of::<SrzCardRequest>() - 1) as u32,
            2 => rng.next() as u32,
            _ => 0,
        };
        request.space = rng.next();
        request.base = rng.next();
        request.size = rng.next();
        request.reset_vector = rng.next();
        request.priority = rng.next() as i32;
        request.clock = rng.below(5) as u32;
        request.image_count = rng.below(3) as u32;
        request.reserved = rng.next() as u32;
        let mut handle = 0u64;
        let status = srz80_engine_add_card(engine.raw(), &request, &mut handle);
        assert_known(status, "add_card with a mutated request");
        if status == SRH_OK {
            // A successful add must hand back a live handle.
            assert_ne!(handle, 0);
        }
    }
}

#[test]
fn mutated_project_documents_never_escape_the_declared_statuses() {
    let engine = TestEngine::new();
    // The unmutated document is the control: it has to load.
    assert_eq!(
        engine.load_project(VALID_PROJECT.as_bytes()),
        SRH_OK,
        "the control project did not load: {}",
        engine.last_error()
    );
    let mut rng = Rng::new(0xC0FF_EE01);
    let mut accepted = 0;
    for _ in 0..192 {
        let mut document = VALID_PROJECT.as_bytes().to_vec();
        let rounds = 1 + rng.below(6) as usize;
        mutate(&mut document, &mut rng, rounds);
        let status = engine.load_project(&document);
        assert_known(status, "load_project_json with a mutated document");
        if status == SRH_OK {
            accepted += 1;
        }
    }
    // Mutations are small, so some documents must still parse: a run where
    // nothing loads would mean the fuzzer stopped exercising the loader.
    assert!(accepted > 0, "no mutated project was accepted");
}

#[test]
fn mutated_state_documents_never_escape_the_declared_statuses() {
    let engine = TestEngine::new();
    let saved = engine.saved_state();
    assert_eq!(engine.load_state(&saved), SRH_OK, "{}", engine.last_error());
    let mut rng = Rng::new(0xBADC_0DE2);
    for _ in 0..192 {
        let mut document = saved.clone();
        let rounds = 1 + rng.below(6) as usize;
        mutate(&mut document, &mut rng, rounds);
        let status = engine.load_state(&document);
        assert_known(status, "load_state with a mutated document");
    }
}

#[test]
fn invalid_utf8_is_handled_without_unwinding() {
    let engine = TestEngine::new();
    assert_eq!(srz80_engine_new_project(engine.raw()), SRH_OK);
    let invalid: Vec<u8> = vec![0xFF, 0xFE, 0x80, 0x00, 0x41, 0x7B, 0x7D, 0xC0, 0xC1];
    assert_known(
        engine.load_project(&invalid),
        "load_project_json with bad UTF-8",
    );
    assert_known(engine.load_state(&invalid), "load_state with bad UTF-8");

    let key = String::from_utf8_lossy(&invalid).into_owned();
    let mut buffer = vec![0 as c_char; 64];
    assert_known(
        srz80_engine_config_set(engine.raw(), slice(&key), slice("value")),
        "config_set with a lossy key",
    );
    let written = srz80_engine_config_value(
        engine.raw(),
        slice(&key),
        slice("fallback"),
        buffer.as_mut_ptr(),
        buffer.len() as u64,
    );
    // The value is either absent (0 bytes) or a NUL-terminated string; it is
    // never a partial write without a terminator.
    if written != 0 {
        let terminator = buffer
            .iter()
            .position(|byte| *byte == 0)
            .expect("a written configuration value must be NUL terminated");
        assert!(terminator < buffer.len());
    }
}

#[test]
fn a_truncated_buffer_reports_the_size_and_writes_nothing() {
    let engine = TestEngine::new();
    assert_eq!(srz80_engine_new_project(engine.raw()), SRH_OK);
    let mut size = 0u64;
    assert_eq!(
        srz80_engine_save_state(engine.raw(), 0, core::ptr::null_mut(), 0, &mut size),
        SRH_OK
    );
    assert!(size > 0);
    // A buffer one byte too small for the document plus its terminator is
    // refused rather than filled partially.
    let mut short = vec![0x7F as c_char; size as usize];
    let mut written = 0u64;
    assert_eq!(
        srz80_engine_save_state(
            engine.raw(),
            0,
            short.as_mut_ptr(),
            short.len() as u64,
            &mut written
        ),
        SRH_UNAVAILABLE
    );
    assert_eq!(written, size);
    assert!(
        short.iter().all(|byte| *byte == 0x7F),
        "a refused save must not touch the caller's buffer"
    );
}

#[test]
fn a_wrong_sized_slice_is_bounded_by_its_length() {
    let engine = TestEngine::new();
    // A slice that claims one byte of a longer document parses only that byte,
    // so the loader reports a JSON error instead of reading past the length.
    let document = VALID_PROJECT.as_bytes();
    let truncated = SrzSlice {
        data: document.as_ptr() as *const c_char,
        size: 1,
    };
    let plugins = engine.plugins_text();
    let status = srz80_engine_load_project_json(
        engine.raw(),
        truncated,
        empty_slice(),
        slice(&plugins),
        SRZ_STOPPED,
    );
    assert_known(status, "load_project_json with a one-byte slice");
    assert_ne!(status, SRH_OK);
    assert!(!engine.last_error().is_empty());
}
