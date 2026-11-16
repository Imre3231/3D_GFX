#ifndef AUDIO_H
#define AUDIO_H

/* ---------------------------------------------------------------------
   MP3, FLAC and WAV files for the FPGA music player.  The FPGA's CPU
   decodes them; the PIC32 sends a header (AH: format, sizes, tags), GO,
   then the file's audio data as it is, in AD chunks of 256 words into a
   ring in the SDRAM (README_MUSIC.md, audio streams):

     MP3   from the byte after an ID3v2 tag to the end, without an ID3v1 or
           APE tag there
     FLAC  'fLaC', STREAMINFO (marked the last block) and the frames: the
           other metadata (pictures, tags, seek table) stays behind
     WAV   the whole file

   The FPGA holds DREQ low while the ring is full, so the data goes out as
   the song plays, and lets the last chunks in without a hold: once all of
   it is out, DREQ low for 20 ms is the end of the song, as for a module.
   fw/host/audio_stream.py builds the same stream on a PC.
   --------------------------------------------------------------------- */

#define AUD_BASE         0x100000ul    /* header, SDRAM word address        */
#define AUD_RING         0x200000ul    /* the ring                          */
#define AUD_RING_WORDS   0x40000ul     /* 1 MB                              */
#define AUD_CHUNK_WORDS  256u          /* AD payload                        */
#define AUD_MAGIC        0x41554450ul  /* 'AUDP'                            */
#define AUD_VERSION      1u

#define AUD_FMT_MP3      1u
#define AUD_FMT_FLAC     2u
#define AUD_FMT_WAV      3u

/* Header words (the rest as a module's: MUS_HW_TRACK, MUS_HW_FILE) */
#define AUD_HW_FORMAT    1u            /* version << 24 | format << 16      */
#define AUD_HW_BYTES     2u            /* stream bytes                      */
#define AUD_HW_RING      3u
#define AUD_HW_RING_LEN  4u            /* ring words                        */
#define AUD_HW_FILE_LEN  5u            /* the file's bytes                  */
#define AUD_HW_TITLE     40u           /* 8 words, 32 bytes each            */
#define AUD_HW_ARTIST    48u
#define AUD_HW_ALBUM     56u

#define AUD_FLAC_FRAME   40956ul       /* the FPGA's largest FLAC frame     */
#define AUD_TEXT_LEN     32u           /* tag characters kept               */
#define AUD_TAG_READ     256u          /* bytes of a tag read for its text  */
#define AUD_DREQ_MS      10000ul       /* held low longer: not responding   */

#define AUD_SENT         0u            /* AUD_Stream: all of it out         */
#define AUD_FAILED       0xFFu         /*   the FPGA took no more data      */

/* The player's key reader (newmain.c): a new key press, 0 if none.
   AUD_Stream returns it when one stops the stream. */
unsigned char AUD_Poll_Key(void);
unsigned long Ticks_Now(void);
unsigned long Ticks_Elapsed_Ms(unsigned long start);

typedef struct
{
    unsigned char fmt;
    unsigned long file_size;
    unsigned char pre[42];        /* FLAC: 'fLaC' and STREAMINFO           */
    unsigned int  pre_n;
    unsigned long from;           /* then file bytes from .. to-1           */
    unsigned long to;
    unsigned long total;          /* stream bytes                           */
    unsigned char have;           /* tags found: 1 title, 2 artist, 4 album */
    char text[3][AUD_TEXT_LEN+1u];
    unsigned long hw[MUS_HDR_WORDS];
} AUD_STATE;

static AUD_STATE g_aud;

/* ------------------------ reading ------------------------------------ */

static unsigned char AUD_File(unsigned long pos,unsigned char *dst,unsigned int n)
{
    memset(dst,0,n);
    if(pos>=g_aud.file_size)
    {
        return 0u;
    }
    if((unsigned long)n>(g_aud.file_size-pos))
    {
        n=(unsigned int)(g_aud.file_size-pos);
    }
    return FS_Read_CurrentFile_Bytes(pos,dst,n);
}

static unsigned long AUD_BE32(const unsigned char *b)
{
    return ((unsigned long)b[0]<<24)|((unsigned long)b[1]<<16)|((unsigned long)b[2]<<8)|(unsigned long)b[3];
}

static unsigned long AUD_Syncsafe(const unsigned char *b)
{
    return ((unsigned long)(b[0] & 0x7Fu)<<21)|((unsigned long)(b[1] & 0x7Fu)<<14)|
           ((unsigned long)(b[2] & 0x7Fu)<<7)|(unsigned long)(b[3] & 0x7Fu);
}

/* ------------------------ tags --------------------------------------- */

/* A character (code point) of tag t's text: accents folded as in file
   names, tabs and line ends as spaces, leading spaces dropped, 32 kept. */
static void AUD_Put(unsigned int t,unsigned long ch)
{
    char *s;
    unsigned int n;
    char c;

    s=g_aud.text[t];
    n=(unsigned int)strlen(s);
    c=((ch==9ul)||(ch==10ul)||(ch==13ul)) ? ' ' : FS_Fold(ch);
    if((n<AUD_TEXT_LEN)&&((n!=0u)||(c!=' ')))
    {
        s[n]=c;
        s[n+1u]=0;
    }
}

/* Tag t from n text bytes, up to a NUL: enc 0 Latin-1, 1 UTF-16 after a
   byte order mark (little-endian without), 2 UTF-16 big-endian, 3 UTF-8. */
static void AUD_Text(unsigned int t,const unsigned char *b,unsigned int n,unsigned char enc)
{
    unsigned long ch;
    unsigned int i;
    unsigned int k;
    unsigned char be;

    g_aud.text[t][0]=0;
    g_aud.have|=(unsigned char)(1u<<t);
    i=0u;

    if((enc==1u)||(enc==2u))
    {
        be=(unsigned char)(enc==2u);
        if((enc==1u)&&(n>=2u)&&(((b[0]==0xFEu)&&(b[1]==0xFFu))||((b[0]==0xFFu)&&(b[1]==0xFEu))))
        {
            be=(unsigned char)(b[0]==0xFEu);
            i=2u;
        }
        for(;(i+1u)<n;i+=2u)
        {
            ch=(be!=0u) ? (((unsigned long)b[i]<<8)|b[i+1u]) : (((unsigned long)b[i+1u]<<8)|b[i]);
            if(ch==0ul)
            {
                break;
            }
            if((ch>=0xD800ul)&&(ch<0xDC00ul))
            {
                i+=2u;                      /* a pair: one character, past Latin */
            }
            AUD_Put(t,ch);
        }
        return;
    }

    while(i<n)
    {
        ch=b[i];
        i++;
        if(ch==0ul)
        {
            break;
        }
        if((enc!=0u)&&(ch>=0x80ul))
        {
            k=(ch>=0xF0ul) ? 3u : (ch>=0xE0ul) ? 2u : (ch>=0xC0ul) ? 1u : 0u;
            ch&=(k==3u) ? 0x07ul : (k==2u) ? 0x0Ful : 0x1Ful;
            for(;(k!=0u)&&(i<n)&&((b[i] & 0xC0u)==0x80u);k--,i++)
            {
                ch=(ch<<6)|(unsigned long)(b[i] & 0x3Fu);
            }
            if((k!=0u)||(ch<0x80ul))
            {
                ch=0xFFFDul;                /* not UTF-8 */
            }
        }
        AUD_Put(t,ch);
    }
}

/* Tag t from n bytes at file position pos. */
static void AUD_Text_At(unsigned int t,unsigned long pos,unsigned long n,unsigned char enc)
{
    unsigned char b[AUD_TAG_READ];

    if(n>AUD_TAG_READ)
    {
        n=AUD_TAG_READ;
    }
    (void)AUD_File(pos,b,(unsigned int)n);
    AUD_Text(t,b,(unsigned int)n,enc);
}

/* An ID3v2 tag at pos 0: its length (0: none), and the first title, artist
   and album frames of it. */
static unsigned long AUD_ID3v2(void)
{
    static const char *const id[6]={"TIT2","TPE1","TALB","TT2","TP1","TAL"};
    unsigned char b[10];
    unsigned long size;
    unsigned long end;
    unsigned long p;
    unsigned long n;
    unsigned int hl;
    unsigned int i;
    unsigned char ver;
    unsigned char flags;

    (void)AUD_File(0ul,b,10u);
    if((memcmp(b,"ID3",3)!=0)||((b[5] & 0x0Fu)!=0u)||(((b[6]|b[7]|b[8]|b[9]) & 0x80u)!=0u))
    {
        return 0ul;
    }

    ver=b[3];
    flags=b[5];
    size=AUD_Syncsafe(&b[6]);
    end=10ul+size;
    if(end>g_aud.file_size)
    {
        end=g_aud.file_size;
    }
    p=10ul;
    if(((flags & 0x40u)!=0u)&&(ver>=3u))     /* extended header */
    {
        (void)AUD_File(p,b,4u);
        p+=(ver==3u) ? (AUD_BE32(b)+4ul) : AUD_Syncsafe(b);
    }

    hl=(ver==2u) ? 6u : 10u;
    while((p+hl)<=end)
    {
        (void)AUD_File(p,b,hl);
        if(ver==2u)
        {
            n=((unsigned long)b[3]<<16)|((unsigned long)b[4]<<8)|(unsigned long)b[5];
        }
        else
        {
            n=(ver==4u) ? AUD_Syncsafe(&b[4]) : AUD_BE32(&b[4]);
        }
        p+=hl;
        if(((b[0]|b[1]|b[2]|((ver==2u) ? 0u : b[3]))==0u)||(n==0ul)||(n>(end-p)))
        {
            break;
        }

        for(i=0u;i<3u;i++)
        {
            if((memcmp(b,id[(ver==2u) ? (i+3u) : i],(ver==2u) ? 3u : 4u)==0)&&((g_aud.have & (1u<<i))==0u))
            {
                (void)AUD_File(p,b,1u);
                AUD_Text_At(i,p+1ul,n-1ul,b[0]);
                break;
            }
        }
        p+=n;
    }

    return size+10ul+(((flags & 0x10u)!=0u) ? 10ul : 0ul);
}

/* A VORBIS_COMMENT block's data (n bytes at p): the first TITLE, ARTIST and
   ALBUM, any case. */
static void AUD_Vorbis(unsigned long p,unsigned long n)
{
    static const char *const key[3]={"TITLE=","ARTIST=","ALBUM="};
    unsigned char b[AUD_TAG_READ];
    unsigned long end;
    unsigned long count;
    unsigned long len;
    unsigned int k;
    unsigned int i;
    unsigned int t;

    end=p+n;
    (void)AUD_File(p,b,4u);
    len=(unsigned long)b[0]|((unsigned long)b[1]<<8)|((unsigned long)b[2]<<16)|((unsigned long)b[3]<<24);
    if(len>n)
    {
        return;
    }
    p+=4ul+len;
    (void)AUD_File(p,b,4u);
    count=(unsigned long)b[0]|((unsigned long)b[1]<<8)|((unsigned long)b[2]<<16)|((unsigned long)b[3]<<24);
    p+=4ul;

    for(;(count!=0ul)&&((p+4ul)<=end)&&(p<g_aud.file_size);count--)
    {
        (void)AUD_File(p,b,4u);
        len=(unsigned long)b[0]|((unsigned long)b[1]<<8)|((unsigned long)b[2]<<16)|((unsigned long)b[3]<<24);
        p+=4ul;
        k=(len>AUD_TAG_READ) ? AUD_TAG_READ : (unsigned int)len;
        (void)AUD_File(p,b,k);
        for(t=0u;t<3u;t++)
        {
            for(i=0u;(key[t][i]!=0)&&(i<k)&&(FS_UpperAscii(b[i])==(unsigned char)key[t][i]);i++)
            {
            }
            if((key[t][i]==0)&&((g_aud.have & (1u<<t))==0u))
            {
                AUD_Text(t,&b[i],k-i,3u);
            }
        }
        if(len>(end-p))
        {
            break;
        }
        p+=len;
    }
}

/* A WAV's LIST INFO: INAM, IART, IPRD (the last of each). */
static void AUD_WAV_Tags(void)
{
    static const char *const sub[3]={"INAM","IART","IPRD"};
    unsigned char b[12];
    unsigned long p;
    unsigned long q;
    unsigned long n;
    unsigned long m;
    unsigned int t;

    for(p=12ul;(p+8ul)<=g_aud.file_size;p+=8ul+n+(n & 1ul))
    {
        (void)AUD_File(p,b,12u);
        n=(unsigned long)b[4]|((unsigned long)b[5]<<8)|((unsigned long)b[6]<<16)|((unsigned long)b[7]<<24);
        if(n>g_aud.file_size)
        {
            break;
        }
        if((memcmp(b,"LIST",4)!=0)||(memcmp(&b[8],"INFO",4)!=0))
        {
            continue;
        }
        for(q=p+12ul;(q+8ul)<=(p+8ul+n);q+=8ul+m+(m & 1ul))
        {
            (void)AUD_File(q,b,8u);
            m=(unsigned long)b[4]|((unsigned long)b[5]<<8)|((unsigned long)b[6]<<16)|((unsigned long)b[7]<<24);
            if(m>n)
            {
                break;
            }
            for(t=0u;t<3u;t++)
            {
                if(memcmp(b,sub[t],4)==0)
                {
                    AUD_Text_At(t,q+8ul,m,0u);
                }
            }
        }
    }
}

/* An ID3v1 tag in the 128 bytes at pos. */
static unsigned char AUD_ID3v1(unsigned long pos)
{
    unsigned char b[128];
    unsigned int t;

    (void)AUD_File(pos,b,128u);
    if(memcmp(b,"TAG",3)!=0)
    {
        return 0u;
    }
    if(g_aud.have==0u)
    {
        for(t=0u;t<3u;t++)
        {
            AUD_Text(t,&b[3u+(t*30u)],30u,0u);
        }
    }
    return 1u;
}

/* ------------------------ the file ----------------------------------- */

/* FLAC from body on: STREAMINFO (34 bytes) into pre[], marked the last
   block; the frames' start; tags from a VORBIS_COMMENT, which replace the
   ID3v2 ones. */
static unsigned char AUD_FLAC(unsigned long body)
{
    char id3[3][AUD_TEXT_LEN+1u];
    unsigned char b[4];
    unsigned long p;
    unsigned long n;
    unsigned char have;

    have=g_aud.have;
    memcpy(id3,g_aud.text,sizeof id3);
    memset(g_aud.text,0,sizeof g_aud.text);
    g_aud.have=0u;
    memcpy(g_aud.pre,"fLaC",4);
    g_aud.pre_n=4u;

    for(p=body+4ul;(p+4ul)<=g_aud.file_size;)
    {
        (void)AUD_File(p,b,4u);
        n=((unsigned long)b[1]<<16)|((unsigned long)b[2]<<8)|(unsigned long)b[3];
        if((b[0] & 0x7Fu)==0u)
        {
            if(n!=34ul)
            {
                return 0u;
            }
            g_aud.pre[4]=0x80u;
            memcpy(&g_aud.pre[5],&b[1],3);
            (void)AUD_File(p+4ul,&g_aud.pre[8],34u);
            g_aud.pre_n=42u;
        }
        else if((b[0] & 0x7Fu)==4u)
        {
            AUD_Vorbis(p+4ul,n);
        }
        p+=4ul+n;
        if((b[0] & 0x80u)!=0u)
        {
            break;
        }
    }

    if(g_aud.pre_n!=42u)
    {
        return 0u;
    }
    if(g_aud.have==0u)
    {
        g_aud.have=have;                    /* no comments: the ID3v2 tags stay */
        memcpy(g_aud.text,id3,sizeof id3);
    }
    g_aud.from=(p<g_aud.file_size) ? p : g_aud.file_size;
    return 1u;
}

/* The FPGA's limits (fw/target/audio.c): a FLAC or WAV it would refuse is
   refused here, before a long file goes out only to be dropped there. */
static unsigned char AUD_Check(void)
{
    const unsigned char *si;
    unsigned char b[26];
    unsigned long p;
    unsigned long n;
    unsigned long hz;
    unsigned int tag;
    unsigned int ch;
    unsigned int bits;
    unsigned int align;

    if(g_aud.fmt==AUD_FMT_FLAC)
    {
        si=&g_aud.pre[8];                   /* STREAMINFO */
        hz=((unsigned long)si[10]<<12)|((unsigned long)si[11]<<4)|((unsigned long)si[12]>>4);
        ch=((si[12]>>1) & 7u)+1u;
        bits=(((si[12] & 1u)<<4)|(si[13]>>4))+1u;
        n=((unsigned long)si[2]<<8)|(unsigned long)si[3];
        if(ch>2u)
        {
            g_mus_error="MORE THAN 2 CHANNELS";
        }
        else if(hz>96000ul)
        {
            g_mus_error="RATE ABOVE 96 KHZ";
        }
        else if((hz==0ul)||(n==0ul)||(n>8192ul)||(bits<4u))
        {
            g_mus_error="FLAC NOT SUPPORTED";
        }
        else if((((unsigned long)si[7]<<16)|((unsigned long)si[8]<<8)|(unsigned long)si[9])>AUD_FLAC_FRAME)
        {
            g_mus_error="FLAC FRAMES TOO LARGE";
        }
        return (unsigned char)(g_mus_error==0);
    }

    if(g_aud.fmt==AUD_FMT_WAV)
    {
        tag=0u;
        ch=0u;
        hz=0ul;
        bits=0u;
        align=0u;
        for(p=12ul;(p+8ul)<=g_aud.file_size;p+=8ul+n+(n & 1ul))
        {
            (void)AUD_File(p,b,8u);
            n=(unsigned long)b[4]|((unsigned long)b[5]<<8)|((unsigned long)b[6]<<16)|((unsigned long)b[7]<<24);
            if(memcmp(b,"data",4)==0)
            {
                break;
            }
            if((memcmp(b,"fmt ",4)==0)&&(n>=16ul))
            {
                (void)AUD_File(p+8ul,b,26u);
                tag=b[0]|((unsigned int)b[1]<<8);
                ch=b[2]|((unsigned int)b[3]<<8);
                hz=(unsigned long)b[4]|((unsigned long)b[5]<<8)|((unsigned long)b[6]<<16)|((unsigned long)b[7]<<24);
                align=b[12]|((unsigned int)b[13]<<8);
                bits=b[14]|((unsigned int)b[15]<<8);
                if((tag==0xFFFEu)&&(n>=26ul))
                {
                    tag=b[24]|((unsigned int)b[25]<<8);   /* extensible: the subformat's */
                }
            }
            if(n>g_aud.file_size)
            {
                break;
            }
        }
        if(tag!=1u)
        {
            g_mus_error="NOT PCM AUDIO";
        }
        else if((ch<1u)||(ch>8u)||(hz<4000ul)||(hz>192000ul)||
                ((bits!=8u)&&(bits!=16u)&&(bits!=24u)&&(bits!=32u))||(align!=(ch*bits/8u)))
        {
            g_mus_error="WAV NOT SUPPORTED";
        }
        return (unsigned char)(g_mus_error==0);
    }

    return 1u;
}

/* Opens an MP3, FLAC or WAV entry and finds its audio data and tags; the
   format is the content's, a FLAC or WAV name must have it.  Returns 0,
   with g_mus_error set, if it cannot be played. */
unsigned char AUD_Prepare(unsigned int entry)
{
    unsigned char b[12];
    unsigned long body;
    unsigned long p;
    unsigned long n;
    unsigned char named;

    named=FS_Get_Entry_Type(entry);
    memset(&g_aud,0,sizeof g_aud);
    g_mus_error=0;
    if(FS_Open_File_By_Index(entry)==0u)
    {
        g_mus_error="CANNOT OPEN FILE";
        return 0u;
    }
    g_aud.file_size=FS_Get_CurrentFile_Size();
    g_aud.to=g_aud.file_size;

    body=AUD_ID3v2();
    if(body>g_aud.file_size)
    {
        body=g_aud.file_size;
    }
    (void)AUD_File(body,b,4u);
    if(memcmp(b,"fLaC",4)==0)
    {
        g_aud.fmt=AUD_FMT_FLAC;
        if(AUD_FLAC(body)==0u)
        {
            g_mus_error="NOT A FLAC FILE";
            return 0u;
        }
    }
    else if(named==VFS_TYPE_FLAC)
    {
        g_mus_error="NOT A FLAC FILE";
        return 0u;
    }
    else
    {
        (void)AUD_File(0ul,b,12u);
        if((memcmp(b,"RIFF",4)==0)&&(memcmp(&b[8],"WAVE",4)==0))
        {
            g_aud.fmt=AUD_FMT_WAV;
            g_aud.have=0u;
            memset(g_aud.text,0,sizeof g_aud.text);
            AUD_WAV_Tags();
        }
        else if(named==VFS_TYPE_WAV)
        {
            g_mus_error="NOT A WAV FILE";
            return 0u;
        }
        else
        {
            /* MP3: an ID3v1 and then an APE tag at the end stay behind */
            g_aud.fmt=AUD_FMT_MP3;
            g_aud.from=body;
            if(((g_aud.to-body)>=128ul)&&(AUD_ID3v1(g_aud.to-128ul)!=0u))
            {
                g_aud.to-=128ul;
            }
            if((g_aud.to-body)>32ul)
            {
                p=g_aud.to-32ul;
                (void)AUD_File(p,b,8u);
                if(memcmp(b,"APETAGEX",8)==0)
                {
                    (void)AUD_File(p+12ul,b,12u);
                    n=(unsigned long)b[0]|((unsigned long)b[1]<<8)|((unsigned long)b[2]<<16)|((unsigned long)b[3]<<24);
                    if((b[11] & 0x80u)!=0u)
                    {
                        n+=32ul;                /* and its header */
                    }
                    g_aud.to=(n<(g_aud.to-body)) ? (g_aud.to-n) : body;
                }
            }
        }
    }

    if(AUD_Check()==0u)
    {
        return 0u;
    }
    g_aud.total=(unsigned long)g_aud.pre_n+(g_aud.to-g_aud.from);

    memset(g_aud.hw,0,sizeof g_aud.hw);
    g_aud.hw[0]=AUD_MAGIC;
    g_aud.hw[AUD_HW_FORMAT]=((unsigned long)AUD_VERSION<<24)|((unsigned long)g_aud.fmt<<16);
    g_aud.hw[AUD_HW_BYTES]=g_aud.total;
    g_aud.hw[AUD_HW_RING]=AUD_RING;
    g_aud.hw[AUD_HW_RING_LEN]=AUD_RING_WORDS;
    g_aud.hw[AUD_HW_FILE_LEN]=g_aud.file_size;
    MUS_Pack_Name(&g_aud.hw[AUD_HW_TITLE],g_aud.text[0],AUD_TEXT_LEN,8u);
    MUS_Pack_Name(&g_aud.hw[AUD_HW_ARTIST],g_aud.text[1],AUD_TEXT_LEN,8u);
    MUS_Pack_Name(&g_aud.hw[AUD_HW_ALBUM],g_aud.text[2],AUD_TEXT_LEN,8u);
    return 1u;
}

/* The file name and its place among the folder's songs, for the FPGA's
   display (as MUS_Set_Track). */
void AUD_Set_Track(const char *file_name,unsigned int index,unsigned int count)
{
    unsigned int n;

    for(n=0u;(n<64u)&&(file_name[n]!=0);n++)
    {
    }
    g_aud.hw[MUS_HW_TRACK]=((unsigned long)index<<16)|(unsigned long)count;
    MUS_Pack_Name(&g_aud.hw[MUS_HW_FILE],file_name,n,16u);
}

/* ------------------------ sending ------------------------------------ */

/* The stream's bytes s .. s+n-1: pre[], then the file. */
static unsigned char AUD_Read(unsigned long s,unsigned char *dst,unsigned int n)
{
    unsigned int k;

    k=0u;
    while((s<(unsigned long)g_aud.pre_n)&&(k<n))
    {
        dst[k]=g_aud.pre[s];
        k++;
        s++;
    }
    if(k==n)
    {
        return 1u;
    }
    return FS_Read_CurrentFile_Bytes(g_aud.from+(s-(unsigned long)g_aud.pre_n),&dst[k],n-k);
}

/* A key pressed while a chunk waited for DREQ, kept for AUD_Stream. */
static unsigned char aud_key=0u;

/* Bytes to the FPGA, waiting while DREQ is low: the FPGA holds it there
   while the ring is full, for as long as the song takes to make room (a
   second and more at a low bit rate), also just as a chunk starts.  A key
   is kept meanwhile.  0 if DREQ stays low AUD_DREQ_MS. */
static unsigned char AUD_Send(const unsigned char *src,unsigned int len)
{
    volatile unsigned char dummy;
    unsigned long t;
    unsigned int i;

    for(i=0u;i<len;i++)
    {
        if(DREQ==0)
        {
            t=Ticks_Now();
            while(DREQ==0)
            {
                if(aud_key==0u)
                {
                    aud_key=AUD_Poll_Key();
                }
                if(Ticks_Elapsed_Ms(t)>=AUD_DREQ_MS)
                {
                    return 0u;
                }
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

/* A chunk (MUS_Send_Chunk's layout) through AUD_Send. */
static unsigned char AUD_Chunk(const char *type,unsigned long addr,const unsigned char *b,unsigned int words)
{
    unsigned char h[16];
    unsigned long sum;
    unsigned int i;

    sum=0ul;
    for(i=0u;i<words;i++)
    {
        sum+=AUD_BE32(&b[i*4u]);
    }

    h[0]=0xA5u;
    h[1]=0x3Du;
    h[2]=(unsigned char)type[0];
    h[3]=(unsigned char)type[1];
    for(i=0u;i<4u;i++)
    {
        h[4u+i]=(unsigned char)(addr>>(24u-(i*8u)));
        h[8u+i]=(unsigned char)((unsigned long)words>>(24u-(i*8u)));
        h[12u+i]=(unsigned char)(sum>>(24u-(i*8u)));
    }

    if((AUD_Send(h,16u)==0u)||((words!=0u)&&(AUD_Send(b,words*4u)==0u)))
    {
        g_mus_error="FPGA NOT RESPONDING";
        return 0u;
    }
    return 1u;
}

/* CT STOP, for a module as for a stream: the FPGA may hold DREQ low for a
   while after a stream that a key stopped. */
unsigned char AUD_Stop(void)
{
    return AUD_Chunk("CT",(unsigned long)MUS_CT_STOP,0,0u);
}

/* The header, GO and the data, a chunk at a time as the FPGA takes them,
   keys read between chunks.  Returns AUD_SENT once all of it is out, the
   key that stopped it, or AUD_FAILED (g_mus_error). */
unsigned char AUD_Stream(void)
{
    unsigned char * const buf=avi_stream_buffer_b;
    unsigned long buf_at;     /* stream offset of buf[0] */
    unsigned long buf_n;
    unsigned long s;          /* stream bytes sent */
    unsigned long len;
    unsigned int words;
    unsigned int i;
    unsigned char key;

    AVI_SPI_DMA_Init();
    aud_key=0u;

    for(i=0u;i<MUS_HDR_WORDS;i++)
    {
        buf[i*4u]=(unsigned char)(g_aud.hw[i]>>24);
        buf[i*4u+1u]=(unsigned char)(g_aud.hw[i]>>16);
        buf[i*4u+2u]=(unsigned char)(g_aud.hw[i]>>8);
        buf[i*4u+3u]=(unsigned char)g_aud.hw[i];
    }
    if((AUD_Chunk("AH",AUD_BASE,buf,MUS_HDR_WORDS)==0u)||(AUD_Chunk("GO",AUD_BASE,0,0u)==0u))
    {
        return AUD_FAILED;
    }

    g_fs_multi_read_ok=1u;
    key=AUD_SENT;
    buf_at=0ul;
    buf_n=0ul;
    for(s=0ul;s<g_aud.total;s+=len)
    {
        if(s>=(buf_at+buf_n))
        {
            buf_at=s;
            buf_n=g_aud.total-s;
            if(buf_n>AVI_STREAM_BUF_SIZE)
            {
                buf_n=AVI_STREAM_BUF_SIZE;
            }
            if(AUD_Read(s,buf,(unsigned int)buf_n)==0u)
            {
                g_mus_error="CANNOT READ FILE";
                key=AUD_FAILED;
                break;
            }
        }

        len=buf_at+buf_n-s;
        if(len>(AUD_CHUNK_WORDS*4ul))
        {
            len=AUD_CHUNK_WORDS*4ul;
        }
        words=(unsigned int)((len+3ul)/4ul);
        for(i=(unsigned int)len;i<(words*4u);i++)
        {
            buf[(s-buf_at)+i]=0u;           /* the last word's padding */
        }

        key=(aud_key!=0u) ? aud_key : AUD_Poll_Key();
        if(key!=0u)
        {
            break;
        }
        if(AUD_Chunk("AD",AUD_RING+((s/4ul) & (AUD_RING_WORDS-1ul)),&buf[s-buf_at],words)==0u)
        {
            key=AUD_FAILED;
            break;
        }
    }

    g_fs_multi_read_ok=0u;
    return key;
}

#endif
