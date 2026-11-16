#ifndef H264_PIC_HEADER
#define H264_PIC_HEADER

/* ---------------------------------------------------------------------
   Software H.264 intra decoder for the PIC32, for seek previews.

   Scope is deliberately narrow - exactly what the file's own SPS and PPS
   ask for and nothing else:

     High profile, 4:2:0, 8 bit          (profile_idc 100)
     CABAC                               (entropy_coding_mode_flag 1)
     8x8 transform                       (transform_8x8_mode_flag 1)
     no scaling matrices                 flat dequant, both SPS and PPS
     frame_mbs_only_flag 1               no interlace, no MBAFF
     IDR / I slices only                 no inter, no MC, no DPB
     no deblocking                       the 2:1 downscale hides it

   The picture is reconstructed one macroblock row at a time and scaled
   2:1 into a 160x120 RGB565 buffer, so the full frame never has to be
   held in RAM.
   --------------------------------------------------------------------- */

#include "H264_TABLES.h"

#define H264_MAX_MB_W   30u      /* 480 px wide is plenty for this board */

/* Why the last SPS or PPS was rejected, so a failure can be reported
   rather than guessed at.  0 means no problem. */
#define H264_SPS_OK        0u
#define H264_SPS_SHORT     1u
#define H264_SPS_SCALING   2u
#define H264_SPS_LOSSLESS  3u
#define H264_SPS_INTERLACE 4u
#define H264_SPS_TOOWIDE   5u
#define H264_PPS_SLICEGRP  6u
#define H264_PPS_SCALING   7u
#define H264_PPS_CAVLC     8u

unsigned char g_h264_reason=H264_SPS_OK;

/* ------------------------- raw bit reader ---------------------------- */

typedef struct
{
    const unsigned char *buf;
    unsigned long        len;      /* bytes */
    unsigned long        pos;      /* bit position */
} H264_BR;

void H264_BR_Init(H264_BR *b,const unsigned char *buf,unsigned long len)
{
    b->buf=buf;
    b->len=len;
    b->pos=0ul;
}

unsigned long H264_U(H264_BR *b,unsigned char n)
{
    unsigned long v;
    unsigned char i;

    v=0ul;
    for(i=0u;i<n;i++)
    {
        if((b->pos>>3)>=b->len) return v<<(n-i);
        v=(v<<1)|(unsigned long)((b->buf[b->pos>>3]>>(7u-(b->pos&7u)))&1u);
        b->pos++;
    }
    return v;
}

unsigned long H264_UE(H264_BR *b)
{
    unsigned char z;

    z=0u;
    while((z<32u)&&(H264_U(b,1u)==0ul))
    {
        if((b->pos>>3)>=b->len) return 0ul;
        z++;
    }
    if(z==0u) return 0ul;
    return ((1ul<<z)-1ul)+H264_U(b,z);
}

/* 7.2 more_rbsp_data(): true while there is anything before the
   rbsp_stop_one_bit, which is the last set bit in the payload.  Guessing
   from the number of bytes left instead gets PPS parsing wrong - a short
   trailing section then reads as absent and transform_8x8_mode_flag is
   silently lost. */
unsigned char H264_More_RBSP(const H264_BR *b)
{
    unsigned long i;
    unsigned long stop;
    unsigned char m;
    unsigned char k;

    i=b->len;
    while((i>0ul)&&(b->buf[i-1ul]==0u)) i--;
    if(i==0ul) return 0u;

    m=b->buf[i-1ul];
    k=0u;
    while(((m>>k)&1u)==0u) k++;

    stop=((i-1ul)<<3)+(unsigned long)(7u-k);

    return (b->pos<stop) ? 1u : 0u;
}

signed long H264_SE(H264_BR *b)
{
    unsigned long k;

    k=H264_UE(b);
    if((k&1ul)!=0ul) return (signed long)((k+1ul)>>1);
    return -(signed long)(k>>1);
}

/* Strips emulation prevention bytes in place, returning the RBSP length.
   dst may alias src. */
unsigned long H264_Unescape(const unsigned char *src,unsigned long len,
                            unsigned char *dst,unsigned long cap)
{
    unsigned long i;
    unsigned long o;
    unsigned char zeros;

    o=0ul;
    zeros=0u;

    for(i=0ul;i<len;i++)
    {
        if((zeros>=2u)&&(src[i]==0x03u))
        {
            zeros=0u;
            continue;
        }

        if(o>=cap) break;
        dst[o++]=src[i];

        zeros=(src[i]==0u) ? (unsigned char)(zeros+1u) : 0u;
    }

    return o;
}

/* --------------------------- CABAC engine ---------------------------- */

typedef struct
{
    const unsigned char *buf;
    unsigned long        len;
    unsigned long        bytepos;
    unsigned char        bitpos;

    unsigned int         range;
    unsigned int         offset;

    /* one byte per context: (pStateIdx<<1) | valMPS */
    unsigned char        state[1024];
} H264_CABAC;

unsigned char H264_CB_Bit(H264_CABAC *c)
{
    unsigned char v;

    if(c->bytepos>=c->len) return 0u;

    v=(unsigned char)((c->buf[c->bytepos]>>(7u-c->bitpos))&1u);

    c->bitpos++;
    if(c->bitpos==8u)
    {
        c->bitpos=0u;
        c->bytepos++;
    }

    return v;
}

static signed int H264_Clip3(signed int lo,signed int hi,signed int v)
{
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}

/* 9.3.1.1 - I slices use the single init table, cabac_init_idc does not
   apply, so slice_qp is the only input. */
void H264_CB_Init(H264_CABAC *c,const unsigned char *buf,unsigned long len,
                  unsigned long start_byte,signed int slice_qp)
{
    unsigned int i;
    signed int pre;
    signed int qp;

    c->buf=buf;
    c->len=len;
    c->bytepos=start_byte;
    c->bitpos=0u;

    qp=H264_Clip3(0,51,slice_qp);

    for(i=0u;i<1024u;i++)
    {
        pre=(((signed int)H264_CtxInitI[i][0]*qp)>>4)+(signed int)H264_CtxInitI[i][1];
        pre=H264_Clip3(1,126,pre);

        if(pre<=63)
        {
            c->state[i]=(unsigned char)(((63-pre)<<1)|0);
        }
        else
        {
            c->state[i]=(unsigned char)(((pre-64)<<1)|1);
        }
    }

    c->range=510u;
    c->offset=0u;
    for(i=0u;i<9u;i++)
    {
        c->offset=(c->offset<<1)|(unsigned int)H264_CB_Bit(c);
    }
}

static void H264_CB_Renorm(H264_CABAC *c)
{
    while(c->range<256u)
    {
        c->range<<=1;
        c->offset=(c->offset<<1)|(unsigned int)H264_CB_Bit(c);
    }
}

/* 9.3.3.2.1 */
unsigned char H264_CB_Decision(H264_CABAC *c,unsigned int ctx)
{
    unsigned char s;
    unsigned char p;
    unsigned char mps;
    unsigned int rlps;
    unsigned char bin;

    s=c->state[ctx];
    p=(unsigned char)(s>>1);
    mps=(unsigned char)(s&1u);

    rlps=(unsigned int)H264_RangeLPS[((unsigned int)p<<2)|((c->range>>6)&3u)];
    c->range-=rlps;

    if(c->offset>=c->range)
    {
        bin=(unsigned char)(1u-mps);
        c->offset-=c->range;
        c->range=rlps;

        if(p==0u) mps=(unsigned char)(1u-mps);
        p=H264_TransLPS[p];
    }
    else
    {
        bin=mps;
        p=H264_TransMPS[p];
    }

    c->state[ctx]=(unsigned char)((p<<1)|mps);

    H264_CB_Renorm(c);
    return bin;
}

/* 9.3.3.2.3 */
unsigned char H264_CB_Bypass(H264_CABAC *c)
{
    c->offset=(c->offset<<1)|(unsigned int)H264_CB_Bit(c);

    if(c->offset>=c->range)
    {
        c->offset-=c->range;
        return 1u;
    }

    return 0u;
}

/* 9.3.3.2.4 - returns 1 at end_of_slice_flag */
unsigned char H264_CB_Terminate(H264_CABAC *c)
{
    c->range-=2u;

    if(c->offset>=c->range)
    {
        return 1u;
    }

    H264_CB_Renorm(c);
    return 0u;
}

/* --------------------------- SPS and PPS ----------------------------- */

typedef struct
{
    unsigned char valid;
    unsigned char profile_idc;
    unsigned char level_idc;
    unsigned char chroma_format_idc;
    unsigned char bit_depth_luma;
    unsigned char bit_depth_chroma;
    unsigned char scaling_matrix_present;
    unsigned char transform_bypass;
    unsigned char frame_mbs_only_flag;
    unsigned int  mb_width;
    unsigned int  mb_height;
    unsigned int  log2_max_frame_num;
    unsigned char pic_order_cnt_type;
    unsigned int  log2_max_poc_lsb;
    unsigned char delta_pic_order_always_zero;
} H264_SPS;

typedef struct
{
    unsigned char valid;
    unsigned char cabac;
    unsigned char num_slice_groups;
    unsigned char num_ref_idx_l0;
    unsigned char num_ref_idx_l1;
    unsigned char weighted_pred;
    unsigned char weighted_bipred;
    signed int    pic_init_qp;
    signed int    chroma_qp_index_offset;
    signed int    second_chroma_qp_index_offset;
    unsigned char deblock_control_present;
    unsigned char constrained_intra_pred;
    unsigned char redundant_pic_cnt_present;
    unsigned char transform_8x8_mode;
    unsigned char scaling_matrix_present;
    unsigned char bottom_field_pic_order;
} H264_PPS;

unsigned char H264_Parse_SPS(const unsigned char *rbsp,unsigned long len,H264_SPS *s)
{
    H264_BR b;
    unsigned long i;
    unsigned long n;
    unsigned char crop;

    s->valid=0u;
    g_h264_reason=H264_SPS_SHORT;
    if(len<4ul) return 0u;

    H264_BR_Init(&b,rbsp,len);

    s->profile_idc=(unsigned char)H264_U(&b,8u);
    (void)H264_U(&b,8u);                     /* constraint flags */
    s->level_idc=(unsigned char)H264_U(&b,8u);
    (void)H264_UE(&b);                       /* sps id */

    s->chroma_format_idc=1u;
    s->transform_bypass=0u;
    s->bit_depth_luma=8u;
    s->bit_depth_chroma=8u;
    s->scaling_matrix_present=0u;

    if((s->profile_idc==100u)||(s->profile_idc==110u)||(s->profile_idc==122u)||
       (s->profile_idc==244u)||(s->profile_idc==44u)||(s->profile_idc==83u)||
       (s->profile_idc==86u)||(s->profile_idc==118u)||(s->profile_idc==128u))
    {
        s->chroma_format_idc=(unsigned char)H264_UE(&b);
        if(s->chroma_format_idc==3u) (void)H264_U(&b,1u);

        s->bit_depth_luma=(unsigned char)(8ul+H264_UE(&b));
        s->bit_depth_chroma=(unsigned char)(8ul+H264_UE(&b));
        /* Lossless: at qp 0 the transform and quantisation are bypassed
           altogether.  A different coding path, and such streams are far
           too large to seek through anyway - reject rather than guess. */
        s->transform_bypass=(unsigned char)H264_U(&b,1u);
        if(s->transform_bypass!=0u) { g_h264_reason=H264_SPS_LOSSLESS; return 0u; }

        s->scaling_matrix_present=(unsigned char)H264_U(&b,1u);
        if(s->scaling_matrix_present!=0u)
        {
            /* Not supported: the files this targets carry none. */
            g_h264_reason=H264_SPS_SCALING;
            return 0u;
        }
    }

    s->log2_max_frame_num=(unsigned int)(H264_UE(&b)+4ul);
    s->pic_order_cnt_type=(unsigned char)H264_UE(&b);
    s->log2_max_poc_lsb=0u;
    s->delta_pic_order_always_zero=0u;

    if(s->pic_order_cnt_type==0u)
    {
        s->log2_max_poc_lsb=(unsigned int)(H264_UE(&b)+4ul);
    }
    else if(s->pic_order_cnt_type==1u)
    {
        s->delta_pic_order_always_zero=(unsigned char)H264_U(&b,1u);
        (void)H264_SE(&b);
        (void)H264_SE(&b);
        n=H264_UE(&b);
        for(i=0ul;i<n;i++) (void)H264_SE(&b);
    }

    (void)H264_UE(&b);                       /* max_num_ref_frames */
    (void)H264_U(&b,1u);                     /* gaps_in_frame_num_allowed */

    s->mb_width=(unsigned int)(H264_UE(&b)+1ul);
    s->mb_height=(unsigned int)(H264_UE(&b)+1ul);

    s->frame_mbs_only_flag=(unsigned char)H264_U(&b,1u);
    if(s->frame_mbs_only_flag==0u)
    {
        g_h264_reason=H264_SPS_INTERLACE;
        return 0u;
    }

    (void)H264_U(&b,1u);                     /* direct_8x8_inference */

    crop=(unsigned char)H264_U(&b,1u);
    if(crop!=0u)
    {
        (void)H264_UE(&b); (void)H264_UE(&b);
        (void)H264_UE(&b); (void)H264_UE(&b);
    }

    if(s->mb_width>H264_MAX_MB_W) { g_h264_reason=H264_SPS_TOOWIDE; return 0u; }

    g_h264_reason=H264_SPS_OK;
    s->valid=1u;
    return 1u;
}

unsigned char H264_Parse_PPS(const unsigned char *rbsp,unsigned long len,H264_PPS *p)
{
    H264_BR b;

    p->valid=0u;
    g_h264_reason=H264_SPS_SHORT;
    if(len<1ul) return 0u;

    H264_BR_Init(&b,rbsp,len);

    (void)H264_UE(&b);                       /* pps id */
    (void)H264_UE(&b);                       /* sps id */

    p->cabac=(unsigned char)H264_U(&b,1u);
    p->bottom_field_pic_order=(unsigned char)H264_U(&b,1u);

    p->num_slice_groups=(unsigned char)(H264_UE(&b)+1ul);
    if(p->num_slice_groups!=1u) { g_h264_reason=H264_PPS_SLICEGRP; return 0u; }

    p->num_ref_idx_l0=(unsigned char)(H264_UE(&b)+1ul);
    p->num_ref_idx_l1=(unsigned char)(H264_UE(&b)+1ul);

    p->weighted_pred=(unsigned char)H264_U(&b,1u);
    p->weighted_bipred=(unsigned char)H264_U(&b,2u);

    p->pic_init_qp=26+(signed int)H264_SE(&b);
    (void)H264_SE(&b);                       /* pic_init_qs */
    p->chroma_qp_index_offset=(signed int)H264_SE(&b);

    p->deblock_control_present=(unsigned char)H264_U(&b,1u);
    p->constrained_intra_pred=(unsigned char)H264_U(&b,1u);
    p->redundant_pic_cnt_present=(unsigned char)H264_U(&b,1u);

    p->transform_8x8_mode=0u;
    p->scaling_matrix_present=0u;
    p->second_chroma_qp_index_offset=p->chroma_qp_index_offset;

    if(H264_More_RBSP(&b)!=0u)
    {
        p->transform_8x8_mode=(unsigned char)H264_U(&b,1u);
        p->scaling_matrix_present=(unsigned char)H264_U(&b,1u);
        if(p->scaling_matrix_present!=0u) { g_h264_reason=H264_PPS_SCALING; return 0u; }
        p->second_chroma_qp_index_offset=(signed int)H264_SE(&b);
    }

    if(p->cabac==0u) { g_h264_reason=H264_PPS_CAVLC; return 0u; }

    g_h264_reason=H264_SPS_OK;
    p->valid=1u;
    return 1u;
}

/* ------------------------- slice header ------------------------------ */

#define H264_SLICE_P 0u
#define H264_SLICE_B 1u
#define H264_SLICE_I 2u

typedef struct
{
    unsigned char  slice_type;
    unsigned long  first_mb;
    signed int     slice_qp;
    unsigned char  disable_deblock_idc;
    unsigned long  cabac_start;     /* byte offset of the CABAC engine start */
} H264_SLICE;

/* Returns 1 for an I slice whose header parsed cleanly. */
unsigned char H264_Parse_Slice_Header(const unsigned char *rbsp,unsigned long len,
                                      unsigned char nal_type,
                                      const H264_SPS *sps,const H264_PPS *pps,
                                      H264_SLICE *sh)
{
    H264_BR b;
    unsigned long st;

    if(len<2ul) return 0u;

    H264_BR_Init(&b,rbsp,len);

    sh->first_mb=H264_UE(&b);
    st=H264_UE(&b);
    if(st>=5ul) st-=5ul;                     /* "all slices same type" forms */
    sh->slice_type=(unsigned char)st;

    if(sh->slice_type!=H264_SLICE_I) return 0u;

    (void)H264_UE(&b);                       /* pic_parameter_set_id */
    (void)H264_U(&b,(unsigned char)sps->log2_max_frame_num);

    if(nal_type==5u)
    {
        (void)H264_UE(&b);                   /* idr_pic_id */
    }

    if(sps->pic_order_cnt_type==0u)
    {
        (void)H264_U(&b,(unsigned char)sps->log2_max_poc_lsb);
        if(pps->bottom_field_pic_order!=0u) (void)H264_SE(&b);
    }
    else if((sps->pic_order_cnt_type==1u)&&(sps->delta_pic_order_always_zero==0u))
    {
        (void)H264_SE(&b);
        if(pps->bottom_field_pic_order!=0u) (void)H264_SE(&b);
    }

    if(pps->redundant_pic_cnt_present!=0u) (void)H264_UE(&b);

    /* dec_ref_pic_marking - I slices are the only ones here, and an IDR
       carries two flags rather than the MMCO loop. */
    if(nal_type==5u)
    {
        (void)H264_U(&b,1u);                 /* no_output_of_prior_pics */
        (void)H264_U(&b,1u);                 /* long_term_reference_flag */
    }
    else
    {
        if(H264_U(&b,1u)!=0ul)
        {
            unsigned long op;
            do
            {
                op=H264_UE(&b);
                if((op==1ul)||(op==3ul)) (void)H264_UE(&b);
                if(op==2ul) (void)H264_UE(&b);
                if((op==3ul)||(op==6ul)) (void)H264_UE(&b);
                if(op==4ul) (void)H264_UE(&b);
            } while((op!=0ul)&&((b.pos>>3)<len));
        }
    }

    /* cabac_init_idc is absent for I slices. */
    sh->slice_qp=pps->pic_init_qp+(signed int)H264_SE(&b);

    sh->disable_deblock_idc=0u;
    if(pps->deblock_control_present!=0u)
    {
        sh->disable_deblock_idc=(unsigned char)H264_UE(&b);
        if(sh->disable_deblock_idc!=1u)
        {
            (void)H264_SE(&b);               /* slice_alpha_c0_offset_div2 */
            (void)H264_SE(&b);               /* slice_beta_offset_div2 */
        }
    }

    /* 9.3.1: the engine starts at the next byte boundary, and every bit of
       padding to get there is a cabac_alignment_one_bit.  If any of them is
       zero the header was misparsed, so this is a free correctness check
       rather than something to assume. */
    while((b.pos&7ul)!=0ul)
    {
        if(H264_U(&b,1u)!=1ul) return 0u;
    }

    sh->cabac_start=b.pos>>3;

    return 1u;
}

/* ===================== I-slice macroblock layer ======================= */

/* Sentinels for an unavailable neighbour.  The spec inverts the sense of
   the luma CBP condition but not the chroma one, and for an intra MB an
   unavailable neighbour counts as coded for coded_block_flag - so the three
   cases need three different "missing" values rather than one blank MB. */
#define H264_NB_CBPL   0x0Fu     /* luma CBP bits read as coded     */
#define H264_NB_CBPC   0x00u     /* chroma CBP reads as not coded   */
#define H264_NB_CBF    1u        /* coded_block_flag reads as coded */

#define H264_I_NxN     0u
#define H264_I_PCM     25u

typedef struct
{
    H264_CABAC     cb;
    const H264_SPS *sps;
    const H264_PPS *pps;

    unsigned int   mb_w;
    unsigned int   mb_h;
    unsigned int   mb_x;
    unsigned int   mb_y;
    unsigned long  mb_count;

    signed int     qp;
    signed int     last_qp_delta;

    /* per-MB results */
    unsigned char  mb_type;
    unsigned char  t8x8;
    unsigned char  cbp_luma;
    unsigned char  cbp_chroma;
    unsigned char  chroma_mode;
    signed char    i4mode[16];          /* raster 4x4 order */
    unsigned char  pcm[384];            /* I_PCM samples, 256 luma + 2x64 */
    unsigned char  fail;                /* 0 ok, 1 PCM misparse, 2 overrun */
    unsigned long  pcm_count;
    signed short   coef[27][64];        /* 16 luma + 1 lumaDC + 8 chroma + 2 DC */

    /* neighbours: "top" is per MB column, "left" is the previous MB */
    unsigned char  top_i16[H264_MAX_MB_W];
    unsigned char  top_t8[H264_MAX_MB_W];
    unsigned char  top_cpm[H264_MAX_MB_W];
    unsigned char  top_cbpl[H264_MAX_MB_W];
    unsigned char  top_cbpc[H264_MAX_MB_W];
    unsigned char  top_dcy[H264_MAX_MB_W];
    unsigned char  top_dcc[H264_MAX_MB_W][2];
    unsigned char  top_nzy[H264_MAX_MB_W][4];
    unsigned char  top_nzc[H264_MAX_MB_W][2][2];
    signed char    top_i4[H264_MAX_MB_W][4];

    unsigned char  left_i16;
    unsigned char  left_t8;
    unsigned char  left_cpm;
    unsigned char  left_cbpl;
    unsigned char  left_cbpc;
    unsigned char  left_dcy;
    unsigned char  left_dcc[2];
    unsigned char  left_nzy[4];
    unsigned char  left_nzc[2][2];
    signed char    left_i4[4];

    /* current MB's 4x4 non-zero counts, for the in-MB neighbours */
    unsigned char  nzy[4][4];
    unsigned char  nzc[2][2][2];
} H264_DEC;

static unsigned char H264_Avail_Left(const H264_DEC *d){ return (d->mb_x>0u)?1u:0u; }
static unsigned char H264_Avail_Top (const H264_DEC *d){ return (d->mb_y>0u)?1u:0u; }

/* ------------------------- residual blocks --------------------------- */

/* Decodes one transform block's levels.  cat picks the context bases, scan
   maps scan order to raster position inside the block.  Returns the number
   of non-zero coefficients, which is also the neighbour context for the
   next block. */
static unsigned char H264_Residual(H264_DEC *d,signed short *out,
                                   unsigned char cat,
                                   const unsigned char *scan,
                                   unsigned char max_coeff)
{
    unsigned char index[64];
    unsigned int sig_base;
    unsigned int last_base;
    unsigned int abs_base;
    unsigned int ctx;
    unsigned char count;
    unsigned char last;
    unsigned char node;
    unsigned char big;
    unsigned int level;
    unsigned char j;

    sig_base =H264_SigOff[cat];
    last_base=H264_LastOff[cat];
    abs_base =H264_AbsOff[cat];

    count=0u;
    big=(unsigned char)((max_coeff==64u)?1u:0u);

    for(last=0u;last<(unsigned char)(max_coeff-1u);last++)
    {
        unsigned char si;
        unsigned char li;

        si=big ? H264_Sig8x8_Inc[last]  : last;
        li=big ? H264_Last8x8_Inc[last] : last;

        if(H264_CB_Decision(&d->cb,sig_base+(unsigned int)si)!=0u)
        {
            index[count++]=last;
            if(H264_CB_Decision(&d->cb,last_base+(unsigned int)li)!=0u)
            {
                break;
            }
        }
    }

    if(last==(unsigned char)(max_coeff-1u))
    {
        index[count++]=last;
    }

    if(count==0u) return 0u;

    /* Levels come out in reverse scan order. */
    node=0u;
    j=count;
    do
    {
        j--;

        ctx=abs_base+(unsigned int)H264_Lvl1Ctx[node];

        if(H264_CB_Decision(&d->cb,ctx)==0u)
        {
            level=1u;
            node=H264_LvlTrans[0][node];
        }
        else
        {
            level=2u;
            ctx=abs_base+(unsigned int)H264_LvlGt1Ctx[node];
            node=H264_LvlTrans[1][node];

            while((level<15u)&&(H264_CB_Decision(&d->cb,ctx)!=0u))
            {
                level++;
            }

            if(level>=15u)
            {
                unsigned char k;

                k=0u;
                while((H264_CB_Bypass(&d->cb)!=0u)&&(k<23u))
                {
                    k++;
                }

                level=1u;
                while(k>0u)
                {
                    k--;
                    level=(level<<1)+(unsigned int)H264_CB_Bypass(&d->cb);
                }
                level+=14u;
            }
        }

        if(H264_CB_Bypass(&d->cb)!=0u)
        {
            out[scan[index[j]]]=(signed short)(-(signed int)level);
        }
        else
        {
            out[scan[index[j]]]=(signed short)level;
        }
    } while(j>0u);

    return count;
}

/* coded_block_flag, 9.3.3.1.1.9 */
static unsigned char H264_CBF(H264_DEC *d,unsigned char cat,
                              unsigned char nza,unsigned char nzb)
{
    unsigned int ctx;

    ctx=0u;
    if(nza>0u) ctx+=1u;
    if(nzb>0u) ctx+=2u;

    return H264_CB_Decision(&d->cb,H264_CbfBase[cat]+ctx);
}

/* --------------------------- MB syntax ------------------------------- */

static unsigned char H264_MB_Type(H264_DEC *d)
{
    unsigned int base;
    unsigned int ctx;
    unsigned char t;

    base=3u;

    ctx=0u;
    if((H264_Avail_Left(d)!=0u)&&(d->left_i16!=0u))         ctx++;
    if((H264_Avail_Top(d) !=0u)&&(d->top_i16[d->mb_x]!=0u)) ctx++;

    if(H264_CB_Decision(&d->cb,base+ctx)==0u) return H264_I_NxN;

    if(H264_CB_Terminate(&d->cb)!=0u) return H264_I_PCM;

    /* I_16x16: cbp_luma, then cbp_chroma, then the prediction mode. */
    t=1u;
    t=(unsigned char)(t+12u*H264_CB_Decision(&d->cb,base+3u));

    if(H264_CB_Decision(&d->cb,base+4u)!=0u)
    {
        t=(unsigned char)(t+4u+4u*H264_CB_Decision(&d->cb,base+5u));
    }

    t=(unsigned char)(t+2u*H264_CB_Decision(&d->cb,base+6u));
    t=(unsigned char)(t+1u*H264_CB_Decision(&d->cb,base+7u));

    return t;
}

static unsigned char H264_Chroma_Mode(H264_DEC *d)
{
    unsigned int ctx;

    ctx=0u;
    if((H264_Avail_Left(d)!=0u)&&(d->left_cpm!=0u))         ctx++;
    if((H264_Avail_Top(d) !=0u)&&(d->top_cpm[d->mb_x]!=0u)) ctx++;

    if(H264_CB_Decision(&d->cb,64u+ctx)==0u) return 0u;
    if(H264_CB_Decision(&d->cb,67u)==0u)     return 1u;
    if(H264_CB_Decision(&d->cb,67u)==0u)     return 2u;

    return 3u;
}

static unsigned char H264_CBP_Luma(H264_DEC *d)
{
    unsigned char a;
    unsigned char b;
    unsigned char cbp;
    unsigned int ctx;

    a=(H264_Avail_Left(d)!=0u) ? d->left_cbpl      : H264_NB_CBPL;
    b=(H264_Avail_Top(d) !=0u) ? d->top_cbpl[d->mb_x] : H264_NB_CBPL;

    cbp=0u;

    ctx=(unsigned int)(((a&0x02u)==0u)?1u:0u)+2u*(unsigned int)(((b&0x04u)==0u)?1u:0u);
    cbp=(unsigned char)(cbp+H264_CB_Decision(&d->cb,73u+ctx));

    ctx=(unsigned int)(((cbp&0x01u)==0u)?1u:0u)+2u*(unsigned int)(((b&0x08u)==0u)?1u:0u);
    cbp=(unsigned char)(cbp+(H264_CB_Decision(&d->cb,73u+ctx)<<1));

    ctx=(unsigned int)(((a&0x08u)==0u)?1u:0u)+2u*(unsigned int)(((cbp&0x01u)==0u)?1u:0u);
    cbp=(unsigned char)(cbp+(H264_CB_Decision(&d->cb,73u+ctx)<<2));

    ctx=(unsigned int)(((cbp&0x04u)==0u)?1u:0u)+2u*(unsigned int)(((cbp&0x02u)==0u)?1u:0u);
    cbp=(unsigned char)(cbp+(H264_CB_Decision(&d->cb,73u+ctx)<<3));

    return cbp;
}

static unsigned char H264_CBP_Chroma(H264_DEC *d)
{
    unsigned char a;
    unsigned char b;
    unsigned int ctx;

    a=(H264_Avail_Left(d)!=0u) ? d->left_cbpc         : H264_NB_CBPC;
    b=(H264_Avail_Top(d) !=0u) ? d->top_cbpc[d->mb_x] : H264_NB_CBPC;

    ctx=0u;
    if(a>0u) ctx+=1u;
    if(b>0u) ctx+=2u;

    if(H264_CB_Decision(&d->cb,77u+ctx)==0u) return 0u;

    ctx=4u;
    if(a==2u) ctx+=1u;
    if(b==2u) ctx+=2u;

    return (unsigned char)(1u+H264_CB_Decision(&d->cb,77u+ctx));
}

static void H264_MB_QP_Delta(H264_DEC *d)
{
    unsigned int ctx;
    signed int val;

    if(H264_CB_Decision(&d->cb,60u+(unsigned int)((d->last_qp_delta!=0)?1:0))==0u)
    {
        d->last_qp_delta=0;
        return;
    }

    val=1;
    ctx=2u;
    while((H264_CB_Decision(&d->cb,60u+ctx)!=0u)&&(val<104))
    {
        ctx=3u;
        val++;
    }

    if((val&1)!=0) val= (val+1)>>1;
    else           val=-((val+1)>>1);

    d->last_qp_delta=val;
    d->qp+=val;

    if(d->qp>51) d->qp-=52;
    if(d->qp<0)  d->qp+=52;
}

/* 8.3.1.1 - predicted Intra_4x4 / Intra_8x8 mode from the neighbours. */
static signed char H264_Pred_I4(H264_DEC *d,unsigned char bx,unsigned char by)
{
    signed char a;
    signed char b;

    if(bx>0u) a=d->i4mode[(by<<2)+bx-1u];
    else      a=(H264_Avail_Left(d)!=0u) ? d->left_i4[by] : -1;

    if(by>0u) b=d->i4mode[((by-1u)<<2)+bx];
    else      b=(H264_Avail_Top(d)!=0u) ? d->top_i4[d->mb_x][bx] : -1;

    if((a<0)||(b<0)) return 2;              /* DC */
    return (a<b) ? a : b;
}

static signed char H264_Read_I4_Mode(H264_DEC *d,signed char pred)
{
    signed char m;

    if(H264_CB_Decision(&d->cb,68u)!=0u) return pred;

    m =(signed char)H264_CB_Decision(&d->cb,69u);
    m|=(signed char)(H264_CB_Decision(&d->cb,69u)<<1);
    m|=(signed char)(H264_CB_Decision(&d->cb,69u)<<2);

    return (signed char)(m+((m>=pred)?1:0));
}

/* ------------------------ one macroblock ----------------------------- */

unsigned char H264_Decode_MB(H264_DEC *d)
{
    unsigned char bx,by,i,c;
    unsigned char nza,nzb,n;
    unsigned char cbf;
    signed char pred;

    for(i=0u;i<27u;i++)
    {
        unsigned char k;
        for(k=0u;k<64u;k++) d->coef[i][k]=0;
    }
    for(by=0u;by<4u;by++) for(bx=0u;bx<4u;bx++) d->nzy[by][bx]=0u;
    for(c=0u;c<2u;c++) for(by=0u;by<2u;by++) for(bx=0u;bx<2u;bx++) d->nzc[c][by][bx]=0u;

    d->mb_type=H264_MB_Type(d);
    d->t8x8=0u;

    if(d->mb_type==H264_I_PCM)
    {
        /* 9.3.3.2.4: the last bit taken into codIOffset is the final
           meaningful one, so the reader is already at the bitstream
           position - no rewind.  Skip the pcm_alignment_zero_bits to the
           byte boundary, take the samples raw, then restart the engine.
           Confirmed against ffmpeg: the samples come out byte-exact. */
        unsigned long bit;

        bit=(d->cb.bytepos<<3)+(unsigned long)d->cb.bitpos;
        bit=(bit+7ul)&~7ul;

        if(((bit>>3)+384ul)>d->cb.len) { d->fail=1u; return 0u; }

        for(i=0u;i<128u;i++)
        {
            d->pcm[i]      =d->cb.buf[(bit>>3)+(unsigned long)i];
            d->pcm[i+128u] =d->cb.buf[(bit>>3)+(unsigned long)i+128ul];
            d->pcm[i+256u] =d->cb.buf[(bit>>3)+(unsigned long)i+256ul];
        }

        d->pcm_count++;

        /* 9.3.1.2: the engine is initialised again after the samples, but
           the context variables carry on unchanged. */
        d->cb.bytepos=(bit>>3)+384ul;
        d->cb.bitpos=0u;
        d->cb.range=510u;
        d->cb.offset=0u;
        for(i=0u;i<9u;i++)
        {
            d->cb.offset=(d->cb.offset<<1)|(unsigned int)H264_CB_Bit(&d->cb);
        }

        /* I_PCM neighbours: coded for coded_block_flag and chroma CBP,
           and "not coded" for the inverted luma CBP condition. */
        d->t8x8=0u;
        d->cbp_luma=0x0Fu;
        d->cbp_chroma=2u;
        d->chroma_mode=0u;
        d->last_qp_delta=0;

        for(i=0u;i<16u;i++) d->i4mode[i]=2;
        for(by=0u;by<4u;by++) for(bx=0u;bx<4u;bx++) d->nzy[by][bx]=16u;
        for(c=0u;c<2u;c++) for(by=0u;by<2u;by++) for(bx=0u;bx<2u;bx++) d->nzc[c][by][bx]=16u;

        d->left_i16=1u;  d->top_i16[d->mb_x]=1u;
        d->left_t8=0u;   d->top_t8[d->mb_x]=0u;
        d->left_cpm=0u;  d->top_cpm[d->mb_x]=0u;
        d->left_cbpl=0x0Fu; d->top_cbpl[d->mb_x]=0x0Fu;
        d->left_cbpc=2u;    d->top_cbpc[d->mb_x]=2u;
        d->left_dcy=1u;  d->top_dcy[d->mb_x]=1u;
        d->left_dcc[0]=1u; d->left_dcc[1]=1u;
        d->top_dcc[d->mb_x][0]=1u; d->top_dcc[d->mb_x][1]=1u;

        for(by=0u;by<4u;by++) d->left_nzy[by]=16u;
        for(bx=0u;bx<4u;bx++) d->top_nzy[d->mb_x][bx]=16u;
        for(c=0u;c<2u;c++)
        {
            d->left_nzc[c][0]=16u; d->left_nzc[c][1]=16u;
            d->top_nzc[d->mb_x][c][0]=16u; d->top_nzc[d->mb_x][c][1]=16u;
        }
        for(by=0u;by<4u;by++) d->left_i4[by]=2;
        for(bx=0u;bx<4u;bx++) d->top_i4[d->mb_x][bx]=2;

        return 1u;
    }

    if(d->mb_type==H264_I_NxN)
    {
        unsigned int ctx;

        if(d->pps->transform_8x8_mode!=0u)
        {
            ctx =(unsigned int)(((H264_Avail_Left(d)!=0u)&&(d->left_t8!=0u))?1u:0u);
            ctx+=(unsigned int)(((H264_Avail_Top(d)!=0u)&&(d->top_t8[d->mb_x]!=0u))?1u:0u);
            d->t8x8=H264_CB_Decision(&d->cb,399u+ctx);
        }

        if(d->t8x8!=0u)
        {
            for(i=0u;i<4u;i++)
            {
                bx=(unsigned char)((i&1u)<<1);
                by=(unsigned char)((i>>1)<<1);
                pred=H264_Pred_I4(d,bx,by);
                d->i4mode[(by<<2)+bx]=H264_Read_I4_Mode(d,pred);
                d->i4mode[(by<<2)+bx+1u]      =d->i4mode[(by<<2)+bx];
                d->i4mode[((by+1u)<<2)+bx]    =d->i4mode[(by<<2)+bx];
                d->i4mode[((by+1u)<<2)+bx+1u] =d->i4mode[(by<<2)+bx];
            }
        }
        else
        {
            for(i=0u;i<16u;i++)
            {
                /* 4x4 blocks arrive in the H.264 block scan, not raster */
                bx=(unsigned char)(((i>>2)&1u)<<1 | (i&1u));
                by=(unsigned char)(((i>>3)&1u)<<1 | ((i>>1)&1u));
                pred=H264_Pred_I4(d,bx,by);
                d->i4mode[(by<<2)+bx]=H264_Read_I4_Mode(d,pred);
            }
        }

        d->chroma_mode=H264_Chroma_Mode(d);
        d->cbp_luma  =H264_CBP_Luma(d);
        d->cbp_chroma=H264_CBP_Chroma(d);
    }
    else
    {
        unsigned char t;

        t=(unsigned char)(d->mb_type-1u);
        d->cbp_luma  =((t/12u)!=0u) ? 0x0Fu : 0x00u;
        d->cbp_chroma=(unsigned char)((t%12u)/4u);

        for(i=0u;i<16u;i++) d->i4mode[i]=2;   /* neighbours see DC */

        d->chroma_mode=H264_Chroma_Mode(d);
    }

    if((d->cbp_luma!=0u)||(d->cbp_chroma!=0u)||(d->mb_type!=H264_I_NxN))
    {
        H264_MB_QP_Delta(d);
    }
    else
    {
        d->last_qp_delta=0;
    }

    /* --- Intra_16x16 luma DC --- */
    if(d->mb_type!=H264_I_NxN)
    {
        nza=(H264_Avail_Left(d)!=0u) ? d->left_dcy         : H264_NB_CBF;
        nzb=(H264_Avail_Top(d) !=0u) ? d->top_dcy[d->mb_x] : H264_NB_CBF;

        cbf=H264_CBF(d,0u,nza,nzb);
        if(cbf!=0u)
        {
            (void)H264_Residual(d,d->coef[16],0u,H264_Zig4,16u);
            d->left_dcy=1u;
        }
        else
        {
            d->left_dcy=0u;
        }
    }
    else
    {
        d->left_dcy=0u;
    }

    /* --- luma residual --- */
    if(d->t8x8!=0u)
    {
        for(i=0u;i<4u;i++)
        {
            bx=(unsigned char)((i&1u)<<1);
            by=(unsigned char)((i>>1)<<1);

            if((d->cbp_luma&(1u<<i))!=0u)
            {
                n=H264_Residual(d,d->coef[(by<<2)+bx],5u,H264_Zig8,64u);
                d->nzy[by][bx]=n; d->nzy[by][bx+1u]=n;
                d->nzy[by+1u][bx]=n; d->nzy[by+1u][bx+1u]=n;
            }
        }
    }
    else
    {
        unsigned char cat;
        unsigned char maxc;
        const unsigned char *scan;

        cat =(d->mb_type!=H264_I_NxN) ? 1u : 2u;
        maxc=(d->mb_type!=H264_I_NxN) ? 15u : 16u;
        scan=(d->mb_type!=H264_I_NxN) ? &H264_Zig4[1] : H264_Zig4;

        for(i=0u;i<16u;i++)
        {
            bx=(unsigned char)(((i>>2)&1u)<<1 | (i&1u));
            by=(unsigned char)(((i>>3)&1u)<<1 | ((i>>1)&1u));

            if((d->cbp_luma&(1u<<((by&2u)+(bx>>1))))==0u)
            {
                d->nzy[by][bx]=0u;
                continue;
            }

            nza=(bx>0u) ? d->nzy[by][bx-1u]
                        : ((H264_Avail_Left(d)!=0u)?d->left_nzy[by]:H264_NB_CBF);
            nzb=(by>0u) ? d->nzy[by-1u][bx]
                        : ((H264_Avail_Top(d)!=0u)?d->top_nzy[d->mb_x][bx]:H264_NB_CBF);

            if(H264_CBF(d,cat,nza,nzb)!=0u)
            {
                d->nzy[by][bx]=H264_Residual(d,d->coef[(by<<2)+bx],cat,scan,maxc);
            }
            else
            {
                d->nzy[by][bx]=0u;
            }
        }
    }

    /* --- chroma DC --- */
    for(c=0u;c<2u;c++)
    {
        if(d->cbp_chroma!=0u)
        {
            nza=(H264_Avail_Left(d)!=0u) ? d->left_dcc[c]         : H264_NB_CBF;
            nzb=(H264_Avail_Top(d) !=0u) ? d->top_dcc[d->mb_x][c] : H264_NB_CBF;

            if(H264_CBF(d,3u,nza,nzb)!=0u)
            {
                (void)H264_Residual(d,d->coef[25u+c],3u,H264_ScanDC4,4u);
                d->left_dcc[c]=1u;
            }
            else
            {
                d->left_dcc[c]=0u;
            }
        }
        else
        {
            d->left_dcc[c]=0u;
        }
    }

    /* --- chroma AC --- */
    for(c=0u;c<2u;c++)
    {
        for(i=0u;i<4u;i++)
        {
            bx=(unsigned char)(i&1u);
            by=(unsigned char)(i>>1);

            if(d->cbp_chroma<2u)
            {
                d->nzc[c][by][bx]=0u;
                continue;
            }

            nza=(bx>0u) ? d->nzc[c][by][bx-1u]
                        : ((H264_Avail_Left(d)!=0u)?d->left_nzc[c][by]:H264_NB_CBF);
            nzb=(by>0u) ? d->nzc[c][by-1u][bx]
                        : ((H264_Avail_Top(d)!=0u)?d->top_nzc[d->mb_x][c][bx]:H264_NB_CBF);

            if(H264_CBF(d,4u,nza,nzb)!=0u)
            {
                d->nzc[c][by][bx]=H264_Residual(d,d->coef[17u+(c<<2)+i],4u,
                                                &H264_Zig4[1],15u);
            }
            else
            {
                d->nzc[c][by][bx]=0u;
            }
        }
    }

    /* --- publish neighbours --- */
    d->left_i16=(unsigned char)((d->mb_type!=H264_I_NxN)?1u:0u);
    d->top_i16[d->mb_x]=d->left_i16;

    d->left_t8=d->t8x8;
    d->top_t8[d->mb_x]=d->t8x8;

    d->left_cpm=(unsigned char)((d->chroma_mode!=0u)?1u:0u);
    d->top_cpm[d->mb_x]=d->left_cpm;

    d->left_cbpl=d->cbp_luma;
    d->top_cbpl[d->mb_x]=d->cbp_luma;

    d->left_cbpc=d->cbp_chroma;
    d->top_cbpc[d->mb_x]=d->cbp_chroma;

    d->top_dcy[d->mb_x]=d->left_dcy;
    d->top_dcc[d->mb_x][0]=d->left_dcc[0];
    d->top_dcc[d->mb_x][1]=d->left_dcc[1];

    for(by=0u;by<4u;by++) d->left_nzy[by]=d->nzy[by][3];
    for(bx=0u;bx<4u;bx++) d->top_nzy[d->mb_x][bx]=d->nzy[3][bx];

    for(c=0u;c<2u;c++)
    {
        d->left_nzc[c][0]=d->nzc[c][0][1];
        d->left_nzc[c][1]=d->nzc[c][1][1];
        d->top_nzc[d->mb_x][c][0]=d->nzc[c][1][0];
        d->top_nzc[d->mb_x][c][1]=d->nzc[c][1][1];
    }

    for(by=0u;by<4u;by++) d->left_i4[by]=d->i4mode[(by<<2)+3u];
    for(bx=0u;bx<4u;bx++) d->top_i4[d->mb_x][bx]=d->i4mode[12u+bx];

    return 1u;
}

/* -------------------------- slice loop ------------------------------- */

unsigned char H264_Decode_Slice(H264_DEC *d,const unsigned char *rbsp,
                                unsigned long len,const H264_SPS *sps,
                                const H264_PPS *pps,const H264_SLICE *sh)
{
    unsigned int i;

    d->sps=sps;
    d->pps=pps;
    d->mb_w=sps->mb_width;
    d->mb_h=sps->mb_height;
    d->qp=sh->slice_qp;
    d->last_qp_delta=0;
    d->mb_count=0ul;
    d->pcm_count=0ul;
    d->fail=0u;

    for(i=0u;i<H264_MAX_MB_W;i++)
    {
        d->top_i16[i]=0u; d->top_t8[i]=0u; d->top_cpm[i]=0u;
        d->top_cbpl[i]=0u; d->top_cbpc[i]=0u; d->top_dcy[i]=0u;
        d->top_dcc[i][0]=0u; d->top_dcc[i][1]=0u;
        d->top_nzy[i][0]=0u; d->top_nzy[i][1]=0u;
        d->top_nzy[i][2]=0u; d->top_nzy[i][3]=0u;
        d->top_nzc[i][0][0]=0u; d->top_nzc[i][0][1]=0u;
        d->top_nzc[i][1][0]=0u; d->top_nzc[i][1][1]=0u;
        d->top_i4[i][0]=2; d->top_i4[i][1]=2; d->top_i4[i][2]=2; d->top_i4[i][3]=2;
    }

    H264_CB_Init(&d->cb,rbsp,len,sh->cabac_start,sh->slice_qp);

    d->mb_x=(unsigned int)(sh->first_mb%d->mb_w);
    d->mb_y=(unsigned int)(sh->first_mb/d->mb_w);

    for(;;)
    {
        if(d->mb_x==0u)
        {
            d->left_i16=0u; d->left_t8=0u; d->left_cpm=0u;
            d->left_cbpl=0u; d->left_cbpc=0u; d->left_dcy=0u;
            d->left_dcc[0]=0u; d->left_dcc[1]=0u;
            d->left_nzy[0]=0u; d->left_nzy[1]=0u;
            d->left_nzy[2]=0u; d->left_nzy[3]=0u;
            d->left_nzc[0][0]=0u; d->left_nzc[0][1]=0u;
            d->left_nzc[1][0]=0u; d->left_nzc[1][1]=0u;
            d->left_i4[0]=2; d->left_i4[1]=2; d->left_i4[2]=2; d->left_i4[3]=2;
        }

        if(H264_Decode_MB(d)==0u) return 0u;

        d->mb_count++;

        if(H264_CB_Terminate(&d->cb)!=0u) break;

        d->mb_x++;
        if(d->mb_x>=d->mb_w)
        {
            d->mb_x=0u;
            d->mb_y++;
            if(d->mb_y>=d->mb_h) { d->fail=2u; return 0u; }
        }
    }

    return (unsigned char)((d->mb_count==((unsigned long)d->mb_w*(unsigned long)d->mb_h))?1u:0u);
}

/* ====================== reconstruction ============================== */

#ifndef H264_MAX_W
#define H264_MAX_W 320u
#endif
#ifndef H264_MAX_H
#define H264_MAX_H 240u
#endif

typedef struct
{
    unsigned char y[H264_MAX_W*H264_MAX_H];
    unsigned char cb[(H264_MAX_W/2u)*(H264_MAX_H/2u)];
    unsigned char cr[(H264_MAX_W/2u)*(H264_MAX_H/2u)];
    unsigned int  w;
    unsigned int  h;
} H264_PIC_BUF;

static unsigned char H264_Clip255(signed int v)
{
    if(v<0)   return 0u;
    if(v>255) return 255u;
    return (unsigned char)v;
}

/* Neighbour samples are held in one array indexed by a signed offset, so
   the prediction formulas can be transcribed from the spec as written
   instead of being rewritten around unsigned index arithmetic.
     ref4[5+x] is p[x,-1] for x = -1..7      (so ref4[4] is p[-1,-1])
     ref4[3-y] is p[-1,y] for y = -1..3
   and the 8x8 pair is the same shape with 8 and 16 in place of 4 and 8. */
#define P4T(r,x) ((signed int)(r)[5+(x)])
#define P4L(r,y) ((signed int)(r)[3-(y)])
#define P8T(r,x) ((signed int)(r)[9+(x)])
#define P8L(r,y) ((signed int)(r)[7-(y)])

/* flat scaling lists: qmul = 16 * normAdjust << (qp/6 + 2) */
static signed int H264_QMul4(signed int qp,unsigned char pos)
{
    unsigned char r;
    unsigned char c;

    r=(unsigned char)(pos>>2);
    c=(unsigned char)(pos&3u);

    return (signed int)(((unsigned long)H264_Deq4[H264_QpRem6[qp]][(r&1u)+(c&1u)]*16ul)
                        <<(H264_QpDiv6[qp]+2u));
}

static signed int H264_QMul8(signed int qp,unsigned char pos)
{
    unsigned char x;

    x=(unsigned char)(((pos&7u)<<3)|(pos>>3));

    return (signed int)(((unsigned long)H264_Deq8[H264_QpRem6[qp]]
                                 [H264_Deq8Scan[((x>>1)&12u)|(x&3u)]]*16ul)
                        <<H264_QpDiv6[qp]);
}

/* 8.5.12.2 */
static void H264_IDCT4(const signed short *b,unsigned char *dst,unsigned int stride)
{
    signed int t[16];
    signed int e0,e1,e2,e3;
    unsigned char i;

    for(i=0u;i<4u;i++)
    {
        e0=(signed int)b[i*4u+0u]+b[i*4u+2u];
        e1=(signed int)b[i*4u+0u]-b[i*4u+2u];
        e2=((signed int)b[i*4u+1u]>>1)-b[i*4u+3u];
        e3=(signed int)b[i*4u+1u]+((signed int)b[i*4u+3u]>>1);

        t[i*4u+0u]=e0+e3; t[i*4u+1u]=e1+e2;
        t[i*4u+2u]=e1-e2; t[i*4u+3u]=e0-e3;
    }

    for(i=0u;i<4u;i++)
    {
        e0=t[0u*4u+i]+t[2u*4u+i];
        e1=t[0u*4u+i]-t[2u*4u+i];
        e2=(t[1u*4u+i]>>1)-t[3u*4u+i];
        e3=t[1u*4u+i]+(t[3u*4u+i]>>1);

        dst[0u*stride+i]=H264_Clip255((signed int)dst[0u*stride+i]+((e0+e3+32)>>6));
        dst[1u*stride+i]=H264_Clip255((signed int)dst[1u*stride+i]+((e1+e2+32)>>6));
        dst[2u*stride+i]=H264_Clip255((signed int)dst[2u*stride+i]+((e1-e2+32)>>6));
        dst[3u*stride+i]=H264_Clip255((signed int)dst[3u*stride+i]+((e0-e3+32)>>6));
    }
}

/* 8.5.13.2 */
static void H264_IDCT8(const signed short *b,unsigned char *dst,unsigned int stride)
{
    signed int t[64];
    signed int a0,a1,a2,a3,a4,a5,a6,a7;
    signed int c0,c1,c2,c3,c4,c5,c6,c7;
    unsigned char i;

    for(i=0u;i<8u;i++)
    {
        a0=(signed int)b[i*8u+0u]+b[i*8u+4u];
        a2=(signed int)b[i*8u+0u]-b[i*8u+4u];
        a4=((signed int)b[i*8u+2u]>>1)-b[i*8u+6u];
        a6=((signed int)b[i*8u+6u]>>1)+b[i*8u+2u];

        c0=a0+a6; c2=a2+a4; c4=a2-a4; c6=a0-a6;

        a1=-(signed int)b[i*8u+3u]+b[i*8u+5u]-b[i*8u+7u]-((signed int)b[i*8u+7u]>>1);
        a3= (signed int)b[i*8u+1u]+b[i*8u+7u]-b[i*8u+3u]-((signed int)b[i*8u+3u]>>1);
        a5=-(signed int)b[i*8u+1u]+b[i*8u+7u]+b[i*8u+5u]+((signed int)b[i*8u+5u]>>1);
        a7= (signed int)b[i*8u+3u]+b[i*8u+5u]+b[i*8u+1u]+((signed int)b[i*8u+1u]>>1);

        c1=(a7>>2)+a1; c3=a3+(a5>>2); c5=(a3>>2)-a5; c7=a7-(a1>>2);

        t[i*8u+0u]=c0+c7; t[i*8u+7u]=c0-c7;
        t[i*8u+1u]=c2+c5; t[i*8u+6u]=c2-c5;
        t[i*8u+2u]=c4+c3; t[i*8u+5u]=c4-c3;
        t[i*8u+3u]=c6+c1; t[i*8u+4u]=c6-c1;
    }

    for(i=0u;i<8u;i++)
    {
        a0=t[0u*8u+i]+t[4u*8u+i];
        a2=t[0u*8u+i]-t[4u*8u+i];
        a4=(t[2u*8u+i]>>1)-t[6u*8u+i];
        a6=(t[6u*8u+i]>>1)+t[2u*8u+i];

        c0=a0+a6; c2=a2+a4; c4=a2-a4; c6=a0-a6;

        a1=-t[3u*8u+i]+t[5u*8u+i]-t[7u*8u+i]-(t[7u*8u+i]>>1);
        a3= t[1u*8u+i]+t[7u*8u+i]-t[3u*8u+i]-(t[3u*8u+i]>>1);
        a5=-t[1u*8u+i]+t[7u*8u+i]+t[5u*8u+i]+(t[5u*8u+i]>>1);
        a7= t[3u*8u+i]+t[5u*8u+i]+t[1u*8u+i]+(t[1u*8u+i]>>1);

        c1=(a7>>2)+a1; c3=a3+(a5>>2); c5=(a3>>2)-a5; c7=a7-(a1>>2);

        dst[0u*stride+i]=H264_Clip255((signed int)dst[0u*stride+i]+((c0+c7+32)>>6));
        dst[7u*stride+i]=H264_Clip255((signed int)dst[7u*stride+i]+((c0-c7+32)>>6));
        dst[1u*stride+i]=H264_Clip255((signed int)dst[1u*stride+i]+((c2+c5+32)>>6));
        dst[6u*stride+i]=H264_Clip255((signed int)dst[6u*stride+i]+((c2-c5+32)>>6));
        dst[2u*stride+i]=H264_Clip255((signed int)dst[2u*stride+i]+((c4+c3+32)>>6));
        dst[5u*stride+i]=H264_Clip255((signed int)dst[5u*stride+i]+((c4-c3+32)>>6));
        dst[3u*stride+i]=H264_Clip255((signed int)dst[3u*stride+i]+((c6+c1+32)>>6));
        dst[4u*stride+i]=H264_Clip255((signed int)dst[4u*stride+i]+((c6-c1+32)>>6));
    }
}

/* 8.5.10 - luma DC Hadamard; each result becomes coefficient 0 of its block */
static void H264_LumaDC(const signed short *in,signed short out[27][64],signed int qp)
{
    signed int temp[16];
    signed int z0,z1,z2,z3;
    signed int qmul;
    unsigned char i;

    qmul=H264_QMul4(qp,0u);

    for(i=0u;i<4u;i++)
    {
        z0=(signed int)in[4u*i+0u]+in[4u*i+1u];
        z1=(signed int)in[4u*i+0u]-in[4u*i+1u];
        z2=(signed int)in[4u*i+2u]-in[4u*i+3u];
        z3=(signed int)in[4u*i+2u]+in[4u*i+3u];

        temp[4u*i+0u]=z0+z3; temp[4u*i+1u]=z0-z3;
        temp[4u*i+2u]=z1-z2; temp[4u*i+3u]=z1+z2;
    }

    for(i=0u;i<4u;i++)
    {
        z0=temp[0u*4u+i]+temp[2u*4u+i];
        z1=temp[0u*4u+i]-temp[2u*4u+i];
        z2=temp[1u*4u+i]-temp[3u*4u+i];
        z3=temp[1u*4u+i]+temp[3u*4u+i];

        /* The second pass runs down column i of the DC array, and those
           four results belong to raster column i of the 4x4 block grid. */
        out[0u*4u+i][0]=(signed short)(((z0+z3)*qmul+128)>>8);
        out[1u*4u+i][0]=(signed short)(((z1+z2)*qmul+128)>>8);
        out[2u*4u+i][0]=(signed short)(((z1-z2)*qmul+128)>>8);
        out[3u*4u+i][0]=(signed short)(((z0-z3)*qmul+128)>>8);
    }
}

/* 8.5.11 - chroma 2x2 DC */
static void H264_ChromaDC(const signed short *in,signed short out[27][64],
                          unsigned char base,signed int qp)
{
    signed int p,q,r,t,qmul;

    qmul=H264_QMul4(qp,0u);

    p=(signed int)in[0]+in[1];
    q=(signed int)in[0]-in[1];
    r=(signed int)in[2]+in[3];
    t=(signed int)in[2]-in[3];

    out[base+0u][0]=(signed short)(((p+r)*qmul)>>7);
    out[base+1u][0]=(signed short)(((q+t)*qmul)>>7);
    out[base+2u][0]=(signed short)(((p-r)*qmul)>>7);
    out[base+3u][0]=(signed short)(((q-t)*qmul)>>7);
}

/* ------------------------ intra prediction --------------------------- */

static void H264_Pred4(unsigned char *dst,unsigned int stride,
                       const unsigned char *r,
                       unsigned char have_t,unsigned char have_l,
                       unsigned char mode)
{
    signed int v;
    signed int x,y,z;

    /* DC is one value for the whole block; leaving it in the pixel loop
       recomputed the neighbour sum sixteen times over. */
    if(mode==2u)
    {
        if((have_t!=0u)&&(have_l!=0u))
            v=(P4T(r,0)+P4T(r,1)+P4T(r,2)+P4T(r,3)
              +P4L(r,0)+P4L(r,1)+P4L(r,2)+P4L(r,3)+4)>>3;
        else if(have_l!=0u)
            v=(P4L(r,0)+P4L(r,1)+P4L(r,2)+P4L(r,3)+2)>>2;
        else if(have_t!=0u)
            v=(P4T(r,0)+P4T(r,1)+P4T(r,2)+P4T(r,3)+2)>>2;
        else v=128;

        for(y=0;y<4;y++)
            for(x=0;x<4;x++)
                dst[(unsigned int)y*stride+(unsigned int)x]=(unsigned char)v;
        return;
    }

    for(y=0;y<4;y++)
    {
        for(x=0;x<4;x++)
        {
            switch(mode)
            {
            case 0:  v=P4T(r,x); break;
            case 1:  v=P4L(r,y); break;
            case 2:
                v=128;
                break;
            case 3:
                if((x==3)&&(y==3)) v=(P4T(r,6)+3*P4T(r,7)+2)>>2;
                else               v=(P4T(r,x+y)+2*P4T(r,x+y+1)+P4T(r,x+y+2)+2)>>2;
                break;
            case 4:
                if(x>y)      v=(P4T(r,x-y-2)+2*P4T(r,x-y-1)+P4T(r,x-y)+2)>>2;
                else if(x<y) v=(P4L(r,y-x-2)+2*P4L(r,y-x-1)+P4L(r,y-x)+2)>>2;
                else         v=(P4T(r,0)+2*P4T(r,-1)+P4L(r,0)+2)>>2;
                break;
            case 5:
                z=2*x-y;
                if((z>=0)&&((z&1)==0))      v=(P4T(r,x-(y>>1)-1)+P4T(r,x-(y>>1))+1)>>1;
                else if((z>=0)&&((z&1)!=0)) v=(P4T(r,x-(y>>1)-2)+2*P4T(r,x-(y>>1)-1)+P4T(r,x-(y>>1))+2)>>2;
                else if(z==-1)              v=(P4L(r,0)+2*P4T(r,-1)+P4T(r,0)+2)>>2;
                else                        v=(P4L(r,y-1)+2*P4L(r,y-2)+P4L(r,y-3)+2)>>2;
                break;
            case 6:
                z=2*y-x;
                if((z>=0)&&((z&1)==0))      v=(P4L(r,y-(x>>1)-1)+P4L(r,y-(x>>1))+1)>>1;
                else if((z>=0)&&((z&1)!=0)) v=(P4L(r,y-(x>>1)-2)+2*P4L(r,y-(x>>1)-1)+P4L(r,y-(x>>1))+2)>>2;
                else if(z==-1)              v=(P4L(r,0)+2*P4T(r,-1)+P4T(r,0)+2)>>2;
                else                        v=(P4T(r,x-2*y-1)+2*P4T(r,x-2*y-2)+P4T(r,x-2*y-3)+2)>>2;
                break;
            case 7:
                if((y&1)==0) v=(P4T(r,x+(y>>1))+P4T(r,x+(y>>1)+1)+1)>>1;
                else         v=(P4T(r,x+(y>>1))+2*P4T(r,x+(y>>1)+1)+P4T(r,x+(y>>1)+2)+2)>>2;
                break;
            default:
                z=x+2*y;
                if((z<5)&&((z&1)==0))      v=(P4L(r,y+(x>>1))+P4L(r,y+(x>>1)+1)+1)>>1;
                else if((z<5)&&((z&1)!=0)) v=(P4L(r,y+(x>>1))+2*P4L(r,y+(x>>1)+1)+P4L(r,y+(x>>1)+2)+2)>>2;
                else if(z==5)              v=(P4L(r,2)+3*P4L(r,3)+2)>>2;
                else                       v=P4L(r,3);
                break;
            }
            dst[(unsigned int)y*stride+(unsigned int)x]=(unsigned char)v;
        }
    }
}

/* 8.3.2.2.1 - reference sample filtering ahead of Intra_8x8 */
static void H264_Filter8(const unsigned char *r,unsigned char *f,
                         unsigned char have_t,unsigned char have_l,
                         unsigned char have_tl)
{
    signed int i;

    for(i=0;i<25;i++) f[i]=r[i];

    if(have_t!=0u)
    {
        if(have_tl!=0u) f[9+0]=(unsigned char)((P8T(r,-1)+2*P8T(r,0)+P8T(r,1)+2)>>2);
        else            f[9+0]=(unsigned char)((3*P8T(r,0)+P8T(r,1)+2)>>2);

        for(i=1;i<=14;i++)
            f[9+i]=(unsigned char)((P8T(r,i-1)+2*P8T(r,i)+P8T(r,i+1)+2)>>2);

        f[9+15]=(unsigned char)((P8T(r,14)+3*P8T(r,15)+2)>>2);
    }

    if((have_tl!=0u)&&(have_t!=0u)&&(have_l!=0u))
        f[8]=(unsigned char)((P8T(r,0)+2*P8T(r,-1)+P8L(r,0)+2)>>2);
    else if((have_tl!=0u)&&(have_t!=0u))
        f[8]=(unsigned char)((3*P8T(r,-1)+P8T(r,0)+2)>>2);
    else if((have_tl!=0u)&&(have_l!=0u))
        f[8]=(unsigned char)((3*P8T(r,-1)+P8L(r,0)+2)>>2);

    if(have_l!=0u)
    {
        if(have_tl!=0u) f[7-0]=(unsigned char)((P8T(r,-1)+2*P8L(r,0)+P8L(r,1)+2)>>2);
        else            f[7-0]=(unsigned char)((3*P8L(r,0)+P8L(r,1)+2)>>2);

        for(i=1;i<=6;i++)
            f[7-i]=(unsigned char)((P8L(r,i-1)+2*P8L(r,i)+P8L(r,i+1)+2)>>2);

        f[7-7]=(unsigned char)((P8L(r,6)+3*P8L(r,7)+2)>>2);
    }
}

static void H264_Pred8(unsigned char *dst,unsigned int stride,
                       const unsigned char *r,
                       unsigned char have_t,unsigned char have_l,
                       unsigned char mode)
{
    signed int v;
    signed int x,y,z;

    /* As for 4x4: the DC sum was being redone for all sixty-four pixels. */
    if(mode==2u)
    {
        v=0;
        if((have_t!=0u)&&(have_l!=0u))
        {
            for(z=0;z<8;z++) v+=P8T(r,z)+P8L(r,z);
            v=(v+8)>>4;
        }
        else if(have_l!=0u)
        {
            for(z=0;z<8;z++) v+=P8L(r,z);
            v=(v+4)>>3;
        }
        else if(have_t!=0u)
        {
            for(z=0;z<8;z++) v+=P8T(r,z);
            v=(v+4)>>3;
        }
        else v=128;

        for(y=0;y<8;y++)
            for(x=0;x<8;x++)
                dst[(unsigned int)y*stride+(unsigned int)x]=(unsigned char)v;
        return;
    }

    for(y=0;y<8;y++)
    {
        for(x=0;x<8;x++)
        {
            switch(mode)
            {
            case 0:  v=P8T(r,x); break;
            case 1:  v=P8L(r,y); break;
            case 2:
                v=128;
                break;
            case 3:
                if((x==7)&&(y==7)) v=(P8T(r,14)+3*P8T(r,15)+2)>>2;
                else               v=(P8T(r,x+y)+2*P8T(r,x+y+1)+P8T(r,x+y+2)+2)>>2;
                break;
            case 4:
                if(x>y)      v=(P8T(r,x-y-2)+2*P8T(r,x-y-1)+P8T(r,x-y)+2)>>2;
                else if(x<y) v=(P8L(r,y-x-2)+2*P8L(r,y-x-1)+P8L(r,y-x)+2)>>2;
                else         v=(P8T(r,0)+2*P8T(r,-1)+P8L(r,0)+2)>>2;
                break;
            case 5:
                z=2*x-y;
                if((z>=0)&&((z&1)==0))      v=(P8T(r,x-(y>>1)-1)+P8T(r,x-(y>>1))+1)>>1;
                else if((z>=0)&&((z&1)!=0)) v=(P8T(r,x-(y>>1)-2)+2*P8T(r,x-(y>>1)-1)+P8T(r,x-(y>>1))+2)>>2;
                else if(z==-1)              v=(P8L(r,0)+2*P8T(r,-1)+P8T(r,0)+2)>>2;
                else                        v=(P8L(r,y-2*x-1)+2*P8L(r,y-2*x-2)+P8L(r,y-2*x-3)+2)>>2;
                break;
            case 6:
                z=2*y-x;
                if((z>=0)&&((z&1)==0))      v=(P8L(r,y-(x>>1)-1)+P8L(r,y-(x>>1))+1)>>1;
                else if((z>=0)&&((z&1)!=0)) v=(P8L(r,y-(x>>1)-2)+2*P8L(r,y-(x>>1)-1)+P8L(r,y-(x>>1))+2)>>2;
                else if(z==-1)              v=(P8L(r,0)+2*P8T(r,-1)+P8T(r,0)+2)>>2;
                else                        v=(P8T(r,x-2*y-1)+2*P8T(r,x-2*y-2)+P8T(r,x-2*y-3)+2)>>2;
                break;
            case 7:
                if((y&1)==0) v=(P8T(r,x+(y>>1))+P8T(r,x+(y>>1)+1)+1)>>1;
                else         v=(P8T(r,x+(y>>1))+2*P8T(r,x+(y>>1)+1)+P8T(r,x+(y>>1)+2)+2)>>2;
                break;
            default:
                z=x+2*y;
                if((z<13)&&((z&1)==0))      v=(P8L(r,y+(x>>1))+P8L(r,y+(x>>1)+1)+1)>>1;
                else if((z<13)&&((z&1)!=0)) v=(P8L(r,y+(x>>1))+2*P8L(r,y+(x>>1)+1)+P8L(r,y+(x>>1)+2)+2)>>2;
                else if(z==13)              v=(P8L(r,6)+3*P8L(r,7)+2)>>2;
                else                        v=P8L(r,7);
                break;
            }
            dst[(unsigned int)y*stride+(unsigned int)x]=(unsigned char)v;
        }
    }
}

/* Intra_16x16 luma and the 8x8 chroma block share four whole-block modes.
   t[] is p[0..n-1,-1], l[] is p[-1,0..n-1], tl is p[-1,-1].
   mode is in the luma numbering: 0 V, 1 H, 2 DC, 3 Plane. */
static void H264_PredFull(unsigned char *dst,unsigned int stride,
                          const unsigned char *t,const unsigned char *l,
                          unsigned char tl,
                          unsigned char have_t,unsigned char have_l,
                          unsigned char mode,unsigned char n)
{
    signed int v,a,b,c,hh,vv;
    signed int x,y;
    signed int half;
    signed int shift;

    half=(signed int)(n>>1);
    shift=(n==16u)?4:3;

    switch(mode)
    {
    case 0:
        for(y=0;y<(signed int)n;y++) for(x=0;x<(signed int)n;x++)
            dst[(unsigned int)y*stride+(unsigned int)x]=t[x];
        break;

    case 1:
        for(y=0;y<(signed int)n;y++) for(x=0;x<(signed int)n;x++)
            dst[(unsigned int)y*stride+(unsigned int)x]=l[y];
        break;

    case 2:
        if((have_t!=0u)&&(have_l!=0u))
        {
            v=0;
            for(x=0;x<(signed int)n;x++) v+=(signed int)t[x]+l[x];
            v=(v+(signed int)n)>>(shift+1);
        }
        else if(have_l!=0u)
        {
            v=0;
            for(y=0;y<(signed int)n;y++) v+=l[y];
            v=(v+(signed int)(n>>1))>>shift;
        }
        else if(have_t!=0u)
        {
            v=0;
            for(x=0;x<(signed int)n;x++) v+=t[x];
            v=(v+(signed int)(n>>1))>>shift;
        }
        else v=128;

        for(y=0;y<(signed int)n;y++) for(x=0;x<(signed int)n;x++)
            dst[(unsigned int)y*stride+(unsigned int)x]=(unsigned char)v;
        break;

    default:
        hh=0; vv=0;
        for(x=0;x<half;x++)
        {
            hh+=(x+1)*((signed int)t[half+x]-(signed int)((x==(half-1))?tl:t[half-2-x]));
            vv+=(x+1)*((signed int)l[half+x]-(signed int)((x==(half-1))?tl:l[half-2-x]));
        }

        a=16*((signed int)l[n-1u]+(signed int)t[n-1u]);

        if(n==16u) { b=(5*hh+32)>>6; c=(5*vv+32)>>6; }
        else       { b=(17*hh+16)>>5; c=(17*vv+16)>>5; }

        for(y=0;y<(signed int)n;y++) for(x=0;x<(signed int)n;x++)
        {
            v=(a+b*(x-(half-1))+c*(y-(half-1))+16)>>5;
            dst[(unsigned int)y*stride+(unsigned int)x]=H264_Clip255(v);
        }
        break;
    }
}

/* 8.3.4 - chroma prediction.  Mode numbering is chroma's own: 0 DC, 1 H,
   2 V, 3 Plane.  DC is not one average over the block: each 4x4 quadrant
   takes its own neighbour subset, and the corner quadrants prefer the
   edge they sit against. */
static void H264_PredChroma(unsigned char *dst,unsigned int stride,
                            const unsigned char *t,const unsigned char *l,
                            unsigned char tl,
                            unsigned char have_t,unsigned char have_l,
                            unsigned char mode)
{
    signed int x,y,sx,sy,v,st,sl;

    if(mode==1u)
    {
        for(y=0;y<8;y++) for(x=0;x<8;x++)
            dst[(unsigned int)y*stride+(unsigned int)x]=l[y];
        return;
    }

    if(mode==2u)
    {
        for(y=0;y<8;y++) for(x=0;x<8;x++)
            dst[(unsigned int)y*stride+(unsigned int)x]=t[x];
        return;
    }

    if(mode==3u)
    {
        H264_PredFull(dst,stride,t,l,tl,have_t,have_l,3u,8u);
        return;
    }

    for(sy=0;sy<8;sy+=4)
    {
        for(sx=0;sx<8;sx+=4)
        {
            st=0; sl=0;
            for(x=0;x<4;x++) { st+=t[sx+x]; sl+=l[sy+x]; }

            if(((sx==0)&&(sy==0))||((sx>0)&&(sy>0)))
            {
                if((have_t!=0u)&&(have_l!=0u)) v=(st+sl+4)>>3;
                else if(have_l!=0u)            v=(sl+2)>>2;
                else if(have_t!=0u)            v=(st+2)>>2;
                else                           v=128;
            }
            else if(sx>0)
            {
                if(have_t!=0u)      v=(st+2)>>2;
                else if(have_l!=0u) v=(sl+2)>>2;
                else                v=128;
            }
            else
            {
                if(have_l!=0u)      v=(sl+2)>>2;
                else if(have_t!=0u) v=(st+2)>>2;
                else                v=128;
            }

            for(y=0;y<4;y++) for(x=0;x<4;x++)
                dst[(unsigned int)(sy+y)*stride+(unsigned int)(sx+x)]=(unsigned char)v;
        }
    }
}

/* ---------------------- macroblock reconstruction -------------------- */

/* Gathers p[-1,-1], p[0..2n-1,-1] and p[-1,0..n-1] for a block at (px,py)
   into the signed-offset array the predictors use.  Unavailable
   above-right samples are replaced by the last available one, per
   8.3.1.2.  n is 4 or 8. */
static void H264_Ref(const unsigned char *plane,unsigned int stride,
                     unsigned int px,unsigned int py,unsigned char n,
                     unsigned char have_t,unsigned char have_l,
                     unsigned char have_tl,unsigned char have_tr,
                     unsigned char *r)
{
    unsigned char i;
    unsigned char mid;
    unsigned char last;

    mid=(unsigned char)((n==4u)?5u:9u);      /* index of p[0,-1] */

    for(i=0u;i<(unsigned char)(2u*n+1u);i++) r[mid-1u+i]=128u;
    for(i=0u;i<n;i++) r[mid-2u-i]=128u;

    if(have_t!=0u)
    {
        for(i=0u;i<n;i++) r[mid+i]=plane[(py-1u)*stride+px+i];

        last=r[mid+n-1u];
        for(i=0u;i<n;i++)
        {
            r[mid+n+i]=(have_tr!=0u) ? plane[(py-1u)*stride+px+n+i] : last;
        }
    }

    if(have_l!=0u)
    {
        for(i=0u;i<n;i++) r[mid-2u-i]=plane[(py+i)*stride+px-1u];
    }

    if(have_tl!=0u) r[mid-1u]=plane[(py-1u)*stride+px-1u];
}

/* Block-scan index of the 4x4 block at (bx,by). */
static unsigned char H264_BlkIdx(unsigned char bx,unsigned char by)
{
    return (unsigned char)((bx&1u)|((by&1u)<<1)|(((bx>>1)&1u)<<2)|(((by>>1)&1u)<<3));
}

/* Reconstructs the macroblock just decoded into the picture. */
void H264_Recon_MB(H264_DEC *d,H264_PIC_BUF *pic)
{
    unsigned char r[25];
    unsigned char f[25];
    unsigned char t[16];
    unsigned char l[16];
    unsigned char bx,by,i,c,n;
    unsigned int  px,py,stride,cstride;
    unsigned char *plane;
    unsigned char at,al,atl,atr;
    signed int qp,qpc;
    signed short tmp[64];

    stride =pic->w;
    cstride=pic->w>>1;

    px=d->mb_x*16u;
    py=d->mb_y*16u;

    at =(unsigned char)((d->mb_y>0u)?1u:0u);
    al =(unsigned char)((d->mb_x>0u)?1u:0u);
    atl=(unsigned char)(((at!=0u)&&(al!=0u))?1u:0u);

    /* ---- I_PCM: samples are the picture ---- */
    if(d->mb_type==H264_I_PCM)
    {
        for(by=0u;by<16u;by++)
            for(bx=0u;bx<16u;bx++)
                pic->y[(py+by)*stride+px+bx]=d->pcm[by*16u+bx];

        for(by=0u;by<8u;by++)
            for(bx=0u;bx<8u;bx++)
            {
                pic->cb[((py>>1)+by)*cstride+(px>>1)+bx]=d->pcm[256u+by*8u+bx];
                pic->cr[((py>>1)+by)*cstride+(px>>1)+bx]=d->pcm[320u+by*8u+bx];
            }
        return;
    }

    qp=d->qp;

    /* ---- luma prediction and residual ---- */
    if(d->mb_type!=H264_I_NxN)
    {
        /* Intra_16x16: whole-block prediction, then DC + AC per 4x4 */
        unsigned char mode;

        mode=(unsigned char)((d->mb_type-1u)%4u);

        for(i=0u;i<16u;i++)
        {
            t[i]=(at!=0u) ? pic->y[(py-1u)*stride+px+i] : 128u;
            l[i]=(al!=0u) ? pic->y[(py+i)*stride+px-1u] : 128u;
        }

        H264_PredFull(&pic->y[py*stride+px],stride,t,l,
                      (atl!=0u)?pic->y[(py-1u)*stride+px-1u]:128u,
                      at,al,mode,16u);

        H264_LumaDC(d->coef[16],d->coef,qp);

        for(i=0u;i<16u;i++)
        {
            bx=(unsigned char)(((i>>2)&1u)<<1 | (i&1u));
            by=(unsigned char)(((i>>3)&1u)<<1 | ((i>>1)&1u));

            for(n=0u;n<16u;n++)
            {
                tmp[n]=(n==0u) ? d->coef[(by<<2)+bx][0]
                               : (signed short)(((signed int)d->coef[(by<<2)+bx][n]
                                                 *H264_QMul4(qp,n)+32)>>6);
            }

            H264_IDCT4(tmp,&pic->y[(py+by*4u)*stride+px+bx*4u],stride);
        }
    }
    else if(d->t8x8!=0u)
    {
        for(i=0u;i<4u;i++)
        {
            bx=(unsigned char)((i&1u)<<1);
            by=(unsigned char)((i>>1)<<1);

            atr=(unsigned char)(((i==0u)&&(at!=0u)) ? 1u :
                                ((i==1u)&&(at!=0u)&&(d->mb_x+1u<d->mb_w)) ? 1u :
                                ((i==2u) ? 1u : 0u));

            H264_Ref(pic->y,stride,px+(unsigned int)bx*4u,py+(unsigned int)by*4u,8u,
                     (unsigned char)((by>0u)?1u:at),
                     (unsigned char)((bx>0u)?1u:al),
                     (unsigned char)(((bx>0u)||(by>0u))?
                                     (((bx>0u)&&(by>0u))?1u:((bx>0u)?at:al)):atl),
                     atr,r);

            H264_Filter8(r,f,
                         (unsigned char)((by>0u)?1u:at),
                         (unsigned char)((bx>0u)?1u:al),
                         (unsigned char)(((bx>0u)||(by>0u))?
                                         (((bx>0u)&&(by>0u))?1u:((bx>0u)?at:al)):atl));

            H264_Pred8(&pic->y[(py+(unsigned int)by*4u)*stride+px+(unsigned int)bx*4u],
                       stride,f,
                       (unsigned char)((by>0u)?1u:at),
                       (unsigned char)((bx>0u)?1u:al),
                       (unsigned char)d->i4mode[(by<<2)+bx]);

            if((d->cbp_luma&(1u<<i))!=0u)
            {
                for(n=0u;n<64u;n++)
                {
                    tmp[n]=(signed short)(((signed int)d->coef[(by<<2)+bx][n]
                                           *H264_QMul8(qp,n)+32)>>6);
                }
                H264_IDCT8(tmp,&pic->y[(py+(unsigned int)by*4u)*stride
                                       +px+(unsigned int)bx*4u],stride);
            }
        }
    }
    else
    {
        for(i=0u;i<16u;i++)
        {
            bx=(unsigned char)(((i>>2)&1u)<<1 | (i&1u));
            by=(unsigned char)(((i>>3)&1u)<<1 | ((i>>1)&1u));

            /* above-right: inside the MB it must already be decoded */
            if(by==0u)
                atr=(unsigned char)((bx<3u)?at:((at!=0u)&&(d->mb_x+1u<d->mb_w))?1u:0u);
            else if(bx==3u)
                atr=0u;
            else
                atr=(unsigned char)((H264_BlkIdx((unsigned char)(bx+1u),
                                                 (unsigned char)(by-1u))<i)?1u:0u);

            H264_Ref(pic->y,stride,px+(unsigned int)bx*4u,py+(unsigned int)by*4u,4u,
                     (unsigned char)((by>0u)?1u:at),
                     (unsigned char)((bx>0u)?1u:al),
                     (unsigned char)(((bx>0u)&&(by>0u))?1u:((bx>0u)?at:((by>0u)?al:atl))),
                     atr,r);

            H264_Pred4(&pic->y[(py+(unsigned int)by*4u)*stride+px+(unsigned int)bx*4u],
                       stride,r,
                       (unsigned char)((by>0u)?1u:at),
                       (unsigned char)((bx>0u)?1u:al),
                       (unsigned char)d->i4mode[(by<<2)+bx]);

            if((d->cbp_luma&(1u<<((by&2u)+(bx>>1))))!=0u)
            {
                for(n=0u;n<16u;n++)
                {
                    tmp[n]=(signed short)(((signed int)d->coef[(by<<2)+bx][n]
                                           *H264_QMul4(qp,n)+32)>>6);
                }
                H264_IDCT4(tmp,&pic->y[(py+(unsigned int)by*4u)*stride
                                       +px+(unsigned int)bx*4u],stride);
            }
        }
    }

    /* ---- chroma ---- */
    for(c=0u;c<2u;c++)
    {
        signed int off;

        plane=(c==0u)?pic->cb:pic->cr;
        off=(c==0u)?d->pps->chroma_qp_index_offset:d->pps->second_chroma_qp_index_offset;

        qpc=qp+off;
        if(qpc<0) qpc=0;
        if(qpc>51) qpc=51;
        qpc=(signed int)H264_ChromaQP[qpc];

        for(i=0u;i<8u;i++)
        {
            t[i]=(at!=0u) ? plane[((py>>1)-1u)*cstride+(px>>1)+i] : 128u;
            l[i]=(al!=0u) ? plane[((py>>1)+i)*cstride+(px>>1)-1u] : 128u;
        }

        H264_PredChroma(&plane[(py>>1)*cstride+(px>>1)],cstride,t,l,
                        (atl!=0u)?plane[((py>>1)-1u)*cstride+(px>>1)-1u]:128u,
                        at,al,d->chroma_mode);

        if(d->cbp_chroma!=0u)
        {
            H264_ChromaDC(d->coef[25u+c],d->coef,(unsigned char)(17u+(c<<2)),qpc);

            for(i=0u;i<4u;i++)
            {
                bx=(unsigned char)(i&1u);
                by=(unsigned char)(i>>1);

                for(n=0u;n<16u;n++)
                {
                    tmp[n]=(n==0u) ? d->coef[17u+(c<<2)+i][0]
                                   : (signed short)(((signed int)d->coef[17u+(c<<2)+i][n]
                                                     *H264_QMul4(qpc,n)+32)>>6);
                }

                H264_IDCT4(tmp,&plane[((py>>1)+(unsigned int)by*4u)*cstride
                                      +(px>>1)+(unsigned int)bx*4u],cstride);
            }
        }
    }
}

/* ------------------------- preview output ---------------------------- */

/* Converts one output row to RGB565, halving the picture as it goes: each
   output pixel averages a 2x2 luma block, and the chroma planes are
   already at that scale so they are read directly.  Row at a time, so no
   intermediate full-size RGB buffer is needed.

   BT.601 limited range, the H.264 default:
     R = (298*C           + 409*E + 128) >> 8
     G = (298*C - 100*D   - 208*E + 128) >> 8
     B = (298*C + 516*D           + 128) >> 8
   with C = Y-16, D = U-128, E = V-128. */
void H264_Row_RGB565(const H264_PIC_BUF *pic,unsigned int oy,unsigned short *row)
{
    unsigned int ow;
    unsigned int x;
    unsigned int sy;
    unsigned int cstride;
    signed int c,dd,e,r,g,b;

    ow=pic->w>>1;
    sy=oy<<1;
    cstride=pic->w>>1;

    for(x=0u;x<ow;x++)
    {
        c=(signed int)pic->y[sy*pic->w+(x<<1)]
         +(signed int)pic->y[sy*pic->w+(x<<1)+1u]
         +(signed int)pic->y[(sy+1u)*pic->w+(x<<1)]
         +(signed int)pic->y[(sy+1u)*pic->w+(x<<1)+1u];
        c=(c+2)>>2;

        c-=16;
        dd=(signed int)pic->cb[oy*cstride+x]-128;
        e =(signed int)pic->cr[oy*cstride+x]-128;

        r=(298*c+409*e+128)>>8;
        g=(298*c-100*dd-208*e+128)>>8;
        b=(298*c+516*dd+128)>>8;

        row[x]=(unsigned short)(((unsigned int)(H264_Clip255(r)&0xF8u)<<8)
                               |((unsigned int)(H264_Clip255(g)&0xFCu)<<3)
                               |((unsigned int)(H264_Clip255(b))>>3));
    }
}

/* Decodes one keyframe NAL into pic.  The NAL is the raw slice, starting
   at its header byte; sps and pps must already be parsed from the avcC.
   Returns 0 if the stream asks for anything outside the supported set. */
unsigned char H264_Decode_Keyframe_RBSP(H264_DEC *d,H264_PIC_BUF *pic,
                                        const unsigned char *rbsp,unsigned long r,
                                        const H264_SPS *sps,const H264_PPS *pps);

unsigned char H264_Decode_Keyframe(H264_DEC *d,H264_PIC_BUF *pic,
                                   const unsigned char *nal,unsigned long nal_len,
                                   unsigned char *scratch,unsigned long scratch_len,
                                   const H264_SPS *sps,const H264_PPS *pps)
{
    unsigned long r;

    if((nal_len<2ul)||((nal[0]&0x1Fu)!=5u)) return 0u;

    r=H264_Unescape(&nal[1],nal_len-1ul,scratch,scratch_len);

    return H264_Decode_Keyframe_RBSP(d,pic,scratch,r,sps,pps);
}

/* Same, for a slice whose RBSP has already been unescaped - which lets the
   caller strip the emulation bytes straight out of the file and avoid
   holding the escaped copy as well. */
unsigned char H264_Decode_Keyframe_RBSP(H264_DEC *d,H264_PIC_BUF *pic,
                                        const unsigned char *rbsp,unsigned long r,
                                        const H264_SPS *sps,const H264_PPS *pps)
{
    H264_SLICE sh;
    unsigned int i;

    if(r<2ul) return 0u;
    if((sps->valid==0u)||(pps->valid==0u))  return 0u;
    if((sps->mb_width*16u>H264_MAX_W)||(sps->mb_height*16u>H264_MAX_H)) return 0u;

    {
    const unsigned char *scratch=rbsp;
    if(H264_Parse_Slice_Header(scratch,r,5u,sps,pps,&sh)==0u) return 0u;
    if(sh.first_mb!=0ul) return 0u;

    pic->w=sps->mb_width*16u;
    pic->h=sps->mb_height*16u;

    d->sps=sps;
    d->pps=pps;
    d->mb_w=sps->mb_width;
    d->mb_h=sps->mb_height;
    d->qp=sh.slice_qp;
    d->last_qp_delta=0;
    d->mb_count=0ul;
    d->pcm_count=0ul;
    d->fail=0u;

    for(i=0u;i<H264_MAX_MB_W;i++)
    {
        d->top_i16[i]=0u; d->top_t8[i]=0u; d->top_cpm[i]=0u;
        d->top_cbpl[i]=0u; d->top_cbpc[i]=0u; d->top_dcy[i]=0u;
        d->top_dcc[i][0]=0u; d->top_dcc[i][1]=0u;
        d->top_nzy[i][0]=0u; d->top_nzy[i][1]=0u;
        d->top_nzy[i][2]=0u; d->top_nzy[i][3]=0u;
        d->top_nzc[i][0][0]=0u; d->top_nzc[i][0][1]=0u;
        d->top_nzc[i][1][0]=0u; d->top_nzc[i][1][1]=0u;
        d->top_i4[i][0]=2; d->top_i4[i][1]=2; d->top_i4[i][2]=2; d->top_i4[i][3]=2;
    }

    H264_CB_Init(&d->cb,scratch,r,sh.cabac_start,sh.slice_qp);
    d->mb_x=0u;
    d->mb_y=0u;

    for(;;)
    {
        if(d->mb_x==0u)
        {
            d->left_i16=0u; d->left_t8=0u; d->left_cpm=0u;
            d->left_cbpl=0u; d->left_cbpc=0u; d->left_dcy=0u;
            d->left_dcc[0]=0u; d->left_dcc[1]=0u;
            d->left_nzy[0]=0u; d->left_nzy[1]=0u;
            d->left_nzy[2]=0u; d->left_nzy[3]=0u;
            d->left_nzc[0][0]=0u; d->left_nzc[0][1]=0u;
            d->left_nzc[1][0]=0u; d->left_nzc[1][1]=0u;
            d->left_i4[0]=2; d->left_i4[1]=2; d->left_i4[2]=2; d->left_i4[3]=2;
        }

        if(H264_Decode_MB(d)==0u) return 0u;

        H264_Recon_MB(d,pic);
        d->mb_count++;

        if(H264_CB_Terminate(&d->cb)!=0u) break;

        d->mb_x++;
        if(d->mb_x>=d->mb_w)
        {
            d->mb_x=0u;
            d->mb_y++;
            if(d->mb_y>=d->mb_h) return 0u;
        }
    }

    return (unsigned char)((d->mb_count==((unsigned long)d->mb_w*(unsigned long)d->mb_h))?1u:0u);
    }
}

#endif
