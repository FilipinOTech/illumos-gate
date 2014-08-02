/*
 *
 *  skd.c: Solaris 11/10 Driver for sTec, Inc. S112x PCIe SSD card
 *
 *  Solaris driver is based on the Linux driver authored by:
 *
 *  Authors/Alphabetical:	Dragan Stancevic <dstancevic@stec-inc.com>
 *				Gordon Waidhofer <gwaidhofer@stec-inc.com>
 *				John Hamilton	 <jhamilton@stec-inc.com>
 */

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

#include	<sys/types.h>
#include	<sys/stream.h>
#include	<sys/cmn_err.h>
#include	<sys/kmem.h>
#include	<sys/file.h>
#include	<sys/buf.h>
#include	<sys/uio.h>
#include	<sys/cred.h>
#include	<sys/modctl.h>
#include 	<sys/debug.h>
#include 	<sys/modctl.h>
#include 	<sys/cmlb.h>
#include 	<sys/list.h>
#include 	<sys/sysmacros.h>
#include 	<sys/dkio.h>
#include 	<sys/vtoc.h>
#include 	<sys/fs/dv_node.h>
#include 	<sys/errno.h>
#include 	<sys/pcie.h>
#include 	<sys/pci.h>
#include	<sys/ddi.h>
#include	<sys/dditypes.h>
#include	<sys/sunddi.h>
#include	<sys/atomic.h>
#include	<sys/mutex.h>
#include	<sys/param.h>
#include	<sys/utsname.h>
#include 	<sys/scsi/scsi.h>
#include	<sys/scsi/impl/uscsi.h>
#include 	<sys/scsi/generic/commands.h>
#include 	<sys/devops.h>

/*
 * This is where SOLARIS11, SOLARIS110, etc., are defined
 */
#include	"skd_os_targets.h"

#ifdef SOLARIS11_1
#include <pciaccess.h>
#endif

#if	(defined(SOLARIS11_1) || defined(SOLARIS11)) && !defined(NEX31)
#define	USE_BLKDEV
#include	<sys/blkdev.h>
#else
#include	"../sblkdev/sblkdev.h"
#endif

#define	P0_RAW_DISK (NDKMAP)

#include	"skd_s1120.h"

#include	"skd.h"

/*
 * Panic() stuff
 */
extern int do_polled_io;

int		skd_dbg_level	  = 0;
int		skd_dbg_ioctl	  = 0;
int		skd_dbg_blkdev	  = 0;

void		*skd_state	  = NULL;
int		skd_disable_msi	  = 0;
int		skd_disable_msix  = 0;

/* Initialized in _init() and tunable, see _init(). */
clock_t		skd_timer_ticks;

static u32	skd_major;
uint64_t	jiffies;
uint32_t	skd_os_release_level;

enum {
	STEC_LINK_2_5GTS = 0,
	STEC_LINK_5GTS = 1,
	STEC_LINK_8GTS = 2,
	STEC_LINK_UNKNOWN = 0xFF
};

/* DMA access attribute structure. */
static ddi_device_acc_attr_t skd_dev_acc_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC
};

/* I/O DMA attributes structures. */
static ddi_dma_attr_t skd_64bit_io_dma_attr = {
	DMA_ATTR_V0,			/* dma_attr_version */
	SKD_DMA_LOW_ADDRESS,		/* low DMA address range */
	SKD_DMA_HIGH_64BIT_ADDRESS,	/* high DMA address range */
	SKD_DMA_XFER_COUNTER,		/* DMA counter register */
	SKD_DMA_ADDRESS_ALIGNMENT,	/* DMA address alignment */
	SKD_DMA_BURSTSIZES,		/* DMA burstsizes */
	SKD_DMA_MIN_XFER_SIZE,		/* min effective DMA size */
	SKD_DMA_MAX_XFER_SIZE,		/* max DMA xfer size */
	SKD_DMA_SEGMENT_BOUNDARY,	/* segment boundary */
	SKD_DMA_SG_LIST_LENGTH,		/* s/g list length */
	SKD_DMA_GRANULARITY,		/* granularity of device */
	SKD_DMA_XFER_FLAGS		/* DMA transfer flags */
};

static ddi_dma_attr_t skd_32bit_io_dma_attr = {
	DMA_ATTR_V0,			/* dma_attr_version */
	SKD_DMA_LOW_ADDRESS,		/* low DMA address range */
	SKD_DMA_HIGH_32BIT_ADDRESS,	/* high DMA address range */
	SKD_DMA_XFER_COUNTER,		/* DMA counter register */
	SKD_DMA_ADDRESS_ALIGNMENT,	/* DMA address alignment */
	SKD_DMA_BURSTSIZES,		/* DMA burstsizes */
	SKD_DMA_MIN_XFER_SIZE,		/* min effective DMA size */
	SKD_DMA_MAX_XFER_SIZE,		/* max DMA xfer size */
	SKD_DMA_SEGMENT_BOUNDARY,	/* segment boundary */
	SKD_DMA_SG_LIST_LENGTH,		/* s/g list length */
	SKD_DMA_GRANULARITY,		/* granularity of device */
	SKD_DMA_XFER_FLAGS		/* DMA transfer flags */
};

#define	SKD_MAX_MSIX_COUNT		13
#define	SKD_MIN_MSIX_COUNT		7
#define	SKD_BASE_MSIX_IRQ		4

#define	SKD_IRQ_DEFAULT SKD_IRQ_MSIX
int skd_isr_type = SKD_IRQ_DEFAULT;

#define	SKD_MAX_QUEUE_DEPTH	    255
#define	SKD_MAX_QUEUE_DEPTH_DEFAULT 64
int skd_max_queue_depth = SKD_MAX_QUEUE_DEPTH_DEFAULT;

#define	SKD_MAX_REQ_PER_MSG	    14
#define	SKD_MAX_REQ_PER_MSG_DEFAULT 1
int skd_max_req_per_msg = SKD_MAX_REQ_PER_MSG_DEFAULT;

#define	SKD_MAX_N_SG_PER_REQ	    4096
int skd_sgs_per_request = SKD_N_SG_PER_REQ_DEFAULT;

static int skd_getinfo(dev_info_t *, ddi_info_cmd_t, void *, void **);
static int skd_open(dev_t *, int, int, cred_t *);
static int skd_close(dev_t, int, int, cred_t *);
static int skd_strategy(struct buf *);
static int skd_ioctl(dev_t, int, intptr_t, int, cred_t *, int *);
static int skd_prop_op(dev_t, dev_info_t *, ddi_prop_op_t, int,
    char *, caddr_t, int *);
static int skd_sys_quiesce_dev(dev_info_t *);
static int skd_quiesce_dev(skd_device_t *);
static int skd_list_skmsg(skd_device_t *, int);
static int skd_list_skreq(skd_device_t *, int);
static int skd_attach(dev_info_t *dip, ddi_attach_cmd_t cmd);
static int skd_detach(dev_info_t *dip, ddi_detach_cmd_t cmd);
static int skd_format_internal_skspcl(struct skd_device *skdev);
static int skd_bstr_to_dec(char *, uint32_t *, uint32_t);
static int skd_start(skd_device_t *, int);
static void skd_destroy_mutex(skd_device_t *skdev);
static void skd_enable_interrupts(struct skd_device *);
static void skd_request_fn_not_online(skd_device_t *skdev);
static void skd_send_internal_skspcl(struct skd_device *,
    struct skd_special_context *, u8);
static void skd_queue_bp(skd_device_t *, int, struct buf *);
static void *skd_alloc_dma_mem(skd_device_t *, dma_mem_t *, uint_t, uint8_t);
static void skd_free_dma_resource(skd_device_t *, dma_mem_t *);
static void skd_release_intr(skd_device_t *skdev);
static void skd_isr_fwstate(struct skd_device *skdev);
static void skd_isr_msg_from_dev(struct skd_device *skdev);
static void skd_soft_reset(struct skd_device *skdev);
static void skd_refresh_device_data(struct skd_device *skdev);
static void skd_update_props(skd_device_t *, dev_info_t *);
static void skd_end_request_abnormal(struct skd_device *, struct buf *,
    int, int);
static uint64_t skd_partsize(skd_device_t *, int, int);

static char *skd_pci_info(struct skd_device *skdev, char *str);

static struct buf *skd_blk_peek_request(skd_device_t *, int);
static struct buf *skd_get_queued_bp(skd_device_t *, int);

static uint8_t  skd_pci_get8(skd_device_t *skdev, off_t offset);
static uint16_t skd_pci_get16(skd_device_t *skdev, off_t offset);
static void 	skd_pci_put16(skd_device_t *skdev, off_t offset,
    uint16_t data);

#ifdef SOLARIS11
static int skd_tg_rdwr(dev_info_t *, uchar_t, void *, diskaddr_t,
    size_t, void *);
static int skd_tg_getinfo(dev_info_t *, int, void *, void *);
#else
static int skd_tg_rdwr(dev_info_t *, uchar_t, void *, diskaddr_t, size_t);
static int skd_tg_getphygeom(dev_info_t *, cmlb_geom_t *);
static int skd_tg_getvirtgeom(dev_info_t *devi, cmlb_geom_t *);
static int skd_tg_getcapacity(dev_info_t *, diskaddr_t *);
static int skd_tg_getattribute(dev_info_t *, tg_attribute_t *);
#endif

#ifdef USE_BLKDEV
#define	sbd_drive_t bd_drive_t
#define	sbd_media_t bd_media_t
#define	sbd_xfer_t  bd_xfer_t
#endif

void skd_bd_driveinfo(void *arg, sbd_drive_t *drive);
int  skd_bd_mediainfo(void *arg, sbd_media_t *media);
int  skd_bd_read(void *arg,  sbd_xfer_t *xfer);
int  skd_bd_write(void *arg, sbd_xfer_t *xfer);
static int  skd_devid_init(void *arg, dev_info_t *, ddi_devid_t *);

/* Special Inquiry s1120 routines */
static void skd_do_inq_page_00(struct skd_device *,
    volatile struct fit_completion_entry_v1 *,
    volatile struct fit_comp_error_info *, uint8_t *, uint8_t *);
static void skd_do_inq_page_da(struct skd_device *,
    volatile struct fit_completion_entry_v1 *,
    volatile struct fit_comp_error_info *, uint8_t *cdb, uint8_t *);
static void skd_do_driver_inq(struct skd_device *,
    volatile struct fit_completion_entry_v1 *,
    volatile struct fit_comp_error_info *, uint8_t *, uint8_t *);
static void skd_process_scsi_inq(struct skd_device *,
    volatile struct fit_completion_entry_v1 *,
    volatile struct fit_comp_error_info *,
    struct skd_special_context *skspcl);
static unsigned char *skd_sg_1st_page_ptr(struct scatterlist *);
static int skd_pci_get_info(skd_device_t *skdev, int *, int *, int *);

#ifdef USE_BLKDEV
static bd_ops_t skd_bd_ops = {
#else
static sbd_ops_t skd_bd_ops = {
#endif
	BD_OPS_VERSION_0,
	skd_bd_driveinfo,
	skd_bd_mediainfo,
	skd_devid_init,
	NULL,			/* sync_cache */
	skd_bd_read,
	skd_bd_write,
#ifndef USE_BLKDEV
	NULL,			/* dump */
	skd_ioctl
#endif
};

#ifdef USE_BLKDEV
#define	sbd_ops_t bd_ops_t
#endif

static ddi_device_acc_attr_t	dev_acc_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC
};

skd_host_t *skd_host_alloc(dev_info_t *, skd_device_t *, int,
    sbd_ops_t *, ddi_dma_attr_t *);
void skd_host_dealloc(skd_device_t *);

/*
 * Solaris module loading/unloading structures
 */

struct cmlb_tg_ops skd_tg_ops = {
#ifdef SOLARIS11
	TG_DK_OPS_VERSION_1,
	skd_tg_rdwr,
	skd_tg_getinfo,
#else
	TG_DK_OPS_VERSION_0,
	skd_tg_rdwr,
	skd_tg_getphygeom,
	skd_tg_getvirtgeom,
	skd_tg_getcapacity,
	skd_tg_getattribute
#endif
};

static struct cb_ops skd_cb_ops = {
	skd_open,			/* b/c open */
	skd_close,			/* b/c close */
	nodev,				/* b strategy */
	nodev,				/* b print */
	nodev,				/* b dump */
	nodev,				/* c read */
	nodev,				/* c write */
	skd_ioctl,			/* c ioctl */
	nodev,				/* c devmap */
	nodev,				/* c mmap */
	nodev,				/* c segmap */
	nochpoll,			/* c poll */
	skd_prop_op,			/* cb_prop_op */
	NULL,				/* streamtab  */
	D_64BIT | D_MP | D_NEW,		/* Driver compatibility flag */
	CB_REV,				/* cb_ops revision */
	nodev,				/* c aread */
	nodev,				/* c awrite */
};

/*
 * Static declarations of dev_ops entry point functions...
 */
struct dev_ops skd_dev_ops = {
	DEVO_REV,			/* devo_rev */
	0,				/* refcnt */
	skd_getinfo,			/* getinfo */
	nulldev,			/* identify */
	nulldev,			/* probe */
	skd_attach,			/* attach */
	skd_detach,			/* detach */
	nodev,				/* reset */
	&skd_cb_ops,			/* char/block ops */
	NULL,				/* bus operations */
	NULL,				/* power management */
#ifdef SOLARIS11
	skd_sys_quiesce_dev		/* quiesce */
#endif
};

static struct modldrv modldrv = {
	&mod_driverops,			/* type of module: driver */
	"sTec skd v" DRV_VER_COMPL,	/* name of module */
	&skd_dev_ops			/* driver dev_ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modldrv,
	NULL
};

/*
 * sTec-required wrapper for debug printing.
 */
static inline void
Dcmn_err(int lvl, const char *fmt, ...)
{
	va_list ap;

	if (skd_dbg_level == 0)
		return;

	va_start(ap, fmt);
	vcmn_err(lvl, fmt, ap);
	va_end(ap);
}

/*
 * Solaris module loading/unloading routines
 */

/*
 *
 * Name:	skd_host_init_ops
 *
 * Inputs:	devops, device operations it can perform.
 *
 * Returns:	Nothing.
 *
 */
void
skd_host_init_ops(struct dev_ops *devops)
{
#ifdef USE_BLKDEV
	bd_mod_init(devops);
#else
	sbd_mod_init(devops);
#endif
}

/*
 *
 * Name:	skd_host_fini_ops
 *
 * Inputs:	devops, device ops it can perform.
 *
 * Returns:	Nothing.
 *
 */
void
skd_host_fini_ops(struct dev_ops *devops)
{
#ifdef USE_BLKDEV
	bd_mod_fini(devops);
#else
	sbd_mod_fini(devops);
#endif
}

/*
 *
 * Name:	_init, performs initial installation
 *
 * Inputs:	None.
 *
 * Returns:	Returns the value returned by the ddi_softstate_init function
 *		on a failure to create the device state structure or the result
 *		of the module install routines.
 *
 */
int
_init(void)
{
	uint16_t	w16;
	int		rval = 0;
	int		tgts = 0;

#ifdef SOLARIS11_1
	tgts |= 0x1;
#endif
#ifdef SOLARIS11
	tgts |= 0x02;
#endif
#ifdef NEX31
	tgts |= 0x04;
#endif
	tgts |= 0x08;	/* In #ifdef NEXENTA block from original sTec drop. */

	/*
	 * drv_usectohz() is a function, so can't initialize it at
	 * instantiation.
	 */
	skd_timer_ticks = drv_usectohz(1000000);

	Dcmn_err(CE_NOTE,
	    "<# Installing skd Driver dbg-lvl=%d %s %x>",
	    skd_dbg_level, DRV_BUILD_ID, tgts);

	/* Get OS major release level. */
	for (w16 = 0; w16 < sizeof (utsname.release); w16++) {
		if (utsname.release[w16] == '.') {
			w16++;
			break;
		}
	}

	if (w16 < sizeof (utsname.release)) {
		(void) skd_bstr_to_dec(&utsname.release[w16],
		    &skd_os_release_level, 0);
	} else {
		skd_os_release_level = 0;
	}

	rval = ddi_soft_state_init(&skd_state, sizeof (skd_device_t), 0);
	if (rval != DDI_SUCCESS)
		return (rval);

	skd_host_init_ops(&skd_dev_ops);

	rval = mod_install(&modlinkage);
	if (rval != DDI_SUCCESS)
		ddi_soft_state_fini(&skd_state);

	if (rval != DDI_SUCCESS)
		cmn_err(CE_CONT, "Unable to install/attach skd driver");

	return (rval);
}

/*
 *
 * Name: 	_info, returns information about loadable module.
 *
 * Inputs: 	modinfo, pointer to module information structure.
 *
 * Returns: 	Value returned by mod_info().
 *
 */
int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

/*
 * _fini 	Prepares a module for unloading. It is called when the system
 *		wants to unload a module. If the module determines that it can
 *		be unloaded, then _fini() returns the value returned by
 *		mod_remove(). Upon successful return from _fini() no other
 *		routine in the module will be called before _init() is called.
 *
 * Inputs:	None.
 *
 * Returns: 	DDI_SUCCESS or DDI_FAILURE.
 *
 */
int
_fini(void)
{
	int rval;

	rval = mod_remove(&modlinkage);
	if (rval == DDI_SUCCESS) {
		ddi_soft_state_fini(&skd_state);

		Dcmn_err(CE_NOTE,
		    "<<<====# Uninstalled skd Driver #====>>>");
	} else
		Dcmn_err(CE_NOTE, "skd_fini: mod_remove failed");

	return (rval);
}

/*
 * Stubs
 */

/*
 *
 * Name:	skd_blk_rq_bytes
 *
 * Inputs:	bp, buf structure
 *
 * Returns:	Number of bytes not transferred,
 *
 */
static size_t
skd_blk_rq_bytes(struct buf *bp)
{
	size_t bytes_left;

	bytes_left = bp->b_resid;

	return (bytes_left);
}

/*
 *
 * Name:	skd_bstr_to_dec
 *
 * Inputs:	s	- string containing Solaris release level.
 *		*ans	- variable to indirectly return release level.
 *		size	- flag to signal calculation of number size.
 *
 * Returns:	cnt, Solaris release level number.
 *
 */
static int
skd_bstr_to_dec(char *s, uint32_t *ans, uint32_t size)
{
	int			mul, num, cnt, pos;
	char			*str;

	/* Calculate size of number. */
	if (size == 0) {
		for (str = s; *str >= '0' && *str <= '9'; str++) {
			size++;
		}
	}

	*ans = 0;
	for (cnt = 0; *s != '\0' && size; size--, cnt++) {
		if (*s >= '0' && *s <= '9') {
			num = *s++ - '0';
		} else {
			break;
		}

		for (mul = 1, pos = 1; pos < size; pos++) {
			mul *= 10;
		}
		*ans += num * mul;
	}

	return (cnt);
}

/*
 * Solaris Register read/write routines
 */

/*
 *
 * Name:	skd_reg_write64, writes a 64-bit value to specified address
 *
 * Inputs:	skdev		- device state structure.
 *		val		- 64-bit value to be written.
 *		offset		- offset from PCI base address.
 *
 * Returns:	Nothing.
 *
 */
/*
 * Local vars are to keep lint silent.  Any compiler worth its weight will
 * optimize it all right out...
 */
static inline void
skd_reg_write64(struct skd_device *skdev, u64 val, u32 offset)
{
	uint64_t *addr;

	ASSERT((offset & 0x7) == 0);
	/* LINTED */
	addr = (uint64_t *)(skdev->dev_iobase + offset);
	ddi_put64(skdev->dev_handle, addr, val);
}

/*
 *
 * Name:	skd_reg_read32, reads a 32-bit value to specified address
 *
 * Inputs:	skdev		- device state structure.
 *		offset		- offset from PCI base address.
 *
 * Returns:	val, 32-bit value read from specified PCI address.
 *
 */
static inline u32
skd_reg_read32(struct skd_device *skdev, u32 offset)
{
	uint32_t *addr;

	ASSERT((offset & 0x3) == 0);
	/* LINTED */
	addr = (uint32_t *)(skdev->dev_iobase + offset);
	return (ddi_get32(skdev->dev_handle, addr));
}

/*
 *
 * Name:	skd_reg_write32, writes a 32-bit value to specified address
 *
 * Inputs:	skdev		- device state structure.
 *		val		- value to be written.
 *		offset		- offset from PCI base address.
 *
 * Returns:	Nothing.
 *
 */
static inline void
skd_reg_write32(struct skd_device *skdev, u32 val, u32 offset)
{
	uint32_t *addr;

	ASSERT((offset & 0x3) == 0);
	/* LINTED */
	addr = (uint32_t *)(skdev->dev_iobase + offset);
	ddi_put32(skdev->dev_handle, addr, val);
}


/*
 * Solaris skd routines
 */

/*
 *
 * Name:	skd_name, generates the name of the driver.
 *
 * Inputs:	skdev	- device state structure
 *
 * Returns:	char pointer to generated driver name.
 *
 */
static const char *
skd_name(struct skd_device *skdev)
{
	bzero(skdev->id_str, sizeof (skdev->id_str));

	(void) snprintf((char *)skdev->id_str, sizeof (skdev->id_str), "%s:",
	    DRV_NAME);

	return ((char *)skdev->id_str);
}

/*
 *
 * Name:	skd_pci_get8, retrieves an unsigned 8-bit value from PCI space.
 *
 * Inputs:	skdev		- device state structure
 *		offset		- offset into PCI register space.
 *
 * Returns:	unsigned 8-bit value read from PCI register space.
 *
 */
static uint8_t
skd_pci_get8(skd_device_t *skdev, off_t offset)
{

	return (pci_config_get8(skdev->pci_handle, offset));
}

/*
 *
 * Name:	skd_pci_get16, retrieves an unsigned 16-bit value
 *		from the specified PCI address space.
 *
 * Inputs:	skdev		- device state structure
 *		offset		- offset into PCI register space.
 *
 * Returns:	unsigned 16-bit value read from PCI register space.
 *
 */
static uint16_t
skd_pci_get16(skd_device_t *skdev, off_t offset)
{

	return (pci_config_get16(skdev->pci_handle, offset));
}

/*
 *
 * Name:	skd_pci_16, writes an unsigned 16-bit value
 *		to the specified PCI address space.
 *
 * Inputs:	skdev		- device state structure
 *		offset		- offset into PCI register space.
 *		date		- unsigned 16-bit value to write.
 *
 * Returns:	unsigned 16-bit value read from PCI register space.
 *
 */
static void
skd_pci_put16(skd_device_t *skdev, off_t offset, uint16_t data)
{

	pci_config_put16(skdev->pci_handle, offset, data);
}

/*
 *
 * Name:	skd_pci_find_capability, searches the PCI capability
 *		list for the specified capability.
 *
 * Inputs:	skdev		- device state structure.
 *		cap		- capability sought.
 *
 * Returns:	Returns position where capability was found.
 *		If not found, returns zero.
 *
 */
static int
skd_pci_find_capability(struct skd_device *skdev, int cap)
{
	uint16_t status;
	uint8_t	 pos, id;
	int	 ttl = 48;

	status = skd_pci_get16(skdev, PCI_STATUS);

	if (!(status & PCI_STATUS_CAP_LIST))
		return (0);

	switch (skdev->header_type & 0x7F) {
	case PCI_HEADER_TYPE_NORMAL:
	case PCI_HEADER_TYPE_BRIDGE:
		pos = skd_pci_get8(skdev, PCI_CAPABILITY_LIST);
		break;

	case PCI_HEADER_TYPE_CARDBUS:

		pos = skd_pci_get8(skdev, PCI_CB_CAPABILITY_LIST);
		break;

	default:
		return (0);
	}

	while (ttl-- && pos >= 0x40) {
		pos &= ~3;
		id = skd_pci_get8(skdev, pos+PCI_CAP_LIST_ID);
		if (id == 0xff)
			break;
		if (id == cap)
			return (pos);
		pos = skd_pci_get8(skdev, pos+PCI_CAP_LIST_NEXT);
	}

	return (0);
}

/*
 *
 * Name:	skd_io_done, called to conclude an I/O operation.
 *
 * Inputs:	skdev		- device state structure.
 *		bp		- buf structure.
 *		error		- contain error value.
 *		mode		- debug only.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_io_done(skd_device_t *skdev, struct buf *bp,
    int error, int mode)
{
	if (bp) {
		dev_t minor    = getminor(bp->b_edev);
		int   slice    = minor & SKD_DEV_SLICEMASK;
		skd_buf_private_t *pbuf = NULL;

		bp->b_resid = error ? bp->b_bcount : 0;

		if (bp->b_private == NULL)
			cmn_err(CE_PANIC, "NULL bp->b_private");

		pbuf = (skd_buf_private_t *)bp->b_private;

		switch (mode) {
		case SKD_IODONE_WIOC:
			skdev->iodone_wioc++;
			break;
		case SKD_IODONE_WNIOC:
			skdev->iodone_wnioc++;
			break;
		case SKD_IODONE_WDEBUG:
			skdev->iodone_wdebug++;
			break;
		default:
			skdev->iodone_unknown++;
		}

		if (error) {
			skdev->ios_errors++;

			cmn_err(CE_NOTE,
			    "%s:skd_io_done:ERR=%d %lld-%ld minor-slice:%ld-%d"
			    " %s", skdev->name, error, bp->b_lblkno,
			    bp->b_bcount, minor, slice,
			    (bp->b_flags & B_READ) ? "Read" : "Write");
		}

		if (pbuf->origin == ORIGIN_SKD) {
			bioerror(bp, error);
			kmem_free(bp->b_private, sizeof (skd_buf_private_t));
			bp->b_private = NULL;
			biodone(bp);
		} else {
			sbd_xfer_t		*xfer;

			ASSERT(pbuf->origin == ORIGIN_BLKDEV);

			xfer = pbuf->x_xfer;

			kmem_free(bp->b_private, sizeof (skd_buf_private_t));

			bp->b_private = NULL;

			freerbuf(bp);

#ifdef USE_BLKDEV
			bd_xfer_done(xfer,  error);
#else
			sbd_xfer_done(xfer,  error);
#endif
		}

	} else
		cmn_err(CE_PANIC,
		    "skd_io_done: NULL bp: SHOULD NOT BE HERE!!!!!!!!");
}

/*
 *
 * Name:	skd_capable, determines is the driver has privilege.
 *
 * Inputs:	cred_p		- cred_t data structure.
 *
 * Returns:	The result of drv_priv - zero on success, EPERM on failure
 *
 */
static int
skd_capable(cred_t *cred_p)
{
	int rval;

	rval = drv_priv(cred_p);

	return (rval);
}

/*
 * Disk info stuff
 */

/*
 *
 * Name:	skd_drivesize, returns number of drive blocks.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Drive number of blocks.
 *
 */
static uint32_t
skd_drivesize(skd_device_t *skdev)
{
	return ((uint32_t)skdev->Nblocks);
}

/*
 *
 * Name:	skd_getpartstart, gets partition start sector
 *
 * Inputs:	skdev		- device state structure.
 *		drive		- drive number.
 *		partnum		- partition number.
 *
 * Returns:	Partition start block.
 *
 */
static uint32_t
skd_getpartstart(skd_device_t *skdev, int drive, int partnum)
{
	struct ipart 	*ipart;

	if (partnum == 0)
		return (0);

	if (partnum > FD_NUMPART)
		partnum = FD_NUMPART;

	ipart = skdev->iparts[drive];

	return (ipart[partnum-1].relsect);
}

/*
 *
 * Name:	skd_partsize, returns partition block count.
 *
 * Inputs:	skdev		- device state structure.
 *		drive		- drive number.
 *		partnum		- target partition.
 *
 * Returns:	Partition's block count.
 *
 */
static uint64_t
skd_partsize(skd_device_t *skdev, int drive, int partnum)
{
	struct ipart *ipart;

	if (partnum == 0)
		return (skd_drivesize(skdev));
	else
		partnum--;

	ipart = skdev->iparts[drive];

	return (ipart[partnum].numsect);
}

#define	MODULE_NAME	"skd"
#define	LOG_BUF_LEN	128

/*
 *
 * Name:	skd_log, logs specified message to system log.
 *
 * Inputs:	arg	- device state structure.
 *		fmt	- string to log.
 *
 * Returns:	Nothing.
 *
 */
void
skd_log(void *arg, const char *fmt, ...)
{
	skd_device_t *skdp = (skd_device_t *)arg;
	char buf[LOG_BUF_LEN];
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);

	if (skdp != NULL)
		cmn_err(CE_NOTE, "!%s%d: %s", MODULE_NAME, skdp->instance, buf);
	else
		cmn_err(CE_NOTE, "!%s: %s", MODULE_NAME, buf);
}

/*
 * IOCTL
 */
static
int skd_ioctl_sg_io(struct skd_device *, struct uscsi_cmd *, int, void *);
static int skd_sg_io_get_and_check_args(struct skd_sg_io *);
static int skd_sg_io_obtain_skspcl(struct skd_device *, struct skd_sg_io *);
static int skd_sg_io_prep_buffering(struct skd_device *, struct skd_sg_io *);
static int skd_sg_io_copy_buffer(struct skd_device *, struct skd_sg_io *, int);
static int skd_sg_io_send_fitmsg(struct skd_device *, struct skd_sg_io *);
static int skd_sg_io_await(struct skd_device *, struct skd_sg_io *);
static int skd_sg_io_release_skspcl(struct skd_device *, struct skd_sg_io *);
static int skd_sg_io_put_status(struct skd_device *, struct skd_sg_io *);


/*
 *
 * Name:	skd_ioctl, process special requests.
 *		These requests maybe disk, USCSI, and debug requests.
 *
 * Inputs:	dev		- device info.
 *		cmd		- IOCTL command to process.
 *		arg		- address to send/receive data.
 *		flag		- value passed and usually just passed to
 *				  other routines (ddi_*) but also specifies
 *				  the model (32/64bit) request.
 *		cred_p		- Credentials for the caller.
 *		rval_p		- Ignored
 *
 * Returns:	Zero on success otherwise errno.
 *
 */
/* ARGSUSED */
static int
skd_ioctl(dev_t dev, int cmd, intptr_t arg, int flag, cred_t *cred_p,
    int *rval_p)
{
	struct skd_device *skdev;
	void 		*ptr = (void *)arg;
	minor_t		instance;
	minor_t		minor;
	int		slice;
	int		drive = 0;
	int 		rc = 0;
	int		ddi_model_type = 0;

	if (skd_capable(cred_p)) {
		cmn_err(CE_NOTE, "Unauthorized user request: %x", cmd);

		return (-EPERM);
	}

	instance = SKD_INST(dev);
	minor    = getminor(dev);
	slice    = minor & SKD_DEV_SLICEMASK;

	if (skd_dbg_level || skd_dbg_ioctl)
		cmn_err(CE_NOTE, "1: <## ioctl: inst=%d minor=%d slice=%d ##>",
		    instance, minor, slice);

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state, instance);
	if (skdev == NULL) {
		cmn_err(CE_NOTE, "skd_ioctl: no adapter, inst=%d minor=%x "
		    "slice=%d", instance, minor, slice);
		return (ENXIO);
	}

	ddi_model_type = ddi_model_convert_from(flag & FMODELS);

	if (skd_dbg_level || skd_dbg_ioctl)
		cmn_err(CE_NOTE, "ioctl: cmd=%x model=%x", cmd, ddi_model_type);

	rc = 0;

	switch (cmd) {
		/*
		 * Start system IOCTLs
		 */
	case DKIOCGMEDIAINFO: {
		struct dk_minfo	    minfo;
		int		    f_part;
		uint32_t	    s_numblks;

		if (skd_dbg_level || skd_dbg_ioctl)
			cmn_err(CE_NOTE, "ioctl: DKIOCGMEDIAINFO (%x)", cmd);

		if (slice >= 16)
			minor |= SKD_FULLDISK;

		drive = 0;

		if (minor & SKD_FULLDISK) {
			f_part    = minor & 0xf;
			s_numblks = skd_partsize(skdev, drive, f_part);
		} else {
			struct extvtoc *vtocptr = &skdev->extvtoc[drive];

			s_numblks  = vtocptr->v_part[slice].p_size;
		}

		bzero(&minfo, sizeof (minfo));
		minfo.dki_media_type	= DK_FIXED_DISK;
		minfo.dki_lbsize	= 512;
		minfo.dki_capacity	= s_numblks;

		if (skd_dbg_level || skd_dbg_ioctl)
			printf("MEDIAINFO: minor = %x slice=%d capacity=%lld",
			    minor, slice, minfo.dki_capacity);

		if (ddi_copyout(&minfo, ptr, sizeof (struct dk_minfo), flag)) {
			rc = EFAULT;
		}

		break;
	}
	case DKIOCGMEDIAINFOEXT: {
		struct dk_minfo_ext minfo;
		uint64_t	    s_numblks;

		if (skd_dbg_level || skd_dbg_ioctl)
			cmn_err(CE_NOTE, "ioctl: DKIOCGMEDIAINFOEXT (%x)", cmd);

		if (slice >= 16 || slice == 0)
			minor |= SKD_FULLDISK;

		drive = 0;

		if (minor & SKD_FULLDISK) {
			s_numblks = skdev->Nblocks;
		} else {
			struct extvtoc *vtocptr = &skdev->extvtoc[drive];

			s_numblks  = vtocptr->v_part[slice].p_size;
		}

		bzero(&minfo, sizeof (minfo));
		minfo.dki_media_type = DK_FIXED_DISK;
		minfo.dki_lbsize = (1U << skdev->d_blkshift);
		if (slice == PART_BACKUP || slice == PART_WD)
			s_numblks = skdev->Nblocks;
		minfo.dki_capacity = s_numblks;
		minfo.dki_pbsize = 512;

		if (skd_dbg_ioctl)
			printf(
			    "MEDIAINFOEXT: minor = %x slice=%d capacity=%llu",
			    minor, slice, minfo.dki_capacity);

		if (ddi_copyout(&minfo, ptr, sizeof (struct dk_minfo_ext),
		    flag))  {
			rc = EFAULT;
		}

		break;
	}
	case DKIOC_GETDISKID: {
		dk_disk_id_t ddid, *ddidp = &ddid;

		if (skd_dbg_level || skd_dbg_ioctl)
			cmn_err(CE_NOTE, "ioctl DKIOC_GETDISKID (%x)", cmd);

		bzero(ddidp, sizeof (*ddidp));

		ddidp->dkd_dtype = DKD_SCSI_TYPE;
		(void) memcpy(ddidp->disk_id.scsi_disk_id.dkd_svendor, "sTec",
		    4);
		(void) memcpy(ddidp->disk_id.scsi_disk_id.dkd_sproduct,
		    skdev->inq_product_id, strlen(skdev->inq_product_id));

		(void) memcpy(ddidp->disk_id.scsi_disk_id.dkd_sfwver,
		    skdev->inq_product_rev, strlen(skdev->inq_product_rev));

		(void) memcpy(ddidp->disk_id.scsi_disk_id.dkd_sserial,
		    skdev->inq_serial_num, strlen(skdev->inq_serial_num));

		if (ddi_copyout(ddidp, ptr, sizeof (dk_disk_id_t), flag))
			rc = EFAULT;

		break;
	}
#ifdef SOLARIS11_1
	case DKIOCFEATURE: {
		dk_feature_t	feature;

		if (skd_dbg_level || skd_dbg_ioctl)
			cmn_err(CE_NOTE, "ioctl DKIOCFEATURE (%x)", cmd);

		feature = DKF_IS_SSD;

		rc = 0;

		if (ddi_copyout(&feature, ptr, sizeof (dk_feature_t), flag))
			rc = EFAULT;

		break;
	}
#endif

		/*
		 * SG3 USCSICMD IOCTLS
		 */
	case USCSICMD: {
		struct uscsi_cmd   ucmd,   *ucmdp   = &ucmd;

		if (skd_dbg_level || skd_dbg_ioctl)
			cmn_err(CE_NOTE, "ioctl USCSICMD (%x) s1120 state=%x",
			    cmd, skdev->state);

		switch (skdev->state) {
		case SKD_DRVR_STATE_ONLINE:
			rc = 0;
			break;
		default:
			cmn_err(CE_NOTE, "%s: drive not online", skdev->name);
			rc = -ENXIO;
		}

		if (rc)
			break;

		switch (ddi_model_type) {
		case DDI_MODEL_ILP32: {
			struct uscsi_cmd32 ucmd32, *ucmd32p = &ucmd32;

			if (ddi_copyin((void *)arg, &ucmd32,
				sizeof (struct uscsi_cmd32), flag)) {
				cmn_err(CE_WARN,
				    "Could not copy in USCSICMD info?");
				rc = EFAULT;
			}

			uscsi_cmd32touscsi_cmd(ucmd32p, ucmdp);

			break;
		}
		case DDI_MODEL_NONE:
			if (ddi_copyin((void *)arg, &ucmd,
				sizeof (struct uscsi_cmd), flag)) {
				cmn_err(CE_WARN,
				    "Could not copy in USCSICMD info?");
				rc = EFAULT;
			}
			break;
		}

		if (rc)
			return (EINVAL);

		rc = skd_ioctl_sg_io(skdev, ucmdp, flag, (void *)arg);

		switch (ddi_model_type) {
		case DDI_MODEL_ILP32: {
			struct uscsi_cmd32 ucmd32, *ucmd32p = &ucmd32;

			uscsi_cmdtouscsi_cmd32(ucmdp, ucmd32p);

			if (ddi_copyout(ucmd32p, (void *)arg,
				sizeof (struct uscsi_cmd32), flag)) {
				cmn_err(CE_WARN, "copyout uscsi FAILED");
				rc = EFAULT;
			}


			break;
		}
		case DDI_MODEL_NONE:
			if (ddi_copyout(ucmdp, (void *)arg,
				sizeof (struct uscsi_cmd), flag)) {
				cmn_err(CE_WARN,
				    "Could not copy OUT USCSICMD info?");
				rc = EFAULT;
			}
			break;
		}

		break;
	}
		/*
		 * STEC SDM IOCTLS
		 */
	case SG_GET_VERSION_NUM: {
		int sg_version = 30527;

		if (skd_dbg_level || skd_dbg_ioctl)
			cmn_err(CE_NOTE, "ioctl SG_GET_VERSION_NUM (%x)", cmd);

		if (ddi_copyout(&sg_version, ptr, sizeof (sg_version), flag)) {
			rc = EFAULT;
		}

		break;
	}
	case SG_IO: {
		sg_io_hdr_t	   sih,	  *sihp	  = &sih;
		sg_io_hdr32_t	   sih32, *sih32p = &sih32;
		struct uscsi_cmd   ucmd,  *ucmdp  = &ucmd;

		if (skd_dbg_level || skd_dbg_ioctl)
			cmn_err(CE_NOTE, "ioctl SG_IO (%x)", cmd);

		/*
		 * Need to convert Linux-type structure request to Solaris-type
		 * request; i.e., sg_io_hdr - > uscsi_cmd
		 */
		switch (ddi_model_type) {
		case DDI_MODEL_ILP32: {

			if (ddi_copyin((void *)arg, &sih32p,
			    sizeof (sg_io_hdr32_t), flag)) {
				cmn_err(CE_WARN,
				    "Could not copy in sg_io_hdr32 info?");
				rc = EFAULT;
			}

			sgiohdr32to_sg_io_hdr(sih32p, sihp);

			break;
		}
		case DDI_MODEL_NONE:
			if (ddi_copyin((void *)arg, sihp,
			    sizeof (sg_io_hdr_t), flag)) {
				cmn_err(CE_WARN,
				    "Could not copy in sg_io_hdr info?");
				rc = EFAULT;
			}
			break;
		}

		if (rc)
			return (EINVAL);

		ucmdp->uscsi_cdblen	= sihp->cmd_len;
		ucmdp->uscsi_rqlen	= sihp->mx_sb_len;
		ucmdp->uscsi_buflen	= sihp->dxfer_len;
		ucmdp->uscsi_bufaddr	= sihp->dxferp;
		ucmdp->uscsi_cdb	= sihp->cmdp;
		ucmdp->uscsi_rqbuf	= sihp->sbp;
		ucmdp->uscsi_rqstatus	= sihp->status;
		ucmdp->uscsi_rqresid	= sihp->sb_len_wr;
		ucmdp->uscsi_resid	= sihp->resid;

		rc = skd_ioctl_sg_io(skdev, ucmdp, flag, (void *)arg);

		switch (ddi_model_type) {
		case DDI_MODEL_ILP32: {
			sg_io_hdrtosgiohdr32(sihp, sih32p);

			if (ddi_copyout(sih32p, (void *)arg,
			    sizeof (sg_io_hdr32_t), flag)) {
				cmn_err(CE_WARN, "copyout uscsi FAILED");
				rc = EFAULT;
			}


			break;
		}
		case DDI_MODEL_NONE:
			if (ddi_copyout(sihp, (void *)arg,
			    sizeof (sg_io_hdr_t), flag)) {
				cmn_err(CE_WARN,
				    "Could not copy OUT USCSICMD info?");
				rc = EFAULT;
			}
			break;
		}
		break;
	}
	default:

		rc = -ENOTTY;

		break;
	}

	if (skd_dbg_level || skd_dbg_ioctl)
		cmn_err(CE_NOTE, "%s IOCTL %x:  completion rc %d",
		    skdev->name, cmd, rc);

	return (rc);
}

/*
 *
 * Name:	skd_ioctl_sg_io, calls all routines to carry out the
 *		SCSI request.
 *
 * Inputs:	skdev		- device state structure.
 *		ucmdp		- SCSI command structure.
 *		flags		- flags variable passed to skd_ioctl.
 *		argp		- the argp argument passed to skd_ioctl.
 *
 * Returns:	-ENXIO, if the device state is not ONLINE or BUSY_IMMINENT.
 *		EFAULT if unable to copy SCSI cdb.
 *		Value is otherwise returned by other SCSI processing routines
 *		calls, e.g. 0 on success.
 *
 */
static int
skd_ioctl_sg_io(struct skd_device *skdev, struct uscsi_cmd *ucmdp,
    int flags, void *argp)
{
	struct skd_sg_io sksgio, *sksgiop = &sksgio;
	int		 rc;

	switch (skdev->state) {
	case SKD_DRVR_STATE_ONLINE:
	case SKD_DRVR_STATE_BUSY_IMMINENT:
		Dcmn_err(CE_NOTE, "USCICMD: ONLINE");
		break;
	default:
		rc = -ENXIO;
		cmn_err(CE_NOTE, "USCSICMD: drive NOT ONLINE");
		goto out2;
	}

	bzero(&sksgio, sizeof (sksgio));

	sksgiop->ucmdp = ucmdp;
	sksgiop->iov   = &sksgiop->no_iov_iov;
	sksgiop->argp  = argp;
	sksgiop->flags = flags;

	if (ddi_copyin((void *)ucmdp->uscsi_cdb, sksgiop->cdb,
	    ucmdp->uscsi_cdblen, flags)) {
		cmn_err(CE_WARN, "Could not copy in USCSICMD cdb info");
		rc = EFAULT;

		goto out2;
	}

	if (((rc = skd_sg_io_get_and_check_args(&sksgio)) != 0) ||
	    ((rc = skd_sg_io_obtain_skspcl(skdev, &sksgio)) != 0)) {
		cmn_err(CE_NOTE, "USCSICMD: failed arg check 1");
		goto out2;
	}

	if ((rc = skd_sg_io_prep_buffering(skdev, &sksgio)) != 0) {
		cmn_err(CE_NOTE, "USCSICMD: failed buf prep");
		goto out;
	}

	if ((rc = skd_sg_io_copy_buffer(skdev, &sksgio, SG_DXFER_TO_DEV)) !=
	    0) {
		cmn_err(CE_NOTE, "USCSICMD: failed copy buffer -->");
		goto out;
	}

	if ((rc = skd_sg_io_send_fitmsg(skdev, &sksgio)) != 0) {
		cmn_err(CE_NOTE, "USCSICMD: FAILED send I/O");
		goto out;
	}

	if ((rc = skd_sg_io_await(skdev, &sksgio)) != 0) {
		cmn_err(CE_NOTE, "USCSICMD: FAILED send I/O");
		goto out;
	}

	if ((rc = skd_sg_io_copy_buffer(skdev, &sksgio, SG_DXFER_FROM_DEV))
	    != 0) {
		cmn_err(CE_NOTE, "USCSICMD: FAILED copy buffer <--");
		goto out;
	}

	if ((rc = skd_sg_io_put_status(skdev, &sksgio)) != 0) {
		cmn_err(CE_NOTE, "USCSICMD: FAILED put status");
		goto out;
	}

	/* It worked! */
	rc = 0;

out:
	(void) skd_sg_io_release_skspcl(skdev, &sksgio);

	if (sksgio.iov != NULL && sksgio.iov != &sksgio.no_iov_iov) {
		kmem_free(sksgio.iov, sksgio.iovcnt);
	}
out2:

	return (rc);
}

/*
 *
 * Name:	skd_sg_io_get_and_check_args, verifies direction and
 *		transfer size of request.
 *
 * Inputs:	sksgio		- request structure.
 *
 * Returns:	-EINVAL on invalid data transfer size request.
 *		Zero on success.
 *
 *
 */
static int
skd_sg_io_get_and_check_args(struct skd_sg_io *sksgio)
{
	struct uscsi_cmd  *ucmdp = sksgio->ucmdp;
	uint32_t	   dir;

	sksgio->dxfer_len = ucmdp->uscsi_buflen;

	dir = ucmdp->uscsi_flags & 0xf;
	sksgio->dxfer_direction = (dir & USCSI_READ) ?
	    SG_DXFER_FROM_DEV : SG_DXFER_TO_DEV;
	if (ucmdp->uscsi_buflen == 0)
		sksgio->dxfer_direction = SG_DXFER_NONE;

	if (sksgio->dxfer_direction != SG_DXFER_NONE) {
		sksgio->sg.dxfer_direction = (dir & USCSI_READ) ?
		    SG_DXFER_FROM_DEV : SG_DXFER_TO_DEV;
	}

	/* unsigned compare */
	if (sksgio->dxfer_len > (PAGE_SIZE * SKD_N_SG_PER_SPECIAL)) {
		cmn_err(CE_NOTE, "dxfer_len invalid %ld\n", sksgio->dxfer_len);

		return (-EINVAL);
	}

	sksgio->iov[0].iov_base = ucmdp->uscsi_bufaddr;
	sksgio->iov[0].iov_len  = ucmdp->uscsi_buflen;
	sksgio->iovcnt = 1;

	return (0);
}

/*
 *
 * Name:	skd_sg_io_obtain_skspcl, obtains a skspcl structure.
 *
 * Inputs:	skdev		- device state structure.
 *		sksgio		- request structure.
 *
 * Returns:	Zero upon success, EIO upon failure.
 *
 */
static int
skd_sg_io_obtain_skspcl(struct skd_device *skdev,
    struct skd_sg_io *sksgio)
{
	struct skd_special_context *skspcl = NULL;
	int tries;

	for (tries = 0; tries < SKD_MAX_RETRIES; tries++) {
		/*
		 * The skspcl_free_list is also manipulated by interrupt
		 * handlers.  Use the INTR_LOCK to insure no races.
		 */
		ASSERT(!WAITQ_LOCK_HELD(skdev, 0, 0));
		INTR_LOCK(skdev);
		skspcl = skdev->skspcl_free_list;
		if (skspcl != NULL) {
			skdev->skspcl_free_list =
			    (struct skd_special_context *)skspcl->req.next;
		}
		INTR_UNLOCK(skdev);

		if (skspcl != NULL)
			break;

		delay(1 * drv_usectohz(1000));
	}

	if (skspcl == NULL)
		return (EIO);

	skspcl->req.id += SKD_ID_INCR;
	skspcl->req.state = SKD_REQ_STATE_SETUP;
	skspcl->orphaned = 0;
	skspcl->req.n_sg = 0;
	sksgio->skspcl = skspcl;

	return (0);
}

/*
 *
 * Name:	skd_sg_io_prep_buffering, allocates buffer space.
 *
 * Inputs:	skdev		- device state structure.
 *		sksgio		- request structure.
 *
 * Returns:	ENOMEM if unable to allocate space.
 *
 */
static int
skd_sg_io_prep_buffering(struct skd_device *skdev,
    struct skd_sg_io *sksgio)
{
	struct skd_special_context *skspcl = sksgio->skspcl;
	u32			   resid = sksgio->dxfer_len;
	int			   rc = 0;

	resid += (-resid) & 3;

	/*
	 * The DMA engine must have aligned addresses and byte counts.
	 */
	skspcl->sg_byte_count = resid;

	skspcl->req.n_sg = 0;

	while (resid > 0) {
		u32			 nbytes = PAGE_SIZE;
		u32			 ix	= skspcl->req.n_sg;
		struct scatterlist	 *sg	= &skspcl->req.sg[ix];
		struct fit_sg_descriptor *sksg	= &skspcl->req.sksg_list[ix];
		char			 *page;
		dma_mem_t		 *mem;

		if (nbytes > resid)
			nbytes = resid;

		mem 			= &skspcl->page_dma_address;
		mem->alignment  	= SG_BOUNDARY;
		mem->cookie_count 	= 1;
		mem->size 		= PAGE_SIZE;
		mem->type 		= LITTLE_ENDIAN_DMA;

		page = skd_alloc_dma_mem(skdev, mem, KM_SLEEP, ATYPE_64BIT);
		if (page == NULL) {
			rc = -ENOMEM;
			goto err_out;
		}

		bzero(page, nbytes);

		sg->page   = page;
		sg->length = nbytes;
		sg->offset = 0;


		sksg->control = FIT_SGD_CONTROL_NOT_LAST;
		sksg->byte_count = nbytes;

		sksg->host_side_addr = mem->cookies->dmac_laddress;


		sksg->dev_side_addr = 0;
		sksg->next_desc_ptr =
		    skspcl->req.sksg_dma_address.cookies->dmac_laddress +
		    (ix+1) * sizeof (*sksg);

		skspcl->req.n_sg++;
		resid -= nbytes;
	}

	if (skspcl->req.n_sg > 0) {
		u32 ix = skspcl->req.n_sg - 1;
		struct fit_sg_descriptor *sksg = &skspcl->req.sksg_list[ix];

		sksg->control = FIT_SGD_CONTROL_LAST;
		sksg->next_desc_ptr = 0LL;
	}

err_out:
	return (rc);
}

/*
 *
 * Name:	skd_addr, sums passed in values.
 *
 * Inputs:	base		- base address
 *		nbytes		- value to add to base.
 *
 * Returns:	base + nbytes.
 *
 */
size_t
skd_addr(size_t base, size_t nbytes)
{
	size_t sum;

	sum = base + nbytes;

	return (sum);
}

/*
 *
 * Name:	skd_sg_io_copy_buffer, copies the user's buffer to
 *		request s/g page.
 *
 * Inputs:	skdev		- device state structure.
 *		skdgio		- request structure.
 *		dxfer_dir	- direction of data transfer.
 *
 * Returns:	-EFAULT if unable to copy in/out from/to user's buffer.
 *		Zero is able to copy to/frem user's buffer.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_sg_io_copy_buffer(struct skd_device *skdev,
    struct skd_sg_io *sksgio, int dxfer_dir)
{
	struct skd_special_context *skspcl = sksgio->skspcl;
	u32			   iov_ix = 0;
	struct sg_iovec		   curiov;
	u32			   sksg_ix = 0;
	char			   *bufp = NULL;
	u32			   buf_len = 0;
	u32			   resid = sksgio->dxfer_len;
	int			   rc = 0;

	curiov.iov_len  = 0;
	curiov.iov_base = NULL;

	if (dxfer_dir != sksgio->sg.dxfer_direction) {
		if (dxfer_dir != SG_DXFER_TO_DEV ||
		    sksgio->sg.dxfer_direction != SG_DXFER_TO_FROM_DEV) {
			return (0);
		}
	}

	while (resid > 0) {
		u32 nbytes = PAGE_SIZE;

		if (curiov.iov_len == 0) {
			/* load next entry */
			curiov = sksgio->iov[iov_ix++];
			continue; /* for iov entries that are 0 length */
		}


		if (buf_len == 0) {
			char *page;

			page = skspcl->req.sg[sksg_ix++].page;
			bufp = page;
			buf_len = PAGE_SIZE;
		}

		nbytes = min_t(u32, nbytes, resid);
		nbytes = min_t(u32, nbytes, curiov.iov_len);
		nbytes = min_t(u32, nbytes, buf_len);

		if (dxfer_dir == SG_DXFER_TO_DEV) {
			if (ddi_copyin((void *)curiov.iov_base, bufp, nbytes,
			    sksgio->flags)) {
				cmn_err(CE_WARN,
				    "uscsi: Could not copy IN buffer");
				rc = EFAULT;
			}
		} else {
			if (ddi_copyout(bufp, curiov.iov_base, nbytes,
			    sksgio->flags)) {
				cmn_err(CE_WARN,
				    "uscsi: Could not copy OUT buffer");
				rc =  EFAULT;
			}
		}

		if (rc) {
			return (-EFAULT);
		}

		resid		-= nbytes;
		curiov.iov_len	-= nbytes;
		curiov.iov_base =
		    (caddr_t)skd_addr((size_t)curiov.iov_base, nbytes);
		buf_len		-= nbytes;
	}

	return (0);
}

/*
 *
 * Name:	skd_sg_io_send_fitmsg, send the FIT msg to hardware.
 *
 * Inputs:	skdev		- device state structure.
 *		sksgio		- request structure.
 *
 * Returns:	Zero - success.
 *
 */
static int
skd_sg_io_send_fitmsg(struct skd_device *skdev,
    struct skd_sg_io *sksgio)
{
	struct skd_special_context *skspcl = sksgio->skspcl;
	struct fit_msg_hdr	   *fmh;
	struct skd_scsi_request	   *scsi_req;
	uint64_t		   dma_address;


	bzero(skspcl->msg_buf, SKD_N_SPECIAL_FITMSG_BYTES);

	/*
	 * Initialize the FIT msg header
	 */
	fmh = (struct fit_msg_hdr *)skspcl->msg_buf64;
	fmh->protocol_id = FIT_PROTOCOL_ID_SOFIT;
	fmh->num_protocol_cmds_coalesced = 1;

	scsi_req = (struct skd_scsi_request *)&fmh[1];
	bzero(scsi_req, sizeof (*scsi_req));

	/* Initialize the SCSI request */
	if (sksgio->sg.dxfer_direction != SG_DXFER_NONE) {
		dma_address =
		    skspcl->req.sksg_dma_address.cookies->_dmu._dmac_ll;
		scsi_req->hdr.sg_list_dma_address = cpu_to_be64(dma_address);
	}

	scsi_req->hdr.tag = skspcl->req.id;
	scsi_req->hdr.sg_list_len_bytes = cpu_to_be32(skspcl->sg_byte_count);
	(void) memcpy(scsi_req->cdb, sksgio->cdb, sizeof (scsi_req->cdb));

	skspcl->req.state = SKD_REQ_STATE_BUSY;

	if (skd_dbg_ioctl)
		cmn_err(CE_NOTE, "USCSI----->: req.id=%x", skspcl->req.id);

	skd_send_special_fitmsg(skdev, skspcl);

	return (0);
}

/*
 *
 * Name:	skd_sg_io_await, waits for I/O completion.
 *
 * Inputs:	skdev		- device state structure
 *		sksgio		- request structure.
 *
 * Returns:	-ETIMEDOUT if I/O timed out.
 *		EINTR if user cancelled.
 *		Zero on successful I/O completion.
 *
 */
static int
skd_sg_io_await(struct skd_device *skdev, struct skd_sg_io *sksgio)
{
	clock_t	cur_ticks, tmo, msecs, ticks, cntr = 0;
	int rc = 1, rval;

	if (skd_dbg_ioctl)
		cmn_err(CE_NOTE,
		    "<=== IO_AWAIT - WAITING  cdb[0]=%x  ===>", *sksgio->cdb);

	if (*sksgio->cdb == SCMD_FORMAT) {
		/*
		 * We'll need to come up with a formula
		 * based of disk size.
		 */
		msecs = 60 * 5;
		ticks = 1000000 * 1;	/* 1 sec */
	} else {
		msecs = 60 * 3;
		ticks = 100000 * 1;
	}

	mutex_enter(&skdev->skd_internalio_mutex);

	for (;;) {
		if (skdev->state != SKD_DRVR_STATE_BUSY_ERASE)
			if (sksgio->skspcl->req.state != SKD_REQ_STATE_BUSY)
				break;

		cur_ticks = ddi_get_lbolt();
		tmo = cur_ticks + drv_usectohz(ticks);
		if ((rval = cv_timedwait(&skdev->cv_waitq,
		    &skdev->skd_internalio_mutex, tmo)) == -1) {
			/* Oops - timed out */

			if (cntr++ > msecs) {
				rc = 0;

				break;
			}
		}

		if (rval >= 0)
			break;
	}

	mutex_exit(&skdev->skd_internalio_mutex);

	if (skd_dbg_ioctl)
		cmn_err(CE_NOTE, "<=== IO_AWAIT - DONE ===>");

	if (*sksgio->cdb == 0x04)
		cmn_err(CE_NOTE, "%s: format completed, cntr=%ld msecs=%ld",
		    skdev->name, cntr, msecs);

	if (sksgio->skspcl->req.state == SKD_REQ_STATE_ABORTED) {
		cmn_err(CE_NOTE, "skspcl %p aborted\n", (void *)sksgio->skspcl);

		/* Build check cond, sense and let command finish. */
		/*
		 * For a timeout, we must fabricate completion and sense
		 * data to complete the command
		 */
		sksgio->skspcl->req.completion.status =
		    SAM_STAT_CHECK_CONDITION;

		bzero(&sksgio->skspcl->req.err_info,
		    sizeof (sksgio->skspcl->req.err_info));

		sksgio->skspcl->req.err_info.type = 0x70;
		sksgio->skspcl->req.err_info.key = ABORTED_COMMAND;
		sksgio->skspcl->req.err_info.code = 0x44;
		sksgio->skspcl->req.err_info.qual = 0;
		rc = 0;
	} else
		if (sksgio->skspcl->req.state != SKD_REQ_STATE_BUSY) {
			/* No longer on the adapter. We finish. */
			rc = 0;
		} else {
			/*
			 * Something's gone wrong. Still busy. Timeout or
			 * user interrupted (control-C). Mark as an orphan
			 * so it will be disposed when completed.
			 */
			sksgio->skspcl->orphaned = 1;
			sksgio->skspcl = NULL;
			if (rc == 0) {
				cmn_err(CE_NOTE,
				    "skd_sg_io_await: timed out %p (%u ms)\n",
				    (void *)sksgio, sksgio->sg.timeout);
				rc = -ETIMEDOUT;
			} else {
				cmn_err(CE_NOTE, "cntlc %p\n", (void *)sksgio);
				rc = -EINTR;
			}
		}

	return (rc);
}

/*
 *
 * Name:	skd_sg_io_put_status, copies error buf to user if
 *		an error was encountered.
 *
 * Inputs:	skdev		- device state structure.
 *		sksgio		- request structure.
 *
 * Returns:	-EFAULT if unable to copy out error info buffer.
 *		Zero on successful transfer or error buffer or
 *		no error occurred during I/O operation.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_sg_io_put_status(struct skd_device *skdev,
    struct skd_sg_io *sksgio)
{
	struct uscsi_cmd	   *ucmdp;
	struct skd_special_context *skspcl = sksgio->skspcl;
	int			   resid = 0;
	u32			   nb =
	    be32_to_cpu(skspcl->req.completion.num_returned_bytes);

	ucmdp = sksgio->ucmdp;

	ucmdp->uscsi_rqstatus	= skspcl->req.completion.status;
	ucmdp->uscsi_status	= skspcl->req.completion.status;
	resid			= sksgio->dxfer_len - nb;
	ucmdp->uscsi_resid	= resid;

	if (skd_dbg_ioctl)
		cmn_err(CE_NOTE, "uscsi status %x", ucmdp->uscsi_status);

	if (ucmdp->uscsi_rqstatus == SAM_STAT_CHECK_CONDITION) {
		if (ucmdp->uscsi_rqlen > 0) {
			struct fit_comp_error_info *ei = &skspcl->req.err_info;
			u32 nbytes = sizeof (*ei);

			nbytes = min_t(u32, nbytes, ucmdp->uscsi_rqlen);

			ucmdp->uscsi_rqresid = (uchar_t)nbytes;

			if (ddi_copyout(ei, ucmdp->uscsi_rqbuf,
			    ucmdp->uscsi_rqlen, sksgio->flags)) {
				cmn_err(CE_NOTE, "copyout sense failed");

				return (-EFAULT);
			}
		}
	}

	return (0);
}

/*
 *
 * Name:	skd_sg_io_release_skspcl, releases the skspcl
 *		data structure.
 *
 * Inputs:	skdev		- device state structure.
 *		sksgio		- request structure.
 *
 * Returns:	Zero.
 *
 */
static int
skd_sg_io_release_skspcl(struct skd_device *skdev,
    struct skd_sg_io *sksgio)
{
	struct skd_special_context *skspcl = sksgio->skspcl;

	if (skspcl != NULL) {
		sksgio->skspcl = NULL;

		ASSERT(!WAITQ_LOCK_HELD(skdev, 0, 0));
		INTR_LOCK(skdev); /* KEBE ASKS RIGHT LOCK? */
		skd_release_special(skdev, skspcl);
		INTR_UNLOCK(skdev); /* KEBE ASKS RIGHT LOCK? */
	}

	return (0);
}

/*
 * QUIESCE DEVICE
 */

/*
 *
 * Name:	skd_sys_quiesce_dev, quiets the device
 *
 * Inputs:	dip		- dev info strucuture
 *
 * Returns:	Zero.
 *
 */
static int
skd_sys_quiesce_dev(dev_info_t *dip)
{
	skd_device_t	*skdev;

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state,
	    ddi_get_instance(dip));

	Dcmn_err(CE_NOTE, "%s: stopping queue", skdev->name);
	skdev->quiesced = 1;

	return (0);
}

/*
 *
 * Name:	skd_quiesce_dev, quiets the device, but doesn't really do much.
 *
 * Inputs:	skdev		- Device state.
 *
 * Returns:	-EINVAL if device is not in proper state otherwise
 *		returns zero.
 *
 */
static int
skd_quiesce_dev(skd_device_t *skdev)
{
	int rc = 0;

	if (skd_dbg_level)
		Dcmn_err(CE_NOTE, "skd_quiece_dev:");

	switch (skdev->state) {
	case SKD_DRVR_STATE_BUSY:
	case SKD_DRVR_STATE_BUSY_IMMINENT:
		Dcmn_err(CE_NOTE, "%s: stopping queue", skdev->name);
		break;
	case SKD_DRVR_STATE_ONLINE:
	case SKD_DRVR_STATE_STOPPING:
	case SKD_DRVR_STATE_SYNCING:
	case SKD_DRVR_STATE_PAUSING:
	case SKD_DRVR_STATE_PAUSED:
	case SKD_DRVR_STATE_STARTING:
	case SKD_DRVR_STATE_RESTARTING:
	case SKD_DRVR_STATE_RESUMING:
	default:
		rc = -EINVAL;
		cmn_err(CE_NOTE, "state [%d] not implemented", skdev->state);
	}

	return (rc);
}

/*
 * UNQUIESCE DEVICE:
 * Note: Assumes lock is held to protect device state.
 */
/*
 *
 * Name:	skd_unquiesce_dev, awkens the device
 *
 * Inputs:	skdev		- Device state.
 *
 * Returns:	-EINVAL if device is not in proper state otherwise
 *		returns zero.
 *
 */
static int
skd_unquiesce_dev(struct skd_device *skdev)
{
	Dcmn_err(CE_NOTE, "skd_unquiece_dev:");

	skd_log_skdev(skdev, "unquiesce");
	if (skdev->state == SKD_DRVR_STATE_ONLINE) {
		Dcmn_err(CE_NOTE, "**** device already ONLINE");

		return (0);
	}
	if (skdev->drive_state != FIT_SR_DRIVE_ONLINE) {
		/*
		 * If there has been an state change to other than
		 * ONLINE, we will rely on controller state change
		 * to come back online and restart the queue.
		 * The BUSY state means that driver is ready to
		 * continue normal processing but waiting for controller
		 * to become available.
		 */
		skdev->state = SKD_DRVR_STATE_BUSY;
		cmn_err(CE_NOTE, "drive BUSY state\n");

		return (0);
	}
	/*
	 * Drive just come online, driver is either in startup,
	 * paused performing a task, or bust waiting for hardware.
	 */
	switch (skdev->state) {
	case SKD_DRVR_STATE_PAUSED:
	case SKD_DRVR_STATE_BUSY:
	case SKD_DRVR_STATE_BUSY_IMMINENT:
	case SKD_DRVR_STATE_BUSY_ERASE:
	case SKD_DRVR_STATE_STARTING:
	case SKD_DRVR_STATE_RESTARTING:
	case SKD_DRVR_STATE_FAULT:
	case SKD_DRVR_STATE_IDLE:
	case SKD_DRVR_STATE_LOAD:
	skdev->state = SKD_DRVR_STATE_ONLINE;
	Dcmn_err(CE_NOTE, "%s: sTec s1120 ONLINE", skdev->name);
	Dcmn_err(CE_NOTE, "%s: Starting request queue", skdev->name);
	Dcmn_err(CE_NOTE,
	    "%s: queue depth limit=%d hard=%d soft=%d lowat=%d",
	    skdev->name,
	    skdev->queue_depth_limit,
	    skdev->hard_queue_depth_limit,
	    skdev->soft_queue_depth_limit,
	    skdev->queue_depth_lowat);

	cv_signal(&skdev->cv_waitq);

	skdev->gendisk_on = 1;
	break;
	case SKD_DRVR_STATE_DISAPPEARED:
	default:
	cmn_err(CE_NOTE, "**** driver state %d, not implemented \n",
	    skdev->state);
	return (-EBUSY);
	}

	return (0);
}

/*
 * READ/WRITE REQUESTS
 */

/*
 *
 * Name:	skd_blkdev_preop_sg_list, builds the S/G list from info
 *		passed in by the sblkdev driver.
 *
 * Inputs:	skdev		- device state structure.
 *		skreq		- request structure.
 *		sg_byte_count	- data transfer byte count.
 *
 * Returns:	EAGAIN if unable to allocate DMA space otherwise
 *		return zero.
 *
 */
static int
skd_blkdev_preop_sg_list(struct skd_device *skdev,
    struct skd_request_context *skreq, u32* sg_byte_count)
{
	struct buf  		*bp;
	sbd_xfer_t		*xfer;
	ddi_dma_cookie_t	*cookiep;
	skd_buf_private_t 	*pbuf;
	int 			i, bcount = 0;
	uint_t 			n_sg;

	*sg_byte_count = 0;

	ASSERT(skreq->sg_data_dir == SKD_DATA_DIR_HOST_TO_CARD ||
	    skreq->sg_data_dir == SKD_DATA_DIR_CARD_TO_HOST);

	bp   = skreq->bp;

	ASSERT(bp != NULL);

	pbuf = (skd_buf_private_t *)bp->b_private;
	xfer = pbuf->x_xfer;
	n_sg = xfer->x_ndmac;

	ASSERT(n_sg <= skdev->sgs_per_request);

	skreq->n_sg = n_sg;

	skreq->io_dma_handle = xfer->x_dmah;

	skreq->total_sg_bcount = 0;

	for (i = 0; i < n_sg; i++) {
		struct fit_sg_descriptor *sgd;
		size_t   cnt;
		uint64_t dma_addr;


		cookiep = &xfer->x_dmac;
		cnt = cookiep->dmac_size;
		if (cnt <= (size_t)0)
			break;

		bcount += cnt;
		dma_addr = cookiep->dmac_laddress;

		if ((uint_t)dma_addr == 0xbaddcafe)
			return (EAGAIN);

		sgd			= &skreq->sksg_list[i];
		sgd->control		= FIT_SGD_CONTROL_NOT_LAST;
		sgd->byte_count		= (uint32_t)cnt;
		sgd->host_side_addr	= dma_addr;
		sgd->dev_side_addr	= 0; /* not used */
		*sg_byte_count		+= cnt;

		skreq->total_sg_bcount += cnt;
		if ((int)sgd->host_side_addr == 0xbaddcafe)
			return (EAGAIN);

		if ((i + 1) != n_sg)
			ddi_dma_nextcookie(skreq->io_dma_handle, &xfer->x_dmac);

		(void) ddi_dma_sync(skreq->io_dma_handle, 0, 0,
		    DDI_DMA_SYNC_FORCPU);
	}

	skreq->sksg_list[n_sg - 1].next_desc_ptr = 0LL;
	skreq->sksg_list[n_sg - 1].control = FIT_SGD_CONTROL_LAST;

	return (0);
}

/*
 *
 * Name:	skd_blkdev_postop_sg_list, deallocates DMA
 *
 * Inputs:	skdev		- device state structure.
 *		skreq		- skreq data structure.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_blkdev_postop_sg_list(struct skd_device *skdev,
    struct skd_request_context *skreq)
{

	if (skreq->bp) {
		(void) ddi_dma_sync(skreq->io_dma_handle, 0, 0,
		    DDI_DMA_SYNC_FORCPU);
	}

	/*
	 * restore the next ptr for next IO request so we
	 * don't have to set it every time.
	 */
	skreq->sksg_list[skreq->n_sg - 1].next_desc_ptr =
	    skreq->sksg_dma_address.cookies->dmac_laddress +
	    ((skreq->n_sg) * sizeof (struct fit_sg_descriptor));
}

/*
 *
 * Name:	skd_start, initiates an I/O.
 *
 * Inputs:	skdev		- device state structure.
 *		id		- identifies caller.
 *
 * Returns:	EAGAIN if devicfe is not ONLINE.
 *		On error, if the caller is the sblkdev driver, return
 *		the error value. Otherwise, return zero.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_start(skd_device_t *skdev, int id)
{
	struct skd_fitmsg_context	*skmsg = NULL;
	struct fit_msg_hdr		*fmh = NULL;
	struct skd_request_context	*skreq = NULL;
	struct buf			*bp = NULL;
	struct skd_scsi_request		*scsi_req;
	skd_buf_private_t		*pbuf = NULL;
	int				error, drive = 0;
	int				bcount;

	u32				lba;
	u32				count;
	u64				be_dmaa;
	u64				cmdctxt;
	u32				timo_slot;
	void				*cmd_ptr;
	u32				sg_byte_count = 0;

	if (skdev->state != SKD_DRVR_STATE_ONLINE) {
		Dcmn_err(CE_NOTE, "Device - not ONLINE");

		skd_request_fn_not_online(skdev);

		return (EAGAIN);
	}

	/*
	 * Stop conditions:
	 *  - There are no more native requests
	 *  - There are already the maximum number of requests is progress
	 *  - There are no more skd_request_context entries
	 *  - There are no more FIT msg buffers
	 */
	for (;;) {
		WAITQ_LOCK(skdev, 0, drive);

		bp = skd_blk_peek_request(skdev, drive);

		/* Are there any native requests to start? */
		if (bp == NULL) {
			WAITQ_UNLOCK(skdev, 0, drive);

			break;
		}

		pbuf = (skd_buf_private_t *)bp->b_private;

		/* Are too many requests already in progress? */
		if (skdev->queue_depth_busy >= skdev->queue_depth_limit) {
			cmn_err(CE_NOTE, "qdepth %d, limit %d\n",
			    /* Dcmn_err(CE_NOTE, "qdepth %d, limit %d\n", */
			    skdev->queue_depth_busy,
			    skdev->queue_depth_limit);

			WAITQ_UNLOCK(skdev, 0, drive);

			break;
		}

		/* Is a skd_request_context available? */
		if (NULL == (skreq = skdev->skreq_free_list)) {

			cmn_err(CE_NOTE, "Out of req=%p\n", (void *)bp);

			WAITQ_UNLOCK(skdev, 0, drive);

			break;
		}

		if (skreq->state != SKD_REQ_STATE_IDLE) {
			cmn_err(CE_NOTE, "ASSERT 1: %d-%d", skreq->state,
			    SKD_REQ_STATE_IDLE);
			WAITQ_UNLOCK(skdev, 0, drive);
			continue;
		}

		if (skreq->id & SKD_ID_INCR) {
			cmn_err(CE_NOTE,
			    "ASSERT x: %d", skreq->id & SKD_ID_INCR);
			WAITQ_UNLOCK(skdev, 0, drive);
			continue;
		}

/* ASSERT PLUG */
		ASSERT(SKD_REQ_STATE_IDLE == skreq->state);
		ASSERT(0 == (skreq->id & SKD_ID_INCR));

		/* Either a FIT msg is in progress or we have to start one. */
		if (NULL == skmsg) {
			/* Are there any FIT msg buffers available? */
			skmsg = skdev->skmsg_free_list;
			if (NULL == skmsg) {
				Dcmn_err(CE_NOTE, "Out of msg req=%p\n",
				    (void *)bp);

				if ((skmsg = skdev->skmsg_free_list) == NULL) {
					WAITQ_UNLOCK(skdev, 0, drive);

					break;
				}
			}

			if (skmsg->state != SKD_MSG_STATE_IDLE) {
				cmn_err(CE_NOTE, "ASSERT 3: %d-%d",
				    skmsg->state, SKD_MSG_STATE_IDLE);
				WAITQ_UNLOCK(skdev, 0, drive);
				continue;
			}

			if (skmsg->id & SKD_ID_INCR) {
				cmn_err(CE_NOTE, "ASSERT 4: %x",
				    skmsg->id & SKD_ID_INCR);
				WAITQ_UNLOCK(skdev, 0, drive);
				continue;
			}


/* ASSERT PLUG */
			ASSERT(SKD_MSG_STATE_IDLE == skmsg->state);
			ASSERT(0 == (skmsg->id & SKD_ID_INCR));

			skdev->skmsg_free_list = skmsg->next;

			skmsg->state = SKD_MSG_STATE_BUSY;
			skmsg->id += SKD_ID_INCR;

			/* Initialize the FIT msg header */
			fmh = (struct fit_msg_hdr *)skmsg->msg_buf64;
			bzero(fmh, sizeof (*fmh)); /* Too expensive */
			fmh->protocol_id = FIT_PROTOCOL_ID_SOFIT;
			skmsg->length = sizeof (*fmh);
		}

		/*
		 * At this point we are committed to either start or reject
		 * the native request. Note that skd_request_context is
		 * available but is still at the head of the free list.
		 * Note also that a FIT msg may have just been started
		 * but contains no SoFIT requests yet.
		 * Now - dequeue bp.
		 */
		if (!(bp = skd_get_queued_bp(skdev, drive))) {
			WAITQ_UNLOCK(skdev, 0, drive);
			break;
		}

		skreq->bp	= bp;
		bp->b_flags	&= ~B_ERROR;
		bp->b_error	= 0;
		bp->b_resid	= bp->b_bcount;
		lba		= (u32)bp->b_lblkno;
		count		= (u32)bp->b_bcount >> 9;
		skreq->did_complete = 0;

		skreq->fitmsg_id = skmsg->id;

		Dcmn_err(CE_NOTE,
		    "bp=%p lba=%u(0x%x) count=%u(0x%x) bcount=%lu b_flags=%x\n",
		    (void *)bp, lba, lba, count, count, bp->b_bcount,
		    bp->b_flags);

		/*
		 * Transcode the request, checking as we go. The outcome of
		 * the transcoding is represented by the error variable.
		 */
		cmd_ptr = &skmsg->msg_buf[skmsg->length];
		bzero(cmd_ptr, 32); /* This is too expensive */

		be_dmaa  = cpu_to_be64(
		    (u64)skreq->sksg_dma_address.cookies->dmac_laddress);
		cmdctxt  = skreq->id + SKD_ID_INCR;

		scsi_req = cmd_ptr;
		scsi_req->hdr.tag = (uint16_t)cmdctxt;
		scsi_req->hdr.sg_list_dma_address = be_dmaa;
		scsi_req->cdb[1] = 0;
		scsi_req->cdb[2] = (lba & 0xff000000) >> 24;
		scsi_req->cdb[3] = (lba & 0xff0000) >> 16;
		scsi_req->cdb[4] = (lba & 0xff00) >> 8;
		scsi_req->cdb[5] = (lba & 0xff);
		scsi_req->cdb[6] = 0;
		scsi_req->cdb[7] = (count & 0xff00) >> 8;
		scsi_req->cdb[8] = count & 0xff;
		scsi_req->cdb[9] = 0;

		if (bp->b_flags & B_READ) {
			scsi_req->cdb[0] = 0x28;
			skreq->sg_data_dir = SKD_DATA_DIR_CARD_TO_HOST;
		} else {
			scsi_req->cdb[0] = 0x2a;
			skreq->sg_data_dir = SKD_DATA_DIR_HOST_TO_CARD;
		}

		if (pbuf->origin == ORIGIN_SKD)
			error = skd_preop_sg_list(skdev, skreq, &sg_byte_count);
		else {
			ASSERT(pbuf->origin == ORIGIN_BLKDEV);
			error = skd_blkdev_preop_sg_list(skdev, skreq,
			    &sg_byte_count);
		}

		if (error != 0) {
			/*
			 * Complete the native request with error.
			 * Note that the request context is still at the
			 * head of the free list, and that the SoFIT request
			 * was encoded into the FIT msg buffer but the FIT
			 * msg length has not been updated. In short, the
			 * only resource that has been allocated but might
			 * not be used is that the FIT msg could be empty.
			 */
			cmn_err(CE_NOTE, "skd_start: error=%d", error);

			skd_end_request(skdev, skreq, error);
			WAITQ_UNLOCK(skdev, 0, drive);
			continue;
		}

		scsi_req->hdr.sg_list_len_bytes = cpu_to_be32(sg_byte_count);

		bcount = (sg_byte_count + 511) / 512;
		scsi_req->cdb[7] = (bcount & 0xff00) >> 8;
		scsi_req->cdb[8] =  bcount & 0xff;

		bp->b_bcount = bcount << 9;

		/*
		 * Complete resource allocations.
		 */
		skdev->skreq_free_list = skreq->next;

		skreq->state = SKD_REQ_STATE_BUSY;
		skreq->id += SKD_ID_INCR;

		Dcmn_err(CE_NOTE,
		    "skd_start: bp=%p skreq->id=%x cmdctxt=%" PRIx64 " "
		    "opc=%x ====>>>>>",
		    (void *)bp, skreq->id, cmdctxt, *scsi_req->cdb);

		skmsg->length += sizeof (struct skd_scsi_request);
		fmh->num_protocol_cmds_coalesced++;

		/*
		 * Update the active request counts.
		 * Capture the timeout timestamp.
		 */
		skreq->timeout_stamp = skdev->timeout_stamp;
		timo_slot = skreq->timeout_stamp & SKD_TIMEOUT_SLOT_MASK;

		skdev->timeout_slot[timo_slot]++;
		skdev->queue_depth_busy++;

		Dcmn_err(CE_NOTE, "req=0x%x busy=%d timo_slot=%d",
		    skreq->id, skdev->queue_depth_busy, timo_slot);
		/*
		 * If the FIT msg buffer is full send it.
		 */
		if (skmsg->length >= SKD_N_FITMSG_BYTES ||
		    fmh->num_protocol_cmds_coalesced >= skd_max_req_per_msg) {

			(void) ddi_dma_sync(skreq->io_dma_handle, 0, 0,
			    DDI_DMA_SYNC_FORDEV);

			skdev->active_cmds++;
			pbuf = (skd_buf_private_t *)bp->b_private;
			pbuf->skreq = (u64 *)skreq;

			skdev->fitmsg_sent1++;
			skd_send_fitmsg(skdev, skmsg, bp);
			skmsg = NULL;
			fmh = NULL;
		}

		WAITQ_UNLOCK(skdev, 0, drive);
	}

	/*
	 * Is a FIT msg in progress? If it is empty put the buffer back
	 * on the free list. If it is non-empty send what we got.
	 * This minimizes latency when there are fewer requests than
	 * what fits in a FIT msg.
	 */
	if (NULL != skmsg) {
		/* Bigger than just a FIT msg header? */
		if (skmsg->length > 64) {
			Dcmn_err(CE_NOTE, "sending msg=%p, len %d",
			    (void *)skmsg, skmsg->length);

			(void) ddi_dma_sync(skreq->io_dma_handle, 0, 0,
			    DDI_DMA_SYNC_FORDEV);

			skdev->active_cmds++;

			skdev->fitmsg_sent2++;
			skd_send_fitmsg(skdev, skmsg, bp);
		} else {
			/*
			 * The FIT msg is empty. It means we got started
			 * on the msg, but the requests were rejected.
			 */
			skmsg->state = SKD_MSG_STATE_IDLE;
			skmsg->id += SKD_ID_INCR;
			WAITQ_LOCK(skdev, 0, drive);
			skmsg->next = skdev->skmsg_free_list;

			skdev->skmsg_free_list = skmsg;
			WAITQ_UNLOCK(skdev, 0, drive);
		}
		skmsg = NULL;
		fmh = NULL;
	}

	/*
	 * If req is non-NULL it means there is something to do but
	 * we are out of a resource.
	 */
	if (NULL != bp && pbuf != NULL) {
		skd_end_request(skdev,
		    (struct skd_request_context *)pbuf->skreq, EAGAIN);
	}

	return (0);
}

/*
 *
 * Name:	skd_end_request
 *
 * Inputs:	skdev		- device state structure.
 *		skreq		- request structure.
 *		error		- I/O error value.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_end_request(struct skd_device *skdev,
    struct skd_request_context *skreq, int error)
{
	skdev->ios_completed++;
	skd_io_done(skdev, skreq->bp, error, SKD_IODONE_WIOC);
	skreq->bp = NULL;
	skreq->did_complete = 1;
}

/*
 *
 * Name:	skd_end_request_abnormal
 *
 * Inputs:	skdev		- device state structure.
 *		bp		- buf structure.
 *		error		- I/O error value.
 *		mode		- debug
 *
 * Returns:	Nothing.
 *
 */
static void
skd_end_request_abnormal(struct skd_device *skdev,
    struct buf *bp, int error, int mode)
{
	skd_io_done(skdev, bp, error, mode);
}

/*
 *
 * Name:	skd_preop_sg_list, builds the S/G list.
 *
 * Inputs:	skdev		- device state structure.
 *		skreq		- request structure.
 *		sg_byte_count	- data transfer byte count.
 *
 * Returns:	-EINVAL if unable to allocate dma handle.
 *		EAGAIN if unable to allocate DMA space otherwise
 *		return zero.
 *
 */
static int
skd_preop_sg_list(struct skd_device *skdev,
    struct skd_request_context *skreq, u32* sg_byte_count)
{
	struct buf  		*bp = skreq->bp;
	int 			i, status, rval = 0;
	uint_t 			n_sg, dir;
	ddi_dma_cookie_t	cookies, *cookiep = &cookies;

	*sg_byte_count = 0;

	ASSERT(skreq->sg_data_dir == SKD_DATA_DIR_HOST_TO_CARD ||
	    skreq->sg_data_dir == SKD_DATA_DIR_CARD_TO_HOST);

	dir = DDI_DMA_CONSISTENT;

	if (ddi_dma_alloc_handle(skdev->dip, &skd_32bit_io_dma_attr,
	    DDI_DMA_SLEEP, NULL, &skreq->io_dma_handle) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "preop_sg_list: alloc handle FAILED");

		return (-EINVAL);
	}

	dir |= (bp->b_flags & B_READ) ? DDI_DMA_READ : DDI_DMA_WRITE;
	status = ddi_dma_buf_bind_handle(skreq->io_dma_handle, bp, dir, NULL,
	    NULL, &cookies, &n_sg);

	switch (status) {
	case DDI_DMA_MAPPED:
	case DDI_DMA_PARTIAL:
		break;
	case DDI_DMA_NORESOURCES:
		rval = EAGAIN;
		goto preop_failure;
	case DDI_DMA_TOOBIG:
		rval = EINVAL;
		goto preop_failure;
	case DDI_DMA_NOMAPPING:
	case DDI_DMA_INUSE:
	default:
		rval = EFAULT;
		goto preop_failure;
	}

	ASSERT(n_sg <= skdev->sgs_per_request);

	skreq->n_sg = n_sg;

	for (i = 0; i < n_sg; i++) {
		struct fit_sg_descriptor *sgd;
		size_t   cnt;
		uint64_t dma_addr;

		cnt = cookiep->dmac_size;
		if (cnt == 0)
			break;

		dma_addr = cookiep->dmac_laddress;

		sgd = &skreq->sksg_list[i];
		sgd->control = FIT_SGD_CONTROL_NOT_LAST;
		sgd->byte_count = (uint32_t)cnt;
		*sg_byte_count += cnt;
		sgd->host_side_addr = dma_addr;
		sgd->dev_side_addr = 0; /* not used */

		ddi_dma_nextcookie(skreq->io_dma_handle, &cookies);
	}

	skreq->sksg_list[n_sg - 1].next_desc_ptr = 0LL;
	skreq->sksg_list[n_sg - 1].control = FIT_SGD_CONTROL_LAST;

	if (skd_dbg_level > 1) {
		cmn_err(CE_NOTE, "skreq=%x sksg_list=%p sksg_dma=%" PRIx64 "\n",
		    skreq->id, (void *)skreq->sksg_list,
		    skreq->sksg_dma_address.cookie.dmac_laddress);
		for (i = 0; i < n_sg; i++) {
			struct fit_sg_descriptor *sgd =
			    &skreq->sksg_list[i];
			cmn_err(CE_NOTE, "  sg[%d] count=%u ctrl=0x%x "
			    "addr=0x%" PRIx64 " next=0x%" PRIx64 "\n",
			    i, sgd->byte_count, sgd->control,
			    sgd->host_side_addr,
			    sgd->next_desc_ptr);
		}
	}

	return (0);

preop_failure:

	cmn_err(CE_NOTE, "preop: allocate buf DMA resources FAILED");

	(void) ddi_dma_unbind_handle(skreq->io_dma_handle);
	ddi_dma_free_handle(&skreq->io_dma_handle);

	return (rval);
}

/*
 *
 * Name:	skd_post_op_sg_list, frees allocated DMA resources.
 *
 * Inputs:	skdev		- device state structure.
 *		skreq		- request structure.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_postop_sg_list(struct skd_device *skdev,
    struct skd_request_context *skreq)
{
	if (skreq->bp) {
		(void) ddi_dma_sync(skreq->io_dma_handle, 0, 0,
		    DDI_DMA_SYNC_FORCPU);
	}

	/*
	 * restore the next ptr for next IO request so we
	 * don't have to set it every time.
	 */
	skreq->sksg_list[skreq->n_sg - 1].next_desc_ptr =
	    skreq->sksg_dma_address.cookies->dmac_laddress +
	    ((skreq->n_sg) * sizeof (struct fit_sg_descriptor));

	/*
	 * Free All DMA resources allocated for this I/O.
	 */
	if (ddi_dma_unbind_handle(skreq->io_dma_handle) != DDI_SUCCESS)
		cmn_err(CE_NOTE, "postop: dma_unbind_handle - FAILED");
	else
		ddi_dma_free_handle(&skreq->io_dma_handle);
}

/*
 *
 * Name:	skd_request_fn_not_online, handles the condition
 *		of the device not being online.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	nothing (void).
 *
 */
static void
skd_request_fn_not_online(skd_device_t *skdev)
{
	int error, drive = 0;
	struct buf *bp;

	ASSERT(skdev->state != SKD_DRVR_STATE_ONLINE);

	skd_log_skdev(skdev, "req_not_online");

	switch (skdev->state) {
	case SKD_DRVR_STATE_PAUSING:
	case SKD_DRVR_STATE_PAUSED:
	case SKD_DRVR_STATE_STARTING:
	case SKD_DRVR_STATE_RESTARTING:
	case SKD_DRVR_STATE_WAIT_BOOT:
		/*
		 * In case of starting, we haven't started the queue,
		 * so we can't get here... but requests are
		 * possibly hanging out waiting for us because we
		 * reported the dev/skd/0 already.  They'll wait
		 * forever if connect doesn't complete.
		 * What to do??? delay dev/skd/0 ??
		 */
	case SKD_DRVR_STATE_BUSY:
	case SKD_DRVR_STATE_BUSY_IMMINENT:
	case SKD_DRVR_STATE_BUSY_ERASE:
	case SKD_DRVR_STATE_DRAINING_TIMEOUT:
		return;

	case SKD_DRVR_STATE_BUSY_SANITIZE:
	case SKD_DRVR_STATE_STOPPING:
	case SKD_DRVR_STATE_SYNCING:
	case SKD_DRVR_STATE_FAULT:
	case SKD_DRVR_STATE_DISAPPEARED:
	default:
		error = -EIO;
		break;
	}

	/*
	 * If we get here, terminate all pending block requeusts
	 * with EIO and any scsi pass thru with appropriate sense
	 */
	for (;;) {
		bp = skd_get_queued_bp(skdev, drive);
		if (bp == NULL)
			break;

		skd_end_request_abnormal(skdev, bp, error, SKD_IODONE_WNIOC);

		cv_signal(&skdev->cv_waitq);
	}
}

/*
 * TIMER
 */

static void skd_timer_tick_not_online(struct skd_device *skdev);

/*
 *
 * Name:	skd_timer_tick, monitors requests for timeouts.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_timer_tick(skd_device_t *skdev)
{
	u32 timo_slot;

	skdev->timer_active = 1;

	if (skdev->state != SKD_DRVR_STATE_ONLINE) {
		skd_timer_tick_not_online(skdev);
		goto timer_func_out;
	}

	skdev->timeout_stamp++;
	timo_slot = skdev->timeout_stamp & SKD_TIMEOUT_SLOT_MASK;

	/*
	 * All requests that happened during the previous use of
	 * this slot should be done by now. The previous use was
	 * over 7 seconds ago.
	 */
	if (skdev->timeout_slot[timo_slot] == 0) {
		goto timer_func_out;
	}

	/* Something is overdue */
	cmn_err(CE_NOTE, "found %d timeouts, draining busy=%d",
	    skdev->timeout_slot[timo_slot],
	    skdev->queue_depth_busy);
	skdev->timer_countdown = SKD_TIMER_SECONDS(3);
	skdev->state = SKD_DRVR_STATE_DRAINING_TIMEOUT;
	skdev->timo_slot = timo_slot;

timer_func_out:
	skdev->timer_active = 0;
}

/*
 *
 * Name:	skd_timer_tick_not_online, handles various device
 *		state transitions.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_timer_tick_not_online(struct skd_device *skdev)
{
	Dcmn_err(CE_NOTE, "skd_skd_timer_tick_not_online: state=%d tmo=%d",
	    skdev->state, skdev->timer_countdown);
	switch (skdev->state) {
	case SKD_DRVR_STATE_IDLE:
	case SKD_DRVR_STATE_LOAD:
		break;
	case SKD_DRVR_STATE_BUSY_SANITIZE:
		cmn_err(CE_NOTE, "drive busy sanitize[%x], driver[%x]\n",
		    skdev->drive_state, skdev->state);
		break;

	case SKD_DRVR_STATE_BUSY:
	case SKD_DRVR_STATE_BUSY_IMMINENT:
	case SKD_DRVR_STATE_BUSY_ERASE:
		Dcmn_err(CE_NOTE, "busy[%x], countdown=%d\n",
		    skdev->state, skdev->timer_countdown);
		if (skdev->timer_countdown > 0) {
			skdev->timer_countdown--;
			return;
		}
		cmn_err(CE_NOTE, "busy[%x], timedout=%d, restarting device.",
		    skdev->state, skdev->timer_countdown);
		skd_restart_device(skdev);
		break;

	case SKD_DRVR_STATE_WAIT_BOOT:
	case SKD_DRVR_STATE_STARTING:
		if (skdev->timer_countdown > 0) {
			skdev->timer_countdown--;
			return;
		}
		/*
		 * For now, we fault the drive.  Could attempt resets to
		 * revcover at some point.
		 */
		skdev->state = SKD_DRVR_STATE_FAULT;

		cmn_err(CE_NOTE, "(%s): DriveFault Connect Timeout (%x)",
		    skd_name(skdev), skdev->drive_state);

		/* start the queue so we can respond with error to requests */
		(void) skd_start(skdev, 1);

		/* wakeup anyone waiting for startup complete */
		skdev->gendisk_on = -1;

		cv_signal(&skdev->cv_waitq);
		break;


	case SKD_DRVR_STATE_ONLINE:
		/* shouldn't get here. */
		break;

	case SKD_DRVR_STATE_PAUSING:
	case SKD_DRVR_STATE_PAUSED:
		break;

	case SKD_DRVR_STATE_DRAINING_TIMEOUT:
		cmn_err(CE_NOTE,
		    "%s: draining busy [%d] tick[%d] qdb[%d] tmls[%d]\n",
		    skdev->name,
		    skdev->timo_slot,
		    skdev->timer_countdown,
		    skdev->queue_depth_busy,
		    skdev->timeout_slot[skdev->timo_slot]);
		/* if the slot has cleared we can let the I/O continue */
		if (skdev->timeout_slot[skdev->timo_slot] == 0) {
			cmn_err(CE_NOTE, "Slot drained, starting queue.");
			skdev->state = SKD_DRVR_STATE_ONLINE;
			(void) skd_start(skdev, 2);
			return;
		}
		if (skdev->timer_countdown > 0) {
			skdev->timer_countdown--;
			return;
		}
		skd_restart_device(skdev);
		break;

	case SKD_DRVR_STATE_RESTARTING:
		if (skdev->timer_countdown > 0) {
			skdev->timer_countdown--;

			return;
		}
		/*
		 * For now, we fault the drive. Could attempt resets to
		 * revcover at some point.
		 */
		skdev->state = SKD_DRVR_STATE_FAULT;
		cmn_err(CE_NOTE, "(%s): DriveFault Reconnect Timeout (%x)\n",
		    skd_name(skdev), skdev->drive_state);

		/*
		 * Recovering does two things:
		 * 1. completes IO with error
		 * 2. reclaims dma resources
		 * When is it safe to recover requests?
		 * - if the drive state is faulted
		 * - if the state is still soft reset after out timeout
		 * - if the drive registers are dead (state = FF)
		 */

		if ((skdev->drive_state == FIT_SR_DRIVE_SOFT_RESET) ||
		    (skdev->drive_state == FIT_SR_DRIVE_FAULT) ||
		    (skdev->drive_state == FIT_SR_DRIVE_STATE_MASK)) {
			/*
			 * It never came out of soft reset. Try to
			 * recover the requests and then let them
			 * fail. This is to mitigate hung processes.
			 *
			 * Acquire the interrupt lock since these lists are
			 * manipulated by interrupt handlers.
			 */
			ASSERT(!WAITQ_LOCK_HELD(skdev, 0, 0));
			INTR_LOCK(skdev);
			skd_recover_requests(skdev);
			INTR_UNLOCK(skdev);
		}
		/* start the queue so we can respond with error to requests */
		(void) skd_start(skdev, 3);
		/* wakeup anyone waiting for startup complete */
		skdev->gendisk_on = -1;
		cv_signal(&skdev->cv_waitq);
		break;

	case SKD_DRVR_STATE_RESUMING:
	case SKD_DRVR_STATE_STOPPING:
	case SKD_DRVR_STATE_SYNCING:
	case SKD_DRVR_STATE_FAULT:
	case SKD_DRVR_STATE_DISAPPEARED:
	default:
		break;
	}
}

/*
 *
 * Name:	skd_timer, kicks off the timer processing.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_timer(void *arg)
{
	skd_device_t *skdev = (skd_device_t *)arg;

	/* Someone set us to 0, don't bother rescheduling. */
	ADAPTER_STATE_LOCK(skdev);
	if (skdev->skd_timer_timeout_id != 0) {
		ADAPTER_STATE_UNLOCK(skdev);
		/* Pardon the drop-and-then-acquire logic here. */
		skd_timer_tick(skdev);
		ADAPTER_STATE_LOCK(skdev);
		/* Restart timer, if not being stopped. */
		if (skdev->skd_timer_timeout_id != 0) {
			skdev->skd_timer_timeout_id =
			    timeout(skd_timer, arg, skd_timer_ticks);
		}
	}
	ADAPTER_STATE_UNLOCK(skdev);
}

/*
 *
 * Name:	skd_start_timer, kicks off the 1-second timer.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Zero.
 *
 */
static void
skd_start_timer(struct skd_device *skdev)
{
	/* Start one second driver timer. */
	ADAPTER_STATE_LOCK(skdev);
	if (skdev->skd_timer_timeout_id == 0) {
		/*
		 * Do first "timeout tick" right away, but not in this
		 * thread.
		 */
		skdev->skd_timer_timeout_id = (timeout_id_t)timeout(skd_timer,
		    (void *)skdev, 1);
	} else {
		cmn_err(CE_WARN, "skd: skd_start_timer() called on");
	}
	ADAPTER_STATE_UNLOCK(skdev);
}

/*
 * INTERNAL REQUESTS -- generated by driver itself
 */

/*
 *
 * Name:	skd_format_internal_skspcl, setups the internal
 *		FIT request message.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	One.
 *
 */
static int
skd_format_internal_skspcl(struct skd_device *skdev)
{
	struct skd_special_context *skspcl = &skdev->internal_skspcl;
	struct fit_sg_descriptor *sgd = &skspcl->req.sksg_list[0];
	struct fit_msg_hdr *fmh;
	uint64_t dma_address;
	struct skd_scsi_request *scsi;

	fmh = (struct fit_msg_hdr *)&skspcl->msg_buf64[0];
	fmh->protocol_id = FIT_PROTOCOL_ID_SOFIT;
	fmh->num_protocol_cmds_coalesced = 1;

	/* Instead of 64-bytes in, use 8-(64-bit-words) for linted alignment. */
	scsi = (struct skd_scsi_request *)&skspcl->msg_buf64[8];
	bzero(scsi, sizeof (*scsi));
	dma_address = skspcl->req.sksg_dma_address.cookies->_dmu._dmac_ll;
	scsi->hdr.sg_list_dma_address = cpu_to_be64(dma_address);
	sgd->control = FIT_SGD_CONTROL_LAST;
	sgd->byte_count = 0;
	sgd->host_side_addr = skspcl->db_dma_address.cookies->_dmu._dmac_ll;
	sgd->dev_side_addr = 0; /* not used */
	sgd->next_desc_ptr = 0LL;

	return (1);
}

/*
 *
 * Name:	skd_send_internal_skspcl, send internal requests to
 *		the hardware.
 *
 * Inputs:	skdev		- device state structure.
 *		skspcl		- request structure
 *		opcode		- just what it says
 *
 * Returns:	Nothing.
 *
 */
void
skd_send_internal_skspcl(struct skd_device *skdev,
    struct skd_special_context *skspcl, u8 opcode)
{
	struct fit_sg_descriptor *sgd = &skspcl->req.sksg_list[0];
	struct skd_scsi_request *scsi;

	if (SKD_REQ_STATE_IDLE != skspcl->req.state) {
		/*
		 * A refresh is already in progress.
		 * Just wait for it to finish.
		 */
		return;
	}

	ASSERT(0 == (skspcl->req.id & SKD_ID_INCR));
	skspcl->req.state = SKD_REQ_STATE_BUSY;
	skspcl->req.id += SKD_ID_INCR;

	/* Instead of 64-bytes in, use 8-(64-bit-words) for linted alignment. */
	scsi = (struct skd_scsi_request *)&skspcl->msg_buf64[8];
	scsi->hdr.tag = skspcl->req.id;

	Dcmn_err(CE_NOTE, "internal skspcl: opcode=%x req.id=%x ==========>",
	    opcode, skspcl->req.id);

	switch (opcode) {
	case TEST_UNIT_READY:
		scsi->cdb[0] = TEST_UNIT_READY;
		scsi->cdb[1] = 0x00;
		scsi->cdb[2] = 0x00;
		scsi->cdb[3] = 0x00;
		scsi->cdb[4] = 0x00;
		scsi->cdb[5] = 0x00;
		sgd->byte_count = 0;
		scsi->hdr.sg_list_len_bytes = 0;
		break;
	case READ_CAPACITY_EXT:
		scsi->cdb[0]  = READ_CAPACITY_EXT;
		scsi->cdb[1]  = 0x10;
		scsi->cdb[2]  = 0x00;
		scsi->cdb[3]  = 0x00;
		scsi->cdb[4]  = 0x00;
		scsi->cdb[5]  = 0x00;
		scsi->cdb[6]  = 0x00;
		scsi->cdb[7]  = 0x00;
		scsi->cdb[8]  = 0x00;
		scsi->cdb[9]  = 0x00;
		scsi->cdb[10] = 0x00;
		scsi->cdb[11] = 0x00;
		scsi->cdb[12] = 0x00;
		scsi->cdb[13] = 0x20;
		scsi->cdb[14] = 0x00;
		scsi->cdb[15] = 0x00;
		sgd->byte_count = SKD_N_READ_CAP_EXT_BYTES;
		scsi->hdr.sg_list_len_bytes = cpu_to_be32(sgd->byte_count);
		break;
	case 0x28:
		(void) memset(skspcl->data_buf, 0x65, SKD_N_INTERNAL_BYTES);

		scsi->cdb[0] = 0x28;
		scsi->cdb[1] = 0x00;
		scsi->cdb[2] = 0x00;
		scsi->cdb[3] = 0x00;
		scsi->cdb[4] = 0x00;
		scsi->cdb[5] = 0x00;
		scsi->cdb[6] = 0x00;
		scsi->cdb[7] = 0x00;
		scsi->cdb[8] = 0x01;
		scsi->cdb[9] = 0x00;
		sgd->byte_count = SKD_N_INTERNAL_BYTES;
		scsi->hdr.sg_list_len_bytes = cpu_to_be32(SKD_N_INTERNAL_BYTES);
		break;
	case INQUIRY:
		scsi->cdb[0] = INQUIRY;
		scsi->cdb[1] = 0x01; /* evpd */
		scsi->cdb[2] = 0x80; /* serial number page */
		scsi->cdb[3] = 0x00;
		scsi->cdb[4] = 0x10;
		scsi->cdb[5] = 0x00;
		sgd->byte_count = 16; /* SKD_N_INQ_BYTES */;
		scsi->hdr.sg_list_len_bytes = cpu_to_be32(sgd->byte_count);
		break;
	case INQUIRY2:
		scsi->cdb[0] = INQUIRY;
		scsi->cdb[1] = 0x00;
		scsi->cdb[2] = 0x00; /* serial number page */
		scsi->cdb[3] = 0x00;
		scsi->cdb[4] = 0x24;
		scsi->cdb[5] = 0x00;
		sgd->byte_count = 36; /* SKD_N_INQ_BYTES */;
		scsi->hdr.sg_list_len_bytes = cpu_to_be32(sgd->byte_count);
		break;
	case SYNCHRONIZE_CACHE:
		scsi->cdb[0] = SYNCHRONIZE_CACHE;
		scsi->cdb[1] = 0x00;
		scsi->cdb[2] = 0x00;
		scsi->cdb[3] = 0x00;
		scsi->cdb[4] = 0x00;
		scsi->cdb[5] = 0x00;
		scsi->cdb[6] = 0x00;
		scsi->cdb[7] = 0x00;
		scsi->cdb[8] = 0x00;
		scsi->cdb[9] = 0x00;
		sgd->byte_count = 0;
		scsi->hdr.sg_list_len_bytes = 0;
		break;
	default:
		ASSERT("Don't know what to send");
		return;

	}

	skd_send_special_fitmsg(skdev, skspcl);
}

/*
 *
 * Name:	skd_refresh_device_data, sends a TUR command.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_refresh_device_data(struct skd_device *skdev)
{
	struct skd_special_context *skspcl = &skdev->internal_skspcl;

	Dcmn_err(CE_NOTE, "refresh_device_data: state=%d", skdev->state);

	skd_send_internal_skspcl(skdev, skspcl, TEST_UNIT_READY);
}

/*
 *
 * Name:	skd_complete_internal, handles the completion of
 *		driver-initiated I/O requests.
 *
 * Inputs:	skdev		- device state structure.
 *		skcomp		- completion structure.
 *		skerr		- error structure.
 *		skspcl		- request structure.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_complete_internal(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr,
    struct skd_special_context *skspcl)
{
	u8 *buf = skspcl->data_buf;
	u8 status = 2;
	int i;
	/* Instead of 64-bytes in, use 8-(64-bit-words) for linted alignment. */
	struct skd_scsi_request *scsi =
	    (struct skd_scsi_request *)&skspcl->msg_buf64[8];

	ASSERT(skspcl == &skdev->internal_skspcl);

	Dcmn_err(CE_NOTE, "complete internal %x", scsi->cdb[0]);

	skspcl->req.completion = *skcomp;
	skspcl->req.state = SKD_REQ_STATE_IDLE;
	skspcl->req.id += SKD_ID_INCR;

	status = skspcl->req.completion.status;

	Dcmn_err(CE_NOTE, "<<<<====== complete_internal: opc=%x", *scsi->cdb);

	switch (scsi->cdb[0]) {
	case TEST_UNIT_READY:
		if (SAM_STAT_GOOD == status) {
			skd_send_internal_skspcl(skdev, skspcl,
			    READ_CAPACITY_EXT);
		} else {
			if (skdev->state == SKD_DRVR_STATE_STOPPING) {
				cmn_err(CE_NOTE,
				    "  %s: TUR failed, don't send anymore"
				    "state 0x%x", skdev->name, skdev->state);

				return;
			}

			Dcmn_err(CE_NOTE, "%s: TUR failed, retry skerr",
			    skdev->name);
			skd_send_internal_skspcl(skdev, skspcl, 0x00);
		}
		break;
	case READ_CAPACITY_EXT: {
		uint64_t cap, Nblocks;
		uint64_t xbuf[1];

		skdev->read_cap_is_valid = 0;
		if (SAM_STAT_GOOD == status) {
			int secptrk = SKD_GEO_HEADS * SKD_GEO_SECS_TRACK;

			(void) memcpy(xbuf, buf, 8);
			cap = be64_to_cpu(*xbuf);
			skdev->read_cap_last_lba = cap;
			skdev->read_cap_blocksize =
			    (buf[8] << 24) | (buf[9] << 16) |
			    (buf[10] << 8) | buf[11];

			cap *= skdev->read_cap_blocksize;
			Dcmn_err(CE_NOTE, "  Last LBA: %" PRIu64 " (0x%" PRIx64
			    "), blk sz: %d, Capacity: %" PRIu64 "GB\n",
			    skdev->read_cap_last_lba,
			    skdev->read_cap_last_lba,
			    skdev->read_cap_blocksize,
			    cap >> 30ULL);

			Nblocks = skdev->read_cap_last_lba;

			Nblocks = (Nblocks / secptrk) * secptrk;

			skdev->Nblocks = Nblocks;
			skdev->read_cap_is_valid = 1;

			skd_send_internal_skspcl(skdev, skspcl,	INQUIRY2);

		} else {
			Dcmn_err(CE_NOTE, "**** READCAP failed, retry TUR");
			skd_send_internal_skspcl(skdev, skspcl,
			    TEST_UNIT_READY);
		}
		break;
	}
	case INQUIRY:
		skdev->inquiry_is_valid = 0;
		if (SAM_STAT_GOOD == status) {
			skdev->inquiry_is_valid = 1;

			if (scsi->cdb[1] == 0x1) {
				for (i = 0; i < 12; i++)
					skdev->inq_serial_num[i] = buf[i+4];

				skdev->inq_serial_num[12] = 0; /* null term */
			} else {
				for (i = 0; i < 16; i++)
					skdev->inq_product_id[i] = buf[i+16];
				for (i = 16; i > 0; i--) {
					if (skdev->inq_product_id[i] == ' ')
						skdev->inq_product_id[i] = 0;
					else
						if (skdev->inq_product_id[i])
							break;
				}

				for (i = 0; i < 4; i++)
					skdev->inq_product_rev[i] = buf[i+32];

				skdev->inq_product_id[15] = 0;
				skdev->inq_product_rev[4] = 0;
			}
		}

		if (skdev->state != SKD_DRVR_STATE_ONLINE)
			if (skd_unquiesce_dev(skdev) < 0)
				cmn_err(CE_NOTE, "** failed, to ONLINE device");
		break;
	case SYNCHRONIZE_CACHE:
		skdev->sync_done = (SAM_STAT_GOOD == status) ? 1 : -1;

		cv_signal(&skdev->cv_waitq);
		break;

	default:
		ASSERT("we didn't send this");
	}
}

/*
 * FIT MESSAGES
 */

/*
 *
 * Name:	skd_send_fitmsg, send a FIT message to the hardware.
 *
 * Inputs:	skdev		- device state structure.
 *		skmsg		- FIT message structure.
 *		bp		- buf request structure.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_send_fitmsg(struct skd_device *skdev,
    struct skd_fitmsg_context *skmsg,
    struct buf *bp)
{
	u64 qcmd;
	struct fit_msg_hdr *fmh;

	Dcmn_err(CE_NOTE, "msgbuf's DMA addr: 0x%" PRIx64 ", qdepth_busy=%d",
	    skmsg->mb_dma_address.cookies->dmac_laddress,
	    skdev->queue_depth_busy);

	Dcmn_err(CE_NOTE, "msg_buf 0x%p, offset %x", (void *)skmsg->msg_buf,
	    skmsg->offset);

	qcmd = skmsg->mb_dma_address.cookies->dmac_laddress;
	qcmd |= FIT_QCMD_QID_NORMAL;

	fmh = (struct fit_msg_hdr *)skmsg->msg_buf64;
	skmsg->outstanding = fmh->num_protocol_cmds_coalesced;

	if (skdev->dbg_level > 1) {
		u8 * bp = (u8 *)skmsg->msg_buf;
		int i;

		for (i = 0; i < skmsg->length; i += 8) {
			cmn_err(CE_NOTE, "  msg[%2d] %02x %02x %02x %02x "
			    "%02x %02x %02x %02x",
			    i, bp[i + 0], bp[i + 1], bp[i + 2],
			    bp[i + 3], bp[i + 4], bp[i + 5],
			    bp[i + 6], bp[i + 7]);
			if (i == 0) i = 64 - 8;
		}
	}

	if (skmsg->length > 256) {
		qcmd |= FIT_QCMD_MSGSIZE_512;
	} else if (skmsg->length > 128) {
		qcmd |= FIT_QCMD_MSGSIZE_256;
	} else if (skmsg->length > 64) {
		qcmd |= FIT_QCMD_MSGSIZE_128;
	} else {
		/*
		 * This makes no sense because the FIT msg header is
		 * 64 bytes. If the msg is only 64 bytes long it has
		 * no payload.
		 */
		qcmd |= FIT_QCMD_MSGSIZE_64;
	}

	skdev->ios_started++;

	SKD_WRITEQ(skdev, qcmd, FIT_Q_COMMAND);
}

/*
 *
 * Name:	skd_send_special_fitmsg, send a special FIT message
 *		to the hardware used for USCSI commands and driver-originated
 *		I/O requests.
 *
 * Inputs:	skdev		- device state structure.
 *		skspcl		- skspcl structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_send_special_fitmsg(struct skd_device *skdev,
    struct skd_special_context *skspcl)
{
	u64 qcmd;

	Dcmn_err(CE_NOTE, "send_special_fitmsg: pt 1");

	if (unlikely(skdev->dbg_level > 1)) {
		u8 * bp = (u8 *)skspcl->msg_buf;
		int i;

		for (i = 0; i < SKD_N_SPECIAL_FITMSG_BYTES; i += 8) {
			cmn_err(CE_NOTE,
			    "  spcl[%2d] %02x %02x %02x %02x  "
			    "%02x %02x %02x %02x\n", i,
			    bp[i + 0], bp[i + 1], bp[i + 2], bp[i + 3],
			    bp[i + 4], bp[i + 5], bp[i + 6], bp[i + 7]);
			if (i == 0) i = 64 - 8;
		}

		for (i = 0; i < skspcl->req.n_sg; i++) {
			struct fit_sg_descriptor *sgd =
			    &skspcl->req.sksg_list[i];

			cmn_err(CE_NOTE, "  sg[%d] count=%u ctrl=0x%x "
			    "addr=0x%" PRIx64 " next=0x%" PRIx64,
			    i, sgd->byte_count, sgd->control,
			    sgd->host_side_addr, sgd->next_desc_ptr);
		}
	}

	/*
	 * Special FIT msgs are always 128 bytes: a 64-byte FIT hdr
	 * and one 64-byte SSDI command.
	 */
	qcmd = skspcl->mb_dma_address.cookies->dmac_laddress;

	qcmd |= FIT_QCMD_QID_NORMAL + FIT_QCMD_MSGSIZE_128;

	SKD_WRITEQ(skdev, qcmd, FIT_Q_COMMAND);
}

/*
 * COMPLETION QUEUE
 */

static void skd_complete_other(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr);
static void skd_complete_special(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr,
    struct skd_special_context *skspcl);

struct sns_info {
	u8 type;
	u8 stat;
	u8 key;
	u8 asc;
	u8 ascq;
	u8 mask;
	enum skd_check_status_action action;
};

static struct sns_info skd_chkstat_table[] = {
	/* Good */
	{0x70, 0x02, RECOVERED_ERROR, 0, 0, 0x1c, SKD_CHECK_STATUS_REPORT_GOOD},

	/* Smart alerts */
	{0x70, 0x02, NO_SENSE, 0x0B, 0x00, 0x1E, /* warnings */
	    SKD_CHECK_STATUS_REPORT_SMART_ALERT},
	{0x70, 0x02, NO_SENSE, 0x5D, 0x00, 0x1E, /* thresholds */
	    SKD_CHECK_STATUS_REPORT_SMART_ALERT},
	{0x70, 0x02, RECOVERED_ERROR, 0x0B, 0x01, 0x1F, /* temp over trigger */
	    SKD_CHECK_STATUS_REPORT_SMART_ALERT},

	/* Retry (with limits) */
	{0x70, 0x02, 0x0B, 0, 0, 0x1C, /* This one is for DMA ERROR */
	    SKD_CHECK_STATUS_REQUEUE_REQUEST},
	{0x70, 0x02, 0x06, 0x0B, 0x00, 0x1E, /* warnings */
	    SKD_CHECK_STATUS_REQUEUE_REQUEST},
	{0x70, 0x02, 0x06, 0x5D, 0x00, 0x1E, /* thresholds */
	    SKD_CHECK_STATUS_REQUEUE_REQUEST},
	{0x70, 0x02, 0x06, 0x80, 0x30, 0x1F, /* backup power */
	    SKD_CHECK_STATUS_REQUEUE_REQUEST},

	/* Busy (or about to be) */
	{0x70, 0x02, 0x06, 0x3f, 0x01, 0x1F, /* fw changed */
	    SKD_CHECK_STATUS_BUSY_IMMINENT},
};

/*
 *
 * Name:	skd_check_status, checks the return status from a
 *		completed I/O request.
 *
 * Inputs:	skdev		- device state structure.
 *		cmp_status	- SCSI status byte.
 *		skerr		- the error data structure.
 *
 * Returns:	Depending on the error condition, return the action
 *		to be taken as specified in the skd_chkstat_table.
 *		If no corresponding value is found in the table
 *		return SKD_CHECK_STATUS_REPORT_GOOD is no error otherwise
 *		return SKD_CHECK_STATUS_REPORT_ERROR.
 *
 */
static enum skd_check_status_action
skd_check_status(struct skd_device *skdev, u8 cmp_status,
    volatile struct fit_comp_error_info *skerr)
{
	/*
	 * Look up status and sense data to decide how to handle the error
	 * from the device.
	 * mask says which fields must match e.g., mask=0x18 means check
	 * type and stat, ignore key, asc, ascq.
	 */
	int i, n;

	Dcmn_err(CE_NOTE, "(%s): key/asc/ascq %02x/%02x/%02x",
	    skd_name(skdev), skerr->key, skerr->code, skerr->qual);

	Dcmn_err(CE_NOTE, "stat: t=%02x stat=%02x k=%02x c=%02x q=%02x",
	    skerr->type, cmp_status, skerr->key, skerr->code, skerr->qual);

	/* Does the info match an entry in the good category? */
	n = sizeof (skd_chkstat_table) / sizeof (skd_chkstat_table[0]);
	for (i = 0; i < n; i++) {
		struct sns_info *sns = &skd_chkstat_table[i];

		if (sns->mask & 0x10)
			if (skerr->type != sns->type) continue;

		if (sns->mask & 0x08)
			if (cmp_status != sns->stat) continue;

		if (sns->mask & 0x04)
			if (skerr->key != sns->key) continue;

		if (sns->mask & 0x02)
			if (skerr->code != sns->asc) continue;

		if (sns->mask & 0x01)
			if (skerr->qual != sns->ascq) continue;

		if (sns->action == SKD_CHECK_STATUS_REPORT_SMART_ALERT) {
			cmn_err(CE_NOTE, "(%s):SMART Alert: sense key/asc/ascq "
			    "%02x/%02x/%02x",
			    skd_name(skdev), skerr->key,
			    skerr->code, skerr->qual);
		}

		Dcmn_err(CE_NOTE, "skd_check_status: returning %x",
		    sns->action);

		return (sns->action);
	}

	/*
	 * No other match, so nonzero status means error,
	 * zero status means good
	 */
	if (cmp_status) {
		cmn_err(CE_NOTE,
		    " %s: status check: qdepth=%d skmfl=%p (%d) skrfl=%p (%d)",
		    skdev->name,
		    skdev->queue_depth_busy,
		    (void *)skdev->skmsg_free_list, skd_list_skmsg(skdev, 0),
		    (void *)skdev->skreq_free_list, skd_list_skreq(skdev, 0));

		cmn_err(CE_NOTE, " %s: t=%02x stat=%02x k=%02x c=%02x q=%02x",
		    skdev->name, skerr->type, cmp_status, skerr->key,
		    skerr->code, skerr->qual);

		return (SKD_CHECK_STATUS_REPORT_ERROR);
	}

	cmn_err(CE_NOTE, "status check good default");

	return (SKD_CHECK_STATUS_REPORT_GOOD);
}

/*
 *
 * Name:	skd_isr_completion_posted, handles I/O completions.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_isr_completion_posted(struct skd_device *skdev)
{
	volatile struct fit_completion_entry_v1 *skcmp = NULL;
	volatile struct fit_comp_error_info  *skerr;
	struct skd_fitmsg_context 	*skmsg;
	struct skd_request_context 	*skreq;
	skd_buf_private_t 		*pbuf;
	struct buf 			*bp;
	int	drive = 0;
	u16 	req_id;
	u32 	req_slot;
	u32 	timo_slot;
	u32 	msg_slot;
	u16 	cmp_cntxt = 0;
	u8 	cmp_status = 0;
	u8 	cmp_cycle = 0;
	u32 	cmp_bytes = 0;

	for (;;) {
		ASSERT(skdev->skcomp_ix < SKD_N_COMPLETION_ENTRY);

		WAITQ_LOCK(skdev, 0, drive);

		skcmp = &skdev->skcomp_table[skdev->skcomp_ix];
		cmp_cycle = skcmp->cycle;
		cmp_cntxt = skcmp->tag;
		cmp_status = skcmp->status;
		cmp_bytes = be32_to_cpu(skcmp->num_returned_bytes);

		skerr = &skdev->skerr_table[skdev->skcomp_ix];

		Dcmn_err(CE_NOTE,
		    "cycle=%d ix=%d got cycle=%d cmdctxt=0x%x stat=%d "
		    "qdepth_busy=%d rbytes=0x%x proto=%d",
		    skdev->skcomp_cycle, skdev->skcomp_ix,
		    cmp_cycle, cmp_cntxt, cmp_status,
		    skdev->queue_depth_busy, cmp_bytes, skdev->proto_ver);

		if (cmp_cycle != skdev->skcomp_cycle) {
			Dcmn_err(CE_NOTE, "%s:end of completions", skdev->name);

			WAITQ_UNLOCK(skdev, 0, drive);
			break;
		}


		skdev->n_req++;

		/*
		 * Update the completion queue head index and possibly
		 * the completion cycle count.
		 */
		skdev->skcomp_ix++;
		if (skdev->skcomp_ix >= SKD_N_COMPLETION_ENTRY) {
			skdev->skcomp_ix = 0;
			skdev->skcomp_cycle++; /* 8-bit wrap-around */
		}


		/*
		 * The command context is a unique 32-bit ID. The low order
		 * bits help locate the request. The request is usually a
		 * r/w request (see skd_start() above) or a special request.
		 */
		req_id   = cmp_cntxt;
		req_slot = req_id & SKD_ID_SLOT_AND_TABLE_MASK;

		Dcmn_err(CE_NOTE,
		    "<<<< completion_posted 1: req_id=%x req_slot=%x",
		    req_id, req_slot);

		/* Is this other than a r/w request? */
		if (req_slot >= skdev->num_req_context) {
			/*
			 * This is not a completion for a r/w request.
			 */
			skd_complete_other(skdev, skcmp, skerr);
			WAITQ_UNLOCK(skdev, 0, drive);
			continue;
		}

		skreq    = &skdev->skreq_table[req_slot];

		/*
		 * Make sure the request ID for the slot matches.
		 */
		if (skreq->id != req_id) {
			u16 new_id = cmp_cntxt;

			cmn_err(CE_NOTE, "(%s): Completion mismatch "
			    "comp_id=0x%04x skreq=0x%04x "
			    "new=0x%04x",
			    skd_name(skdev), req_id,
			    skreq->id, new_id);

			WAITQ_UNLOCK(skdev, 0, drive);

			continue;
		}


		if (SKD_REQ_STATE_BUSY != skreq->state) {
			if (SKD_REQ_STATE_ABORTED == skreq->state) {
				cmn_err(CE_NOTE, "reclaim req %p id=%04x\n",
				    (void *)skreq, skreq->id);
				/*
				 * a previously timed out command can
				 * now be cleaned up
				 */
				msg_slot = skreq->fitmsg_id & SKD_ID_SLOT_MASK;
				ASSERT(msg_slot <
				    skdev->num_fitmsg_context);
				skmsg = &skdev->skmsg_table[msg_slot];
				if (skmsg->id == skreq->fitmsg_id) {
					ASSERT(skmsg->outstanding > 0);
					skmsg->outstanding--;
					if (skmsg->outstanding == 0) {
						ASSERT(SKD_MSG_STATE_BUSY
						    == skmsg->state);
						skmsg->state =
						    SKD_MSG_STATE_IDLE;
						skmsg->id += SKD_ID_INCR;
						skmsg->next =
						    skdev->skmsg_free_list;
						skdev->skmsg_free_list = skmsg;
					}
				}
				/*
				 * Reclaim the skd_request_context
				 */
				skreq->state = SKD_REQ_STATE_IDLE;
				skreq->id += SKD_ID_INCR;
				skreq->next = skdev->skreq_free_list;
				skdev->skreq_free_list = skreq;
				WAITQ_UNLOCK(skdev, 0, drive);
				continue;
			} else {
				ASSERT(SKD_REQ_STATE_BUSY == skreq->state);
			}
		}

		skreq->completion.status = cmp_status;

		if ((bp = skreq->bp) != NULL) {
			Dcmn_err(CE_NOTE, "<<<< completion_posted 2: bp=%p "
			    "req_id=%x req_slot=%x", (void *)bp, req_id,
			    req_slot);
			pbuf = (skd_buf_private_t *)bp->b_private;
			if (cmp_status && skdev->disks_initialized) {
				cmn_err(CE_NOTE, "%s: "
				    "I/O err: bp=%p blk=%lld (%llx) cnt=%ld ",
				    skdev->name, (void *)bp, bp->b_lblkno,
				    bp->b_lblkno, bp->b_bcount);
			}

			/* Release DMA resources for the request. */
			if (bp->b_bcount != 0) {
				if (pbuf->origin == ORIGIN_SKD)
					skd_postop_sg_list(skdev, skreq);
				else {
					ASSERT(pbuf->origin == ORIGIN_BLKDEV);
					if (cmp_status == 0) {
						skd_blkdev_postop_sg_list(skdev,
						    skreq);
					}
				}
			}

			if (skdev->active_cmds)
				skdev->active_cmds--;

			if (likely(SAM_STAT_GOOD == cmp_status)) {
				bp->b_resid = 0;

				WAITQ_UNLOCK(skdev, 0, drive);
				skd_end_request(skdev, skreq, 0);
				WAITQ_LOCK(skdev, 0, drive);
			} else {
				switch (skd_check_status(skdev,
				    cmp_status, skerr)) {
				case SKD_CHECK_STATUS_REPORT_GOOD:
				case SKD_CHECK_STATUS_REPORT_SMART_ALERT:
					bp->b_resid = 0;
					WAITQ_UNLOCK(skdev, 0, drive);
					skd_end_request(skdev, skreq, 0);
					WAITQ_LOCK(skdev, 0, drive);
					break;

				case SKD_CHECK_STATUS_BUSY_IMMINENT:
					skd_log_skreq(skdev, skreq,
					    "retry(busy)");

					skd_queue_bp(skdev, drive, bp);
					skdev->state =
					    SKD_DRVR_STATE_BUSY_IMMINENT;
					skdev->timer_countdown =
					    SKD_TIMER_MINUTES(20);

					(void) skd_quiesce_dev(skdev);
					break;

					/* FALLTHRU */
				case SKD_CHECK_STATUS_REPORT_ERROR:
					/* fall thru to report error */
				default:
					/*
					 * Save the entire completion
					 * and error entries for
					 * later error interpretation.
					 */
					skreq->completion = *skcmp;
					skreq->err_info = *skerr;
					WAITQ_UNLOCK(skdev, 0, drive);
					skd_end_request(skdev, skreq, -EIO);
					WAITQ_LOCK(skdev, 0, drive);
					break;
				}
			}
		} else {
			cmn_err(CE_NOTE, "NULL request %p, skdreq %p, "
			    "req=0x%x req_id=0x%x\n",
			    (void *)bp, (void *)skreq, skreq->id, req_id);
		}

		/*
		 * Reclaim the FIT msg buffer if this is
		 * the first of the requests it carried to
		 * be completed. The FIT msg buffer used to
		 * send this request cannot be reused until
		 * we are sure the s1120 card has copied
		 * it to its memory. The FIT msg might have
		 * contained several requests. As soon as
		 * any of them are completed we know that
		 * the entire FIT msg was transferred.
		 * Only the first completed request will
		 * match the FIT msg buffer id. The FIT
		 * msg buffer id is immediately updated.
		 * When subsequent requests complete the FIT
		 * msg buffer id won't match, so we know
		 * quite cheaply that it is already done.
		 */
		msg_slot = skreq->fitmsg_id & SKD_ID_SLOT_MASK;

		ASSERT(msg_slot < skdev->num_fitmsg_context);
		skmsg = &skdev->skmsg_table[msg_slot];
		if (skmsg->id == skreq->fitmsg_id) {
			ASSERT(SKD_MSG_STATE_BUSY == skmsg->state);
			skmsg->state = SKD_MSG_STATE_IDLE;
			skmsg->id += SKD_ID_INCR;
			skmsg->next = skdev->skmsg_free_list;
			skdev->skmsg_free_list = skmsg;
		}

		/*
		 * Decrease the number of active requests.
		 * This also decrements the count in the
		 * timeout slot.
		 */
		timo_slot = skreq->timeout_stamp & SKD_TIMEOUT_SLOT_MASK;
		ASSERT(skdev->timeout_slot[timo_slot] > 0);
		ASSERT(skdev->queue_depth_busy > 0);

		skdev->timeout_slot[timo_slot] -= 1;

		atomic_dec_32(&skdev->queue_depth_busy);

		if ((int)skdev->queue_depth_busy < 0)
			skdev->queue_depth_busy = 0;

		/*
		 * Reclaim the skd_request_context
		 */
		skreq->state = SKD_REQ_STATE_IDLE;
		skreq->id += SKD_ID_INCR;
		skreq->next = skdev->skreq_free_list;
		skdev->skreq_free_list = skreq;

		WAITQ_UNLOCK(skdev, 0, drive);

		/*
		 * make sure the lock is held by caller.
		 */
		if ((skdev->state == SKD_DRVR_STATE_PAUSING) &&
		    (0 == skdev->queue_depth_busy)) {
			skdev->state = SKD_DRVR_STATE_PAUSED;
			cv_signal(&skdev->cv_waitq);
		}
	} /* for(;;) */
}

/*
 *
 * Name:	skd_complete_other, handle the completion of a
 *		non-r/w request.
 *
 * Inputs:	skdev		- device state structure.
 *		skcomp		- FIT completion structure.
 *		skerr		- error structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_complete_other(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr)
{
	u32 req_id = 0;
	u32 req_table;
	u32 req_slot;
	struct skd_special_context *skspcl;

	req_id = skcomp->tag;
	req_table = req_id & SKD_ID_TABLE_MASK;
	req_slot = req_id & SKD_ID_SLOT_MASK;

	Dcmn_err(CE_NOTE, "complete_other: table=0x%x id=0x%x slot=%d",
	    req_table, req_id, req_slot);

	/*
	 * Based on the request id, determine how to dispatch this completion.
	 * This swich/case is finding the good cases and forwarding the
	 * completion entry. Errors are reported below the switch.
	 */
	switch (req_table) {
	case SKD_ID_RW_REQUEST:
		/*
		 * The caller, skd_completion_posted_isr() above,
		 * handles r/w requests. The only way we get here
		 * is if the req_slot is out of bounds.
		 */
		Dcmn_err(CE_NOTE, "<<<<== complete_other: Complete R-W");
		break;

	case SKD_ID_SPECIAL_REQUEST:
		/*
		 * Make sure the req_slot is in bounds and that the id
		 * matches.
		 */
		if (req_slot < SKD_N_SPECIAL_CONTEXT) {
			skspcl = &skdev->skspcl_table[req_slot];
			if (skspcl->req.id == req_id &&
			    skspcl->req.state == SKD_REQ_STATE_BUSY) {
				Dcmn_err(CE_NOTE,
				    "<<<<== complete_other: SPECIAL_REQUEST");
				skd_complete_special(skdev,
				    skcomp, skerr, skspcl);
				return;
			}
		}
		break;

	case SKD_ID_INTERNAL:
		if (req_slot == 0) {
			skspcl = &skdev->internal_skspcl;
			if (skspcl->req.id == req_id &&
			    skspcl->req.state == SKD_REQ_STATE_BUSY) {
				Dcmn_err(CE_NOTE,
				    "<<<<== complete_other: ID_INTERNAL");
				skd_complete_internal(skdev,
				    skcomp, skerr, skspcl);
				return;
			}
		}
		break;

	case SKD_ID_FIT_MSG:
		/*
		 * These id's should never appear in a completion record.
		 */
		break;

	default:
		/*
		 * These id's should never appear anywhere;
		 */
		break;
	}

	Dcmn_err(CE_NOTE, "<<<<=== complete_other:  "
	    "STALE id-slot: %d-%d\n", req_id, req_slot);

	/*
	 * If we get here it is a bad or stale id.
	 */
}

/*
 *
 * Name:	skd_complete_other, handle the completion of a
 *		non-r/w request.
 *
 * Inputs:	skdev		- device state structure.
 *		skcomp		- FIT completion structure.
 *		skerr		- error structure.
 *		skspcl		- request structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_complete_special(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr,
    struct skd_special_context *skspcl)
{
	struct skd_scsi_request    *scsi_req;

	/* Instead of 64-bytes in, use 8-(64-bit-words) for linted alignment. */
	scsi_req = (struct skd_scsi_request *)&skspcl->msg_buf64[8];

	Dcmn_err(CE_NOTE, " completing special request %p, cdb[0]=%x",
	    (void *)skspcl, *scsi_req->cdb);

	if (skspcl->orphaned) {
		/* Discard orphaned request */
		/*
		 * ?: Can this release directly or does it need
		 * to use a worker?
		 */
		cmn_err(CE_NOTE, "release orphaned %p", (void *)skspcl);
		skd_release_special(skdev, skspcl);
		return;
	}

	skd_process_scsi_inq(skdev, skcomp, skerr, skspcl);

	skspcl->req.state = SKD_REQ_STATE_COMPLETED;
	skspcl->req.completion = *skcomp;
	skspcl->req.err_info = *skerr;

	cv_signal(&skdev->cv_waitq);
}

/* assume spinlock (INTR_LOCK) is already held */
/*
 *
 * Name:	skd_release_special, put the completed
 *		skspcl back on the free list.
 *
 * Inputs:	skdev		- device state structure.
 *		skspcl		- skspcl structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_release_special(struct skd_device *skdev,
    struct skd_special_context *skspcl)
{
	int i;

	/* XXX KEBE SAYS ASSERT() lock held here!!! */
	VERIFY(INTR_LOCK_HELD(skdev));

	for (i = 0; i < skspcl->req.n_sg; i++) {
		skd_free_dma_resource(skdev, &skspcl->page_dma_address);
	}

	skspcl->req.state = SKD_REQ_STATE_IDLE;
	skspcl->req.id += SKD_ID_INCR;
	skspcl->req.next =
	    (struct skd_request_context *)skdev->skspcl_free_list;
	skdev->skspcl_free_list = (struct skd_special_context *)skspcl;
}

/*
 *
 * Name:	skd_reset_skcomp, does what it says, resetting completion
 *		tables.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_reset_skcomp(struct skd_device *skdev)
{
	u32 nbytes;

	nbytes =  sizeof (struct fit_completion_entry_v1) *
	    SKD_N_COMPLETION_ENTRY;
	nbytes += sizeof (struct fit_comp_error_info)  * SKD_N_COMPLETION_ENTRY;

	if (skdev->skcomp_table)
		bzero(skdev->skcomp_table, nbytes);

	skdev->skcomp_ix = 0;
	skdev->skcomp_cycle = 1;
}



/*
 * INTERRUPTS
 */

/*
 *
 * Name:	skd_isr_aif, handles the device interrupts.
 *
 * Inputs:	arg		- skdev device state structure.
 *		intvec		- not referenced
 *
 * Returns:	DDI_INTR_CLAIMED if interrupt is handled otherwise
 *		return DDI_INTR_UNCLAIMED.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
uint_t
skd_isr_aif(caddr_t arg, caddr_t intvec)
{
	u32		  intstat;
	u32		  ack;
	int		  rc = DDI_INTR_UNCLAIMED;
	struct skd_device *skdev;

	skdev = (skd_device_t *)(uintptr_t)arg;

	ASSERT(skdev != NULL);

	skdev->intr_cntr++;

	Dcmn_err(CE_NOTE, "skd_isr_aif: intr=%" PRId64 "\n", skdev->intr_cntr);

	for (;;) {

		ASSERT(!WAITQ_LOCK_HELD(skdev, 0, 0));
		INTR_LOCK(skdev);

		intstat = SKD_READL(skdev, FIT_INT_STATUS_HOST);

		ack = FIT_INT_DEF_MASK;
		ack &= intstat;

		Dcmn_err(CE_NOTE, "intstat=0x%x ack=0x%x", intstat, ack);

		/*
		 * As long as there is an int pending on device, keep
		 * running loop.  When none, get out, but if we've never
		 * done any processing, call completion handler?
		 */
		if (0 == ack) {
			/*
			 * No interrupts on device, but run the completion
			 * processor anyway?
			 */
			if (0 == rc)
				if (skdev->state == SKD_DRVR_STATE_ONLINE) {
					Dcmn_err(CE_NOTE,
					    "1: Want isr_comp_posted call");
					skd_isr_completion_posted(skdev);
				}
			INTR_UNLOCK(skdev);

			break;
		}

		rc = DDI_INTR_CLAIMED;

		SKD_WRITEL(skdev, ack, FIT_INT_STATUS_HOST);

		if ((skdev->state != SKD_DRVR_STATE_LOAD) &&
		    (skdev->state != SKD_DRVR_STATE_STOPPING)) {
			if (intstat & FIT_ISH_COMPLETION_POSTED) {
				Dcmn_err(CE_NOTE,
				    "2: Want isr_comp_posted call");
				skd_isr_completion_posted(skdev);
			}

			if (intstat & FIT_ISH_FW_STATE_CHANGE) {
				Dcmn_err(CE_NOTE, "isr: fwstate change");

				skd_isr_fwstate(skdev);
				if (skdev->state == SKD_DRVR_STATE_FAULT ||
				    skdev->state ==
				    SKD_DRVR_STATE_DISAPPEARED) {
					INTR_UNLOCK(skdev);

					return (rc);
				}
			}

			if (intstat & FIT_ISH_MSG_FROM_DEV) {
				Dcmn_err(CE_NOTE, "isr: msg_from_dev change");
				skd_isr_msg_from_dev(skdev);
			}
		}

		INTR_UNLOCK(skdev);
	}

	(void) skd_start(skdev, 99);

	return (rc);
}

/*
 *
 * Name:	skd_drive_fault, set the drive state to DRV_STATE_FAULT.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
void
skd_drive_fault(struct skd_device *skdev)
{
	skdev->state = SKD_DRVR_STATE_FAULT;
	cmn_err(CE_NOTE, "(%s): Drive FAULT\n",
	    skd_name(skdev));
}

/*
 *
 * Name:	skd_drive_disappeared, set the drive state to DISAPPEARED..
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
void
skd_drive_disappeared(struct skd_device *skdev)
{
	skdev->state = SKD_DRVR_STATE_DISAPPEARED;
	cmn_err(CE_NOTE, "(%s): Drive DISAPPEARED\n",
	    skd_name(skdev));
}

/*
 *
 * Name:	skd_isr_fwstate, handles the various device states.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_isr_fwstate(struct skd_device *skdev)
{
	u32 sense;
	u32 state;
	int prev_driver_state;
	u32 mtd;

	prev_driver_state = skdev->state;

	sense = SKD_READL(skdev, FIT_STATUS);
	state = sense & FIT_SR_DRIVE_STATE_MASK;

	Dcmn_err(CE_NOTE, "s1120 state %s(%d)=>%s(%d)",
	    skd_drive_state_to_str(skdev->drive_state), skdev->drive_state,
	    skd_drive_state_to_str(state), state);

	skdev->drive_state = state;

	switch (skdev->drive_state) {
	case FIT_SR_DRIVE_INIT:
		if (skdev->state == SKD_DRVR_STATE_PROTOCOL_MISMATCH) {
			skd_disable_interrupts(skdev);
			break;
		}
		if (skdev->state == SKD_DRVR_STATE_RESTARTING) {
			skd_recover_requests(skdev);
		}
		if (skdev->state == SKD_DRVR_STATE_WAIT_BOOT) {
			skdev->timer_countdown =
			    SKD_TIMER_SECONDS(SKD_STARTING_TO);
			skdev->state = SKD_DRVR_STATE_STARTING;
			skd_soft_reset(skdev);
			break;
		}
		mtd = FIT_MXD_CONS(FIT_MTD_FITFW_INIT, 0, 0);
		SKD_WRITEL(skdev, mtd, FIT_MSG_TO_DEVICE);
		skdev->last_mtd = mtd;
		break;

	case FIT_SR_DRIVE_ONLINE:
		skdev->queue_depth_limit = skdev->soft_queue_depth_limit;
		if (skdev->queue_depth_limit > skdev->hard_queue_depth_limit) {
			skdev->queue_depth_limit =
			    skdev->hard_queue_depth_limit;
		}

		skdev->queue_depth_lowat = skdev->queue_depth_limit * 2 / 3 + 1;
		if (skdev->queue_depth_lowat < 1)
			skdev->queue_depth_lowat = 1;
		Dcmn_err(CE_NOTE,
		    "%s queue depth limit=%d hard=%d soft=%d lowat=%d",
		    DRV_NAME,
		    skdev->queue_depth_limit,
		    skdev->hard_queue_depth_limit,
		    skdev->soft_queue_depth_limit,
		    skdev->queue_depth_lowat);

		skd_refresh_device_data(skdev);
		break;
	case FIT_SR_DRIVE_BUSY:
		skdev->state = SKD_DRVR_STATE_BUSY;
		skdev->timer_countdown = SKD_TIMER_MINUTES(20);
		(void) skd_quiesce_dev(skdev);
		break;
	case FIT_SR_DRIVE_BUSY_SANITIZE:
		skdev->state = SKD_DRVR_STATE_BUSY_SANITIZE;
		(void) skd_start(skdev, 4);
		break;
	case FIT_SR_DRIVE_BUSY_ERASE:
		skdev->state = SKD_DRVR_STATE_BUSY_ERASE;
		skdev->timer_countdown = SKD_TIMER_MINUTES(20);
		break;
	case FIT_SR_DRIVE_OFFLINE:
		skdev->state = SKD_DRVR_STATE_IDLE;
		break;
	case FIT_SR_DRIVE_SOFT_RESET:
		skdev->state = SKD_DRVR_STATE_RESTARTING;

		switch (skdev->state) {
		case SKD_DRVR_STATE_STARTING:
		case SKD_DRVR_STATE_RESTARTING:
			break;
		default:
			skdev->state = SKD_DRVR_STATE_RESTARTING;
			break;
		}
		break;
	case FIT_SR_DRIVE_FW_BOOTING:
		Dcmn_err(CE_NOTE,
		    "ISR FIT_SR_DRIVE_FW_BOOTING %s", skdev->name);
		skdev->state = SKD_DRVR_STATE_WAIT_BOOT;
		skdev->timer_countdown = SKD_TIMER_SECONDS(SKD_WAIT_BOOT_TO);
		break;

	case FIT_SR_DRIVE_DEGRADED:
	case FIT_SR_PCIE_LINK_DOWN:
	case FIT_SR_DRIVE_NEED_FW_DOWNLOAD:
		break;

	case FIT_SR_DRIVE_FAULT:
		skd_drive_fault(skdev);
		skd_recover_requests(skdev);
		(void) skd_start(skdev, 5);
		break;

	case 0xFF:
		skd_drive_disappeared(skdev);
		skd_recover_requests(skdev);
		(void) skd_start(skdev, 6);
		break;
	default:
		/*
		 * Uknown FW State. Wait for a state we recognize.
		 */
		break;
	}

	Dcmn_err(CE_NOTE, "Driver state %s(%d)=>%s(%d)",
	    skd_skdev_state_to_str(prev_driver_state), prev_driver_state,
	    skd_skdev_state_to_str(skdev->state), skdev->state);
}

/*
 *
 * Name:	skd_recover_requests, attempts to recover requests.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_recover_requests(struct skd_device *skdev)
{
	skd_buf_private_t *p;
	int i;

	/* XXX KEBE SAYS make this an ASSERT(). */
	VERIFY(INTR_LOCK_HELD(skdev));

	for (i = 0; i < skdev->num_req_context; i++) {
		struct skd_request_context *skreq = &skdev->skreq_table[i];

		if (skreq->state == SKD_REQ_STATE_BUSY) {
			skd_log_skreq(skdev, skreq, "requeue");

			ASSERT(0 != (skreq->id & SKD_ID_INCR));
			ASSERT(NULL != skreq->bp);

			p = (skd_buf_private_t *)skreq->bp->b_private;

			/* Release DMA resources for the request. */
			if (skd_blk_rq_bytes(skreq->bp) != 0) {
				if (p->origin == ORIGIN_SKD)
					skd_postop_sg_list(skdev, skreq);
				else {
					ASSERT(p->origin == ORIGIN_BLKDEV);
					skd_blkdev_postop_sg_list(skdev, skreq);
				}
			}

			skd_end_request(skdev, skreq, EAGAIN);

			skreq->bp = NULL;
			skreq->state = SKD_REQ_STATE_IDLE;
			skreq->id += SKD_ID_INCR;
		}
		if (i > 0) {
			skreq[-1].next = skreq;
		}
		skreq->next = NULL;
	}

	WAITQ_LOCK(skdev, 0, 0);
	skdev->skreq_free_list = skdev->skreq_table;
	WAITQ_UNLOCK(skdev, 0, 0);

	for (i = 0; i < skdev->num_fitmsg_context; i++) {
		struct skd_fitmsg_context *skmsg = &skdev->skmsg_table[i];

		if (skmsg->state == SKD_MSG_STATE_BUSY) {
			skd_log_skmsg(skdev, skmsg, "salvaged");
			ASSERT(0 != (skmsg->id & SKD_ID_INCR));
			skmsg->state = SKD_MSG_STATE_IDLE;
			skmsg->id += SKD_ID_INCR;
		}
		if (i > 0) {
			skmsg[-1].next = skmsg;
		}
		skmsg->next = NULL;
	}
	WAITQ_LOCK(skdev, 0, 0);
	skdev->skmsg_free_list = skdev->skmsg_table;
	WAITQ_UNLOCK(skdev, 0, 0);

	for (i = 0; i < SKD_N_SPECIAL_CONTEXT; i++) {
		struct skd_special_context *skspcl = &skdev->skspcl_table[i];

		/*
		 * If orphaned, reclaim it because it has already been reported
		 * to the process as an error (it was just waiting for
		 * a completion that didn't come, and now it will never come)
		 * If busy, change to a state that will cause it to error
		 * out in the wait routine and let it do the normal
		 * reporting and reclaiming
		 */
		if (skspcl->req.state == SKD_REQ_STATE_BUSY) {
			if (skspcl->orphaned) {
				DPRINTK(skdev, "orphaned %p\n",
				    (void *)skspcl);
				skd_release_special(skdev, skspcl);
			} else {
				DPRINTK(skdev, "not orphaned %p\n",
				    (void *)skspcl);
				skspcl->req.state = SKD_REQ_STATE_ABORTED;
			}
		}
	}
	/* Only needs INTR_LOCK for protection. */
	skdev->skspcl_free_list = skdev->skspcl_table;

	for (i = 0; i < SKD_N_TIMEOUT_SLOT; i++) {
		skdev->timeout_slot[i] = 0;
	}
	skdev->queue_depth_busy = 0;
}

/*
 *
 * Name:	skd_isr_msg_from_dev, handles a message from the device.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_isr_msg_from_dev(struct skd_device *skdev)
{
	u32 mfd;
	u32 mtd;

	Dcmn_err(CE_NOTE, "skd_isr_msg_from_dev:");

	mfd = SKD_READL(skdev, FIT_MSG_FROM_DEVICE);

	Dcmn_err(CE_NOTE, "mfd=0x%x last_mtd=0x%x\n", mfd, skdev->last_mtd);

	/*
	 * ignore any mtd that is an ack for something we didn't send
	 */
	if (FIT_MXD_TYPE(mfd) != FIT_MXD_TYPE(skdev->last_mtd)) {
		return;
	}

	switch (FIT_MXD_TYPE(mfd)) {
	case FIT_MTD_FITFW_INIT:
		skdev->proto_ver = FIT_PROTOCOL_MAJOR_VER(mfd);

		if (skdev->proto_ver != FIT_PROTOCOL_VERSION_1) {
			cmn_err(CE_NOTE, "(%s): protocol mismatch\n",
			    skdev->name);
			cmn_err(CE_NOTE, "(%s):   got=%d support=%d\n",
			    skdev->name, skdev->proto_ver,
			    FIT_PROTOCOL_VERSION_1);
			cmn_err(CE_NOTE, "(%s):   please upgrade driver\n",
			    skdev->name);
			skdev->state = SKD_DRVR_STATE_PROTOCOL_MISMATCH;
			skd_soft_reset(skdev);
			break;
		}
		mtd = FIT_MXD_CONS(FIT_MTD_GET_CMDQ_DEPTH, 0, 0);
		SKD_WRITEL(skdev, mtd, FIT_MSG_TO_DEVICE);
		skdev->last_mtd = mtd;
		break;

	case FIT_MTD_GET_CMDQ_DEPTH:
		skdev->hard_queue_depth_limit = FIT_MXD_DATA(mfd);
		mtd = FIT_MXD_CONS(FIT_MTD_SET_COMPQ_DEPTH, 0,
		    SKD_N_COMPLETION_ENTRY);
		SKD_WRITEL(skdev, mtd, FIT_MSG_TO_DEVICE);
		skdev->last_mtd = mtd;
		break;

	case FIT_MTD_SET_COMPQ_DEPTH:
		SKD_WRITEQ(skdev, skdev->cq_dma_address.cookies->dmac_laddress,
		    FIT_MSG_TO_DEVICE_ARG);
		mtd = FIT_MXD_CONS(FIT_MTD_SET_COMPQ_ADDR, 0, 0);
		SKD_WRITEL(skdev, mtd, FIT_MSG_TO_DEVICE);
		skdev->last_mtd = mtd;
		break;

	case FIT_MTD_SET_COMPQ_ADDR:
		skd_reset_skcomp(skdev);
		mtd = FIT_MXD_CONS(FIT_MTD_ARM_QUEUE, 0, 0);
		SKD_WRITEL(skdev, mtd, FIT_MSG_TO_DEVICE);
		skdev->last_mtd = mtd;
		break;

	case FIT_MTD_ARM_QUEUE:
		skdev->last_mtd = 0;
		/*
		 * State should be, or soon will be, FIT_SR_DRIVE_ONLINE.
		 */
		break;

	default:
		break;
	}
}


/*
 *
 * Name:	skd_disable_interrupts, issues command to disable
 *		device interrupts.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_disable_interrupts(struct skd_device *skdev)
{
	u32 sense;

	Dcmn_err(CE_NOTE, "skd_disable_interrupts:");

	sense = SKD_READL(skdev, FIT_CONTROL);
	sense &= ~FIT_CR_ENABLE_INTERRUPTS;
	SKD_WRITEL(skdev, sense, FIT_CONTROL);

	Dcmn_err(CE_NOTE, "sense 0x%x", sense);

	/*
	 * Note that the 1s is written. A 1-bit means
	 * disable, a 0 means enable.
	 */
	SKD_WRITEL(skdev, ~0, FIT_INT_MASK_HOST);
}

/*
 *
 * Name:	skd_enable_interrupts, issues command to enable
 *		device interrupts.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_enable_interrupts(struct skd_device *skdev)
{
	u32 val;

	Dcmn_err(CE_NOTE, "skd_enable_interrupts:");

	/* unmask interrupts first */
	val = FIT_ISH_FW_STATE_CHANGE +
	    FIT_ISH_COMPLETION_POSTED +
	    FIT_ISH_MSG_FROM_DEV;

	/*
	 * Note that the compliment of mask is written. A 1-bit means
	 * disable, a 0 means enable.
	 */
	SKD_WRITEL(skdev, ~val, FIT_INT_MASK_HOST);

	Dcmn_err(CE_NOTE, "interrupt mask=0x%x", ~val);

	val = SKD_READL(skdev, FIT_CONTROL);
	val |= FIT_CR_ENABLE_INTERRUPTS;

	Dcmn_err(CE_NOTE, "control=0x%x", val);

	SKD_WRITEL(skdev, val, FIT_CONTROL);
}

/*
 *
 * Name:	skd_soft_reset, issues a soft reset to the hardware.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_soft_reset(struct skd_device *skdev)
{
	u32 val;

	Dcmn_err(CE_NOTE, "skd_soft_reset:");

	val = SKD_READL(skdev, FIT_CONTROL);
	val |= (FIT_CR_SOFT_RESET);

	Dcmn_err(CE_NOTE, "soft_reset: control=0x%x", val);

	SKD_WRITEL(skdev, val, FIT_CONTROL);
}

/*
 *
 * Name:	skd_start_device, gets the device going.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_start_device(struct skd_device *skdev)
{
	u32 state;
	int delay_action = 0;

	Dcmn_err(CE_NOTE, "skd_start_device:");

	/* ack all ghost interrupts */
	SKD_WRITEL(skdev, FIT_INT_DEF_MASK, FIT_INT_STATUS_HOST);

	state = SKD_READL(skdev, FIT_STATUS);

	Dcmn_err(CE_NOTE, "initial status=0x%x", state);

	state &= FIT_SR_DRIVE_STATE_MASK;
	skdev->drive_state = state;
	skdev->last_mtd = 0;

	skdev->state = SKD_DRVR_STATE_STARTING;
	skdev->timer_countdown = SKD_TIMER_SECONDS(SKD_STARTING_TO);

	skd_enable_interrupts(skdev);

	switch (skdev->drive_state) {
	case FIT_SR_DRIVE_OFFLINE:
		Dcmn_err(CE_NOTE, "(%s): Drive offline...",
		    skd_name(skdev));
		break;

	case FIT_SR_DRIVE_FW_BOOTING:
		Dcmn_err(CE_NOTE, "FIT_SR_DRIVE_FW_BOOTING %s\n", skdev->name);
		skdev->state = SKD_DRVR_STATE_WAIT_BOOT;
		skdev->timer_countdown = SKD_TIMER_SECONDS(SKD_WAIT_BOOT_TO);
		break;

	case FIT_SR_DRIVE_BUSY_SANITIZE:
		Dcmn_err(CE_NOTE, "(%s): Start: BUSY_SANITIZE\n",
		    skd_name(skdev));
		skdev->state = SKD_DRVR_STATE_BUSY_SANITIZE;
		skdev->timer_countdown = SKD_TIMER_SECONDS(60);
		break;

	case FIT_SR_DRIVE_BUSY_ERASE:
		Dcmn_err(CE_NOTE, "(%s): Start: BUSY_ERASE\n",
		    skd_name(skdev));
		skdev->state = SKD_DRVR_STATE_BUSY_ERASE;
		skdev->timer_countdown = SKD_TIMER_SECONDS(60);
		break;

	case FIT_SR_DRIVE_INIT:
	case FIT_SR_DRIVE_ONLINE:
		skd_soft_reset(skdev);

		break;

	case FIT_SR_DRIVE_BUSY:
		Dcmn_err(CE_NOTE, "(%s): Drive Busy...\n",
		    skd_name(skdev));
		skdev->state = SKD_DRVR_STATE_BUSY;
		skdev->timer_countdown = SKD_TIMER_SECONDS(60);
		break;

	case FIT_SR_DRIVE_SOFT_RESET:
		Dcmn_err(CE_NOTE, "(%s) drive soft reset in prog\n",
		    skd_name(skdev));
		break;

	case FIT_SR_DRIVE_FAULT:
		/*
		 * Fault state is bad...soft reset won't do it...
		 * Hard reset, maybe, but does it work on device?
		 * For now, just fault so the system doesn't hang.
		 */
		skd_drive_fault(skdev);

		delay_action = 1;
		break;

	case 0xFF:
		skd_drive_disappeared(skdev);

		delay_action = 1;
		break;

	default:
		Dcmn_err(CE_NOTE, "(%s) Start: unknown state %x\n",
		    skd_name(skdev), skdev->drive_state);
		break;
	}

	state = SKD_READL(skdev, FIT_CONTROL);
	Dcmn_err(CE_NOTE, "FIT Control Status=0x%x\n", state);

	state = SKD_READL(skdev, FIT_INT_STATUS_HOST);
	Dcmn_err(CE_NOTE, "Intr Status=0x%x\n", state);

	state = SKD_READL(skdev, FIT_INT_MASK_HOST);
	Dcmn_err(CE_NOTE, "Intr Mask=0x%x\n", state);

	state = SKD_READL(skdev, FIT_MSG_FROM_DEVICE);
	Dcmn_err(CE_NOTE, "Msg from Dev=0x%x\n", state);

	state = SKD_READL(skdev, FIT_HW_VERSION);
	Dcmn_err(CE_NOTE, "HW version=0x%x\n", state);

	if (delay_action) {
		/* start the queue so we can respond with error to requests */
		Dcmn_err(CE_NOTE, "Starting %s queue\n", skdev->name);
		(void) skd_start(skdev, 7);
		skdev->gendisk_on = -1;
		cv_signal(&skdev->cv_waitq);
	}
}

/*
 *
 * Name:	skd_restart_device, restart the hardware.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_restart_device(struct skd_device *skdev)
{
	u32 state;

	Dcmn_err(CE_NOTE, "skd_restart_device:");

	/* ack all ghost interrupts */
	SKD_WRITEL(skdev, FIT_INT_DEF_MASK, FIT_INT_STATUS_HOST);

	state = SKD_READL(skdev, FIT_STATUS);

	cmn_err(CE_NOTE, "skd_restart_device: drive status=0x%x\n", state);

	state &= FIT_SR_DRIVE_STATE_MASK;
	skdev->drive_state = state;
	skdev->last_mtd = 0;

	skdev->state = SKD_DRVR_STATE_RESTARTING;
	skdev->timer_countdown = SKD_TIMER_MINUTES(4);

	skd_soft_reset(skdev);
}

/*
 *
 * Name:	skd_stop_device, stops the device.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_stop_device(struct skd_device *skdev)
{
	clock_t	cur_ticks, tmo;
	int secs;
	struct skd_special_context *skspcl = &skdev->internal_skspcl;

	if (SKD_DRVR_STATE_ONLINE != skdev->state) {
		cmn_err(CE_NOTE, "(%s): skd_stop_device not online no sync\n",
		    skdev->name);
		goto stop_out;
	}

	if (SKD_REQ_STATE_IDLE != skspcl->req.state) {
		Dcmn_err(CE_NOTE, "(%s): skd_stop_device no special\n",
		    skdev->name);
		goto stop_out;
	}

	skdev->state = SKD_DRVR_STATE_SYNCING;
	skdev->sync_done = 0;

	skd_send_internal_skspcl(skdev, skspcl, SYNCHRONIZE_CACHE);

	secs = 10;
	mutex_enter(&skdev->skd_internalio_mutex);
	while (skdev->sync_done == 0) {
		cur_ticks = ddi_get_lbolt();
		tmo = cur_ticks + drv_usectohz(1000000 * secs);
		if (cv_timedwait(&skdev->cv_waitq,
		    &skdev->skd_internalio_mutex, tmo) == -1) {
			/* Oops - timed out */

			cmn_err(CE_NOTE, "stop_device - %d secs TMO", secs);
		}
	}

	mutex_exit(&skdev->skd_internalio_mutex);

	switch (skdev->sync_done) {
	case 0:
		cmn_err(CE_NOTE, "(%s): skd_stop_device no sync\n",
		    skdev->name);
		break;
	case 1:
		cmn_err(CE_NOTE, "(%s): skd_stop_device sync done\n",
		    skdev->name);
		break;
	default:
		cmn_err(CE_NOTE, "(%s): skd_stop_device sync error\n",
		    skdev->name);
	}


stop_out:
	skdev->state = SKD_DRVR_STATE_STOPPING;

	skd_disable_interrupts(skdev);

	/* ensure all ints on device are cleared */
	SKD_WRITEL(skdev, FIT_INT_DEF_MASK, FIT_INT_STATUS_HOST);
	/* soft reset the device to unload with a clean slate */
	SKD_WRITEL(skdev, FIT_CR_SOFT_RESET, FIT_CONTROL);
}

/*
 * CONSTRUCT
 */

static int skd_cons_skcomp(struct skd_device *);
static int skd_cons_skmsg(struct skd_device *);
static int skd_cons_skreq(struct skd_device *);
static int skd_cons_skspcl(struct skd_device *);
static int skd_cons_sksb(struct skd_device *);
static struct fit_sg_descriptor *skd_cons_sg_list(struct skd_device *, u32,
    dma_mem_t *);

#define	SKD_N_DEV_TABLE		16u
static u32 skd_next_devno = 0;
static struct skd_device *skd_dev_table[SKD_N_DEV_TABLE];

/*
 *
 * Name:	skd_construct, calls other routines to build device
 *		interface structures.
 *
 * Inputs:	skdev		- device state structure.
 *		instance	- DDI instance number.
 *
 * Returns:	Returns DDI_FAILURE on any failure otherwise returns
 *		DDI_SUCCESS.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_construct(skd_device_t *skdev, int instance)
{
	int rc = 0;

	if (skdev == NULL) {
		cmn_err(CE_NOTE, "%s: skd_construct: memory alloc failure",
		    skdev->name);

		return (DDI_FAILURE);
	}

	while (skd_dev_table[skd_next_devno % SKD_N_DEV_TABLE] != NULL) {
		skd_next_devno++;
	}

	skd_major = ddi_driver_major(skdev->dip);

	skdev->state = SKD_DRVR_STATE_LOAD;
	skdev->devno = skd_next_devno;
	skdev->major = skd_major;
	skdev->irq_type = skd_isr_type;
	skdev->soft_queue_depth_limit = skd_max_queue_depth;
	skdev->hard_queue_depth_limit = 10; /* until GET_CMDQ_DEPTH */

	skdev->num_req_context = skd_max_queue_depth;
	skdev->num_fitmsg_context = skd_max_queue_depth;

	skdev->queue_depth_limit = skdev->hard_queue_depth_limit;
	skdev->queue_depth_lowat = 1;
	skdev->proto_ver = 99; /* initialize to invalid value */
	skdev->sgs_per_request = skd_sgs_per_request;
	skdev->dbg_level = skd_dbg_level;

	skdev->device_count = 0;

	rc = skd_cons_skcomp(skdev);
	if (rc < 0) {
		goto err_out;
	}

	rc = skd_cons_skmsg(skdev);
	if (rc < 0) {
		goto err_out;
	}

	rc = skd_cons_skreq(skdev);
	if (rc < 0) {
		goto err_out;
	}

	rc = skd_cons_skspcl(skdev);
	if (rc < 0) {
		goto err_out;
	}

	rc = skd_cons_sksb(skdev);
	if (rc < 0) {
		goto err_out;
	}

	skd_dev_table[skdev->devno % SKD_N_DEV_TABLE] = skdev;

	skd_next_devno++;

	Dcmn_err(CE_NOTE, "CONSTRUCT VICTORY");

	return (DDI_SUCCESS);

err_out:
	cmn_err(CE_NOTE, "construct failed\n");
	skd_destruct(skdev);

	return (DDI_FAILURE);
}

/*
 *
 * Name:	skd_bind_dma_bufffer, bind's mem->bp DMA address.
 *
 * Inputs:	skdev		- device state structure.
 *		mem		- DMA info.
 *		sleep		- spcifies whether call can sleep or not.
 *
 * Returns:	DDI_DMA_MAPPED on success.
 *		DDI_DMA_TOOBIG is size request is too large.
 *		DDI_DMA_NORESOURCES is out of memory.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_bind_dma_buffer(skd_device_t *skdev, dma_mem_t *mem, int sleep)
{
	int			rval;
	ddi_dma_cookie_t	*cookiep;
	uint32_t		cnt = mem->cookie_count;

	if (mem->type == STRUCT_BUF_MEMORY) {
		rval = ddi_dma_buf_bind_handle(mem->dma_handle, mem->bp,
		    mem->flags, (sleep == KM_SLEEP) ? DDI_DMA_SLEEP :
		    DDI_DMA_DONTWAIT, NULL, &mem->cookie, &mem->cookie_count);
	} else {
		rval = ddi_dma_addr_bind_handle(mem->dma_handle, NULL, mem->bp,
		    mem->size, mem->flags, (sleep == KM_SLEEP) ?
		    DDI_DMA_SLEEP : DDI_DMA_DONTWAIT, NULL, &mem->cookie,
		    &mem->cookie_count);
	}

	if (rval == DDI_DMA_MAPPED) {
		if (mem->cookie_count > cnt) {
			(void) ddi_dma_unbind_handle(mem->dma_handle);
			cmn_err(CE_NOTE,
			    "bind_dma_buffer: failed, cookie_count %d > %d",
			    mem->cookie_count, cnt);
			rval = DDI_DMA_TOOBIG;
		} else {
			if (mem->cookie_count > 1) {
				mem->cookies = (ddi_dma_cookie_t *)kmem_zalloc(
				    sizeof (ddi_dma_cookie_t) *
				    mem->cookie_count, sleep);
				if (mem->cookies != NULL) {

					*mem->cookies = mem->cookie;
					cookiep = mem->cookies;
					for (cnt = 1; cnt < mem->cookie_count;
					    cnt++) {
						ddi_dma_nextcookie(
						    mem->dma_handle,
						    ++cookiep);
					}
				} else {
					(void) ddi_dma_unbind_handle(
					    mem->dma_handle);
					cmn_err(CE_NOTE, "bind_dma_buffer: "
					    "failed, kmem_zalloc");
					rval = DDI_DMA_NORESOURCES;
				}
			} else {
				mem->cookies = &mem->cookie;
				mem->cookies->dmac_size = mem->size;
			}
		}
	}

	if (rval != DDI_DMA_MAPPED)
		cmn_err(CE_NOTE, "bind_dma_buffer FAILED=%xh\n", rval);

	return (rval);
}

/*
 *
 * Name:	skd_unbuind_dma_buffer, unbind, frees allocated DMA memory.
 *
 * Inputs:	skdev		- device state structure.
 *		mem		- DMA info
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_unbind_dma_buffer(skd_device_t *skdev, dma_mem_t *mem)
{
	if (ddi_dma_unbind_handle(mem->dma_handle) != DDI_SUCCESS)
		cmn_err(CE_NOTE, "skd_unbind_dma_buffer FAILED");

	if (mem->cookie_count > 1) {
		kmem_free(mem->cookies, sizeof (ddi_dma_cookie_t) *
		    mem->cookie_count);

		mem->cookies = NULL;
	}

	mem->cookie_count = 0;
}

/*
 *
 * Name:	skd_free_phys, frees DMA memory.
 *
 * Inputs:	skdev		- device state structure.
 *		mem		- DMA info.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_free_phys(skd_device_t *skdev, dma_mem_t *mem)
{
	if (mem != NULL && mem->dma_handle != NULL) {
		skd_unbind_dma_buffer(skdev, mem);

		switch (mem->type) {
		case KERNEL_MEM:
			if (mem->bp != NULL) {
				kmem_free(mem->bp, mem->size);
			}
			break;
		case LITTLE_ENDIAN_DMA:
		case BIG_ENDIAN_DMA:
		case NO_SWAP_DMA:
			if (mem->acc_handle != NULL) {
				ddi_dma_mem_free(&mem->acc_handle);
				mem->acc_handle = NULL;
			}
			break;
		default:
			break;
		}

		mem->bp = NULL;
		ddi_dma_free_handle(&mem->dma_handle);
		mem->dma_handle = NULL;
	}
}

/*
 *
 * Name:	skd_free_dma_resouces
 *
 * Inputs:	skdev		- device state structure.
 *		mem		- DMA info structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_free_dma_resource(skd_device_t *skdev, dma_mem_t *mem)
{
	skd_free_phys(skdev, mem);
}

/*
 *
 * Name:	skd_alloc_dma_mem, allocates DMA memory.
 *
 * Inputs:	skdev		- device state structure.
 *		mem		- DMA data structure.
 *		sleep		- indicates whether called routine can sleep.
 *		atype		- specified 32 or 64 bit allocation.
 *
 * Returns:	Void pointer to mem->bp on success else NULL.
 *		NOTE:  There are some failure modes even if sleep is set
 *		to KM_SLEEP, so callers MUST check the return code even
 *		if KM_SLEEP is passed in.
 *
 */
static void *
skd_alloc_dma_mem(skd_device_t *skdev, dma_mem_t *mem, uint_t sleep,
    uint8_t atype)
{
	size_t		rlen;
	ddi_dma_attr_t	dma_attr;
	ddi_device_acc_attr_t acc_attr = skd_dev_acc_attr;

	dma_attr = (atype == ATYPE_64BIT) ?
	    skd_64bit_io_dma_attr : skd_32bit_io_dma_attr;

	dma_attr.dma_attr_align  = mem->alignment; /* byte address alignment */
	dma_attr.dma_attr_sgllen = (int)mem->cookie_count; /* s/g list count */


	mem->flags = DDI_DMA_CONSISTENT;

	/*
	 * Allocate DMA memory.
	 */
	if (ddi_dma_alloc_handle(skdev->dip, &dma_attr, (sleep == KM_SLEEP) ?
	    DDI_DMA_SLEEP : DDI_DMA_DONTWAIT, NULL, &mem->dma_handle) !=
	    DDI_SUCCESS) {
		cmn_err(CE_NOTE, "alloc_dma_mem-1, failed");

		mem->dma_handle = NULL;

		return (NULL);
	}

	switch (mem->type) {
	case KERNEL_MEM:
		mem->bp = (struct buf *)kmem_zalloc(mem->size, sleep);
		break;
	case BIG_ENDIAN_DMA:
	case LITTLE_ENDIAN_DMA:
	case NO_SWAP_DMA:
		if (mem->type == BIG_ENDIAN_DMA) {
			acc_attr.devacc_attr_endian_flags =
			    DDI_STRUCTURE_BE_ACC;
		} else if (mem->type == NO_SWAP_DMA) {
			acc_attr.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC;
		}

		if (ddi_dma_mem_alloc(mem->dma_handle, mem->size, &acc_attr,
		    mem->flags, (sleep == KM_SLEEP) ? DDI_DMA_SLEEP :
		    DDI_DMA_DONTWAIT, NULL, (caddr_t *)&mem->bp, &rlen,
		    &mem->acc_handle) == DDI_SUCCESS) {
			bzero(mem->bp, mem->size);

			if (dma_attr.dma_attr_addr_hi == NULL) {
				if (mem->cookie.dmac_notused != NULL) {
					cmn_err(CE_NOTE,
					    "failed, ddi_dma_mem_alloc "
					    "returned 64 bit DMA address");
					skd_free_phys(skdev, mem);

					return (NULL);
				}
			}
		} else {
			mem->acc_handle = NULL;
			mem->bp = NULL;
		}
		break;
	default:
		cmn_err(CE_NOTE,
		    "skd_alloc_dma_mem, failed unknown type = %xh", mem->type);
		mem->acc_handle = NULL;
		mem->bp = NULL;
		break;
	}

	if (mem->bp == NULL) {
		cmn_err(CE_NOTE, "skd_alloc_dma_mem-2, failed");
		ddi_dma_free_handle(&mem->dma_handle);
		mem->dma_handle = NULL;

		return (NULL);
	}

	mem->flags |= DDI_DMA_RDWR;

	if (skd_bind_dma_buffer(skdev, mem, sleep) != DDI_DMA_MAPPED) {
		cmn_err(CE_NOTE, "skd_alloc_dma_mem-3, failed");
		skd_free_phys(skdev, mem);

		return (NULL);
	}

	return (mem->bp);
}

/*
 *
 * Name:	skd_cons_skcomp, allocates space for the skcomp table.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	-ENOMEM if no memory otherwise NULL.
 *
 */
static int
skd_cons_skcomp(struct skd_device *skdev)
{
	u64		*dma_alloc;
	struct fit_completion_entry_v1 *skcomp;
	int 		rc = 0;
	u32 		nbytes;
	dma_mem_t	*mem;

	nbytes = sizeof (*skcomp) * SKD_N_COMPLETION_ENTRY;
	nbytes += sizeof (struct fit_comp_error_info) * SKD_N_COMPLETION_ENTRY;

	Dcmn_err(CE_NOTE, "cons_skcomp: nbytes=%d,entries=%d", nbytes,
	    SKD_N_COMPLETION_ENTRY);

	mem 			= &skdev->cq_dma_address;
	mem->alignment  	= SG_BOUNDARY;
	mem->cookie_count 	= 1;
	mem->size 		= nbytes;
	mem->type 		= LITTLE_ENDIAN_DMA;

	dma_alloc = skd_alloc_dma_mem(skdev, mem, KM_SLEEP, ATYPE_64BIT);
	skcomp = (struct fit_completion_entry_v1 *)dma_alloc;
	if (skcomp == NULL) {
		rc = -ENOMEM;
		goto err_out;
	}

	bzero(skcomp, nbytes);

	Dcmn_err(CE_NOTE, "cons_skcomp: skcomp=%p nbytes=%d",
	    (void *)skcomp, nbytes);

	skdev->skcomp_table = skcomp;
	skdev->skerr_table = (struct fit_comp_error_info *)(dma_alloc +
	    (SKD_N_COMPLETION_ENTRY * sizeof (*skcomp) / sizeof (uint64_t)));

err_out:
	return (rc);
}

/*
 *
 * Name:	skd_cons_skmsg, allocates space for the skmsg table.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	-ENOMEM if no memory otherwise NULL.
 *
 */
static int
skd_cons_skmsg(struct skd_device *skdev)
{
	dma_mem_t	*mem;
	int 		rc = 0;
	u32 		i;

	Dcmn_err(CE_NOTE, "skmsg_table kzalloc, struct %lu, count %u total %lu",
	    (ulong_t)sizeof (struct skd_fitmsg_context),
	    skdev->num_fitmsg_context,
	    (ulong_t)(sizeof (struct skd_fitmsg_context) *
	    skdev->num_fitmsg_context));

	skdev->skmsg_table = (struct skd_fitmsg_context *)kmem_zalloc(
	    sizeof (struct skd_fitmsg_context) * skdev->num_fitmsg_context,
	    KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

	for (i = 0; i < skdev->num_fitmsg_context; i++) {
		struct skd_fitmsg_context *skmsg;

		skmsg = &skdev->skmsg_table[i];

		skmsg->id = i + SKD_ID_FIT_MSG;

		skmsg->state = SKD_MSG_STATE_IDLE;

		mem = &skmsg->mb_dma_address;
		mem->alignment = SG_BOUNDARY;
		mem->cookie_count = 1;
		mem->size = SKD_N_FITMSG_BYTES + 64;
		mem->type = LITTLE_ENDIAN_DMA;

		skmsg->msg_buf = skd_alloc_dma_mem(skdev, mem, KM_SLEEP,
		    ATYPE_64BIT);

		if (NULL == skmsg->msg_buf) {
			rc = -ENOMEM;
			i++;
			break;	/* Out of for loop. */
		}

		skmsg->offset = 0;

		bzero(skmsg->msg_buf, SKD_N_FITMSG_BYTES);

		skmsg->next = &skmsg[1];
	}

	/* Free list is in order starting with the 0th entry. */
	skdev->skmsg_table[i - 1].next = NULL;
	skdev->skmsg_free_list = skdev->skmsg_table;

	return (rc);
}

/*
 *
 * Name:	skd_cons_skreq, allocates space for the skreq table.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	-ENOMEM if no memory otherwise NULL.
 *
 */
static int
skd_cons_skreq(struct skd_device *skdev)
{
	int 	rc = 0;
	size_t 	size;
	u32 	i;

	Dcmn_err(CE_NOTE,
	    "skreq_table kmem_zalloc, struct %lu, count %u total %lu",
	    (ulong_t)sizeof (struct skd_request_context),
	    skdev->num_req_context,
	    (ulong_t) (sizeof (struct skd_request_context) *
	    skdev->num_req_context));

	skdev->skreq_table = (struct skd_request_context *)kmem_zalloc(
	    sizeof (struct skd_request_context) * skdev->num_req_context,
	    KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

	for (i = 0; i < skdev->num_req_context; i++) {
		struct skd_request_context *skreq;

		skreq = &skdev->skreq_table[i];

		skreq->id = (u16)(i + SKD_ID_RW_REQUEST);
		skreq->state = SKD_REQ_STATE_IDLE;

		size = sizeof (struct scatterlist) * skdev->sgs_per_request;
		skreq->sg = (struct scatterlist *)kmem_zalloc(size, KM_SLEEP);
		/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

		bzero(skreq->sg, sizeof (*skreq->sg) *
		    skdev->sgs_per_request);

		skreq->sksg_list = skd_cons_sg_list(skdev,
		    skdev->sgs_per_request,
		    &skreq->sksg_dma_address);

		if (NULL == skreq->sksg_list) {
			rc = -ENOMEM;
			goto err_out;
		}

		skreq->next = &skreq[1];
	}

	/* Free list is in order starting with the 0th entry. */
	skdev->skreq_table[i - 1].next = NULL;
	skdev->skreq_free_list = skdev->skreq_table;

err_out:
	return (rc);
}

/*
 *
 * Name:	skd_cons_skreq, allocates space for the skreq table.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	-ENOMEM if no memory otherwise NULL.
 *
 */
static int
skd_cons_skspcl(struct skd_device *skdev)
{
	int rc = 0;
	u32 i, nbytes;

	Dcmn_err(CE_NOTE,
	    "skspcl_table kzalloc, struct %lu, count %u total %lu",
	    (ulong_t)sizeof (struct skd_special_context),
	    SKD_N_SPECIAL_CONTEXT,
	    (ulong_t)(sizeof (struct skd_special_context) *
	    SKD_N_SPECIAL_CONTEXT));

	skdev->skspcl_table = (struct skd_special_context *)kmem_zalloc(
	    sizeof (struct skd_special_context) * SKD_N_SPECIAL_CONTEXT,
	    KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

	for (i = 0; i < SKD_N_SPECIAL_CONTEXT; i++) {
		struct skd_special_context *skspcl;
		dma_mem_t		   *mem;

		skspcl = &skdev->skspcl_table[i];

		skspcl->req.id = i + SKD_ID_SPECIAL_REQUEST;
		skspcl->req.state = SKD_REQ_STATE_IDLE;

		skspcl->req.next = &skspcl[1].req;

		nbytes = SKD_N_SPECIAL_FITMSG_BYTES;

		mem 			= &skspcl->mb_dma_address;
		mem->alignment  	= SG_BOUNDARY;
		mem->cookie_count 	= 1;
		mem->size 		= nbytes;
		mem->type 		= LITTLE_ENDIAN_DMA;

		skspcl->msg_buf = skd_alloc_dma_mem(skdev, mem, KM_SLEEP,
		    ATYPE_64BIT);
		if (skspcl->msg_buf == NULL) {
			rc = -ENOMEM;
			goto err_out;
		}

		bzero(skspcl->msg_buf, nbytes);

		skspcl->req.sg = (struct scatterlist *)kmem_zalloc(
		    sizeof (struct scatterlist) * SKD_N_SG_PER_SPECIAL,
		    KM_SLEEP);
		/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

		skspcl->req.sksg_list = skd_cons_sg_list(skdev,
		    SKD_N_SG_PER_SPECIAL, &skspcl->req.sksg_dma_address);

		if (skspcl->req.sksg_list == NULL) {
			rc = -ENOMEM;
			goto err_out;
		}
	}

	/* Free list is in order starting with the 0th entry. */
	skdev->skspcl_table[i - 1].req.next = NULL;
	skdev->skspcl_free_list = skdev->skspcl_table;

	return (rc);

err_out:
	return (rc);
}

/*
 *
 * Name:	skd_cons_sksb, allocates space for the skspcl msg buf
 *		and data buf.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	-ENOMEM if no memory otherwise NULL.
 *
 */
static int
skd_cons_sksb(struct skd_device *skdev)
{
	int 				rc = 0;
	struct skd_special_context 	*skspcl;
	dma_mem_t			*mem;
	u32 				nbytes;

	skspcl = &skdev->internal_skspcl;

	skspcl->req.id = 0 + SKD_ID_INTERNAL;
	skspcl->req.state = SKD_REQ_STATE_IDLE;

	nbytes = SKD_N_INTERNAL_BYTES;

	mem 			= &skspcl->db_dma_address;
	mem->alignment  	= SG_BOUNDARY;
	mem->cookie_count 	= 1;
	mem->size 		= nbytes;
	mem->type 		= LITTLE_ENDIAN_DMA;

	/* data_buf's DMA pointer is skspcl->db_dma_address */
	skspcl->data_buf = skd_alloc_dma_mem(skdev, mem, KM_SLEEP, ATYPE_64BIT);
	if (skspcl->data_buf == NULL) {
		rc = -ENOMEM;
		goto err_out;
	}

	bzero(skspcl->data_buf, nbytes);

	nbytes = SKD_N_SPECIAL_FITMSG_BYTES;

	mem 			= &skspcl->mb_dma_address;
	mem->alignment  	= SG_BOUNDARY;
	mem->cookie_count 	= 1;
	mem->size 		= nbytes;
	mem->type 		= LITTLE_ENDIAN_DMA;

	/* msg_buf DMA pointer is skspcl->mb_dma_address */
	skspcl->msg_buf = skd_alloc_dma_mem(skdev, mem, KM_SLEEP, ATYPE_64BIT);
	if (skspcl->msg_buf == NULL) {
		rc = -ENOMEM;
		goto err_out;
	}


	bzero(skspcl->msg_buf, nbytes);

	skspcl->req.sksg_list = skd_cons_sg_list(skdev, 1,
	    &skspcl->req.sksg_dma_address);


	if (skspcl->req.sksg_list == NULL) {
		rc = -ENOMEM;
		goto err_out;
	}

	if (skd_format_internal_skspcl(skdev) == 0) {
		rc = -EINVAL;
		goto err_out;
	}

err_out:
	return (rc);
}

/*
 *
 * Name:	skd_cons_sg_list, allocates the S/G list.
 *
 * Inputs:	skdev		- device state structure.
 *		n_sg		- Number of scatter-gather entries.
 *		ret_dma_addr	- S/G list DMA pointer.
 *
 * Returns:	A list of FIT message descriptors.
 *
 */
static struct fit_sg_descriptor
*skd_cons_sg_list(struct skd_device *skdev,
    u32 n_sg, dma_mem_t *ret_dma_addr)
{
	struct fit_sg_descriptor *sg_list;
	u32 nbytes;
	dma_mem_t *mem;

	nbytes = sizeof (*sg_list) * n_sg;

	mem 			= ret_dma_addr;
	mem->alignment  	= SG_BOUNDARY;
	mem->cookie_count 	= 1;
	mem->size 		= nbytes;
	mem->type 		= LITTLE_ENDIAN_DMA;

	/* sg_list's DMA pointer is *ret_dma_addr */
	sg_list = skd_alloc_dma_mem(skdev, mem, KM_SLEEP, ATYPE_32BIT);

	if (sg_list != NULL) {
		uint64_t dma_address = ret_dma_addr->cookie.dmac_laddress;
		u32 i;

		bzero(sg_list, nbytes);

		for (i = 0; i < n_sg - 1; i++) {
			uint64_t ndp_off;
			ndp_off = (i + 1) * sizeof (struct fit_sg_descriptor);

			sg_list[i].next_desc_ptr = dma_address + ndp_off;
		}
		sg_list[i].next_desc_ptr = 0LL;
	}

	return (sg_list);
}

/*
 * DESTRUCT (FREE)
 */

static void skd_free_skcomp(struct skd_device *skdev);
static void skd_free_skmsg(struct skd_device *skdev);
static void skd_free_skreq(struct skd_device *skdev);
static void skd_free_skspcl(struct skd_device *skdev);
static void skd_free_sksb(struct skd_device *skdev);

static void skd_free_sg_list(struct skd_device *skdev,
    struct fit_sg_descriptor *sg_list,
    u32 n_sg, dma_mem_t dma_addr);

/*
 *
 * Name:	skd_destruct, call various rouines to deallocate
 *		space acquired during initialization.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_destruct(struct skd_device *skdev)
{
	if (skdev == NULL) {
		return;
	}

	if (skd_dev_table[skdev->devno % SKD_N_DEV_TABLE] == skdev) {
		skd_dev_table[skdev->devno % SKD_N_DEV_TABLE] = NULL;
	} else {
		cmn_err(CE_NOTE, "dev not in its slot %d", skdev->devno);
	}

	Dcmn_err(CE_NOTE, "destruct sksb");
	skd_free_sksb(skdev);

	Dcmn_err(CE_NOTE, "destruct skspcl");
	skd_free_skspcl(skdev);

	Dcmn_err(CE_NOTE, "destruct skreq");
	skd_free_skreq(skdev);

	Dcmn_err(CE_NOTE, "destruct skmsg");
	skd_free_skmsg(skdev);

	Dcmn_err(CE_NOTE, "destruct skcomp");
	skd_free_skcomp(skdev);

	Dcmn_err(CE_NOTE, "DESTRUCT VICTORY");
}

/*
 *
 * Name:	skd_free_skcomp, deallocates skcomp table DMA resources.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_free_skcomp(struct skd_device *skdev)
{
	if (skdev->skcomp_table != NULL) {
		skd_free_dma_resource(skdev, &skdev->cq_dma_address);
	}

	skdev->skcomp_table = NULL;
}

/*
 *
 * Name:	skd_free_skmsg, deallocates skmsg table DMA resources.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_free_skmsg(struct skd_device *skdev)
{
	u32 		i;

	if (NULL == skdev->skmsg_table)
		return;

	for (i = 0; i < skdev->num_fitmsg_context; i++) {
		struct skd_fitmsg_context *skmsg;

		skmsg = &skdev->skmsg_table[i];

		if (skmsg->msg_buf != NULL) {
			skd_free_dma_resource(skdev, &skmsg->mb_dma_address);
		}


		skmsg->msg_buf = NULL;
	}

	kmem_free(skdev->skmsg_table, sizeof (struct skd_fitmsg_context) *
	    skdev->num_fitmsg_context);

	skdev->skmsg_table = NULL;

}

/*
 *
 * Name:	skd_free_skreq, deallocates skspcl table DMA resources.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_free_skreq(struct skd_device *skdev)
{
	u32 i;

	if (NULL == skdev->skreq_table)
		return;

	for (i = 0; i < skdev->num_req_context; i++) {
		struct skd_request_context *skreq;

		skreq = &skdev->skreq_table[i];

		skd_free_sg_list(skdev, skreq->sksg_list,
		    skdev->sgs_per_request, skreq->sksg_dma_address);

		skreq->sksg_list = NULL;

		kmem_free(skreq->sg, sizeof (struct scatterlist) *
		    skdev->sgs_per_request);
	}

	kmem_free(skdev->skreq_table, sizeof (struct skd_request_context) *
	    skdev->num_req_context);

	skdev->skreq_table = NULL;

}

/*
 *
 * Name:	skd_free_skspcl, deallocates skspcl table DMA resources.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_free_skspcl(struct skd_device *skdev)
{
	u32 i;

	if (NULL == skdev->skspcl_table)
		return;

	for (i = 0; i < SKD_N_SPECIAL_CONTEXT; i++) {
		struct skd_special_context *skspcl;

		skspcl = &skdev->skspcl_table[i];

		if (skspcl->msg_buf != NULL) {
			skd_free_dma_resource(skdev, &skspcl->mb_dma_address);
		}

		skspcl->msg_buf = NULL;

		skd_free_sg_list(skdev, skspcl->req.sksg_list, 1,
		    skspcl->req.sksg_dma_address);

		skspcl->req.sksg_list = NULL;

		kmem_free(skspcl->req.sg, sizeof (struct scatterlist) *
		    SKD_N_SG_PER_SPECIAL);
	}


	kmem_free(skdev->skspcl_table, sizeof (struct skd_special_context) *
	    SKD_N_SPECIAL_CONTEXT);

	skdev->skspcl_table = NULL;
}

/*
 *
 * Name:	skd_free_sksb, deallocates skspcl data buf and
 *		msg buf DMA resources.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_free_sksb(struct skd_device *skdev)
{
	struct skd_special_context *skspcl;

	skspcl = &skdev->internal_skspcl;

	if (skspcl->data_buf != NULL) {
		skd_free_dma_resource(skdev, &skspcl->db_dma_address);
	}

	skspcl->data_buf = NULL;

	if (skspcl->msg_buf != NULL) {
		skd_free_dma_resource(skdev, &skspcl->mb_dma_address);
	}

	skspcl->msg_buf = NULL;

	skd_free_sg_list(skdev, skspcl->req.sksg_list, 1,
	    skspcl->req.sksg_dma_address);

	skspcl->req.sksg_list = NULL;
}

/*
 *
 * Name:	skd_free_sg_list, deallocates S/G DMA resources.
 *
 * Inputs:	skdev		- device state structure.
 *		sg_list		- S/G list itself.
 *		n_sg		- nukmber of segments
 *		dma_addr	- S/G list DMA address.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_free_sg_list(struct skd_device *skdev,
    struct fit_sg_descriptor *sg_list,
    u32 n_sg, dma_mem_t dma_addr)
{
	if (sg_list != NULL) {
		skd_free_dma_resource(skdev, &dma_addr);
	}
}

/*
 *
 * Name:	skd_open, open the device.
 *
 * Inputs:	skdev		- device state structure.
 *		flags		- open flags.
 *		otyp 		- not used.
 *		credp		- not used.
 *
 * Returns:	Zero on success. ENXIO or EAGAIN on failure.
 *
 */
static int
skd_open(dev_t *devp, int flags, int otyp, cred_t *credp)
{
	dev_t		dev = *devp;
	minor_t		instance;
	skd_device_t	*skdev;

	_NOTE(ARGUNUSED(otyp));
	_NOTE(ARGUNUSED(credp));

	instance = SKD_INST(dev);

	skdev    = (skd_device_t *)ddi_get_soft_state(skd_state, instance);

	if (skdev == NULL) {
		cmn_err(CE_NOTE, "skd_open: NULL skdev");

		return (ENXIO);
	}

	if ((flags & FEXCL) && !skdev->device_count) {
		cmn_err(CE_NOTE, "skd_open: NULL device_count");

		return (EAGAIN);
	}

	return (0);
}

/*
 *
 * Name:	skd_close, closes the device.
 *
 * Inputs:	skdev		- device state structure.
 *		flags		- not used.
 *		otyp 		- not used
 *		credp		- not used.
 *
 * Returns:	Zero on success. ENXIO or EAGAIN on failure.
 *
 */
static int
skd_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
	minor_t		instance;
	skd_device_t	*skdev;

	_NOTE(ARGUNUSED(flag));
	_NOTE(ARGUNUSED(otyp));
	_NOTE(ARGUNUSED(credp));

	instance = SKD_INST(dev);

	skdev    = (skd_device_t *)ddi_get_soft_state(skd_state, instance);


	if (skdev == NULL) {
		cmn_err(CE_NOTE, "skd_close: bad instance#:%d\n",
		    SKD_INST(dev));

		return (ENXIO);
	}

	return (0);
}

#define	b_cylin	b_resid

/*
 *
 * Name:	skd_disksort, places bp request in proper place in list.
 *
 * Inputs:	dp		- hashed buffer queue.
 *		bp		- buf request structure.
 *
 * Returns:	Nothing.
 *
 */
void
skd_disksort(register struct diskhd *dp, register struct buf *bp)
{
	register struct buf *ap;

	/*
	 * If nothing on the activity queue, then
	 * we become the only thing.
	 */
	ap = dp->b_actf;
	if (ap == NULL) {
		dp->b_actf = bp;
		dp->b_actl = bp;
		bp->av_forw = NULL;
		return;
	}
	/*
	 * If we lie after the first (currently active)
	 * request, then we must locate the second request list
	 * and add ourselves to it.
	 */
	if (bp->b_cylin < ap->b_cylin) {
		while (ap->av_forw) {
			/*
			 * Check for an ``inversion'' in the
			 * normally ascending cylinder numbers,
			 * indicating the start of the second request list.
			 */
			if (ap->av_forw->b_cylin < ap->b_cylin) {
				/*
				 * Search the second request list
				 * for the first request at a larger
				 * cylinder number.  We go before that;
				 * if there is no such request, we go at end.
				 */
				do {
					if (bp->b_cylin < ap->av_forw->b_cylin)
						goto insert;
					ap = ap->av_forw;
				} while (ap->av_forw);
				goto insert;		/* after last */
			}
			ap = ap->av_forw;
		}
		/*
		 * No inversions... we will go after the last, and
		 * be the first request in the second request list.
		 */
		goto insert;
	}
	/*
	 * Request is at/after the current request...
	 * sort in the first request list.
	 */
	while (ap->av_forw) {
		/*
		 * We want to go after the current request
		 * if there is an inversion after it (i.e. it is
		 * the end of the first request list), or if
		 * the next request is a larger cylinder than our request.
		 */
		if (ap->av_forw->b_cylin < ap->b_cylin ||
		    bp->b_cylin < ap->av_forw->b_cylin)
			goto insert;
		ap = ap->av_forw;
	}
	/*
	 * Neither a second list nor a larger
	 * request... we go at the end of the first list,
	 * which is the same as the end of the whole schebang.
	 */
insert:
	bp->av_forw = ap->av_forw;
	ap->av_forw = bp;
	if (ap == dp->b_actl)
		dp->b_actl = bp;
}

/*
 *
 * Name:	skd_queue_bp, queues the buf request.
 *
 * Inputs:	skdev		- device state structure.
 *		drive		- target drive.
 *		bp		- buf structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_queue_bp(skd_device_t *skdev, int drive, struct buf *bp)
{
	struct diskhd *dp;

	if (bp == NULL) {
		cmn_err(CE_NOTE, "skd_queue_bp: NULL bp");

		return;
	}

	if (drive >= SKD_NUMDRIVES) {
		cmn_err(CE_NOTE, "skd_queue_bp: drive# of range: %d", drive);

		return;
	}

	dp = &skdev->waitqueue[drive];

	disksort(dp, bp);
}

/*
 *
 * Name:	skd_cancel_pending_requests
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Returns number of entries canceled.
 *
 */
static int
skd_cancel_pending_requests(skd_device_t *skdev)
{
	struct skd_request_context *skreq;
	struct buf		   *bp;
	int			   inx, count = 0;

	for (inx = 0; inx < skdev->num_req_context; inx++) {
		if ((skreq = &skdev->skreq_table[inx]) != NULL &&
		    (skreq->did_complete == 0) &&
		    (bp = skreq->bp)) {
			if (bp->b_flags & B_BUSY &&
			    !(bp->b_flags & B_DONE)) {
				count++;
				printf("skreq[%d]: busy bp - CANCELED\n", inx);
				(void) skd_list_skreq(skdev, 1);
			}
		}
	}

	return (count);
}

/*
 *
 * Name:	skd_list_skreq, displays the skreq table entries.
 *
 * Inputs:	skdev		- device state structure.
 *		list		- flag, if true displays the entry address.
 *
 * Returns:	Returns number of skmsg entries found.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_list_skreq(skd_device_t *skdev, int list)
{
	int	inx = 0;
	struct skd_request_context *skreq;

	if (list) {
		printf("skreq_table[0]\n");

		skreq = &skdev->skreq_table[0];
		while (skreq) {
			if (list)
				printf(
				    "%d: skreq=%p state=%d id=%x fid=%x "
				    "bp=%p dir=%d comp=%d\n",
				    inx, (void *)skreq, skreq->state,
				    skreq->id, skreq->fitmsg_id,
				    (void *)skreq->bp,
				    skreq->sg_data_dir, skreq->did_complete);
			inx++;
			skreq = skreq->next;
		}
	}

	inx = 0;
	skreq = skdev->skreq_free_list;

	if (list)
		printf("skreq_free_list\n");
	while (skreq) {
		if (list)
			printf("%d: skreq=%p state=%d id=%x fid=%x bp=%p "
			    "dir=%d\n", inx, (void *)skreq, skreq->state,
			    skreq->id, skreq->fitmsg_id, (void *)skreq->bp,
			    skreq->sg_data_dir);
		inx++;
		skreq = skreq->next;
	}

	return (inx);
}

/*
 *
 * Name:	skd_list_skmsg, displays the skmsg table entries.
 *
 * Inputs:	skdev		- device state structure.
 *		list		- flag, if true displays the entry address.
 *
 * Returns:	Returns number of skmsg entries found.
 *
 */
static int
skd_list_skmsg(skd_device_t *skdev, int list)
{
	int	inx = 0;
	struct skd_fitmsg_context *skmsgp;

	skmsgp = &skdev->skmsg_table[0];

	if (list) {
		printf("skmsg_table[0]\n");

		while (skmsgp) {
			if (list)
				printf("%d: skmsgp=%p id=%x outs=%d l=%d "
				    "o=%d nxt=%p\n", inx, (void *)skmsgp,
				    skmsgp->id, skmsgp->outstanding,
				    skmsgp->length, skmsgp->offset,
				    (void *)skmsgp->next);
			inx++;
			skmsgp = skmsgp->next;
		}
	}

	inx = 0;
	if (list)
		printf("skmsg_free_list\n");
	skmsgp = skdev->skmsg_free_list;
	while (skmsgp) {
		if (list)
			printf("%d: skmsgp=%p id=%x outs=%d l=%d o=%d nxt=%p\n",
			    inx, (void *)skmsgp, skmsgp->id,
			    skmsgp->outstanding, skmsgp->length,
			    skmsgp->offset, (void *)skmsgp->next);
		inx++;
		skmsgp = skmsgp->next;
	}

	return (inx);
}

/*
 *
 * Name:	skd_blk_peek_request, retrieves top of queue entry.
 *
 * Inputs:	skdev		- device state structure.
 *		drive		- device number
 *
 * Returns:	Returns the top of the job queue entry.
 *
 */
static struct buf
*skd_blk_peek_request(skd_device_t *skdev, int drive)
{
	struct diskhd *ioq;
	struct buf    *bp;

	ioq = &skdev->waitqueue[drive];
	bp  =  ioq->av_forw;

	return (bp);
}

/*
 *
 * Name:	skd_get_queue_bp, retrieves top of queue entry and
 *		delinks entry from the queue.
 *
 * Inputs:	skdev		- device state structure.
 *		drive		- device number
 *
 * Returns:	Returns the top of the job queue entry.
 *
 */
static struct buf
*skd_get_queued_bp(skd_device_t *skdev, int drive)
{
	struct diskhd *ioq;
	struct buf    *bp = NULL;

	ioq = &skdev->waitqueue[drive];
	bp  =  ioq->av_forw;

	if (bp == NULL) {
		return (NULL);
	}

	ioq->av_forw = bp->av_forw;
	bp->av_forw  = NULL;

	return (bp);
}

/*
 *
 * Name:	skd_check_pending_interrupt, reads interrupt register.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Returns interrupt status.
 *
 */
static int
skd_check_pending_interrupt(skd_device_t *skdev)
{
	u32 intstat, ack;

	intstat = SKD_READL(skdev, FIT_INT_STATUS_HOST);

	ack  = FIT_INT_DEF_MASK;
	ack &= intstat;

	return (ack);
}

/*
 *
 * Name:	skd_handle_panic_syncfs, determines if there's an
 *		interrupt pending. Called during panic to sync.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_handle_panic_syncfs(skd_device_t *skdev)
{
	if (skd_check_pending_interrupt(skdev)) {
		(void) skd_isr_aif((caddr_t)skdev, 0);
	} else {
		int inx, got_int = 0;

		(void) skd_start(skdev, 8);

		for (inx = 0; inx < 0x7fffffff; inx++) {
			if (skd_check_pending_interrupt(skdev)) {
				got_int++;
				break;
			}
		}

		if (got_int)
			(void) skd_isr_aif((caddr_t)skdev, 0);
	}
}

/*
 *
 * Name:	skd_strategy, used to perform I/O to P0 partition.
 *
 * Inputs:	bp		- buf structure
 *
 * Returns:	Returns zero if unable to retrieve device state
 *		structure. Also returns 0 if we're out of resources.
 *		Returns zero on no error.  Errors are expressed through
 *		the skd_io_done() path.
 *
 */
static int
skd_strategy(struct buf *bp)
{
	int			drive = 0;
	int			slice;
	int			blk_count;
	uint64_t		blockno;
	minor_t			instance;
	minor_t 		minor;
	skd_device_t		*skdev;
	skd_buf_private_t	*pbuf;
	diskaddr_t		bp_lba = (diskaddr_t)0;

	instance = SKD_INST(bp->b_edev);
	minor    = getminor(bp->b_edev);
	slice    = minor & SKD_DEV_SLICEMASK;

	if (slice == SKD_P0)
		minor |= SKD_FULLDISK;

	Dcmn_err(CE_NOTE, "skd_strategy:enter: %s bp=%p minor=%x",
	    (bp->b_flags & B_READ) ? "READ" : "WRITE", (void *)bp, minor);

	skdev    = (skd_device_t *)ddi_get_soft_state(skd_state, instance);
	if (skdev == NULL) {
		cmn_err(CE_PANIC, "skd_strategy: NULL skdev instance=%d",
		    instance);

		skd_end_request_abnormal(skdev, bp, EINVAL, SKD_IODONE_WNIOC);

		return (0);
	}

	bp->b_flags &= ~B_DONE;

	bp->b_private = (void *)kmem_zalloc(sizeof (skd_buf_private_t),
	    KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */
	pbuf = (skd_buf_private_t *)bp->b_private;
	pbuf->origin  = ORIGIN_SKD;

	if ((int)skdev->queue_depth_busy < 0)
		skdev->queue_depth_busy = 0;

	/* Are too many requests already in progress? */
	if (skdev->queue_depth_busy >= skdev->queue_depth_limit) {
		cmn_err(CE_NOTE, "QFULL: qdepth %d, limit %d\n",
		    skdev->queue_depth_busy,
		    skdev->queue_depth_limit);

		skd_end_request_abnormal(skdev, bp, EAGAIN, SKD_IODONE_WNIOC);

		return (0);
	}

	blockno   = bp->b_lblkno;
	blk_count = (bp->b_bcount) >> BLK_BSHIFT;

	if (blk_count == 0)	{ /* Solaris zero block count request !!! */
		bp->b_bcount = (bp->b_bufsize) >> BLK_BSHIFT;
	}

	Dcmn_err(CE_NOTE,
	    "<<< strategy: minor=%x slice=%d blockno=%" PRId64
	    " blk_count=%d >>>", minor, slice, blockno, blk_count);

	/*
	 * And now......... slice stuff.
	 * First - whole disk access - internal I/O requests.
	 */
	if (minor & SKD_FULLDISK) {
		int	 f_part;
		int	 s_block;
		uint64_t s_numblks;

		f_part = (slice >= 16) ? slice - SKD_P0 : slice;

		s_block   = skd_getpartstart(skdev, drive, f_part);
		bp_lba    = s_block;

		s_numblks = (slice >= SKD_P0) ?
		    skd_partsize(skdev, drive, f_part) : skdev->Nblocks;

		Dcmn_err(CE_NOTE, "FULL part=%d s_block=%d s_numblks=%" PRId64
		    " slice=%d", f_part, s_block, s_numblks, slice);

		if ((blockno + (blk_count-1)) > s_numblks) {
			cmn_err(CE_NOTE, "Request LBA %" PRIu64 " (0x%" PRIx64
			    ") extends beyond last LBA %" PRIu64 " (0x%" PRIx64
			    ")", blockno, blockno, s_numblks, s_numblks);

			cmn_err(CE_NOTE,
			    "strat FULL, part=%d s_block=%d s_numblks=%" PRIu64
			    " lastblk=%" PRIu64,
			    f_part, s_block, s_numblks, s_block+s_numblks);
			bp->b_resid = bp->b_bcount;

			skd_end_request_abnormal(skdev, bp, EINVAL,
			    SKD_IODONE_WNIOC);

			return (0);
		}

		bp->b_lblkno = blockno;
	} else {	/* referencing slices c*d*s[0..15] */
		cmn_err(CE_PANIC, "skd_strategy: should not be here");
	}

	/*
	 * Queue the request.
	 */
	bp->av_forw = NULL;
	bp->av_back = NULL;

	/*
	 * Add the slice offset.
	 */
	bp->b_lblkno = bp->b_lblkno + bp_lba;
	bp->b_resid  = bp->b_lblkno;

	pbuf->origin = ORIGIN_SKD;

	skd_queue_bp(skdev, drive, bp);

	if (skdev->active_cmds > skdev->active_cmds_max)
		skdev->active_cmds_max = skdev->active_cmds;

	/*
	 * Hmmmm, panic sync mode?
	 */
	if (do_polled_io == 0)
		(void) skd_start(skdev, 98);
	else
		skd_handle_panic_syncfs(skdev);

	return (0);
}

/*
 * PCI DRIVER GLUE
 */

/*
 *
 * Name:	skd_pci_info, logs certain device PCI info.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	str which contains the device speed info..
 *
 */
static char *
skd_pci_info(struct skd_device *skdev, char *str)
{
	int pcie_reg;

	(void) strcpy(str, "PCIe (");
	pcie_reg = skd_pci_find_capability(skdev, PCI_CAP_ID_EXP);

	if (pcie_reg) {

		char lwstr[16];
		uint16_t lstat, lspeed, lwidth;

		pcie_reg += 0x12;
		lstat  = skd_pci_get16(skdev, pcie_reg);
		lspeed = lstat & (0xF);
		lwidth = (lstat & 0x3F0) >> 4;

		if (lspeed == 1)
			(void) strcat(str, "2.5GT/s ");
		else if (lspeed == 2)
			(void) strcat(str, "5.0GT/s ");
		else
			(void) strcat(str, "<unknown> ");
		(void) snprintf(lwstr, sizeof (lwstr), "rev %d)", lwidth);
		(void) strcat(str, lwstr);
	}

	return (str);
}

/*
 * MODULE GLUE
 */

/*
 *
 * Name:	skd_init, initializes certain values.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Zero.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_init(skd_device_t *skdev)
{
	Dcmn_err(CE_NOTE, "skd_init: v%s-b%s\n", DRV_VERSION, DRV_BUILD_ID);

	switch (skd_isr_type) {
	case SKD_IRQ_LEGACY:
	case SKD_IRQ_MSI:
	case SKD_IRQ_MSIX:
		break;
	default:
		cmn_err(CE_NOTE, "skd_isr_type %d invalid, re-set to %d\n",
		    skd_isr_type, SKD_IRQ_DEFAULT);
		skd_isr_type = SKD_IRQ_DEFAULT;
	}

	if (skd_max_queue_depth < 1 ||
	    skd_max_queue_depth > SKD_MAX_QUEUE_DEPTH) {
		cmn_err(CE_NOTE, "skd_max_q_depth %d invalid, re-set to %d\n",
		    skd_max_queue_depth, SKD_MAX_QUEUE_DEPTH_DEFAULT);
		skd_max_queue_depth = SKD_MAX_QUEUE_DEPTH_DEFAULT;
	}

	if (skd_max_req_per_msg < 1 || skd_max_req_per_msg > 14) {
		cmn_err(CE_NOTE, "skd_max_req_per_msg %d invalid, set to %d\n",
		    skd_max_req_per_msg, SKD_MAX_REQ_PER_MSG_DEFAULT);
		skd_max_req_per_msg = SKD_MAX_REQ_PER_MSG_DEFAULT;
	}


	if (skd_sgs_per_request < 1 || skd_sgs_per_request > 4096) {
		cmn_err(CE_NOTE, "skd_sg_per_request %d invalid, set to %d\n",
		    skd_sgs_per_request, SKD_N_SG_PER_REQ_DEFAULT);
		skd_sgs_per_request = SKD_N_SG_PER_REQ_DEFAULT;
	}

	if (skd_dbg_level < 0 || skd_dbg_level > 2) {
		cmn_err(CE_NOTE, "skd_dbg_level %d invalid, re-set to %d\n",
		    skd_dbg_level, 0);
		skd_dbg_level = 0;
	}

	return (0);
}

/*
 *
 * Name:	skd_exit, exits the driver & logs the fact.
 *
 * Inputs:	none.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_exit(void)
{
	cmn_err(CE_NOTE, "skd v%s unloading", DRV_VERSION);
}

/*
 *
 * Name:	skd_drive_state_to_str, converts binary drive state
 *		to its corresponding string value.
 *
 * Inputs:	Drive state.
 *
 * Returns:	String representing drive state.
 *
 */
const char *
skd_drive_state_to_str(int state)
{
	switch (state) {
	case FIT_SR_DRIVE_OFFLINE:	return ("OFFLINE");
	case FIT_SR_DRIVE_INIT:		return ("INIT");
	case FIT_SR_DRIVE_ONLINE:	return ("ONLINE");
	case FIT_SR_DRIVE_BUSY:		return ("BUSY");
	case FIT_SR_DRIVE_FAULT:	return ("FAULT");
	case FIT_SR_DRIVE_DEGRADED:	return ("DEGRADED");
	case FIT_SR_PCIE_LINK_DOWN:	return ("LINK_DOWN");
	case FIT_SR_DRIVE_SOFT_RESET:	return ("SOFT_RESET");
	case FIT_SR_DRIVE_NEED_FW_DOWNLOAD: return ("NEED_FW");
	case FIT_SR_DRIVE_INIT_FAULT:	return ("INIT_FAULT");
	case FIT_SR_DRIVE_BUSY_SANITIZE:return ("BUSY_SANITIZE");
	case FIT_SR_DRIVE_BUSY_ERASE:	return ("BUSY_ERASE");
	case FIT_SR_DRIVE_FW_BOOTING:	return ("FW_BOOTING");
	default:			return ("???");
	}
}

/*
 *
 * Name:	skd_skdev_state_to_str, converts binary driver state
 *		to its corresponding string value.
 *
 * Inputs:	Driver state.
 *
 * Returns:	String representing driver state.
 *
 */
const char *
skd_skdev_state_to_str(enum skd_drvr_state state)
{
	switch (state) {
	case SKD_DRVR_STATE_LOAD:	return ("LOAD");
	case SKD_DRVR_STATE_IDLE:	return ("IDLE");
	case SKD_DRVR_STATE_BUSY:	return ("BUSY");
	case SKD_DRVR_STATE_STARTING:	return ("STARTING");
	case SKD_DRVR_STATE_ONLINE:	return ("ONLINE");
	case SKD_DRVR_STATE_PAUSING:	return ("PAUSING");
	case SKD_DRVR_STATE_PAUSED:	return ("PAUSED");
	case SKD_DRVR_STATE_DRAINING_TIMEOUT: return ("DRAINING_TIMEOUT");
	case SKD_DRVR_STATE_RESTARTING:	return ("RESTARTING");
	case SKD_DRVR_STATE_RESUMING:	return ("RESUMING");
	case SKD_DRVR_STATE_STOPPING:	return ("STOPPING");
	case SKD_DRVR_STATE_SYNCING:	return ("SYNCING");
	case SKD_DRVR_STATE_FAULT:	return ("FAULT");
	case SKD_DRVR_STATE_DISAPPEARED: return ("DISAPPEARED");
	case SKD_DRVR_STATE_BUSY_ERASE:	return ("BUSY_ERASE");
	case SKD_DRVR_STATE_BUSY_SANITIZE:return ("BUSY_SANITIZE");
	case SKD_DRVR_STATE_BUSY_IMMINENT: return ("BUSY_IMMINENT");
	case SKD_DRVR_STATE_WAIT_BOOT:  return ("WAIT_BOOT");

	default:			return ("???");
	}
}

/*
 *
 * Name:	skd_skmsg_state_to_str, converts binary driver state
 *		to its corresponding string value.
 *
 * Inputs:	Msg state.
 *
 * Returns:	String representing msg state.
 *
 */
const char *
skd_skmsg_state_to_str(enum skd_fit_msg_state state)
{
	switch (state) {
	case SKD_MSG_STATE_IDLE:	return ("IDLE");
	case SKD_MSG_STATE_BUSY:	return ("BUSY");
	default:			return ("???");
	}
}

/*
 *
 * Name:	skd_skreq_state_to_str, converts binary req state
 *		to its corresponding string value.
 *
 * Inputs:	Req state.
 *
 * Returns:	String representing req state.
 *
 */
const char *
skd_skreq_state_to_str(enum skd_req_state state)
{
	switch (state) {
	case SKD_REQ_STATE_IDLE:	return ("IDLE");
	case SKD_REQ_STATE_SETUP:	return ("SETUP");
	case SKD_REQ_STATE_BUSY:	return ("BUSY");
	case SKD_REQ_STATE_COMPLETED:	return ("COMPLETED");
	case SKD_REQ_STATE_TIMEOUT:	return ("TIMEOUT");
	case SKD_REQ_STATE_ABORTED:	return ("ABORTED");
	default:			return ("???");
	}
}

/*
 *
 * Name:	skd_log_skdev, logs device state & parameters.
 *
 * Inputs:	skdev		- device state structure.
 *		event		- event (string) to log.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_log_skdev(struct skd_device *skdev, const char *event)
{
	Dcmn_err(CE_NOTE, "log_skdev(%s) skdev=%p event='%s'",
	    skdev->name, (void *)skdev, event);
	Dcmn_err(CE_NOTE, "  drive_state=%s(%d) driver_state=%s(%d)",
	    skd_drive_state_to_str(skdev->drive_state), skdev->drive_state,
	    skd_skdev_state_to_str(skdev->state), skdev->state);
	Dcmn_err(CE_NOTE, "  busy=%d limit=%d soft=%d hard=%d lowat=%d",
	    skdev->queue_depth_busy, skdev->queue_depth_limit,
	    skdev->soft_queue_depth_limit, skdev->hard_queue_depth_limit,
	    skdev->queue_depth_lowat);
	Dcmn_err(CE_NOTE, "  timestamp=0x%x cycle=%d cycle_ix=%d",
	    skdev->timeout_stamp, skdev->skcomp_cycle, skdev->skcomp_ix);
}

/*
 *
 * Name:	skd_log_skmsg, logs the skmsg event.
 *
 * Inputs:	skdev		- device state structure.
 *		skmsg		- FIT message structure.
 *		event		- event string to log.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_log_skmsg(struct skd_device *skdev,
    struct skd_fitmsg_context *skmsg, const char *event)
{
	cmn_err(CE_NOTE, "log_skmsg:(%s) skmsg=%p event='%s'",
	    skdev->name, (void *)skmsg, event);
	cmn_err(CE_NOTE, "  state=%s(%d) id=0x%04x length=%d",
	    skd_skmsg_state_to_str(skmsg->state), skmsg->state,
	    skmsg->id, skmsg->length);
}

/*
 *
 * Name:	skd_log_skreq, logs the skreq event.
 *
 * Inputs:	skdev		- device state structure.
 *		skreq		-skreq structure.
 *		event		- event string to log.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_log_skreq(struct skd_device *skdev,
    struct skd_request_context *skreq, const char *event)
{
	struct buf *bp;

	cmn_err(CE_NOTE, "log_skreq: (%s) skreq=%p bp=%p event='%s'",
	    skdev->name, (void *)skreq, (void *)skreq->bp, event);
	cmn_err(CE_NOTE, "  state=%s(%d) id=0x%04x fitmsg=0x%04x",
	    skd_skreq_state_to_str(skreq->state), skreq->state,
	    skreq->id, skreq->fitmsg_id);
	cmn_err(CE_NOTE, "  timo=0x%x sg_dir=%d n_sg=%d",
	    skreq->timeout_stamp, skreq->sg_data_dir, skreq->n_sg);
	if ((bp = skreq->bp) != NULL) {
		u32 lba, count;

		lba   = (u32) bp->b_blkno;
		count = (u32) bp->b_bcount;
		cmn_err(CE_NOTE, "  bp=%p lba=%u(0x%x) b_bcount=%u(0x%x) ",
		    (void *)bp, lba, lba, count, count);
		cmn_err(CE_NOTE, "  slice=%lx dir=%s "
		    " intrs=%" PRId64 " qdepth=%d",
		    bp->b_edev, (bp->b_flags & B_READ) ? "Read" : "Write",
		    skdev->intr_cntr, skdev->queue_depth_busy);
	} else {
		cmn_err(CE_NOTE, "  req=NULL\n");
	}
}

/*
 *
 * Name:	skd_isr, interrupt handler.
 *
 * Inputs:	arg1		- device state structure.
 *
 * Returns:	Returns value of skd_isr_aif().
 *
 */
uint_t
skd_isr(caddr_t arg1)
{
	return (skd_isr_aif(arg1, 0));
}

/*
 *
 * Name:	skd_init_mutex, initializes all mutexes.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	DDI_FAILURE on failure otherwise DDI_SUCCESS.
 *
 */
static int
skd_init_mutex(skd_device_t *skdev)
{
	int	ret;
	void	*intr;

	Dcmn_err(CE_CONT, "(%s%d): init_mutex flags=%x", DRV_NAME,
	    skdev->instance, skdev->flags);


	if (skdev->iflags & IFLG_INTR_AIF) {
		intr = (void *)(uintptr_t)skdev->intr_pri;
	} else {
		/* Get iblock cookies to initialize mutexes */
		if ((ret = ddi_get_iblock_cookie(skdev->dip, 0,
		    &skdev->iblock_cookie)) != DDI_SUCCESS) {
			cmn_err(CE_NOTE, "failed, get_iblock: %xh", ret);

			return (DDI_FAILURE);
		}

		intr = (void *)skdev->iblock_cookie;
	}

	if (skdev->flags & SKD_MUTEX_INITED)
		cmn_err(CE_NOTE, "init_mutex: Oh-Oh - already INITED");

	/* mutexes to protect the adapter state structure. */
	mutex_init(&skdev->skd_lock_mutex, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(intr));
	mutex_init(&skdev->skd_intr_mutex, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(intr));
	mutex_init(&skdev->waitqueue_mutex[0], NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(intr));
	mutex_init(&skdev->skd_internalio_mutex, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(intr));
	mutex_init(&skdev->skd_s1120wait_mutex, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(intr));

	cv_init(&skdev->cv_waitq, NULL, CV_DRIVER, NULL);
	cv_init(&skdev->cv_s1120wait, NULL, CV_DRIVER, NULL);

	skdev->flags |= SKD_MUTEX_INITED;
	if (skdev->flags & SKD_MUTEX_DESTROYED)
		skdev->flags &= ~SKD_MUTEX_DESTROYED;

	Dcmn_err(CE_CONT, "init_mutex (%s%d): done, flags=%x", DRV_NAME,
	    skdev->instance, skdev->flags);

	return (DDI_SUCCESS);
}

/*
 *
 * Name:	skd_destroy_mutex, destroys all mutexes.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_destroy_mutex(skd_device_t *skdev)
{
	if ((skdev->flags & SKD_MUTEX_DESTROYED) == 0) {
		if (skdev->flags & SKD_MUTEX_INITED) {
			mutex_destroy(&skdev->waitqueue_mutex[0]);
			mutex_destroy(&skdev->skd_intr_mutex);
			mutex_destroy(&skdev->skd_lock_mutex);
			mutex_destroy(&skdev->skd_internalio_mutex);
			mutex_destroy(&skdev->skd_s1120wait_mutex);

			cv_destroy(&skdev->cv_waitq);
			cv_destroy(&skdev->cv_s1120wait);

			skdev->flags |= SKD_MUTEX_DESTROYED;

			if (skdev->flags & SKD_MUTEX_INITED)
				skdev->flags &= ~SKD_MUTEX_INITED;
		}
	}
}

/*
 *
 * Name:	skd_setup_msi, setup the MSI interrupt handling capability.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	DDI_FAILURE on failure otherwise DDI_SUCCESS.
 *
 */
static int
skd_setup_msi(skd_device_t *skdev)
{
	int32_t		count = 0;
	int32_t		avail = 0;
	int32_t		actual = 0;
	int32_t		msitype = DDI_INTR_TYPE_MSI;
	int32_t		ret;
	skd_ifunc_t	itrfun[1] = {0};

	Dcmn_err(CE_CONT, "(%d): setup_msi, flags=%x", skdev->instance,
	    skdev->flags);

	if (skd_disable_msi != 0) {
		cmn_err(CE_NOTE, "MSI is disabled by user");

		return (DDI_FAILURE);
	}

	/* Get number of MSI interrupts the system supports */
	if (((ret = ddi_intr_get_nintrs(skdev->dip, msitype, &count)) !=
	    DDI_SUCCESS) || count == 0) {
		cmn_err(CE_NOTE, "failed, nintrs ret=%xh, cnt=%xh", ret, count);

		return (DDI_FAILURE);
	}

	/* Get number of available MSI interrupts */
	if (((ret = ddi_intr_get_navail(skdev->dip, msitype, &avail)) !=
	    DDI_SUCCESS) || avail == 0) {
		cmn_err(CE_NOTE, "failed, navail ret=%xh, avail=%xh",
		    ret, avail);

		return (DDI_FAILURE);
	}

	/* MSI requires only 1.  */
	count = 1;
	itrfun[0].ifunc = &skd_isr_aif;

	/* Allocate space for interrupt handles */
	skdev->hsize = ((uint32_t)(sizeof (ddi_intr_handle_t)) * count);
	skdev->htable = kmem_zalloc(skdev->hsize, KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */
	skdev->iflags |= IFLG_INTR_MSI;

	/* Allocate the interrupts */
	if ((ret = ddi_intr_alloc(skdev->dip, skdev->htable, msitype, 0, count,
	    &actual, 0)) != DDI_SUCCESS || actual < count) {
		cmn_err(CE_NOTE, "failed, intr_alloc ret=%xh, count = %xh, "
		    "actual=%xh", ret, count, actual);
		skd_release_intr(skdev);

		return (DDI_FAILURE);
	}

	skdev->intr_cnt = actual;

	/* Get interrupt priority */
	if ((ret = ddi_intr_get_pri(skdev->htable[0], &skdev->intr_pri)) !=
	    DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, get_pri ret=%xh", ret);
		skd_release_intr(skdev);

		return (ret);
	}

	/* Add the interrupt handler */
	if ((ret = ddi_intr_add_handler(skdev->htable[0], itrfun[0].ifunc,
	    (caddr_t)skdev, (caddr_t)0)) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, intr_add ret=%xh", ret);
		skd_release_intr(skdev);

		return (ret);
	}

	/* Setup mutexes */
	if ((ret = skd_init_mutex(skdev)) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, mutex init ret=%xh", ret);
		skd_release_intr(skdev);

		return (ret);
	}

	/* Get the capabilities */
	(void) ddi_intr_get_cap(skdev->htable[0], &skdev->intr_cap);

	/* Enable interrupts */
	if (skdev->intr_cap & DDI_INTR_FLAG_BLOCK) {
		if ((ret = ddi_intr_block_enable(skdev->htable,
		    skdev->intr_cnt)) != DDI_SUCCESS) {
			cmn_err(CE_NOTE, "failed, block enable, ret=%xh", ret);
			skd_destroy_mutex(skdev);
			skd_release_intr(skdev);

			return (ret);
		}
	} else {
		if ((ret = ddi_intr_enable(skdev->htable[0])) != DDI_SUCCESS) {
			cmn_err(CE_NOTE, "failed, intr enable, ret=%xh", ret);
			skd_destroy_mutex(skdev);
			skd_release_intr(skdev);

			return (ret);
		}
	}

	Dcmn_err(CE_CONT, "(%s%d): setup MSI done, flags=%x",
	    DRV_NAME, skdev->instance, skdev->flags);

	skdev->irq_type = SKD_IRQ_MSI;

	return (DDI_SUCCESS);
}

#define	ITRFSZ	13

/*
 *
 * Name:	skd_setup_msix, setup the MSIX interrupt handling capability.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	DDI_FAILURE on failure otherwise DDI_SUCCESS.
 *
 */
static int
skd_setup_msix(skd_device_t *skdev)
{
	int32_t		count = 0;
	int32_t		avail = 0;
	int32_t		actual = 0;
	int32_t		msitype = DDI_INTR_TYPE_MSIX;
	int32_t		ret;
	uint32_t	i;
	skd_ifunc_t	itrfun[ITRFSZ] = {0};

	Dcmn_err(CE_CONT, "(%s%d): setup_msix", DRV_NAME, skdev->instance);

	if (skd_disable_msix != 0) {
		cmn_err(CE_NOTE, "MSI-X is disabled by user");

		return (DDI_FAILURE);
	}

	/* Get number of MSI-X interrupts the platform h/w supports */
	if (((ret = ddi_intr_get_nintrs(skdev->dip, msitype, &count)) !=
	    DDI_SUCCESS) || count == 0) {
		cmn_err(CE_NOTE, "msix_setup failed, nintrs ret=%xh, cnt=%xh",
		    ret, count);

		return (DDI_FAILURE);
	}

	if (count < SKD_MIN_MSIX_COUNT) {
		cmn_err(CE_NOTE, "msix_setup failed, min h/w vectors "
		    "req'd: %d, avail: %d",
		    SKD_MSIX_MAXAIF, count);

		return (DDI_FAILURE);
	}

	/* Get number of available system interrupts */
	if (((ret = ddi_intr_get_navail(skdev->dip, msitype, &avail)) !=
	    DDI_SUCCESS) || avail == 0) {
		cmn_err(CE_NOTE, "msix_setup failed, navail ret=%xh, "
		    "avail=%xh", ret, avail);

		return (DDI_FAILURE);
	}

	/* Fill out the intr table */
	itrfun[SKD_MSIX_AIF].ifunc  = &skd_isr_aif;
	itrfun[SKD_MSIX_RSPQ].ifunc = &skd_isr_aif;

	if (avail > ITRFSZ)
		avail = ITRFSZ;

	for (i = 0; i < avail; i++) {
		itrfun[i].ifunc = &skd_isr_aif;
		if (i >= ITRFSZ)
			break;
	}

	/* Allocate space for interrupt handles */
	skdev->hsize = ((uint32_t)(sizeof (ddi_intr_handle_t)) *
	    SKD_MAX_MSIX_COUNT);
	skdev->htable = kmem_zalloc(skdev->hsize, KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */
	skdev->iflags |= IFLG_INTR_MSIX;

	/* Allocate the interrupts */
	if (((ret = ddi_intr_alloc(skdev->dip, skdev->htable, msitype,
	    0, count, &actual, 0)) != DDI_SUCCESS) ||
	    actual < SKD_MSIX_MAXAIF) {
		cmn_err(CE_NOTE, "msix_setup failed, intr_alloc ret=%xh, "
		    "count = %xh, " "actual=%xh", ret, count, actual);

		skd_release_intr(skdev);

		return (DDI_FAILURE);
	}

	skdev->intr_cnt = actual;

	/* Get interrupt priority */
	if ((ret = ddi_intr_get_pri(skdev->htable[0], &skdev->intr_pri)) !=
	    DDI_SUCCESS) {
		cmn_err(CE_NOTE, "msix_setup failed, get_pri ret=%xh", ret);
		skd_release_intr(skdev);

		return (ret);
	}

	/* Add the interrupt handlers */
	for (i = 0; i < actual; i++) {
		if ((ret = ddi_intr_add_handler(skdev->htable[i],
		    itrfun[i].ifunc, (void *)skdev, (void *)((ulong_t)i))) !=
		    DDI_SUCCESS) {
			cmn_err(CE_NOTE, "msix_setup failed, addh#=%xh, "
			    "act=%xh, ret=%xh", i, actual, ret);
			skd_release_intr(skdev);

			return (ret);
		}
	}

	if ((ret = ddi_intr_enable(skdev->htable[0])) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "msix_setup: failed to enable interrupt");
		skd_release_intr(skdev);

		return (ret);
	}


	/*
	 * duplicate the rest of the intr's
	 * ddi_intr_dup_handler() isn't working on x86 just yet...
	 */
	for (i = actual; i < SKD_MIN_MSIX_COUNT; i++) {
		if ((ret = ddi_intr_dup_handler(skdev->htable[0], i,
		    &skdev->htable[i])) != DDI_SUCCESS) {
			cmn_err(CE_NOTE,
			    "msix_setup dup_handler: insufficient MSIX slots, "
			    "avail=%d, need=%d", actual, SKD_MIN_MSIX_COUNT);
			skd_release_intr(skdev);

			return (ret);
		}
	}

	/* Setup mutexes */
	if ((ret = skd_init_mutex(skdev)) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "msix_setup failed, mutex init ret=%xh", ret);
		skd_release_intr(skdev);

		return (ret);
	}

	/* Get the capabilities */
	(void) ddi_intr_get_cap(skdev->htable[0], &skdev->intr_cap);

	/* Enable interrupts */
	if (skdev->intr_cap & DDI_INTR_FLAG_BLOCK) {
		if ((ret = ddi_intr_block_enable(skdev->htable,
		    skdev->intr_cnt)) != DDI_SUCCESS) {
			cmn_err(CE_NOTE, "failed, msix_setup block enable, "
			    "ret=%xh", ret);
			skd_destroy_mutex(skdev);
			skd_release_intr(skdev);

			return (ret);
		}
	} else {
		for (i = 1; i < skdev->intr_cnt; i++) {
			if ((ret = ddi_intr_enable(skdev->htable[i])) !=
			    DDI_SUCCESS) {
				cmn_err(CE_NOTE, "msix_setup failed, "
				    "intr enable, ret=%xh", ret);
				skd_destroy_mutex(skdev);
				skd_release_intr(skdev);

				return (ret);
			}
		}
	}

	skdev->irq_type = SKD_IRQ_MSIX;

	return (DDI_SUCCESS);
}

/*
 *
 * Name:	skd_setup_fixed, setup the FIXED interrupt handling capability.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	DDI_FAILURE on failure otherwise DDI_SUCCESS.
 *
 */
static int
skd_setup_fixed(skd_device_t *skdev)
{
	int32_t		count = 0;
	int32_t		actual = 0;
	int32_t		ret;

	Dcmn_err(CE_CONT, "(%s%d): setup_fixed", DRV_NAME, skdev->instance);

	/* Get number of fixed interrupts the system supports */
	if (((ret = ddi_intr_get_nintrs(skdev->dip, DDI_INTR_TYPE_FIXED,
	    &count)) != DDI_SUCCESS) || count != 1) {
		cmn_err(CE_NOTE, "failed, nintrs ret=%xh, cnt=%xh", ret, count);

		return (DDI_FAILURE);
	}

	skdev->iflags |= IFLG_INTR_FIXED;

	/* Allocate space for interrupt handles */
	skdev->hsize = ((uint32_t)(sizeof (ddi_intr_handle_t)) * count);
	skdev->htable = kmem_zalloc(skdev->hsize, KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

	/* Allocate the interrupts */
	if (((ret = ddi_intr_alloc(skdev->dip, skdev->htable,
	    DDI_INTR_TYPE_FIXED, DDI_INTR_ALLOC_NORMAL, count, &actual, 0)) !=
	    DDI_SUCCESS) || actual < count) {
		cmn_err(CE_NOTE, "failed, intr_alloc ret=%xh, count=%xh, "
		    "actual=%xh", ret, count, actual);
		skd_release_intr(skdev);

		return (DDI_FAILURE);
	}

	skdev->intr_cnt = actual;

	/* Get interrupt priority */
	if ((ret = ddi_intr_get_pri(skdev->htable[0], &skdev->intr_pri)) !=
	    DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, get_pri ret=%xh", ret);
		skd_release_intr(skdev);

		return (ret);
	}

	if (skdev->intr_pri >= ddi_intr_get_hilevel_pri()) {
		cmn_err(CE_NOTE, "failed, Hi level interrupt not supported");
		skd_release_intr(skdev);

		return (ret);
	}

	(void) ddi_intr_get_cap(skdev->htable[0], &skdev->intr_cap);

	ret = ddi_intr_set_pri(skdev->htable[0], 10);

	/* Setup mutexes */
	if ((ret = skd_init_mutex(skdev)) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, mutex init ret=%xh", ret);
		skd_release_intr(skdev);

		return (ret);
	}

	/* Add the interrupt handler */
	if ((ret = ddi_intr_add_handler(skdev->htable[0], skd_isr_aif,
	    (void *)skdev, 0)) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, intr_add ret=%xh", ret);
		skd_release_intr(skdev);
		skd_destroy_mutex(skdev);

		return (ret);
	}

	skdev->iflags |= IFLG_INTR_FIXED;

	/* Enable interrupts */
	if ((ret = ddi_intr_enable(skdev->htable[0])) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, intr enable, ret=%xh", ret);
		skd_destroy_mutex(skdev);
		skd_release_intr(skdev);

		return (ret);
	}

	ret = ddi_intr_clr_mask(skdev->htable[0]);

	Dcmn_err(CE_NOTE, "%s: using FIXED interupts", skdev->name);

	skdev->irq_type = SKD_IRQ_LEGACY;

	return (DDI_SUCCESS);
}

/*
 *
 * Name:	skd_disable_intr, disable interrupt handling.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_disable_intr(skd_device_t *skdev)
{
	uint32_t	i, rval;

	if ((skdev->iflags & IFLG_INTR_AIF) == 0) {

		/* Disable legacy interrupts */
		(void) ddi_remove_intr(skdev->dip, 0, skdev->iblock_cookie);

	} else if ((skdev->intr_cap & DDI_INTR_FLAG_BLOCK) &&
	    (skdev->iflags & (IFLG_INTR_MSI | IFLG_INTR_MSIX))) {

		/* Remove AIF block interrupts (MSI) */
		if ((rval = ddi_intr_block_disable(skdev->htable,
		    skdev->intr_cnt)) != DDI_SUCCESS) {
			cmn_err(CE_NOTE, "failed intr block disable, rval=%x",
			    rval);
		}

	} else {

		/* Remove AIF non-block interrupts (fixed).  */
		for (i = 0; i < skdev->intr_cnt; i++) {
			if ((rval = ddi_intr_disable(skdev->htable[i])) !=
			    DDI_SUCCESS) {
				cmn_err(CE_NOTE, "failed intr disable, "
				    "intr#=%xh, " "rval=%xh", i, rval);
			}
		}
	}
}

/*
 *
 * Name:	skd_release_intr, disables interrupt handling.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_release_intr(skd_device_t *skdev)
{
	int32_t 	i;
	int		rval;


	Dcmn_err(CE_CONT, "REL_INTR intr_cnt=%d", skdev->intr_cnt);

	if ((skdev->iflags & IFLG_INTR_AIF) == 0) {
		Dcmn_err(CE_CONT, "release_intr: (%s%d): done",
		    DRV_NAME, skdev->instance);
		return;
	}

	skdev->iflags &= ~(IFLG_INTR_AIF);
	if (skdev->htable != NULL && skdev->hsize > 0) {
		i = (int32_t)skdev->hsize / (int32_t)sizeof (ddi_intr_handle_t);

		while (i-- > 0) {
			if (skdev->htable[i] == 0) {
				Dcmn_err(CE_NOTE, "htable[%x]=0h", i);
				continue;
			}

			if ((rval = ddi_intr_disable(skdev->htable[i])) !=
			    DDI_SUCCESS)
				Dcmn_err(CE_NOTE, "release_intr: intr_disable "
				    "htable[%d], rval=%d", i, rval);

			if (i < skdev->intr_cnt) {
				if ((rval = ddi_intr_remove_handler(
				    skdev->htable[i])) != DDI_SUCCESS)
					cmn_err(CE_NOTE, "release_intr: "
					    "intr_remove_handler FAILED, "
					    "rval=%d", rval);

				Dcmn_err(CE_NOTE, "release_intr: "
				    "remove_handler htable[%d]", i);
			}

			if ((rval = ddi_intr_free(skdev->htable[i])) !=
			    DDI_SUCCESS)
				cmn_err(CE_NOTE, "release_intr: intr_free "
				    "FAILED, rval=%d", rval);
			Dcmn_err(CE_NOTE, "release_intr: intr_free htable[%d]",
			    i);
		}

		kmem_free(skdev->htable, skdev->hsize);
		skdev->htable = NULL;
	}

	skdev->hsize    = 0;
	skdev->intr_cnt = 0;
	skdev->intr_pri = 0;
	skdev->intr_cap = 0;
}

/*
 *
 * Name:	skd_legacy_intr, disables interrupt handling.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static int
skd_legacy_intr(skd_device_t *skdev)
{
	int	rval = DDI_SUCCESS;

	Dcmn_err(CE_CONT, "(%s%d): legacy_intr\n", DRV_NAME, skdev->instance);

	/* Setup mutexes */
	if (skd_init_mutex(skdev) != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed, mutex init");

		return (DDI_FAILURE);
	}

	/* Setup standard/legacy interrupt handler */
	if (ddi_add_intr(skdev->dip, (uint_t)0, &skdev->iblock_cookie,
	    (ddi_idevice_cookie_t *)0, skd_isr, (caddr_t)skdev) !=
	    DDI_SUCCESS) {
		cmn_err(CE_WARN, "%s(%d): Failed to add legacy interrupt",
		    DRV_NAME, skdev->instance);
		skd_destroy_mutex(skdev);

		rval = DDI_FAILURE;
	}

	if (rval == DDI_SUCCESS) {
		skdev->iflags |= IFLG_INTR_LEGACY;

		cmn_err(CE_NOTE, "using legacy interrupts");
	}

	Dcmn_err(CE_CONT, "(%s%d): done", DRV_NAME, skdev->instance);

	return (rval);
}

/*
 *
 * Name:	skd_dealloc_resources, deallocate resources allocated
 *		during attach.
 *
 * Inputs:	dip		- DDI device info pointer.
 *		skdev		- device state structure.
 * 		seq		- bit flag representing allocated item.
 *		instance	- device instance.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_dealloc_resources(dev_info_t *dip, skd_device_t *skdev,
    uint32_t seq, int instance)
{

	if (skdev) {
		if (seq & SKD_DID_MINOR)  {
			/*
			 * Remove ALL minor nodes.
			 */
			ddi_remove_minor_node(dip, NULL);
			skdev->flags &= ~SKD_DID_MINOR;
		}

		if (seq & SKD_CONFIG_SPACE_SETUP)
			pci_config_teardown(&skdev->pci_handle);

		if (seq & SKD_REGS_MAPPED)
			ddi_regs_map_free(&skdev->iobase_handle);

		if (seq & SKD_IOMAP_IOBASE_MAPPED)
			ddi_regs_map_free(&skdev->iomap_handle);

		if (seq & SKD_DEV_IOBASE_MAPPED)
			ddi_regs_map_free(&skdev->dev_handle);

		if (seq & SKD_SOFT_STATE_ALLOCED)  {
			if (skdev->pathname &&
			    (skdev->flags & SKD_PATHNAME_ALLOCED)) {
				kmem_free(skdev->pathname,
				    strlen(skdev->pathname)+1);
			}
		}

		if (skdev->s1120_devid)
			ddi_devid_free(skdev->s1120_devid);
	}
}

/*
 *
 * Name:	skd_setup_interrupt, sets up the appropriate interrupt type
 *		msi, msix, or fixed.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	DDI_FAILURE on failure otherwise DDI_SUCCESS.
 *
 */
static int
skd_setup_interrupts(skd_device_t *skdev)
{
	int32_t		rval = DDI_FAILURE;
	int32_t		i;
	int32_t		itypes = 0;

	if (skd_os_release_level < 10) {
		cmn_err(CE_NOTE, "interrupt framework is not supported or is "
		    "disabled, using legacy\n");
		return (skd_legacy_intr(skdev));
	} else if (skd_os_release_level == 10) {
		/*
		 * See if the advanced interrupt functions (aif) are
		 * in the kernel
		 */
		void	*fptr = (void *)&ddi_intr_get_supported_types;

		if (fptr == NULL) {
			cmn_err(CE_NOTE, "aif is not supported, using legacy "
			    "interrupts (rev)\n");
			return (skd_legacy_intr(skdev));
		}
	}

	/*
	 * See what types of interrupts this adapter and platform support
	 * 0x1 - fixed
	 * 0x2 - MSI
	 * 0x4 - MSIX
	 */
	if ((i = ddi_intr_get_supported_types(skdev->dip, &itypes)) !=
	    DDI_SUCCESS) {
		cmn_err(CE_NOTE, "intr supported types failed, rval=%xh, "
		    "assuming FIXED interrupt", i);
		itypes = DDI_INTR_TYPE_FIXED;
	}

	Dcmn_err(CE_NOTE, "%s:supported interrupts types: %x",
	    skdev->name, itypes);

	switch (skdev->irq_type) {
	case SKD_IRQ_MSI:
		if ((itypes & DDI_INTR_TYPE_MSI) &&
		    (rval = skd_setup_msi(skdev)) == DDI_SUCCESS) {
			cmn_err(CE_NOTE, "%s: successful MSI setup",
			    skdev->name);

		} else if ((itypes & DDI_INTR_TYPE_MSIX) &&
		    (rval = skd_setup_msix(skdev)) == DDI_SUCCESS) {
			cmn_err(CE_NOTE, "%s: successful MSI-X setup",
			    skdev->name);
		} else {
			rval = skd_setup_fixed(skdev);
			if (rval == DDI_SUCCESS)
				cmn_err(CE_NOTE, "%s: "
				    "successful fixed intr setup", skdev->name);
		}

		break;
	case SKD_IRQ_LEGACY:
		if ((itypes & DDI_INTR_TYPE_FIXED) &&
		    (rval = skd_setup_fixed(skdev)) == DDI_SUCCESS) {
			cmn_err(CE_NOTE,
			    "%s: successful fixed intr setup", skdev->name);
		} else if ((itypes & DDI_INTR_TYPE_MSI) &&
		    (rval = skd_setup_msi(skdev)) == DDI_SUCCESS) {
			cmn_err(CE_NOTE,
			    "%s: successful MSI setup", skdev->name);
		} else if ((itypes & DDI_INTR_TYPE_MSIX) &&
		    (rval = skd_setup_msix(skdev)) == DDI_SUCCESS) {
			cmn_err(CE_NOTE,
			    "%s: successful MSI-X setup", skdev->name);
		}

		break;
	case SKD_IRQ_MSIX:
	default:
		if ((itypes & DDI_INTR_TYPE_MSIX) &&
		    (rval = skd_setup_msix(skdev)) == DDI_SUCCESS) {
			Dcmn_err(CE_NOTE, "%s: successful MSIX setup",
			    skdev->name);
		} else if ((itypes & DDI_INTR_TYPE_MSI) &&
		    (rval = skd_setup_msi(skdev)) == DDI_SUCCESS) {
			Dcmn_err(CE_NOTE, "%s: successful MSI setup",
			    skdev->name);
		} else {
			rval = skd_setup_fixed(skdev);
			if (rval == DDI_SUCCESS)
				Dcmn_err(CE_NOTE, "%s: "
				    "successful fixed intr setup", skdev->name);
		}

		break;
	}

	if (rval != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "failed setup interrupts, rval=%xh", rval);
	} else {
		Dcmn_err(CE_CONT, "%s: setup interrupts done", skdev->name);
	}

	return (rval);
}

/*
 *
 * Name:	skd_get_properties, retrieves properties from skd.conf.
 *
 * Inputs:	skdev		- device state structure.
 *		dip		- dev_info data structure.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
void
skd_get_properties(dev_info_t *dip, skd_device_t *skdev)
{
	int	prop_value;

	prop_value =  ddi_prop_get_int(DDI_DEV_T_ANY, dip, 0,
	    "intr-type-cap", -1);
	if (prop_value >= SKD_IRQ_LEGACY && prop_value <= SKD_IRQ_MSIX)
		skd_isr_type = prop_value;

	prop_value =  ddi_prop_get_int(DDI_DEV_T_ANY, dip, 0,
	    "max-scsi-reqs", -1);
	if (prop_value >= 1 && prop_value <= SKD_MAX_QUEUE_DEPTH)
		skd_max_queue_depth = prop_value;

	prop_value =  ddi_prop_get_int(DDI_DEV_T_ANY, dip, 0,
	    "max-scsi-reqs-per-msg", -1);
	if (prop_value >= 1 && prop_value <= SKD_MAX_REQ_PER_MSG)
		skd_max_req_per_msg = prop_value;

	prop_value =  ddi_prop_get_int(DDI_DEV_T_ANY, dip, 0,
	    "max-sgs-per-req", -1);
	if (prop_value >= 1 && prop_value <= SKD_MAX_N_SG_PER_REQ)
		skd_sgs_per_request = prop_value;

	prop_value =  ddi_prop_get_int(DDI_DEV_T_ANY, dip, 0,
	    "dbg-level", -1);
	if (prop_value >= 1 && prop_value <= 2)
		skd_dbg_level = prop_value;
}

/*
 *
 * Name:	skd_wait_for_s1120, wait for device to finish
 *		its initialization.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	DDI_SUCCESS or DDI_FAILURE.
 *
 */
static int
skd_wait_for_s1120(skd_device_t *skdev)
{
	clock_t	cur_ticks, tmo;
	int 	secs = 1, loop_cntr = 0;
	int	rc = DDI_SUCCESS;

	mutex_enter(&skdev->skd_s1120wait_mutex);

	while (skdev->gendisk_on != 1) {
		cur_ticks = ddi_get_lbolt();
		tmo = cur_ticks + drv_usectohz(10000 * secs);
		if (cv_timedwait(&skdev->cv_s1120wait,
		    &skdev->skd_s1120wait_mutex, tmo) == -1) {
			/* Oops - timed out */

			if (loop_cntr++ > 120) {
				rc = DDI_FAILURE;

				break;
			}
		}
	}

	mutex_exit(&skdev->skd_s1120wait_mutex);

	if (skdev->gendisk_on == 1)
		rc = DDI_SUCCESS;

	return (rc);
}

/*
 *
 * Name:	skd_create_minor_nodes, creates skd's special node
 *		its initialization.
 *
 * Inputs:	dip		- DDI device information pointer.
 *		skdev		- device state structure.
 *		instance	- instance number
 *
 * Returns:	DDI_SUCCESS or DDI_FAILURE.
 *
 */
int
skd_create_minor_nodes(dev_info_t *dip, skd_device_t *skdev, int instance)
{
	int  minor = 0;
	char devname[20];
	int  partnum, rv = DDI_SUCCESS;

	/* "solaris slice" minor nodes/slices */
	skdev->slice_offset = SLICE_OFFSET;

	partnum = 16;
	(void) sprintf(devname, "%c", (partnum-16) + 'q');

	skdev->skd_fulldisk_minor =
	    (minor | SKD_FULLDISK | partnum) | (instance << 6);

	/*
	 * Just create the *p0 node to permit ioctls otherwise blkdev
	 * will ignore them.
	 */
	(void) sprintf(devname, "%c,raw", (partnum-16) + 'q');
	if (ddi_create_minor_node(dip, devname, S_IFCHR,
	    skdev->skd_fulldisk_minor, DDI_NT_BLOCK, 0) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "could not make control device node %s",
		    devname);
	} else {
		Dcmn_err(CE_NOTE,
		    "%s: created control device node %s", skdev->name, devname);
	}

	return (rv);
}

/*
 *
 * Name:	skd_update_props, updates certain device properties.
 *
 * Inputs:	skdev		- device state structure.
 *		dip		- dev info structure
 *
 * Returns:	Nothing.
 *
 */
static void
skd_update_props(skd_device_t *skdev, dev_info_t *dip)
{
	int	blksize = 512;
	dev_t   dev = makedevice(skdev->major, skdev->skd_fulldisk_minor);


	if (ddi_prop_update_int64(dev, dip, "Nblocks",
	    lbtodb(skdev->Nblocks)) ||
	    ddi_prop_update_int64(dev, dip, "Size", skdev->Nblocks)) {
		cmn_err(CE_NOTE, "%s: update_props p0 FAILED", skdev->name);
	}

	dev = makedevice(skdev->major, skdev->skd_fulldisk_minor + 1);
	if (ddi_prop_update_int64(dev, dip, "Nblocks",
	    lbtodb(skdev->Nblocks)) ||
	    ddi_prop_update_int64(dev, dip, "Size", skdev->Nblocks)) {
		cmn_err(CE_NOTE, "%s: update_props p1 FAILED", skdev->name);
	}

	if ((ddi_prop_update_int64(DDI_DEV_T_NONE, dip, "device-nblocks",
	    lbtodb(skdev->Nblocks)) != DDI_SUCCESS) ||
	    (ddi_prop_update_int(DDI_DEV_T_NONE,   dip, "device-blksize",
	    blksize) != DDI_SUCCESS)) {
		cmn_err(CE_NOTE, "%s: FAILED to create driver properties",
		    skdev->name);
	}
}

/*
 *
 * Name:	skd_setup_devid, sets up device ID info.
 *
 * Inputs:	skdev		- device state structure.
 *		devid		- Device ID for the DDI.
 *
 * Returns:	DDI_SUCCESS or DDI_FAILURE.
 *
 */
static int
skd_setup_devid(skd_device_t *skdev, ddi_devid_t *devid)
{
	int  rc, sz_model, sz_sn, sz;

	sz_model = strlen((char *)skdev->inq_product_id);
	sz_sn = strlen((char *)skdev->inq_serial_num);
	sz = sz_model + sz_sn + 1;

	(void) sprintf(skdev->devid_str, "%s=%s", skdev->inq_product_id,
	    skdev->inq_serial_num);
	rc = ddi_devid_init(skdev->dip, DEVID_SCSI_SERIAL, sz,
	    skdev->devid_str, devid);

	if (rc != DDI_SUCCESS)
		cmn_err(CE_NOTE, "%s: devid_init FAILED", skdev->name);

	return (rc);

}

struct sbd_handle {
	sbd_ops_t	h_ops;
	ddi_dma_attr_t	*h_dma;
	dev_info_t	*h_parent;
	dev_info_t	*h_child;
	void		*h_private;
	char		*h_bd;
	char		*h_name;
	char		h_addr[20];	/* enough for %X,%X */
};

#ifdef SOLARIS11
#define	sbd_handle bd_handle
#endif

/*
 *
 * Name:	skd_bd_attach, attach to sblkdev driver
 *
 * Inputs:	skdev		- device state structure.
 *        	dip		- device info structure.
 *
 * Returns:	DDI_SUCCESS or DDI_FAILURE.
 *
 */
static int
skd_bd_attach(dev_info_t *dip, skd_device_t *skdev)
{
	int		rv;
	skd_host_t	*h;

	(void) skd_host_alloc(dip, skdev, 1, &skd_bd_ops,
	    &skd_64bit_io_dma_attr);

	h = skdev->s_hostp;

#ifdef USE_BLKDEV
	skdev->s_bdh = bd_alloc_handle(skdev, &skd_bd_ops, h->h_dma, KM_SLEEP);
#else
	skdev->s_bdh = sbd_alloc_handle(skdev, &skd_bd_ops, h->h_dma, KM_SLEEP);
#endif

	if (skdev->s_bdh == NULL) {
		cmn_err(CE_NOTE, "skd_bd_attach: FAILED");

		return (DDI_FAILURE);
	}

#ifdef USE_BLKDEV
	rv = bd_attach_handle(h->h_dip, skdev->s_bdh);
#else
	rv = sbd_attach_handle(h->h_dip, skdev->s_bdh);
#endif

	if (rv != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "sbd_attach_handle FAILED\n");
	} else {
		Dcmn_err(CE_NOTE, "sbd_attach_handle OK\n");
		skdev->bd_attached++;
	}

	return (rv);
}

/*
 *
 * Name:	skd_slot_detach, detach from the sblkdev driver.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
void
skd_slot_detach(skd_device_t *skdev)
{
	skd_host_dealloc(skdev);

	if (skdev->bd_attached)
#ifdef USE_BLKDEV
		(void) bd_detach_handle(skdev->s_bdh);
#else
	(void) sbd_detach_handle(skdev->s_bdh);
#endif

#ifdef USE_BLKDEV
	bd_free_handle(skdev->s_bdh);
#else
	sbd_free_handle(skdev->s_bdh);
#endif
}

/*
 *
 * Name:	skd_attach, attach sdk device driver
 *
 * Inputs:	dip		- device info structure.
 *		cmd		- DDI attach argument (ATTACH, RESUME, etc.)
 *
 * Returns:	DDI_SUCCESS or DDI_FAILURE.
 *
 */
static int
skd_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	int			instance;
	int			nregs;
	skd_device_t   		*skdev = NULL;
	uint32_t		regnum;
	uint16_t		inx;
	uint16_t 		cmd_reg;
	int			progress = 0;
	char			name[MAXPATHLEN];
	int			rv1 = DDI_FAILURE;
	off_t			regsize;
	char 			pci_str[32];
	char 			fw_version[8];
	char			intr_type[5], *pit = intr_type;

	instance = ddi_get_instance(dip);

	(void) ddi_get_parent_data(dip);

	switch (cmd) {
	case DDI_ATTACH:
		Dcmn_err(CE_NOTE, "sTec S1120 Driver v%s "
		    "Instance: %d", VERSIONSTR, instance);

		/*
		 * Check that hardware is installed in a DMA-capable slot
		 */
		if (ddi_slaveonly(dip) == DDI_SUCCESS) {
			cmn_err(CE_WARN, "s1120%d: installed in a "
			    "slot that isn't DMA-capable slot", instance);
			break;
		}

		/*
		 * No support for high-level interrupts
		 */
		if (ddi_intr_hilevel(dip, 0) != 0) {
			cmn_err(CE_WARN, "%s%d: High level interrupt "
			    " not supported", DRV_NAME, instance);
			break;
		}

		/*
		 * Allocate our per-device-instance structure
		 */
		if (ddi_soft_state_zalloc(skd_state, instance) !=
		    DDI_SUCCESS) {
			cmn_err(CE_WARN, "%s%d: soft state zalloc failed ",
			    DRV_NAME, instance);
			break;
		}

		progress |= SKD_SOFT_STATE_ALLOCED;

		skdev = (skd_device_t *)ddi_get_soft_state(skd_state, instance);
		if (skdev == NULL) {
			cmn_err(CE_WARN, "%s%d: Unable to get soft"
			    " state structure", DRV_NAME, instance);
			goto skd_attach_failed;
		}

		(void) sprintf(skdev->name, DRV_NAME "%d", instance);

		skdev->pdev	   = &skdev->dev;
		skdev->pdev->skdev = skdev;
		skdev->base_addr   = skdev;
		skdev->dip	   = dip;
		skdev->instance	   = instance;

		ddi_set_driver_private(dip, skdev);

		(void) ddi_pathname(dip, name);
		for (inx = (uint16_t)strlen(name); inx; inx--) {
			if (name[inx] == ',') {
				name[inx] = '\0';
				break;
			}
			if (name[inx] == '@') {
				break;
			}
		}

		skdev->pathname = (char *)kmem_zalloc(strlen(name) + 1,
		    KM_SLEEP);
		/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */
		(void) strcpy(skdev->pathname, name);

		progress	|= SKD_PATHNAME_ALLOCED;
		skdev->flags	|= SKD_PATHNAME_ALLOCED;

#if	defined(SOLARIS11) || defined(SOLARIS110)
		if (skd_create_minor_nodes(dip, skdev, instance)
		    != DDI_SUCCESS) {
			goto skd_attach_failed;
		}

		progress	|= SKD_DID_MINOR;
		skdev->flags	|= SKD_DID_MINOR;
#endif

		if (pci_config_setup(dip, &skdev->pci_handle) != DDI_SUCCESS) {

			cmn_err(CE_NOTE, "%s%d: pci_config_setup FAILED",
			    DRV_NAME, instance);
			goto skd_attach_failed;
		}

		progress |= SKD_CONFIG_SPACE_SETUP;

		/* Save adapter path. */

		(void) ddi_dev_nregs(dip, &nregs);

		/*
		 *	0x0   Configuration Space
		 *	0x1   I/O Space
		 *	0x2   s1120 register space
		 */
		regnum = 1;
		if (ddi_dev_regsize(dip, regnum, &regsize) != DDI_SUCCESS ||
		    ddi_regs_map_setup(dip, regnum, &skdev->iobase,
		    0, regsize, &dev_acc_attr, &skdev->iobase_handle) !=
		    DDI_SUCCESS) {
			cmn_err(CE_WARN, "%s(%d): regs_map_setup(mem) failed",
			    DRV_NAME, instance);
			goto skd_attach_failed;
		}
		progress |= SKD_REGS_MAPPED;

		if (regnum == 1) {
			skdev->iomap_iobase = skdev->iobase;
			skdev->iomap_handle = skdev->iobase_handle;
		} else {
			if (ddi_dev_regsize(dip, 1, &regsize) != DDI_SUCCESS ||
			    ddi_regs_map_setup(dip, 1,
			    &skdev->iomap_iobase, 0, regsize,
			    &dev_acc_attr, &skdev->iomap_handle) !=
			    DDI_SUCCESS) {
				cmn_err(CE_WARN, "%s(%d): regs_map_"
				    "setup(I/O) failed", DRV_NAME,
				    instance);
				goto skd_attach_failed;
			}
			progress |= SKD_IOMAP_IOBASE_MAPPED;
		}

		Dcmn_err(CE_NOTE, "%s: PCI iobase=%ph, iomap=%ph, regnum=%d, "
		    "regsize=%ld", skdev->name, (void *)skdev->iobase,
		    (void *)skdev->iomap_iobase, regnum, regsize);

		regnum = 2;
		if (ddi_dev_regsize(dip, regnum, &regsize) != DDI_SUCCESS ||
		    ddi_regs_map_setup(dip, regnum, &skdev->dev_iobase,
		    0, regsize, &dev_acc_attr, &skdev->dev_handle) !=
		    DDI_SUCCESS) {
			cmn_err(CE_WARN, "%s(%s): regs_map_setup(mem) failed",
			    DRV_NAME, skdev->name);

			goto skd_attach_failed;
		}

		skdev->dev_memsize = (int)regsize;

		Dcmn_err(CE_NOTE, "%s: DEV iobase=%ph regsize=%d",
		    skdev->name, (void *)skdev->dev_iobase,
		    skdev->dev_memsize);

		progress |= SKD_DEV_IOBASE_MAPPED;

		cmd_reg = skd_pci_get16(skdev, PCI_CONF_COMM);
		cmd_reg |= (PCI_COMM_ME | PCI_COMM_INTX_DISABLE);
		cmd_reg &= ~PCI_COMM_PARITY_DETECT;
		skd_pci_put16(skdev, PCI_CONF_COMM, cmd_reg);

		/* Get adapter PCI device information. */
		skdev->vendor_id = (uint16_t)skd_pci_get16(skdev,
		    PCI_CONF_VENID);
		skdev->device_id = (uint16_t)skd_pci_get16(skdev,
		    PCI_CONF_DEVID);
		skdev->sub_vendor_id = (uint16_t)skd_pci_get16(skdev,
		    PCI_CONF_SUBVENID);
		skdev->sub_device_id = (uint16_t)skd_pci_get16(skdev,
		    PCI_CONF_SUBSYSID);
		skdev->device_ipin = (uint8_t)skd_pci_get8(skdev,
		    PCI_CONF_IPIN);

		Dcmn_err(CE_NOTE, "%s: %x-%x card detected",
		    skdev->name, skdev->vendor_id, skdev->device_id);

		skd_get_properties(dip, skdev);

		(void) skd_init(skdev);

		if (skd_construct(skdev, instance)) {
			cmn_err(CE_NOTE, "%s: construct FAILED", skdev->name);
			goto skd_attach_failed;
		}

		progress |= SKD_PROBED;
		progress |= SKD_CONSTRUCTED;

		/*
		 * Setup interrupt handler
		 */
		if ((rv1 = skd_setup_interrupts(skdev)) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "%s: Unable to add interrupt",
			    skdev->name);
			goto skd_attach_failed;
		}

		progress |= SKD_INTR_ADDED;

		ADAPTER_STATE_LOCK(skdev);
		skdev->flags |= SKD_ATTACHED;
		ADAPTER_STATE_UNLOCK(skdev);

		skdev->d_blkshift = 9;
		progress |= SKD_ATTACHED;


		/* Enable timer */
		skd_start_timer(skdev);

		skd_start_device(skdev);

		ADAPTER_STATE_LOCK(skdev);
		skdev->progress = progress;
		ADAPTER_STATE_UNLOCK(skdev);

		/*
		 * Give the board a chance to
		 * complete its initialization.
		 */
		rv1 = DDI_SUCCESS;

		while (skdev->gendisk_on != 1)
			(void) skd_wait_for_s1120(skdev);

		if (skdev->gendisk_on != 1) {
			cmn_err(CE_NOTE, "%s: s1120 failed to come ONLINE",
			    skdev->name);
			rv1 = DDI_FAILURE;
		} else {
			struct skd_special_context *skspcl =
			    &skdev->internal_skspcl;

			ddi_report_dev(dip);

			skd_send_internal_skspcl(skdev, skspcl,	INQUIRY);

			skdev->disks_initialized++;

			(void) strcpy(fw_version, "???");
			(void) skd_pci_info(skdev, pci_str);
			Dcmn_err(CE_NOTE, " sTec S1120 Driver(%s) "
			    "version %s-b%s",
			    DRV_NAME, DRV_VERSION, DRV_BUILD_ID);

			Dcmn_err(CE_NOTE, " sTec S1120 %04x:%04x %s 64 bit",
			    skdev->vendor_id, skdev->device_id, pci_str);

			Dcmn_err(CE_NOTE, " sTec S1120 %s\n", skdev->pathname);

			if (*skdev->inq_serial_num)
				Dcmn_err(CE_NOTE, " sTec S1120 serial#=%s",
				    skdev->inq_serial_num);

			if (*skdev->inq_product_id &&
			    *skdev->inq_product_rev)
				Dcmn_err(CE_NOTE, " sTec S1120 prod ID=%s "
				    "prod rev=%s",
				    skdev->inq_product_id,
				    skdev->inq_product_rev);

			switch (skdev->irq_type) {
			case SKD_IRQ_MSIX:
				pit = "msix"; break;
			case SKD_IRQ_MSI:
				pit = "msi";  break;
			case SKD_IRQ_LEGACY:
				pit = "fixed"; break;
			}

			Dcmn_err(CE_NOTE, "%s: intr-type-cap:        %s",
			    skdev->name, pit);
			Dcmn_err(CE_NOTE, "%s: max-scsi-reqs:        %d",
			    skdev->name, skd_max_queue_depth);
			Dcmn_err(CE_NOTE, "%s: max-sgs-per-req:      %d",
			    skdev->name, skd_sgs_per_request);
			Dcmn_err(CE_NOTE, "%s: max-scsi-req-per-msg: %d",
			    skdev->name, skd_max_req_per_msg);

			if (skd_bd_attach(dip, skdev) == DDI_FAILURE)
				goto skd_attach_failed;

			skd_update_props(skdev, dip);

		}

		ADAPTER_STATE_LOCK(skdev);
		skdev->progress = progress;
		ADAPTER_STATE_UNLOCK(skdev);
		break;

	skd_attach_failed:
		skd_dealloc_resources(dip, skdev, progress, instance);

		cmn_err(CE_NOTE, "skd_attach FAILED: progress=%x", progress);

		rv1 = DDI_FAILURE;

		break;
	case DDI_RESUME:
		rv1 = DDI_SUCCESS;
		/* Re-enable timer */
		skd_start_timer(skdev);
		break;

	default:
		cmn_err(CE_WARN, "%s%d: unsupported device\n",
		    DRV_NAME, instance);
		goto skd_attach_failed;

	}

	if (rv1 != DDI_SUCCESS) {
		switch (cmd) {
		case DDI_ATTACH:
		cmn_err(CE_WARN, "skd%d: Unable to attach (%x)",
		    instance, rv1);
		break;
		case DDI_RESUME:
		cmn_err(CE_WARN, "skd%d: Unable to resume (%x)",
		    instance, rv1);
		break;
		default:
		cmn_err(CE_WARN, "skd%d: attach, unknown code: %x",
		    instance, cmd);
		break;
		}
	} else
		Dcmn_err(CE_NOTE, "skd_attach: exiting");

	if (rv1 != DDI_SUCCESS) {
		rv1 = DDI_FAILURE;
	} else
		skdev->attached = 1;

	return (rv1);
}

/*
 *
 * Name:	skd_halt
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_halt(skd_device_t *skdev)
{
	Dcmn_err(CE_NOTE, "%s: halt/suspend ......", skdev->name);
}

/*
 *
 * Name:	skd_detach, detaches driver from the system.
 *
 * Inputs:	dip		- device info structure.
 *
 * Returns:	DDI_SUCCESS on successful detach otherwise DDI_FAILURE.
 *
 */
static int
skd_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	struct buf	*bp;
	skd_device_t   	*skdev;
	int		instance, drive = 0;
	timeout_id_t	timer_id = NULL;
	int		rv1 = DDI_SUCCESS;
	struct skd_special_context *skspcl;

	instance = ddi_get_instance(dip);

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state, instance);
	if (skdev == NULL) {
		cmn_err(CE_NOTE, "detach failed: NULL skd state");

		return (DDI_FAILURE);
	}

	Dcmn_err(CE_CONT, "skd_detach(%d): entered", instance);

	switch (cmd) {
	case DDI_DETACH:
		/* Test for packet cache inuse. */
		ADAPTER_STATE_LOCK(skdev);

		/* Stop command/event processing. */
		skdev->flags |= (SKD_SUSPENDED | SKD_CMD_ABORT_TMO);

		/* Disable driver timer if no adapters. */
		if (skdev->skd_timer_timeout_id != 0) {
			timer_id = skdev->skd_timer_timeout_id;
			skdev->skd_timer_timeout_id = 0;
		}
		ADAPTER_STATE_UNLOCK(skdev);

		if (timer_id != 0) {
			(void) untimeout(timer_id);
		}

#ifdef	SKD_PM
		if (skdev->power_level != LOW_POWER_LEVEL) {
			skd_halt(skdev);
			skdev->power_level = LOW_POWER_LEVEL;
		}
#endif
		skspcl = &skdev->internal_skspcl;
		skd_send_internal_skspcl(skdev, skspcl, SYNCHRONIZE_CACHE);

		skd_stop_device(skdev);

		/*
		 * Clear request queue.
		 */
		do {
			bp = skd_get_queued_bp(skdev, drive);
			if (bp) {
				skd_end_request_abnormal(skdev, bp, ECANCELED,
				    SKD_IODONE_WNIOC);
				cmn_err(CE_NOTE,
				    "detach: cancelled bp %p %ld <%s> %lld\n",
				    (void *)bp, bp->b_bcount,
				    (bp->b_flags & B_READ) ? "Read" : "Write",
				    bp->b_lblkno);
			}
		} while (bp);

		(void) skd_cancel_pending_requests(skdev);

		skd_slot_detach(skdev);

		skd_destruct(skdev);

		skd_disable_intr(skdev);

		skd_release_intr(skdev);

		skd_dealloc_resources(dip, skdev, skdev->progress, instance);

		if ((skdev->flags & SKD_MUTEX_DESTROYED) == 0) {
			skd_destroy_mutex(skdev);
		}

		ddi_soft_state_free(skd_state, instance);

		skd_exit();

		break;

	case DDI_SUSPEND:
		/* Block timer. */

		ADAPTER_STATE_LOCK(skdev);
		skdev->flags |= SKD_SUSPENDED;

		/* Disable driver timer if last adapter. */
		if (skdev->skd_timer_timeout_id != 0) {
			timer_id = skdev->skd_timer_timeout_id;
			skdev->skd_timer_timeout_id = NULL;
		}
		ADAPTER_STATE_UNLOCK(skdev);

		if (timer_id != NULL) {
			(void) untimeout(timer_id);
		}

		ddi_prop_remove_all(dip);

		skd_halt(skdev);

		break;
	default:
		rv1 = DDI_FAILURE;
		break;
	}

	if (rv1 != DDI_SUCCESS) {
		cmn_err(CE_NOTE, "skd_detach, failed, rv1=%x", rv1);
	} else {
		Dcmn_err(CE_CONT, "skd_detach: exiting");
	}

	if (rv1 != DDI_SUCCESS)
		return (DDI_FAILURE);

	return (rv1);
}

/*
 *
 * Name:	skd_getinfo, retreives device info.
 *
 * Inputs:	dip		- DDI device info, used to get "skdev".
 * 		cmd		- detach type.
 *		arg		- dev_t info.
 *		resultp		- pointer to store dip or instance.
 *
 * Returns:	DDI_SUCCESS or DDI_FAILURE.
 *
 */
static int
skd_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **resultp)
{
	skd_device_t	*skdev;
	int		minor;
	int 		instance;
	int		rval = DDI_FAILURE;

	_NOTE(ARGUNUSED(dip));

	minor    = (int)(getminor((dev_t)arg));
	instance = GETINSTANCE(minor);
	skdev = (skd_device_t *)ddi_get_soft_state(skd_state, instance);

	*resultp = NULL;
	if (skdev == NULL) {
		cmn_err(CE_CONT, "failed, unknown minor=%d",
		    getminor((dev_t)arg));
		*resultp = NULL;

		return (rval);
	}

	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*resultp = (void *)skdev->dip;
		rval = DDI_SUCCESS;
	break;
	case DDI_INFO_DEVT2INSTANCE:
		*resultp = (void *)(uintptr_t)(skdev->instance);
		rval = DDI_SUCCESS;
		break;
	default:
		cmn_err(CE_NOTE, "failed, unsupported cmd=%d (getinfo)", cmd);
		rval = DDI_FAILURE;
		break;
	}

	return (rval);
}

/*
 *
 * Name:	skd_prop_op, called by system to retrieve certain
 *		device properties.
 *
 * Inputs:	dev		device addressing info.
 *		dip		dev_info data structure.
 *		prop_op		property operation type.
 *		prop_flags	property flags
 *		prop_name	property name.
 *		prop_val	property value.
 *		szp		pointer where length is stored.
 *
 * Returns:	Nothing.
 *
 */
static int
skd_prop_op(dev_t dev, dev_info_t *dip, ddi_prop_op_t prop_op, int prop_flags,
    char *prop_name, caddr_t prop_val, int *szp)
{
	skd_device_t	*skdev;
	uint64_t 	nblocks;
	caddr_t		baddr = NULL;
	void		*vp;
	long long 	bcnt = 0;
	int 		rsz = 4;

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state,
	    ddi_get_instance(dip));
	if (skdev == NULL) {
		return (ddi_prop_op(dev, dip, prop_op, prop_flags, prop_name,
		    prop_val, szp));
	}

	if (dev == DDI_DEV_T_ANY) {
		Dcmn_err(CE_NOTE, " skd_prop_op: DDI_DEV_T_ANY - ignoring");

		return (ddi_prop_op(dev, dip, prop_op, prop_flags, prop_name,
		    prop_val, szp));
	}


	if ((strcmp(prop_name, "nblocks") != 0) &&
	    (strcmp(prop_name, "Nblocks") != 0) &&
	    (strcmp(prop_name, "blksize") != 0) &&
	    (strcmp(prop_name, "size") != 0) &&
	    (strcmp(prop_name, "Size") != 0)) {
		Dcmn_err(CE_NOTE, "skd_prop_op: (%s) - not recognized",
		    prop_name);

		return (ddi_prop_op(dev, dip, prop_op, prop_flags,
		    prop_name, prop_val, szp));
	}

	if (prop_op == PROP_EXISTS)
		return (DDI_PROP_SUCCESS);

	nblocks = skdev->Nblocks;

	if (strcmp(prop_name, "nblocks") == 0) {
		rsz = 4;
	} else
		if (strcmp(prop_name, "Nblocks") == 0) {
			bcnt = nblocks;
			rsz = 8;
		} else
			if (strcmp(prop_name, "blksize") == 0) {
				nblocks = 512;
				rsz = 4;
			} else
				if (strcmp(prop_name, "size") == 0) {
					nblocks = (nblocks << DEV_BSHIFT);
					rsz = 4;
				} else
					if (strcmp(prop_name, "Size") == 0) {
						bcnt = (nblocks << DEV_BSHIFT);
						rsz = 8;
					}

	if (prop_op == PROP_LEN) {
		*szp = rsz;

		return (DDI_PROP_SUCCESS);
	}

	if (prop_op == PROP_LEN_AND_VAL_ALLOC) {
		baddr = (caddr_t)kmem_alloc((size_t)rsz,
		    ((prop_flags & DDI_PROP_CANSLEEP) ? KM_SLEEP : KM_NOSLEEP));
		if (baddr == NULL)  {
			Dcmn_err(CE_WARN, "skd_prop_op: no memory");

			return (DDI_PROP_NO_MEMORY);
		}

		/* LINTED */
		*(caddr_t *)prop_val = baddr;

		*szp = rsz;
	} else {
		/* else */
		if (prop_val == NULL) {
			Dcmn_err(CE_NOTE, "skd_prop_op: NULL mem ptr");

			return (DDI_PROP_NO_MEMORY);
		}

		baddr = prop_val;
	}

	if (rsz > (*szp)) {
		Dcmn_err(CE_NOTE, "skd_prop_op: mem ptr - too small");

		return (DDI_PROP_BUF_TOO_SMALL);
	}

	vp = (rsz == 8) ? (void *)&bcnt : (void *)&nblocks;
	bcopy(vp, baddr, rsz);

	return (DDI_PROP_SUCCESS);
}

/*
 *
 * Name:	skd_devid_init, calls skd_setup_devid to setup
 *		the device's devid structure.
 *
 * Inputs:	arg		- device state structure.
 *		dip		- dev_info structure.
 *		devid		- devid structure.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static int
skd_devid_init(void *arg, dev_info_t *dip, ddi_devid_t *devid)
{
	skd_device_t	*skdev = arg;

	(void) skd_setup_devid(skdev, devid);

	return (0);
}


#ifdef SOLARIS11
/*
 *
 * Name:	skd_tg_getinfo, called to retrieve disk geometry,
 *		device capacity, and device blocksize.
 *
 * Inputs:	dip		- dev_info structure.
 *		cmd		- command request.
 *		arg		- data destination.
 *		cookie		- device state structure.
 *
 * Returns:	ENOTTY for geometry requests.
 *		EINVAL on unrecognized request.
 *		Zero on success.
 *
 */
static int
skd_tg_getinfo(dev_info_t *dip, int cmd, void *arg, void *tg_cookie)
{
	skd_device_t	*skdev;

	_NOTE(ARGUNUSED(dip));
	skdev = (skd_device_t *)tg_cookie;

	if (skd_dbg_blkdev)
		cmn_err(CE_NOTE, "skd_tg_getinfo: cmd=%x", cmd);

	switch (cmd) {
	case TG_GETPHYGEOM:
	case TG_GETVIRTGEOM:
		/*
		 * We don't have any "geometry" as such, let cmlb
		 * fabricate something.
		 */
		if (skd_dbg_blkdev)
			cmn_err(CE_NOTE, "tg_getinfo: GETPHYGEOM");

		return (ENOTTY);

	case TG_GETCAPACITY:
		*(diskaddr_t *)arg = skdev->Nblocks;
		if (skd_dbg_blkdev)
			cmn_err(CE_NOTE,
			    "tg_getinfo: TG_GETCAPACITY - %" PRIu64,
			    skdev->Nblocks);

		return (0);

	case TG_GETBLOCKSIZE: {
		int blksize;

		blksize = (1U << skdev->d_blkshift);
		*(uint32_t *)arg = blksize;
		if (skd_dbg_blkdev) {
			cmn_err(CE_NOTE,
			    "tg_getinfo: TG_GETBLOCKSIZE - %d", blksize);
		}

		return (0);
	}
	case TG_GETATTR:
		/*
		 * It turns out that cmlb really doesn't do much for
		 * non-writable media, but lets make the information
		 * available for it in case it does more in the
		 * future.  (The value is currently used for
		 * triggering special behavior for CD-ROMs.)
		 */
		((tg_attribute_t *)arg)->media_is_writable = B_TRUE;

		((tg_attribute_t *)arg)->media_is_solid_state = B_TRUE;

		if (skd_dbg_blkdev) {
			cmn_err(CE_NOTE, "tg_getinfo: "
			    "TG_GETATTR - media_is_writeable=1\n");

			cmn_err(CE_NOTE, "tg_getinfo: "
			    "TG_GETATTR - media_is_solid_state=1\n");
		}

		return (0);

	default:
		if (skd_dbg_blkdev)
			cmn_err(CE_NOTE, "tg_getinfo: inv request");
		return (EINVAL);
	}
}

/*
 *
 * Name:	skd_tg_rdwr, performs R/W operations for the
 *		sblkdev driver.
 *
 * Inputs:	dip		- dev_info sturcture.
 *		cmd		- command to execute.
 *		bufaddr		- data buffer.
 *		start		- disk block address.
 *		length		- size of data transfer.
 *		cookie		- device state structure.
 *
 * Returns:	Error number, or 0 upon success.
 *
 */
static int
skd_tg_rdwr(dev_info_t *dip, uchar_t cmd, void *bufaddr, diskaddr_t start,
    size_t length, void *tg_cookie)
{
	skd_device_t	*skdev;
	buf_t		*bp;
	int		rv;

	if (skd_dbg_blkdev)
		cmn_err(CE_NOTE,
		    "skd_tg_rdwr: %s %ld bytes from blk %lld",
		    cmd == TG_READ ? "Read" : "Write",
		    length, start);

	_NOTE(ARGUNUSED(dip));

	skdev = tg_cookie;
	if (P2PHASE(length, (1U << skdev->d_blkshift)) != 0) {
		/* We can only transfer whole blocks at a time! */
		return (EINVAL);
	}

	if (skdev->state != SKD_DRVR_STATE_ONLINE) {
		Dcmn_err(CE_NOTE, "Device - not ONLINE");

		return (EAGAIN);
	}

	bp = getrbuf(KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

	switch (cmd) {
	case TG_READ:
		bp->b_flags = B_READ;
		break;
	case TG_WRITE:
		bp->b_flags = B_WRITE;
		break;
	default:
		freerbuf(bp);
		return (EINVAL);
	}

	bp->b_un.b_addr = bufaddr;
	bp->b_bcount	= length;
	bp->b_lblkno	= start;
	bp->b_edev	= P0_RAW_DISK | SKD_FULLDISK;

	(void) skd_strategy(bp);

	(void) biowait(bp);

	rv = geterror(bp);

	freerbuf(bp);

	return (rv);
}
#else

/*
 *
 * Name:	skd_tg_rdwr, performs R/W operations for the
 *		sblkdev driver.
 *
 * Inputs:	dip		- dev_info sturcture.
 *		cmd		- command to execute.
 *		bufaddr		- data buffer.
 *		start		- disk block address.
 *		length		- size of data transfer.
 *
 * Returns:	Nothing.
 *
 */
static int
skd_tg_rdwr(dev_info_t *dip, uchar_t cmd, void *bufaddr,
    diskaddr_t start, size_t length)
{
	skd_device_t	*skdev;
	buf_t		*bp;
	int		rv;

	if (skd_dbg_blkdev)
		cmn_err(CE_NOTE, "skd_tg_rdwr: %s %d bytes from blk %lld",
		    cmd == TG_READ ? "Read" : "Write", length, start);

	_NOTE(ARGUNUSED(dip));

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state,
	    ddi_get_instance(dip));
	if (P2PHASE(length, (1U << skdev->d_blkshift)) != 0) {
		/* We can only transfer whole blocks at a time! */
		return (EINVAL);
	}

	bp = getrbuf(KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */

	if (skdev->state != SKD_DRVR_STATE_ONLINE) {
		Dcmn_err(CE_NOTE, "Device - not ONLINE");

		return (EAGAIN);
	}

	switch (cmd) {
	case TG_READ:
		bp->b_flags = B_READ;
		break;
	case TG_WRITE:
		bp->b_flags = B_WRITE;
		break;
	default:
		freerbuf(bp);
		return (EINVAL);
	}

	bp->b_un.b_addr = bufaddr;
	bp->b_bcount	= length;
	bp->b_lblkno	= start;
	bp->b_edev	= P0_RAW_DISK | SKD_FULLDISK;

	(void) skd_strategy(bp);

	(void) biowait(bp);

	rv = geterror(bp);

	freerbuf(bp);

	return (rv);
}

/*
 *
 * Name:	skd_tg_getphygeom, gets disk geometry.
 *
 * Inputs:	dip		- dev_info structure.
 *		phygeomp	- pointer to geometry structure.
 *
 * Returns:	Zero.
 *
 */
static int
skd_tg_getphygeom(dev_info_t *dip, cmlb_geom_t *phygeomp)
{
	skd_device_t *skdev;

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state,
	    ddi_get_instance(dip));

	if (skd_dbg_blkdev)
		cmn_err(CE_NOTE, "skd_tg_getphygeom");

	phygeomp->g_ncyl	= (uint_t)((skdev->Nblocks) /
	    (SKD_GEO_SECS_TRACK * SKD_GEO_HEADS));
	phygeomp->g_acyl	= 2;
	phygeomp->g_nhead	= (ushort_t)SKD_GEO_HEADS;
	phygeomp->g_nsect	= (ushort_t)SKD_GEO_SECS_TRACK;
	phygeomp->g_secsize	= 512;
	phygeomp->g_capacity	= skdev->Nblocks;
	phygeomp->g_intrlv	= 0;
	phygeomp->g_rpm		= 1;

	return (0);
}

/*
 *
 * Name:	skd_tg_getvirtgeom, gets disk geometry.
 *
 * Inputs:	dip		- dev_info structure.
 *		phygeomp	- pointer to geometry structure.
 *
 * Returns:	Zero.
 *
 */
static int
skd_tg_getvirtgeom(dev_info_t *dip, cmlb_geom_t *virtgeomp)
{
	skd_device_t *skdev;

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state,
	    ddi_get_instance(dip));

	if (skd_dbg_blkdev)
		cmn_err(CE_NOTE, "skd_tg_getvirtgeom");

	virtgeomp->g_ncyl	= (uint_t)((skdev->Nblocks) /
	    (SKD_GEO_SECS_TRACK * SKD_GEO_HEADS));
	virtgeomp->g_acyl	= 2;
	virtgeomp->g_nhead	= (ushort_t)SKD_GEO_HEADS;
	virtgeomp->g_nsect	= (ushort_t)SKD_GEO_SECS_TRACK;
	virtgeomp->g_secsize	= 512;
	virtgeomp->g_capacity	= skdev->Nblocks;
	virtgeomp->g_intrlv	= 0;
	virtgeomp->g_rpm	= 3600;

	return (0);
}

/*
 *
 * Name:	skd_tg_getcapacity, retrieves device's capacity.
 *
 * Inputs:	dip		- dev_info structure.
 *		capp		- address to store capacity.
 *
 * Returns:	Zero.
 *
 */
static int
skd_tg_getcapacity(dev_info_t *dip, diskaddr_t *capp)
{
	skd_device_t *skdev;
	diskaddr_t   Nblocks;

	skdev = (skd_device_t *)ddi_get_soft_state(skd_state,
	    ddi_get_instance(dip));

	if (skd_dbg_blkdev)
		cmn_err(CE_NOTE, "skd_tg_getcapacity");

	Nblocks = skdev->Nblocks;
	*capp = Nblocks;

	return (0);
}

/*
 *
 * Name:	skd_tg_getattribute, retrieves device's R/W capability.
 *
 * Inputs:	dip		- dev_info structure.
 *		tgattribute	- address to store attribute.
 *
 * Returns:	Zero.
 *
 */
static int
skd_tg_getattribute(dev_info_t *dip, tg_attribute_t *tgattribute)
{
	if (skd_dbg_blkdev)
		cmn_err(CE_NOTE, "skd_tg_getattribute");

	tgattribute->media_is_writable = B_TRUE;

	return (0);
}
#endif

/*
 *
 * Name:	skd_bd_driveinfo, retrieves device's info.
 *
 * Inputs:	drive		- drive data structure.
 *		arg		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
void
skd_bd_driveinfo(void *arg, sbd_drive_t *drive)
{
	skd_device_t	*skdev = arg;

	drive->d_qsize		= (skdev->queue_depth_limit * 4) / 5;
	drive->d_maxxfer	= SKD_DMA_MAXXFER;
	drive->d_removable	= B_FALSE;
	drive->d_hotpluggable	= B_FALSE;
	drive->d_target		= 0;
	drive->d_lun		= 0;
}

/*
 *
 * Name:	skd_bd_mediainfo, retrieves device media info.
 *
 * Inputs:	arg		- device state structure.
 *		media		- container for media info.
 *
 * Returns:	Zero.
 *
 */
int
skd_bd_mediainfo(void *arg, sbd_media_t *media)
{
	skd_device_t	*skdev = arg;

	media->m_nblks    = skdev->Nblocks;
	media->m_blksize  = 512;
	media->m_readonly = B_FALSE;

	return (0);
}

/*
 *
 * Name:	skd_rw, performs R/W requests for sblkdev driver.
 *
 * Inputs:	skdev		- device state structure.
 *		xfer		- tranfer structure.
 *		dir		- I/O direction.
 *
 * Returns:	EAGAIN if device is not online.  EIO if blkdev wants us to
 *		be a dump device (for now).
 *		Value returned by skd_start().
 *
 */
int
skd_rw(skd_device_t *skdev, sbd_xfer_t *xfer, int dir)
{
	struct buf		*bp;
	skd_buf_private_t 	*pbuf;
	int 			drive = 0;
	int			errnum = 0;

	/*
	 * The x_flags structure element is not defined in Oracle Solaris
	 */
	/* We'll need to fix this in order to support dump on this device. */
	if (xfer->x_flags & BD_XFER_POLL)
		return (EIO);

	if (skdev->state != SKD_DRVR_STATE_ONLINE) {
		Dcmn_err(CE_NOTE, "Device - not ONLINE");

		return (EAGAIN);
	}

	/*
	 * Ensure we got the resources to go on.
	 */
	bp = getrbuf(KM_SLEEP);
	bzero(bp, sizeof (struct buf));

	bp->b_private = (void *)kmem_zalloc(sizeof (skd_buf_private_t),
	    KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */
	pbuf = (skd_buf_private_t *)bp->b_private;

	WAITQ_LOCK(skdev, 0, 0);
	bp->b_flags  = dir;
	bp->b_lblkno = xfer->x_blkno;
	bp->b_bcount = xfer->x_nblks  << 9;
	bp->b_resid  = bp->b_lblkno;

	pbuf->origin = ORIGIN_BLKDEV;
	pbuf->x_xfer = xfer;

	skd_queue_bp(skdev, drive, bp);
	skdev->ios_queued++;
	WAITQ_UNLOCK(skdev, 0, 0);
	errnum = skd_start(skdev, 200);

	return (errnum);
}

/*
 *
 * Name:	skd_bd_read, performs sblkdev read requests.
 *
 * Inputs:	arg		- device state structure.
 *		xfer		- tranfer request structure.
 *
 * Returns:	Value return by skd_rw().
 *
 */
int
skd_bd_read(void *arg, sbd_xfer_t *xfer)
{
	int	rval;

	rval = skd_rw((skd_device_t *)arg, xfer, B_READ);

	return (rval);
}

/*
 *
 * Name:	skd_bd_write, performs sblkdev write requests.
 *
 * Inputs:	arg		- device state structure.
 *		xfer		- tranfer request structure.
 *
 * Returns:	Value return by skd_rw().
 *
 */
int
skd_bd_write(void *arg, sbd_xfer_t *xfer)
{
	return (skd_rw((skd_device_t *)arg, xfer, B_WRITE));
}

/*
 *
 * Name:	skd_host_alloc, setup sblkdev interface.
 *
 * Inputs:	dip		- dev_info structure.
 *		skdev		- device state structure.
 *		nslot		- number of devices (1).
 *		ops		- supported operations.
 *		dma		- DMA pointer for this instance.
 *
 * Returns:	Host_t data structure.
 *
 */
skd_host_t *
skd_host_alloc(dev_info_t *dip, skd_device_t *skdev, int nslot,
    sbd_ops_t *ops, ddi_dma_attr_t *dma)
{
	skd_host_t	*h;

	h = (skd_host_t *)kmem_zalloc(sizeof (*h), KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */
	h->h_nslot = nslot;
	h->h_slots = (skd_device_t *)kmem_zalloc(sizeof (skd_device_t) *
	    nslot, KM_SLEEP);
	/* No NULL check needed with KM_SLEEP to kmem_*alloc(). */
	h->h_dma   = dma;
	h->h_dip   = dip;

	skdev->s_hostp	  = h;
	skdev->s_slot_num = 0;
	skdev->s_ops = *ops;

	/* initialize each slot */
	for (int i = 0; i < nslot; i++) {
		skd_device_t *skdev = &h->h_slots[i];

		skdev->s_hostp	  = h;
		skdev->s_slot_num = i;
		skdev->s_ops	  = *ops;
	}

	return (h);
}

/*
 *
 * Name:	skd_host_dealloc, deallocates the host structure.
 *
 * Inputs:	skdev		- device state structure.
 *
 * Returns:	Nothing.
 *
 */
void
skd_host_dealloc(skd_device_t *skdev)
{
	skd_host_t	*h;

	h = skdev->s_hostp;
	kmem_free(h->h_slots, sizeof (skd_device_t));

	kmem_free(skdev->s_hostp, sizeof (*h));
}

/*
 *
 * Name:	skd_do_inq_page_00, creates a special inquiry string.
 *
 * Inputs:	skdev		- device state structure.
 *		skcomp		- skcomp structure.
 *		skerr		- error structure.
 *		cdb		- command data buffer.
 *		buf		- contains built inquiry.
 *
 * Returns:	Nothing.
 *
 */
/* ARGSUSED */	/* Upstream common source with other platforms. */
static void
skd_do_inq_page_00(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr,
    uint8_t *cdb, uint8_t *buf)
{
	uint16_t insert_pt, max_bytes, drive_pages, drive_bytes, new_size;

	/*
	 * Caller requested "supported pages".  The driver needs to insert
	 * its page.
	 */
	Dcmn_err(CE_NOTE, "skd_do_driver_inquiry: modify supported pages.");

	/*
	 * If the device rejected the request because the CDB was
	 * improperly formed, then just leave.
	 */
	if (skcomp->status == SAM_STAT_CHECK_CONDITION &&
	    skerr->key == ILLEGAL_REQUEST &&
	    skerr->code == 0x24) {

		return;
	}

	/* Get the amount of space the caller allocated */
	max_bytes = (cdb[3] << 8) | cdb[4];

	/* Get the number of pages actually returned by the device */
	drive_pages = (buf[2] << 8) | buf[3];
	drive_bytes = drive_pages + 4;
	new_size = drive_pages + 1;

	/*
	 * Supported pages must be in numerical order, so find where
	 * the driver page needs to be inserted into the list of
	 * pages returned by the device.
	 */
	for (insert_pt = 4; insert_pt < drive_bytes; insert_pt++) {
		if (buf[insert_pt] == DRIVER_INQ_EVPD_PAGE_CODE) {
			return; /* Device using this page code. abort */
		} else if (buf[insert_pt] > DRIVER_INQ_EVPD_PAGE_CODE) {
			break;
		}
	}

	if (insert_pt < max_bytes) {
		uint16_t uu;

		/* Shift everything up one byte to make room. */
		for (uu = new_size + 3; uu > insert_pt; uu--) {
			buf[uu] = buf[uu - 1];
		}
		buf[insert_pt] = DRIVER_INQ_EVPD_PAGE_CODE;

		/* SCSI byte order increment of num_returned_bytes by 1 */
		skcomp->num_returned_bytes =
		    be32_to_cpu(skcomp->num_returned_bytes) + 1;
		skcomp->num_returned_bytes =
		    be32_to_cpu(skcomp->num_returned_bytes);
	}

	/* update page length field to reflect the driver's page too */
	buf[2] = (uint8_t)((new_size >> 8) & 0xFF);
	buf[3] = (uint8_t)((new_size >> 0) & 0xFF);
}

/*
 *
 * Name:	skd_get_link_info, retrieves device speed info.
 *
 * Inputs:	pdev		- pci_dev structure
 *		speed		- address to store speed.
 *		width		- address to storewidth.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_get_link_info(struct pci_dev *pdev, u8* speed, u8* width)
{
	int 		pcie_reg;
	u16 		pci_bus_speed;
	u8 		pci_lanes;
	skd_device_t	*skdev = pdev->skdev;

	pcie_reg = skd_pci_find_capability(skdev, PCI_CAP_ID_EXP);
	if (pcie_reg) {
		u16 linksta;

		linksta  = skd_pci_get16(skdev, pcie_reg + PCI_EXP_LNKSTA);

		pci_bus_speed = linksta & 0xF;
		pci_lanes = (linksta & 0x3F0) >> 4;
	} else {
		*speed = STEC_LINK_UNKNOWN;
		*width = 0xFF;
		return;
	}

	switch (pci_bus_speed) {
	case 1:
	*speed = STEC_LINK_2_5GTS;
	break;
	case 2:
	*speed = STEC_LINK_5GTS;
	break;
	case 3:
	*speed = STEC_LINK_8GTS;
	break;
	default:
	*speed = STEC_LINK_UNKNOWN;
	break;
	}

	if (pci_lanes <= 0x20) {
		*width = pci_lanes;
	} else {
		*width = 0xFF;
	}
}

/*
 *
 * Name:	skd_do_inq_page_da, creates a special inquiry string.
 *
 * Inputs:	skdev		- device state structure.
 *		skcomp		- skcomp structure.
 *		skerr		- error structure.
 *		cdb		- SCSI command block.
 *		buf		- contains built inquiry.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_do_inq_page_da(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr,
    uint8_t *cdb, uint8_t *buf)
{
	unsigned 			ver_byte;
	unsigned 			max_bytes;
	char 				*ver = DRV_VER_COMPL;
	u16 				val;
	int 				bus, slot, func;
	struct driver_inquiry_data 	inq;


	Dcmn_err(CE_NOTE, "skd_do_driver_inquiry: return driver page");

	bzero(&inq, sizeof (inq));

	inq.pageCode = DRIVER_INQ_EVPD_PAGE_CODE;

	/*
	 * Need to figure out how to bus, func, & dev
	 */
	if (skd_pci_get_info(skdev, &bus, &slot, &func) == DDI_SUCCESS) {
		skd_get_link_info(skdev->pdev,
		    &inq.pcieLinkSpeed, &inq.pcieLinkLanes);

		inq.pcieBusNumber = (uint16_t)bus;
		inq.pcieDeviceNumber = (uint8_t)slot;
		inq.pcieFunctionNumber = (uint8_t)func;

		val = skd_pci_get16(skdev, PCI_VENDOR_ID);
		inq.pcieVendorId = cpu_to_be16(val);

		val = skd_pci_get16(skdev, PCI_DEVICE_ID);
		inq.pcieDeviceId = cpu_to_be16(val);

		val = skd_pci_get16(skdev, PCI_SUBSYSTEM_VENDOR_ID);
		inq.pcieSubsystemVendorId = cpu_to_be16(val);

		val = skd_pci_get16(skdev, PCI_SUBSYSTEM_ID);
		inq.pcieSubsystemDeviceId = cpu_to_be16(val);
	} else {
		inq.pcieBusNumber = 0xFFFF;
		inq.pcieDeviceNumber = 0xFF;
		inq.pcieFunctionNumber = 0xFF;
		inq.pcieLinkSpeed = 0xFF;
		inq.pcieLinkLanes = 0xFF;
		inq.pcieVendorId = 0xFFFF;
		inq.pcieDeviceId = 0xFFFF;
		inq.pcieSubsystemVendorId = 0xFFFF;
		inq.pcieSubsystemDeviceId = 0xFFFF;
	}

	/* Driver version, fixed lenth, padded with spaces on the right */
	inq.driverVersionLength = sizeof (inq.driverVersion);
	(void) memset(&inq.driverVersion, ' ', sizeof (inq.driverVersion));
	for (ver_byte = 0; ver_byte < sizeof (inq.driverVersion); ver_byte++) {
		if (ver[ver_byte] != 0) {
			inq.driverVersion[ver_byte] = ver[ver_byte];
		} else {
			break;
		}
	}

	inq.pageLength = cpu_to_be16((sizeof (inq) - 4));

	/* Clear the error set by the device */
	skcomp->status = SAM_STAT_GOOD;
	bzero((void *)skerr, sizeof (*skerr));

	/* copy response into output buffer */
	max_bytes = (cdb[3] << 8) | cdb[4];
	(void) memcpy(buf, &inq, min_t(unsigned, max_bytes, sizeof (inq)));

	skcomp->num_returned_bytes =
	    be32_to_cpu(min_t(uint16_t, max_bytes, sizeof (inq)));
}

/*
 *
 * Name:	skd_do_driver_inq, calls routines to build an inquiry string.
 *
 * Inputs:	skdev		- device state structure.
 *		skcomp		- skcomp structure.
 *		skerr		- error structure,
 *		cdb		- SCSI command block.
 *		buf		- buffer to contain inquiry string.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_do_driver_inq(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr,
    uint8_t *cdb, uint8_t *buf)
{
	if (buf == NULL)
		return;
	else if (cdb[0] != INQUIRY)
		return; /* Not an INQUIRY */
	else if ((cdb[1] & 1) == 0) {
		return; /* EVPD not set */
	} else if (cdb[2] == 0) {
		/* Need to add driver's page to supported pages list */
		skd_do_inq_page_00(skdev, skcomp, skerr, cdb, buf);
	} else if (cdb[2] == DRIVER_INQ_EVPD_PAGE_CODE) {
		/* Caller requested driver's page */
		skd_do_inq_page_da(skdev, skcomp, skerr, cdb, buf);
	}
}

/*
 *
 * Name:	skd_process_scsi_inq, builds driver inquiry string.
 *
 * Inputs:	skdev		- device state structure.
 *		skcomp		- skcomp data structure.
 *		skerr		- error data structure.
 *		spspcl		- skspcl data structure.
 *
 * Returns:	Nothing.
 *
 */
static void
skd_process_scsi_inq(struct skd_device *skdev,
    volatile struct fit_completion_entry_v1 *skcomp,
    volatile struct fit_comp_error_info *skerr,
    struct skd_special_context *skspcl)
{
	uint8_t 		*buf;
	struct fit_msg_hdr 	*fmh = (struct fit_msg_hdr *)skspcl->msg_buf64;
	struct skd_scsi_request *scsi_req = (struct skd_scsi_request *)&fmh[1];

	buf = skd_sg_1st_page_ptr(skspcl->req.sg);

	if (buf)
		skd_do_driver_inq(skdev, skcomp, skerr, scsi_req->cdb, buf);
}

/*
 *
 * Name:	skd_sg_1st_page_ptr,  retrieve a valid S/G page.
 *
 * Inputs:	sg		- S/G list.
 *
 * Returns:	Returns a chunk of DMAable address space.
 *
 */
static unsigned char
*skd_sg_1st_page_ptr(struct scatterlist *sg)
{
	unsigned char *page;

	if (sg == NULL)
		return (NULL);

	page = (unsigned char *)sg[0].page;

	return (page);
}

/*
 *
 * Name:	skd_pci_get_info, retrieves PCI bus, slot, & func.
 *
 * Inputs:	skdev		- device state structure.
 *		bus		- PCI bus container.
 *		slot		- PCI slot container.
 *		func		- PCI func container.
 *
 * Returns:	DDI_SUCCESS or DDI_FAILURE.
 *
 */
static int
skd_pci_get_info(skd_device_t *skdev, int *bus, int *slot, int *func)
{
	int 	*regs_list;
	uint_t 	nregs = 0;

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, skdev->dip,
	    DDI_PROP_DONTPASS, "reg", (int **)&regs_list, &nregs)
	    != DDI_PROP_SUCCESS) {
		cmn_err(CE_NOTE, "skd_pci_get_info: get pci function "
		    "bus device failed");
		goto error;
	}

	*bus  = (int)PCI_BUS(regs_list[0]);
	*slot = (int)PCI_DEVICE(regs_list[0]);
	*func = (int)PCI_FUNCTION(regs_list[0]);

	if (nregs > 0)
		ddi_prop_free(regs_list);

	return (DDI_SUCCESS);
error:
	if (nregs > 0)
		ddi_prop_free(regs_list);

	return (DDI_FAILURE);
}
