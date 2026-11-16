#define DREQ  PORTEbits.RE5
#define MEMWR PORTAbits.RA0
#define MODE_VGA  PORTGbits.RG15

#define Button1 PORTAbits.RA2  /* SELECT up   / seek back    */
#define Button2 PORTAbits.RA3  /* SELECT down / seek forward */
#define Button3 PORTAbits.RA4  /* ENTER       / stop         */

#define White2       0xFFFFFF
#define Black2       0x000000
#define White        0xFFFF
#define Black        0x0000
#define Grey         0xF7DE
#define Grey2        0x8430
#define Light_Grey   0xC618
#define Dark_Grey    0x8410
#define Purple       0xF81F
#define Grey_Blue    0x5458
#define Blue         0x001F
#define Dark_Blue    0x01CF
#define Light_Blue   0x051F
#define Light_Blue_2 0x7D7C
#define Red          0xF800
#define Green        0x07E0
#define Cyan         0x7FFF
#define Yellow       0xFFE0
#define Orange       0xFC08
#define Magenta      0xF81F

unsigned char play;
unsigned long File_Size;
unsigned long long Address;
unsigned int Number_Of_Files_Indirectory;
unsigned int x;
unsigned int print;
unsigned char data2;

unsigned long long Directory_Stack[16];
unsigned char Directory_Depth=0;
unsigned int browser_top=1;

#include <xc.h>
#include "DELAY.h"
#include "CONFIG.h"
#include "SPI.h"

/* FONT.h calls Pixel() before FUNCTIONS.h defines it. */
void Pixel(unsigned int x, unsigned int y, unsigned int data);

#include "FONT.h"
#include "ICON.h"
#include "FUNCTIONS.h"
#include "GFX.h"
#include "BOOT.h"

#include "USB_MSC_BARE.h"
#include "FS_GENERIC.h"
#include "JPEG.h"
#define STB_IMAGE_IMPLEMENTATION

#include "STREAM.h"
#include "MKV_SEEK.h"
#include "H264_PIC.h"
#include "OSD.h"
#include "GUI.h"
#include "MUSIC.h"
#include "AUDIO.h"

/* Core timer runs at SYSCLK/2. */
#define CPU_HZ            192000000ul
#define TICKS_PER_MS      (CPU_HZ/2000ul)

unsigned long Ticks_Now(void)
{
    return (unsigned long)_CP0_GET_COUNT();
}

/* Unsigned difference, so the 44 s wrap of the core timer is harmless. */
unsigned long Ticks_Elapsed_Ms(unsigned long start)
{
    return ((unsigned long)(Ticks_Now()-start))/TICKS_PER_MS;
}

void Write(unsigned char data)
{
    while(DREQ==0);

    WriteSPI3(data);
    MEMWR=1;
    MEMWR=0;
}

void fillScreen(unsigned int color)
{
    GFX_Fill_Rect(0u,0u,GFX_W,GFX_H,color);
}

void Draw_Frame(unsigned int x0,unsigned int y0,unsigned int width,unsigned int height,unsigned int color)
{
    GFX_Frame(x0,y0,width,height,color);
}

/* Waits for every key to come up, with a 2.5 s escape hatch in case one
   reads as stuck. */
void Wait_Button_Release(void)
{
    unsigned int tmo;

    tmo=0u;
    while((Button1==1)||(Button2==1)||(Button3==1))
    {
        Delay_ms(10);
        tmo++;
        if(tmo>250u)
        {
            break;
        }
    }
}

/* ---------------------------------------------------------------------
   BMP viewer
   --------------------------------------------------------------------- */

unsigned char BMP_Read_Byte(unsigned long pos,unsigned char *value)
{
    return FS_Read_CurrentFile_Byte(pos,value);
}

unsigned int BMP_Read_LE16(unsigned long pos)
{
    unsigned char b0;
    unsigned char b1;

    if(BMP_Read_Byte(pos+0ul,&b0)==0u) return 0u;
    if(BMP_Read_Byte(pos+1ul,&b1)==0u) return 0u;

    return (unsigned int)b0|((unsigned int)b1<<8);
}

unsigned long BMP_Read_LE32(unsigned long pos)
{
    unsigned char b0;
    unsigned char b1;
    unsigned char b2;
    unsigned char b3;

    if(BMP_Read_Byte(pos+0ul,&b0)==0u) return 0ul;
    if(BMP_Read_Byte(pos+1ul,&b1)==0u) return 0ul;
    if(BMP_Read_Byte(pos+2ul,&b2)==0u) return 0ul;
    if(BMP_Read_Byte(pos+3ul,&b3)==0u) return 0ul;

    return (unsigned long)b0|((unsigned long)b1<<8)|
           ((unsigned long)b2<<16)|((unsigned long)b3<<24);
}

unsigned char BMP_Mask_To_8(unsigned long value,unsigned long mask)
{
    unsigned long max_value;

    if(mask==0ul)
    {
        return 0u;
    }

    value&=mask;
    while((mask & 1ul)==0ul)
    {
        mask>>=1;
        value>>=1;
    }

    max_value=mask;
    if(max_value==0ul)
    {
        return 0u;
    }

    return (unsigned char)((value*255ul+(max_value/2ul))/max_value);
}

unsigned int BMP_Pack_RGB565(unsigned char r,unsigned char g,unsigned char b)
{
    return RGB565(r,g,b);
}

unsigned char BMP_Display_CurrentFile(void)
{
    unsigned long file_size;
    unsigned long off_bits;
    unsigned long dib_size;
    long src_w_signed;
    long src_h_signed;
    unsigned long src_w;
    unsigned long src_h;
    unsigned int planes;
    unsigned int bpp;
    unsigned long compression;
    unsigned long clr_used;
    unsigned long stride;
    unsigned long palette_colors;
    unsigned int palette565[256];
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    unsigned char top_down;
    unsigned long target_w;
    unsigned long target_h;
    unsigned int off_x;
    unsigned int off_y;
    unsigned long dy;
    unsigned long dx;
    unsigned long src_x;
    unsigned long src_y;
    unsigned long src_row;
    unsigned long row_pos;
    unsigned long pixel_pos;
    unsigned long raw;
    unsigned char b0;
    unsigned char b1;
    unsigned char pr;
    unsigned char pg;
    unsigned char pb;
    unsigned int color;
    unsigned long i;

    file_size=FS_Get_CurrentFile_Size();
    if(file_size<54ul)
    {
        return 0u;
    }

    if((BMP_Read_Byte(0ul,&b0)==0u)||(BMP_Read_Byte(1ul,&b1)==0u))
    {
        return 0u;
    }
    if((b0!='B')||(b1!='M'))
    {
        return 0u;
    }

    off_bits=BMP_Read_LE32(10ul);
    dib_size=BMP_Read_LE32(14ul);
    src_w_signed=(long)BMP_Read_LE32(18ul);
    src_h_signed=(long)BMP_Read_LE32(22ul);
    planes=BMP_Read_LE16(26ul);
    bpp=BMP_Read_LE16(28ul);
    compression=BMP_Read_LE32(30ul);
    clr_used=BMP_Read_LE32(46ul);

    if((planes!=1u)||(src_w_signed<=0l)||(src_h_signed==0l))
    {
        return 0u;
    }

    top_down=0u;
    if(src_h_signed<0l)
    {
        top_down=1u;
        src_h=(unsigned long)(0l-src_h_signed);
    }
    else
    {
        src_h=(unsigned long)src_h_signed;
    }
    src_w=(unsigned long)src_w_signed;

    if((bpp!=8u)&&(bpp!=16u)&&(bpp!=24u)&&(bpp!=32u))
    {
        return 0u;
    }

    if((compression!=0ul)&&(compression!=3ul))
    {
        return 0u;
    }

    stride=((src_w*(unsigned long)bpp+31ul)/32ul)*4ul;

    if(((bpp==16u)||(bpp==32u))&&(compression==3ul)&&(off_bits>=66ul))
    {
        red_mask=BMP_Read_LE32(54ul);
        green_mask=BMP_Read_LE32(58ul);
        blue_mask=BMP_Read_LE32(62ul);
    }
    else if(bpp==16u)
    {
        red_mask=0xF800ul;
        green_mask=0x07E0ul;
        blue_mask=0x001Ful;
    }
    else if(bpp==32u)
    {
        red_mask=0x00FF0000ul;
        green_mask=0x0000FF00ul;
        blue_mask=0x000000FFul;
    }
    else
    {
        red_mask=0ul;
        green_mask=0ul;
        blue_mask=0ul;
    }

    if(bpp==8u)
    {
        palette_colors=(clr_used==0ul) ? 256ul : clr_used;
        if(palette_colors>256ul)
        {
            palette_colors=256ul;
        }

        for(i=0ul;i<palette_colors;i++)
        {
            if(BMP_Read_Byte(54ul+i*4ul+0ul,&pb)==0u) return 0u;
            if(BMP_Read_Byte(54ul+i*4ul+1ul,&pg)==0u) return 0u;
            if(BMP_Read_Byte(54ul+i*4ul+2ul,&pr)==0u) return 0u;
            palette565[i]=BMP_Pack_RGB565(pr,pg,pb);
        }
        for(;i<256ul;i++)
        {
            palette565[i]=0u;
        }
    }

    target_w=src_w;
    target_h=src_h;
    if((target_w>320ul)||(target_h>240ul))
    {
        if((320ul*target_h)<=(240ul*target_w))
        {
            target_w=320ul;
            target_h=(src_h*320ul)/src_w;
            if(target_h==0ul) target_h=1ul;
        }
        else
        {
            target_h=240ul;
            target_w=(src_w*240ul)/src_h;
            if(target_w==0ul) target_w=1ul;
        }
    }

    off_x=(unsigned int)((320ul-target_w)/2ul);
    off_y=(unsigned int)((240ul-target_h)/2ul);

    for(dy=0ul;dy<target_h;dy++)
    {
        src_y=(dy*src_h)/target_h;
        src_row=(top_down==0u) ? ((src_h-1ul)-src_y) : src_y;
        row_pos=off_bits+(src_row*stride);

        for(dx=0ul;dx<target_w;dx++)
        {
            src_x=(dx*src_w)/target_w;

            if(bpp==24u)
            {
                pixel_pos=row_pos+src_x*3ul;
                if(BMP_Read_Byte(pixel_pos+0ul,&pb)==0u) return 0u;
                if(BMP_Read_Byte(pixel_pos+1ul,&pg)==0u) return 0u;
                if(BMP_Read_Byte(pixel_pos+2ul,&pr)==0u) return 0u;
                color=BMP_Pack_RGB565(pr,pg,pb);
            }
            else if(bpp==32u)
            {
                pixel_pos=row_pos+src_x*4ul;
                raw=BMP_Read_LE32(pixel_pos);
                pr=BMP_Mask_To_8(raw,red_mask);
                pg=BMP_Mask_To_8(raw,green_mask);
                pb=BMP_Mask_To_8(raw,blue_mask);
                color=BMP_Pack_RGB565(pr,pg,pb);
            }
            else if(bpp==16u)
            {
                pixel_pos=row_pos+src_x*2ul;
                raw=(unsigned long)BMP_Read_LE16(pixel_pos);
                pr=BMP_Mask_To_8(raw,red_mask);
                pg=BMP_Mask_To_8(raw,green_mask);
                pb=BMP_Mask_To_8(raw,blue_mask);
                color=BMP_Pack_RGB565(pr,pg,pb);
            }
            else
            {
                pixel_pos=row_pos+src_x;
                if(BMP_Read_Byte(pixel_pos,&b0)==0u) return 0u;
                color=palette565[b0];
            }

            GFX_Put((unsigned long)(off_y+(unsigned int)dy)*GFX_W+
                    (unsigned long)(off_x+(unsigned int)dx),color);
        }
    }

    GFX_Flush();
    (void)dib_size;
    return 1u;
}

/* ---------------------------------------------------------------------
   Image browsing
   --------------------------------------------------------------------- */

unsigned char Browser_Row_Is_Image(unsigned int row)
{
    unsigned char type;

    type=GUI_Row_Type(row);
    if((type==VFS_TYPE_JPEG)||(type==VFS_TYPE_BMP))
    {
        return 1u;
    }

    return 0u;
}

unsigned int Browser_Find_Image_Row(unsigned int current_row,int direction)
{
    int row;
    int total;

    total=(int)Browser_Total_Items();
    row=(int)current_row;

    while(1)
    {
        row+=direction;
        if((row<1)||(row>total))
        {
            break;
        }

        if(Browser_Row_Is_Image((unsigned int)row)!=0u)
        {
            return (unsigned int)row;
        }
    }

    return current_row;
}

unsigned char Browser_Display_Image_Row(unsigned int row)
{
    unsigned int entry;
    unsigned char type;
    unsigned char ok;

    entry=Browser_Row_To_Entry(row);
    if(entry==0u)
    {
        return 0u;
    }

    type=FS_Get_Entry_Type(entry);
    if((type!=VFS_TYPE_JPEG)&&(type!=VFS_TYPE_BMP))
    {
        return 0u;
    }

    if(FS_Open_File_By_Index(entry)==0u)
    {
        return 0u;
    }

    fillScreen(Black);

    ok=(type==VFS_TYPE_JPEG) ? JPEG_Display_CurrentFile() : BMP_Display_CurrentFile();

    if(ok==0u)
    {
        fillScreen(Black);
        GFX_Text("IMAGE ERROR",100u,110u,Black,White);
    }

    return 1u;
}

void Browser_View_Image(unsigned int start_row)
{
    unsigned int next_row;

    x=start_row;
    if(Browser_Display_Image_Row(x)==0u)
    {
        return;
    }

    while(1)
    {
        USBH_Service();

        if(Button3==1)
        {
            Delay_ms(20);
            if(Button3==1)
            {
                Wait_Button_Release();
                GUI_Invalidate();
                return;
            }
        }

        if(Button1==1)
        {
            Delay_ms(20);
            if(Button1==1)
            {
                next_row=Browser_Find_Image_Row(x,-1);
                if(next_row!=x)
                {
                    x=next_row;
                    (void)Browser_Display_Image_Row(x);
                }
                Wait_Button_Release();
            }
        }

        if(Button2==1)
        {
            Delay_ms(20);
            if(Button2==1)
            {
                next_row=Browser_Find_Image_Row(x,1);
                if(next_row!=x)
                {
                    x=next_row;
                    (void)Browser_Display_Image_Row(x);
                }
                Wait_Button_Release();
            }
        }
    }
}

/* ---------------------------------------------------------------------
   AVI / MPEG-2 playback (unchanged trick play)
   --------------------------------------------------------------------- */

unsigned char AVI_Chunk_Is_IFrame(unsigned long chunk_data_pos,unsigned long chunk_size)
{
    unsigned long scan_limit;
    unsigned long i;
    unsigned char b0;
    unsigned char b1;
    unsigned char b2;
    unsigned char b3;
    unsigned char b4;
    unsigned char b5;

    if(chunk_data_pos>=File_Size)
    {
        return 0u;
    }

    scan_limit=chunk_size;
    if(scan_limit>(File_Size-chunk_data_pos))
    {
        scan_limit=File_Size-chunk_data_pos;
    }
    if(scan_limit>4096ul)
    {
        scan_limit=4096ul;
    }
    if(scan_limit<6ul)
    {
        return 0u;
    }

    for(i=0ul;(i+5ul)<scan_limit;i++)
    {
        if(AVI_Read_Byte(chunk_data_pos+i+0ul,&b0)==0u) return 0u;
        if(AVI_Read_Byte(chunk_data_pos+i+1ul,&b1)==0u) return 0u;
        if(AVI_Read_Byte(chunk_data_pos+i+2ul,&b2)==0u) return 0u;
        if(AVI_Read_Byte(chunk_data_pos+i+3ul,&b3)==0u) return 0u;

        if((b0==0x00u)&&(b1==0x00u)&&(b2==0x01u)&&(b3==0x00u))
        {
            if(AVI_Read_Byte(chunk_data_pos+i+4ul,&b4)==0u) return 0u;
            if(AVI_Read_Byte(chunk_data_pos+i+5ul,&b5)==0u) return 0u;

            (void)b4;
            return (((b5>>3)&0x07u)==1u) ? 1u : 0u;
        }
    }

    return 0u;
}

unsigned char AVI_Find_Next_00dc(unsigned long start_pos,
                                 unsigned long scan_limit,
                                 unsigned char require_iframe,
                                 unsigned long *chunk_pos,
                                 unsigned long *chunk_size,
                                 unsigned long *chunk_end)
{
    unsigned long pos;
    unsigned long end_pos;
    unsigned long size;
    unsigned long end_chunk;
    unsigned char b0;
    unsigned char b1;
    unsigned char b2;
    unsigned char b3;

    if(File_Size<8ul) return 0u;
    if(start_pos>(File_Size-8ul)) return 0u;

    end_pos=start_pos+scan_limit;
    if(end_pos>File_Size) end_pos=File_Size;
    if(end_pos<8ul) return 0u;

    pos=start_pos;
    while(pos<=(end_pos-8ul))
    {
        if(AVI_Read_Byte(pos+0ul,&b0)==0u) return 0u;
        if(AVI_Read_Byte(pos+1ul,&b1)==0u) return 0u;
        if(AVI_Read_Byte(pos+2ul,&b2)==0u) return 0u;
        if(AVI_Read_Byte(pos+3ul,&b3)==0u) return 0u;

        if((b0=='0')&&(b1=='0')&&(b2=='d')&&(b3=='c'))
        {
            if(AVI_Read_LE32(pos+4ul,&size)==0u)
            {
                return 0u;
            }

            if((size!=0ul)&&(size<(File_Size-pos-8ul)))
            {
                end_chunk=pos+8ul+size+(size & 1ul);
                if(end_chunk<=File_Size)
                {
                    if((require_iframe==0u)||AVI_Chunk_Is_IFrame(pos+8ul,size))
                    {
                        *chunk_pos=pos;
                        *chunk_size=size;
                        *chunk_end=end_chunk;
                        return 1u;
                    }

                    if(end_chunk>pos)
                    {
                        pos=end_chunk;
                        continue;
                    }
                }
            }
        }

        pos++;
    }

    return 0u;
}

unsigned char AVI_Get_Trick_Speed(unsigned long repeat_count)
{
    if(repeat_count<2ul) return 1u;
    if(repeat_count<6ul) return 2u;
    return 3u;
}

unsigned long AVI_Get_Skip_Bytes(unsigned char speed)
{
    if(speed==1u) return (256ul*1024ul);
    if(speed==2u) return (1024ul*1024ul);
    return (4ul*1024ul*1024ul);
}

unsigned int AVI_Get_Repeat_Delay(unsigned char speed)
{
    if(speed==1u) return 180u;
    if(speed==2u) return 90u;
    return 30u;
}

void Play_MPEG2(void)
{
    unsigned long current_pos;
    unsigned long target_pos;
    unsigned long chunk_pos;
    unsigned long chunk_size;
    unsigned long chunk_end;
    unsigned long repeat_count;
    unsigned long skip_bytes;
    unsigned long stream_count;
    unsigned char trick_dir;
    unsigned char b1;
    unsigned char b2;
    unsigned char speed;
    const unsigned long SEARCH_IFRAME_WINDOW=(4ul*1024ul*1024ul);
    const unsigned long SEARCH_ANY_00DC=(512ul*1024ul);

    if(File_Size==0ul)
    {
        return;
    }

    current_pos=0ul;
    repeat_count=0ul;
    trick_dir=0u;
    AVI_SeekCache_Reset();
    Wait_Button_Release();

    while(current_pos<File_Size)
    {
        b1=(Button1==1) ? 1u : 0u;
        b2=(Button2==1) ? 1u : 0u;

        if(Button3==1)
        {
            Delay_ms(15);
            if(Button3==1)
            {
                break;
            }
        }

        if((b1!=0u)||(b2!=0u))
        {
            if(b1!=0u)
            {
                if(trick_dir!=2u)
                {
                    repeat_count=0ul;
                    trick_dir=2u;
                }
            }
            else
            {
                if(trick_dir!=1u)
                {
                    repeat_count=0ul;
                    trick_dir=1u;
                }
            }

            speed=AVI_Get_Trick_Speed(repeat_count);
            skip_bytes=AVI_Get_Skip_Bytes(speed);

            if(b1!=0u)
            {
                target_pos=(current_pos>skip_bytes) ? (current_pos-skip_bytes) : 0ul;
            }
            else
            {
                target_pos=current_pos+skip_bytes;
                if(target_pos>=File_Size)
                {
                    target_pos=File_Size-1ul;
                }
            }

            AVI_SeekCache_Reset();
            if(AVI_Find_Next_00dc(target_pos,SEARCH_IFRAME_WINDOW,1u,&chunk_pos,&chunk_size,&chunk_end)==0u)
            {
                AVI_SeekCache_Reset();
                if(AVI_Find_Next_00dc(target_pos,SEARCH_ANY_00DC,0u,&chunk_pos,&chunk_size,&chunk_end)==0u)
                {
                    current_pos=target_pos;
                    repeat_count++;
                    Delay_ms(AVI_Get_Repeat_Delay(speed));
                    continue;
                }
            }

            AVI_SeekCache_Reset();
            if(AVI_Stream_Range(chunk_pos,chunk_end-chunk_pos,&current_pos)==0u)
            {
                break;
            }

            repeat_count++;
            Delay_ms(AVI_Get_Repeat_Delay(speed));
            continue;
        }

        repeat_count=0ul;
        trick_dir=0u;

        stream_count=AVI_STREAM_BUF_SIZE;
        if(stream_count>(File_Size-current_pos))
        {
            stream_count=File_Size-current_pos;
        }

        if(AVI_Stream_Range(current_pos,stream_count,&current_pos)==0u)
        {
            break;
        }
    }

    Wait_Button_Release();
}

/* ---------------------------------------------------------------------
   MKV playback with seek
   --------------------------------------------------------------------- */

/* Pushes the cluster the seek landed on so the decoder has an I-frame to
   show.  The seek keys are still held at this point, so their abort is
   masked off for the duration of the push. */
/* Sends one keyframe to the decoder as a synthetic single-frame cluster.
   new_pos is where normal playback should resume, which is the real
   cluster - the preview does not advance the file position. */
/* ===================== software I-frame preview ======================

   MODE_VGA is a source select, so while the overlay is on screen the
   decoder's picture is not.  Decoding the keyframe here instead puts the
   preview into the same frame buffer the bar lives in, which composites
   for free and needs no RTL change.

   Set H264_PREVIEW 0 to drop the whole thing - it costs about 120 KB of
   RAM and ~40 ms a step. */
#define H264_PREVIEW 1

/* Shows how long each part of a seek step took, in milliseconds, on the
   line above the bar: S=search D=decode B=blit.  Set to 0 once the timing
   is understood. */
#define H264_PREVIEW_TIME 0

#if H264_PREVIEW

/* Largest keyframe accepted.  Real 320x240 material sits well under this;
   a frame bigger than the buffer just skips its preview, the bar carries
   on as normal. */
#define H264_RBSP_MAX (64u*1024u)

static H264_SPS      h264_sps;
static H264_PPS      h264_pps;
static H264_DEC      h264_dec;
static H264_PIC_BUF  h264_pic;
static unsigned char h264_rbsp[H264_RBSP_MAX];
static unsigned short h264_row[H264_MAX_W/2u];
static unsigned char h264_ready=0u;

/* Why no preview appeared.  Shown on the overlay so a failure names itself
   instead of just being a blank area. */
#define PRV_OK        0u
#define PRV_NO_CODEC  1u   /* no CodecPrivate in the track                */
#define PRV_BAD_AVCC  2u   /* CodecPrivate is not an avcC record          */
#define PRV_SPS       3u   /* SPS rejected - g_h264_reason says why       */
#define PRV_PPS       4u   /* PPS rejected - g_h264_reason says why       */
#define PRV_SIZE      5u   /* picture larger than H264_MAX_W/H            */
#define PRV_NO_BLOCK  6u   /* no keyframe block in the cluster            */
#define PRV_NO_IDR    7u   /* no IDR NAL inside that block                */
#define PRV_TOO_BIG   8u   /* frame larger than H264_RBSP_MAX             */
#define PRV_DECODE    9u   /* decoder rejected the slice                  */
#define PRV_NOT_OPEN 10u   /* H264_Preview_Open never succeeded           */
#define PRV_READ     11u   /* a read off the end of the block             */
#define PRV_BAD_NAL  12u   /* NAL length overruns the block               */

unsigned char g_prv_fail=PRV_NO_CODEC;
unsigned long g_prv_read_ms=0ul;
unsigned long g_prv_decode_ms=0ul;
unsigned long g_prv_blit_ms=0ul;
unsigned long g_prv_search_ms=0ul;

/* Unescapes a NAL straight from the file.  Returns 0 if it would not fit,
   which simply means that frame gets no preview. */
/* Pulls the NAL in with one multi-sector read and strips the emulation
   bytes in place.  Reading it in small chunks meant a separate USB transfer
   every few hundred bytes - about a hundred of them for a 25 KB keyframe. */
unsigned long H264_Unescape_File(unsigned long pos,unsigned long len,
                                 unsigned char *dst,unsigned long cap)
{
    unsigned long o;
    unsigned long i;
    unsigned long done;
    unsigned int  chunk;
    unsigned char zeros;

    if(len>cap) return 0ul;

    done=0ul;
    g_fs_multi_read_ok=1u;

    while(done<len)
    {
        chunk=((len-done)>32768ul) ? 32768u : (unsigned int)(len-done);
        if(AVI_Read_Block(pos+done,&dst[done],chunk)==0u)
        {
            g_fs_multi_read_ok=0u;
            return 0ul;
        }
        done+=(unsigned long)chunk;
    }

    g_fs_multi_read_ok=0u;

    /* Compact in place: unescaping only ever removes bytes, so the write
       index never overtakes the read index. */
    o=0ul;
    zeros=0u;

    for(i=0ul;i<len;i++)
    {
        if((zeros>=2u)&&(dst[i]==0x03u))
        {
            zeros=0u;
            continue;
        }

        dst[o++]=dst[i];
        zeros=(dst[i]==0u) ? (unsigned char)(zeros+1u) : 0u;
    }

    return o;
}

/* Splits the avcC record from CodecPrivate into its SPS and PPS. */
unsigned char H264_Preview_Open(void)
{
    unsigned int p;
    unsigned int l;
    unsigned long r;
    unsigned char n;
    unsigned char i;

    h264_ready=0u;
    h264_sps.valid=0u;
    h264_pps.valid=0u;

    g_prv_fail=PRV_NO_CODEC;
    if(g_mkv_codec_private_len<7u) return 0u;

    g_prv_fail=PRV_BAD_AVCC;
    if(g_mkv_codec_private[0]!=1u)  return 0u;

    n=(unsigned char)(g_mkv_codec_private[5]&0x1Fu);
    p=6u;

    for(i=0u;i<n;i++)
    {
        if((p+2u)>g_mkv_codec_private_len) return 0u;
        l=((unsigned int)g_mkv_codec_private[p]<<8)|g_mkv_codec_private[p+1u];
        p+=2u;
        if(((unsigned long)p+l)>g_mkv_codec_private_len) return 0u;

        if(l>1u)
        {
            r=H264_Unescape(&g_mkv_codec_private[p+1u],(unsigned long)(l-1u),
                            h264_rbsp,sizeof h264_rbsp);
            (void)H264_Parse_SPS(h264_rbsp,r,&h264_sps);
        }
        p+=l;
    }

    /* Stop here if the SPS was rejected, so its reason code survives -
       parsing the PPS would overwrite it. */
    if(h264_sps.valid==0u) { g_prv_fail=PRV_SPS; return 0u; }

    if(p>=g_mkv_codec_private_len) return 0u;
    n=g_mkv_codec_private[p];
    p++;

    for(i=0u;i<n;i++)
    {
        if((p+2u)>g_mkv_codec_private_len) return 0u;
        l=((unsigned int)g_mkv_codec_private[p]<<8)|g_mkv_codec_private[p+1u];
        p+=2u;
        if(((unsigned long)p+l)>g_mkv_codec_private_len) return 0u;

        if(l>1u)
        {
            r=H264_Unescape(&g_mkv_codec_private[p+1u],(unsigned long)(l-1u),
                            h264_rbsp,sizeof h264_rbsp);
            (void)H264_Parse_PPS(h264_rbsp,r,&h264_pps);
        }
        p+=l;
    }

    if(h264_pps.valid==0u) { g_prv_fail=PRV_PPS; return 0u; }

    if((h264_sps.mb_width*16u>H264_MAX_W)||(h264_sps.mb_height*16u>H264_MAX_H))
    {
        g_prv_fail=PRV_SIZE;
        return 0u;
    }

    g_prv_fail=PRV_OK;
    h264_ready=1u;
    return 1u;
}

/* Decodes the keyframe in this cluster and draws it centred, halved. */
unsigned char H264_Preview_Draw(unsigned long cluster_pos,unsigned long cluster_end)
{
    unsigned long blk_pos;
    unsigned long blk_len;
    unsigned long np;
    unsigned long nl;
    unsigned long end;
    unsigned char hdr_len;
    unsigned char lb[4];
    unsigned char i;
    unsigned int  ox,oy,ow,oh,y;
#if H264_PREVIEW_TIME
    unsigned long t0;

    t0=Ticks_Now();
#endif

    if(h264_ready==0u) { g_prv_fail=PRV_NOT_OPEN; return 0u; }

    AVI_SeekCache_Reset();
    if(MKV_Find_Key_Block(cluster_pos,cluster_end,&blk_pos,&blk_len,&hdr_len)==0u)
    {
        g_prv_fail=PRV_NO_BLOCK;
        return 0u;
    }

    /* Walk the length-prefixed NAL units for the IDR. */
    np=blk_pos+(unsigned long)hdr_len;
    end=blk_pos+blk_len;

    while((np+(unsigned long)g_mkv_nal_len_size)<=end)
    {
        nl=0ul;
        for(i=0u;i<g_mkv_nal_len_size;i++)
        {
            if(AVI_Read_Byte(np+(unsigned long)i,&lb[0])==0u)
            {
                g_prv_fail=PRV_READ;
                return 0u;
            }
            nl=(nl<<8)|(unsigned long)lb[0];
        }
        np+=(unsigned long)g_mkv_nal_len_size;

        if((nl==0ul)||((np+nl)>end)) { g_prv_fail=PRV_BAD_NAL; return 0u; }

        if(AVI_Read_Byte(np,&lb[0])==0u) { g_prv_fail=PRV_READ; return 0u; }

        if((lb[0]&0x1Fu)==5u)
        {
            unsigned long rl;

            /* Strip the emulation bytes straight out of the file, so only
               the unescaped copy is ever held in RAM. */
            rl=H264_Unescape_File(np+1ul,nl-1ul,h264_rbsp,(unsigned long)H264_RBSP_MAX);
            if(rl==0ul) { g_prv_fail=PRV_TOO_BIG; return 0u; }

#if H264_PREVIEW_TIME
            g_prv_read_ms=Ticks_Elapsed_Ms(t0);
            t0=Ticks_Now();
#endif

            if(H264_Decode_Keyframe_RBSP(&h264_dec,&h264_pic,h264_rbsp,rl,
                                         &h264_sps,&h264_pps)==0u)
            {
                g_prv_fail=PRV_DECODE;
                return 0u;
            }

#if H264_PREVIEW_TIME
            g_prv_decode_ms=Ticks_Elapsed_Ms(t0);
            t0=Ticks_Now();
#endif

            g_prv_fail=PRV_OK;

            ow=h264_pic.w>>1;
            oh=h264_pic.h>>1;
            ox=(GFX_W>ow) ? ((GFX_W-ow)>>1) : 0u;
            oy=(OSD_Y0>oh) ? ((OSD_Y0-oh)>>1) : 0u;

            for(y=0u;y<oh;y++)
            {
                H264_Row_RGB565(&h264_pic,y,h264_row);
                GFX_Blit_Row(ox,oy+y,ow,h264_row);
            }

#if H264_PREVIEW_TIME
            g_prv_blit_ms=Ticks_Elapsed_Ms(t0);
#endif

            return 1u;
        }

        np+=nl;
    }

    g_prv_fail=PRV_NO_IDR;
    return 0u;
}

#if H264_PREVIEW_TIME
/* "S123 D456 B78" - milliseconds in search, decode and blit. */
const char *H264_Preview_Timing(void)
{
    static char line[32];
    unsigned int n;

    n=0u;
    line[n++]='S';
    n+=GFX_U32_To_Str(g_prv_search_ms,&line[n]);
    line[n++]=' ';
    line[n++]='D';
    n+=GFX_U32_To_Str(g_prv_decode_ms,&line[n]);
    line[n++]=' ';
    line[n++]='R';
    n+=GFX_U32_To_Str(g_prv_read_ms,&line[n]);
    line[n++]=' ';
    line[n++]='B';
    n+=GFX_U32_To_Str(g_prv_blit_ms,&line[n]);
    line[n]=0;

    return line;
}
#endif

/* One short line naming the stage that failed, drawn above the bar. */
const char *H264_Preview_Status(void)
{
    switch(g_prv_fail)
    {
    case PRV_OK:       return 0;
    case PRV_NO_CODEC: return "no codec header in file";
    case PRV_BAD_AVCC: return "codec header not avcC";
    case PRV_SIZE:     return "picture too large";
    case PRV_NO_BLOCK: return "no keyframe in cluster";
    case PRV_NO_IDR:   return "no IDR in keyframe block";
    case PRV_TOO_BIG:  return "frame larger than buffer";
    case PRV_DECODE:   return "decoder rejected slice";
    case PRV_NOT_OPEN: return "codec header not usable";
    case PRV_READ:     return "read failed";
    case PRV_BAD_NAL:  return "malformed NAL length";
    default: break;
    }

    switch(g_h264_reason)
    {
    case H264_SPS_SCALING:   return "scaling matrices in SPS";
    case H264_SPS_LOSSLESS:  return "lossless stream";
    case H264_SPS_INTERLACE: return "interlaced stream";
    case H264_SPS_TOOWIDE:   return "picture too wide";
    case H264_PPS_SLICEGRP:  return "slice groups in PPS";
    case H264_PPS_SCALING:   return "scaling matrices in PPS";
    case H264_PPS_CAVLC:     return "CAVLC, not CABAC";
    default: break;
    }

    return (g_prv_fail==PRV_SPS) ? "SPS rejected" : "PPS rejected";
}

#endif

unsigned char MKV_Push_Preview(unsigned long cluster_pos,
                               unsigned long cluster_end,
                               unsigned long *new_pos)
{
    unsigned char hdr[MKV_PREVIEW_HDR_MAX];
    unsigned long blk_pos;
    unsigned long blk_len;
    unsigned long dummy;
    unsigned int hdr_bytes;
    unsigned int sent;
    unsigned char hdr_len;
    unsigned char track_was;
    unsigned char ok;

    *new_pos=cluster_pos;

    AVI_SeekCache_Reset();
    if(MKV_Find_Key_Block(cluster_pos,cluster_end,&blk_pos,&blk_len,&hdr_len)==0u)
    {
        return 0u;
    }

    if(MKV_Build_Preview_Header(blk_pos,blk_len,hdr_len,hdr,&hdr_bytes)==0u)
    {
        return 0u;
    }

    /* Tracking off: the NAL payload would otherwise be scanned for cluster
       headers and a false match would corrupt the reported position. */
    track_was=g_mkv_track_enable;
    g_mkv_track_enable=0u;

    g_stream_seek_keys=0u;
    AVI_SPI_DMA_Init();

    ok=AVI_Send_Buffer(hdr,hdr_bytes,&sent);

    if(ok!=0u)
    {
        /* The NAL data follows the header bytes already sent. */
        AVI_SeekCache_Reset();
        ok=AVI_Stream_Range(blk_pos+(unsigned long)hdr_len,
                            blk_len-(unsigned long)hdr_len,
                            &dummy);
    }

    g_stream_seek_keys=1u;
    g_mkv_track_enable=track_was;

    return ok;
}

/* Runs for as long as a seek key is held.  Returns the byte position to
   resume from, or 0xFFFFFFFF if STOP was pressed. */
/* Runs while a seek key is held.

   Nothing is streamed to the decoder during the seek.  MODE_VGA selects the
   display source rather than compositing, so while the bar is on screen the
   decoder's picture is not - a preview frame pushed here could not be seen,
   and pushing one would only cost a frame period per step.  The chosen
   keyframe cluster is handed back instead, and Play_MKV resumes from it: the
   first picture after the key is released is that I-frame, and playback
   carries on from there.

   MKV_SEEK_PREVIEW 1 restores the per-step preview push, for a display path
   that can composite the overlay over video.

   Returns the byte position to resume from, or 0xFFFFFFFF if STOP was
   pressed. */
#define MKV_SEEK_PREVIEW 0

unsigned long MKV_Seek_Session(unsigned long current_pos,unsigned char have_time)
{
    unsigned long press_ticks;
#if (MKV_SEEK_REPEAT_MS > 0)
    unsigned long step_ticks;
#endif
    unsigned long hold_ms;
    unsigned long step_ms;
    unsigned long cur_ms;
    unsigned long target_ms;
    unsigned long cluster_pos;
    unsigned long cluster_end;
    unsigned long cluster_data;
    unsigned long landed_ms;
    unsigned long new_pos;
    unsigned long step_bytes;
    unsigned long target_pos;
    unsigned long frac_num;
    unsigned long frac_den;
    unsigned long have_cluster;
    unsigned long flush_to;
    unsigned long resume_pos;
    unsigned long release_ticks;
    unsigned long low_ticks;
#if H264_PREVIEW_TIME
    unsigned long search_ticks;
#endif
    unsigned long fine_ms;
    unsigned long cand_pos;
    unsigned long cand_end;
    unsigned long cand_ms;
    unsigned long cur_pos;
    unsigned long cur_end;
    unsigned char held;
    unsigned char low_seen;
    unsigned char walk;
    unsigned char level;
    signed char dir;

    cur_ms=g_mkv_play_ms;
    if((have_time!=0u)&&(cur_ms==0ul))
    {
        cur_ms=MKV_Byte_To_Time(current_pos);
    }

    /* Bar up first, so the key press gets an immediate response. */
    OSD_Show(g_mkv_duration_ms,have_time);
    frac_den=(have_time!=0u) ? g_mkv_duration_ms : File_Size;
    frac_num=(have_time!=0u) ? cur_ms : current_pos;
    OSD_Update(cur_ms,frac_num,frac_den,0ul,1,have_time);

    press_ticks=Ticks_Now();
    resume_pos=current_pos;

    /* The abort left the reader partway through a block.  Send the rest of
       the cluster so it comes back to segment level, otherwise the cluster
       we resume with is swallowed as block payload.

       No flush padding here: the reader is mid-block and counting payload
       bytes, so padding would be taken as part of that block and shift
       everything after it.  The real bytes below are what it is waiting
       for. */
    if(MKV_Cluster_Flush_Target(current_pos,&flush_to)!=0u)
    {
        g_stream_seek_keys=0u;
        if(AVI_Stream_Range(current_pos,flush_to-current_pos,&new_pos)!=0u)
        {
            resume_pos=new_pos;
        }
        g_stream_seek_keys=1u;

        if(Button3==1)
        {
            OSD_Hide();
            return 0xFFFFFFFFul;
        }

        /* The flush left the reader on a cluster boundary, and we know
           where it is - so the hop machinery has somewhere to start from
           on the very first step, instead of that one step having to go
           through the expensive search. */
        if(MKV_Check_Cluster(flush_to,0u,&cluster_data,&cluster_end,&landed_ms)!=0u)
        {
            cur_pos=flush_to;
            cur_end=cluster_end;
            if(have_time!=0u) cur_ms=landed_ms;
            have_cluster=1ul;
            MKV_Cache_Add(cur_pos,cur_end,landed_ms);
        }
    }

    step_ms=0ul;
    dir=1;
    cluster_end=0ul;
    cluster_pos=0ul;
    release_ticks=Ticks_Now();
    low_ticks=release_ticks;
    fine_ms=MKV_SEEK_STEP_MIN_MS;
    cand_pos=0ul; cand_end=0ul; cand_ms=0ul;
    cur_pos=0ul;  cur_end=0ul;
    have_cluster=0ul;
    held=1u;
    low_seen=0u;

    for(;;)
    {
        if(Button3==1)
        {
            OSD_Hide();
            return 0xFFFFFFFFul;
        }

        if((Button1==0)&&(Button2==0))
        {
            /* Debounce first: a contact bounce partway through a long hold
               must not read as a release, or the step ramp restarts. */
            if(low_seen==0u)
            {
                low_seen=1u;
                low_ticks=Ticks_Now();
            }

            if((held!=0u)&&(Ticks_Elapsed_Ms(low_ticks)>=MKV_SEEK_DEBOUNCE_MS))
            {
                /* Key up: hold the overlay for a moment so tapping
                   repeatedly stays in one session instead of tearing the
                   screen down and rebuilding it between presses. */
                held=0u;
                release_ticks=Ticks_Now();
                OSD_Update(cur_ms,frac_num,frac_den,0ul,dir,have_time);
            }

            if((held==0u)&&(Ticks_Elapsed_Ms(release_ticks)>=MKV_SEEK_LINGER_MS))
            {
                break;
            }
            continue;
        }

        low_seen=0u;

        if(held==0u)
        {
            /* New press within the linger window: the ramp starts again
               from the smallest step rather than carrying on from where
               the previous press had got to. */
            held=1u;
            press_ticks=Ticks_Now();
        }

        dir=(Button1==1) ? -1 : 1;
        hold_ms=Ticks_Elapsed_Ms(press_ticks);
        level=MKV_Seek_Level(hold_ms);
        step_ms=MKV_Seek_Step_Ms(hold_ms);

        if(have_time!=0u)
        {
            fine_ms=(g_mkv_key_gap_ms>0ul) ? g_mkv_key_gap_ms
                                            : MKV_SEEK_STEP_MIN_MS;

            if(dir<0)
            {
                /* Backwards there are no back-pointers, so a fine step is a
                   normal search one keyframe earlier - the margin has to
                   cover a whole keyframe gap or it lands where it started. */
                target_ms=(cur_ms>step_ms) ? (cur_ms-step_ms) : 0ul;
            }
            else
            {
                target_ms=cur_ms+step_ms;
                if(target_ms>g_mkv_duration_ms)
                {
                    target_ms=g_mkv_duration_ms;
                }
            }

            MKV_Set_Seek_Margin((dir<0)?(step_ms+fine_ms):step_ms);
            walk=0u;
#if H264_PREVIEW_TIME
            search_ticks=Ticks_Now();
#endif

            /* Hopping cluster to cluster is cheap at any step size - one
               short read per cluster - so it is tried first whichever way
               and however far we are going.  The interpolating search is
               the fallback only: it reads forward from a byte estimate
               that lands mid-cluster, so with megabyte clusters it can
               chew through several of them.  Leaving it on the normal path
               is why holding the key was slow while tapping was quick: the
               ramp grew the step past the hop threshold and dropped into
               the search on every repeat. */
            if(have_cluster!=0ul)
            {
                if(dir<0)
                {
                    if(MKV_Key_Cluster_At_Or_Before(target_ms,&cand_pos,
                                                    &cand_end,&cand_ms)==0u)
                    {
                        /* Budget ran out: settle for the keyframe just
                           behind, which is still a move in the right
                           direction and still only hops. */
                        if((cur_ms>0ul)&&
                           (MKV_Key_Cluster_At_Or_Before(cur_ms-1ul,&cand_pos,
                                                         &cand_end,&cand_ms)!=0u))
                        {
                            walk=2u;
                        }
                    }
                    else
                    {
                        walk=2u;
                    }
                }
                else
                {
                    walk=MKV_Walk_Forward(cur_end,target_ms,&cand_pos,
                                          &cand_end,&cand_ms);

                    /* A step smaller than the keyframe spacing puts the
                       target between two keyframes, so there is none at or
                       before it and the walk comes back empty.  The next
                       keyframe is then the smallest move that is actually
                       forward - and it is two or three short reads away,
                       where the search would grind through megabytes and
                       land back where it started. */
                    if((walk==0u)||(cand_ms<=cur_ms))
                    {
                        if(MKV_Next_Key_Cluster(cur_end,&cand_pos,
                                                &cand_end,&cand_ms)!=0u)
                        {
                            walk=2u;
                        }
                        else
                        {
                            walk=0u;
                        }
                    }
                    else
                    {
                        walk=2u;
                    }
                }
            }

            if(walk!=2u)
            {
                if(MKV_Seek_To_Ms(target_ms,&cand_pos,&cand_end,&cand_ms)!=0u)
                {
                    walk=2u;
                }
            }

            /* The search minimises the distance to the target; it has no
               notion of which way we asked to go.  On variable bitrate
               material its best match for "one keyframe on" can be the
               keyframe we are already sitting on, or an earlier one, and
               the position then walks backwards and forwards instead of
               stepping.  So a candidate that did not move the requested way
               is replaced by an exact hop, which is directional by
               construction. */
            if((walk==2u)&&(have_cluster!=0ul))
            {
                if((dir>0)&&(cand_ms<=cur_ms))
                {
                    if(MKV_Next_Key_Cluster(cur_end,&cand_pos,
                                            &cand_end,&cand_ms)==0u)
                    {
                        walk=0u;              /* already at the last one */
                    }
                }
                else if((dir<0)&&(cand_ms>=cur_ms))
                {
                    if((cur_ms==0ul)||
                       (MKV_Key_Cluster_At_Or_Before(cur_ms-1ul,&cand_pos,
                                                     &cand_end,&cand_ms)==0u))
                    {
                        walk=0u;              /* already at the first one */
                    }
                }
            }

#if H264_PREVIEW_TIME
            g_prv_search_ms=Ticks_Elapsed_Ms(search_ticks);
#endif

            if(walk==2u)
            {
                if((have_cluster!=0ul)&&(cand_ms!=cur_ms))
                {
                    /* Two successive keyframes give the spacing, which sets
                       how far a single tap moves. */
                    g_mkv_key_gap_ms=(cand_ms>cur_ms) ? (cand_ms-cur_ms)
                                                      : (cur_ms-cand_ms);
                }

                cur_pos=cand_pos;
                cur_end=cand_end;
                cur_ms=cand_ms;

                have_cluster=1ul;
                g_mkv_play_ms=cur_ms;
                resume_pos=cur_pos;
                MKV_Cache_Add(cur_pos,cur_end,cur_ms);

                cluster_pos=cur_pos;
                cluster_end=cur_end;
                landed_ms=cur_ms;

#if H264_PREVIEW
                /* Decoded here on the PIC32 and drawn into the same frame
                   buffer as the bar, because MODE_VGA cannot show both the
                   overlay and the FPGA's output at once. */
                if(H264_Preview_Draw(cur_pos,cur_end)==0u)
                {
                    OSD_Note(H264_Preview_Status());
                }
#if H264_PREVIEW_TIME
                else
                {
                    OSD_Note(H264_Preview_Timing());
                }
#endif
#endif

#if MKV_SEEK_PREVIEW
                (void)MKV_Push_Preview(cur_pos,cur_end,&new_pos);
                if(g_stream_abort_reason==STREAM_ABORT_STOP)
                {
                    OSD_Hide();
                    return 0xFFFFFFFFul;
                }
#endif
            }

            frac_num=cur_ms;
            frac_den=g_mkv_duration_ms;
        }
        else
        {
            /* No timeline: step through the file by percentage instead. */
            step_bytes=(File_Size/100ul)*MKV_Level_Pct(level);
            if(step_bytes<(64ul*1024ul))
            {
                step_bytes=(64ul*1024ul);
            }

            if(dir<0)
            {
                target_pos=(resume_pos>step_bytes) ? (resume_pos-step_bytes) : 0ul;
            }
            else
            {
                target_pos=resume_pos+step_bytes;
                if(target_pos>=File_Size)
                {
                    target_pos=(File_Size>1ul) ? (File_Size-1ul) : 0ul;
                }
            }

            AVI_SeekCache_Reset();
            if(MKV_Scan_Cluster(target_pos,MKV_KEY_WINDOW,1u,
                                &cluster_pos,&cluster_data,&cluster_end,&landed_ms)!=0u)
            {
                resume_pos=cluster_pos;
                have_cluster=1ul;
            }
            else
            {
                AVI_SeekCache_Reset();
                if(MKV_Scan_Cluster(target_pos,MKV_ANY_WINDOW,0u,
                                    &cluster_pos,&cluster_data,&cluster_end,&landed_ms)!=0u)
                {
                    resume_pos=cluster_pos;
                    have_cluster=1ul;
                }
            }

#if H264_PREVIEW
            if(have_cluster!=0ul)
            {
                if(H264_Preview_Draw(cluster_pos,cluster_end)==0u)
                {
                    OSD_Note(H264_Preview_Status());
                }
            }
#endif

            frac_num=resume_pos;
            frac_den=File_Size;
        }

        OSD_Update(cur_ms,frac_num,frac_den,step_ms,dir,have_time);

        if(Button3==1)
        {
            OSD_Hide();
            return 0xFFFFFFFFul;
        }

#if (MKV_SEEK_REPEAT_MS > 0)
        step_ticks=Ticks_Now();
        while(Ticks_Elapsed_Ms(step_ticks)<MKV_SEEK_REPEAT_MS)
        {
            if((Button1==0)&&(Button2==0)) break;
            if(Button3==1)
            {
                OSD_Hide();
                return 0xFFFFFFFFul;
            }
        }
#endif
    }

    /* Make sure the resume point really is a keyframe cluster.  The search
       falls back to any cluster when no key cluster is in range, and
       resuming on one of those feeds the decoder P-frames with nothing to
       reference. */
    if((have_cluster!=0ul)&&(cluster_end>cluster_pos))
    {
        if(MKV_Ensure_Key_Cluster(cluster_pos,cluster_end,
                                  &cluster_pos,&cluster_end,&landed_ms)!=0u)
        {
            resume_pos=cluster_pos;
            if(have_time!=0u)
            {
                cur_ms=landed_ms;
                g_mkv_play_ms=landed_ms;
            }
        }

        /* Wipe the overlay and the preview before the display is handed
           back, so nothing of the seek screen survives behind the video. */
        GFX_Fill_Rect(0u,0u,GFX_W,GFX_H,Black);

        /* Push padding through first, so whatever the aborted burst left
           in the decoder's input FIFO is gone before the keyframe goes in
           - otherwise the stale tail runs into the front of the new
           picture. */
        (void)AVI_Flush_Decoder(MKV_FLUSH_BYTES);

        /* Decode that keyframe while the frame buffer is still the surface
           on screen, so switching back to video lands on the new picture
           rather than flashing the stale one the decoder still holds. */
        (void)MKV_Push_Preview(cluster_pos,cluster_end,&new_pos);
        Delay_ms((int)MKV_SEEK_RESUME_MS);

        if(g_stream_abort_reason==STREAM_ABORT_STOP)
        {
            OSD_Hide();
            return 0xFFFFFFFFul;
        }
    }

    /* Decoder output back on screen; Play_MKV streams the real cluster from
       here, so playback continues from that same keyframe. */
    OSD_Hide();
    return resume_pos;
}

void Play_MKV(void)
{
    unsigned long current_pos;
    unsigned long stream_count;
    unsigned char have_time;

    if(File_Size==0ul)
    {
        return;
    }

    AVI_SeekCache_Reset();
    have_time=MKV_Open_Timeline();

#if H264_PREVIEW
    /* SPS and PPS come from the track's CodecPrivate, not the stream. */
    (void)H264_Preview_Open();
#endif

    g_mkv_track_enable=1u;
    g_stream_seek_keys=1u;
    current_pos=0ul;

    Wait_Button_Release();

    while(current_pos<File_Size)
    {
        if(Button3==1)
        {
            Delay_ms(15);
            if(Button3==1)
            {
                break;
            }
        }

        if((Button1==1)||(Button2==1))
        {
            current_pos=MKV_Seek_Session(current_pos,have_time);
            if(current_pos==0xFFFFFFFFul)
            {
                break;
            }
            continue;
        }

        stream_count=AVI_STREAM_BUF_SIZE;
        if(stream_count>(File_Size-current_pos))
        {
            stream_count=File_Size-current_pos;
        }

        if(AVI_Stream_Range(current_pos,stream_count,&current_pos)==0u)
        {
            if(g_stream_abort_reason==STREAM_ABORT_SEEK)
            {
                continue;
            }
            break;
        }
    }

    g_mkv_track_enable=0u;
    g_stream_seek_keys=0u;
    OSD_Hide();
    Wait_Button_Release();
}

/* ---------------------------------------------------------------------
   G3D scene: the file goes to the FPGA's scene loader as it is.  Loading
   ends at the end of the file, or when DREQ stays low (the FPGA holds the
   stream); the 3D view then stays up until Enter.  Enter while loading
   cancels straight back to the browser.
   --------------------------------------------------------------------- */

void Play_G3D(void)
{
    unsigned long current_pos;
    unsigned long stream_count;
    unsigned char cancelled;

    if(File_Size==0ul)
    {
        return;
    }

    AVI_SeekCache_Reset();
    g_mkv_track_enable=0u;
    g_stream_seek_keys=0u;
    current_pos=0ul;
    cancelled=0u;

    Wait_Button_Release();

    while(current_pos<File_Size)
    {
        stream_count=AVI_STREAM_BUF_SIZE;
        if(stream_count>(File_Size-current_pos))
        {
            stream_count=File_Size-current_pos;
        }

        if(AVI_Stream_Range(current_pos,stream_count,&current_pos)==0u)
        {
            cancelled=(g_stream_abort_reason==STREAM_ABORT_STOP) ? 1u : 0u;
            break;
        }
    }

    if(cancelled==0u)
    {
        while(1)                                    /* 3D running: wait for Enter */
        {
            if(Button3==1)
            {
                Delay_ms(20);
                if(Button3==1)
                {
                    break;
                }
            }
        }
    }

    Wait_Button_Release();
}

/* ---------------------------------------------------------------------
   Music player.  A module (MOD, S3M, XM, IT) is turned by MUSIC.h into the
   FPGA player's SDRAM image and sent whole, then plays; an MP3, FLAC or WAV
   file is sent by AUDIO.h as it plays.  The open folder is the playlist,
   in its order or shuffled:

     song ends    next song; after the last one, the first again
     DOWN / UP    next / previous song, also while one is loading
     Enter        back to the browser, cursor on the last song

   The FPGA reports the end of a song by holding DREQ low for 50 ms.  While
   a module plays nothing is sent, and DREQ is otherwise never low for more
   than a few microseconds; a recording has been sent whole by then, the
   FPGA keeping DREQ high from its last chunk on.  The song picked in the
   browser is refused there if it cannot be played; one the playlist comes
   to is skipped.
   --------------------------------------------------------------------- */

#define PLAYER_END_LOW_MS  20ul     /* DREQ low this long: the song ended   */
#define PLAYER_RELEASE_MS  30ul     /* all keys up this long between presses */

#define PLAYER_NONE        0u
#define PLAYER_STOP        1u       /* Enter         */
#define PLAYER_PREV        2u       /* UP            */
#define PLAYER_NEXT        3u       /* DOWN          */
#define PLAYER_END         4u       /* song finished */

static unsigned char player_held=0u;      /* last press not yet released    */
static unsigned char player_up=0u;        /* all keys seen up ...           */
static unsigned long player_up_ticks=0ul; /* ... since this time            */
static unsigned char player_abort=0u;     /* key that stopped the last load */

/* Shuffled: the folder's entries in the order of a keyed bijection of
   0 .. 4^half-1, stepped a place at a time; places past the folder's end
   and entries that are not songs are passed over.  A lap plays every song
   once, UP goes back along it, and the next lap has a new order. */
static unsigned char player_shuffle=0u;
static unsigned int  shuffle_half=1u;     /* bits of each half of a place */
static unsigned long shuffle_pos=0ul;
static unsigned long shuffle_key[4];

unsigned char Player_Format(unsigned char type)
{
    switch(type)
    {
    case VFS_TYPE_MOD: return MUS_FMT_MOD;
    case VFS_TYPE_S3M: return MUS_FMT_S3M;
    case VFS_TYPE_XM:  return MUS_FMT_XM;
    default:           return MUS_FMT_IT;
    }
}

unsigned char Player_Is_Audio(unsigned char type)
{
    return ((type>=VFS_TYPE_MP3)&&(type<=VFS_TYPE_WAV)) ? 1u : 0u;
}

/* A song, by its name or, if that does not say, by its first bytes. */
unsigned char Player_Is_Song(unsigned int entry)
{
    return Browser_Is_Song(FS_Get_Entry_Type(entry));
}

/* Four Feistel rounds: each half in turn mixed into the other through a
   multiplicative hash of it and the round's key (the product's top bits). */
unsigned long Shuffle_Map(unsigned long i)
{
    unsigned long mask;
    unsigned long l;
    unsigned long r;
    unsigned long t;
    unsigned int k;

    mask=(1ul<<shuffle_half)-1ul;
    l=(i>>shuffle_half) & mask;
    r=i & mask;
    for(k=0u;k<4u;k++)
    {
        t=l^((((r+shuffle_key[k])*0x9E3779B1ul)>>(32u-shuffle_half)) & mask);
        l=r;
        r=t;
    }
    return (l<<shuffle_half)|r;
}

/* A new order, keyed by the time of the key press or the lap's end. */
void Shuffle_Rekey(void)
{
    unsigned long t;
    unsigned int k;

    t=Ticks_Now();
    for(k=0u;k<4u;k++)
    {
        t=(t^(t>>15))*0x85EBCA6Bul+shuffle_key[k];
        shuffle_key[k]=t;
    }
}

void Shuffle_Begin(void)
{
    shuffle_half=1u;
    while((1ul<<(2u*shuffle_half))<(unsigned long)Number_Of_Files_Indirectory)
    {
        shuffle_half++;
    }
    shuffle_pos=(1ul<<(2u*shuffle_half))-1ul;     /* the first step goes to 0 */
}

/* The next song after (step 1) or before (step -1) entry, round the
   folder or along the shuffled order; entry itself when it is the only
   one. */
unsigned int Player_Step(unsigned int entry,signed char step)
{
    unsigned long m;
    unsigned long i;
    unsigned int n;
    unsigned int e;

    n=Number_Of_Files_Indirectory;

    if(player_shuffle!=0u)
    {
        m=(1ul<<(2u*shuffle_half))-1ul;
        for(i=0ul;i<=m;i++)
        {
            shuffle_pos=(shuffle_pos+(unsigned long)(long)step) & m;
            if((step>0)&&(shuffle_pos==0ul))
            {
                Shuffle_Rekey();
            }
            e=(unsigned int)Shuffle_Map(shuffle_pos)+1u;
            if((e<=n)&&(Player_Is_Song(e)!=0u))
            {
                return e;
            }
        }
        return entry;
    }

    e=entry;
    for(i=0ul;i<(unsigned long)n;i++)
    {
        if(step>0)
        {
            e=(e>=n) ? 1u : (e+1u);
        }
        else
        {
            e=(e<=1u) ? n : (e-1u);
        }

        if(Player_Is_Song(e)!=0u)
        {
            return e;
        }
    }

    return entry;
}

/* entry's place among the folder's songs (1-based) and their count, from
   the types known: no file is opened for it. */
void Player_Position(unsigned int entry,unsigned int *index,unsigned int *count)
{
    unsigned int e;

    *index=0u;
    *count=0u;

    for(e=1u;e<=Number_Of_Files_Indirectory;e++)
    {
        if(Browser_Is_Song(FS_Peek_Entry_Type(e))!=0u)
        {
            (*count)++;
            if(e==entry)
            {
                *index=*count;
            }
        }
    }
}

/* A new key press: all keys up for PLAYER_RELEASE_MS since the last one,
   then a key down and still down 15 ms later. */
unsigned char Player_Key(void)
{
    unsigned char key;

    if((Button1==0)&&(Button2==0)&&(Button3==0))
    {
        if(player_up==0u)
        {
            player_up=1u;
            player_up_ticks=Ticks_Now();
        }
        else if(Ticks_Elapsed_Ms(player_up_ticks)>=PLAYER_RELEASE_MS)
        {
            player_held=0u;
        }
        return PLAYER_NONE;
    }

    player_up=0u;
    if(player_held!=0u)
    {
        return PLAYER_NONE;
    }

    Delay_ms(15);
    if(Button3==1)
    {
        key=PLAYER_STOP;
    }
    else if(Button1==1)
    {
        key=PLAYER_PREV;
    }
    else if(Button2==1)
    {
        key=PLAYER_NEXT;
    }
    else
    {
        return PLAYER_NONE;
    }

    player_held=1u;
    return key;
}

/* MUSIC.h polls this during a load.  A key press is kept until the load
   ends at the next chunk boundary, so a short press is not missed. */
unsigned char MUS_Load_Abort(void)
{
    if(player_abort==PLAYER_NONE)
    {
        player_abort=Player_Key();
    }
    return (player_abort!=PLAYER_NONE) ? 1u : 0u;
}

/* AUDIO.h polls this while a recording goes out. */
unsigned char AUD_Poll_Key(void)
{
    return Player_Key();
}

/* Waits while the song plays; returns the key pressed, or PLAYER_END. */
unsigned char Player_Wait(void)
{
    unsigned long low_ticks;
    unsigned char low;
    unsigned char key;

    low=0u;
    low_ticks=0ul;

    while(1)
    {
        key=Player_Key();
        if(key!=PLAYER_NONE)
        {
            return key;
        }

        if(DREQ!=0)
        {
            low=0u;
        }
        else if(low==0u)
        {
            low=1u;
            low_ticks=Ticks_Now();
        }
        else if(Ticks_Elapsed_Ms(low_ticks)>=PLAYER_END_LOW_MS)
        {
            return PLAYER_END;
        }
    }
}

/* Opens a song and lays out its image (a module) or finds its audio data
   and tags (a recording).  Returns 0, with g_mus_error set, if it cannot
   be played. */
unsigned char Player_Prepare(unsigned int entry,unsigned int *count)
{
    char name[VFS_NAME_LEN];
    unsigned int index;
    unsigned char type;

    Player_Position(entry,&index,count);
    type=FS_Get_Entry_Type(entry);
    FS_ClearString(name,VFS_NAME_LEN);
    FS_Get_Entry_Name(entry,name);

    if(Player_Is_Audio(type)!=0u)
    {
        if(AUD_Prepare(entry)==0u)
        {
            return 0u;
        }
        AUD_Set_Track(name,index,*count);
        return 1u;
    }

    if(FS_Open_File_By_Index(entry)==0u)
    {
        g_mus_error="CANNOT OPEN FILE";
        return 0u;
    }
    File_Size=FS_Get_CurrentFile_Size();

    if(MUS_Prepare(Player_Format(type))==0u)
    {
        return 0u;
    }
    MUS_Set_Track(name,index,*count);
    return 1u;
}

/* Message box on the browser until a key is pressed or 4 s pass. */
void Player_Error(const char *title)
{
    unsigned long t;

    GUI_Message(title,g_mus_error);
    Wait_Button_Release();

    t=Ticks_Now();
    while(Ticks_Elapsed_Ms(t)<4000ul)
    {
        if((Button1==1)||(Button2==1)||(Button3==1))
        {
            break;
        }
    }

    Wait_Button_Release();
    GUI_Invalidate();
}

/* Plays the folder from entry on, or shuffled, until Enter. */
void Play_Music(unsigned int entry,unsigned char shuffle)
{
    const char *title;        /* set: back in the browser, say why      */
    unsigned int count;
    unsigned int failed;      /* songs refused in a row                 */
    unsigned char shown;      /* MODE_VGA is up                         */
    unsigned char playing;    /* GO went out and no STOP since          */
    unsigned char action;
    signed char step;

    title=0;
    count=0u;
    failed=0u;
    shown=0u;
    playing=0u;
    step=1;
    player_held=1u;           /* keys must be seen up before a press counts */
    player_up=0u;
    player_shuffle=shuffle;
    if(shuffle!=0u)
    {
        Shuffle_Begin();
        entry=Player_Step(1u,1);
    }

    while(1)
    {
        if(Player_Prepare(entry,&count)==0u)
        {
            if((shown==0u)&&(shuffle==0u))
            {
                Player_Error("CANNOT PLAY");
                return;
            }

            failed++;
            if(failed>=count)
            {
                title="CANNOT PLAY";
                break;
            }
            entry=Player_Step(entry,step);
            continue;
        }
        failed=0u;

        if(shown==0u)
        {
            fillScreen(Black);
            MODE_VGA=1;
            shown=1u;
        }
        else if(playing!=0u)
        {
            playing=0u;
            if(AUD_Stop()==0u)
            {
                title="LOAD FAILED";
                break;
            }
        }

        if(Player_Is_Audio(FS_Get_Entry_Type(entry))!=0u)
        {
            action=AUD_Stream();
            if(action==AUD_FAILED)
            {
                title="LOAD FAILED";
                break;
            }
            playing=1u;
            if(action==AUD_SENT)
            {
                action=Player_Wait();
            }
        }
        else
        {
            player_abort=PLAYER_NONE;
            if(MUS_Stream()!=0u)
            {
                playing=1u;
                action=Player_Wait();
            }
            else if(g_mus_cancelled!=0u)
            {
                action=player_abort;
            }
            else
            {
                title="LOAD FAILED";
                break;
            }
        }

        if(action==PLAYER_STOP)
        {
            break;
        }
        step=(action==PLAYER_PREV) ? -1 : 1;
        entry=Player_Step(entry,step);
    }

    if(playing!=0u)
    {
        (void)AUD_Stop();
    }

    Wait_Button_Release();
    MODE_VGA=0;
    fillScreen(Black);
    player_shuffle=0u;

    x=Browser_Entry_To_Row(entry);
    Browser_Clamp();
    GUI_Invalidate();

    if(title!=0)
    {
        GUI_Render();
        Player_Error(title);
    }
}

/* ---------------------------------------------------------------------
   Browser actions
   --------------------------------------------------------------------- */

void Browser_Play_File(unsigned char type)
{
    fillScreen(Black);
    MODE_VGA=1;

    if(type==VFS_TYPE_MKV)
    {
        Play_MKV();
    }
    else if(type==VFS_TYPE_G3D)
    {
        Play_G3D();
    }
    else
    {
        Play_MPEG2();
    }

    MODE_VGA=0;
    fillScreen(Black);
    GUI_Invalidate();
}

void Browser_Open_Selected(void)
{
    unsigned long long selected_address;
    char name[VFS_NAME_LEN];
    unsigned int entry;

    if((Browser_Total_Items()==0u)||(x<1u))
    {
        return;
    }

    if(Browser_Is_Back_Row(x)!=0u)
    {
        Browser_Back();
        return;
    }
    if(Browser_Is_Shuffle_Row(x)!=0u)
    {
        Play_Music(0u,1u);
        return;
    }

    entry=Browser_Row_To_Entry(x);
    if(entry==0u)
    {
        return;
    }

    data2=FS_Get_Entry_Type(entry);

    if(data2==VFS_TYPE_DIR)
    {
        selected_address=FS_Get_Entry_Id(entry);

        FS_ClearString(name,VFS_NAME_LEN);
        FS_Get_Entry_Name(entry,name);

        if(Directory_Depth<15)
        {
            Directory_Stack[Directory_Depth]=Address;
            Directory_Depth++;
        }

        Browser_Open_Directory(selected_address);
        GUI_Set_Path(name);
        return;
    }

    if((data2==VFS_TYPE_JPEG)||(data2==VFS_TYPE_BMP))
    {
        Browser_View_Image(x);
        return;
    }

    if((data2==VFS_TYPE_AVI)||(data2==VFS_TYPE_MKV)||(data2==VFS_TYPE_G3D))
    {
        if(FS_Open_File_By_Index(entry)==0u)
        {
            return;
        }

        File_Size=FS_Get_CurrentFile_Size();
        Browser_Play_File(data2);
        return;
    }

    if(Browser_Is_Song(data2)!=0u)
    {
        Play_Music(entry,0u);
    }
}

/* Up/down with auto repeat: 400 ms before the first repeat, then 130 ms,
   60 ms once the key has been down for two seconds.  Held on, the steps
   grow, so that a folder of thousands is crossed in seconds: after 5 s a
   step is 1 % of the folder, after 10 s 5 %, five steps a second. */
#define BROWSE_FAST_MS    5000ul
#define BROWSE_FASTER_MS 10000ul

void Browser_Step(signed char dir,unsigned int n)
{
    if(dir<0)
    {
        x=(x>n) ? (x-n) : 1u;
    }
    else
    {
        x+=n;
    }
    Browser_Clamp();
    GUI_Render();
}

void Browser_Keys(void)
{
    unsigned long press_ticks;
    unsigned long last_ticks;
    unsigned long held;
    unsigned int delay_ms;
    unsigned int n;

    if((Button1==0)&&(Button2==0))
    {
        return;
    }

    Delay_ms(15);
    if((Button1==0)&&(Button2==0))
    {
        return;
    }

    press_ticks=Ticks_Now();
    last_ticks=press_ticks;
    Browser_Step((Button1==1) ? -1 : 1,1u);

    while((Button1==1)||(Button2==1))
    {
        if(Button3==1)
        {
            return;
        }

        held=Ticks_Elapsed_Ms(press_ticks);
        n=1u;
        if(held<400ul)
        {
            delay_ms=400u;
        }
        else if(held<2000ul)
        {
            delay_ms=130u;
        }
        else if(held<BROWSE_FAST_MS)
        {
            delay_ms=60u;
        }
        else
        {
            delay_ms=200u;
            n=Browser_Total_Items()/((held<BROWSE_FASTER_MS) ? 100u : 20u);
            if(n<1u)
            {
                n=1u;
            }
        }

        if(Ticks_Elapsed_Ms(last_ticks)>=(unsigned long)delay_ms)
        {
            last_ticks=Ticks_Now();
            Browser_Step((Button1==1) ? -1 : 1,n);
        }
    }
}

/* Short press opens, long press (>500 ms) goes back one level. */
void Browser_Enter_Key(void)
{
    unsigned long press_ticks;
    unsigned long held;

    if(Button3==0)
    {
        return;
    }

    Delay_ms(20);
    if(Button3==0)
    {
        return;
    }

    press_ticks=Ticks_Now();
    while(Button3==1)
    {
        if(Ticks_Elapsed_Ms(press_ticks)>3000ul)
        {
            break;
        }
    }

    held=Ticks_Elapsed_Ms(press_ticks);

    if(held>=500ul)
    {
        Browser_Back();
    }
    else
    {
        Browser_Open_Selected();
    }

    Wait_Button_Release();
}

/* ---------------------------------------------------------------------
   Boot screen
   --------------------------------------------------------------------- */

void Boot_Clock_Text(unsigned long hz,char *out)
{
    unsigned int n;

    n=GFX_U32_To_Str(hz/1000000ul,out);
    out[n]='.';
    n++;
    out[n]=(char)('0'+(unsigned char)((hz%1000000ul)/100000ul));
    n++;
    (void)GFX_Str_Copy(" MHz",&out[n],8u);
}

void Boot_Show_Static(void)
{
    char txt[32];
    unsigned long pb2_hz;
    unsigned long spi_hz;

    Boot_Begin("PIC32 MEDIA PLAYER");

    Boot_Clock_Text(CPU_HZ,txt);
    Boot_Set(0u,"CPU",txt,BOOT_VALUE);

    pb2_hz=CPU_HZ/((unsigned long)PB2DIVbits.PBDIV+1ul);
    spi_hz=pb2_hz/(2ul*((unsigned long)SPI1BRG+1ul));
    Boot_Clock_Text(spi_hz,txt);
    Boot_Set(1u,"FPGA SPI1",txt,BOOT_VALUE);

    Boot_Set(2u,"FRAME BUFFER","320x240 RGB565",BOOT_VALUE);
    Boot_Set(3u,"USB HOST","STARTING",BOOT_WARN);
    Boot_Set(4u,"DEVICE","-",BOOT_LABEL);
    Boot_Set(5u,"CAPACITY","-",BOOT_LABEL);
    Boot_Set(6u,"FILE SYSTEM","-",BOOT_LABEL);
    Boot_Set(7u,"CLUSTER","-",BOOT_LABEL);
    Boot_Set(8u,"ROOT","-",BOOT_LABEL);
    Boot_Set(9u,"STATUS","WAITING FOR DRIVE",BOOT_WARN);
}

void Boot_Show_Usb(void)
{
    char txt[40];
    unsigned int n;
    unsigned long mb;

    if(USBH_IsReady()==0u)
    {
        Boot_Value(3u,"NOT READY",BOOT_FAIL);
        Boot_Error(g_usbh.last_msg);
        return;
    }

    if(g_usbh.speed_code==USB_HOST_SPEED_HS)
    {
        Boot_Value(3u,"HIGH SPEED 480M",BOOT_OK);
    }
    else if(g_usbh.speed_code==USB_HOST_SPEED_FS)
    {
        Boot_Value(3u,"FULL SPEED 12M",BOOT_WARN);
    }
    else
    {
        Boot_Value(3u,"LOW SPEED 1.5M",BOOT_WARN);
    }

    n=GFX_Str_Copy(g_usbh.vendor,txt,20u);
    txt[n]=' ';
    n++;
    (void)GFX_Str_Copy(g_usbh.product,&txt[n],18u);
    Boot_Value(4u,txt,BOOT_VALUE);

    mb=(unsigned long)(((unsigned long long)g_usbh.block_count*
                        (unsigned long long)g_usbh.block_size)/(1024ull*1024ull));
    n=GFX_U32_To_Str(mb,txt);
    (void)GFX_Str_Copy(" MB",&txt[n],8u);
    Boot_Value(5u,txt,BOOT_VALUE);
}

void Boot_Show_Filesystem(void)
{
    char txt[32];
    char fs_name[20];
    unsigned int n;

    FS_ClearString(fs_name,20u);
    FS_Get_Type_Name(fs_name);
    Boot_Value(6u,fs_name,BOOT_OK);

    n=GFX_U32_To_Str(g_fs_cluster_size_bytes/1024ul,txt);
    (void)GFX_Str_Copy(" KB",&txt[n],8u);
    Boot_Value(7u,txt,BOOT_VALUE);

    n=GFX_U32_To_Str((unsigned long)Number_Of_Files_Indirectory,txt);
    (void)GFX_Str_Copy(" ITEMS",&txt[n],10u);
    Boot_Value(8u,txt,BOOT_VALUE);
}

/* Brings the drive and the filesystem up, reporting each stage.  Loops on
   the boot screen until both succeed. */
void Boot_Mount(void)
{
    while(1)
    {
        SDcard_init();

        if(USBH_IsReady()==0u)
        {
            Boot_Show_Usb();
            Boot_Value(9u,"NO DRIVE - RETRYING",BOOT_FAIL);
            USB_WaitReconnect();
            continue;
        }

        Boot_Show_Usb();
        Boot_Value(9u,"MOUNTING",BOOT_WARN);

        if(Init_FileSystem()==0u)
        {
            Boot_Value(6u,"UNRECOGNISED",BOOT_FAIL);
            Boot_Value(9u,"NO FILE SYSTEM",BOOT_FAIL);
            Boot_Error("FAT32 / exFAT / NTFS NOT FOUND");
            USB_WaitReconnect();
            continue;
        }

        Directory_Depth=0;
        Address=FS_Get_RootId();
        Number_Of_Files_Indirectory=FS_Open_DirectoryById(Address);
        Browser_Count_Songs();
        x=1;
        browser_top=1;

        Boot_Show_Filesystem();
        Boot_Value(9u,"READY",BOOT_OK);
        Boot_Error(0);
        return;
    }
}

int main(void)
{
    InitPorts();
    Init_Spi();
    TRISE=0XFF;

    MODE_VGA=0;
    fillScreen(Black);
    Delay_ms(200);

    Boot_Show_Static();
    Boot_Mount();

    Delay_ms(1200);
    Boot_End();

    SPI2BRG=1;

    GUI_Set_Path("ROOT");
    GUI_Invalidate();

    while(1)
    {
        USBH_Service();

        if(USBH_IsReady()==0u)
        {
            Boot_Show_Static();
            USB_WaitReconnect();
            Boot_Mount();
            Delay_ms(800);
            Boot_End();
            GUI_Set_Path("ROOT");
            GUI_Invalidate();
        }

        Browser_Clamp();
        GUI_Render();

        Browser_Keys();
        Browser_Clamp();
        Browser_Enter_Key();
        Browser_Clamp();
    }
}
