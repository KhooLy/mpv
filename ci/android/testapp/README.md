# Android test app

A small harness that embeds libmpv in a SurfaceView with `vo=mediacodec_embed`
and `hwdec=mediacodec`, shows the display's HDR/Dolby Vision capabilities, and
has buttons for the bundled SDR/HDR10/HLG clips plus a file picker.

Build libmpv first, then the app:

    ci/android/build.sh
    cd ci/android/testapp && gradle assembleDebug

The app picks up `android-build/out` from the repository root; pass
`-PmpvOut=/path` to use another `build.sh` output (e.g. one built with
`WORK=`).

It can also be started from adb with extra options and a path or URL:

    adb shell "am start -S -n org.mpv.androidtest/.MainActivity --es opts 'sid=1;sub-pos=70' --es path URL"

Quote the whole command, otherwise the device shell splits on `;`. Log
messages go to logcat under the tag `mpvtest`.
