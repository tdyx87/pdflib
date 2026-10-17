/*
 * << Haru Free PDF Library 2.0.8 >> -- hpdf.h
 *
 * URL http://libharu.org/
 *
 * Copyright (c) 1999-2006 Takeshi Kanno
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.
 * It is provided "as is" without express or implied warranty.
 *
 */

#ifndef _PDFX_H
#define _PDFX_H

#include "pdfx_config.h"
#include "pdfx_version.h"

#define PDFX_UNUSED(a) ((void)(a))

/* ------------------------------------------------------------------ *
 * pdflib libharu-compatibility header.
 *
 * This is libharu 2.4.4's public hpdf.h with one deliberate difference:
 * the handle types are always the opaque `void *` form (libharu's
 * PDFX_SHARED mode) and the internal headers (pdfx_doc.h, pdfx_pdfa.h)
 * are not required. Normal application code only passes these handles
 * around, so source compatibility is preserved while the implementation
 * lives in pdfx_compat.c over the pdflib core.
 *
 * The compat layer is a *source/link* compatibility facade: link
 * hpdf.lib (static) or pdflib_hpdf.lib (DLL) instead of libharu's
 * hpdf.lib. Extensions for backend selection are at the bottom of this
 * header.
 * ------------------------------------------------------------------ */

#if defined(PDFLIB_PDFX_BUILD_DLL) && defined(_WIN32)
#  define PDFX_EXPORT(A) __declspec(dllexport) A
#elif defined(PDFLIB_PDFX_IMPORT_DLL) && defined(_WIN32)
#  define PDFX_EXPORT(A) __declspec(dllimport) A
#else
#  define PDFX_EXPORT(A) extern A
#endif

#include "pdfx_consts.h"
#include "pdfx_types.h"
#include "pdfx_errors.h"

typedef void         *PDFX_HANDLE;
typedef PDFX_HANDLE   PDFX_Boolean;
typedef PDFX_HANDLE   PDFX_Doc;
typedef PDFX_HANDLE   PDFX_Page;
typedef PDFX_HANDLE   PDFX_Pages;
typedef PDFX_HANDLE   PDFX_Stream;
typedef PDFX_HANDLE   PDFX_Image;
typedef PDFX_HANDLE   PDFX_Font;
typedef PDFX_HANDLE   PDFX_Outline;
typedef PDFX_HANDLE   PDFX_Encoder;
typedef PDFX_HANDLE   PDFX_3DMeasure;
typedef PDFX_HANDLE   PDFX_ExData;
typedef PDFX_HANDLE   PDFX_Destination;
typedef PDFX_HANDLE   PDFX_XObject;
typedef PDFX_HANDLE   PDFX_Annotation;
typedef PDFX_HANDLE   PDFX_ExtGState;
typedef PDFX_HANDLE   PDFX_FontDef;
typedef PDFX_HANDLE   PDFX_U3D;
typedef PDFX_HANDLE   PDFX_JavaScript;
typedef PDFX_HANDLE   PDFX_Error;
typedef PDFX_HANDLE   PDFX_MMgr;
typedef PDFX_HANDLE   PDFX_Dict;
typedef PDFX_HANDLE   PDFX_EmbeddedFile;
typedef PDFX_HANDLE   PDFX_OutputIntent;
typedef PDFX_HANDLE   PDFX_Xref;
typedef PDFX_HANDLE   PDFX_Shading;

#ifdef __cplusplus
extern "C" {
#endif

PDFX_EXPORT(const char *)
PDFX_GetVersion  (void);


PDFX_EXPORT(PDFX_Doc)
PDFX_NewEx  (PDFX_Error_Handler   user_error_fn,
             PDFX_Alloc_Func      user_alloc_fn,
             PDFX_Free_Func       user_free_fn,
             PDFX_UINT            mem_pool_buf_size,
             void                *user_data);

PDFX_EXPORT(PDFX_Doc)
PDFX_New  (PDFX_Error_Handler   user_error_fn,
           void                *user_data);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetErrorHandler  (PDFX_Doc            pdf,
                       PDFX_Error_Handler  user_error_fn);


PDFX_EXPORT(void)
PDFX_Free  (PDFX_Doc  pdf);

PDFX_EXPORT(PDFX_MMgr)
PDFX_GetDocMMgr  (PDFX_Doc doc);

PDFX_EXPORT(PDFX_STATUS)
PDFX_NewDoc  (PDFX_Doc  pdf);


PDFX_EXPORT(void)
PDFX_FreeDoc  (PDFX_Doc  pdf);


PDFX_EXPORT(PDFX_BOOL)
PDFX_HasDoc  (PDFX_Doc  pdf);


PDFX_EXPORT(void)
PDFX_FreeDocAll  (PDFX_Doc  pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SaveToStream  (PDFX_Doc   pdf);

PDFX_EXPORT(PDFX_STATUS)
PDFX_GetContents   (PDFX_Doc   pdf,
                   PDFX_BYTE  *buf,
                 PDFX_UINT32  *size);

PDFX_EXPORT(PDFX_UINT32)
PDFX_GetStreamSize  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_ReadFromStream  (PDFX_Doc       pdf,
                      PDFX_BYTE     *buf,
                      PDFX_UINT32   *size);


PDFX_EXPORT(PDFX_STATUS)
PDFX_ResetStream  (PDFX_Doc     pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SaveToFile  (PDFX_Doc     pdf,
                  const char  *file_name);


PDFX_EXPORT(PDFX_STATUS)
PDFX_GetError  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_GetErrorDetail  (PDFX_Doc   pdf);

PDFX_EXPORT(void)
PDFX_ResetError  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_CheckError  (PDFX_Error   error);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetPagesConfiguration  (PDFX_Doc    pdf,
                             PDFX_UINT   page_per_pages);


PDFX_EXPORT(PDFX_Page)
PDFX_GetPageByIndex  (PDFX_Doc    pdf,
                      PDFX_UINT   index);


/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

PDFX_EXPORT(PDFX_MMgr)
PDFX_GetPageMMgr    (PDFX_Page  page);

PDFX_EXPORT(PDFX_PageLayout)
PDFX_GetPageLayout  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetPageLayout  (PDFX_Doc          pdf,
                     PDFX_PageLayout   layout);


PDFX_EXPORT(PDFX_PageMode)
PDFX_GetPageMode  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetPageMode  (PDFX_Doc        pdf,
                   PDFX_PageMode   mode);


PDFX_EXPORT(PDFX_UINT)
PDFX_GetViewerPreference  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetViewerPreference  (PDFX_Doc     pdf,
                           PDFX_UINT    value);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetOpenAction  (PDFX_Doc           pdf,
                     PDFX_Destination   open_action);


/*---------------------------------------------------------------------------*/
/*----- page handling -------------------------------------------------------*/


PDFX_EXPORT(PDFX_Page)
PDFX_GetCurrentPage  (PDFX_Doc  pdf);


PDFX_EXPORT(PDFX_Page)
PDFX_AddPage  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_Page)
PDFX_InsertPage  (PDFX_Doc    pdf,
                  PDFX_Page   page);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetWidth  (PDFX_Page   page,
                     PDFX_REAL   value);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetHeight  (PDFX_Page   page,
                      PDFX_REAL   value);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetBoundary  (PDFX_Page           page,
                        PDFX_PageBoundary   boundary,
                        PDFX_REAL           left,
                        PDFX_REAL           bottom,
                        PDFX_REAL           right,
                        PDFX_REAL           top);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetSize  (PDFX_Page            page,
                    PDFX_PageSizes       size,
                    PDFX_PageDirection   direction);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetRotate  (PDFX_Page     page,
                      PDFX_UINT16   angle);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetZoom  (PDFX_Page     page,
                    PDFX_REAL     zoom);

/*---------------------------------------------------------------------------*/
/*----- font handling -------------------------------------------------------*/


PDFX_EXPORT(PDFX_Font)
PDFX_GetFont  (PDFX_Doc     pdf,
               const char  *font_name,
               const char  *encoding_name);


PDFX_EXPORT(const char*)
PDFX_LoadType1FontFromFile  (PDFX_Doc     pdf,
                             const char  *afm_file_name,
                             const char  *data_file_name);


PDFX_EXPORT(PDFX_FontDef)
PDFX_GetTTFontDefFromFile (PDFX_Doc     pdf,
                           const char  *file_name,
                           PDFX_BOOL    embedding);

PDFX_EXPORT(const char*)
PDFX_LoadTTFontFromFile (PDFX_Doc     pdf,
                         const char  *file_name,
                         PDFX_BOOL    embedding);


PDFX_EXPORT(const char*)
PDFX_LoadTTFontFromFile2 (PDFX_Doc     pdf,
                          const char  *file_name,
                          PDFX_UINT    index,
                          PDFX_BOOL    embedding);


PDFX_EXPORT(PDFX_STATUS)
PDFX_AddPageLabel  (PDFX_Doc            pdf,
                    PDFX_UINT           page_num,
                    PDFX_PageNumStyle   style,
                    PDFX_UINT           first_page,
                    const char         *prefix);


PDFX_EXPORT(PDFX_STATUS)
PDFX_UseJPFonts   (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_UseKRFonts   (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_UseCNSFonts   (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_UseCNTFonts   (PDFX_Doc   pdf);


/*--------------------------------------------------------------------------*/
/*----- outline ------------------------------------------------------------*/


PDFX_EXPORT(PDFX_Outline)
PDFX_CreateOutline  (PDFX_Doc       pdf,
                     PDFX_Outline   parent,
                     const char    *title,
                     PDFX_Encoder   encoder);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Outline_SetOpened  (PDFX_Outline  outline,
                         PDFX_BOOL     opened);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Outline_SetDestination (PDFX_Outline      outline,
                             PDFX_Destination  dst);


/*--------------------------------------------------------------------------*/
/*----- destination --------------------------------------------------------*/

PDFX_EXPORT(PDFX_Destination)
PDFX_Page_CreateDestination  (PDFX_Page   page);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetXYZ  (PDFX_Destination  dst,
                          PDFX_REAL         left,
                          PDFX_REAL         top,
                          PDFX_REAL         zoom);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetFit  (PDFX_Destination  dst);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetFitH  (PDFX_Destination  dst,
                           PDFX_REAL         top);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetFitV  (PDFX_Destination  dst,
                           PDFX_REAL         left);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetFitR  (PDFX_Destination  dst,
                           PDFX_REAL         left,
                           PDFX_REAL         bottom,
                           PDFX_REAL         right,
                           PDFX_REAL         top);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetFitB  (PDFX_Destination  dst);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetFitBH  (PDFX_Destination  dst,
                            PDFX_REAL         top);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Destination_SetFitBV  (PDFX_Destination  dst,
                            PDFX_REAL         left);

/*--------------------------------------------------------------------------*/
/*----- encoder ------------------------------------------------------------*/

PDFX_EXPORT(PDFX_Encoder)
PDFX_GetEncoder  (PDFX_Doc     pdf,
                  const char  *encoding_name);


PDFX_EXPORT(PDFX_Encoder)
PDFX_GetCurrentEncoder  (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetCurrentEncoder  (PDFX_Doc     pdf,
                         const char  *encoding_name);


PDFX_EXPORT(PDFX_EncoderType)
PDFX_Encoder_GetType  (PDFX_Encoder   encoder);


PDFX_EXPORT(PDFX_ByteType)
PDFX_Encoder_GetByteType  (PDFX_Encoder    encoder,
                           const char     *text,
                           PDFX_UINT       index);


PDFX_EXPORT(PDFX_UNICODE)
PDFX_Encoder_GetUnicode  (PDFX_Encoder   encoder,
                          PDFX_UINT16    code);


PDFX_EXPORT(PDFX_WritingMode)
PDFX_Encoder_GetWritingMode (PDFX_Encoder   encoder);


PDFX_EXPORT(PDFX_STATUS)
PDFX_UseJPEncodings   (PDFX_Doc   pdf);



PDFX_EXPORT(PDFX_STATUS)
PDFX_UseKREncodings   (PDFX_Doc   pdf);



PDFX_EXPORT(PDFX_STATUS)
PDFX_UseCNSEncodings   (PDFX_Doc   pdf);



PDFX_EXPORT(PDFX_STATUS)
PDFX_UseCNTEncodings   (PDFX_Doc   pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_UseUTFEncodings   (PDFX_Doc   pdf);


/*--------------------------------------------------------------------------*/
/*----- XObject ------------------------------------------------------------*/

PDFX_EXPORT(PDFX_XObject)
PDFX_Page_CreateXObjectFromImage    (PDFX_Doc       pdf,
                                     PDFX_Page      page,
                                     PDFX_Rect      rect,
                                     PDFX_Image     image,
                                     PDFX_BOOL      zoom);

PDFX_EXPORT(PDFX_XObject)
PDFX_Page_CreateXObjectAsWhiteRect  (PDFX_Doc   pdf,
                                     PDFX_Page  page,
                                     PDFX_Rect  rect);

/*--------------------------------------------------------------------------*/
/*----- annotation ---------------------------------------------------------*/

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_Create3DAnnot    (PDFX_Page       page,
							PDFX_Rect       rect,
                            PDFX_BOOL       tb,
                            PDFX_BOOL       np,
                            PDFX_U3D        u3d,
                            PDFX_Image      ap);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateTextAnnot  (PDFX_Page       page,
                            PDFX_Rect       rect,
                            const char     *text,
                            PDFX_Encoder    encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateFreeTextAnnot  (PDFX_Page       page,
								PDFX_Rect       rect,
								const char     *text,
								PDFX_Encoder    encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateLineAnnot  (PDFX_Page       page,
							const char     *text,
							PDFX_Encoder    encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateWidgetAnnot_WhiteOnlyWhilePrint (PDFX_Doc   pdf,
                                                 PDFX_Page  page,
                                                 PDFX_Rect  rect);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateWidgetAnnot (PDFX_Page  page,
                             PDFX_Rect  rect);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateLinkAnnot  (PDFX_Page          page,
                            PDFX_Rect          rect,
                            PDFX_Destination   dst);


PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateURILinkAnnot  (PDFX_Page     page,
                               PDFX_Rect     rect,
                               const char   *uri);


PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateTextMarkupAnnot (PDFX_Page     page,
								PDFX_Rect      rect,
								const char     *text,
								PDFX_Encoder   encoder,
								PDFX_AnnotType subType);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateHighlightAnnot  (PDFX_Page   page,
								PDFX_Rect    rect,
								const char   *text,
								PDFX_Encoder encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateUnderlineAnnot (PDFX_Page    page,
								PDFX_Rect    rect,
								const char   *text,
								PDFX_Encoder encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateSquigglyAnnot  (PDFX_Page    page,
								PDFX_Rect    rect,
								const char   *text,
								PDFX_Encoder encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateStrikeOutAnnot  (PDFX_Page   page,
								PDFX_Rect    rect,
								const char   *text,
								PDFX_Encoder encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreatePopupAnnot  (	PDFX_Page          page,
								PDFX_Rect          rect,
								PDFX_Annotation	   parent);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateStampAnnot  (	PDFX_Page           page,
								PDFX_Rect           rect,
								PDFX_StampAnnotName name,
								const char*			text,
								PDFX_Encoder		encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateProjectionAnnot(PDFX_Page page,
								PDFX_Rect rect,
								const char* text,
								PDFX_Encoder encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateSquareAnnot (PDFX_Page          page,
							 PDFX_Rect          rect,
							 const char			*text,
							 PDFX_Encoder       encoder);

PDFX_EXPORT(PDFX_Annotation)
PDFX_Page_CreateCircleAnnot (PDFX_Page          page,
							 PDFX_Rect          rect,
							 const char			*text,
							 PDFX_Encoder       encoder);

PDFX_EXPORT(PDFX_STATUS)
PDFX_LinkAnnot_SetHighlightMode  (PDFX_Annotation           annot,
                                  PDFX_AnnotHighlightMode   mode);

PDFX_EXPORT(PDFX_STATUS)
PDFX_LinkAnnot_SetJavaScript(PDFX_Annotation    annot,
                             PDFX_JavaScript    javascript);

PDFX_EXPORT(PDFX_STATUS)
PDFX_LinkAnnot_SetBorderStyle  (PDFX_Annotation  annot,
                                PDFX_REAL        width,
                                PDFX_UINT16      dash_on,
                                PDFX_UINT16      dash_off);


PDFX_EXPORT(PDFX_STATUS)
PDFX_TextAnnot_SetIcon  (PDFX_Annotation   annot,
                         PDFX_AnnotIcon    icon);


PDFX_EXPORT(PDFX_STATUS)
PDFX_TextAnnot_SetOpened  (PDFX_Annotation   annot,
                          PDFX_BOOL          opened);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Annot_SetRGBColor (PDFX_Annotation annot, PDFX_RGBColor color);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Annot_SetCMYKColor (PDFX_Annotation annot, PDFX_CMYKColor color);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Annot_SetGrayColor (PDFX_Annotation annot, PDFX_REAL color);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Annot_SetNoColor (PDFX_Annotation annot);

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetTitle (PDFX_Annotation annot, const char* name);

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetSubject (PDFX_Annotation annot, const char* name);

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetCreationDate (PDFX_Annotation annot, PDFX_Date value);

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetTransparency (PDFX_Annotation annot, PDFX_REAL value);

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetIntent (PDFX_Annotation  annot, PDFX_AnnotIntent  intent);

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetPopup (PDFX_Annotation annot, PDFX_Annotation popup);

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetRectDiff (PDFX_Annotation  annot, PDFX_Rect  rect); /* RD entry */

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetCloudEffect (PDFX_Annotation  annot, PDFX_INT cloudIntensity); /* BE entry */

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetInteriorRGBColor (PDFX_Annotation  annot, PDFX_RGBColor color); /* IC with RGB entry */

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetInteriorCMYKColor (PDFX_Annotation  annot, PDFX_CMYKColor color); /* IC with CMYK entry */

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetInteriorGrayColor (PDFX_Annotation  annot, PDFX_REAL color); /* IC with Gray entry */

PDFX_EXPORT(PDFX_STATUS)
PDFX_MarkupAnnot_SetInteriorTransparent (PDFX_Annotation  annot); /* IC with No Color entry */

PDFX_EXPORT(PDFX_STATUS)
PDFX_TextMarkupAnnot_SetQuadPoints ( PDFX_Annotation annot, PDFX_Point lb, PDFX_Point rb, PDFX_Point rt, PDFX_Point lt); /* l-left, r-right, b-bottom, t-top positions */

PDFX_EXPORT(PDFX_STATUS)
PDFX_Annot_Set3DView  ( PDFX_MMgr mmgr,
					 	PDFX_Annotation	annot,
					 	PDFX_Annotation	annot3d,
					 	PDFX_Dict			view);

PDFX_EXPORT(PDFX_STATUS)
PDFX_PopupAnnot_SetOpened  (PDFX_Annotation   annot,
                            PDFX_BOOL         opened);

PDFX_EXPORT(PDFX_STATUS)
PDFX_FreeTextAnnot_SetLineEndingStyle (PDFX_Annotation annot, PDFX_LineAnnotEndingStyle startStyle, PDFX_LineAnnotEndingStyle endStyle);

PDFX_EXPORT(PDFX_STATUS)
PDFX_FreeTextAnnot_Set3PointCalloutLine (PDFX_Annotation annot, PDFX_Point startPoint, PDFX_Point kneePoint, PDFX_Point endPoint); /* Callout line will be in default user space */

PDFX_EXPORT(PDFX_STATUS)
PDFX_FreeTextAnnot_Set2PointCalloutLine (PDFX_Annotation annot, PDFX_Point startPoint, PDFX_Point endPoint); /* Callout line will be in default user space */

PDFX_EXPORT(PDFX_STATUS)
PDFX_FreeTextAnnot_SetDefaultStyle (PDFX_Annotation  annot, const char* style);

PDFX_EXPORT(PDFX_STATUS)
PDFX_LineAnnot_SetPosition (PDFX_Annotation annot,
							PDFX_Point startPoint, PDFX_LineAnnotEndingStyle startStyle,
							PDFX_Point endPoint, PDFX_LineAnnotEndingStyle endStyle);

PDFX_EXPORT(PDFX_STATUS)
PDFX_LineAnnot_SetLeader (PDFX_Annotation annot, PDFX_INT leaderLen, PDFX_INT leaderExtLen, PDFX_INT leaderOffsetLen);

PDFX_EXPORT(PDFX_STATUS)
PDFX_LineAnnot_SetCaption (PDFX_Annotation annot, PDFX_BOOL showCaption, PDFX_LineAnnotCapPosition position, PDFX_INT horzOffset, PDFX_INT vertOffset);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Annotation_SetBorderStyle  (PDFX_Annotation  annot,
                                 PDFX_BSSubtype   subtype,
                                 PDFX_REAL        width,
                                 PDFX_UINT16      dash_on,
                                 PDFX_UINT16      dash_off,
                                 PDFX_UINT16      dash_phase);

PDFX_EXPORT(PDFX_STATUS)
PDFX_ProjectionAnnot_SetExData(PDFX_Annotation annot, PDFX_ExData exdata);


/*--------------------------------------------------------------------------*/
/*----- 3D Measure ---------------------------------------------------------*/
PDFX_EXPORT(PDFX_3DMeasure)
PDFX_Page_Create3DC3DMeasure(PDFX_Page       page,
					         PDFX_Point3D    firstanchorpoint,
					         PDFX_Point3D    textanchorpoint
					         );

PDFX_EXPORT(PDFX_3DMeasure)
PDFX_Page_CreatePD33DMeasure(PDFX_Page       page,
					         PDFX_Point3D    annotationPlaneNormal,
					         PDFX_Point3D    firstAnchorPoint,
   					         PDFX_Point3D    secondAnchorPoint,
	        				 PDFX_Point3D    leaderLinesDirection,
   					         PDFX_Point3D    measurementValuePoint,
					         PDFX_Point3D    textYDirection,
					         PDFX_REAL       value,
							 const char*     unitsString
  					        );

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DMeasure_SetName(PDFX_3DMeasure measure,
					   const char* name);

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DMeasure_SetColor(PDFX_3DMeasure measure,
						   PDFX_RGBColor color);

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DMeasure_SetTextSize(PDFX_3DMeasure measure,
							  PDFX_REAL textsize);

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DC3DMeasure_SetTextBoxSize(PDFX_3DMeasure measure,
							 PDFX_INT32 x,
							 PDFX_INT32 y);

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DC3DMeasure_SetText(PDFX_3DMeasure measure,
						  const char* text,
						  PDFX_Encoder encoder);

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DC3DMeasure_SetProjectionAnotation(PDFX_3DMeasure measure,
										 PDFX_Annotation projectionanotation);

/*--------------------------------------------------------------------------*/
/*----- External Data ---------------------------------------------------------*/

PDFX_EXPORT(PDFX_ExData)
PDFX_Page_Create3DAnnotExData(PDFX_Page       page );

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DAnnotExData_Set3DMeasurement(PDFX_ExData exdata, PDFX_3DMeasure measure);

/*--------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------*/
/*----- 3D View ---------------------------------------------------------*/

PDFX_EXPORT(PDFX_Dict)
PDFX_Page_Create3DView    (PDFX_Page       page,
						   PDFX_U3D        u3d,
						   PDFX_Annotation	annot3d,
						   const char *name);

PDFX_EXPORT(PDFX_STATUS)
PDFX_3DView_Add3DC3DMeasure(PDFX_Dict       view,
							PDFX_3DMeasure measure);

/*--------------------------------------------------------------------------*/
/*----- image data ---------------------------------------------------------*/

PDFX_EXPORT(PDFX_Image)
PDFX_LoadPngImageFromMem  (PDFX_Doc     pdf,
                    const PDFX_BYTE    *buffer,
                          PDFX_UINT     size);

PDFX_EXPORT(PDFX_Image)
PDFX_LoadPngImageFromFile (PDFX_Doc      pdf,
                           const char    *filename);


PDFX_EXPORT(PDFX_Image)
PDFX_LoadPngImageFromFile2 (PDFX_Doc      pdf,
                            const char    *filename);


PDFX_EXPORT(PDFX_Image)
PDFX_LoadJpegImageFromFile (PDFX_Doc      pdf,
                            const char    *filename);

PDFX_EXPORT(PDFX_Image)
PDFX_LoadJpegImageFromMem   (PDFX_Doc      pdf,
                      const PDFX_BYTE     *buffer,
                            PDFX_UINT      size);

PDFX_EXPORT(PDFX_Image)
PDFX_LoadU3DFromFile (PDFX_Doc      pdf,
                            const char    *filename);

PDFX_EXPORT(PDFX_Image)
PDFX_LoadU3DFromMem  (PDFX_Doc      pdf,
               const PDFX_BYTE     *buffer,
                     PDFX_UINT      size);

PDFX_EXPORT(PDFX_Image)
PDFX_Image_LoadRaw1BitImageFromMem  (PDFX_Doc           pdf,
                           const PDFX_BYTE   *buf,
                          PDFX_UINT          width,
                          PDFX_UINT          height,
                          PDFX_UINT          line_width,
                          PDFX_BOOL          black_is1,
                          PDFX_BOOL          top_is_first);


PDFX_EXPORT(PDFX_Image)
PDFX_LoadRawImageFromFile  (PDFX_Doc           pdf,
                            const char         *filename,
                            PDFX_UINT          width,
                            PDFX_UINT          height,
                            PDFX_ColorSpace    color_space);


PDFX_EXPORT(PDFX_Image)
PDFX_LoadRawImageFromMem  (PDFX_Doc           pdf,
                           const PDFX_BYTE   *buf,
                           PDFX_UINT          width,
                           PDFX_UINT          height,
                           PDFX_ColorSpace    color_space,
                           PDFX_UINT          bits_per_component);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Image_AddSMask  (PDFX_Image    image,
                      PDFX_Image    smask);

PDFX_EXPORT(PDFX_Point)
PDFX_Image_GetSize (PDFX_Image  image);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Image_GetSize2 (PDFX_Image  image, PDFX_Point *size);


PDFX_EXPORT(PDFX_UINT)
PDFX_Image_GetWidth  (PDFX_Image   image);


PDFX_EXPORT(PDFX_UINT)
PDFX_Image_GetHeight  (PDFX_Image   image);


PDFX_EXPORT(PDFX_UINT)
PDFX_Image_GetBitsPerComponent (PDFX_Image  image);


PDFX_EXPORT(const char*)
PDFX_Image_GetColorSpace (PDFX_Image  image);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Image_SetColorMask (PDFX_Image   image,
                         PDFX_UINT    rmin,
                         PDFX_UINT    rmax,
                         PDFX_UINT    gmin,
                         PDFX_UINT    gmax,
                         PDFX_UINT    bmin,
                         PDFX_UINT    bmax);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Image_SetMaskImage  (PDFX_Image   image,
                          PDFX_Image   mask_image);


/*--------------------------------------------------------------------------*/
/*----- info dictionary ----------------------------------------------------*/


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetInfoAttr (PDFX_Doc        pdf,
                  PDFX_InfoType   type,
                  const char     *value);


PDFX_EXPORT(const char*)
PDFX_GetInfoAttr (PDFX_Doc        pdf,
                  PDFX_InfoType   type);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetInfoDateAttr (PDFX_Doc        pdf,
                      PDFX_InfoType   type,
                      PDFX_Date       value);


/*--------------------------------------------------------------------------*/
/*----- encryption ---------------------------------------------------------*/

PDFX_EXPORT(PDFX_STATUS)
PDFX_SetPassword  (PDFX_Doc      pdf,
                   const char   *owner_passwd,
                   const char   *user_passwd);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetPermission  (PDFX_Doc    pdf,
                     PDFX_UINT   permission);


PDFX_EXPORT(PDFX_STATUS)
PDFX_SetEncryptionMode  (PDFX_Doc           pdf,
                         PDFX_EncryptMode   mode,
                         PDFX_UINT          key_len);


/*--------------------------------------------------------------------------*/
/*----- compression --------------------------------------------------------*/

PDFX_EXPORT(PDFX_STATUS)
PDFX_SetCompressionMode  (PDFX_Doc    pdf,
                          PDFX_UINT   mode);


/*--------------------------------------------------------------------------*/
/*----- font ---------------------------------------------------------------*/

PDFX_EXPORT(const char*)
PDFX_Font_GetFontName  (PDFX_Font    font);


PDFX_EXPORT(const char*)
PDFX_Font_GetEncodingName  (PDFX_Font    font);


PDFX_EXPORT(PDFX_INT)
PDFX_Font_GetUnicodeWidth  (PDFX_Font       font,
                            PDFX_UNICODE    code);

PDFX_EXPORT(PDFX_Box)
PDFX_Font_GetBBox  (PDFX_Font    font);


PDFX_EXPORT(PDFX_INT)
PDFX_Font_GetAscent  (PDFX_Font  font);


PDFX_EXPORT(PDFX_INT)
PDFX_Font_GetDescent  (PDFX_Font  font);


PDFX_EXPORT(PDFX_UINT)
PDFX_Font_GetXHeight  (PDFX_Font  font);


PDFX_EXPORT(PDFX_UINT)
PDFX_Font_GetCapHeight  (PDFX_Font  font);


PDFX_EXPORT(PDFX_TextWidth)
PDFX_Font_TextWidth  (PDFX_Font          font,
                      const PDFX_BYTE   *text,
                      PDFX_UINT          len);


PDFX_EXPORT(PDFX_UINT)
PDFX_Font_MeasureText (PDFX_Font          font,
                       const PDFX_BYTE   *text,
                       PDFX_UINT          len,
                       PDFX_REAL          width,
                       PDFX_REAL          font_size,
                       PDFX_REAL          char_space,
                       PDFX_REAL          word_space,
                       PDFX_BOOL          wordwrap,
                       PDFX_REAL         *real_width);


/*--------------------------------------------------------------------------*/
/*----- attachments -------------------------------------------------------*/

PDFX_EXPORT(PDFX_EmbeddedFile)
PDFX_AttachFile  (PDFX_Doc    pdf,
                  const char *file);


/*--------------------------------------------------------------------------*/
/*----- extended graphics state --------------------------------------------*/

PDFX_EXPORT(PDFX_ExtGState)
PDFX_CreateExtGState  (PDFX_Doc  pdf);


PDFX_EXPORT(PDFX_STATUS)
PDFX_ExtGState_SetAlphaStroke  (PDFX_ExtGState   ext_gstate,
                                PDFX_REAL        value);


PDFX_EXPORT(PDFX_STATUS)
PDFX_ExtGState_SetAlphaFill  (PDFX_ExtGState   ext_gstate,
                              PDFX_REAL        value);



PDFX_EXPORT(PDFX_STATUS)
PDFX_ExtGState_SetBlendMode  (PDFX_ExtGState   ext_gstate,
                              PDFX_BlendMode   mode);


/*--------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------*/

PDFX_EXPORT(PDFX_REAL)
PDFX_Page_TextWidth  (PDFX_Page    page,
                      const char  *text);


PDFX_EXPORT(PDFX_UINT)
PDFX_Page_MeasureText  (PDFX_Page    page,
                        const char  *text,
                        PDFX_REAL    width,
                        PDFX_BOOL    wordwrap,
                        PDFX_REAL   *real_width);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetWidth  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetHeight  (PDFX_Page   page);


PDFX_EXPORT(PDFX_UINT16)
PDFX_Page_GetGMode  (PDFX_Page   page);


PDFX_EXPORT(PDFX_Point)
PDFX_Page_GetCurrentPos  (PDFX_Page   page);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_GetCurrentPos2  (PDFX_Page    page,
                           PDFX_Point  *pos);


PDFX_EXPORT(PDFX_Point)
PDFX_Page_GetCurrentTextPos (PDFX_Page   page);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_GetCurrentTextPos2  (PDFX_Page    page,
                               PDFX_Point  *pos);


PDFX_EXPORT(PDFX_Font)
PDFX_Page_GetCurrentFont  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetCurrentFontSize  (PDFX_Page   page);


PDFX_EXPORT(PDFX_TransMatrix)
PDFX_Page_GetTransMatrix  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetLineWidth  (PDFX_Page   page);


PDFX_EXPORT(PDFX_LineCap)
PDFX_Page_GetLineCap  (PDFX_Page   page);


PDFX_EXPORT(PDFX_LineJoin)
PDFX_Page_GetLineJoin  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetMiterLimit  (PDFX_Page   page);


PDFX_EXPORT(PDFX_DashMode)
PDFX_Page_GetDash  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetFlat  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetCharSpace  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetWordSpace  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetHorizontalScalling  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetTextLeading  (PDFX_Page   page);


PDFX_EXPORT(PDFX_TextRenderingMode)
PDFX_Page_GetTextRenderingMode  (PDFX_Page   page);


/* This function is obsolete. Use PDFX_Page_GetTextRise.  */
PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetTextRaise  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetTextRise  (PDFX_Page   page);


PDFX_EXPORT(PDFX_RGBColor)
PDFX_Page_GetRGBFill  (PDFX_Page   page);


PDFX_EXPORT(PDFX_RGBColor)
PDFX_Page_GetRGBStroke  (PDFX_Page   page);


PDFX_EXPORT(PDFX_CMYKColor)
PDFX_Page_GetCMYKFill  (PDFX_Page   page);


PDFX_EXPORT(PDFX_CMYKColor)
PDFX_Page_GetCMYKStroke  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetGrayFill  (PDFX_Page   page);


PDFX_EXPORT(PDFX_REAL)
PDFX_Page_GetGrayStroke  (PDFX_Page   page);


PDFX_EXPORT(PDFX_ColorSpace)
PDFX_Page_GetStrokingColorSpace (PDFX_Page   page);


PDFX_EXPORT(PDFX_ColorSpace)
PDFX_Page_GetFillingColorSpace (PDFX_Page   page);


PDFX_EXPORT(PDFX_TransMatrix)
PDFX_Page_GetTextMatrix  (PDFX_Page   page);


PDFX_EXPORT(PDFX_UINT)
PDFX_Page_GetGStateDepth  (PDFX_Page   page);


/*--------------------------------------------------------------------------*/
/*----- GRAPHICS OPERATORS -------------------------------------------------*/


/*--- General graphics state ---------------------------------------------*/

/* w */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetLineWidth  (PDFX_Page  page,
                         PDFX_REAL  line_width);

/* J */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetLineCap  (PDFX_Page     page,
                       PDFX_LineCap  line_cap);

/* j */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetLineJoin  (PDFX_Page      page,
                        PDFX_LineJoin  line_join);

/* M */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetMiterLimit  (PDFX_Page  page,
                          PDFX_REAL  miter_limit);

/* d */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetDash  (PDFX_Page           page,
                    const PDFX_REAL  *dash_ptn,
                    PDFX_UINT           num_param,
                    PDFX_REAL           phase);



/* ri --not implemented yet */

/* i */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetFlat  (PDFX_Page    page,
                    PDFX_REAL    flatness);

/* gs */

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetExtGState  (PDFX_Page        page,
                         PDFX_ExtGState   ext_gstate);

/* sh */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetShading  (PDFX_Page    page,
                       PDFX_Shading shading);


/*--- Special graphic state operator --------------------------------------*/

/* q */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_GSave  (PDFX_Page    page);

/* Q */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_GRestore  (PDFX_Page    page);

/* cm */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Concat  (PDFX_Page    page,
                   PDFX_REAL    a,
                   PDFX_REAL    b,
                   PDFX_REAL    c,
                   PDFX_REAL    d,
                   PDFX_REAL    x,
                   PDFX_REAL    y);

/*--- Path construction operator ------------------------------------------*/

/* m */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_MoveTo  (PDFX_Page    page,
                   PDFX_REAL    x,
                   PDFX_REAL    y);

/* l */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_LineTo  (PDFX_Page    page,
                   PDFX_REAL    x,
                   PDFX_REAL    y);

/* c */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_CurveTo  (PDFX_Page    page,
                    PDFX_REAL    x1,
                    PDFX_REAL    y1,
                    PDFX_REAL    x2,
                    PDFX_REAL    y2,
                    PDFX_REAL    x3,
                    PDFX_REAL    y3);

/* v */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_CurveTo2  (PDFX_Page    page,
                     PDFX_REAL    x2,
                     PDFX_REAL    y2,
                     PDFX_REAL    x3,
                     PDFX_REAL    y3);

/* y */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_CurveTo3  (PDFX_Page  page,
                     PDFX_REAL  x1,
                     PDFX_REAL  y1,
                     PDFX_REAL  x3,
                     PDFX_REAL  y3);

/* h */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ClosePath  (PDFX_Page  page);

/* re */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Rectangle  (PDFX_Page  page,
                      PDFX_REAL  x,
                      PDFX_REAL  y,
                      PDFX_REAL  width,
                      PDFX_REAL  height);


/*--- Path painting operator ---------------------------------------------*/

/* S */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Stroke  (PDFX_Page  page);

/* s */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ClosePathStroke  (PDFX_Page  page);

/* f */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Fill  (PDFX_Page  page);

/* f* */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Eofill  (PDFX_Page  page);

/* B */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_FillStroke  (PDFX_Page  page);

/* B* */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_EofillStroke  (PDFX_Page  page);

/* b */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ClosePathFillStroke  (PDFX_Page  page);

/* b* */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ClosePathEofillStroke  (PDFX_Page  page);

/* n */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_EndPath  (PDFX_Page  page);


/*--- Clipping paths operator --------------------------------------------*/

/* W */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Clip  (PDFX_Page  page);

/* W* */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Eoclip  (PDFX_Page  page);


/*--- Text object operator -----------------------------------------------*/

/* BT */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_BeginText  (PDFX_Page  page);

/* ET */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_EndText  (PDFX_Page  page);

/*--- Text state ---------------------------------------------------------*/

/* Tc */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetCharSpace  (PDFX_Page  page,
                         PDFX_REAL  value);

/* Tw */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetWordSpace  (PDFX_Page  page,
                         PDFX_REAL  value);

/* Tz */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetHorizontalScalling  (PDFX_Page  page,
                                  PDFX_REAL  value);

/* TL */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetTextLeading  (PDFX_Page  page,
                           PDFX_REAL  value);

/* Tf */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetFontAndSize  (PDFX_Page  page,
                           PDFX_Font  font,
                           PDFX_REAL  size);

/* Tr */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetTextRenderingMode  (PDFX_Page               page,
                                 PDFX_TextRenderingMode  mode);

/* Ts */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetTextRise  (PDFX_Page   page,
                        PDFX_REAL   value);

/* This function is obsolete. Use PDFX_Page_SetTextRise.  */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetTextRaise  (PDFX_Page   page,
                         PDFX_REAL   value);

/*--- Text positioning ---------------------------------------------------*/

/* Td */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_MoveTextPos  (PDFX_Page   page,
                        PDFX_REAL   x,
                        PDFX_REAL   y);

/* TD */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_MoveTextPos2  (PDFX_Page  page,
                         PDFX_REAL   x,
                         PDFX_REAL   y);

/* Tm */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetTextMatrix  (PDFX_Page         page,
                          PDFX_REAL    a,
                          PDFX_REAL    b,
                          PDFX_REAL    c,
                          PDFX_REAL    d,
                          PDFX_REAL    x,
                          PDFX_REAL    y);


/* T* */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_MoveToNextLine  (PDFX_Page  page);

/*--- Text showing -------------------------------------------------------*/

/* Tj */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ShowText  (PDFX_Page    page,
                     const char  *text);

/* TJ */

/* ' */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ShowTextNextLine  (PDFX_Page    page,
                             const char  *text);

/* " */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ShowTextNextLineEx  (PDFX_Page    page,
                               PDFX_REAL    word_space,
                               PDFX_REAL    char_space,
                               const char  *text);


/*--- Color showing ------------------------------------------------------*/

/* cs --not implemented yet */
/* CS --not implemented yet */
/* sc --not implemented yet */
/* scn --not implemented yet */
/* SC --not implemented yet */
/* SCN --not implemented yet */

/* g */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetGrayFill  (PDFX_Page   page,
                        PDFX_REAL   gray);

/* G */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetGrayStroke  (PDFX_Page   page,
                          PDFX_REAL   gray);

/* rg */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetRGBFill  (PDFX_Page  page,
                       PDFX_REAL  r,
                       PDFX_REAL  g,
                       PDFX_REAL  b);

/* RG */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetRGBStroke  (PDFX_Page  page,
                         PDFX_REAL  r,
                         PDFX_REAL  g,
                         PDFX_REAL  b);

/* k */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetCMYKFill  (PDFX_Page  page,
                        PDFX_REAL  c,
                        PDFX_REAL  m,
                        PDFX_REAL  y,
                        PDFX_REAL  k);

/* K */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetCMYKStroke  (PDFX_Page  page,
                          PDFX_REAL  c,
                          PDFX_REAL  m,
                          PDFX_REAL  y,
                          PDFX_REAL  k);

/*--- Shading patterns ---------------------------------------------------*/

/* Notes for docs:
 * - ShadingType must be PDFX_SHADING_FREE_FORM_TRIANGLE_MESH (the only
 *   defined option...)
 * - colorSpace must be PDFX_CS_DEVICE_RGB for now.
 */
PDFX_EXPORT(PDFX_Shading)
PDFX_Shading_New  (PDFX_Doc         pdf,
                   PDFX_ShadingType type,
                   PDFX_ColorSpace  colorSpace,
                   PDFX_REAL xMin, PDFX_REAL xMax,
                   PDFX_REAL yMin, PDFX_REAL yMax);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Shading_AddVertexRGB(PDFX_Shading shading,
                          PDFX_Shading_FreeFormTriangleMeshEdgeFlag edgeFlag,
                          PDFX_REAL x, PDFX_REAL y,
                          PDFX_UINT8 r, PDFX_UINT8 g, PDFX_UINT8 b);

/*--- In-line images -----------------------------------------------------*/

/* BI --not implemented yet */
/* ID --not implemented yet */
/* EI --not implemented yet */

/*--- XObjects -----------------------------------------------------------*/

/* Do */
PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_ExecuteXObject  (PDFX_Page     page,
                           PDFX_XObject  obj);

/*--- Content streams ----------------------------------------------------*/

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_New_Content_Stream  (PDFX_Page page,
                               PDFX_Dict* new_stream);

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Insert_Shared_Content_Stream  (PDFX_Page page,
                                         PDFX_Dict shared_stream);


/*--- Marked content -----------------------------------------------------*/

/* BMC --not implemented yet */
/* BDC --not implemented yet */
/* EMC --not implemented yet */
/* MP --not implemented yet */
/* DP --not implemented yet */

/*--- Compatibility ------------------------------------------------------*/

/* BX --not implemented yet */
/* EX --not implemented yet */

PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_DrawImage  (PDFX_Page    page,
                      PDFX_Image   image,
                      PDFX_REAL    x,
                      PDFX_REAL    y,
                      PDFX_REAL    width,
                      PDFX_REAL    height);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Circle  (PDFX_Page     page,
                   PDFX_REAL     x,
                   PDFX_REAL     y,
                   PDFX_REAL     ray);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Ellipse  (PDFX_Page   page,
                    PDFX_REAL   x,
                    PDFX_REAL   y,
                    PDFX_REAL  xray,
                    PDFX_REAL  yray);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_Arc  (PDFX_Page    page,
                PDFX_REAL    x,
                PDFX_REAL    y,
                PDFX_REAL    ray,
                PDFX_REAL    ang1,
                PDFX_REAL    ang2);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_TextOut  (PDFX_Page    page,
                    PDFX_REAL    xpos,
                    PDFX_REAL    ypos,
                    const char  *text);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_TextRect  (PDFX_Page            page,
                     PDFX_REAL            left,
                     PDFX_REAL            top,
                     PDFX_REAL            right,
                     PDFX_REAL            bottom,
                     const char          *text,
                     PDFX_TextAlignment   align,
                     PDFX_UINT           *len);


PDFX_EXPORT(PDFX_STATUS)
PDFX_Page_SetSlideShow  (PDFX_Page              page,
                         PDFX_TransitionStyle   type,
                         PDFX_REAL              disp_time,
                         PDFX_REAL              trans_time);


PDFX_EXPORT(PDFX_OutputIntent)
PDFX_ICC_LoadIccFromMem (PDFX_Doc   pdf,
                        PDFX_MMgr   mmgr,
                        PDFX_Stream iccdata,
                        PDFX_Xref   xref,
                        int         numcomponent);

PDFX_EXPORT(PDFX_OutputIntent)
PDFX_LoadIccProfileFromFile  (PDFX_Doc  pdf,
                            const char* icc_file_name,
                                   int  numcomponent);

/* ------------------------------------------------------------------ */
/* pdflib extensions (not part of libharu)                             */
/*                                                                     */
/* The PDFX_* calls above are implemented over pdflib, so the same     */
/* program can be rendered by libharu or PoDoFo. Select one here, or   */
/* set the environment variable PDFLIB_BACKEND=auto|dummy|libharu|     */
/* podofo before PDFX_New(). The default is AUTO (PoDoFo first).       */
/* ------------------------------------------------------------------ */

#define PDFLIB_PDFX_BACKEND_AUTO    0
#define PDFLIB_PDFX_BACKEND_DUMMY   1
#define PDFLIB_PDFX_BACKEND_LIBHARU 2
#define PDFLIB_PDFX_BACKEND_PODOFO  3

PDFX_EXPORT(PDFX_STATUS) PDFX_SetBackend (PDFX_UINT backend);
PDFX_EXPORT(PDFX_UINT)   PDFX_GetBackend (void);
PDFX_EXPORT(const char*) PDFX_GetBackendName (void);
PDFX_EXPORT(const char*) PDFX_GetBackendVersion (void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* _PDFX_H */

