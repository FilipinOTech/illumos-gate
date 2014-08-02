/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2013 STEC, Inc.  All rights reserved.
 */

#ifndef _SKD_H
#define	_SKD_H

#ifdef __cplusplus
extern "C" {
#endif

#include	<sys/types.h>
#include	<sys/stropts.h>
#include	<sys/stream.h>
#include	<sys/cmn_err.h>
#include	<sys/kmem.h>
#include	<sys/modctl.h>
#include	<sys/ddi.h>
#include	<sys/sunddi.h>
#include	<sys/strsun.h>
#include	<sys/stat.h>
#include	<sys/kstat.h>
#include	<sys/dlpi.h>
#include 	<sys/conf.h>
#include 	<sys/debug.h>
#include 	<sys/vtrace.h>
#include 	<sys/modctl.h>
#include 	<sys/errno.h>
#include 	<sys/pci.h>
#include 	<sys/memlist.h>
#include 	<sys/note.h>
#include 	<sys/param.h>
#include 	<sys/vtoc.h>

#define	DRV_NAME 	"skd"
#define	DRV_VERSION 	"2.2.1"
#define	DRV_BUILD_ID 	"0264"
#define	PFX DRV_NAME    ": "
#define	DRV_BIN_VERSION 0x100
#define	DRV_VER_COMPL   DRV_VERSION "." DRV_BUILD_ID
#define	VERSIONSTR 	DRV_VERSION


#define	MODULE_NAME	"skd"
#define	LOG_BUF_LEN	128

#define	SKD_GEO_HEADS		255	/* tracks  per cylinder */
#define	SKD_GEO_SECS_TRACK	126	/* sectors per track */
#define	SG_BOUNDARY		0x20000

#define	PART_BACKUP		2	/* backup partition */
#define	PART_WD			7	/* whole disk partition (EFI) */

#ifdef _BIG_ENDIAN
#define	be64_to_cpu(x) (x)
#define	be32_to_cpu(x) (x)
#define	__be16_to_cpu(x) (x)
#define	cpu_to_be64(x) (x)
#define	cpu_to_be32(x) (x)
#define	cpu_to_be16(x) (x)
#define	__cpu_to_be16(x) (x)
#define	__le64_to_cpu(x) BSWAP_64(x)
#define	__le32_to_cpu(x) BSWAP_32(x)
#define	__le16_to_cpu(x) BSWAP_16(x)
#define	__cpu_to_le64(x) BSWAP_64(x)
#define	__cpu_to_le32(x) BSWAP_32(x)
#define	__cpu_to_le16(x) BSWAP_16(x)

#else

#define	be64_to_cpu(x) BSWAP_64(x)
#define	be32_to_cpu(x) BSWAP_32(x)
#define	__be16_to_cpu(x) BSWAP_16(x)
#define	cpu_to_be64(x) BSWAP_64(x)
#define	cpu_to_be32(x) BSWAP_32(x)
#define	cpu_to_be16(x) BSWAP_16(x)
#define	__cpu_to_be16(x) BSWAP_16(x)
#define	__le64_to_cpu(x) (x)
#define	__le32_to_cpu(x) (x)
#define	__le16_to_cpu(x) (x)
#define	__cpu_to_le64(x) (x)
#define	__cpu_to_le32(x) (x)
#define	__cpu_to_le16(x) (x)
#endif

#define	ATYPE_64BIT		0
#define	ATYPE_32BIT		1

#define	BIT_0			0x00001
#define	BIT_1			0x00002
#define	BIT_2			0x00004
#define	BIT_3			0x00008
#define	BIT_4			0x00010
#define	BIT_5			0x00020
#define	BIT_6			0x00040
#define	BIT_7			0x00080
#define	BIT_8			0x00100
#define	BIT_9			0x00200
#define	BIT_10			0x00400
#define	BIT_11			0x00800
#define	BIT_12			0x01000
#define	BIT_13			0x02000
#define	BIT_14			0x04000
#define	BIT_15			0x08000
#define	BIT_16			0x10000
#define	BIT_17			0x20000
#define	BIT_18			0x40000
#define	BIT_19			0x80000

#define	T_PT0			BIT_0
#define	T_PT1			BIT_1
#define	T_PT2			BIT_2
#define	T_PT3			BIT_3
#define	T_PT4			BIT_4
#define	T_PT5			BIT_5
#define	T_PT6			BIT_6
#define	T_PT7			BIT_7
#define	T_PT8			BIT_8
#define	T_PT9			BIT_9
#define	T_PT10			BIT_10
#define	T_PT11			BIT_11
#define	T_PT12			BIT_12
#define	T_PT13			BIT_13
#define	T_PT14			BIT_14
#define	T_PT15			BIT_15

/* Attach progress flags */
#define	SKD_ATTACHED			BIT_0
#define	SKD_SOFT_STATE_ALLOCED		BIT_1
#define	SKD_DID_MINOR			BIT_2
#define	SKD_CONFIG_SPACE_SETUP		BIT_3
#define	SKD_IOBASE_MAPPED		BIT_4
#define	SKD_IOMAP_IOBASE_MAPPED		BIT_5
#define	SKD_REGS_MAPPED			BIT_6
#define	SKD_DEV_IOBASE_MAPPED		BIT_7
#define	SKD_CONSTRUCTED			BIT_8
#define	SKD_PROBED			BIT_9
#define	SKD_INTR_ADDED			BIT_10
#define	SKD_PATHNAME_ALLOCED		BIT_11
#define	SKD_SUSPENDED			BIT_12
#define	SKD_CMD_ABORT_TMO		BIT_13
#define	SKD_MUTEX_INITED		BIT_14
#define	SKD_MUTEX_DESTROYED		BIT_15
#define	SKD_KSTAT_CREATED		BIT_16
#define	SKD_CMLB_INIT			BIT_17

#define	SKD_IODONE_WIOC			1	/* I/O done */
#define	SKD_IODONE_WNIOC		2	/* I/O NOT done */
#define	SKD_IODONE_WDEBUG		3	/* I/O - debug stuff */

/* State flags */
#define	ONLINE				BIT_0
#define	INTERRUPTS_ENABLED		BIT_1
#define	ADAPTER_SUSPENDED		BIT_2
#define	ADAPTER_TIMER_BUSY		BIT_3
#define	PARITY_ERROR			BIT_4

/*
 * Disk label stuff
 */
#define	DEV_INSTANCEMULT	16
#define	DEV_DRIVEMASK		0x30
#define	DEV_DRIVEMULT		0x10
#define	SKD_FULLDISK		0x1000
#define	SKD_SECTSIZE		512
#define	SKD_DEV_SLICEMASK	0x1f
#define	SKD_FULLDISK		0x1000
#define	SKD_P0			16	/* PARTITION 0 (p0) slice offset */

#define	SKD_MAXPART		64
#define	SKD_INST(dev)		((getminor(dev) / SKD_MAXPART) & 0x3f)
#define	SKD_PART(dev)		((getminor(dev) % SKD_MAXPART) & 0x3f)
#define	GETINSTANCE(minor)	(((minor & 0xfff) >> 6) & 0x3f)

/*
 * Interrupt configuration flags
 */
#define	IFLG_INTR_LEGACY		BIT_0
#define	IFLG_INTR_FIXED			BIT_1
#define	IFLG_INTR_MSI			BIT_2
#define	IFLG_INTR_MSIX			BIT_3

#define	IFLG_INTR_AIF	(IFLG_INTR_MSI | IFLG_INTR_FIXED | IFLG_INTR_MSIX)
#define	MAX_POWER_LEVEL			0
#define	LOW_POWER_LEVEL			(BIT_1 | BIT_0)

#ifndef	DDI_INTR_FLAG_BLOCK
#define	DDI_INTR_FLAG_BLOCK		0x100
#endif
#ifndef	DDI_INTR_ALLOC_NORMAL
#define	DDI_INTR_ALLOC_NORMAL		0
#endif
#ifndef	DDI_INTR_ALLOC_STRICT
#define	DDI_INTR_ALLOC_STRICT		1
#endif

#define	LOCK_NOT_HELD	0
#define	LOCK_IS_HELD	1

#define	VTOC_OLD	1
#define	VTOC_NEW	2
#define	VTOC_32		3

#define	INIT_PARTITIONS		1
#define	INIT_VTOC		2
#define	INIT_DISK_GEO		3

#define	BAD_LISTCNT		5	/* number of backup disk labels */

#define	spc()			(nhead * nsect - apc)
#define	chs2bn(c, h, s)		(((diskaddr_t)(c) * spc() + (h) * nsect + (s)))

/*
 * aif function table
 */
typedef struct skd_ifunc {
	uint_t		(*ifunc)();
} skd_ifunc_t;

#define	SKD_MSIX_AIF		0x0
#define	SKD_MSIX_RSPQ		0x1
#define	SKD_MSIX_MAXAIF		SKD_MSIX_RSPQ + 1

#define	PAGE_SIZE		PAGESIZE

#define	USEC_PER_TICK		drv_hztousec(1)

#define	SKMSG_FREE_LIST		1
#define	SKREQ_FREE_LIST		2
#define	SKREQ_SKMSG_FREE_LIST	4
#define	SKQ_FREE_LIST		8

#define	BLK_BSHIFT		9

/*
 * Stuff from Linux
 */

#define	__user

typedef struct sg_iovec /* same structure as used by readv() Linux system */
{			/* call. It defines one scatter-gather element. */
	void __user *iov_base;	/* Starting address  */
	size_t iov_len;		/* Length in bytes  */
} sg_iovec_t;


typedef struct sg_io_hdr
{
	int interface_id;	/* [i] 'S' for SCSI generic (required) */
	int dxfer_direction;	/* [i] data transfer direction	*/
	unsigned char cmd_len;	/* [i] SCSI command length ( <= 16 bytes) */
	unsigned char mx_sb_len; /* [i] max length to write to sbp */
	unsigned short iovec_count; /* [i] 0 implies no scatter gather */
	unsigned int dxfer_len;	/* [i] byte count of data transfer */
	caddr_t __user dxferp;	/* [i], [*io] points to data transfer memory */
				/* or scatter gather list */
	caddr_t __user cmdp; /* [i], [*i] points to command to perform */
	caddr_t sbp;		/* [i], [*o] points to sense_buffer memory */
	unsigned int timeout;	/* [i] MAX_UINT->no timeout (unit: millisec) */
	unsigned int flags;	/* [i] 0 -> default, see SG_FLAG... */
	int pack_id;		/* [i->o] unused internally (normally) */
	caddr_t __user usr_ptr;	 /* [i->o] unused internally */
	unsigned char status;	/* [o] scsi status */
	unsigned char masked_status; /* [o] shifted, masked scsi status */
	unsigned char msg_status; /* [o] messaging level data (optional) */
	unsigned char sb_len_wr; /* [o] byte count actually written to sbp */
	unsigned short host_status; /* [o] errors from host adapter */
	unsigned short driver_status; /* [o] errors from software driver */
	int resid;		/* [o] dxfer_len - actual_transferred */
	unsigned int duration;	/* [o] time taken by cmd (unit: millisec) */
	unsigned int info;	/* [o] auxiliary information */
} sg_io_hdr_t;	/* 64 bytes long (on i386) */

typedef struct sg_io_hdr32
{
	int32_t interface_id;	/* [i] 'S' for SCSI generic (required) */
	int32_t dxfer_direction; /* [i] data transfer direction	*/
	uchar_t cmd_len;	/* [i] SCSI command length ( <= 16 bytes) */
	uchar_t mx_sb_len;	/* [i] max length to write to sbp */
	uint16_t iovec_count;	/* [i] 0 implies no scatter gather */
	uint32_t dxfer_len;	/* [i] byte count of data transfer */
	caddr32_t __user dxferp; /* [i], [*io] points to data transfer */
				/* memory or scatter gather list */
	caddr32_t __user cmdp; /* [i], [*i] points to command to perform */
	caddr32_t __user sbp;	/* [i], [*o] points to sense_buffer memory */
	uint32_t timeout;	/* [i] MAX_UINT->no timeout (unit: millisec) */
	uint32_t flags;		/* [i] 0 -> default, see SG_FLAG... */
	int32_t pack_id;	/* [i->o] unused internally (normally) */
	caddr32_t __user usr_ptr; /* [i->o] unused internally */
	uchar_t status;		/* [o] scsi status */
	uchar_t masked_status;	/* [o] shifted, masked scsi status */
	uchar_t msg_status;	/* [o] messaging level data (optional) */
	uchar_t sb_len_wr;	/* [o] byte count actually written to sbp */
	uint16_t host_status;	/* [o] errors from host adapter */
	uint16_t driver_status;	/* [o] errors from software driver */
	int32_t resid;		/* [o] dxfer_len - actual_transferred */
	uint32_t duration;	/* [o] time taken by cmd (unit: millisec) */
	uint32_t info;		/* [o] auxiliary information */
} sg_io_hdr32_t;  /* 64 bytes long (on i386) */

#define	sgiohdr32to_sg_io_hdr(u32, sgp)					\
	sgp->interface_id	= u32->interface_id;			\
	sgp->dxfer_direction	= u32->dxfer_direction;			\
	sgp->cmd_len		= u32->cmd_len;				\
	sgp->mx_sb_len		= u32->mx_sb_len;			\
	sgp->iovec_count	= u32->iovec_count;			\
	sgp->dxferp		= (caddr_t)(uintptr_t)u32->dxferp;	\
	sgp->cmdp		= (caddr_t)(uintptr_t)u32->cmdp;	\
	sgp->sbp		= (caddr_t)(uintptr_t)u32->sbp;		\
	sgp->timeout		= u32->timeout;				\
	sgp->flags		= u32->flags;				\
	sgp->pack_id		= u32->pack_id;				\
	sgp->usr_ptr		= (caddr_t)(uintptr_t)u32->usr_ptr;	\
	sgp->status		= u32->status;				\
	sgp->masked_status	= u32->masked_status;			\
	sgp->msg_status		= u32->msg_status;			\
	sgp->sb_len_wr		= u32->sb_len_wr;			\
	sgp->host_status	= u32->host_status;			\
	sgp->driver_status	= u32->driver_status;			\
	sgp->resid		= u32->resid;				\
	sgp->duration		= u32->duration;			\
	sgp->info		= u32->info;

#define	sg_io_hdrtosgiohdr32(sgp, u32)					\
	u32->interface_id	= sgp->interface_id;			\
	u32->dxfer_direction	= sgp->dxfer_direction;			\
	u32->cmd_len		= sgp->cmd_len;				\
	u32->mx_sb_len		= sgp->mx_sb_len;			\
	u32->iovec_count	= sgp->iovec_count;			\
	u32->dxferp		= (caddr32_t)(uintptr_t)sgp->dxferp;	\
	u32->cmdp		= (caddr32_t)(uintptr_t)sgp->cmdp;	\
	u32->sbp		= (caddr32_t)(uintptr_t)sgp->sbp;	\
	u32->timeout		= sgp->timeout;				\
	u32->flags		= sgp->flags;				\
	u32->status		= sgp->status;				\
	u32->masked_status	= sgp->masked_status;			\
	u32->msg_status		= sgp->msg_status;			\
	u32->sb_len_wr		= sgp->sb_len_wr;			\
	u32->host_status	= sgp->host_status;			\
	u32->driver_status	= sgp->driver_status;			\
	u32->resid		= sgp->resid;				\
	u32->duration		= sgp->duration;			\
	u32->info		= sgp->info;

#define	DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL<<(n))-1))

#define	TICKS_PER_SEC	1000000
#define	CAP_SYS_ADMIN	1

/* Nothing calling uses expressions, from what I can see, so... */
#define	min_t(type, x, y)  (type)(MIN(x, y))
#define	max_t(type, x, y)  (type)(MAX(x, y))

#define	VERIFY_READ		0
#define	VERIFY_WRITE		1

#define	SG_INTERFACE_ID_ORIG	'S'

/* Use negative values to flag difference from original sg_header structure */
#define	SG_DXFER_NONE (-1)	/* e.g. a SCSI Test Unit Ready command */
#define	SG_DXFER_TO_DEV (-2)	/* e.g. a SCSI WRITE command */
#define	SG_DXFER_FROM_DEV (-3)	/* e.g. a SCSI READ command */
#define	SG_DXFER_TO_FROM_DEV (-4) /* treated like SG_DXFER_FROM_DEV with the */
				/* additional property than during indirect */
				/* IO the user buffer is copied into the */
				/* kernel buffers before the transfer */
#define	SG_DXFER_UNKNOWN (-5)	/* Unknown data direction */

#define	SG_SET_TIMEOUT		0x2201	/* unit: jiffies (10ms on i386) */
#define	SG_GET_TIMEOUT		0x2202	/* yield timeout as _return_ value */
#define	SG_IO	0x2285	 /* similar effect as write() followed by read() */
#define	SG_GET_VERSION_NUM 0x2282 /* Example: version 2.1.34 yields 20134 */

/* following flag values can be "or"-ed together */
#define	SG_FLAG_DIRECT_IO 1	/* default is indirect IO */
#define	SG_FLAG_UNUSED_LUN_INHIBIT 2   /* default is overwrite lun in SCSI */
				/* command block (when <= SCSI_2) */
#define	SG_FLAG_MMAP_IO 4	/* request memory mapped IO */
#define	SG_FLAG_NO_DXFER 0x10000 /* no transfer of kernel buffers to/from */
				/* user space (debug indirect IO) */
#define	QUEUE_FLAG_QUEUED	1
#define	QUEUE_FLAG_STOPPED	2

#define	SAM_STAT_GOOD			0x00
#define	SAM_STAT_CHECK_CONDITION	0x02

#define	u8	uint8_t
#define	u16	uint16_t
#define	u32	uint32_t
#define	u64	uint64_t

#define	__user
#define	__bitwise
#define	__iomem
#define	likely
#define	unlikely
#define	barrier()
#define	PCI_ANY_ID		(~0)

#define	PCI_STATUS		0x06
#define	PCI_STATUS_CAP_LIST	PCI_STAT_CAP
#define	PCI_HEADER_TYPE_NORMAL	PCI_HEADER_ZERO
#define	PCI_HEADER_TYPE_BRIDGE	PCI_HEADER_ONE
#define	PCI_HEADER_TYPE_CARDBUS PCI_HEADER_TWO
#define	PCI_CAPABILITY_LIST	PCI_CONF_CAP_PTR
#define	PCI_CB_CAPABILITY_LIST	PCI_CBUS_CAP_PTR
#define	PCI_CAP_LIST_ID		PCI_CAP_ID
#define	PCI_CAP_LIST_NEXT	PCI_CAP_NEXT_PTR

#define	PCI_DMA_BIDIRECTIONAL	0
#define	PCI_DMA_TODEVICE	1
#define	PCI_DMA_FROMDEVICE	2
#define	PCI_DMA_NONE		3

#define	PCI_VENDOR_ID		PCI_CONF_VENID
#define	PCI_DEVICE_ID		PCI_CONF_DEVID
#define	PCI_SUBSYSTEM_VENDOR_ID PCI_CONF_SUBVENID
#define	PCI_SUBSYSTEM_ID	PCI_CONF_SUBSYSID

#define	TEST_UNIT_READY		0x00
#define	INQUIRY			0x12
#define	INQUIRY2		(0x12 + 0xe0)
#define	READ_CAPACITY		0x25
#define	READ_CAPACITY_EXT	0x9e
#define	SYNCHRONIZE_CACHE	0x35

#define	DRIVER_INQ_EVPD_PAGE_CODE	0xDA

#define	PCI_EXP_LNKSTA		0x12

#define	PCI_DEVICE(x)	(((x)>>11) & 0x1f)
#define	PCI_FUNCTION(x)	(((x) & 0x700) >> 8)
#define	PCI_BUS(x)	(((x) & 0xff0000) >> 16)

/*
 *  SENSE KEYS
 */
#define	NO_SENSE	    0x00
#define	RECOVERED_ERROR	    0x01
#define	NOT_READY	    0x02
#define	MEDIUM_ERROR	    0x03
#define	HARDWARE_ERROR	    0x04
#define	ILLEGAL_REQUEST	    0x05
#define	UNIT_ATTENTION	    0x06
#define	DATA_PROTECT	    0x07
#define	BLANK_CHECK	    0x08
#define	COPY_ABORTED	    0x0a
#define	ABORTED_COMMAND	    0x0b
#define	VOLUME_OVERFLOW	    0x0d
#define	MISCOMPARE	    0x0e

/*
 * enum irqreturn
 * @IRQ_NONE		 interrupt was not from this device
 * @IRQ_HANDLED	 interrupt was handled by this device
 * @IRQ_WAKE_THREAD	 handler requests to wake the handler thread
 */
enum irqreturn {
	IRQ_NONE		 = (0 << 0),
	IRQ_HANDLED		 = (1 << 0),
	IRQ_WAKE_THREAD	 = (1 << 1),
};

typedef enum irqreturn irqreturn_t;
typedef irqreturn_t    (*irq_handler_t) (int, void *);

#define	printk	cmn_err

#define	readl(addr)	  (*(volatile uint32_t *) (addr))
#define	writel(val, addr) *(volatile u32 *) (addr) = (u32)(val)

typedef uint64_t	dma_addr_t;
typedef unsigned	fmode_t;
typedef u64		phys_addr_t;
typedef phys_addr_t	resource_size_t;
typedef kmutex_t	spinlock_t;
typedef uint32_t	atomic_t;

struct msix_entry {
	u32 vector;
	u16 entry;
};

typedef struct pci_device_id {
	int	vendor;
	int	device;
	int	subvendor;
	int	subdevice;
	int	class;
	int	class_mask;
	long	driver_data;
} pci_device_id_t;

typedef struct skd_wait_queue {
	kcondvar_t	cv;
	kmutex_t	lock;
} wait_queue_head_t;

struct list_head {
	struct list_head *next, *prev;
};

typedef enum mem_alloc_type {
	UNKNOWN_MEMORY,
	TASK_MEMORY,
	LITTLE_ENDIAN_DMA,
	BIG_ENDIAN_DMA,
	KERNEL_MEM,
	NO_SWAP_DMA,
	STRUCT_BUF_MEMORY
} mem_alloc_type_t;

typedef struct dma_mem_t {
	void			*bp;
	ddi_acc_handle_t	acc_handle;
	ddi_dma_handle_t	dma_handle;
	ddi_dma_cookie_t	cookie;
	ddi_dma_cookie_t	*cookies;
	uint64_t		alignment;
	uint32_t		cookie_count;
	uint32_t		size;
	uint32_t		memflags;
	uint32_t		flags;
	mem_alloc_type_t	type;
} dma_mem_t;


struct timer_list {
	struct list_head	entry;
	unsigned long		expires;
	void (*function)	(unsigned long);
	unsigned long		data;
	int			slack;
};

struct scatterlist {
	unsigned long	page_link;
	unsigned int	offset;
	unsigned int	length;
	dma_addr_t	dma_address;
	char		*page;
};

#define	SKD_WRITEL(DEV, VAL, OFF)	skd_reg_write32(DEV, VAL, OFF)
#define	SKD_READL(DEV, OFF)		skd_reg_read32(DEV, OFF)
#define	SKD_WRITEQ(DEV, VAL, OFF)	skd_reg_write64(DEV, VAL, OFF)
#define	SKD_READQ(DEV, OFF)		skd_reg_read64(DEV, OFF)

/* Capability lists */
#define	PCI_CAP_ID_SLOTID	0x04	/* Slot Identification */
#define	PCI_CAP_ID_CHSWP	0x06	/* CompactPCI HotSwap */
#define	PCI_CAP_ID_VNDR		0x09	/* Vendor specific */
#define	PCI_CAP_ID_DBG		0x0A	/* Debug port */
#define	PCI_CAP_ID_CCRC		0x0B	/* CompactPCI Central Resrc. Control */
#define	PCI_CAP_ID_SHPC		0x0C	/* PCI Standard Hot-Plug Controller */
#define	PCI_CAP_ID_SSVID	0x0D	/* Bridge subsystem vendor/device ID */
#define	PCI_CAP_ID_AGP3		0x0E	/* AGP Target PCI-PCI bridge */
#define	PCI_CAP_ID_EXP		0x10	/* PCI Express */
#define	PCI_CAP_ID_MSIX		0x11	/* MSI-X */
#define	PCI_CAP_ID_AF		0x13	/* PCI Advanced Features */
#define	PCI_CAP_FLAGS		2	/* Capability defined flags (16 bits) */
#define	PCI_CAP_SIZEOF		4

/*
 * End Stuff from Linux
 */

/*
 * Debugging
 */
#ifdef DEBUG_LEVEL
#define	DPRINTF(n, args)	if (skd_dbg_debug > (n)) cmn_err args
#else
#define	DPRINTF(n, args)
#endif

#define	VPRINTK	cmn_err

#define	DPRINTK(skdev, fmt, args...) \
	if ((skdev)->dbg_level > 0) { \
	    cmn_err(CE_NOTE, "%s" fmt, (skdev)->name, args); \
	}

#define	SKD_DMA_MAXXFER			(2048 * DEV_BSIZE)

#define	SKD_DMA_LOW_ADDRESS		(uint64_t)0
#define	SKD_DMA_HIGH_64BIT_ADDRESS	(uint64_t)0xffffffffffffffff
#define	SKD_DMA_HIGH_32BIT_ADDRESS	(uint64_t)0xffffffff
#define	SKD_DMA_XFER_COUNTER		(uint64_t)0xffffffff
#define	SKD_DMA_ADDRESS_ALIGNMENT	(uint64_t)SG_BOUNDARY
#define	SKD_DMA_BURSTSIZES		0xff
#define	SKD_DMA_MIN_XFER_SIZE		1
#define	SKD_DMA_MAX_XFER_SIZE		(uint64_t)0xfffffe00
#define	SKD_DMA_SEGMENT_BOUNDARY	(uint64_t)0xffffffff
#define	SKD_DMA_SG_LIST_LENGTH		256
#define	SKD_DMA_XFER_FLAGS		0
#define	SKD_DMA_GRANULARITY		512 /* 1 */

#define	PCI_VENDOR_ID_STEC  0x1B39
#define	PCI_DEVICE_ID_SUMO  0x0001


#define	SKD_MINORS_PER_DEVICE	16		/* max minors per blkdev */

#define	SKD_PAUSE_TIMEOUT	(5*1000)	/* timeout in ms */

#define	SKD_N_FITMSG_BYTES	(512u)

#define	SKD_N_SPECIAL_CONTEXT	64u
#define	SKD_N_SPECIAL_FITMSG_BYTES (128u)
#define	SKD_N_SPECIAL_DATA_BYTES  (8u*1024u)


/*
 * SG elements are 32 bytes, so we can make this 4096 and still be under the
 * 128KB limit.	 That allows 4096*4K = 16M xfer size
 */
#define	SKD_N_SG_PER_REQ_DEFAULT 256u
#define	SKD_N_SG_PER_SPECIAL	256u


#define	SKD_N_COMPLETION_ENTRY		256u
#define	SKD_N_READ_CAP_BYTES		(8u)
#define	SKD_N_READ_CAP_EXT_BYTES	(16)

#define	SKD_N_INTERNAL_BYTES   (512u)

/* 5 bits of uniqifier, 0xF800 */
#define	SKD_ID_INCR		(0x400)
#define	SKD_ID_TABLE_MASK	(3u << 8u)
#define	SKD_ID_RW_REQUEST	(0u << 8u)
#define	SKD_ID_INTERNAL		(1u << 8u)
#define	SKD_ID_SPECIAL_REQUEST	(2u << 8u)
#define	SKD_ID_FIT_MSG		(3u << 8u)
#define	SKD_ID_SLOT_MASK	0x00FFu
#define	SKD_ID_SLOT_AND_TABLE_MASK 0x03FFu

#define	SKD_N_TIMEOUT_SLOT	8u
#define	SKD_TIMEOUT_SLOT_MASK	7u

#define	SKD_N_MAX_SECTORS 2048u

#define	SKD_MAX_RETRIES 10u

#define	SKD_TIMER_SECONDS(seconds) (seconds)
#define	SKD_TIMER_MINUTES(minutes) ((minutes)*(60))

#define	SLICE_OFFSET	0

/*
 * NOTE:  INTR_LOCK() should be held prior to grabbing WAITQ_LOCK() if both
 * are needed.
 */
#define	INTR_LOCK(skdev)		mutex_enter(&skdev->skd_intr_mutex)
#define	INTR_UNLOCK(skdev)		mutex_exit(&skdev->skd_intr_mutex)
#define	INTR_LOCK_HELD(skdev)		MUTEX_HELD(&skdev->skd_intr_mutex)

#define	WAITQ_LOCK(skdev, locked, disk) \
	mutex_enter(&skdev->waitqueue_mutex[0])
#define	WAITQ_UNLOCK(skdev, locked, disk) \
	mutex_exit(&skdev->waitqueue_mutex[0])
#define	WAITQ_LOCK_HELD(skdev, locked, disk) \
	MUTEX_HELD(&skdev->waitqueue_mutex[0])

#define	ADAPTER_STATE_LOCK(skdev)	mutex_enter(&skdev->skd_lock_mutex)
#define	ADAPTER_STATE_UNLOCK(skdev)	mutex_exit(&skdev->skd_lock_mutex)

enum skd_drvr_state {
	SKD_DRVR_STATE_LOAD,			/* 0 when driver first loaded */
	SKD_DRVR_STATE_IDLE,			/* 1 when device goes offline */
	SKD_DRVR_STATE_BUSY,			/* 2 */
	SKD_DRVR_STATE_STARTING,		/* 3 */
	SKD_DRVR_STATE_ONLINE,			/* 4 */
	SKD_DRVR_STATE_PAUSING,			/* 5 */
	SKD_DRVR_STATE_PAUSED,			/* 6 */
	SKD_DRVR_STATE_DRAINING_TIMEOUT,	/* 7 */
	SKD_DRVR_STATE_RESTARTING,		/* 8 */
	SKD_DRVR_STATE_RESUMING,		/* 9 */
	SKD_DRVR_STATE_STOPPING,	/* 10 when driver is unloading */
	SKD_DRVR_STATE_FAULT,			/* 11 */
	SKD_DRVR_STATE_DISAPPEARED,		/* 12 */
	SKD_DRVR_STATE_PROTOCOL_MISMATCH,	/* 13 */
	SKD_DRVR_STATE_BUSY_ERASE,		/* 14 */
	SKD_DRVR_STATE_BUSY_SANITIZE,		/* 15 */
	SKD_DRVR_STATE_BUSY_IMMINENT,		/* 16 */
	SKD_DRVR_STATE_WAIT_BOOT,		/* 17 */
	SKD_DRVR_STATE_SYNCING			/* 18 */
};

#define	SKD_WAIT_BOOT_TO 90u
#define	SKD_STARTING_TO	 248u

enum skd_req_state {
	SKD_REQ_STATE_IDLE,
	SKD_REQ_STATE_SETUP,
	SKD_REQ_STATE_BUSY,
	SKD_REQ_STATE_COMPLETED,
	SKD_REQ_STATE_TIMEOUT,
	SKD_REQ_STATE_ABORTED,
};

enum skd_fit_msg_state {
	SKD_MSG_STATE_IDLE,
	SKD_MSG_STATE_BUSY,
};

enum skd_check_status_action {
	SKD_CHECK_STATUS_REPORT_GOOD,
	SKD_CHECK_STATUS_REPORT_SMART_ALERT,
	SKD_CHECK_STATUS_REQUEUE_REQUEST,
	SKD_CHECK_STATUS_REPORT_ERROR,
	SKD_CHECK_STATUS_BUSY_IMMINENT,
};

/* NOTE:  mbu_t users should name this field "mbu". */
typedef union {
	u8 *mb8;
	u64 *mb64;
} mbu_t;
#define	msg_buf mbu.mb8
#define	msg_buf64 mbu.mb64

struct skd_fitmsg_context {
	enum skd_fit_msg_state state;
	struct skd_fitmsg_context *next;
	u32		id;
	u16		outstanding;
	u32		length;
	u32		offset;
	mbu_t		mbu;	/* msg_buf & msg_buf64 */
	dma_mem_t	mb_dma_address;
};

/*
 * Stuff from Linux
 */

struct pci_dev {
	int			irq;
	int			func;
	int			devfn;
	int			bus;
	struct skd_device	*skdev;
};

/*
 * End Stuff from Linux
 */

struct skd_request_context {
	enum skd_req_state		state;
	struct skd_request_context	*next;
	u16				did_complete;
	u16				id;
	u32				fitmsg_id;
	struct buf			*bp;
	u32				timeout_stamp;
	u8				sg_data_dir;
	struct scatterlist		*sg;
	u32				n_sg;
	ddi_dma_handle_t		io_dma_handle;
	struct fit_sg_descriptor	*sksg_list;
	dma_mem_t			sksg_dma_address;
	struct fit_completion_entry_v1	completion;
	struct fit_comp_error_info	err_info;
	int				total_sg_bcount;
};

#define	SKD_DATA_DIR_HOST_TO_CARD	1
#define	SKD_DATA_DIR_CARD_TO_HOST	2

struct skd_special_context {
	struct skd_request_context req;
	u8			   orphaned;
	u32			   sg_byte_count;
	void			   *data_buf;
	dma_mem_t		   db_dma_address;
	mbu_t			   mbu;	/* msg_buf & msg_buf64 */
	dma_mem_t		   mb_dma_address;
	dma_mem_t		   page_dma_address;
	int			   io_pending;
};

struct skd_sg_io {
	struct skd_special_context *skspcl;
	struct page		   *page;
	struct sg_iovec		   *iov;
	struct sg_iovec		   no_iov_iov;
	struct sg_io_hdr	   sg;
	u8			   cdb[16];
	size_t			   dxfer_len;
	u32			   iovcnt;
	struct uscsi_cmd	   *ucmdp;
	void			   *argp;
	int			   flags;
	int			   dxfer_direction;
};


typedef enum skd_irq_type {
	SKD_IRQ_LEGACY,
	SKD_IRQ_MSI,
	SKD_IRQ_MSIX
} skd_irq_type_t;

typedef struct skd_device skd_device_t;

#define	SKD_MAX_BARS			2

typedef struct skd_xfer {
	/*
	 * NB: If using DMA the br_ndmac will be non-zero.  Otherwise
	 * the br_kaddr will be non-NULL.
	 */
	diskaddr_t		x_blkno;
	size_t			x_nblks;
	ddi_dma_handle_t	x_dmah;
	ddi_dma_cookie_t	x_dmac;
	unsigned		x_ndmac;
	caddr_t			x_kaddr;
} skd_xfer_t;

typedef struct skd_drive {
	uint32_t	d_qsize;
	uint32_t	d_maxxfer;
	boolean_t	d_removable;
	boolean_t	d_hotpluggable;
	int		d_target;
	int		d_lun;
} skd_drive_t;

typedef struct skd_media {
	/*
	 * NB: The block size must be a power of two not less than
	 * DEV_BSIZE (512).  Other values of the block size will
	 * simply not function and the media will be rejected.
	 *
	 * The block size must also divide evenly into the device's
	 * d_maxxfer field.  If the maxxfer is a power of two larger
	 * than the block size, then this will automatically be
	 * satisfied.
	 */
	uint64_t	m_nblks;
	uint32_t	m_blksize;
	boolean_t	m_readonly;
} skd_media_t;

#define	BD_OPS_VERSION_0	0

typedef struct skd_cops {
	int	o_version;
	void	(*o_drive_info) (skd_device_t *, skd_drive_t *);
	int	(*o_media_info) (skd_device_t *, skd_media_t *);
	int	(*o_devid_init) (skd_device_t *, dev_info_t *, ddi_devid_t *);
	int	(*o_sync_cache) (skd_device_t *, skd_xfer_t  *);
	int	(*o_read)(skd_device_t	*, skd_xfer_t *);
	int	(*o_write)(skd_device_t *, skd_xfer_t *);
	int	(*o_dump) (skd_device_t *, skd_xfer_t *);
} skd_cops_t;

typedef struct skd_handle {
	skd_cops_t		h_ops;
	ddi_dma_attr_t		*h_dma;
	dev_info_t		*h_parent;
	dev_info_t		*h_child;
	void			*h_private;
	struct skd_device	*h_bd;
	char			*h_name;
	char			h_addr[20];	/* enough for %X,%X */
} skd_handle_t;

typedef struct skd_host {
	dev_info_t	*h_dip;
	int		h_nslot;
	skd_device_t	*h_slots;
	ddi_dma_attr_t	*h_dma;		/* dma attr, needed for mem */

	list_node_t	h_node;		/* nexus node linkage */

	uint32_t	h_flags;
#define	HOST_ATTACH	(1U << 0)	/* host attach completed */
#define	HOST_XOPEN	(1U << 2)	/* exclusive open */
#define	HOST_SOPEN	(1U << 3)	/* shared open */
} skd_host_t;

struct skd_device {
	skd_irq_type_t	irq_type;
	u32		msix_count;
	struct skd_msix_entry *msix_entries;

	struct pci_dev	dev;
	struct pci_dev	*pdev;

	struct gendisk	*disk;
	int		gendisk_on;
	int		sync_done;

	int		device_count;
	u32		devno;
	u32		major;
	minor_t		skd_fulldisk_minor;
	char		name[32];
	char		isr_name[30];

	enum		skd_drvr_state state;
	u32		drive_state;

	uint32_t	queue_depth_busy;
	u32		queue_depth_limit;
	u32		queue_depth_lowat;
	u32		soft_queue_depth_limit;
	u32		hard_queue_depth_limit;

	u32		num_fitmsg_context;
	u32		num_req_context;

	u32		timeout_slot[SKD_N_TIMEOUT_SLOT];
	u32		timeout_stamp;

	struct skd_fitmsg_context *skmsg_free_list;
	struct skd_fitmsg_context *skmsg_table;

	struct skd_request_context *skreq_free_list;
	struct skd_request_context *skreq_table;

	struct skd_special_context *skspcl_free_list;
	struct skd_special_context *skspcl_table;

	struct skd_special_context internal_skspcl;

	uint64_t	read_cap_last_lba;
	u32		read_cap_blocksize;
	int		read_cap_is_valid;
	int		inquiry_is_valid;
	char		inq_serial_num[13]; /* 12 chars plus null term */
	char		inq_vendor_id[9];
	char		inq_product_id[17];
	char		inq_product_rev[5];
	char		id_str[128]; /* holds a composite name (pci + sernum) */

	u8		skcomp_cycle;
	u32		skcomp_ix;
	struct fit_completion_entry_v1 *skcomp_table;
	struct fit_comp_error_info *skerr_table;
	dma_mem_t	cq_dma_address;

	wait_queue_head_t waitq;

	struct timer_list timer;
	u32		timer_active;
	u32		timer_countdown;
	u32		timer_substate;

	int		sgs_per_request;
	u32		last_mtd;

	u32		proto_ver;

	int		dbg_level;

	u32		timo_slot;

	ddi_acc_handle_t	pci_handle;
	ddi_acc_handle_t	iobase_handle;
	ddi_acc_handle_t	iomap_handle;
	caddr_t			iobase;
	caddr_t			iomap_iobase;

	ddi_acc_handle_t	dev_handle;
	caddr_t			dev_iobase;
	int			dev_memsize;

	uint8_t			header_type;

	char			*pathname;

	struct skd_device	*base_addr;

	dev_info_t		*dip;

	int			instance;
	uint16_t		vendor_id;
	uint16_t		sub_vendor_id;
	uint16_t		device_id;
	uint16_t		sub_device_id;
	uint8_t			device_ipin;

	kmutex_t		skd_lock_mutex;
	kmutex_t		skd_intr_mutex;
	kmutex_t		skd_fit_mutex;

	uint32_t		flags;

	uint8_t			power_level;

	/* AIF (Advanced Interrupt Framework) support */
	ddi_intr_handle_t	*htable;
	uint32_t		hsize;
	int32_t			intr_cnt;
	uint32_t		intr_pri;
	int32_t			intr_cap;
	uint32_t		iflags;
	int			actual;
	int			dup_cnt;
	int			inum;

	uint64_t		Nblocks;

	ddi_iblock_cookie_t	iblock_cookie;

	/*
	 * bp I/O queue
	 */
	struct buf	*list_head;
	struct buf	*list_tail;
	struct buf	*bp;


	int		n_req;
	uint32_t	progress;
	uint64_t	intr_cntr;
	uint64_t	fitmsg_sent1;
	uint64_t	fitmsg_sent2;
	uint64_t	active_cmds;
	uint64_t	active_cmds_max;

	kmutex_t	skd_internalio_mutex;
	kcondvar_t	cv_waitq;

	kmutex_t	skd_s1120wait_mutex;
	kcondvar_t	cv_s1120wait;

#define	SKD_NUMDRIVES	1
	kmutex_t	waitqueue_mutex[SKD_NUMDRIVES];
	struct diskhd	waitqueue[SKD_NUMDRIVES];

	struct ipart	iparts[SKD_NUMDRIVES][FD_NUMPART];
	struct vtoc	partitions[SKD_NUMDRIVES];
	struct vtoc32	partitions32[SKD_NUMDRIVES];

	struct extpartition extpart[SKD_NUMDRIVES];
	struct extvtoc	extvtoc[SKD_NUMDRIVES];

	struct dk_geom	dkg;
	struct dk_cinfo cinfo;

	int		slice_offset;
	int		disks_initialized;

	int		quiesced;
	int		waiting_for_resources;

	ddi_devid_t	s1120_devid;
	char		devid_str[80];

	ddi_devid_t	d_devid;

	uint32_t	d_blkshift;

	int		attached;

	int		ios_queued;
	int		ios_started;
	int		ios_completed;
	int		ios_errors;
	int		iodone_wioc;
	int		iodone_wnioc;
	int		iodone_wdebug;
	int		iodone_unknown;

#ifdef USE_BLKDEV
	bd_handle_t	s_bdh;
#else
	sbd_handle_t	s_bdh;
#endif
	int		bd_attached;

	skd_host_t	*s_hostp;
#ifdef USE_BLKDEV
	bd_ops_t	s_ops;
#else
	sbd_ops_t	s_ops;
#endif
	int		s_slot_num;

#ifdef USE_SKE_EMULATOR
	ske_device_t *ske_handle;
#endif

	timeout_id_t	skd_timer_timeout_id;
};

struct skd_msix_entry {
	int have_irq;
	u32 vector;
	u32 entry;
	struct skd_device *rsp;
	char isr_name[30];
};

struct skd_init_msix_entry {
	const char *name;
	irq_handler_t handler;
};

static void skd_disable_interrupts(struct skd_device *skdev);
static void skd_isr_completion_posted(struct skd_device *skdev);
static void skd_recover_requests(struct skd_device *skdev);
static void skd_log_skdev(struct skd_device *skdev, const char *event);
static void skd_restart_device(struct skd_device *skdev);
static void skd_destruct(struct skd_device *skdev);
static int skd_unquiesce_dev(struct skd_device *skdev);
static void skd_send_special_fitmsg(struct skd_device *skdev,
    struct skd_special_context *skspcl);
static void skd_end_request(struct skd_device *skdev,
    struct skd_request_context *skreq, int error);
static int skd_preop_sg_list(struct skd_device *skdev,
    struct skd_request_context *skreq, u32* sg_byte_count);
static void skd_release_special(struct skd_device *skdev,
    struct skd_special_context *skspcl);
static void skd_log_skmsg(struct skd_device *skdev,
    struct skd_fitmsg_context *skmsg, const char *event);
static void skd_log_skreq(struct skd_device *skdev,
    struct skd_request_context *skreq, const char *event);
static void skd_send_fitmsg(struct skd_device *skdev,
    struct skd_fitmsg_context *skmsg, struct buf *bp);

const char *skd_drive_state_to_str(int state);
const char *skd_skdev_state_to_str(enum skd_drvr_state state);

#define	ORIGIN_SKD	1
#define	ORIGIN_BLKDEV	2

typedef struct skd_buf_private {
	/* Use u64 for lint-clean alignment. */
	u64		 *skreq;
	char		 origin;
#ifdef USE_BLKDEV
	bd_xfer_t	 *x_xfer;
#else
	sbd_xfer_t	 *x_xfer;
#endif
} skd_buf_private_t;

#ifdef	__cplusplus
}
#endif

#endif /* _SKD_H */
