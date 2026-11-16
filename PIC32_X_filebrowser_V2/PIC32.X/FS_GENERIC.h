#ifndef FS_GENERIC_H
#define FS_GENERIC_H

#include <string.h>

#define FS_TYPE_NONE   0u
#define FS_TYPE_FAT32  1u
#define FS_TYPE_EXFAT  2u
#define FS_TYPE_NTFS   3u

#define VFS_TYPE_DIR   1u
#define VFS_TYPE_AVI   2u
#define VFS_TYPE_FILE  3u
#define VFS_TYPE_JPEG  4u
#define VFS_TYPE_BMP   5u
#define VFS_TYPE_MKV   6u
#define VFS_TYPE_G3D   7u     /* 3D scene for the FPGA renderer */
#define VFS_TYPE_MOD   8u     /* tracker modules for the FPGA player */
#define VFS_TYPE_S3M   9u
#define VFS_TYPE_XM    10u
#define VFS_TYPE_IT    11u
#define VFS_TYPE_OTHER 12u    /* content checked, nothing known: reported as FILE */
#define VFS_TYPE_MP3   13u    /* audio files for the FPGA player */
#define VFS_TYPE_FLAC  14u
#define VFS_TYPE_WAV   15u

#define VFS_MAX_ENTRIES       10000u
#define VFS_NAME_LEN            80u
#define FS_INFO_SLOTS           16u   /* entries whose name and size are kept */
#define NTFS_MAX_RECORD_SIZE  4096u
#define NTFS_MAX_RUNS          512u   /* runs of one extent: a 1 KB record holds fewer */
#define FILE_RESIDENT_MAX     2048u

unsigned char Filename_Table[50];

static unsigned char g_fs_type=FS_TYPE_NONE;
static unsigned long g_fs_part_start=0ul;
static unsigned int g_fs_bytes_per_sector=512u;
static unsigned long g_fs_sectors_per_cluster=1ul;
static unsigned long g_fs_cluster_size_bytes=512ul;

/*
 * g_fs_multi_read_ok: when 1, FS_Read_CurrentFile_Bytes may issue multi-
 * sector READ(10) commands (faster but some drives require single-sector for
 * reliability).  Set to 1 only during AVI_Stream_Range; 0 everywhere else
 * (file browser, FAT reads, seek cache, JPEG thumbnails, etc.).
 */
static unsigned char g_fs_multi_read_ok = 0u;

/* One FAT sector held in RAM.  A FAT sector carries 128 consecutive chain
   entries, so walking a chain without this costs a 512 byte USB read per
   cluster - over a thousand of them to reach the middle of a 100 MB file,
   which is seconds per seek.  With it, the same walk is a handful. */
static unsigned char g_fat_cache[512];
static unsigned long g_fat_cache_sector = 0xFFFFFFFFul;

static void FS_Fat_Cache_Reset(void)
{
    g_fat_cache_sector = 0xFFFFFFFFul;
}

static const unsigned char *FS_Fat_Sector(unsigned long sector)
{
    if(g_fat_cache_sector != sector)
    {
        Read_Buffer(sector, g_fat_cache);
        g_fat_cache_sector = sector;
    }

    return g_fat_cache;
}

/* Entries in the sparse cluster-chain map, per open file. */
#define FS_CHAIN_SLOTS 64u


static unsigned long g_fat32_reserved_sectors=0ul;
static unsigned long g_fat32_sectors_per_fat=0ul;
static unsigned long g_fat32_fat_start=0ul;
static unsigned long g_fat32_data_start=0ul;
static unsigned long g_fat32_root_cluster=2ul;
static unsigned char g_fat32_num_fats=0u;

static unsigned long g_exfat_fat_start=0ul;
static unsigned long g_exfat_fat_length=0ul;
static unsigned long g_exfat_cluster_heap_start=0ul;
static unsigned long g_exfat_cluster_count=0ul;
static unsigned long g_exfat_root_cluster=2ul;
static unsigned char g_exfat_num_fats=0u;

static unsigned long long g_ntfs_total_sectors=0ull;
static unsigned long long g_ntfs_mft_lcn=0ull;
static unsigned int g_ntfs_record_size=1024u;
static unsigned int g_ntfs_index_block_size=4096u;
static unsigned char g_ntfs_record_buf[NTFS_MAX_RECORD_SIZE];

static unsigned long long g_current_dir_id=0ull;

/* A browser entry is 8 bytes, so that a folder of VFS_MAX_ENTRIES fits in
   80 KB: its type and where its directory record is.  Name, size and first
   cluster are read back from there when asked for (FS_Entry_Info), the last
   FS_INFO_SLOTS kept.  FAT32 and exFAT: the directory cluster holding the
   entry's first record, and that record's index in the cluster; NTFS: the
   MFT record number. */
typedef struct
{
    unsigned long where;      /* cluster; NTFS: record number bits 31..0       */
    unsigned long info;       /* 23..0: record index (NTFS: number bits 47..32),
                                 31..24: type                                  */
} FS_BROWSER_ENTRY;

static FS_BROWSER_ENTRY g_browser_entries[VFS_MAX_ENTRIES+1u];
static unsigned int g_browser_entry_count=0u;

#define FS_ENTRY_TYPE(i)   ((unsigned char)(g_browser_entries[i].info>>24))

typedef struct
{
    unsigned int index;               /* the entry; 0: slot unused */
    unsigned long size;
    unsigned long long object_id;     /* first cluster (exFAT: FS_EXFAT_Read_Entry); NTFS: record */
    char name[VFS_NAME_LEN];
} FS_ENTRY_INFO;

static FS_ENTRY_INFO g_fs_info[FS_INFO_SLOTS];
static unsigned int g_fs_info_next=0u;

typedef struct
{
    unsigned long lcn;        /* sectors are 32-bit throughout: 2 TB volumes */
    unsigned long len;
} FS_NTFS_RUN;

/* The $MFT's own runs, so records are found when it is fragmented. */
static FS_NTFS_RUN g_ntfs_mft_runs[NTFS_MAX_RUNS];
static unsigned int g_ntfs_mft_run_count=0u;

#define NTFS_MAX_EXTENTS  64u

/* Where a non-resident attribute's clusters are: its extents (first VCN,
   the record holding it; one when it has no attribute list) and the runs
   of the one in use (FS_NTFS_Map_Lcn). */
typedef struct
{
    FS_NTFS_RUN runs[NTFS_MAX_RUNS];
    unsigned int run_count;
    unsigned int ext_count;
    unsigned int ext_loaded;          /* the extent runs[] are from */
    unsigned long ext_vcn[NTFS_MAX_EXTENTS];
    unsigned long ext_frn[NTFS_MAX_EXTENTS];
    unsigned long type;
    unsigned char name_len;
} FS_NTFS_MAP;

typedef struct
{
    unsigned char valid;
    unsigned char fs_type;
    unsigned char flags;
    unsigned char resident;
    unsigned long size;
    unsigned long start_cluster;
    unsigned long cached_cluster;
    unsigned long cached_cluster_index;
    unsigned char cluster_cache_valid;

    /* Sparse map of the cluster chain.  Without it a backward jump has to
       walk from the first cluster every time, which on a 100 MB file is
       over a thousand FAT reads - seconds, on every backward seek.  Filled
       in as the chain is walked, so it costs nothing extra. */
    unsigned long chain_index[FS_CHAIN_SLOTS];
    unsigned long chain_stride;
    unsigned int  chain_count;
    unsigned long contig_upto;
    unsigned char contig_valid;
    unsigned long cached_sector_number;
    unsigned char sector_cache_valid;
    unsigned char sector_buffer[512];
    unsigned int resident_size;
    unsigned char resident_buffer[FILE_RESIDENT_MAX];
    FS_NTFS_MAP map;
} FS_FILE_HANDLE;

static FS_FILE_HANDLE g_current_file;

static unsigned long FS_ReadLE32(const unsigned char *p)
{
    unsigned long v;
    v=(unsigned long)p[0];
    v|=((unsigned long)p[1] << 8);
    v|=((unsigned long)p[2] << 16);
    v|=((unsigned long)p[3] << 24);
    return v;
}

static unsigned int FS_ReadLE16(const unsigned char *p)
{
    unsigned int v;
    v=(unsigned int)p[0];
    v|=((unsigned int)p[1] << 8);
    return v;
}

static unsigned long long FS_ReadLE64(const unsigned char *p)
{
    unsigned long long v;
    v=(unsigned long long)p[0];
    v|=((unsigned long long)p[1] << 8);
    v|=((unsigned long long)p[2] << 16);
    v|=((unsigned long long)p[3] << 24);
    v|=((unsigned long long)p[4] << 32);
    v|=((unsigned long long)p[5] << 40);
    v|=((unsigned long long)p[6] << 48);
    v|=((unsigned long long)p[7] << 56);
    return v;
}

static signed long long FS_ReadSignedLE(const unsigned char *p,unsigned int count)
{
    signed long long v;
    unsigned int i;
    unsigned long long u;

    u=0ull;
    for(i=0u;i<count;i++)
    {
        u|=((unsigned long long)p[i] << (i*8u));
    }

    if((count!=0u)&&((p[count-1u] & 0x80u)!=0u))
    {
        for(i=count;i<8u;i++)
        {
            u|=((unsigned long long)0xFFu << (i*8u));
        }
    }

    v=(signed long long)u;
    return v;
}

static unsigned long FS_Cap32(unsigned long long v)
{
    return (v>0xFFFFFFFFull) ? 0xFFFFFFFFul : (unsigned long)v;
}

static unsigned char FS_UpperAscii(unsigned char c)
{
    if((c>='a')&&(c<='z'))
    {
        c=(unsigned char)(c-0x20u);
    }
    return c;
}

/* A character code (Unicode) as the ASCII the screens show: Latin-1 and
   Latin Extended-A letters without their accents (a Hungarian name keeps its
   letters), anything else not ASCII as '_'. */
static const char g_fs_fold[193]=
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiii"
    "dnooooo_ouuuuytyAaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOo"
    "OoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static char FS_Fold(unsigned long ch)
{
    if((ch>=0x20ul)&&(ch<0x7Ful))
    {
        return (char)ch;
    }
    if((ch>=0xC0ul)&&(ch<0x180ul))
    {
        return g_fs_fold[ch-0xC0ul];
    }
    return '_';
}

static void FS_ClearString(char *s,unsigned int size)
{
    unsigned int i;
    for(i=0u;i<size;i++)
    {
        s[i]=0;
    }
}

static void FS_CopyName(char *dst,const char *src)
{
    unsigned int i;
    unsigned int len;
    unsigned int keep_head;
    unsigned int keep_tail;

    len=0u;
    while((len<259u)&&(src[len]!=0))
    {
        len++;
    }

    if(len<(VFS_NAME_LEN-1u))
    {
        for(i=0u;i<len;i++)
        {
            dst[i]=src[i];
        }
        dst[len]=0;
        return;
    }

    if((VFS_NAME_LEN-1u)<=12u)
    {
        for(i=0u;i<(VFS_NAME_LEN-1u);i++)
        {
            dst[i]=src[i];
        }
        dst[VFS_NAME_LEN-1u]=0;
        return;
    }

    keep_tail=8u;
    keep_head=(VFS_NAME_LEN-1u) - keep_tail - 1u;

    for(i=0u;i<keep_head;i++)
    {
        dst[i]=src[i];
    }

    dst[keep_head]='~';

    for(i=0u;i<keep_tail;i++)
    {
        dst[keep_head+1u+i]=src[len-keep_tail+i];
    }

    dst[VFS_NAME_LEN-1u]=0;
}

static void FS_TrimTrailingName(char *name)
{
    unsigned int len;

    len=0u;
    while((len<259u)&&(name[len]!=0))
    {
        len++;
    }

    while(len>0u)
    {
        if((name[len-1u]==' ')||(name[len-1u]=='.'))
        {
            name[len-1u]=0;
            len--;
        }
        else
        {
            break;
        }
    }
}

static unsigned char FS_NameIsAVI(const char *name)
{
    unsigned int len;
    unsigned int i;

    len=0u;
    for(i=0u;i<(VFS_NAME_LEN-1u);i++)
    {
        if(name[i]==0)
        {
            len=i;
            break;
        }
    }

    if(len<4u)
    {
        return 0u;
    }

    if((FS_UpperAscii((unsigned char)name[len-3u])=='A')&&
       (FS_UpperAscii((unsigned char)name[len-2u])=='V')&&
       (FS_UpperAscii((unsigned char)name[len-1u])=='I'))
    {
        if(name[len-4u]=='.')
        {
            return 1u;
        }
    }

    return 0u;
}

static unsigned char FS_NameIsMKV(const char *name)
{
    unsigned int len;
    unsigned int i;

    len=0u;
    for(i=0u;i<(VFS_NAME_LEN-1u);i++)
    {
        if(name[i]==0)
        {
            len=i;
            break;
        }
    }

    if(len<4u)
    {
        return 0u;
    }

    if((FS_UpperAscii((unsigned char)name[len-3u])=='M')&&
       (FS_UpperAscii((unsigned char)name[len-2u])=='K')&&
       (FS_UpperAscii((unsigned char)name[len-1u])=='V'))
    {
        if(name[len-4u]=='.')
        {
            return 1u;
        }
    }

    return 0u;
}

static unsigned char FS_NameEndsWithExt(const char *name,const char *ext)
{
    unsigned int name_len;
    unsigned int ext_len;
    unsigned int i;

    if((name==0)||(ext==0))
    {
        return 0u;
    }

    name_len=0u;
    while((name_len<(VFS_NAME_LEN-1u))&&(name[name_len]!=0))
    {
        name_len++;
    }

    ext_len=0u;
    while((ext_len<8u)&&(ext[ext_len]!=0))
    {
        ext_len++;
    }

    if((name_len<=ext_len)||(name[name_len-ext_len-1u]!='.'))
    {
        return 0u;
    }

    for(i=0u;i<ext_len;i++)
    {
        if(FS_UpperAscii((unsigned char)name[name_len-ext_len+i])!=FS_UpperAscii((unsigned char)ext[i]))
        {
            return 0u;
        }
    }

    return 1u;
}

static unsigned char FS_NameIsJPEG(const char *name)
{
    if(FS_NameEndsWithExt(name,"JPG")!=0u)
    {
        return 1u;
    }

    if(FS_NameEndsWithExt(name,"JPEG")!=0u)
    {
        return 1u;
    }

    return 0u;
}

static unsigned char FS_NameIsBMP(const char *name)
{
    if(FS_NameEndsWithExt(name,"BMP")!=0u)
    {
        return 1u;
    }

    return 0u;
}

static unsigned char FS_NameIsG3D(const char *name)
{
    if(FS_NameEndsWithExt(name,"G3D")!=0u)
    {
        return 1u;
    }

    return 0u;
}

/* Tracker modules by extension; Amiga collections also name MODs with a
   "MOD." prefix instead. */
static unsigned char FS_NameModuleType(const char *name)
{
    if(FS_NameEndsWithExt(name,"MOD")!=0u) return VFS_TYPE_MOD;
    if(FS_NameEndsWithExt(name,"S3M")!=0u) return VFS_TYPE_S3M;
    if(FS_NameEndsWithExt(name,"XM")!=0u)  return VFS_TYPE_XM;
    if(FS_NameEndsWithExt(name,"IT")!=0u)  return VFS_TYPE_IT;

    if((FS_UpperAscii((unsigned char)name[0])=='M')&&(FS_UpperAscii((unsigned char)name[1])=='O')&&
       (FS_UpperAscii((unsigned char)name[2])=='D')&&(name[3]=='.')&&(name[4]!=0))
    {
        return VFS_TYPE_MOD;
    }

    return 0u;
}

static unsigned char FS_DetectFileTypeByName(const char *name)
{
    unsigned char type;

    if(FS_NameIsAVI(name)!=0u)
    {
        return VFS_TYPE_AVI;
    }

    if(FS_NameIsMKV(name)!=0u)
    {
        return VFS_TYPE_MKV;
    }

    if(FS_NameIsJPEG(name)!=0u)
    {
        return VFS_TYPE_JPEG;
    }

    if(FS_NameIsBMP(name)!=0u)
    {
        return VFS_TYPE_BMP;
    }

    if(FS_NameIsG3D(name)!=0u)
    {
        return VFS_TYPE_G3D;
    }

    type=FS_NameModuleType(name);
    if(type!=0u)
    {
        return type;
    }

    if(FS_NameEndsWithExt(name,"MP3")!=0u)
    {
        return VFS_TYPE_MP3;
    }
    if((FS_NameEndsWithExt(name,"FLAC")!=0u)||(FS_NameEndsWithExt(name,"FLA")!=0u))
    {
        return VFS_TYPE_FLAC;
    }
    if((FS_NameEndsWithExt(name,"WAV")!=0u)||(FS_NameEndsWithExt(name,"WAVE")!=0u))
    {
        return VFS_TYPE_WAV;
    }

    return VFS_TYPE_FILE;
}

static void FS_Debug_Text(const char *txt,unsigned int y)
{
    text3((unsigned char*)txt,0u,y,1u,Black,White);
}

static void FS_Debug_UL(const char *txt,unsigned long value,unsigned int y)
{
    FS_Debug_Text(txt,y);
    tostring3(value,buff2);
    text3(buff2,180u,y,1u,Black,White);
}

static unsigned char FS_IsEOC_FAT32(unsigned long cluster)
{
    if(cluster>=0x0FFFFFF8ul)
    {
        return 1u;
    }
    return 0u;
}

static unsigned char FS_IsEOC_EXFAT(unsigned long cluster)
{
    if(cluster>=0xFFFFFFF8ul)
    {
        return 1u;
    }
    return 0u;
}

/* Spreads FS_CHAIN_SLOTS entries across the file, so at most one stride of
   FAT walking separates any position from a recorded point.  Slot 0 is the
   chain's head: a backward jump into the second stride starts from there.
   Call with start_cluster already set. */
static void FS_Set_Chain_Stride(unsigned long size)
{
    unsigned long clusters;

    g_current_file.chain_stride=1ul;
    g_current_file.chain_index[0]=g_current_file.start_cluster;
    g_current_file.chain_count=1u;

    if(g_fs_cluster_size_bytes==0ul) return;

    clusters=(size+g_fs_cluster_size_bytes-1ul)/g_fs_cluster_size_bytes;

    if(clusters>(unsigned long)FS_CHAIN_SLOTS)
    {
        g_current_file.chain_stride=
            (clusters+(unsigned long)FS_CHAIN_SLOTS-1ul)/(unsigned long)FS_CHAIN_SLOTS;
    }
}

static void FS_Reset_File_Handle(void)
{
    FS_Fat_Cache_Reset();
    g_current_file.valid=0u;
    g_current_file.fs_type=FS_TYPE_NONE;
    g_current_file.flags=0u;
    g_current_file.resident=0u;
    g_current_file.size=0ul;
    g_current_file.start_cluster=0ul;
    g_current_file.cached_cluster=0ul;
    g_current_file.cached_cluster_index=0ul;
    g_current_file.cluster_cache_valid=0u;
    g_current_file.chain_stride=0ul;
    g_current_file.chain_count=0u;
    g_current_file.contig_upto=0ul;
    g_current_file.contig_valid=0u;
    g_current_file.cached_sector_number=0ul;
    g_current_file.sector_cache_valid=0u;
    g_current_file.resident_size=0u;
    g_current_file.map.run_count=0u;
    g_current_file.map.ext_count=0u;
}

static void FS_Clear_Entries(void)
{
    unsigned int i;

    g_browser_entry_count=0u;
    g_browser_entries[0].where=0ul;
    g_browser_entries[0].info=0ul;
    for(i=0u;i<FS_INFO_SLOTS;i++)
    {
        g_fs_info[i].index=0u;
    }
    g_fs_info_next=0u;
}

/* Case-insensitive compare against a literal, for the hidden-name list. */
static unsigned char FS_NameEqualsCI(const char *name,const char *lit)
{
    unsigned int i;
    unsigned char a;
    unsigned char b;

    for(i=0u;i<(VFS_NAME_LEN-1u);i++)
    {
        a=(unsigned char)name[i];
        b=(unsigned char)lit[i];

        if((a>='a')&&(a<='z')) a=(unsigned char)(a-32u);
        if((b>='a')&&(b<='z')) b=(unsigned char)(b-32u);

        if(a!=b) return 0u;
        if(a==0u) return 1u;
    }

    return 0u;
}

/* Housekeeping folders the operating systems leave on removable media.
   Windows marks them hidden+system, which the listers below also catch,
   but the names are checked too in case the attributes were lost. */
static unsigned char FS_NameIsSystemJunk(const char *name)
{
    if(FS_NameEqualsCI(name,"System Volume Information")!=0u) return 1u;
    if(FS_NameEqualsCI(name,"RECYCLER")!=0u)                  return 1u;
    if(FS_NameEqualsCI(name,"LOST.DIR")!=0u)                  return 1u;
    if(FS_NameEqualsCI(name,"FOUND.000")!=0u)                 return 1u;

    return 0u;
}

static unsigned char FS_NameVisibleForBrowser(const char *name)
{
    unsigned int i;
    unsigned char c;

    if(name==0)
    {
        return 0u;
    }

    if((name[0]==0)||(name[0]=='$'))
    {
        return 0u;
    }

    /* Any leading dot: ".", "..", and the housekeeping directories macOS
       and Linux leave behind (.Trashes, .Spotlight-V100, ._name). */
    if(name[0]=='.')
    {
        return 0u;
    }

    if(FS_NameIsSystemJunk(name)!=0u)
    {
        return 0u;
    }

    for(i=0u;i<(VFS_NAME_LEN-1u);i++)
    {
        c=(unsigned char)name[i];
        if(c==0u)
        {
            break;
        }
        if(c<0x20u)
        {
            return 0u;
        }
    }

    return 1u;
}

/* where / rec: the entry's first directory record (FS_BROWSER_ENTRY). */
static unsigned char FS_Add_Entry(const char *name,unsigned char type,unsigned long where,unsigned long rec)
{
    if(FS_NameVisibleForBrowser(name)==0u)
    {
        return 0u;
    }

    if(g_browser_entry_count>=VFS_MAX_ENTRIES)
    {
        return 0u;
    }

    if(type!=VFS_TYPE_DIR)
    {
        type=FS_DetectFileTypeByName(name);
    }

    g_browser_entry_count++;
    g_browser_entries[g_browser_entry_count].where=where;
    g_browser_entries[g_browser_entry_count].info=(rec & 0x00FFFFFFul)|((unsigned long)type<<24);
    return 1u;
}

static unsigned char FS_ReadBootSector(unsigned long sector)
{
    Read_Buffer(sector,buffer);
    if((buffer[510]!=0x55u)||(buffer[511]!=0xAAu))
    {
        return 0u;
    }
    return 1u;
}

static unsigned char FS_DetectBootAtSector(unsigned long sector)
{
    if(FS_ReadBootSector(sector)==0u)
    {
        return 0u;
    }

    if((buffer[3]=='E')&&(buffer[4]=='X')&&(buffer[5]=='F')&&(buffer[6]=='A')&&(buffer[7]=='T'))
    {
        g_fs_part_start=sector;
        g_fs_type=FS_TYPE_EXFAT;
        return 1u;
    }

    if((buffer[82]=='F')&&(buffer[83]=='A')&&(buffer[84]=='T')&&(buffer[85]=='3')&&(buffer[86]=='2'))
    {
        g_fs_part_start=sector;
        g_fs_type=FS_TYPE_FAT32;
        return 1u;
    }

    if((buffer[3]=='N')&&(buffer[4]=='T')&&(buffer[5]=='F')&&(buffer[6]=='S'))
    {
        g_fs_part_start=sector;
        g_fs_type=FS_TYPE_NTFS;
        return 1u;
    }

    return 0u;
}

static unsigned char FS_Detect_FileSystem(void)
{
    unsigned int i;
    unsigned long lba;
    unsigned char type;

    g_fs_type=FS_TYPE_NONE;
    g_fs_part_start=0ul;

    if(FS_DetectBootAtSector(0ul)!=0u)
    {
        return 1u;
    }

    if(FS_ReadBootSector(0ul)==0u)
    {
        return 0u;
    }

    for(i=0u;i<4u;i++)
    {
        type=buffer[0x1BEu + (i*16u) + 4u];
        lba=FS_ReadLE32(&buffer[0x1BEu + (i*16u) + 8u]);
        if((type!=0u)&&(lba!=0ul))
        {
            if(FS_DetectBootAtSector(lba)!=0u)
            {
                return 1u;
            }
        }
    }

    if(FS_DetectBootAtSector(8192ul)!=0u)
    {
        return 1u;
    }

    return 0u;
}

static unsigned long FS_FAT32_ClusterToSector(unsigned long cluster)
{
    return (g_fat32_data_start + ((cluster-2ul) * g_fs_sectors_per_cluster));
}

static unsigned long FS_FAT32_NextCluster(unsigned long cluster)
{
    unsigned long fat_offset;
    unsigned long fat_sector;
    unsigned int off;
    unsigned long next;

    fat_offset=cluster * 4ul;
    fat_sector=g_fat32_fat_start + (fat_offset / 512ul);
    off=(unsigned int)(fat_offset & 511ul);
    next=FS_ReadLE32(&FS_Fat_Sector(fat_sector)[off]) & 0x0FFFFFFFul;
    return next;
}

/* An 8.3 name; byte 12's flags ask for a lowercase name (0x08) or
   extension (0x10), as Windows and Linux store such names. */
static char FS_Short83Char(unsigned char c,unsigned char lower)
{
    if((lower!=0u)&&(c>='A')&&(c<='Z'))
    {
        c=(unsigned char)(c+0x20u);
    }
    return (char)c;
}

static void FS_FormatShort83(const unsigned char *entry,char *name)
{
    unsigned int i;
    unsigned int out;
    out=0u;
    FS_ClearString(name,VFS_NAME_LEN);
    for(i=0u;i<8u;i++)
    {
        if(entry[i]==0x20u)
        {
            break;
        }
        if(out<(VFS_NAME_LEN-1u))
        {
            name[out]=FS_Short83Char(entry[i],(unsigned char)(entry[12] & 0x08u));
            out++;
        }
    }

    if((entry[8]!=0x20u)||(entry[9]!=0x20u)||(entry[10]!=0x20u))
    {
        if(out<(VFS_NAME_LEN-1u))
        {
            name[out]='.';
            out++;
        }
        for(i=8u;i<11u;i++)
        {
            if(entry[i]==0x20u)
            {
                break;
            }
            if(out<(VFS_NAME_LEN-1u))
            {
                name[out]=FS_Short83Char(entry[i],(unsigned char)(entry[12] & 0x10u));
                out++;
            }
        }
    }
    name[out]=0;
}

static void FS_LFN_Append13(const unsigned char *entry,char *lfn)
{
    unsigned char ord;
    unsigned int base;
    unsigned int char_index;
    unsigned int pos;
    static const unsigned char char_offsets[13]={1u,3u,5u,7u,9u,14u,16u,18u,20u,22u,24u,28u,30u};
    unsigned int ch;

    ord=(unsigned char)(entry[0] & 0x1Fu);
    if(ord==0u)
    {
        return;
    }
    if((entry[0] & 0x40u)!=0u)
    {
        FS_ClearString(lfn,260u);
    }

    base=(unsigned int)(ord-1u) * 13u;

    for(char_index=0u;char_index<13u;char_index++)
    {
        pos=char_offsets[char_index];
        ch=FS_ReadLE16(&entry[pos]);
        if(ch==0x0000u)
        {
            if((base+char_index)<259u)
            {
                lfn[base+char_index]=0;
            }
            return;
        }
        if(ch==0xFFFFu)
        {
            return;
        }
        if((base+char_index)<259u)
        {
            lfn[base+char_index]=(ch<0x80u) ? (char)ch : FS_Fold(ch);
        }
    }
}

static unsigned char FS_FAT32_Init(void)
{
    g_fs_bytes_per_sector=FS_ReadLE16(&buffer[0x0Bu]);
    g_fs_sectors_per_cluster=(unsigned long)buffer[0x0Du];
    g_fat32_reserved_sectors=(unsigned long)FS_ReadLE16(&buffer[0x0Eu]);
    g_fat32_num_fats=buffer[0x10u];
    g_fat32_sectors_per_fat=FS_ReadLE32(&buffer[0x24u]);
    g_fat32_root_cluster=FS_ReadLE32(&buffer[0x2Cu]);
    g_fat32_fat_start=g_fs_part_start + g_fat32_reserved_sectors;
    g_fat32_data_start=g_fs_part_start + g_fat32_reserved_sectors + ((unsigned long)g_fat32_num_fats * g_fat32_sectors_per_fat);
    g_fs_cluster_size_bytes=(unsigned long)g_fs_bytes_per_sector * g_fs_sectors_per_cluster;

    FS_Debug_Text("FAT32 Found",60u);
    FS_Debug_UL("Part start",g_fs_part_start,80u);
    FS_Debug_UL("Bytes/Sector",g_fs_bytes_per_sector,100u);
    FS_Debug_UL("Sec/Cluster",g_fs_sectors_per_cluster,120u);
    FS_Debug_UL("Root Cluster",g_fat32_root_cluster,140u);
    FS_Debug_UL("Data Start",g_fat32_data_start,160u);
    return 1u;
}

/* ------------------------ directory records -------------------------
   FAT32 and exFAT directories are read through a cursor: a cluster and a
   32-byte record's index in it.  Up to 8 sectors of the cluster are held
   (g_fs_blk, which NTFS uses for its index blocks), so a listing reads 4 KB
   at a time. */
typedef struct
{
    unsigned long cluster;    /* 0: past the directory's end */
    unsigned long rec;
} FS_DIR_CUR;

static unsigned char g_fs_blk[NTFS_MAX_RECORD_SIZE];
static unsigned long g_fs_blk_lba=0ul;    /* first sector held */
static unsigned long g_fs_blk_n=0ul;      /* sectors held; 0: none */
static unsigned long g_fs_dir_end=0ul;    /* exFAT contiguous directory: the cluster
                                             past its end; 0: the FAT chains it */

static unsigned long FS_EXFAT_ClusterToSector(unsigned long cluster);
static unsigned long FS_EXFAT_NextCluster(unsigned long cluster);

/* The record at the cursor; 0 if its cluster is not one. */
static const unsigned char *FS_Dir_Record(const FS_DIR_CUR *c)
{
    unsigned long lba;
    unsigned long sec;
    unsigned long n;

    if(c->cluster<2ul)
    {
        return 0;
    }

    if(g_fs_type==FS_TYPE_EXFAT)
    {
        if(c->cluster>(g_exfat_cluster_count+1ul))
        {
            return 0;
        }
        lba=FS_EXFAT_ClusterToSector(c->cluster);
    }
    else
    {
        lba=FS_FAT32_ClusterToSector(c->cluster);
    }

    sec=(c->rec*32ul)/512ul;
    if(((lba+sec)<g_fs_blk_lba)||((lba+sec)>=(g_fs_blk_lba+g_fs_blk_n)))
    {
        n=g_fs_sectors_per_cluster-(sec & ~7ul);
        if(n>8ul)
        {
            n=8ul;
        }
        g_fs_blk_lba=lba+(sec & ~7ul);
        g_fs_blk_n=n;
        Read_Buffers(g_fs_blk_lba,(unsigned char)n,g_fs_blk);
    }

    return &g_fs_blk[((lba+sec-g_fs_blk_lba)*512ul)+((c->rec*32ul)&511ul)];
}

/* On to the next record: 0 past the directory's end. */
static unsigned char FS_Dir_Advance(FS_DIR_CUR *c)
{
    unsigned long next;

    c->rec++;
    if((c->rec*32ul)<g_fs_cluster_size_bytes)
    {
        return 1u;
    }

    c->rec=0ul;
    if(g_fs_type==FS_TYPE_EXFAT)
    {
        if(g_fs_dir_end!=0ul)
        {
            c->cluster++;
            if(c->cluster>=g_fs_dir_end)
            {
                c->cluster=0ul;
                return 0u;
            }
            return 1u;
        }
        next=FS_EXFAT_NextCluster(c->cluster);
        if((next<2ul)||(FS_IsEOC_EXFAT(next)!=0u))
        {
            c->cluster=0ul;
            return 0u;
        }
    }
    else
    {
        next=FS_FAT32_NextCluster(c->cluster);
        if((next<2ul)||(FS_IsEOC_FAT32(next)!=0u))
        {
            c->cluster=0ul;
            return 0u;
        }
    }

    c->cluster=next;
    return 1u;
}

/* The 8.3 name's checksum, which its long-name records carry. */
static unsigned char FS_SFN_Sum(const unsigned char *e)
{
    unsigned char sum;
    unsigned int i;

    sum=0u;
    for(i=0u;i<11u;i++)
    {
        sum=(unsigned char)(((sum & 1u) << 7) + (sum >> 1) + e[i]);
    }
    return sum;
}

static unsigned int FS_FAT32_ListDirectory(unsigned long dir_cluster)
{
    FS_DIR_CUR c;
    const unsigned char *e;
    unsigned long set_cluster;     /* the entry's first record: its long name's */
    unsigned long set_rec;         /* first, or the 8.3 one                     */
    unsigned char have_set;
    unsigned char sum;             /* the long name's checksum                  */
    unsigned char first;
    unsigned char attr;
    char lfn[260];
    char name[VFS_NAME_LEN];

    FS_Clear_Entries();
    FS_ClearString(lfn,260u);

    c.cluster=(dir_cluster<2ul) ? g_fat32_root_cluster : dir_cluster;
    c.rec=0ul;
    set_cluster=0ul;
    set_rec=0ul;
    have_set=0u;
    sum=0u;

    while((g_browser_entry_count<VFS_MAX_ENTRIES)&&((e=FS_Dir_Record(&c))!=0))
    {
        first=e[0];
        attr=e[11];

        if(first==0x00u)
        {
            break;
        }

        /* deleted, volume label, hidden+system (an OS folder such as System
           Volume Information; media files never carry both), dot entries */
        if((first==0xE5u)||
           ((attr!=0x0Fu)&&(((attr & 0x08u)!=0u)||((attr & 0x06u)==0x06u)||(first=='.'))))
        {
            FS_ClearString(lfn,260u);
            have_set=0u;
        }
        else if(attr==0x0Fu)
        {
            if(((first & 0x40u)!=0u)||(have_set==0u))
            {
                set_cluster=c.cluster;
                set_rec=c.rec;
                have_set=1u;
            }
            sum=e[13];
            FS_LFN_Append13(e,lfn);
        }
        else
        {
            /* a long name left by a deleted file does not match the 8.3 one */
            if((have_set==0u)||(lfn[0]==0)||(sum!=FS_SFN_Sum(e)))
            {
                set_cluster=c.cluster;
                set_rec=c.rec;
                FS_FormatShort83(e,name);
            }
            else
            {
                FS_CopyName(name,lfn);
            }

            (void)FS_Add_Entry(name,((attr & 0x10u)!=0u) ? VFS_TYPE_DIR : VFS_TYPE_FILE,
                               set_cluster,set_rec);
            FS_ClearString(lfn,260u);
            have_set=0u;
        }

        if(FS_Dir_Advance(&c)==0u)
        {
            break;
        }
    }

    return g_browser_entry_count;
}

/* An entry's name, size and first cluster, from its first record on. */
static unsigned char FS_FAT32_Read_Entry(unsigned long cluster,unsigned long rec,FS_ENTRY_INFO *out)
{
    FS_DIR_CUR c;
    const unsigned char *e;
    unsigned int n;
    unsigned char sum;
    char lfn[260];

    c.cluster=cluster;
    c.rec=rec;
    sum=0u;
    FS_ClearString(lfn,260u);

    for(n=0u;n<24u;n++)                 /* 20 long-name records at most */
    {
        e=FS_Dir_Record(&c);
        if(e==0)
        {
            return 0u;
        }

        if(e[11]!=0x0Fu)
        {
            if((lfn[0]!=0)&&(sum==FS_SFN_Sum(e)))
            {
                FS_CopyName(out->name,lfn);
            }
            else
            {
                FS_FormatShort83(e,out->name);
            }
            out->size=FS_ReadLE32(&e[28]);
            out->object_id=(unsigned long long)(((unsigned long)FS_ReadLE16(&e[20]) << 16) |
                                                (unsigned long)FS_ReadLE16(&e[26]));
            return 1u;
        }

        sum=e[13];
        FS_LFN_Append13(e,lfn);
        if(FS_Dir_Advance(&c)==0u)
        {
            return 0u;
        }
    }

    return 0u;
}

static unsigned char FS_FAT32_OpenFile(unsigned long start_cluster,unsigned long size)
{
    FS_Reset_File_Handle();
    if(start_cluster<2ul)
    {
        return 0u;
    }
    g_current_file.valid=1u;
    g_current_file.fs_type=FS_TYPE_FAT32;
    g_current_file.size=size;
    g_current_file.start_cluster=start_cluster;
    FS_Set_Chain_Stride(size);
    return 1u;
}

static unsigned char FS_FAT32_MapPos(unsigned long pos,unsigned long *sector,unsigned int *offset)
{
    unsigned long cluster_index;
    unsigned long cluster;
    unsigned long i;
    unsigned long byte_in_cluster;
    unsigned long sector_in_cluster;
    unsigned long next;
    unsigned long slot;

    cluster_index=pos / g_fs_cluster_size_bytes;
    byte_in_cluster=pos % g_fs_cluster_size_bytes;
    sector_in_cluster=byte_in_cluster / g_fs_bytes_per_sector;
    *offset=(unsigned int)(byte_in_cluster % g_fs_bytes_per_sector);

    /* A file laid down in one piece needs no walk at all. */
    if((g_current_file.contig_valid!=0u)&&(cluster_index<=g_current_file.contig_upto))
    {
        cluster=g_current_file.start_cluster+cluster_index;
        g_current_file.cached_cluster=cluster;
        g_current_file.cached_cluster_index=cluster_index;
        g_current_file.cluster_cache_valid=1u;
        *sector=FS_FAT32_ClusterToSector(cluster) + sector_in_cluster;
        return 1u;
    }

    cluster=g_current_file.start_cluster;
    i=0ul;

    /* Start from the sparse map where it reaches furthest without passing
       the target, then from the forward cache if that is nearer still. */
    if(g_current_file.chain_stride>0ul)
    {
        slot=cluster_index/g_current_file.chain_stride;
        if(slot>=(unsigned long)g_current_file.chain_count)
        {
            slot=(unsigned long)g_current_file.chain_count;
        }
        if(slot>0ul)
        {
            slot--;
            cluster=g_current_file.chain_index[slot];
            i=slot*g_current_file.chain_stride;
        }
    }

    if((g_current_file.cluster_cache_valid!=0u)&&
       (cluster_index>=g_current_file.cached_cluster_index)&&
       (g_current_file.cached_cluster_index>=i))
    {
        cluster=g_current_file.cached_cluster;
        i=g_current_file.cached_cluster_index;
    }

    while(i<cluster_index)
    {
        next=FS_FAT32_NextCluster(cluster);
        if((next<2ul)||(FS_IsEOC_FAT32(next)!=0u))
        {
            return 0u;
        }

        if(next!=(cluster+1ul))
        {
            g_current_file.contig_valid=1u;      /* contiguous up to here */
            if(i<g_current_file.contig_upto) g_current_file.contig_upto=i;
        }
        else if((g_current_file.contig_valid==0u)&&((i+1ul)>g_current_file.contig_upto))
        {
            g_current_file.contig_upto=i+1ul;
        }

        cluster=next;
        i++;

        /* Record the chain as it is walked, so this is the only walk. */
        if(g_current_file.chain_stride>0ul)
        {
            if((i%g_current_file.chain_stride)==0ul)
            {
                slot=i/g_current_file.chain_stride;
                if(slot<(unsigned long)FS_CHAIN_SLOTS)
                {
                    g_current_file.chain_index[slot]=cluster;
                    if((slot+1ul)>(unsigned long)g_current_file.chain_count)
                    {
                        g_current_file.chain_count=(unsigned int)(slot+1ul);
                    }
                }
            }
        }
    }

    g_current_file.cached_cluster=cluster;
    g_current_file.cached_cluster_index=cluster_index;
    g_current_file.cluster_cache_valid=1u;
    *sector=FS_FAT32_ClusterToSector(cluster) + sector_in_cluster;
    return 1u;
}

static unsigned long FS_EXFAT_ClusterToSector(unsigned long cluster)
{
    return (g_exfat_cluster_heap_start + ((cluster-2ul) * g_fs_sectors_per_cluster));
}

static unsigned long FS_EXFAT_NextCluster(unsigned long cluster)
{
    unsigned long fat_sector;
    unsigned int fat_offset;
    unsigned long next;
    unsigned long fat_entry_offset;

    fat_entry_offset=cluster * 4ul;
    fat_sector=g_exfat_fat_start + (fat_entry_offset / 512ul);
    fat_offset=(unsigned int)(fat_entry_offset & 511ul);
    next=FS_ReadLE32(&FS_Fat_Sector(fat_sector)[fat_offset]);
    return next;
}

static unsigned char FS_EXFAT_Init(void)
{
    g_fs_bytes_per_sector=(unsigned int)(1u << buffer[0x6Cu]);
    g_fs_sectors_per_cluster=(unsigned long)(1ul << buffer[0x6Du]);
    g_exfat_num_fats=buffer[0x6Eu];
    g_exfat_fat_start=g_fs_part_start + FS_ReadLE32(&buffer[0x50u]);
    g_exfat_fat_length=FS_ReadLE32(&buffer[0x54u]);
    g_exfat_cluster_heap_start=g_fs_part_start + FS_ReadLE32(&buffer[0x58u]);
    g_exfat_cluster_count=FS_ReadLE32(&buffer[0x5Cu]);
    g_exfat_root_cluster=FS_ReadLE32(&buffer[0x60u]);
    g_fs_cluster_size_bytes=(unsigned long)g_fs_bytes_per_sector * g_fs_sectors_per_cluster;

    FS_Debug_Text("exFAT Found",60u);
    FS_Debug_UL("Part start",g_fs_part_start,80u);
    FS_Debug_UL("Bytes/Sector",g_fs_bytes_per_sector,100u);
    FS_Debug_UL("Sec/Cluster",g_fs_sectors_per_cluster,120u);
    FS_Debug_UL("Root Cluster",g_exfat_root_cluster,140u);
    FS_Debug_UL("Heap Start",g_exfat_cluster_heap_start,160u);
    return 1u;
}

static void FS_EXFAT_AppendName15(const unsigned char *entry,char *name)
{
    unsigned int i;
    unsigned int len;
    unsigned int ch;

    len=0u;
    while((len<259u)&&(name[len]!=0))
    {
        len++;
    }

    for(i=0u;i<15u;i++)
    {
        ch=FS_ReadLE16(&entry[2u + (i*2u)]);
        if(ch==0x0000u)
        {
            break;
        }
        if(ch==0xFFFFu)
        {
            break;
        }
        if(len<259u)
        {
            name[len]=(ch<0x80u) ? (char)ch : FS_Fold(ch);
            len++;
            name[len]=0;
        }
    }
}

/* dir_id: the first cluster, bit 32 set for a contiguous directory and its
   length in clusters from bit 33 (FS_EXFAT_Read_Entry); 0 the root. */
static unsigned int FS_EXFAT_ListDirectory(unsigned long long dir_id)
{
    FS_DIR_CUR c;
    const unsigned char *e;
    unsigned long set_cluster;    /* the entry's file record */
    unsigned long set_rec;
    unsigned int pending;         /* its secondary records still to come */
    unsigned int attr;
    unsigned char have_stream;
    char lfn[260];
    char name[VFS_NAME_LEN];

    FS_Clear_Entries();
    c.cluster=(unsigned long)(dir_id & 0xFFFFFFFFull);
    c.rec=0ul;
    g_fs_dir_end=0ul;
    if(c.cluster<2ul)
    {
        c.cluster=g_exfat_root_cluster;
    }
    else if(((dir_id>>32) & 1ull)!=0ull)
    {
        g_fs_dir_end=c.cluster+(unsigned long)(dir_id>>33);
    }

    set_cluster=0ul;
    set_rec=0ul;
    pending=0u;
    attr=0u;
    have_stream=0u;
    FS_ClearString(lfn,260u);

    while((g_browser_entry_count<VFS_MAX_ENTRIES)&&((e=FS_Dir_Record(&c))!=0))
    {
        if(e[0]==0x00u)
        {
            break;
        }

        if(pending!=0u)
        {
            if(e[0]==0xC0u)
            {
                have_stream=1u;
            }
            else if(e[0]==0xC1u)
            {
                FS_EXFAT_AppendName15(e,lfn);
            }

            pending--;
            if((pending==0u)&&(have_stream!=0u)&&((attr & 0x06u)!=0x06u))
            {
                FS_TrimTrailingName(lfn);
                FS_CopyName(name,lfn);
                (void)FS_Add_Entry(name,((attr & 0x10u)!=0u) ? VFS_TYPE_DIR : VFS_TYPE_FILE,
                                   set_cluster,set_rec);
            }
        }
        else if(e[0]==0x85u)
        {
            pending=e[1];
            attr=FS_ReadLE16(&e[4]);
            set_cluster=c.cluster;
            set_rec=c.rec;
            have_stream=0u;
            FS_ClearString(lfn,260u);
        }

        if(FS_Dir_Advance(&c)==0u)
        {
            break;
        }
    }

    return g_browser_entry_count;
}

/* An entry's name, size and first cluster from its file record on.  The
   object id carries the contiguous flag in bit 32 and, from bit 33, the
   length in clusters (a contiguous directory's end). */
static unsigned char FS_EXFAT_Read_Entry(unsigned long cluster,unsigned long rec,FS_ENTRY_INFO *out)
{
    FS_DIR_CUR c;
    const unsigned char *e;
    unsigned int n;
    unsigned int count;
    unsigned long long len;
    unsigned long long clusters;
    char lfn[260];

    c.cluster=cluster;
    c.rec=rec;
    e=FS_Dir_Record(&c);
    if((e==0)||(e[0]!=0x85u))
    {
        return 0u;
    }

    count=e[1];
    FS_ClearString(lfn,260u);
    out->size=0ul;
    out->object_id=0ull;

    for(n=0u;n<count;n++)
    {
        if(FS_Dir_Advance(&c)==0u)
        {
            return 0u;
        }
        e=FS_Dir_Record(&c);
        if(e==0)
        {
            return 0u;
        }

        if(e[0]==0xC0u)
        {
            len=FS_ReadLE64(&e[24]);
            clusters=(len+g_fs_cluster_size_bytes-1ull)/g_fs_cluster_size_bytes;
            out->size=(len>0xFFFFFFFFull) ? 0xFFFFFFFFul : (unsigned long)len;
            out->object_id=(unsigned long long)FS_ReadLE32(&e[20]);
            if((e[1] & 0x02u)!=0u)
            {
                out->object_id|=(1ull<<32)|(clusters<<33);
            }
        }
        else if(e[0]==0xC1u)
        {
            FS_EXFAT_AppendName15(e,lfn);
        }
    }

    FS_TrimTrailingName(lfn);
    FS_CopyName(out->name,lfn);
    return 1u;
}

static unsigned char FS_EXFAT_OpenFile(unsigned long long object_id,unsigned long size)
{
    FS_Reset_File_Handle();
    g_current_file.valid=1u;
    g_current_file.fs_type=FS_TYPE_EXFAT;
    g_current_file.start_cluster=(unsigned long)(object_id & 0xFFFFFFFFull);
    g_current_file.flags=(unsigned char)((object_id >> 32) & 0x01ull);
    g_current_file.size=size;
    FS_Set_Chain_Stride(size);
    return 1u;
}

static unsigned char FS_EXFAT_MapPos(unsigned long pos,unsigned long *sector,unsigned int *offset)
{
    unsigned long cluster_index;
    unsigned long cluster;
    unsigned long i;
    unsigned long byte_in_cluster;
    unsigned long sector_in_cluster;
    unsigned long slot;
    unsigned char nofat;

    cluster_index=pos / g_fs_cluster_size_bytes;
    byte_in_cluster=pos % g_fs_cluster_size_bytes;
    sector_in_cluster=byte_in_cluster / g_fs_bytes_per_sector;
    *offset=(unsigned int)(byte_in_cluster % g_fs_bytes_per_sector);
    nofat=g_current_file.flags & 0x01u;

    if(nofat!=0u)
    {
        cluster=g_current_file.start_cluster + cluster_index;
    }
    else
    {
        cluster=g_current_file.start_cluster;
        i=0ul;

        /* Same sparse map as FAT32: without it every backward jump walks
           the chain from the beginning. */
        if(g_current_file.chain_stride>0ul)
        {
            slot=cluster_index/g_current_file.chain_stride;
            if(slot>=(unsigned long)g_current_file.chain_count)
            {
                slot=(unsigned long)g_current_file.chain_count;
            }
            if(slot>0ul)
            {
                slot--;
                cluster=g_current_file.chain_index[slot];
                i=slot*g_current_file.chain_stride;
            }
        }

        if((g_current_file.cluster_cache_valid!=0u)&&
           (cluster_index>=g_current_file.cached_cluster_index)&&
           (g_current_file.cached_cluster_index>=i))
        {
            cluster=g_current_file.cached_cluster;
            i=g_current_file.cached_cluster_index;
        }

        while(i<cluster_index)
        {
            cluster=FS_EXFAT_NextCluster(cluster);
            if((cluster<2ul)||(FS_IsEOC_EXFAT(cluster)!=0u))
            {
                return 0u;
            }
            i++;

            if((g_current_file.chain_stride>0ul)&&
               ((i%g_current_file.chain_stride)==0ul))
            {
                slot=i/g_current_file.chain_stride;
                if(slot<(unsigned long)FS_CHAIN_SLOTS)
                {
                    g_current_file.chain_index[slot]=cluster;
                    if((slot+1ul)>(unsigned long)g_current_file.chain_count)
                    {
                        g_current_file.chain_count=(unsigned int)(slot+1ul);
                    }
                }
            }
        }

        g_current_file.cached_cluster=cluster;
        g_current_file.cached_cluster_index=cluster_index;
        g_current_file.cluster_cache_valid=1u;
    }

    *sector=FS_EXFAT_ClusterToSector(cluster) + sector_in_cluster;
    return 1u;
}

static unsigned char FS_NTFS_ApplyFixup(unsigned char *rec,unsigned int size)
{
    unsigned int usa_ofs;
    unsigned int usa_count;
    unsigned int i;
    unsigned int sector_end;

    usa_ofs=FS_ReadLE16(&rec[4]);
    usa_count=FS_ReadLE16(&rec[6]);
    if((usa_ofs+((unsigned int)usa_count*2u))>size)
    {
        return 0u;
    }

    for(i=1u;i<usa_count;i++)
    {
        sector_end=(unsigned int)(i * g_fs_bytes_per_sector);
        if(sector_end<2u)
        {
            return 0u;
        }
        rec[sector_end-2u]=rec[usa_ofs + (i*2u)];
        rec[sector_end-1u]=rec[usa_ofs + (i*2u) + 1u];
    }
    return 1u;
}

/* A runlist (mapping pairs) appended to runs[*run_count..max-1]. */
static unsigned char FS_NTFS_ParseRuns(const unsigned char *runlist,FS_NTFS_RUN *runs,unsigned int *run_count,
                                       unsigned int max)
{
    unsigned char head;
    unsigned char len_size;
    unsigned char off_size;
    unsigned long long length;
    signed long long delta;
    signed long long current_lcn;
    unsigned int count;

    current_lcn=0ll;
    count=*run_count;
    while(1)
    {
        head=*runlist;
        runlist++;
        if(head==0u)
        {
            break;
        }

        len_size=(unsigned char)(head & 0x0Fu);
        off_size=(unsigned char)((head >> 4) & 0x0Fu);
        if((len_size==0u)||(count>=max))
        {
            return 0u;
        }

        length=(unsigned long long)FS_ReadSignedLE(runlist,len_size);
        runlist+=len_size;
        if(off_size!=0u)
        {
            delta=FS_ReadSignedLE(runlist,off_size);
        }
        else
        {
            delta=0ll;
        }
        runlist+=off_size;

        current_lcn+=delta;
        runs[count].lcn=(unsigned long)current_lcn;
        runs[count].len=(unsigned long)length;
        count++;
    }

    *run_count=count;
    return 1u;
}

/* The first attribute of type from start, resident (0) or not (1). */
static unsigned int FS_NTFS_FindAttr(const unsigned char *rec,unsigned long type,unsigned int start,unsigned char need_nonresident)
{
    unsigned int pos;
    unsigned long attr_type;
    unsigned int attr_len;

    pos=start;
    while((pos+16u)<g_ntfs_record_size)
    {
        attr_type=FS_ReadLE32(&rec[pos]);
        if(attr_type==0xFFFFFFFFul)
        {
            break;
        }
        attr_len=FS_ReadLE32(&rec[pos+4u]);
        if(attr_len<16u)
        {
            break;
        }
        if(attr_type==type)
        {
            if(rec[pos+8u]==need_nonresident)
            {
                return pos;
            }
        }
        pos=(unsigned int)(pos + attr_len);
    }
    return 0u;
}

/* In a record, the non-resident extent of type (a name of name_len
   characters: $I30 4, the unnamed $DATA 0) that starts at vcn. */
static unsigned int FS_NTFS_FindExtent(const unsigned char *rec,unsigned long type,unsigned char name_len,
                                       unsigned long long vcn)
{
    unsigned int pos;
    unsigned int len;

    for(pos=FS_ReadLE16(&rec[20]);(pos+64u)<=g_ntfs_record_size;pos+=len)
    {
        if(FS_ReadLE32(&rec[pos])==0xFFFFFFFFul)
        {
            break;
        }
        len=FS_ReadLE32(&rec[pos+4u]);
        if(len<16u)
        {
            break;
        }
        if((FS_ReadLE32(&rec[pos])==type)&&(rec[pos+8u]==1u)&&(rec[pos+9u]==name_len)&&
           (FS_ReadLE64(&rec[pos+16u])==vcn))
        {
            return pos;
        }
    }
    return 0u;
}

/* len bytes at byte pos of an attribute whose runs (from VCN 0) are given
   into dst, whole sectors, a run's part at a time: the MFT, attribute lists. */
static unsigned char FS_NTFS_ReadRuns(const FS_NTFS_RUN *runs,unsigned int run_count,
                                      unsigned long long pos,unsigned int len,unsigned char *dst)
{
    unsigned long long cl;
    unsigned long long base;
    unsigned int off;
    unsigned int in_cl;
    unsigned int take;
    unsigned int r;

    for(off=0u;off<len;off+=take)
    {
        cl=(pos+off)/g_fs_cluster_size_bytes;
        in_cl=(unsigned int)((pos+off)%g_fs_cluster_size_bytes);

        base=0ull;
        for(r=0u;r<run_count;r++)
        {
            if(cl<(base+runs[r].len))
            {
                break;
            }
            base+=runs[r].len;
        }
        if(r>=run_count)
        {
            return 0u;
        }

        take=(unsigned int)(g_fs_cluster_size_bytes-in_cl);
        if(take>(len-off))
        {
            take=len-off;
        }
        Read_Buffers(g_fs_part_start+(unsigned long)(((unsigned long long)runs[r].lcn+(cl-base))*g_fs_sectors_per_cluster)+(in_cl>>9),
                     (unsigned char)(take>>9),&dst[off]);
    }

    return 1u;
}

static unsigned char FS_NTFS_ReadRecord(unsigned long long frn,unsigned char *out)
{
    if(FS_NTFS_ReadRuns(g_ntfs_mft_runs,g_ntfs_mft_run_count,frn*(unsigned long long)g_ntfs_record_size,
                        g_ntfs_record_size,out)==0u)
    {
        return 0u;
    }

    if((out[0]!='F')||(out[1]!='I')||(out[2]!='L')||(out[3]!='E'))
    {
        return 0u;
    }

    return FS_NTFS_ApplyFixup(out,g_ntfs_record_size);
}

static unsigned char FS_NTFS_Init(void)
{
    unsigned int run_count;
    unsigned int attr;
    signed char rec_code;
    signed char idx_code;

    g_fs_bytes_per_sector=FS_ReadLE16(&buffer[0x0Bu]);
    g_fs_sectors_per_cluster=(unsigned long)buffer[0x0Du];
    g_fs_cluster_size_bytes=(unsigned long)g_fs_bytes_per_sector * g_fs_sectors_per_cluster;
    g_ntfs_total_sectors=FS_ReadLE64(&buffer[0x28u]);
    g_ntfs_mft_lcn=FS_ReadLE64(&buffer[0x30u]);

    rec_code=(signed char)buffer[0x40u];
    if(rec_code>0)
    {
        g_ntfs_record_size=(unsigned int)((unsigned int)rec_code * g_fs_cluster_size_bytes);
    }
    else
    {
        g_ntfs_record_size=(unsigned int)(1u << (unsigned int)(-rec_code));
    }

    idx_code=(signed char)buffer[0x44u];
    if(idx_code>0)
    {
        g_ntfs_index_block_size=(unsigned int)((unsigned int)idx_code * g_fs_cluster_size_bytes);
    }
    else
    {
        g_ntfs_index_block_size=(unsigned int)(1u << (unsigned int)(-idx_code));
    }

    if(g_ntfs_record_size>NTFS_MAX_RECORD_SIZE)
    {
        return 0u;
    }
    if(g_ntfs_index_block_size>NTFS_MAX_RECORD_SIZE)
    {
        g_ntfs_index_block_size=NTFS_MAX_RECORD_SIZE;
    }

    /* Record 0 is the $MFT itself, at the start of its first run: its $DATA
       runs locate the rest (else the MFT is taken as contiguous). */
    g_ntfs_mft_runs[0].lcn=(unsigned long)g_ntfs_mft_lcn;
    g_ntfs_mft_runs[0].len=0xFFFFFFFFul;
    g_ntfs_mft_run_count=1u;
    if(FS_NTFS_ReadRecord(0ull,g_ntfs_record_buf)!=0u)
    {
        attr=FS_NTFS_FindExtent(g_ntfs_record_buf,0x80ul,0u,0ull);
        run_count=0u;
        if((attr!=0u)&&
           (FS_NTFS_ParseRuns(&g_ntfs_record_buf[attr+FS_ReadLE16(&g_ntfs_record_buf[attr+32u])],
                              g_ntfs_mft_runs,&run_count,NTFS_MAX_RUNS)!=0u)&&(run_count!=0u))
        {
            g_ntfs_mft_run_count=run_count;
        }
        else
        {
            g_ntfs_mft_runs[0].lcn=(unsigned long)g_ntfs_mft_lcn;
            g_ntfs_mft_runs[0].len=0xFFFFFFFFul;
        }
    }

    FS_Debug_Text("NTFS Found",60u);
    FS_Debug_UL("Part start",g_fs_part_start,80u);
    FS_Debug_UL("Bytes/Sector",g_fs_bytes_per_sector,100u);
    FS_Debug_UL("Sec/Cluster",g_fs_sectors_per_cluster,120u);
    FS_Debug_UL("MFT LCN",(unsigned long)g_ntfs_mft_lcn,140u);
    FS_Debug_UL("Rec Size",g_ntfs_record_size,160u);
    return 1u;
}

/* ------------------------ attribute lists ---------------------------
   A record too full for all its attributes (a folder of thousands of
   files, a file in hundreds of fragments) lists them in $ATTRIBUTE_LIST,
   naming the extension records that hold them; a long runlist is split
   there into extents, each starting at a VCN. */

static unsigned char g_ntfs_blk2[NTFS_MAX_RECORD_SIZE];

/* The base record's $ATTRIBUTE_LIST into g_ntfs_blk2; its length, 0 if it
   has none. */
static unsigned int FS_NTFS_Read_AttrList(const unsigned char *rec)
{
    FS_NTFS_RUN runs[16];
    unsigned int n;
    unsigned int a;
    unsigned int ofs;
    unsigned long long len;

    a=FS_NTFS_FindAttr(rec,0x20ul,FS_ReadLE16(&rec[20]),0u);
    if(a!=0u)
    {
        len=FS_ReadLE32(&rec[a+16u]);
        ofs=a+FS_ReadLE16(&rec[a+20u]);
        if((ofs+len)>g_ntfs_record_size)
        {
            return 0u;
        }
        memcpy(g_ntfs_blk2,&rec[ofs],(size_t)len);
        return (unsigned int)len;
    }

    a=FS_NTFS_FindAttr(rec,0x20ul,FS_ReadLE16(&rec[20]),1u);
    if(a==0u)
    {
        return 0u;
    }
    n=0u;
    len=FS_ReadLE64(&rec[a+48u]);
    if(len>NTFS_MAX_RECORD_SIZE)
    {
        len=NTFS_MAX_RECORD_SIZE;
    }
    if((FS_NTFS_ParseRuns(&rec[a+FS_ReadLE16(&rec[a+32u])],runs,&n,16u)==0u)||
       (FS_NTFS_ReadRuns(runs,n,0ull,((unsigned int)len+511u)&~511u,g_ntfs_blk2)==0u))
    {
        return 0u;
    }
    return (unsigned int)len;
}

/* From *pos in the list (len bytes), the next entry of type whose name is
   name_len characters: its record, *vcn the VCN its extent starts at; 0
   past the last. */
static unsigned long long FS_NTFS_AttrList_Next(unsigned int len,unsigned int *pos,unsigned long type,
                                                unsigned char name_len,unsigned long long *vcn)
{
    unsigned int p;
    unsigned int elen;

    for(p=*pos;(p+26u)<=len;p+=elen)
    {
        elen=FS_ReadLE16(&g_ntfs_blk2[p+4u]);
        if(elen<26u)
        {
            break;
        }
        if((FS_ReadLE32(&g_ntfs_blk2[p])==type)&&(g_ntfs_blk2[p+6u]==name_len))
        {
            *pos=p+elen;
            *vcn=FS_ReadLE64(&g_ntfs_blk2[p+8u]);
            return FS_ReadLE64(&g_ntfs_blk2[p+16u]) & 0x0000FFFFFFFFFFFFull;
        }
    }

    *pos=len;
    return 0ull;
}

/* ------------------------ attribute maps ----------------------------
   Where a non-resident attribute's clusters are (FS_NTFS_MAP): one
   extent's runs at a time, the next read in from its record when a VCN in
   it is wanted, so a file in any number of pieces can be read. */

/* Extent e's runs into the map, its record read into scratch: the
   attribute's offset there, 0 if it cannot be read. */
static unsigned int FS_NTFS_Map_Load(FS_NTFS_MAP *m,unsigned int e,unsigned char *scratch)
{
    unsigned int a;

    m->ext_loaded=NTFS_MAX_EXTENTS;
    m->run_count=0u;
    if(FS_NTFS_ReadRecord((unsigned long long)m->ext_frn[e],scratch)==0u)
    {
        return 0u;
    }
    a=FS_NTFS_FindExtent(scratch,m->type,m->name_len,(unsigned long long)m->ext_vcn[e]);
    if((a==0u)||(FS_NTFS_ParseRuns(&scratch[a+FS_ReadLE16(&scratch[a+32u])],m->runs,&m->run_count,NTFS_MAX_RUNS)==0u))
    {
        return 0u;
    }
    m->ext_loaded=e;
    return a;
}

/* The map of a non-resident attribute of record frn, which is in
   g_ntfs_record_buf (list: the length of its attribute list, read by
   FS_NTFS_Read_AttrList, or 0); *size its data size. */
static unsigned char FS_NTFS_Map_Init(FS_NTFS_MAP *m,unsigned long long frn,unsigned int list,unsigned long type,
                                      unsigned char name_len,unsigned long long *size,unsigned char *scratch)
{
    unsigned long long efrn;
    unsigned long long vcn;
    unsigned int pos;
    unsigned int a;

    m->type=type;
    m->name_len=name_len;
    m->run_count=0u;
    m->ext_count=0u;
    m->ext_loaded=NTFS_MAX_EXTENTS;

    if(list==0u)
    {
        a=FS_NTFS_FindExtent(g_ntfs_record_buf,type,name_len,0ull);
        if((a==0u)||
           (FS_NTFS_ParseRuns(&g_ntfs_record_buf[a+FS_ReadLE16(&g_ntfs_record_buf[a+32u])],m->runs,&m->run_count,
                              NTFS_MAX_RUNS)==0u))
        {
            return 0u;
        }
        *size=FS_ReadLE64(&g_ntfs_record_buf[a+48u]);
        m->ext_vcn[0]=0ul;
        m->ext_frn[0]=(unsigned long)frn;
        m->ext_count=1u;
        m->ext_loaded=0u;
        return 1u;
    }

    pos=0u;
    while((efrn=FS_NTFS_AttrList_Next(list,&pos,type,name_len,&vcn))!=0ull)
    {
        if(m->ext_count>=NTFS_MAX_EXTENTS)
        {
            return 0u;
        }
        m->ext_vcn[m->ext_count]=(unsigned long)vcn;
        m->ext_frn[m->ext_count]=(unsigned long)efrn;
        m->ext_count++;
    }
    if((m->ext_count==0u)||(m->ext_vcn[0]!=0ul))
    {
        return 0u;
    }

    a=FS_NTFS_Map_Load(m,0u,scratch);
    if(a==0u)
    {
        return 0u;
    }
    *size=FS_ReadLE64(&scratch[a+48u]);
    return 1u;
}

/* The cluster (LCN) of a VCN and how many follow it in its run; 0 past the
   end.  An extent not held is read in through scratch. */
static unsigned char FS_NTFS_Map_Lcn(FS_NTFS_MAP *m,unsigned long long vcn,unsigned long long *lcn,
                                     unsigned long long *left,unsigned char *scratch)
{
    unsigned long long base;
    unsigned int e;
    unsigned int r;

    e=m->ext_count;
    while((e>0u)&&((unsigned long long)m->ext_vcn[e-1u]>vcn))
    {
        e--;
    }
    if(e==0u)
    {
        return 0u;
    }
    e--;
    if((e!=m->ext_loaded)&&(FS_NTFS_Map_Load(m,e,scratch)==0u))
    {
        return 0u;
    }

    base=m->ext_vcn[e];
    for(r=0u;r<m->run_count;r++)
    {
        if(vcn<(base+m->runs[r].len))
        {
            *lcn=(unsigned long long)m->runs[r].lcn+(vcn-base);
            *left=base+m->runs[r].len-vcn;
            return 1u;
        }
        base+=m->runs[r].len;
    }
    return 0u;
}

/* len bytes (whole sectors, at most 4 KB) at byte pos of the attribute into
   dst.  The pieces are looked up first, dst serving as the scratch for any
   extent read in, then read. */
static unsigned char FS_NTFS_Map_Read(FS_NTFS_MAP *m,unsigned long long pos,unsigned int len,unsigned char *dst)
{
    unsigned long sec[8];
    unsigned int cnt[8];
    unsigned long long lcn;
    unsigned long long left;
    unsigned long long avail;
    unsigned int n;
    unsigned int i;
    unsigned int off;
    unsigned int in_cl;
    unsigned int take;

    n=0u;
    for(off=0u;off<len;off+=take)
    {
        in_cl=(unsigned int)((pos+off)%g_fs_cluster_size_bytes);
        if((n>=8u)||(FS_NTFS_Map_Lcn(m,(pos+off)/g_fs_cluster_size_bytes,&lcn,&left,dst)==0u))
        {
            return 0u;
        }
        avail=(left*g_fs_cluster_size_bytes)-in_cl;
        take=(avail<(unsigned long long)(len-off)) ? (unsigned int)avail : (len-off);
        sec[n]=g_fs_part_start+(unsigned long)(lcn*g_fs_sectors_per_cluster)+(in_cl>>9);
        cnt[n]=take>>9;
        n++;
    }

    off=0u;
    for(i=0u;i<n;i++)
    {
        Read_Buffers(sec[i],(unsigned char)cnt[i],&dst[off]);
        off+=cnt[i]<<9;
    }
    return 1u;
}

/* ------------------------ folders -------------------------------------- */

/* A $FILE_NAME (an index key or the attribute) as ASCII, accents folded. */
static void FS_NTFS_Name(const unsigned char *fn,char *lfn)
{
    unsigned int n;
    unsigned int i;

    n=fn[0x40u];
    for(i=0u;(i<n)&&(i<259u);i++)
    {
        lfn[i]=FS_Fold(FS_ReadLE16(&fn[0x42u+(i*2u)]));
    }
    lfn[i]=0;
}

/* The open folder's $INDEX_ALLOCATION. */
static FS_NTFS_MAP g_ntfs_dir_map;

/* Index block vcn into buf.  A vcn counts clusters, or 512-byte units when
   blocks are smaller than a cluster. */
static unsigned char FS_NTFS_ReadIndexBlock(unsigned long long vcn,unsigned char *buf)
{
    if(FS_NTFS_Map_Read(&g_ntfs_dir_map,
                        (g_ntfs_index_block_size<g_fs_cluster_size_bytes) ? (vcn<<9) :
                                                                           (vcn*(unsigned long long)g_fs_cluster_size_bytes),
                        g_ntfs_index_block_size,buf)==0u)
    {
        return 0u;
    }

    if((buf[0]!='I')||(buf[1]!='N')||(buf[2]!='D')||(buf[3]!='X'))
    {
        return 0u;
    }
    return FS_NTFS_ApplyFixup(buf,g_ntfs_index_block_size);
}

/* The folder's $I30 index walked in order: its B-tree from the root (in the
   record), each entry's sub-node before the entry, so the names come sorted
   and only blocks the tree uses are read.  Odd and even levels have a block
   buffer each (g_fs_blk, g_ntfs_blk2, free once the walk starts): a tree of
   two block levels, as a folder of 10000 files has, reads each block once;
   deeper ones read a block again on the way back up. */
#define NTFS_MAX_DEPTH   8u
#define NTFS_MAX_BLOCKS  16384u

static unsigned int FS_NTFS_ListDirectory(unsigned long long dir_frn)
{
    unsigned long long vcn[NTFS_MAX_DEPTH];
    unsigned int pos[NTFS_MAX_DEPTH];     /* the entry in the node; 0: block just entered */
    unsigned int end[NTFS_MAX_DEPTH];     /* past the node's entries */
    unsigned char down[NTFS_MAX_DEPTH];   /* the entry's sub-node done */
    unsigned long long held[2];           /* the block in each buffer */
    unsigned long long frn;
    unsigned long long size;
    const unsigned char *node;
    const unsigned char *ie;
    unsigned int depth;
    unsigned int blocks;
    unsigned int attr;
    unsigned int len;
    unsigned int list;                    /* the attribute list's length */
    unsigned int lpos;
    unsigned char have_blocks;
    unsigned char flags;
    char lfn[260];
    char name[VFS_NAME_LEN];

    FS_Clear_Entries();
    if(FS_NTFS_ReadRecord(dir_frn,g_ntfs_record_buf)==0u)
    {
        return 0u;
    }

    /* the index allocation's map, then the root: in the record, or in the
       extension record the attribute list names */
    list=FS_NTFS_Read_AttrList(g_ntfs_record_buf);
    have_blocks=FS_NTFS_Map_Init(&g_ntfs_dir_map,dir_frn,list,0xA0ul,4u,&size,g_fs_blk);
    if(list!=0u)
    {
        lpos=0u;
        frn=FS_NTFS_AttrList_Next(list,&lpos,0x90ul,4u,&size);
        if((frn!=0ull)&&(frn!=dir_frn)&&(FS_NTFS_ReadRecord(frn,g_ntfs_record_buf)==0u))
        {
            return 0u;
        }
    }

    attr=FS_NTFS_FindAttr(g_ntfs_record_buf,0x90ul,FS_ReadLE16(&g_ntfs_record_buf[20]),0u);
    if(attr==0u)
    {
        return 0u;
    }

    attr+=FS_ReadLE16(&g_ntfs_record_buf[attr+20u])+16u;    /* the root's index header */
    pos[0]=attr+FS_ReadLE32(&g_ntfs_record_buf[attr]);
    end[0]=attr+FS_ReadLE32(&g_ntfs_record_buf[attr+4u]);
    if(end[0]>g_ntfs_record_size)
    {
        return 0u;
    }
    if((g_ntfs_record_buf[attr+12u] & 0x01u)==0u)
    {
        have_blocks=0u;                   /* a small index: the root alone */
    }

    depth=0u;
    down[0]=0u;
    held[0]=~0ull;
    held[1]=~0ull;
    blocks=0u;
    g_fs_blk_n=0ul;

    while(g_browser_entry_count<VFS_MAX_ENTRIES)
    {
        node=g_ntfs_record_buf;
        if(depth!=0u)
        {
            node=((depth & 1u)!=0u) ? g_fs_blk : g_ntfs_blk2;
            if(held[depth & 1u]!=vcn[depth])
            {
                blocks++;
                held[depth & 1u]=~0ull;
                if((blocks>NTFS_MAX_BLOCKS)||(FS_NTFS_ReadIndexBlock(vcn[depth],(unsigned char *)node)==0u))
                {
                    break;
                }
                held[depth & 1u]=vcn[depth];
            }
            if(pos[depth]==0u)
            {
                pos[depth]=24u+FS_ReadLE32(&node[24]);
                end[depth]=24u+FS_ReadLE32(&node[28]);
                if(end[depth]>g_ntfs_index_block_size)
                {
                    break;
                }
            }
        }

        if((pos[depth]+16u)>end[depth])
        {
            break;
        }
        ie=&node[pos[depth]];
        len=FS_ReadLE16(&ie[8]);
        flags=ie[12];
        if((len<16u)||((pos[depth]+len)>end[depth]))
        {
            break;
        }

        if(((flags & 0x01u)!=0u)&&(down[depth]==0u)&&(have_blocks!=0u)&&((depth+1u)<NTFS_MAX_DEPTH))
        {
            down[depth]=1u;
            depth++;
            vcn[depth]=FS_ReadLE64(&ie[len-8u]);
            pos[depth]=0u;
            down[depth]=0u;
            continue;
        }
        down[depth]=0u;

        if((flags & 0x02u)!=0u)           /* the node's last entry carries no name */
        {
            if(depth==0u)
            {
                break;
            }
            depth--;
            continue;
        }

        /* DOS 8.3 aliases of long names left out */
        if((FS_ReadLE16(&ie[10])>=0x42u)&&(ie[16u+0x41u]!=0x02u))
        {
            frn=FS_ReadLE64(ie) & 0x0000FFFFFFFFFFFFull;
            FS_NTFS_Name(&ie[16],lfn);
            FS_CopyName(name,lfn);
            (void)FS_Add_Entry(name,((FS_ReadLE32(&ie[16u+0x38u]) & 0x10000000ul)!=0ul) ? VFS_TYPE_DIR : VFS_TYPE_FILE,
                               (unsigned long)frn,(unsigned long)(frn>>32));
        }
        pos[depth]+=len;
    }

    return g_browser_entry_count;
}

/* The $FILE_NAME attributes of a record ranked: 3 the open folder's long
   name, 2 a long name, 1 an 8.3 alias; the best so far kept in lfn. */
static void FS_NTFS_Rank_Names(const unsigned char *rec,unsigned char *best,char *lfn)
{
    const unsigned char *fn;
    unsigned int pos;
    unsigned int len;
    unsigned char rank;

    for(pos=FS_ReadLE16(&rec[20]);(pos+24u)<=g_ntfs_record_size;pos+=len)
    {
        if(FS_ReadLE32(&rec[pos])==0xFFFFFFFFul)
        {
            break;
        }
        len=FS_ReadLE32(&rec[pos+4u]);
        if((len<24u)||((pos+len)>g_ntfs_record_size))
        {
            break;
        }
        if((FS_ReadLE32(&rec[pos])==0x30ul)&&(rec[pos+8u]==0u))
        {
            fn=&rec[pos+FS_ReadLE16(&rec[pos+20u])];
            rank=(fn[0x41u]==0x02u) ? 1u :
                 (((FS_ReadLE64(fn) & 0x0000FFFFFFFFFFFFull)==g_current_dir_id) ? 3u : 2u);
            if(rank>*best)
            {
                *best=rank;
                FS_NTFS_Name(fn,lfn);
            }
        }
    }
}

/* A record's name in the open folder and the size of its data; both can
   be in extension records. */
static unsigned char FS_NTFS_Read_Entry(unsigned long long frn,FS_ENTRY_INFO *out)
{
    unsigned long long efrn;
    unsigned long long vcn;
    unsigned int pos;
    unsigned int list;
    unsigned char best;
    char lfn[260];

    if(FS_NTFS_ReadRecord(frn,g_ntfs_record_buf)==0u)
    {
        return 0u;
    }

    best=0u;
    lfn[0]=0;
    out->size=0ul;
    out->object_id=frn;
    FS_NTFS_Rank_Names(g_ntfs_record_buf,&best,lfn);

    /* a resident $DATA, or the first extent of one */
    pos=FS_NTFS_FindAttr(g_ntfs_record_buf,0x80ul,FS_ReadLE16(&g_ntfs_record_buf[20]),0u);
    if((pos!=0u)&&(g_ntfs_record_buf[pos+9u]==0u))
    {
        out->size=FS_ReadLE32(&g_ntfs_record_buf[pos+16u]);
    }
    else
    {
        pos=FS_NTFS_FindExtent(g_ntfs_record_buf,0x80ul,0u,0ull);
        if(pos!=0u)
        {
            out->size=FS_Cap32(FS_ReadLE64(&g_ntfs_record_buf[pos+48u]));
        }
    }

    list=FS_NTFS_Read_AttrList(g_ntfs_record_buf);
    if(list!=0u)
    {
        g_fs_blk_n=0ul;
        pos=0u;
        while((best<3u)&&((efrn=FS_NTFS_AttrList_Next(list,&pos,0x30ul,0u,&vcn))!=0ull))
        {
            if((efrn!=frn)&&(FS_NTFS_ReadRecord(efrn,g_fs_blk)!=0u))
            {
                FS_NTFS_Rank_Names(g_fs_blk,&best,lfn);
            }
        }

        pos=0u;
        efrn=FS_NTFS_AttrList_Next(list,&pos,0x80ul,0u,&vcn);
        if((efrn!=0ull)&&(efrn!=frn)&&(vcn==0ull)&&(FS_NTFS_ReadRecord(efrn,g_fs_blk)!=0u))
        {
            pos=FS_NTFS_FindExtent(g_fs_blk,0x80ul,0u,0ull);
            if(pos!=0u)
            {
                out->size=FS_Cap32(FS_ReadLE64(&g_fs_blk[pos+48u]));
            }
        }
    }

    if(best==0u)
    {
        return 0u;
    }
    FS_CopyName(out->name,lfn);
    return 1u;
}

/* ------------------------ files ---------------------------------------- */

/* The unnamed $DATA: resident, or mapped (FS_NTFS_MAP). */
static unsigned char FS_NTFS_OpenFile(unsigned long long frn)
{
    unsigned long long size;
    unsigned int pos;
    unsigned int len;

    FS_Reset_File_Handle();
    if(FS_NTFS_ReadRecord(frn,g_ntfs_record_buf)==0u)
    {
        return 0u;
    }
    g_current_file.fs_type=FS_TYPE_NTFS;

    pos=FS_NTFS_FindAttr(g_ntfs_record_buf,0x80ul,FS_ReadLE16(&g_ntfs_record_buf[20]),0u);
    if((pos!=0u)&&(g_ntfs_record_buf[pos+9u]==0u))
    {
        len=FS_ReadLE32(&g_ntfs_record_buf[pos+16u]);
        if(len>FILE_RESIDENT_MAX)
        {
            return 0u;
        }
        memcpy(g_current_file.resident_buffer,&g_ntfs_record_buf[pos+FS_ReadLE16(&g_ntfs_record_buf[pos+20u])],len);
        g_current_file.resident_size=len;
        g_current_file.size=len;
        g_current_file.resident=1u;
        g_current_file.valid=1u;
        return 1u;
    }

    if(FS_NTFS_Map_Init(&g_current_file.map,frn,FS_NTFS_Read_AttrList(g_ntfs_record_buf),0x80ul,0u,
                        &size,g_fs_blk)==0u)
    {
        return 0u;
    }
    g_fs_blk_n=0ul;
    g_current_file.size=FS_Cap32(size);
    g_current_file.valid=1u;
    return 1u;
}

static unsigned char FS_NTFS_MapPos(unsigned long pos,unsigned long *sector,unsigned int *offset)
{
    unsigned long long lcn;
    unsigned long long left;
    unsigned long byte_in_cluster;

    byte_in_cluster=pos % g_fs_cluster_size_bytes;
    *offset=(unsigned int)(byte_in_cluster % g_fs_bytes_per_sector);

    g_fs_blk_n=0ul;
    if(FS_NTFS_Map_Lcn(&g_current_file.map,(unsigned long long)(pos / g_fs_cluster_size_bytes),&lcn,&left,g_fs_blk)==0u)
    {
        return 0u;
    }

    *sector=g_fs_part_start+(unsigned long)(lcn*g_fs_sectors_per_cluster)+(byte_in_cluster/g_fs_bytes_per_sector);
    return 1u;
}

unsigned char Init_FileSystem(void)
{
    FS_Reset_File_Handle();
    FS_Clear_Entries();
    g_fs_blk_n=0ul;
    g_fs_dir_end=0ul;

    if(FS_Detect_FileSystem()==0u)
    {
        return 0u;
    }

    Read_Buffer(g_fs_part_start,buffer);
    if(g_fs_type==FS_TYPE_FAT32)
    {
        return FS_FAT32_Init();
    }
    if(g_fs_type==FS_TYPE_EXFAT)
    {
        return FS_EXFAT_Init();
    }
    if(g_fs_type==FS_TYPE_NTFS)
    {
        return FS_NTFS_Init();
    }

    return 0u;
}

unsigned long long FS_Get_RootId(void)
{
    if(g_fs_type==FS_TYPE_FAT32)
    {
        return (unsigned long long)g_fat32_root_cluster;
    }
    if(g_fs_type==FS_TYPE_EXFAT)
    {
        return (unsigned long long)g_exfat_root_cluster;
    }
    if(g_fs_type==FS_TYPE_NTFS)
    {
        return 5ull;
    }
    return 0ull;
}

unsigned int FS_Open_DirectoryById(unsigned long long dir_id)
{
    g_current_dir_id=dir_id;
    g_fs_blk_n=0ul;
    g_fs_dir_end=0ul;
    if(g_fs_type==FS_TYPE_FAT32)
    {
        return FS_FAT32_ListDirectory((unsigned long)dir_id);
    }
    if(g_fs_type==FS_TYPE_EXFAT)
    {
        return FS_EXFAT_ListDirectory(dir_id);
    }
    if(g_fs_type==FS_TYPE_NTFS)
    {
        return FS_NTFS_ListDirectory(dir_id);
    }
    FS_Clear_Entries();
    return 0u;
}

/* An entry's name, size and first cluster, read back from its directory
   record (NTFS: its MFT record); the last FS_INFO_SLOTS asked for are kept.
   0 if it cannot be read. */
static FS_ENTRY_INFO *FS_Entry_Info(unsigned int index)
{
    FS_ENTRY_INFO *f;
    unsigned long where;
    unsigned long rec;
    unsigned char ok;
    unsigned int i;

    if((index<1u)||(index>g_browser_entry_count))
    {
        return 0;
    }

    for(i=0u;i<FS_INFO_SLOTS;i++)
    {
        if(g_fs_info[i].index==index)
        {
            return &g_fs_info[i];
        }
    }

    f=&g_fs_info[g_fs_info_next];
    g_fs_info_next=(g_fs_info_next+1u)%FS_INFO_SLOTS;
    f->index=0u;

    where=g_browser_entries[index].where;
    rec=g_browser_entries[index].info & 0x00FFFFFFul;
    ok=0u;
    if(g_fs_type==FS_TYPE_FAT32)
    {
        ok=FS_FAT32_Read_Entry(where,rec,f);
    }
    else if(g_fs_type==FS_TYPE_EXFAT)
    {
        ok=FS_EXFAT_Read_Entry(where,rec,f);
    }
    else if(g_fs_type==FS_TYPE_NTFS)
    {
        ok=FS_NTFS_Read_Entry(((unsigned long long)rec<<32)|(unsigned long long)where,f);
    }

    if(ok==0u)
    {
        return 0;
    }
    f->index=index;
    return f;
}

static void FS_Set_Entry_Type(unsigned int index,unsigned char type)
{
    g_browser_entries[index].info=(g_browser_entries[index].info & 0x00FFFFFFul)|((unsigned long)type<<24);
}

static unsigned char FS_Open_Entry_Raw(unsigned int index)
{
    FS_ENTRY_INFO *f;

    if((index<1u)||(index>g_browser_entry_count)||(FS_ENTRY_TYPE(index)==VFS_TYPE_DIR))
    {
        return 0u;
    }

    if(g_fs_type==FS_TYPE_NTFS)
    {
        return FS_NTFS_OpenFile(((unsigned long long)(g_browser_entries[index].info & 0x00FFFFFFul)<<32)|
                                (unsigned long long)g_browser_entries[index].where);
    }

    f=FS_Entry_Info(index);
    if(f==0)
    {
        return 0u;
    }
    if(g_fs_type==FS_TYPE_FAT32)
    {
        return FS_FAT32_OpenFile((unsigned long)f->object_id,f->size);
    }
    if(g_fs_type==FS_TYPE_EXFAT)
    {
        return FS_EXFAT_OpenFile(f->object_id,f->size);
    }
    return 0u;
}

unsigned char FS_Read_CurrentFile_Bytes(unsigned long pos,unsigned char *dst,unsigned int len);

/* What a file is by its first bytes, for names that do not say; 0 if
   nothing known. */
static unsigned char FS_FileSniff(unsigned int index)
{
    unsigned char b[48];
    unsigned char m[4];
    unsigned long size;
    unsigned long tag;

    if(FS_Open_Entry_Raw(index)==0u)
    {
        return 0u;
    }

    size=g_current_file.size;
    if(size<12ul)
    {
        return 0u;
    }
    memset(b,0,sizeof(b));
    if(FS_Read_CurrentFile_Bytes(0ul,b,(size<48ul) ? (unsigned int)size : 48u)==0u)
    {
        return 0u;
    }

    if(!memcmp(b,"RIFF",4))
    {
        if(!memcmp(&b[8],"AVI ",4)) return VFS_TYPE_AVI;
        if(!memcmp(&b[8],"WAVE",4)) return VFS_TYPE_WAV;
        return 0u;
    }
    /* EBML magic ID: Matroska/MKV/WebM */
    if((b[0]==0x1Au)&&(b[1]==0x45u)&&(b[2]==0xDFu)&&(b[3]==0xA3u)) return VFS_TYPE_MKV;
    if(!memcmp(b,"fLaC",4)) return VFS_TYPE_FLAC;
    if(!memcmp(b,"ID3",3))
    {
        /* an ID3v2 tag: MP3, or now and then FLAC behind it */
        tag=10ul+((unsigned long)(b[6] & 0x7Fu)<<21)+((unsigned long)(b[7] & 0x7Fu)<<14)+
            ((unsigned long)(b[8] & 0x7Fu)<<7)+(unsigned long)(b[9] & 0x7Fu)+(((b[5] & 0x10u)!=0u) ? 10ul : 0ul);
        if(((tag+4ul)<=size)&&(FS_Read_CurrentFile_Bytes(tag,m,4u)!=0u)&&!memcmp(m,"fLaC",4))
        {
            return VFS_TYPE_FLAC;
        }
        return VFS_TYPE_MP3;
    }

    if(size<48ul) return 0u;
    if(!memcmp(b,"IMPM",4)) return VFS_TYPE_IT;
    if(!memcmp(b,"Extended Module: ",17)) return VFS_TYPE_XM;
    if(!memcmp(&b[44],"SCRM",4)&&(b[29]==0x10u)) return VFS_TYPE_S3M;

    if((size>=1084ul)&&(FS_Read_CurrentFile_Bytes(1080ul,m,4u)!=0u))
    {
        if(!memcmp(m,"M.K.",4)||!memcmp(m,"M!K!",4)||!memcmp(m,"FLT4",4)||!memcmp(m,"FLT8",4)||
           (((m[0]>='1')&&(m[0]<='9'))&&!memcmp(&m[1],"CHN",3))||
           (((m[0]>='1')&&(m[0]<='9'))&&((m[1]>='0')&&(m[1]<='9'))&&(m[2]=='C')&&((m[3]=='H')||(m[3]=='N'))))
        {
            return VFS_TYPE_MOD;
        }
    }

    return 0u;
}

/* The type the name gave, without opening the file: VFS_TYPE_FILE when it
   does not say (FS_Get_Entry_Type looks inside). */
unsigned char FS_Peek_Entry_Type(unsigned int index)
{
    unsigned char type;

    if((index<1u)||(index>g_browser_entry_count))
    {
        return 0u;
    }
    type=FS_ENTRY_TYPE(index);
    return (type==VFS_TYPE_OTHER) ? VFS_TYPE_FILE : type;
}

unsigned char FS_Get_Entry_Type(unsigned int index)
{
    unsigned char type;

    if((index<1u)||(index>g_browser_entry_count))
    {
        return 0u;
    }

    type=FS_ENTRY_TYPE(index);
    if(type==VFS_TYPE_OTHER)
    {
        return VFS_TYPE_FILE;
    }
    if(type!=VFS_TYPE_FILE)
    {
        return type;
    }

    /* Sniffed once: a plain file is not reopened on every repaint. */
    type=FS_FileSniff(index);
    FS_Set_Entry_Type(index,(type!=0u) ? type : VFS_TYPE_OTHER);
    return (type!=0u) ? type : VFS_TYPE_FILE;
}

void FS_Get_Entry_Name(unsigned int index,char *txtbuff)
{
    FS_ENTRY_INFO *f;

    f=FS_Entry_Info(index);
    if(f==0)
    {
        txtbuff[0]=0;
        return;
    }
    FS_CopyName(txtbuff,f->name);
}

unsigned long FS_Get_Entry_Size(unsigned int index)
{
    FS_ENTRY_INFO *f;

    f=FS_Entry_Info(index);
    return (f!=0) ? f->size : 0ul;
}

unsigned long long FS_Get_Entry_Id(unsigned int index)
{
    FS_ENTRY_INFO *f;

    if((index<1u)||(index>g_browser_entry_count))
    {
        return 0ull;
    }
    if(g_fs_type==FS_TYPE_NTFS)
    {
        return ((unsigned long long)(g_browser_entries[index].info & 0x00FFFFFFul)<<32)|
               (unsigned long long)g_browser_entries[index].where;
    }

    f=FS_Entry_Info(index);
    return (f!=0) ? f->object_id : 0ull;
}

unsigned char FS_Open_File_By_Index(unsigned int index)
{
    if(FS_Get_Entry_Type(index)==VFS_TYPE_DIR)
    {
        return 0u;
    }

    return FS_Open_Entry_Raw(index);
}

unsigned long FS_Get_CurrentFile_Size(void)
{
    return g_current_file.size;
}

static unsigned char FS_Map_CurrentFile_Pos(unsigned long pos,unsigned long *sector,unsigned int *offset)
{
    if(g_current_file.fs_type==FS_TYPE_FAT32)
    {
        return FS_FAT32_MapPos(pos,sector,offset);
    }

    if(g_current_file.fs_type==FS_TYPE_EXFAT)
    {
        return FS_EXFAT_MapPos(pos,sector,offset);
    }

    if(g_current_file.fs_type==FS_TYPE_NTFS)
    {
        return FS_NTFS_MapPos(pos,sector,offset);
    }

    return 0u;
}

unsigned char FS_Read_CurrentFile_Bytes(unsigned long pos,unsigned char *dst,unsigned int len)
{
    unsigned long sector;
    unsigned long sector2;
    unsigned long remain_file;
    unsigned int offset;
    unsigned int offset2;
    unsigned int take;
    unsigned char blocks;
    unsigned char max_blocks;

    if((g_current_file.valid==0u)||(dst==0)||(len==0u)||(pos>=g_current_file.size))
    {
        return 0u;
    }

    remain_file=g_current_file.size-pos;
    if((unsigned long)len>remain_file)
    {
        len=(unsigned int)remain_file;
    }

    if(g_current_file.resident!=0u)
    {
        if(pos>=g_current_file.resident_size)
        {
            return 0u;
        }
        if(((unsigned long)len)>(g_current_file.resident_size-pos))
        {
            len=(unsigned int)(g_current_file.resident_size-pos);
        }
        memcpy(dst,&g_current_file.resident_buffer[pos],len);
        return 1u;
    }

    while(len!=0u)
    {
        if(FS_Map_CurrentFile_Pos(pos,&sector,&offset)==0u)
        {
            return 0u;
        }

        if((offset==0u)&&(len>=512u))
        {
            max_blocks=(unsigned char)(len>>9);
            if(max_blocks>32u)
            {
                max_blocks=32u;
            }
            if(g_fs_multi_read_ok==0u)
            {
                max_blocks=1u;          /* single-sector reads only */
            }

            blocks=1u;
            while(blocks<max_blocks)
            {
                if(FS_Map_CurrentFile_Pos(pos+((unsigned long)blocks*512ul),&sector2,&offset2)==0u)
                {
                    break;
                }
                if((offset2!=0u)||(sector2!=(sector+(unsigned long)blocks)))
                {
                    break;
                }
                blocks++;
            }

            if(blocks > 1u)
            {
                Read_Buffers(sector,blocks,dst);
            }
            else
            {
                Read_Buffer(sector,dst);
            }

            g_current_file.sector_cache_valid=0u;
            take=(unsigned int)blocks<<9;
            pos+=(unsigned long)take;
            dst+=take;
            len=(unsigned int)(len-take);
            continue;
        }

        if((g_current_file.sector_cache_valid==0u)||(g_current_file.cached_sector_number!=sector))
        {
            Read_Buffer(sector,g_current_file.sector_buffer);
            g_current_file.cached_sector_number=sector;
            g_current_file.sector_cache_valid=1u;
        }

        take=(unsigned int)(512u-offset);
        if(take>len)
        {
            take=len;
        }

        memcpy(dst,&g_current_file.sector_buffer[offset],take);
        pos+=(unsigned long)take;
        dst+=take;
        len=(unsigned int)(len-take);
    }

    return 1u;
}

unsigned char FS_Read_CurrentFile_Byte(unsigned long pos,unsigned char *value)
{
    unsigned long sector;
    unsigned int offset;

    if((g_current_file.valid==0u)||(pos>=g_current_file.size))
    {
        return 0u;
    }

    if(g_current_file.resident!=0u)
    {
        if(pos>=g_current_file.resident_size)
        {
            return 0u;
        }
        *value=g_current_file.resident_buffer[pos];
        return 1u;
    }

    if(FS_Map_CurrentFile_Pos(pos,&sector,&offset)==0u)
    {
        return 0u;
    }

    if((g_current_file.sector_cache_valid==0u)||(g_current_file.cached_sector_number!=sector))
    {
        Read_Buffer(sector,g_current_file.sector_buffer);
        g_current_file.cached_sector_number=sector;
        g_current_file.sector_cache_valid=1u;
    }

    *value=g_current_file.sector_buffer[offset];
    return 1u;
}

void FS_Get_Type_Name(char *txtbuff)
{
    if(g_fs_type==FS_TYPE_FAT32)
    {
        FS_CopyName(txtbuff,"FAT32");
        return;
    }
    if(g_fs_type==FS_TYPE_EXFAT)
    {
        FS_CopyName(txtbuff,"exFAT");
        return;
    }
    if(g_fs_type==FS_TYPE_NTFS)
    {
        FS_CopyName(txtbuff,"NTFS");
        return;
    }
    FS_CopyName(txtbuff,"UNKNOWN");
}

unsigned char FS_Get_Type(void)
{
    return g_fs_type;
}

#endif