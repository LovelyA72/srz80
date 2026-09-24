# Video shaders

The desktop app can apply one fragment shader to eligible video surfaces. Set
`video.shader_path` under **Settings → UI → Video**, or choose a file with
**Browse**. The shader description is JSON:

```json
{
  "format": "srz80-video-shader",
  "version": 1,
  "name": "Example",
  "output": {
    "input_chunk": 1,
    "output_chunk": 1
  },
  "textures": [
    {
      "path": "lookup.rgba8",
      "format": "rgba8_uint",
      "width": 128,
      "height": 64
    }
  ],
  "fragment": {
    "entrypoint": "main",
    "samplers": 2,
    "uniform_buffers": 1,
    "spirv": "example.frag.spv",
    "dxil": "example.frag.dxil",
    "msl": "example.frag.metal"
  }
}
```

The host selects the first binary supported by SDL's GPU renderer. Ship the JSON
description, shader binaries, and auxiliary textures together; paths are resolved
relative to the description file.

## Bindings

The source texture and sampler use `t0` and `s0` in space 2. Optional auxiliary
textures bind consecutively from `t1` and `s1`. The optional uniform buffer uses
`b0` in space 3.

Its first four floats are:

1. physical source width;
2. physical source height;
3. card frame parity, retained for compatibility; and
4. logical input width used for shader output cadence.

An appended `uint4` contains the low and high 32-bit words of the card's 64-bit
scanout frame, the next scanline, and the scanline count. A zero scanline count
means timing is unavailable. Integer frame words preserve phase during long runs.

Raw `rgba8_uint` textures contain exactly `width × height × 4` bytes in row-major
RGBA order and use nearest filtering. The sampler count includes the source and
every auxiliary texture.

## Processing size

`input_chunk` and `output_chunk` request an intermediate width of:

```text
ceil(source_width / input_chunk) * output_chunk
```

The shader processes the complete source frame at that size before scaling it for
display. Video panel size does not affect shader resolution or cadence.

## Surface eligibility

A video plugin must register its surface with `SRH_VIDEO_ALLOW_SHADER` and provide
scanout timing through the optional `host.video.v1.set_video_timing` tail. Plugins
must check `struct_size` before using that field.

The engine validates and copies timing through `srz80_engine_video_timing`. The
controller reads it immediately after the pixel buffer and publishes both values
together without advancing simulation. If timing is missing or cannot be read,
the app shows the unfiltered frame without clearing the saved preference.

Shader enablement is stored under `video.shaders`, keyed by persisted card order
and surface ordinal. Reordering cards updates those keys.

## Renderer requirements

Custom render states require SDL 3.4 or newer and its `gpu` renderer. When a
shader is configured, SRZ80 requests that renderer and enables its supported
shader formats during startup. If the selected renderer cannot run the shader,
the Video panel reports the reason and continues to show the unfiltered surface.

Shader code controls its own signal model and phase. The host supplies source
pixels and timing; it does not assume NTSC, PAL, CRT behavior, or a particular
card palette.
