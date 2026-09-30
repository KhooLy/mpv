/*
 * JNI utility functions
 *
 * Copyright (c) 2015-2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
 *
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

#include <android/api-level.h>
#include <libavcodec/jni.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/common.h"
#include "jni.h"
#include "mpv_talloc.h"
#include "osdep/threads.h"

static JavaVM *java_vm;
static pthread_key_t current_env;
static mp_once once = MP_STATIC_ONCE_INITIALIZER;
static mp_static_mutex lock = MP_STATIC_MUTEX_INITIALIZER;

static void jni_detach_env(void *data)
{
    if (java_vm) {
        (*java_vm)->DetachCurrentThread(java_vm);
    }
}

static void jni_create_pthread_key(void)
{
    pthread_key_create(&current_env, jni_detach_env);
}

JNIEnv *mp_jni_get_env(struct mp_log *log)
{
    JNIEnv *env = NULL;

    mp_mutex_lock(&lock);
    if (!java_vm)
        java_vm = av_jni_get_java_vm(NULL);

    if (!java_vm) {
        mp_err(log, "No Java virtual machine has been registered\n");
        goto done;
    }

    mp_exec_once(&once, jni_create_pthread_key);

    if ((env = pthread_getspecific(current_env)) != NULL)
        goto done;

    int ret = (*java_vm)->GetEnv(java_vm, (void **)&env, JNI_VERSION_1_6);
    switch(ret) {
    case JNI_EDETACHED:
        if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != 0) {
            mp_err(log, "Failed to attach the JNI environment to the current thread\n");
            env = NULL;
        } else {
            pthread_setspecific(current_env, env);
        }
        break;
    case JNI_OK:
        break;
    case JNI_EVERSION:
        mp_err(log, "The specified JNI version is not supported\n");
        break;
    default:
        mp_err(log, "Failed to get the JNI environment attached to this thread\n");
        break;
    }

done:
    mp_mutex_unlock(&lock);
    return env;
}

char *mp_jni_jstring_to_utf_chars(JNIEnv *env, jstring string, struct mp_log *log)
{
    if (!string)
        return NULL;

    const char *utf_chars = (*env)->GetStringUTFChars(env, string, NULL);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "getStringUTFChars() threw an exception\n");
        return NULL;
    }

    char *ret = talloc_strdup(NULL, utf_chars);

    (*env)->ReleaseStringUTFChars(env, string, utf_chars);

    return ret;
}

jstring mp_jni_utf_chars_to_jstring(JNIEnv *env, const char *utf_chars,
                                    struct mp_log *log)
{
    jstring ret = (*env)->NewStringUTF(env, utf_chars);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "NewStringUTF() threw an exception\n");
        return NULL;
    }

    return ret;
}

int mp_jni_exception_get_summary(JNIEnv *env, jthrowable exception,
                                 char **error, struct mp_log *log)
{
    int ret = 0;

    char *name = NULL;
    char *message = NULL;

    jclass class_class = NULL;
    jclass exception_class = NULL;
    jstring string = NULL;

    *error = NULL;

    exception_class = (*env)->GetObjectClass(env, exception);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find Throwable class\n");
        ret = -1;
        goto done;
    }

    class_class = (*env)->GetObjectClass(env, exception_class);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find Throwable class's class\n");
        ret = -1;
        goto done;
    }

    jmethodID get_name_id = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find method Class.getName()\n");
        ret = -1;
        goto done;
    }

    string = (*env)->CallObjectMethod(env, exception_class, get_name_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Class.getName() threw an exception\n");
        ret = -1;
        goto done;
    }

    if (string) {
        name = mp_jni_jstring_to_utf_chars(env, string, log);
        MP_JNI_LOCAL_FREEP(&string);
    }

    jmethodID get_message_id = (*env)->GetMethodID(env, exception_class, "getMessage", "()Ljava/lang/String;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find method Throwable.getMessage()\n");
        ret = -1;
        goto done;
    }

    string = (*env)->CallObjectMethod(env, exception, get_message_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Throwable.getMessage() threw an exception\n");
        ret = -1;
        goto done;
    }

    if (string) {
        message = mp_jni_jstring_to_utf_chars(env, string, log);
        MP_JNI_LOCAL_FREEP(&string);
    }

    if (name && message) {
        *error = talloc_asprintf(NULL, "%s: %s", name, message);
    } else if (name && !message) {
        *error = talloc_asprintf(NULL, "%s occurred", name);
    } else if (!name && message) {
        *error = talloc_asprintf(NULL, "Exception: %s", message);
    } else {
        mp_warn(log, "Could not retrieve exception name and message\n");
        *error = talloc_strdup(NULL, "Exception occurred");
    }

done:

    talloc_free(name);
    talloc_free(message);

    MP_JNI_LOCAL_FREEP(&class_class);
    MP_JNI_LOCAL_FREEP(&exception_class);
    MP_JNI_LOCAL_FREEP(&string);

    return ret;
}

int mp_jni_exception_check(JNIEnv *env, int logging, struct mp_log *log)
{
    if (!(*env)->ExceptionCheck(env))
        return 0;

    if (!logging) {
        (*env)->ExceptionClear(env);
        return -1;
    }

    jthrowable exception = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);

    char *message = NULL;
    int ret = mp_jni_exception_get_summary(env, exception, &message, log);
    MP_JNI_LOCAL_FREEP(&exception);
    if (ret < 0)
        return ret;

    mp_err(log, "%s\n", message);
    talloc_free(message);
    return -1;
}

#define CHECK_EXC_MANDATORY() do { \
        if ((ret = mp_jni_exception_check(env, mandatory, log)) < 0 && \
             mandatory) { \
            goto done; \
        } \
    } while (0)

int mp_jni_init_jfields(JNIEnv *env, void *jfields,
                        const struct MPJniField *jfields_mapping,
                        int global, struct mp_log *log)
{
    int ret = 0;
    jclass last_clazz = NULL;

    for (int i = 0; jfields_mapping[i].name; i++) {
        bool mandatory = !!jfields_mapping[i].mandatory;
        enum MPJniFieldType type = jfields_mapping[i].type;

        void *jfield = (uint8_t*)jfields + jfields_mapping[i].offset;

        if (type == MP_JNI_CLASS) {
            last_clazz = NULL;

            jclass clazz = (*env)->FindClass(env, jfields_mapping[i].name);
            CHECK_EXC_MANDATORY();

            last_clazz = *(jclass*)jfield =
                    global ? (*env)->NewGlobalRef(env, clazz) : clazz;

            if (global)
                MP_JNI_LOCAL_FREEP(&clazz);

            continue;
        }

        if (!last_clazz) {
            ret = -1;
            break;
        }

        switch (type) {
        case MP_JNI_FIELD: {
            jfieldID field_id = (*env)->GetFieldID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            *(jfieldID*)jfield = field_id;
            break;
        }
        case MP_JNI_STATIC_FIELD_AS_INT:
        case MP_JNI_STATIC_FIELD: {
            jfieldID field_id = (*env)->GetStaticFieldID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            if (type == MP_JNI_STATIC_FIELD_AS_INT) {
                if (field_id) {
                    jint value = (*env)->GetStaticIntField(env, last_clazz, field_id);
                    CHECK_EXC_MANDATORY();
                    *(jint*)jfield = value;
                }
            } else {
                *(jfieldID*)jfield = field_id;
            }
            break;
        }
        case MP_JNI_METHOD: {
            jmethodID method_id = (*env)->GetMethodID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            *(jmethodID*)jfield = method_id;
            break;
        }
        case MP_JNI_STATIC_METHOD: {
            jmethodID method_id = (*env)->GetStaticMethodID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            *(jmethodID*)jfield = method_id;
            break;
        }
        default:
            mp_err(log, "Unknown JNI field type\n");
            ret = -1;
            goto done;
        }

        ret = 0;
    }

done:
    if (ret < 0) {
        /* reset jfields in case of failure so it does not leak references */
        mp_jni_reset_jfields(env, jfields, jfields_mapping, global, log);
    }

    return ret;
}

#undef CHECK_EXC_MANDATORY

int mp_jni_reset_jfields(JNIEnv *env, void *jfields,
                         const struct MPJniField *jfields_mapping,
                         int global, struct mp_log *log)
{
    for (int i = 0; jfields_mapping[i].name; i++) {
        enum MPJniFieldType type = jfields_mapping[i].type;

        void *jfield = (uint8_t*)jfields + jfields_mapping[i].offset;

        switch (type) {
        case MP_JNI_CLASS: {
            jclass clazz = *(jclass*)jfield;
            if (!clazz)
                continue;

            if (global) {
                MP_JNI_GLOBAL_FREEP(&clazz);
            } else {
                MP_JNI_LOCAL_FREEP(&clazz);
            }

            *(jclass*)jfield = NULL;
            break;
        }
        case MP_JNI_FIELD:
        case MP_JNI_STATIC_FIELD:
            *(jfieldID*)jfield = NULL;
            break;
        case MP_JNI_STATIC_FIELD_AS_INT:
            *(jint*)jfield = 0;
            break;
        case MP_JNI_METHOD:
        case MP_JNI_STATIC_METHOD:
            *(jmethodID*)jfield = NULL;
            break;
        default:
            mp_err(log, "Unknown JNI field type\n");
        }
    }

    return 0;
}

static jobject get_app_context(JNIEnv *env)
{
    jobject ctx = av_jni_get_android_app_ctx();
    if (ctx)
        return (*env)->NewLocalRef(env, ctx);

    jclass thread = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!thread) {
        mp_jni_exception_check(env, 0, NULL);
        return NULL;
    }
    jmethodID current = (*env)->GetStaticMethodID(env, thread, "currentApplication",
                                                  "()Landroid/app/Application;");
    jobject app = current ? (*env)->CallStaticObjectMethod(env, thread, current) : NULL;
    mp_jni_exception_check(env, 0, NULL);
    (*env)->DeleteLocalRef(env, thread);
    return app;
}

static char *cache_dir(JNIEnv *env)
{
    jobject ctx = get_app_context(env);
    if (!ctx)
        return NULL;
    char *res = NULL;
    jclass ctx_class = (*env)->GetObjectClass(env, ctx);
    jclass file_class = (*env)->FindClass(env, "java/io/File");
    jmethodID get_dir = (*env)->GetMethodID(env, ctx_class, "getCacheDir", "()Ljava/io/File;");
    jmethodID get_path = file_class ? (*env)->GetMethodID(env, file_class,
        "getAbsolutePath", "()Ljava/lang/String;") : NULL;
    jobject dir = get_dir ? (*env)->CallObjectMethod(env, ctx, get_dir) : NULL;
    jstring path = dir && get_path ? (*env)->CallObjectMethod(env, dir, get_path) : NULL;
    if (mp_jni_exception_check(env, 0, NULL) >= 0 && path) {
        const char *s = (*env)->GetStringUTFChars(env, path, NULL);
        if (s) {
            res = talloc_strdup(NULL, s);
            (*env)->ReleaseStringUTFChars(env, path, s);
        }
    }
    (*env)->DeleteLocalRef(env, ctx);
    return res;
}

static bool write_ca_bundle(const char *dir, const char *out)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    char *tmp = talloc_asprintf(NULL, "%s.tmp", out);
    FILE *f = fopen(tmp, "wb");
    int count = 0;
    struct dirent *e;
    while (f && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char *name = talloc_asprintf(tmp, "%s/%s", dir, e->d_name);
        FILE *in = fopen(name, "rb");
        if (!in)
            continue;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
            fwrite(buf, 1, n, f);
        fputc('\n', f);
        fclose(in);
        count++;
    }
    closedir(d);
    bool ok = f && fclose(f) == 0 && count && rename(tmp, out) == 0;
    talloc_free(tmp);
    return ok;
}

const char *mp_jni_ca_bundle(struct mp_log *log)
{
    static mp_static_mutex ca_lock = MP_STATIC_MUTEX_INITIALIZER;
    static char *bundle;
    static bool tried;
    mp_mutex_lock(&ca_lock);
    if (!tried) {
        tried = true;
        JNIEnv *env = mp_jni_get_env(log);
        char *dir = env ? cache_dir(env) : NULL;
        if (dir) {
            char *out = talloc_asprintf(dir, "%s/mpv-cacerts.pem", dir);
            if (write_ca_bundle("/apex/com.android.conscrypt/cacerts", out) ||
                write_ca_bundle("/system/etc/security/cacerts", out))
                bundle = talloc_strdup(NULL, out);
        }
        talloc_free(dir);
        if (bundle) {
            mp_verbose(log, "Using system CA certificates from %s\n", bundle);
        } else {
            mp_warn(log, "Could not build a CA bundle from the system store.\n");
        }
    }
    mp_mutex_unlock(&ca_lock);
    return bundle;
}

int mp_jni_display_hdr_types(struct mp_log *log, float *peak)
{
    JNIEnv *env = mp_jni_get_env(log);
    if (!env)
        return 0;

    int supported = 0;
    if (peak)
        *peak = 0;
    jobject ctx = get_app_context(env);
    jobject manager = NULL, display = NULL, caps = NULL;
    jintArray types = NULL;
    jclass context_class = NULL, manager_class = NULL, display_class = NULL,
           caps_class = NULL;
    jstring service = NULL;
    if (!ctx)
        goto done;

    context_class = (*env)->FindClass(env, "android/content/Context");
    manager_class = (*env)->FindClass(env, "android/hardware/display/DisplayManager");
    display_class = (*env)->FindClass(env, "android/view/Display");
    caps_class = (*env)->FindClass(env, "android/view/Display$HdrCapabilities");
    if (!context_class || !manager_class || !display_class || !caps_class)
        goto done;

    jmethodID get_service = (*env)->GetMethodID(env, context_class, "getSystemService",
                                                "(Ljava/lang/String;)Ljava/lang/Object;");
    jmethodID get_display = (*env)->GetMethodID(env, manager_class, "getDisplay",
                                                "(I)Landroid/view/Display;");
    jmethodID is_hdr = (*env)->GetMethodID(env, display_class, "isHdr", "()Z");
    jmethodID get_caps = (*env)->GetMethodID(env, display_class, "getHdrCapabilities",
                                             "()Landroid/view/Display$HdrCapabilities;");
    jmethodID get_types = (*env)->GetMethodID(env, caps_class, "getSupportedHdrTypes", "()[I");
    jmethodID get_peak = (*env)->GetMethodID(env, caps_class, "getDesiredMaxLuminance", "()F");
    if (!get_service || !get_display || !is_hdr || !get_caps || !get_types || !get_peak)
        goto done;

    service = (*env)->NewStringUTF(env, "display");
    manager = (*env)->CallObjectMethod(env, ctx, get_service, service);
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !manager)
        goto done;
    display = (*env)->CallObjectMethod(env, manager, get_display, 0);
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !display)
        goto done;
    if (!(*env)->CallBooleanMethod(env, display, is_hdr))
        goto done;
    caps = (*env)->CallObjectMethod(env, display, get_caps);
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !caps)
        goto done;
    if (peak)
        *peak = (*env)->CallFloatMethod(env, caps, get_peak);
    types = (*env)->CallObjectMethod(env, caps, get_types);
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !types)
        goto done;

    jsize n = (*env)->GetArrayLength(env, types);
    jint *values = (*env)->GetIntArrayElements(env, types, NULL);
    for (jsize i = 0; values && i < n; i++) {
        if (values[i] > 0 && values[i] < 31)
            supported |= 1 << values[i];
    }
    if (values)
        (*env)->ReleaseIntArrayElements(env, types, values, JNI_ABORT);

done:
    mp_jni_exception_check(env, 0, NULL);
    jobject refs[] = {ctx, manager, display, caps, types, context_class,
                      manager_class, display_class, caps_class, service};
    for (int i = 0; i < MP_ARRAY_SIZE(refs); i++) {
        if (refs[i])
            (*env)->DeleteLocalRef(env, refs[i]);
    }
    return supported;
}

bool mp_jni_display_supports_dolby_vision(struct mp_log *log)
{
    return mp_jni_display_hdr_types(log, NULL) & MP_JNI_HDR_DOLBY_VISION;
}

static int max_profile_bits(JNIEnv *env, jobject caps, jfieldID levels_field,
                            jfieldID profile_field, jfieldID level_field,
                            int *dv_levels)
{
    int bits = 0;
    jobjectArray levels = (*env)->GetObjectField(env, caps, levels_field);
    if (!levels)
        return 0;
    jsize n = (*env)->GetArrayLength(env, levels);
    for (jsize i = 0; i < n; i++) {
        jobject pl = (*env)->GetObjectArrayElement(env, levels, i);
        if (!pl)
            continue;
        int profile = (*env)->GetIntField(env, pl, profile_field);
        bits |= profile;
        if (dv_levels && profile > 0 && profile < 0x800 && !(profile & (profile - 1)))
            dv_levels[__builtin_ctz(profile)] |= (*env)->GetIntField(env, pl, level_field);
        (*env)->DeleteLocalRef(env, pl);
    }
    (*env)->DeleteLocalRef(env, levels);
    return bits;
}

static void add_codec_caps(struct mp_jni_video_caps *caps, const char *type,
                           int profiles)
{
    if (!strcmp(type, "video/avc")) {
        caps->h264 = true;
    } else if (!strcmp(type, "video/hevc")) {
        caps->hevc = true;
        caps->hevc_10bit |= profiles & 0x3002;
    } else if (!strcmp(type, "video/av01")) {
        caps->av1 = true;
        caps->av1_10bit |= profiles & 0x3002;
    } else if (!strcmp(type, "video/x-vnd.on2.vp9")) {
        caps->vp9 = true;
        caps->vp9_10bit |= profiles & 0xf00c;
    } else if (!strcmp(type, "video/dolby-vision")) {
        caps->dv_profiles |= profiles & 0x7ff;
    }
}

void mp_jni_video_caps(struct mp_log *log, struct mp_jni_video_caps *caps)
{
    *caps = (struct mp_jni_video_caps){0};
    JNIEnv *env = mp_jni_get_env(log);
    if (!env || (*env)->PushLocalFrame(env, 64) < 0)
        return;

    jclass list_class = (*env)->FindClass(env, "android/media/MediaCodecList");
    jclass info_class = (*env)->FindClass(env, "android/media/MediaCodecInfo");
    jclass caps_class = (*env)->FindClass(env, "android/media/MediaCodecInfo$CodecCapabilities");
    jclass pl_class = (*env)->FindClass(env, "android/media/MediaCodecInfo$CodecProfileLevel");
    if (!list_class || !info_class || !caps_class || !pl_class)
        goto done;
    jmethodID list_init = (*env)->GetMethodID(env, list_class, "<init>", "(I)V");
    jmethodID get_infos = (*env)->GetMethodID(env, list_class, "getCodecInfos",
        "()[Landroid/media/MediaCodecInfo;");
    jmethodID is_encoder = (*env)->GetMethodID(env, info_class, "isEncoder", "()Z");
    jmethodID get_types = (*env)->GetMethodID(env, info_class, "getSupportedTypes",
        "()[Ljava/lang/String;");
    jmethodID get_caps = (*env)->GetMethodID(env, info_class, "getCapabilitiesForType",
        "(Ljava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;");
    jmethodID is_hw = android_get_device_api_level() >= 29 ?
        (*env)->GetMethodID(env, info_class, "isHardwareAccelerated", "()Z") : NULL;
    jfieldID levels_field = (*env)->GetFieldID(env, caps_class, "profileLevels",
        "[Landroid/media/MediaCodecInfo$CodecProfileLevel;");
    jfieldID profile_field = (*env)->GetFieldID(env, pl_class, "profile", "I");
    jfieldID level_field = (*env)->GetFieldID(env, pl_class, "level", "I");
    if (!list_init || !get_infos || !is_encoder || !get_types || !get_caps ||
        !levels_field || !profile_field || !level_field)
        goto done;

    jobject list = (*env)->NewObject(env, list_class, list_init, 0);
    jobjectArray infos = list ? (*env)->CallObjectMethod(env, list, get_infos) : NULL;
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !infos)
        goto done;

    jsize n = (*env)->GetArrayLength(env, infos);
    for (jsize i = 0; i < n; i++) {
        if ((*env)->PushLocalFrame(env, 32) < 0)
            break;
        jobject info = (*env)->GetObjectArrayElement(env, infos, i);
        if (!info || (*env)->CallBooleanMethod(env, info, is_encoder))
            goto next;
        if (is_hw && !(*env)->CallBooleanMethod(env, info, is_hw))
            goto next;
        jobjectArray types = (*env)->CallObjectMethod(env, info, get_types);
        if (mp_jni_exception_check(env, 0, NULL) < 0 || !types)
            goto next;
        jsize ntypes = (*env)->GetArrayLength(env, types);
        for (jsize t = 0; t < ntypes; t++) {
            jstring jtype = (*env)->GetObjectArrayElement(env, types, t);
            const char *type = jtype ? (*env)->GetStringUTFChars(env, jtype, NULL) : NULL;
            if (!type)
                continue;
            jobject c = (*env)->CallObjectMethod(env, info, get_caps, jtype);
            if (mp_jni_exception_check(env, 0, NULL) >= 0 && c) {
                bool dv = !strcmp(type, "video/dolby-vision");
                add_codec_caps(caps, type,
                               max_profile_bits(env, c, levels_field, profile_field,
                                                level_field, dv ? caps->dv_levels : NULL));
                (*env)->DeleteLocalRef(env, c);
            }
            (*env)->ReleaseStringUTFChars(env, jtype, type);
            (*env)->DeleteLocalRef(env, jtype);
        }
    next:
        mp_jni_exception_check(env, 0, NULL);
        (*env)->PopLocalFrame(env, NULL);
    }

done:
    mp_jni_exception_check(env, 0, NULL);
    (*env)->PopLocalFrame(env, NULL);
    mp_verbose(log, "Video decoder caps: h264=%d hevc=%d/%d av1=%d/%d vp9=%d/%d dv=0x%x\n",
               caps->h264, caps->hevc, caps->hevc_10bit, caps->av1, caps->av1_10bit,
               caps->vp9, caps->vp9_10bit, caps->dv_profiles);
}

enum {
    ENC_PCM_16 = 2,
    ENC_AC3 = 5,
    ENC_E_AC3 = 6,
    ENC_DTS = 7,
    ENC_DTS_HD = 8,
    ENC_IEC61937 = 13,
    ENC_TRUEHD = 14,
    ENC_E_AC3_JOC = 18,
    MASK_STEREO = 0xc,
    MASK_5_1 = 0xfc,
    MASK_7_1 = 0x18fc,
};

struct direct_probe {
    jclass track, builder, attr_builder;
    jmethodID supported, init, set_encoding, set_rate, set_mask, build;
    jobject attrs;
};

static bool init_direct_probe(JNIEnv *env, struct direct_probe *d)
{
    d->track = (*env)->FindClass(env, "android/media/AudioTrack");
    d->builder = (*env)->FindClass(env, "android/media/AudioFormat$Builder");
    d->attr_builder = (*env)->FindClass(env, "android/media/AudioAttributes$Builder");
    if (!d->track || !d->builder || !d->attr_builder)
        return false;
    d->supported = (*env)->GetStaticMethodID(env, d->track, "isDirectPlaybackSupported",
        "(Landroid/media/AudioFormat;Landroid/media/AudioAttributes;)Z");
    d->init = (*env)->GetMethodID(env, d->builder, "<init>", "()V");
    d->set_encoding = (*env)->GetMethodID(env, d->builder, "setEncoding",
        "(I)Landroid/media/AudioFormat$Builder;");
    d->set_rate = (*env)->GetMethodID(env, d->builder, "setSampleRate",
        "(I)Landroid/media/AudioFormat$Builder;");
    d->set_mask = (*env)->GetMethodID(env, d->builder, "setChannelMask",
        "(I)Landroid/media/AudioFormat$Builder;");
    d->build = (*env)->GetMethodID(env, d->builder, "build", "()Landroid/media/AudioFormat;");
    jmethodID ainit = (*env)->GetMethodID(env, d->attr_builder, "<init>", "()V");
    jmethodID usage = (*env)->GetMethodID(env, d->attr_builder, "setUsage",
        "(I)Landroid/media/AudioAttributes$Builder;");
    jmethodID abuild = (*env)->GetMethodID(env, d->attr_builder, "build",
        "()Landroid/media/AudioAttributes;");
    if (!d->supported || !d->init || !d->set_encoding || !d->set_rate ||
        !d->set_mask || !d->build || !ainit || !usage || !abuild)
        return false;
    jobject ab = (*env)->NewObject(env, d->attr_builder, ainit);
    if (!ab)
        return false;
    (*env)->CallObjectMethod(env, ab, usage, 1);
    d->attrs = (*env)->CallObjectMethod(env, ab, abuild);
    return mp_jni_exception_check(env, 0, NULL) >= 0 && d->attrs;
}

static bool probe_direct(JNIEnv *env, struct direct_probe *d, int encoding,
                         int rate, int mask)
{
    if ((*env)->PushLocalFrame(env, 8) < 0)
        return false;
    bool ok = false;
    jobject b = (*env)->NewObject(env, d->builder, d->init);
    if (b) {
        (*env)->CallObjectMethod(env, b, d->set_encoding, encoding);
        (*env)->CallObjectMethod(env, b, d->set_rate, rate);
        (*env)->CallObjectMethod(env, b, d->set_mask, mask);
        jobject fmt = (*env)->CallObjectMethod(env, b, d->build);
        if (mp_jni_exception_check(env, 0, NULL) >= 0 && fmt) {
            ok = (*env)->CallStaticBooleanMethod(env, d->track, d->supported,
                                                 fmt, d->attrs);
        }
    }
    if (mp_jni_exception_check(env, 0, NULL) < 0)
        ok = false;
    (*env)->PopLocalFrame(env, NULL);
    return ok;
}

static void hdmi_plug_intent(JNIEnv *env, struct mp_jni_audio_caps *caps)
{
    jobject ctx = get_app_context(env);
    if (!ctx)
        return;
    jclass ctx_class = (*env)->FindClass(env, "android/content/Context");
    jclass filter_class = (*env)->FindClass(env, "android/content/IntentFilter");
    jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
    if (!ctx_class || !filter_class || !intent_class)
        return;
    jmethodID filter_init = (*env)->GetMethodID(env, filter_class, "<init>",
                                                "(Ljava/lang/String;)V");
    jmethodID get_int = (*env)->GetMethodID(env, intent_class, "getIntExtra",
                                            "(Ljava/lang/String;I)I");
    jmethodID get_arr = (*env)->GetMethodID(env, intent_class, "getIntArrayExtra",
                                            "(Ljava/lang/String;)[I");
    if (!filter_init || !get_int || !get_arr)
        return;
    jobject filter = (*env)->NewObject(env, filter_class, filter_init,
        (*env)->NewStringUTF(env, "android.media.action.HDMI_AUDIO_PLUG"));
    if (!filter)
        return;
    jobject intent;
    if (android_get_device_api_level() >= 33) {
        jmethodID reg = (*env)->GetMethodID(env, ctx_class, "registerReceiver",
            "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;I)Landroid/content/Intent;");
        intent = reg ? (*env)->CallObjectMethod(env, ctx, reg, NULL, filter, 4) : NULL;
    } else {
        jmethodID reg = (*env)->GetMethodID(env, ctx_class, "registerReceiver",
            "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;");
        intent = reg ? (*env)->CallObjectMethod(env, ctx, reg, NULL, filter) : NULL;
    }
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !intent)
        return;
    jint state = (*env)->CallIntMethod(env, intent, get_int,
        (*env)->NewStringUTF(env, "android.media.extra.AUDIO_PLUG_STATE"), 0);
    if (state != 1)
        return;
    jint ch = (*env)->CallIntMethod(env, intent, get_int,
        (*env)->NewStringUTF(env, "android.media.extra.MAX_CHANNEL_COUNT"), 2);
    caps->max_channels = MPMAX(caps->max_channels, ch);
    jintArray enc = (*env)->CallObjectMethod(env, intent, get_arr,
        (*env)->NewStringUTF(env, "android.media.extra.ENCODINGS"));
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !enc)
        return;
    jsize n = (*env)->GetArrayLength(env, enc);
    jint *v = (*env)->GetIntArrayElements(env, enc, NULL);
    for (jsize i = 0; v && i < n; i++) {
        caps->ac3 |= v[i] == ENC_AC3;
        caps->eac3 |= v[i] == ENC_E_AC3 || v[i] == ENC_E_AC3_JOC;
        caps->dts |= v[i] == ENC_DTS;
        caps->dtshd |= v[i] == ENC_DTS_HD;
        caps->truehd |= v[i] == ENC_TRUEHD;
    }
    if (v)
        (*env)->ReleaseIntArrayElements(env, enc, v, JNI_ABORT);
}

static bool spatializer_active(JNIEnv *env, struct direct_probe *d)
{
    jobject ctx = get_app_context(env);
    jclass ctx_class = (*env)->FindClass(env, "android/content/Context");
    jclass am_class = (*env)->FindClass(env, "android/media/AudioManager");
    jclass sp_class = (*env)->FindClass(env, "android/media/Spatializer");
    if (!ctx || !ctx_class || !am_class || !sp_class)
        return false;
    jmethodID get_service = (*env)->GetMethodID(env, ctx_class, "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;");
    jmethodID get_sp = (*env)->GetMethodID(env, am_class, "getSpatializer",
        "()Landroid/media/Spatializer;");
    jmethodID available = (*env)->GetMethodID(env, sp_class, "isAvailable", "()Z");
    jmethodID enabled = (*env)->GetMethodID(env, sp_class, "isEnabled", "()Z");
    jmethodID can = (*env)->GetMethodID(env, sp_class, "canBeSpatialized",
        "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;)Z");
    if (!get_service || !get_sp || !available || !enabled || !can)
        return false;
    jobject am = (*env)->CallObjectMethod(env, ctx, get_service,
                                          (*env)->NewStringUTF(env, "audio"));
    jobject sp = am ? (*env)->CallObjectMethod(env, am, get_sp) : NULL;
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !sp)
        return false;
    if (!(*env)->CallBooleanMethod(env, sp, available) ||
        !(*env)->CallBooleanMethod(env, sp, enabled))
        return false;
    jobject b = (*env)->NewObject(env, d->builder, d->init);
    if (!b)
        return false;
    (*env)->CallObjectMethod(env, b, d->set_encoding, ENC_PCM_16);
    (*env)->CallObjectMethod(env, b, d->set_rate, 48000);
    (*env)->CallObjectMethod(env, b, d->set_mask, MASK_5_1);
    jobject fmt = (*env)->CallObjectMethod(env, b, d->build);
    if (mp_jni_exception_check(env, 0, NULL) < 0 || !fmt)
        return false;
    bool ok = (*env)->CallBooleanMethod(env, sp, can, d->attrs, fmt);
    return mp_jni_exception_check(env, 0, NULL) >= 0 && ok;
}

void mp_jni_audio_caps(struct mp_log *log, struct mp_jni_audio_caps *caps)
{
    *caps = (struct mp_jni_audio_caps){.max_channels = 2};
    JNIEnv *env = mp_jni_get_env(log);
    if (!env || (*env)->PushLocalFrame(env, 64) < 0)
        return;

    hdmi_plug_intent(env, caps);
    mp_jni_exception_check(env, 0, NULL);

    struct direct_probe d = {0};
    if (android_get_device_api_level() >= 29 && init_direct_probe(env, &d)) {
        caps->ac3 |= probe_direct(env, &d, ENC_AC3, 48000, MASK_STEREO);
        caps->eac3 |= probe_direct(env, &d, ENC_E_AC3, 48000, MASK_STEREO);
        caps->dts |= probe_direct(env, &d, ENC_DTS, 48000, MASK_STEREO);
        caps->dtshd |= probe_direct(env, &d, ENC_DTS_HD, 48000, MASK_STEREO);
        caps->truehd |= probe_direct(env, &d, ENC_TRUEHD, 48000, MASK_STEREO);
        if (!probe_direct(env, &d, ENC_IEC61937, 192000, MASK_7_1))
            caps->dtshd = caps->truehd = false;
        if (!probe_direct(env, &d, ENC_IEC61937, 192000, MASK_STEREO))
            caps->eac3 = false;
        if (probe_direct(env, &d, ENC_PCM_16, 48000, MASK_7_1))
            caps->max_channels = 8;
        if (android_get_device_api_level() >= 32)
            caps->spatial = spatializer_active(env, &d);
    }
    mp_jni_exception_check(env, 0, NULL);
    (*env)->PopLocalFrame(env, NULL);

    mp_verbose(log, "Audio output caps: ac3=%d eac3=%d dts=%d dts-hd=%d truehd=%d channels=%d spatial=%d\n",
               caps->ac3, caps->eac3, caps->dts, caps->dtshd, caps->truehd,
               caps->max_channels, caps->spatial);
}
