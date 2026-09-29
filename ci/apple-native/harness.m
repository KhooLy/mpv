#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mpv/client.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_prop(mpv_handle *mpv, const char *name)
{
    char *v = mpv_get_property_string(mpv, name);
    printf("  %-40s %s\n", name, v ? v : "(unavailable)");
    mpv_free(v);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: harness FILE [option=value...]\n");
        return 2;
    }

    AVSampleBufferDisplayLayer *layer = [[AVSampleBufferDisplayLayer alloc] init];
    layer.bounds = CGRectMake(0, 0, 1280, 720);
    layer.contentsScale = 1;

    mpv_handle *mpv = mpv_create();
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "msg-level", "all=info,vd=v,vo=v,ao=v,cplayer=v");
    mpv_set_option_string(mpv, "vo", "apple_native");
    mpv_set_option_string(mpv, "ao", "avfoundation,null");
    int64_t wid = (int64_t)(intptr_t)layer;
    mpv_set_option(mpv, "wid", MPV_FORMAT_INT64, &wid);
    bool expect_overlay = false, expect_shaded = false;
    const char *expect_gamma = NULL, *expect_audio = NULL;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "expect-gamma=", 13) == 0) {
            expect_gamma = argv[i] + 13;
            continue;
        }
        if (strncmp(argv[i], "expect-audio=", 13) == 0) {
            expect_audio = argv[i] + 13;
            continue;
        }
        if (strcmp(argv[i], "expect-shaded=yes") == 0) {
            expect_shaded = true;
            continue;
        }
        if (strcmp(argv[i], "expect-overlay=yes") == 0) {
            expect_overlay = true;
            continue;
        }
        char *eq = strchr(argv[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (mpv_set_option_string(mpv, argv[i], eq + 1) < 0)
            fprintf(stderr, "bad option %s\n", argv[i]);
    }
    if (mpv_initialize(mpv) < 0)
        return 1;
    mpv_request_log_messages(mpv, "info");
    int chain_frames = 0, chain_skipped = 0, chain_checked = 0, chain_passes = 0;
    double chain_in = 0, chain_out = 0, chain_std = 0;
    bool chain_reported = false, chain_error = false;

    const char *cmd[] = {"loadfile", argv[1], NULL};
    mpv_command(mpv, cmd);

    double start = 0, deadline = now() + 60, duration = 0, start_pos = 0;
    bool sampled = false, eof = false, overlay_used = false, native = false;
    char *gamma = NULL, *audio = NULL;
    int end_error = 0;
    while (now() < deadline) {
        mpv_event *ev = mpv_wait_event(mpv, 0.005);
        if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART && !start) {
            start = now();
            mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
            mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &start_pos);
        }
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *lm = ev->data;
            fputs(lm->text, stdout);
            const char *line = strstr(lm->text, "shader-chain: ");
            if (line && sscanf(line, "shader-chain: frames=%d skipped=%d checked=%d in_luma=%lf "
                               "out_luma=%lf out_std=%lf passes=%d", &chain_frames,
                               &chain_skipped, &chain_checked, &chain_in, &chain_out,
                               &chain_std, &chain_passes) >= 3)
                chain_reported = true;
            if (strstr(lm->text, "Metal compile failed") || strstr(lm->text, "Pipeline failed") ||
                strstr(lm->text, "Metal command buffer failed"))
                chain_error = true;
        }
        if (ev->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *ef = ev->data;
            eof = ef->reason == MPV_END_FILE_REASON_EOF;
            end_error = ef->error;
            break;
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);

        for (CALayer *sub in layer.sublayers) {
            if (sub.zPosition == 1 && sub.sublayers.count)
                overlay_used = true;
        }

        double pos = 0;
        if (!sampled && start &&
            mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0 &&
            pos >= duration * 0.8)
        {
            sampled = true;
            printf("properties at %.2fs:\n", pos);
            print_prop(mpv, "current-vo");
            print_prop(mpv, "current-tracks/video/decoder-desc");
            char *desc = mpv_get_property_string(mpv, "current-tracks/video/decoder-desc");
            native = desc && strcmp(desc, "AVSampleBufferDisplayLayer") == 0;
            mpv_free(desc);
            gamma = mpv_get_property_string(mpv, "video-params/gamma");
            audio = mpv_get_property_string(mpv, "audio-params/format");
            print_prop(mpv, "current-tracks/video/codec");
            print_prop(mpv, "video-params/pixelformat");
            print_prop(mpv, "video-params/gamma");
            print_prop(mpv, "current-ao");
            print_prop(mpv, "audio-codec-name");
            print_prop(mpv, "audio-params/format");
            print_prop(mpv, "avsync");
            print_prop(mpv, "frame-drop-count");
            print_prop(mpv, "decoder-frame-drop-count");
            print_prop(mpv, "mistimed-frame-count");
            print_prop(mpv, "vo-delayed-frame-count");
        }
    }
    double wall = start ? now() - start : 0;

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    NSError *error = layer.sampleBufferRenderer.error;
    printf("layer status: %ld%s%s\n", (long)layer.sampleBufferRenderer.status,
           error ? " error: " : "", error ? error.description.UTF8String : "");
    printf("overlay drew: %s\n", overlay_used ? "yes" : "no");
    double expected = duration - start_pos;
    printf("played %.2fs of media in %.2fs\n", expected, wall);

    int fail = 0;
    if (!eof) {
        printf("FAIL: playback did not reach EOF (%s)\n", mpv_error_string(end_error));
        fail = 1;
    }
    if (layer.sampleBufferRenderer.status == AVQueuedSampleBufferRenderingStatusFailed) {
        printf("FAIL: display layer failed\n");
        fail = 1;
    }
    if (expect_shaded) {
        printf("shader chain: reported=%d frames=%d skipped=%d checked=%d passes=%d "
               "in_luma=%.4f out_luma=%.4f out_std=%.4f\n", chain_reported, chain_frames,
               chain_skipped, chain_checked, chain_passes, chain_in, chain_out, chain_std);
        if (!chain_reported || chain_passes < 1) {
            printf("FAIL: shader chain did not start\n");
            fail = 1;
        } else {
            if (chain_error) {
                printf("FAIL: Metal reported an error\n");
                fail = 1;
            }
            if (chain_frames < duration * 8) {
                printf("FAIL: too few frames went through the shaders\n");
                fail = 1;
            }
            if (chain_checked < 1 || chain_std < 0.05) {
                printf("FAIL: shaded output is blank or flat\n");
                fail = 1;
            }
            if (fabs(chain_in - chain_out) > 0.08) {
                printf("FAIL: shaded output brightness differs from the source\n");
                fail = 1;
            }
        }
    } else if (!native) {
        printf("FAIL: video did not use the native decoder\n");
        fail = 1;
    }
    if (expect_gamma && (!gamma || strcmp(gamma, expect_gamma) != 0)) {
        printf("FAIL: expected gamma %s\n", expect_gamma);
        fail = 1;
    }
    mpv_free(gamma);
    if (expect_audio && (!audio || strcmp(audio, expect_audio) != 0)) {
        printf("FAIL: expected audio format %s\n", expect_audio);
        fail = 1;
    }
    mpv_free(audio);
    if (expect_overlay && !overlay_used) {
        printf("FAIL: subtitles were not drawn into the overlay\n");
        fail = 1;
    }
    if (expected > 0 && fabs(wall - expected) > expected * 0.2 + 0.5) {
        printf("FAIL: wall time does not match media duration\n");
        fail = 1;
    }

    mpv_terminate_destroy(mpv);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    [layer release];
    return fail;
}
