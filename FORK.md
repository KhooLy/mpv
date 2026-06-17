# gpu-next render backend for libmpv

## Why

`vo=gpu-next` and libmpv's render API have always been structurally
separate. `render_backend_gpu`, the backend libmpv embedders actually get,
only wraps `gl_video`. An app embedding mpv via `mpv_render_context` --
the documented way to put mpv inside a window you don't hand mpv full
control of -- never ran `pl_renderer` at all, regardless of any
`--tone-mapping`, `--deband`, or other libplacebo-only options it set.
They just silently did nothing.

This fork adds `render_backend_gpu_next`, a second backend implementing
the same `render_backend_fns` interface, that derives a `pl_gpu` from the
same embeddable GL context the existing backend uses and renders through
`pl_queue` + `pl_render_image_mix` -- the same machinery `vo_gpu_next.c`
uses for normal windowed playback.

## What works

- Tone-mapping, debanding, dithering, scaler selection -- the same
  `gl_video_opts`-driven options `vo=gpu-next` reads
- hwdec mapping (VAAPI etc.)
- OSD/subtitle overlay rendering
- Screenshots
- `--lut` / `--image-lut` / `--target-lut`
- `--glsl-shaders` user hooks
- Motion interpolation (`--interpolation`), given the embedder drives
  `mpv_render_context_render()` off real vsync
- `vo->target_params` / peak-detect feedback for dynamic HDR metadata
  (Dolby Vision, HDR10+)

## What doesn't, structurally

- Compositor colorspace negotiation (`target-colorspace-hint`) and
  automatic ICC profile detection both assume a real swapchain/window.
  There isn't one -- the target is whatever FBO the embedder hands over
  each frame. Target colorspace is controlled via the normal
  `--target-trc`/`--target-peak`/etc. options instead.
- Vulkan/D3D11. libmpv's render API has no Vulkan API type to attach to
  (`MPV_RENDER_PARAM_API_TYPE` only defines `"opengl"` and `"sw"`), so
  there's nothing to implement against.

## Using it

Opt-in, off by default -- every other libmpv embedder is unaffected. Set
an environment variable before creating the render context:

```sh
MPV_LIBMPV_RENDER_BACKEND=gpu-next
```

Then create the render context the normal way, with
`MPV_RENDER_PARAM_API_TYPE` set to `MPV_RENDER_API_TYPE_OPENGL`.

## What changed

- New file: `video/out/gpu_next/libmpv_gpu_next.c`
- `video/out/vo_libmpv.c`: registers the new backend
- `video/out/libmpv.h`: declares it
- `video/out/gpu_next/context.h`: moved `struct gl_next_opts` and
  `struct user_lut` here (out of `vo_gpu_next.c`) so the new backend can
  share the same `--lut`/`--image-lut`/`--target-lut` options
- `meson.build`: builds the new file

Diffable against upstream: everything except those is unmodified mpv.

## Building

```sh
meson setup build -Dlibmpv=true -Dgpl=false
ninja -C build
```

Produces `build/libmpv.so.2.5.0` (Linux), `build/libmpv*.dylib` (macOS), or
`build/mpv-2.dll`/`build/libmpv-2.dll` (Windows via MSYS2/MinGW). Needs
[libplacebo](https://code.videolan.org/videolan/libplacebo) >= 7.349 --
see [`.github/workflows/build-libmpv.yml`](.github/workflows/build-libmpv.yml)
for exact per-platform dependency lists. Tagged releases (`v*`) attach
prebuilt libraries for all three platforms.
