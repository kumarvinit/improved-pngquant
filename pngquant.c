/* pngquant.c - quantize the colors in an alphamap down to a specified number
**
** Copyright (C) 1989, 1991 by Jef Poskanzer.
** Copyright (C) 1997, 2000, 2002 by Greg Roelofs; based on an idea by
**                                Stefan Schneider.
** © 2009-2013 by Kornel Lesinski.
**
** Permission to use, copy, modify, and distribute this software and its
** documentation for any purpose and without fee is hereby granted, provided
** that the above copyright notice appear in all copies and that both that
** copyright notice and this permission notice appear in supporting
** documentation.  This software is provided "as is" without express or
** implied warranty.
*/

#define PNGQUANT_VERSION "1.8.3 (February 2013)"

#define PNGQUANT_USAGE "\
usage:  pngquant [options] [ncolors] [pngfile [pngfile ...]]\n\n\
options:\n\
  --force           overwrite existing output files (synonym: -f)\n\
  --nofs            disable Floyd-Steinberg dithering\n\
  --ext new.png     set custom suffix/extension for output filename\n\
  --speed N         speed/quality trade-off. 1=slow, 3=default, 10=fast & rough\n\
  --quality min-max don't save below min, use less colors below max (0-100)\n\
  --verbose         print status messages (synonym: -v)\n\
  --iebug           increase opacity to work around Internet Explorer 6 bug\n\
  --transbug        transparent color will be placed at the end of the palette\n\
\n\
Quantizes one or more 32-bit RGBA PNGs to 8-bit (or smaller) RGBA-palette\n\
PNGs using Floyd-Steinberg diffusion dithering (unless disabled).\n\
The output filename is the same as the input name except that\n\
it ends in \"-fs8.png\", \"-or8.png\" or your custom extension (unless the\n\
input is stdin, in which case the quantized image will go to stdout).\n\
The default behavior if the output file exists is to skip the conversion;\n\
use --force to overwrite.\n"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <getopt.h>

#if defined(WIN32) || defined(__WIN32__)
#  include <fcntl.h>    /* O_BINARY */
#  include <io.h>   /* setmode() */
#endif

#ifdef _OPENMP
#include <omp.h>
#else
#define omp_get_max_threads() 1
#define omp_get_thread_num() 0
#endif

#include "rwpng.h"  /* typedefs, common macros, public prototypes */
#include "pam.h"
#include "mediancut.h"
#include "nearest.h"
#include "blur.h"
#include "viter.h"

#include "pngquant.h"

struct liq_attr {
    void* (*malloc)(size_t);
    void (*free)(void*);

    double target_mse, max_mse;
    float min_opaque_val;
    unsigned int max_colors;
    unsigned int speed_tradeoff;
    bool last_index_transparent;
    liq_log_callback_function *log_callback;
    void *log_callback_user_info;
    liq_log_flush_callback_function *log_flush_callback;
    void *log_flush_callback_user_info;
};

struct pngquant_options {
    liq_attr *liq;
    bool floyd, using_stdin, force, ie_mode;
};

struct liq_image {
    rgb_pixel *pixels;
    rgb_pixel **rows;
    double gamma;
    int width, height;
    float *noise, *edges, *dither_map;
    bool modified, free_rows, free_pixels;
};

struct liq_result {
    colormap *palette;
    liq_palette int_palette;
    double gamma;
    double palette_error;
    float min_opaque_val, dither_level;
    bool use_dither_map, palette_only;
};

static liq_result *pngquant_quantize(histogram *hist, const liq_attr *options);
static pngquant_error prepare_output_image(liq_result *result, liq_image *input_image, png8_image *output_image);
static void set_palette(liq_result *result, liq_image *input_image, png8_image *output_image);
static void modify_alpha(liq_image *input_image, const float min_opaque_val);
static void contrast_maps(liq_image *image);
static void pngquant_output_image_free(png8_image *output_image);
static histogram *get_histogram(liq_image *input_image, liq_attr *options);
static pngquant_error read_image(const char *filename, int using_stdin, png24_image *input_image_p);
static pngquant_error write_image(png8_image *output_image, png24_image *output_image24, const char *outname, struct pngquant_options *options);
static char *add_filename_extension(const char *filename, const char *newext);
static bool file_exists(const char *outname);

static void verbose_printf(const liq_attr *context, const char *fmt, ...)
{
    if (context->log_callback) {
        va_list va;
        va_start(va, fmt);
        int required_space = vsnprintf(NULL, 0, fmt, va)+1; // +\0
        va_end(va);

        char buf[required_space];
        va_start(va, fmt);
        vsnprintf(buf, required_space, fmt, va);
        va_end(va);

        context->log_callback(context, buf, context->log_callback_user_info);
    }
}

inline static void verbose_print(const liq_attr *attr, const char *msg)
{
    if (attr->log_callback) attr->log_callback(attr, msg, attr->log_callback_user_info);
}

static void log_callback(const liq_attr *attr, const char *msg, void* user_info)
{
    fprintf(stderr, "%s\n", msg);
}

static void verbose_printf_flush(liq_attr *attr)
{
    if (attr->log_flush_callback) attr->log_flush_callback(attr, attr->log_flush_callback_user_info);
}

#ifdef _OPENMP
#define LOG_BUFFER_SIZE 1300
struct buffered_log {
    int buf_used;
    char buf[LOG_BUFFER_SIZE];
};

static void log_callback_buferred_flush(const liq_attr *attr, void *context)
{
    struct buffered_log *log = context;
    if (log->buf_used) {
        fwrite(log->buf, 1, log->buf_used, stderr);
        log->buf_used = 0;
    }
}

static void log_callback_buferred(const liq_attr *attr, const char *msg, void* context)
{
    struct buffered_log *log = context;
    int len = MIN(LOG_BUFFER_SIZE-1, strlen(msg));

    if (len > LOG_BUFFER_SIZE - log->buf_used - 2) log_callback_buferred_flush(attr, log);
    memcpy(&log->buf[log->buf_used], msg, len);
    log->buf_used += len+1;
    assert(log->buf_used < LOG_BUFFER_SIZE);
    log->buf[log->buf_used-1] = '\n';
    log->buf[log->buf_used] = '\0';
}
#endif

static void print_full_version(FILE *fd)
{
    fprintf(fd, "pngquant, %s, by Greg Roelofs, Kornel Lesinski.\n"
        #ifndef NDEBUG
                    "   DEBUG (slow) version.\n"
        #endif
        #if USE_SSE
                    "   Compiled with SSE2 instructions.\n"
        #endif
        #if _OPENMP
                    "   Compiled with OpenMP (multicore support).\n"
        #endif
        , PNGQUANT_VERSION);
    rwpng_version_info(fd);
    fputs("\n", fd);
}

static void print_usage(FILE *fd)
{
    fputs(PNGQUANT_USAGE, fd);
}

#if USE_SSE
inline static bool is_sse2_available()
{
#if (defined(__x86_64__) || defined(__amd64))
    return true;
#endif
    int a,b,c,d;
        cpuid(1, a, b, c, d);
    return d & (1<<26); // edx bit 26 is set when SSE2 is present
        }
#endif

static double quality_to_mse(long quality)
{
    if (quality == 0) return MAX_DIFF;

    // curve fudged to be roughly similar to quality of libjpeg
    return 2.5/pow(210.0 + quality, 1.2) * (100.1-quality)/100.0;
}


/**
 *   N = automatic quality, uses limit unless force is set (N-N or 0-N)
 *  -N = no better than N (same as 0-N)
 * N-M = no worse than N, no better than M
 * N-  = no worse than N, perfect if possible (same as N-100)
 *
 * where N,M are numbers between 0 (lousy) and 100 (perfect)
 */
static bool parse_quality(const char *quality, liq_attr *options)
{
    long limit, target;
    const char *str = quality; char *end;

    long t1 = strtol(str, &end, 10);
    if (str == end) return false;
    str = end;

    if ('\0' == end[0] && t1 < 0) { // quality="-%d"
        target = -t1;
        limit = 0;
    } else if ('\0' == end[0]) { // quality="%d"
        target = t1;
        limit = t1*9/10;
    } else if ('-' == end[0] && '\0' == end[1]) { // quality="%d-"
        target = 100;
        limit = t1;
    } else { // quality="%d-%d"
        long t2 = strtol(str, &end, 10);
        if (str == end || t2 > 0) return false;
        target = -t2;
        limit = t1;
    }

    return LIQ_OK == liq_set_quality(options, target, limit);
}

LIQ_EXPORT liq_error liq_set_quality(liq_attr* attr, int target, int minimum)
{
    if (target < 0 || target > 100 || target < minimum || minimum < 0) return LIQ_VALUE_OUT_OF_RANGE;
    attr->target_mse = quality_to_mse(target);
    attr->max_mse = quality_to_mse(minimum);
    return LIQ_OK;
}

LIQ_EXPORT liq_error liq_set_max_colors(liq_attr* attr, int colors)
{
    if (colors < 2 || colors > 256) return LIQ_VALUE_OUT_OF_RANGE;
    attr->max_colors = colors;
    return LIQ_OK;
}

LIQ_EXPORT liq_error liq_set_speed(liq_attr* attr, int speed) {
    if (speed < 1 || speed > 10) return LIQ_VALUE_OUT_OF_RANGE;
    attr->speed_tradeoff = speed;
    return LIQ_OK;
}

LIQ_EXPORT liq_error liq_set_output_gamma(liq_result* res, double gamma)
{
    if (gamma <= 0 || gamma >= 1.0) return LIQ_VALUE_OUT_OF_RANGE;
    res->gamma = gamma;
    return LIQ_OK;
}

LIQ_EXPORT liq_error liq_set_min_opacity(liq_attr* attr, int min) {
    if (min < 0 || min > 255) return LIQ_VALUE_OUT_OF_RANGE;
    attr->min_opaque_val = (double)min/255.0;
    return LIQ_OK;
}

LIQ_EXPORT liq_error liq_set_last_index_transparent(liq_attr* attr, int is_last)
{
    attr->last_index_transparent = !!is_last;
    return LIQ_OK;
}

LIQ_EXPORT void liq_set_log_callback(liq_attr *attr, liq_log_callback_function *callback, void* user_info)
{
    attr->log_callback = callback;
    attr->log_callback_user_info = user_info;
}

LIQ_EXPORT void liq_set_log_flush_callback(liq_attr *attr, liq_log_flush_callback_function *callback, void* user_info)
{
    attr->log_flush_callback = callback;
    attr->log_flush_callback_user_info = user_info;
}

static const struct {const char *old; char *new;} obsolete_options[] = {
    {"-fs","--floyd"},
    {"-nofs", "--ordered"},
    {"-floyd", "--floyd"},
    {"-nofloyd", "--ordered"},
    {"-ordered", "--ordered"},
    {"-force", "--force"},
    {"-noforce", "--no-force"},
    {"-verbose", "--verbose"},
    {"-quiet", "--quiet"},
    {"-noverbose", "--quiet"},
    {"-noquiet", "--verbose"},
    {"-help", "--help"},
    {"-version", "--version"},
    {"-ext", "--ext"},
    {"-speed", "--speed"},
};

static void fix_obsolete_options(const unsigned int argc, char *argv[])
{
    for(unsigned int argn=1; argn < argc; argn++) {
        if ('-' != argv[argn][0]) continue;

        if ('-' == argv[argn][1]) break; // stop on first --option or --

        for(unsigned int i=0; i < sizeof(obsolete_options)/sizeof(obsolete_options[0]); i++) {
            if (0 == strcmp(obsolete_options[i].old, argv[argn])) {
                fprintf(stderr, "  warning: option '%s' has been replaced with '%s'.\n", obsolete_options[i].old, obsolete_options[i].new);
                argv[argn] = obsolete_options[i].new;
            }
        }
    }
}

enum {arg_floyd=1, arg_ordered, arg_ext, arg_no_force, arg_iebug, arg_transbug, arg_quality};

static const struct option long_options[] = {
    {"verbose", no_argument, NULL, 'v'},
    {"quiet", no_argument, NULL, 'q'},
    {"force", no_argument, NULL, 'f'},
    {"no-force", no_argument, NULL, arg_no_force},
    {"floyd", no_argument, NULL, arg_floyd},
    {"ordered", no_argument, NULL, arg_ordered},
    {"nofs", no_argument, NULL, arg_ordered},
    {"iebug", no_argument, NULL, arg_iebug},
    {"transbug", no_argument, NULL, arg_transbug},
    {"ext", required_argument, NULL, arg_ext},
    {"speed", required_argument, NULL, 's'},
    {"quality", required_argument, NULL, arg_quality},
    {"version", no_argument, NULL, 'V'},
    {"help", no_argument, NULL, 'h'},
};

int pngquant_file(const char *filename, const char *newext, struct pngquant_options *options);


LIQ_EXPORT liq_attr* liq_attr_create()
{
    return liq_attr_create_with_allocator(malloc, free);
}

LIQ_EXPORT void liq_attr_destroy(liq_attr *attr)
{
    if (!attr) return;
    attr->free(attr);
}

LIQ_EXPORT liq_attr* liq_attr_copy(liq_attr *orig)
{
    liq_attr *attr = orig->malloc(sizeof(liq_attr));
    *attr = *orig;
    return attr;
}

LIQ_EXPORT liq_attr* liq_attr_create_with_allocator(void* (*malloc)(size_t), void (*free)(void*))
{
    liq_attr *attr = malloc(sizeof(liq_attr));
    *attr = (liq_attr) {
        .malloc = malloc,
        .free = free,
        .max_colors = 256,
        .min_opaque_val = 1, // whether preserve opaque colors for IE (1.0=no, does not affect alpha)
        .speed_tradeoff = 3, // 1 max quality, 10 rough & fast. 3 is optimum.
        .last_index_transparent = false, // puts transparent color at last index. This is workaround for blu-ray subtitles.
        .target_mse = 0,
        .max_mse = MAX_DIFF,
    };
    return attr;
}

int main(int argc, char *argv[])
{
    struct pngquant_options options = {
        .floyd = true, // floyd-steinberg dithering
    };
    options.liq = liq_attr_create();

    unsigned int error_count=0, skipped_count=0, file_count=0;
    pngquant_error latest_error=SUCCESS;
    const char *newext = NULL;

    fix_obsolete_options(argc, argv);

    int opt;
    do {
        opt = getopt_long(argc, argv, "Vvqfhs:", long_options, NULL);
        switch (opt) {
            case 'v': liq_set_log_callback(options.liq, log_callback, NULL); break;
            case 'q': liq_set_log_callback(options.liq, NULL, NULL); break;
            case arg_floyd: options.floyd = true; break;
            case arg_ordered: options.floyd = false; break;
            case 'f': options.force = true; break;
            case arg_no_force: options.force = false; break;
            case arg_ext: newext = optarg; break;

            case arg_iebug:
                // opacities above 238 will be rounded up to 255, because IE6 truncates <255 to 0.
                liq_set_min_opacity(options.liq, 238);
                options.ie_mode = true;
                break;

            case arg_transbug:
                liq_set_last_index_transparent(options.liq, true);
                break;

            case 's':
                if (LIQ_OK != liq_set_speed(options.liq, atoi(optarg))) {
                    fputs("Speed should be between 1 (slow) and 10 (fast).\n", stderr);
                    return INVALID_ARGUMENT;
                }
                break;

            case arg_quality:
                if (!parse_quality(optarg, options.liq)) {
                    fputs("Quality should be in format min-max where min and max are numbers in range 0-100.\n", stderr);
                    return INVALID_ARGUMENT;
                }
                break;

            case 'h':
                print_full_version(stdout);
                print_usage(stdout);
                return SUCCESS;

            case 'V':
                puts(PNGQUANT_VERSION);
                return SUCCESS;

            case -1: break;

            default:
                return INVALID_ARGUMENT;
        }
    } while (opt != -1);

    int argn = optind;

    if (argn >= argc) {
        if (argn > 1) {
            fputs("No input files specified. See -h for help.\n", stderr);
        } else {
            print_full_version(stderr);
            print_usage(stderr);
        }
        return MISSING_ARGUMENT;
    }

    char *colors_end;
    unsigned long colors = strtoul(argv[argn], &colors_end, 10);
    if (colors_end != argv[argn] && '\0' == colors_end[0]) {
        if (LIQ_OK != liq_set_max_colors(options.liq, colors)) {
            fputs("Number of colors must be between 2 and 256.\n", stderr);
            return INVALID_ARGUMENT;
        }
        argn++;
    }

    // new filename extension depends on options used. Typically basename-fs8.png
    if (newext == NULL) {
        newext = options.floyd ? "-ie-fs8.png" : "-ie-or8.png";
        if (!options.ie_mode) newext += 3; /* skip "-ie" */
    }

    if (argn == argc || (argn == argc-1 && 0==strcmp(argv[argn],"-"))) {
        options.using_stdin = true;
        argn = argc-1;
    }

#if USE_SSE
    if (!is_sse2_available()) {
        print_full_version(stderr);
        fputs("SSE2-capable CPU is required for this build.\n", stderr);
        return WRONG_ARCHITECTURE;
    }
#endif

    const int num_files = argc-argn;

#ifdef _OPENMP
    // if there's a lot of files, coarse parallelism can be used
    if (num_files > 2*omp_get_max_threads()) {
        omp_set_nested(0);
        omp_set_dynamic(1);
    } else {
        omp_set_nested(1);
    }
#endif

    #pragma omp parallel for \
        schedule(dynamic) reduction(+:skipped_count) reduction(+:error_count) reduction(+:file_count) shared(latest_error)
    for(int i=0; i < num_files; i++) {
        struct pngquant_options opts = options;
        opts.liq = liq_attr_copy(options.liq);

        const char *filename = opts.using_stdin ? "stdin" : argv[argn+i];

        #ifdef _OPENMP
        struct buffered_log buf = {};
        if (opts.liq->log_callback && omp_get_num_threads() > 1 && num_files > 1) {
            verbose_printf_flush(opts.liq);
            liq_set_log_callback(opts.liq, log_callback_buferred, &buf);
            liq_set_log_flush_callback(opts.liq, log_callback_buferred_flush, &buf);
        }
        #endif

        pngquant_error retval = pngquant_file(filename, newext, &opts);

        verbose_printf_flush(opts.liq);

        liq_attr_destroy(opts.liq);

        if (retval) {
            #pragma omp critical
            {
                latest_error = retval;
            }
            if (retval == TOO_LOW_QUALITY) {
                skipped_count++;
            } else {
                error_count++;
            }
        }
        ++file_count;
    }

    if (error_count) {
        verbose_printf(options.liq, "There were errors quantizing %d file%s out of a total of %d file%s.",
                       error_count, (error_count == 1)? "" : "s", file_count, (file_count == 1)? "" : "s");
    }
    if (skipped_count) {
        verbose_printf(options.liq, "Skipped %d file%s out of a total of %d file%s.",
                       skipped_count, (skipped_count == 1)? "" : "s", file_count, (file_count == 1)? "" : "s");
    }
    if (!skipped_count && !error_count) {
        verbose_printf(options.liq, "No errors detected while quantizing %d image%s.",
                       file_count, (file_count == 1)? "" : "s");
    }

    verbose_printf_flush(options.liq);

    liq_attr_destroy(options.liq);

    return latest_error;
}

LIQ_EXPORT liq_image *liq_image_create_rgba_rows(liq_attr *attr, void* rows[], int width, int height, double gamma, int ownership_flags)
{
    if (width <= 0 || height <= 0 || gamma < 0 || gamma > 1.0 || !attr || !rows) return NULL;

    liq_image *img = malloc(sizeof(liq_image));
    *img = (liq_image){
        .width = width, .height = height,
        .gamma = gamma ? gamma : 0.45455,
        .rows = (rgb_pixel **)rows,
        .free_rows = (ownership_flags & LIQ_OWN_ROWS) != 0,
        .free_pixels = (ownership_flags & LIQ_OWN_PIXELS) != 0,
    };
    if (img->free_pixels) {
        // for simplicity of this API there's no explicit bitmap argument,
        // so the row with the lowest address is assumed to be at the start of the bitmap
        img->pixels = img->rows[0];
        for(int i=1; i < img->height; i++) if (img->rows[i] < img->pixels) img->pixels = img->rows[i];
    }

    if (attr->min_opaque_val <= 254.f/255.f) {
        verbose_print(attr, "  Working around IE6 bug by making image less transparent...");
        modify_alpha(img, attr->min_opaque_val);
    }

    if (attr->speed_tradeoff < 8 && img->width >= 4 && img->height >= 4) {
        contrast_maps(img);
    }

    return img;
}

LIQ_EXPORT liq_image *liq_image_create_rgba(liq_attr *attr, void* bitmap, int width, int height, double gamma, int ownership_flags)
{
    if (width <= 0 || height <= 0 || gamma < 0 || gamma > 1.0 || !attr || !bitmap || (ownership_flags & LIQ_OWN_ROWS)) return NULL;

    rgb_pixel *pixels = bitmap;
    rgb_pixel **rows = malloc(sizeof(rows[0])*height);
    for(int i=0; i < height; i++) {
        rows[i] = pixels + width * i;
    }
    return liq_image_create_rgba_rows(attr, (void**)rows, width, height, gamma, ownership_flags | LIQ_OWN_ROWS);
}

LIQ_EXPORT void liq_image_destroy(liq_image *input_image)
{
    if (!input_image) return;

    /* now we're done with the INPUT data and row_pointers, so free 'em */
    if (input_image->free_pixels && input_image->pixels) {
        free(input_image->pixels);
        input_image->pixels = NULL;
    }

    if (input_image->free_rows && input_image->rows) {
        free(input_image->rows);
        input_image->rows = NULL;
    }

    if (input_image->noise) {
        free(input_image->noise);
        input_image->noise = NULL;
    }

    if (input_image->edges) {
        free(input_image->edges);
        input_image->edges = NULL;
    }
}

static void pngquant_output_image_free(png8_image *output_image)
{
    if (output_image->indexed_data) {
        free(output_image->indexed_data);
        output_image->indexed_data = NULL;
    }

    if (output_image->row_pointers) {
        free(output_image->row_pointers);
        output_image->row_pointers = NULL;
}
}

LIQ_EXPORT liq_result *liq_quantize_image(liq_attr *options, liq_image *input_image)
{
    histogram *hist = get_histogram(input_image, options);
    if (input_image->noise) {
        free(input_image->noise);
        input_image->noise = NULL;
    }

    liq_result *result = pngquant_quantize(hist, options);
    pam_freeacolorhist(hist);
    return result;
}

LIQ_EXPORT liq_error liq_set_dithering_level(liq_result *res, float dither_level)
{
    if (res->dither_level < 0 || res->dither_level > 1.0f) return LIQ_VALUE_OUT_OF_RANGE;
    res->dither_level = dither_level;
    return LIQ_OK;
}

LIQ_EXPORT void liq_result_destroy(liq_result *res)
{
    if (!res) return;
    pam_freecolormap(res->palette);
    free(res);
}

int pngquant_file(const char *filename, const char *newext, struct pngquant_options *options)
{
    int retval = 0;

    verbose_printf(options->liq, "%s:", filename);

    char *outname = NULL;
    if (!options->using_stdin) {
        outname = add_filename_extension(filename,newext);
        if (!options->force && file_exists(outname)) {
            fprintf(stderr, "  error:  %s exists; not overwriting\n", outname);
            retval = NOT_OVERWRITING_ERROR;
        }
    }

    liq_image *input_image = NULL;
    png24_image input_image_rwpng = {};
    if (!retval) {
        retval = read_image(filename, options->using_stdin, &input_image_rwpng);
        input_image = liq_image_create_rgba_rows(options->liq, (void**)input_image_rwpng.row_pointers,
                                                 input_image_rwpng.width, input_image_rwpng.height, input_image_rwpng.gamma, LIQ_OWN_PIXELS | LIQ_OWN_ROWS);
        if (!input_image) {
            retval = OUT_OF_MEMORY_ERROR;
        }
    }

    png8_image output_image = {};
    if (!retval) {
        verbose_printf(options->liq, "  read %luKB file corrected for gamma %2.1f",
                       (input_image_rwpng.file_size+1023UL)/1024UL, 1.0/input_image_rwpng.gamma);

        liq_result *result = liq_quantize_image(options->liq, input_image);

        if (result) {
            retval = prepare_output_image(result, input_image, &output_image);
            if (!retval) {
                liq_set_dithering_level(result, options->floyd ? 1.0 : 0);
                liq_write_remapped_image_rows(result, input_image, output_image.row_pointers);

                set_palette(result, input_image, &output_image);

                if (result->palette_error >= 0) {
                    verbose_printf(options->liq, "  mapped image to new colors...MSE=%.3f", result->palette_error*65536.0/6.0);
                }
            }
            liq_result_destroy(result);
        } else {
            retval = TOO_LOW_QUALITY;
        }
    }

    if (!retval) {
        retval = write_image(&output_image, NULL, outname, options);
    } else if (TOO_LOW_QUALITY == retval && options->using_stdin) {
        // when outputting to stdout it'd be nasty to create 0-byte file
        // so if quality is too low, output 24-bit original
        if (!options->ie_mode) {
            int write_retval = write_image(NULL, &input_image_rwpng, outname, options);
            if (write_retval) retval = write_retval;
        } else {
            // iebug preprocessing changes the original image
            fputs("  error:  can't write the original image when iebug option is enabled\n", stderr);
            retval = INVALID_ARGUMENT;
        }
    }

    liq_image_destroy(input_image);
    pngquant_output_image_free(&output_image);

    return retval;
}

static int compare_popularity(const void *ch1, const void *ch2)
{
    const float v1 = ((const colormap_item*)ch1)->popularity;
    const float v2 = ((const colormap_item*)ch2)->popularity;
    return v1 > v2 ? 1 : -1;
}

static void sort_palette(colormap *map, const liq_attr *options)
{
    /*
    ** Step 3.5 [GRR]: remap the palette colors so that all entries with
    ** the maximal alpha value (i.e., fully opaque) are at the end and can
    ** therefore be omitted from the tRNS chunk.
    */


    if (options->last_index_transparent) for(unsigned int i=0; i < map->colors; i++) {
        if (map->palette[i].acolor.a < 1.0/256.0) {
            const unsigned int old = i, transparent_dest = map->colors-1;

            const colormap_item tmp = map->palette[transparent_dest];
            map->palette[transparent_dest] = map->palette[old];
            map->palette[old] = tmp;

            /* colors sorted by popularity make pngs slightly more compressible */
            qsort(map->palette, map->colors-1, sizeof(map->palette[0]), compare_popularity);
            return;
        }
    }

    /* move transparent colors to the beginning to shrink trns chunk */
    unsigned int num_transparent=0;
    for(unsigned int i=0; i < map->colors; i++) {
        if (map->palette[i].acolor.a < 255.0/256.0) {
            // current transparent color is swapped with earlier opaque one
            if (i != num_transparent) {
                const colormap_item tmp = map->palette[num_transparent];
                map->palette[num_transparent] = map->palette[i];
                map->palette[i] = tmp;
                i--;
            }
            num_transparent++;
        }
    }

    verbose_printf(options, "  eliminated opaque tRNS-chunk entries...%d entr%s transparent", num_transparent, (num_transparent == 1)? "y" : "ies");

    /* colors sorted by popularity make pngs slightly more compressible
     * opaque and transparent are sorted separately
     */
    qsort(map->palette, num_transparent, sizeof(map->palette[0]), compare_popularity);
    qsort(map->palette+num_transparent, map->colors-num_transparent, sizeof(map->palette[0]), compare_popularity);
}

static void set_palette(liq_result *result, liq_image *input_image, png8_image *output_image)
{
    const liq_palette *palette = &result->int_palette;

    // tRNS, etc.
    output_image->num_palette = palette->count;
    output_image->num_trans = 0;
    for(unsigned int i=0; i < palette->count; i++) {
        liq_color px = palette->entries[i];
        if (px.a < 255) {
            output_image->num_trans = i+1;
        }
        output_image->palette[i] = (png_color){.red=px.r, .green=px.g, .blue=px.b};
        output_image->trans[i] = px.a;
    }
}

static void set_rounded_palette(liq_result *result)
{
    liq_palette *dest = &result->int_palette;
    colormap *const map = result->palette;

    to_f_set_gamma(result->gamma);

    dest->count = map->colors;
    for(unsigned int x = 0; x < map->colors; ++x) {
        rgb_pixel px = to_rgb(result->gamma, map->palette[x].acolor);
        map->palette[x].acolor = to_f(px); /* saves rounding error introduced by to_rgb, which makes remapping & dithering more accurate */

        dest->entries[x] = (liq_color){.r=px.r,.g=px.g,.b=px.b,.a=px.a};
    }
}

static float remap_to_palette(liq_image *input_image, unsigned char *const output_pixels[], colormap *const map, const float min_opaque_val)
{
    const rgb_pixel *const *const input_pixels = (const rgb_pixel **)input_image->rows;
    const int rows = input_image->height;
    const unsigned int cols = input_image->width;

    to_f_set_gamma(input_image->gamma);

    int remapped_pixels=0;
    float remapping_error=0;

    struct nearest_map *const n = nearest_init(map);
    const unsigned int transparent_ind = nearest_search(n, (f_pixel){0,0,0,0}, min_opaque_val, NULL);

    const unsigned int max_threads = omp_get_max_threads();
    viter_state average_color[map->colors * max_threads];
    viter_init(map, max_threads, average_color);

    #pragma omp parallel for if (rows*cols > 3000) \
        default(none) shared(average_color) reduction(+:remapping_error) reduction(+:remapped_pixels)
    for(int row = 0; row < rows; ++row) {
        for(unsigned int col = 0; col < cols; ++col) {

            f_pixel px = to_f(input_pixels[row][col]);
            unsigned int match;

            if (px.a < 1.0/256.0) {
                match = transparent_ind;
            } else {
                float diff;
                match = nearest_search(n, px, min_opaque_val, &diff);

                remapped_pixels++;
                remapping_error += diff;
            }

            output_pixels[row][col] = match;

            viter_update_color(px, 1.0, map, match, omp_get_thread_num(), average_color);
        }
    }

    viter_finalize(map, max_threads, average_color);

    nearest_free(n);

    return remapping_error / MAX(1,remapped_pixels);
}

static float distance_from_closest_other_color(const colormap *map, const unsigned int i)
{
    float second_best=MAX_DIFF;
    for(unsigned int j=0; j < map->colors; j++) {
        if (i == j) continue;
        float diff = colordifference(map->palette[i].acolor, map->palette[j].acolor);
        if (diff <= second_best) {
            second_best = diff;
        }
    }
    return second_best;
}

inline static float min_4(float a, float b, float c, float d)
{
    float x = MIN(a,b), y = MIN(c,d);
    return MIN(x,y);
}

inline static f_pixel get_dithered_pixel(const float dither_level, const float max_dither_error, const f_pixel thiserr, const f_pixel px)
{
    /* Use Floyd-Steinberg errors to adjust actual color. */
    const float sr = thiserr.r * dither_level,
                sg = thiserr.g * dither_level,
                sb = thiserr.b * dither_level,
                sa = thiserr.a * dither_level;

    float ratio = min_4((sr < 0) ? px.r/-sr : (sr > 0) ? (1.0-px.r)/sr : 1.0,
                        (sg < 0) ? px.g/-sg : (sg > 0) ? (1.0-px.g)/sg : 1.0,
                        (sb < 0) ? px.b/-sb : (sb > 0) ? (1.0-px.b)/sb : 1.0,
                        (sa < 0) ? px.a/-sa : (sa > 0) ? (1.0-px.a)/sa : 1.0);

     // If dithering error is crazy high, don't propagate it that much
     // This prevents crazy geen pixels popping out of the blue (or red or black! ;)
     const float dither_error = sr*sr + sg*sg + sb*sb + sa*sa;
     if (dither_error > max_dither_error) {
         ratio *= 0.8;
     } else if (dither_error < 2.f/256.f/256.f) {
        // don't dither areas that don't have noticeable error — makes file smaller
        return px;
     }

     if (ratio > 1.0) ratio = 1.0;
     if (ratio < 0) ratio = 0;

     return (f_pixel){
         .r=px.r + sr * ratio,
         .g=px.g + sg * ratio,
         .b=px.b + sb * ratio,
         .a=px.a + sa * ratio,
     };
}

/**
  Uses edge/noise map to apply dithering only to flat areas. Dithering on edges creates jagged lines, and noisy areas are "naturally" dithered.

  If output_image_is_remapped is true, only pixels noticeably changed by error diffusion will be written to output image.
 */
static void remap_to_palette_floyd(liq_image *input_image, unsigned char *const output_pixels[], const colormap *map, const float min_opaque_val, bool use_dither_map, bool output_image_is_remapped, const float max_dither_error)
{
    const rgb_pixel *const *const input_pixels = (const rgb_pixel *const *const)input_image->rows;
    const unsigned int rows = input_image->height, cols = input_image->width;
    const float *dither_map = use_dither_map ? (input_image->dither_map ? input_image->dither_map : input_image->edges) : NULL;

    to_f_set_gamma(input_image->gamma);

    const colormap_item *acolormap = map->palette;

    struct nearest_map *const n = nearest_init(map);
    const unsigned int transparent_ind = nearest_search(n, (f_pixel){0,0,0,0}, min_opaque_val, NULL);

    float difference_tolerance[map->colors];

    if (output_image_is_remapped) for(unsigned int i=0; i < map->colors; i++) {
        difference_tolerance[i] = distance_from_closest_other_color(map,i) / 4.f; // half of squared distance
    }

    /* Initialize Floyd-Steinberg error vectors. */
    f_pixel *restrict thiserr, *restrict nexterr;
    thiserr = malloc((cols + 2) * sizeof(*thiserr));
    nexterr = malloc((cols + 2) * sizeof(*thiserr));
    srand(12345); /* deterministic dithering is better for comparing results */

    for (unsigned int col = 0; col < cols + 2; ++col) {
        const double rand_max = RAND_MAX;
        thiserr[col].r = ((double)rand() - rand_max/2.0)/rand_max/255.0;
        thiserr[col].g = ((double)rand() - rand_max/2.0)/rand_max/255.0;
        thiserr[col].b = ((double)rand() - rand_max/2.0)/rand_max/255.0;
        thiserr[col].a = ((double)rand() - rand_max/2.0)/rand_max/255.0;
    }

    bool fs_direction = true;
    for (unsigned int row = 0; row < rows; ++row) {
        memset(nexterr, 0, (cols + 2) * sizeof(*nexterr));

        unsigned int col = (fs_direction) ? 0 : (cols - 1);

        do {
            float dither_level = dither_map ? dither_map[row*cols + col] : 15.f/16.f;
            const f_pixel spx = get_dithered_pixel(dither_level, max_dither_error, thiserr[col + 1], to_f(input_pixels[row][col]));

            unsigned int ind;
            if (spx.a < 1.0/256.0) {
                ind = transparent_ind;
            } else {
                unsigned int curr_ind = output_pixels[row][col];
                if (output_image_is_remapped && colordifference(map->palette[curr_ind].acolor, spx) < difference_tolerance[curr_ind]) {
                    ind = curr_ind;
                } else {
                    ind = nearest_search(n, spx, min_opaque_val, NULL);
            }
            }

            output_pixels[row][col] = ind;

            const f_pixel xp = acolormap[ind].acolor;
            f_pixel err = {
                .r = (spx.r - xp.r),
                .g = (spx.g - xp.g),
                .b = (spx.b - xp.b),
                .a = (spx.a - xp.a),
            };

            // If dithering error is crazy high, don't propagate it that much
            // This prevents crazy geen pixels popping out of the blue (or red or black! ;)
            if (err.r*err.r + err.g*err.g + err.b*err.b + err.a*err.a > max_dither_error) {
                dither_level *= 0.75;
            }

            const float colorimp = (3.0f + acolormap[ind].acolor.a)/4.0f * dither_level;
            err.r *= colorimp;
            err.g *= colorimp;
            err.b *= colorimp;
            err.a *= dither_level;

            /* Propagate Floyd-Steinberg error terms. */
            if (fs_direction) {
                thiserr[col + 2].a += (err.a * 7.0f) / 16.0f;
                thiserr[col + 2].r += (err.r * 7.0f) / 16.0f;
                thiserr[col + 2].g += (err.g * 7.0f) / 16.0f;
                thiserr[col + 2].b += (err.b * 7.0f) / 16.0f;

                nexterr[col    ].a += (err.a * 3.0f) / 16.0f;
                nexterr[col    ].r += (err.r * 3.0f) / 16.0f;
                nexterr[col    ].g += (err.g * 3.0f) / 16.0f;
                nexterr[col    ].b += (err.b * 3.0f) / 16.0f;

                nexterr[col + 1].a += (err.a * 5.0f) / 16.0f;
                nexterr[col + 1].r += (err.r * 5.0f) / 16.0f;
                nexterr[col + 1].g += (err.g * 5.0f) / 16.0f;
                nexterr[col + 1].b += (err.b * 5.0f) / 16.0f;

                nexterr[col + 2].a += (err.a       ) / 16.0f;
                nexterr[col + 2].r += (err.r       ) / 16.0f;
                nexterr[col + 2].g += (err.g       ) / 16.0f;
                nexterr[col + 2].b += (err.b       ) / 16.0f;
            } else {
                thiserr[col    ].a += (err.a * 7.0f) / 16.0f;
                thiserr[col    ].r += (err.r * 7.0f) / 16.0f;
                thiserr[col    ].g += (err.g * 7.0f) / 16.0f;
                thiserr[col    ].b += (err.b * 7.0f) / 16.0f;

                nexterr[col    ].a += (err.a       ) / 16.0f;
                nexterr[col    ].r += (err.r       ) / 16.0f;
                nexterr[col    ].g += (err.g       ) / 16.0f;
                nexterr[col    ].b += (err.b       ) / 16.0f;

                nexterr[col + 1].a += (err.a * 5.0f) / 16.0f;
                nexterr[col + 1].r += (err.r * 5.0f) / 16.0f;
                nexterr[col + 1].g += (err.g * 5.0f) / 16.0f;
                nexterr[col + 1].b += (err.b * 5.0f) / 16.0f;

                nexterr[col + 2].a += (err.a * 3.0f) / 16.0f;
                nexterr[col + 2].r += (err.r * 3.0f) / 16.0f;
                nexterr[col + 2].g += (err.g * 3.0f) / 16.0f;
                nexterr[col + 2].b += (err.b * 3.0f) / 16.0f;
            }

            // remapping is done in zig-zag
            if (fs_direction) {
                ++col;
                if (col >= cols) break;
            } else {
                if (col <= 0) break;
                --col;
            }
        }
        while(1);

        f_pixel *const temperr = thiserr;
        thiserr = nexterr;
        nexterr = temperr;
        fs_direction = !fs_direction;
    }

    free(thiserr);
    free(nexterr);
    nearest_free(n);
}

static bool file_exists(const char *outname)
{
    FILE *outfile = fopen(outname, "rb");
    if ((outfile ) != NULL) {
        fclose(outfile);
        return true;
    }
    return false;
}

/* build the output filename from the input name by inserting "-fs8" or
 * "-or8" before the ".png" extension (or by appending that plus ".png" if
 * there isn't any extension), then make sure it doesn't exist already */
static char *add_filename_extension(const char *filename, const char *newext)
{
    size_t x = strlen(filename);

    char* outname = malloc(x+4+strlen(newext)+1);

    strncpy(outname, filename, x);
    if (strncmp(outname+x-4, ".png", 4) == 0)
        strcpy(outname+x-4, newext);
    else
        strcpy(outname+x, newext);

    return outname;
}

static void set_binary_mode(FILE *fp)
{
#if defined(WIN32) || defined(__WIN32__)
    setmode(fp == stdout ? 1 : 0, O_BINARY);
#endif
}

static pngquant_error write_image(png8_image *output_image, png24_image *output_image24, const char *outname, struct pngquant_options *options)
{
    FILE *outfile;
    if (options->using_stdin) {
        set_binary_mode(stdout);
        outfile = stdout;

        if (output_image) {
            verbose_printf(options->liq, "  writing %d-color image to stdout", output_image->num_palette);
        } else {
            verbose_print(options->liq, "  writing truecolor image to stdout");
        }
    } else {

        if ((outfile = fopen(outname, "wb")) == NULL) {
            fprintf(stderr, "  error:  cannot open %s for writing\n", outname);
            return CANT_WRITE_ERROR;
        }

        const char *outfilename = strrchr(outname, '/');
        if (outfilename) outfilename++; else outfilename = outname;

        if (output_image) {
            verbose_printf(options->liq, "  writing %d-color image as %s", output_image->num_palette, outfilename);
        } else {
            verbose_printf(options->liq, "  writing truecolor image as %s", outfilename);
        }
    }

    pngquant_error retval;
    #pragma omp critical (libpng)
    {
        if (output_image) {
            retval = rwpng_write_image8(outfile, output_image);
        } else {
            retval = rwpng_write_image24(outfile, output_image24);
        }
    }

    if (retval) {
        fprintf(stderr, "  error: failed writing image to %s\n", outname);
    }

    if (!options->using_stdin)
        fclose(outfile);

    return retval;
}

/* histogram contains information how many times each color is present in the image, weighted by importance_map */
static histogram *get_histogram(liq_image *input_image, liq_attr *options)
{
    unsigned int ignorebits=0;
    const rgb_pixel **input_pixels = (const rgb_pixel **)input_image->rows;
    const unsigned int cols = input_image->width, rows = input_image->height;

   /*
    ** Step 2: attempt to make a histogram of the colors, unclustered.
    ** If at first we don't succeed, increase ignorebits to increase color
    ** coherence and try again.
    */

    if (options->speed_tradeoff > 7) ignorebits++;
    unsigned int maxcolors = (1<<17) + (1<<18)*(10-options->speed_tradeoff);

    struct acolorhash_table *acht = pam_allocacolorhash(maxcolors, rows*cols, ignorebits);
    for (; ;) {

        // histogram uses noise contrast map for importance. Color accuracy in noisy areas is not very important.
        // noise map does not include edges to avoid ruining anti-aliasing
        if (pam_computeacolorhash(acht, input_pixels, cols, rows, input_image->noise)) {
            break;
        }

        ignorebits++;
        verbose_print(options, "  too many colors! Scaling colors to improve clustering...");
        pam_freeacolorhash(acht);
        acht = pam_allocacolorhash(maxcolors, rows*cols, ignorebits);
    }

    if (input_image->noise) {
        free(input_image->noise);
        input_image->noise = NULL;
    }

    histogram *hist = pam_acolorhashtoacolorhist(acht, input_image->gamma);
    pam_freeacolorhash(acht);

    verbose_printf(options, "  made histogram...%d colors found", hist->size);
    return hist;
}

static void modify_alpha(liq_image *input_image, const float min_opaque_val)
{
    /* IE6 makes colors with even slightest transparency completely transparent,
       thus to improve situation in IE, make colors that are less than ~10% transparent
       completely opaque */

    rgb_pixel *const *const input_pixels = input_image->rows;
    const unsigned int rows = input_image->height, cols = input_image->width;
    const float gamma = input_image->gamma;
    to_f_set_gamma(gamma);

    const float almost_opaque_val = min_opaque_val * 169.f/256.f;
    const unsigned int almost_opaque_val_int = almost_opaque_val*255.f;


    for(unsigned int row = 0; row < rows; ++row) {
        for(unsigned int col = 0; col < cols; col++) {
            const rgb_pixel srcpx = input_pixels[row][col];

            /* ie bug: to avoid visible step caused by forced opaqueness, linearily raise opaqueness of almost-opaque colors */
            if (srcpx.a >= almost_opaque_val_int) {
                f_pixel px = to_f(srcpx);

                float al = almost_opaque_val + (px.a-almost_opaque_val) * (1-almost_opaque_val) / (min_opaque_val-almost_opaque_val);
                if (al > 1) al = 1;
                px.a = al;
                input_pixels[row][col].a = to_rgb(gamma, px).a;
            }
        }
    }
    input_image->modified = true;
}

static pngquant_error read_image(const char *filename, int using_stdin, png24_image *input_image_p)
{
    FILE *infile;

    if (using_stdin) {
        set_binary_mode(stdin);
        infile = stdin;
    } else if ((infile = fopen(filename, "rb")) == NULL) {
        fprintf(stderr, "  error: cannot open %s for reading\n", filename);
        return READ_ERROR;
    }

    pngquant_error retval;
    #pragma omp critical (libpng)
    {
            retval = rwpng_read_image24(infile, input_image_p);
    }

    if (!using_stdin)
        fclose(infile);

    if (retval) {
        fprintf(stderr, "  error: rwpng_read_image() error %d\n", retval);
        return retval;
    }

    return SUCCESS;
    }

/**
 Builds two maps:
    noise - approximation of areas with high-frequency noise, except straight edges. 1=flat, 0=noisy.
    edges - noise map including all edges
 */
static void contrast_maps(liq_image *image)
{
    const int cols = image->width, rows = image->height;
    rgb_pixel **apixels = image->rows;
    float *restrict noise = malloc(sizeof(float)*cols*rows);
    float *restrict tmp = malloc(sizeof(float)*cols*rows);
    float *restrict edges = malloc(sizeof(float)*cols*rows);

    to_f_set_gamma(image->gamma);

    for (unsigned int j=0; j < rows; j++) {
        f_pixel prev, curr = to_f(apixels[j][0]), next=curr;
        for (unsigned int i=0; i < cols; i++) {
            prev=curr;
            curr=next;
            next = to_f(apixels[j][MIN(cols-1,i+1)]);

            // contrast is difference between pixels neighbouring horizontally and vertically
            const float a = fabsf(prev.a+next.a - curr.a*2.f),
            r = fabsf(prev.r+next.r - curr.r*2.f),
            g = fabsf(prev.g+next.g - curr.g*2.f),
            b = fabsf(prev.b+next.b - curr.b*2.f);

            const f_pixel prevl = to_f(apixels[MIN(rows-1,j+1)][i]);
            const f_pixel nextl = to_f(apixels[j > 1 ? j-1 : 0][i]);

            const float a1 = fabsf(prevl.a+nextl.a - curr.a*2.f),
            r1 = fabsf(prevl.r+nextl.r - curr.r*2.f),
            g1 = fabsf(prevl.g+nextl.g - curr.g*2.f),
            b1 = fabsf(prevl.b+nextl.b - curr.b*2.f);

            const float horiz = MAX(MAX(a,r),MAX(g,b));
            const float vert = MAX(MAX(a1,r1),MAX(g1,b1));
            const float edge = MAX(horiz,vert);
            float z = edge - fabsf(horiz-vert)*.5f;
            z = 1.f - MAX(z,MIN(horiz,vert));
            z *= z; // noise is amplified
            z *= z;

            noise[j*cols+i] = z;
            edges[j*cols+i] = 1.f-edge;
}
    }

    // noise areas are shrunk and then expanded to remove thin edges from the map
    max3(noise, tmp, cols, rows);
    max3(tmp, noise, cols, rows);

    blur(noise, tmp, noise, cols, rows, 3);

    max3(noise, tmp, cols, rows);

    min3(tmp, noise, cols, rows);
    min3(noise, tmp, cols, rows);
    min3(tmp, noise, cols, rows);

    min3(edges, tmp, cols, rows);
    max3(tmp, edges, cols, rows);
    for(unsigned int i=0; i < cols*rows; i++) edges[i] = MIN(noise[i], edges[i]);

    free(tmp);

    image->noise = noise;
    image->edges = edges;
}

/**
 * Builds map of neighbor pixels mapped to the same palette entry
 *
 * For efficiency/simplicity it mainly looks for same consecutive pixels horizontally
 * and peeks 1 pixel above/below. Full 2d algorithm doesn't improve it significantly.
 * Correct flood fill doesn't have visually good properties.
 */
static void update_dither_map(unsigned char *const *const row_pointers, liq_image *input_image)
{
    const unsigned int width = input_image->width;
    const unsigned int height = input_image->height;
    float *const edges = input_image->edges;

    for(unsigned int row=0; row < height; row++) {
        unsigned char lastpixel = row_pointers[row][0];
        unsigned int lastcol=0;

        for(unsigned int col=1; col < width; col++) {
            const unsigned char px = row_pointers[row][col];

            if (px != lastpixel || col == width-1) {
                float neighbor_count = 2.5f + col-lastcol;

                unsigned int i=lastcol;
                while(i < col) {
                    if (row > 0) {
                        unsigned char pixelabove = row_pointers[row-1][i];
                        if (pixelabove == lastpixel) neighbor_count += 1.f;
                    }
                    if (row < height-1) {
                        unsigned char pixelbelow = row_pointers[row+1][i];
                        if (pixelbelow == lastpixel) neighbor_count += 1.f;
                    }
                    i++;
                }

                while(lastcol <= col) {
                    edges[row*width + lastcol++] *= 1.f - 2.5f/neighbor_count;
                }
                lastpixel = px;
            }
        }
    }
    input_image->dither_map = input_image->edges;
    input_image->edges = NULL;
}

static void adjust_histogram_callback(hist_item *item, float diff)
{
    item->adjusted_weight = (item->perceptual_weight+item->adjusted_weight) * (sqrtf(1.f+diff));
}

/**
 Repeats mediancut with different histogram weights to find palette with minimum error.

 feedback_loop_trials controls how long the search will take. < 0 skips the iteration.
 */
static colormap *find_best_palette(histogram *hist, int feedback_loop_trials, const liq_attr *options, double *palette_error_p)
{
    unsigned int max_colors = options->max_colors;
    const double target_mse = options->target_mse;
    colormap *acolormap = NULL;
    double least_error = MAX_DIFF;
    double target_mse_overshoot = feedback_loop_trials>0 ? 1.05 : 1.0;
    const double percent = (double)(feedback_loop_trials>0?feedback_loop_trials:1)/100.0;

    do {
        colormap *newmap = mediancut(hist, options->min_opaque_val, max_colors, target_mse * target_mse_overshoot, MAX(MAX(90.0/65536.0, target_mse), least_error)*1.2);

        if (feedback_loop_trials <= 0) {
            return newmap;
        }

        // after palette has been created, total error (MSE) is calculated to keep the best palette
        // at the same time Voronoi iteration is done to improve the palette
        // and histogram weights are adjusted based on remapping error to give more weight to poorly matched colors

        const bool first_run_of_target_mse = !acolormap && target_mse > 0;
        double total_error = viter_do_iteration(hist, newmap, options->min_opaque_val, first_run_of_target_mse ? NULL : adjust_histogram_callback);

        // goal is to increase quality or to reduce number of colors used if quality is good enough
        if (!acolormap || total_error < least_error || (total_error <= target_mse && newmap->colors < max_colors)) {
            if (acolormap) pam_freecolormap(acolormap);
            acolormap = newmap;

            if (total_error < target_mse && total_error > 0) {
                // voronoi iteration improves quality above what mediancut aims for
                // this compensates for it, making mediancut aim for worse
                target_mse_overshoot = MIN(target_mse_overshoot*1.25, target_mse/total_error);
            }

            least_error = total_error;

            // if number of colors could be reduced, try to keep it that way
            // but allow extra color as a bit of wiggle room in case quality can be improved too
            max_colors = MIN(newmap->colors+1, max_colors);

            feedback_loop_trials -= 1; // asymptotic improvement could make it go on forever
        } else {
            for(unsigned int j=0; j < hist->size; j++) {
                hist->achv[j].adjusted_weight = (hist->achv[j].perceptual_weight + hist->achv[j].adjusted_weight)/2.0;
            }

            target_mse_overshoot = 1.0;
            feedback_loop_trials -= 6;
            // if error is really bad, it's unlikely to improve, so end sooner
            if (total_error > least_error*4) feedback_loop_trials -= 3;
            pam_freecolormap(newmap);
        }

        verbose_printf(options, "  selecting colors...%d%%",100-MAX(0,(int)(feedback_loop_trials/percent)));
    }
    while(feedback_loop_trials > 0);

    *palette_error_p = least_error;
    return acolormap;
}

static liq_result *pngquant_quantize(histogram *hist, const liq_attr *options)
{
    const double max_mse = options->max_mse;
    colormap *acolormap;
    double palette_error = -1;

    // If image has few colors to begin with (and no quality degradation is required)
    // then it's possible to skip quantization entirely
    if (hist->size <= options->max_colors && options->target_mse == 0) {
        acolormap = pam_colormap(hist->size);
        for(unsigned int i=0; i < hist->size; i++) {
            acolormap->palette[i].acolor = hist->achv[i].acolor;
            acolormap->palette[i].popularity = hist->achv[i].perceptual_weight;
        }
        palette_error = 0;
    }
    else {
        acolormap = find_best_palette(hist, 56-9*options->speed_tradeoff, options, &palette_error);

        // Voronoi iteration approaches local minimum for the palette
        unsigned int iterations = MAX(8-options->speed_tradeoff,0); iterations += iterations * iterations/2;
        if (!iterations && palette_error < 0 && max_mse < MAX_DIFF) iterations = 1; // otherwise total error is never calculated and MSE limit won't work

        if (iterations) {
            verbose_print(options, "  moving colormap towards local minimum");

            const double iteration_limit = 1.0/(double)(1<<(23-options->speed_tradeoff));
            double previous_palette_error = MAX_DIFF;
            for(unsigned int i=0; i < iterations; i++) {
                palette_error = viter_do_iteration(hist, acolormap, options->min_opaque_val, NULL);

                if (fabs(previous_palette_error-palette_error) < iteration_limit) {
                    break;
                }

                if (palette_error > max_mse*1.5) { // probably hopeless
                    if (palette_error > max_mse*3.0) break; // definitely hopeless
                    iterations++;
                }

                previous_palette_error = palette_error;
            }
        }

        if (palette_error > max_mse) {
            verbose_printf(options, "  image degradation MSE=%.3f exceeded limit of %.3f", palette_error*65536.0/6.0, max_mse*65536.0/6.0);
            pam_freecolormap(acolormap);
            return NULL;
        }
    }

    sort_palette(acolormap, options);

    liq_result *result = malloc(sizeof(liq_result));
    *result = (liq_result){
        .palette = acolormap,
        .palette_error = palette_error,
        .use_dither_map = options->speed_tradeoff < 6,
        .min_opaque_val = options->min_opaque_val,
        .gamma = 0.45455, // fixed gamma ~2.2 for the web. PNG can't store exact 1/2.2
    };
    return result;
}

static pngquant_error prepare_output_image(liq_result *result, liq_image *input_image, png8_image *output_image)
{
    output_image->width = input_image->width;
    output_image->height = input_image->height;
    output_image->gamma = result->gamma;

    /*
    ** Step 3.7 [GRR]: allocate memory for the entire indexed image
    */

    output_image->indexed_data = malloc(output_image->height * output_image->width);
    output_image->row_pointers = malloc(output_image->height * sizeof(output_image->row_pointers[0]));

    if (!output_image->indexed_data || !output_image->row_pointers) {
        return OUT_OF_MEMORY_ERROR;
    }

    for(unsigned int row = 0;  row < output_image->height;  ++row) {
        output_image->row_pointers[row] = output_image->indexed_data + row*output_image->width;
    }

    // tRNS, etc.
    output_image->num_palette = result->palette->colors;
    output_image->num_trans = 0;
    for(unsigned int i=0; i < result->palette->colors; i++) {
        if (result->palette->palette[i].acolor.a < 255.0/256.0) {
            output_image->num_trans = i+1;
        }
    }

    return SUCCESS;
}

LIQ_EXPORT liq_error liq_write_remapped_image(liq_result *result, liq_image *input_image, void *buffer, size_t buffer_size)
{
    const size_t required_size = input_image->width * input_image->height;
    if (buffer_size < required_size) {
        return LIQ_BUFFER_TOO_SMALL;
    }

    unsigned char *rows[input_image->height];
    unsigned char *buffer_bytes = buffer;
    for(unsigned int i=0; i < input_image->height; i++) {
        rows[i] = &buffer_bytes[input_image->width * i];
    }
    return liq_write_remapped_image_rows(result, input_image, rows);
}

LIQ_EXPORT liq_error liq_write_remapped_image_rows(liq_result *result, liq_image *input_image, unsigned char **row_pointers)
{
    /*
     ** Step 4: map the colors in the image to their closest match in the
     ** new colormap, and write 'em out.
     */

    float remapping_error = result->palette_error;
    if (result->dither_level == 0) {
        set_rounded_palette(result);
        remapping_error = remap_to_palette(input_image, row_pointers, result->palette, result->min_opaque_val);
    } else {
        const bool generate_dither_map = result->use_dither_map && (input_image->edges && !input_image->dither_map);
        if (generate_dither_map) {
            // If dithering (with dither map) is required, this image is used to find areas that require dithering
            remapping_error = remap_to_palette(input_image, row_pointers, result->palette, result->min_opaque_val);
            update_dither_map(row_pointers, input_image);
        }

        // remapping above was the last chance to do voronoi iteration, hence the final palette is set after remapping
        set_rounded_palette(result);

        remap_to_palette_floyd(input_image, row_pointers, result->palette, result->min_opaque_val, result->use_dither_map, generate_dither_map, MAX(remapping_error*2.4, 16.f/256.f));
    }

    // remapping error from dithered image is absurd, so always non-dithered value is used
    // palette_error includes some perceptual weighting from histogram which is closer correlated with dssim
    // so that should be used when possible.
    if (result->palette_error < 0) {
        result->palette_error = remapping_error;
    }

    return LIQ_OK;
}

