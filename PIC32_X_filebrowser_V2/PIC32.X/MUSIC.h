#ifndef MUSIC_H
#define MUSIC_H

/* ---------------------------------------------------------------------
   Tracker modules - MOD, S3M, XM, IT - for the FPGA music player.

   The PIC32 does for a module what gfx_cow.py does for a 3D scene: it
   turns the file into the loader's chunk stream.  Everything that is only
   encoding is undone here - packed patterns, parapointers, delta, ADPCM
   and IT214/215 compressed samples, 8/16 bit, signed/unsigned, endianness
   - so the FPGA always finds the same fixed layout in SDRAM.  Playback
   semantics stay native: notes, effects and volume-column bytes are the
   format's own.  README_MUSIC.md documents the layout word by word.

   Chunk on the wire, as g3d_loader.v parses it (big-endian):
       A5 3D type[2] addr[4] len[4] sum[4] payload[len*4]
   addr/len count SDRAM words; sum is the 32-bit sum of the payload words.
   GO (addr = header) ends the load and starts the song.  A CT chunk (len 0,
   command in addr) controls playback: STOP before the next load.

   MUS_Prepare() reads headers only and lays out the whole image, so a bad
   or oversized file is refused before anything is sent.  MUS_Stream()
   then sends the image and GO.  A chunk always goes out whole - the
   player's MUS_Load_Abort() is polled between chunks - so the loader
   never loses step.
   --------------------------------------------------------------------- */

#define MUS_BASE          0x100000ul    /* image start, SDRAM word address   */
#define MUS_SDRAM_END     0x800000ul    /* 8M words: 2 x IS42S16320D, BA=0   */

#define MUS_CHUNK_WORDS   4096u         /* largest payload in one chunk      */
#define MUS_RD_SIZE       4096u         /* sequential file reader buffer     */

#define MUS_MAX_CH        64u
#define MUS_MAX_ORD       256u
#define MUS_MAX_PAT       256u
#define MUS_MAX_ROWS      1024u
#define MUS_MAX_INS       255u
#define MUS_MAX_SMP       512u
#define MUS_MAX_FRAMES    0x01000000ul  /* one sample channel, whole SDRAM   */

/* Image layout: word offsets from the header. */
#define MUS_HDR_WORDS     64u
#define MUS_OFS_CHAN      64u
#define MUS_OFS_ORDER     128u
#define MUS_OFS_PATTAB    384u          /* 256 x {address, rows | flags}     */
#define MUS_OFS_TABLES    896u          /* MIDI macros, instruments, samples */
#define MUS_MIDI_WORDS    1224u         /* 4896 bytes of IT MIDI macros      */
#define MUS_INS_WORDS     256u
#define MUS_SMP_WORDS     32u
#define MUS_SMP_PAD       8u            /* zero words after every sample     */

#define MUS_MAGIC         0x4D4F4450ul  /* 'MODP' */
#define MUS_VERSION       1u

#define MUS_FMT_MOD       1u
#define MUS_FMT_S3M       2u
#define MUS_FMT_XM        3u
#define MUS_FMT_IT        4u

/* Pattern cell notes: 1..120 = C-0..B-9 with 61 = middle C. */
#define MUS_NOTE_FADE     0xFDu
#define MUS_NOTE_CUT      0xFEu
#define MUS_NOTE_OFF      0xFFu

/* Header flags (word 6) */
#define MUS_HF_LINEAR     0x0001ul      /* linear frequency slides           */
#define MUS_HF_INSTR      0x0002ul      /* instrument mode                   */
#define MUS_HF_STEREO     0x0004ul
#define MUS_HF_OLDFX      0x0008ul      /* IT old effects                    */
#define MUS_HF_COMPATG    0x0010ul      /* IT compatible Gxx                 */
#define MUS_HF_FASTVOL    0x0020ul      /* S3M fast volume slides            */
#define MUS_HF_AMIGALIM   0x0040ul      /* S3M Amiga limits                  */
#define MUS_HF_ST2VIB     0x0080ul      /* S3M ST2 vibrato                   */
#define MUS_HF_PTMODE     0x0100ul      /* MOD: M.K./M!K!, Amiga notes only  */
#define MUS_HF_VBLANK     0x0200ul      /* MOD: VBlank-timed tracker         */
#define MUS_HF_FT2        0x0400ul      /* XM: saved by FastTracker 2        */
#define MUS_HF_ADLIB      0x0800ul      /* S3M: Adlib instruments, silent    */
#define MUS_HF_MIDICFG    0x1000ul      /* IT: MIDI macros in the image      */
#define MUS_HF_CHCUT      0x2000ul      /* channels beyond 64 were dropped   */
#define MUS_HF_ST15       0x4000ul      /* MOD: 15-sample Soundtracker       */

/* Channel table */
#define MUS_CF_MUTE       0x00010000ul
#define MUS_CF_SURROUND   0x00020000ul
#define MUS_CF_UNUSED     0x00040000ul

/* Pattern table flags (word 1, bits 31:16) */
#define MUS_PF_EMPTY      0x00010000ul  /* no data: points at the zero block */
#define MUS_PF_MISSING    0x00020000ul  /* referenced but not in the file    */

/* Sample record flags */
#define MUS_SF_DATA       0x0001u
#define MUS_SF_LOOP       0x0002u
#define MUS_SF_BIDI       0x0004u
#define MUS_SF_SUS        0x0008u
#define MUS_SF_SUSBIDI    0x0010u
#define MUS_SF_SRC16      0x0020u
#define MUS_SF_STEREO     0x0040u
#define MUS_SF_PAN        0x0080u
#define MUS_SF_ADLIB      0x0100u
#define MUS_SF_PACKED     0x0200u       /* compressed or ADPCM in the file   */
#define MUS_SF_NODATA     0x0400u       /* data absent or unsupported codec  */

/* Instrument record flags (word 0, bits 31:24) */
#define MUS_IF_PAN        0x01u
#define MUS_IF_CUTOFF     0x02u
#define MUS_IF_RESO       0x04u
#define MUS_IF_OLDIT      0x08u

/* Envelope flags */
#define MUS_EF_ON         0x01u
#define MUS_EF_LOOP       0x02u
#define MUS_EF_SUS        0x04u
#define MUS_EF_CARRY      0x08u
#define MUS_EF_FILTER     0x10u

/* Control commands (CT chunk, address field; other values reserved) */
#define MUS_CT_STOP       1u

/* Header words the player fills in (MUS_Set_Track) */
#define MUS_HW_TRACK      21u           /* index << 16 | count, 1-based      */
#define MUS_HW_FILE       24u           /* 16 words: file name, 64 bytes     */

/* Sample encodings in the file */
#define MUS_ENC_16        0x0001u
#define MUS_ENC_UNSIGNED  0x0002u
#define MUS_ENC_DELTA     0x0004u
#define MUS_ENC_BE        0x0008u
#define MUS_ENC_STEREO    0x0010u
#define MUS_ENC_ADPCM     0x0020u
#define MUS_ENC_IT214     0x0040u
#define MUS_ENC_IT215     0x0080u
#define MUS_ENC_PTM16     0x0100u

#define MUS_PAT_MISSING   0u
#define MUS_PAT_EMPTY     1u
#define MUS_PAT_DATA      2u

/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned long pos;        /* file offset of the pattern data         */
    unsigned long len;        /* packed bytes (S3M: 0, read to row 64)   */
    unsigned long addr;       /* SDRAM address of the unpacked pattern   */
    unsigned int  rows;
    unsigned char kind;
} MUS_PAT;

typedef struct
{
    unsigned long hdr;        /* file offset of the sample header        */
    unsigned long data;       /* file offset of the sample data          */
    unsigned long addr;       /* SDRAM address, mono or left             */
    unsigned long addr_r;     /* SDRAM address, right (stereo)           */
    unsigned char ins;        /* XM: owning instrument, 1-based          */
} MUS_SMP;

/* One sample header in common terms. */
typedef struct
{
    unsigned long data;
    unsigned long length;     /* frames                                  */
    unsigned long loop_start;
    unsigned long loop_end;
    unsigned long sus_start;
    unsigned long sus_end;
    unsigned long c5speed;
    unsigned int  flags;      /* MUS_SF_*                                */
    unsigned int  enc;        /* MUS_ENC_*                               */
    unsigned char volume;     /* 0..64                                   */
    unsigned char gvol;       /* 0..64                                   */
    unsigned char pan;        /* 0..255                                  */
    signed char   finetune;
    signed char   relnote;
    unsigned char vib[4];     /* type, sweep, depth, rate                */
    char          name[32];
    char          file[12];
} MUS_SMPHDR;

typedef struct
{
    unsigned char  points;
    unsigned char  flags;     /* MUS_EF_*                                */
    unsigned char  loop_start;
    unsigned char  loop_end;
    unsigned char  sus_start;
    unsigned char  sus_end;
    unsigned int   tick[25];
    int            value[25];
} MUS_ENV;

/* One instrument in common terms. */
typedef struct
{
    unsigned char  nna;
    unsigned char  dct;
    unsigned char  dca;
    unsigned char  flags;     /* MUS_IF_*                                */
    unsigned long  fadeout;   /* per tick, 65536 = full volume           */
    unsigned char  gvol;      /* 0..128                                  */
    unsigned char  pan;
    signed char    pps;
    unsigned char  ppc;       /* note 1..120                             */
    unsigned char  rv;
    unsigned char  rp;
    unsigned char  ifc;
    unsigned char  ifr;
    unsigned char  vib[4];
    unsigned char  midi_ch;
    unsigned char  midi_prog;
    unsigned int   midi_bank;
    unsigned int   nsmp;
    unsigned int   key_smp[120];   /* 1-based sample, 0 = none           */
    unsigned char  key_note[120];
    MUS_ENV        env[3];         /* volume, panning, pitch/filter      */
    char           name[32];
} MUS_INSHDR;

typedef struct
{
    unsigned char fmt;
    unsigned char ch;         /* channels in the image                   */
    unsigned char file_ch;    /* channels per row in the file (MOD, XM)  */
    unsigned char flt8;       /* MOD: Startrekker FLT8 pattern pairs     */
    unsigned char strek;      /* MOD: Startrekker (FLTx/EXOx)            */
    unsigned char it_old;     /* IT: pre-2.00 instruments                */
    unsigned int  version;    /* XM version, IT cwt                      */
    unsigned int  orders;
    unsigned int  patterns;   /* pattern table entries                   */
    unsigned int  instruments;
    unsigned int  samples;
    unsigned int  zero_rows;
    unsigned long midi_pos;   /* IT MIDI macros in the file, 0 = none    */
    unsigned long ins_tab;
    unsigned long smp_tab;
    unsigned long midi;
    unsigned long zero;
    unsigned long pat_data;
    unsigned long smp_data;
    unsigned long end;
    unsigned long hw[MUS_HDR_WORDS];  /* header, filled in by Prepare    */
    unsigned long chan[MUS_MAX_CH];   /* channel table                   */
    unsigned char ord[MUS_MAX_ORD];
} MUS_STATE;

static MUS_STATE     g_mus;
static MUS_PAT       g_mus_pat[MUS_MAX_PAT];
static MUS_SMP       g_mus_smp[MUS_MAX_SMP];
static unsigned long g_mus_ins_pos[MUS_MAX_INS+1u];
static unsigned int  g_mus_ins_first[MUS_MAX_INS+1u];  /* XM: first sample */
static MUS_SMPHDR    g_mus_sh;
static MUS_INSHDR    g_mus_ih;

const char *g_mus_error=0;
unsigned char g_mus_cancelled=0u;         /* MUS_Load_Abort() ended the load */
unsigned long g_mus_chunks=0ul;
unsigned long g_mus_words=0ul;

/* =====================================================================
   Sequential file reader
   ===================================================================== */

/* The video stream buffers are idle while a module loads: the reader
   window borrows one of them, the chunk being built the other. */
#if (MUS_RD_SIZE>AVI_STREAM_BUF_SIZE)||((MUS_CHUNK_WORDS*4u)>AVI_STREAM_BUF_SIZE)
#error "MUSIC.h buffers do not fit the AVI stream buffers"
#endif

static unsigned char * const mus_rd=avi_stream_buffer_b;
static unsigned long mus_rd_base=0ul;     /* file offset of mus_rd[0]    */
static unsigned int  mus_rd_fill=0u;      /* valid bytes in mus_rd       */
static unsigned long mus_pos=0ul;
static unsigned char mus_eof=0u;          /* a read went past the end    */

void MUS_Seek(unsigned long pos)
{
    mus_pos=pos;
}

unsigned long MUS_Tell(void)
{
    return mus_pos;
}

unsigned char MUS_Get(void)
{
    unsigned long take;

    if((mus_pos<mus_rd_base)||(mus_pos>=(mus_rd_base+(unsigned long)mus_rd_fill)))
    {
        if(mus_pos>=File_Size)
        {
            mus_eof=1u;
            mus_pos++;
            return 0u;
        }

        mus_rd_base=mus_pos&~511ul;
        take=File_Size-mus_rd_base;
        if(take>(unsigned long)MUS_RD_SIZE)
        {
            take=(unsigned long)MUS_RD_SIZE;
        }

        if(FS_Read_CurrentFile_Bytes(mus_rd_base,mus_rd,(unsigned int)take)==0u)
        {
            mus_rd_fill=0u;
            mus_eof=1u;
            mus_pos++;
            return 0u;
        }
        mus_rd_fill=(unsigned int)take;
    }

    return mus_rd[(unsigned int)((mus_pos++)-mus_rd_base)];
}

void MUS_Skip(unsigned long n)
{
    mus_pos+=n;
}

unsigned int MUS_U16(void)
{
    unsigned int v;

    v=MUS_Get();
    return v|((unsigned int)MUS_Get()<<8);
}

unsigned int MUS_U16BE(void)
{
    unsigned int v;

    v=(unsigned int)MUS_Get()<<8;
    return v|MUS_Get();
}

unsigned long MUS_U32(void)
{
    unsigned long v;

    v=MUS_U16();
    return v|((unsigned long)MUS_U16()<<16);
}

unsigned char MUS_U8_At(unsigned long pos)
{
    MUS_Seek(pos);
    return MUS_Get();
}

unsigned int MUS_U16_At(unsigned long pos)
{
    MUS_Seek(pos);
    return MUS_U16();
}

unsigned int MUS_U16BE_At(unsigned long pos)
{
    MUS_Seek(pos);
    return MUS_U16BE();
}

unsigned long MUS_U32_At(unsigned long pos)
{
    MUS_Seek(pos);
    return MUS_U32();
}

unsigned char MUS_Match_At(unsigned long pos,const char *s,unsigned int n)
{
    unsigned int i;

    if((pos+(unsigned long)n)>File_Size)
    {
        return 0u;
    }

    MUS_Seek(pos);
    for(i=0u;i<n;i++)
    {
        if(MUS_Get()!=(unsigned char)s[i])
        {
            return 0u;
        }
    }
    return 1u;
}

/* Reads a name field.  A NUL ends it, except in IT where NULs inside a
   name read as spaces. */
void MUS_Read_Name(char *dst,unsigned int n,unsigned char nul_is_space)
{
    unsigned int i;
    unsigned char c;
    unsigned char ended;

    ended=0u;
    for(i=0u;i<n;i++)
    {
        c=MUS_Get();
        if((c==0u)&&(nul_is_space==0u))
        {
            ended=1u;
        }
        dst[i]=(ended!=0u) ? 0 : (char)((c==0u) ? ' ' : c);
    }
}

/* Packs a name (up to 64 bytes) into big-endian words: control bytes
   become spaces and trailing spaces are dropped. */
void MUS_Pack_Name(unsigned long *dst,const char *s,unsigned int n,unsigned int words)
{
    unsigned char b[64];
    unsigned int i;
    unsigned int k;

    for(i=0u;i<64u;i++)
    {
        b[i]=(i<n) ? (unsigned char)s[i] : 0u;
        if((b[i]!=0u)&&(b[i]<32u))
        {
            b[i]=' ';
        }
    }

    k=(n<64u) ? n : 64u;
    while((k>0u)&&((b[k-1u]==' ')||(b[k-1u]==0u)))
    {
        k--;
        b[k]=0u;
    }

    for(i=0u;i<words;i++)
    {
        dst[i]=((unsigned long)b[i*4u]<<24)|((unsigned long)b[i*4u+1u]<<16)|
               ((unsigned long)b[i*4u+2u]<<8)|(unsigned long)b[i*4u+3u];
    }
}

/* =====================================================================
   Chunk writer
   ===================================================================== */

static unsigned char * const mus_out=avi_stream_buffer_a;
static unsigned int  mus_n=0u;            /* words in mus_out            */
static unsigned long mus_sum=0ul;
static unsigned long mus_addr=0ul;        /* SDRAM address of mus_out[0] */
static char          mus_type[2];
static unsigned char mus_fail=0u;

#define MUS_DREQ_TIMEOUT 12000000ul

/* Defined by the player (newmain.c).  Called often during a load to catch
   key presses; a nonzero return ends the load at the next chunk boundary. */
unsigned char MUS_Load_Abort(void);

/* AVI_Send_Buffer_Polled without its abort check: a chunk goes out whole.
   DREQ may stay low for MUS_DREQ_TIMEOUT polls over the whole buffer, not
   per byte: a top that only trickles the data in (a video bitstream) fails
   the load instead of holding it for minutes. */
unsigned char MUS_Send_Bytes(const unsigned char *src,unsigned int len)
{
    volatile unsigned char dummy;
    unsigned long budget;
    unsigned int i;

    budget=MUS_DREQ_TIMEOUT;
    for(i=0u;i<len;i++)
    {
        if((i&0xFFu)==0u)
        {
            (void)MUS_Load_Abort();
        }
        while(DREQ==0)
        {
            if(budget==0ul)
            {
                return 0u;
            }
            budget--;
            if((budget&0xFFFFul)==0ul)
            {
                (void)MUS_Load_Abort();
            }
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
    return 1u;
}

unsigned char MUS_Send_Chunk(const char *type,unsigned long addr,
                             const unsigned char *payload,unsigned int words,
                             unsigned long sum)
{
    unsigned char h[16];

    h[0]=0xA5u;
    h[1]=0x3Du;
    h[2]=(unsigned char)type[0];
    h[3]=(unsigned char)type[1];
    h[4]=(unsigned char)(addr>>24);
    h[5]=(unsigned char)(addr>>16);
    h[6]=(unsigned char)(addr>>8);
    h[7]=(unsigned char)addr;
    h[8]=(unsigned char)((unsigned long)words>>24);
    h[9]=(unsigned char)((unsigned long)words>>16);
    h[10]=(unsigned char)(words>>8);
    h[11]=(unsigned char)words;
    h[12]=(unsigned char)(sum>>24);
    h[13]=(unsigned char)(sum>>16);
    h[14]=(unsigned char)(sum>>8);
    h[15]=(unsigned char)sum;

    if(MUS_Send_Bytes(h,16u)==0u)
    {
        return 0u;
    }
    if((words!=0u)&&(MUS_Send_Bytes(payload,words*4u)==0u))
    {
        return 0u;
    }

    g_mus_chunks++;
    g_mus_words+=(unsigned long)words;
    return 1u;
}

/* Sends what has been collected.  A load is only ever stopped here,
   between chunks, where the loader is back at a chunk boundary. */
void MUS_Flush(void)
{
    if((mus_fail!=0u)||(mus_n==0u))
    {
        return;
    }

    if(MUS_Send_Chunk(mus_type,mus_addr,mus_out,mus_n,mus_sum)==0u)
    {
        mus_fail=1u;
        g_mus_error="FPGA NOT RESPONDING";
        return;
    }

    mus_addr+=(unsigned long)mus_n;
    mus_n=0u;
    mus_sum=0ul;

    if(MUS_Load_Abort()!=0u)
    {
        mus_fail=1u;
        g_mus_cancelled=1u;
        g_mus_error="CANCELLED";
    }
}

void MUS_Begin(const char *type,unsigned long addr)
{
    mus_type[0]=type[0];
    mus_type[1]=type[1];
    mus_addr=addr;
    mus_n=0u;
    mus_sum=0ul;
}

void MUS_Put(unsigned long w)
{
    unsigned char *p;

    if(mus_fail!=0u)
    {
        return;
    }

    p=&mus_out[mus_n*4u];
    p[0]=(unsigned char)(w>>24);
    p[1]=(unsigned char)(w>>16);
    p[2]=(unsigned char)(w>>8);
    p[3]=(unsigned char)w;
    mus_sum+=w;
    mus_n++;

    if(mus_n==MUS_CHUNK_WORDS)
    {
        MUS_Flush();
    }
}

void MUS_Put_Zeros(unsigned long n)
{
    while((n!=0ul)&&(mus_fail==0u))
    {
        MUS_Put(0ul);
        n--;
    }
}

void MUS_Put_Name(const char *s,unsigned int n,unsigned int words)
{
    unsigned long w[8];
    unsigned int i;

    MUS_Pack_Name(w,s,n,words);
    for(i=0u;i<words;i++)
    {
        MUS_Put(w[i]);
    }
}

/* =====================================================================
   Pattern cells
   ===================================================================== */

typedef struct
{
    unsigned char note;
    unsigned char ins;
    unsigned char vol;
    unsigned char has_vol;
    unsigned char fx;
    unsigned char param;
} MUS_CELL;

static MUS_CELL mus_row[MUS_MAX_CH];
static MUS_CELL mus_dummy;

void MUS_Row_Clear(void)
{
    memset(mus_row,0,sizeof mus_row);
}

MUS_CELL *MUS_Cell(unsigned int c)
{
    return (c<(unsigned int)g_mus.ch) ? &mus_row[c] : &mus_dummy;
}

void MUS_Row_Put(void)
{
    unsigned int c;
    const MUS_CELL *m;

    for(c=0u;c<(unsigned int)g_mus.ch;c++)
    {
        m=&mus_row[c];
        MUS_Put(((unsigned long)m->note<<24)|((unsigned long)m->ins<<16)|
                ((unsigned long)m->vol<<8)|(unsigned long)m->has_vol);
        MUS_Put(((unsigned long)m->fx<<24)|((unsigned long)m->param<<16));
    }
}

/* ProTracker periods, seven octaves from 2*1712.  Index 0 is note 25, so
   period 428 lands on note 61, middle C. */
static const unsigned int mus_pt_periods[84]=
{
    3424,3232,3048,2880,2712,2560,2416,2280,2152,2032,1920,1812,
    1712,1616,1524,1440,1356,1280,1208,1140,1076,1016, 960, 907,
     856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
     428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
     214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
     107, 101,  95,  90,  85,  80,  75,  71,  67,  63,  60,  56,
      53,  50,  47,  45,  42,  40,  37,  35,  33,  31,  30,  28
};

/* Nearest table period, the way OpenMPT reads MOD notes. */
unsigned char MUS_Period_To_Note(unsigned int period)
{
    unsigned int i;

    if((period==0u)||(period==0xFFFu))
    {
        return 0u;
    }

    for(i=0u;i<84u;i++)
    {
        if(period>=mus_pt_periods[i])
        {
            if((i!=0u)&&(period!=mus_pt_periods[i])&&
               ((mus_pt_periods[i-1u]-period)<(period-mus_pt_periods[i])))
            {
                return (unsigned char)(i+24u);
            }
            return (unsigned char)(i+25u);
        }
    }

    return 108u;
}

/* =====================================================================
   Sample data: any source encoding to signed 16-bit frames
   ===================================================================== */

static struct
{
    unsigned int  enc;
    unsigned int  acc;            /* delta accumulator                   */
    signed char   adpcm[16];
    unsigned char nib;            /* pending high nibble (ADPCM)         */
    unsigned char have_nib;
    /* IT214 / IT215 */
    unsigned long blk_end;        /* file offset after the block         */
    unsigned long bits;
    unsigned int  nbits;
    unsigned char dead;           /* block ran out or was damaged        */
} mus_src;

/* Two frames to a word, the even frame in bits 15:0. */
static unsigned int  mus_pair=0u;
static unsigned char mus_pair_have=0u;

void MUS_Frame(unsigned int f)
{
    f&=0xFFFFu;

    if(mus_pair_have==0u)
    {
        mus_pair=f;
        mus_pair_have=1u;
    }
    else
    {
        MUS_Put(((unsigned long)f<<16)|(unsigned long)mus_pair);
        mus_pair_have=0u;
    }
}

void MUS_Frame_Flush(void)
{
    if(mus_pair_have!=0u)
    {
        MUS_Put((unsigned long)mus_pair);
        mus_pair_have=0u;
    }
}

void MUS_PCM_Begin(unsigned int enc)
{
    unsigned int i;

    mus_src.enc=enc;
    mus_src.acc=0u;
    mus_src.have_nib=0u;

    if((enc&MUS_ENC_ADPCM)!=0u)
    {
        for(i=0u;i<16u;i++)
        {
            mus_src.adpcm[i]=(signed char)MUS_Get();
        }
    }
}

/* Next frame of a PCM or ADPCM channel.  Delta coding replaces the
   signed/unsigned choice, as in OpenMPT. */
unsigned int MUS_PCM_Next(void)
{
    unsigned int enc;
    unsigned int v;

    enc=mus_src.enc;

    if((enc&MUS_ENC_ADPCM)!=0u)
    {
        if(mus_src.have_nib==0u)
        {
            v=MUS_Get();
            mus_src.nib=(unsigned char)(v>>4);
            mus_src.have_nib=1u;
            v&=0x0Fu;
        }
        else
        {
            v=mus_src.nib;
            mus_src.have_nib=0u;
        }
        mus_src.acc=(mus_src.acc+(unsigned int)(int)mus_src.adpcm[v])&0xFFu;
        return mus_src.acc<<8;
    }

    if((enc&MUS_ENC_PTM16)!=0u)
    {
        mus_src.acc=(mus_src.acc+MUS_Get())&0xFFu;
        v=mus_src.acc;
        mus_src.acc=(mus_src.acc+MUS_Get())&0xFFu;
        return v|(mus_src.acc<<8);
    }

    if((enc&MUS_ENC_16)!=0u)
    {
        v=((enc&MUS_ENC_BE)!=0u) ? MUS_U16BE() : MUS_U16();
        if((enc&MUS_ENC_DELTA)!=0u)
        {
            mus_src.acc=(mus_src.acc+v)&0xFFFFu;
            return mus_src.acc;
        }
        return ((enc&MUS_ENC_UNSIGNED)!=0u) ? (v^0x8000u) : v;
    }

    v=MUS_Get();
    if((enc&MUS_ENC_DELTA)!=0u)
    {
        mus_src.acc=(mus_src.acc+v)&0xFFu;
        return mus_src.acc<<8;
    }
    return (((enc&MUS_ENC_UNSIGNED)!=0u) ? (v^0x80u) : v)<<8;
}

/* IT214/215 bit reader: LSB first, never past the current block. */
unsigned long MUS_IT_Bits(unsigned int n)
{
    unsigned long v;

    while(mus_src.nbits<n)
    {
        if(MUS_Tell()>=mus_src.blk_end)
        {
            mus_src.dead=1u;
            return 0ul;
        }
        mus_src.bits|=(unsigned long)MUS_Get()<<mus_src.nbits;
        mus_src.nbits+=8u;
    }

    v=mus_src.bits&((1ul<<n)-1ul);
    mus_src.bits>>=n;
    mus_src.nbits-=n;
    return v;
}

/* One channel of an IT214/IT215 sample, after OpenMPT's ITDecompression:
   a damaged block ends early and the next block continues from the frame
   where it stopped.  Frames never decoded read as silence. */
void MUS_IT_Unpack(unsigned long frames,unsigned int enc)
{
    unsigned long done;
    unsigned long blk;
    unsigned long csize;
    unsigned long v;
    unsigned long top;
    unsigned long mem1;
    unsigned long mem2;
    unsigned int  is16;
    unsigned int  defw;
    unsigned int  fetch;
    unsigned int  lower;          /* -lowerB: 4 or 8 */
    unsigned int  upper;          /*  upperB: 3 or 7 */
    unsigned int  w;
    unsigned int  nw;

    is16=((enc&MUS_ENC_16)!=0u) ? 1u : 0u;
    defw=(is16!=0u) ? 17u : 9u;
    fetch=(is16!=0u) ? 4u : 3u;
    lower=(is16!=0u) ? 8u : 4u;
    upper=(is16!=0u) ? 7u : 3u;
    done=0ul;

    while((done<frames)&&(mus_fail==0u)&&((MUS_Tell()+2ul)<=File_Size))
    {
        csize=MUS_U16();
        if(csize==0ul)
        {
            continue;
        }

        mus_src.blk_end=MUS_Tell()+csize;
        if(mus_src.blk_end>File_Size)
        {
            mus_src.blk_end=File_Size;
        }
        mus_src.bits=0ul;
        mus_src.nbits=0u;
        mus_src.dead=0u;
        mem1=0ul;
        mem2=0ul;
        w=defw;

        blk=(is16!=0u) ? 0x4000ul : 0x8000ul;
        if(blk>(frames-done))
        {
            blk=frames-done;
        }

        while((blk!=0ul)&&(w<=defw))
        {
            v=MUS_IT_Bits(w);
            if(mus_src.dead!=0u)
            {
                break;
            }
            top=1ul<<(w-1u);

            if(w<=6u)
            {
                if(v==top)                              /* mode A */
                {
                    nw=(unsigned int)MUS_IT_Bits(fetch)+1u;
                    if(mus_src.dead!=0u)
                    {
                        break;
                    }
                    w=(nw>=w) ? (nw+1u) : nw;
                    continue;
                }
            }
            else if(w<defw)
            {
                if(((v+lower)>=top)&&(v<=(top+upper)))    /* mode B */
                {
                    nw=(unsigned int)(v+lower-top)+1u;
                    w=(nw>=w) ? (nw+1u) : nw;
                    continue;
                }
            }
            else
            {
                if((v&top)!=0ul)                        /* mode C */
                {
                    w=(unsigned int)(v&~top)+1u;
                    continue;
                }
                top=0ul;
            }

            if((v&top)!=0ul)
            {
                v-=top<<1;                              /* sign extend */
            }
            mem1+=v;
            mem2+=mem1;
            v=((enc&MUS_ENC_IT215)!=0u) ? mem2 : mem1;

            MUS_Frame((is16!=0u) ? (unsigned int)(v&0xFFFFul) : (unsigned int)((v&0xFFul)<<8));
            done++;
            blk--;
        }

        MUS_Seek(mus_src.blk_end);
    }

    while((done<frames)&&(mus_fail==0u))
    {
        MUS_Frame(0u);
        done++;
    }
}

/* SDRAM words for one channel: data, then MUS_SMP_PAD zero words,
   rounded up to 4 words. */
unsigned long MUS_Sample_Words(unsigned long frames)
{
    return ((((frames+1ul)>>1)+MUS_SMP_PAD)+3ul)&~3ul;
}

/* One channel of sample data as SD chunks.  Bytes missing from a
   truncated file read as silence. */
void MUS_Emit_Channel(unsigned long addr,unsigned long frames,unsigned int enc)
{
    unsigned long i;

    MUS_Begin("SD",addr);
    mus_pair_have=0u;

    if((enc&(MUS_ENC_IT214|MUS_ENC_IT215))!=0u)
    {
        MUS_IT_Unpack(frames,enc);
    }
    else
    {
        MUS_PCM_Begin(enc);
        for(i=0ul;(i<frames)&&(mus_fail==0u);i++)
        {
            MUS_Frame(MUS_PCM_Next());
        }
    }

    MUS_Frame_Flush();
    MUS_Put_Zeros(MUS_Sample_Words(frames)-((frames+1ul)>>1));
    MUS_Flush();
}

/* Reserves SDRAM for a sample's data (Prepare).  g_mus.end never passes
   MUS_SDRAM_END: a sample that does not fit fails the whole load. */
void MUS_Alloc_Sample(unsigned int index,const MUS_SMPHDR *s)
{
    MUS_SMP *m;
    unsigned long words;

    m=&g_mus_smp[index];
    m->addr=0ul;
    m->addr_r=0ul;

    if((s->flags&MUS_SF_DATA)==0u)
    {
        return;
    }
    if(s->length>MUS_MAX_FRAMES)
    {
        g_mus_error="TOO LARGE FOR SDRAM";
        return;
    }

    words=MUS_Sample_Words(s->length);
    if((MUS_SDRAM_END-g_mus.end)<(((s->flags&MUS_SF_STEREO)!=0u) ? (words<<1) : words))
    {
        g_mus_error="TOO LARGE FOR SDRAM";
        return;
    }

    m->addr=g_mus.end;
    g_mus.end+=words;

    if((s->flags&MUS_SF_STEREO)!=0u)
    {
        m->addr_r=g_mus.end;
        g_mus.end+=words;
    }
}

/* Sample record (32 words), then the data. */
void MUS_Emit_Sample(unsigned int index,const MUS_SMPHDR *s)
{
    const MUS_SMP *m;
    unsigned long w[8];
    unsigned long bytes;
    unsigned int  enc;

    m=&g_mus_smp[index];

    MUS_Begin("SM",g_mus.smp_tab+(unsigned long)index*MUS_SMP_WORDS);
    MUS_Put(m->addr);
    MUS_Put(((s->flags&MUS_SF_DATA)!=0u) ? s->length : 0ul);
    MUS_Put(s->loop_start);
    MUS_Put(s->loop_end);
    MUS_Put(s->sus_start);
    MUS_Put(s->sus_end);
    MUS_Put((unsigned long)s->flags);
    MUS_Put(s->c5speed);
    MUS_Put((unsigned long)s->volume|((unsigned long)s->gvol<<8)|((unsigned long)s->pan<<16));
    MUS_Put((unsigned long)(unsigned char)s->finetune|((unsigned long)(unsigned char)s->relnote<<8));
    MUS_Put((unsigned long)s->vib[0]|((unsigned long)s->vib[1]<<8)|
            ((unsigned long)s->vib[2]<<16)|((unsigned long)s->vib[3]<<24));
    MUS_Put(m->addr_r);
    MUS_Put_Zeros(4ul);
    MUS_Pack_Name(w,s->name,32u,8u);
    MUS_Put(w[0]); MUS_Put(w[1]); MUS_Put(w[2]); MUS_Put(w[3]);
    MUS_Put(w[4]); MUS_Put(w[5]); MUS_Put(w[6]); MUS_Put(w[7]);
    MUS_Put_Name(s->file,12u,3u);
    MUS_Put_Zeros(5ul);
    MUS_Flush();

    if((m->addr==0ul)||(mus_fail!=0u))
    {
        return;
    }

    enc=s->enc&(unsigned int)~MUS_ENC_STEREO;
    MUS_Seek(s->data);
    MUS_Emit_Channel(m->addr,s->length,enc);

    if(m->addr_r!=0ul)
    {
        /* Split stereo: the right channel follows the left.  The packed
           codecs have left the reader there already. */
        if((enc&(MUS_ENC_IT214|MUS_ENC_IT215))==0u)
        {
            bytes=s->length;
            if((enc&(MUS_ENC_16|MUS_ENC_PTM16))!=0u)
            {
                bytes<<=1;
            }
            MUS_Seek(s->data+bytes);
        }
        MUS_Emit_Channel(m->addr_r,s->length,enc);
    }
}

void MUS_Clear_SmpHdr(MUS_SMPHDR *s)
{
    memset(s,0,sizeof(MUS_SMPHDR));
    s->c5speed=8363ul;
    s->gvol=64u;
    s->pan=128u;
}

/* Loop ends clamped to the length; an empty loop is switched off. */
void MUS_Sanitize_Loops(MUS_SMPHDR *s)
{
    if(s->loop_end>s->length) s->loop_end=s->length;
    if(s->sus_end>s->length)  s->sus_end=s->length;

    if((s->loop_start>=s->loop_end)||((s->flags&MUS_SF_LOOP)==0u))
    {
        s->loop_start=0ul;
        s->loop_end=0ul;
        s->flags&=(unsigned int)~(MUS_SF_LOOP|MUS_SF_BIDI);
    }
    if((s->sus_start>=s->sus_end)||((s->flags&MUS_SF_SUS)==0u))
    {
        s->sus_start=0ul;
        s->sus_end=0ul;
        s->flags&=(unsigned int)~(MUS_SF_SUS|MUS_SF_SUSBIDI);
    }
}

/* Sets the data flag once length, encoding and offset are known.  A
   sample running past the end of the file is cut to the frames the file
   holds, as libxmp does; packed data is bounded at 8 frames a byte. */
void MUS_Finish_SmpHdr(MUS_SMPHDR *s,unsigned char present)
{
    unsigned long avail;
    unsigned long max;
    unsigned long unit;

    if((present!=0u)&&(s->length!=0ul)&&(s->data>=File_Size))
    {
        s->flags|=MUS_SF_NODATA;
        present=0u;
    }

    if((present!=0u)&&(s->length!=0ul))
    {
        avail=File_Size-s->data;
        if((s->enc&(MUS_ENC_IT214|MUS_ENC_IT215))!=0u)
        {
            max=(avail>(MUS_MAX_FRAMES>>3)) ? (MUS_MAX_FRAMES+1ul) : (avail<<3);
        }
        else if((s->enc&MUS_ENC_ADPCM)!=0u)
        {
            max=(avail<=16ul) ? 0ul : ((avail>MUS_MAX_FRAMES) ? (MUS_MAX_FRAMES+1ul) : ((avail-16ul)<<1));
        }
        else
        {
            unit=((s->enc&(MUS_ENC_16|MUS_ENC_PTM16))!=0u) ? 2ul : 1ul;
            if((s->enc&MUS_ENC_STEREO)!=0u) unit<<=1;
            max=avail/unit;
        }
        if(s->length>max)
        {
            s->length=max;
        }
    }

    if((present!=0u)&&(s->length!=0ul))
    {
        s->flags|=MUS_SF_DATA;
    }
    else
    {
        s->length=0ul;
        s->flags&=(unsigned int)~MUS_SF_STEREO;
    }
    MUS_Sanitize_Loops(s);
}

/* =====================================================================
   Instruments
   ===================================================================== */

void MUS_Clear_InsHdr(MUS_INSHDR *h)
{
    unsigned int i;

    memset(h,0,sizeof(MUS_INSHDR));
    h->gvol=128u;
    h->pan=128u;
    h->ppc=61u;
    for(i=0u;i<120u;i++)
    {
        h->key_note[i]=(unsigned char)(i+1u);
    }
}

/* Ticks saved with only their low byte get the high byte back, the way
   OpenMPT repairs them. */
void MUS_Env_Fix_Ticks(MUS_ENV *e)
{
    unsigned int i;

    for(i=1u;i<e->points;i++)
    {
        if((e->tick[i]<e->tick[i-1u])&&((e->tick[i]&0xFF00u)==0u))
        {
            e->tick[i]|=e->tick[i-1u]&0xFF00u;
            if(e->tick[i]<e->tick[i-1u])
            {
                e->tick[i]+=0x100u;
            }
        }
    }
}

void MUS_Env_Put(const MUS_ENV *e)
{
    unsigned int i;

    MUS_Put((unsigned long)e->points|((unsigned long)e->loop_start<<8)|
            ((unsigned long)e->loop_end<<16)|((unsigned long)e->flags<<24));
    MUS_Put((unsigned long)e->sus_start|((unsigned long)e->sus_end<<8));

    for(i=0u;i<25u;i++)
    {
        MUS_Put((i<e->points) ? ((unsigned long)(e->tick[i]&0xFFFFu)|
                                 ((unsigned long)((unsigned int)e->value[i]&0xFFFFu)<<16)) : 0ul);
    }
}

void MUS_Emit_Instrument(unsigned int index,const MUS_INSHDR *h)
{
    unsigned int i;

    MUS_Begin("IN",g_mus.ins_tab+(unsigned long)index*MUS_INS_WORDS);
    MUS_Put((unsigned long)h->nna|((unsigned long)h->dct<<8)|((unsigned long)h->dca<<16)|
            ((unsigned long)h->flags<<24));
    MUS_Put(h->fadeout);
    MUS_Put((unsigned long)h->gvol|((unsigned long)h->pan<<8)|
            ((unsigned long)(unsigned char)h->pps<<16)|((unsigned long)h->ppc<<24));
    MUS_Put((unsigned long)h->rv|((unsigned long)h->rp<<8)|((unsigned long)h->ifc<<16)|
            ((unsigned long)h->ifr<<24));
    MUS_Put((unsigned long)h->vib[0]|((unsigned long)h->vib[1]<<8)|
            ((unsigned long)h->vib[2]<<16)|((unsigned long)h->vib[3]<<24));
    MUS_Put((unsigned long)h->midi_ch|((unsigned long)h->midi_prog<<8)|
            ((unsigned long)h->midi_bank<<16));
    MUS_Put((unsigned long)h->nsmp);
    MUS_Put(0ul);

    MUS_Env_Put(&h->env[0]);
    MUS_Env_Put(&h->env[1]);
    MUS_Env_Put(&h->env[2]);
    MUS_Put_Zeros(7ul);

    for(i=0u;i<120u;i++)
    {
        MUS_Put((unsigned long)h->key_smp[i]|((unsigned long)h->key_note[i]<<16));
    }

    MUS_Put_Name(h->name,32u,8u);
    MUS_Put_Zeros(32ul);
    MUS_Flush();
}

/* =====================================================================
   Layout (Prepare): tables, zero block and pattern data get their SDRAM
   addresses here; sample data is allocated after, one sample at a time.
   ===================================================================== */

/* Extends the pattern table to every pattern the order list names; the
   ones not in the file play as empty 64-row patterns.  Order values from
   first_marker up are markers (S3M/IT FE skip, FF end; MOD 80-FF), not
   patterns. */
void MUS_Extend_Patterns(unsigned int first_marker)
{
    unsigned int i;
    unsigned int n;

    n=g_mus.patterns;
    for(i=0u;i<g_mus.orders;i++)
    {
        if(((unsigned int)g_mus.ord[i]<first_marker)&&(((unsigned int)g_mus.ord[i]+1u)>n))
        {
            n=(unsigned int)g_mus.ord[i]+1u;
        }
    }
    if(n>MUS_MAX_PAT)
    {
        n=MUS_MAX_PAT;
    }
    for(i=g_mus.patterns;i<n;i++)
    {
        g_mus_pat[i].kind=MUS_PAT_MISSING;
        g_mus_pat[i].rows=64u;
    }
    g_mus.patterns=n;
}

unsigned char MUS_Layout(void)
{
    unsigned long a;
    unsigned long row_words;
    unsigned int p;

    row_words=(unsigned long)g_mus.ch*2ul;
    a=MUS_BASE+MUS_OFS_TABLES;

    g_mus.midi=0ul;
    if(g_mus.midi_pos!=0ul)
    {
        g_mus.midi=a;
        a+=MUS_MIDI_WORDS;
    }

    g_mus.ins_tab=0ul;
    if(g_mus.instruments!=0u)
    {
        g_mus.ins_tab=a;
        a+=(unsigned long)g_mus.instruments*MUS_INS_WORDS;
    }

    g_mus.smp_tab=a;
    a+=(unsigned long)g_mus.samples*MUS_SMP_WORDS;

    g_mus.zero_rows=0u;
    for(p=0u;p<g_mus.patterns;p++)
    {
        if((g_mus_pat[p].kind!=MUS_PAT_DATA)&&(g_mus_pat[p].rows>g_mus.zero_rows))
        {
            g_mus.zero_rows=g_mus_pat[p].rows;
        }
    }

    g_mus.zero=0ul;
    if(g_mus.zero_rows!=0u)
    {
        g_mus.zero=a;
        a+=(unsigned long)g_mus.zero_rows*row_words;
    }

    g_mus.pat_data=a;
    for(p=0u;p<g_mus.patterns;p++)
    {
        if(g_mus_pat[p].kind==MUS_PAT_DATA)
        {
            g_mus_pat[p].addr=a;
            a+=(unsigned long)g_mus_pat[p].rows*row_words;
        }
        else
        {
            g_mus_pat[p].addr=g_mus.zero;
        }
        if(a>MUS_SDRAM_END)
        {
            g_mus_error="TOO LARGE FOR SDRAM";
            return 0u;
        }
    }

    a=(a+3ul)&~3ul;
    g_mus.smp_data=a;
    g_mus.end=a;
    return 1u;
}

/* =====================================================================
   MOD
   ===================================================================== */

static unsigned char mus_mod_nsmp;        /* 31, or 15 for Soundtracker */
static unsigned long mus_mod_pat_ofs;     /* first pattern in the file  */

/* Channels from the signature at 1080, 0 if it is not one we know. */
unsigned char MUS_MOD_Magic(const unsigned char *m,unsigned long *hf)
{
    unsigned char a;
    unsigned char b;

    a=m[0];
    b=m[1];

    if(!memcmp(m,"M.K.",4)||!memcmp(m,"M!K!",4)||!memcmp(m,"PATT",4)||
       !memcmp(m,"NSMS",4)||!memcmp(m,"LARD",4))
    {
        return 4u;
    }
    if(!memcmp(m,"M&K!",4)||!memcmp(m,"FEST",4)||!memcmp(m,"N.T.",4))
    {
        *hf|=MUS_HF_VBLANK;
        return 4u;
    }
    if(!memcmp(m,"OKTA",4)||!memcmp(m,"OCTA",4))
    {
        return 8u;
    }
    if(!memcmp(m,"CD61",4)||!memcmp(m,"CD81",4))
    {
        return (unsigned char)(m[2]-'0');
    }
    if(!memcmp(m,"FA0",3)&&(m[3]>='4')&&(m[3]<='8'))
    {
        mus_mod_pat_ofs=1088ul;           /* four extra bytes after magic */
        return (unsigned char)(m[3]-'0');
    }
    if((!memcmp(m,"FLT",3)||!memcmp(m,"EXO",3))&&((m[3]=='4')||(m[3]=='8')))
    {
        *hf|=MUS_HF_VBLANK;
        g_mus.strek=1u;
        g_mus.flt8=(unsigned char)(m[3]=='8');
        return (unsigned char)(m[3]-'0');
    }
    if((a>='1')&&(a<='9')&&!memcmp(m+1,"CHN",3))
    {
        return (unsigned char)(a-'0');
    }
    if((a>='1')&&(a<='9')&&(b>='0')&&(b<='9')&&(!memcmp(m+2,"CH",2)||!memcmp(m+2,"CN",2)))
    {
        return (unsigned char)((a-'0')*10u+(b-'0'));
    }
    if(!memcmp(m,"TDZ",3)&&(m[3]>='1')&&(m[3]<='9'))
    {
        return (unsigned char)(m[3]-'0');
    }
    return 0u;
}

/* Control and high-bit bytes in a name field, OpenMPT's CountInvalidChars. */
unsigned int MUS_Bad_Chars(unsigned long pos,unsigned int n)
{
    unsigned int i;
    unsigned int bad;
    unsigned char c;

    bad=0u;
    MUS_Seek(pos);
    for(i=0u;i<n;i++)
    {
        c=MUS_Get();
        if((c!=0u)&&((c<32u)||(c>=128u))) bad++;
    }
    return bad;
}

/* A 15-sample Soundtracker module has no signature to find it by, so it
   is accepted on OpenMPT's header and first-pattern checks (Load_stk). */
unsigned char MUS_MOD_Is_ST15(void)
{
    unsigned int  i;
    unsigned int  r;
    unsigned int  bad;
    unsigned int  title_bad;
    unsigned int  len;
    unsigned int  maxpat;
    unsigned int  period;
    unsigned int  k;
    unsigned char vols;
    unsigned char b0;
    unsigned char b1;
    unsigned long total;
    unsigned long need;

    if(File_Size<(600ul+1024ul))
    {
        return 0u;
    }

    title_bad=MUS_Bad_Chars(0ul,20u);
    bad=title_bad;
    total=0ul;
    vols=0u;
    for(i=0u;i<15u;i++)
    {
        bad+=MUS_Bad_Chars(20ul+i*30ul,22u);
        len=MUS_U16BE_At(20ul+i*30ul+22ul);
        if(MUS_U8_At(20ul+i*30ul+24ul)!=0u) bad+=16u;
        if((MUS_U8_At(20ul+i*30ul+25ul)>64u)||(len>37000u)||(bad>48u))
        {
            return 0u;
        }
        total+=(unsigned long)len*2ul;
        vols|=MUS_U8_At(20ul+i*30ul+25ul);
    }
    if((title_bad>5u)&&(bad>(title_bad+8u)))
    {
        return 0u;
    }
    if((total==0ul)||(vols==0u))
    {
        return 0u;
    }

    len=MUS_U8_At(470ul);
    if((len>128u)||(MUS_Get()>220u))
    {
        return 0u;
    }
    maxpat=0u;
    for(i=0u;i<128u;i++)
    {
        k=MUS_Get();
        if(k>maxpat) maxpat=k;
    }
    if((maxpat>63u)||((len==0u)&&(maxpat==0u)&&(MUS_U8_At(471ul)==0u)))
    {
        return 0u;
    }

    /* First pattern: high instrument bits and periods off the table count
       as malformed; more than 16 and this is not a Soundtracker module. */
    bad=0u;
    MUS_Seek(600ul);
    for(r=0u;r<256u;r++)
    {
        b0=MUS_Get();
        b1=MUS_Get();
        (void)MUS_Get();
        (void)MUS_Get();
        if((b0&0xF0u)!=0u) bad++;
        period=((unsigned int)(b0&0x0Fu)<<8)|b1;
        if((period!=0u)&&(period!=0xFFFu))
        {
            for(k=24u;k<60u;k++)
            {
                if((period+1u>=mus_pt_periods[k])&&(period<=mus_pt_periods[k]+1u)) break;
            }
            if(k==60u) bad+=2u;
        }
    }
    if(bad>16u)
    {
        return 0u;
    }

    need=((unsigned long)maxpat+1ul)*1024ul+total;
    need=(need>65536ul) ? (need-65536ul) : 0ul;
    return ((File_Size-600ul)>=need) ? 1u : 0u;
}

void MUS_MOD_Sample(unsigned int i,MUS_SMPHDR *s)
{
    unsigned long len;
    unsigned long ls;
    unsigned long ll;
    unsigned long le;
    unsigned char ft;

    MUS_Clear_SmpHdr(s);
    MUS_Seek(g_mus_smp[i].hdr);
    MUS_Read_Name(s->name,22u,0u);
    len=(unsigned long)MUS_U16BE()*2ul;
    ft=MUS_Get();
    s->volume=MUS_Get();
    ls=(unsigned long)MUS_U16BE();
    ll=(unsigned long)MUS_U16BE()*2ul;
    s->data=g_mus_smp[i].data;

    if(s->volume>64u) s->volume=64u;

    if(mus_mod_nsmp==15u)
    {
        /* Soundtracker: loop start in bytes, no finetune, and a looped
           sample plays from its loop start - the data before it is never
           heard (OpenMPT Load_stk, libxmp st_load). */
        if((ll>2ul)&&(ls<len))
        {
            le=ls+ll;
            if(le>len) le=len;
            s->data+=ls;
            len-=ls;
            s->loop_start=0ul;
            s->loop_end=le-ls;
            s->flags|=MUS_SF_LOOP;
        }
    }
    else
    {
        /* Loop rules from OpenMPT's MODSampleHeader::ConvertToMPT */
        s->finetune=(signed char)((int)((ft&0x0Fu)^0x08u)-8);
        ls<<=1;
        if((ll>2ul)&&((ls+ll)>len)&&(((ls>>1)+ll)<=len))
        {
            ls>>=1;                       /* start given in bytes */
        }
        if(len==2ul)
        {
            len=0ul;
        }
        if(len!=0ul)
        {
            le=ls+ll;
            if(ls>=len) ls=len-1ul;
            if((ls>le)||(le<4ul)||((le-ls)<4ul))
            {
                ls=0ul;
                le=0ul;
            }
            if((le<=8ul)&&(ls==0ul)&&(len>le)&&(g_mus.ch==4u))
            {
                le=0ul;
            }
            if(le>ls)
            {
                s->flags|=MUS_SF_LOOP;
                s->loop_start=ls;
                s->loop_end=le;
            }
        }
    }

    s->length=len;
    if(MUS_Match_At(s->data,"ADPCM",5u))
    {
        s->data+=5ul;
        s->enc=MUS_ENC_ADPCM;
        s->flags|=MUS_SF_PACKED;
    }
    MUS_Finish_SmpHdr(s,1u);
}

/* 64 rows; FLT8 keeps channels 5-8 in the odd pattern of each pair. */
void MUS_MOD_Pattern(unsigned int p)
{
    unsigned long row_bytes;
    unsigned long base;
    unsigned int  r;
    unsigned int  c;
    unsigned char b[4];
    MUS_CELL     *m;

    row_bytes=(unsigned long)g_mus.file_ch*4ul;
    MUS_Begin("PD",g_mus_pat[p].addr);

    for(r=0u;(r<64u)&&(mus_fail==0u);r++)
    {
        MUS_Row_Clear();
        for(c=0u;c<(unsigned int)g_mus.ch;c++)
        {
            if(g_mus.flt8!=0u)
            {
                base=g_mus_pat[p].pos+((c>=4u) ? 1024ul : 0ul)+(unsigned long)r*16ul+(unsigned long)(c&3u)*4ul;
            }
            else
            {
                base=g_mus_pat[p].pos+(unsigned long)r*row_bytes+(unsigned long)c*4ul;
            }

            MUS_Seek(base);
            b[0]=MUS_Get();
            b[1]=MUS_Get();
            b[2]=MUS_Get();
            b[3]=MUS_Get();

            m=MUS_Cell(c);
            m->note=MUS_Period_To_Note(((unsigned int)(b[0]&0x0Fu)<<8)|b[1]);
            m->ins=(unsigned char)((b[0]&0xF0u)|(b[2]>>4));
            m->fx=(unsigned char)(b[2]&0x0Fu);
            m->param=b[3];
            if(g_mus.strek!=0u)
            {
                /* Startrekker: no Exx (its macros are not supported), speed
                   capped at 31 - as OpenMPT loads it. */
                if(m->fx==0x0Eu) { m->fx=0u; m->param=0u; }
                if((m->fx==0x0Fu)&&(m->param>0x1Fu)) m->param=0x1Fu;
            }
        }
        MUS_Row_Put();
    }
    MUS_Flush();
}

unsigned char MUS_MOD_Prepare(void)
{
    unsigned char magic[4];
    unsigned long hf;
    unsigned long pos;
    unsigned long wow;
    unsigned int  len;
    unsigned int  pats;
    unsigned int  i;
    unsigned int  hdr_orders;
    unsigned char ch;
    unsigned char restart;
    unsigned char wow_ok;

    hf=MUS_HF_STEREO;
    mus_mod_nsmp=31u;
    mus_mod_pat_ofs=1084ul;
    ch=0u;

    if(File_Size>=1084ul)
    {
        MUS_Seek(1080ul);
        magic[0]=MUS_Get();
        magic[1]=MUS_Get();
        magic[2]=MUS_Get();
        magic[3]=MUS_Get();
        ch=MUS_MOD_Magic(magic,&hf);
    }

    if(ch==0u)
    {
        if(MUS_MOD_Is_ST15()==0u)
        {
            g_mus_error="NOT A MOD FILE";
            return 0u;
        }
        ch=4u;
        mus_mod_nsmp=15u;
        mus_mod_pat_ofs=600ul;
        magic[0]=magic[1]=magic[2]=magic[3]=0u;
        hf|=MUS_HF_ST15;
    }
    if(ch>MUS_MAX_CH)
    {
        g_mus_error="TOO MANY CHANNELS";
        return 0u;
    }

    hdr_orders=(mus_mod_nsmp==31u) ? 950u : 470u;
    len=MUS_U8_At(hdr_orders);
    restart=MUS_Get();

    pats=0u;
    for(i=0u;i<128u;i++)
    {
        g_mus.ord[i]=MUS_Get();
        if((g_mus.ord[i]<0x80u)&&(((unsigned int)g_mus.ord[i]+1u)>pats))
        {
            pats=(unsigned int)g_mus.ord[i]+1u;
        }
    }

    if(len>128u)
    {
        len=128u;
    }
    if(len==0u)
    {
        len=128u;
        while((len>1u)&&(g_mus.ord[len-1u]==0u)) len--;
    }
    g_mus.orders=len;

    /* Mod's Grave WOW: an M.K. file sized for eight channels. */
    if(!memcmp(magic,"M.K.",4)&&(restart==0u))
    {
        wow=0ul;
        wow_ok=1u;
        for(i=0u;i<31u;i++)
        {
            unsigned int sl;

            sl=MUS_U16BE_At(20ul+i*30ul+22ul);
            wow+=(unsigned long)sl*2ul;
            if(MUS_U8_At(20ul+i*30ul+24ul)!=0u) wow_ok=0u;
            if((sl!=0u)&&(MUS_U8_At(20ul+i*30ul+25ul)!=64u)) wow_ok=0u;
        }
        if((wow_ok!=0u)&&((1084ul+wow+(unsigned long)pats*2048ul)==(File_Size&~1ul)))
        {
            ch=8u;
        }
    }

    g_mus.fmt=MUS_FMT_MOD;
    g_mus.ch=ch;
    g_mus.file_ch=ch;

    if(g_mus.flt8!=0u)
    {
        /* FLT8: one logical pattern = two stored 4-channel patterns. */
        g_mus.file_ch=4u;
        pats=(pats+1u)/2u;
        for(i=0u;i<g_mus.orders;i++) { if(g_mus.ord[i]<0x80u) g_mus.ord[i]>>=1; }
    }

    for(i=0u;i<pats;i++)
    {
        g_mus_pat[i].kind=MUS_PAT_DATA;
        g_mus_pat[i].rows=64u;
        g_mus_pat[i].len=0ul;
        g_mus_pat[i].pos=mus_mod_pat_ofs+(unsigned long)i*((g_mus.flt8!=0u) ? 2048ul : (unsigned long)ch*256ul);
    }
    g_mus.patterns=pats;
    MUS_Extend_Patterns(0x80u);
    g_mus.samples=mus_mod_nsmp;
    g_mus.instruments=0u;

    if(MUS_Layout()==0u)
    {
        return 0u;
    }

    /* Sample data follows the patterns, one sample after another. */
    pos=mus_mod_pat_ofs+(unsigned long)pats*((g_mus.flt8!=0u) ? 2048ul : (unsigned long)ch*256ul);
    for(i=0u;i<mus_mod_nsmp;i++)
    {
        unsigned long words;

        g_mus_smp[i].hdr=20ul+(unsigned long)i*30ul;
        g_mus_smp[i].data=pos;
        words=MUS_U16BE_At(g_mus_smp[i].hdr+22ul);

        MUS_MOD_Sample(i,&g_mus_sh);
        MUS_Alloc_Sample(i,&g_mus_sh);

        if((g_mus_sh.enc&MUS_ENC_ADPCM)!=0u)
        {
            pos+=5ul+16ul+((words*2ul+1ul)>>1);
        }
        else
        {
            pos+=words*2ul;
        }
    }

    /* ProTracker mode: M.K./M!K! with every note inside the Amiga range */
    if(!memcmp(magic,"M.K.",4)||!memcmp(magic,"M!K!",4))
    {
        hf|=MUS_HF_PTMODE;
    }

    g_mus.hw[4]=(125ul<<16)|(6ul<<8)|(((restart<0x7Fu)&&(restart!=0x78u)&&(restart<g_mus.orders)) ? restart : 0u);
    g_mus.hw[5]=(64ul<<24);
    g_mus.hw[6]=hf;
    g_mus.hw[7]=0ul;
    g_mus.hw[8]=((unsigned long)magic[0]<<24)|((unsigned long)magic[1]<<16)|
                ((unsigned long)magic[2]<<8)|(unsigned long)magic[3];
    g_mus.hw[9]=(unsigned long)restart;
    g_mus.hw[20]=(unsigned long)(-36l);

    for(i=0u;i<ch;i++)
    {
        /* Amiga LRRL: channels 1 and 2 of every four on the right */
        g_mus.chan[i]=(((i&3u)==1u)||((i&3u)==2u)) ? 0xFFul : 0x00ul;
        g_mus.chan[i]|=64ul<<8;
    }

    {
        char name[20];

        MUS_Seek(0ul);
        MUS_Read_Name(name,20u,0u);
        MUS_Pack_Name(&g_mus.hw[48],name,20u,8u);
        MUS_Pack_Name(&g_mus.hw[56],(mus_mod_nsmp==15u) ? "Soundtracker" : (const char *)magic,
                      (mus_mod_nsmp==15u) ? 12u : 4u,8u);
    }

    return 1u;
}

void MUS_MOD_Emit(void)
{
    unsigned int i;

    for(i=0u;(i<g_mus.patterns)&&(mus_fail==0u);i++)
    {
        if(g_mus_pat[i].kind==MUS_PAT_DATA)
        {
            MUS_MOD_Pattern(i);
        }
    }

    for(i=0u;(i<g_mus.samples)&&(mus_fail==0u);i++)
    {
        MUS_MOD_Sample(i,&g_mus_sh);
        MUS_Emit_Sample(i,&g_mus_sh);
    }
}

/* =====================================================================
   S3M
   ===================================================================== */

static unsigned int  mus_s3m_ffi;
static unsigned int  mus_s3m_cwtv;

void MUS_S3M_Sample(unsigned int i,MUS_SMPHDR *s)
{
    unsigned long hdr;
    unsigned long memseg;
    unsigned long ls;
    unsigned long le;
    unsigned char type;
    unsigned char pack;
    unsigned char fl;

    MUS_Clear_SmpHdr(s);
    hdr=g_mus_smp[i].hdr;
    if(hdr==0ul)
    {
        return;
    }

    type=MUS_U8_At(hdr);
    MUS_Read_Name(s->file,12u,0u);
    memseg=(unsigned long)MUS_Get()<<16;
    memseg|=MUS_U16();
    s->length=MUS_U32();
    ls=MUS_U32();
    le=MUS_U32();
    s->volume=MUS_Get();
    (void)MUS_Get();
    pack=MUS_Get();
    fl=MUS_Get();
    s->c5speed=MUS_U32();
    MUS_Seek(hdr+0x30ul);
    MUS_Read_Name(s->name,28u,0u);

    if(type>=2u)
    {
        s->flags=MUS_SF_ADLIB;
        s->length=0ul;
        s->c5speed&=0xFFFFul;
    }
    else if(type==0u)
    {
        s->length=0ul;
    }

    if(s->length!=0ul)
    {
        s->loop_start=(ls<s->length) ? ls : (s->length-1ul);
        s->loop_end=(le<s->length) ? le : s->length;
        if(((fl&1u)!=0u)&&(s->loop_end>=2ul)&&(s->loop_start<s->loop_end))
        {
            s->flags|=MUS_SF_LOOP;
        }
    }

    if(s->volume>64u) s->volume=64u;
    if(((mus_s3m_cwtv>>12)==1u)&&(s->c5speed>0xFFFFul)) s->c5speed=0xFFFFul;
    if(s->c5speed==0ul)        s->c5speed=8363ul;
    else if(s->c5speed<1024ul) s->c5speed=1024ul;

    s->data=memseg<<4;
    if((pack==4u)&&((fl&6u)==0u))
    {
        s->enc=MUS_ENC_ADPCM;
        s->flags|=MUS_SF_PACKED;
    }
    else
    {
        if((fl&4u)!=0u) { s->enc|=MUS_ENC_16;     s->flags|=MUS_SF_SRC16; }
        if((fl&2u)!=0u) { s->enc|=MUS_ENC_STEREO; s->flags|=MUS_SF_STEREO; }
        if(mus_s3m_ffi==2u) s->enc|=MUS_ENC_UNSIGNED;
    }

    MUS_Finish_SmpHdr(s,(unsigned char)(type==1u));
}

unsigned char MUS_S3M_Note(unsigned char n)
{
    unsigned int v;

    if(n<0xF0u)
    {
        v=(unsigned int)(n&0x0Fu)+12u*(unsigned int)(n>>4)+13u;
        return (unsigned char)((v>120u) ? 120u : v);
    }
    return (n==0xFEu) ? MUS_NOTE_CUT : 0u;
}

void MUS_S3M_Pattern(unsigned int p)
{
    unsigned int  r;
    unsigned char b;
    unsigned char v;
    MUS_CELL     *m;

    MUS_Begin("PD",g_mus_pat[p].addr);
    MUS_Seek(g_mus_pat[p].pos+2ul);

    for(r=0u;(r<64u)&&(mus_fail==0u);r++)
    {
        MUS_Row_Clear();
        while(mus_eof==0u)
        {
            b=MUS_Get();
            if(b==0u)
            {
                break;
            }

            m=MUS_Cell(b&31u);
            if((b&0x20u)!=0u)
            {
                m->note=MUS_S3M_Note(MUS_Get());
                m->ins=MUS_Get();
            }
            if((b&0x40u)!=0u)
            {
                v=MUS_Get();
                m->vol=((v>=128u)&&(v<=192u)) ? v : ((v>64u) ? 64u : v);
                m->has_vol=1u;
            }
            if((b&0x80u)!=0u)
            {
                v=MUS_Get();
                m->fx=((v>=1u)&&(v<=26u)) ? v : 0u;
                m->param=MUS_Get();
                if(m->fx==0u) m->param=0u;
            }
        }
        MUS_Row_Put();
    }
    MUS_Flush();
}

unsigned char MUS_S3M_Prepare(void)
{
    unsigned int  ordnum;
    unsigned int  insnum;
    unsigned int  patnum;
    unsigned int  flags;
    unsigned int  i;
    unsigned int  gus;
    unsigned long pp;
    unsigned long pan;
    unsigned char chset[32];
    unsigned char gv;
    unsigned char spd;
    unsigned char tempo;
    unsigned char mv;
    unsigned char uc;
    unsigned char dp;
    unsigned char ctype;
    unsigned char b;
    unsigned long hf;

    if((File_Size<96ul)||!MUS_Match_At(44ul,"SCRM",4u)||(MUS_U8_At(29ul)!=0x10u))
    {
        g_mus_error="NOT AN S3M FILE";
        return 0u;
    }

    ordnum=MUS_U16_At(32ul);
    insnum=MUS_U16();
    patnum=MUS_U16();
    flags=MUS_U16();
    mus_s3m_cwtv=MUS_U16();
    mus_s3m_ffi=MUS_U16();
    MUS_Seek(48ul);
    gv=MUS_Get();
    spd=MUS_Get();
    tempo=MUS_Get();
    mv=MUS_Get();
    uc=MUS_Get();
    dp=MUS_Get();
    MUS_Seek(64ul);
    for(i=0u;i<32u;i++) chset[i]=MUS_Get();

    if((mus_s3m_ffi!=1u)&&(mus_s3m_ffi!=2u))
    {
        g_mus_error="NOT AN S3M FILE";
        return 0u;
    }
    if(insnum>MUS_MAX_SMP)
    {
        g_mus_error="TOO MANY SAMPLES";
        return 0u;
    }

    g_mus.fmt=MUS_FMT_S3M;
    g_mus.ch=4u;
    for(i=0u;i<32u;i++)
    {
        if(chset[i]!=0xFFu) g_mus.ch=(unsigned char)(i+1u);
    }

    MUS_Seek(96ul);
    for(i=0u;i<ordnum;i++)
    {
        b=MUS_Get();
        if(i<MUS_MAX_ORD) g_mus.ord[i]=b;
    }
    g_mus.orders=(ordnum>MUS_MAX_ORD) ? MUS_MAX_ORD : ordnum;

    for(i=0u;i<insnum;i++)
    {
        g_mus_smp[i].hdr=(unsigned long)MUS_U16()<<4;
    }
    g_mus.patterns=(patnum>255u) ? 255u : patnum;
    pp=MUS_Tell();
    for(i=0u;i<g_mus.patterns;i++)
    {
        MUS_Seek(pp+(unsigned long)i*2ul);
        g_mus_pat[i].pos=(unsigned long)MUS_U16()<<4;
        g_mus_pat[i].rows=64u;
        g_mus_pat[i].len=0ul;
        g_mus_pat[i].kind=((g_mus_pat[i].pos!=0ul)&&(g_mus_pat[i].pos<File_Size)) ? MUS_PAT_DATA : MUS_PAT_EMPTY;
    }

    /* Channel table: S3M stereo pans, then the default pan table */
    hf=0ul;
    if((mv&0x80u)!=0u) hf|=MUS_HF_STEREO;
    for(i=0u;i<(unsigned int)g_mus.ch;i++)
    {
        ctype=(unsigned char)(chset[i]&0x7Fu);
        g_mus.chan[i]=((unsigned long)chset[i]<<24)|(64ul<<8);
        if(chset[i]==0xFFu)
        {
            g_mus.chan[i]|=MUS_CF_UNUSED|0x80ul;
            continue;
        }
        if((chset[i]&0x80u)!=0u) g_mus.chan[i]|=MUS_CF_MUTE;
        if((ctype>=16u)&&(ctype<=29u))
        {
            g_mus.chan[i]|=MUS_CF_UNUSED|0x80ul;      /* Adlib channel */
            continue;
        }
        g_mus.chan[i]|=((mv&0x80u)!=0u) ? (((ctype&8u)!=0u) ? 0xCCul : 0x33ul) : 0x80ul;
    }
    if(dp==0xFCu)
    {
        MUS_Seek(pp+(unsigned long)patnum*2ul);
        for(i=0u;i<32u;i++)
        {
            b=MUS_Get();
            if((i<(unsigned int)g_mus.ch)&&((b&0x20u)!=0u)&&((g_mus.chan[i]&MUS_CF_UNUSED)==0ul))
            {
                pan=((unsigned long)(b&0x0Fu)*256ul+8ul)/15ul;
                g_mus.chan[i]=(g_mus.chan[i]&~0xFFul)|((pan>255ul) ? 255ul : pan);
            }
        }
    }

    MUS_Extend_Patterns(0xFEu);

    g_mus.samples=insnum;
    g_mus.instruments=0u;
    if(MUS_Layout()==0u)
    {
        return 0u;
    }

    gus=0u;
    for(i=0u;i<insnum;i++)
    {
        MUS_S3M_Sample(i,&g_mus_sh);
        g_mus_smp[i].data=g_mus_sh.data;
        if((g_mus_sh.flags&MUS_SF_ADLIB)!=0u) hf|=MUS_HF_ADLIB;
        if((g_mus_smp[i].hdr!=0ul)&&(MUS_U8_At(g_mus_smp[i].hdr)==1u))
        {
            gus|=MUS_U16_At(g_mus_smp[i].hdr+0x28ul);
        }
        MUS_Alloc_Sample(i,&g_mus_sh);
    }

    if((flags&0x40u)||(mus_s3m_cwtv==0x1300u)) hf|=MUS_HF_FASTVOL;
    if(flags&0x10u) hf|=MUS_HF_AMIGALIM;
    if(flags&0x01u) hf|=MUS_HF_ST2VIB;

    g_mus.hw[4]=((unsigned long)((tempo<33u) ? 125u : tempo)<<16)|
                ((unsigned long)(((spd==0u)||(spd==255u)) ? 6u : spd)<<8);
    g_mus.hw[5]=((unsigned long)((gv>64u) ? 64u : gv)<<24)|((unsigned long)(mv&0x7Fu)<<16);
    g_mus.hw[6]=hf;
    g_mus.hw[7]=((unsigned long)mus_s3m_ffi<<16)|(unsigned long)mus_s3m_cwtv;
    g_mus.hw[8]=(unsigned long)flags|((unsigned long)mv<<16)|((unsigned long)uc<<24);
    g_mus.hw[9]=(unsigned long)dp|((unsigned long)gv<<8)|((unsigned long)gus<<16);
    g_mus.hw[20]=(unsigned long)(-12l);

    {
        char name[28];

        MUS_Seek(0ul);
        MUS_Read_Name(name,28u,0u);
        MUS_Pack_Name(&g_mus.hw[48],name,28u,8u);
        MUS_Pack_Name(&g_mus.hw[56],"Scream Tracker 3",16u,8u);
    }
    return 1u;
}

void MUS_S3M_Emit(void)
{
    unsigned int i;

    for(i=0u;(i<g_mus.patterns)&&(mus_fail==0u);i++)
    {
        if(g_mus_pat[i].kind==MUS_PAT_DATA)
        {
            MUS_S3M_Pattern(i);
        }
    }

    for(i=0u;(i<g_mus.samples)&&(mus_fail==0u);i++)
    {
        MUS_S3M_Sample(i,&g_mus_sh);
        MUS_Emit_Sample(i,&g_mus_sh);
    }
}

/* =====================================================================
   XM
   ===================================================================== */

/* Instrument header field, 0 beyond the declared header size (OpenMPT
   reads the struct partially). */
unsigned char MUS_XM_Ins_U8(unsigned long ipos,unsigned long size,unsigned long ofs)
{
    return (ofs<size) ? MUS_U8_At(ipos+ofs) : 0u;
}

unsigned int MUS_XM_Ins_U16(unsigned long ipos,unsigned long size,unsigned long ofs)
{
    return (unsigned int)MUS_XM_Ins_U8(ipos,size,ofs)|
           ((unsigned int)MUS_XM_Ins_U8(ipos,size,ofs+1ul)<<8);
}

unsigned long MUS_XM_Ins_Size(unsigned long ipos)
{
    unsigned long size;

    size=MUS_U32_At(ipos);
    return (size==0ul) ? 263ul : size;
}

void MUS_XM_Sample(unsigned int i,MUS_SMPHDR *s)
{
    unsigned long hdr;
    unsigned long ipos;
    unsigned long isize;
    unsigned long len;
    unsigned long ls;
    unsigned long ll;
    unsigned char type;
    unsigned char rsv;
    unsigned int  k;

    MUS_Clear_SmpHdr(s);
    hdr=g_mus_smp[i].hdr;

    MUS_Seek(hdr);
    len=MUS_U32();
    ls=MUS_U32();
    ll=MUS_U32();
    s->volume=MUS_Get();
    s->finetune=(signed char)MUS_Get();
    type=MUS_Get();
    s->pan=MUS_Get();
    s->relnote=(signed char)MUS_Get();
    rsv=MUS_Get();
    MUS_Read_Name(s->name,22u,0u);

    if(s->volume>64u) s->volume=64u;
    s->flags=MUS_SF_PAN;

    if((rsv==0xADu)&&((type&0x30u)==0u))
    {
        s->enc=MUS_ENC_ADPCM;
        s->flags|=MUS_SF_PACKED;
    }
    else
    {
        s->enc=MUS_ENC_DELTA;
        if((type&0x10u)!=0u)
        {
            s->enc|=MUS_ENC_16;
            s->flags|=MUS_SF_SRC16;
            len>>=1; ls>>=1; ll>>=1;
        }
        if((type&0x20u)!=0u)
        {
            s->enc|=MUS_ENC_STEREO;
            s->flags|=MUS_SF_STEREO;
            len>>=1; ls>>=1; ll>>=1;
        }
    }

    s->length=len;
    s->loop_start=ls;
    s->loop_end=ls+ll;
    if(((type&3u)!=0u)&&(s->loop_end>s->loop_start))
    {
        s->flags|=MUS_SF_LOOP;
        if((type&2u)!=0u) s->flags|=MUS_SF_BIDI;
    }

    /* Auto-vibrato lives in the instrument; every sample gets a copy. */
    k=g_mus_smp[i].ins;
    if(k!=0u)
    {
        ipos=g_mus_ins_pos[k];
        isize=MUS_XM_Ins_Size(ipos);
        s->vib[0]=MUS_XM_Ins_U8(ipos,isize,235ul);
        s->vib[1]=MUS_XM_Ins_U8(ipos,isize,236ul);
        s->vib[2]=MUS_XM_Ins_U8(ipos,isize,237ul);
        s->vib[3]=MUS_XM_Ins_U8(ipos,isize,238ul);
    }

    s->data=g_mus_smp[i].data;

    /* OXM: Ogg Vorbis samples cannot be decoded here, they stay silent. */
    if((s->length>=8ul)&&MUS_Match_At(s->data+4ul,"OggS",4u))
    {
        s->flags|=MUS_SF_NODATA;
        s->flags&=(unsigned int)~MUS_SF_STEREO;
        s->length=0ul;
        MUS_Sanitize_Loops(s);
        return;
    }

    MUS_Finish_SmpHdr(s,1u);
}

/* Bytes a sample occupies in the file, from its header. */
unsigned long MUS_XM_Sample_Bytes(unsigned long hdr)
{
    unsigned long len;
    unsigned char type;
    unsigned char rsv;

    len=MUS_U32_At(hdr);
    type=MUS_U8_At(hdr+14ul);
    rsv=MUS_U8_At(hdr+17ul);

    if((rsv==0xADu)&&((type&0x30u)==0u))
    {
        return 16ul+((len+1ul)>>1);
    }
    return len;
}

void MUS_XM_Env(MUS_ENV *e,unsigned long ipos,unsigned long isize,
                unsigned long pts_ofs,unsigned char n,unsigned char type,
                unsigned char sus,unsigned char ls,unsigned char le,int bias)
{
    unsigned int i;
    int          v;

    memset(e,0,sizeof(MUS_ENV));
    e->points=(unsigned char)((n>12u) ? 12u : n);
    for(i=0u;i<e->points;i++)
    {
        e->tick[i]=MUS_XM_Ins_U16(ipos,isize,pts_ofs+(unsigned long)i*4ul);
        v=(int)(signed short)MUS_XM_Ins_U16(ipos,isize,pts_ofs+(unsigned long)i*4ul+2ul);
        e->value[i]=((v<0) ? 0 : ((v>64) ? 64 : v))+bias;
    }
    MUS_Env_Fix_Ticks(e);

    if(((type&1u)!=0u)&&(e->points!=0u)) e->flags|=MUS_EF_ON;
    if(sus<12u)
    {
        if((type&2u)!=0u) e->flags|=MUS_EF_SUS;
        e->sus_start=sus;
        e->sus_end=sus;
    }
    if((le<12u)&&(le>=ls))
    {
        if((type&4u)!=0u) e->flags|=MUS_EF_LOOP;
        e->loop_start=ls;
        e->loop_end=le;
    }
}

void MUS_XM_Instrument(unsigned int k,MUS_INSHDR *h)
{
    unsigned long ipos;
    unsigned long isize;
    unsigned int  nsm;
    unsigned int  i;
    unsigned char s;

    MUS_Clear_InsHdr(h);
    ipos=g_mus_ins_pos[k];
    if(ipos==0ul)
    {
        return;
    }
    isize=MUS_XM_Ins_Size(ipos);

    MUS_Seek(ipos+4ul);
    MUS_Read_Name(h->name,22u,0u);
    nsm=(isize>=29ul) ? MUS_XM_Ins_U16(ipos,isize,27ul) : 0u;
    h->nsmp=nsm;

    if(nsm!=0u)
    {
        for(i=0u;i<96u;i++)
        {
            s=MUS_XM_Ins_U8(ipos,isize,33ul+i);
            h->key_smp[i+12u]=(((unsigned int)s<nsm)&&((g_mus_ins_first[k]+(unsigned int)s)<g_mus.samples)) ?
                              (g_mus_ins_first[k]+(unsigned int)s+1u) : 0u;
        }

        MUS_XM_Env(&h->env[0],ipos,isize,129ul,MUS_XM_Ins_U8(ipos,isize,225ul),
                   MUS_XM_Ins_U8(ipos,isize,233ul),MUS_XM_Ins_U8(ipos,isize,227ul),
                   MUS_XM_Ins_U8(ipos,isize,228ul),MUS_XM_Ins_U8(ipos,isize,229ul),0);
        MUS_XM_Env(&h->env[1],ipos,isize,177ul,MUS_XM_Ins_U8(ipos,isize,226ul),
                   MUS_XM_Ins_U8(ipos,isize,234ul),MUS_XM_Ins_U8(ipos,isize,230ul),
                   MUS_XM_Ins_U8(ipos,isize,231ul),MUS_XM_Ins_U8(ipos,isize,232ul),-32);

        h->vib[0]=MUS_XM_Ins_U8(ipos,isize,235ul);
        h->vib[1]=MUS_XM_Ins_U8(ipos,isize,236ul);
        h->vib[2]=MUS_XM_Ins_U8(ipos,isize,237ul);
        h->vib[3]=MUS_XM_Ins_U8(ipos,isize,238ul);
        h->fadeout=(unsigned long)MUS_XM_Ins_U16(ipos,isize,239ul)*2ul;
    }
}

/* Pattern headers from pos: positions, rows and packed sizes. */
unsigned long MUS_XM_Walk_Patterns(unsigned long pos,unsigned int count)
{
    unsigned long hlen;
    unsigned int  psize;
    unsigned int  rows;
    unsigned int  p;

    for(p=0u;p<count;p++)
    {
        hlen=MUS_U32_At(pos);
        if((hlen<8ul)||((pos+hlen)>File_Size))
        {
            break;
        }
        (void)MUS_Get();
        if(g_mus.version==0x0102u)
        {
            rows=(unsigned int)MUS_Get()+1u;
        }
        else
        {
            rows=MUS_U16();
        }
        psize=MUS_U16();

        if(rows==0u) rows=64u;
        if(rows>MUS_MAX_ROWS) rows=MUS_MAX_ROWS;

        g_mus_pat[p].rows=rows;
        g_mus_pat[p].pos=pos+hlen;
        g_mus_pat[p].len=psize;
        g_mus_pat[p].kind=(psize!=0u) ? MUS_PAT_DATA : MUS_PAT_EMPTY;
        pos+=hlen+(unsigned long)psize;
    }

    for(;p<count;p++)
    {
        g_mus_pat[p].kind=MUS_PAT_MISSING;
        g_mus_pat[p].rows=64u;
    }
    return pos;
}

/* Instrument headers from pos.  With data_inline (XM 1.04) each
   instrument's sample data follows its sample headers. */
unsigned long MUS_XM_Walk_Instruments(unsigned long pos,unsigned int count,unsigned char data_inline)
{
    unsigned long isize;
    unsigned long data;
    unsigned int  nsm;
    unsigned int  k;
    unsigned int  j;
    unsigned int  s;

    s=0u;
    for(k=1u;k<=count;k++)
    {
        g_mus_ins_pos[k]=pos;
        g_mus_ins_first[k]=s;

        if((pos+4ul)>File_Size)
        {
            g_mus_ins_pos[k]=0ul;
            continue;
        }

        isize=MUS_XM_Ins_Size(pos);
        nsm=(isize>=29ul) ? MUS_XM_Ins_U16(pos,isize,27ul) : 0u;
        if(nsm>255u)
        {
            g_mus_error="BAD INSTRUMENT";
            return 0ul;
        }
        if((s+nsm)>MUS_MAX_SMP)
        {
            g_mus_error="TOO MANY SAMPLES";
            return 0ul;
        }

        pos+=isize;
        data=pos+(unsigned long)nsm*40ul;
        for(j=0u;j<nsm;j++)
        {
            g_mus_smp[s+j].hdr=pos+(unsigned long)j*40ul;
            g_mus_smp[s+j].ins=(unsigned char)k;
            g_mus_smp[s+j].data=data;
            if(data_inline!=0u)
            {
                data+=MUS_XM_Sample_Bytes(g_mus_smp[s+j].hdr);
            }
        }
        pos=(data_inline!=0u) ? data : (pos+(unsigned long)nsm*40ul);
        s+=nsm;
    }

    g_mus.samples=s;
    return pos;
}

void MUS_XM_Pattern(unsigned int p)
{
    unsigned long left;
    unsigned int  r;
    unsigned int  c;
    unsigned char info;
    unsigned char v;
    MUS_CELL     *m;

    MUS_Begin("PD",g_mus_pat[p].addr);
    MUS_Seek(g_mus_pat[p].pos);
    left=g_mus_pat[p].len;

    for(r=0u;(r<g_mus_pat[p].rows)&&(mus_fail==0u);r++)
    {
        MUS_Row_Clear();
        for(c=0u;(c<(unsigned int)g_mus.file_ch)&&(left!=0ul);c++)
        {
            m=MUS_Cell(c);
            info=MUS_Get();
            left--;

            if((info&0x80u)==0u)
            {
                v=info;
                info=0x1Fu;
            }
            else
            {
                v=0u;
                if(((info&1u)!=0u)&&(left!=0ul)) { v=MUS_Get(); left--; }
            }
            m->note=((v>0u)&&(v<97u)) ? (unsigned char)(v+12u) : ((v==97u) ? MUS_NOTE_OFF : 0u);

            if(((info&2u)!=0u)&&(left!=0ul))
            {
                v=MUS_Get(); left--;
                m->ins=(v==0xFFu) ? 0u : v;
            }
            if(((info&4u)!=0u)&&(left!=0ul))
            {
                v=MUS_Get(); left--;
                if(((v>=0x10u)&&(v<=0x50u))||(v>=0x60u))
                {
                    m->vol=v;
                    m->has_vol=1u;
                }
            }
            if(((info&8u)!=0u)&&(left!=0ul))
            {
                m->fx=MUS_Get(); left--;
            }
            if(((info&16u)!=0u)&&(left!=0ul))
            {
                m->param=MUS_Get(); left--;
            }
            if(m->fx>35u)
            {
                m->fx=0u;
                m->param=0u;
            }
        }
        MUS_Row_Put();
    }
    MUS_Flush();
}

unsigned char MUS_XM_Prepare(void)
{
    unsigned long hsize;
    unsigned long pos;
    unsigned int  songlen;
    unsigned int  restart;
    unsigned int  chn;
    unsigned int  pats;
    unsigned int  ins;
    unsigned int  flags;
    unsigned int  spd;
    unsigned int  bpm;
    unsigned int  i;
    unsigned long hf;
    char          name[20];
    char          tracker[20];

    if((File_Size<80ul)||!MUS_Match_At(0ul,"Extended Module: ",17u))
    {
        g_mus_error="NOT AN XM FILE";
        return 0u;
    }

    MUS_Seek(17ul);
    MUS_Read_Name(name,20u,0u);
    MUS_Seek(38ul);
    MUS_Read_Name(tracker,20u,0u);
    g_mus.version=MUS_U16_At(58ul);
    hsize=MUS_U32();
    songlen=MUS_U16();
    restart=MUS_U16();
    chn=MUS_U16();
    pats=MUS_U16();
    ins=MUS_U16();
    flags=MUS_U16();
    spd=MUS_U16();
    bpm=MUS_U16();

    if((chn==0u)||(pats>256u)||(ins>MUS_MAX_INS))
    {
        g_mus_error="BAD XM HEADER";
        return 0u;
    }

    hf=MUS_HF_STEREO|MUS_HF_INSTR;
    g_mus.fmt=MUS_FMT_XM;
    g_mus.file_ch=(chn>255u) ? 255u : (unsigned char)chn;
    g_mus.ch=(chn>MUS_MAX_CH) ? (unsigned char)MUS_MAX_CH : (unsigned char)chn;
    if(chn>MUS_MAX_CH) hf|=MUS_HF_CHCUT;

    /* Orders past the declared header size read as 0, as in FT2. */
    g_mus.orders=(songlen>MUS_MAX_ORD) ? MUS_MAX_ORD : songlen;
    MUS_Seek(80ul);
    for(i=0u;i<g_mus.orders;i++)
    {
        g_mus.ord[i]=((20ul+(unsigned long)i)<hsize) ? MUS_Get() : 0u;
    }
    if(g_mus.orders==0u)
    {
        g_mus.orders=1u;
        g_mus.ord[0]=0u;
    }

    pos=60ul+hsize;
    if(g_mus.version>=0x0104u)
    {
        pos=MUS_XM_Walk_Patterns(pos,pats);
        if(MUS_XM_Walk_Instruments(pos,ins,1u)==0ul) return 0u;
    }
    else
    {
        pos=MUS_XM_Walk_Instruments(pos,ins,0u);
        if(pos==0ul) return 0u;
        pos=MUS_XM_Walk_Patterns(pos,pats);
        for(i=0u;i<g_mus.samples;i++)
        {
            g_mus_smp[i].data=pos;
            pos+=MUS_XM_Sample_Bytes(g_mus_smp[i].hdr);
        }
    }

    g_mus.patterns=pats;
    MUS_Extend_Patterns(0x100u);
    g_mus.instruments=ins;

    if(MUS_Layout()==0u)
    {
        return 0u;
    }

    for(i=0u;i<g_mus.samples;i++)
    {
        MUS_XM_Sample(i,&g_mus_sh);
        MUS_Alloc_Sample(i,&g_mus_sh);
    }

    if((flags&1u)!=0u) hf|=MUS_HF_LINEAR;
    if(!memcmp(tracker,"FastTracker v2.00",17)) hf|=MUS_HF_FT2;

    g_mus.hw[4]=((unsigned long)((bpm==0u) ? 125u : bpm)<<16)|
                ((unsigned long)((spd==0u) ? 6u : (spd&0xFFu))<<8)|
                (unsigned long)((restart<g_mus.orders) ? restart : 0u);
    g_mus.hw[5]=(64ul<<24);
    g_mus.hw[6]=hf;
    g_mus.hw[7]=(unsigned long)g_mus.version;
    g_mus.hw[8]=(unsigned long)flags;
    g_mus.hw[9]=0ul;
    g_mus.hw[20]=(unsigned long)(-12l);

    for(i=0u;i<(unsigned int)g_mus.ch;i++)
    {
        g_mus.chan[i]=0x80ul|(64ul<<8);
    }

    MUS_Pack_Name(&g_mus.hw[48],name,20u,8u);
    MUS_Pack_Name(&g_mus.hw[56],tracker,20u,8u);
    return 1u;
}

void MUS_XM_Emit(void)
{
    unsigned int i;
    unsigned int k;
    unsigned int j;
    unsigned int s;

    for(i=0u;(i<g_mus.patterns)&&(mus_fail==0u);i++)
    {
        if(g_mus_pat[i].kind==MUS_PAT_DATA)
        {
            MUS_XM_Pattern(i);
        }
    }

    for(k=1u;(k<=g_mus.instruments)&&(mus_fail==0u);k++)
    {
        MUS_XM_Instrument(k,&g_mus_ih);
        MUS_Emit_Instrument(k-1u,&g_mus_ih);

        s=g_mus_ins_first[k];
        for(j=0u;(j<g_mus_ih.nsmp)&&((s+j)<g_mus.samples)&&(mus_fail==0u);j++)
        {
            MUS_XM_Sample(s+j,&g_mus_sh);
            MUS_Emit_Sample(s+j,&g_mus_sh);
        }
    }
}

/* =====================================================================
   IT
   ===================================================================== */

static unsigned int  mus_it_flags;
static unsigned int  mus_it_cmwt;

void MUS_IT_Sample(unsigned int i,MUS_SMPHDR *s)
{
    unsigned long hdr;
    unsigned char gvl;
    unsigned char flg;
    unsigned char cvt;
    unsigned char dfp;

    MUS_Clear_SmpHdr(s);
    hdr=g_mus_smp[i].hdr;
    if((hdr==0ul)||((hdr+80ul)>File_Size))
    {
        return;
    }

    MUS_Seek(hdr+4ul);
    MUS_Read_Name(s->file,12u,0u);
    (void)MUS_Get();
    gvl=MUS_Get();
    flg=MUS_Get();
    s->volume=MUS_Get();
    MUS_Read_Name(s->name,26u,1u);
    cvt=MUS_Get();
    dfp=MUS_Get();
    s->length=MUS_U32();
    s->loop_start=MUS_U32();
    s->loop_end=MUS_U32();
    s->c5speed=MUS_U32();
    s->sus_start=MUS_U32();
    s->sus_end=MUS_U32();
    s->data=MUS_U32();
    s->vib[3]=MUS_Get();                  /* ViS: rate  */
    s->vib[2]=MUS_Get();                  /* ViD: depth */
    s->vib[1]=MUS_Get();                  /* ViR: sweep */
    s->vib[0]=MUS_Get();                  /* ViT: type  */

    if(s->volume>64u) s->volume=64u;
    s->gvol=(gvl>64u) ? 64u : gvl;
    s->pan=(unsigned char)((((unsigned int)(dfp&0x7Fu)*4u)>255u) ? 255u : ((unsigned int)(dfp&0x7Fu)*4u));
    if((dfp&0x80u)!=0u) s->flags|=MUS_SF_PAN;
    if(s->c5speed==0ul)       s->c5speed=8363ul;
    else if(s->c5speed<256ul) s->c5speed=256ul;

    if((flg&0x10u)!=0u) s->flags|=MUS_SF_LOOP;
    if((flg&0x20u)!=0u) s->flags|=MUS_SF_SUS;
    if((flg&0x40u)!=0u) s->flags|=MUS_SF_BIDI;
    if((flg&0x80u)!=0u) s->flags|=MUS_SF_SUSBIDI;

    if((flg&0x02u)!=0u)
    {
        s->enc|=MUS_ENC_16;
        s->flags|=MUS_SF_SRC16;
    }
    if(((flg&0x04u)!=0u)&&(g_mus.version>=0x214u))
    {
        s->enc|=MUS_ENC_STEREO;
        s->flags|=MUS_SF_STEREO;
    }

    if((flg&0x08u)!=0u)
    {
        s->enc|=((cvt&4u)!=0u) ? MUS_ENC_IT215 : MUS_ENC_IT214;
        s->flags|=MUS_SF_PACKED;
    }
    else if(((flg&0x02u)==0u)&&(cvt==0xFFu))
    {
        s->enc=MUS_ENC_ADPCM;
        s->flags=(unsigned int)((s->flags|MUS_SF_PACKED)&~MUS_SF_STEREO);
    }
    else
    {
        if((cvt&1u)==0u) s->enc|=MUS_ENC_UNSIGNED;
        if((cvt&2u)!=0u) s->enc|=MUS_ENC_BE;
        if((cvt&4u)!=0u) s->enc|=MUS_ENC_DELTA;
        if(((cvt&8u)!=0u)&&((flg&0x02u)!=0u)) s->enc|=MUS_ENC_PTM16;
    }

    MUS_Finish_SmpHdr(s,(unsigned char)((flg&1u)!=0u));
}

/* IT envelope (82 bytes at pos); bias 0 for volume, signed otherwise. */
void MUS_IT_Env(MUS_ENV *e,unsigned long pos,unsigned char is_vol)
{
    unsigned char fl;
    unsigned char num;
    unsigned int  i;
    int           v;

    memset(e,0,sizeof(MUS_ENV));
    MUS_Seek(pos);
    fl=MUS_Get();
    num=MUS_Get();
    e->loop_start=MUS_Get();
    e->loop_end=MUS_Get();
    e->sus_start=MUS_Get();
    e->sus_end=MUS_Get();

    e->points=(unsigned char)((num>25u) ? 25u : num);
    if(e->loop_start>25u) e->loop_start=25u;
    if(e->loop_end<e->loop_start) e->loop_end=e->loop_start;
    if(e->loop_end>25u) e->loop_end=25u;
    if(e->sus_start>25u) e->sus_start=25u;
    if(e->sus_end<e->sus_start) e->sus_end=e->sus_start;
    if(e->sus_end>25u) e->sus_end=25u;

    for(i=0u;i<25u;i++)
    {
        v=(int)(signed char)MUS_Get();
        e->tick[i]=MUS_U16();
        if(is_vol!=0u)
        {
            v=(v<0) ? 0 : ((v>64) ? 64 : v);
        }
        else
        {
            v=(v<-32) ? -32 : ((v>32) ? 32 : v);
        }
        e->value[i]=v;
    }
    MUS_Env_Fix_Ticks(e);

    if(((fl&1u)!=0u)&&(e->points!=0u)) e->flags|=MUS_EF_ON;
    if((fl&2u)!=0u) e->flags|=MUS_EF_LOOP;
    if((fl&4u)!=0u) e->flags|=MUS_EF_SUS;
    if((fl&8u)!=0u) e->flags|=MUS_EF_CARRY;
    if((fl&0x80u)!=0u) e->flags|=MUS_EF_FILTER;
}

void MUS_IT_Keyboard(MUS_INSHDR *h,unsigned long pos)
{
    unsigned int  i;
    unsigned char n;
    unsigned char s;

    MUS_Seek(pos);
    for(i=0u;i<120u;i++)
    {
        n=MUS_Get();
        s=MUS_Get();
        h->key_note[i]=(n<120u) ? (unsigned char)(n+1u) : (unsigned char)(i+1u);
        h->key_smp[i]=((unsigned int)s<=g_mus.samples) ? (unsigned int)s : 0u;
    }
}

void MUS_IT_Instrument(unsigned int k,MUS_INSHDR *h)
{
    unsigned long pos;
    unsigned char dfp;
    unsigned int  i;

    MUS_Clear_InsHdr(h);
    pos=g_mus_ins_pos[k];
    if((pos==0ul)||((pos+554ul)>File_Size))
    {
        return;
    }

    MUS_Seek(pos+32ul);
    MUS_Read_Name(h->name,26u,1u);
    h->nsmp=MUS_U8_At(pos+30ul);

    if(g_mus.it_old!=0u)
    {
        unsigned char fl;

        fl=MUS_U8_At(pos+17ul);
        h->flags=MUS_IF_OLDIT;
        h->env[0].loop_start=MUS_Get();
        h->env[0].loop_end=MUS_Get();
        h->env[0].sus_start=MUS_Get();
        h->env[0].sus_end=MUS_Get();
        h->fadeout=(unsigned long)MUS_U16_At(pos+24ul)*128ul;
        h->nna=MUS_Get();
        h->dct=(MUS_Get()!=0u) ? 1u : 0u;

        MUS_Seek(pos+504ul);
        for(i=0u;i<25u;i++)
        {
            unsigned char t;

            t=MUS_Get();
            h->env[0].value[i]=(int)MUS_Get();
            if(t==0xFFu)
            {
                break;
            }
            h->env[0].tick[i]=t;
        }
        h->env[0].points=(unsigned char)i;
        if((fl&1u)!=0u) h->env[0].flags|=MUS_EF_ON;
        if(((fl&2u)!=0u)&&(h->env[0].loop_start<h->env[0].points)&&(h->env[0].loop_end<h->env[0].points))
        {
            h->env[0].flags|=MUS_EF_LOOP;
        }
        if(((fl&4u)!=0u)&&(h->env[0].sus_start<h->env[0].points)&&(h->env[0].sus_end<h->env[0].points))
        {
            h->env[0].flags|=MUS_EF_SUS;
        }
        MUS_IT_Keyboard(h,pos+64ul);
        return;
    }

    MUS_Seek(pos+17ul);
    h->nna=MUS_Get();
    h->dct=MUS_Get();
    h->dca=MUS_Get();
    h->fadeout=(unsigned long)MUS_U16()*64ul;
    h->pps=(signed char)MUS_Get();
    h->ppc=(unsigned char)(MUS_Get()+1u);
    h->gvol=MUS_Get();
    dfp=MUS_Get();
    h->rv=MUS_Get();
    h->rp=MUS_Get();
    if(h->gvol>128u) h->gvol=128u;
    if(h->rv>100u) h->rv=100u;
    if(h->rp>64u) h->rp=64u;
    h->pan=(unsigned char)((((unsigned int)(dfp&0x7Fu)*4u)>256u) ? 128u :
                           ((((unsigned int)(dfp&0x7Fu)*4u)>255u) ? 255u : ((unsigned int)(dfp&0x7Fu)*4u)));
    if((dfp&0x80u)==0u) h->flags|=MUS_IF_PAN;

    MUS_Seek(pos+58ul);
    h->ifc=MUS_Get();
    h->ifr=MUS_Get();
    h->midi_ch=MUS_Get();
    h->midi_prog=MUS_Get();
    h->midi_bank=MUS_U16();
    if((h->ifc&0x80u)!=0u) h->flags|=MUS_IF_CUTOFF;
    if((h->ifr&0x80u)!=0u) h->flags|=MUS_IF_RESO;
    h->ifc&=0x7Fu;
    h->ifr&=0x7Fu;

    MUS_IT_Keyboard(h,pos+64ul);
    MUS_IT_Env(&h->env[0],pos+304ul,1u);
    MUS_IT_Env(&h->env[1],pos+386ul,0u);
    MUS_IT_Env(&h->env[2],pos+468ul,0u);
}

/* Packed IT pattern, after OpenMPT's reader: channel = (b & 0x7F) - 1,
   "last value" bits applied, new values read in note-ins-vol-fx order. */
static unsigned char mus_it_mask[MUS_MAX_CH];
static unsigned char mus_it_last[MUS_MAX_CH][5];

void MUS_IT_Pattern(unsigned int p)
{
    unsigned long left;
    unsigned int  r;
    unsigned int  c;
    unsigned char b;
    unsigned char mk;
    unsigned char raw[5];
    unsigned char *last;
    unsigned char dummy_mask;
    unsigned char dummy_last[5];
    MUS_CELL     *m;

    MUS_Begin("PD",g_mus_pat[p].addr);
    MUS_Seek(g_mus_pat[p].pos);
    left=g_mus_pat[p].len;
    memset(mus_it_mask,0,sizeof mus_it_mask);
    memset(mus_it_last,0,sizeof mus_it_last);
    dummy_mask=0u;
    memset(dummy_last,0,sizeof dummy_last);

    for(r=0u;(r<g_mus_pat[p].rows)&&(mus_fail==0u);r++)
    {
        MUS_Row_Clear();
        while(left!=0ul)
        {
            b=MUS_Get();
            left--;
            if(b==0u)
            {
                break;
            }

            c=(unsigned int)(b&0x7Fu);
            if(c!=0u) c--;

            if((b&0x80u)!=0u)
            {
                if(left==0ul) break;
                mk=MUS_Get();
                left--;
                if(c<MUS_MAX_CH) mus_it_mask[c]=mk; else dummy_mask=mk;
            }
            mk=(c<MUS_MAX_CH) ? mus_it_mask[c] : dummy_mask;
            last=(c<MUS_MAX_CH) ? mus_it_last[c] : dummy_last;

            raw[0]=raw[1]=raw[2]=raw[3]=raw[4]=0u;
            if((mk&0x01u)!=0u) { if(left<1ul) { left=0ul; break; } raw[0]=MUS_Get(); left--; last[0]=raw[0]; }
            if((mk&0x02u)!=0u) { if(left<1ul) { left=0ul; break; } raw[1]=MUS_Get(); left--; last[1]=raw[1]; }
            if((mk&0x04u)!=0u) { if(left<1ul) { left=0ul; break; } raw[2]=MUS_Get(); left--; last[2]=raw[2]; }
            if((mk&0x08u)!=0u)
            {
                if(left<2ul) { left=0ul; break; }
                raw[3]=MUS_Get();
                raw[4]=MUS_Get();
                left-=2ul;
                last[3]=raw[3];
                last[4]=raw[4];
            }

            if(c>=(unsigned int)g_mus.ch)
            {
                continue;
            }
            m=&mus_row[c];

            if((mk&0x11u)!=0u)
            {
                b=((mk&0x01u)!=0u) ? raw[0] : last[0];
                m->note=(b<120u) ? (unsigned char)(b+1u) :
                        ((b==0xFFu) ? MUS_NOTE_OFF : ((b==0xFEu) ? MUS_NOTE_CUT : MUS_NOTE_FADE));
            }
            if((mk&0x22u)!=0u)
            {
                m->ins=((mk&0x02u)!=0u) ? raw[1] : last[1];
            }
            if((mk&0x44u)!=0u)
            {
                b=((mk&0x04u)!=0u) ? raw[2] : last[2];
                if((b<=124u)||((b>=128u)&&(b<=212u))||((b>=223u)&&(b<=232u)))
                {
                    m->vol=b;
                    m->has_vol=1u;
                }
            }
            if((mk&0x88u)!=0u)
            {
                b=((mk&0x08u)!=0u) ? raw[3] : last[3];
                m->fx=(b<=31u) ? b : 0u;
                m->param=(b<=31u) ? (((mk&0x08u)!=0u) ? raw[4] : last[4]) : 0u;
            }
        }
        MUS_Row_Put();
    }
    MUS_Flush();
}

/* Channels in use: the highest channel carrying data in any pattern. */
unsigned char MUS_IT_Count_Channels(void)
{
    static const unsigned char skip[16]={0,1,1,2,1,2,2,3,2,3,3,4,3,4,4,5};
    unsigned char mask[128];
    unsigned long left;
    unsigned int  p;
    unsigned int  r;
    unsigned int  c;
    unsigned int  n;
    unsigned char b;

    n=1u;
    for(p=0u;p<g_mus.patterns;p++)
    {
        if(g_mus_pat[p].kind!=MUS_PAT_DATA)
        {
            continue;
        }

        memset(mask,0,sizeof mask);
        MUS_Seek(g_mus_pat[p].pos);
        left=g_mus_pat[p].len;
        r=0u;

        while((r<g_mus_pat[p].rows)&&(left!=0ul))
        {
            b=MUS_Get();
            left--;
            if(b==0u)
            {
                r++;
                continue;
            }
            c=(unsigned int)(b&0x7Fu);
            if(c!=0u) c--;
            if((b&0x80u)!=0u)
            {
                if(left==0ul) break;
                mask[c]=MUS_Get();
                left--;
            }
            if((mask[c]&0x0Fu)!=0u)
            {
                if(c>=MUS_MAX_CH) g_mus.hw[6]|=MUS_HF_CHCUT;
                else if((c+1u)>n) n=c+1u;
                if(left<(unsigned long)skip[mask[c]&0x0Fu]) break;
                MUS_Skip((unsigned long)skip[mask[c]&0x0Fu]);
                left-=(unsigned long)skip[mask[c]&0x0Fu];
            }
        }
    }
    return (unsigned char)n;
}

unsigned char MUS_IT_Prepare(void)
{
    unsigned int  ordnum;
    unsigned int  insnum;
    unsigned int  smpnum;
    unsigned int  patnum;
    unsigned int  special;
    unsigned int  i;
    unsigned int  rows;
    unsigned long pos;
    unsigned long minptr;
    unsigned long v;
    unsigned long hf;
    unsigned char gv;
    unsigned char mv;
    unsigned char is;
    unsigned char it;
    unsigned char sep;
    unsigned char pwd;
    unsigned char pan;
    char          name[26];

    if((File_Size<192ul)||!MUS_Match_At(0ul,"IMPM",4u))
    {
        g_mus_error="NOT AN IT FILE";
        return 0u;
    }

    MUS_Seek(4ul);
    MUS_Read_Name(name,26u,1u);
    ordnum=MUS_U16_At(32ul);
    insnum=MUS_U16();
    smpnum=MUS_U16();
    patnum=MUS_U16();
    g_mus.version=MUS_U16();
    mus_it_cmwt=MUS_U16();
    mus_it_flags=MUS_U16();
    special=MUS_U16();
    gv=MUS_Get();
    mv=MUS_Get();
    is=MUS_Get();
    it=MUS_Get();
    sep=MUS_Get();
    pwd=MUS_Get();

    if((insnum>MUS_MAX_INS)||(smpnum>MUS_MAX_SMP))
    {
        g_mus_error="TOO MANY SAMPLES";
        return 0u;
    }

    g_mus.fmt=MUS_FMT_IT;
    g_mus.it_old=(unsigned char)(mus_it_cmwt<0x200u);
    g_mus.instruments=((mus_it_flags&4u)!=0u) ? insnum : 0u;
    g_mus.samples=smpnum;

    MUS_Seek(192ul);
    for(i=0u;i<ordnum;i++)
    {
        unsigned char o;

        o=MUS_Get();
        if(i<MUS_MAX_ORD) g_mus.ord[i]=o;
    }
    g_mus.orders=(ordnum>MUS_MAX_ORD) ? MUS_MAX_ORD : ordnum;

    pos=192ul+(unsigned long)ordnum;
    minptr=0xFFFFFFFFul;
    for(i=0u;i<insnum;i++)
    {
        v=MUS_U32_At(pos+(unsigned long)i*4ul);
        if(i<=MUS_MAX_INS) g_mus_ins_pos[i+1u]=v;
        if((v!=0ul)&&(v<minptr)) minptr=v;
    }
    pos+=(unsigned long)insnum*4ul;
    for(i=0u;i<smpnum;i++)
    {
        v=MUS_U32_At(pos+(unsigned long)i*4ul);
        g_mus_smp[i].hdr=v;
        if((v!=0ul)&&(v<minptr)) minptr=v;
    }
    pos+=(unsigned long)smpnum*4ul;
    g_mus.patterns=(patnum>MUS_MAX_PAT) ? MUS_MAX_PAT : patnum;
    for(i=0u;i<patnum;i++)
    {
        v=MUS_U32_At(pos+(unsigned long)i*4ul);
        if((v!=0ul)&&(v<minptr)) minptr=v;
        if(i>=MUS_MAX_PAT)
        {
            continue;
        }

        g_mus_pat[i].kind=MUS_PAT_EMPTY;
        g_mus_pat[i].rows=64u;
        g_mus_pat[i].pos=0ul;
        g_mus_pat[i].len=0ul;
        if((v!=0ul)&&((v+8ul)<=File_Size))
        {
            rows=MUS_U16_At(v+2ul);
            if((rows>=1u)&&(rows<=MUS_MAX_ROWS))
            {
                g_mus_pat[i].kind=MUS_PAT_DATA;
                g_mus_pat[i].rows=rows;
                g_mus_pat[i].pos=v+8ul;
                g_mus_pat[i].len=MUS_U16_At(v);
            }
        }
    }
    pos+=(unsigned long)patnum*4ul;
    if((special&1u)&&(MUS_U32_At(56ul)!=0ul)&&(MUS_U32_At(56ul)<minptr))
    {
        minptr=MUS_U32_At(56ul);
    }

    /* Edit history (only if it ends before the first parapointer, as some
       trackers set the flag without writing it), then MIDI macros. */
    if((special&2u)!=0u)
    {
        v=MUS_U16_At(pos);
        if((pos+2ul+v*8ul)<=minptr)
        {
            pos+=2ul+v*8ul;
        }
    }
    g_mus.midi_pos=0ul;
    if(((mus_it_flags&0x80u)||(special&0x08u))&&((pos+4896ul)<=File_Size))
    {
        g_mus.midi_pos=pos;
    }

    MUS_Extend_Patterns(0xFEu);

    g_mus.ch=MUS_IT_Count_Channels();
    g_mus.file_ch=g_mus.ch;

    if(MUS_Layout()==0u)
    {
        return 0u;
    }

    for(i=0u;i<smpnum;i++)
    {
        MUS_IT_Sample(i,&g_mus_sh);
        g_mus_smp[i].data=g_mus_sh.data;
        MUS_Alloc_Sample(i,&g_mus_sh);
    }

    hf=g_mus.hw[6]&MUS_HF_CHCUT;
    if(mus_it_flags&0x01u) hf|=MUS_HF_STEREO;
    if(mus_it_flags&0x04u) hf|=MUS_HF_INSTR;
    if(mus_it_flags&0x08u) hf|=MUS_HF_LINEAR;
    if(mus_it_flags&0x10u) hf|=MUS_HF_OLDFX;
    if(mus_it_flags&0x20u) hf|=MUS_HF_COMPATG;
    if(g_mus.midi_pos!=0ul) hf|=MUS_HF_MIDICFG;

    g_mus.hw[4]=((unsigned long)it<<16)|((unsigned long)((is==0u) ? 6u : is)<<8);
    g_mus.hw[5]=((unsigned long)gv<<24)|((unsigned long)mv<<16)|((unsigned long)sep<<8)|(unsigned long)pwd;
    g_mus.hw[6]=hf;
    g_mus.hw[7]=((unsigned long)mus_it_cmwt<<16)|(unsigned long)g_mus.version;
    g_mus.hw[8]=(unsigned long)mus_it_flags|((unsigned long)special<<16);
    g_mus.hw[9]=(unsigned long)MUS_U16_At(30ul);
    g_mus.hw[20]=0ul;

    for(i=0u;i<(unsigned int)g_mus.ch;i++)
    {
        pan=MUS_U8_At(64ul+i);
        v=MUS_U8_At(128ul+i);
        g_mus.chan[i]=((unsigned long)pan<<24)|(((v>64ul) ? 64ul : v)<<8);
        if((pan&0x80u)!=0u) g_mus.chan[i]|=MUS_CF_MUTE;
        if((pan&0x7Fu)==100u)
        {
            g_mus.chan[i]|=MUS_CF_SURROUND|0x80ul;
        }
        else
        {
            v=(unsigned long)(pan&0x7Fu)*4ul;
            g_mus.chan[i]|=(v>255ul) ? 255ul : v;
        }
    }

    MUS_Pack_Name(&g_mus.hw[48],name,26u,8u);
    MUS_Pack_Name(&g_mus.hw[56],"Impulse Tracker",15u,8u);
    return 1u;
}

void MUS_IT_Emit(void)
{
    unsigned int i;

    if(g_mus.midi!=0ul)
    {
        MUS_Begin("MM",g_mus.midi);
        MUS_Seek(g_mus.midi_pos);
        for(i=0u;(i<MUS_MIDI_WORDS)&&(mus_fail==0u);i++)
        {
            unsigned long w;

            w=(unsigned long)MUS_Get()<<24;
            w|=(unsigned long)MUS_Get()<<16;
            w|=(unsigned long)MUS_Get()<<8;
            w|=(unsigned long)MUS_Get();
            MUS_Put(w);
        }
        MUS_Flush();
    }

    for(i=0u;(i<g_mus.patterns)&&(mus_fail==0u);i++)
    {
        if(g_mus_pat[i].kind==MUS_PAT_DATA)
        {
            MUS_IT_Pattern(i);
        }
    }

    for(i=1u;(i<=g_mus.instruments)&&(mus_fail==0u);i++)
    {
        MUS_IT_Instrument(i,&g_mus_ih);
        MUS_Emit_Instrument(i-1u,&g_mus_ih);
    }

    for(i=0u;(i<g_mus.samples)&&(mus_fail==0u);i++)
    {
        MUS_IT_Sample(i,&g_mus_sh);
        MUS_Emit_Sample(i,&g_mus_sh);
    }
}

/* =====================================================================
   Top level
   ===================================================================== */

/* Parses and lays out the module now open in the file system layer.
   Returns 0 with g_mus_error set if it cannot be played. */
unsigned char MUS_Prepare(unsigned char fmt)
{
    unsigned char ok;

    memset(&g_mus,0,sizeof g_mus);
    memset(g_mus_pat,0,sizeof g_mus_pat);
    memset(g_mus_smp,0,sizeof g_mus_smp);
    memset(g_mus_ins_pos,0,sizeof g_mus_ins_pos);
    memset(g_mus_ins_first,0,sizeof g_mus_ins_first);
    g_mus_error=0;
    mus_rd_fill=0u;
    mus_rd_base=0ul;
    mus_eof=0u;

    switch(fmt)
    {
    case MUS_FMT_MOD: ok=MUS_MOD_Prepare(); break;
    case MUS_FMT_S3M: ok=MUS_S3M_Prepare(); break;
    case MUS_FMT_XM:  ok=MUS_XM_Prepare();  break;
    case MUS_FMT_IT:  ok=MUS_IT_Prepare();  break;
    default:          ok=0u; g_mus_error="UNKNOWN FORMAT"; break;
    }

    if((ok==0u)||(g_mus_error!=0))
    {
        if(g_mus_error==0) g_mus_error="CANNOT READ FILE";
        return 0u;
    }
    if(g_mus.patterns==0u)
    {
        g_mus_error="NO PATTERNS";
        return 0u;
    }
    if(g_mus.end>MUS_SDRAM_END)
    {
        g_mus_error="TOO LARGE FOR SDRAM";
        return 0u;
    }

    g_mus.hw[0]=MUS_MAGIC;
    g_mus.hw[1]=((unsigned long)MUS_VERSION<<24)|((unsigned long)g_mus.fmt<<16)|
                ((unsigned long)((g_mus.fmt==MUS_FMT_S3M)||(g_mus.fmt==MUS_FMT_IT))<<8)|
                (unsigned long)g_mus.ch;
    g_mus.hw[2]=((unsigned long)g_mus.orders<<16)|(unsigned long)g_mus.patterns;
    g_mus.hw[3]=((unsigned long)g_mus.instruments<<16)|(unsigned long)g_mus.samples;
    g_mus.hw[10]=MUS_BASE+MUS_OFS_CHAN;
    g_mus.hw[11]=MUS_BASE+MUS_OFS_ORDER;
    g_mus.hw[12]=MUS_BASE+MUS_OFS_PATTAB;
    g_mus.hw[13]=g_mus.ins_tab;
    g_mus.hw[14]=g_mus.smp_tab;
    g_mus.hw[15]=g_mus.midi;
    g_mus.hw[16]=g_mus.zero;
    g_mus.hw[17]=g_mus.pat_data;
    g_mus.hw[18]=g_mus.smp_data;
    g_mus.hw[19]=g_mus.end;
    return 1u;
}

/* Header, tables, patterns, instruments, samples, then GO.  Returns 0
   with g_mus_error set if the load was cancelled (g_mus_cancelled) or the
   FPGA stopped taking data. */
unsigned char MUS_Stream(void)
{
    unsigned int i;

    mus_fail=0u;
    g_mus_cancelled=0u;
    g_mus_chunks=0ul;
    g_mus_words=0ul;
    mus_rd_fill=0u;
    mus_eof=0u;

    AVI_SPI_DMA_Init();
    g_fs_multi_read_ok=1u;

    MUS_Begin("MH",MUS_BASE);
    for(i=0u;i<MUS_HDR_WORDS;i++) MUS_Put(g_mus.hw[i]);
    MUS_Flush();

    MUS_Begin("CH",MUS_BASE+MUS_OFS_CHAN);
    for(i=0u;i<MUS_MAX_CH;i++) MUS_Put(g_mus.chan[i]);
    MUS_Flush();

    MUS_Begin("OR",MUS_BASE+MUS_OFS_ORDER);
    for(i=0u;i<g_mus.orders;i++) MUS_Put((unsigned long)g_mus.ord[i]);
    MUS_Flush();

    MUS_Begin("PT",MUS_BASE+MUS_OFS_PATTAB);
    for(i=0u;i<g_mus.patterns;i++)
    {
        MUS_Put(g_mus_pat[i].addr);
        MUS_Put((unsigned long)g_mus_pat[i].rows|
                ((g_mus_pat[i].kind==MUS_PAT_EMPTY) ? MUS_PF_EMPTY : 0ul)|
                ((g_mus_pat[i].kind==MUS_PAT_MISSING) ? (MUS_PF_EMPTY|MUS_PF_MISSING) : 0ul));
    }
    MUS_Flush();

    if(g_mus.zero!=0ul)
    {
        MUS_Begin("ZP",g_mus.zero);
        MUS_Put_Zeros((unsigned long)g_mus.zero_rows*(unsigned long)g_mus.ch*2ul);
        MUS_Flush();
    }

    switch(g_mus.fmt)
    {
    case MUS_FMT_MOD: MUS_MOD_Emit(); break;
    case MUS_FMT_S3M: MUS_S3M_Emit(); break;
    case MUS_FMT_XM:  MUS_XM_Emit();  break;
    default:          MUS_IT_Emit();  break;
    }

    if((mus_fail==0u)&&(MUS_Send_Chunk("GO",MUS_BASE,0,0u,0ul)==0u))
    {
        mus_fail=1u;
        g_mus_error="FPGA NOT RESPONDING";
    }

    g_fs_multi_read_ok=0u;
    return (unsigned char)(mus_fail==0u);
}

/* Where the module sits in the player's folder, and its file name, for the
   FPGA's display.  Call between MUS_Prepare() and MUS_Stream(). */
void MUS_Set_Track(const char *file_name,unsigned int index,unsigned int count)
{
    char name[64];
    unsigned int i;

    for(i=0u;i<64u;i++)
    {
        name[i]=file_name[i];
        if(name[i]==0)
        {
            break;
        }
    }
    for(;i<64u;i++)
    {
        name[i]=0;
    }

    g_mus.hw[MUS_HW_TRACK]=((unsigned long)index<<16)|(unsigned long)count;
    MUS_Pack_Name(&g_mus.hw[MUS_HW_FILE],name,64u,16u);
}

/* Playback control while the module runs on the FPGA. */
unsigned char MUS_Control(unsigned char cmd,unsigned int arg)
{
    return MUS_Send_Chunk("CT",((unsigned long)arg<<8)|(unsigned long)cmd,0,0u,0ul);
}

#endif
