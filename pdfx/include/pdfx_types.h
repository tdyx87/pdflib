/*
 * << Haru Free PDF Library >> -- pdfx_types.h
 *
 * URL: http://libharu.org
 *
 * Copyright (c) 1999-2006 Takeshi Kanno <takeshi_kanno@est.hi-ho.ne.jp>
 * Copyright (c) 2007-2009 Antony Dovgal <tony@daylessday.org>
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.
 * It is provided "as is" without express or implied warranty.
 *
 */

#ifndef _PDFX_TYPES_H
#define _PDFX_TYPES_H

#ifndef PDFX_STDCALL
#ifdef PDFX_DLL_MAKE
#define PDFX_STDCALL __stdcall
#else
#ifdef PDFX_DLL
#define PDFX_STDCALL __stdcall
#else
#define PDFX_STDCALL
#endif
#endif
#endif

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------------------*/
/*----- type definition ------------------------------------------------------*/


/*  native OS integer types */
typedef  signed int          PDFX_INT;
typedef  unsigned int        PDFX_UINT;


/*  64bit integer types
 */
typedef  signed long long    PDFX_INT64;
typedef  unsigned long long  PDFX_UINT64;


/*  32bit integer types
 */
typedef  signed int          PDFX_INT32;
typedef  unsigned int        PDFX_UINT32;


/*  16bit integer types
 */
typedef  signed short        PDFX_INT16;
typedef  unsigned short      PDFX_UINT16;


/*  8bit integer types
 */
typedef  signed char         PDFX_INT8;
typedef  unsigned char       PDFX_UINT8;


/*  8bit binary types
 */
typedef  unsigned char       PDFX_BYTE;


/*  float type (32bit IEEE754)
 */
typedef  float               PDFX_REAL;


/*  double type (64bit IEEE754)
 */
typedef  double              PDFX_DOUBLE;


/*  boolean type (0: False, !0: True)
 */
typedef  signed int          PDFX_BOOL;


/*  error-no type (32bit unsigned integer)
 */
typedef  unsigned long       PDFX_STATUS;


/*  character-code type (16bit)
 */
typedef  PDFX_UINT16         PDFX_CID;
typedef  PDFX_UINT16         PDFX_UNICODE;


/*  PDFX_Point struct
 */
typedef  struct  _PDFX_Point {
    PDFX_REAL  x;
    PDFX_REAL  y;
} PDFX_Point;

typedef  struct _PDFX_Rect {
    PDFX_REAL  left;
    PDFX_REAL  bottom;
    PDFX_REAL  right;
    PDFX_REAL  top;
} PDFX_Rect;

/*  PDFX_Point3D struct
*/
typedef  struct  _PDFX_Point3D {
	PDFX_REAL  x;
	PDFX_REAL  y;
	PDFX_REAL  z;
} PDFX_Point3D;

typedef struct _PDFX_Rect PDFX_Box;

/* PDFX_Date struct
 */
typedef  struct  _PDFX_Date {
    PDFX_INT    year;
    PDFX_INT    month;
    PDFX_INT    day;
    PDFX_INT    hour;
    PDFX_INT    minutes;
    PDFX_INT    seconds;
    char        ind;
    PDFX_INT    off_hour;
    PDFX_INT    off_minutes;
} PDFX_Date;


typedef enum _PDFX_InfoType {
    /* date-time type parameters */
    PDFX_INFO_CREATION_DATE = 0,
    PDFX_INFO_MOD_DATE,

    /* string type parameters */
    PDFX_INFO_AUTHOR,
    PDFX_INFO_CREATOR,
    PDFX_INFO_PRODUCER,
    PDFX_INFO_TITLE,
    PDFX_INFO_SUBJECT,
    PDFX_INFO_KEYWORDS,
    PDFX_INFO_TRAPPED,
    PDFX_INFO_GTS_PDFX,
    PDFX_INFO_EOF
} PDFX_InfoType;

/* PDF-A Types */

typedef enum _PDFX_PDFA_TYPE
{
    PDFX_PDFA_1A = 0,
    PDFX_PDFA_1B = 1
} PDFX_PDFAType;


typedef enum _PDFX_PdfVer {
    PDFX_VER_12 = 0,
    PDFX_VER_13,
    PDFX_VER_14,
    PDFX_VER_15,
    PDFX_VER_16,
    PDFX_VER_17,
    PDFX_VER_EOF
} PDFX_PDFVer;

typedef enum  _PDFX_EncryptMode {
    PDFX_ENCRYPT_R2    = 2,
    PDFX_ENCRYPT_R3    = 3
} PDFX_EncryptMode;


typedef void
(PDFX_STDCALL *PDFX_Error_Handler)  (PDFX_STATUS   error_no,
                                     PDFX_STATUS   detail_no,
                                     void         *user_data);

typedef void*
(PDFX_STDCALL *PDFX_Alloc_Func)  (PDFX_UINT  size);


typedef void
(PDFX_STDCALL *PDFX_Free_Func)  (void  *aptr);


/*---------------------------------------------------------------------------*/
/*------ text width struct --------------------------------------------------*/

typedef struct _PDFX_TextWidth {
    PDFX_UINT numchars;

    /* don't use this value (it may be change in the feature).
       use numspace as alternated. */
    PDFX_UINT numwords;

    PDFX_UINT width;
    PDFX_UINT numspace;
} PDFX_TextWidth;


/*---------------------------------------------------------------------------*/
/*------ dash mode ----------------------------------------------------------*/

typedef struct _PDFX_DashMode {
    PDFX_REAL  ptn[8];
    PDFX_UINT  num_ptn;
    PDFX_REAL  phase;
} PDFX_DashMode;


/*---------------------------------------------------------------------------*/
/*----- PDFX_TransMatrix struct ---------------------------------------------*/

typedef struct _PDFX_TransMatrix {
    PDFX_REAL   a;
    PDFX_REAL   b;
    PDFX_REAL   c;
    PDFX_REAL   d;
    PDFX_REAL   x;
    PDFX_REAL   y;
} PDFX_TransMatrix;

/*---------------------------------------------------------------------------*/
/*----- PDFX_3DMatrix struct ------------------------------------------------*/

typedef struct _PDFX_3DMatrix {
    PDFX_REAL   a;
    PDFX_REAL   b;
    PDFX_REAL   c;
    PDFX_REAL   d;
    PDFX_REAL   e;
    PDFX_REAL   f;
    PDFX_REAL   g;
    PDFX_REAL   h;
    PDFX_REAL   i;
    PDFX_REAL   tx;
    PDFX_REAL   ty;
    PDFX_REAL   tz;
} PDFX_3DMatrix;

/*---------------------------------------------------------------------------*/

typedef enum _PDFX_ColorSpace {
    PDFX_CS_DEVICE_GRAY = 0,
    PDFX_CS_DEVICE_RGB,
    PDFX_CS_DEVICE_CMYK,
    PDFX_CS_CAL_GRAY,
    PDFX_CS_CAL_RGB,
    PDFX_CS_LAB,
    PDFX_CS_ICC_BASED,
    PDFX_CS_SEPARATION,
    PDFX_CS_DEVICE_N,
    PDFX_CS_INDEXED,
    PDFX_CS_PATTERN,
    PDFX_CS_EOF
} PDFX_ColorSpace;

/*---------------------------------------------------------------------------*/
/*----- PDFX_RGBColor struct ------------------------------------------------*/

typedef struct _PDFX_RGBColor {
    PDFX_REAL   r;
    PDFX_REAL   g;
    PDFX_REAL   b;
} PDFX_RGBColor;

/*---------------------------------------------------------------------------*/
/*----- PDFX_CMYKColor struct -----------------------------------------------*/

typedef struct _PDFX_CMYKColor {
    PDFX_REAL   c;
    PDFX_REAL   m;
    PDFX_REAL   y;
    PDFX_REAL   k;
} PDFX_CMYKColor;

/*---------------------------------------------------------------------------*/
/*------ The line cap style -------------------------------------------------*/

typedef enum _PDFX_LineCap {
    PDFX_BUTT_END = 0,
    PDFX_ROUND_END,
    PDFX_PROJECTING_SQUARE_END,
    PDFX_LINECAP_EOF
} PDFX_LineCap;

/*----------------------------------------------------------------------------*/
/*------ The line join style -------------------------------------------------*/

typedef enum _PDFX_LineJoin {
    PDFX_MITER_JOIN = 0,
    PDFX_ROUND_JOIN,
    PDFX_BEVEL_JOIN,
    PDFX_LINEJOIN_EOF
} PDFX_LineJoin;

/*----------------------------------------------------------------------------*/
/*------ The text rendering mode ---------------------------------------------*/

typedef enum _PDFX_TextRenderingMode {
    PDFX_FILL = 0,
    PDFX_STROKE,
    PDFX_FILL_THEN_STROKE,
    PDFX_INVISIBLE,
    PDFX_FILL_CLIPPING,
    PDFX_STROKE_CLIPPING,
    PDFX_FILL_STROKE_CLIPPING,
    PDFX_CLIPPING,
    PDFX_RENDERING_MODE_EOF
} PDFX_TextRenderingMode;


typedef enum _PDFX_WritingMode {
    PDFX_WMODE_HORIZONTAL = 0,
    PDFX_WMODE_VERTICAL,
    PDFX_WMODE_EOF
} PDFX_WritingMode;


typedef enum _PDFX_PageLayout {
    PDFX_PAGE_LAYOUT_SINGLE = 0,
    PDFX_PAGE_LAYOUT_ONE_COLUMN,
    PDFX_PAGE_LAYOUT_TWO_COLUMN_LEFT,
    PDFX_PAGE_LAYOUT_TWO_COLUMN_RIGHT,
    PDFX_PAGE_LAYOUT_TWO_PAGE_LEFT,
    PDFX_PAGE_LAYOUT_TWO_PAGE_RIGHT,
    PDFX_PAGE_LAYOUT_EOF
} PDFX_PageLayout;


typedef enum _PDFX_PageMode {
    PDFX_PAGE_MODE_USE_NONE = 0,
    PDFX_PAGE_MODE_USE_OUTLINE,
    PDFX_PAGE_MODE_USE_THUMBS,
    PDFX_PAGE_MODE_FULL_SCREEN,
/*  PDFX_PAGE_MODE_USE_OC,
    PDFX_PAGE_MODE_USE_ATTACHMENTS,
 */
    PDFX_PAGE_MODE_EOF
} PDFX_PageMode;


typedef enum _PDFX_PageNumStyle {
    PDFX_PAGE_NUM_STYLE_DECIMAL = 0,
    PDFX_PAGE_NUM_STYLE_UPPER_ROMAN,
    PDFX_PAGE_NUM_STYLE_LOWER_ROMAN,
    PDFX_PAGE_NUM_STYLE_UPPER_LETTERS,
    PDFX_PAGE_NUM_STYLE_LOWER_LETTERS,
    PDFX_PAGE_NUM_STYLE_EOF
} PDFX_PageNumStyle;


typedef enum _PDFX_DestinationType {
    PDFX_XYZ = 0,
    PDFX_FIT,
    PDFX_FIT_H,
    PDFX_FIT_V,
    PDFX_FIT_R,
    PDFX_FIT_B,
    PDFX_FIT_BH,
    PDFX_FIT_BV,
    PDFX_DST_EOF
} PDFX_DestinationType;


typedef enum _PDFX_AnnotType {
    PDFX_ANNOT_TEXT_NOTES,
    PDFX_ANNOT_LINK,
    PDFX_ANNOT_SOUND,
    PDFX_ANNOT_FREE_TEXT,
    PDFX_ANNOT_STAMP,
    PDFX_ANNOT_SQUARE,
    PDFX_ANNOT_CIRCLE,
    PDFX_ANNOT_STRIKE_OUT,
    PDFX_ANNOT_HIGHTLIGHT,
    PDFX_ANNOT_UNDERLINE,
    PDFX_ANNOT_INK,
    PDFX_ANNOT_FILE_ATTACHMENT,
    PDFX_ANNOT_POPUP,
    PDFX_ANNOT_3D,
    PDFX_ANNOT_SQUIGGLY,
	PDFX_ANNOT_LINE,
	PDFX_ANNOT_PROJECTION,
	PDFX_ANNOT_WIDGET
} PDFX_AnnotType;


typedef enum _PDFX_AnnotFlgs {
    PDFX_ANNOT_INVISIBLE,
    PDFX_ANNOT_HIDDEN,
    PDFX_ANNOT_PRINT,
    PDFX_ANNOT_NOZOOM,
    PDFX_ANNOT_NOROTATE,
    PDFX_ANNOT_NOVIEW,
    PDFX_ANNOT_READONLY
} PDFX_AnnotFlgs;


typedef enum _PDFX_AnnotHighlightMode {
    PDFX_ANNOT_NO_HIGHTLIGHT = 0,
    PDFX_ANNOT_INVERT_BOX,
    PDFX_ANNOT_INVERT_BORDER,
    PDFX_ANNOT_DOWN_APPEARANCE,
    PDFX_ANNOT_HIGHTLIGHT_MODE_EOF
} PDFX_AnnotHighlightMode;


typedef enum _PDFX_AnnotIcon {
    PDFX_ANNOT_ICON_COMMENT = 0,
    PDFX_ANNOT_ICON_KEY,
    PDFX_ANNOT_ICON_NOTE,
    PDFX_ANNOT_ICON_HELP,
    PDFX_ANNOT_ICON_NEW_PARAGRAPH,
    PDFX_ANNOT_ICON_PARAGRAPH,
    PDFX_ANNOT_ICON_INSERT,
    PDFX_ANNOT_ICON_EOF
} PDFX_AnnotIcon;

typedef enum _PDFX_AnnotIntent {
    PDFX_ANNOT_INTENT_FREETEXTCALLOUT = 0,
    PDFX_ANNOT_INTENT_FREETEXTTYPEWRITER,
    PDFX_ANNOT_INTENT_LINEARROW,
    PDFX_ANNOT_INTENT_LINEDIMENSION,
    PDFX_ANNOT_INTENT_POLYGONCLOUD,
    PDFX_ANNOT_INTENT_POLYLINEDIMENSION,
    PDFX_ANNOT_INTENT_POLYGONDIMENSION
} PDFX_AnnotIntent;

typedef enum _PDFX_LineAnnotEndingStyle {
    PDFX_LINE_ANNOT_NONE = 0,
    PDFX_LINE_ANNOT_SQUARE,
    PDFX_LINE_ANNOT_CIRCLE,
    PDFX_LINE_ANNOT_DIAMOND,
    PDFX_LINE_ANNOT_OPENARROW,
    PDFX_LINE_ANNOT_CLOSEDARROW,
    PDFX_LINE_ANNOT_BUTT,
    PDFX_LINE_ANNOT_ROPENARROW,
    PDFX_LINE_ANNOT_RCLOSEDARROW,
    PDFX_LINE_ANNOT_SLASH
} PDFX_LineAnnotEndingStyle;

typedef enum _PDFX_LineAnnotCapPosition{
    PDFX_LINE_ANNOT_CAP_INLINE = 0,
    PDFX_LINE_ANNOT_CAP_TOP
} PDFX_LineAnnotCapPosition;

typedef enum _PDFX_StampAnnotName{
    PDFX_STAMP_ANNOT_APPROVED = 0,
    PDFX_STAMP_ANNOT_EXPERIMENTAL,
    PDFX_STAMP_ANNOT_NOTAPPROVED,
    PDFX_STAMP_ANNOT_ASIS,
    PDFX_STAMP_ANNOT_EXPIRED,
    PDFX_STAMP_ANNOT_NOTFORPUBLICRELEASE,
    PDFX_STAMP_ANNOT_CONFIDENTIAL,
    PDFX_STAMP_ANNOT_FINAL,
    PDFX_STAMP_ANNOT_SOLD,
    PDFX_STAMP_ANNOT_DEPARTMENTAL,
    PDFX_STAMP_ANNOT_FORCOMMENT,
    PDFX_STAMP_ANNOT_TOPSECRET,
    PDFX_STAMP_ANNOT_DRAFT,
    PDFX_STAMP_ANNOT_FORPUBLICRELEASE
} PDFX_StampAnnotName;

/*----------------------------------------------------------------------------*/
/*------ border stype --------------------------------------------------------*/

typedef enum _PDFX_BSSubtype {
    PDFX_BS_SOLID,
    PDFX_BS_DASHED,
    PDFX_BS_BEVELED,
    PDFX_BS_INSET,
    PDFX_BS_UNDERLINED
} PDFX_BSSubtype;


/*----- blend modes ----------------------------------------------------------*/

typedef enum _PDFX_BlendMode {
    PDFX_BM_NORMAL,
    PDFX_BM_MULTIPLY,
    PDFX_BM_SCREEN,
    PDFX_BM_OVERLAY,
    PDFX_BM_DARKEN,
    PDFX_BM_LIGHTEN,
    PDFX_BM_COLOR_DODGE,
    PDFX_BM_COLOR_BUM,
    PDFX_BM_HARD_LIGHT,
    PDFX_BM_SOFT_LIGHT,
    PDFX_BM_DIFFERENCE,
    PDFX_BM_EXCLUSHON,
    PDFX_BM_EOF
} PDFX_BlendMode;

/*----- slide show -----------------------------------------------------------*/

typedef enum _PDFX_TransitionStyle {
    PDFX_TS_WIPE_RIGHT = 0,
    PDFX_TS_WIPE_UP,
    PDFX_TS_WIPE_LEFT,
    PDFX_TS_WIPE_DOWN,
    PDFX_TS_BARN_DOORS_HORIZONTAL_OUT,
    PDFX_TS_BARN_DOORS_HORIZONTAL_IN,
    PDFX_TS_BARN_DOORS_VERTICAL_OUT,
    PDFX_TS_BARN_DOORS_VERTICAL_IN,
    PDFX_TS_BOX_OUT,
    PDFX_TS_BOX_IN,
    PDFX_TS_BLINDS_HORIZONTAL,
    PDFX_TS_BLINDS_VERTICAL,
    PDFX_TS_DISSOLVE,
    PDFX_TS_GLITTER_RIGHT,
    PDFX_TS_GLITTER_DOWN,
    PDFX_TS_GLITTER_TOP_LEFT_TO_BOTTOM_RIGHT,
    PDFX_TS_REPLACE,
    PDFX_TS_EOF
} PDFX_TransitionStyle;

/*----------------------------------------------------------------------------*/

typedef enum _PDFX_PageSizes {
    PDFX_PAGE_SIZE_LETTER = 0,
    PDFX_PAGE_SIZE_LEGAL,
    PDFX_PAGE_SIZE_A3,
    PDFX_PAGE_SIZE_A4,
    PDFX_PAGE_SIZE_A5,
    PDFX_PAGE_SIZE_B4,
    PDFX_PAGE_SIZE_B5,
    PDFX_PAGE_SIZE_EXECUTIVE,
    PDFX_PAGE_SIZE_US4x6,
    PDFX_PAGE_SIZE_US4x8,
    PDFX_PAGE_SIZE_US5x7,
    PDFX_PAGE_SIZE_COMM10,
    PDFX_PAGE_SIZE_EOF
} PDFX_PageSizes;


typedef enum _PDFX_PageDirection {
    PDFX_PAGE_PORTRAIT = 0,
    PDFX_PAGE_LANDSCAPE
} PDFX_PageDirection;


typedef enum  _PDFX_EncoderType {
    PDFX_ENCODER_TYPE_SINGLE_BYTE,
    PDFX_ENCODER_TYPE_DOUBLE_BYTE,
    PDFX_ENCODER_TYPE_UNINITIALIZED,
    PDFX_ENCODER_UNKNOWN
} PDFX_EncoderType;


typedef enum _PDFX_ByteType {
    PDFX_BYTE_TYPE_SINGLE = 0,
    PDFX_BYTE_TYPE_LEAD,
    PDFX_BYTE_TYPE_TRAIL,
    PDFX_BYTE_TYPE_UNKNOWN
} PDFX_ByteType;


typedef enum _PDFX_TextAlignment {
    PDFX_TALIGN_LEFT = 0,
    PDFX_TALIGN_RIGHT,
    PDFX_TALIGN_CENTER,
    PDFX_TALIGN_JUSTIFY
} PDFX_TextAlignment;

/*----------------------------------------------------------------------------*/

/* Name Dictionary values -- see PDF reference section 7.7.4 */
typedef enum _PDFX_NameDictKey {
    PDFX_NAME_EMBEDDED_FILES = 0,    /* TODO the rest */
    PDFX_NAME_EOF
} PDFX_NameDictKey;

/*----------------------------------------------------------------------------*/

typedef enum _PDFX_PageBoundary {
    PDFX_PAGE_MEDIABOX = 0,
    PDFX_PAGE_CROPBOX,
    PDFX_PAGE_BLEEDBOX,
    PDFX_PAGE_TRIMBOX,
    PDFX_PAGE_ARTBOX
} PDFX_PageBoundary;

/*----------------------------------------------------------------------------*/

typedef enum _PDFX_ShadingType {
  PDFX_SHADING_FREE_FORM_TRIANGLE_MESH = 4 /* TODO the rest */
} PDFX_ShadingType;

typedef enum _PDFX_Shading_FreeFormTriangleMeshEdgeFlag {
  PDFX_FREE_FORM_TRI_MESH_EDGEFLAG_NO_CONNECTION = 0,
  PDFX_FREE_FORM_TRI_MESH_EDGEFLAG_BC,
  PDFX_FREE_FORM_TRI_MESH_EDGEFLAG_AC
} PDFX_Shading_FreeFormTriangleMeshEdgeFlag;

/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* _PDFX_TYPES_H */

