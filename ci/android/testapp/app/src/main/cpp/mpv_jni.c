#include <jni.h>
#include <android/log.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/jni.h>
#include <mpv/client.h>

struct mpv_test_instance {
    mpv_handle *mpv;
    jobject surface;
    pthread_t event_thread;
    atomic_bool event_thread_running;
    bool event_thread_started;
    pthread_t probe_thread;
    atomic_bool probe_running;
    bool probe_started;
};

static void *probe_loop(void *arg)
{
    struct mpv_test_instance *instance = arg;
    int64_t window = mpv_get_time_us(instance->mpv), worst = 0, sum = 0, n = 0;
    while (atomic_load(&instance->probe_running)) {
        int64_t t0 = mpv_get_time_us(instance->mpv);
        double v;
        mpv_get_property(instance->mpv, "time-pos", MPV_FORMAT_DOUBLE, &v);
        int64_t d = mpv_get_time_us(instance->mpv) - t0;
        if (d > worst)
            worst = d;
        sum += d;
        n++;
        if (t0 - window >= 1000000) {
            int64_t drops = 0;
            mpv_get_property(instance->mpv, "frame-drop-count", MPV_FORMAT_INT64, &drops);
            __android_log_print(ANDROID_LOG_INFO, "mpvprobe", "max=%lldus avg=%lldus drops=%lld",
                                (long long)worst, (long long)(sum / n), (long long)drops);
            window = t0;
            worst = sum = n = 0;
        }
        usleep(5000);
    }
    return NULL;
}


static void *mpv_event_loop(void *arg)
{
    struct mpv_test_instance *instance = arg;
    while (atomic_load(&instance->event_thread_running)) {
        mpv_event *event = mpv_wait_event(instance->mpv, 1.0);
        if (!event)
            continue;
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *prop = event->data;
            if (prop->format == MPV_FORMAT_STRING)
                __android_log_print(ANDROID_LOG_INFO, "mpvstat", "%s=%s",
                                    prop->name, *(char **)prop->data);
            continue;
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *ef = event->data;
            __android_log_print(ANDROID_LOG_INFO, "mpvstat", "end-file reason=%d error=%d",
                                ef->reason, ef->error);
            continue;
        }
        if (event->event_id != MPV_EVENT_LOG_MESSAGE)
            continue;

        mpv_event_log_message *message = event->data;
        __android_log_print(ANDROID_LOG_INFO, "mpvtest", "[%s/%s] %s",
                            message->prefix ? message->prefix : "mpv",
                            message->level ? message->level : "info",
                            message->text ? message->text : "");
    }
    return NULL;
}

static void stop_mpv_event_loop(struct mpv_test_instance *instance)
{
    if (instance->probe_started) {
        atomic_store(&instance->probe_running, false);
        pthread_join(instance->probe_thread, NULL);
        instance->probe_started = false;
    }
    if (!instance->event_thread_started)
        return;
    atomic_store(&instance->event_thread_running, false);
    mpv_wakeup(instance->mpv);
    pthread_join(instance->event_thread, NULL);
    instance->event_thread_started = false;
}

static int set_option(mpv_handle *ctx, const char *name, const char *value)
{
    return mpv_set_option_string(ctx, name, value);
}

JNIEXPORT jlong JNICALL
Java_org_mpv_androidtest_MainActivity_nativeCreate(JNIEnv *env, jclass cls,
                                                    jobject surface,
                                                    jstring path,
                                                    jint tone_map_to_sdr,
                                                    jint prefer_base_layer,
                                                    jstring extra_opts)
{
    (void)cls;
    struct mpv_test_instance *instance = calloc(1, sizeof(*instance));
    if (!instance)
        return 0;

    JavaVM *vm = NULL;
    if ((*env)->GetJavaVM(env, &vm) != JNI_OK || !vm)
        goto fail;
    av_jni_set_java_vm(vm, NULL);

    instance->mpv = mpv_create();
    if (!instance->mpv)
        goto fail;

    if (set_option(instance->mpv, "vo", "mediacodec_embed") < 0 ||
        set_option(instance->mpv, "hwdec", "mediacodec") < 0 ||
        set_option(instance->mpv, "ao", "audiotrack") < 0 ||
        set_option(instance->mpv, "config", "no") < 0 ||
        set_option(instance->mpv, "keep-open", "yes") < 0 ||
        set_option(instance->mpv, "msg-level", "all=info") < 0)
        goto fail;

    if (tone_map_to_sdr || prefer_base_layer) {
        char lavc_options[96];
        snprintf(lavc_options, sizeof(lavc_options),
                 "tone_map_to_sdr=%d,prefer_base_layer=%d",
                 tone_map_to_sdr ? 1 : 0, prefer_base_layer ? 1 : 0);
        if (set_option(instance->mpv, "vd-lavc-o", lavc_options) < 0)
            goto fail;
    }

    if (extra_opts) {
        const char *opts = (*env)->GetStringUTFChars(env, extra_opts, NULL);
        char *copy = opts ? strdup(opts) : NULL;
        if (opts)
            (*env)->ReleaseStringUTFChars(env, extra_opts, opts);
        char *save = NULL;
        for (char *kv = copy ? strtok_r(copy, ";", &save) : NULL; kv;
             kv = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(kv, '=');
            if (!eq)
                continue;
            *eq = '\0';
            int r = set_option(instance->mpv, kv, eq + 1);
            __android_log_print(ANDROID_LOG_INFO, "mpvstat", "option %s=%s -> %d",
                                kv, eq + 1, r);
        }
        free(copy);
    }

    instance->surface = (*env)->NewGlobalRef(env, surface);
    if (!instance->surface)
        goto fail;

    char wid[32];
    snprintf(wid, sizeof(wid), "%" PRIdPTR, (intptr_t)instance->surface);
    if (set_option(instance->mpv, "wid", wid) < 0)
        goto fail;
    if (mpv_initialize(instance->mpv) < 0)
        goto fail;

    mpv_request_log_messages(instance->mpv, "v");
    static const char *const props[] = {
        "hwdec-current", "current-vo", "current-ao", "audio-codec-name",
        "audio-params/format", "audio-params/channels", "audio-params/samplerate",
        "frame-drop-count", "decoder-frame-drop-count", "mistimed-frame-count",
        "vo-delayed-frame-count", "eof-reached", "video-params/gamma",
        "video-params/primaries", "video-params/colormatrix", "track-list/count",
    };
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++)
        mpv_observe_property(instance->mpv, 0, props[i], MPV_FORMAT_STRING);
    atomic_init(&instance->event_thread_running, true);
    if (pthread_create(&instance->event_thread, NULL, mpv_event_loop, instance) != 0)
        goto fail;
    instance->event_thread_started = true;

    const char *file = (*env)->GetStringUTFChars(env, path, NULL);
    if (!file)
        goto fail;
    const char *command[] = {"loadfile", file, "replace", NULL};
    int rc = mpv_command(instance->mpv, command);
    (*env)->ReleaseStringUTFChars(env, path, file);
    if (rc < 0)
        goto fail;

    return (jlong)(intptr_t)instance;

fail:
    stop_mpv_event_loop(instance);
    if (instance->surface)
        (*env)->DeleteGlobalRef(env, instance->surface);
    if (instance->mpv)
        mpv_terminate_destroy(instance->mpv);
    free(instance);
    return 0;
}

JNIEXPORT jint JNICALL
Java_org_mpv_androidtest_MainActivity_nativeLoad(JNIEnv *env, jclass cls,
                                                  jlong handle, jstring path)
{
    (void)cls;
    struct mpv_test_instance *instance = (void *)(intptr_t)handle;
    if (!instance || !instance->mpv)
        return -1;
    const char *file = (*env)->GetStringUTFChars(env, path, NULL);
    if (!file)
        return -1;
    const char *command[] = {"loadfile", file, "replace", NULL};
    int rc = mpv_command(instance->mpv, command);
    (*env)->ReleaseStringUTFChars(env, path, file);
    return rc;
}

JNIEXPORT void JNICALL
Java_org_mpv_androidtest_MainActivity_nativeDestroy(JNIEnv *env, jclass cls,
                                                     jlong handle)
{
    (void)cls;
    struct mpv_test_instance *instance = (void *)(intptr_t)handle;
    if (!instance)
        return;
    stop_mpv_event_loop(instance);
    if (instance->mpv)
        mpv_terminate_destroy(instance->mpv);
    if (instance->surface)
        (*env)->DeleteGlobalRef(env, instance->surface);
    free(instance);
}

JNIEXPORT jint JNICALL
Java_org_mpv_androidtest_MainActivity_nativeCommand(JNIEnv *env, jclass cls,
                                                     jlong handle, jstring cmd)
{
    (void)cls;
    struct mpv_test_instance *instance = (void *)(intptr_t)handle;
    if (!instance || !instance->mpv)
        return -1;
    const char *s = (*env)->GetStringUTFChars(env, cmd, NULL);
    if (!s)
        return -1;
    int rc;
    if (!strcmp(s, "probe")) {
        rc = 0;
        if (!instance->probe_started) {
            atomic_store(&instance->probe_running, true);
            instance->probe_started = !pthread_create(&instance->probe_thread, NULL,
                                                      probe_loop, instance);
        }
    } else if (!strncmp(s, "thumb ", 6)) {
        const char *args[] = {"thumbnail", s + 6, NULL};
        mpv_node res;
        int64_t t0 = mpv_get_time_us(instance->mpv);
        rc = mpv_command_ret(instance->mpv, args, &res);
        int64_t t1 = mpv_get_time_us(instance->mpv);
        char *info = mpv_get_property_string(instance->mpv, "thumbnail-info");
        __android_log_print(ANDROID_LOG_INFO, "mpvstat", "thumbinfo %s", info ? info : "-");
        mpv_free(info);
        if (rc >= 0) {
            int64_t w = 0, h = 0; double tm = 0; int exact = 0; mpv_byte_array *ba = NULL;
            for (int i = 0; i < res.u.list->num; i++) {
                const char *k = res.u.list->keys[i]; mpv_node *v = &res.u.list->values[i];
                if (!strcmp(k, "w")) w = v->u.int64;
                if (!strcmp(k, "h")) h = v->u.int64;
                if (!strcmp(k, "time")) tm = v->u.double_;
                if (!strcmp(k, "exact")) exact = v->u.flag;
                if (!strcmp(k, "data")) ba = v->u.ba;
            }
            __android_log_print(ANDROID_LOG_INFO, "mpvstat", "thumb %s -> %lldx%lld t=%.1f exact=%d %lldus",
                                s + 6, (long long)w, (long long)h, tm, exact, (long long)(t1 - t0));
            FILE *f = fopen("/data/user/0/org.mpv.androidtest/files/thumb.bgra", "wb");
            if (f && ba) { fwrite(ba->data, ba->size, 1, f); }
            if (f) fclose(f);
            mpv_free_node_contents(&res);
        }
    } else {
        rc = mpv_command_string(instance->mpv, s);
    }
    __android_log_print(ANDROID_LOG_INFO, "mpvstat", "command %s -> %d", s, rc);
    (*env)->ReleaseStringUTFChars(env, cmd, s);
    return rc;
}
