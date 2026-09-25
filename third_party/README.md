# Third-party integration patches

## FFmpeg MediaCodec

Apply `ffmpeg-mediacodec.patch` to the FFmpeg checkout used to build mpv,
before configuring FFmpeg:

```sh
git apply /path/to/ffmpeg-mediacodec.patch
```

It is generated against FFmpeg `bf1b838` and mirrors Media3's Android decoder
behavior:

- streams carrying `AV_PKT_DATA_DOVI_CONF` try `video/dolby-vision` first,
  with the Android Dolby Vision profile and stream color information in the
  `MediaFormat`, then retry with the compatible base-layer MIME
  (`video/hevc`, `video/avc`, or `video/av01`) if the device cannot
  initialize the Dolby Vision decoder;
- `prefer_base_layer` skips the Dolby Vision decoder, for displays that do not
  advertise Dolby Vision;
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
