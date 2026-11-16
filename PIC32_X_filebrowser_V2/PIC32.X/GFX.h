#ifndef GFX_HEADER
#define GFX_HEADER

/* ---------------------------------------------------------------------
   Frame buffer primitives.

   One pixel write is 3 address bytes + 2 colour bytes on SPI1 followed by
   a MEMWR strobe.  Pixel() in FUNCTIONS.h polls SPIRBF after every byte,
   which idles the shifter between bytes.  GFX_Put() keeps the shifter fed
   back to back and only waits for SRMT before the MEMWR strobe, so the
   bus waveform is the same but the gaps are gone (~1.6x faster fills).

   GFX_PIPELINE 0 uses WriteSPI3() for every byte, so a pixel write is
   byte-for-byte identical to the original Pixel()/Memory_Wr() that the BMP
   and JPEG viewers use.  That is the default: it is the timing known to
   produce correct colours on this board.

   GFX_PIPELINE 1 queues the five bytes back-to-back and waits for SRMT
   before MEMWR, which is faster but gives the FPGA no inter-byte gap.  Try
   it once colours are confirmed correct; GFX_BYTE_GAP adds the gap back.
   --------------------------------------------------------------------- */
#define GFX_PIPELINE 0

/* Clocks of idle between pixel-write bytes when GFX_PIPELINE is 1.  The
   stream path inserts a gap of its own (AVI_SPI_Byte_Gap), so the FPGA
   evidently wants one; back-to-back bytes are what GFX_PIPELINE 1 sends. */
#define GFX_BYTE_GAP 4u

#define GFX_W 320u
#define GFX_H 240u

#define RGB565(r,g,b) ((unsigned int)((((r)&0xF8u)<<8)|(((g)&0xFCu)<<3)|(((b)&0xFFu)>>3)))

/* Drops any bytes the RX side collected during a burst.  WriteSPI3() waits
   for SPIRBF, so a stale RX byte would make it return early. */
#if GFX_PIPELINE
void GFX_Byte(unsigned char b)
{
    unsigned char g;

    while(SPI1STATbits.SPITBF)
    {
    }
    SPI1BUF=b;

    for(g=0u;g<GFX_BYTE_GAP;g++)
    {
        __asm__ volatile("nop");
    }
}
#endif

void GFX_Flush(void)
{
    volatile unsigned char dummy;

    while(!SPI1STATbits.SRMT)
    {
    }

    while(SPI1STATbits.SPIRBF)
    {
        dummy=(unsigned char)SPI1BUF;
        (void)dummy;
    }

    SPI1STATbits.SPIROV=0;
}

void GFX_Put(unsigned long addr,unsigned int color)
{
#if GFX_PIPELINE
    GFX_Byte((unsigned char)(addr>>16));
    GFX_Byte((unsigned char)(addr>>8));
    GFX_Byte((unsigned char)(addr));
    GFX_Byte((unsigned char)(color>>8));
    GFX_Byte((unsigned char)(color));
    while(!SPI1STATbits.SRMT){}
#else
    (void)WriteSPI3((unsigned char)(addr>>16));
    (void)WriteSPI3((unsigned char)(addr>>8));
    (void)WriteSPI3((unsigned char)(addr));
    (void)WriteSPI3((unsigned char)(color>>8));
    (void)WriteSPI3((unsigned char)(color));
#endif

    MEMWR=1;
    MEMWR=0;
}

/* Writes a run of pixels along one row.  GFX_Pixel flushes the SPI after
   every single pixel, which is the right thing for the odd dot but costs a
   drain loop 19200 times over during an image blit - here the flush happens
   once for the whole row. */
void GFX_Blit_Row(unsigned int x,unsigned int y,unsigned int w,
                  const unsigned short *src)
{
    unsigned long addr;
    unsigned int i;

    if(y>=GFX_H) return;
    if(x>=GFX_W) return;
    if((x+w)>GFX_W) w=GFX_W-x;

    addr=((unsigned long)y*GFX_W)+(unsigned long)x;

    for(i=0u;i<w;i++)
    {
        GFX_Put(addr+(unsigned long)i,(unsigned int)src[i]);
    }

    GFX_Flush();
}

void GFX_Pixel(unsigned int x,unsigned int y,unsigned int color)
{
    if((x>=GFX_W)||(y>=GFX_H))
    {
        return;
    }

    GFX_Put(((unsigned long)y*GFX_W)+(unsigned long)x,color);
    GFX_Flush();
}

void GFX_Fill_Rect(unsigned int x,unsigned int y,unsigned int w,unsigned int h,unsigned int color)
{
    unsigned long addr;
    unsigned int row;
    unsigned int col;

    if((x>=GFX_W)||(y>=GFX_H)||(w==0u)||(h==0u))
    {
        return;
    }

    if((x+w)>GFX_W)
    {
        w=GFX_W-x;
    }
    if((y+h)>GFX_H)
    {
        h=GFX_H-y;
    }

    addr=((unsigned long)y*GFX_W)+(unsigned long)x;

    for(row=0u;row<h;row++)
    {
        for(col=0u;col<w;col++)
        {
            GFX_Put(addr+(unsigned long)col,color);
        }
        addr+=GFX_W;
    }

    GFX_Flush();
}

void GFX_HLine(unsigned int x,unsigned int y,unsigned int w,unsigned int color)
{
    GFX_Fill_Rect(x,y,w,1u,color);
}

void GFX_VLine(unsigned int x,unsigned int y,unsigned int h,unsigned int color)
{
    GFX_Fill_Rect(x,y,1u,h,color);
}

void GFX_Frame(unsigned int x,unsigned int y,unsigned int w,unsigned int h,unsigned int color)
{
    if((w<2u)||(h<2u))
    {
        return;
    }

    GFX_HLine(x,y,w,color);
    GFX_HLine(x,y+h-1u,w,color);
    GFX_VLine(x,y,h,color);
    GFX_VLine(x+w-1u,y,h,color);
}

/* Filled circle, used for the progress bar knob.  r stays small so the
   bounding-box test costs less than a Bresenham setup. */
void GFX_Disc(unsigned int cx,unsigned int cy,unsigned int r,unsigned int color)
{
    int dx;
    int dy;
    int rr;
    int px;
    int py;

    rr=(int)(r*r)+(int)r;

    for(dy=-(int)r;dy<=(int)r;dy++)
    {
        py=(int)cy+dy;
        if((py<0)||(py>=(int)GFX_H))
        {
            continue;
        }

        for(dx=-(int)r;dx<=(int)r;dx++)
        {
            if(((dx*dx)+(dy*dy))>rr)
            {
                continue;
            }

            px=(int)cx+dx;
            if((px<0)||(px>=(int)GFX_W))
            {
                continue;
            }

            GFX_Put(((unsigned long)py*GFX_W)+(unsigned long)px,color);
        }
    }

    GFX_Flush();
}

/* Scroll-bar arrows: w is the base width, the tip sits w/2 rows away. */
void GFX_Tri_Up(unsigned int x,unsigned int y,unsigned int w,unsigned int color)
{
    unsigned int row;
    unsigned int half;

    half=w/2u;

    for(row=0u;row<half;row++)
    {
        GFX_Fill_Rect(x+half-row,y+row,(row*2u)+1u,1u,color);
    }
}

void GFX_Tri_Down(unsigned int x,unsigned int y,unsigned int w,unsigned int color)
{
    unsigned int row;
    unsigned int half;

    half=w/2u;

    for(row=0u;row<half;row++)
    {
        GFX_Fill_Rect(x+row,y+row,((half-row)*2u)+1u,1u,color);
    }
}

/* ---------------------------------------------------------------------
   Text helpers.  text3() paints the background of every glyph cell, so
   text can be redrawn over itself without clearing first as long as the
   new string is at least as wide as the old one.
   --------------------------------------------------------------------- */
#define GFX_FONT_H 16u
#define GFX_SPACING 1u

unsigned int GFX_Char_Width(unsigned char c)
{
    if((c<32u)||(c>126u))
    {
        c='?';
    }

    return (unsigned int)CHARACTER_WIDTH((unsigned int)c-32u,Times_New_Roman16x16)+GFX_SPACING;
}

unsigned int GFX_Text_Width(const char *s)
{
    unsigned int w;

    w=0u;
    while(*s!=0)
    {
        w+=GFX_Char_Width((unsigned char)*s);
        s++;
    }

    return w;
}

void GFX_Text(const char *s,unsigned int x,unsigned int y,unsigned int bg,unsigned int fg)
{
    text3((unsigned char *)s,x,y,(unsigned char)GFX_SPACING,bg,fg);
}

/* Copies s into out, replacing unprintable bytes and truncating with ".."
   so the result renders no wider than max_w pixels. */
void GFX_Fit(const char *s,char *out,unsigned int out_size,unsigned int max_w)
{
    unsigned int w;
    unsigned int n;
    unsigned int dots;
    unsigned char c;

    w=0u;
    n=0u;
    dots=GFX_Char_Width('.')*2u;

    while((s[n]!=0)&&(n<(out_size-1u)))
    {
        c=(unsigned char)s[n];
        if((c<32u)||(c>126u))
        {
            c=' ';
        }

        if((w+GFX_Char_Width(c))>max_w)
        {
            /* Back off far enough to fit ".." in place of the rest. */
            while((n>0u)&&((w+dots)>max_w))
            {
                n--;
                w-=GFX_Char_Width((unsigned char)out[n]);
            }
            out[n]='.';
            if((n+1u)<(out_size-1u))
            {
                out[n+1u]='.';
                n+=2u;
            }
            else
            {
                n++;
            }
            break;
        }

        out[n]=(char)c;
        w+=GFX_Char_Width(c);
        n++;
    }

    out[n]=0;
}

void GFX_Text_Right(const char *s,unsigned int right_x,unsigned int y,unsigned int bg,unsigned int fg)
{
    unsigned int w;

    w=GFX_Text_Width(s);
    if(w>right_x)
    {
        w=right_x;
    }

    GFX_Text(s,right_x-w,y,bg,fg);
}

/* ---------------------------------------------------------------------
   Small formatting helpers (no printf in this build).
   --------------------------------------------------------------------- */
unsigned int GFX_U32_To_Str(unsigned long v,char *out)
{
    char tmp[12];
    unsigned int i;
    unsigned int n;

    i=0u;
    if(v==0ul)
    {
        out[0]='0';
        out[1]=0;
        return 1u;
    }

    while((v!=0ul)&&(i<11u))
    {
        tmp[i]=(char)('0'+(unsigned char)(v%10ul));
        v/=10ul;
        i++;
    }

    n=i;
    while(i>0u)
    {
        i--;
        *out=tmp[i];
        out++;
    }
    *out=0;

    return n;
}

unsigned int GFX_Str_Copy(const char *src,char *dst,unsigned int limit)
{
    unsigned int n;

    n=0u;
    while((src[n]!=0)&&(n<limit))
    {
        dst[n]=src[n];
        n++;
    }
    dst[n]=0;

    return n;
}

/* Two-digit zero padded field, used by the clock formatter. */
void GFX_Pad2(unsigned long v,char *out)
{
    out[0]=(char)('0'+(unsigned char)((v/10ul)%10ul));
    out[1]=(char)('0'+(unsigned char)(v%10ul));
    out[2]=0;
}

#endif
