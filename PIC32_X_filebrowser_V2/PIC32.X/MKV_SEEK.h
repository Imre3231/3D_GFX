#ifndef MKV_SEEK_H
#define MKV_SEEK_H

/* ---------------------------------------------------------------------
   MKV timeline and seek engine.

   The PIC32 still does not demux the container - the FPGA does that.  All
   this code needs is enough EBML to answer three questions:

     how long is the file      -> Info/Duration, or the last cluster's
                                  Timecode if the muxer left Duration out
     where am I now            -> Cluster/Timecode, picked up both from the
                                  blocks being streamed and from the cluster
                                  a seek lands on
     which byte do I jump to   -> linear time/byte interpolation followed by
                                  a keyframe cluster search, then one
                                  correction pass against the timecode that
                                  was actually found

   Cluster searching reads 8 KB blocks into RAM and matches there.  The old
   byte-at-a-time scan through the 512 byte cache needed one USB transfer
   per 512 bytes, which is what made seeking feel dead.
   --------------------------------------------------------------------- */

#define MKV_ID_EBML          0x1A45DFA3ul
#define MKV_ID_SEGMENT       0x18538067ul
#define MKV_ID_INFO          0x1549A966ul
#define MKV_ID_TIMECODESCALE 0x2AD7B1ul
#define MKV_ID_DURATION      0x4489ul
#define MKV_ID_CLUSTER       0x1F43B675ul
#define MKV_ID_TIMECODE      0xE7ul
#define MKV_ID_SIMPLEBLOCK   0xA3ul
#define MKV_ID_BLOCKGROUP    0xA0ul
#define MKV_ID_CRC32         0xBFul
#define MKV_ID_TRACKS        0x1654AE6Bul
#define MKV_ID_TRACKENTRY    0xAEul
#define MKV_ID_TRACKNUMBER   0xD7ul
#define MKV_ID_TRACKTYPE     0x83ul
#define MKV_ID_CODECPRIVATE  0x63A2ul
#define MKV_ID_VOID          0xECul

#define MKV_SCAN_BLOCK       8192u
/* Interleaved files run to megabyte clusters with keyframes every second
   or third one, so the fallback search needs room to reach one. */
#define MKV_KEY_WINDOW       (8ul*1024ul*1024ul)
/* Must exceed one cluster, or the boundary after a given byte cannot be
   found at all - interleaved files run to two megabytes a cluster.  That
   also matters for correctness: the flush that walks the FPGA reader back
   to segment level depends on finding the next boundary. */
#define MKV_ANY_WINDOW       (6ul*1024ul*1024ul)
/* How far ahead of the target the search aims.  The scanner only searches
   forwards, so some lead is needed to land at or before the target - but a
   fixed 1.5 s lead costs a whole extra cluster on a fine step, turning a
   one second step into two.  The seek session narrows it to half the step;
   MKV_SEEK_MARGIN_MS is the ceiling. */
#define MKV_SEEK_MARGIN_MS   1500ul
#define MKV_SEEK_MARGIN_MIN  120ul
#define MKV_SEEK_TOL_MS      1500ul      /* close enough, stop refining     */
#define MKV_SEEK_TOL_FINE_MS 400ul       /* ...when the step itself is fine */
#define MKV_SEEK_PASSES      5u          /* probes per seek                 */

/* Step size ramp.  A tap moves MKV_SEEK_STEP_MIN_MS, which is about one
   cluster - the finest step the file allows, since keyframes only exist at
   cluster starts.  Holding reaches full speed at MKV_SEEK_HOLD_FULL_MS.

   Full speed is capped hard rather than being a fraction of the running
   time: many small steps with a visible frame at each one beats a few huge
   jumps.  Traversal comes from the repeat rate, not from the step size. */
/* Floor for a single tap when the keyframe spacing is not known yet.  Once
   the session has measured the real spacing, a tap moves exactly one
   keyframe - which is as fine as the file allows and shows every picture
   that can be previewed. */
#define MKV_SEEK_STEP_MIN_MS 2000ul
#define MKV_SEEK_STEP_MAX_MS 180000ul    /* never skip more than this      */
#define MKV_SEEK_HOLD_FULL_MS 10000ul

/* Short files get a proportionally smaller tap, so a clip is not crossed in
   one press. */
#define MKV_SEEK_MIN_DIVISOR 16ul

/* No pause between repeats.  Decoding and drawing the preview paces the
   loop on its own, and the release is debounced separately, so any wait
   here is dead time. */
#define MKV_SEEK_REPEAT_MS   0ul

/* The overlay stays up for this long after the key is released, so tapping
   repeatedly keeps one seek session rather than tearing the screen down and
   rebuilding it between presses. */
#define MKV_SEEK_LINGER_MS   1000ul

/* Time given to the decoder to produce the keyframe before the display is
   switched back to it on the way out of a seek. */
#define MKV_SEEK_RESUME_MS   80ul

/* Contact bounce must not read as a release mid-hold. */
#define MKV_SEEK_DEBOUNCE_MS 30ul

/* A step no larger than this is taken by walking cluster to cluster from
   where we already are - one 512 byte read per cluster instead of an 8 KB
   block scan.  MKV_SEEK_WALK_HOPS bounds the walk. */
#define MKV_SEEK_WALK_MS     6000ul
#define MKV_SEEK_WALK_HOPS   96u
#define MKV_TAIL_SCAN        (3ul*1024ul*1024ul)
#define MKV_UNKNOWN_SPAN     (300ul*1024ul)

/* Interleaved clusters hold hundreds of blocks, and the video keyframe is
   not necessarily among the first few, so these bounds have to be generous
   rather than the handful that a video-only file needs. */
#define MKV_KEY_PROBE        (64ul*1024ul)
#define MKV_KEY_PROBE_CHILDREN 256u
#define MKV_CLUSTER_CHILDREN 1024u

/* A cluster bigger than this is cut short when used as a seek preview so a
   single step cannot stall for a second on the SPI link. */

unsigned long g_mkv_timescale_ns=1000000ul;
unsigned long g_mkv_duration_ms=0ul;
unsigned long g_mkv_play_ms=0ul;
unsigned long g_mkv_first_cluster=0ul;
unsigned char g_mkv_have_duration=0u;
unsigned char g_mkv_truncated=0u;

/* Measured spacing between keyframes, 0 until the first pair is seen. */
unsigned long g_mkv_key_gap_ms=0ul;

/* Codec configuration: for H.264 in Matroska the SPS and PPS are not in the
   stream, they sit in the video track's CodecPrivate as an avcC record. */
#define MKV_CODEC_PRIVATE_MAX 256u

unsigned char g_mkv_codec_private[MKV_CODEC_PRIVATE_MAX];
unsigned int  g_mkv_codec_private_len=0u;
unsigned long g_mkv_video_track=0ul;
unsigned char g_mkv_nal_len_size=4u;

unsigned long g_mkv_seek_margin_ms=MKV_SEEK_MARGIN_MS;

/* Called before a seek with the size of the step being taken. */
void MKV_Set_Seek_Margin(unsigned long step_ms)
{
    unsigned long m;

    m=step_ms/2ul;
    if(m>MKV_SEEK_MARGIN_MS) m=MKV_SEEK_MARGIN_MS;
    if(m<MKV_SEEK_MARGIN_MIN) m=MKV_SEEK_MARGIN_MIN;

    g_mkv_seek_margin_ms=m;
}

/* ------------------------ EBML primitives --------------------------- */

unsigned char MKV_Read_Vint_Size(unsigned long pos,
                                 unsigned long *value,
                                 unsigned char *vint_len)
{
    unsigned char b0;
    unsigned char mask;
    unsigned char len;
    unsigned char i;
    unsigned char bx;
    unsigned long long v;
    unsigned long long all_ones;

    if(AVI_Read_Byte(pos,&b0)==0u) return 0u;
    if(b0==0u) return 0u;

    mask=0x80u;
    len=1u;
    while((len<=8u)&&((b0 & mask)==0u))
    {
        mask=(unsigned char)(mask>>1);
        len++;
    }
    if(len>8u) return 0u;

    /* ffmpeg pads Segment and Void sizes out to a full 8 byte vint even
       when the value is small, so the length says nothing about the
       magnitude.  Only an all-ones payload is the EBML "unknown size"
       marker; anything that genuinely does not fit a 32 bit file offset
       gets the same sentinel. */
    v=(unsigned long long)(b0 & (unsigned char)(mask-1u));
    for(i=1u;i<len;i++)
    {
        if(AVI_Read_Byte(pos+(unsigned long)i,&bx)==0u) return 0u;
        v=(v<<8)|(unsigned long long)bx;
    }

    all_ones=(1ull<<(7u*(unsigned int)len))-1ull;

    if((v==all_ones)||(v>=0xFFFFFFFFull))
    {
        *value=0xFFFFFFFFul;
    }
    else
    {
        *value=(unsigned long)v;
    }

    *vint_len=len;
    return 1u;
}

unsigned char MKV_Read_Element_Header(unsigned long pos,
                                      unsigned long *id,
                                      unsigned char *id_len,
                                      unsigned long *size,
                                      unsigned char *size_len)
{
    unsigned char b0;
    unsigned char mask;
    unsigned char len;
    unsigned char i;
    unsigned char bx;
    unsigned long v;

    if(AVI_Read_Byte(pos,&b0)==0u) return 0u;
    if(b0==0u) return 0u;

    mask=0x80u;
    len=1u;
    while((len<=4u)&&((b0 & mask)==0u))
    {
        mask=(unsigned char)(mask>>1);
        len++;
    }
    if(len>4u) return 0u;

    v=(unsigned long)b0;
    for(i=1u;i<len;i++)
    {
        if(AVI_Read_Byte(pos+(unsigned long)i,&bx)==0u) return 0u;
        v=(v<<8)|(unsigned long)bx;
    }
    *id=v;
    *id_len=len;

    return MKV_Read_Vint_Size(pos+(unsigned long)len,size,size_len);
}

unsigned char MKV_Read_UInt(unsigned long pos,unsigned char len,unsigned long *value)
{
    unsigned char i;
    unsigned char bx;
    unsigned long v;

    if((len==0u)||(len>4u)) return 0u;

    v=0ul;
    for(i=0u;i<len;i++)
    {
        if(AVI_Read_Byte(pos+(unsigned long)i,&bx)==0u) return 0u;
        v=(v<<8)|(unsigned long)bx;
    }

    *value=v;
    return 1u;
}

/* IEEE-754 to integer.  Duration is always well above 1.0 and well below
   2^31 in practice, so the fractional part and the sign are dropped. */
unsigned long MKV_Read_Float(unsigned long pos,unsigned char len)
{
    unsigned char b[8];
    unsigned char i;
    unsigned long exp;
    unsigned long long mant;
    int shift;

    if((len!=4u)&&(len!=8u)) return 0ul;

    for(i=0u;i<len;i++)
    {
        if(AVI_Read_Byte(pos+(unsigned long)i,&b[i])==0u) return 0ul;
    }

    if(len==4u)
    {
        exp=(((unsigned long)(b[0]&0x7Fu))<<1)|((unsigned long)b[1]>>7);
        if(exp<127ul) return 0ul;
        if(exp>158ul) return 0xFFFFFFFFul;

        mant=(unsigned long long)((((unsigned long)(b[1]&0x7Fu))<<16)
                                 |(((unsigned long)b[2])<<8)
                                 |((unsigned long)b[3]));
        mant|=(1ull<<23);
        shift=23-(int)(exp-127ul);
    }
    else
    {
        exp=(((unsigned long)(b[0]&0x7Fu))<<4)|((unsigned long)b[1]>>4);
        if(exp<1023ul) return 0ul;
        if(exp>1054ul) return 0xFFFFFFFFul;

        mant=((unsigned long long)(b[1]&0x0Fu)<<48)
            |((unsigned long long)b[2]<<40)
            |((unsigned long long)b[3]<<32)
            |((unsigned long long)b[4]<<24)
            |((unsigned long long)b[5]<<16)
            |((unsigned long long)b[6]<<8)
            |((unsigned long long)b[7]);
        mant|=(1ull<<52);
        shift=52-(int)(exp-1023ul);
    }

    if(shift>=0)
    {
        return (unsigned long)(mant>>shift);
    }

    return 0xFFFFFFFFul;
}

unsigned long MKV_Ticks_To_Ms(unsigned long ticks)
{
    unsigned long long v;

    v=(unsigned long long)ticks*(unsigned long long)g_mkv_timescale_ns;
    v/=1000000ull;

    return (unsigned long)v;
}

/* ------------------------ cluster validation ------------------------ */

/* Walks the direct children of a cluster for the first SimpleBlock and
   reports its keyframe flag.  BlockGroup-wrapped blocks are treated as
   "not confirmed key" - ffmpeg's muxer uses SimpleBlock almost always. */
/* True when this block belongs to the video track.  Before Tracks has been
   parsed the track number is unknown, and then any block has to count. */
static unsigned char MKV_Is_Video_Track(unsigned long track)
{
    if(g_mkv_video_track==0ul) return 1u;
    return (track==g_mkv_video_track) ? 1u : 0u;
}

unsigned char MKV_Cluster_First_Block_Is_Key(unsigned long cluster_data_pos)
{
    unsigned long pos;
    unsigned long window_end;
    unsigned long id;
    unsigned long size;
    unsigned char id_len;
    unsigned char size_len;
    unsigned int  iter;
    unsigned long payload_pos;
    unsigned long tn_value;
    unsigned char tn_len;
    unsigned char flags;

    pos=cluster_data_pos;
    if(pos>=File_Size) return 0u;

    window_end=pos+MKV_KEY_PROBE;
    if(window_end>File_Size) window_end=File_Size;

    for(iter=0u;iter<MKV_KEY_PROBE_CHILDREN;iter++)
    {
        if(pos>=window_end) return 0u;
        if(MKV_Read_Element_Header(pos,&id,&id_len,&size,&size_len)==0u) return 0u;

        payload_pos=pos+(unsigned long)id_len+(unsigned long)size_len;

        if(id==MKV_ID_SIMPLEBLOCK)
        {
            if(MKV_Read_Vint_Size(payload_pos,&tn_value,&tn_len)==0u) return 0u;

            /* Only the video track counts.  Audio blocks carry the keyframe
               flag on every frame, and in an interleaved file the first
               block of a cluster is usually audio - taking that at face
               value makes every cluster look like a keyframe cluster. */
            if(MKV_Is_Video_Track(tn_value)!=0u)
            {
                if(AVI_Read_Byte(payload_pos+(unsigned long)tn_len+2ul,&flags)==0u) return 0u;
                return ((flags & 0x80u)!=0u) ? 1u : 0u;
            }
        }

        if(size==0xFFFFFFFFul)
        {
            return 0u;
        }

        pos=payload_pos+size;
    }

    return 0u;
}

/* Confirms that pos really is a cluster and not a byte pattern inside
   compressed data: the first child has to be a Timecode of 1..8 bytes.
   That check also hands back the cluster's position on the timeline. */
unsigned char MKV_Check_Cluster(unsigned long pos,
                                unsigned char require_key,
                                unsigned long *data_pos,
                                unsigned long *end_pos,
                                unsigned long *cluster_ms)
{
    unsigned long size;
    unsigned char size_len;
    unsigned long dp;
    unsigned long id;
    unsigned char id_len;
    unsigned long csize;
    unsigned char csize_len;
    unsigned long ticks;
    unsigned long span;
    unsigned long tp;
    unsigned char skip;

    if(MKV_Read_Vint_Size(pos+4ul,&size,&size_len)==0u) return 0u;

    dp=pos+4ul+(unsigned long)size_len;
    if(dp>=File_Size) return 0u;

    /* ffmpeg writes a CRC-32 element (and occasionally Void padding) ahead
       of the cluster Timecode, so skip those before demanding Timecode.
       Any other leading element means this was not a real cluster header. */
    tp=dp;
    for(skip=0u;skip<4u;skip++)
    {
        if(MKV_Read_Element_Header(tp,&id,&id_len,&csize,&csize_len)==0u) return 0u;
        if(id!=MKV_ID_CRC32 && id!=MKV_ID_VOID) break;
        if(csize==0xFFFFFFFFul) return 0u;
        tp+=(unsigned long)id_len+(unsigned long)csize_len+csize;
        if(tp>=File_Size) return 0u;
    }

    if(id!=MKV_ID_TIMECODE) return 0u;
    if((csize<1ul)||(csize>8ul)) return 0u;
    if(csize>4ul) csize=4ul;

    if(MKV_Read_UInt(tp+(unsigned long)id_len+(unsigned long)csize_len,
                     (unsigned char)csize,&ticks)==0u) return 0u;

    span=(size==0xFFFFFFFFul) ? MKV_UNKNOWN_SPAN : size;

    if((require_key!=0u)&&(MKV_Cluster_First_Block_Is_Key(dp)==0u))
    {
        return 0u;
    }

    *data_pos=dp;
    *end_pos=dp+span;
    if(*end_pos>File_Size)
    {
        *end_pos=File_Size;
    }
    *cluster_ms=MKV_Ticks_To_Ms(ticks);

    return 1u;
}

/* ------------------------ block scanning ---------------------------- */

/* RAM twin of MKV_Read_Vint_Size.  Long vints are decoded in full: ffmpeg
   pads sizes out to 8 bytes even for small values, so only an all-ones
   payload (or a value too large for a 32 bit offset) gets the sentinel. */
unsigned char MKV_Vint_Ram(const unsigned char *p,unsigned long *value,unsigned char *len)
{
    unsigned char mask;
    unsigned char n;
    unsigned char i;
    unsigned long long v;
    unsigned long long all_ones;

    if(p[0]==0u) return 0u;

    mask=0x80u;
    n=1u;
    while((n<=8u)&&((p[0] & mask)==0u))
    {
        mask=(unsigned char)(mask>>1);
        n++;
    }
    if(n>8u) return 0u;

    v=(unsigned long long)(p[0] & (unsigned char)(mask-1u));
    for(i=1u;i<n;i++)
    {
        v=(v<<8)|(unsigned long long)p[i];
    }

    all_ones=(1ull<<(7u*(unsigned int)n))-1ull;

    *value=((v==all_ones)||(v>=0xFFFFFFFFull)) ? 0xFFFFFFFFul
                                               : (unsigned long)v;
    *len=n;
    return 1u;
}

/* Finds the last cluster timecode inside a RAM block.  A cluster header is
   at most 4 + 8 + 1 + 8 + 4 bytes, so matches are only accepted with that
   much margin left in the buffer; one that straddles the block boundary is
   skipped and picked up on the next block. */
#define MKV_CLUSTER_HDR_MAX 28u

unsigned char MKV_Block_Last_Timecode(const unsigned char *buf,unsigned int len,
                                      unsigned long *ms,unsigned int *offset)
{
    unsigned int i;
    unsigned int p;
    unsigned long size;
    unsigned long tc_size;
    unsigned long ticks;
    unsigned char slen;
    unsigned char tlen;
    unsigned char n;
    unsigned char found;

    found=0u;
    if(len<MKV_CLUSTER_HDR_MAX) return 0u;

    for(i=0u;(i+MKV_CLUSTER_HDR_MAX)<=len;i++)
    {
        if(buf[i]!=0x1Fu) continue;
        if(buf[i+1u]!=0x43u) continue;
        if(buf[i+2u]!=0xB6u) continue;
        if(buf[i+3u]!=0x75u) continue;

        /* cluster size vint - length only, the value is not needed here */
        p=i+4u;
        if(MKV_Vint_Ram(&buf[p],&size,&slen)==0u) continue;
        p+=(unsigned int)slen;

        /* step over a leading CRC-32 / Void before the Timecode */
        for(n=0u;n<3u;n++)
        {
            if((buf[p]!=0xBFu)&&(buf[p]!=0xECu)) break;
            p++;
            if(MKV_Vint_Ram(&buf[p],&size,&slen)==0u) break;
            p+=(unsigned int)slen+(unsigned int)size;
            if((p+8u)>len) break;
        }
        if((p+8u)>len) continue;

        if(buf[p]!=0xE7u) continue;
        p++;

        if(MKV_Vint_Ram(&buf[p],&tc_size,&tlen)==0u) continue;
        p+=(unsigned int)tlen;

        if((tc_size<1ul)||(tc_size>4ul)) continue;
        if((p+(unsigned int)tc_size)>len) continue;

        ticks=0ul;
        for(n=0u;n<(unsigned char)tc_size;n++)
        {
            ticks=(ticks<<8)|(unsigned long)buf[p+(unsigned int)n];
        }

        *ms=MKV_Ticks_To_Ms(ticks);
        *offset=i;
        found=1u;
    }

    return found;
}

/* Called from AVI_Stream_Range() for every block on its way to the FPGA. */
void MKV_Track_Block(const unsigned char *buf,unsigned int len,unsigned long file_pos)
{
    unsigned long ms;
    unsigned int off;

    (void)file_pos;

    if(MKV_Block_Last_Timecode(buf,len,&ms,&off)!=0u)
    {
        g_mkv_play_ms=ms;
    }
}

unsigned char MKV_Scan_Cluster(unsigned long start_pos,
                               unsigned long scan_limit,
                               unsigned char require_key,
                               unsigned long *cluster_pos,
                               unsigned long *cluster_data_pos,
                               unsigned long *cluster_end,
                               unsigned long *cluster_ms)
{
    unsigned char *buf;
    unsigned long pos;
    unsigned long end_pos;
    unsigned long hop;
    unsigned int take;
    unsigned int i;

    if(File_Size<16ul) return 0u;

    buf=avi_stream_buffer_b;
    pos=start_pos;

    end_pos=start_pos+scan_limit;
    if(end_pos>File_Size) end_pos=File_Size;

    g_fs_multi_read_ok=1u;

    while((pos+8ul)<end_pos)
    {
        take=MKV_SCAN_BLOCK;
        if((end_pos-pos)<(unsigned long)take)
        {
            take=(unsigned int)(end_pos-pos);
        }

        if(AVI_Read_Block(pos,buf,take)==0u)
        {
            break;
        }

        hop=0ul;

        for(i=0u;(i+4u)<=take;i++)
        {
            if(buf[i]!=0x1Fu) continue;
            if(buf[i+1u]!=0x43u) continue;
            if(buf[i+2u]!=0xB6u) continue;
            if(buf[i+3u]!=0x75u) continue;

            AVI_SeekCache_Reset();
            if(MKV_Check_Cluster(pos+(unsigned long)i,0u,
                                 cluster_data_pos,cluster_end,cluster_ms)==0u)
            {
                continue;
            }

            if((require_key==0u)||
               (MKV_Cluster_First_Block_Is_Key(*cluster_data_pos)!=0u))
            {
                *cluster_pos=pos+(unsigned long)i;
                g_fs_multi_read_ok=0u;
                return 1u;
            }

            /* A real cluster, just not a keyframe one.  Its own size says
               where the next cluster starts, so hop there instead of
               reading through the payload looking for the next magic -
               with megabyte clusters that is the whole cost of a seek. */
            if(*cluster_end>(pos+(unsigned long)i))
            {
                hop=*cluster_end;
                break;
            }
        }

        if(hop!=0ul)
        {
            pos=hop;
            continue;
        }

        if(take<8u) break;
        pos+=(unsigned long)(take-3u);
    }

    g_fs_multi_read_ok=0u;
    return 0u;
}

/* ------------------------ file timeline ----------------------------- */

unsigned long MKV_Body_Bytes(void)
{
    if(File_Size>g_mkv_first_cluster)
    {
        return File_Size-g_mkv_first_cluster;
    }

    return File_Size;
}

/* Converts a duration into the number of body bytes it occupies, using the
   file's average rate.  Used for relative corrections, where an absolute
   byte position would throw away what the last probe already told us. */
unsigned long MKV_Ms_To_Bytes(unsigned long ms)
{
    unsigned long long v;

    if((g_mkv_have_duration==0u)||(g_mkv_duration_ms==0ul)) return 0ul;

    if(ms>g_mkv_duration_ms) ms=g_mkv_duration_ms;

    v=(unsigned long long)MKV_Body_Bytes()*(unsigned long long)ms;
    v/=(unsigned long long)g_mkv_duration_ms;

    return (unsigned long)v;
}

unsigned long MKV_Time_To_Byte(unsigned long ms)
{
    unsigned long long v;

    if((g_mkv_have_duration==0u)||(g_mkv_duration_ms==0ul))
    {
        return g_mkv_first_cluster;
    }

    if(ms>g_mkv_duration_ms) ms=g_mkv_duration_ms;

    v=(unsigned long long)MKV_Body_Bytes()*(unsigned long long)ms;
    v/=(unsigned long long)g_mkv_duration_ms;

    return g_mkv_first_cluster+(unsigned long)v;
}

unsigned long MKV_Byte_To_Time(unsigned long pos)
{
    unsigned long long v;
    unsigned long body;

    body=MKV_Body_Bytes();
    if((g_mkv_have_duration==0u)||(body==0ul)) return 0ul;
    if(pos<=g_mkv_first_cluster) return 0ul;

    v=(unsigned long long)(pos-g_mkv_first_cluster)*(unsigned long long)g_mkv_duration_ms;
    v/=(unsigned long long)body;

    return (unsigned long)v;
}

void MKV_Parse_Info(unsigned long info_pos,unsigned long info_size)
{
    unsigned long pos;
    unsigned long end_pos;
    unsigned long id;
    unsigned long size;
    unsigned char id_len;
    unsigned char size_len;
    unsigned long payload;
    unsigned long dur_ticks;
    unsigned char iter;

    pos=info_pos;
    end_pos=info_pos+info_size;
    if(end_pos>File_Size) end_pos=File_Size;
    dur_ticks=0ul;

    for(iter=0u;iter<32u;iter++)
    {
        if(pos>=end_pos) break;
        if(MKV_Read_Element_Header(pos,&id,&id_len,&size,&size_len)==0u) break;

        payload=pos+(unsigned long)id_len+(unsigned long)size_len;

        if((id==MKV_ID_TIMECODESCALE)&&(size>=1ul)&&(size<=4ul))
        {
            (void)MKV_Read_UInt(payload,(unsigned char)size,&g_mkv_timescale_ns);
        }
        else if(id==MKV_ID_DURATION)
        {
            dur_ticks=MKV_Read_Float(payload,(unsigned char)size);
        }

        if(size==0xFFFFFFFFul) break;
        pos=payload+size;
    }

    if(g_mkv_timescale_ns==0ul)
    {
        g_mkv_timescale_ns=1000000ul;
    }

    if(dur_ticks!=0ul)
    {
        g_mkv_duration_ms=MKV_Ticks_To_Ms(dur_ticks);
        if(g_mkv_duration_ms!=0ul)
        {
            g_mkv_have_duration=1u;
        }
    }
}

/* Muxers that write the file in one pass often leave Duration out.  The
   timecode of the last cluster is a good stand-in. */
unsigned long MKV_Duration_From_Tail(void)
{
    unsigned char *buf;
    unsigned long pos;
    unsigned long best;
    unsigned long ms;
    unsigned int take;
    unsigned int off;

    if(File_Size<64ul) return 0ul;

    buf=avi_stream_buffer_b;
    best=0ul;

    pos=(File_Size>MKV_TAIL_SCAN) ? (File_Size-MKV_TAIL_SCAN) : 0ul;

    g_fs_multi_read_ok=1u;

    while(pos<File_Size)
    {
        take=MKV_SCAN_BLOCK;
        if((File_Size-pos)<(unsigned long)take)
        {
            take=(unsigned int)(File_Size-pos);
        }

        if(AVI_Read_Block(pos,buf,take)==0u) break;

        if(MKV_Block_Last_Timecode(buf,take,&ms,&off)!=0u)
        {
            if(ms>best) best=ms;
        }

        if(take<=MKV_CLUSTER_HDR_MAX) break;
        pos+=(unsigned long)(take-MKV_CLUSTER_HDR_MAX);
    }

    g_fs_multi_read_ok=0u;

    return best;
}

/* ------------------- cluster boundary cache --------------------------
   Hopping cluster to cluster is cheap - a cluster's size says where the
   next one starts - but only from a boundary that is already known.  An
   interpolated byte position lands in the middle of a cluster, and finding
   the next boundary from there means reading through the rest of its
   payload; with megabyte clusters that is most of the cost of a seek
   backwards.  So boundaries are remembered as they are discovered, and a
   seek starts from the nearest one behind its target. */

#define MKV_CC_SLOTS    32u
#define MKV_CC_HOPS     96u      /* hop budget before falling back        */

unsigned long g_cc_pos[MKV_CC_SLOTS];
unsigned long g_cc_end[MKV_CC_SLOTS];
unsigned long g_cc_ms[MKV_CC_SLOTS];
unsigned char g_cc_count=0u;

void MKV_Cache_Reset(void)
{
    g_cc_count=0u;
}

void MKV_Cache_Add(unsigned long pos,unsigned long end,unsigned long ms)
{
    unsigned char i;
    unsigned char worst;
    unsigned long gap;
    unsigned long best_gap;

    for(i=0u;i<g_cc_count;i++)
    {
        if(g_cc_pos[i]==pos) return;
    }

    if(g_cc_count<MKV_CC_SLOTS)
    {
        g_cc_pos[g_cc_count]=pos;
        g_cc_end[g_cc_count]=end;
        g_cc_ms[g_cc_count]=ms;
        g_cc_count++;
        return;
    }

    /* Full: drop whichever entry sits closest to another, so what is kept
       stays spread across the file rather than bunched where the user
       happened to be stepping. */
    worst=0u;
    best_gap=0xFFFFFFFFul;

    for(i=0u;i<g_cc_count;i++)
    {
        gap=(g_cc_ms[i]>ms) ? (g_cc_ms[i]-ms) : (ms-g_cc_ms[i]);
        if(gap<best_gap)
        {
            best_gap=gap;
            worst=i;
        }
    }

    g_cc_pos[worst]=pos;
    g_cc_end[worst]=end;
    g_cc_ms[worst]=ms;
}

/* Nearest known boundary at or before a time.  The first cluster is always
   available as a fallback anchor. */
static unsigned char MKV_Cache_Anchor(unsigned long target_ms,
                                      unsigned long *pos,unsigned long *ms)
{
    unsigned char i;
    unsigned char found;

    *pos=g_mkv_first_cluster;
    *ms=0ul;
    found=(g_mkv_first_cluster>0ul) ? 1u : 0u;

    for(i=0u;i<g_cc_count;i++)
    {
        if((g_cc_ms[i]<=target_ms)&&(g_cc_ms[i]>=*ms))
        {
            *pos=g_cc_pos[i];
            *ms=g_cc_ms[i];
            found=1u;
        }
    }

    return found;
}

/* The last keyframe cluster at or before a time, by hopping from the
   nearest known boundary.  Returns 0 if the budget ran out first, which
   leaves the caller to fall back to the interpolating search. */
unsigned char MKV_Key_Cluster_At_Or_Before(unsigned long target_ms,
                                           unsigned long *cluster_pos,
                                           unsigned long *cluster_end,
                                           unsigned long *landed_ms)
{
    unsigned long pos;
    unsigned long anchor_ms;
    unsigned long dp;
    unsigned long ce;
    unsigned long ms;
    unsigned long best_pos;
    unsigned long best_end;
    unsigned long best_ms;
    unsigned int  hop;
    unsigned char have;

    if(MKV_Cache_Anchor(target_ms,&pos,&anchor_ms)==0u) return 0u;

    have=0u;
    best_pos=0ul; best_end=0ul; best_ms=0ul;

    for(hop=0u;hop<MKV_CC_HOPS;hop++)
    {
        if(pos>=File_Size) break;

        if(MKV_Check_Cluster(pos,0u,&dp,&ce,&ms)==0u) break;

        if(ms>target_ms)
        {
            if(have!=0u)
            {
                MKV_Cache_Add(best_pos,best_end,best_ms);
                *cluster_pos=best_pos;
                *cluster_end=best_end;
                *landed_ms=best_ms;
                return 1u;
            }
            break;
        }

        if(MKV_Cluster_First_Block_Is_Key(dp)!=0u)
        {
            best_pos=pos; best_end=ce; best_ms=ms;
            have=1u;
            MKV_Cache_Add(pos,ce,ms);   /* better anchors for next time */
        }

        if(ce<=pos) break;
        pos=ce;
    }

    return 0u;
}

/* ---------------------- codec configuration --------------------------
   The SPS and PPS a software decoder needs are not in the stream: for
   H.264 in Matroska they sit in the video track's CodecPrivate element as
   an avcC record.  Parsed once when the file is opened. */


/* Walks one TrackEntry, keeping the video track's CodecPrivate. */
static unsigned char MKV_Parse_TrackEntry(unsigned long pos,unsigned long end)
{
    unsigned long id,size,payload,v;
    unsigned char id_len,size_len;
    unsigned long track_no;
    unsigned long track_type;
    unsigned long cp_pos;
    unsigned int  cp_len;
    unsigned int  i;
    unsigned char child;

    track_no=0ul;
    track_type=0ul;
    cp_pos=0ul;
    cp_len=0u;

    for(child=0u;child<32u;child++)
    {
        if(pos>=end) break;
        if(MKV_Read_Element_Header(pos,&id,&id_len,&size,&size_len)==0u) break;
        if(size==0xFFFFFFFFul) break;

        payload=pos+(unsigned long)id_len+(unsigned long)size_len;

        if(id==MKV_ID_TRACKNUMBER)
        {
            if(MKV_Read_UInt(payload,(unsigned char)size,&v)!=0u) track_no=v;
        }
        else if(id==MKV_ID_TRACKTYPE)
        {
            if(MKV_Read_UInt(payload,(unsigned char)size,&v)!=0u) track_type=v;
        }
        else if(id==MKV_ID_CODECPRIVATE)
        {
            cp_pos=payload;
            cp_len=(size<(unsigned long)MKV_CODEC_PRIVATE_MAX)
                   ? (unsigned int)size : MKV_CODEC_PRIVATE_MAX;
        }

        pos=payload+size;
    }

    if((track_type!=1ul)||(cp_len==0u)) return 0u;

    for(i=0u;i<cp_len;i++)
    {
        if(AVI_Read_Byte(cp_pos+(unsigned long)i,&g_mkv_codec_private[i])==0u) return 0u;
    }

    g_mkv_codec_private_len=cp_len;
    g_mkv_video_track=track_no;

    /* avcC: byte 4 holds lengthSizeMinusOne in its low two bits. */
    if((cp_len>=5u)&&(g_mkv_codec_private[0]==1u))
    {
        g_mkv_nal_len_size=(unsigned char)((g_mkv_codec_private[4]&3u)+1u);
    }

    return 1u;
}

/* Walks the TrackEntry list; stops at the first video track found. */
unsigned char MKV_Parse_TrackList(unsigned long pos,unsigned long size)
{
    unsigned long tend;
    unsigned char e;

    tend=pos+size;

    for(e=0u;e<16u;e++)
    {
        unsigned long eid,esize,epay;
        unsigned char eid_len,esize_len;

        if(pos>=tend) break;
        if(MKV_Read_Element_Header(pos,&eid,&eid_len,&esize,&esize_len)==0u) break;
        if(esize==0xFFFFFFFFul) break;

        epay=pos+(unsigned long)eid_len+(unsigned long)esize_len;

        if(eid==MKV_ID_TRACKENTRY)
        {
            if(MKV_Parse_TrackEntry(epay,epay+esize)!=0u) return 1u;
        }

        pos=epay+esize;
    }

    return 0u;
}

/* Reads TimecodeScale/Duration and locates the first cluster.  Returns 1
   when a usable timeline was established. */
unsigned char MKV_Open_Timeline(void)
{
    unsigned long pos;
    unsigned long id;
    unsigned long size;
    unsigned char id_len;
    unsigned char size_len;
    unsigned long payload;
    unsigned char iter;
    unsigned long dummy_dp;
    unsigned long dummy_end;
    unsigned long dummy_ms;
    unsigned long tail_ms;
    unsigned long seg_end;

    g_mkv_timescale_ns=1000000ul;
    g_mkv_duration_ms=0ul;
    g_mkv_play_ms=0ul;
    g_mkv_have_duration=0u;
    g_mkv_first_cluster=0ul;
    g_mkv_codec_private_len=0u;
    g_mkv_video_track=0ul;
    g_mkv_nal_len_size=4u;
    g_mkv_key_gap_ms=0ul;
    MKV_Cache_Reset();

    AVI_SeekCache_Reset();

    pos=0ul;
    if(MKV_Read_Element_Header(pos,&id,&id_len,&size,&size_len)==0u) return 0u;
    if(id!=MKV_ID_EBML) return 0u;
    if(size==0xFFFFFFFFul) return 0u;

    pos=pos+(unsigned long)id_len+(unsigned long)size_len+size;

    if(MKV_Read_Element_Header(pos,&id,&id_len,&size,&size_len)==0u) return 0u;
    if(id!=MKV_ID_SEGMENT) return 0u;

    pos=pos+(unsigned long)id_len+(unsigned long)size_len;

    /* Where the Segment claims to end.  A partial copy keeps the original
       header, so this reaching past the end of the file is what tells us
       the rest was never written. */
    seg_end=(size==0xFFFFFFFFul) ? 0ul : (pos+size);

    for(iter=0u;iter<48u;iter++)
    {
        if(pos>=File_Size) break;
        if(MKV_Read_Element_Header(pos,&id,&id_len,&size,&size_len)==0u) break;

        payload=pos+(unsigned long)id_len+(unsigned long)size_len;

        if(id==MKV_ID_CLUSTER)
        {
            g_mkv_first_cluster=pos;
            break;
        }

        if(id==MKV_ID_INFO)
        {
            MKV_Parse_Info(payload,size);
        }

        if((id==MKV_ID_TRACKS)&&(size!=0xFFFFFFFFul))
        {
            (void)MKV_Parse_TrackList(payload,size);
        }

        if(size==0xFFFFFFFFul) break;
        pos=payload+size;
    }

    if(g_mkv_first_cluster==0ul)
    {
        if(MKV_Scan_Cluster(0ul,2ul*1024ul*1024ul,0u,
                            &g_mkv_first_cluster,&dummy_dp,&dummy_end,&dummy_ms)==0u)
        {
            g_mkv_first_cluster=0ul;
        }
    }

    /* Truncated file: the header still describes the whole recording, so
       its duration is far longer than what is actually here.  Scale it by
       the fraction of the body present rather than scanning for the last
       timecode, which would mean reading megabytes at every file open. */
    g_mkv_truncated=0u;

    if((seg_end>File_Size)&&(g_mkv_have_duration!=0u)&&
       (g_mkv_first_cluster<File_Size)&&(seg_end>g_mkv_first_cluster))
    {
        unsigned long long v;

        g_mkv_truncated=1u;

        v=(unsigned long long)g_mkv_duration_ms
         *(unsigned long long)(File_Size-g_mkv_first_cluster);
        v/=(unsigned long long)(seg_end-g_mkv_first_cluster);

        g_mkv_duration_ms=(unsigned long)v;
        if(g_mkv_duration_ms==0ul) g_mkv_have_duration=0u;
    }

    if(g_mkv_have_duration==0u)
    {
        tail_ms=MKV_Duration_From_Tail();
        if(tail_ms>0ul)
        {
            /* The last cluster still plays out after its own timecode. */
            g_mkv_duration_ms=tail_ms+2000ul;
            g_mkv_have_duration=1u;
        }
    }

    return g_mkv_have_duration;
}

/* ------------------------ seeking ----------------------------------- */

/* Hold time to speed level.  Level 8, the fastest, is reached after the
   10 second hold, where one repeat covers a quarter of the file. */
/* Finds where the cluster the stream is currently inside ends.

   A seek key aborts the stream mid-buffer, so the reader is left partway
   through a SimpleBlock, waiting for the rest of its payload.  Anything
   sent next is swallowed as payload bytes - and even at a child boundary a
   Cluster element found inside a cluster is skipped by the reader's
   S_GENERIC_SKIP, because a cluster is not a legal cluster child.  So the
   reader has to be walked out to segment level before a preview can be
   parsed at all.

   Clusters are contiguous, so the next cluster header is the end of the
   one we are in; streaming the real bytes up to it takes the reader
   through the rest of the current cluster exactly as it expected. */
unsigned char MKV_Cluster_Flush_Target(unsigned long pos,unsigned long *flush_to)
{
    unsigned long cp;
    unsigned long dp;
    unsigned long ce;
    unsigned long ms;

    if(pos>=File_Size) return 0u;

    AVI_SeekCache_Reset();
    if(MKV_Scan_Cluster(pos,MKV_ANY_WINDOW,0u,&cp,&dp,&ce,&ms)==0u)
    {
        return 0u;
    }

    if(cp<=pos) return 0u;

    *flush_to=cp;
    return 1u;
}

/* Walks forward cluster by cluster from a known cluster boundary, keeping
   the last keyframe cluster at or before the target.  A cluster's own size
   gives the next cluster's offset, so each hop is a single 512 byte read
   rather than a block scan - much cheaper than a full seek for the small
   steps that fine stepping is made of.

   Returns 2 when it reached the target, 1 when it ran out of hops first
   (the caller should fall back to MKV_Seek_To_Ms), 0 when it found nothing. */
unsigned char MKV_Walk_Forward(unsigned long from_pos,
                               unsigned long target_ms,
                               unsigned long *cluster_pos,
                               unsigned long *cluster_end,
                               unsigned long *landed_ms)
{
    unsigned long pos;
    unsigned long dp;
    unsigned long ce;
    unsigned long ms;
    unsigned long best_pos;
    unsigned long best_end;
    unsigned long best_ms;
    unsigned char have;
    unsigned char done;
    unsigned char hop;

    pos=from_pos;
    have=0u;
    done=0u;
    best_pos=0ul; best_end=0ul; best_ms=0ul;

    for(hop=0u;hop<MKV_SEEK_WALK_HOPS;hop++)
    {
        if(pos>=File_Size) { done=1u; break; }

        AVI_SeekCache_Reset();
        if(MKV_Check_Cluster(pos,0u,&dp,&ce,&ms)==0u) break;

        if(ms>target_ms) { done=1u; break; }

        if(MKV_Cluster_First_Block_Is_Key(dp)!=0u)
        {
            best_pos=pos;
            best_end=ce;
            best_ms=ms;
            have=1u;
        }

        if(ce<=pos) break;
        pos=ce;
    }

    if(have==0u) return 0u;

    *cluster_pos=best_pos;
    *cluster_end=best_end;
    *landed_ms=best_ms;

    return (done!=0u) ? 2u : 1u;
}

/* The next keyframe cluster at or after a cluster boundary, found by
   hopping cluster to cluster - each hop is one short read, because a
   cluster's own size says where the next one begins.  Far cheaper than
   scanning the file for the cluster magic, which would mean reading
   through megabytes of payload. */
unsigned char MKV_Next_Key_Cluster(unsigned long from_pos,
                                   unsigned long *cluster_pos,
                                   unsigned long *cluster_end,
                                   unsigned long *landed_ms)
{
    unsigned long pos;
    unsigned long dp;
    unsigned long ce;
    unsigned long ms;
    unsigned int  hop;

    pos=from_pos;

    for(hop=0u;hop<MKV_SEEK_WALK_HOPS;hop++)
    {
        if(pos>=File_Size) break;

        if(MKV_Check_Cluster(pos,0u,&dp,&ce,&ms)==0u) break;

        if(MKV_Cluster_First_Block_Is_Key(dp)!=0u)
        {
            MKV_Cache_Add(pos,ce,ms);
            *cluster_pos=pos;
            *cluster_end=ce;
            *landed_ms=ms;
            return 1u;
        }

        if(ce<=pos) break;
        pos=ce;
    }

    return 0u;
}

/* Confirms a cluster really starts with a keyframe, and if it does not,
   finds the next one that does.  MKV_Seek_To_Ms prefers key clusters but
   falls back to any cluster when none is within the window, and resuming
   on a non-key cluster hands the decoder P-frames with no reference. */
unsigned char MKV_Ensure_Key_Cluster(unsigned long cluster_pos,
                                     unsigned long cluster_end,
                                     unsigned long *out_pos,
                                     unsigned long *out_end,
                                     unsigned long *out_ms)
{
    unsigned long dp;
    unsigned long ce;
    unsigned long ms;

    AVI_SeekCache_Reset();
    if(MKV_Check_Cluster(cluster_pos,0u,&dp,&ce,&ms)!=0u)
    {
        if(MKV_Cluster_First_Block_Is_Key(dp)!=0u)
        {
            *out_pos=cluster_pos;
            *out_end=ce;
            *out_ms=ms;
            return 1u;
        }
    }

    /* Look forward first - that keeps the landing at or after where the
       user asked. */
    if(cluster_end>cluster_pos)
    {
        AVI_SeekCache_Reset();
        if(MKV_Scan_Cluster(cluster_end,MKV_KEY_WINDOW,1u,
                            out_pos,&dp,out_end,out_ms)!=0u)
        {
            return 1u;
        }
    }

    /* Nothing ahead: on long-GOP material the last group has no further
       keyframe, so fall back to the nearest one behind.  The scanner only
       runs forwards, so sweep a window that ends here and keep the last
       key cluster it finds before this position. */
    {
        unsigned long win;
        unsigned long start;
        unsigned long p;
        unsigned long cp;
        unsigned long ce;
        unsigned long ms;
        unsigned long best_pos;
        unsigned long best_end;
        unsigned long best_ms;
        unsigned char have;
        unsigned char pass;

        for(pass=0u;pass<3u;pass++)
        {
            win=MKV_KEY_WINDOW<<pass;

            start=(cluster_pos>(g_mkv_first_cluster+win))
                  ? (cluster_pos-win) : g_mkv_first_cluster;

            have=0u;
            best_pos=0ul; best_end=0ul; best_ms=0ul;
            p=start;

            while(p<cluster_pos)
            {
                AVI_SeekCache_Reset();
                if(MKV_Scan_Cluster(p,cluster_pos-p,1u,&cp,&dp,&ce,&ms)==0u) break;
                if(cp>=cluster_pos) break;

                best_pos=cp; best_end=ce; best_ms=ms;
                have=1u;

                if(ce<=cp) break;
                p=ce;
            }

            if(have!=0u)
            {
                *out_pos=best_pos;
                *out_end=best_end;
                *out_ms=best_ms;
                return 1u;
            }

            if(start<=g_mkv_first_cluster) break;
        }
    }

    return 0u;
}

/* ---------------------- I-frame preview ------------------------------
   The FPGA reader gates every SimpleBlock through its frame-wait state:
   it releases one frame, waits for frame_done and the frame period, then
   takes the next.  Pushing a whole cluster therefore costs a full second
   of wall time for a 25-frame GOP, with the host stalled on DREQ the
   whole way - which is what made seeking feel like it had stopped.

   So a preview sends a synthetic single-frame cluster instead:

       1F 43 B6 75 <size>   E7 81 00   A3 <size> <keyframe block>

   The reader parses that as an ordinary cluster holding one keyframe,
   emits the IDR and decodes exactly one picture.  Sizes are written by
   the PIC32 to match what it actually sends, so the reader's element
   bookkeeping stays consistent and no resync is needed.
   -------------------------------------------------------------------- */

#define MKV_PREVIEW_HDR_MAX  32u

/* Locates the keyframe SimpleBlock in a cluster.  blk_pos/blk_len describe
   the block payload - track number vint, 16 bit timecode, flags, then the
   length-prefixed NAL units - and hdr_len is the part ahead of the NALs. */
unsigned char MKV_Find_Key_Block(unsigned long cluster_pos,
                                 unsigned long cluster_end,
                                 unsigned long *blk_pos,
                                 unsigned long *blk_len,
                                 unsigned char *hdr_len)
{
    unsigned long size;
    unsigned long csize;
    unsigned long pos;
    unsigned long payload;
    unsigned long id;
    unsigned long tn;
    unsigned char size_len;
    unsigned char id_len;
    unsigned char csize_len;
    unsigned char tn_len;
    unsigned char flags;
    unsigned int  child;

    if(MKV_Read_Vint_Size(cluster_pos+4ul,&size,&size_len)==0u) return 0u;

    pos=cluster_pos+4ul+(unsigned long)size_len;

    for(child=0u;child<MKV_CLUSTER_CHILDREN;child++)
    {
        if((pos>=cluster_end)||(pos>=File_Size)) break;
        if(MKV_Read_Element_Header(pos,&id,&id_len,&csize,&csize_len)==0u) break;
        if(csize==0xFFFFFFFFul) break;

        payload=pos+(unsigned long)id_len+(unsigned long)csize_len;

        if((id==MKV_ID_SIMPLEBLOCK)&&(csize>4ul))
        {
            if(MKV_Read_Vint_Size(payload,&tn,&tn_len)!=0u)
            {
                if((MKV_Is_Video_Track(tn)!=0u)&&
                   (AVI_Read_Byte(payload+(unsigned long)tn_len+2ul,&flags)!=0u))
                {
                    if((flags & 0x80u)!=0u)
                    {
                        *blk_pos=payload;
                        *blk_len=csize;
                        *hdr_len=(unsigned char)(tn_len+3u);
                        return 1u;
                    }
                }
            }
        }

        pos=payload+csize;
    }

    return 0u;
}

/* Builds the synthetic cluster prologue plus the patched block header.  The
   track number vint is copied verbatim because the reader matches the block
   against the video track; the block timecode is zeroed and the keyframe
   flag forced, so the reader's frame pacing sees a clean single frame. */
unsigned char MKV_Build_Preview_Header(unsigned long blk_pos,
                                       unsigned long blk_len,
                                       unsigned char hdr_len,
                                       unsigned char *out,
                                       unsigned int *out_len)
{
    unsigned long total;
    unsigned int n;
    unsigned char i;

    if((hdr_len<3u)||(hdr_len>12u)) return 0u;
    if(blk_len<(unsigned long)hdr_len) return 0u;
    if(blk_len>=0x0FFFFFF0ul) return 0u;

    total=blk_len+8ul;          /* Timecode (3) + SimpleBlock header (5) */

    n=0u;
    out[n++]=0x1Fu; out[n++]=0x43u; out[n++]=0xB6u; out[n++]=0x75u;
    out[n++]=(unsigned char)(0x10u|((total>>24)&0x0Ful));
    out[n++]=(unsigned char)(total>>16);
    out[n++]=(unsigned char)(total>>8);
    out[n++]=(unsigned char)total;

    out[n++]=0xE7u; out[n++]=0x81u; out[n++]=0x00u;

    out[n++]=0xA3u;
    out[n++]=(unsigned char)(0x10u|((blk_len>>24)&0x0Ful));
    out[n++]=(unsigned char)(blk_len>>16);
    out[n++]=(unsigned char)(blk_len>>8);
    out[n++]=(unsigned char)blk_len;

    for(i=0u;i<hdr_len;i++)
    {
        if(AVI_Read_Byte(blk_pos+(unsigned long)i,&out[n+(unsigned int)i])==0u)
        {
            return 0u;
        }
    }

    out[n+(unsigned int)hdr_len-3u]=0x00u;
    out[n+(unsigned int)hdr_len-2u]=0x00u;
    out[n+(unsigned int)hdr_len-1u]|=0x80u;

    *out_len=n+(unsigned int)hdr_len;
    return 1u;
}

unsigned char MKV_Seek_Level(unsigned long hold_ms)
{
    if(hold_ms<800ul)   return 1u;
    if(hold_ms<2000ul)  return 2u;
    if(hold_ms<3500ul)  return 3u;
    if(hold_ms<5000ul)  return 4u;
    if(hold_ms<6500ul)  return 5u;
    if(hold_ms<8000ul)  return 6u;
    if(hold_ms<10000ul) return 7u;

    return 8u;
}

/* Share of the whole file covered by one repeat at each level. */
unsigned long MKV_Level_Pct(unsigned char level)
{
    if(level<=1u) return 1ul;
    if(level==2u) return 2ul;
    if(level==3u) return 4ul;
    if(level==4u) return 7ul;
    if(level==5u) return 10ul;
    if(level==6u) return 15ul;
    if(level==7u) return 20ul;

    return 25ul;
}

/* Fixed seconds per level, so short clips do not jump in whole-file steps
   at the bottom of the ladder.  The larger of the two rules wins. */
/* Step size grows smoothly with hold time rather than jumping between a
   handful of fixed levels, which is what made stepping feel coarse.  The
   curve is cubic in hold time: gentle for the first second or two, then
   opening out to full speed at MKV_SEEK_HOLD_FULL_MS. */
unsigned long MKV_Seek_Step_Ms(unsigned long hold_ms)
{
    unsigned long dur;
    unsigned long top;
    unsigned long bottom;
    unsigned long step;
    unsigned long long v;
    unsigned long long full;

    dur=g_mkv_duration_ms;
    if(dur==0ul) dur=600000ul;

    bottom=(g_mkv_key_gap_ms>0ul) ? g_mkv_key_gap_ms : MKV_SEEK_STEP_MIN_MS;
    if((dur/MKV_SEEK_MIN_DIVISOR)<bottom)
    {
        bottom=dur/MKV_SEEK_MIN_DIVISOR;
        if(bottom<1000ul) bottom=1000ul;
    }

    top=dur/8ul;
    if(top>MKV_SEEK_STEP_MAX_MS) top=MKV_SEEK_STEP_MAX_MS;
    if(top<=bottom) return bottom;
    if(hold_ms>=MKV_SEEK_HOLD_FULL_MS) return top;

    full=(unsigned long long)MKV_SEEK_HOLD_FULL_MS;

    v=(unsigned long long)hold_ms*(unsigned long long)hold_ms;
    v/=full;                                  /* keep the product in range */
    v*=(unsigned long long)hold_ms;
    v*=(unsigned long long)(top-bottom);
    v/=(full*full);

    step=bottom+(unsigned long)v;
    if(step>top) step=top;

    return step;
}

/* Jumps to target_ms and reports the keyframe cluster that was found.
   One correction pass fixes up variable-bitrate files where the linear
   byte estimate lands wide of the mark. */
/* Measures the byte rate around a cluster from its own span and the next
   cluster's timecode.  Two adjacent timecodes describe the local bitrate
   far better than the file average, which is what makes seeking converge
   on variable bitrate material.  Returns bytes per second, 0 if unknown. */
unsigned long MKV_Local_Rate(unsigned long cluster_pos,
                             unsigned long cluster_end,
                             unsigned long cluster_ms)
{
    unsigned long dp,ce,ms2;
    unsigned long long v;

    if(cluster_end<=cluster_pos) return 0ul;
    if(cluster_end>=File_Size)   return 0ul;

    AVI_SeekCache_Reset();
    if(MKV_Check_Cluster(cluster_end,0u,&dp,&ce,&ms2)==0u) return 0ul;
    if(ms2<=cluster_ms) return 0ul;

    v=(unsigned long long)(cluster_end-cluster_pos)*1000ull;
    v/=(unsigned long long)(ms2-cluster_ms);

    return (v>0xFFFFFFFFull) ? 0xFFFFFFFFul : (unsigned long)v;
}

/* Seeks to a time.  The file average gives the first probe; after that the
   search corrects from where it actually landed, using the bitrate measured
   around that cluster, and keeps a [lo,hi] bracket of clusters with known
   timecodes so it can fall back to false position if a correction would
   leave the bracket.  Biased to land at or just before the target. */
unsigned char MKV_Seek_To_Ms(unsigned long target_ms,
                             unsigned long *cluster_pos,
                             unsigned long *cluster_end,
                             unsigned long *landed_ms)
{
    unsigned long lo,hi;
    unsigned long lo_ms,hi_ms;
    unsigned long guess;
    unsigned long margin;
    unsigned long tol;
    unsigned long rate;
    unsigned long delta;
    unsigned long data_pos;
    unsigned long ms;
    unsigned long err;
    unsigned long best_pos,best_end,best_ms,best_err;
    unsigned long last_pos;
    unsigned long long v;
    unsigned char pass;
    unsigned char have_best;
    unsigned char ok;

    if((g_mkv_have_duration!=0u)&&(target_ms>g_mkv_duration_ms))
    {
        target_ms=g_mkv_duration_ms;
    }

    lo=g_mkv_first_cluster;  lo_ms=0ul;
    hi=File_Size;            hi_ms=g_mkv_duration_ms;
    if(hi<=lo) return 0u;

    /* Aim a little ahead of the target in time, not by a fixed byte count:
       the scanner only searches forwards, and a fixed bias is far too
       coarse on a short file and far too fine on a long one. */
    margin=MKV_Ms_To_Bytes(g_mkv_seek_margin_ms)+512ul;

    tol=g_mkv_seek_margin_ms;
    if(tol>MKV_SEEK_TOL_MS) tol=MKV_SEEK_TOL_MS;
    if(tol<MKV_SEEK_TOL_FINE_MS) tol=MKV_SEEK_TOL_FINE_MS;

    guess=MKV_Time_To_Byte(target_ms);
    guess=(guess>(lo+margin)) ? (guess-margin) : lo;

    have_best=0u;
    best_pos=0ul; best_end=0ul; best_ms=0ul; best_err=0ul;
    last_pos=0xFFFFFFFFul;

    for(pass=0u;pass<MKV_SEEK_PASSES;pass++)
    {
        if(guess<lo) guess=lo;
        if(guess>=hi) guess=(hi>16ul) ? (hi-16ul) : lo;

        AVI_SeekCache_Reset();
        ok=MKV_Scan_Cluster(guess,MKV_KEY_WINDOW,1u,cluster_pos,&data_pos,cluster_end,&ms);

        if(ok==0u)
        {
            AVI_SeekCache_Reset();
            ok=MKV_Scan_Cluster(guess,MKV_ANY_WINDOW,0u,cluster_pos,&data_pos,cluster_end,&ms);
        }

        if(ok==0u)
        {
            /* Nothing ahead of the probe: the bracket top is too high. */
            if(guess<=lo) break;
            hi=guess;
            if(hi<=(lo+4096ul)) break;
            guess=lo+((hi-lo)>>1);
            continue;
        }

        err=(ms>target_ms) ? (ms-target_ms) : (target_ms-ms);

        if((have_best==0u)||(err<best_err))
        {
            best_pos=*cluster_pos;
            best_end=*cluster_end;
            best_ms=ms;
            best_err=err;
            have_best=1u;
        }

        if(g_mkv_have_duration==0u) break;
        if(err<=tol) break;
        if(*cluster_pos==last_pos) break;       /* no longer moving */
        last_pos=*cluster_pos;

        if(ms>target_ms) { hi=*cluster_pos; hi_ms=ms; }
        else             { lo=*cluster_pos;  lo_ms=ms; }

        if((hi<=lo)||((hi-lo)<4096ul)) break;

        rate=MKV_Local_Rate(*cluster_pos,*cluster_end,ms);
        if(rate==0ul) rate=MKV_Ms_To_Bytes(1000ul);
        if(rate==0ul) rate=1ul;

        v=(unsigned long long)rate*(unsigned long long)g_mkv_seek_margin_ms;
        v/=1000ull;
        margin=(unsigned long)v+512ul;

        v=(unsigned long long)rate*(unsigned long long)err;
        v/=1000ull;
        delta=(v>0xFFFFFFFFull) ? 0xFFFFFFFFul : (unsigned long)v;

        if(ms>target_ms)
        {
            guess=(*cluster_pos>(lo+delta+margin)) ? (*cluster_pos-delta-margin)
                                                   : lo;
        }
        else
        {
            guess=*cluster_pos+delta;
            guess=(guess>(*cluster_pos+margin)) ? (guess-margin) : *cluster_end;
            if(guess<*cluster_end) guess=*cluster_end;
        }

        /* A correction that leaves the bracket means the local rate did not
           describe the gap; split the bracket by false position instead. */
        if((guess<=lo)||(guess>=hi))
        {
            if(hi_ms>lo_ms)
            {
                v=(unsigned long long)(hi-lo)*
                  (unsigned long long)((target_ms>lo_ms)?(target_ms-lo_ms):0ul);
                v/=(unsigned long long)(hi_ms-lo_ms);
                guess=lo+(unsigned long)v;
            }
            else
            {
                guess=lo+((hi-lo)>>1);
            }

            if(guess<=lo) guess=lo;
            if(guess>=hi) guess=lo+((hi-lo)>>1);
        }
    }

    if(have_best==0u) return 0u;

    *cluster_pos=best_pos;
    *cluster_end=best_end;
    *landed_ms=best_ms;
    return 1u;
}

/* ------------------------ text helpers ------------------------------ */

void MKV_Fmt_Time(unsigned long ms,unsigned char with_hours,char *out)
{
    unsigned long total;
    unsigned long h;
    unsigned long m;
    unsigned long s;
    unsigned int n;

    total=ms/1000ul;
    h=total/3600ul;
    m=(total/60ul)%60ul;
    s=total%60ul;

    if(with_hours!=0u)
    {
        n=GFX_U32_To_Str(h,out);
        out[n]=':';
        n++;
        GFX_Pad2(m,&out[n]);
        n+=2u;
        out[n]=':';
        n++;
        GFX_Pad2(s,&out[n]);
        return;
    }

    if(h>0ul)
    {
        m+=h*60ul;
    }

    GFX_Pad2(m,out);
    out[2]=':';
    GFX_Pad2(s,&out[3]);
}

void MKV_Fmt_Step(unsigned long step_ms,signed char dir,char *out)
{
    unsigned int n;

    n=0u;
    if(dir<0)
    {
        out[0]='<';
        out[1]='<';
    }
    else
    {
        out[0]='>';
        out[1]='>';
    }
    out[2]=' ';
    n=3u;

    if(step_ms>=60000ul)
    {
        n+=GFX_U32_To_Str(step_ms/60000ul,&out[n]);
        (void)GFX_Str_Copy(" MIN",&out[n],8u);
        return;
    }

    n+=GFX_U32_To_Str(step_ms/1000ul,&out[n]);
    (void)GFX_Str_Copy(" S",&out[n],8u);
}

#endif
