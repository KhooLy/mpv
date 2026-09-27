/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ao.h"
#include "audio/format.h"
#include "audio/out/ao_coreaudio_chmap.h"
#include "audio/out/ao_coreaudio_utils.h"
#include "common/common.h"
#include "common/msg.h"
#include "internal.h"
#include "osdep/timer.h"
#include "ta/ta_talloc.h"

#include <libavutil/intreadwrite.h>

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <CoreAudioTypes/CoreAudioTypes.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>


@interface AVObserver : NSObject {
    struct ao *ao;
}
- (void)handleRestartNotification:(NSNotification*)notification;
@end

struct priv {
    AVSampleBufferAudioRenderer *renderer;
    AVSampleBufferRenderSynchronizer *synchronizer;
    dispatch_queue_t queue;
    CMAudioFormatDescriptionRef format_description;
    AVObserver *observer;
    int64_t end_time_av;
    int period;
};

static int64_t CMTimeGetNanoseconds(CMTime time)
{
    time = CMTimeConvertScale(time, 1000000000, kCMTimeRoundingMethod_Default);
    return time.value;
}

static CMTime CMTimeFromNanoseconds(int64_t time)
{
    return CMTimeMake(time, 1000000000);
}

static bool create_compressed_format(struct ao *ao, const uint8_t *d, int len)
{
    struct priv *p = ao->priv;
    static const int rates[] = {48000, 44100, 32000, 0};
    static const int acmod_channels[] = {2, 1, 2, 3, 3, 4, 4, 5};

    if (len < 7 || d[0] != 0x0B || d[1] != 0x77)
        return false;

    AudioStreamBasicDescription asbd = {.mFramesPerPacket = 1536};
    int fscod = d[4] >> 6, acmod, lfe;
    if (ao->format == AF_FORMAT_S_EAC3) {
        asbd.mFormatID = kAudioFormatEnhancedAC3;
        asbd.mSampleRate = fscod == 3 ? rates[(d[4] >> 4) & 3] / 2 : rates[fscod];
        acmod = (d[4] >> 1) & 7;
        lfe = d[4] & 1;
    } else {
        asbd.mFormatID = kAudioFormatAC3;
        asbd.mSampleRate = rates[fscod];
        acmod = d[6] >> 5;
        int skip = ((acmod & 1) && acmod != 1 ? 2 : 0) + (acmod & 4 ? 2 : 0) +
                   (acmod == 2 ? 2 : 0);
        lfe = (d[6] >> (4 - skip)) & 1;
    }
    if (!asbd.mSampleRate)
        return false;
    asbd.mChannelsPerFrame = acmod_channels[acmod] + lfe;

    OSStatus err = CMAudioFormatDescriptionCreate(NULL, &asbd, 0, NULL, 0, NULL,
                                                  NULL, &p->format_description);
    if (err != noErr) {
        MP_ERR(ao, "failed to create compressed format description (%d)\n", err);
        return false;
    }
    MP_VERBOSE(ao, "compressed passthrough: %d Hz, %d channels\n",
               (int)asbd.mSampleRate, (int)asbd.mChannelsPerFrame);
    return true;
}

static void enqueue_packet(struct ao *ao, const uint8_t *src, int len, int64_t pts)
{
    struct priv *p = ao->priv;
    int size = (len + 1) & ~1;
    uint8_t *data = CFAllocatorAllocate(NULL, size, 0);
    for (int i = 0; i < size; i += 2) {
        data[i] = src[i + 1];
        data[i + 1] = src[i];
    }

    if (!p->format_description && !create_compressed_format(ao, data, len)) {
        CFAllocatorDeallocate(NULL, data);
        return;
    }

    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    if (CMBlockBufferCreateWithMemoryBlock(NULL, data, size, NULL, NULL, 0, len, 0,
                                           &block) != noErr)
    {
        CFAllocatorDeallocate(NULL, data);
        return;
    }
    AudioStreamPacketDescription desc = {.mDataByteSize = len};
    if (CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            NULL, block, p->format_description, 1, CMTimeFromNanoseconds(pts),
            &desc, &sample) == noErr)
    {
        [p->renderer enqueueSampleBuffer:sample];
        CFRelease(sample);
    }
    CFRelease(block);
}

static void feed_compressed(struct ao *ao)
{
    struct priv *p = ao->priv;
    int frames = MPMAX(1, ao->samplerate / 10 / p->period) * p->period;
    int bytes = frames * ao->sstride;
    uint8_t *buf = talloc_size(NULL, bytes);
    void *data[] = {buf};

    int64_t cur_time_av = CMTimeGetNanoseconds([p->synchronizer currentTime]);
    int64_t end_time_av = MPMAX(p->end_time_av, cur_time_av);
    int64_t period_ns = CMTimeGetNanoseconds(CMTimeMake(p->period, ao->samplerate));
    bool eof;
    int got = ao_read_data(ao, data, frames,
                           end_time_av - cur_time_av + mp_time_ns() + period_ns,
                           &eof, false, true);
    if (eof) {
        [p->renderer stopRequestingMediaData];
        ao_stop_streaming(ao);
    }

    for (int n = 0; n + p->period <= got; n += p->period) {
        uint8_t *burst = buf + n * ao->sstride;
        int64_t pts = end_time_av;
        end_time_av += period_ns;
        if (AV_RL16(burst) != 0xF872 || AV_RL16(burst + 2) != 0x4E1F)
            continue;
        int len = AV_RL16(burst + 6);
        if ((burst[4] & 0x7f) == 0x01)
            len /= 8;
        if (len <= 0 || 8 + len > p->period * ao->sstride)
            continue;
        enqueue_packet(ao, burst + 8, len, pts);
    }
    p->end_time_av = end_time_av;
    talloc_free(buf);
}

static void feed(struct ao *ao)
{
    struct priv *p = ao->priv;
    int samplerate = ao->samplerate;
    int sstride = ao->sstride;

    CMBlockBufferRef block_buffer = NULL;
    CMSampleBufferRef sample_buffer = NULL;
    OSStatus err;

    int request_sample_count = samplerate / 10;
    int buffer_size = request_sample_count * sstride;
    void *data[] = {CFAllocatorAllocate(NULL, buffer_size, 0)};

    int64_t cur_time_av = CMTimeGetNanoseconds([p->synchronizer currentTime]);
    int64_t cur_time_mp = mp_time_ns();
    int64_t end_time_av = MPMAX(p->end_time_av, cur_time_av);
    int64_t time_delta = CMTimeGetNanoseconds(CMTimeMake(request_sample_count, samplerate));
    bool eof;
    int real_sample_count = ao_read_data(ao, data, request_sample_count, end_time_av - cur_time_av + cur_time_mp + time_delta, &eof, false, true);
    if (eof) {
        [p->renderer stopRequestingMediaData];
        ao_stop_streaming(ao);
    }
    if (real_sample_count == 0) {
        goto finish;
    }

    if ((err = CMBlockBufferCreateWithMemoryBlock(
        NULL,
        data[0],
        buffer_size,
        NULL,
        NULL,
        0,
        real_sample_count * sstride,
        0,
        &block_buffer
    )) != noErr) {
        MP_FATAL(ao, "failed to create block buffer\n");
        MP_VERBOSE(ao, "CMBlockBufferCreateWithMemoryBlock returned %d\n", err);
        goto error;
    }
    data[0] = NULL;

    CMSampleTimingInfo sample_timing_into[] = {(CMSampleTimingInfo) {
        .duration = CMTimeMake(1, samplerate),
        .presentationTimeStamp = CMTimeFromNanoseconds(end_time_av),
        .decodeTimeStamp = kCMTimeInvalid
    }};
    size_t sample_size_array[] = {sstride};
    if ((err = CMSampleBufferCreateReady(
        NULL,
        block_buffer,
        p->format_description,
        real_sample_count,
        1,
        sample_timing_into,
        1,
        sample_size_array,
        &sample_buffer
    )) != noErr) {
        MP_FATAL(ao, "failed to create sample buffer\n");
        MP_VERBOSE(ao, "CMSampleBufferCreateReady returned %d\n", err);
        goto error;
    }

    [p->renderer enqueueSampleBuffer:sample_buffer];

    time_delta = CMTimeGetNanoseconds(CMTimeMake(real_sample_count, samplerate));
    p->end_time_av = end_time_av + time_delta;

    goto finish;

error:
    ao_request_reload(ao);
finish:
    if (data[0]) CFAllocatorDeallocate(NULL, data[0]);
    if (block_buffer) CFRelease(block_buffer);
    if (sample_buffer) CFRelease(sample_buffer);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;
    void (*fill)(struct ao *) = p->period ? feed_compressed : feed;

    p->end_time_av = -1;
    [p->synchronizer setRate:1];
    [p->renderer requestMediaDataWhenReadyOnQueue:p->queue usingBlock:^{
        fill(ao);
    }];
}

static void stop(struct ao *ao)
{
    struct priv *p = ao->priv;

    dispatch_sync(p->queue, ^{
        [p->renderer stopRequestingMediaData];
        [p->renderer flush];
        [p->synchronizer setRate:0];
    });
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    if (paused) {
        [p->synchronizer setRate:0];
    } else {
        [p->synchronizer setRate:1];
    }

    return true;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;

    switch (cmd) {
    case AOCONTROL_GET_MUTE:
        *(bool*)arg = [p->renderer isMuted];
        return CONTROL_OK;
    case AOCONTROL_GET_VOLUME:
        *(float*)arg = [p->renderer volume] * 100;
        return CONTROL_OK;
    case AOCONTROL_SET_MUTE:
        [p->renderer setMuted:*(bool*)arg];
        return CONTROL_OK;
    case AOCONTROL_SET_VOLUME:
        [p->renderer setVolume:*(float*)arg / 100];
        return CONTROL_OK;
    default:
        return CONTROL_UNKNOWN;
    }
}

@implementation AVObserver
- (instancetype)initWithAO:(struct ao*)_ao {
    self = [super init];
    if (self) {
        ao = _ao;
    }
    return self;
}
- (void)handleRestartNotification:(NSNotification*)notification {
    char *name = cfstr_get_cstr((CFStringRef)notification.name);
    MP_WARN(ao, "restarting due to system notification; this will cause desync\n");
    MP_VERBOSE(ao, "notification name: %s\n", name);
    talloc_free(name);
    stop(ao);
    start(ao);
}
@end

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    AudioChannelLayout *layout = NULL;

#if TARGET_OS_IPHONE
    AVAudioSession *instance = AVAudioSession.sharedInstance;
    NSInteger maxChannels = instance.maximumOutputNumberOfChannels;
    NSInteger prefChannels = MIN(maxChannels, ao->channels.num);
    [instance setCategory:AVAudioSessionCategoryPlayback error:nil];
    [instance setMode:AVAudioSessionModeMoviePlayback error:nil];
    [instance setActive:YES error:nil];
    [instance setPreferredOutputNumberOfChannels:prefChannels error:nil];
#endif

    if ((p->renderer = [[AVSampleBufferAudioRenderer alloc] init]) == nil) {
        MP_FATAL(ao, "failed to create audio renderer\n");
        MP_VERBOSE(ao, "AVSampleBufferAudioRenderer failed to initialize\n");
        goto error;
    }
    if ((p->synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init]) == nil) {
        MP_FATAL(ao, "failed to create rendering synchronizer\n");
        MP_VERBOSE(ao, "AVSampleBufferRenderSynchronizer failed to initialize\n");
        goto error;
    }
    if ((p->queue = dispatch_queue_create(
        "avfoundation event",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0)
    )) == NULL) {
        MP_FATAL(ao, "failed to create dispatch queue\n");
        MP_VERBOSE(ao, "dispatch_queue_create failed\n");
        goto error;
    }

#if HAVE_COREAUDIO_HAL
    if (ao->device && ao->device[0]) {
        [p->renderer setAudioOutputDeviceUniqueID:(NSString*)cfstr_from_cstr(ao->device)];
    }
#endif

    [p->synchronizer addRenderer:p->renderer];
#if HAVE_MACOS_11_3_FEATURES
    if (@available(tvOS 14.5, iOS 14.5, macOS 11.3, *)) {
        [p->synchronizer setDelaysRateChangeUntilHasSufficientMediaData:NO];
    }
#endif

    if (ao->format == AF_FORMAT_S_AC3 || ao->format == AF_FORMAT_S_EAC3) {
        p->period = ao->format == AF_FORMAT_S_EAC3 ? 6144 : 1536;
        ao->device_buffer = ao->samplerate * 2;
        goto observe;
    }
    if (af_fmt_is_spdif(ao->format)) {
        MP_FATAL(ao, "avfoundation only passes through AC3 and E-AC3\n");
        goto error;
    }

    // AVSampleBufferAudioRenderer only supports interleaved formats
    ao->format = af_fmt_from_planar(ao->format);
    if (af_fmt_is_planar(ao->format)) {
        MP_FATAL(ao, "planar audio formats are unsupported\n");
        goto error;
    }

    AudioStreamBasicDescription asbd;
    ca_fill_asbd(ao, &asbd);
    size_t layout_size;
    layout = ca_get_acl(ao, &layout_size);

    OSStatus err;
    if ((err = CMAudioFormatDescriptionCreate(
        NULL,
        &asbd,
        layout_size,
        layout,
        0,
        NULL,
        NULL,
        &p->format_description
    )) != noErr) {
        MP_FATAL(ao, "failed to create audio format description\n");
        MP_VERBOSE(ao, "CMAudioFormatDescriptionCreate returned %d\n", err);
        goto error;
    }
    talloc_free(layout);
    layout = NULL;

    // AVSampleBufferAudioRenderer read ahead aggressively
    ao->device_buffer = ao->samplerate * 2;

observe:
    p->observer = [[AVObserver alloc] initWithAO:ao];
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
#if HAVE_MACOS_12_FEATURES
    if (@available(tvOS 15.0, iOS 15.0, macOS 12.0, *)) {
        [center addObserver:p->observer selector:@selector(handleRestartNotification:) name:AVSampleBufferAudioRendererOutputConfigurationDidChangeNotification object:p->renderer];
    }
#endif
    [center addObserver:p->observer selector:@selector(handleRestartNotification:) name:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification object:p->renderer];

    return CONTROL_OK;

error:
    talloc_free(layout);
    if (p->renderer) [p->renderer release];
    if (p->synchronizer) [p->synchronizer release];
    if (p->queue) dispatch_release(p->queue);
    if (p->format_description) CFRelease(p->format_description);

#if TARGET_OS_IPHONE
    [AVAudioSession.sharedInstance setActive:NO
        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
        error:nil
    ];
#endif

    return CONTROL_ERROR;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    stop(ao);

    [p->renderer release];
    [p->synchronizer release];
    dispatch_release(p->queue);
    if (p->format_description)
        CFRelease(p->format_description);

    [[NSNotificationCenter defaultCenter] removeObserver:p->observer];
    [p->observer release];

#if TARGET_OS_IPHONE
    [AVAudioSession.sharedInstance setActive:NO
        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
        error:nil
    ];
#endif
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_avfoundation = {
    .description    = "AVFoundation AVSampleBufferAudioRenderer",
    .name           = "avfoundation",
    .uninit         = uninit,
    .init           = init,
    .control        = control,
    .reset          = stop,
    .start          = start,
    .set_pause      = set_pause,
#if HAVE_COREAUDIO_HAL
    .list_devs      = ca_get_device_list,
#endif
    .priv_size      = sizeof(struct priv),
};
