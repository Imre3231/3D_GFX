#ifndef BOOT_H
#define BOOT_H

/* ---------------------------------------------------------------------
   Boot console.

   Ten fixed label/value slots plus two free-running lines at the bottom:
   the newest USB host message and the last reported error.  Every
   USBH_dbg_*() call in USB_MSC_BARE.h is routed here through
   Boot_USB_Line(), so the enumeration trace lands in one tidy place
   instead of being scattered over the screen at fixed line numbers.

   Boot_Active() gates all drawing, so nothing can scribble over the
   browser or over video once the boot screen has been closed.
   --------------------------------------------------------------------- */

#define BOOT_BG        RGB565(0x0A,0x0C,0x10)
#define BOOT_BAR       RGB565(0x16,0x1A,0x22)
#define BOOT_RULE      RGB565(0x2A,0x31,0x3C)
#define BOOT_LABEL     RGB565(0x84,0x8C,0x99)
#define BOOT_VALUE     RGB565(0xFF,0xFF,0xFF)
#define BOOT_OK        RGB565(0x46,0xD0,0x6A)
#define BOOT_WARN      RGB565(0xF0,0xB4,0x30)
#define BOOT_FAIL      RGB565(0xE8,0x3A,0x30)
#define BOOT_INFO      RGB565(0x58,0xB8,0xF0)

#define BOOT_SLOTS     10u
#define BOOT_TOP       26u
#define BOOT_LINE_H    17u
#define BOOT_LABEL_X   8u
#define BOOT_VALUE_X   150u
#define BOOT_VALUE_W   (GFX_W-BOOT_VALUE_X-6u)
#define BOOT_DETAIL_Y  200u
#define BOOT_ERROR_Y   220u

unsigned char g_boot_active=0u;

unsigned char Boot_Active(void)
{
    return g_boot_active;
}

void Boot_Begin(const char *title)
{
    g_boot_active=1u;

    GFX_Fill_Rect(0u,0u,GFX_W,GFX_H,BOOT_BG);
    GFX_Fill_Rect(0u,0u,GFX_W,21u,BOOT_BAR);
    GFX_HLine(0u,21u,GFX_W,BOOT_RULE);
    GFX_Text(title,8u,3u,BOOT_BAR,BOOT_VALUE);
    GFX_Text_Right("v2.0",312u,3u,BOOT_BAR,BOOT_LABEL);
    GFX_HLine(0u,BOOT_DETAIL_Y-4u,GFX_W,BOOT_RULE);
}

void Boot_End(void)
{
    g_boot_active=0u;
}

void Boot_Value(unsigned char slot,const char *value,unsigned int color)
{
    char fit[40];
    unsigned int y;

    if((g_boot_active==0u)||(slot>=BOOT_SLOTS))
    {
        return;
    }

    y=BOOT_TOP+((unsigned int)slot*BOOT_LINE_H);
    GFX_Fit(value,fit,40u,BOOT_VALUE_W);
    GFX_Fill_Rect(BOOT_VALUE_X,y,GFX_W-BOOT_VALUE_X,GFX_FONT_H,BOOT_BG);
    GFX_Text(fit,BOOT_VALUE_X,y,BOOT_BG,color);
}

void Boot_Set(unsigned char slot,const char *label,const char *value,unsigned int color)
{
    unsigned int y;

    if((g_boot_active==0u)||(slot>=BOOT_SLOTS))
    {
        return;
    }

    y=BOOT_TOP+((unsigned int)slot*BOOT_LINE_H);
    GFX_Fill_Rect(0u,y,GFX_W,GFX_FONT_H,BOOT_BG);
    GFX_Text(label,BOOT_LABEL_X,y,BOOT_BG,BOOT_LABEL);
    Boot_Value(slot,value,color);
}

void Boot_Set_U32(unsigned char slot,const char *label,unsigned long value,const char *unit,unsigned int color)
{
    char txt[32];
    unsigned int n;

    n=GFX_U32_To_Str(value,txt);
    (void)GFX_Str_Copy(unit,&txt[n],31u-n);
    Boot_Set(slot,label,txt,color);
}

void Boot_Detail(const char *msg)
{
    char fit[40];

    if((g_boot_active==0u)||(msg==(const char *)0))
    {
        return;
    }

    GFX_Fit(msg,fit,40u,GFX_W-24u);
    GFX_Fill_Rect(0u,BOOT_DETAIL_Y,GFX_W,GFX_FONT_H,BOOT_BG);
    GFX_Text(">",6u,BOOT_DETAIL_Y,BOOT_BG,BOOT_RULE);
    GFX_Text(fit,20u,BOOT_DETAIL_Y,BOOT_BG,BOOT_INFO);
}

void Boot_Error(const char *msg)
{
    char fit[40];

    if(g_boot_active==0u)
    {
        return;
    }

    GFX_Fill_Rect(0u,BOOT_ERROR_Y,GFX_W,GFX_FONT_H,BOOT_BG);

    if(msg==(const char *)0)
    {
        return;
    }

    GFX_Fit(msg,fit,40u,GFX_W-24u);
    GFX_Text("!",6u,BOOT_ERROR_Y,BOOT_BG,BOOT_FAIL);
    GFX_Text(fit,20u,BOOT_ERROR_Y,BOOT_BG,BOOT_FAIL);
}

/* Called from USBH_dbg_line().  The original line number is dropped: the
   USB code numbers its lines for a full-screen dump, we only keep the
   newest message. */
void Boot_USB_Line(unsigned int line,const char *msg)
{
    (void)line;
    Boot_Detail(msg);
}

#endif
