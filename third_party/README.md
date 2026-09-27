# Third-party integration patches

## FFmpeg MediaCodec

Apply `ffmpeg-mediacodec.patch` to the FFmpeg checkout used to build mpv,
before configuring FFmpeg:

```sh
git apply /path/to/ffmpeg-mediacodec.patch
```

It is generated against FFmpeg `bf1b838` and mirrors Media3's Android decoder
behavior:

- Dolby Vision decoder selection follows Media3's `MediaCodecVideoRenderer`
  and `MediaCodecUtil.getAlternativeCodecMimeType()`: `video/dolby-vision`
  is tried first, with the Android DV profile in the `MediaFormat`. Only
  profiles 4 and 8 (HEVC), 9 (AVC) and 10 (AV1, except full-range PQ) have a
  base-layer alternative. Profiles 5 and 7 never fall back, since their base
  layer is not a displayable stream;
- `prefer_base_layer` goes straight to that alternative decoder, as Media3
  does when the display does not advertise Dolby Vision. mpv sets it
  automatically on Android after querying the default display;
- mastering display and content light level metadata are passed as
  `hdr-static-info`, together with `frame-rate` and `priority=0`, like
  Media3's `MediaFormatUtil.maybeSetColorInfo()`;
- HDR10+ metadata carried beside the frames (Matroska BlockAdditions, as
  used by VP9 and AV1) is sent as `hdr10-plus-info` through
  `MediaCodec.setParameters()` before each packet, like Media3;
- decoder selection falls back to software-only codecs when no hardware
  decoder handles the stream;
- `tone_map_to_sdr` requests MediaCodec HDR-to-SDR output
  (`--vd-lavc-o=tone_map_to_sdr=1`);
- codec selection also checks `VideoCapabilities.areSizeAndRateSupported()`;
- H.264/HEVC streams without extradata (MPEG-TS, raw Annex B) are configured
  without `csd-*` buffers, so MediaCodec picks the parameter sets up in-band
  instead of failing to open.

This is not a software Dolby Vision decoder. Final Dolby Vision output still
requires a device/display path that advertises and implements Dolby Vision.
On devices without that capability, the base-layer fallback is the expected
behavior.

## libass Android font provider

Apply `libass-android-fontprovider.patch` to libass `b2fe9d8` before building
it for Android. libass has no system font provider there, so without it only
embedded fonts and the single default font work, and scripts that font lacks
(CJK, Arabic, Devanagari, ...) render as boxes.

The provider uses the NDK `AFontMatcher` API (Android 10+), loaded with
`dlsym` so the library still runs on older releases, where libass falls back
to having no provider. It resolves requested family names to system fonts
(e.g. `Arial` to Roboto) and answers per-codepoint fallback queries, opening
font files only when a glyph is actually needed. Fonts attached to the file
still take precedence.
