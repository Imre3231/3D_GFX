#ifndef OSD_HEADER
#define OSD_HEADER

/* ---------------------------------------------------------------------
   Seek overlay: red progress bar with a round knob, elapsed time on the
   left, total running time on the right.

   MODE_VGA selects what the display scans out - the PIC32 frame buffer at
   0, the decoder's output at 1.  It is a source select, not a compositor,
   so the bar and live video cannot both be on screen.  Toggling it per
   paint is what made the bar flash red over green: green is the decoder
   showing an all-zero YUV frame, red was the bar during the brief moment
   the frame buffer was selected.

   So MODE_VGA is dropped once, for the whole seek, and put back on the way
   out.  The bar sits still and readable, and the decoder's picture returns
   when the key is released.

   OSD_MODE_SWITCH 0 leaves MODE_VGA alone, for a build whose display path
   can composite the frame buffer over video.
   --------------------------------------------------------------------- */
#define OSD_MODE_SWITCH        1

#define OSD_COLOUR_TEST 0

#define OSD_BG         Black
#define OSD_RED        Red
#define OSD_TEXT       Red
#define OSD_DIM        Red
#define OSD_TRACK      Dark_Grey

#define OSD_Y0         186u
#define OSD_H          (GFX_H-OSD_Y0)
#define OSD_TEXT_Y     191u
#define OSD_BAR_X      12u
#define OSD_BAR_W      296u
#define OSD_BAR_Y      217u
#define OSD_BAR_T      6u
#define OSD_KNOB_R     7u
#define OSD_BAND_Y     (OSD_BAR_Y-OSD_KNOB_R)
#define OSD_BAND_H     ((2u*OSD_KNOB_R)+OSD_BAR_T)

unsigned char g_osd_on=0u;
unsigned char g_osd_hours=0u;

/* Select the frame buffer for the duration of the seek. */
void OSD_Paint_Begin(void)
{
#if OSD_MODE_SWITCH
    GFX_Flush();
    MODE_VGA=0;
#endif
}

void OSD_Paint_End(void)
{
#if OSD_MODE_SWITCH
    GFX_Flush();
    MODE_VGA=1;
#endif
}

#if OSD_COLOUR_TEST
/* Four reference blocks so the frame-buffer field layout can be read off
   the screen.  The block under "R" is drawn with Red (0xF800). */
void OSD_Colour_Test(void)
{
    unsigned int w;

    w=GFX_W/4u;

    GFX_Fill_Rect(0u,       OSD_Y0,w,OSD_H,Red);
    GFX_Fill_Rect(w,        OSD_Y0,w,OSD_H,Green);
    GFX_Fill_Rect(w*2u,     OSD_Y0,w,OSD_H,Blue);
    GFX_Fill_Rect(w*3u,OSD_Y0,GFX_W-(w*3u),OSD_H,White);

    GFX_Text("R",(w/2u)-4u,       OSD_Y0+8u,Red,  Black);
    GFX_Text("G",w+(w/2u)-4u,     OSD_Y0+8u,Green,Black);
    GFX_Text("B",(w*2u)+(w/2u)-4u,OSD_Y0+8u,Blue, Black);
    GFX_Text("W",(w*3u)+(w/2u)-4u,OSD_Y0+8u,White,Black);
}
#endif

/* A short line centred just above the bar, for the preview status.  Pass 0
   to clear it. */
void OSD_Note(const char *s)
{
    unsigned int w;

    GFX_Fill_Rect(0u,OSD_Y0-20u,GFX_W,18u,Black);

    if(s==0) return;

    w=GFX_Text_Width(s);
    GFX_Text(s,(GFX_W>w)?((GFX_W-w)>>1):0u,OSD_Y0-18u,Black,OSD_DIM);
}

void OSD_Show(unsigned long dur_ms,unsigned char have_time)
{
    char txt[16];

    if(g_osd_on!=0u)
    {
        return;
    }

    g_osd_on=1u;

    /* Held down for the whole seek, not per paint. */
    OSD_Paint_Begin();

    /* The frame buffer still holds whatever was last drawn into it, and it
       is about to become the visible surface, so clear it first. */
    GFX_Fill_Rect(0u,0u,GFX_W,GFX_H,Black);

    g_osd_hours=(unsigned char)((dur_ms>=3600000ul) ? 1u : 0u);

    GFX_Fill_Rect(0u,OSD_Y0,GFX_W,OSD_H,OSD_BG);

    if(have_time!=0u)
    {
        MKV_Fmt_Time(dur_ms,g_osd_hours,txt);
    }
    else
    {
        (void)GFX_Str_Copy("--:--",txt,15u);
    }

    GFX_Text_Right(txt,308u,OSD_TEXT_Y,OSD_BG,OSD_DIM);

#if OSD_COLOUR_TEST
    OSD_Colour_Test();
#endif
}

void OSD_Hide(void)
{
    if(g_osd_on==0u)
    {
        return;
    }

    g_osd_on=0u;

    /* Nothing is erased here: OSD_Show clears the whole frame buffer on the
       way in, and writing pixels right after a stream push would put bytes
       on SPI1 that the FPGA has just been fed video on. */
    OSD_Paint_End();
}

/* frac_num/frac_den is the playback position as a fraction of the file.
   pos_ms is only used for the elapsed-time text. */
void OSD_Update(unsigned long pos_ms,
                unsigned long frac_num,
                unsigned long frac_den,
                unsigned long step_ms,
                signed char dir,
                unsigned char have_time)
{
    char txt[20];
    unsigned long long v;
    unsigned int fill;
    unsigned int knob_x;

    if(g_osd_on==0u)
    {
        return;
    }

    if(frac_den==0ul)
    {
        fill=0u;
    }
    else
    {
        if(frac_num>frac_den)
        {
            frac_num=frac_den;
        }
        v=(unsigned long long)OSD_BAR_W*(unsigned long long)frac_num;
        v/=(unsigned long long)frac_den;
        fill=(unsigned int)v;
    }

    /* Repaint the whole bar band so the knob leaves no trail. */
    GFX_Fill_Rect(0u,OSD_BAND_Y,GFX_W,OSD_BAND_H,OSD_BG);

    if(fill<OSD_BAR_W)
    {
        GFX_Fill_Rect(OSD_BAR_X+fill,OSD_BAR_Y,OSD_BAR_W-fill,OSD_BAR_T,OSD_TRACK);
    }
    if(fill>0u)
    {
        GFX_Fill_Rect(OSD_BAR_X,OSD_BAR_Y,fill,OSD_BAR_T,OSD_RED);
    }

    knob_x=OSD_BAR_X+fill;
    GFX_Disc(knob_x,OSD_BAR_Y+(OSD_BAR_T/2u),OSD_KNOB_R,OSD_RED);

    /* Elapsed time, left. */
    GFX_Fill_Rect(0u,OSD_TEXT_Y,120u,GFX_FONT_H,OSD_BG);
    if(have_time!=0u)
    {
        MKV_Fmt_Time(pos_ms,g_osd_hours,txt);
    }
    else
    {
        (void)GFX_Str_Copy("--:--",txt,19u);
    }
    GFX_Text(txt,OSD_BAR_X,OSD_TEXT_Y,OSD_BG,OSD_TEXT);

    /* Step size and direction, centred. */
    GFX_Fill_Rect(120u,OSD_TEXT_Y,100u,GFX_FONT_H,OSD_BG);
    if(step_ms>0ul)
    {
        MKV_Fmt_Step(step_ms,dir,txt);
        GFX_Text(txt,132u,OSD_TEXT_Y,OSD_BG,OSD_DIM);
    }
}

#endif
