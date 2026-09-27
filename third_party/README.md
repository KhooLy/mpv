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
