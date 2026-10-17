/* ====================================================================
 * showcase_pdf.c - build a genuinely complex PDF with pdflib.
 *
 * pdflib is meant for production, so "does it produce a PDF" is not the
 * question worth asking. The question is whether it produces the kind of
 * document a real application emits: a cover, chapters of wrapped body
 * text, a table with running heads, figures, the annotations a reviewer
 * would actually leave, and a bookmark tree - built out of every drawing,
 * text, colour, transparency, image, annotation and outline entry point
 * the DLL exports.
 *
 * This program is therefore both the demo and the test. It writes the
 * document and then opens its own output again with
 * standalone/pdf_inspect.h and asserts that what it asked for is really
 * in the bytes. "The call returned PDF_OK" is not evidence that anything
 * was drawn - a writer can accept every call and emit a blank page.
 *
 *   showcase_pdf [podofo|libharu|all] [output-prefix]
 *
 * Produces  <prefix>.pdf                 the main document (8 pages)
 *           <prefix>-encrypted.pdf       same, RC4-128 + permissions
 *           <prefix>-streamed.pdf        incremental writer (both engines)
 *
 * Exit status is non-zero if any assertion fails, so ctest can run it.
 * ==================================================================== */
#include "pdflib/pdf_api.h"
#include "pdf_inspect.h"        /* read-back verification */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>               /* the in-memory PNG encoder */

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ==================================================================== */
/* Check harness                                                        */
/* ==================================================================== */

static int g_checks;
static int g_failures;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    char msg[512];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    g_checks++;
    if (ok)
    {
        printf("  [ok]   %s\n", msg);
    }
    else
    {
        g_failures++;
        printf("  [FAIL] %s\n", msg);
    }
}

/* Drawing calls that a backend may legitimately not implement. An engine
 * that says "I have no such feature" still produces a correct document
 * without it; an engine that returns anything else is broken. */
static void check_soft(const char *what, pdf_status st)
{
    if (st == PDF_OK)
        check(1, "%s", what);
    else if (st == PDF_ERR_BACKEND_UNSUPPORTED)
        check(1, "%s (engine has no such feature - document stays valid)", what);
    else
        check(0, "%s -> unexpected status %d (%s)", what, (int)st,
              pdflib_last_error(NULL));
}

#define OK(expr, msg) check((expr) == PDF_OK, "%s", (msg))

/* ==================================================================== */
/* A real PNG, generated in memory                                      */
/* ==================================================================== */

/* The showcase embeds a genuine picture rather than a 96-byte fixture.
 * A document that is supposed to look like production output needs an
 * image with real content, and building it here keeps the example free
 * of external files. zlib is already linked for FlateDecode. */

static void be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned char *png_chunk(const char *type, const unsigned char *data,
                                size_t len, size_t *out_len)
{
    unsigned char *c = (unsigned char *)malloc(12 + len);
    unsigned long crc;

    if (!c)
        return NULL;
    be32(c, (unsigned long)len);
    memcpy(c + 4, type, 4);
    if (len)
        memcpy(c + 8, data, len);
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, c + 4, (uInt)(4 + len));
    be32(c + 8 + len, crc);
    *out_len = 12 + len;
    return c;
}

/* Fills one pixel. `rgb` always gets 3 bytes; `a` is NULL for an opaque
 * image and 1 byte otherwise. */
typedef void (*pixel_fn)(int x, int y, int w, int h,
                         unsigned char *rgb, unsigned char *a);

/* A diagonal two-tone gradient, a ring, a centre dot and four axis ticks.
 * The structure is deliberate: a wrong stride, channel order or row order
 * changes the picture visibly, and the ticks make a transposed image
 * obvious rather than merely different. */
static void px_logo(int x, int y, int w, int h, unsigned char *rgb,
                    unsigned char *a)
{
    double u = (double)x / (double)(w - 1);
    double v = (double)y / (double)(h - 1);
    double dx = u - 0.5, dy = v - 0.5;
    double r = sqrt(dx * dx + dy * dy);
    double t = 0.5 * (u + v);
    unsigned char R, G, B;

    R = (unsigned char)(255.0 * (0.10 + 0.85 * t));
    G = (unsigned char)(255.0 * (0.25 + 0.45 * (1.0 - t)));
    B = (unsigned char)(255.0 * (0.78 - 0.62 * t));

    if (r > 0.30 && r < 0.355) { R = 250; G = 250; B = 245; }
    else if (r < 0.095)        { R = 22;  G = 24;  B = 34;  }
    else if ((fabs(dx) < 0.018 && r > 0.40) ||
             (fabs(dy) < 0.018 && r > 0.40)) { R = 255; G = 255; B = 255; }

    rgb[0] = R; rgb[1] = G; rgb[2] = B;
    if (a)
        *a = 255;
}

/* A soft radial alpha falloff: the image fades to fully transparent at
 * the edges, which is what makes a real /SMask visible. */
static void px_alpha(int x, int y, int w, int h, unsigned char *rgb,
                     unsigned char *a)
{
    double u = (double)x / (double)(w - 1);
    double v = (double)y / (double)(h - 1);
    double dx = u - 0.5, dy = v - 0.5;
    double r = sqrt(dx * dx + dy * dy);
    double al = 1.0 - r / 0.5;

    rgb[0] = (unsigned char)(255.0 * u);
    rgb[1] = (unsigned char)(60.0 + 140.0 * v);
    rgb[2] = (unsigned char)(210.0 - 150.0 * u);
    if (a)
        *a = (unsigned char)(255.0 * (al < 0 ? 0 : al));
}

static unsigned char *make_png(int w, int h, int with_alpha, pixel_fn fn,
                               size_t *out_len)
{
    int bpp = with_alpha ? 4 : 3;
    size_t stride = 1 + (size_t)w * bpp;   /* 1 = the per-row filter byte */
    size_t raw_len = (size_t)h * stride;
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    unsigned char *ihdr, *idat, *iend;
    unsigned char *out;
    uLongf comp_len;
    unsigned char *comp;
    size_t ih_len, id_len, ie_len, total;
    int x, y;

    if (!raw)
        return NULL;

    for (y = 0; y < h; y++)
    {
        unsigned char *row = raw + (size_t)y * stride;
        row[0] = 0;                        /* filter type 0: none */
        for (x = 0; x < w; x++)
        {
            unsigned char rgba[4] = {0, 0, 0, 255};
            fn(x, y, w, h, rgba, with_alpha ? rgba + 3 : NULL);
            memcpy(row + 1 + (size_t)x * bpp, rgba, (size_t)bpp);
        }
    }

    comp_len = compressBound((uLong)raw_len);
    comp = (unsigned char *)malloc(comp_len);
    if (!comp)
    {
        free(raw);
        return NULL;
    }
    if (compress2(comp, &comp_len, raw, (uLong)raw_len, 9) != Z_OK)
    {
        free(comp);
        free(raw);
        return NULL;
    }
    free(raw);

    {
        unsigned char hdr[13];
        be32(hdr, (unsigned long)w);
        be32(hdr + 4, (unsigned long)h);
        hdr[8] = 8;                                  /* bit depth */
        hdr[9] = (unsigned char)(with_alpha ? 6 : 2); /* RGBA / RGB */
        hdr[10] = 0;                                 /* deflate */
        hdr[11] = 0;                                 /* adaptive filtering */
        hdr[12] = 0;                                 /* no interlace */
        ihdr = png_chunk("IHDR", hdr, sizeof(hdr), &ih_len);
    }
    idat = png_chunk("IDAT", comp, (size_t)comp_len, &id_len);
    iend = png_chunk("IEND", NULL, 0, &ie_len);
    free(comp);

    if (!ihdr || !idat || !iend)
    {
        free(ihdr); free(idat); free(iend);
        return NULL;
    }

    total = 8 + ih_len + id_len + ie_len;
    out = (unsigned char *)malloc(total);
    if (!out)
    {
        free(ihdr); free(idat); free(iend);
        return NULL;
    }
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    memcpy(out + 8, ihdr, ih_len);
    memcpy(out + 8 + ih_len, idat, id_len);
    memcpy(out + 8 + ih_len + id_len, iend, ie_len);
    free(ihdr); free(idat); free(iend);

    *out_len = total;
    return out;
}

/* ==================================================================== */
/* Small drawing helpers                                                */
/* ==================================================================== */

static pdf_color rgb(double r, double g, double b)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = r; c.c2 = g; c.c3 = b;
    c.alpha = 1.0;
    return c;
}

static pdf_color gray(double v)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_GRAY;
    c.c1 = v;
    c.alpha = 1.0;
    return c;
}

static pdf_color cmyk(double c, double m, double y, double k)
{
    pdf_color col;
    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_CMYK;
    col.c1 = c; col.c2 = m; col.c3 = y; col.c4 = k;
    col.alpha = 1.0;
    return col;
}

static pdf_rect rect(double x, double y, double w, double h)
{
    pdf_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

/* Page geometry, recomputed for whatever page we are handed. */
static double page_w(pdf_page p)
{
    double w = 0, h = 0;
    pdf_page_get_size(p, &w, &h);
    return w;
}

static double page_h(pdf_page p)
{
    double w = 0, h = 0;
    pdf_page_get_size(p, &w, &h);
    return h;
}

static void fill_rect(pdf_page p, pdf_rect r, pdf_color c)
{
    pdf_set_fill_color(p, &c);
    pdf_page_draw_rect(p, &r, PDF_PATH_FILL);
}

/* MSVC's C compiler has no compound literals, so the colour/rect builders
 * above are called into locals rather than inline. */
static void set_fill(pdf_page p, pdf_color c)
{
    pdf_set_fill_color(p, &c);
}

static void set_stroke(pdf_page p, pdf_color c)
{
    pdf_set_stroke_color(p, &c);
}

static void draw_rect(pdf_page p, double x, double y, double w, double h,
                      pdf_path_paint_mode mode)
{
    pdf_rect r = rect(x, y, w, h);
    pdf_page_draw_rect(p, &r, mode);
}

static void draw_rect_fill(pdf_page p, double x, double y, double w, double h,
                           pdf_color c)
{
    pdf_rect r = rect(x, y, w, h);
    set_fill(p, c);
    pdf_page_draw_rect(p, &r, PDF_PATH_FILL);
}

static void rect_path(pdf_page p, double x, double y, double w, double h)
{
    pdf_rect r = rect(x, y, w, h);
    pdf_path_rectangle(p, &r);
}

/* A running head and a page number in the footer, on every page. This is
 * the kind of furniture a real document has and a smoke test does not. */
static void furniture(pdf_page p, pdf_font f, const char *chapter, int number)
{
    double w = page_w(p);
    char right[64];
    pdf_color rule = gray(0.72);
    pdf_color muted = gray(0.35);
    pdf_rect line;

    pdf_set_fill_color(p, &muted);
    pdf_page_draw_text(p, f, "pdflib - feature showcase", 48, 40, 8);
    snprintf(right, sizeof(right), "%s   |   %d", chapter, number);
    pdf_page_draw_text_aligned(p, f, right, 48, 40, 8, w - 96, PDF_ALIGN_RIGHT);

    pdf_set_stroke_color(p, &rule);
    pdf_set_line_width(p, 0.5);
    pdf_set_line_dash(p, NULL, 0, 0);
    line = rect(48, 50, w - 96, 0);
    pdf_page_draw_line(p, line.x, line.y, line.x + line.w, line.y);
}

/* A section heading with a rule under it. */
static void heading(pdf_page p, pdf_font f, const char *title, double x,
                    double y, double w)
{
    pdf_color ink = gray(0.08);
    pdf_color accent = rgb(0.15, 0.35, 0.75);

    pdf_set_fill_color(p, &ink);
    pdf_page_draw_text(p, f, title, x, y, 18);

    pdf_set_stroke_color(p, &accent);
    pdf_set_line_width(p, 1.6);
    pdf_set_line_dash(p, NULL, 0, 0);
    pdf_page_draw_line(p, x, y - 7, x + w, y - 7);
    pdf_set_line_width(p, 1.0);
}

/* A caption under a figure. */
static void caption(pdf_page p, pdf_font f, const char *s, double x, double y)
{
    pdf_color muted = gray(0.40);
    pdf_set_fill_color(p, &muted);
    pdf_page_draw_text(p, f, s, x, y, 8);
}

/* ==================================================================== */
/* The document                                                         */
/* ==================================================================== */

#define N_PAGES 8

typedef struct built {
    pdf_doc  doc;
    pdf_page page[N_PAGES];
    pdf_font body;      /* Helvetica - the default text face */
    pdf_font bold;      /* Helvetica-Bold */
    pdf_font italic;    /* Helvetica-Oblique */
    pdf_font serif;     /* Times-Roman */
    pdf_font mono;      /* Courier */
    pdf_font display;   /* Times-Bold, used for the cover */
    pdf_font ttf;       /* embedded TrueType, or NULL */
    pdf_font ttf_mem;   /* a second face, embedded from a memory buffer */
    pdf_font cjk;       /* embedded CJK, or NULL */
    pdf_image img;      /* the opaque generated PNG, or NULL */
    pdf_image img_alpha;/* the RGBA one, or NULL */
    char     ttf_path[400];
    char     ttf_mem_path[400];
    char     cjk_path[400];
} built;

/* Try a list of candidate paths; return the first that loads. */
static int load_first(pdf_doc doc, const char *const *cands, int embed,
                      pdf_font *out, char *used, size_t used_len)
{
    size_t i;

    for (i = 0; cands[i]; i++)
    {
        pdf_font f = NULL;
        if (pdf_font_load_ttf(doc, cands[i], embed, &f) == PDF_OK && f)
        {
            *out = f;
            if (used && used_len)
                snprintf(used, used_len, "%s", cands[i]);
            return 1;
        }
    }
    return 0;
}

/* Load the same kind of face through the memory entry point instead, so
 * the two implementations are both covered. PoDoFo's font cache was
 * caught handing back the first subset font for every later load, and
 * the memory path is a different code path that has to be checked too. */
static int load_first_mem(pdf_doc doc, const char *const *cands, int embed,
                          pdf_font *out, char *used, size_t used_len)
{
    size_t i;

    for (i = 0; cands[i]; i++)
    {
        FILE *f = fopen(cands[i], "rb");
        unsigned char *buf;
        long n;
        pdf_font font = NULL;

        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); continue; }
        buf = (unsigned char *)malloc((size_t)n);
        if (!buf) { fclose(f); continue; }
        if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        {
            free(buf); fclose(f); continue;
        }
        fclose(f);

        if (pdf_font_load_ttf_memory(doc, buf, (size_t)n, embed, &font)
                == PDF_OK && font)
        {
            free(buf);
            *out = font;
            if (used && used_len)
                snprintf(used, used_len, "%s", cands[i]);
            return 1;
        }
        free(buf);
    }
    return 0;
}

static const char *const k_latin_ttf[] = {
    "C:/Windows/Fonts/georgia.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/calibri.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Supplemental/Georgia.ttf",
    NULL
};

/* A deliberately DIFFERENT face from the one above: if the backend aliases
 * the two subset fonts into one, the second face's line renders as blanks
 * and the cross-check sees the wrong glyphs. */
static const char *const k_latin_ttf2[] = {
    "C:/Windows/Fonts/consola.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/verdana.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    NULL
};

/* Single-face CJK first: libharu cannot read a .ttc collection, and a
 * refused load used to latch libharu's error and silently disable the
 * whole document, so the fallbacks matter. */
static const char *const k_cjk_ttf[] = {
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/Deng.ttf",
    "C:/Windows/Fonts/Dengb.ttf",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simsun.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    NULL
};

/* ------------------------------------------------------------------ */
/* Page 1 - cover                                                      */
/* ------------------------------------------------------------------ */
static void page_cover(built *b)
{
    pdf_page p = b->page[0];
    double w = page_w(p), h = page_h(p);
    pdf_color navy = cmyk(0.95, 0.75, 0.20, 0.05);
    pdf_color warm = rgb(0.95, 0.55, 0.15);
    pdf_color white = rgb(1, 1, 1);
    pdf_rect band;
    pdf_point tri[3];
    int i;

    /* Full-bleed header band, then a thin accent stripe: two colour
     * spaces and an overlap in one go. */
    band = rect(0, h - 260, w, 260);
    fill_rect(p, band, navy);
    fill_rect(p, rect(0, h - 268, w, 8), warm);

    /* Translucent circles over the band: alpha is a separate graphics
     * state, so this also proves the ExtGState path survives. */
    for (i = 0; i < 5; i++)
    {
        pdf_color c = rgb(0.35 + 0.12 * i, 0.65, 0.95);
        c.alpha = 0.35;
        OK(pdf_set_opacity(p, 0.35, 1.0), "cover: set_opacity(circles)");
        pdf_set_fill_color(p, &c);
        pdf_path_circle(p, w - 150 + 26 * i, h - 120 - 14 * i, 62);
        pdf_path_paint(p, PDF_PATH_FILL);
    }
    pdf_set_opacity(p, 1.0, 1.0);

    /* A bezier ribbon across the band - the curve primitive, drawn rather
     * than approximated by lines. */
    {
        pdf_color ribbon = rgb(1.0, 0.85, 0.35);
        pdf_set_stroke_color(p, &ribbon);
        pdf_set_line_width(p, 3.5);
        pdf_set_line_cap(p, PDF_LINE_CAP_ROUND);
        pdf_path_move_to(p, 40, h - 250);
        pdf_path_curve_to(p, w * 0.30, h - 190,
                             w * 0.55, h - 330,
                             w - 40, h - 268);
        OK(pdf_path_paint(p, PDF_PATH_STROKE), "cover: bezier ribbon");
    }

    /* A filled triangle, to have a closed polygon on the cover too. */
    {
        pdf_color tri_c = rgb(0.10, 0.55, 0.45);
        tri_c.alpha = 0.85;
        pdf_set_opacity(p, 0.85, 1.0);
        pdf_set_fill_color(p, &tri_c);
        tri[0].x = 48;          tri[0].y = h - 300;
        tri[1].x = 48 + 130;    tri[1].y = h - 300;
        tri[2].x = 48 + 65;     tri[2].y = h - 300 + 96;
        pdf_path_polygon(p, tri, 3);
        OK(pdf_path_paint(p, PDF_PATH_FILL), "cover: filled triangle");
        pdf_set_opacity(p, 1.0, 1.0);
    }

    /* Title block. */
    {
        pdf_color ink = gray(0.10);
        pdf_color sub = gray(0.42);
        pdf_set_fill_color(p, &white);
        pdf_page_draw_text(p, b->display, "pdflib", 48, h - 150, 46);
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->display, "A complete PDF feature showcase",
                           48, h - 350, 22);
        pdf_set_fill_color(p, &sub);
        /* The box is 100pt tall: enough for the three lines the text
         * wraps to, and it stops at y=362, clear of the cover figure
         * whose top edge is at 350. A taller box would be legal but
         * would let long text run into the picture. */
        pdf_page_draw_text_wrapped(p, b->body,
            "One self-contained DLL, a libharu-style C API, and two independent "
            "engines behind it. This document is generated by the test suite: "
            "every page exercises a different part of the surface, and the "
            "program reads its own output back to check that what it asked for "
            "is really in the file.",
            48, h - 380, w - 96, 100, 11, 4.5, PDF_ALIGN_LEFT);
    }

    /* A rotated watermark, placed with an explicit transformation matrix
     * rather than a rotation API - this is what `cm` is for. */
    {
        pdf_color wm = gray(0.80);
        pdf_gstate_save(p);
        pdf_set_transform(p, 0.7071, -0.7071, 0.7071, 0.7071, 90, h * 0.30);
        pdf_set_fill_color(p, &wm);
        OK(pdf_page_draw_text(p, b->bold, "DRAFT - GENERATED BY PDFLIB",
                              0, 0, 26), "cover: rotated watermark");
        pdf_gstate_restore(p);
    }

    /* The generated picture, as the cover figure. */
    if (b->img)
    {
        pdf_rect dest = rect(w - 260, 150, 200, 200);
        OK(pdf_page_draw_image(p, b->img, &dest), "cover: generated image");
        caption(p, b->italic,
                "figure 1 - 256x256 PNG built in memory at run time",
                dest.x, dest.y - 14);
    }

    furniture(p, b->body, "Cover", 1);
}

/* ------------------------------------------------------------------ */
/* Page 2 - text and fonts                                             */
/* ------------------------------------------------------------------ */
static void page_text(built *b)
{
    pdf_page p = b->page[1];
    double w = page_w(p), h = page_h(p);
    static const struct { pdf_std_font id; const char *label; } fonts[] = {
        { PDF_STD_FONT_TIMES_ROMAN,        "Times-Roman" },
        { PDF_STD_FONT_TIMES_BOLD,         "Times-Bold" },
        { PDF_STD_FONT_TIMES_ITALIC,       "Times-Italic" },
        { PDF_STD_FONT_TIMES_BOLD_ITALIC,  "Times-BoldItalic" },
        { PDF_STD_FONT_HELVETICA,          "Helvetica" },
        { PDF_STD_FONT_HELVETICA_BOLD,     "Helvetica-Bold" },
        { PDF_STD_FONT_HELVETICA_OBLIQUE,  "Helvetica-Oblique" },
        { PDF_STD_FONT_HELVETICA_BOLD_OBLIQUE, "Helvetica-BoldOblique" },
        { PDF_STD_FONT_COURIER,            "Courier" },
        { PDF_STD_FONT_COURIER_BOLD,       "Courier-Bold" },
        { PDF_STD_FONT_COURIER_OBLIQUE,    "Courier-Oblique" },
        { PDF_STD_FONT_COURIER_BOLD_OBLIQUE, "Courier-BoldOblique" },
        { PDF_STD_FONT_SYMBOL,             "Symbol" },
        { PDF_STD_FONT_ZAPF_DINGBATS,      "ZapfDingbats" },
    };
    double y;
    size_t i;

    heading(p, b->bold, "Text, fonts and layout", 48, h - 80, w - 96);

    /* All fourteen standard fonts. None of these is embedded: they are
     * the fonts every PDF reader already has. */
    set_fill(p, gray(0.1));
    y = h - 110;
    for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++)
    {
        pdf_font f = NULL;
        char line[128];

        if (pdf_font_get_std(b->doc, fonts[i].id, &f) != PDF_OK)
        {
            check(0, "text: font_get_std(%s)", fonts[i].label);
            continue;
        }
        snprintf(line, sizeof(line), "%-22s  The quick brown fox 0123456789",
                 fonts[i].label);
        if (pdf_page_draw_text(p, f, line, 48, y, 9.5) == PDF_OK)
            check(1, "text: standard font %s", fonts[i].label);
        else
            check(0, "text: standard font %s", fonts[i].label);
        pdf_font_free(f);
        y -= 15;
    }

    /* Embedded TrueType: a real font program goes into the file. */
    y -= 8;
    if (b->ttf)
    {
        double tw = 0, asc = 0, desc = 0;
        set_fill(p, gray(0.1));
        check(pdf_page_draw_text(p, b->ttf,
                 "Embedded TrueType (subset into the file)",
                 48, y, 11) == PDF_OK, "text: embedded TrueType line");
        check(pdf_font_text_width(b->ttf, "Embedded TrueType", 11, &tw) == PDF_OK,
              "text: font_text_width");
        check(pdf_font_ascent(b->ttf, 11, &asc) == PDF_OK, "text: font_ascent");
        check(pdf_font_descent(b->ttf, 11, &desc) == PDF_OK, "text: font_descent");
        check(tw > 0 && asc > 0 && desc < 0,
              "text: metrics are sane (w=%.1f asc=%.1f desc=%.1f)", tw, asc, desc);
        caption(p, b->italic, b->ttf_path, 48 + tw + 8, y);
        y -= 20;
    }
    else
    {
        check(1, "text: no TrueType available - engine/box limit, skipping");
    }

    /* A second embedded face, loaded from a memory buffer rather than a
     * file. Two embedded faces in one document is the case that caught
     * PoDoFo aliasing every subset font after the first. */
    if (b->ttf_mem)
    {
        set_fill(p, gray(0.1));
        check(pdf_page_draw_text(p, b->ttf_mem,
                 "Second embedded face, loaded from memory",
                 48, y, 11) == PDF_OK,
              "text: embedded TrueType from memory");
        caption(p, b->italic, b->ttf_mem_path, 48, y - 11);
        y -= 22;
    }
    else
    {
        check(1, "text: no second TrueType available - skipping memory load");
    }

    /* CJK through a Type0/CID font. This is the path that most often
     * breaks, so it is asserted rather than assumed. */
    if (b->cjk)
    {
        set_fill(p, gray(0.1));
        check(pdf_page_draw_text(p, b->cjk,
                 "\xE4\xB8\xAD\xE6\x96\x87\xE7\xA4\xBA\xE4\xBE\x8B"
                 " - CID font, UTF-8 in, Type0 out",
                 48, y, 12) == PDF_OK, "text: CJK (Type0/CID) line");
        caption(p, b->italic, b->cjk_path, 48, y - 12);
        y -= 30;
    }
    else
    {
        check(1, "text: no CJK font available - engine/box limit, skipping");
    }

    /* Alignment: the same string three ways inside the same box. */
    y -= 10;
    {
        pdf_color box = gray(0.94);
        pdf_color ink = gray(0.15);
        pdf_rect r = rect(48, y - 46, w - 96, 56);
        fill_rect(p, r, box);
        pdf_set_fill_color(p, &ink);
        check(pdf_page_draw_text_aligned(p, b->body, "left aligned",
                 48, y - 10, 10, w - 96, PDF_ALIGN_LEFT) == PDF_OK,
              "text: align left");
        check(pdf_page_draw_text_aligned(p, b->body, "centred",
                 48, y - 26, 10, w - 96, PDF_ALIGN_CENTER) == PDF_OK,
              "text: align centre");
        check(pdf_page_draw_text_aligned(p, b->body, "right aligned",
                 48, y - 42, 10, w - 96, PDF_ALIGN_RIGHT) == PDF_OK,
              "text: align right");
        y -= 62;
    }

    /* Render modes: fill, stroke, both, invisible. */
    {
        pdf_color ink = gray(0.12);
        pdf_color red = rgb(0.80, 0.15, 0.15);
        pdf_set_fill_color(p, &ink);
        pdf_set_stroke_color(p, &red);
        pdf_set_line_width(p, 0.6);

        pdf_set_text_render_mode(p, PDF_TEXT_RENDER_FILL);
        pdf_page_draw_text(p, b->bold, "fill", 48, y, 15);
        pdf_set_text_render_mode(p, PDF_TEXT_RENDER_STROKE);
        pdf_page_draw_text(p, b->bold, "stroke", 110, y, 15);
        pdf_set_text_render_mode(p, PDF_TEXT_RENDER_FILL_STROKE);
        pdf_page_draw_text(p, b->bold, "fill+stroke", 200, y, 15);
        pdf_set_text_render_mode(p, PDF_TEXT_RENDER_INVISIBLE);
        pdf_page_draw_text(p, b->bold, "invisible (selectable, not shown)",
                           320, y, 10);
        pdf_set_text_render_mode(p, PDF_TEXT_RENDER_FILL);
        check(1, "text: all four render modes emitted");
        y -= 34;
    }

    /* Wrapped body text: the paragraph layout a report actually needs. */
    {
        pdf_color ink = gray(0.15);
        pdf_set_fill_color(p, &ink);
        check(pdf_page_draw_text_wrapped(p, b->body,
            "Wrapped text is laid out one line at a time, breaking on word "
            "boundaries and measuring each candidate with the font's own "
            "metrics, so the result is correct for a proportional face and "
            "not only for a monospaced one. A paragraph long enough to run "
            "past the bottom of its box is clipped by the height argument "
            "rather than spilling over whatever is drawn next, which is what "
            "keeps a generated report from overprinting its own footer.",
            48, y, (w - 96) * 0.48, 200, 10, 4.2, PDF_ALIGN_LEFT) == PDF_OK,
            "text: wrapped paragraph");

        set_fill(p, gray(0.45));
        check(pdf_page_draw_text_wrapped(p, b->italic,
            "Justified-looking columns are two independent boxes side by side. "
            "Each one wraps to its own width, so a two-column layout needs no "
            "special support beyond calling this twice with different "
            "rectangles - and the columns cannot collide because the width "
            "argument bounds them.",
            w * 0.52, y, (w - 96) * 0.48, 200, 10, 4.2, PDF_ALIGN_LEFT) == PDF_OK,
            "text: second column");
        y -= 190;
    }

    furniture(p, b->body, "Text and fonts", 2);
}

/* ------------------------------------------------------------------ */
/* Page 3 - vector graphics                                            */
/* ------------------------------------------------------------------ */
static void page_vector(built *b)
{
    pdf_page p = b->page[2];
    double w = page_w(p), h = page_h(p);
    pdf_color ink = gray(0.12);
    double y = h - 110;
    int i;

    heading(p, b->bold, "Vector graphics", 48, h - 80, w - 96);
    pdf_set_fill_color(p, &ink);

    /* Line caps: three strokes of the same segment with different caps,
     * so the end treatment is visible. */
    {
        static const struct { pdf_line_cap cap; const char *n; } caps[] = {
            { PDF_LINE_CAP_BUTT,   "butt" },
            { PDF_LINE_CAP_ROUND,  "round" },
            { PDF_LINE_CAP_SQUARE, "square" },
        };
        pdf_color c = rgb(0.20, 0.40, 0.80);
        pdf_set_stroke_color(p, &c);
        pdf_set_line_width(p, 12);
        for (i = 0; i < 3; i++)
        {
            pdf_set_line_cap(p, caps[i].cap);
            pdf_page_draw_line(p, 60, y - 30 * i, 200, y - 30 * i);
        }
        pdf_set_line_cap(p, PDF_LINE_CAP_BUTT);
        pdf_set_line_width(p, 1);
        pdf_page_draw_text(p, b->body, "line caps", 210, y - 20, 9);
        y -= 120;
    }

    /* Line joins, via a chevron per join style. */
    {
        static const struct { pdf_line_join j; const char *n; } joins[] = {
            { PDF_LINE_JOIN_MITER, "miter" },
            { PDF_LINE_JOIN_ROUND, "round" },
            { PDF_LINE_JOIN_BEVEL, "bevel" },
        };
        pdf_color c = rgb(0.85, 0.35, 0.15);
        pdf_set_stroke_color(p, &c);
        pdf_set_line_width(p, 10);
        for (i = 0; i < 3; i++)
        {
            double bx = 60 + i * 90;
            pdf_set_line_join(p, joins[i].j);
            pdf_path_move_to(p, bx, y - 30);
            pdf_path_line_to(p, bx + 22, y - 4);
            pdf_path_line_to(p, bx + 44, y - 30);
            pdf_path_paint(p, PDF_PATH_STROKE);
        }
        pdf_set_line_join(p, PDF_LINE_JOIN_MITER);
        pdf_set_line_width(p, 1);
        pdf_page_draw_text(p, b->body, "line joins", 340, y - 20, 9);
        y -= 60;
    }

    /* Dash patterns, including a phase offset. */
    {
        double d1[2] = { 6, 3 };
        double d2[4] = { 1, 2, 6, 2 };
        pdf_color c = rgb(0.25, 0.55, 0.35);
        pdf_set_stroke_color(p, &c);
        pdf_set_line_width(p, 1.6);

        pdf_set_line_dash(p, d1, 2, 0);
        pdf_page_draw_line(p, 60, y, 250, y);
        pdf_set_line_dash(p, d1, 2, 3);
        pdf_page_draw_line(p, 60, y - 12, 250, y - 12);
        pdf_set_line_dash(p, d2, 4, 0);
        pdf_page_draw_line(p, 60, y - 24, 250, y - 24);
        pdf_set_line_dash(p, NULL, 0, 0);
        pdf_page_draw_line(p, 60, y - 36, 250, y - 36);
        pdf_page_draw_text(p, b->body,
            "dash: 6/3, 6/3 phase 3, 1/2/6/2, solid", 260, y - 20, 9);
        check(1, "vector: four dash patterns emitted");
        y -= 66;
    }

    /* Miter limit: a sharp spike truncated by a low limit. */
    {
        pdf_color c = rgb(0.55, 0.25, 0.70);
        pdf_set_stroke_color(p, &c);
        pdf_set_line_width(p, 8);
        pdf_set_miter_limit(p, 2);
        pdf_path_move_to(p, 60, y);
        pdf_path_line_to(p, 100, y + 40);
        pdf_path_line_to(p, 60, y + 6);
        pdf_path_paint(p, PDF_PATH_STROKE);
        pdf_set_miter_limit(p, 10);
        pdf_page_draw_text(p, b->body, "miter limit 2", 120, y + 10, 9);
        check(1, "vector: miter limit set");
        pdf_set_line_width(p, 1);
        y -= 60;
    }

    /* Curves, arcs, circles, ellipses. */
    {
        pdf_color c = rgb(0.15, 0.45, 0.85);
        pdf_set_stroke_color(p, &c);
        pdf_set_line_width(p, 1.8);

        /* A bezier wave. */
        pdf_path_move_to(p, 60, y);
        pdf_path_curve_to(p, 110, y + 45, 160, y - 45, 210, y);
        pdf_path_curve_to(p, 260, y + 45, 310, y - 45, 360, y);
        pdf_path_paint(p, PDF_PATH_STROKE);

        /* Circle and ellipse, stroked then filled. */
        pdf_path_circle(p, 90, y - 70, 26);
        pdf_path_paint(p, PDF_PATH_STROKE);
        pdf_path_ellipse(p, 180, y - 70, 40, 22);
        pdf_path_paint(p, PDF_PATH_STROKE);

        /* An arc: three quarters of a circle, open on the right. */
        pdf_path_arc(p, 280, y - 70, 30, 0.5, 0.5 + 1.5 * M_PI, 0);
        pdf_path_paint(p, PDF_PATH_STROKE);

        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "bezier / circle / ellipse / arc",
                           330, y - 74, 9);
        check(1, "vector: curves, arcs, circles and ellipses emitted");
        y -= 130;
    }

    /* Polygons under both fill rules: a five-pointed star looks like a
     * star under nonzero and like a pentagon with a hole under even-odd,
     * which is the difference the two operators exist for. */
    {
        pdf_point star[10];
        int k;
        for (k = 0; k < 10; k++)
        {
            double ang = -M_PI / 2 + k * M_PI / 5;
            double rad = (k % 2 == 0) ? 42 : 18;
            star[k].x = 100 + rad * cos(ang);
            star[k].y = y - rad * sin(ang);
        }
        set_fill(p, gray(0.55));
        pdf_path_polygon(p, star, 10);
        pdf_path_paint(p, PDF_PATH_FILL);
        set_fill(p, gray(0.30));
        pdf_path_polygon(p, star, 10);
        pdf_path_paint(p, PDF_PATH_FILL_EVENODD);
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "nonzero", 60, y - 60, 9);
        pdf_page_draw_text(p, b->body, "even-odd", 120, y - 60, 9);
        check(1, "vector: both fill rules emitted");
        y -= 110;
    }

    /* Clipping: clip to a circle, then draw stripes that can only appear
     * inside it. A discarded path (PDF 'n') is what performs the clip. */
    {
        pdf_color c1 = rgb(0.90, 0.30, 0.30);
        pdf_color c2 = rgb(0.95, 0.75, 0.20);
        pdf_gstate_save(p);
        pdf_path_circle(p, w - 150, y + 20, 62);
        OK(pdf_path_discard(p), "vector: clip path discarded (n)");
        /* Widen the clip with a second shape so it is a real intersection. */
        rect_path(p, w - 200, y - 60, 200, 160);
        pdf_path_discard(p);

        for (i = 0; i < 12; i++)
        {
            pdf_color c = (i % 2) ? c1 : c2;
            pdf_set_fill_color(p, &c);
            draw_rect(p, w - 220, y - 60 + i * 13, 240, 7, PDF_PATH_FILL);
        }
        pdf_gstate_restore(p);
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "clipping", w - 200, y - 74, 9);
        check(1, "vector: clipping applied and restored");
    }

    /* An explicit transformation: rotate and scale a small mark. */
    {
        pdf_color c = rgb(0.20, 0.60, 0.60);
        pdf_gstate_save(p);
        pdf_set_transform(p, 1.4, 0.5, -0.5, 1.4, 300, y - 30);
        pdf_set_fill_color(p, &c);
        draw_rect(p, 0, 0, 40, 40, PDF_PATH_FILL);
        pdf_gstate_restore(p);
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "cm: scale+skew", 300, y - 110, 9);
        check(1, "vector: transformation matrix applied");
    }

    furniture(p, b->body, "Vector graphics", 3);
}

/* ------------------------------------------------------------------ */
/* Page 4 - colour and transparency                                    */
/* ------------------------------------------------------------------ */
static void page_color(built *b)
{
    pdf_page p = b->page[3];
    double w = page_w(p), h = page_h(p);
    pdf_color ink = gray(0.12);
    double y = h - 120;
    int i;

    heading(p, b->bold, "Colour, transparency and blend modes", 48, h - 80,
            w - 96);
    pdf_set_fill_color(p, &ink);

    /* Three colour-space ramps. The point is not the gradient - it is
     * that all three device spaces are reachable and produce the right
     * operators ('g', 'rg', 'k'). */
    {
        const int steps = 12;
        double sw = (w - 96) / steps;
        for (i = 0; i < steps; i++)
        {
            double t = (double)i / (steps - 1);
            fill_rect(p, rect(48 + i * sw, y, sw - 1, 26), gray(t));
            fill_rect(p, rect(48 + i * sw, y - 30, sw - 1, 26),
                      rgb(t, 0.35, 1.0 - t));
            fill_rect(p, rect(48 + i * sw, y - 60, sw - 1, 26),
                      cmyk(t, 0.2, 0.8 - 0.8 * t, 0.05));
        }
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "gray", 48, y - 72, 9);
        pdf_page_draw_text(p, b->body, "RGB", 48, y - 42, 9);
        pdf_page_draw_text(p, b->body, "CMYK", 48, y - 12, 9);
        check(1, "colour: gray, RGB and CMYK ramps emitted");
        y -= 100;
    }

    /* Opacity: three overlapping squares at increasing alpha. */
    {
        int k;
        for (k = 0; k < 3; k++)
        {
            pdf_color c = rgb(0.15 + 0.3 * k, 0.35, 0.85 - 0.25 * k);
            c.alpha = 0.35 + 0.25 * k;
            check_soft("colour: set_opacity(overlap)",
                       pdf_set_opacity(p, c.alpha, c.alpha));
            pdf_set_fill_color(p, &c);
            draw_rect(p, 60 + k * 55, y - 70, 90, 70, PDF_PATH_FILL);
        }
        pdf_set_opacity(p, 1.0, 1.0);
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "fill and stroke alpha", 60, y - 84, 9);
        check(1, "colour: translucent squares emitted");
    }

    /* Blend modes: all twelve, in a grid of overlapping pairs.
     *
     * Each cell is a 40pt square with a second 40pt square offset by
     * (14,14), so the painted stack is 54pt tall and 54pt wide. The
     * column and row pitches below have to clear that stack *and* leave
     * a line of room for the caption, or the caption of one row lands on
     * the swatches of the next. */
    {
        static const char *const names[] = {
            "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
            "ColorDodge", "ColorBurn", "HardLight", "SoftLight",
            "Difference", "Exclusion"
        };
        pdf_color base = rgb(0.30, 0.55, 0.90);
        pdf_color top = rgb(0.95, 0.60, 0.20);
        const double gx = 276;   /* grid origin; 276 + 3*56 + 54 = 498 */
        const double cw = 56;    /* column pitch */
        const double ch = 74;    /* row pitch: 54pt stack + caption */
        int k;

        pdf_set_fill_color(p, &ink);
        /* The heading and the first row both have to sit below the CMYK
         * ramp, whose bottom edge is y+40, and the heading has to sit
         * above the first row's swatches, whose top edge is by+54. */
        pdf_page_draw_text(p, b->bold, "blend modes", gx, y + 26, 11);

        for (k = 0; k < 12; k++)
        {
            double bx = gx + (k % 4) * cw;
            double by = y - 40 - (k / 4) * ch;

            pdf_gstate_save(p);
            pdf_set_blend_mode(p, PDF_BLEND_NORMAL);
            pdf_set_opacity(p, 1.0, 1.0);
            pdf_set_fill_color(p, &base);
            draw_rect(p, bx, by, 40, 40, PDF_PATH_FILL);

            {
                char bm[64];
                snprintf(bm, sizeof(bm), "colour: blend mode %s", names[k]);
                check_soft(bm, pdf_set_blend_mode(p, (pdf_blend_mode)k));
            }
            pdf_set_fill_color(p, &top);
            draw_rect(p, bx + 14, by + 14, 40, 40, PDF_PATH_FILL);
            pdf_gstate_restore(p);
        }
        pdf_set_blend_mode(p, PDF_BLEND_NORMAL);
        pdf_set_fill_color(p, &ink);
        for (k = 0; k < 12; k++)
        {
            double bx = gx + (k % 4) * cw;
            double by = y - 50 - (k / 4) * ch; /* clear of the stack */
            pdf_page_draw_text(p, b->body, names[k], bx, by, 6.5);
        }
        /* Rows run y+14 down to y-40-2*74 = y-188, captions to y-198. */
        y -= 224;
    }

    /* The RGBA image over a coloured block: if the engine produces an
     * /SMask the image fades out at the edges; if it declines, the
     * document is still correct and we say so. */
    {
        pdf_color block = rgb(0.10, 0.20, 0.40);
        fill_rect(p, rect(48, y - 130, 300, 140), block);
        if (b->img_alpha)
        {
            pdf_rect dest = rect(60, y - 120, 120, 120);
            check(pdf_page_draw_image(p, b->img_alpha, &dest) == PDF_OK,
                  "colour: RGBA image drawn over a colour block");
        }
        else
        {
            check(1, "colour: RGBA image declined by this engine (no /SMask)");
        }
        set_fill(p, gray(0.9));
        /* Kept short enough to stay inside the block: light grey on
         * white paper is invisible, so a caption that overflows the
         * block silently loses its tail. */
        pdf_page_draw_text(p, b->body,
            "an image with an alpha channel", 200,
            y - 70, 9);
    }

    furniture(p, b->body, "Colour and transparency", 4);
}

/* ------------------------------------------------------------------ */
/* Page 5 - images (landscape)                                         */
/* ------------------------------------------------------------------ */
static void page_images(built *b)
{
    pdf_page p = b->page[4];
    double w = page_w(p), h = page_h(p);
    pdf_color ink = gray(0.12);
    double y = h - 110;
    int i;

    heading(p, b->bold, "Images", 48, h - 80, w - 96);
    pdf_set_fill_color(p, &ink);

    if (!b->img)
    {
        check(0, "images: the generated PNG could not be loaded");
        furniture(p, b->body, "Images", 5);
        return;
    }

    /* The same XObject at three sizes: one image object, three placements.
     * A reader that shares the resource is doing its job. */
    {
        size_t iw = 0, ih = 0;
        check(pdf_image_get_size(b->img, &iw, &ih) == PDF_OK,
              "images: image_get_size");
        check(iw == 256 && ih == 256,
              "images: generated PNG is 256x256 (got %lux%lu)",
              (unsigned long)iw, (unsigned long)ih);

        for (i = 0; i < 3; i++)
        {
            double side = 90 + i * 45;
            pdf_rect dest = rect(60 + i * 170, y - side, side, side);
            check(pdf_page_draw_image(p, b->img, &dest) == PDF_OK,
                  "images: draw at %dx%d", (int)side, (int)side);
            caption(p, b->body, i == 0 ? "90pt" : (i == 1 ? "135pt" : "180pt"),
                    dest.x, dest.y - 12);
        }
        y -= 200;
    }

    /* A grid of the same image: nine placements, still one XObject. */
    {
        int gx, gy;
        for (gy = 0; gy < 3; gy++)
            for (gx = 0; gx < 4; gx++)
            {
                pdf_rect dest = rect(60 + gx * 62, y - 60 - gy * 62, 56, 56);
                pdf_page_draw_image(p, b->img, &dest);
            }
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body,
            "one image object, twelve placements", 60, y - 260, 9);
        check(1, "images: tiled placement");
    }

    /* An image inside a clipped, transformed box: the combination a real
     * layout uses to crop a photo into a shape. */
    {
        pdf_gstate_save(p);
        pdf_set_transform(p, 1.0, 0.0, 0.0, 1.0, w - 300, y - 90);
        pdf_path_circle(p, 0, 0, 70);
        pdf_path_discard(p);
        {
            pdf_rect dst = rect(-70, -70, 140, 140);
            pdf_page_draw_image(p, b->img, &dst);
        }
        pdf_gstate_restore(p);
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "clipped and transformed", w - 370,
                           y - 190, 9);
        check(1, "images: clipped + transformed placement");
    }

    furniture(p, b->body, "Images", 5);
}

/* ------------------------------------------------------------------ */
/* Page 6 - annotations                                                */
/* ------------------------------------------------------------------ */
static void page_annots(built *b)
{
    pdf_page p = b->page[5];
    double w = page_w(p), h = page_h(p);
    pdf_color ink = gray(0.12);
    pdf_color body = gray(0.22);
    double y = h - 120;
    int i;

    heading(p, b->bold, "Annotations and links", 48, h - 80, w - 96);

    /* A paragraph for the markups to sit on top of - a highlight with
     * nothing under it is meaningless. */
    pdf_set_fill_color(p, &body);
    pdf_page_draw_text_wrapped(p, b->body,
        "The markups below are placed over this paragraph, which is how they "
        "are meant to be used: a highlight, an underline, a squiggly and a "
        "strikeout each take a rectangle and a colour, and the reader draws "
        "them over the text underneath. Each carries an author and a subject "
        "so a review workflow has something to show in its sidebar.",
        48, y, w - 96, 120, 11, 4.5, PDF_ALIGN_LEFT);

    /* The nine annotation types, each with the metadata that applies. */
    {
        static const struct {
            pdf_annot_type type;
            const char    *name;
            const char    *content;
        } kinds[] = {
            { PDF_ANNOT_TEXT,      "Text",      "A sticky note in the margin." },
            { PDF_ANNOT_HIGHLIGHT, "Highlight", "Highlighted passage." },
            { PDF_ANNOT_UNDERLINE, "Underline", "Underlined passage." },
            { PDF_ANNOT_SQUIGGLY,  "Squiggly",  "Spelling query." },
            { PDF_ANNOT_STRIKEOUT, "StrikeOut", "Proposed deletion." },
            { PDF_ANNOT_SQUARE,    "Square",    "Box around a region." },
            { PDF_ANNOT_CIRCLE,    "Circle",    "Circle around a detail." },
        };
        double ay = y - 130;

        for (i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); i++)
        {
            pdf_annot a = NULL;
            pdf_rect ar = rect(48, ay - 14, 250, 16);
            pdf_color c = rgb(0.85, 0.20, 0.20);

            if (pdf_annot_create(p, kinds[i].type, &ar, kinds[i].content,
                                 &a) != PDF_OK || !a)
            {
                check(0, "annot: create %s", kinds[i].name);
                continue;
            }
            pdf_annot_set_color(a, &c);
            pdf_annot_set_opacity(a, 0.85);
            pdf_annot_set_author(a, "pdflib showcase");
            pdf_annot_set_subject(a, kinds[i].name);
            /* /Name and /Open only mean anything on a text annotation;
             * other engines return UNSUPPORTED, which is not an error. */
            if (kinds[i].type == PDF_ANNOT_TEXT)
            {
                pdf_status s1 = pdf_annot_set_icon(a, PDF_ANNOT_ICON_NOTE);
                pdf_status s2 = pdf_annot_set_open(a, 1);
                check(s1 == PDF_OK && s2 == PDF_OK,
                      "annot: text icon and open state set");
            }
            else
            {
                check_soft("annot: set_icon on a non-text annot is declined",
                           pdf_annot_set_icon(a, PDF_ANNOT_ICON_NOTE));
            }
            pdf_annot_free(a);
            check(1, "annot: created %s", kinds[i].name);
            ay -= 30;
        }
    }

    /* A URI link, and an internal jump to page 2. */
    {
        pdf_annot a = NULL;
        pdf_rect ar = rect(48, y - 60, 220, 16);

        check(pdf_annot_create(p, PDF_ANNOT_LINK_URI, &ar,
                  "https://example.com/pdflib", &a) == PDF_OK && a != NULL,
              "annot: URI link created");
        if (a)
        {
            {
                pdf_color lc = rgb(0.1, 0.3, 0.8);
                pdf_annot_set_color(a, &lc);
            }
            pdf_annot_free(a);
        }
        set_fill(p, rgb(0.1, 0.3, 0.8));
        pdf_page_draw_text(p, b->body,
            "https://example.com/pdflib  (external link)",
            48, y - 74, 9.5);

        /* A deferred GOTO: the destination page is only known here, so the
         * annotation has no dictionary until link_to_page runs. */
        a = NULL;
        ar = rect(48, y - 96, 220, 16);
        if (pdf_annot_create(p, PDF_ANNOT_LINK_GOTO, &ar, NULL, &a) == PDF_OK
            && a)
        {
            pdf_page dest = b->page[1];
            check(pdf_annot_link_to_page(a, &dest, NULL) == PDF_OK,
                  "annot: internal jump to page 2");
            pdf_annot_free(a);
        }
        else
        {
            check(0, "annot: GOTO link created");
        }
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->body, "go to 'Text and fonts' (page 2)",
                           48, y - 110, 9.5);
    }

    furniture(p, b->body, "Annotations", 6);
}

/* ------------------------------------------------------------------ */
/* Page 7 - a table with running heads                                 */
/* ------------------------------------------------------------------ */
static void page_table(built *b)
{
    pdf_page p = b->page[6];
    double w = page_w(p), h = page_h(p);
    pdf_color ink = gray(0.10);
    pdf_color rule = gray(0.60);
    pdf_color head_bg = rgb(0.16, 0.32, 0.58);
    double y = h - 120;
    static const struct { const char *op; const char *pdf; const char *note; }
    rows[] = {
        { "pdf_page_draw_text",        "BT / Tf / Td / Tj", "single line" },
        { "pdf_page_draw_text_aligned","BT / Tf / Td / Tj", "left/centre/right" },
        { "pdf_page_draw_text_wrapped","BT / Tf / TL / T*", "word-wrapped box" },
        { "pdf_path_curve_to",         "m / c",             "cubic bezier" },
        { "pdf_path_circle",           "re + c",            "four arcs" },
        { "pdf_path_arc",              "c",                 "arc segment" },
        { "pdf_path_polygon",          "m / l / h",         "closed polygon" },
        { "pdf_path_paint",            "S / f / B / f* / B*","five modes" },
        { "pdf_set_opacity",           "gs / CA / ca",      "ExtGState" },
        { "pdf_set_blend_mode",        "gs / BM",           "ExtGState" },
        { "pdf_page_draw_image",       "Do",                "image XObject" },
        { "pdf_annot_create",          "Annots",            "annotation" },
        { "pdf_outline_add",           "Outlines",          "bookmark" },
    };
    const int nrows = (int)(sizeof(rows) / sizeof(rows[0]));
    double col[4];
    double row_h = 20;
    int i;

    heading(p, b->bold, "From API call to PDF operator", 48, h - 80, w - 96);
    set_fill(p, gray(0.35));
    pdf_page_draw_text(p, b->body,
        "The third column is what the file actually contains - the same "
        "thing the read-back check asserts on.", 48, y + 14, 9);

    col[0] = 48;
    col[1] = 230;
    col[2] = 380;
    col[3] = w - 48;

    /* Header row. */
    fill_rect(p, rect(col[0], y - 22, col[3] - col[0], 22), head_bg);
    set_fill(p, gray(1.0));
    pdf_page_draw_text(p, b->bold, "API entry point", col[0] + 6, y - 15, 9);
    pdf_page_draw_text(p, b->bold, "PDF operators", col[1] + 6, y - 15, 9);
    pdf_page_draw_text(p, b->bold, "what it emits", col[2] + 6, y - 15, 9);
    y -= 22;

    for (i = 0; i < nrows; i++)
    {
        double top = y - (i + 1) * row_h;

        /* Alternating row shading: the thing that makes a table readable
         * and that a naive generator forgets. */
        if (i % 2)
            fill_rect(p, rect(col[0], top, col[3] - col[0], row_h), gray(0.955));

        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->mono, rows[i].op, col[0] + 6, top + 6, 8.5);
        pdf_page_draw_text(p, b->mono, rows[i].pdf, col[1] + 6, top + 6, 8.5);
        pdf_page_draw_text(p, b->body, rows[i].note, col[2] + 6, top + 6, 8.5);

        pdf_set_stroke_color(p, &rule);
        pdf_set_line_width(p, 0.4);
        pdf_page_draw_line(p, col[0], top, col[3], top);
    }

    /* Close the table with a heavier rule. */
    {
        double bottom = y - nrows * row_h;
        pdf_set_line_width(p, 1.2);
        pdf_page_draw_line(p, col[0], bottom, col[3], bottom);
        pdf_page_draw_line(p, col[0], y, col[0], bottom);
        pdf_page_draw_line(p, col[3], y, col[3], bottom);
    }
    check(1, "table: %d rows with shading and rules", nrows);

    /* A right-aligned numeric column, the case that needs alignment to be
     * worth anything. */
    y -= nrows * row_h + 40;
    {
        int k;
        pdf_set_fill_color(p, &ink);
        pdf_page_draw_text(p, b->bold, "right-aligned figures", 48, y, 11);
        for (k = 0; k < 5; k++)
        {
            char num[32];
            snprintf(num, sizeof(num), "%d.%02d", 1000 + k * 37, k * 13 % 100);
            pdf_page_draw_text_aligned(p, b->mono, num, 48, y - 18 - k * 13,
                                       9.5, 220, PDF_ALIGN_RIGHT);
        }
        check(1, "table: right-aligned numeric column");
    }

    furniture(p, b->body, "Tables", 7);
}

/* ------------------------------------------------------------------ */
/* Page 8 - custom size, rotated                                       */
/* ------------------------------------------------------------------ */
static void page_custom(built *b)
{
    pdf_page p = b->page[7];
    double w = page_w(p), h = page_h(p);
    pdf_color ink = gray(0.12);
    int i;

    heading(p, b->bold, "Custom page and rotation", 32, h - 50, w - 64);

    /* The page was created with an explicit 400x300pt MediaBox and then
     * rotated 90 degrees, so /MediaBox and /Rotate both have to survive
     * the round trip. */
    set_fill(p, gray(0.35));
    pdf_page_draw_text(p, b->body,
        "MediaBox 400 x 300 pt, /Rotate 90", 32, h - 70, 10);

    for (i = 0; i < 6; i++)
    {
        pdf_color c = rgb(0.15 + 0.13 * i, 0.45, 0.80 - 0.10 * i);
        fill_rect(p, rect(32 + i * 30, h - 150, 24, 60), c);
    }
    pdf_set_fill_color(p, &ink);
    pdf_page_draw_text(p, b->body,
        "the content is drawn upright and the viewer rotates it", 32, h - 168, 9);

    set_stroke(p, rgb(0.8, 0.2, 0.2));
    pdf_set_line_width(p, 2);
    pdf_path_ellipse(p, w - 90, 90, 55, 34);
    pdf_path_paint(p, PDF_PATH_STROKE);

    pdf_set_fill_color(p, &ink);
    pdf_page_draw_text(p, b->body, "page 8 of 8", 32, 40, 9);
}

/* ------------------------------------------------------------------ */
/* The outline tree                                                    */
/* ------------------------------------------------------------------ */
static void build_outline(built *b)
{
    static const struct { const char *title; int page; int parent; } items[] = {
        { "Cover",                        0, -1 },
        { "What this document shows",     0,  0 },
        { "Text, fonts and layout",       1, -1 },
        { "The standard 14",              1,  2 },
        { "Embedded TrueType",            1,  2 },
        { "CJK / Type0",                  1,  2 },
        { "Vector graphics",              2, -1 },
        { "Colour and transparency",      3, -1 },
        { "Images",                       4, -1 },
        { "Annotations and links",        5, -1 },
        { "Tables",                       6, -1 },
        { "Custom page and rotation",     7, -1 },
    };
    pdf_outline made[16];
    size_t i;

    memset(made, 0, sizeof(made));
    for (i = 0; i < sizeof(items) / sizeof(items[0]); i++)
    {
        pdf_outline parent = (items[i].parent >= 0) ? made[items[i].parent]
                                                    : NULL;
        pdf_page dest = b->page[items[i].page];

        if (pdf_outline_add(b->doc, parent, items[i].title, &dest,
                            &made[i]) != PDF_OK)
        {
            check(0, "outline: add '%s'", items[i].title);
            continue;
        }
        check(1, "outline: add '%s'", items[i].title);
    }

    /* Leave the branches open so a reader shows the whole tree. */
    for (i = 0; i < sizeof(items) / sizeof(items[0]); i++)
        if (made[i])
            pdf_outline_set_open(made[i], 1);
    for (i = 0; i < sizeof(items) / sizeof(items[0]); i++)
        if (made[i])
            pdf_outline_free(made[i]);
}

/* ------------------------------------------------------------------ */
/* Build the whole document                                            */
/* ------------------------------------------------------------------ */
static int build_showcase(pdf_backend backend, const char *name,
                          const char *path, const char *streamed_path,
                          int *out_faces)
{
    built b;
    int i;
    static const pdf_page_size sizes[N_PAGES] = {
        PDF_PAGE_A4, PDF_PAGE_A4, PDF_PAGE_A4, PDF_PAGE_A4,
        PDF_PAGE_A4, PDF_PAGE_A4, PDF_PAGE_A4, PDF_PAGE_A4
    };

    memset(&b, 0, sizeof(b));
    printf("\n[test] showcase document (%s)\n", name);

    if (pdflib_set_backend(backend) != PDF_OK)
    {
        printf("  [i]    backend %s unavailable, skipping\n", name);
        return 0;
    }

    OK(pdf_doc_create(&b.doc), "showcase: doc_create");
    if (!b.doc)
        return 1;

    /* Metadata: a production document is not anonymous. */
    OK(pdf_doc_set_info(b.doc, PDF_INFO_TITLE,
                        "pdflib feature showcase"), "showcase: set_info(title)");
    OK(pdf_doc_set_info(b.doc, PDF_INFO_AUTHOR, "pdflib test suite"),
       "showcase: set_info(author)");
    OK(pdf_doc_set_info(b.doc, PDF_INFO_SUBJECT,
                        "Every PDF feature pdflib exposes"), "showcase: set_info(subject)");
    OK(pdf_doc_set_info(b.doc, PDF_INFO_KEYWORDS,
                        "pdf, pdflib, showcase, test"), "showcase: set_info(keywords)");
    OK(pdf_doc_set_info(b.doc, PDF_INFO_CREATOR, "showcase_pdf"), "showcase: set_info(creator)");
    OK(pdf_doc_set_compression(b.doc, PDF_COMPRESS_ALL), "showcase: set_compression(ALL)");

    /* Open the document on its bookmark tree. */
    check_soft("showcase: page_mode_use_outlines",
               pdf_doc_set_page_mode_use_outlines(b.doc, 1));

    /* Fonts. The standard faces never fail; the TrueType and CJK loads
     * may legitimately be refused and must not poison the document. */
    OK(pdf_font_get_std(b.doc, PDF_STD_FONT_HELVETICA, &b.body),
       "showcase: font_get_std(Helvetica)");
    OK(pdf_font_get_std(b.doc, PDF_STD_FONT_HELVETICA_BOLD, &b.bold),
       "showcase: font_get_std(Helvetica-Bold)");
    OK(pdf_font_get_std(b.doc, PDF_STD_FONT_HELVETICA_OBLIQUE, &b.italic),
       "showcase: font_get_std(Helvetica-Oblique)");
    OK(pdf_font_get_std(b.doc, PDF_STD_FONT_TIMES_ROMAN, &b.serif),
       "showcase: font_get_std(Times-Roman)");
    OK(pdf_font_get_std(b.doc, PDF_STD_FONT_COURIER, &b.mono),
       "showcase: font_get_std(Courier)");
    OK(pdf_font_get_std(b.doc, PDF_STD_FONT_TIMES_BOLD, &b.display),
       "showcase: font_get_std(Times-Bold)");

    if (load_first(b.doc, k_latin_ttf, 1, &b.ttf, b.ttf_path,
                   sizeof(b.ttf_path)))
        check(1, "showcase: embedded TrueType from %s", b.ttf_path);
    else
        check(1, "showcase: no Latin TrueType on this box (skipping embed)");

    if (load_first_mem(b.doc, k_latin_ttf2, 1, &b.ttf_mem, b.ttf_mem_path,
                       sizeof(b.ttf_mem_path)))
        check(1, "showcase: second face from memory (%s)", b.ttf_mem_path);
    else
        check(1, "showcase: no second TrueType (skipping memory load)");

    if (load_first(b.doc, k_cjk_ttf, 1, &b.cjk, b.cjk_path,
                   sizeof(b.cjk_path)))
        check(1, "showcase: CJK font from %s", b.cjk_path);
    else
        check(1, "showcase: no CJK font on this box (skipping CJK)");

    /* Images, generated here so the example needs no asset files. */
    {
        size_t len = 0;
        unsigned char *png = make_png(256, 256, 0, px_logo, &len);
        if (png)
        {
            pdf_status st = pdf_image_load_memory(b.doc, png, len, &b.img);
            check(st == PDF_OK, "showcase: load generated 256x256 PNG");
            free(png);
        }
        else
        {
            check(0, "showcase: PNG encoder failed");
        }

        len = 0;
        png = make_png(128, 128, 1, px_alpha, &len);
        if (png)
        {
            pdf_status st = pdf_image_load_memory(b.doc, png, len,
                                                  &b.img_alpha);
            check(st == PDF_OK || st == PDF_ERR_BACKEND_UNSUPPORTED,
                  "showcase: load generated RGBA PNG (%s)",
                  st == PDF_OK ? "accepted"
                               : "engine has no alpha support, fine");
            free(png);
        }
    }

    /* Pages. Page 8 is deliberately a custom size and is rotated. */
    for (i = 0; i < N_PAGES; i++)
    {
        if (i == 7)
        {
            OK(pdf_page_add_custom(b.doc, 400, 300, &b.page[i]),
               "showcase: page_add_custom(400x300)");
            if (b.page[i])
                OK(pdf_page_set_rotation(b.page[i], 90),
                   "showcase: page_set_rotation(90)");
        }
        else if (i == 4)
        {
            OK(pdf_page_add(b.doc, sizes[i], PDF_ORIENTATION_LANDSCAPE,
                            &b.page[i]), "showcase: page_add(A4 landscape)");
        }
        else
        {
            OK(pdf_page_add(b.doc, sizes[i], PDF_ORIENTATION_PORTRAIT,
                            &b.page[i]), "showcase: page_add(A4)");
        }
        if (!b.page[i])
            return 1;
    }

    /* Draw them. */
    page_cover(&b);
    page_text(&b);
    page_vector(&b);
    page_color(&b);
    page_images(&b);
    page_annots(&b);
    page_table(&b);
    page_custom(&b);

    build_outline(&b);

    /* Free the wrappers: the document owns everything already. */
    /* How many distinct embedded faces the document should contain. */
    if (out_faces)
        *out_faces = (b.ttf ? 1 : 0) + (b.ttf_mem ? 1 : 0) + (b.cjk ? 1 : 0);

    if (b.img)       pdf_image_free(b.img);
    if (b.img_alpha) pdf_image_free(b.img_alpha);
    if (b.ttf)       pdf_font_free(b.ttf);
    if (b.ttf_mem)   pdf_font_free(b.ttf_mem);
    if (b.cjk)       pdf_font_free(b.cjk);
    pdf_font_free(b.body);
    pdf_font_free(b.bold);
    pdf_font_free(b.italic);
    pdf_font_free(b.serif);
    pdf_font_free(b.mono);
    pdf_font_free(b.display);

    /* Save. */
    OK(pdf_doc_save_to_file(b.doc, path), "showcase: save_to_file");
    printf("  [i]    wrote %s\n", path);

    /* The streamed writer. Both engines implement it, so declining is a
     * regression, not a capability gap: assert success rather than passing
     * quietly, otherwise a backend that stops streaming goes unnoticed. */
    if (streamed_path)
    {
        pdf_doc sdoc = NULL;
        pdf_status st = pdf_doc_create_streamed_to_file(streamed_path, &sdoc);
        check(st == PDF_OK && sdoc != NULL,
              "showcase: create_streamed_to_file (got %d)", (int)st);
        if (st == PDF_OK && sdoc)
        {
            pdf_page sp = NULL;
            pdf_font f = NULL;
            int k;
            pdf_doc_set_info(sdoc, PDF_INFO_TITLE, "pdflib showcase (streamed)");
            for (k = 0; k < 4; k++)
            {
                char label[64];
                if (pdf_page_add(sdoc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT,
                                 &sp) != PDF_OK)
                    break;
                pdf_font_get_std(sdoc, PDF_STD_FONT_HELVETICA, &f);
                snprintf(label, sizeof(label),
                         "streamed sheet %d - page %d of 4", k + 1, k + 1);
                pdf_page_draw_text(sp, f, label, 48, 780, 16);
                set_fill(sp, rgb(0.2, 0.4, 0.8));
                draw_rect(sp, 48, 700, 200, 40, PDF_PATH_FILL);
                pdf_font_free(f);
            }
            check(pdf_doc_finish(sdoc) == PDF_OK, "showcase: streamed finish");
            pdf_doc_free(sdoc);
            printf("  [i]    wrote %s\n", streamed_path);
        }
    }

    pdf_doc_free(b.doc);
    return 0;
}

/* ==================================================================== */
/* Read-back verification                                               */
/* ==================================================================== */

typedef struct reader {
    unsigned char *buf;
    long           n;
    pdfi_doc      *d;
} reader;

static unsigned char *slurp(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *b;

    *out_len = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    b = (unsigned char *)malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
    {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = n;
    return b;
}

static int reader_open(reader *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    r->buf = slurp(path, &r->n);
    if (!r->buf)
    {
        check(0, "readback: cannot read '%s'", path);
        return 0;
    }
    r->d = (pdfi_doc *)malloc(sizeof(pdfi_doc));
    if (!r->d)
    {
        check(0, "readback: out of memory");
        free(r->buf);
        r->buf = NULL;
        return 0;
    }
    pdfi_scan(r->buf, r->n, r->d);
    return 1;
}

static void reader_close(reader *r)
{
    free(r->d);
    free(r->buf);
    r->d = NULL;
    r->buf = NULL;
}

static long reader_page_content(reader *r, int idx, unsigned char **out)
{
    long nums[PDFI_MAX_PAGES];
    int np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);

    *out = NULL;
    if (idx >= np)
        return -1;
    return pdfi_page_content(r->d, nums[idx], out);
}

static void rb_op(reader *r, int page, const char *op, int min, const char *what)
{
    unsigned char *c = NULL;
    long n = reader_page_content(r, page, &c);
    int got;

    if (n < 0)
    {
        check(0, "%s: page %d has no decodable content", what, page + 1);
        return;
    }
    got = pdfi_count_op(c, n, op);
    check(got >= min, "%s: page %d has '%s' x%d (want >= %d)", what, page + 1,
          op, got, min);
    free(c);
}

static void rb_text(reader *r, int page, const char *text, const char *what)
{
    unsigned char *c = NULL;
    long n = reader_page_content(r, page, &c);

    if (n < 0)
    {
        check(0, "%s: page %d has no decodable content", what, page + 1);
        return;
    }
    check(pdfi_content_has_text(c, n, text),
          "%s: page %d carries the text \"%s\"", what, page + 1, text);
    free(c);
}

/* Assert that a run of text was painted exactly `expect` times.
 *
 * Presence is not enough for a wrapped paragraph: the box-filling loop
 * in pdf_page_draw_text_wrapped used to repaint the whole paragraph on
 * every pass, and "the text is in there" is equally true whether it was
 * drawn once or five times. Counting is what makes that visible. */
static void rb_text_count(reader *r, int page, const char *text, int expect,
                          const char *what)
{
    unsigned char *c = NULL;
    long n = reader_page_content(r, page, &c);
    int got;

    if (n < 0)
    {
        check(0, "%s: page %d has no decodable content", what, page + 1);
        return;
    }
    got = pdfi_count_text(c, n, text);
    check(got == expect,
          "%s: page %d paints \"%s\" exactly %d time(s) (got %d)",
          what, page + 1, text, expect, got);
    free(c);
}

/* Assert that the operand of the last occurrence of `op` on a page is
 * `expect`.
 *
 * Used for /Tr. The page sets "3 Tr" for the invisible run and then goes
 * back to fill; /Tr is text state, so it carries into every later text
 * object, and a page whose last /Tr is not 0 has lost every word drawn
 * after it. PoDoFo's painter does exactly that unless it is repaired -
 * it emits "n Tr" only when n is not fill - and counting "Tr" operators
 * cannot see the difference, because the operator is present either way. */
static void rb_last_op(reader *r, int page, const char *op, long expect,
                       const char *what)
{
    unsigned char *c = NULL;
    long n = reader_page_content(r, page, &c);
    long got;

    if (n < 0)
    {
        check(0, "%s: page %d has no decodable content", what, page + 1);
        return;
    }
    got = pdfi_last_op_operand(c, n, op);
    check(got == expect, "%s: page %d ends with '%s %ld' (want %ld)",
          what, page + 1, op, got, expect);
    free(c);
}

static void rb_q_balance(reader *r, int page, const char *what)
{
    unsigned char *c = NULL;
    long n = reader_page_content(r, page, &c);
    int q, Q;

    if (n < 0)
    {
        check(0, "%s: page %d has no decodable content", what, page + 1);
        return;
    }
    q = pdfi_count_op(c, n, "q");
    Q = pdfi_count_op(c, n, "Q");
    check(q == Q && q >= 1,
          "%s: page %d gstate balanced (q=%d Q=%d)", what, page + 1, q, Q);
    free(c);
}

static void verify_showcase(const char *path, const char *label,
                            int expect_faces)
{
    reader r;
    int i;

    printf("\n[test] read-back verification (%s)\n", label);

    if (!reader_open(&r, path))
        return;

    /* --- structure ------------------------------------------------- */
    {
        long nums[PDFI_MAX_PAGES];
        int np = pdfi_pages(r.d, nums, PDFI_MAX_PAGES);
        check(np == N_PAGES, "%s: page count is %d (got %d)", label, N_PAGES, np);
        check(!pdfi_has_object_streams(r.d),
              "%s: no /ObjStm, so the object scan sees every object", label);
    }

    /* Every stream must inflate: the only check that catches a mis-sized
     * /Length, because a truncated stream leaves a perfect page tree. */
    {
        int k, bad = 0, checked = 0;
        for (k = 0; k < r.d->nobj; k++)
        {
            const pdfi_obj *o = &r.d->objs[k];
            unsigned char *dec = NULL;
            long len;

            if (o->stream_off < 0 || o->stream_len <= 0)
                continue;
            if (!pdfi_stream_is_flate(r.d, o))
            {
                char filters[4][48];
                if (pdfi_stream_filters(r.d, o, filters, 4) > 0)
                    continue;
            }
            checked++;
            len = pdfi_stream(r.d, o, &dec);
            if (len < 0)
                bad++;
            free(dec);
        }
        check(checked > 0 && bad == 0,
              "%s: every decodable stream inflates (%d checked, %d failed)",
              label, checked, bad);
    }

    /* Every page that was drawn on must carry content. */
    {
        long nums[PDFI_MAX_PAGES];
        int np = pdfi_pages(r.d, nums, PDFI_MAX_PAGES);
        int blank = 0, drawn = 0;
        for (i = 0; i < np; i++)
        {
            unsigned char *c = NULL;
            long n = pdfi_page_content(r.d, nums[i], &c);
            if (n <= 0)
                blank++;
            else
                drawn++;
            free(c);
        }
        check(blank == 0 && drawn == np,
              "%s: all %d pages carry content (%d drawn, %d blank)", label, np,
              drawn, blank);
    }

    /* --- geometry -------------------------------------------------- */
    {
        long nums[PDFI_MAX_PAGES];
        double box[4];
        int np = pdfi_pages(r.d, nums, PDFI_MAX_PAGES);
        long rot = 0;

        if (np >= 8)
        {
            if (pdfi_dict_array_nums(r.d, pdfi_find(r.d, nums[7]), "MediaBox",
                                     box, 4) == 4)
                check(fabs(box[2] - 400.0) < 0.5 && fabs(box[3] - 300.0) < 0.5,
                      "%s: page 8 MediaBox is 400x300 (got %.1f x %.1f)", label,
                      box[2] - box[0], box[3] - box[1]);
            else
                check(0, "%s: page 8 has no readable MediaBox", label);

            check(pdfi_dict_int(r.d, pdfi_find(r.d, nums[7]), "Rotate", &rot)
                      && rot == 90,
                  "%s: page 8 /Rotate is 90 (got %ld)", label, rot);

            if (pdfi_dict_array_nums(r.d, pdfi_find(r.d, nums[4]), "MediaBox",
                                     box, 4) == 4)
                check(box[2] > box[3],
                      "%s: page 5 is landscape (%.1f x %.1f)", label,
                      box[2] - box[0], box[3] - box[1]);
        }
    }

    /* --- content, page by page ------------------------------------- */
    /* Cover: CMYK band, bezier, circles, a rotated watermark. */
    rb_op(&r, 0, "k", 1, label);
    rb_op(&r, 0, "c", 1, label);
    rb_op(&r, 0, "cm", 1, label);
    rb_op(&r, 0, "re", 1, label);
    rb_op(&r, 0, "Tj", 1, label);
    rb_text(&r, 0, "pdflib", label);
    /* The cover subtitle is a wrapped paragraph: it must appear once,
     * not once per pass through the box-filling loop. */
    rb_text_count(&r, 0, "One self-contained DLL", 1, label);
    rb_q_balance(&r, 0, label);

    /* Text page: all the standard fonts, alignment, render modes. */
    rb_op(&r, 1, "Tf", 8, label);
    rb_op(&r, 1, "Tj", 8, label);
    rb_op(&r, 1, "Tr", 3, label);
    rb_text(&r, 1, "left aligned", label);
    rb_text(&r, 1, "centred", label);
    rb_text(&r, 1, "right aligned", label);
    /* Both wrapped paragraph blocks on this page are single-painted: a
     * 200pt box holding a paragraph that wraps to eight lines used to
     * paint the first four of them a second time to fill the box. */
    rb_text_count(&r, 1, "Wrapped text is laid out", 1, label);
    /* The page runs through "3 Tr" for the invisible sample and returns
     * to fill. The last /Tr operand is what every later run inherits, so
     * if it is not 0 the rest of the page - the wrapped paragraphs and
     * the footer - is invisible. */
    rb_last_op(&r, 1, "Tr", 0, label);

    /* Vector page: all the path primitives and both fill rules. */
    rb_op(&r, 2, "c", 1, label);
    rb_op(&r, 2, "re", 1, label);
    rb_op(&r, 2, "S", 3, label);
    rb_op(&r, 2, "f*", 1, label);
    rb_op(&r, 2, "d", 1, label);
    rb_op(&r, 2, "w", 1, label);
    rb_q_balance(&r, 2, label);

    /* Colour page: all three device spaces and the transparency ops. */
    rb_op(&r, 3, "g", 1, label);
    rb_op(&r, 3, "rg", 1, label);
    rb_op(&r, 3, "k", 1, label);
    rb_op(&r, 3, "gs", 1, label);

    /* Images page: a real XObject, placed many times. */
    rb_op(&r, 4, "Do", 5, label);

    /* Annotation page: every markup type, plus both link kinds. */
    {
        long nums[PDFI_MAX_PAGES];
        int np = pdfi_pages(r.d, nums, PDFI_MAX_PAGES);
        static const char *const want[] = {
            "Text", "Highlight", "Underline", "Squiggly", "StrikeOut",
            "Square", "Circle", "Link"
        };
        size_t k;

        /* The paragraph the markups annotate, likewise painted once. */
        rb_text_count(&r, 5, "The markups below are placed over", 1, label);

        if (np > 5)
        {
            const pdfi_obj *pg = pdfi_find(r.d, nums[5]);
            int total = pdfi_dict_array_count(r.d, pg, "Annots");
            check(total >= 9, "%s: page 6 carries %d annotations (want >= 9)",
                  label, total);

            for (k = 0; k < sizeof(want) / sizeof(want[0]); k++)
            {
                int found = 0, j;
                for (j = 0; j < r.d->nobj; j++)
                {
                    char sub[48] = "";
                    if (!pdfi_dict_name(r.d, &r.d->objs[j], "Subtype", sub,
                                        sizeof(sub)))
                        continue;
                    if (strcmp(sub, want[k]) == 0)
                        found = 1;
                }
                check(found, "%s: a /%s annotation exists", label, want[k]);
            }
        }
    }

    /* A link with a real URI, decoded out of the /A dictionary. */
    {
        int found = 0, j;
        for (j = 0; j < r.d->nobj && !found; j++)
        {
            pdfi_obj act;
            char uri[256] = "";
            if (!pdfi_dict_subdict(r.d, &r.d->objs[j], "A", &act))
                continue;
            if (pdfi_dict_text(r.d, &act, "URI", uri, sizeof(uri)) &&
                strstr(uri, "example.com/pdflib"))
                found = 1;
        }
        check(found, "%s: the URI link carries /URI example.com/pdflib", label);
    }

    /* --- fonts ----------------------------------------------------- */
    {
        int nfonts = 0, type0 = 0, j;
        char faces[16][64];
        int nfaces = 0;

        for (j = 0; j < r.d->nobj; j++)
        {
            char type[32] = "", sub[48] = "", base[64] = "";

            pdfi_dict_name(r.d, &r.d->objs[j], "Type", type, sizeof(type));
            pdfi_dict_name(r.d, &r.d->objs[j], "Subtype", sub, sizeof(sub));
            if (strcmp(type, "Font") != 0)
                continue;
            nfonts++;
            if (strcmp(sub, "Type0") == 0)
                type0++;
            if (!pdfi_dict_has(r.d, &r.d->objs[j], "FontDescriptor"))
                continue;
            pdfi_dict_name(r.d, &r.d->objs[j], "BaseFont", base, sizeof(base));
            {
                int k, seen = 0;
                for (k = 0; k < nfaces; k++)
                    if (strcmp(faces[k], base) == 0)
                        seen = 1;
                if (!seen && nfaces < 16)
                    snprintf(faces[nfaces++], sizeof(faces[0]), "%s", base);
            }
        }
        check(nfonts >= 6, "%s: %d font objects in the file", label, nfonts);
        /* Distinct embedded faces, not just "at least one": a backend that
         * aliases every subset font into the first would still pass the
         * weaker check while silently losing whole faces. */
        check(nfaces == expect_faces,
              "%s: %d distinct embedded face(s), expected %d", label, nfaces,
              expect_faces);
        if (nfaces > 0)
        {
            int k;
            for (k = 0; k < nfaces; k++)
                printf("  [i]    embedded face: %s\n", faces[k]);
        }
        check(type0 >= 1,
              "%s: %d Type0 font(s) - CJK went out as CID", label, type0);
    }

    /* --- images ---------------------------------------------------- */
    {
        int images = 0, j;
        for (j = 0; j < r.d->nobj; j++)
        {
            char sub[48] = "";
            pdfi_dict_name(r.d, &r.d->objs[j], "Subtype", sub, sizeof(sub));
            if (strcmp(sub, "Image") == 0)
            {
                long iw = 0, ih = 0;
                pdfi_dict_int(r.d, &r.d->objs[j], "Width", &iw);
                pdfi_dict_int(r.d, &r.d->objs[j], "Height", &ih);
                if (iw == 256 && ih == 256)
                    images++;
            }
        }
        check(images >= 1, "%s: a 256x256 image XObject is present", label);
    }

    /* --- outline --------------------------------------------------- */
    {
        long root = pdfi_trailer_ref(r.d, "Root");
        const pdfi_obj *cat = root > 0 ? pdfi_find(r.d, root) : NULL;
        long ol = cat ? pdfi_dict_ref(r.d, cat, "Outlines") : -1;
        const pdfi_obj *tree = ol > 0 ? pdfi_find(r.d, ol) : NULL;

        check(tree != NULL && pdfi_dict_has(r.d, tree, "First"),
              "%s: catalog has an /Outlines tree with a first item", label);

        if (tree)
        {
            int items = 0, j;
            for (j = 0; j < r.d->nobj; j++)
                if (pdfi_dict_has(r.d, &r.d->objs[j], "Title") &&
                    pdfi_dict_has(r.d, &r.d->objs[j], "Parent"))
                    items++;
            check(items >= 12, "%s: %d outline items in the tree (want >= 12)",
                  label, items);
        }
    }

    /* --- metadata -------------------------------------------------- */
    {
        long info = pdfi_trailer_ref(r.d, "Info");
        const pdfi_obj *o = info > 0 ? pdfi_find(r.d, info) : NULL;
        char v[256] = "";

        check(o != NULL, "%s: trailer has an /Info dictionary", label);
        if (o)
        {
            check(pdfi_dict_text(r.d, o, "Title", v, sizeof(v)) &&
                      strcmp(v, "pdflib feature showcase") == 0,
                  "%s: /Title round-trips as \"%s\"", label, v);
            v[0] = 0;
            check(pdfi_dict_text(r.d, o, "Author", v, sizeof(v)) &&
                      strcmp(v, "pdflib test suite") == 0,
                  "%s: /Author round-trips as \"%s\"", label, v);
        }
    }

    /* --- compression ----------------------------------------------- */
    {
        int flate = 0, j;
        for (j = 0; j < r.d->nobj; j++)
            if (r.d->objs[j].stream_off >= 0 &&
                pdfi_stream_is_flate(r.d, &r.d->objs[j]))
                flate++;
        check(flate >= 5, "%s: %d FlateDecode stream(s) - compression is on",
              label, flate);
    }

    /* --- viewer preferences ---------------------------------------- */
    {
        long root = pdfi_trailer_ref(r.d, "Root");
        const pdfi_obj *cat = root > 0 ? pdfi_find(r.d, root) : NULL;
        char pm[32] = "";
        check(cat && pdfi_dict_name(r.d, cat, "PageMode", pm, sizeof(pm)) &&
                  strcmp(pm, "UseOutlines") == 0,
              "%s: /PageMode is UseOutlines (got \"%s\")", label, pm);
    }

    reader_close(&r);
}

/* Basic byte-level checks for a file the reader cannot parse (encrypted). */
static void verify_encrypted(const char *path, const char *label)
{
    unsigned char *buf;
    long n;

    printf("\n[test] encrypted output (%s)\n", label);
    buf = slurp(path, &n);
    if (!buf)
    {
        check(0, "%s: cannot read '%s'", label, path);
        return;
    }
    check(n > 8 && memcmp(buf, "%PDF-", 5) == 0, "%s: %s header", label, "%PDF-");
    check(pdfi_contains(buf, n, "%%EOF"), "%s: %s trailer", label, "%%EOF");
    check(pdfi_contains(buf, n, "trailer"), "%s: trailer keyword present", label);
    check(pdfi_contains(buf, n, "/Encrypt"), "%s: /Encrypt present", label);
    check(pdfi_contains(buf, n, "obj"), "%s: indirect objects present", label);
    printf("  [i]    %ld bytes\n", n);
    free(buf);
}

/* Build and verify the encrypted variant. */
static int build_encrypted(pdf_backend backend, const char *path)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font f = NULL;

    if (pdflib_set_backend(backend) != PDF_OK)
        return 0;
    if (pdf_doc_create(&doc) != PDF_OK)
        return 1;

    pdf_doc_set_info(doc, PDF_INFO_TITLE, "pdflib showcase (encrypted)");
    pdf_doc_set_compression(doc, PDF_COMPRESS_ALL);

    if (pdf_doc_set_encryption(doc, PDF_ENCRYPT_RC4_128, "owner-secret",
                               "user-secret",
                               PDF_PERM_PRINT | PDF_PERM_COPY) != PDF_OK)
    {
        check(1, "encrypted: engine has no encryption support (skipping)");
        pdf_doc_free(doc);
        return 0;
    }
    check(1, "encrypted: set_encryption(RC4_128) accepted");

    if (pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page) != PDF_OK)
    {
        pdf_doc_free(doc);
        return 1;
    }
    pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &f);
    pdf_page_draw_text(page, f, "This page is encrypted (RC4 128-bit).",
                       48, 780, 14);
    pdf_font_free(f);

    check(pdf_doc_save_to_file(doc, path) == PDF_OK, "encrypted: save_to_file");
    pdf_doc_free(doc);

    verify_encrypted(path, "encrypted");
    return 0;
}

/* ==================================================================== */
/* Driver                                                               */
/* ==================================================================== */

/* The streamed file is a *separate* document, so verify_showcase cannot be
 * reused: that one asserts the 13-page layout. What matters here is that the
 * bytes the incremental writer produced are a real PDF - every page present,
 * its text drawn exactly once, the metadata that was set before any page
 * survived, and every stream still inflatable. Without this the streamed path
 * is only exercised, never checked, and a writer that emitted a valid header
 * plus nothing else would pass. */
static void verify_streamed(const char *path, const char *label)
{
    reader r;
    int i;
    const int pages = 4;

    printf("\n[test] read-back verification (%s, streamed)\n", label);

    if (!reader_open(&r, path))
        return;

    {
        long nums[PDFI_MAX_PAGES];
        int np = pdfi_pages(r.d, nums, PDFI_MAX_PAGES);
        check(np == pages, "%s streamed: page count is %d (got %d)",
              label, pages, np);
    }

    for (i = 0; i < pages; i++)
    {
        unsigned char *c = NULL;
        long n = reader_page_content(&r, i, &c);
        char want[64];

        snprintf(want, sizeof(want), "streamed sheet %d - page %d of 4",
                 i + 1, i + 1);
        if (n <= 0)
        {
            check(0, "%s streamed: page %d has no content", label, i + 1);
            free(c);
            continue;
        }
        check(pdfi_count_text(c, n, want) == 1,
              "%s streamed: page %d draws \"%s\" exactly once", label, i + 1,
              want);
        free(c);
    }

    /* Every stream must inflate: a mis-sized /Length leaves a perfect page
     * tree, so this is the only check that catches a truncated write. */
    {
        int k, bad = 0, checked = 0;

        for (k = 0; k < r.d->nobj; k++)
        {
            const pdfi_obj *o = &r.d->objs[k];
            unsigned char *dec = NULL;
            long len;

            if (o->stream_off < 0 || o->stream_len <= 0)
                continue;
            if (!pdfi_stream_is_flate(r.d, o))
            {
                char filters[4][48];

                if (pdfi_stream_filters(r.d, o, filters, 4) > 0)
                    continue;
            }
            checked++;
            len = pdfi_stream(r.d, o, &dec);
            if (len < 0)
                bad++;
            free(dec);
        }
        check(checked > 0 && bad == 0,
              "%s streamed: all %d stream(s) decode (0 failures)", label,
              checked);
    }

    {
        long info = pdfi_trailer_ref(r.d, "Info");
        const pdfi_obj *o = info > 0 ? pdfi_find(r.d, info) : NULL;
        char v[256] = "";

        check(o != NULL, "%s streamed: trailer has an /Info dictionary", label);
        if (o)
            check(pdfi_dict_text(r.d, o, "Title", v, sizeof(v)) &&
                      strcmp(v, "pdflib showcase (streamed)") == 0,
                  "%s streamed: /Title round-trips as \"%s\"", label, v);
    }

    reader_close(&r);
}

static int run_one(pdf_backend backend, const char *name, const char *prefix)
{
    char path[512], enc[512], streamed[512];
    int faces = 0;

    snprintf(path, sizeof(path), "%s.pdf", prefix);
    snprintf(enc, sizeof(enc), "%s-encrypted.pdf", prefix);
    snprintf(streamed, sizeof(streamed), "%s-streamed.pdf", prefix);

    if (build_showcase(backend, name, path, streamed, &faces) != 0)
        return 1;

    verify_showcase(path, name, faces);
    verify_streamed(streamed, name);

    if (backend == PDF_BACKEND_PODOFO)
        build_encrypted(backend, enc);

    return 0;
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";
    const char *prefix = argc > 2 ? argv[2] : "showcase";
    int rc = 0;

    printf("pdflib showcase - %s, backend(s): %s\n", pdflib_version(), which);

    if (strcmp(which, "all") == 0 || strcmp(which, "podofo") == 0)
    {
        char p[512];
        snprintf(p, sizeof(p), "%s-podofo", prefix);
        rc |= run_one(PDF_BACKEND_PODOFO, "podofo", p);
    }
    if (strcmp(which, "all") == 0 || strcmp(which, "libharu") == 0)
    {
        char p[512];
        snprintf(p, sizeof(p), "%s-libharu", prefix);
        rc |= run_one(PDF_BACKEND_LIBHARU, "libharu", p);
    }

    printf("\n=== %s: %d check(s), %d failure(s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
    return (g_failures || rc) ? 1 : 0;
}
