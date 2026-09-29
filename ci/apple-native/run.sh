#!/bin/sh
set -u

dir=$(mktemp -d)
build=${1:-build}
cc -o "$dir/harness" ci/apple-native/harness.m -Iinclude -L"$build" -lmpv \
    -framework Foundation -framework AVFoundation -framework QuartzCore -framework CoreMedia || exit 1

video="-f lavfi -i testsrc2=size=1280x720:rate=24 -t 5"
audio="-f lavfi -i sine=frequency=440:sample_rate=48000 -t 5"
hdr="hdr10=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400"
ff="ffmpeg -loglevel error -y"

$ff $video -c:v libx264 -pix_fmt yuv420p "$dir/h264.mkv"
$ff $video -c:v libx265 -pix_fmt yuv420p10le -x265-params "$hdr:log-level=error" \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$dir/hdr10.mkv"
$ff -i "$dir/hdr10.mkv" -c copy -tag:v hvc1 "$dir/hdr10.mp4"
python3 -c '
import json
scene = {"BezierCurveData": {"Anchors": [256, 512, 768, 1024, 1280, 1536, 1792, 2048, 2304], "KneePointX": 100, "KneePointY": 200},
         "LuminanceParameters": {"AverageRGB": 1000, "LuminanceDistributions": {"DistributionIndex": [1, 5, 10, 25, 50, 75, 90, 95, 99], "DistributionValues": [0, 10, 50, 100, 200, 400, 600, 800, 1000]}, "MaxScl": [1000, 1000, 1000]},
         "NumberOfWindows": 1, "TargetedSystemDisplayMaximumLuminance": 1000}
frames = [dict(scene, SceneFrameIndex=i, SceneId=0, SequenceFrameIndex=i) for i in range(120)]
json.dump({"JSONInfo": {"HDR10plusProfile": "B", "Version": "1.0"}, "SceneInfo": frames, "SceneInfoSummary": {"SceneFirstFrameIndex": [0], "SceneFrameNumbers": [120]}, "ToneMappingMode": "LLC"}, open("'"$dir"'/hdr10plus.json", "w"))
'
$ff $video -c:v libx265 -pix_fmt yuv420p10le -x265-params "$hdr:dhdr10-info=$dir/hdr10plus.json:log-level=error" \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$dir/hdr10plus.mkv"
$ff $video -c:v libsvtav1 -pix_fmt yuv420p10le "$dir/av1.mkv"
$ff $video $audio -filter_complex "[1:a]pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0[a]" \
    -map 0:v -map "[a]" -c:v libx264 -pix_fmt yuv420p -c:a eac3 "$dir/eac3.mkv"
printf '[Script Info]\nScriptType: v4.00+\nPlayResX: 1280\nPlayResY: 720\n\n[V4+ Styles]\nFormat: Name, Fontsize, PrimaryColour\nStyle: Default,48,&H00FFFFFF\n\n[Events]\nFormat: Layer, Start, End, Style, Text\nDialogue: 0,0:00:00.00,0:00:05.00,Default,subtitle test\n' > "$dir/sub.ass"

base=https://raw.githubusercontent.com/bloc97/Anime4K/master/glsl
shaders="$dir/shaders"
mkdir -p "$shaders"
for f in Restore/Anime4K_Clamp_Highlights Restore/Anime4K_Restore_CNN_S \
         Restore/Anime4K_Restore_CNN_M Upscale/Anime4K_Upscale_CNN_x2_S \
         Upscale/Anime4K_Upscale_CNN_x2_M; do
    curl -fsSL "$base/$f.glsl" -o "$shaders/$(basename "$f").glsl" || exit 1
done
mode_a="$shaders/Anime4K_Clamp_Highlights.glsl:$shaders/Anime4K_Restore_CNN_S.glsl:$shaders/Anime4K_Upscale_CNN_x2_S.glsl"
mode_b="$shaders/Anime4K_Clamp_Highlights.glsl:$shaders/Anime4K_Restore_CNN_M.glsl:$shaders/Anime4K_Upscale_CNN_x2_M.glsl"

status=0
run() {
    name=$1; shift
    echo "::group::$name"
    DYLD_LIBRARY_PATH="$build" "$dir/harness" "$@"
    rc=$?
    echo "::endgroup::"
    if [ $rc -ne 0 ]; then
        echo "::error::$name failed"
        status=1
    fi
}

run "H.264 MKV" "$dir/h264.mkv"
run "HEVC HDR10 MKV" "$dir/hdr10.mkv" expect-gamma=pq
run "HEVC HDR10 MP4" "$dir/hdr10.mp4" expect-gamma=pq
run "HEVC HDR10+ MKV" "$dir/hdr10plus.mkv" expect-gamma=pq
run "AV1 MKV" "$dir/av1.mkv"
run "E-AC3 5.1 PCM" "$dir/eac3.mkv"
run "E-AC3 5.1 passthrough" "$dir/eac3.mkv" audio-spdif=eac3 expect-audio=spdif-eac3
run "E-AC3 auto passthrough" "$dir/eac3.mkv" audio-spdif=auto expect-audio=spdif-eac3
run "ASS subtitles" "$dir/h264.mkv" sub-files="$dir/sub.ass" expect-overlay=yes
run "Anime4K fast" "$dir/h264.mkv" hwdec=videotoolbox vo-apple-native-shaders="$mode_a" expect-shaded=yes
run "Anime4K quality" "$dir/h264.mkv" hwdec=videotoolbox vo-apple-native-shaders="$mode_b" expect-shaded=slow
run "Seek and pause" "$dir/hdr10.mkv" start=2 pause=no

exit $status
