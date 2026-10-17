#define PDFX_MAJOR_VERSION 2
#define PDFX_MINOR_VERSION 4
#define PDFX_BUGFIX_VERSION 4
#define PDFX_EXTRA_VERSION ""

#define pdfx_vquote(x) #x
#define pdfx_vexp(x) pdfx_vquote(x)

#define PDFX_VERSION_TEXT (pdfx_vexp(PDFX_MAJOR_VERSION) "." pdfx_vexp(PDFX_MINOR_VERSION) "." pdfx_vexp(PDFX_BUGFIX_VERSION) PDFX_EXTRA_VERSION)
#define PDFX_VERSION_ID ((PDFX_MAJOR_VERSION) * 10000 + (PDFX_MINOR_VERSION) * 100 + (PDFX_BUGFIX_VERSION))
