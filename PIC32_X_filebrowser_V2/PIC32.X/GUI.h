#ifndef GUI_H
#define GUI_H

/* ---------------------------------------------------------------------
   File browser view.

   The old browser cleared all 76800 pixels and repainted every row on
   every key press (~125k pixel writes).  This one paints the chrome once
   and then repaints only what actually changed:

     move inside the page : 2 rows + footer   (~35k writes)
     page scrolled        : 6 rows + thumb    (~110k writes)
     directory changed    : everything

   GUI_Render() compares the live browser state against the shadow copy of
   what is currently on screen and picks the cheapest repaint.
   --------------------------------------------------------------------- */

#define UI_BG          RGB565(0x22,0x26,0x2E)
#define UI_HEADER      RGB565(0x15,0x18,0x1E)
#define UI_ROW_SEL     RGB565(0x33,0x3A,0x46)
#define UI_SEP         RGB565(0x2C,0x32,0x3C)
#define UI_TEXT        RGB565(0xFF,0xFF,0xFF)
#define UI_TEXT_DIM    RGB565(0x8A,0x92,0x9E)
#define UI_TEXT_FAINT  RGB565(0x60,0x68,0x74)
#define UI_ACCENT      RGB565(0xE8,0x0C,0x10)
#define UI_SB_TRACK    RGB565(0x16,0x19,0x20)
#define UI_SB_THUMB    RGB565(0x96,0x9E,0xAB)
#define UI_SB_ARROW    RGB565(0xB8,0xC0,0xCA)

#define GUI_ROWS       6u
#define GUI_ROW_H      33u
#define GUI_LIST_Y     24u
#define GUI_LIST_H     (GUI_ROWS*GUI_ROW_H)
#define GUI_HDR_H      22u
#define GUI_FOOT_Y     (GUI_LIST_Y+GUI_LIST_H)
#define GUI_FOOT_H     (GFX_H-GUI_FOOT_Y)

#define GUI_ICON_X     8u
#define GUI_TEXT_X     46u
#define GUI_CONTENT_W  301u
#define GUI_TEXT_W     (GUI_CONTENT_W-GUI_TEXT_X-4u)

#define GUI_SB_X       303u
#define GUI_SB_W       14u
#define GUI_SB_ARROW_H 12u

#define GUI_TYPE_SHUFFLE 0xF0u     /* the shuffle row's icon */

char gui_path[26];
unsigned int gui_shadow_top=0xFFFFu;
unsigned int gui_shadow_sel=0xFFFFu;
unsigned int gui_shadow_total=0xFFFFu;
unsigned char gui_chrome_valid=0u;
unsigned int browser_songs=0u;      /* songs in the open folder, by name */

/* ------------------------ browser model ----------------------------
   Rows: [ BACK ] in a subfolder, SHUFFLE PLAY in a folder of two songs or
   more, then the entries. */

/* A song for the music player: a module or a recording. */
unsigned char Browser_Is_Song(unsigned char type)
{
    return (((type>=VFS_TYPE_MOD)&&(type<=VFS_TYPE_IT))||
            ((type>=VFS_TYPE_MP3)&&(type<=VFS_TYPE_WAV))) ? 1u : 0u;
}

/* Counted from the types the names give.  Files whose names do not say
   are looked into first if there are 64 or fewer, so that a song without
   its extension counts from the start; a folder of more is not held up. */
#define BROWSER_SNIFF_MAX 64u

void Browser_Count_Songs(void)
{
    unsigned int e;
    unsigned int unknown;

    unknown=0u;
    for(e=1u;e<=Number_Of_Files_Indirectory;e++)
    {
        if(FS_Peek_Entry_Type(e)==VFS_TYPE_FILE)
        {
            unknown++;
        }
    }

    browser_songs=0u;
    for(e=1u;e<=Number_Of_Files_Indirectory;e++)
    {
        if(Browser_Is_Song((unknown<=BROWSER_SNIFF_MAX) ? FS_Get_Entry_Type(e) : FS_Peek_Entry_Type(e))!=0u)
        {
            browser_songs++;
        }
    }
}

unsigned char Browser_Has_Back_Row(void)
{
    return (Directory_Depth>0u) ? 1u : 0u;
}

unsigned char Browser_Has_Shuffle_Row(void)
{
    return (browser_songs>=2u) ? 1u : 0u;
}

/* Rows before the first entry. */
unsigned int Browser_Extra_Rows(void)
{
    return (unsigned int)Browser_Has_Back_Row()+(unsigned int)Browser_Has_Shuffle_Row();
}

unsigned int Browser_Total_Items(void)
{
    return Number_Of_Files_Indirectory+Browser_Extra_Rows();
}

unsigned char Browser_Is_Back_Row(unsigned int row)
{
    return ((Browser_Has_Back_Row()!=0u)&&(row==1u)) ? 1u : 0u;
}

unsigned char Browser_Is_Shuffle_Row(unsigned int row)
{
    return ((Browser_Has_Shuffle_Row()!=0u)&&(row==(1u+(unsigned int)Browser_Has_Back_Row()))) ? 1u : 0u;
}

/* The entry on a row; 0 on BACK and SHUFFLE PLAY. */
unsigned int Browser_Row_To_Entry(unsigned int row)
{
    return (row>Browser_Extra_Rows()) ? (row-Browser_Extra_Rows()) : 0u;
}

unsigned int Browser_Entry_To_Row(unsigned int entry)
{
    return entry+Browser_Extra_Rows();
}

void GUI_Set_Path(const char *name)
{
    char fit[26];

    GFX_Fit(name,fit,26u,200u);
    (void)GFX_Str_Copy(fit,gui_path,25u);
}

void GUI_Invalidate(void)
{
    gui_chrome_valid=0u;
    gui_shadow_top=0xFFFFu;
    gui_shadow_sel=0xFFFFu;
    gui_shadow_total=0xFFFFu;
}

void Browser_Open_Directory(unsigned long long new_address)
{
    Address=new_address;
    Number_Of_Files_Indirectory=FS_Open_DirectoryById(Address);
    Browser_Count_Songs();
    x=1;
    browser_top=1;
    GUI_Invalidate();
}

void Browser_Back(void)
{
    if(Directory_Depth>0u)
    {
        Directory_Depth--;
        Browser_Open_Directory(Directory_Stack[Directory_Depth]);

        if(Directory_Depth==0u)
        {
            GUI_Set_Path("ROOT");
        }
    }
}

void Browser_Clamp(void)
{
    unsigned int total;

    total=Browser_Total_Items();
    if(total==0u)
    {
        x=0u;
        browser_top=1u;
        return;
    }

    if(x<1u)
    {
        x=1u;
    }
    if(x>total)
    {
        x=total;
    }
    if(browser_top<1u)
    {
        browser_top=1u;
    }
    if(x<browser_top)
    {
        browser_top=x;
    }
    if(x>(browser_top+(GUI_ROWS-1u)))
    {
        browser_top=x-(GUI_ROWS-1u);
    }
}

/* ------------------------ icon blitting ---------------------------- */

/* Same pixel data as ICON.h but written with linear addressing through
   GFX_Put() instead of one Pixel() call per dot. */
void GUI_Icon_Raw(const unsigned char *src,unsigned int x0,unsigned int y0)
{
    unsigned long addr;
    unsigned int row;
    unsigned int col;
    unsigned int p;

    p=70u;
    addr=((unsigned long)y0*GFX_W)+(unsigned long)x0;

    for(row=0u;row<30u;row++)
    {
        for(col=0u;col<30u;col++)
        {
            GFX_Put(addr+(unsigned long)col,
                    (unsigned int)src[p]|((unsigned int)src[p+1u]<<8));
            p+=2u;
        }
        addr+=GFX_W;
    }

    GFX_Flush();
}

void GUI_Icon_Bmp24(const unsigned char *src,unsigned int x0,unsigned int y0)
{
    unsigned long addr;
    unsigned int row;
    unsigned int col;
    unsigned int off;
    unsigned int stride;
    unsigned int p;
    unsigned char b;
    unsigned char g;
    unsigned char r;

    off=(unsigned int)src[10]|((unsigned int)src[11]<<8);
    stride=(30u*3u+3u)&(~3u);
    addr=((unsigned long)y0*GFX_W)+(unsigned long)x0;

    for(row=0u;row<30u;row++)
    {
        p=off+((29u-row)*stride);

        for(col=0u;col<30u;col++)
        {
            b=src[p+(col*3u)+0u];
            g=src[p+(col*3u)+1u];
            r=src[p+(col*3u)+2u];
            GFX_Put(addr+(unsigned long)col,RGB565(r,g,b));
        }
        addr+=GFX_W;
    }

    GFX_Flush();
}

void GUI_Icon(unsigned char type,unsigned int x0,unsigned int y0)
{
    if(type==VFS_TYPE_DIR)
    {
        GUI_Icon_Raw(folder1,x0,y0);
    }
    else if(type==VFS_TYPE_JPEG)
    {
        GUI_Icon_Raw(jpeg,x0,y0);
    }
    else if(type==VFS_TYPE_BMP)
    {
        GUI_Icon_Raw(bmp,x0,y0);
    }
    else if(type==VFS_TYPE_AVI)
    {
        GUI_Icon_Bmp24(avi_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_MKV)
    {
        GUI_Icon_Bmp24(mkv_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_G3D)
    {
        GUI_Icon_Bmp24(g3d_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_MOD)
    {
        GUI_Icon_Bmp24(mod_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_S3M)
    {
        GUI_Icon_Bmp24(s3m_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_XM)
    {
        GUI_Icon_Bmp24(xm_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_IT)
    {
        GUI_Icon_Bmp24(it_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_MP3)
    {
        GUI_Icon_Bmp24(mp3_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_FLAC)
    {
        GUI_Icon_Bmp24(flac_icon_bmp,x0,y0);
    }
    else if(type==VFS_TYPE_WAV)
    {
        GUI_Icon_Bmp24(wav_icon_bmp,x0,y0);
    }
    else if(type==GUI_TYPE_SHUFFLE)
    {
        GUI_Icon_Bmp24(shuffle_icon_bmp,x0,y0);
    }
    else
    {
        GFX_Fill_Rect(x0,y0,30u,30u,UI_BG);
        GFX_Frame(x0+4u,y0+3u,22u,24u,UI_TEXT_FAINT);
    }
}

/* ------------------------ row painting ----------------------------- */

void GUI_Row_Name(unsigned int index,char *out,unsigned int out_size)
{
    char raw[VFS_NAME_LEN];
    unsigned int entry;

    if(Browser_Is_Back_Row(index)!=0u)
    {
        (void)GFX_Str_Copy("[ BACK ]",out,out_size-1u);
        return;
    }
    if(Browser_Is_Shuffle_Row(index)!=0u)
    {
        (void)GFX_Str_Copy("SHUFFLE PLAY",out,out_size-1u);
        return;
    }

    entry=Browser_Row_To_Entry(index);
    FS_ClearString(raw,VFS_NAME_LEN);
    FS_Get_Entry_Name(entry,raw);
    GFX_Fit(raw,out,out_size,GUI_TEXT_W);
}

unsigned char GUI_Row_Type(unsigned int index)
{
    unsigned int entry;

    if(Browser_Is_Back_Row(index)!=0u)
    {
        return VFS_TYPE_DIR;
    }
    if(Browser_Is_Shuffle_Row(index)!=0u)
    {
        return GUI_TYPE_SHUFFLE;
    }

    entry=Browser_Row_To_Entry(index);
    if(entry==0u)
    {
        return VFS_TYPE_FILE;
    }

    return FS_Get_Entry_Type(entry);
}

void GUI_Draw_Row(unsigned int slot,unsigned int index,unsigned char selected)
{
    char name[40];
    unsigned int row_y;
    unsigned int bg;
    unsigned int fg;
    unsigned int total;

    row_y=GUI_LIST_Y+(slot*GUI_ROW_H);
    total=Browser_Total_Items();

    if((index<1u)||(index>total))
    {
        GFX_Fill_Rect(0u,row_y,GUI_CONTENT_W,GUI_ROW_H,UI_BG);
        return;
    }

    bg=(selected!=0u) ? UI_ROW_SEL : UI_BG;
    fg=(selected!=0u) ? UI_TEXT : UI_TEXT_DIM;

    GFX_Fill_Rect(0u,row_y,GUI_CONTENT_W,GUI_ROW_H-1u,bg);

    if(selected!=0u)
    {
        GFX_Fill_Rect(1u,row_y+3u,4u,GUI_ROW_H-7u,UI_ACCENT);
    }

    GUI_Icon(GUI_Row_Type(index),GUI_ICON_X,row_y+1u);

    GUI_Row_Name(index,name,40u);
    GFX_Text(name,GUI_TEXT_X,row_y+8u,bg,fg);

    GFX_HLine(0u,row_y+GUI_ROW_H-1u,GUI_CONTENT_W,UI_SEP);
}

/* ------------------------ scroll bar -------------------------------- */

void GUI_Draw_Scrollbar(unsigned char full)
{
    unsigned int total;
    unsigned int track_y;
    unsigned int track_h;
    unsigned int thumb_h;
    unsigned int thumb_y;
    unsigned int span;

    track_y=GUI_LIST_Y+GUI_SB_ARROW_H;
    track_h=GUI_LIST_H-(2u*GUI_SB_ARROW_H);

    if(full!=0u)
    {
        GFX_Fill_Rect(GUI_SB_X-2u,GUI_LIST_Y,GFX_W-GUI_SB_X+2u,GUI_LIST_H,UI_BG);
        GFX_Fill_Rect(GUI_SB_X,GUI_LIST_Y,GUI_SB_W,GUI_LIST_H,UI_SB_TRACK);
        GFX_Tri_Up(GUI_SB_X+2u,GUI_LIST_Y+3u,9u,UI_SB_ARROW);
        GFX_Tri_Down(GUI_SB_X+2u,GUI_LIST_Y+GUI_LIST_H-8u,9u,UI_SB_ARROW);
    }
    else
    {
        GFX_Fill_Rect(GUI_SB_X,track_y,GUI_SB_W,track_h,UI_SB_TRACK);
    }

    total=Browser_Total_Items();
    if(total<=GUI_ROWS)
    {
        thumb_h=track_h;
        thumb_y=track_y;
    }
    else
    {
        thumb_h=(track_h*GUI_ROWS)/total;
        if(thumb_h<18u)
        {
            thumb_h=18u;
        }

        span=total-GUI_ROWS;
        thumb_y=track_y+(((track_h-thumb_h)*(browser_top-1u))/span);
    }

    /* Rounded ends: the first and last row are inset by one pixel. */
    GFX_Fill_Rect(GUI_SB_X+4u,thumb_y,6u,1u,UI_SB_THUMB);
    GFX_Fill_Rect(GUI_SB_X+3u,thumb_y+1u,8u,thumb_h-2u,UI_SB_THUMB);
    GFX_Fill_Rect(GUI_SB_X+4u,thumb_y+thumb_h-1u,6u,1u,UI_SB_THUMB);
}

/* ------------------------ chrome ------------------------------------ */

void GUI_Draw_Header(void)
{
    GFX_Fill_Rect(0u,0u,GFX_W,GUI_HDR_H,UI_HEADER);
    GFX_Text(gui_path,8u,3u,UI_HEADER,UI_TEXT);
    GFX_HLine(0u,GUI_HDR_H,GFX_W,UI_SEP);
    GFX_HLine(0u,GUI_HDR_H+1u,GFX_W,UI_BG);

    /* The up/down pair marks the list as scrollable. */
    GFX_Tri_Up(296u,4u,9u,UI_TEXT_DIM);
    GFX_Tri_Down(296u,12u,9u,UI_TEXT_DIM);
}

void GUI_Size_Text(unsigned long bytes,char *out)
{
    unsigned int n;

    if(bytes>=(1024ul*1024ul))
    {
        n=GFX_U32_To_Str(bytes/(1024ul*1024ul),out);
        (void)GFX_Str_Copy(" MB",&out[n],8u);
    }
    else if(bytes>=1024ul)
    {
        n=GFX_U32_To_Str(bytes/1024ul,out);
        (void)GFX_Str_Copy(" KB",&out[n],8u);
    }
    else
    {
        n=GFX_U32_To_Str(bytes,out);
        (void)GFX_Str_Copy(" B",&out[n],8u);
    }
}

void GUI_Draw_Footer(void)
{
    char txt[32];
    char num[16];             /* "PARENT FOLDER" and its NUL need 14 */
    unsigned int total;
    unsigned int n;
    unsigned char type;

    total=Browser_Total_Items();

    GFX_Fill_Rect(0u,GUI_FOOT_Y,GFX_W,GUI_FOOT_H,UI_HEADER);

    n=GFX_U32_To_Str((unsigned long)x,txt);
    txt[n]='/';
    n++;
    (void)GFX_U32_To_Str((unsigned long)total,&txt[n]);
    GFX_Text(txt,8u,GUI_FOOT_Y+1u,UI_HEADER,UI_TEXT_DIM);

    if((total==0u)||(x<1u))
    {
        return;
    }

    type=GUI_Row_Type(x);

    if(Browser_Is_Back_Row(x)!=0u)
    {
        (void)GFX_Str_Copy("PARENT FOLDER",num,15u);
        GFX_Text_Right(num,312u,GUI_FOOT_Y+1u,UI_HEADER,UI_TEXT_DIM);
        return;
    }

    if(type==GUI_TYPE_SHUFFLE)
    {
        n=GFX_U32_To_Str((unsigned long)browser_songs,num);
        (void)GFX_Str_Copy(" SONGS",&num[n],8u);
        GFX_Text_Right(num,312u,GUI_FOOT_Y+1u,UI_HEADER,UI_TEXT_DIM);
        return;
    }

    if(type==VFS_TYPE_DIR)
    {
        GFX_Text_Right("FOLDER",312u,GUI_FOOT_Y+1u,UI_HEADER,UI_TEXT_DIM);
        return;
    }

    GUI_Size_Text(FS_Get_Entry_Size(Browser_Row_To_Entry(x)),num);
    GFX_Text_Right(num,312u,GUI_FOOT_Y+1u,UI_HEADER,UI_TEXT_DIM);
}

void GUI_Draw_Empty(void)
{
    GFX_Fill_Rect(0u,GUI_LIST_Y,GUI_CONTENT_W,GUI_LIST_H,UI_BG);
    GFX_Text("EMPTY FOLDER",90u,110u,UI_BG,UI_TEXT_FAINT);
}

void GUI_Draw_All(void)
{
    unsigned int slot;
    unsigned int total;

    total=Browser_Total_Items();

    GUI_Draw_Header();

    if(total==0u)
    {
        GUI_Draw_Empty();
    }
    else
    {
        for(slot=0u;slot<GUI_ROWS;slot++)
        {
            GUI_Draw_Row(slot,browser_top+slot,(unsigned char)(((browser_top+slot)==x)?1u:0u));
        }
    }

    GUI_Draw_Scrollbar(1u);
    GUI_Draw_Footer();

    gui_chrome_valid=1u;
}

/* Box over the list, e.g. why a file cannot be played.  The caller waits
   for a key and calls GUI_Invalidate() afterwards. */
void GUI_Message(const char *title,const char *text)
{
    char fit[40];

    GFX_Fill_Rect(30u,78u,260u,80u,UI_HEADER);
    GFX_Frame(30u,78u,260u,80u,UI_SEP);
    GFX_Fill_Rect(30u,78u,260u,3u,UI_ACCENT);
    GFX_Fit(title,fit,40u,236u);
    GFX_Text(fit,42u,88u,UI_HEADER,UI_TEXT);
    GFX_Fit((text!=0) ? text : "",fit,40u,236u);
    GFX_Text(fit,42u,110u,UI_HEADER,UI_ACCENT);
    GFX_Text("PRESS ANY KEY",42u,132u,UI_HEADER,UI_TEXT_FAINT);
}

/* Repaints the minimum needed to match the current browser state. */
void GUI_Render(void)
{
    unsigned int slot;
    unsigned int total;

    total=Browser_Total_Items();

    if((gui_chrome_valid==0u)||(total!=gui_shadow_total))
    {
        GUI_Draw_All();
    }
    else if(browser_top!=gui_shadow_top)
    {
        for(slot=0u;slot<GUI_ROWS;slot++)
        {
            GUI_Draw_Row(slot,browser_top+slot,(unsigned char)(((browser_top+slot)==x)?1u:0u));
        }
        GUI_Draw_Scrollbar(0u);
        GUI_Draw_Footer();
    }
    else if(x!=gui_shadow_sel)
    {
        if((gui_shadow_sel>=browser_top)&&(gui_shadow_sel<(browser_top+GUI_ROWS)))
        {
            GUI_Draw_Row(gui_shadow_sel-browser_top,gui_shadow_sel,0u);
        }
        if((x>=browser_top)&&(x<(browser_top+GUI_ROWS)))
        {
            GUI_Draw_Row(x-browser_top,x,1u);
        }
        GUI_Draw_Footer();
    }
    else
    {
        return;
    }

    gui_shadow_top=browser_top;
    gui_shadow_sel=x;
    gui_shadow_total=total;
}

#endif
