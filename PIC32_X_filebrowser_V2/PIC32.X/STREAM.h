#ifndef STREAM_H
#define STREAM_H

/* ---------------------------------------------------------------------
   Byte transport to the FPGA plus the small cached reader used by the
   container parsers.  Moved out of newmain.c unchanged except for two
   additions:

     - a chunk can now be aborted by the seek keys as well as by STOP, and
       the caller is told exactly how many bytes made it out, so the file
       position stays exact after an abort;
     - each streamed block is handed to the MKV clock tracker before it is
       sent, which is what keeps the elapsed time on the seek OSD honest.
   --------------------------------------------------------------------- */

#define AVI_STREAM_BUF_SIZE 16384u
#define AVI_SEEK_CACHE_SIZE 512u
#define AVI_SPI_DMA_ENABLE 1u
#define AVI_SPI_DMA_TIMEOUT 12000000ul
#define AVI_DREQ_TIMEOUT 12000000ul

/*
   DREQ is still used as FPGA flow control.  A large continuous SPI DMA
   transfer cannot pause when DREQ drops, so the stream is split into small
   DMA bursts and DREQ is checked before every burst.

   AVI_DREQ_DMA_BURST_SIZE == 1  polled byte loop, one DREQ check per byte.
   AVI_DREQ_DMA_BURST_SIZE > 1   DMA bursts; the FPGA FIFO must accept that
                                 many bytes after one DREQ==1.

   AVI_SPI_BYTE_GAP_CYCLES (polled mode only) NOP gap after each byte.
     0 : ~6.0 MB/s   4 : ~5.1 MB/s   8 : ~4.5 MB/s   12 : ~4.0 MB/s
*/
#define AVI_DREQ_DMA_BURST_SIZE 1u
#define AVI_SPI_BYTE_GAP_CYCLES 4u

#ifndef AVI_SPI1_TX_DMA_IRQ
    #if defined(_SPI1_TX_IRQ)
        #define AVI_SPI1_TX_DMA_IRQ _SPI1_TX_IRQ
    #elif defined(_SPI1_TX_VECTOR)
        #define AVI_SPI1_TX_DMA_IRQ _SPI1_TX_VECTOR
    #else
        #define AVI_SPI1_TX_DMA_IRQ 111u
    #endif
#endif

#ifndef KVA_TO_PA
#define KVA_TO_PA(v) (((unsigned long)(v)) & 0x1FFFFFFFul)
#endif

static unsigned char avi_stream_buffer_a[AVI_STREAM_BUF_SIZE] __attribute__((coherent, aligned(16)));
static unsigned char avi_stream_buffer_b[AVI_STREAM_BUF_SIZE] __attribute__((coherent, aligned(16)));

#define STREAM_ABORT_NONE  0u
#define STREAM_ABORT_STOP  1u
#define STREAM_ABORT_SEEK  2u
#define STREAM_ABORT_ERROR 3u

unsigned char g_stream_seek_keys=0u;    /* 1 = UP/DOWN also abort a chunk */
unsigned char g_stream_abort_reason=STREAM_ABORT_NONE;
unsigned char g_mkv_track_enable=0u;    /* 1 = feed blocks to the MKV clock */

/* Defined in MKV_SEEK.h, which is included after this header. */
void MKV_Track_Block(const unsigned char *buf,unsigned int len,unsigned long file_pos);

unsigned char Stream_Check_Abort(void)
{
    if(Button3==1)
    {
        g_stream_abort_reason=STREAM_ABORT_STOP;
        return 1u;
    }

    if((g_stream_seek_keys!=0u)&&((Button1==1)||(Button2==1)))
    {
        g_stream_abort_reason=STREAM_ABORT_SEEK;
        return 1u;
    }

    return 0u;
}

/* ------------------------ cached file reader ------------------------ */

static unsigned char avi_seek_buffer[512];
static unsigned long avi_seek_cached_sector=0xFFFFFFFFul;
static unsigned char avi_seek_cache_valid=0u;

void AVI_SeekCache_Reset(void)
{
    avi_seek_cache_valid=0u;
    avi_seek_cached_sector=0xFFFFFFFFul;
}

unsigned char AVI_Read_Block(unsigned long file_pos,unsigned char *dst,unsigned int len)
{
    unsigned long remain;

    if((file_pos>=File_Size)||(dst==0)||(len==0u))
    {
        return 0u;
    }

    remain=File_Size-file_pos;
    if(((unsigned long)len)>remain)
    {
        len=(unsigned int)remain;
    }

    return FS_Read_CurrentFile_Bytes(file_pos,dst,len);
}

unsigned char AVI_Read_Byte(unsigned long file_pos,unsigned char *value)
{
    unsigned long cache_base;

    if(file_pos>=File_Size)
    {
        return 0u;
    }

    cache_base=file_pos & 0xFFFFFE00ul;
    if((avi_seek_cache_valid==0u)||(avi_seek_cached_sector!=cache_base))
    {
        if(AVI_Read_Block(cache_base,avi_seek_buffer,AVI_SEEK_CACHE_SIZE)==0u)
        {
            return 0u;
        }
        avi_seek_cached_sector=cache_base;
        avi_seek_cache_valid=1u;
    }

    *value=avi_seek_buffer[(unsigned int)(file_pos-cache_base)];
    return 1u;
}

unsigned char AVI_Read_LE32(unsigned long file_pos,unsigned long *value)
{
    unsigned char b0;
    unsigned char b1;
    unsigned char b2;
    unsigned char b3;

    if(AVI_Read_Byte(file_pos+0ul,&b0)==0u) return 0u;
    if(AVI_Read_Byte(file_pos+1ul,&b1)==0u) return 0u;
    if(AVI_Read_Byte(file_pos+2ul,&b2)==0u) return 0u;
    if(AVI_Read_Byte(file_pos+3ul,&b3)==0u) return 0u;

    *value=((unsigned long)b0)
         | ((unsigned long)b1<<8)
         | ((unsigned long)b2<<16)
         | ((unsigned long)b3<<24);
    return 1u;
}

/* ------------------------ SPI transport ----------------------------- */

unsigned char AVI_Wait_DREQ(unsigned char allow_abort)
{
    unsigned long timeout;

    timeout=AVI_DREQ_TIMEOUT;
    while(DREQ==0)
    {
        if((allow_abort!=0u)&&(Stream_Check_Abort()!=0u))
        {
            return 0u;
        }

        if(timeout==0ul)
        {
            g_stream_abort_reason=STREAM_ABORT_ERROR;
            return 0u;
        }
        timeout--;
    }

    return 1u;
}

void AVI_SPI_Byte_Gap(void)
{
#if (AVI_SPI_BYTE_GAP_CYCLES != 0u)
    unsigned int n;

    for(n=0u;n<AVI_SPI_BYTE_GAP_CYCLES;n++)
    {
        __asm__ volatile ("nop");
    }
#endif
}

void AVI_SPI_Drain_RX(void)
{
    volatile unsigned char dummy;

    while(SPI1STATbits.SPIRBF)
    {
        dummy=(unsigned char)SPI1BUF;
        (void)dummy;
    }
    SPI1STATbits.SPIROV=0;
}

void AVI_SPI_DMA_Init(void)
{
#if AVI_SPI_DMA_ENABLE
    SPI1CON2bits.IGNROV=1;
    SPI1CON2bits.IGNTUR=1;
    DMACONbits.ON=1;
    DCH0CONbits.CHEN=0;
    DCH0CON=0;
    DCH0ECON=0;
    DCH0INTCLR=0xFFFFFFFFul;
    DCH0CONbits.CHPRI=3;
    DCH0ECONbits.CHSIRQ=AVI_SPI1_TX_DMA_IRQ;
    DCH0ECONbits.SIRQEN=1;
#endif
}

unsigned char AVI_SPI_DMA_Start(const unsigned char *src,unsigned int len)
{
#if AVI_SPI_DMA_ENABLE
    if((src==0)||(len==0u))
    {
        return 0u;
    }

    if(AVI_Wait_DREQ(1u)==0u)
    {
        return 0u;
    }

    AVI_SPI_Drain_RX();
    DCH0CONbits.CHEN=0;
    DCH0ECON=0;
    DCH0INTCLR=0xFFFFFFFFul;
    DCH0SSA=KVA_TO_PA(src);
    DCH0DSA=KVA_TO_PA((void *)&SPI1BUF);
    DCH0SSIZ=len;
    DCH0DSIZ=1u;
    DCH0CSIZ=1u;
    DCH0CONbits.CHPRI=3;
    DCH0ECONbits.CHSIRQ=AVI_SPI1_TX_DMA_IRQ;
    DCH0ECONbits.SIRQEN=1;
    DCH0CONbits.CHEN=1;
    DCH0ECONbits.CFORCE=1;
    return 1u;
#else
    (void)src;
    (void)len;
    return 0u;
#endif
}

unsigned char AVI_SPI_DMA_Wait(unsigned char allow_abort)
{
#if AVI_SPI_DMA_ENABLE
    unsigned long timeout;

    timeout=AVI_SPI_DMA_TIMEOUT;
    while(DCH0INTbits.CHBCIF==0)
    {
        if(DCH0INTbits.CHERIF!=0)
        {
            DCH0CONbits.CHEN=0;
            AVI_SPI_Drain_RX();
            g_stream_abort_reason=STREAM_ABORT_ERROR;
            return 0u;
        }

        if((allow_abort!=0u)&&(Stream_Check_Abort()!=0u))
        {
            DCH0ECONbits.CABORT=1;
            DCH0CONbits.CHEN=0;
            AVI_SPI_Drain_RX();
            return 0u;
        }

        if(timeout==0ul)
        {
            DCH0ECONbits.CABORT=1;
            DCH0CONbits.CHEN=0;
            AVI_SPI_Drain_RX();
            g_stream_abort_reason=STREAM_ABORT_ERROR;
            return 0u;
        }
        timeout--;
    }

    while(!SPI1STATbits.SRMT)
    {
    }
    AVI_SPI_Drain_RX();
    return 1u;
#else
    (void)allow_abort;
    return 0u;
#endif
}

/* Returns 1 when the whole buffer went out.  *sent always holds the
   number of bytes actually transmitted, including on an abort. */
unsigned char AVI_Send_Buffer_Polled(const unsigned char *src,unsigned int len,unsigned int *sent)
{
    volatile unsigned char dummy;
    unsigned int i;

    for(i=0u;i<len;i++)
    {
        if((i & 0x0Fu)==0u)
        {
            if(Stream_Check_Abort()!=0u)
            {
                *sent=i;
                return 0u;
            }
        }

        if(AVI_Wait_DREQ(1u)==0u)
        {
            *sent=i;
            return 0u;
        }

        while(SPI1STATbits.SPITBF)
        {
        }
        SPI1BUF=src[i];
        while(!SPI1STATbits.SPIRBF)
        {
        }
        dummy=(unsigned char)SPI1BUF;
        (void)dummy;
        AVI_SPI_Byte_Gap();
    }

    while(!SPI1STATbits.SRMT)
    {
    }
    AVI_SPI_Drain_RX();

    *sent=len;
    return 1u;
}

unsigned char AVI_Send_Buffer(const unsigned char *src,unsigned int len,unsigned int *sent)
{
#if AVI_SPI_DMA_ENABLE
    const unsigned char *p;
    unsigned int chunk;
    unsigned int done;
    unsigned int part;

    if((src==0)||(len==0u))
    {
        *sent=0u;
        return 0u;
    }

    if(AVI_DREQ_DMA_BURST_SIZE<=1u)
    {
        return AVI_Send_Buffer_Polled(src,len,sent);
    }

    AVI_SPI_DMA_Init();
    p=src;
    done=0u;

    while(done<len)
    {
        chunk=AVI_DREQ_DMA_BURST_SIZE;
        if((len-done)<chunk)
        {
            chunk=len-done;
        }

        if(AVI_Wait_DREQ(1u)==0u)
        {
            *sent=done;
            return 0u;
        }

        if(AVI_SPI_DMA_Start(p,chunk)==0u)
        {
            if(AVI_Send_Buffer_Polled(p,chunk,&part)==0u)
            {
                *sent=done+part;
                return 0u;
            }
        }
        else
        {
            if(AVI_SPI_DMA_Wait(1u)==0u)
            {
                /* Burst granularity: a DMA abort loses the byte count of
                   the running burst, so the whole burst is reported as
                   not sent and the caller re-sends from there. */
                *sent=done;
                return 0u;
            }
        }

        p+=chunk;
        done+=chunk;
    }

    *sent=len;
    return 1u;
#else
    return AVI_Send_Buffer_Polled(src,len,sent);
#endif
}

/* Streams byte_count bytes starting at start_pos.  *end_pos is always set
   to the position after the last byte that actually reached the FPGA. */
/* ------------------------- decoder flush -----------------------------
   Pushes padding through to the FPGA so nothing of the previous burst is
   left sitting in its input FIFO when playback resumes.

   The padding is a Void element - 0xEC, a four byte size, then the filler
   - rather than bare zeros, because the reader parses whatever arrives.
   A run of zeros is not inert to it: a 0x00 ID byte is a one byte ID and
   the 0x00 size byte that follows starts an eight byte size vint, so zeros
   are swallowed nine at a time as empty elements.  That works only while
   the count stays a multiple of nine - end the run mid-vint and the first
   bytes of the next cluster header get eaten as size bytes, which is
   exactly the corruption it was meant to prevent.  A Void says how long it
   is, so the reader consumes precisely the bytes sent whatever the length.

   Set MKV_FLUSH_RAW to 1 to send bare zeros instead; the length is then
   rounded to a multiple of nine. */

#ifndef MKV_FLUSH_RAW
#define MKV_FLUSH_RAW 0
#endif

#ifndef MKV_FLUSH_BYTES
#define MKV_FLUSH_BYTES 1024u
#endif

static unsigned char avi_flush_buf[64];

unsigned char AVI_Flush_Decoder(unsigned int bytes)
{
    unsigned int sent;
    unsigned int chunk;
    unsigned int i;
    unsigned char keys;
    unsigned char ok;

    keys=g_stream_seek_keys;
    g_stream_seek_keys=0u;          /* a held key must not cut the flush */

    AVI_SPI_DMA_Init();

    for(i=0u;i<sizeof avi_flush_buf;i++) avi_flush_buf[i]=0u;

    ok=1u;

#if MKV_FLUSH_RAW
    bytes=(bytes/9u)*9u;            /* whole empty elements only */
    if(bytes==0u) bytes=9u;
#else
    {
        unsigned char hdr[5];

        hdr[0]=0xECu;                                    /* Void */
        hdr[1]=(unsigned char)(0x10u|((bytes>>24)&0x0Fu));
        hdr[2]=(unsigned char)(bytes>>16);
        hdr[3]=(unsigned char)(bytes>>8);
        hdr[4]=(unsigned char)bytes;

        ok=AVI_Send_Buffer(hdr,5u,&sent);
    }
#endif

    while((ok!=0u)&&(bytes>0u))
    {
        chunk=(bytes>(unsigned int)sizeof avi_flush_buf)
              ? (unsigned int)sizeof avi_flush_buf : bytes;

        ok=AVI_Send_Buffer(avi_flush_buf,chunk,&sent);
        if(sent<chunk) { ok=0u; break; }

        bytes-=chunk;
    }

    g_stream_seek_keys=keys;

    return ok;
}

unsigned char AVI_Stream_Range(unsigned long start_pos,
                               unsigned long byte_count,
                               unsigned long *end_pos)
{
    unsigned long pos;
    unsigned int take;
    unsigned int sent;
    unsigned char *buf;

    pos=start_pos;
    buf=avi_stream_buffer_a;
    g_stream_abort_reason=STREAM_ABORT_NONE;

    if(byte_count==0ul)
    {
        if(end_pos!=0)
        {
            *end_pos=pos;
        }
        return 1u;
    }

    AVI_SPI_DMA_Init();

    /* Multi-sector reads only while streaming; the browser, FAT and JPEG
       paths all run with this off. */
    g_fs_multi_read_ok=1u;

    while(byte_count!=0ul)
    {
        take=AVI_STREAM_BUF_SIZE;
        if(byte_count<((unsigned long)take))
        {
            take=(unsigned int)byte_count;
        }

        if(AVI_Read_Block(pos,buf,take)==0u)
        {
            g_fs_multi_read_ok=0u;
            g_stream_abort_reason=STREAM_ABORT_ERROR;
            if(end_pos!=0)
            {
                *end_pos=pos;
            }
            return 0u;
        }

        if(g_mkv_track_enable!=0u)
        {
            MKV_Track_Block(buf,take,pos);
        }

        if(AVI_Send_Buffer(buf,take,&sent)==0u)
        {
            g_fs_multi_read_ok=0u;
            if(end_pos!=0)
            {
                *end_pos=pos+(unsigned long)sent;
            }
            return 0u;
        }

        pos+=(unsigned long)take;
        byte_count-=((unsigned long)take);

        buf=(buf==avi_stream_buffer_a) ? avi_stream_buffer_b : avi_stream_buffer_a;
    }

    g_fs_multi_read_ok=0u;

    if(end_pos!=0)
    {
        *end_pos=pos;
    }

    return 1u;
}

#endif
