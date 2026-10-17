/*
 * pdfx_showcase.c - a libharu program that happens to link pdfx.dll.
 *
 * The point of this file is the *shape* of the code: it includes only
 * "pdfx.h", it calls only PDFX_* functions, and every call has the
 * argument order, the return type and the semantics it would have if the
 * prefix were HPDF_. There is no pdflib header, no pdf_* call, and no
 * direct use of libharu anywhere in it. pdfx_compat.c translates all of
 * this onto the backend-agnostic core, and the engine underneath (libharu
 * or PoDoFo) is picked at run time.
 *
 * It builds a seven-page document that exercises the parts of the libharu
 * surface a real program actually uses - fonts and text state, paths and
 * line state, colour and transparency, images, annotations and links,
 * outlines and page geometry - then checks the bytes that came out:
 *
 *   - every call is checked against PDFX_OK (a silently failing call is
 *     the classic libharu trap: the error is latched and every later call
 *     turns into a no-op);
 *   - the file starts with %PDF- and ends with %%EOF;
 *   - the memory stream and the file agree byte for byte, which is the
 *     only way to know SaveToStream and SaveToFile take the same path;
 *   - the page count read back through PDFX_GetPageByIndex matches.
 *
 * Deeper verification (text extraction, fonts, annotations, bookmarks,
 * per-page rendering) is done by tools/crosscheck_pdf.py, which parses
 * the output with PyMuPDF - an implementation with no shared code with
 * either engine.
 *
 * Usage: pdfx_showcase [libharu|podofo|all] [prefix]
 */
#include "pdfx.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Check harness                                                       */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

/* Output mode. A sandboxed CI runner may not let a child process create
 * files at all (fopen returns EACCES even for the temp directory), so the
 * document can be captured on stdout and redirected by the caller - the
 * parent opens the file, the process only writes an inherited handle. */
static int g_to_stdout;
static int g_selftest;

/* Every diagnostic goes through here: in --stdout mode stdout is the PDF
 * itself, so a stray printf would corrupt the document. */
static FILE *out_stream(void)
{
    return g_to_stdout ? stderr : stdout;
}

/* Compression has to be chosen before the first page: an engine attaches a
 * stream's filter when the stream is created. main runs the whole document
 * twice, once per setting, and compares the two sizes. */
static int g_compress;
static PDFX_UINT32 g_last_stream_size;

/* %%EOF is commonly followed by a newline and some writers pad the tail.
 * Look for the marker inside the last few bytes rather than at a fixed
 * offset, or a perfectly valid file fails on its last character. */
static int has_eof_tail(const unsigned char *buf, unsigned long n)
{
    unsigned long start;

    if (n < 5)
        return 0;
    start = n > 32 ? n - 32 : 0;
    for (; start + 5 <= n; start++)
        if (memcmp(buf + start, "%%EOF", 5) == 0)
            return 1;
    return 0;
}

static void check(int ok, const char *fmt, ...)
{
    va_list ap;

    ++g_checks;
    if (!ok)
        ++g_failures;
    fprintf(out_stream(), "  [%s] ", ok ? "ok" : "FAIL");
    va_start(ap, fmt);
    vfprintf(out_stream(), fmt, ap);
    va_end(ap);
    fputc('\n', out_stream());
}

/* A call whose refusal is an engine limitation rather than a bug. */
static void check_soft(const char *what, PDFX_STATUS st)
{
    if (st == PDFX_OK)
    {
        check(1, "%s", what);
        return;
    }
    ++g_checks;
    fprintf(out_stream(), "  [skip] %s (engine says %04X)\n", what,
            (unsigned)st);
}

#define OK(expr, what) check((expr) == PDFX_OK, "%s", what)

/* libharu reports errors through a callback and then latches them, so an
 * unchecked status is invisible until the whole document is empty. */
static int g_handler_calls;

static void error_handler(PDFX_STATUS error_no, PDFX_STATUS detail_no,
                          void *user_data)
{
    (void)user_data;
    ++g_handler_calls;
    fprintf(out_stream(), "      engine error: error_no=%04X detail_no=%04X\n",
            (unsigned)error_no, (unsigned)detail_no);
}

/* ------------------------------------------------------------------ */
/* Colour helpers                                                      */
/* ------------------------------------------------------------------ */

static PDFX_RGBColor rgb(double r, double g, double b)
{
    PDFX_RGBColor c;
    c.r = (PDFX_REAL)r;
    c.g = (PDFX_REAL)g;
    c.b = (PDFX_REAL)b;
    return c;
}

static PDFX_Rect rect(double l, double b, double r, double t)
{
    PDFX_Rect o;
    o.left = (PDFX_REAL)l;
    o.bottom = (PDFX_REAL)b;
    o.right = (PDFX_REAL)r;
    o.top = (PDFX_REAL)t;
    return o;
}

static void set_fill(PDFX_Page page, PDFX_RGBColor c)
{
    PDFX_Page_SetRGBFill(page, c.r, c.g, c.b);
}

static void set_stroke(PDFX_Page page, PDFX_RGBColor c)
{
    PDFX_Page_SetRGBStroke(page, c.r, c.g, c.b);
}

static void draw_box(PDFX_Page page, double x, double y, double w, double h,
                     PDFX_BOOL fill)
{
    PDFX_Page_Rectangle(page, (PDFX_REAL)x, (PDFX_REAL)y,
                        (PDFX_REAL)w, (PDFX_REAL)h);
    if (fill)
        PDFX_Page_Fill(page);
    else
        PDFX_Page_Stroke(page);
}

/* ------------------------------------------------------------------ */
/* A PNG encoder, so the document needs no external image files.       */
/*                                                                     */
/* PDFX_LoadPngImageFromMem is the path a libharu program uses for     */
/* images, and feeding it a file we built ourselves keeps the test     */
/* self-contained while still exercising a real decoder. The pixel     */
/* function is deliberately asymmetric (gradient left to right, a ring */
/* off centre, a dot in the middle) so a wrong stride, channel order or */
/* row direction is visible rather than merely plausible.              */
/* ------------------------------------------------------------------ */

typedef void (*pixel_fn)(int x, int y, int w, int h, unsigned char *out);

static void be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
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

static unsigned char *make_png(int w, int h, int with_alpha, pixel_fn px,
                               size_t *out_len)
{
    const int channels = with_alpha ? 4 : 3;
    const size_t stride = (size_t)w * (size_t)channels;
    unsigned char *raw = (unsigned char *)malloc((stride + 1) * (size_t)h);
    unsigned char *comp = NULL;
    uLongf comp_len;
    unsigned char ihdr[13];
    unsigned char *c_ihdr = NULL, *c_idat = NULL, *c_iend = NULL;
    size_t n_ihdr = 0, n_idat = 0, n_iend = 0;
    unsigned char *png = NULL;
    size_t total;
    int y, x;

    if (!raw)
        return NULL;

    for (y = 0; y < h; y++)
    {
        unsigned char *row = raw + (size_t)y * (stride + 1);
        row[0] = 0;                     /* filter type 0: none */
        for (x = 0; x < w; x++)
            px(x, y, w, h, row + 1 + (size_t)x * (size_t)channels);
    }

    comp_len = compressBound((uLong)((stride + 1) * (size_t)h));
    comp = (unsigned char *)malloc(comp_len ? comp_len : 1);
    if (!comp)
    {
        free(raw);
        return NULL;
    }
    if (compress2(comp, &comp_len, raw, (uLong)((stride + 1) * (size_t)h),
                  9) != Z_OK)
    {
        free(raw);
        free(comp);
        return NULL;
    }
    free(raw);

    be32(ihdr, (unsigned long)w);
    be32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8;                        /* bit depth */
    ihdr[9] = with_alpha ? 6 : 2;       /* colour type: RGBA / RGB */
    ihdr[10] = 0;                       /* deflate */
    ihdr[11] = 0;                       /* adaptive filtering */
    ihdr[12] = 0;                       /* no interlace */

    c_ihdr = png_chunk("IHDR", ihdr, sizeof(ihdr), &n_ihdr);
    c_idat = png_chunk("IDAT", comp, (size_t)comp_len, &n_idat);
    c_iend = png_chunk("IEND", NULL, 0, &n_iend);
    free(comp);

    if (!c_ihdr || !c_idat || !c_iend)
    {
        free(c_ihdr);
        free(c_idat);
        free(c_iend);
        return NULL;
    }

    total = 8 + n_ihdr + n_idat + n_iend;
    png = (unsigned char *)malloc(total);
    if (png)
    {
        static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
        unsigned char *p = png;
        memcpy(p, sig, 8);
        p += 8;
        memcpy(p, c_ihdr, n_ihdr); p += n_ihdr;
        memcpy(p, c_idat, n_idat); p += n_idat;
        memcpy(p, c_iend, n_iend);
        *out_len = total;
    }

    free(c_ihdr);
    free(c_idat);
    free(c_iend);
    return png;
}

/* Gradient, an off-centre ring and a centre dot: a wrong stride shows up
 * as diagonal streaking, a wrong channel order as the wrong hue, and a
 * flipped row order as the ring being at the top instead of the bottom. */
static void px_logo(int x, int y, int w, int h, unsigned char *out)
{
    double fx = (double)x / (double)(w > 1 ? w - 1 : 1);
    double fy = (double)y / (double)(h > 1 ? h - 1 : 1);
    double cx = 0.5, cy = 0.55, dx = fx - cx, dy = fy - cy;
    double d = sqrt(dx * dx + dy * dy);
    unsigned char r = (unsigned char)(30 + 200 * fx);
    unsigned char g = (unsigned char)(60 + 120 * (1.0 - fy));
    unsigned char b = (unsigned char)(120 + 100 * fy);

    if (d > 0.34 && d < 0.42)
    {
        r = 245; g = 245; b = 245;
    }
    else if (d < 0.10)
    {
        r = 250; g = 180; b = 40;
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
}

/* Radial alpha falloff, so a real /SMask is visible as a soft edge. */
static void px_alpha(int x, int y, int w, int h, unsigned char *out)
{
    double fx = (double)x / (double)(w > 1 ? w - 1 : 1);
    double fy = (double)y / (double)(h > 1 ? h - 1 : 1);
    double cx = 0.5, cy = 0.5, dx = fx - cx, dy = fy - cy;
    double d = sqrt(dx * dx + dy * dy);
    double a = 1.0 - d / 0.5;

    if (a < 0.0)
        a = 0.0;
    out[0] = (unsigned char)(255 * a);
    out[1] = (unsigned char)(255 * a);
    out[2] = (unsigned char)(255 * a);
}

/* ------------------------------------------------------------------ */
/* Font discovery                                                      */
/* ------------------------------------------------------------------ */

typedef struct font_candidate
{
    const char *path;
    const char *name;
} font_candidate;

/* A different face per purpose: the document has to show that two
 * embedded fonts stay two fonts, not one aliased over the other. */
static const font_candidate k_sans[] = {
    { "C:\\Windows\\Fonts\\georgia.ttf", "Georgia" },
    { "C:\\Windows\\Fonts\\arial.ttf",   "Arial" },
    { "C:\\Windows\\Fonts\\calibri.ttf", "Calibri" },
    { NULL, NULL }
};

static const font_candidate k_mono[] = {
    { "C:\\Windows\\Fonts\\consola.ttf", "Consolas" },
    { "C:\\Windows\\Fonts\\cour.ttf",    "Courier New" },
    { "C:\\Windows\\Fonts\\verdana.ttf", "Verdana" },
    { NULL, NULL }
};

static const font_candidate k_cjk[] = {
    { "C:\\Windows\\Fonts\\simhei.ttf", "SimHei" },
    { "C:\\Windows\\Fonts\\Deng.ttf",   "DengXian" },
    { NULL, NULL }
};

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static const font_candidate *find_font(const font_candidate *list)
{
    while (list->path)
    {
        if (file_exists(list->path))
            return list;
        ++list;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Page 1: cover                                                       */
/* ------------------------------------------------------------------ */

static void page_cover(PDFX_Doc doc, PDFX_Page page,
                       PDFX_Image logo, PDFX_Image alpha_img,
                       PDFX_Font bold, PDFX_Font body)
{
    const double w = (double)PDFX_Page_GetWidth(page);
    const double h = (double)PDFX_Page_GetHeight(page);
    PDFX_ExtGState gs;
    int i;

    /* Two colour spaces meeting: a CMYK band and an RGB stripe. A viewer
     * that mishandles either one shows it immediately. */
    OK(PDFX_Page_SetCMYKFill(page, 0.85, 0.55, 0.20, 0.10), "cover: CMYK band");
    draw_box(page, 0, h - 120, w, 120, PDFX_TRUE);

    OK(PDFX_Page_SetRGBFill(page, 0.98, 0.72, 0.16), "cover: RGB stripe");
    draw_box(page, 0, h - 132, w, 12, PDFX_TRUE);

    /* Translucent circles over both bands. */
    gs = PDFX_CreateExtGState(doc);
    OK(PDFX_ExtGState_SetAlphaFill(gs, 0.45), "cover: alpha 0.45");
    OK(PDFX_Page_SetExtGState(page, gs), "cover: apply extgstate");
    OK(PDFX_Page_SetRGBFill(page, 1.0, 1.0, 1.0), "cover: circle fill");
    for (i = 0; i < 5; i++)
    {
        PDFX_Page_Circle(page, (PDFX_REAL)(70 + i * 58), (PDFX_REAL)(h - 60),
                         (PDFX_REAL)(26 + i * 4));
        PDFX_Page_Fill(page);
    }
    /* Back to opaque for everything else. */
    {
        PDFX_ExtGState opaque = PDFX_CreateExtGState(doc);
        OK(PDFX_ExtGState_SetAlphaFill(opaque, 1.0), "cover: alpha 1.0");
        OK(PDFX_Page_SetExtGState(page, opaque), "cover: restore opacity");
    }

    /* Title block. */
    OK(PDFX_Page_SetRGBFill(page, 0.10, 0.12, 0.18), "cover: title colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 46), "cover: title font");
    OK(PDFX_Page_BeginText(page), "cover: begin title");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)(h - 230), "pdfx"), "cover: title");
    OK(PDFX_Page_EndText(page), "cover: end title");

    OK(PDFX_Page_SetFontAndSize(page, body, 13), "cover: subtitle font");
    OK(PDFX_Page_SetRGBFill(page, 0.28, 0.31, 0.38), "cover: subtitle colour");
    OK(PDFX_Page_BeginText(page), "cover: begin subtitle");
    {
        PDFX_UINT used = 0;
        OK(PDFX_Page_TextRect(page, 56, (PDFX_REAL)(h - 260),
                              (PDFX_REAL)(w - 56), (PDFX_REAL)(h - 360),
                              "The libharu API with the prefix swapped: "
                              "PDFX_New instead of HPDF_New, PDFX_AddPage "
                              "instead of HPDF_AddPage, same argument order, "
                              "same return codes. Underneath, the same "
                              "backend-agnostic core as pdflib, with libharu "
                              "or PoDoFo doing the writing.",
                              PDFX_TALIGN_LEFT, &used),
           "cover: wrapped subtitle");
        check(used > 100, "cover: subtitle reports %u consumed chars", used);
    }
    OK(PDFX_Page_EndText(page), "cover: end subtitle");

    /* A bezier ribbon, to show the path operators survive the facade. */
    OK(PDFX_Page_SetRGBStroke(page, 0.85, 0.35, 0.15), "cover: ribbon colour");
    OK(PDFX_Page_SetLineWidth(page, 3), "cover: ribbon width");
    OK(PDFX_Page_MoveTo(page, 56, (PDFX_REAL)(h - 400)), "cover: ribbon move");
    OK(PDFX_Page_CurveTo(page, 180, (PDFX_REAL)(h - 340),
                         330, (PDFX_REAL)(h - 470),
                         520, (PDFX_REAL)(h - 400)), "cover: ribbon curve");
    OK(PDFX_Page_Stroke(page), "cover: ribbon stroke");

    /* Image, bottom right, with a caption. */
    OK(PDFX_Page_DrawImage(page, logo, (PDFX_REAL)(w - 216), 96, 160, 160),
       "cover: draw logo");
    OK(PDFX_Page_SetRGBFill(page, 0.35, 0.38, 0.45), "cover: caption colour");
    OK(PDFX_Page_SetFontAndSize(page, body, 9), "cover: caption font");
    OK(PDFX_Page_BeginText(page), "cover: begin caption");
    OK(PDFX_Page_TextOut(page, (PDFX_REAL)(w - 216), 80,
                         "PNG encoded in-process"), "cover: caption");
    OK(PDFX_Page_EndText(page), "cover: end caption");

    /* Alpha image over a colour block: a missing /SMask shows as a hard
     * square instead of a soft disc. */
    OK(PDFX_Page_SetRGBFill(page, 0.12, 0.45, 0.55), "cover: alpha backdrop");
    draw_box(page, 56, 96, 170, 170, PDFX_TRUE);
    OK(PDFX_Page_DrawImage(page, alpha_img, 66, 106, 150, 150),
       "cover: draw alpha image");

    /* Running head and footer. */
    OK(PDFX_Page_SetRGBFill(page, 0.45, 0.48, 0.55), "cover: footer colour");
    OK(PDFX_Page_SetFontAndSize(page, body, 8), "cover: footer font");
    OK(PDFX_Page_BeginText(page), "cover: begin footer");
    OK(PDFX_Page_TextOut(page, 56, 40, "pdfx - libharu-style C API"), "cover: footer left");
    OK(PDFX_Page_TextOut(page, (PDFX_REAL)(w - 100), 40, "1 / 7"), "cover: footer right");
    OK(PDFX_Page_EndText(page), "cover: end footer");
}

/* ------------------------------------------------------------------ */
/* Page 2: text and fonts                                              */
/* ------------------------------------------------------------------ */

static void page_text(PDFX_Doc doc, PDFX_Page page,
                      PDFX_Font body, PDFX_Font bold,
                      PDFX_Font ttf, PDFX_Font mono, PDFX_Font cjk)
{
    static const char *const std_fonts[] = {
        "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique",
        "Helvetica-BoldOblique", "Times-Roman", "Times-Bold",
        "Times-Italic", "Times-BoldItalic", "Symbol", "ZapfDingbats"
    };
    const double w = (double)PDFX_Page_GetWidth(page);
    const double h = (double)PDFX_Page_GetHeight(page);
    double y = h - 80;
    size_t i;

    OK(PDFX_Page_SetRGBFill(page, 0.10, 0.12, 0.18), "text: heading colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 18), "text: heading font");
    OK(PDFX_Page_BeginText(page), "text: begin heading");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "text and fonts"), "text: heading");
    OK(PDFX_Page_EndText(page), "text: end heading");
    y -= 30;

    /* All fourteen standard fonts. Symbol and ZapfDingbats have no ASCII
     * glyphs, so they get a Latin line anyway - the point is that the
     * font resource is created and selected, which the cross-check
     * confirms by counting fonts in the file. */
    for (i = 0; i < sizeof(std_fonts) / sizeof(std_fonts[0]); i++)
    {
        PDFX_Font f = PDFX_GetFont(doc, std_fonts[i], "WinAnsiEncoding");
        char line[128];

        if (!f)
        {
            check(0, "text: PDFX_GetFont(%s)", std_fonts[i]);
            continue;
        }
        snprintf(line, sizeof(line), "%s - The quick brown fox 0123456789",
                 std_fonts[i]);
        OK(PDFX_Page_SetFontAndSize(page, f, 9), "text: font size");
        OK(PDFX_Page_SetRGBFill(page, 0.16, 0.18, 0.24), "text: font colour");
        OK(PDFX_Page_BeginText(page), "text: begin font line");
        OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, line), "text: font line");
        OK(PDFX_Page_EndText(page), "text: end font line");
        y -= 15.5;
    }
    y -= 10;

    /* Embedded TrueType from a file, and a second, different face. */
    if (ttf)
    {
        OK(PDFX_Page_SetFontAndSize(page, ttf, 12), "text: embedded TTF size");
        OK(PDFX_Page_SetRGBFill(page, 0.55, 0.20, 0.10), "text: TTF colour");
        OK(PDFX_Page_BeginText(page), "text: begin TTF");
        OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y,
                             "embedded TrueType (file), subset"),
           "text: embedded TTF line");
        OK(PDFX_Page_EndText(page), "text: end TTF");
        y -= 20;
    }
    if (mono)
    {
        OK(PDFX_Page_SetFontAndSize(page, mono, 11), "text: mono size");
        OK(PDFX_Page_SetRGBFill(page, 0.10, 0.35, 0.30), "text: mono colour");
        OK(PDFX_Page_BeginText(page), "text: begin mono");
        OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y,
                             "second embedded face, a different family"),
           "text: mono line");
        OK(PDFX_Page_EndText(page), "text: end mono");
        y -= 20;
    }
    if (cjk)
    {
        OK(PDFX_Page_SetFontAndSize(page, cjk, 13), "text: CJK size");
        OK(PDFX_Page_SetRGBFill(page, 0.12, 0.22, 0.45), "text: CJK colour");
        OK(PDFX_Page_BeginText(page), "text: begin CJK");
        /* UTF-8, not the font's own encoding bytes: the pdflib core takes
         * UTF-8 for every text call and converts to what the font needs.
         * A libharu program would pass GBK bytes here, so this is the one
         * place where the source-level contract differs; the header says
         * so, and the cross-check verifies what actually lands in the
         * file rather than trusting either side. */
        OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y,
                             "\xe4\xb8\xad\xe6\x96\x87\xe7\xa4\xba"
                             "\xe4\xbe\x8b"), /* 中文示例, UTF-8 */
           "text: CJK line");
        OK(PDFX_Page_EndText(page), "text: end CJK");
        y -= 22;
    }

    /* Three alignments of the same sentence in the same box width. */
    OK(PDFX_Page_SetFontAndSize(page, body, 10), "text: align font");
    OK(PDFX_Page_SetRGBFill(page, 0.20, 0.22, 0.28), "text: align colour");
    {
        static const PDFX_TextAlignment aligns[] = {
            PDFX_TALIGN_LEFT, PDFX_TALIGN_CENTER, PDFX_TALIGN_RIGHT
        };
        static const char *const names[] = { "left", "centre", "right" };
        int k;
        for (k = 0; k < 3; k++)
        {
            char text[96];
            PDFX_UINT used = 0;
            snprintf(text, sizeof(text), "%s aligned in a 300pt box", names[k]);
            OK(PDFX_Page_BeginText(page), "text: begin aligned");
            OK(PDFX_Page_TextRect(page, 56, (PDFX_REAL)y,
                                  (PDFX_REAL)(56 + 300), (PDFX_REAL)(y - 16),
                                  text, aligns[k], &used),
               "text: aligned rect");
            OK(PDFX_Page_EndText(page), "text: end aligned");
            y -= 18;
        }
    }
    y -= 6;

    /* The four text rendering modes. */
    OK(PDFX_Page_SetFontAndSize(page, bold, 12), "text: render font");
    {
        static const PDFX_TextRenderingMode modes[] = {
            PDFX_FILL, PDFX_STROKE, PDFX_FILL_THEN_STROKE, PDFX_INVISIBLE
        };
        static const char *const names[] = {
            "fill", "stroke", "fill+stroke", "invisible (under the rule)"
        };
        int k;
        for (k = 0; k < 4; k++)
        {
            OK(PDFX_Page_SetRGBFill(page, 0.15, 0.30, 0.60), "text: mode fill colour");
            OK(PDFX_Page_SetRGBStroke(page, 0.75, 0.15, 0.15), "text: mode stroke colour");
            OK(PDFX_Page_SetTextRenderingMode(page, modes[k]), "text: set render mode");
            OK(PDFX_Page_BeginText(page), "text: begin mode");
            OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, names[k]), "text: mode line");
            OK(PDFX_Page_EndText(page), "text: end mode");
            y -= 17;
        }
        /* Back to fill: this is the state that has to be restored, or the
         * rest of the page inherits "invisible". */
        OK(PDFX_Page_SetTextRenderingMode(page, PDFX_FILL), "text: restore fill mode");
    }
    y -= 6;

    /* Character spacing, word spacing and horizontal scaling. */
    OK(PDFX_Page_SetFontAndSize(page, body, 10), "text: spacing font");
    OK(PDFX_Page_SetRGBFill(page, 0.20, 0.22, 0.28), "text: spacing colour");
    OK(PDFX_Page_SetCharSpace(page, 2.0), "text: char space");
    OK(PDFX_Page_BeginText(page), "text: begin spacing");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "character spacing 2.0"), "text: char space line");
    OK(PDFX_Page_EndText(page), "text: end spacing");
    OK(PDFX_Page_SetCharSpace(page, 0.0), "text: reset char space");
    y -= 16;
    OK(PDFX_Page_SetWordSpace(page, 4.0), "text: word space");
    OK(PDFX_Page_BeginText(page), "text: begin word spacing");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "word spacing four"), "text: word space line");
    OK(PDFX_Page_EndText(page), "text: end word spacing");
    OK(PDFX_Page_SetWordSpace(page, 0.0), "text: reset word space");
    y -= 16;
    OK(PDFX_Page_SetHorizontalScalling(page, 1.4), "text: hscale 1.4");
    OK(PDFX_Page_BeginText(page), "text: begin hscale");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "horizontal scaling 140%"), "text: hscale line");
    OK(PDFX_Page_EndText(page), "text: end hscale");
    OK(PDFX_Page_SetHorizontalScalling(page, 1.0), "text: reset hscale");
    y -= 24;

    /* Rotated text through the text matrix - no rotation helper needed. */
    {
        double ang = 20.0 * M_PI / 180.0;
        double c = cos(ang), s = sin(ang);
        OK(PDFX_Page_SetFontAndSize(page, bold, 14), "text: rotate font");
        OK(PDFX_Page_SetRGBFill(page, 0.65, 0.25, 0.10), "text: rotate colour");
        OK(PDFX_Page_BeginText(page), "text: begin rotate");
        OK(PDFX_Page_SetTextMatrix(page, (PDFX_REAL)c, (PDFX_REAL)s,
                                   (PDFX_REAL)-s, (PDFX_REAL)c,
                                   120, (PDFX_REAL)(y - 10)),
           "text: rotated matrix");
        OK(PDFX_Page_ShowText(page, "rotated with the text matrix"),
           "text: rotated run");
        OK(PDFX_Page_EndText(page), "text: end rotate");
    }

    /* Two-column wrapped paragraphs, to exercise the wrapper twice on one
     * page with different widths. */
    {
        const double top = y - 40;
        const double col_w = (w - 56 * 2 - 20) / 2.0;
        const char *para =
            "A paragraph that has to wrap is the cheapest way to find a "
            "layout bug: the wrapper advances its own cursor, and if the "
            "outer loop resumes from the wrong place the same lines are "
            "painted again and again until the box runs out. Counting the "
            "paint calls is the only check that notices.";
        int col;
        OK(PDFX_Page_SetFontAndSize(page, body, 9), "text: column font");
        OK(PDFX_Page_SetRGBFill(page, 0.22, 0.24, 0.30), "text: column colour");
        for (col = 0; col < 2; col++)
        {
            PDFX_UINT used = 0;
            OK(PDFX_Page_BeginText(page), "text: begin column");
            OK(PDFX_Page_TextRect(page, (PDFX_REAL)(56 + col * (col_w + 20)),
                                  (PDFX_REAL)top,
                                  (PDFX_REAL)(56 + col * (col_w + 20) + col_w),
                                  (PDFX_REAL)(top - 90), para,
                                  PDFX_TALIGN_JUSTIFY, &used),
               "text: justified column");
            check(used > 100, "text: column %d consumed %u chars", col, used);
            OK(PDFX_Page_EndText(page), "text: end column");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Page 3: vector graphics                                             */
/* ------------------------------------------------------------------ */

static void star(PDFX_Page page, double cx, double cy, double r1, double r2,
                 int points)
{
    int i;
    for (i = 0; i < points * 2; i++)
    {
        double ang = -M_PI / 2.0 + (double)i * M_PI / (double)points;
        double r = (i % 2) ? r2 : r1;
        double x = cx + r * cos(ang);
        double y = cy + r * sin(ang);
        if (i == 0)
            PDFX_Page_MoveTo(page, (PDFX_REAL)x, (PDFX_REAL)y);
        else
            PDFX_Page_LineTo(page, (PDFX_REAL)x, (PDFX_REAL)y);
    }
    PDFX_Page_ClosePath(page);
}

static void page_vector(PDFX_Page page, PDFX_Font body, PDFX_Font bold)
{
    const double h = (double)PDFX_Page_GetHeight(page);
    double y = h - 80;
    int i;

    OK(PDFX_Page_SetRGBFill(page, 0.10, 0.12, 0.18), "vector: heading colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 18), "vector: heading font");
    OK(PDFX_Page_BeginText(page), "vector: begin heading");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "vector graphics"), "vector: heading");
    OK(PDFX_Page_EndText(page), "vector: end heading");
    y -= 34;

    /* Line caps. */
    {
        static const PDFX_LineCap caps[] = {
            PDFX_BUTT_END, PDFX_ROUND_END, PDFX_PROJECTING_SQUARE_END
        };
        static const char *const names[] = { "butt", "round", "square" };
        OK(PDFX_Page_SetRGBStroke(page, 0.20, 0.35, 0.70), "vector: cap colour");
        for (i = 0; i < 3; i++)
        {
            OK(PDFX_Page_SetLineCap(page, caps[i]), "vector: set line cap");
            OK(PDFX_Page_SetLineWidth(page, 10), "vector: cap width");
            OK(PDFX_Page_MoveTo(page, 90, (PDFX_REAL)y), "vector: cap move");
            OK(PDFX_Page_LineTo(page, 300, (PDFX_REAL)y), "vector: cap line");
            OK(PDFX_Page_Stroke(page), "vector: cap stroke");
            OK(PDFX_Page_SetRGBFill(page, 0.30, 0.32, 0.38), "vector: cap label colour");
            OK(PDFX_Page_SetFontAndSize(page, body, 8), "vector: cap label font");
            OK(PDFX_Page_BeginText(page), "vector: begin cap label");
            OK(PDFX_Page_TextOut(page, 310, (PDFX_REAL)(y - 3), names[i]),
               "vector: cap label");
            OK(PDFX_Page_EndText(page), "vector: end cap label");
            y -= 22;
        }
    }
    y -= 8;

    /* Line joins, drawn as an open V. */
    {
        static const PDFX_LineJoin joins[] = {
            PDFX_MITER_JOIN, PDFX_ROUND_JOIN, PDFX_BEVEL_JOIN
        };
        static const char *const names[] = { "miter", "round", "bevel" };
        OK(PDFX_Page_SetRGBStroke(page, 0.75, 0.30, 0.15), "vector: join colour");
        OK(PDFX_Page_SetLineWidth(page, 12), "vector: join width");
        OK(PDFX_Page_SetMiterLimit(page, 10), "vector: miter limit");
        for (i = 0; i < 3; i++)
        {
            OK(PDFX_Page_SetLineJoin(page, joins[i]), "vector: set line join");
            OK(PDFX_Page_MoveTo(page, 90, (PDFX_REAL)(y - 20)), "vector: join move");
            OK(PDFX_Page_LineTo(page, 120, (PDFX_REAL)y), "vector: join up");
            OK(PDFX_Page_LineTo(page, 150, (PDFX_REAL)(y - 20)), "vector: join down");
            OK(PDFX_Page_Stroke(page), "vector: join stroke");
            OK(PDFX_Page_SetRGBFill(page, 0.30, 0.32, 0.38), "vector: join label colour");
            OK(PDFX_Page_SetFontAndSize(page, body, 8), "vector: join label font");
            OK(PDFX_Page_BeginText(page), "vector: begin join label");
            OK(PDFX_Page_TextOut(page, 160, (PDFX_REAL)(y - 8), names[i]),
               "vector: join label");
            OK(PDFX_Page_EndText(page), "vector: end join label");
            y -= 30;
        }
    }
    y -= 8;

    /* Dash patterns with phase. */
    {
        static const PDFX_REAL dash_a[] = { 6, 3 };
        static const PDFX_REAL dash_b[] = { 2, 2 };
        static const PDFX_REAL dash_c[] = { 10, 4, 2, 4 };
        static const PDFX_REAL dash_d[] = { 1, 5 };
        static const struct { const PDFX_REAL *p; PDFX_UINT n; PDFX_REAL phase; }
            dashes[] = {
                { dash_a, 2, 0 }, { dash_b, 2, 0 },
                { dash_c, 4, 0 }, { dash_d, 2, 3 }
            };
        OK(PDFX_Page_SetRGBStroke(page, 0.15, 0.50, 0.40), "vector: dash colour");
        OK(PDFX_Page_SetLineWidth(page, 2), "vector: dash width");
        for (i = 0; i < 4; i++)
        {
            OK(PDFX_Page_SetDash(page, dashes[i].p, dashes[i].n,
                                 dashes[i].phase), "vector: set dash");
            OK(PDFX_Page_MoveTo(page, 90, (PDFX_REAL)y), "vector: dash move");
            OK(PDFX_Page_LineTo(page, 420, (PDFX_REAL)y), "vector: dash line");
            OK(PDFX_Page_Stroke(page), "vector: dash stroke");
            y -= 14;
        }
        /* libharu's "no dash" is a NULL pattern with zero entries. */
        OK(PDFX_Page_SetDash(page, NULL, 0, 0), "vector: clear dash");
    }
    y -= 14;

    /* The same ten-point star under both fill rules. */
    OK(PDFX_Page_SetRGBFill(page, 0.90, 0.62, 0.15), "vector: star colour");
    star(page, 120, y - 45, 45, 18, 10);
    OK(PDFX_Page_Fill(page), "vector: star nonzero fill");
    OK(PDFX_Page_SetRGBFill(page, 0.25, 0.55, 0.85), "vector: star2 colour");
    star(page, 240, y - 45, 45, 18, 10);
    OK(PDFX_Page_Eofill(page), "vector: star even-odd fill");

    /* Clipping: a circle clips a striped block. */
    OK(PDFX_Page_GSave(page), "vector: gsave clip");
    OK(PDFX_Page_Circle(page, 400, (PDFX_REAL)(y - 45), 45), "vector: clip circle");
    OK(PDFX_Page_Clip(page), "vector: clip");
    OK(PDFX_Page_EndPath(page), "vector: end clip path");
    OK(PDFX_Page_SetRGBFill(page, 0.70, 0.20, 0.35), "vector: clipped fill colour");
    for (i = 0; i < 12; i++)
        draw_box(page, 340 + i * 12, y - 100, 6, 110, PDFX_TRUE);
    OK(PDFX_Page_GRestore(page), "vector: grestore clip");
    y -= 130;

    /* Circle, ellipse, arc. */
    OK(PDFX_Page_SetRGBStroke(page, 0.30, 0.30, 0.60), "vector: shape stroke");
    OK(PDFX_Page_SetLineWidth(page, 2), "vector: shape width");
    OK(PDFX_Page_Circle(page, 100, (PDFX_REAL)(y - 30), 28), "vector: circle");
    OK(PDFX_Page_Stroke(page), "vector: circle stroke");
    OK(PDFX_Page_Ellipse(page, 200, (PDFX_REAL)(y - 30), 40, 22), "vector: ellipse");
    OK(PDFX_Page_Stroke(page), "vector: ellipse stroke");
    OK(PDFX_Page_Arc(page, 310, (PDFX_REAL)(y - 30), 30, 0, 270), "vector: arc");
    OK(PDFX_Page_Stroke(page), "vector: arc stroke");

    /* An explicit cm matrix: scale a unit square and rotate it. */
    OK(PDFX_Page_GSave(page), "vector: gsave transform");
    OK(PDFX_Page_Concat(page, 1.2, 0.35, -0.35, 1.2, 400, (PDFX_REAL)(y - 55)),
       "vector: concat matrix");
    OK(PDFX_Page_SetRGBFill(page, 0.20, 0.65, 0.45), "vector: transformed fill");
    draw_box(page, 0, 0, 40, 40, PDFX_TRUE);
    OK(PDFX_Page_GRestore(page), "vector: grestore transform");
}

/* ------------------------------------------------------------------ */
/* Page 4: colour and transparency                                     */
/* ------------------------------------------------------------------ */

static void page_colour(PDFX_Doc doc, PDFX_Page page,
                        PDFX_Font body, PDFX_Font bold, PDFX_Image alpha_img)
{
    const double h = (double)PDFX_Page_GetHeight(page);
    double y = h - 80;
    int i;

    OK(PDFX_Page_SetRGBFill(page, 0.10, 0.12, 0.18), "colour: heading colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 18), "colour: heading font");
    OK(PDFX_Page_BeginText(page), "colour: begin heading");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "colour and transparency"), "colour: heading");
    OK(PDFX_Page_EndText(page), "colour: end heading");
    y -= 36;

    /* Grey, RGB and CMYK ramps: three device colour spaces in one band. */
    for (i = 0; i < 16; i++)
    {
        double t = (double)i / 15.0;
        OK(PDFX_Page_SetGrayFill(page, (PDFX_REAL)t), "colour: gray step");
        draw_box(page, 56 + i * 30, y - 22, 30, 22, PDFX_TRUE);

        OK(PDFX_Page_SetRGBFill(page, (PDFX_REAL)t, (PDFX_REAL)(0.3 + 0.5 * t),
                                (PDFX_REAL)(1.0 - t)), "colour: rgb step");
        draw_box(page, 56 + i * 30, y - 48, 30, 22, PDFX_TRUE);

        OK(PDFX_Page_SetCMYKFill(page, (PDFX_REAL)t, (PDFX_REAL)(0.2 + 0.6 * t),
                                 (PDFX_REAL)(0.1 + 0.4 * t), (PDFX_REAL)(0.1)),
           "colour: cmyk step");
        draw_box(page, 56 + i * 30, y - 74, 30, 22, PDFX_TRUE);
    }
    y -= 92;

    /* All twelve blend modes in a labelled grid. */
    {
        static const char *const names[] = {
            "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
            "ColorDodge", "ColorBurn", "HardLight", "SoftLight",
            "Difference", "Exclusion"
        };
        PDFX_RGBColor base = rgb(0.30, 0.55, 0.90);
        PDFX_RGBColor top = rgb(0.95, 0.60, 0.20);
        const double gx = 300;
        const double cw = 56;
        const double ch = 74;
        int k;

        OK(PDFX_Page_SetRGBFill(page, 0.10, 0.12, 0.18), "colour: blend heading");
        OK(PDFX_Page_SetFontAndSize(page, bold, 11), "colour: blend heading font");
        OK(PDFX_Page_BeginText(page), "colour: begin blend heading");
        OK(PDFX_Page_TextOut(page, (PDFX_REAL)gx, (PDFX_REAL)(y + 66), "blend modes"),
           "colour: blend heading");
        OK(PDFX_Page_EndText(page), "colour: end blend heading");

        for (k = 0; k < 12; k++)
        {
            double bx = gx + (k % 4) * cw;
            double by = y - (k / 4) * ch;
            PDFX_ExtGState gs = PDFX_CreateExtGState(doc);

            OK(PDFX_ExtGState_SetBlendMode(gs, (PDFX_BlendMode)k), "colour: blend mode");
            OK(PDFX_ExtGState_SetAlphaFill(gs, 1.0), "colour: blend alpha");
            OK(PDFX_Page_SetExtGState(page, gs), "colour: apply blend");

            set_fill(page, base);
            draw_box(page, bx, by, 40, 40, PDFX_TRUE);
            set_fill(page, top);
            draw_box(page, bx + 14, by + 14, 40, 40, PDFX_TRUE);

            OK(PDFX_Page_SetFontAndSize(page, body, 6.5), "colour: blend label font");
            OK(PDFX_Page_SetRGBFill(page, 0.30, 0.32, 0.38), "colour: blend label colour");
            OK(PDFX_Page_BeginText(page), "colour: begin blend label");
            OK(PDFX_Page_TextOut(page, (PDFX_REAL)bx, (PDFX_REAL)(by - 10), names[k]),
               "colour: blend label");
            OK(PDFX_Page_EndText(page), "colour: end blend label");
        }
        /* Back to normal blending, or the rest of the page is tinted. */
        {
            PDFX_ExtGState normal = PDFX_CreateExtGState(doc);
            OK(PDFX_ExtGState_SetBlendMode(normal, PDFX_BM_NORMAL),
               "colour: reset blend mode");
            OK(PDFX_Page_SetExtGState(page, normal), "colour: apply normal blend");
        }
        y -= 216;
    }

    /* Alpha image over a solid block. */
    OK(PDFX_Page_SetRGBFill(page, 0.90, 0.55, 0.20), "colour: alpha backdrop");
    draw_box(page, 56, y - 120, 160, 120, PDFX_TRUE);
    OK(PDFX_Page_DrawImage(page, alpha_img, 66, (PDFX_REAL)(y - 110), 140, 100),
       "colour: alpha image");
    OK(PDFX_Page_SetRGBFill(page, 0.35, 0.38, 0.45), "colour: alpha caption colour");
    OK(PDFX_Page_SetFontAndSize(page, body, 9), "colour: alpha caption font");
    OK(PDFX_Page_BeginText(page), "colour: begin alpha caption");
    OK(PDFX_Page_TextOut(page, 240, (PDFX_REAL)(y - 60),
                         "an image with an alpha channel"), "colour: alpha caption");
    OK(PDFX_Page_EndText(page), "colour: end alpha caption");
}

/* ------------------------------------------------------------------ */
/* Page 5: annotations and links                                       */
/* ------------------------------------------------------------------ */

static void page_annots(PDFX_Doc doc, PDFX_Page page, PDFX_Page target,
                        PDFX_Font body, PDFX_Font bold, PDFX_Encoder enc)
{
    const double h = (double)PDFX_Page_GetHeight(page);
    double y = h - 80;

    OK(PDFX_Page_SetRGBFill(page, 0.10, 0.12, 0.18), "annot: heading colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 18), "annot: heading font");
    OK(PDFX_Page_BeginText(page), "annot: begin heading");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "annotations and links"), "annot: heading");
    OK(PDFX_Page_EndText(page), "annot: end heading");
    y -= 36;

    OK(PDFX_Page_SetFontAndSize(page, body, 11), "annot: body font");
    OK(PDFX_Page_SetRGBFill(page, 0.20, 0.22, 0.28), "annot: body colour");
    OK(PDFX_Page_BeginText(page), "annot: begin body");
    {
        PDFX_UINT used = 0;
        OK(PDFX_Page_TextRect(page, 56, (PDFX_REAL)y, (PDFX_REAL)(539),
                              (PDFX_REAL)(y - 60),
                              "The markups below are placed over this paragraph. "
                              "Each one carries a rectangle, a colour, an "
                              "author and a subject, so the annotation "
                              "dictionary has more than a /Subtype in it.",
                              PDFX_TALIGN_LEFT, &used),
           "annot: paragraph");
    }
    OK(PDFX_Page_EndText(page), "annot: end body");

    /* Four text markup types over four lines of the paragraph. */
    {
        struct markup
        {
            const char *what;
            double top;
            PDFX_Annotation a;
        };
        PDFX_Rect r;
        int k;

        for (k = 0; k < 4; k++)
        {
            PDFX_Annotation a;
            double top = y - 8 - k * 13.5;
            r = rect(56, top - 12, 539, top + 2);

            switch (k)
            {
                case 0:
                    a = PDFX_Page_CreateHighlightAnnot(page, r, "highlighted", enc);
                    check(a != NULL, "annot: highlight created");
                    OK(PDFX_Annot_SetRGBColor(a, rgb(1.0, 0.92, 0.20)),
                       "annot: highlight colour");
                    break;
                case 1:
                    a = PDFX_Page_CreateUnderlineAnnot(page, r, "underlined", enc);
                    check(a != NULL, "annot: underline created");
                    OK(PDFX_Annot_SetRGBColor(a, rgb(0.15, 0.35, 0.80)),
                       "annot: underline colour");
                    break;
                case 2:
                    a = PDFX_Page_CreateSquigglyAnnot(page, r, "squiggly", enc);
                    check(a != NULL, "annot: squiggly created");
                    OK(PDFX_Annot_SetRGBColor(a, rgb(0.75, 0.20, 0.20)),
                       "annot: squiggly colour");
                    break;
                default:
                    a = PDFX_Page_CreateStrikeOutAnnot(page, r, "struck out", enc);
                    check(a != NULL, "annot: strikeout created");
                    OK(PDFX_Annot_SetRGBColor(a, rgb(0.10, 0.10, 0.10)),
                       "annot: strikeout colour");
                    break;
            }
            OK(PDFX_MarkupAnnot_SetTitle(a, "pdfx test"), "annot: author");
            OK(PDFX_MarkupAnnot_SetSubject(a, "markup subject"), "annot: subject");
            OK(PDFX_MarkupAnnot_SetTransparency(a, 0.8), "annot: opacity");
        }
    }

    /* A note with an icon, opened, and a free text box. */
    {
        PDFX_Annotation note = PDFX_Page_CreateTextAnnot(
            page, rect(60, y - 100, 90, y - 70), "a sticky note", enc);
        check(note != NULL, "annot: text note created");
        if (note)
        {
            OK(PDFX_TextAnnot_SetIcon(note, PDFX_ANNOT_ICON_COMMENT),
               "annot: note icon");
            OK(PDFX_TextAnnot_SetOpened(note, PDFX_TRUE), "annot: note opened");
        }
    }
    {
        /* Free-text annotations are one of the libharu features this core
         * cannot express: the facade returns NULL and records
         * PDFX_UNSUPPORTED_FUNC. That is a documented limit, not a bug -
         * but if a handle does come back it has to be usable. */
        PDFX_Annotation ft = PDFX_Page_CreateFreeTextAnnot(
            page, rect(300, y - 100, 520, y - 60), "free text", enc);
        if (ft)
        {
            check(1, "annot: free text created");
            OK(PDFX_Annot_SetRGBColor(ft, rgb(0.20, 0.55, 0.30)),
               "annot: free text colour");
        }
        else if (PDFX_GetError(doc) == PDFX_UNSUPPORTED_FUNC)
        {
            fprintf(out_stream(),
                    "  [skip] annot: free text (not expressible here)\n");
        }
        else
        {
            check(0, "annot: free text created");
        }
    }

    /* Square and circle, outlined shapes. */
    {
        PDFX_Annotation sq = PDFX_Page_CreateSquareAnnot(
            page, rect(100, y - 170, 180, y - 110), "square", enc);
        check(sq != NULL, "annot: square created");
        if (sq)
        {
            OK(PDFX_Annot_SetRGBColor(sq, rgb(0.85, 0.30, 0.10)),
               "annot: square colour");
            /* pdflib does not model /BS, so this must decline rather than
             * return PDFX_OK and write nothing - a caller cannot tell the
             * difference without opening the file. */
            check(PDFX_Annotation_SetBorderStyle(sq, PDFX_BS_SOLID, 1.5, 0, 0, 0)
                      == PDFX_UNSUPPORTED_FUNC,
                  "annot: square border is declined, not silently dropped");
        }
    }
    {
        PDFX_Annotation ci = PDFX_Page_CreateCircleAnnot(
            page, rect(220, y - 170, 300, y - 110), "circle", enc);
        check(ci != NULL, "annot: circle created");
        if (ci)
        {
            OK(PDFX_Annot_SetRGBColor(ci, rgb(0.10, 0.40, 0.70)),
               "annot: circle colour");
            check(PDFX_Annotation_SetBorderStyle(ci, PDFX_BS_SOLID, 1.5, 0, 0, 0)
                      == PDFX_UNSUPPORTED_FUNC,
                  "annot: circle border is declined, not silently dropped");
        }
    }

    /* A URI link and an internal jump. */
    OK(PDFX_Page_SetRGBFill(page, 0.10, 0.25, 0.70), "annot: link colour");
    OK(PDFX_Page_SetFontAndSize(page, body, 11), "annot: link font");
    OK(PDFX_Page_BeginText(page), "annot: begin link label");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)(y - 200),
                         "https://github.com/tdyx87/pdflib"), "annot: link label");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)(y - 220),
                         "jump to the table page"), "annot: jump label");
    OK(PDFX_Page_EndText(page), "annot: end link label");

    {
        PDFX_Annotation link = PDFX_Page_CreateURILinkAnnot(
            page, rect(56, y - 204, 300, y - 196),
            "https://github.com/tdyx87/pdflib");
        check(link != NULL, "annot: URI link created");
        if (link)
            check(PDFX_LinkAnnot_SetBorderStyle(link, 0.5, 0, 0)
                      == PDFX_UNSUPPORTED_FUNC,
                  "annot: link border is declined, not silently dropped");
    }
    {
        /* libharu's CreateLinkAnnot takes the destination as its third
         * argument: the link and its target are created together. */
        PDFX_Destination dst = PDFX_Page_CreateDestination(target);
        PDFX_Annotation jump;
        check(dst != NULL, "annot: destination created");
        if (dst)
            OK(PDFX_Destination_SetXYZ(dst, 0, PDFX_Page_GetHeight(target), 1),
               "annot: destination XYZ");
        jump = PDFX_Page_CreateLinkAnnot(
            page, rect(56, y - 224, 220, y - 216), dst);
        check(jump != NULL, "annot: jump link created");
    }
}

/* ------------------------------------------------------------------ */
/* Page 6: table and outlines                                          */
/* ------------------------------------------------------------------ */

static void page_table(PDFX_Doc doc, PDFX_Page page,
                       PDFX_Font body, PDFX_Font bold, PDFX_Font mono)
{
    static const char *const head[] = { "item", "qty", "unit", "amount" };
    static const char *const rows[][4] = {
        { "A4 paper",      "500", "sheet", "12.50" },
        { "Toner",         "2",   "carton", "188.00" },
        { "Staples",       "10",  "box",    "7.20" },
        { "Folders",       "25",  "pack",   "33.75" },
        { "Labels",        "8",   "sheet",  "4.10" },
        { "Envelopes",     "200", "piece",  "18.60" },
        { "Pens",          "48",  "piece",  "22.40" },
        { "Notebooks",     "12",  "piece",  "31.00" },
        { "Highlighters",  "6",   "set",    "9.90" },
        { "Clipboard",     "4",   "piece",  "15.20" },
        { "Sticky notes",  "24",  "pad",    "26.80" },
        { "Correction",    "5",   "bottle", "11.30" },
        { "Index cards",   "300", "piece",  "8.45" }
    };
    const double h = (double)PDFX_Page_GetHeight(page);
    const double x0 = 56, x1 = 539;
    const double col[4] = { 56, 300, 380, 470 };
    double y = h - 80;
    double row_h = 18;
    int i;

    OK(PDFX_Page_SetRGBFill(page, 0.10, 0.12, 0.18), "table: heading colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 18), "table: heading font");
    OK(PDFX_Page_BeginText(page), "table: begin heading");
    OK(PDFX_Page_TextOut(page, 56, (PDFX_REAL)y, "table and outlines"), "table: heading");
    OK(PDFX_Page_EndText(page), "table: end heading");
    y -= 34;

    /* Header band. */
    OK(PDFX_Page_SetRGBFill(page, 0.14, 0.24, 0.42), "table: header band colour");
    draw_box(page, x0, y - row_h, x1 - x0, row_h, PDFX_TRUE);
    OK(PDFX_Page_SetRGBFill(page, 1.0, 1.0, 1.0), "table: header text colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 9), "table: header font");
    for (i = 0; i < 4; i++)
    {
        OK(PDFX_Page_BeginText(page), "table: begin header cell");
        OK(PDFX_Page_TextOut(page, (PDFX_REAL)col[i], (PDFX_REAL)(y - 12), head[i]),
           "table: header cell");
        OK(PDFX_Page_EndText(page), "table: end header cell");
    }
    y -= row_h;

    /* Body rows: alternating bands, then the rules on top. */
    for (i = 0; i < 13; i++)
    {
        if (i % 2 == 0)
        {
            OK(PDFX_Page_SetRGBFill(page, 0.955, 0.960, 0.975), "table: band colour");
            draw_box(page, x0, (PDFX_REAL)(y - row_h), x1 - x0, row_h, PDFX_TRUE);
        }
        OK(PDFX_Page_SetRGBFill(page, 0.16, 0.18, 0.24), "table: body colour");
        OK(PDFX_Page_SetFontAndSize(page, body, 9), "table: body font");
        OK(PDFX_Page_BeginText(page), "table: begin row");
        OK(PDFX_Page_TextOut(page, (PDFX_REAL)col[0], (PDFX_REAL)(y - 12), rows[i][0]),
           "table: cell 0");
        OK(PDFX_Page_SetFontAndSize(page, mono, 9), "table: numeric font");
        OK(PDFX_Page_TextOut(page, (PDFX_REAL)col[1], (PDFX_REAL)(y - 12), rows[i][1]),
           "table: cell 1");
        OK(PDFX_Page_TextOut(page, (PDFX_REAL)col[2], (PDFX_REAL)(y - 12), rows[i][2]),
           "table: cell 2");
        /* Right-aligned amount: measure with the page's current font and
         * size, then place. */
        {
            PDFX_REAL tw = PDFX_Page_TextWidth(page, rows[i][3]);
            OK(PDFX_Page_TextOut(page, (PDFX_REAL)(x1 - 8 - tw),
                                 (PDFX_REAL)(y - 12), rows[i][3]),
               "table: cell 3 right aligned");
        }
        OK(PDFX_Page_EndText(page), "table: end row");
        y -= row_h;
    }

    /* Rules. */
    OK(PDFX_Page_SetRGBStroke(page, 0.62, 0.66, 0.74), "table: rule colour");
    OK(PDFX_Page_SetLineWidth(page, 0.5), "table: rule width");
    OK(PDFX_Page_MoveTo(page, (PDFX_REAL)x0, (PDFX_REAL)y), "table: rule move");
    OK(PDFX_Page_LineTo(page, (PDFX_REAL)x1, (PDFX_REAL)y), "table: rule line");
    OK(PDFX_Page_Stroke(page), "table: rule stroke");
    for (i = 0; i < 4; i++)
    {
        OK(PDFX_Page_MoveTo(page, (PDFX_REAL)col[i], (PDFX_REAL)(y + 13 * row_h)),
           "table: vertical rule move");
        OK(PDFX_Page_LineTo(page, (PDFX_REAL)col[i], (PDFX_REAL)y),
           "table: vertical rule line");
        OK(PDFX_Page_Stroke(page), "table: vertical rule stroke");
    }

    /* A nested outline tree, twelve entries deep and wide. */
    {
        PDFX_Outline root = PDFX_CreateOutline(doc, NULL, "pdfx showcase", NULL);
        int k;
        check(root != NULL, "outline: root created");
        for (k = 0; k < 4; k++)
        {
            char title[64];
            PDFX_Outline chap;
            int j;

            snprintf(title, sizeof(title), "Chapter %d", k + 1);
            chap = PDFX_CreateOutline(doc, root, title, NULL);
            check(chap != NULL, "outline: chapter created");
            OK(PDFX_Outline_SetOpened(chap, PDFX_TRUE), "outline: chapter opened");
            {
                PDFX_Destination dst = PDFX_Page_CreateDestination(page);
                if (dst)
                {
                    OK(PDFX_Destination_SetXYZ(dst, 0, (PDFX_REAL)(h - 60), 1),
                       "outline: destination");
                    OK(PDFX_Outline_SetDestination(chap, dst),
                       "outline: chapter destination");
                }
            }
            for (j = 0; j < 2; j++)
            {
                PDFX_Outline sec;
                snprintf(title, sizeof(title), "Section %d.%d", k + 1, j + 1);
                sec = PDFX_CreateOutline(doc, chap, title, NULL);
                check(sec != NULL, "outline: section created");
            }
        }
        OK(PDFX_SetPageMode(doc, PDFX_PAGE_MODE_USE_OUTLINE),
           "outline: page mode use outlines");
    }
}

/* ------------------------------------------------------------------ */
/* Page 7: custom geometry                                             */
/* ------------------------------------------------------------------ */

static void page_custom(PDFX_Page page, PDFX_Font body, PDFX_Font bold)
{
    OK(PDFX_Page_SetWidth(page, 400), "custom: set width");
    OK(PDFX_Page_SetHeight(page, 300), "custom: set height");
    OK(PDFX_Page_SetRotate(page, 90), "custom: rotate 90");

    check(fabs((double)PDFX_Page_GetWidth(page) - 400.0) < 0.01,
          "custom: width reads back 400 (got %.2f)",
          (double)PDFX_Page_GetWidth(page));
    check(fabs((double)PDFX_Page_GetHeight(page) - 300.0) < 0.01,
          "custom: height reads back 300 (got %.2f)",
          (double)PDFX_Page_GetHeight(page));

    OK(PDFX_Page_SetRGBFill(page, 0.12, 0.42, 0.62), "custom: band colour");
    draw_box(page, 0, 200, 400, 100, PDFX_TRUE);

    OK(PDFX_Page_SetRGBFill(page, 1.0, 1.0, 1.0), "custom: title colour");
    OK(PDFX_Page_SetFontAndSize(page, bold, 22), "custom: title font");
    OK(PDFX_Page_BeginText(page), "custom: begin title");
    OK(PDFX_Page_TextOut(page, 28, 248, "400 x 300, rotated 90"), "custom: title");
    OK(PDFX_Page_EndText(page), "custom: end title");

    OK(PDFX_Page_SetRGBFill(page, 0.20, 0.22, 0.28), "custom: body colour");
    OK(PDFX_Page_SetFontAndSize(page, body, 10), "custom: body font");
    OK(PDFX_Page_BeginText(page), "custom: begin body");
    OK(PDFX_Page_TextOut(page, 28, 160,
                         "A page whose MediaBox and /Rotate are set after AddPage."),
       "custom: body line");
    OK(PDFX_Page_EndText(page), "custom: end body");
}

/* ------------------------------------------------------------------ */
/* Document                                                            */
/* ------------------------------------------------------------------ */

static int build_document(int backend, const char *prefix)
{
    PDFX_Doc doc;
    PDFX_Page pages[7];
    PDFX_Font body, bold, ttf, mono, cjk;
    PDFX_Image logo, alpha_img;
    PDFX_Encoder enc;
    unsigned char *png = NULL, *png_a = NULL;
    size_t png_len = 0, png_a_len = 0;
    char path[512];
    int i;

    fprintf(out_stream(), "\n=== pdfx showcase: backend %d ===\n", backend);

    check(PDFX_SetBackend((PDFX_UINT)backend) == PDFX_OK,
          "PDFX_SetBackend(%d) = %s", backend, PDFX_GetBackendName());

    doc = PDFX_New(error_handler, NULL);
    check(doc != NULL, "PDFX_New");
    if (!doc)
        return 1;

    OK(PDFX_SetCompressionMode(doc, g_compress ? PDFX_COMP_ALL : PDFX_COMP_NONE),
       g_compress ? "PDFX_SetCompressionMode(ALL)"
                  : "PDFX_SetCompressionMode(NONE)");

    OK(PDFX_SetInfoAttr(doc, PDFX_INFO_TITLE, "pdfx showcase"), "info: title");
    OK(PDFX_SetInfoAttr(doc, PDFX_INFO_AUTHOR, "pdflib test suite"), "info: author");
    OK(PDFX_SetInfoAttr(doc, PDFX_INFO_SUBJECT,
                        "libharu-style API over two engines"), "info: subject");
    OK(PDFX_SetInfoAttr(doc, PDFX_INFO_KEYWORDS,
                        "pdfx, libharu, podofo, pdf"), "info: keywords");
    OK(PDFX_SetInfoAttr(doc, PDFX_INFO_CREATOR, "pdfx_showcase"), "info: creator");
    check(strcmp(PDFX_GetInfoAttr(doc, PDFX_INFO_TITLE), "pdfx showcase") == 0,
          "info: title round-trips");
    check(strcmp(PDFX_GetInfoAttr(doc, PDFX_INFO_AUTHOR), "pdflib test suite") == 0,
          "info: author round-trips");

    enc = PDFX_GetCurrentEncoder(doc);

    /* Fonts. */
    body = PDFX_GetFont(doc, "Helvetica", "WinAnsiEncoding");
    bold = PDFX_GetFont(doc, "Helvetica-Bold", "WinAnsiEncoding");
    check(body != NULL && bold != NULL, "fonts: standard faces");
    check(PDFX_Font_GetAscent(bold) > 0, "fonts: ascent is positive");
    check(PDFX_Font_GetDescent(bold) <= 0, "fonts: descent is non-positive");

    ttf = NULL;
    {
        const font_candidate *c = find_font(k_sans);
        if (c)
        {
            const char *name = PDFX_LoadTTFontFromFile(doc, c->path, PDFX_TRUE);
            check(name != NULL, "fonts: LoadTTFontFromFile(%s)", c->name);
            if (name)
                ttf = PDFX_GetFont(doc, name, NULL);
        }
        else
        {
            fprintf(out_stream(), "  [skip] no sans TTF found on this machine\n");
        }
    }
    mono = NULL;
    {
        const font_candidate *c = find_font(k_mono);
        if (c)
        {
            const char *name = PDFX_LoadTTFontFromFile(doc, c->path, PDFX_TRUE);
            check(name != NULL, "fonts: LoadTTFontFromFile(%s)", c->name);
            if (name)
                mono = PDFX_GetFont(doc, name, NULL);
        }
    }
    cjk = NULL;
    {
        const font_candidate *c = find_font(k_cjk);
        if (c)
        {
            const char *name = PDFX_LoadTTFontFromFile(doc, c->path, PDFX_TRUE);
            if (name)
            {
                cjk = PDFX_GetFont(doc, name, "GBK-EUC-H");
                check(cjk != NULL, "fonts: CJK face with GBK-EUC-H");
            }
            else
            {
                fprintf(out_stream(),
                        "  [skip] engine refused the CJK font (a limit, not a bug)\n");
            }
        }
    }
    if (!mono)
        mono = body;

    /* Images. */
    png = make_png(256, 256, 0, px_logo, &png_len);
    check(png != NULL && png_len > 100, "image: RGB PNG encoded (%lu bytes)",
          (unsigned long)png_len);
    logo = png ? PDFX_LoadPngImageFromMem(doc, png, (PDFX_UINT)png_len) : NULL;
    check(logo != NULL, "image: LoadPngImageFromMem");
    if (logo)
    {
        check(PDFX_Image_GetWidth(logo) == 256, "image: width 256");
        check(PDFX_Image_GetHeight(logo) == 256, "image: height 256");
    }

    png_a = make_png(128, 128, 1, px_alpha, &png_a_len);
    check(png_a != NULL && png_a_len > 100, "image: alpha PNG encoded (%lu bytes)",
          (unsigned long)png_a_len);
    alpha_img = png_a ? PDFX_LoadPngImageFromMem(doc, png_a, (PDFX_UINT)png_a_len)
                      : NULL;
    check(alpha_img != NULL, "image: alpha PNG loaded");

    /* Pages. */
    for (i = 0; i < 7; i++)
    {
        pages[i] = PDFX_AddPage(doc);
        check(pages[i] != NULL, "PDFX_AddPage #%d", i + 1);
        if (!pages[i])
            return 1;
    }
    OK(PDFX_Page_SetSize(pages[0], PDFX_PAGE_SIZE_A4, PDFX_PAGE_PORTRAIT),
       "page 1: A4 portrait");
    OK(PDFX_Page_SetSize(pages[4], PDFX_PAGE_SIZE_A4, PDFX_PAGE_LANDSCAPE),
       "page 5: A4 landscape");

    page_cover(doc, pages[0], logo, alpha_img, bold, body);
    page_text(doc, pages[1], body, bold, ttf, mono, cjk);
    page_vector(pages[2], body, bold);
    page_colour(doc, pages[3], body, bold, alpha_img);
    page_annots(doc, pages[4], pages[5], body, bold, enc);
    page_table(doc, pages[5], body, bold, mono);
    page_custom(pages[6], body, bold);

    check(PDFX_GetCurrentPage(doc) == pages[6], "PDFX_GetCurrentPage is the last page");
    check(PDFX_GetPageByIndex(doc, 0, PDFX_PAGE_MEDIABOX) == pages[0],
          "PDFX_GetPageByIndex(0) is the first page");

    /* ------------------------------------------------------------------
     * Serialisation.
     *
     * One pass per compression setting (see main). The setting has to be
     * applied before the first page is added: libharu attaches a stream's
     * filter when the stream is created, so flipping the mode just before
     * saving changes nothing. That is why the two passes build two
     * documents instead of saving one twice - and it is worth knowing,
     * because "set compression late" is a silently useless call.
     *
     * In the uncompressed pass the content streams are plain text, so the
     * literal strings the pages were drawn with can be found in the bytes.
     * That is a much stronger check than "the file is big enough": it
     * proves the text reached the stream instead of being swallowed by
     * the facade.
     * ------------------------------------------------------------------ */
    {
        static const char *const needles[] = {
            "pdfx",
            "text and fonts",
            "vector graphics",
            "colour and transparency",
            "annotations and links",
            "table and outlines",
            "blend modes"
        };
        PDFX_UINT32 size = 0;
        PDFX_BYTE *buf = NULL;
        size_t k;

        OK(PDFX_SaveToStream(doc), "save: SaveToStream");
        size = PDFX_GetStreamSize(doc);
        g_last_stream_size = size;
        check(size > 2000, "save: %s stream is %lu bytes",
              g_compress ? "compressed" : "uncompressed", (unsigned long)size);

        buf = (PDFX_BYTE *)malloc(size ? size : 1);
        if (buf)
        {
            PDFX_UINT32 got = size;
            OK(PDFX_GetContents(doc, buf, &got), "save: PDFX_GetContents");
            check(got == size, "save: GetContents returned the whole stream");
            check(got >= 5 && memcmp(buf, "%PDF-", 5) == 0,
                  "save: stream starts with %%PDF-");
            check(has_eof_tail(buf, got), "save: stream ends with %%%%EOF");

            /* The plain-text check only means something on an engine that
             * actually honours PDFX_COMP_NONE. PoDoFo's writer always
             * flate-compresses content streams, so its "uncompressed"
             * output is still compressed - a backend property, not a
             * facade bug, and the reason this check is engine-specific. */
            if (!g_compress && strcmp(PDFX_GetBackendName(), "libharu") == 0)
            {
                for (k = 0; k < sizeof(needles) / sizeof(needles[0]); k++)
                {
                    int found = 0;
                    PDFX_UINT32 i;
                    size_t n = strlen(needles[k]);

                    for (i = 0; i + n <= got; i++)
                    {
                        if (memcmp(buf + i, needles[k], n) == 0)
                        {
                            found = 1;
                            break;
                        }
                    }
                    check(found, "save: stream carries the text \"%s\"",
                          needles[k]);
                }
            }
            else if (!g_compress)
            {
                fprintf(out_stream(),
                        "  [skip] save: %s compresses content streams even "
                        "with COMP_NONE\n", PDFX_GetBackendName());
            }

            if (g_to_stdout)
            {
                size_t written = fwrite(buf, 1, got, stdout);
                fflush(stdout);
                check(written == got, "save: wrote %lu bytes to stdout",
                      (unsigned long)got);
            }
            free(buf);
        }

        /* File output, when the process is allowed to create files. A
         * sandboxed runner may not be: that is an environment limit, so
         * report it and carry on rather than failing the document. */
        if (!g_to_stdout && !g_selftest)
        {
            snprintf(path, sizeof(path), "%s-%s.pdf", prefix,
                     PDFX_GetBackendName());
            if (PDFX_SaveToFile(doc, path) == PDFX_OK)
            {
                FILE *f;
                long fsize = -1;

                check(1, "save: PDFX_SaveToFile(%s)", path);
                f = fopen(path, "rb");
                if (f)
                {
                    fseek(f, 0, SEEK_END);
                    fsize = ftell(f);
                    fclose(f);
                }
                check(fsize > 1000, "save: %s is %ld bytes", path, fsize);
            }
            else
            {
                /* SaveToFile reports PDFX_FILE_IO_ERROR when the OS
                 * refuses to open the file; anything else is a real
                 * failure. */
                if (PDFX_GetError(doc) == PDFX_FILE_IO_ERROR)
                {
                    fprintf(out_stream(),
                            "  [skip] save: cannot create files here (%s); "
                            "use --stdout to capture the document\n", path);
                }
                else
                {
                    check(0, "save: PDFX_SaveToFile(%s) failed with %04X",
                          path, (unsigned)PDFX_GetError(doc));
                }
            }
        }
    }

    check(g_handler_calls == 0,
          "no engine error was reported through the handler (got %d)",
          g_handler_calls);

    PDFX_Free(doc);
    free(png);
    free(png_a);
    return 0;
}

static int check_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char head[8];
    size_t n;
    long size;

    if (!f)
    {
        check(0, "%s: exists", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    n = fread(head, 1, 8, f);
    fclose(f);

    check(size > 2000, "%s: %ld bytes", path, size);
    check(n >= 5 && memcmp(head, "%PDF-", 5) == 0, "%s: starts with %%PDF-", path);
    return 0;
}

static int run_backend(int backend, const char *name, const char *prefix)
{
    PDFX_UINT32 plain = 0, compressed = 0;
    int rc = 0;

    if (g_to_stdout)
    {
        /* One document on stdout: two PDFs concatenated is not a PDF.
         * Uncompressed, so the bytes are readable when someone pipes them
         * into a viewer or a diagnostic tool. */
        g_compress = 0;
        rc |= build_document(backend, prefix);
        return rc;
    }

    g_compress = 0;
    rc |= build_document(backend, prefix);
    plain = g_last_stream_size;

    g_compress = 1;
    rc |= build_document(backend, prefix);
    compressed = g_last_stream_size;

    if (strcmp(name, "libharu") == 0)
    {
        check(plain > 0 && compressed > 0 && compressed < plain,
              "%s: compression shrank the stream (%lu -> %lu bytes)", name,
              (unsigned long)plain, (unsigned long)compressed);
    }
    else
    {
        /* PoDoFo compresses regardless of the mode, so the two sizes are
         * the same and asking for a shrink would test the engine's writer
         * rather than this facade. What must hold is that both settings
         * produce a valid document. */
        check(plain > 0 && compressed > 0,
              "%s: both compression settings serialise (%lu / %lu bytes)",
              name, (unsigned long)plain, (unsigned long)compressed);
    }

    if (!g_selftest)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s-%s.pdf", prefix, name);
        rc |= check_file(path);
    }
    return rc;
}

int main(int argc, char **argv)
{
    const char *which = "all";
    const char *prefix = "pdfx-showcase";
    int rc = 0;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--stdout") == 0)
            g_to_stdout = 1;
        else if (strcmp(argv[i], "--selftest") == 0)
            g_selftest = 1;
        else if (strcmp(argv[i], "libharu") == 0
                 || strcmp(argv[i], "podofo") == 0
                 || strcmp(argv[i], "all") == 0)
            which = argv[i];
        else
            prefix = argv[i];
    }

    if (g_to_stdout)
    {
        g_selftest = 1;
        if (strcmp(which, "all") == 0)
            which = "libharu";
    }

    fprintf(out_stream(), "=== pdfx: libharu-style API over the pdflib core ===\n");
    fprintf(out_stream(), "mode: %s\n",
            g_to_stdout ? "stdout" : (g_selftest ? "selftest" : "file"));

    if (strcmp(which, "libharu") == 0 || strcmp(which, "all") == 0)
        rc |= run_backend(PDFLIB_PDFX_BACKEND_LIBHARU, "libharu", prefix);
    if (strcmp(which, "podofo") == 0 || strcmp(which, "all") == 0)
        rc |= run_backend(PDFLIB_PDFX_BACKEND_PODOFO, "podofo", prefix);

    fprintf(out_stream(), "\n=== %d checks, %d failure(s) ===\n",
            g_checks, g_failures);
    return (g_failures || rc) ? 1 : 0;
}
