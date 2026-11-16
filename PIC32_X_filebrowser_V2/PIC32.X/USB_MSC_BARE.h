#ifndef USB_MSC_BARE_H
#define USB_MSC_BARE_H

#include <xc.h>
#include <string.h>

/*
 * Bare-metal PIC32MZ USB Mass Storage host backend.
 *
 * This header intentionally preserves the original storage API expected by the
 * file browser project:
 *   - SDcard_init() initializes USB host + MSC BOT/SCSI
 *   - Read_Buffer(sector, buff) reads one 512-byte LBA sector
 *
 * VBUS NOTE:
 *   This version is for boards where USB VBUS is already powered externally,
 *   so no VBUS switch GPIO is used here.
 */

unsigned char buffer[513];
unsigned char buff2[15];

#define USB_CSR0_HSEN            (1u << 13)
#define USB_CSR0_HSMODE          (1u << 12)
#define USB_CSR0_RESET           (1u << 11)

#define USB_CSR2_RESETIF         (1u << 18)
#define USB_CSR2_CONNIF          (1u << 20)
#define USB_CSR2_DISCONIF        (1u << 21)
#define USB_CSR2_VBUSERRIF       (1u << 23)

#define USB_CSR3_ENDPOINT_SHIFT  16u
#define USB_CSR3_ENDPOINT_MASK   (0xFu << USB_CSR3_ENDPOINT_SHIFT)

#define USB_CRCON_USBIDOVEN      (1u << 9)
#define USB_CRCON_USBIDVAL       (1u << 8)
#define USB_CRCON_PHYIDEN        (1u << 7)
#define USB_CRCON_VBUSMONEN      (1u << 6)
#define USB_CRCON_ASVALMONEN     (1u << 5)
#define USB_CRCON_BSVALMONEN     (1u << 4)
#define USB_CRCON_SENDMONEN      (1u << 3)
#define USB_CRCON_USBIE          (1u << 2)
#define USB_CRCON_USBRIE         (1u << 1)
#define USB_CRCON_USBWKUPEN      (1u << 0)

#define USB_OTG_RXFIFOSZ_SHIFT   24u
#define USB_OTG_TXFIFOSZ_SHIFT   16u
#define USB_OTG_FSDEV            (1u << 6)
#define USB_OTG_LSDEV            (1u << 5)
#define USB_OTG_HOSTMODE         (1u << 2)
#define USB_OTG_HOSTREQ          (1u << 1)
#define USB_OTG_SESSION          (1u << 0)

#define USB_FIFO_SIZE_64         3u
#define USBH_FAST_FIFO_32BIT      1u

#define USB_HOST_SPEED_HS        1u
#define USB_HOST_SPEED_FS        2u
#define USB_HOST_SPEED_LS        3u

#define E0CSR0_FLSHFIFO          (1u << 24)
#define E0CSR0_SVCSETEND         (1u << 23)
#define E0CSR0_SVCRPR            (1u << 22)
#define E0CSR0_SETUPEND          (1u << 20)
#define E0CSR0_DATAEND           (1u << 19)
#define E0CSR0_TXPKTRDY          (1u << 17)
#define E0CSR0_RXPKTRDY          (1u << 16)
#define E0CSR0_NAKTMOUT          (1u << 23)
#define E0CSR0_STATPKT           (1u << 22)
#define E0CSR0_REQPKT            (1u << 21)
#define E0CSR0_ERROR             (1u << 20)
#define E0CSR0_SETUPPKT          (1u << 19)
#define E0CSR0_RXSTALL           (1u << 18)
#ifndef E0CSR0_DISPING
#define E0CSR0_DISPING           (1u << 27)
#endif

#define EP0CSR_LO_RXPKTRDY       0x01u
#define EP0CSR_LO_TXPKTRDY       0x02u
#define EP0CSR_LO_RXSTALL        0x04u
#define EP0CSR_LO_SETUPPKT       0x08u
#define EP0CSR_LO_ERROR          0x10u
#define EP0CSR_LO_REQPKT         0x20u
#define EP0CSR_LO_STATPKT        0x40u
#define EP0CSR_LO_NAKTMOUT       0x80u
#define EP0CSR_HI_FLSHFIFO       0x01u
#define EP0CSR_HI_DATATGGL       0x02u
#define EP0CSR_HI_DTWREN         0x04u
#define EP0CSR_HI_DISPING        0x08u


#define E0CSR2_NAKLIM_SHIFT      24u
#define E0CSR2_SPEED_SHIFT       22u
#define E0CSR2_RXCNT_MASK        0x7Fu

#define TXCSR0_AUTOSET           (1u << 31)
#define TXCSR0_DMAREQEN          (1u << 29)
#define TXCSR0_FRCDATTG          (1u << 27)
#define TXCSR0_DMAREQMD          (1u << 26)
#define TXCSR0_DATAWEN           (1u << 25)
#define TXCSR0_DATATGGL          (1u << 24)
#define TXCSR0_NAKTMOUT          (1u << 23)
#define TXCSR0_CLRDT             (1u << 22)
#define TXCSR0_RXSTALL           (1u << 21)
#define TXCSR0_SETUPPKT          (1u << 20)
#define TXCSR0_FLUSH             (1u << 19)
#define TXCSR0_ERROR             (1u << 18)
#define TXCSR0_FIFONE            (1u << 17)
#define TXCSR0_TXPKTRDY          (1u << 16)
#define TXCSR0_TXMAXP_MASK       0x7FFu

#define RXCSR1_AUTOREQ           (1u << 30)
#define RXCSR1_DMAREQEN          (1u << 29)
#define RXCSR1_PIDERR            (1u << 28)
#define RXCSR1_DMAREQMD          (1u << 27)
#define RXCSR1_DATATWEN          (1u << 26)
#define RXCSR1_DATATGGL          (1u << 25)
#define RXCSR1_CLRDT             (1u << 23)
#define RXCSR1_RXSTALL           (1u << 22)
#define RXCSR1_REQPKT            (1u << 21)
#define RXCSR1_FLUSH             (1u << 20)
#define RXCSR1_DERRNAKT          (1u << 19)
#define RXCSR1_ERROR             (1u << 18)
#define RXCSR1_FIFOFULL          (1u << 17)
#define RXCSR1_RXPKTRDY          (1u << 16)
#define RXCSR1_RXMAXP_MASK       0x7FFu

#define TXCSR2_TXINTERV_SHIFT    24u
#define TXCSR2_SPEED_SHIFT       22u
#define TXCSR2_PROTOCOL_SHIFT    20u
#define TXCSR2_TEP_SHIFT         16u
#define TXCSR2_RXCNT_MASK        0x3FFFu

#define RXCSR3_RXINTERV_SHIFT    8u
#define RXCSR3_SPEED_SHIFT       6u
#define RXCSR3_PROTOCOL_SHIFT    4u
#define RXCSR3_TEP_SHIFT         0u

#define USB_PROTO_CONTROL        0u
#define USB_PROTO_BULK           2u

#define USB_EP_ADDR_MASK         0x0Fu
#define USB_EP_DIR_IN            0x80u

#define USB_REQ_CLEAR_FEATURE     1u
#define USB_REQ_SET_ADDRESS       5u
#define USB_REQ_GET_DESCRIPTOR    6u
#define USB_REQ_SET_CONFIGURATION 9u

#define USB_DT_DEVICE             1u
#define USB_DT_CONFIG             2u

#define USB_RT_DEV_IN             0x80u
#define USB_RT_DEV_OUT            0x00u
#define USB_RT_STD_EP_OUT         0x02u
#define USB_RT_STD_EP_IN          0x82u
#define USB_RT_CLASS_IF_OUT       0x21u

#define MSC_CLASS                 0x08u
#define MSC_SUBCLASS_SCSI         0x06u
#define MSC_PROTO_BOT             0x50u
#define MSC_REQ_RESET             0xFFu
#define MSC_REQ_GET_MAX_LUN       0xFEu

#define SCSI_TEST_UNIT_READY      0x00u
#define SCSI_REQUEST_SENSE        0x03u
#define SCSI_INQUIRY              0x12u
#define SCSI_READ_CAPACITY_10     0x25u
#define SCSI_READ_10              0x28u

#define MSC_CSW_SIG               0x53425355ul

#define USBH_ERR_NONE             0u
#define USBH_ERR_NO_DEVICE        1u
#define USBH_ERR_RESET_TIMEOUT    2u
#define USBH_ERR_EP0_TIMEOUT      3u
#define USBH_ERR_EP0_STALL        4u
#define USBH_ERR_EP0_ERROR        5u
#define USBH_ERR_DESC_PARSE       6u
#define USBH_ERR_NO_MSC           7u
#define USBH_ERR_SET_CONFIG       8u
#define USBH_ERR_BULK_TIMEOUT     9u
#define USBH_ERR_CSW              10u
#define USBH_ERR_NOT_READY        11u
#define USBH_ERR_BLOCK_SIZE       12u
#define USBH_ERR_READ10           13u
#define USBH_ERR_DISCONNECT       14u
#define USBH_ERR_ENUM_FAIL        15u
#define USBH_ERR_NOT_HS           16u

#define USBH_DBG_DY               18u

typedef struct
{
    unsigned char bmRequestType;
    unsigned char bRequest;
    unsigned short wValue;
    unsigned short wIndex;
    unsigned short wLength;
} USB_SETUP_PKT;

typedef struct
{
    unsigned char valid;
    unsigned char interface_number;
    unsigned char bulk_in_addr;
    unsigned char bulk_out_addr;
    unsigned short bulk_in_mps;
    unsigned short bulk_out_mps;
} USB_MSC_INTF;

typedef struct
{
    unsigned char ready;
    unsigned char configured;
    unsigned char device_address;
    unsigned char speed_code;
    unsigned char ep0_mps;
    unsigned char max_lun;
    unsigned char interface_number;
    unsigned char bulk_in_addr;
    unsigned char bulk_out_addr;
    unsigned short bulk_in_mps;
    unsigned short bulk_out_mps;
    unsigned long block_count;
    unsigned long block_size;
    unsigned long cbw_tag;
    unsigned int last_error;
    const char *last_msg; /* exact text of the most recent USBH_set_error() call */
    unsigned int last_ep0csr;
    unsigned int last_txcsr;
    unsigned int last_rxcsr;
    unsigned int last_usbcsr0;
    unsigned int last_usbcsr2;
    unsigned int last_usbotg;
    unsigned int last_usbcrcon;
    unsigned char sense_key;
    unsigned char sense_asc;
    unsigned char sense_ascq;
    char vendor[9];
    char product[17];
} USBH_STATE;

static USBH_STATE g_usbh;

/*
 * g_msc_force_single_block: set to 1 the first time a multi-block READ(10)
 * fails on the currently attached drive.  Some pendrive-to-USB bridge
 * controllers accept SET_CONFIGURATION and INQUIRY fine but choke on a
 * READ(10) whose Transfer Length spans more than one logical block -
 * either STALLing the bulk endpoint, timing out, or returning a CSW with
 * a bad status.  Once detected we stop attempting multi-block reads for
 * the rest of this drive's session (cleared again on the next successful
 * enumeration, so a different/faster drive always gets a fresh chance).
 */
static unsigned char g_msc_force_single_block = 0u;

/* Forward declarations to keep XC32 happy in single-header style builds. */
static void USBH_show_regs(const char *msg);
static void USBH_set_error(unsigned int err, const char *msg);
static void USBH_bulk_ep_stall_recover_in(void);
static void USBH_bulk_ep_stall_recover_out(void);
static unsigned short USBH_min16(unsigned short a, unsigned short b)
{
    return (a < b) ? a : b;
}

static unsigned long USBH_be32(const unsigned char *p)
{
    return (((unsigned long)p[0]) << 24) |
           (((unsigned long)p[1]) << 16) |
           (((unsigned long)p[2]) << 8)  |
           ((unsigned long)p[3]);
}

static unsigned short USBH_le16(const unsigned char *p)
{
    return (unsigned short)(((unsigned short)p[1] << 8) | p[0]);
}

static unsigned long USBH_le32(const unsigned char *p)
{
    return (((unsigned long)p[3]) << 24) |
           (((unsigned long)p[2]) << 16) |
           (((unsigned long)p[1]) << 8)  |
           ((unsigned long)p[0]);
}

static void USBH_u32_to_dec(unsigned long v, char *out)
{
    char tmp[11];
    unsigned char i = 0u;
    if(v == 0ul)
    {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while(v && i < 10u)
    {
        tmp[i++] = (char)('0' + (v % 10ul));
        v /= 10ul;
    }
    while(i)
    {
        *out++ = tmp[--i];
    }
    *out = 0;
}

/* Enumeration trace goes to the boot console's detail line (BOOT.h), which
   ignores it once the boot screen is closed.  The old version wrote at
   line * USBH_DBG_DY, scattering messages over whatever was on screen. */
static void USBH_dbg_line(unsigned int line, const char *msg)
{
    Boot_USB_Line(line, msg);
}

static void USBH_dbg_hex8(unsigned int line, const char *label, unsigned char value)
{
    char t[40];
    unsigned int i = 0u;
    while((label[i] != 0) && (i < (sizeof(t) - 3u)))
    {
        t[i] = label[i];
        i++;
    }
    tohex(value, (char *)&t[i]);
    USBH_dbg_line(line, t);
}

static void USBH_dbg_hex32(unsigned int line, const char *label, unsigned int value)
{
    const char hex[] = "0123456789ABCDEF";
    char t[40];
    unsigned int i = 0u;
    while((label[i] != 0) && (i < (sizeof(t) - 9u)))
    {
        t[i] = label[i];
        i++;
    }
    t[i + 0u] = hex[(value >> 28) & 0xFu];
    t[i + 1u] = hex[(value >> 24) & 0xFu];
    t[i + 2u] = hex[(value >> 20) & 0xFu];
    t[i + 3u] = hex[(value >> 16) & 0xFu];
    t[i + 4u] = hex[(value >> 12) & 0xFu];
    t[i + 5u] = hex[(value >> 8) & 0xFu];
    t[i + 6u] = hex[(value >> 4) & 0xFu];
    t[i + 7u] = hex[(value >> 0) & 0xFu];
    t[i + 8u] = 0;
    USBH_dbg_line(line, t);
}

static void USBH_dbg_u32(unsigned int line, const char *label, unsigned long value)
{
    char t[40];
    char n[16];
    unsigned int i = 0u;
    unsigned int j = 0u;
    while((label[i] != 0) && (i < (sizeof(t) - 16u)))
    {
        t[i] = label[i];
        i++;
    }
    USBH_u32_to_dec(value, n);
    while((n[j] != 0) && (i < (sizeof(t) - 1u)))
    {
        t[i++] = n[j++];
    }
    t[i] = 0;
    USBH_dbg_line(line, t);
}

static void USBH_select_ep(unsigned char ep)
{
    unsigned int r = USBCSR3;
    r &= ~USB_CSR3_ENDPOINT_MASK;
    r |= ((unsigned int)ep << USB_CSR3_ENDPOINT_SHIFT);
    USBCSR3 = r;
}

static volatile unsigned int *USBH_fifo_reg(unsigned char ep_index)
{
    switch(ep_index)
    {
        case 0u: return &USBFIFO0;
        case 1u: return &USBFIFO1;
        case 2u: return &USBFIFO2;
        case 3u: return &USBFIFO3;
        case 4u: return &USBFIFO4;
        case 5u: return &USBFIFO5;
        case 6u: return &USBFIFO6;
        default: return &USBFIFO7;
    }
}

static void USBH_fifo_write8(unsigned char ep_index, const unsigned char *src, unsigned short len)
{
#if USBH_FAST_FIFO_32BIT
    volatile unsigned int *fifo32 = USBH_fifo_reg(ep_index);
    volatile unsigned char *fifo8 = (volatile unsigned char *)fifo32;
    unsigned short i = 0u;
    unsigned int w;

    while((unsigned short)(i + 4u) <= len)
    {
        w=((unsigned int)src[i + 0u]) |
          ((unsigned int)src[i + 1u] << 8) |
          ((unsigned int)src[i + 2u] << 16) |
          ((unsigned int)src[i + 3u] << 24);
        *fifo32=w;
        i=(unsigned short)(i + 4u);
    }

    while(i<len)
    {
        *fifo8=src[i];
        i++;
    }
#else
    volatile unsigned char *fifo = (volatile unsigned char *)USBH_fifo_reg(ep_index);
    unsigned short i;
    for(i = 0u; i < len; i++)
    {
        *fifo = src[i];
    }
#endif
}

static void USBH_fifo_read8(unsigned char ep_index, unsigned char *dst, unsigned short len)
{
#if USBH_FAST_FIFO_32BIT
    volatile unsigned int *fifo32 = USBH_fifo_reg(ep_index);
    volatile unsigned char *fifo8 = (volatile unsigned char *)fifo32;
    unsigned short i = 0u;
    unsigned int w;

    while((unsigned short)(i + 4u) <= len)
    {
        w=*fifo32;
        dst[i + 0u]=(unsigned char)(w & 0xFFu);
        dst[i + 1u]=(unsigned char)((w >> 8) & 0xFFu);
        dst[i + 2u]=(unsigned char)((w >> 16) & 0xFFu);
        dst[i + 3u]=(unsigned char)((w >> 24) & 0xFFu);
        i=(unsigned short)(i + 4u);
    }

    while(i<len)
    {
        dst[i]=*(fifo8 + (i & 3u));
        i++;
    }
#else
    volatile unsigned char *fifo = (volatile unsigned char *)USBH_fifo_reg(ep_index);
    unsigned short i;
    for(i = 0u; i < len; i++)
    {
        dst[i] = *(fifo + (i & 3u));
    }
#endif
}

static volatile unsigned char *USBH_ep0csr_lo_reg(void)
{
    return ((volatile unsigned char *)&USBE0CSR0) + 2;
}

static volatile unsigned char *USBH_ep0csr_hi_reg(void)
{
    return ((volatile unsigned char *)&USBE0CSR0) + 3;
}

static void USBH_ep0_snapshot(void)
{
    unsigned char lo = *USBH_ep0csr_lo_reg();
    unsigned char hi = *USBH_ep0csr_hi_reg();
    g_usbh.last_ep0csr = (((unsigned int)hi) << 24) | (((unsigned int)lo) << 16);
}

static void USBH_ep0_issue(unsigned char lo_bits)
{
    *USBH_ep0csr_lo_reg() = lo_bits;
    USBH_ep0_snapshot();
}

static void USBH_ep0_clear_irq(void)
{
    USBCSR0bits.EP0IF = 0;
    g_usbh.last_usbcsr0 = USBCSR0;
}

static void USBH_ep0_clear_faults(void)
{
    unsigned char lo = *USBH_ep0csr_lo_reg();
    unsigned char clr = 0u;

    if(lo & EP0CSR_LO_NAKTMOUT)
    {
        clr |= EP0CSR_LO_NAKTMOUT;
    }
    if(lo & EP0CSR_LO_RXSTALL)
    {
        clr |= EP0CSR_LO_RXSTALL;
    }
    if(lo & EP0CSR_LO_ERROR)
    {
        clr |= EP0CSR_LO_ERROR;
    }
    if(clr != 0u)
    {
        *USBH_ep0csr_lo_reg() = clr;
        Delay_ms(1);
    }
    USBH_ep0_clear_irq();
    USBH_ep0_snapshot();
}

static unsigned char USBH_ep0_wait_irq(unsigned long timeout_ms, const char *timeout_msg)
{
    while(timeout_ms--)
    {
        unsigned int csr0;
        unsigned char lo;

        lo = *USBH_ep0csr_lo_reg();
        csr0 = USBCSR0;
        g_usbh.last_usbcsr0 = csr0;
        USBH_ep0_snapshot();

        if(lo & EP0CSR_LO_RXSTALL)
        {
            /*
             * Clear the bit immediately.  If we leave it set, every
             * subsequent ep0_wait_irq call sees the stale RXSTALL and
             * returns STALL at once, killing the rest of enumeration.
             * This is the most common failure mode after GET_MAX_LUN
             * (which many drives are allowed to STALL) because the
             * error cascades into INQUIRY, MSC_RESET, etc.
             */
            *USBH_ep0csr_lo_reg() = EP0CSR_LO_RXSTALL;
            USBH_ep0_clear_irq();
            Delay_ms(2u);
            USBH_set_error(USBH_ERR_EP0_STALL, "EP0 STALL");
            return 0u;
        }
        if(lo & EP0CSR_LO_ERROR)
        {
            *USBH_ep0csr_lo_reg() = EP0CSR_LO_ERROR;
            USBH_ep0_clear_irq();
            Delay_ms(2u);
            USBH_set_error(USBH_ERR_EP0_ERROR, "EP0 ERROR");
            return 0u;
        }
        if(lo & EP0CSR_LO_NAKTMOUT)
        {
            *USBH_ep0csr_lo_reg() = EP0CSR_LO_NAKTMOUT;
            USBH_ep0_clear_irq();
            Delay_ms(2u);
            USBH_set_error(USBH_ERR_EP0_TIMEOUT, "EP0 NAKTO");
            return 0u;
        }
        if(csr0 & 0x00010000u)
        {
            USBCSR0bits.EP0IF = 0;
            g_usbh.last_usbcsr0 = USBCSR0;
            return 1u;
        }
        Delay_ms(1);
    }
    USBH_ep0_snapshot();
    USBH_set_error(USBH_ERR_EP0_TIMEOUT, timeout_msg);
    return 0u;
}

static void USBH_show_regs(const char *msg)
{
    if(msg != (const char *)0)
    {
        USBH_dbg_line(8u, msg);
    }
    USBH_dbg_hex32(9u,  "EP0 ", g_usbh.last_ep0csr);
    USBH_dbg_hex32(10u, "TX  ", g_usbh.last_txcsr);
    USBH_dbg_hex32(11u, "RX  ", g_usbh.last_rxcsr);
    USBH_dbg_hex32(12u, "OTG ", g_usbh.last_usbotg);
    USBH_dbg_hex32(13u, "CSR2", g_usbh.last_usbcsr2);
}

static void USBH_set_error(unsigned int err, const char *msg)
{
    g_usbh.last_error = err;
    g_usbh.last_msg = msg;
    USBH_show_regs(msg);
}

/*
 * Maps a USBH_ERR_* code to a short human-readable label.  Used to freeze
 * and display the ROOT CAUSE of a read failure - i.e. the reason the very
 * FIRST attempt broke, captured before recovery/retry logic runs and
 * overwrites g_usbh.last_error / slot 8 with later, less specific messages.
 */
static const char *USBH_err_name(unsigned int err)
{
    switch(err)
    {
        case USBH_ERR_NONE:          return "NONE";
        case USBH_ERR_NO_DEVICE:     return "NO DEVICE";
        case USBH_ERR_RESET_TIMEOUT: return "RESET TIMEOUT";
        case USBH_ERR_EP0_TIMEOUT:   return "EP0 TIMEOUT";
        case USBH_ERR_EP0_STALL:     return "EP0 STALL";
        case USBH_ERR_EP0_ERROR:     return "EP0 ERROR";
        case USBH_ERR_DESC_PARSE:    return "DESC PARSE";
        case USBH_ERR_NO_MSC:        return "NO MSC IF";
        case USBH_ERR_SET_CONFIG:    return "SET CONFIG FAIL";
        case USBH_ERR_BULK_TIMEOUT:  return "BULK TIMEOUT/STALL";
        case USBH_ERR_CSW:           return "CSW ERROR";
        case USBH_ERR_NOT_READY:     return "NOT READY";
        case USBH_ERR_BLOCK_SIZE:    return "BLOCK SIZE";
        case USBH_ERR_READ10:        return "READ10";
        case USBH_ERR_DISCONNECT:    return "DISCONNECT";
        case USBH_ERR_ENUM_FAIL:     return "ENUM FAIL";
        case USBH_ERR_NOT_HS:        return "NOT HS";
        default:                     return "UNKNOWN";
    }
}

static void USBH_phy_soft_reset(void)
{
    volatile unsigned char *raw = ((volatile unsigned char *)&USBE0CSR0) + 0x7Fu;
    *raw = 0x03u;
    Delay_ms(10);
    *raw = 0x00u;
    Delay_ms(10);
}

static void USBH_clear_usb_irqs(void)
{
    g_usbh.last_usbcsr0 = USBCSR0;
    (void)USBCSR1;
    g_usbh.last_usbcsr2 = USBCSR2;
}

static unsigned char USBH_device_connected(void)
{
    unsigned int otg = USBOTG;
    unsigned int csr0 = USBCSR0;
    g_usbh.last_usbotg = otg;
    g_usbh.last_usbcsr0 = csr0;

    if((otg & (USB_OTG_FSDEV | USB_OTG_LSDEV)) != 0u)
    {
        return 1u;
    }
    if(csr0 & USB_CSR0_HSMODE)
    {
        return 1u;
    }
    return 0u;
}

static void USBH_ep0_prepare_pipe(void);

static void USBH_restart_session(unsigned int off_ms, unsigned int on_ms)
{
    USBOTG &= ~(USB_OTG_SESSION | USB_OTG_HOSTREQ);
    g_usbh.last_usbotg = USBOTG;
    Delay_ms(off_ms);
    USBH_phy_soft_reset();
    Delay_ms(10);
    USBOTG |= (USB_OTG_SESSION | USB_OTG_HOSTREQ);
    g_usbh.last_usbotg = USBOTG;
    Delay_ms(on_ms);
}

static unsigned char USBH_wait_hs_mode(unsigned long timeout_ms)
{
    while(timeout_ms--)
    {
        g_usbh.last_usbcsr0 = USBCSR0;
        g_usbh.last_usbcsr2 = USBCSR2;
        g_usbh.last_usbotg = USBOTG;
        if((g_usbh.last_usbcsr0 & USB_CSR0_HSMODE) != 0u)
        {
            return 1u;
        }
        Delay_ms(1);
    }
    return 0u;
}

static unsigned char USBH_wait_connect_and_reset(void)
{
    unsigned long t;
    unsigned int csr2;
    unsigned char hs_try;

    for(t = 0ul; t < 5000ul; t++)
    {
        csr2 = USBCSR2;
        g_usbh.last_usbcsr2 = csr2;
        if((csr2 & USB_CSR2_CONNIF) != 0u)
        {
            USBH_dbg_line(2u, "CONNECT IRQ");
        }
        if(USBH_device_connected())
        {
            break;
        }
        Delay_ms(1);
    }

    if(!USBH_device_connected())
    {
        USBH_set_error(USBH_ERR_NO_DEVICE, "NO USB DEVICE");
        return 0u;
    }

    if((USBOTG & USB_OTG_HOSTMODE) == 0u)
    {
        USBH_dbg_line(4u, "NO HOSTMODE YET");
    }

    for(hs_try = 0u; hs_try < 6u; hs_try++)
    {
        USBH_dbg_u32(3u, "HS RESET ", (unsigned long)(hs_try + 1u));
        USBH_clear_usb_irqs();
        USBH_ep0_clear_faults();

        USBCSR0bits.HSEN = 1;
        Delay_ms(2);
        USBCSR0 |= USB_CSR0_RESET;
        g_usbh.last_usbcsr0 = USBCSR0;
        Delay_ms(65);
        USBCSR0 &= ~USB_CSR0_RESET;
        g_usbh.last_usbcsr0 = USBCSR0;
        Delay_ms(5u); /* brief post-reset settling before sampling HSMODE */

        if(USBH_wait_hs_mode(400u))
        {
            g_usbh.speed_code = USB_HOST_SPEED_HS;
            g_usbh.ep0_mps = 64u;
            USBH_dbg_line(4u, "SPEED HS");
            USBH_ep0_prepare_pipe();
            Delay_ms(40);
            return 1u;
        }

        USBH_dbg_line(4u, "HS CHIRP MISS");
        g_usbh.last_usbotg = USBOTG;
        g_usbh.last_usbcsr0 = USBCSR0;
        g_usbh.last_usbcsr2 = USBCSR2;

        if((USBOTG & USB_OTG_LSDEV) != 0u)
        {
            USBH_dbg_line(5u, "ATTACHED LS");
        }
        else
        {
            USBH_dbg_line(5u, "ATTACHED FS");
        }

        USBH_restart_session(40u, 140u);

        for(t = 0ul; t < 800ul; t++)
        {
            if(USBH_device_connected())
            {
                break;
            }
            Delay_ms(1);
        }
        if(!USBH_device_connected())
        {
            USBH_set_error(USBH_ERR_NO_DEVICE, "NO USB DEVICE");
            return 0u;
        }
    }

    if(USBCSR0 & USB_CSR0_HSMODE)
    {
        g_usbh.speed_code = USB_HOST_SPEED_HS;
        g_usbh.ep0_mps = 64u;
        USBH_dbg_line(4u, "SPEED HS");
    }
    else if(USBOTG & USB_OTG_LSDEV)
    {
        g_usbh.speed_code = USB_HOST_SPEED_LS;
        g_usbh.ep0_mps = 8u;
        USBH_dbg_line(4u, "SPEED LS");
    }
    else
    {
        g_usbh.speed_code = USB_HOST_SPEED_FS;
        g_usbh.ep0_mps = 64u;
        USBH_dbg_line(4u, "SPEED FS");
    }

    USBH_ep0_prepare_pipe();
    Delay_ms((g_usbh.speed_code == USB_HOST_SPEED_HS) ? 40 : 150);

    return 1u;
}

static void USBH_ep0_clear_status(void)
{
    USBH_ep0_clear_faults();
    *USBH_ep0csr_hi_reg() = 0u;
    USBH_ep0_snapshot();
}

static void USBH_ep0_prepare_pipe(void)
{
    USBCSR3bits.ENDPOINT = 0;
    USBFIFOA = 0x00000000u;
    USBCSR0bits.EP0IF = 0;

    USBE0CSR0bits.TXMAXP = g_usbh.ep0_mps;
    USBE0CSR2bits.SPEED = g_usbh.speed_code;
    USBE0CSR2bits.NAKLIM = 15;

    USBE0TXA = (unsigned int)(g_usbh.device_address & 0x7Fu);
    USBE0RXA = (unsigned int)(g_usbh.device_address & 0x7Fu);

    USBH_ep0_clear_status();
    if(g_usbh.speed_code == USB_HOST_SPEED_HS)
    {
        *USBH_ep0csr_hi_reg() = EP0CSR_HI_DISPING;
    }
    Delay_ms(2);
    USBH_ep0_snapshot();
}

static void USBH_setup_pack(const USB_SETUP_PKT *s, unsigned char *pkt)
{
    pkt[0] = s->bmRequestType;
    pkt[1] = s->bRequest;
    pkt[2] = (unsigned char)(s->wValue & 0xFFu);
    pkt[3] = (unsigned char)(s->wValue >> 8);
    pkt[4] = (unsigned char)(s->wIndex & 0xFFu);
    pkt[5] = (unsigned char)(s->wIndex >> 8);
    pkt[6] = (unsigned char)(s->wLength & 0xFFu);
    pkt[7] = (unsigned char)(s->wLength >> 8);
}

static unsigned char USBH_ep0_send_setup(const USB_SETUP_PKT *s)
{
    unsigned char pkt[8];

    USBH_ep0_prepare_pipe();
    USBH_setup_pack(s, pkt);
    USBH_fifo_write8(0u, pkt, 8u);
    USBH_ep0_clear_irq();
    *USBH_ep0csr_lo_reg() = 0x0Au;
    USBH_ep0_snapshot();
    if(!USBH_ep0_wait_irq(1200u, "EP0 TX TIMEOUT"))
    {
        return 0u;
    }
    return 1u;
}

static unsigned short USBH_ctrl_in(const USB_SETUP_PKT *s, unsigned char *dst, unsigned short max_len)
{
    unsigned short total = 0u;
    unsigned short expect = s->wLength;
    unsigned short cnt;
    unsigned char first_req = 1u;
    unsigned char first_mode = 0x60u;

    if(!USBH_ep0_send_setup(s))
    {
        return 0xFFFFu;
    }

    while(total < expect)
    {
        USBH_ep0_clear_faults();
        if(first_req)
        {
            USBH_ep0_issue(first_mode);
            first_req = 0u;
        }
        else
        {
            USBH_ep0_issue(0x20u);
        }

        if(!USBH_ep0_wait_irq(1200u, "EP0 RX TIMEOUT"))
        {
            if((total == 0u) && (first_mode == 0x60u) && (g_usbh.last_error == USBH_ERR_EP0_ERROR))
            {
                USBH_dbg_line(14u, "EP0 ALT REQPKT");
                USBH_ep0_clear_status();
                if(!USBH_ep0_send_setup(s))
                {
                    return 0xFFFFu;
                }
                first_req = 1u;
                first_mode = 0x20u;
                continue;
            }
            return 0xFFFFu;
        }

        cnt = (unsigned short)(USBIE0CSR2 & E0CSR2_RXCNT_MASK);
        if(cnt > (unsigned short)(expect - total))
        {
            cnt = (unsigned short)(expect - total);
        }
        if((unsigned short)(total + cnt) > max_len)
        {
            cnt = (unsigned short)(max_len - total);
        }
        if(cnt != 0u)
        {
            USBH_fifo_read8(0u, &dst[total], cnt);
            total = (unsigned short)(total + cnt);
        }

        USBE0CSR0bits.RXRDYC = 1;
        Delay_ms(1);

        if((cnt == 0u) || (cnt < g_usbh.ep0_mps) || (total >= expect))
        {
            break;
        }
    }

    USBH_ep0_clear_faults();
    USBH_ep0_issue(0x42u);
    if(!USBH_ep0_wait_irq(1200u, "EP0 STATUS TIMEOUT"))
    {
        return 0xFFFFu;
    }
    USBH_ep0_issue(0u);
    return total;
}

static unsigned char USBH_ctrl_no_data(const USB_SETUP_PKT *s)
{
    if(!USBH_ep0_send_setup(s))
    {
        return 0u;
    }

    USBH_ep0_clear_irq();
    USBH_ep0_issue(0x60u);
    if(!USBH_ep0_wait_irq(1200u, "EP0 STATUS TIMEOUT"))
    {
        return 0u;
    }
    USBH_ep0_issue(0u);
    return 1u;
}

static unsigned char USBH_get_descriptor(unsigned char dtype, unsigned char dindex, unsigned char *dst, unsigned short len)
{
    USB_SETUP_PKT s;
    unsigned short got;
    unsigned char retry;
    s.bmRequestType = USB_RT_DEV_IN;
    s.bRequest = USB_REQ_GET_DESCRIPTOR;
    s.wValue = (unsigned short)(((unsigned short)dtype << 8) | dindex);
    s.wIndex = 0u;
    s.wLength = len;

    for(retry = 0u; retry < 4u; retry++)
    {
        got = USBH_ctrl_in(&s, dst, len);
        if(got != 0xFFFFu)
        {
            return 1u;
        }
        USBH_dbg_line(14u, "EP0 RETRY DESC");
        if(retry == 1u)
        {
            USBCSR0 |= USB_CSR0_RESET;
            Delay_ms(80);
            USBCSR0 &= ~USB_CSR0_RESET;
            Delay_ms(120);
        }
        USBH_ep0_prepare_pipe();
        Delay_ms(40);
    }
    return 0u;
}

static unsigned char USBH_set_address(unsigned char addr)
{
    USB_SETUP_PKT s;
    s.bmRequestType = USB_RT_DEV_OUT;
    s.bRequest = USB_REQ_SET_ADDRESS;
    s.wValue = addr;
    s.wIndex = 0u;
    s.wLength = 0u;
    if(!USBH_ctrl_no_data(&s))
    {
        return 0u;
    }
    Delay_ms(20);
    g_usbh.device_address = addr;
    USBH_ep0_prepare_pipe();
    return 1u;
}

static unsigned char USBH_set_configuration(unsigned char cfg)
{
    USB_SETUP_PKT s;
    s.bmRequestType = USB_RT_DEV_OUT;
    s.bRequest = USB_REQ_SET_CONFIGURATION;
    s.wValue = cfg;
    s.wIndex = 0u;
    s.wLength = 0u;
    return USBH_ctrl_no_data(&s);
}

static unsigned char USBH_clear_halt(unsigned char ep_addr)
{
    USB_SETUP_PKT s;
    s.bmRequestType = (ep_addr & USB_EP_DIR_IN) ? USB_RT_STD_EP_IN : USB_RT_STD_EP_OUT;
    s.bRequest = USB_REQ_CLEAR_FEATURE;
    s.wValue = 0u;
    s.wIndex = ep_addr;
    s.wLength = 0u;
    return USBH_ctrl_no_data(&s);
}

static unsigned char USBH_get_max_lun(unsigned char *max_lun)
{
    USB_SETUP_PKT s;
    unsigned char lun = 0u;
    unsigned short got;
    s.bmRequestType = 0xA1u;
    s.bRequest = MSC_REQ_GET_MAX_LUN;
    s.wValue = 0u;
    s.wIndex = g_usbh.interface_number;
    s.wLength = 1u;
    got = USBH_ctrl_in(&s, &lun, 1u);
    if(got == 1u)
    {
        *max_lun = lun;
        return 1u;
    }
    *max_lun = 0u;
    return 0u;
}

static unsigned char USBH_parse_config(const unsigned char *cfg, unsigned short total_len, USB_MSC_INTF *out)
{
    unsigned short i = 0u;
    unsigned char in_msc_if = 0u;

    memset(out, 0, sizeof(*out));

    while((unsigned short)(i + 2u) <= total_len)
    {
        unsigned char len = cfg[i + 0u];
        unsigned char type = cfg[i + 1u];
        if(len == 0u)
        {
            break;
        }
        if((unsigned short)(i + len) > total_len)
        {
            break;
        }

        if((type == 4u) && (len >= 9u))
        {
            if((cfg[i + 5u] == MSC_CLASS) &&
               (cfg[i + 6u] == MSC_SUBCLASS_SCSI) &&
               (cfg[i + 7u] == MSC_PROTO_BOT))
            {
                in_msc_if = 1u;
                out->interface_number = cfg[i + 2u];
            }
            else
            {
                in_msc_if = 0u;
            }
        }
        else if((type == 5u) && (len >= 7u) && in_msc_if)
        {
            unsigned char ep = cfg[i + 2u];
            unsigned short mps = USBH_le16(&cfg[i + 4u]);
            if((cfg[i + 3u] & 0x03u) == 2u)
            {
                if(ep & USB_EP_DIR_IN)
                {
                    out->bulk_in_addr = ep;
                    out->bulk_in_mps = mps;
                }
                else
                {
                    out->bulk_out_addr = ep;
                    out->bulk_out_mps = mps;
                }
            }
        }

        i = (unsigned short)(i + len);
    }

    if((out->bulk_in_addr != 0u) && (out->bulk_out_addr != 0u))
    {
        out->valid = 1u;
        return 1u;
    }
    return 0u;
}

static unsigned char USBH_fifo_size_code(unsigned short mps)
{
    if(mps <= 8u) return 0u;
    if(mps <= 16u) return 1u;
    if(mps <= 32u) return 2u;
    if(mps <= 64u) return 3u;
    if(mps <= 128u) return 4u;
    if(mps <= 256u) return 5u;
    if(mps <= 512u) return 6u;
    return 7u;
}

static unsigned short USBH_fifo_bytes_from_code(unsigned char code)
{
    return (unsigned short)(8u << code);
}

static void USBH_configure_bulk_pipe(void)
{
    unsigned char tx_sz_code;
    unsigned char rx_sz_code;
    unsigned short tx_bytes;
    unsigned short rx_bytes;
    unsigned short tx_addr_units;
    unsigned short rx_addr_units;

    USBH_select_ep(1u);

    tx_sz_code = USBH_fifo_size_code(g_usbh.bulk_out_mps);
    rx_sz_code = USBH_fifo_size_code(g_usbh.bulk_in_mps);
    tx_bytes = USBH_fifo_bytes_from_code(tx_sz_code);
    rx_bytes = USBH_fifo_bytes_from_code(rx_sz_code);

    /* FIFO0 occupies the first 64 bytes. Endpoint 1 TX starts at 0x40 and
       endpoint 1 RX follows immediately after TX so the FIFOs do not overlap. */
    tx_addr_units = 8u;
    rx_addr_units = (unsigned short)(tx_addr_units + (tx_bytes >> 3));

    USBOTGbits.TXFIFOSZ = tx_sz_code;
    USBOTGbits.RXFIFOSZ = rx_sz_code;
    USBFIFOA = (((unsigned int)rx_addr_units) << 16) | (unsigned int)tx_addr_units;

    USBIENCSR0bits.MODE = 1; /* host OUT uses the TX side */
    USBIENCSR0bits.TXMAXP = g_usbh.bulk_out_mps;
    USBIENCSR1bits.RXMAXP = g_usbh.bulk_in_mps;
    USBIENCSR0bits.CLRDT = 1;
    USBIENCSR1bits.CLRDT = 1;
    USBIENCSR1bits.PIDERR = 0;

    USBIENCSR2bits.TXINTERV = 1;
    USBIENCSR2bits.SPEED = g_usbh.speed_code;
    USBIENCSR2bits.PROTOCOL = 2;
    USBIENCSR2bits.TEP = (g_usbh.bulk_out_addr & USB_EP_ADDR_MASK);

    USBIENCSR3bits.RXINTERV = 1;
    USBIENCSR3bits.SPEED = g_usbh.speed_code;
    USBIENCSR3bits.PROTOCOL = 2;
    USBIENCSR3bits.TEP = (g_usbh.bulk_in_addr & USB_EP_ADDR_MASK);

    USBE1TXAbits.TXFADDR = (g_usbh.device_address & 0x7Fu);
    USBE1TXAbits.TXHUBADD = 0;
    USBE1TXAbits.TXHUBPRT = 0;
    USBE1RXAbits.RXFADDR = (g_usbh.device_address & 0x7Fu);
    USBE1RXAbits.RXHUBADD = 0;
    USBE1RXAbits.RXHUBPRT = 0;

    USBH_dbg_u32(7u, "TX FIFO ", tx_bytes);
    USBH_dbg_u32(8u, "RX FIFO ", rx_bytes);
    USBH_dbg_line(9u, "BULK PIPE READY");
}

static unsigned char USBH_bulk_out(const unsigned char *src, unsigned short len)
{
    while(len)
    {
        unsigned short pkt = USBH_min16(len, g_usbh.bulk_out_mps);
        unsigned int csr;
        unsigned long timeout;
        unsigned int nak_count = 0u;
        unsigned char out_done = 0u;

        USBH_select_ep(1u);
        USBIENCSR0bits.MODE = 1;
        USBIENCSR0 &= ~(TXCSR0_NAKTMOUT | TXCSR0_RXSTALL | TXCSR0_ERROR);
        if(USBIENCSR0 & TXCSR0_FIFONE)
        {
            USBIENCSR0 |= TXCSR0_FLUSH;
            USBIENCSR0 |= TXCSR0_FLUSH;
        }
        Delay_ms(1);

        USBH_fifo_write8(1u, src, pkt);
        USBIENCSR0 |= TXCSR0_TXPKTRDY;

        for(timeout = 0ul; timeout < 4000ul; timeout++)
        {
            csr = USBIENCSR0;
            g_usbh.last_txcsr = csr;
            if(csr & TXCSR0_RXSTALL)
            {
                USBIENCSR0 &= ~TXCSR0_RXSTALL;
                USBH_bulk_ep_stall_recover_out();
                USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK OUT STALL");
                return 0u;
            }
            if(csr & TXCSR0_ERROR)
            {
                USBIENCSR0 &= ~TXCSR0_ERROR;
                USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK OUT ERROR");
                return 0u;
            }
            if((csr & TXCSR0_TXPKTRDY) == 0u)
            {
                out_done = 1u;
                break;
            }
            if(csr & TXCSR0_NAKTMOUT)
            {
                USBIENCSR0 &= ~TXCSR0_NAKTMOUT;
                nak_count++;
                if(nak_count >= 250u)
                {
                    USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK OUT NAKTO");
                    return 0u;
                }
                Delay_ms(5u);
            }
            else
            {
                /*
                 * Two-phase wait: the first 2000 iterations spin with no
                 * sleep at all (a compliant HS device responds within
                 * microseconds, so this costs almost no real time and keeps
                 * the common case fast).  If a device still hasn't responded
                 * after that, switch to a real 1 ms sleep every iteration
                 * for the remaining budget - restoring genuine multi-second
                 * patience for drives with occasional internal latency
                 * spikes that don't assert NAKTMOUT, instead of timing out
                 * after only a few milliseconds.
                 */
                if(timeout >= 2000ul)
                {
                    Delay_ms(1u);
                }
            }
        }
        if(!out_done)
        {
            USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK OUT TIMEOUT");
            return 0u;
        }

        src += pkt;
        len = (unsigned short)(len - pkt);
    }
    return 1u;
}

static unsigned char USBH_bulk_in(unsigned char *dst, unsigned short expect_len, unsigned short *actual_len, unsigned char short_ok)
{
    unsigned short total = 0u;

    while(total < expect_len)
    {
        unsigned int csr;
        unsigned short cnt;
        unsigned long timeout;
        unsigned int nak_count = 0u;
        unsigned char in_done = 0u;

        USBH_select_ep(1u);
        USBIENCSR0bits.MODE = 0;
        USBIENCSR1 &= ~(RXCSR1_ERROR | RXCSR1_RXSTALL | RXCSR1_DERRNAKT | RXCSR1_RXPKTRDY);
        USBIENCSR1 |= RXCSR1_REQPKT;

        for(timeout = 0ul; timeout < 4000ul; timeout++)
        {
            csr = USBIENCSR1;
            g_usbh.last_rxcsr = csr;
            if(csr & RXCSR1_RXSTALL)
            {
                USBIENCSR1 &= ~RXCSR1_RXSTALL;
                USBH_bulk_ep_stall_recover_in();
                USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK IN STALL");
                return 0u;
            }
            if(csr & RXCSR1_ERROR)
            {
                USBIENCSR1 &= ~RXCSR1_ERROR;
                USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK IN ERROR");
                return 0u;
            }
            if(csr & RXCSR1_RXPKTRDY)
            {
                in_done = 1u;
                break;
            }
            if(csr & RXCSR1_DERRNAKT)
            {
                USBIENCSR1 &= ~RXCSR1_DERRNAKT;
                nak_count++;
                if(nak_count >= 250u)
                {
                    USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK IN NAKTO");
                    return 0u;
                }
                USBIENCSR1 |= RXCSR1_REQPKT;
                Delay_ms(5u);
            }
            else
            {
                if(timeout >= 2000ul)
                {
                    Delay_ms(1u);
                }
            }
        }
        if(!in_done)
        {
            USBH_set_error(USBH_ERR_BULK_TIMEOUT, "BULK IN TIMEOUT");
            return 0u;
        }

        cnt = (unsigned short)(USBIENCSR2 & TXCSR2_RXCNT_MASK);
        if((unsigned short)(total + cnt) > expect_len)
        {
            cnt = (unsigned short)(expect_len - total);
        }
        USBH_fifo_read8(1u, &dst[total], cnt);
        total = (unsigned short)(total + cnt);
        USBIENCSR1 &= ~RXCSR1_RXPKTRDY;

        if(cnt < g_usbh.bulk_in_mps)
        {
            break;
        }
    }

    *actual_len = total;
    if(short_ok)
    {
        return 1u;
    }
    return (total == expect_len) ? 1u : 0u;
}

static unsigned char USBH_msc_command(const unsigned char *cdb, unsigned char cdb_len, unsigned char dir_in,
                                      unsigned char *data, unsigned long xfer_len)
{
    unsigned char cbw[31];
    unsigned char csw[13];
    unsigned short got;
    unsigned long tag = g_usbh.cbw_tag++;

    memset(cbw, 0, sizeof(cbw));
    cbw[0] = 0x55u;
    cbw[1] = 0x53u;
    cbw[2] = 0x42u;
    cbw[3] = 0x43u;
    cbw[4] = (unsigned char)(tag & 0xFFu);
    cbw[5] = (unsigned char)((tag >> 8) & 0xFFu);
    cbw[6] = (unsigned char)((tag >> 16) & 0xFFu);
    cbw[7] = (unsigned char)((tag >> 24) & 0xFFu);
    cbw[8] = (unsigned char)(xfer_len & 0xFFu);
    cbw[9] = (unsigned char)((xfer_len >> 8) & 0xFFu);
    cbw[10] = (unsigned char)((xfer_len >> 16) & 0xFFu);
    cbw[11] = (unsigned char)((xfer_len >> 24) & 0xFFu);
    cbw[12] = dir_in ? 0x80u : 0x00u;
    cbw[13] = 0u;
    cbw[14] = cdb_len;
    memcpy(&cbw[15], cdb, cdb_len);

    if(!USBH_bulk_out(cbw, (unsigned short)sizeof(cbw)))
    {
        return 0u;
    }

    if(xfer_len != 0ul)
    {
        if(dir_in)
        {
            if(!USBH_bulk_in(data, (unsigned short)xfer_len, &got, 0u))
            {
                return 0u;
            }
            /* ZLP handling is done reactively in the CSW read below. */
        }
        else
        {
            if(!USBH_bulk_out(data, (unsigned short)xfer_len))
            {
                return 0u;
            }
        }
    }

    if(!USBH_bulk_in(csw, (unsigned short)sizeof(csw), &got, 0u))
    {
        /*
         * got == 0: We received a ZLP instead of the CSW.  This happens when
         * the drive sends the ZLP AFTER the data phase completes but BEFORE
         * we issue the CSW REQPKT (race condition on slow drives).  Retry
         * the CSW read exactly once.
         */
        if(got == 0u)
        {
            if(!USBH_bulk_in(csw, (unsigned short)sizeof(csw), &got, 0u))
            {
                return 0u;
            }
        }
        else
        {
            return 0u;
        }
    }
    if(got != sizeof(csw))
    {
        USBH_set_error(USBH_ERR_CSW, "CSW LEN");
        return 0u;
    }
    if(USBH_le32(&csw[0]) != MSC_CSW_SIG)
    {
        USBH_set_error(USBH_ERR_CSW, "CSW SIG");
        return 0u;
    }
    if(USBH_le32(&csw[4]) != tag)
    {
        USBH_set_error(USBH_ERR_CSW, "CSW TAG");
        return 0u;
    }
    if(csw[12] != 0u)
    {
        /*
         * Status 1 = Command Failed (the SCSI command itself was rejected;
         *            sense data via REQUEST SENSE explains why).
         * Status 2 = Phase Error    (transport got out of sync; only a
         *            full Reset Recovery fixes this, sense data is not
         *            meaningful here).
         * Slot 19 is dedicated and never reused, so this survives on
         * screen past any later show_regs() call.
         */
        USBH_dbg_hex8(19u, "CSW STATUS ", csw[12]);
        USBH_set_error(USBH_ERR_CSW, "CSW STATUS");
        return 0u;
    }
    return 1u;
}

static unsigned char USBH_msc_request_sense(void)
{
    unsigned char cdb[6];
    unsigned char rsp[18];
    memset(cdb, 0, sizeof(cdb));
    memset(rsp, 0, sizeof(rsp));
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = sizeof(rsp);
    if(!USBH_msc_command(cdb, 6u, 1u, rsp, sizeof(rsp)))
    {
        return 0u;
    }
    g_usbh.sense_key = (unsigned char)(rsp[2] & 0x0Fu);
    g_usbh.sense_asc = rsp[12];
    g_usbh.sense_ascq = rsp[13];
    /*
     * Slots 8/9/10 are reused moments later by USBH_show_regs() (called from
     * USBH_set_error on the next failure), which silently erases this data
     * before it can ever be read off-screen.  Dedicated slots 16-18 are not
     * touched by anything else, so the actual SCSI failure reason survives.
     */
    USBH_dbg_hex8(16u, "SENSE KEY ", g_usbh.sense_key);
    USBH_dbg_hex8(17u, "SENSE ASC ", g_usbh.sense_asc);
    USBH_dbg_hex8(18u, "SENSE ASCQ ", g_usbh.sense_ascq);
    return 1u;
}

static unsigned char USBH_msc_test_unit_ready(void)
{
    unsigned char cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_TEST_UNIT_READY;
    return USBH_msc_command(cdb, 6u, 0u, (unsigned char *)0, 0ul);
}

static unsigned char USBH_msc_inquiry(void)
{
    unsigned char cdb[6];
    unsigned char rsp[36];
    unsigned char i;
    memset(cdb, 0, sizeof(cdb));
    memset(rsp, 0, sizeof(rsp));
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = sizeof(rsp);
    if(!USBH_msc_command(cdb, 6u, 1u, rsp, sizeof(rsp)))
    {
        return 0u;
    }
    for(i = 0u; i < 8u; i++)
    {
        g_usbh.vendor[i] = (char)rsp[8u + i];
    }
    g_usbh.vendor[8] = 0;
    for(i = 0u; i < 16u; i++)
    {
        g_usbh.product[i] = (char)rsp[16u + i];
    }
    g_usbh.product[16] = 0;
    USBH_dbg_line(5u, g_usbh.vendor);
    USBH_dbg_line(6u, g_usbh.product);
    return 1u;
}

static unsigned char USBH_msc_read_capacity10(void)
{
    unsigned char cdb[10];
    unsigned char rsp[8];
    unsigned long last_lba;
    memset(cdb, 0, sizeof(cdb));
    memset(rsp, 0, sizeof(rsp));
    cdb[0] = SCSI_READ_CAPACITY_10;
    if(!USBH_msc_command(cdb, 10u, 1u, rsp, sizeof(rsp)))
    {
        return 0u;
    }
    last_lba = USBH_be32(&rsp[0]);
    g_usbh.block_count = last_lba + 1ul;
    g_usbh.block_size = USBH_be32(&rsp[4]);
    USBH_dbg_u32(7u, "BLOCKS ", g_usbh.block_count);
    USBH_dbg_u32(8u, "BLKSZ ", g_usbh.block_size);
    return 1u;
}

static unsigned char USBH_msc_reset_recovery(void)
{
    USB_SETUP_PKT s;
    unsigned char in_ok;
    unsigned char out_ok;

    /*
     * Bulk-Only Mass Storage Reset Recovery (USB BOT spec 5.3.4) is three
     * steps: (a) class-specific Mass Storage Reset, (b) Clear Feature HALT
     * on Bulk-In, (c) Clear Feature HALT on Bulk-Out.
     *
     * Step (a) is frequently STALLed by newer/faster pendrive bridge
     * controllers - some simply don't implement it, or use STALL as a
     * "not supported" response.  That is a valid response, NOT a fatal
     * error.  The old code returned immediately when step (a) failed,
     * which left both bulk endpoints halted forever: every later read
     * would fail instantly with the pipe still stuck, looking exactly
     * like "EP0 STALL then permanent READ10 failure".  Steps (b) and (c)
     * must always run regardless of what happened in step (a), because
     * they are what actually un-wedges the data pipes.
     */
    s.bmRequestType = USB_RT_CLASS_IF_OUT;
    s.bRequest = MSC_REQ_RESET;
    s.wValue = 0u;
    s.wIndex = g_usbh.interface_number;
    s.wLength = 0u;
    (void)USBH_ctrl_no_data(&s); /* result intentionally ignored - see above */
    Delay_ms(2);

    in_ok  = USBH_clear_halt(g_usbh.bulk_in_addr);
    out_ok = USBH_clear_halt(g_usbh.bulk_out_addr);

    USBH_select_ep(1u);
    USBIENCSR0 |= TXCSR0_CLRDT;
    USBIENCSR1 |= RXCSR1_CLRDT;
    Delay_ms(2);

    return (unsigned char)(in_ok && out_ok);
}

/* Recover a stalled bulk IN endpoint: send CLEAR_FEATURE and reset toggle. */
static void USBH_bulk_ep_stall_recover_in(void)
{
    USBH_select_ep(1u);
    USBIENCSR1 |= RXCSR1_CLRDT;
    (void)USBH_clear_halt(g_usbh.bulk_in_addr);
    Delay_ms(5u);
}

/* Recover a stalled bulk OUT endpoint: send CLEAR_FEATURE and reset toggle. */
static void USBH_bulk_ep_stall_recover_out(void)
{
    USBH_select_ep(1u);
    USBIENCSR0 |= TXCSR0_CLRDT;
    (void)USBH_clear_halt(g_usbh.bulk_out_addr);
    Delay_ms(5u);
}

static unsigned char USBH_msc_read10(unsigned long lba, unsigned char blocks, unsigned char *dst)
{
    unsigned char cdb[10];
    unsigned long bytes = (unsigned long)blocks * g_usbh.block_size;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_10;
    cdb[2] = (unsigned char)((lba >> 24) & 0xFFu);
    cdb[3] = (unsigned char)((lba >> 16) & 0xFFu);
    cdb[4] = (unsigned char)((lba >> 8) & 0xFFu);
    cdb[5] = (unsigned char)(lba & 0xFFu);
    cdb[7] = 0u;
    cdb[8] = blocks;
    return USBH_msc_command(cdb, 10u, 1u, dst, bytes);
}

static unsigned char USBH_enumerate_msc(void)
{
    unsigned char dev8[8];
    unsigned char dev18[18];
    unsigned char cfg9[9];
    unsigned char cfg[256];
    unsigned short cfg_total;
    unsigned long retry;
    USB_MSC_INTF intf;

    memset(dev8, 0, sizeof(dev8));
    memset(dev18, 0, sizeof(dev18));
    memset(cfg9, 0, sizeof(cfg9));
    memset(cfg, 0, sizeof(cfg));

    if(g_usbh.speed_code == USB_HOST_SPEED_HS)
    {
        USBH_dbg_line(5u, "EP0 DEV DESC18");
        if(!USBH_get_descriptor(USB_DT_DEVICE, 0u, dev18, 18u))
        {
            return 0u;
        }
        memcpy(dev8, dev18, sizeof(dev8));
        g_usbh.ep0_mps = 64u;
        USBH_ep0_prepare_pipe();
    }
    else
    {
        USBH_dbg_line(5u, "EP0 DEV DESC");
        if(!USBH_get_descriptor(USB_DT_DEVICE, 0u, dev8, 8u))
        {
            return 0u;
        }

        if((dev8[7] == 8u) || (dev8[7] == 16u) || (dev8[7] == 32u) || (dev8[7] == 64u))
        {
            g_usbh.ep0_mps = dev8[7];
            USBH_ep0_prepare_pipe();
        }
    }

    USBH_dbg_line(5u, "SET ADDRESS 1");
    if(!USBH_set_address(1u))
    {
        return 0u;
    }

    USBH_dbg_line(5u, "EP0 CFG DESC9");
    if(!USBH_get_descriptor(USB_DT_CONFIG, 0u, cfg9, 9u))
    {
        return 0u;
    }

    cfg_total = USBH_le16(&cfg9[2]);
    if(cfg_total > sizeof(cfg))
    {
        cfg_total = sizeof(cfg);
    }

    USBH_dbg_line(5u, "EP0 CFG FULL");
    if(!USBH_get_descriptor(USB_DT_CONFIG, 0u, cfg, cfg_total))
    {
        return 0u;
    }

    if(!USBH_parse_config(cfg, cfg_total, &intf))
    {
        USBH_set_error(USBH_ERR_NO_MSC, "NO MSC IF");
        return 0u;
    }

    g_usbh.interface_number = intf.interface_number;
    g_usbh.bulk_in_addr = intf.bulk_in_addr;
    g_usbh.bulk_out_addr = intf.bulk_out_addr;
    g_usbh.bulk_in_mps = intf.bulk_in_mps;
    g_usbh.bulk_out_mps = intf.bulk_out_mps;

    USBH_dbg_hex8(6u, "BULK IN  ", g_usbh.bulk_in_addr);
    USBH_dbg_hex8(7u, "BULK OUT ", g_usbh.bulk_out_addr);
    USBH_dbg_u32(8u, "OUT MPS ", g_usbh.bulk_out_mps);
    USBH_dbg_u32(9u, "IN MPS ", g_usbh.bulk_in_mps);

    if(!USBH_set_configuration(cfg[5u]))
    {
        USBH_set_error(USBH_ERR_SET_CONFIG, "SET CONFIG FAIL");
        return 0u;
    }

    /*
     * Some newer drives (especially those with hardware encryption or
     * USB 3.x SuperSpeed controllers in HS fallback mode) need longer
     * to complete internal SET_CONFIGURATION processing before they will
     * respond to class requests.  800 ms is conservative but reliable.
     */
    Delay_ms(800);
    (void)USBH_get_max_lun(&g_usbh.max_lun);
    Delay_ms(50);
    USBH_configure_bulk_pipe();
    Delay_ms(50);
    (void)USBH_msc_reset_recovery();
    Delay_ms(50);

    if(!USBH_msc_inquiry())
    {
        (void)USBH_msc_reset_recovery();
        if(!USBH_msc_inquiry())
        {
            return 0u;
        }
    }

    for(retry = 0ul; retry < 60ul; retry++)
    {
        if(USBH_msc_test_unit_ready())
        {
            break;
        }
        (void)USBH_msc_request_sense();
        Delay_ms(100);
    }
    if(retry >= 40ul)
    {
        USBH_set_error(USBH_ERR_NOT_READY, "TUR FAIL");
        return 0u;
    }

    if(!USBH_msc_read_capacity10())
    {
        return 0u;
    }

    if(g_usbh.block_size != 512ul)
    {
        USBH_set_error(USBH_ERR_BLOCK_SIZE, "BLKSZ != 512");
        return 0u;
    }

    g_usbh.ready = 1u;
    g_usbh.configured = 1u;
    g_msc_force_single_block = 0u; /* give this drive a fresh chance at multi-block speed */
    USBH_dbg_line(9u, "USB MSC READY");
    return 1u;
}

static void USBH_hw_init(void)
{
    memset(&g_usbh, 0, sizeof(g_usbh));
    g_usbh.device_address = 0u;
    g_usbh.cbw_tag = 1ul;
    g_usbh.ep0_mps = 8u;

    USBH_dbg_line(0u, "USB HOST HS FIX2");
    USBH_dbg_line(1u, "VBUS ALWAYS ON");

    CFGCONbits.USBSSEN = 0;
    CNPDFbits.CNPDF3 = 1;
    TRISFbits.TRISF3 = 0;
    LATFbits.LATF3 = 0;

    USBCRCONbits.USBIE = 1;
    *((unsigned char*)&USBEOFRST + 0x3) = 0x03u;
    Delay_ms(100);
    *((unsigned char*)&USBEOFRST + 0x3) = 0x00u;
    Delay_ms(20);

    USBCSR2bits.SESSRQIE = 1;
    USBCSR2bits.CONNIE = 1;
    USBCSR2bits.RESETIE = 1;
    USBCSR2bits.DISCONIE = 1;
    USBCSR2bits.VBUSERRIE = 0;
    USBCSR2bits.EP1RXIE = 1;
    USBCSR1bits.EP1TXIE = 1;

    USBCRCONbits.VBUSMONEN = 0;
    USBCRCONbits.ASVALMONEN = 0;
    USBCRCONbits.BSVALMONEN = 0;
    USBCRCONbits.SENDMONEN = 0;
    USBCRCONbits.USBIDOVEN = 1;
    USBCRCONbits.PHYIDEN = 1;
    USBCRCONbits.USBIDVAL = 0;
    g_usbh.last_usbcrcon = USBCRCON;

    USBOTGbits.HOSTMODE = 1;
    USBCSR0bits.HSEN = 1;

    USBH_clear_usb_irqs();

    USBOTG &= ~(USB_OTG_SESSION | USB_OTG_HOSTREQ);
    Delay_ms(20);
    USBOTG |= (USB_OTG_SESSION | USB_OTG_HOSTREQ);
    g_usbh.last_usbotg = USBOTG;
    Delay_ms(80);

    if((USBOTG & USB_OTG_HOSTMODE) != 0u)
    {
        USBH_dbg_line(2u, "HOST MODE OK");
    }
    else
    {
        USBH_dbg_line(2u, "HOST MODE WAIT");
    }
    USBH_dbg_line(3u, "WAIT FOR DEVICE");
}

void USBH_Service(void)
{
    if(g_usbh.ready)
    {
        if(!USBH_device_connected())
        {
            g_usbh.ready = 0u;
            g_usbh.configured = 0u;
            g_msc_force_single_block = 0u;
            USBH_set_error(USBH_ERR_DISCONNECT, "USB DISCONNECTED");
        }
    }
}

unsigned char USBH_IsReady(void)
{
    return g_usbh.ready;
}

unsigned int USBH_LastError(void)
{
    return g_usbh.last_error;
}

/*
 * USB_WaitReconnect
 * -----------------
 * Call this whenever you detect g_usbh.ready == 0 (drive pulled out, or a
 * read failure that left the drive unusable).
 *
 * It:
 *   1. Opens the boot console (BOOT.h).
 *   2. Shows the drive as REMOVED and waits until it is physically gone
 *      (important: if the drive is still connected but just errored, we do a
 *      short wait then proceed anyway so we don't hang forever).
 *   3. Loops calling SDcard_init() until the drive mounts successfully.
 *   4. Returns once the drive is ready - the caller can then reinitialise
 *      the filesystem and resume normal operation.
 *
 * The caller closes the console with Boot_End() once it has remounted the
 * filesystem.  BOOT.h is included before this header in newmain.c.
 */
void SDcard_init(void);      /* defined below; called by the retry loop */

void USB_WaitReconnect(void)
{
    unsigned long t;

    

    Boot_Begin("USB RECONNECT");
    Boot_Set(0u, "DRIVE", "REMOVED", BOOT_FAIL);
    Boot_Detail("waiting for the drive to leave the bus");

    /* Wait up to 2 s for the drive to physically disappear from the bus.
       Skip the wait if it is already gone so we do not add unnecessary delay
       when the drive errored out but is still electrically connected. */
    for(t = 0ul; t < 2000ul; t++)
    {
        if(!USBH_device_connected())
        {
            break;
        }
        Delay_ms(1u);
    }

    /* Reconnect loop - keeps retrying until SDcard_init succeeds. */
    Boot_Set(0u, "DRIVE", "WAITING", BOOT_WARN);
    Boot_Detail("insert the drive to continue");

    while(!g_usbh.ready)
    {
        SDcard_init();
        if(!g_usbh.ready)
        {
            Delay_ms(500u);   /* brief pause before next attempt */
        }
    }

    Boot_Set(0u, "DRIVE", "READY", BOOT_OK);
}

void SDcard_init(void)
{
    unsigned char attempt;

    for(attempt = 0u; attempt < 6u; attempt++)
    {
        USBH_hw_init();
        USBH_dbg_u32(4u, "HS TRY ", (unsigned long)(attempt + 1u));

        if(!USBH_wait_connect_and_reset())
        {
            return;
        }

        if(g_usbh.speed_code != USB_HOST_SPEED_HS)
        {
            USBH_set_error(USBH_ERR_NOT_HS, "NOT HIGH SPEED");
            USBH_restart_session(60u, 200u);
            Delay_ms(400u);
            continue;
        }

        if(USBH_enumerate_msc())
        {
            return;
        }

        if(g_usbh.last_error == USBH_ERR_NONE)
        {
            USBH_set_error(USBH_ERR_ENUM_FAIL, "ENUM FAIL");
        }
        else
        {
            USBH_dbg_line(15u, "ENUM RETRY");
        }

        USBOTGbits.SESSION = 0;
        Delay_ms(100u);
        USBH_phy_soft_reset();
        Delay_ms(2000u);
    }

    /*
     * FS FALLBACK
     * -----------
     * All High-Speed attempts failed.  This happens with:
     *   - Full-Speed-only USB 2.0 drives (rare but real)
     *   - USB 3.x drives whose HS chirp is marginal on this PCB
     *   - Drives that need more time between power-on and HS negotiation
     *
     * We now issue a plain USB reset with HSEN=0 so we do not try to
     * negotiate HS at all.  The drive connects at Full Speed (12 Mbps).
     * USB FS is still fast enough for streaming: at FS bulk throughput
     * (~800 KB/s practical) reading 16 KB takes ~20 ms, while SPI output
     * of the same 16 KB at 48 MHz takes ~27 ms, so USB is not the bottleneck.
     */
    USBH_dbg_line(3u, "FS FALLBACK");
    USBH_hw_init();

    /* hw_init sets HSEN=1; override it now before the device attaches. */
    USBCSR0bits.HSEN = 0u;
    Delay_ms(50u); /* let device settle after session start */

    if(!USBH_device_connected())
    {
        /* Give the device up to 1 s to appear. */
        unsigned long t;
        USBH_restart_session(60u, 200u);
        for(t = 0ul; t < 1000ul; t++)
        {
            if(USBH_device_connected()) { break; }
            Delay_ms(1u);
        }
    }

    if(!USBH_device_connected())
    {
        USBH_set_error(USBH_ERR_NO_DEVICE, "NO DEV FS");
        return;
    }

    /* Standard USB reset (SE0) ? no HS chirp. */
    USBCSR0 |= USB_CSR0_RESET;
    g_usbh.last_usbcsr0 = USBCSR0;
    Delay_ms(55u);
    USBCSR0 &= ~USB_CSR0_RESET;
    g_usbh.last_usbcsr0 = USBCSR0;
    Delay_ms(150u); /* post-reset recovery time per USB 2.0 spec */

    if((USBOTG & USB_OTG_LSDEV) != 0u)
    {
        g_usbh.speed_code = USB_HOST_SPEED_LS;
        g_usbh.ep0_mps    = 8u;
        USBH_dbg_line(4u, "FS-FALL: LS");
    }
    else
    {
        g_usbh.speed_code = USB_HOST_SPEED_FS;
        g_usbh.ep0_mps    = 64u;
        USBH_dbg_line(4u, "FS-FALL: FS");
    }

    USBH_ep0_prepare_pipe();
    Delay_ms(40u);

    (void)USBH_enumerate_msc();
}

/* Multi-block capability flag declared earlier, near g_usbh. */

void Read_Buffers(unsigned long sector, unsigned char blocks, unsigned char *buff)
{
    unsigned long total_bytes;
    unsigned char i;

    USBH_Service();

    if(blocks==0u)
    {
        return;
    }

    total_bytes=(unsigned long)blocks * 512ul;

    if(!g_usbh.ready)
    {
        memset(buff, 0, total_bytes);
        return;
    }

    if((blocks==1u) || (g_msc_force_single_block!=0u))
    {
        /* Single-block path: identical to the slow/compatible version.
           Used directly for blocks==1, and for ALL reads on a drive that
           has already shown it cannot do multi-block READ(10). */
        for(i=0u; i<blocks; i++)
        {
            if(!USBH_msc_read10(sector+(unsigned long)i, 1u, buff+((unsigned long)i*512ul)))
            {
                /* Freeze the ROOT CAUSE before recovery/sense overwrite it. */
                const char *root_msg = g_usbh.last_msg;
                (void)USBH_msc_reset_recovery();
                (void)USBH_msc_request_sense();
                USBH_Service();
                if(!g_usbh.ready)
                {
                    memset(buff, 0, total_bytes);
                    return;
                }
                if(!USBH_msc_read10(sector+(unsigned long)i, 1u, buff+((unsigned long)i*512ul)))
                {
                    USBH_set_error(USBH_ERR_READ10, "READ10 SINGLE FAIL");
                    /* Slot 2 is stale (connection-phase only) by the time we
                       are doing steady-state reads, so it is safe and stays
                       visible: this is what actually broke the transfer. */
                    USBH_dbg_line(2u, (root_msg != (const char *)0) ? root_msg : "UNKNOWN");
                    memset(buff+((unsigned long)i*512ul), 0, total_bytes-((unsigned long)i*512ul));
                    return;
                }
            }
        }
        return;
    }

    /* Multi-block attempt: fastest path, used while the drive keeps proving
       it can handle it. */
    if(!USBH_msc_read10(sector, blocks, buff))
    {
        /* Freeze the ROOT CAUSE before recovery/sense overwrite it. */
        const char *multi_root_msg = g_usbh.last_msg;
        (void)USBH_msc_reset_recovery();
        (void)USBH_msc_request_sense();
        USBH_Service();
        if(!g_usbh.ready)
        {
            memset(buff, 0, total_bytes);
            return;
        }

        /* Slot 2 is stale by the time we are doing steady-state reads. */
        USBH_dbg_line(2u, (multi_root_msg != (const char *)0) ? multi_root_msg : "UNKNOWN");

        /*
         * Multi-block failed even after reset/recovery.  This drive does
         * not reliably support multi-block READ(10).  Mark it so every
         * future call - browsing AND streaming - goes straight to the
         * single-block path without wasting time on a doomed retry.
         * Then satisfy THIS request immediately via single-block reads so
         * the caller still gets valid data instead of zeros.
         */
        g_msc_force_single_block = 1u;
        USBH_dbg_line(10u, "MULTI-RD UNSUP: FALLBACK");

        for(i=0u; i<blocks; i++)
        {
            if(!USBH_msc_read10(sector+(unsigned long)i, 1u, buff+((unsigned long)i*512ul)))
            {
                const char *fb_root_msg = g_usbh.last_msg;
                (void)USBH_msc_reset_recovery();
                (void)USBH_msc_request_sense();
                USBH_Service();
                if(!g_usbh.ready)
                {
                    memset(buff, 0, total_bytes);
                    return;
                }
                if(!USBH_msc_read10(sector+(unsigned long)i, 1u, buff+((unsigned long)i*512ul)))
                {
                    USBH_set_error(USBH_ERR_READ10, "READ10 FALLBACK FAIL");
                    USBH_dbg_line(2u, (fb_root_msg != (const char *)0) ? fb_root_msg : "UNKNOWN");
                    memset(buff+((unsigned long)i*512ul), 0, total_bytes-((unsigned long)i*512ul));
                    return;
                }
            }
        }
    }
}

void Read_Buffer(unsigned long sector, unsigned char *buff)
{
    Read_Buffers(sector, 1u, buff);
}

#endif