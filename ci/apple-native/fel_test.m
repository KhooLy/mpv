#define APPLE_FEL_TEST 1
#include "../../video/out/apple_fel.m"

#include <dispatch/dispatch.h>

#define W 64
#define H 32

#if PL_API_VER >= 367

static const double M1 = 0.1593017578125, M2 = 78.84375;
static const double C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;

static double pq_dec(double x)
{
    x = pow(fmax(x, 0), 1 / M2);
    return pow(fmax(x - C1, 0) / (C2 - C3 * x), 1 / M1);
}

static double pq_enc(double x)
{
    x = pow(fmax(x, 0), M1);
    return pow((C1 + C2 * x) / (1 + C3 * x), M2);
}

static double reshape(const struct pl_dovi_metadata *d, int c, const double sig[3])
{
    const struct pl_reshape_data *comp = &d->comp[c];
    double s = sig[c];
    if (comp->method[0] == 0)
        return (comp->poly_coeffs[0][2] * s + comp->poly_coeffs[0][1]) * s + comp->poly_coeffs[0][0];
    const float *w = comp->mmr_coeffs[0][0];
    double x[4] = {sig[0] * sig[1], sig[0] * sig[2], sig[1] * sig[2], sig[0] * sig[1] * sig[2]};
    double r = comp->mmr_constant[0];
    for (int i = 0; i < 3; i++)
        r += w[i] * sig[i];
    for (int i = 0; i < 4; i++)
        r += w[3 + i] * x[i];
    return r;
}

static void reference(const struct pl_dovi_metadata *d, const double bl[3], const double *el,
                      double out[3])
{
    double c[3], sig[3];
    for (int i = 0; i < 3; i++)
        sig[i] = c[i] = bl[i] / 1023;
    for (int i = 0; i < 3; i++)
        c[i] = fmin(fmax(reshape(d, i, sig), 0), 1);
    for (int i = 0; el && i < 3; i++) {
        double e = el[i] / 1023 - d->nlq[i].offset;
        double sgn = e > 0 ? 1 : e < 0 ? -1 : 0;
        c[i] += sgn * (fabs(e) * d->nlq[i].deadzone_slope + d->nlq[i].deadzone_threshold);
    }
    double v[3], lin[3];
    for (int i = 0; i < 3; i++) {
        double t = c[i] - d->nonlinear_offset[i] * (1024.0 / 1023.0);
        c[i] = t;
    }
    for (int i = 0; i < 3; i++) {
        v[i] = d->nonlinear.m[i][0] * c[0] + d->nonlinear.m[i][1] * c[1] + d->nonlinear.m[i][2] * c[2];
        v[i] = pq_dec(v[i]);
    }
    static const double lms2rgb[3][3] = {
        { 3.06441879, -2.16597676,  0.10155818},
        {-0.65612108,  1.78554118, -0.12943749},
        { 0.01736321, -0.04725154,  1.03004253},
    };
    double lms[3];
    for (int i = 0; i < 3; i++) {
        lms[i] = 0;
        for (int j = 0; j < 3; j++)
            lms[i] += d->linear.m[i][j] * v[j];
    }
    for (int i = 0; i < 3; i++) {
        lin[i] = 0;
        for (int j = 0; j < 3; j++)
            lin[i] += lms2rgb[i][j] * lms[j];
        out[i] = pq_enc(lin[i]) * 1023;
    }
}

static CVPixelBufferRef make_p010(int w, int h, const int code[3], bool ramp)
{
    NSDictionary *attrs = @{
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferRef pix = NULL;
    CVPixelBufferCreate(kCFAllocatorDefault, w, h,
                        kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
                        (CFDictionaryRef)attrs, &pix);
    CVPixelBufferLockBaseAddress(pix, 0);
    for (int y = 0; y < h; y++) {
        uint16_t *row = CVPixelBufferGetBaseAddressOfPlane(pix, 0) +
                        y * CVPixelBufferGetBytesPerRowOfPlane(pix, 0);
        for (int x = 0; x < w; x++)
            row[x] = (code[0] + (ramp ? y * 6 : 0)) << 6;
    }
    for (int y = 0; y < h / 2; y++) {
        uint16_t *row = CVPixelBufferGetBaseAddressOfPlane(pix, 1) +
                        y * CVPixelBufferGetBytesPerRowOfPlane(pix, 1);
        for (int x = 0; x < w / 2; x++) {
            row[2 * x] = code[1] << 6;
            row[2 * x + 1] = code[2] << 6;
        }
    }
    CVPixelBufferUnlockBaseAddress(pix, 0);
    return pix;
}

static int run_case(struct apple_fel *fel, const struct pl_dovi_metadata *d, bool use_el)
{
    const int bl_code[3] = {300, 470, 560};
    const int el_code[3] = {530, 500, 490};
    CVPixelBufferRef bl = make_p010(W, H, bl_code, true);
    CVPixelBufferRef el = use_el ? make_p010(W / 2, H / 2, el_code, false) : NULL;

    __block CVPixelBufferRef result = NULL;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    int ret = apple_fel_compose(fel, bl, el, d, ^(CVPixelBufferRef out) {
        result = (CVPixelBufferRef)CFRetain(out);
        dispatch_semaphore_signal(sem);
    });
    if (ret != APPLE_FEL_QUEUED) {
        printf("FAIL: compose returned %d\n", ret);
        return 1;
    }
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) {
        printf("FAIL: compose timed out\n");
        return 1;
    }

    int fail = 0;
    double worst = 0;
    CVPixelBufferLockBaseAddress(result, kCVPixelBufferLock_ReadOnly);
    for (int y = 2; y < H - 2; y += 3) {
        const uint32_t *row = CVPixelBufferGetBaseAddress(result) +
                              y * CVPixelBufferGetBytesPerRow(result);
        double in[3] = {bl_code[0] + y * 6, bl_code[1], bl_code[2]};
        double want[3];
        double el_in[3] = {el_code[0], el_code[1], el_code[2]};
        reference(d, in, use_el ? el_in : NULL, want);
        uint32_t px = row[W / 2];
        double got[3] = {(px >> 20) & 0x3ff, (px >> 10) & 0x3ff, px & 0x3ff};
        for (int i = 0; i < 3; i++) {
            double err = fabs(got[i] - want[i]);
            worst = fmax(worst, err);
            if (err > 8 && !fail) {
                printf("FAIL: row %d channel %d got %.0f want %.1f\n", y, i, got[i], want[i]);
                fail = 1;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(result, kCVPixelBufferLock_ReadOnly);
    printf("%s: worst error %.1f codes\n", use_el ? "with EL" : "without EL", worst);

    CFRelease(result);
    CVPixelBufferRelease(bl);
    if (el)
        CVPixelBufferRelease(el);
    return fail;
}

int main(void)
{
    struct apple_fel *fel = apple_fel_create(NULL);
    if (!fel) {
        printf("FAIL: could not create the compose pipeline\n");
        return 1;
    }

    struct pl_dovi_metadata d = {
        .nonlinear_offset = {64.0f / 1024, 512.0f / 1024, 512.0f / 1024},
        .nonlinear = {{
            {1.1689f, 0.0f, 1.7237f},
            {1.1689f, -0.1923f, -0.6678f},
            {1.1689f, 2.1990f, 0.0f},
        }},
        .linear = {{
            {0.41f, 0.52f, 0.07f},
            {0.28f, 0.62f, 0.10f},
            {0.04f, 0.16f, 0.80f},
        }},
        .nlq_active = true,
    };
    for (int c = 0; c < 3; c++) {
        d.comp[c].num_pivots = 2;
        d.comp[c].pivots[0] = 0;
        d.comp[c].pivots[1] = 1;
        d.nlq[c].offset = 0.45f + 0.025f * c;
        d.nlq[c].deadzone_slope = 0.2f - 0.02f * c;
        d.nlq[c].deadzone_threshold = 0.01f * (c + 1);
    }
    d.comp[0].poly_coeffs[0][1] = 1;
    d.comp[1].poly_coeffs[0][0] = 0.02f;
    d.comp[1].poly_coeffs[0][1] = 0.95f;
    d.comp[2].method[0] = 1;
    d.comp[2].mmr_order[0] = 1;
    d.comp[2].mmr_constant[0] = 0.1f;
    float mmr[7] = {0.5f, 0.25f, 0.1f, 0.05f, 0, 0, 0};
    memcpy(d.comp[2].mmr_coeffs[0][0], mmr, sizeof(mmr));

    int fail = run_case(fel, &d, false);
    fail |= run_case(fel, &d, true);
    apple_fel_destroy(fel);
    return fail;
}

#else

int main(void)
{
    printf("libplacebo too old for Dolby Vision composition, skipping\n");
    return 0;
}

#endif
