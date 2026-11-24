/* blurref.h - see blurref.c. Ren'Py's blur as the oracle, host-only. */
#ifndef MR_BLURREF_H
#define MR_BLURREF_H

/* Stands in for SDL_Surface: 32bpp, `pitch` in bytes. */
typedef struct refsurf {
    int   w, h, pitch;
    void *pixels;
} refsurf;

void ref_blur_filters(float sigma, int n, int *wl, int *wu, int *m);
void ref_linblur32(const refsurf *src, refsurf *dst, int radius, int vertical);
void ref_blur32(const refsurf *src, refsurf *wrk, refsurf *dst,
                float xrad, float yrad);

#endif /* MR_BLURREF_H */
