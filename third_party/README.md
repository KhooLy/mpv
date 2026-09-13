# Third-party integration patches

## FFmpeg MediaCodec Dolby Vision

Apply `ffmpeg-mediacodec-dolbyvision.patch` to the FFmpeg checkout used to
build mpv, before configuring FFmpeg:

```sh
git apply /path/to/ffmpeg-mediacodec-dolbyvision.patch
```

The patch keeps the normal FFmpeg codec ID, but mirrors Media3's Android
selection behavior for streams carrying `AV_PKT_DATA_DOVI_CONF`:

- try `video/dolby-vision` first;
- pass the Android Dolby Vision profile and stream color information in the
  `MediaFormat`;
- retry with the compatible base-layer MIME (`video/hevc`, `video/avc`, or
  `video/av01`) if the device cannot initialize the Dolby Vision decoder;
- restore the base codec profile before the fallback configure attempt.
- `ffmpeg-mediacodec-tonemap.patch` adds the `tone_map_to_sdr` decoder option.
  Pass it through mpv as `--vd-lavc-o=tone_map_to_sdr=1` when an SDR output is
  desired. The decoder logs the resulting MediaFormat so support can be
  verified per device.
- `ffmpeg-mediacodec-selection.patch` adds `prefer_base_layer`, which the
  Android frontend sets when the display does not advertise Dolby Vision.
- `ffmpeg-mediacodec-capabilities.patch` adds the optional
  `VideoCapabilities.areSizeAndRateSupported()` check, so codec selection also
  rejects decoders that cannot handle the stream dimensions or frame rate.

This is not a software Dolby Vision decoder. Final Dolby Vision output still
requires a device/display path that advertises and implements Dolby Vision.
On devices without that capability, the base-layer fallback is the expected
behavior.
