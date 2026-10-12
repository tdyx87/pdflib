/**
 * pdf_inspect.h - a small read-back PDF inspector for the tests.
 *
 * The point of this file: "the call returned PDF_OK" proves nothing about
 * the bytes that came out. A writer can accept every drawing call and
 * still emit an empty page, drop the font, forget the image XObject or
 * write a content stream nobody can decode. To test pdflib properly the
 * tests have to read the produced file back and check that what was drawn
 * is actually in there.
 *
 * This is deliberately NOT a general PDF parser. It is the smallest thing
 * that can answer the questions the tests ask:
 *
 *   - which objects are in the file, and what are their dictionaries?
 *   - which of them are pages, and what is each page's decoded content?
 *   - does the content stream contain this operator / this text / how many
 *     times?
 *   - what does this key hold (int / name / reference / array)?
 *   - is the stream Flate-compressed, and does it decode to the same bytes
 *     the uncompressed save produced?
 *   - what are the actual pixel values of an image XObject (PNG predictors
 *     included)?
 *
 * It is header-only and every function is static, so including it costs
 * nothing at link time. It is used by apitest.c and can be reused by any
 * other self-checking consumer.
 *
 * Limitations, all deliberate:
 *   - objects stored inside object streams (/ObjStm) are invisible here;
 *     pdfi_has_object_streams() reports that so a test can say so instead
 *     of silently finding nothing.
 *   - no encryption support: an encrypted file cannot be inspected.
 *   - stream /Length must be a direct integer; when it is an indirect
 *     reference the scanner falls back to searching for "endstream".
 */
#ifndef PDFLIB_PDF_INSPECT_H
#define PDFLIB_PDF_INSPECT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>
#define PDFI_MAX_OBJS   8192
#define PDFI_MAX_PAGES  512

typedef struct pdfi_obj
{
    long num;           /* object number from "N G obj" */
    long dict_off;      /* offset of the dictionary text ("<<" ... ">>") */
    long dict_len;
    long stream_off;    /* offset of the stream bytes, -1 when there is none */
    long stream_len;    /* encoded length as written */
} pdfi_obj;

typedef struct pdfi_doc
{
    const unsigned char *buf;
    long n;
    pdfi_obj objs[PDFI_MAX_OBJS];
    int nobj;
} pdfi_doc;

/* ------------------------------------------------------------------ */
/* Byte helpers                                                        */
/* ------------------------------------------------------------------ */

static int pdfi_is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

/* Delimiters per the PDF spec, plus the two comment/escape starters. */
static int pdfi_is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static int pdfi_tok_is(const unsigned char *p, long len, const char *tok)
{
    size_t t = strlen(tok);

    return (size_t)len == t && memcmp(p, tok, t) == 0;
}

/* Is the token [off, off+len) a run of digits? Object numbers and
 * generation numbers are, which is what distinguishes a reference from,
 * say, the real 1.0 in "1.0 0 R". */
static int pdfi_tok_is_uint(const unsigned char *buf, long off, long len)
{
    long k;

    if (len <= 0)
        return 0;
    for (k = off; k < off + len; k++)
        if (buf[k] < '0' || buf[k] > '9')
            return 0;
    return 1;
}

/* Convert a digit run to a long. Unlike atol() this never reads past the
 * token, which matters because the buffer is a whole file and is not
 * NUL-terminated at the token boundary. */
static long pdfi_tok_to_long(const unsigned char *buf, long off, long len)
{
    long v = 0;
    long k;

    for (k = off; k < off + len; k++)
    {
        if (buf[k] < '0' || buf[k] > '9')
            break;
        v = v * 10 + (buf[k] - '0');
        if (v > 1000000000L)     /* clamp: no sane PDF object number */
            break;
    }
    return v;
}

/* Read one token starting at *pos. Returns the token length, 0 at the end
 * of the buffer. Comments are skipped. `*pos` is advanced past the token. */
static long pdfi_next_tok(const unsigned char *buf, long n, long *pos,
                          long *off)
{
    long start;

    while (*pos < n)
    {
        if (pdfi_is_ws(buf[*pos]))
        {
            (*pos)++;
            continue;
        }
        if (buf[*pos] == '%')
        {
            while (*pos < n && buf[*pos] != '\n' && buf[*pos] != '\r')
                (*pos)++;
            continue;
        }
        break;
    }
    if (*pos >= n)
        return 0;

    start = *pos;

    if (buf[*pos] == '<' && *pos + 1 < n && buf[*pos + 1] == '<')
    {
        *pos += 2;
        *off = start;
        return 2;
    }
    if (buf[*pos] == '>' && *pos + 1 < n && buf[*pos + 1] == '>')
    {
        *pos += 2;
        *off = start;
        return 2;
    }
    if (pdfi_is_delim(buf[*pos]))
    {
        (*pos)++;
        *off = start;
        return 1;
    }
    while (*pos < n && !pdfi_is_ws(buf[*pos]) && !pdfi_is_delim(buf[*pos]))
        (*pos)++;
    *off = start;
    return *pos - start;
}

/* Skip a literal string, a hex string, an array or a dictionary, starting
 * at the token that opens it. `*pos` must be at the opener.
 *
 * The dictionary and array branches are depth-tracked and recurse into
 * nested spans. That is not optional: a single "scan to the first '>'"
 * implementation stops at the INNER '>>' of << /A << /B 1 >> >>, leaving
 * the caller mid-dictionary. */
static void pdfi_skip_span(const unsigned char *buf, long n, long *pos)
{
    if (*pos >= n)
        return;

    if (buf[*pos] == '(')
    {
        /* Literal string: parentheses nest and '\' escapes the next byte. */
        int nest = 0;

        while (*pos < n)
        {
            if (buf[*pos] == '\\')
            {
                *pos += 2;
                continue;
            }
            if (buf[*pos] == '(')
                nest++;
            else if (buf[*pos] == ')')
            {
                nest--;
                if (nest == 0)
                {
                    (*pos)++;
                    return;
                }
            }
            (*pos)++;
        }
        return;
    }

    if (buf[*pos] == '<' && *pos + 1 < n && buf[*pos + 1] == '<')
    {
        /* Dictionary. */
        int depth = 1;

        *pos += 2;
        while (*pos < n && depth > 0)
        {
            if (buf[*pos] == '(' || buf[*pos] == '[')
                pdfi_skip_span(buf, n, pos);
            else if (buf[*pos] == '<' && *pos + 1 < n && buf[*pos + 1] == '<')
            {
                depth++;
                *pos += 2;
            }
            else if (buf[*pos] == '>' && *pos + 1 < n && buf[*pos + 1] == '>')
            {
                depth--;
                *pos += 2;
            }
            else
                (*pos)++;
        }
        return;
    }

    if (buf[*pos] == '<')
    {
        /* Hex string. */
        (*pos)++;
        while (*pos < n && buf[*pos] != '>')
            (*pos)++;
        if (*pos < n)
            (*pos)++;
        return;
    }

    if (buf[*pos] == '[')
    {
        /* Array. */
        int depth = 1;

        (*pos)++;
        while (*pos < n && depth > 0)
        {
            if (buf[*pos] == '(' || buf[*pos] == '<')
                pdfi_skip_span(buf, n, pos);
            else if (buf[*pos] == '[')
            {
                depth++;
                (*pos)++;
            }
            else if (buf[*pos] == ']')
            {
                depth--;
                (*pos)++;
            }
            else
                (*pos)++;
        }
        return;
    }

    /* Not an opener. Consume one byte so a caller looping on this cannot
     * spin forever. */
    (*pos)++;
}

/* ------------------------------------------------------------------ */
/* Object scan                                                         */
/* ------------------------------------------------------------------ */

/* Does "stream" appear here as a keyword (not as part of "endstream")? */
static int pdfi_at_stream_kw(const unsigned char *buf, long n, long i)
{
    if (i + 6 > n || memcmp(buf + i, "stream", 6) != 0)
        return 0;
    if (i >= 3 && memcmp(buf + i - 3, "end", 3) == 0)
        return 0;
    /* The keyword must be preceded by a delimiter or whitespace. */
    if (i > 0 && !pdfi_is_ws(buf[i - 1]) && !pdfi_is_delim(buf[i - 1]))
        return 0;
    return 1;
}

/* Find where an object body ends: either its "stream" keyword or its
 * "endobj". Returns the offset of whichever was found and sets
 * *is_stream. Returns n when neither is present.
 *
 * The walk skips strings, hex strings, arrays and dictionaries, which
 * matters twice over:
 *   - an "endobj" inside /Title (...) or /Contents (...) must not end the
 *     object early;
 *   - the result bounds the dictionary search. A plain "scan forward for
 *     <<" does not: an object with no dictionary at all (an array, or the
 *     bare integer that an indirect /Length points at) would run past its
 *     own endobj and adopt the NEXT object's dictionary, giving that
 *     object a negative dict_len and every key in the file a chance of
 *     being read from the wrong place. */
static long pdfi_body_end(const unsigned char *buf, long n, long start,
                          int *is_stream)
{
    long p = start;

    *is_stream = 0;

    while (p < n)
    {
        if (pdfi_is_ws(buf[p]))
        {
            p++;
            continue;
        }
        if (buf[p] == '%')
        {
            while (p < n && buf[p] != '\n' && buf[p] != '\r')
                p++;
            continue;
        }
        if (buf[p] == '(' || buf[p] == '[' || buf[p] == '<')
        {
            long save = p;

            pdfi_skip_span(buf, n, &p);
            if (p <= save)      /* no progress: never spin */
                p = save + 1;
            continue;
        }
        if (pdfi_at_stream_kw(buf, n, p))
        {
            *is_stream = 1;
            return p;
        }
        if (p + 6 <= n && memcmp(buf + p, "endobj", 6) == 0 &&
            (p + 6 >= n || pdfi_is_ws(buf[p + 6]) ||
             pdfi_is_delim(buf[p + 6])))
            return p;
        p++;
    }
    return n;
}

/* Scan "N G obj ... endobj" spans. Xref-independent on purpose: a broken
 * xref would still let this find the objects, which is what we want when
 * the question is "is the content there". */
static int pdfi_scan(const unsigned char *buf, long n, pdfi_doc *d)
{
    long i = 0;

    d->buf = buf;
    d->n = n;
    d->nobj = 0;

    while (i < n)
    {
        long p = i;
        long num;
        long dict_off, dict_end;

        /* Look for the literal "obj". */
        while (p + 3 <= n && memcmp(buf + p, "obj", 3) != 0)
            p++;
        if (p + 3 > n)
            break;

        /* Walk backwards: G, then N. */
        {
            long q = p;
            long g_end, n_end;

            /* previous token end = q (exclusive of whitespace) */
            g_end = q;
            while (g_end > 0 && pdfi_is_ws(buf[g_end - 1]))
                g_end--;
            {
                long g_start = g_end;
                while (g_start > 0 && !pdfi_is_ws(buf[g_start - 1]) &&
                       !pdfi_is_delim(buf[g_start - 1]))
                    g_start--;
                if (g_start == g_end)
                    { i = p + 3; continue; }

                n_end = g_start;
                while (n_end > 0 && pdfi_is_ws(buf[n_end - 1]))
                    n_end--;
                {
                    long n_start = n_end;
                    while (n_start > 0 && !pdfi_is_ws(buf[n_start - 1]) &&
                           !pdfi_is_delim(buf[n_start - 1]))
                        n_start--;
                    if (n_start == n_end)
                        { i = p + 3; continue; }

                    /* object number must be all digits */
                    {
                        long k;
                        int ok = 1;
                        for (k = n_start; k < n_end; k++)
                            if (buf[k] < '0' || buf[k] > '9')
                                { ok = 0; break; }
                        if (!ok)
                            { i = p + 3; continue; }
                        num = 0;
                        for (k = n_start; k < n_end && k < n_start + 9; k++)
                            num = num * 10 + (buf[k] - '0');
                    }
                }
            }
        }

        /* Find the end of the object body first: that bounds every search
         * below. */
        {
            long e;
            long stream_off = -1, stream_len = -1;
            long after = n;
            int is_stream = 0;

            e = pdfi_body_end(buf, n, p + 3, &is_stream);

            /* Body span. Normally this is the object's dictionary, but an
             * object whose body is a bare ARRAY (a /Kids or /Widths target
             * reached by reference) records the array instead. That keeps
             * pdfi_dict_get harmless - handed a '[' it consumes one value,
             * finds no key/value pair and reports the key absent - while
             * letting pdfi_dict_array_span read array OBJECTS as well as
             * inline arrays. */
            dict_off = p + 3;
            while (dict_off < e && pdfi_is_ws(buf[dict_off]))
                dict_off++;
            if (dict_off + 1 < e && buf[dict_off] == '<' &&
                buf[dict_off + 1] == '<')
            {
                dict_end = e;
            }
            else if (dict_off < e && buf[dict_off] == '[')
            {
                long q = dict_off;

                pdfi_skip_span(buf, n, &q);
                dict_end = (q > dict_off && q <= e) ? q : e;
            }
            else
            {
                dict_end = dict_off;    /* no dictionary and no array */
            }

            if (is_stream)
            {
                long s = e + 6;

                if (s < n && buf[s] == '\r')
                    s++;
                if (s < n && buf[s] == '\n')
                    s++;
                stream_off = s;

                /* Prefer the declared /Length. */
                {
                    long lend = -1;
                    long k;

                    for (k = dict_off; k + 7 < stream_off; k++)
                    {
                        if (memcmp(buf + k, "/Length", 7) == 0)
                        {
                            long v = k + 7;
                            long val = 0;
                            int digits = 0;

                            while (v < stream_off && pdfi_is_ws(buf[v]))
                                v++;
                            while (v < stream_off && buf[v] >= '0' && buf[v] <= '9')
                            {
                                val = val * 10 + (buf[v] - '0');
                                v++;
                                digits++;
                            }
                            if (digits)
                            {
                                /* Reject an indirect /Length. The value is
                                 * only a usable length when it is a plain
                                 * integer; "/Length 8 0 R" (which PoDoFo's
                                 * streamed writer emits) would otherwise be
                                 * read as the direct length 8, truncating
                                 * the stream and making every content
                                 * stream look undecodable. Detect the
                                 * "N G R" shape by looking past the
                                 * whitespace that a naive "is the number
                                 * glued to a non-delimiter" test happily
                                 * accepts. */
                                long w = v;
                                long g1s, g1e;
                                int indirect = 0;

                                while (w < stream_off && pdfi_is_ws(buf[w]))
                                    w++;
                                g1s = w;
                                while (w < stream_off && buf[w] >= '0' &&
                                       buf[w] <= '9')
                                    w++;
                                g1e = w;
                                if (g1e > g1s)
                                {
                                    long g2s, g2e;

                                    while (w < stream_off && pdfi_is_ws(buf[w]))
                                        w++;
                                    g2s = w;
                                    while (w < stream_off && !pdfi_is_ws(buf[w]) &&
                                           !pdfi_is_delim(buf[w]))
                                        w++;
                                    g2e = w;
                                    if (g2e == g2s + 1 && buf[g2s] == 'R')
                                        indirect = 1;
                                }

                                if (!indirect &&
                                    !(v < stream_off && !pdfi_is_ws(buf[v]) &&
                                      !pdfi_is_delim(buf[v])))
                                    lend = val;
                            }
                            break;
                        }
                    }

                    if (lend >= 0 && stream_off + lend <= n)
                        stream_len = lend;
                    else
                    {
                        /* Fall back: search for the terminating
                         * "endstream". The keyword must be preceded by an
                         * EOL (that is what the spec mandates between the
                         * data and "endstream"), so requiring it here stops
                         * a chance occurrence of the nine bytes inside
                         * compressed data from truncating the stream. */
                        long q = stream_off;

                        while (q + 9 <= n)
                        {
                            if (memcmp(buf + q, "endstream", 9) == 0 &&
                                q > stream_off &&
                                (buf[q - 1] == '\n' || buf[q - 1] == '\r'))
                                break;
                            q++;
                        }
                        stream_len = (q + 9 <= n) ? (q - stream_off)
                                                  : (n - stream_off);
                        while (stream_len > 0 &&
                               (buf[stream_off + stream_len - 1] == '\n' ||
                                buf[stream_off + stream_len - 1] == '\r'))
                            stream_len--;
                    }
                }

                /* Resume after this object's "endstream ... endobj", not
                 * from inside its data: the compressed bytes may contain
                 * "obj" or "endobj" by chance. */
                {
                    long q = stream_off + (stream_len > 0 ? stream_len : 0);

                    while (q + 9 <= n && memcmp(buf + q, "endstream", 9) != 0)
                        q++;
                    q = (q + 9 <= n) ? (q + 9) : n;
                    while (q + 6 <= n && memcmp(buf + q, "endobj", 6) != 0)
                        q++;
                    after = (q + 6 <= n) ? (q + 6) : n;
                }
            }
            else
            {
                after = (e + 6 <= n) ? (e + 6) : n;
            }

            if (d->nobj < PDFI_MAX_OBJS)
            {
                pdfi_obj *o = &d->objs[d->nobj++];
                o->num = num;
                o->dict_off = dict_off;
                o->dict_len = dict_end - dict_off;
                o->stream_off = stream_off;
                o->stream_len = stream_len;
            }

            i = after;
        }
    }
    return d->nobj;
}

static const pdfi_obj *pdfi_find(const pdfi_doc *d, long num)
{
    int i;

    for (i = 0; i < d->nobj; i++)
        if (d->objs[i].num == num)
            return &d->objs[i];
    return NULL;
}

/* Does the file use object streams? Page dictionaries inside them are not
 * visible to the raw scanner, so a test should say so rather than report
 * "no pages found" as a failure. */
static int pdfi_has_object_streams(const pdfi_doc *d)
{
    static const char needle[] = "/ObjStm";
    long i;

    for (i = 0; i + (long)sizeof(needle) - 1 <= d->n; i++)
        if (memcmp(d->buf + i, needle, sizeof(needle) - 1) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dictionary access (depth aware)                                     */
/* ------------------------------------------------------------------ */

typedef enum pdfi_vkind
{
    PDFI_V_NONE = 0,
    PDFI_V_INT,
    PDFI_V_REAL,
    PDFI_V_NAME,
    PDFI_V_STRING,
    PDFI_V_HEXSTRING,
    PDFI_V_ARRAY,
    PDFI_V_DICT,
    PDFI_V_REF,
    PDFI_V_BOOL
} pdfi_vkind;

typedef struct pdfi_val
{
    pdfi_vkind kind;
    long off;       /* token span for NAME / STRING / HEXSTRING / ARRAY / DICT */
    long len;
    long ival;      /* INT, BOOL, or the object number of a REF */
    double rval;
} pdfi_val;

/* Read the value token that starts at `pos`, advancing past it. */
static void pdfi_read_value(const unsigned char *buf, long n, long *pos,
                            pdfi_val *v)
{
    long off = 0, len;
    long p = *pos;

    memset(v, 0, sizeof(*v));

    while (p < n && pdfi_is_ws(buf[p]))
        p++;
    if (p >= n)
    {
        v->kind = PDFI_V_NONE;
        *pos = p;
        return;
    }

    if (buf[p] == '<' && p + 1 < n && buf[p + 1] == '<')
    {
        long start = p;
        pdfi_skip_span(buf, n, &p);
        v->kind = PDFI_V_DICT;
        v->off = start;
        v->len = p - start;
        *pos = p;
        return;
    }
    if (buf[p] == '[')
    {
        long start = p;
        pdfi_skip_span(buf, n, &p);
        v->kind = PDFI_V_ARRAY;
        v->off = start;
        v->len = p - start;
        *pos = p;
        return;
    }
    if (buf[p] == '(')
    {
        long start = p;
        pdfi_skip_span(buf, n, &p);
        v->kind = PDFI_V_STRING;
        v->off = start;
        v->len = p - start;
        *pos = p;
        return;
    }
    if (buf[p] == '<')
    {
        long start = p;
        pdfi_skip_span(buf, n, &p);
        v->kind = PDFI_V_HEXSTRING;
        v->off = start;
        v->len = p - start;
        *pos = p;
        return;
    }
    if (buf[p] == '/')
    {
        long start = p;
        p++;
        while (p < n && !pdfi_is_ws(buf[p]) && !pdfi_is_delim(buf[p]))
            p++;
        v->kind = PDFI_V_NAME;
        v->off = start + 1;   /* skip the slash */
        v->len = p - start - 1;
        *pos = p;
        return;
    }

    len = pdfi_next_tok(buf, n, &p, &off);
    if (len <= 0)
    {
        v->kind = PDFI_V_NONE;
        *pos = p;
        return;
    }

    if (pdfi_tok_is(buf + off, len, "true") || pdfi_tok_is(buf + off, len, "false"))
    {
        v->kind = PDFI_V_BOOL;
        v->ival = (buf[off] == 't') ? 1 : 0;
        *pos = p;
        return;
    }
    {
        long k;
        int is_num = 1, has_dot = 0;

        for (k = off; k < off + len; k++)
        {
            if (buf[k] == '.' || buf[k] == '-')
            {
                if (buf[k] == '.')
                    has_dot = 1;
                continue;
            }
            if (buf[k] < '0' || buf[k] > '9')
            {
                is_num = 0;
                break;
            }
        }

        /* "N G R" - an indirect reference.
         *
         * This lookahead MUST come before the plain-integer branch below.
         * "7 0 R" starts with a perfectly good integer 7, so a naive
         * implementation returns INT(7) and the caller never learns the
         * value was a reference - which is exactly how every
         * /Contents 7 0 R went unresolved. Only an unsigned integer can
         * start a reference, so the check is gated on !has_dot. */
        if (is_num && !has_dot && pdfi_tok_is_uint(buf, off, len))
        {
            long save = p;
            long off2 = 0, len2;
            long p2 = p;
            long off3 = 0, len3;
            long p3;

            len2 = pdfi_next_tok(buf, n, &p2, &off2);
            if (pdfi_tok_is_uint(buf, off2, len2))
            {
                p3 = p2;
                len3 = pdfi_next_tok(buf, n, &p3, &off3);
                if (pdfi_tok_is(buf + off3, len3, "R"))
                {
                    v->kind = PDFI_V_REF;
                    v->ival = pdfi_tok_to_long(buf, off, len);
                    *pos = p3;
                    return;
                }
            }
            *pos = save;
        }

        if (is_num && len > 0)
        {
            if (has_dot)
            {
                v->kind = PDFI_V_REAL;
                v->rval = atof((const char *)(buf + off));
            }
            else
            {
                v->kind = PDFI_V_INT;
                v->ival = pdfi_tok_to_long(buf, off, len);
            }
            *pos = p;
            return;
        }
    }

    v->kind = PDFI_V_NAME;   /* unknown keyword: report as a bare name */
    v->off = off;
    v->len = len;
    *pos = p;
}

/* Find `key` at depth 1 of the object's dictionary and read its value.
 *
 * The dictionary is walked as a sequence of key/value PAIRS, which is the
 * only correct way to do it: a /Name can be a value as well as a key, so
 * "scan for the next /key and read what follows it" gets
 *     << /Type /Action /S /URI /URI (https://...) >>
 * wrong twice over - it finds /URI as the value of /S, reads the name
 * /URI, and never reaches the real key. (That is not hypothetical: it is
 * exactly what PoDoFo writes for a URI link, and it is how this reader
 * first failed.) */
static int pdfi_dict_get(const pdfi_doc *d, const pdfi_obj *o,
                         const char *key, pdfi_val *out)
{
    const unsigned char *buf = d->buf;
    long end = o->dict_off + o->dict_len;
    long p = o->dict_off;
    size_t klen = strlen(key);

    /* Step over the opening "<<" of the outer dictionary. A nested "<<" is
     * a value and is consumed by pdfi_read_value below. */
    if (p + 1 < end && buf[p] == '<' && buf[p + 1] == '<')
        p += 2;

    while (p < end)
    {
        long ks, ke;

        while (p < end && pdfi_is_ws(buf[p]))
            p++;
        if (p >= end)
            break;
        if (buf[p] == '%')
        {
            while (p < end && buf[p] != '\n' && buf[p] != '\r')
                p++;
            continue;
        }
        if (buf[p] == '>' && p + 1 < end && buf[p + 1] == '>')
            break;                      /* end of this dictionary */

        if (buf[p] != '/')
        {
            /* Not a key: skip a value and try to resync. Guards against a
             * malformed dictionary spinning forever. */
            pdfi_val junk;
            long save = p;

            pdfi_read_value(buf, end, &p, &junk);
            if (p <= save)
                p = save + 1;
            continue;
        }

        ks = p + 1;
        ke = ks;
        while (ke < end && !pdfi_is_ws(buf[ke]) && !pdfi_is_delim(buf[ke]))
            ke++;
        p = ke;

        {
            pdfi_val v;
            long save = p;

            pdfi_read_value(buf, end, &p, &v);
            if (p <= save)
            {
                p = save + 1;
                continue;
            }
            if ((size_t)(ke - ks) == klen &&
                memcmp(buf + ks, key, klen) == 0)
            {
                *out = v;
                return 1;
            }
        }
    }
    return 0;
}

static int pdfi_dict_has(const pdfi_doc *d, const pdfi_obj *o, const char *key)
{
    pdfi_val v;

    return pdfi_dict_get(d, o, key, &v);
}

static int pdfi_dict_int(const pdfi_doc *d, const pdfi_obj *o,
                         const char *key, long *out)
{
    pdfi_val v;

    if (!pdfi_dict_get(d, o, key, &v) || v.kind != PDFI_V_INT)
        return 0;
    *out = v.ival;
    return 1;
}

/* Read a /Name value into `out` (without the slash). Returns 0 when the
 * key is absent or does not hold a name. */
static int pdfi_dict_name(const pdfi_doc *d, const pdfi_obj *o,
                          const char *key, char *out, size_t cap)
{
    pdfi_val v;
    size_t n;

    if (!pdfi_dict_get(d, o, key, &v) || v.kind != PDFI_V_NAME)
        return 0;
    n = (size_t)v.len;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, d->buf + v.off, n);
    out[n] = 0;
    return 1;
}

/* First integer of an array value, e.g. /MediaBox [0 0 595 842]. Returns
 * the number of integers read. */
static int pdfi_dict_array_ints(const pdfi_doc *d, const pdfi_obj *o,
                                const char *key, long *out, int max)
{
    pdfi_val v;
    long p, end;
    int got = 0;

    if (!pdfi_dict_get(d, o, key, &v) || v.kind != PDFI_V_ARRAY)
        return 0;
    p = v.off + 1;
    end = v.off + v.len;
    while (p < end && got < max)
    {
        pdfi_val e;

        while (p < end && pdfi_is_ws(d->buf[p]))
            p++;
        if (p >= end)
            break;
        if (d->buf[p] == ']')
            break;
        pdfi_read_value(d->buf, end, &p, &e);
        if (e.kind == PDFI_V_INT)
            out[got++] = e.ival;
        else if (e.kind == PDFI_V_REAL)
            out[got++] = (long)e.rval;
        else if (e.kind == PDFI_V_NONE)
            break;
    }
    return got;
}

/* Same, but without throwing away the fraction. A /MediaBox is usually
 * real-valued ([0 0 595.276 841.89]), so asserting on the exact page size
 * needs the full value: the integer variant above turns A4 into 595x841
 * and Letter landscape into 792x612, which is fine for "is it there" and
 * useless for "is it right". */
static int pdfi_dict_array_nums(const pdfi_doc *d, const pdfi_obj *o,
                                const char *key, double *out, int max)
{
    pdfi_val v;
    long p, end;
    int got = 0;

    if (!pdfi_dict_get(d, o, key, &v) || v.kind != PDFI_V_ARRAY)
        return 0;
    p = v.off + 1;
    end = v.off + v.len;
    while (p < end && got < max)
    {
        pdfi_val e;

        while (p < end && pdfi_is_ws(d->buf[p]))
            p++;
        if (p >= end || d->buf[p] == ']')
            break;
        pdfi_read_value(d->buf, end, &p, &e);
        if (e.kind == PDFI_V_INT)
            out[got++] = (double)e.ival;
        else if (e.kind == PDFI_V_REAL)
            out[got++] = e.rval;
        else if (e.kind == PDFI_V_NONE)
            break;
    }
    return got;
}

/* The object number a key points at, or -1 when it is absent or direct. */
static long pdfi_dict_ref(const pdfi_doc *d, const pdfi_obj *o, const char *key)
{
    pdfi_val v;

    if (!pdfi_dict_get(d, o, key, &v) || v.kind != PDFI_V_REF)
        return -1;
    return v.ival;
}

/* Locate the CONTENTS of an array-valued key, whether the array is inline
 *   << /DescendantFonts [ 17 0 R ] >>      <- what PoDoFo writes
 * or an array object reached by reference
 *   << /Kids 3 0 R >>    with    3 0 obj [ 4 0 R 5 0 R ] endobj
 * Both shapes occur in real files, and a reader that only understands one
 * of them silently reports the key as empty.
 *
 * On success *out_start/*out_end bound the elements (inside the brackets).
 * Returns 0 when the key is absent or is not an array. */
static int pdfi_dict_array_span(const pdfi_doc *d, const pdfi_obj *o,
                                const char *key, long *out_start,
                                long *out_end)
{
    pdfi_val v;

    if (!pdfi_dict_get(d, o, key, &v))
        return 0;

    if (v.kind == PDFI_V_ARRAY)
    {
        *out_start = v.off + 1;
        *out_end = v.off + v.len - 1;   /* before the closing ']' */
        return 1;
    }

    if (v.kind == PDFI_V_REF)
    {
        const pdfi_obj *arr = pdfi_find(d, v.ival);
        long e;

        if (!arr || arr->dict_len <= 0)
            return 0;
        if (d->buf[arr->dict_off] != '[')
            return 0;
        e = arr->dict_off + arr->dict_len;
        while (e > arr->dict_off + 1 && pdfi_is_ws(d->buf[e - 1]))
            e--;
        if (e > arr->dict_off + 1 && d->buf[e - 1] == ']')
            e--;
        *out_start = arr->dict_off + 1;
        *out_end = e;
        return 1;
    }
    return 0;
}

/* Resolve a key to a dictionary, whether it is written INLINE
 *   /A << /S /URI /URI (https://...) >>          <- what libharu writes
 * or as a reference to a dictionary object
 *   /A 11 0 R                                    <- what PoDoFo writes
 * Both shapes are legal and both occur, so every reader of a nested
 * dictionary has to accept both. On success `fake` is a synthetic object
 * spanning the dictionary, ready for pdfi_dict_*. */
static int pdfi_dict_subdict(const pdfi_doc *d, const pdfi_obj *o,
                             const char *key, pdfi_obj *fake)
{
    pdfi_val v;

    if (!pdfi_dict_get(d, o, key, &v))
        return 0;

    if (v.kind == PDFI_V_DICT)
    {
        fake->num = -1;
        fake->dict_off = v.off;
        fake->dict_len = v.len;
        fake->stream_off = -1;
        fake->stream_len = -1;
        return 1;
    }

    if (v.kind == PDFI_V_REF)
    {
        const pdfi_obj *r = pdfi_find(d, v.ival);

        if (!r || r->dict_len <= 0)
            return 0;
        *fake = *r;
        return 1;
    }
    return 0;
}

/* The first reference inside an array-valued key, or -1. */
static long pdfi_dict_first_ref(const pdfi_doc *d, const pdfi_obj *o,
                                const char *key)
{
    long p, end;

    if (!pdfi_dict_array_span(d, o, key, &p, &end))
        return -1;
    while (p < end)
    {
        pdfi_val e;

        while (p < end && pdfi_is_ws(d->buf[p]))
            p++;
        if (p >= end || d->buf[p] == ']')
            break;
        pdfi_read_value(d->buf, end, &p, &e);
        if (e.kind == PDFI_V_REF)
            return e.ival;
        if (e.kind == PDFI_V_NONE)
            break;
    }
    return -1;
}

/* Number of elements in an array-valued key (-1 when absent/not an array).
 * Used for /Annots, /Contents, /Kids and /W-style arrays. */
static long pdfi_dict_array_count(const pdfi_doc *d, const pdfi_obj *o,
                                  const char *key)
{
    pdfi_val v;
    long p, end, count = 0;

    if (!pdfi_dict_get(d, o, key, &v) || v.kind != PDFI_V_ARRAY)
        return -1;
    p = v.off + 1;
    end = v.off + v.len;
    while (p < end)
    {
        pdfi_val e;

        while (p < end && pdfi_is_ws(d->buf[p]))
            p++;
        if (p >= end || d->buf[p] == ']')
            break;
        pdfi_read_value(d->buf, end, &p, &e);
        if (e.kind == PDFI_V_NONE)
            break;
        count++;
    }
    return count;
}

/* Read a string-valued key into a UTF-8 buffer.
 *
 * PDF has two string syntaxes and neither is UTF-8:
 *   - a literal string "(...)" holds raw bytes;
 *   - a hex string "<...>" holds bytes as hex pairs, and writers use
 *     UTF-16BE-with-BOM (FE FF) for anything beyond ASCII - which is what
 *     PoDoFo and libharu both do for annotation /Contents and for
 *     non-ASCII /Info values.
 * Both are handled, including the byte-level escapes of a literal string,
 * so a test can compare text it drew against text it reads back. */
static int pdfi_dict_text(const pdfi_doc *d, const pdfi_obj *o,
                          const char *key, char *out, size_t cap)
{
    pdfi_val v;
    unsigned char raw[1024];
    long rlen = 0;          /* bytes actually decoded into raw[] */
    size_t w = 0;

    memset(raw, 0, sizeof(raw));
    if (!pdfi_dict_get(d, o, key, &v))
        return 0;

    if (v.kind == PDFI_V_STRING)
    {
        long p = v.off + 1;
        long end = v.off + v.len - 1;   /* before the closing ')' */

        while (p < end && rlen < (long)sizeof(raw))
        {
            int c = d->buf[p];

            if (c == '\\' && p + 1 < end)
            {
                int e = d->buf[p + 1];

                p += 2;
                switch (e)
                {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case '(': case ')': case '\\': c = e; break;
                    default:
                        if (e >= '0' && e <= '7')
                        {
                            int oct = e - '0';
                            int k;
                            for (k = 0; k < 2 && p < end &&
                                        d->buf[p] >= '0' && d->buf[p] <= '7'; k++)
                            {
                                oct = oct * 8 + (d->buf[p] - '0');
                                p++;
                            }
                            c = oct & 0xff;
                        }
                        else
                            c = e;
                        break;
                }
            }
            else
                p++;
            raw[rlen++] = (unsigned char)c;
        }
    }
    else if (v.kind == PDFI_V_HEXSTRING)
    {
        long p = v.off + 1;
        long end = v.off + v.len - 1;
        int hi = -1;

        while (p < end && rlen < (long)sizeof(raw))
        {
            int c = d->buf[p++];
            int digit;

            if (c >= '0' && c <= '9')      digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else continue;

            if (hi < 0)
                hi = digit;
            else
            {
                raw[rlen++] = (unsigned char)((hi << 4) | digit);
                hi = -1;
            }
        }
        if (hi >= 0)
            raw[rlen++] = (unsigned char)(hi << 4);
    }
    else
        return 0;

    /* UTF-16BE with a BOM -> UTF-8. */
    if (rlen >= 2 && raw[0] == 0xFE && raw[1] == 0xFF)
    {
        long i;

        for (i = 2; i + 1 < rlen && w + 4 < cap; i += 2)
        {
            unsigned int u = ((unsigned int)raw[i] << 8) | raw[i + 1];

            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < rlen)
            {
                unsigned int lo = ((unsigned int)raw[i + 2] << 8) | raw[i + 3];
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (u < 0x80)
                out[w++] = (char)u;
            else if (u < 0x800)
            {
                out[w++] = (char)(0xC0 | (u >> 6));
                out[w++] = (char)(0x80 | (u & 0x3F));
            }
            else if (u < 0x10000)
            {
                out[w++] = (char)(0xE0 | (u >> 12));
                out[w++] = (char)(0x80 | ((u >> 6) & 0x3F));
                out[w++] = (char)(0x80 | (u & 0x3F));
            }
            else
            {
                out[w++] = (char)(0xF0 | (u >> 18));
                out[w++] = (char)(0x80 | ((u >> 12) & 0x3F));
                out[w++] = (char)(0x80 | ((u >> 6) & 0x3F));
                out[w++] = (char)(0x80 | (u & 0x3F));
            }
        }
    }
    else
    {
        /* Copy exactly the bytes that were decoded. Iterating to
         * sizeof(raw) instead would walk off the end of the decoded data
         * into uninitialised stack, which produced an Info value of
         * "apitestreadback" from a stored "apitest" the first time this
         * ran. */
        long i;

        for (i = 0; i < rlen && w + 1 < cap; i++)
        {
            if (raw[i] == 0)
                break;
            out[w++] = (char)raw[i];
        }
    }

    out[w] = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Stream decoding                                                     */
/* ------------------------------------------------------------------ */

/* FlateDecode. Tries the zlib wrapper first, then raw deflate. Returns 0
 * on success. `*out` is malloc'd by the caller's CRT and must be freed. */
static int pdfi_inflate(const unsigned char *in, long in_len,
                        unsigned char **out, long *out_len)
{
    z_stream zs;
    unsigned char *dst;
    long cap = in_len * 6 + 1024;
    int rc;
    int attempt;

    *out = NULL;
    *out_len = 0;
    if (in_len <= 0)
        return -1;

    for (attempt = 0; attempt < 2; attempt++)
    {
        int window = attempt == 0 ? 15 : -15;

        for (;;)
        {
            dst = (unsigned char *)malloc((size_t)cap);
            if (!dst)
                return -1;

            memset(&zs, 0, sizeof(zs));
            zs.next_in = (Bytef *)in;
            zs.avail_in = (uInt)in_len;
            zs.next_out = dst;
            zs.avail_out = (uInt)cap;

            rc = inflateInit2(&zs, window);
            if (rc != Z_OK)
            {
                free(dst);
                break;
            }
            rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);

            if (rc == Z_STREAM_END)
            {
                *out = dst;
                *out_len = (long)zs.total_out;
                return 0;
            }
            free(dst);
            if (rc == Z_BUF_ERROR && cap < (1L << 26))
            {
                cap *= 4;   /* not enough room: grow and retry */
                continue;
            }
            break;          /* wrong wrapper or corrupt: try the next one */
        }
    }
    return -1;
}

/* Undo PNG predictors (/DecodeParms << /Predictor 15 /Colors c
 * /BitsPerComponent b /Columns n >>). Returns 0 on success. */
static int pdfi_unpredict(unsigned char *data, long len, int colors, int bpc,
                          int columns)
{
    int bpp = (colors * bpc + 7) / 8;
    int rowlen = (colors * bpc * columns + 7) / 8;
    long stride = (long)rowlen + 1;
    long rows, r, i;
    unsigned char *out;

    if (bpp <= 0 || rowlen <= 0 || len < stride)
        return -1;
    rows = len / stride;
    out = (unsigned char *)malloc((size_t)(rows * rowlen + 1));
    if (!out)
        return -1;

    for (r = 0; r < rows; r++)
    {
        const unsigned char *src = data + r * stride;
        unsigned char *cur = out + r * rowlen;
        const unsigned char *prev = (r > 0) ? (out + (r - 1) * rowlen) : NULL;
        int ft = src[0];

        for (i = 0; i < rowlen; i++)
        {
            int raw = src[1 + i];
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int val;

            switch (ft)
            {
                case 0: val = raw; break;
                case 1: val = raw + a; break;
                case 2: val = raw + b; break;
                case 3: val = raw + ((a + b) >> 1); break;
                case 4:
                {
                    int pp = a + b - c;
                    int pa = pp > a ? pp - a : a - pp;
                    int pb = pp > b ? pp - b : b - pp;
                    int pc = pp > c ? pp - c : c - pp;
                    int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    val = raw + pred;
                    break;
                }
                default:
                    free(out);
                    return -1;
            }
            cur[i] = (unsigned char)(val & 0xff);
        }
    }

    memcpy(data, out, (size_t)(rows * rowlen));
    free(out);
    return (int)(rows * rowlen);
}

/* Collect a stream's /Filter names.
 *
 * The value is either a single name or an ARRAY of names, and both shapes
 * are common: PoDoFo writes "/Filter/FlateDecode" while libharu writes
 * "/Filter [ /FlateDecode ]". A reader that only understands the name form
 * sees no filter at all in libharu's output and hands the caller raw
 * deflate bytes - which then look like a content stream containing no
 * operators at all. Returns the number of filters found. */
static int pdfi_stream_filters(const pdfi_doc *d, const pdfi_obj *o,
                               char out[][48], int max)
{
    pdfi_val v;
    int got = 0;

    if (!pdfi_dict_get(d, o, "Filter", &v))
        return 0;

    if (v.kind == PDFI_V_NAME)
    {
        size_t n = (size_t)v.len;

        if (n >= 48)
            n = 47;
        memcpy(out[0], d->buf + v.off, n);
        out[0][n] = 0;
        return 1;
    }

    if (v.kind == PDFI_V_ARRAY)
    {
        long p = v.off + 1;
        long end = v.off + v.len - 1;   /* before the closing ']' */

        while (p < end && got < max)
        {
            pdfi_val e;

            while (p < end && pdfi_is_ws(d->buf[p]))
                p++;
            if (p >= end || d->buf[p] == ']')
                break;
            pdfi_read_value(d->buf, end, &p, &e);
            if (e.kind == PDFI_V_NAME)
            {
                size_t n = (size_t)e.len;

                if (n >= 48)
                    n = 47;
                memcpy(out[got], d->buf + e.off, n);
                out[got][n] = 0;
                got++;
            }
            else if (e.kind == PDFI_V_NONE)
                break;
        }
    }
    return got;
}

/* Is this stream Flate-coded (possibly as one element of a filter chain)? */
static int pdfi_stream_is_flate(const pdfi_doc *d, const pdfi_obj *o)
{
    char filters[4][48];
    int nf = pdfi_stream_filters(d, o, filters, 4);
    int i;

    for (i = 0; i < nf; i++)
        if (strcmp(filters[i], "FlateDecode") == 0 ||
            strcmp(filters[i], "Fl") == 0)
            return 1;
    return 0;
}

/* Decode an object's stream. Handles /FlateDecode (with optional PNG
 * predictors). Returns the decoded length, or -1 when there is no stream
 * or the filter is not supported. */
static long pdfi_stream(const pdfi_doc *d, const pdfi_obj *o,
                        unsigned char **out)
{
    char filters[4][48];
    int nf;
    unsigned char *raw = NULL;
    long raw_len = -1;
    int i;

    *out = NULL;
    if (!o || o->stream_off < 0)
        return -1;

    /* A zero-length stream decodes to zero bytes, whatever /Filter says.
     *
     * libharu writes exactly this for a page on which nothing was drawn:
     *   << /Length 0 /Filter [ /FlateDecode ] >> stream\n\nendstream
     * and inflate() fails on zero input bytes, so without this guard the
     * empty content stream of a perfectly blank page reads as "will not
     * inflate" - a blank page misdiagnosed as a corrupt one.
     *
     * Return an empty buffer rather than NULL, so callers can still tell
     * "decoded to nothing" from "could not decode": NULL means failure,
     * non-NULL with length 0 means genuinely empty. */
    if (o->stream_len <= 0)
    {
        *out = (unsigned char *)malloc(1);
        return *out ? 0 : -1;
    }

    nf = pdfi_stream_filters(d, o, filters, 4);

    if (nf == 0)
    {
        raw = (unsigned char *)malloc((size_t)o->stream_len);
        if (!raw)
            return -1;
        memcpy(raw, d->buf + o->stream_off, (size_t)o->stream_len);
        *out = raw;
        return o->stream_len;
    }

    /* Only an all-Flate chain is decodable here; anything else (a
     * DCTDecode image, say) is opaque on purpose. */
    for (i = 0; i < nf; i++)
        if (strcmp(filters[i], "FlateDecode") != 0 &&
            strcmp(filters[i], "Fl") != 0)
            return -1;

    /* A filter chain is applied left to right when encoding, so decoding
     * runs from the last filter back to the first. */
    for (i = nf - 1; i >= 0; i--)
    {
        unsigned char *next = NULL;
        long next_len = -1;

        if (raw == NULL)
        {
            if (pdfi_inflate(d->buf + o->stream_off, o->stream_len, &next,
                             &next_len) != 0)
                return -1;
        }
        else if (pdfi_inflate(raw, raw_len, &next, &next_len) != 0)
        {
            free(raw);
            return -1;
        }
        free(raw);
        raw = next;
        raw_len = next_len;
    }

    if (raw == NULL)
        return -1;

    /* Predictor? Only worth undoing for image XObjects. /DecodeParms may
     * be an inline dict or a reference; pdfi_dict_subdict accepts both. */
    {
        pdfi_obj fake;

        if (pdfi_dict_subdict(d, o, "DecodeParms", &fake))
        {
            long pred = 1, colors = 1, bpc = 8, columns = 1;

            pdfi_dict_int(d, &fake, "Predictor", &pred);
            pdfi_dict_int(d, &fake, "Colors", &colors);
            pdfi_dict_int(d, &fake, "BitsPerComponent", &bpc);
            pdfi_dict_int(d, &fake, "Columns", &columns);

            if (pred >= 10)
            {
                int got = pdfi_unpredict(raw, raw_len, (int)colors, (int)bpc,
                                         (int)columns);
                if (got >= 0)
                    raw_len = got;
            }
        }
    }

    *out = raw;
    return raw_len;
}

/* ------------------------------------------------------------------ */
/* Pages and content                                                   */
/* ------------------------------------------------------------------ */

/* Object numbers of every /Type /Page (not /Pages). Returns the count. */
static int pdfi_pages(const pdfi_doc *d, long *nums, int max)
{
    int i, got = 0;

    for (i = 0; i < d->nobj && got < max; i++)
    {
        char type[32];

        if (!pdfi_dict_name(d, &d->objs[i], "Type", type, sizeof(type)))
            continue;
        if (strcmp(type, "Page") != 0)
            continue;
        nums[got++] = d->objs[i].num;
    }
    return got;
}

/* Concatenate and decode every stream reachable through the page's
 * /Contents (a single reference or an array of references). Returns the
 * decoded length, or -1. */
static long pdfi_page_content(const pdfi_doc *d, long page_num,
                              unsigned char **out)
{
    const pdfi_obj *page = pdfi_find(d, page_num);
    pdfi_val c;
    unsigned char *acc = NULL;
    long acc_len = 0;

    *out = NULL;
    if (!page)
        return -1;
    if (!pdfi_dict_get(d, page, "Contents", &c))
        return -1;

    if (c.kind == PDFI_V_REF)
    {
        unsigned char *s;
        long n = pdfi_stream(d, pdfi_find(d, c.ival), &s);

        if (n < 0)
            return -1;
        *out = s;
        return n;
    }
    if (c.kind == PDFI_V_ARRAY)
    {
        long p = c.off + 1, end = c.off + c.len;

        while (p < end)
        {
            pdfi_val e;
            unsigned char *s;
            long n;

            while (p < end && pdfi_is_ws(d->buf[p]))
                p++;
            if (p >= end || d->buf[p] == ']')
                break;
            pdfi_read_value(d->buf, end, &p, &e);
            if (e.kind != PDFI_V_REF)
                continue;
            n = pdfi_stream(d, pdfi_find(d, e.ival), &s);
            if (n < 0)
                continue;
            {
                unsigned char *bigger = (unsigned char *)malloc(
                    (size_t)(acc_len + n + 1));
                if (!bigger)
                {
                    free(s);
                    free(acc);
                    return -1;
                }
                if (acc_len)
                    memcpy(bigger, acc, (size_t)acc_len);
                memcpy(bigger + acc_len, s, (size_t)n);
                free(s);
                free(acc);
                acc = bigger;
                acc_len += n;
            }
        }
        /* An array of empty streams concatenates to nothing; hand back an
         * empty buffer rather than NULL, for the same reason pdfi_stream
         * does - NULL has to keep meaning "could not decode". */
        if (acc == NULL)
            acc = (unsigned char *)malloc(1);
        *out = acc;
        return acc ? acc_len : -1;
    }
    return -1;
}

/* 1 if the page dict actually carries a /Contents entry, 0 if it has none.
 *
 * pdfi_page_content() returns -1 for both "no /Contents" and "a /Contents
 * that will not decode", and those are not the same thing: a page with no
 * /Contents is a legitimately blank page - PoDoFo omits the key entirely
 * when nothing was drawn on the page - whereas a page whose /Contents exists
 * but refuses to inflate is a real defect. Reporting both as one message
 * hides the difference, so callers that care ask this first. */
static int pdfi_page_has_contents(const pdfi_doc *d, long page_num)
{
    const pdfi_obj *page = pdfi_find(d, page_num);
    pdfi_val c;

    if (!page)
        return 0;
    return pdfi_dict_get(d, page, "Contents", &c);
}

/* ------------------------------------------------------------------ */
/* Content stream lexing                                               */
/* ------------------------------------------------------------------ */

/* Count occurrences of a bare operator token. Strings, hex strings, names,
 * arrays and dictionaries are skipped, so a "Tj" inside a text string does
 * not count. */
static int pdfi_count_op(const unsigned char *c, long n, const char *op)
{
    long p = 0;
    int count = 0;

    while (p < n)
    {
        long off = 0, len;

        while (p < n && pdfi_is_ws(c[p]))
            p++;
        if (p >= n)
            break;
        if (c[p] == '%')
        {
            while (p < n && c[p] != '\n' && c[p] != '\r')
                p++;
            continue;
        }
        if (c[p] == '(' || c[p] == '<' || c[p] == '[')
        {
            pdfi_skip_span(c, n, &p);
            continue;
        }
        if (c[p] == '/')
        {
            p++;
            while (p < n && !pdfi_is_ws(c[p]) && !pdfi_is_delim(c[p]))
                p++;
            continue;
        }
        len = pdfi_next_tok(c, n, &p, &off);
        if (len <= 0)
            break;
        if (pdfi_tok_is(c + off, len, op))
            count++;
    }
    return count;
}

static int pdfi_has_op(const unsigned char *c, long n, const char *op)
{
    return pdfi_count_op(c, n, op) > 0;
}

/* Does the raw byte sequence appear? Used for text payloads and for keys
 * we do not want to parse. */
static int pdfi_contains(const unsigned char *buf, long n, const char *needle)
{
    size_t m = strlen(needle);
    long i;

    if (m == 0 || n < (long)m)
        return 0;
    for (i = 0; i + (long)m <= n; i++)
        if (memcmp(buf + i, needle, m) == 0)
            return 1;
    return 0;
}

/* How many times does the raw byte sequence appear? */
static int pdfi_count_plain(const unsigned char *buf, long n, const char *needle)
{
    size_t m = strlen(needle);
    long i;
    int hits = 0;

    if (m == 0 || n < (long)m)
        return 0;
    for (i = 0; i + (long)m <= n; i++)
        if (memcmp(buf + i, needle, m) == 0)
            hits++;
    return hits;
}

/* Resolve a trailer key ("Info", "Root", "Encrypt") to an object number.
 *
 * The object scan deliberately ignores the trailer, so the document
 * catalog and the info dictionary have to be found separately. Searching
 * backwards for the LAST "/Key N G R" in the file is the right move for
 * both shapes of trailer: the classic "trailer << ... >>" and the
 * dictionary of a cross-reference stream, and it also picks the newest
 * one in an incrementally updated file. Returns -1 when absent. */
static long pdfi_trailer_ref(const pdfi_doc *d, const char *key)
{
    const unsigned char *buf = d->buf;
    long n = d->n;
    long i;
    long from = 0;
    char pat[32];
    size_t plen;

    if (strlen(key) + 2 > sizeof(pat))
        return -1;
    pat[0] = '/';
    strcpy(pat + 1, key);
    plen = strlen(pat);

    /* Bound the search to the trailer itself. Scanning the whole file
     * would risk matching "/Info 3 0 R" inside a compressed stream that
     * happens to hold those bytes; the trailer always sits at the end, so
     * start from the last "trailer" keyword, or the last 2 KB when the
     * file uses a cross-reference stream and has no such keyword. */
    for (i = n - 7; i >= 0; i--)
    {
        if (memcmp(buf + i, "trailer", 7) == 0)
        {
            from = i;
            break;
        }
    }
    if (from == 0)
        from = n > 2048 ? n - 2048 : 0;

    for (i = n - (long)plen; i >= from; i--)
    {
        long p;
        pdfi_val v;

        if (memcmp(buf + i, pat, plen) != 0)
            continue;
        /* The key must be a whole name: not "/InfoX" and not preceded by a
         * name character. */
        if (i + (long)plen < n && !pdfi_is_ws(buf[i + plen]) &&
            !pdfi_is_delim(buf[i + plen]))
            continue;
        p = i + (long)plen;
        pdfi_read_value(buf, n, &p, &v);
        if (v.kind == PDFI_V_REF)
            return v.ival;
    }
    return -1;
}

/* Does a drawn text string appear in a decoded content stream?
 *
 * A text-showing operator carries the text in one of two shapes and the
 * engine picks whichever suits the font:
 *   - as a literal string, "(Hello) Tj";
 *   - as a hex string of the font's byte codes, "<48656C6C6F> Tj".
 * For a simple font (WinAnsi / standard 14) those bytes ARE the Latin-1
 * characters, so an ASCII needle can be matched either literally or after
 * hex-encoding it. For a CID font the codes are glyph indices and the text
 * is genuinely not recoverable without the CMap - this returns 0 there,
 * which is why callers must treat a miss on an embedded-CJK font as
 * inconclusive rather than as a failure. */
static int pdfi_content_has_text(const unsigned char *c, long n,
                                 const char *text)
{
    static const char hexd[] = "0123456789ABCDEF";
    static const char hexl[] = "0123456789abcdef";
    char up[512], lo[512];
    size_t len = strlen(text);
    size_t i;

    if (len == 0)
        return 1;
    if (pdfi_contains(c, n, text))
        return 1;
    if (len * 2 + 1 > sizeof(up))
        return 0;

    for (i = 0; i < len; i++)
    {
        unsigned char b = (unsigned char)text[i];

        up[i * 2] = hexd[b >> 4];
        up[i * 2 + 1] = hexd[b & 0x0f];
        lo[i * 2] = hexl[b >> 4];
        lo[i * 2 + 1] = hexl[b & 0x0f];
    }
    up[len * 2] = 0;
    lo[len * 2] = 0;

    return pdfi_contains(c, n, up) || pdfi_contains(c, n, lo);
}

/* How many times does a drawn text string appear in a decoded content
 * stream? Same matching rules as pdfi_content_has_text - literal or
 * hex-encoded - but it counts instead of just answering yes/no.
 *
 * The count is what catches a writer that paints the same run more than
 * once. "The text is present" stays true however many times it was
 * drawn, so a presence check is blind to a duplicated paragraph; this
 * was a real bug: pdf_page_draw_text_wrapped re-drew the whole
 * paragraph on every wrap pass until the box filled up.
 *
 * A given run is written in exactly one of the two forms, so the answer
 * is the larger of the two counts rather than their sum: summing would
 * report a run as doubled when one form merely happens to contain the
 * other as a substring. */
static int pdfi_count_text(const unsigned char *c, long n, const char *text)
{
    static const char hexd[] = "0123456789ABCDEF";
    char up[512];
    size_t len = strlen(text);
    size_t i;
    int lit, hex;

    if (len == 0)
        return 0;

    lit = pdfi_count_plain(c, n, text);
    if (len * 2 + 1 > sizeof(up))
        return lit;

    for (i = 0; i < len; i++)
    {
        unsigned char b = (unsigned char)text[i];

        up[i * 2] = hexd[b >> 4];
        up[i * 2 + 1] = hexd[b & 0x0f];
    }
    up[len * 2] = 0;
    hex = pdfi_count_plain(c, n, up);

    return hex > lit ? hex : lit;
}

/* A content-stream token character: anything that could be part of a name
 * or a number, used to make sure an operator match is a whole token and
 * not a substring of one. */
static int pdfi_token_char(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z') || ch == '/' || ch == '.';
}

/* The integer operand of the LAST occurrence of `op` in a content stream,
 * or -1 when the operator never appears.
 *
 * Written for the text rendering mode. "Did this page finish in fill
 * mode?" is answered by the last /Tr operand and by nothing else: /Tr is
 * text state, so it carries forward to every later text object, and a
 * page whose last /Tr says 3 has lost every word drawn after it. A count
 * of "Tr" operators cannot see that - the operator is present either way.
 *
 * Both engines write /Tr with an integer operand ("0 Tr"), so a plain
 * integer scan is enough; a real-number operand is treated as a token
 * with no integer part and skipped. */
static long pdfi_last_op_operand(const unsigned char *c, long n,
                                 const char *op)
{
    size_t m = strlen(op);
    long i;
    long found = -1;

    if (m == 0)
        return -1;

    for (i = 0; i + (long)m <= n; i++)
    {
        long j;
        long digits = 0;
        long val = 0;
        int neg = 0;

        if (memcmp(c + i, op, m) != 0)
            continue;
        if (i > 0 && pdfi_token_char(c[i - 1]))
            continue;                       /* e.g. "Tz", "Transfer" */
        if (i + (long)m < n && pdfi_token_char(c[i + m]))
            continue;                       /* e.g. "Trx" */

        j = i - 1;
        while (j >= 0 && (c[j] == ' ' || c[j] == '\n' || c[j] == '\r'
                          || c[j] == '\t'))
            j--;
        while (j >= 0 && c[j] >= '0' && c[j] <= '9')
        {
            digits++;
            j--;
        }
        if (digits == 0)
            continue;
        /* "1.0 Tr" would leave us holding the fractional part: the digits
         * we scanned are only the tail of a real. Skip it rather than
         * report a number that was never the operand. */
        if (j >= 0 && c[j] == '.')
            continue;
        if (j >= 0 && c[j] == '-')
            neg = 1;
        {
            long k = j + 1;
            long end = j + 1 + digits;

            for (; k < end; k++)
                val = val * 10 + (c[k] - '0');
        }
        found = neg ? -val : val;
    }
    return found;
}

#endif /* PDFLIB_PDF_INSPECT_H */
