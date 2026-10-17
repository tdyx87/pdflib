/*
 * << Haru Free PDF Library >> -- pdfx_consts.h
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


#ifndef _PDFX_CONSTS_H
#define _PDFX_CONSTS_H

/*----------------------------------------------------------------------------*/

#define  PDFX_TRUE                  1
#define  PDFX_FALSE                 0

#define  PDFX_OK                    0
#define  PDFX_NOERROR               0

/*----- default values -------------------------------------------------------*/

/* buffer size which is required when we convert to character string. */
#define PDFX_TMP_BUF_SIZ            512
#define PDFX_SHORT_BUF_SIZ          32
#define PDFX_REAL_LEN               64
#define PDFX_INT_LEN                11
#define PDFX_TEXT_DEFAULT_LEN       256
#define PDFX_UNICODE_HEADER_LEN     2
#define PDFX_DATE_TIME_STR_LEN      23

/* length of each item defined in PDF */
#define PDFX_BYTE_OFFSET_LEN        10
#define PDFX_OBJ_ID_LEN             7
#define PDFX_GEN_NO_LEN             5

/* default value of Graphic State */
#define PDFX_DEF_FONT               "Helvetica"
#define PDFX_DEF_PAGE_LAYOUT        PDFX_PAGE_LAYOUT_SINGLE
#define PDFX_DEF_PAGE_MODE          PDFX_PAGE_MODE_USE_NONE
#define PDFX_DEF_WORDSPACE          0
#define PDFX_DEF_CHARSPACE          0
#define PDFX_DEF_FONTSIZE           10
#define PDFX_DEF_HSCALING           100
#define PDFX_DEF_LEADING            0
#define PDFX_DEF_RENDERING_MODE     PDFX_FILL
#define PDFX_DEF_RISE               0
#define PDFX_DEF_RAISE              PDFX_DEF_RISE
#define PDFX_DEF_LINEWIDTH          1
#define PDFX_DEF_LINECAP            PDFX_BUTT_END
#define PDFX_DEF_LINEJOIN           PDFX_MITER_JOIN
#define PDFX_DEF_MITERLIMIT         10
#define PDFX_DEF_FLATNESS           1
#define PDFX_DEF_PAGE_NUM           1

#define PDFX_BS_DEF_WIDTH           1

/* default page-size */
#define PDFX_DEF_PAGE_WIDTH         595.276F
#define PDFX_DEF_PAGE_HEIGHT        841.89F

/*---------------------------------------------------------------------------*/
/*----- compression mode ----------------------------------------------------*/

#define  PDFX_COMP_NONE            0x00
#define  PDFX_COMP_TEXT            0x01
#define  PDFX_COMP_IMAGE           0x02
#define  PDFX_COMP_METADATA        0x04
#define  PDFX_COMP_ALL             0x0F
/* #define  PDFX_COMP_BEST_COMPRESS   0x10
 * #define  PDFX_COMP_BEST_SPEED      0x20
 */
#define  PDFX_COMP_MASK            0xFF


/*----------------------------------------------------------------------------*/
/*----- permission flags (only Revision 2 is supported)-----------------------*/

#define PDFX_ENABLE_READ         0
#define PDFX_ENABLE_PRINT        4
#define PDFX_ENABLE_EDIT_ALL     8
#define PDFX_ENABLE_COPY         16
#define PDFX_ENABLE_EDIT         32


/*----------------------------------------------------------------------------*/
/*------ viewer preferences definitions --------------------------------------*/

#define PDFX_HIDE_TOOLBAR    1
#define PDFX_HIDE_MENUBAR    2
#define PDFX_HIDE_WINDOW_UI  4
#define PDFX_FIT_WINDOW      8
#define PDFX_CENTER_WINDOW   16
#define PDFX_PRINT_SCALING_NONE   32


/*---------------------------------------------------------------------------*/
/*------ limitation of object implementation (PDF1.4) -----------------------*/

#define PDFX_LIMIT_MAX_INT             2147483647
#define PDFX_LIMIT_MIN_INT             -2147483647

#define PDFX_LIMIT_MAX_REAL             3.4E38f // per PDF 1.7 spec, Annex C, old value  32767
#define PDFX_LIMIT_MIN_REAL            -3.4E38f // per PDF 1.7 spec, Annex C, old value -32767

#define PDFX_LIMIT_MAX_STRING_LEN      2147483646 // per PDF 1.7 spec, limit 32767 is for strings in content stream and no limit in other cases => open the limit to max Integer, old value 65535
#define PDFX_LIMIT_MAX_NAME_LEN        127

#define PDFX_LIMIT_MAX_ARRAY           8388607  // per PDF 1.7 spec, "Maximum number of indirect objects in a PDF file" is 8388607, old value 8191
#define PDFX_LIMIT_MAX_DICT_ELEMENT    8388607  // per PDF 1.7 spec, "Maximum number of indirect objects in a PDF file" is 8388607, old value 4095
#define PDFX_LIMIT_MAX_XREF_ELEMENT    8388607
#define PDFX_LIMIT_MAX_GSTATE          28
#define PDFX_LIMIT_MAX_DEVICE_N        8
#define PDFX_LIMIT_MAX_DEVICE_N_V15    32
#define PDFX_LIMIT_MAX_CID             65535
#define PDFX_MAX_GENERATION_NUM        65535

#define PDFX_MIN_PAGE_HEIGHT           3
#define PDFX_MIN_PAGE_WIDTH            3
#define PDFX_MAX_PAGE_HEIGHT           14400
#define PDFX_MAX_PAGE_WIDTH            14400
#define PDFX_MIN_MAGNIFICATION_FACTOR  8
#define PDFX_MAX_MAGNIFICATION_FACTOR  3200

/*---------------------------------------------------------------------------*/
/*------ limitation of various properties -----------------------------------*/

#define PDFX_MIN_PAGE_SIZE          3
#define PDFX_MAX_PAGE_SIZE          14400
#define PDFX_MIN_HORIZONTALSCALING  10
#define PDFX_MAX_HORIZONTALSCALING  300
#define PDFX_MIN_WORDSPACE          -30
#define PDFX_MAX_WORDSPACE          300
#define PDFX_MIN_CHARSPACE          -30
#define PDFX_MAX_CHARSPACE          300
#define PDFX_MAX_FONTSIZE           600
#define PDFX_MAX_ZOOMSIZE           10
#define PDFX_MAX_LEADING            300
#define PDFX_MAX_LINEWIDTH          100
#define PDFX_MAX_DASH_PATTERN       100

#define PDFX_MAX_JWW_NUM            128

/*----------------------------------------------------------------------------*/
/*----- country code definition ----------------------------------------------*/

#define PDFX_COUNTRY_AF  "AF"    /* AFGHANISTAN */
#define PDFX_COUNTRY_AL  "AL"    /* ALBANIA */
#define PDFX_COUNTRY_DZ  "DZ"    /* ALGERIA */
#define PDFX_COUNTRY_AS  "AS"    /* AMERICAN SAMOA */
#define PDFX_COUNTRY_AD  "AD"    /* ANDORRA */
#define PDFX_COUNTRY_AO  "AO"    /* ANGOLA */
#define PDFX_COUNTRY_AI  "AI"    /* ANGUILLA */
#define PDFX_COUNTRY_AQ  "AQ"    /* ANTARCTICA */
#define PDFX_COUNTRY_AG  "AG"    /* ANTIGUA AND BARBUDA */
#define PDFX_COUNTRY_AR  "AR"    /* ARGENTINA */
#define PDFX_COUNTRY_AM  "AM"    /* ARMENIA */
#define PDFX_COUNTRY_AW  "AW"    /* ARUBA */
#define PDFX_COUNTRY_AU  "AU"    /* AUSTRALIA */
#define PDFX_COUNTRY_AT  "AT"    /* AUSTRIA */
#define PDFX_COUNTRY_AZ  "AZ"    /* AZERBAIJAN */
#define PDFX_COUNTRY_BS  "BS"    /* BAHAMAS */
#define PDFX_COUNTRY_BH  "BH"    /* BAHRAIN */
#define PDFX_COUNTRY_BD  "BD"    /* BANGLADESH */
#define PDFX_COUNTRY_BB  "BB"    /* BARBADOS */
#define PDFX_COUNTRY_BY  "BY"    /* BELARUS */
#define PDFX_COUNTRY_BE  "BE"    /* BELGIUM */
#define PDFX_COUNTRY_BZ  "BZ"    /* BELIZE */
#define PDFX_COUNTRY_BJ  "BJ"    /* BENIN */
#define PDFX_COUNTRY_BM  "BM"    /* BERMUDA */
#define PDFX_COUNTRY_BT  "BT"    /* BHUTAN */
#define PDFX_COUNTRY_BO  "BO"    /* BOLIVIA */
#define PDFX_COUNTRY_BA  "BA"    /* BOSNIA AND HERZEGOWINA */
#define PDFX_COUNTRY_BW  "BW"    /* BOTSWANA */
#define PDFX_COUNTRY_BV  "BV"    /* BOUVET ISLAND */
#define PDFX_COUNTRY_BR  "BR"    /* BRAZIL */
#define PDFX_COUNTRY_IO  "IO"    /* BRITISH INDIAN OCEAN TERRITORY */
#define PDFX_COUNTRY_BN  "BN"    /* BRUNEI DARUSSALAM */
#define PDFX_COUNTRY_BG  "BG"    /* BULGARIA */
#define PDFX_COUNTRY_BF  "BF"    /* BURKINA FASO */
#define PDFX_COUNTRY_BI  "BI"    /* BURUNDI */
#define PDFX_COUNTRY_KH  "KH"    /* CAMBODIA */
#define PDFX_COUNTRY_CM  "CM"    /* CAMEROON */
#define PDFX_COUNTRY_CA  "CA"    /* CANADA */
#define PDFX_COUNTRY_CV  "CV"    /* CAPE VERDE */
#define PDFX_COUNTRY_KY  "KY"    /* CAYMAN ISLANDS */
#define PDFX_COUNTRY_CF  "CF"    /* CENTRAL AFRICAN REPUBLIC */
#define PDFX_COUNTRY_TD  "TD"    /* CHAD */
#define PDFX_COUNTRY_CL  "CL"    /* CHILE */
#define PDFX_COUNTRY_CN  "CN"    /* CHINA */
#define PDFX_COUNTRY_CX  "CX"    /* CHRISTMAS ISLAND */
#define PDFX_COUNTRY_CC  "CC"    /* COCOS (KEELING) ISLANDS */
#define PDFX_COUNTRY_CO  "CO"    /* COLOMBIA */
#define PDFX_COUNTRY_KM  "KM"    /* COMOROS */
#define PDFX_COUNTRY_CG  "CG"    /* CONGO */
#define PDFX_COUNTRY_CK  "CK"    /* COOK ISLANDS */
#define PDFX_COUNTRY_CR  "CR"    /* COSTA RICA */
#define PDFX_COUNTRY_CI  "CI"    /* COTE D'IVOIRE */
#define PDFX_COUNTRY_HR  "HR"    /* CROATIA (local name: Hrvatska) */
#define PDFX_COUNTRY_CU  "CU"    /* CUBA */
#define PDFX_COUNTRY_CY  "CY"    /* CYPRUS */
#define PDFX_COUNTRY_CZ  "CZ"    /* CZECH REPUBLIC */
#define PDFX_COUNTRY_DK  "DK"    /* DENMARK */
#define PDFX_COUNTRY_DJ  "DJ"    /* DJIBOUTI */
#define PDFX_COUNTRY_DM  "DM"    /* DOMINICA */
#define PDFX_COUNTRY_DO  "DO"    /* DOMINICAN REPUBLIC */
#define PDFX_COUNTRY_TP  "TP"    /* EAST TIMOR */
#define PDFX_COUNTRY_EC  "EC"    /* ECUADOR */
#define PDFX_COUNTRY_EG  "EG"    /* EGYPT */
#define PDFX_COUNTRY_SV  "SV"    /* EL SALVADOR */
#define PDFX_COUNTRY_GQ  "GQ"    /* EQUATORIAL GUINEA */
#define PDFX_COUNTRY_ER  "ER"    /* ERITREA */
#define PDFX_COUNTRY_EE  "EE"    /* ESTONIA */
#define PDFX_COUNTRY_ET  "ET"    /* ETHIOPIA */
#define PDFX_COUNTRY_FK  "FK"   /* FALKLAND ISLANDS (MALVINAS) */
#define PDFX_COUNTRY_FO  "FO"    /* FAROE ISLANDS */
#define PDFX_COUNTRY_FJ  "FJ"    /* FIJI */
#define PDFX_COUNTRY_FI  "FI"    /* FINLAND */
#define PDFX_COUNTRY_FR  "FR"    /* FRANCE */
#define PDFX_COUNTRY_FX  "FX"    /* FRANCE, METROPOLITAN */
#define PDFX_COUNTRY_GF  "GF"    /* FRENCH GUIANA */
#define PDFX_COUNTRY_PF  "PF"    /* FRENCH POLYNESIA */
#define PDFX_COUNTRY_TF  "TF"    /* FRENCH SOUTHERN TERRITORIES */
#define PDFX_COUNTRY_GA  "GA"    /* GABON */
#define PDFX_COUNTRY_GM  "GM"    /* GAMBIA */
#define PDFX_COUNTRY_GE  "GE"    /* GEORGIA */
#define PDFX_COUNTRY_DE  "DE"    /* GERMANY */
#define PDFX_COUNTRY_GH  "GH"    /* GHANA */
#define PDFX_COUNTRY_GI  "GI"    /* GIBRALTAR */
#define PDFX_COUNTRY_GR  "GR"    /* GREECE */
#define PDFX_COUNTRY_GL  "GL"    /* GREENLAND */
#define PDFX_COUNTRY_GD  "GD"    /* GRENADA */
#define PDFX_COUNTRY_GP  "GP"    /* GUADELOUPE */
#define PDFX_COUNTRY_GU  "GU"    /* GUAM */
#define PDFX_COUNTRY_GT  "GT"    /* GUATEMALA */
#define PDFX_COUNTRY_GN  "GN"    /* GUINEA */
#define PDFX_COUNTRY_GW  "GW"    /* GUINEA-BISSAU */
#define PDFX_COUNTRY_GY  "GY"    /* GUYANA */
#define PDFX_COUNTRY_HT  "HT"    /* HAITI */
#define PDFX_COUNTRY_HM  "HM"    /* HEARD AND MC DONALD ISLANDS */
#define PDFX_COUNTRY_HN  "HN"    /* HONDURAS */
#define PDFX_COUNTRY_HK  "HK"    /* HONG KONG */
#define PDFX_COUNTRY_HU  "HU"    /* HUNGARY */
#define PDFX_COUNTRY_IS  "IS"    /* ICELAND */
#define PDFX_COUNTRY_IN  "IN"    /* INDIA */
#define PDFX_COUNTRY_ID  "ID"    /* INDONESIA */
#define PDFX_COUNTRY_IR  "IR"    /* IRAN (ISLAMIC REPUBLIC OF) */
#define PDFX_COUNTRY_IQ  "IQ"    /* IRAQ */
#define PDFX_COUNTRY_IE  "IE"    /* IRELAND */
#define PDFX_COUNTRY_IL  "IL"    /* ISRAEL */
#define PDFX_COUNTRY_IT  "IT"    /* ITALY */
#define PDFX_COUNTRY_JM  "JM"    /* JAMAICA */
#define PDFX_COUNTRY_JP  "JP"    /* JAPAN */
#define PDFX_COUNTRY_JO  "JO"    /* JORDAN */
#define PDFX_COUNTRY_KZ  "KZ"    /* KAZAKHSTAN */
#define PDFX_COUNTRY_KE  "KE"    /* KENYA */
#define PDFX_COUNTRY_KI  "KI"    /* KIRIBATI */
#define PDFX_COUNTRY_KP  "KP"    /* KOREA, DEMOCRATIC PEOPLE'S REPUBLIC OF */
#define PDFX_COUNTRY_KR  "KR"    /* KOREA, REPUBLIC OF */
#define PDFX_COUNTRY_KW  "KW"    /* KUWAIT */
#define PDFX_COUNTRY_KG  "KG"    /* KYRGYZSTAN */
#define PDFX_COUNTRY_LA  "LA"    /* LAO PEOPLE'S DEMOCRATIC REPUBLIC */
#define PDFX_COUNTRY_LV  "LV"    /* LATVIA */
#define PDFX_COUNTRY_LB  "LB"    /* LEBANON */
#define PDFX_COUNTRY_LS  "LS"    /* LESOTHO */
#define PDFX_COUNTRY_LR  "LR"    /* LIBERIA */
#define PDFX_COUNTRY_LY  "LY"    /* LIBYAN ARAB JAMAHIRIYA */
#define PDFX_COUNTRY_LI  "LI"    /* LIECHTENSTEIN */
#define PDFX_COUNTRY_LT  "LT"    /* LITHUANIA */
#define PDFX_COUNTRY_LU  "LU"    /* LUXEMBOURG */
#define PDFX_COUNTRY_MO  "MO"    /* MACAU */
#define PDFX_COUNTRY_MK  "MK"   /* MACEDONIA, THE FORMER YUGOSLAV REPUBLIC OF */
#define PDFX_COUNTRY_MG  "MG"    /* MADAGASCAR */
#define PDFX_COUNTRY_MW  "MW"    /* MALAWI */
#define PDFX_COUNTRY_MY  "MY"    /* MALAYSIA */
#define PDFX_COUNTRY_MV  "MV"    /* MALDIVES */
#define PDFX_COUNTRY_ML  "ML"    /* MALI */
#define PDFX_COUNTRY_MT  "MT"    /* MALTA */
#define PDFX_COUNTRY_MH  "MH"    /* MARSHALL ISLANDS */
#define PDFX_COUNTRY_MQ  "MQ"    /* MARTINIQUE */
#define PDFX_COUNTRY_MR  "MR"    /* MAURITANIA */
#define PDFX_COUNTRY_MU  "MU"    /* MAURITIUS */
#define PDFX_COUNTRY_YT  "YT"    /* MAYOTTE */
#define PDFX_COUNTRY_MX  "MX"    /* MEXICO */
#define PDFX_COUNTRY_FM  "FM"    /* MICRONESIA, FEDERATED STATES OF */
#define PDFX_COUNTRY_MD  "MD"    /* MOLDOVA, REPUBLIC OF */
#define PDFX_COUNTRY_MC  "MC"    /* MONACO */
#define PDFX_COUNTRY_MN  "MN"    /* MONGOLIA */
#define PDFX_COUNTRY_MS  "MS"    /* MONTSERRAT */
#define PDFX_COUNTRY_MA  "MA"    /* MOROCCO */
#define PDFX_COUNTRY_MZ  "MZ"    /* MOZAMBIQUE */
#define PDFX_COUNTRY_MM  "MM"    /* MYANMAR */
#define PDFX_COUNTRY_NA  "NA"    /* NAMIBIA */
#define PDFX_COUNTRY_NR  "NR"    /* NAURU */
#define PDFX_COUNTRY_NP  "NP"    /* NEPAL */
#define PDFX_COUNTRY_NL  "NL"    /* NETHERLANDS */
#define PDFX_COUNTRY_AN  "AN"    /* NETHERLANDS ANTILLES */
#define PDFX_COUNTRY_NC  "NC"    /* NEW CALEDONIA */
#define PDFX_COUNTRY_NZ  "NZ"    /* NEW ZEALAND */
#define PDFX_COUNTRY_NI  "NI"    /* NICARAGUA */
#define PDFX_COUNTRY_NE  "NE"    /* NIGER */
#define PDFX_COUNTRY_NG  "NG"    /* NIGERIA */
#define PDFX_COUNTRY_NU  "NU"    /* NIUE */
#define PDFX_COUNTRY_NF  "NF"    /* NORFOLK ISLAND */
#define PDFX_COUNTRY_MP  "MP"    /* NORTHERN MARIANA ISLANDS */
#define PDFX_COUNTRY_NO  "NO"    /* NORWAY */
#define PDFX_COUNTRY_OM  "OM"    /* OMAN */
#define PDFX_COUNTRY_PK  "PK"    /* PAKISTAN */
#define PDFX_COUNTRY_PW  "PW"    /* PALAU */
#define PDFX_COUNTRY_PA  "PA"    /* PANAMA */
#define PDFX_COUNTRY_PG  "PG"    /* PAPUA NEW GUINEA */
#define PDFX_COUNTRY_PY  "PY"    /* PARAGUAY */
#define PDFX_COUNTRY_PE  "PE"    /* PERU */
#define PDFX_COUNTRY_PH  "PH"    /* PHILIPPINES */
#define PDFX_COUNTRY_PN  "PN"    /* PITCAIRN */
#define PDFX_COUNTRY_PL  "PL"    /* POLAND */
#define PDFX_COUNTRY_PT  "PT"    /* PORTUGAL */
#define PDFX_COUNTRY_PR  "PR"    /* PUERTO RICO */
#define PDFX_COUNTRY_QA  "QA"    /* QATAR */
#define PDFX_COUNTRY_RE  "RE"    /* REUNION */
#define PDFX_COUNTRY_RO  "RO"    /* ROMANIA */
#define PDFX_COUNTRY_RU  "RU"    /* RUSSIAN FEDERATION */
#define PDFX_COUNTRY_RW  "RW"    /* RWANDA */
#define PDFX_COUNTRY_KN  "KN"    /* SAINT KITTS AND NEVIS */
#define PDFX_COUNTRY_LC  "LC"    /* SAINT LUCIA */
#define PDFX_COUNTRY_VC  "VC"    /* SAINT VINCENT AND THE GRENADINES */
#define PDFX_COUNTRY_WS  "WS"    /* SAMOA */
#define PDFX_COUNTRY_SM  "SM"    /* SAN MARINO */
#define PDFX_COUNTRY_ST  "ST"    /* SAO TOME AND PRINCIPE */
#define PDFX_COUNTRY_SA  "SA"    /* SAUDI ARABIA */
#define PDFX_COUNTRY_SN  "SN"    /* SENEGAL */
#define PDFX_COUNTRY_SC  "SC"    /* SEYCHELLES */
#define PDFX_COUNTRY_SL  "SL"    /* SIERRA LEONE */
#define PDFX_COUNTRY_SG  "SG"    /* SINGAPORE */
#define PDFX_COUNTRY_SK  "SK"    /* SLOVAKIA (Slovak Republic) */
#define PDFX_COUNTRY_SI  "SI"    /* SLOVENIA */
#define PDFX_COUNTRY_SB  "SB"    /* SOLOMON ISLANDS */
#define PDFX_COUNTRY_SO  "SO"    /* SOMALIA */
#define PDFX_COUNTRY_ZA  "ZA"    /* SOUTH AFRICA */
#define PDFX_COUNTRY_ES  "ES"    /* SPAIN */
#define PDFX_COUNTRY_LK  "LK"    /* SRI LANKA */
#define PDFX_COUNTRY_SH  "SH"    /* ST. HELENA */
#define PDFX_COUNTRY_PM  "PM"    /* ST. PIERRE AND MIQUELON */
#define PDFX_COUNTRY_SD  "SD"    /* SUDAN */
#define PDFX_COUNTRY_SR  "SR"    /* SURINAME */
#define PDFX_COUNTRY_SJ  "SJ"    /* SVALBARD AND JAN MAYEN ISLANDS */
#define PDFX_COUNTRY_SZ  "SZ"    /* SWAZILAND */
#define PDFX_COUNTRY_SE  "SE"    /* SWEDEN */
#define PDFX_COUNTRY_CH  "CH"    /* SWITZERLAND */
#define PDFX_COUNTRY_SY  "SY"    /* SYRIAN ARAB REPUBLIC */
#define PDFX_COUNTRY_TW  "TW"    /* TAIWAN, PROVINCE OF CHINA */
#define PDFX_COUNTRY_TJ  "TJ"    /* TAJIKISTAN */
#define PDFX_COUNTRY_TZ  "TZ"    /* TANZANIA, UNITED REPUBLIC OF */
#define PDFX_COUNTRY_TH  "TH"    /* THAILAND */
#define PDFX_COUNTRY_TG  "TG"    /* TOGO */
#define PDFX_COUNTRY_TK  "TK"    /* TOKELAU */
#define PDFX_COUNTRY_TO  "TO"    /* TONGA */
#define PDFX_COUNTRY_TT  "TT"    /* TRINIDAD AND TOBAGO */
#define PDFX_COUNTRY_TN  "TN"    /* TUNISIA */
#define PDFX_COUNTRY_TR  "TR"    /* TURKEY */
#define PDFX_COUNTRY_TM  "TM"    /* TURKMENISTAN */
#define PDFX_COUNTRY_TC  "TC"    /* TURKS AND CAICOS ISLANDS */
#define PDFX_COUNTRY_TV  "TV"    /* TUVALU */
#define PDFX_COUNTRY_UG  "UG"    /* UGANDA */
#define PDFX_COUNTRY_UA  "UA"    /* UKRAINE */
#define PDFX_COUNTRY_AE  "AE"    /* UNITED ARAB EMIRATES */
#define PDFX_COUNTRY_GB  "GB"    /* UNITED KINGDOM */
#define PDFX_COUNTRY_US  "US"    /* UNITED STATES */
#define PDFX_COUNTRY_UM  "UM"    /* UNITED STATES MINOR OUTLYING ISLANDS */
#define PDFX_COUNTRY_UY  "UY"    /* URUGUAY */
#define PDFX_COUNTRY_UZ  "UZ"    /* UZBEKISTAN */
#define PDFX_COUNTRY_VU  "VU"    /* VANUATU */
#define PDFX_COUNTRY_VA  "VA"    /* VATICAN CITY STATE (HOLY SEE) */
#define PDFX_COUNTRY_VE  "VE"    /* VENEZUELA */
#define PDFX_COUNTRY_VN  "VN"    /* VIET NAM */
#define PDFX_COUNTRY_VG  "VG"    /* VIRGIN ISLANDS (BRITISH) */
#define PDFX_COUNTRY_VI  "VI"    /* VIRGIN ISLANDS (U.S.) */
#define PDFX_COUNTRY_WF  "WF"    /* WALLIS AND FUTUNA ISLANDS */
#define PDFX_COUNTRY_EH  "EH"    /* WESTERN SAHARA */
#define PDFX_COUNTRY_YE  "YE"    /* YEMEN */
#define PDFX_COUNTRY_YU  "YU"    /* YUGOSLAVIA */
#define PDFX_COUNTRY_ZR  "ZR"    /* ZAIRE */
#define PDFX_COUNTRY_ZM  "ZM"    /* ZAMBIA */
#define PDFX_COUNTRY_ZW  "ZW"    /* ZIMBABWE */

/*----------------------------------------------------------------------------*/
/*----- lang code definition -------------------------------------------------*/

#define PDFX_LANG_AA    "aa"     /* Afar */
#define PDFX_LANG_AB    "ab"     /* Abkhazian */
#define PDFX_LANG_AF    "af"     /* Afrikaans */
#define PDFX_LANG_AM    "am"     /* Amharic */
#define PDFX_LANG_AR    "ar"     /* Arabic */
#define PDFX_LANG_AS    "as"     /* Assamese */
#define PDFX_LANG_AY    "ay"     /* Aymara */
#define PDFX_LANG_AZ    "az"     /* Azerbaijani */
#define PDFX_LANG_BA    "ba"     /* Bashkir */
#define PDFX_LANG_BE    "be"     /* Byelorussian */
#define PDFX_LANG_BG    "bg"     /* Bulgarian */
#define PDFX_LANG_BH    "bh"     /* Bihari */
#define PDFX_LANG_BI    "bi"     /* Bislama */
#define PDFX_LANG_BN    "bn"     /* Bengali Bangla */
#define PDFX_LANG_BO    "bo"     /* Tibetan */
#define PDFX_LANG_BR    "br"     /* Breton */
#define PDFX_LANG_CA    "ca"     /* Catalan */
#define PDFX_LANG_CO    "co"     /* Corsican */
#define PDFX_LANG_CS    "cs"     /* Czech */
#define PDFX_LANG_CY    "cy"     /* Welsh */
#define PDFX_LANG_DA    "da"     /* Danish */
#define PDFX_LANG_DE    "de"     /* German */
#define PDFX_LANG_DZ    "dz"     /* Bhutani */
#define PDFX_LANG_EL    "el"     /* Greek */
#define PDFX_LANG_EN    "en"     /* English */
#define PDFX_LANG_EO    "eo"     /* Esperanto */
#define PDFX_LANG_ES    "es"     /* Spanish */
#define PDFX_LANG_ET    "et"     /* Estonian */
#define PDFX_LANG_EU    "eu"     /* Basque */
#define PDFX_LANG_FA    "fa"     /* Persian */
#define PDFX_LANG_FI    "fi"     /* Finnish */
#define PDFX_LANG_FJ    "fj"     /* Fiji */
#define PDFX_LANG_FO    "fo"     /* Faeroese */
#define PDFX_LANG_FR    "fr"     /* French */
#define PDFX_LANG_FY    "fy"     /* Frisian */
#define PDFX_LANG_GA    "ga"     /* Irish */
#define PDFX_LANG_GD    "gd"     /* Scots Gaelic */
#define PDFX_LANG_GL    "gl"     /* Galician */
#define PDFX_LANG_GN    "gn"     /* Guarani */
#define PDFX_LANG_GU    "gu"     /* Gujarati */
#define PDFX_LANG_HA    "ha"     /* Hausa */
#define PDFX_LANG_HI    "hi"     /* Hindi */
#define PDFX_LANG_HR    "hr"     /* Croatian */
#define PDFX_LANG_HU    "hu"     /* Hungarian */
#define PDFX_LANG_HY    "hy"     /* Armenian */
#define PDFX_LANG_IA    "ia"     /* Interlingua */
#define PDFX_LANG_IE    "ie"     /* Interlingue */
#define PDFX_LANG_IK    "ik"     /* Inupiak */
#define PDFX_LANG_IN    "in"     /* Indonesian */
#define PDFX_LANG_IS    "is"     /* Icelandic */
#define PDFX_LANG_IT    "it"     /* Italian */
#define PDFX_LANG_IW    "iw"     /* Hebrew */
#define PDFX_LANG_JA    "ja"     /* Japanese */
#define PDFX_LANG_JI    "ji"     /* Yiddish */
#define PDFX_LANG_JW    "jw"     /* Javanese */
#define PDFX_LANG_KA    "ka"     /* Georgian */
#define PDFX_LANG_KK    "kk"     /* Kazakh */
#define PDFX_LANG_KL    "kl"     /* Greenlandic */
#define PDFX_LANG_KM    "km"     /* Cambodian */
#define PDFX_LANG_KN    "kn"     /* Kannada */
#define PDFX_LANG_KO    "ko"     /* Korean */
#define PDFX_LANG_KS    "ks"     /* Kashmiri */
#define PDFX_LANG_KU    "ku"     /* Kurdish */
#define PDFX_LANG_KY    "ky"     /* Kirghiz */
#define PDFX_LANG_LA    "la"     /* Latin */
#define PDFX_LANG_LN    "ln"     /* Lingala */
#define PDFX_LANG_LO    "lo"     /* Laothian */
#define PDFX_LANG_LT    "lt"     /* Lithuanian */
#define PDFX_LANG_LV    "lv"     /* Latvian,Lettish */
#define PDFX_LANG_MG    "mg"     /* Malagasy */
#define PDFX_LANG_MI    "mi"     /* Maori */
#define PDFX_LANG_MK    "mk"     /* Macedonian */
#define PDFX_LANG_ML    "ml"     /* Malayalam */
#define PDFX_LANG_MN    "mn"     /* Mongolian */
#define PDFX_LANG_MO    "mo"     /* Moldavian */
#define PDFX_LANG_MR    "mr"     /* Marathi */
#define PDFX_LANG_MS    "ms"     /* Malay */
#define PDFX_LANG_MT    "mt"     /* Maltese */
#define PDFX_LANG_MY    "my"     /* Burmese */
#define PDFX_LANG_NA    "na"     /* Nauru */
#define PDFX_LANG_NE    "ne"     /* Nepali */
#define PDFX_LANG_NL    "nl"     /* Dutch */
#define PDFX_LANG_NO    "no"     /* Norwegian */
#define PDFX_LANG_OC    "oc"     /* Occitan */
#define PDFX_LANG_OM    "om"     /* (Afan)Oromo */
#define PDFX_LANG_OR    "or"     /* Oriya */
#define PDFX_LANG_PA    "pa"     /* Punjabi */
#define PDFX_LANG_PL    "pl"     /* Polish */
#define PDFX_LANG_PS    "ps"     /* Pashto,Pushto */
#define PDFX_LANG_PT    "pt"     /* Portuguese  */
#define PDFX_LANG_QU    "qu"     /* Quechua */
#define PDFX_LANG_RM    "rm"     /* Rhaeto-Romance */
#define PDFX_LANG_RN    "rn"     /* Kirundi */
#define PDFX_LANG_RO    "ro"     /* Romanian */
#define PDFX_LANG_RU    "ru"     /* Russian */
#define PDFX_LANG_RW    "rw"     /* Kinyarwanda */
#define PDFX_LANG_SA    "sa"     /* Sanskrit */
#define PDFX_LANG_SD    "sd"     /* Sindhi */
#define PDFX_LANG_SG    "sg"     /* Sangro */
#define PDFX_LANG_SH    "sh"     /* Serbo-Croatian */
#define PDFX_LANG_SI    "si"     /* Singhalese */
#define PDFX_LANG_SK    "sk"     /* Slovak */
#define PDFX_LANG_SL    "sl"     /* Slovenian */
#define PDFX_LANG_SM    "sm"     /* Samoan */
#define PDFX_LANG_SN    "sn"     /* Shona */
#define PDFX_LANG_SO    "so"     /* Somali */
#define PDFX_LANG_SQ    "sq"     /* Albanian */
#define PDFX_LANG_SR    "sr"     /* Serbian */
#define PDFX_LANG_SS    "ss"     /* Siswati */
#define PDFX_LANG_ST    "st"     /* Sesotho */
#define PDFX_LANG_SU    "su"     /* Sundanese */
#define PDFX_LANG_SV    "sv"     /* Swedish */
#define PDFX_LANG_SW    "sw"     /* Swahili */
#define PDFX_LANG_TA    "ta"     /* Tamil */
#define PDFX_LANG_TE    "te"     /* Tegulu */
#define PDFX_LANG_TG    "tg"     /* Tajik */
#define PDFX_LANG_TH    "th"     /* Thai */
#define PDFX_LANG_TI    "ti"     /* Tigrinya */
#define PDFX_LANG_TK    "tk"     /* Turkmen */
#define PDFX_LANG_TL    "tl"     /* Tagalog */
#define PDFX_LANG_TN    "tn"     /* Setswanato Tonga */
#define PDFX_LANG_TR    "tr"     /* Turkish */
#define PDFX_LANG_TS    "ts"     /* Tsonga */
#define PDFX_LANG_TT    "tt"     /* Tatar */
#define PDFX_LANG_TW    "tw"     /* Twi */
#define PDFX_LANG_UK    "uk"     /* Ukrainian */
#define PDFX_LANG_UR    "ur"     /* Urdu */
#define PDFX_LANG_UZ    "uz"     /* Uzbek */
#define PDFX_LANG_VI    "vi"     /* Vietnamese */
#define PDFX_LANG_VO    "vo"     /* Volapuk */
#define PDFX_LANG_WO    "wo"     /* Wolof */
#define PDFX_LANG_XH    "xh"     /* Xhosa */
#define PDFX_LANG_YO    "yo"     /* Yoruba */
#define PDFX_LANG_ZH    "zh"     /* Chinese */
#define PDFX_LANG_ZU    "zu"     /* Zulu */


/*----------------------------------------------------------------------------*/
/*----- Graphics mode --------------------------------------------------------*/

#define   PDFX_GMODE_PAGE_DESCRIPTION       0x0001
#define   PDFX_GMODE_PATH_OBJECT            0x0002
#define   PDFX_GMODE_TEXT_OBJECT            0x0004
#define   PDFX_GMODE_CLIPPING_PATH          0x0008
#define   PDFX_GMODE_SHADING                0x0010
#define   PDFX_GMODE_INLINE_IMAGE           0x0020
#define   PDFX_GMODE_EXTERNAL_OBJECT        0x0040


/*----------------------------------------------------------------------------*/

#endif /* _PDFX_CONSTS_H */
