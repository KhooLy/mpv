#pragma once

#include <stdbool.h>
#include <stdint.h>

struct vo;
struct mp_image;
struct android_fel;

struct android_fel *android_fel_create(struct vo *vo);
void android_fel_destroy(struct android_fel *f);
void android_fel_select(struct android_fel *f, bool on);
bool android_fel_active(struct android_fel *f);
void android_fel_render(struct android_fel *f, struct mp_image *img,
                        int64_t present_ns, bool drop);
