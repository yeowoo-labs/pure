#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned char component(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    unsigned long shift = __builtin_ctzl(mask);
    unsigned long bits = mask >> shift;
    return (unsigned char)(((pixel & mask) >> shift) * 255 / bits);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 2;
    int screen = DefaultScreen(d), width = DisplayWidth(d, screen), height = DisplayHeight(d, screen);
    XImage *image = XGetImage(d, RootWindow(d, screen), 0, 0, width, height, AllPlanes, ZPixmap);
    FILE *file = fopen(argv[1], "wb");
    if (!image || !file) return 2;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) return 2;
    png_init_io(png, file);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    unsigned char *row = malloc((size_t)width * 3);
    if (!row) return 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned long pixel = XGetPixel(image, x, y);
            row[x * 3] = component(pixel, image->red_mask);
            row[x * 3 + 1] = component(pixel, image->green_mask);
            row[x * 3 + 2] = component(pixel, image->blue_mask);
        }
        png_write_row(png, row);
    }
    png_write_end(png, info);
    free(row); png_destroy_write_struct(&png, &info);
    XDestroyImage(image); fclose(file); XCloseDisplay(d);
    return 0;
}
