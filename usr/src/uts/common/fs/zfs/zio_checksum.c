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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2013 Nexenta Systems, Inc.  All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/zil.h>
#include <zfs_fletcher.h>
#ifdef	NZA_CLOSED
#include <sys/isal.h>
#endif	/* NZA_CLOSED */

/*
 * Tunables
 *
 * enable accelleration by default (acceleration module is required)
 * set min size to the point where FPU save/restore overhead is breaking
 * even with the speedup obtained by acceleration
 */
int zfs_parallel_checksum_enabled_write = 1;
int zfs_parallel_checksum_enabled_read = 1;

/*
 * Checksum vectors.
 *
 * In the SPA, everything is checksummed.  We support checksum vectors
 * for three distinct reasons:
 *
 *   1. Different kinds of data need different levels of protection.
 *	For SPA metadata, we always want a very strong checksum.
 *	For user data, we let users make the trade-off between speed
 *	and checksum strength.
 *
 *   2. Cryptographic hash and MAC algorithms are an area of active research.
 *	It is likely that in future hash functions will be at least as strong
 *	as current best-of-breed, and may be substantially faster as well.
 *	We want the ability to take advantage of these new hashes as soon as
 *	they become available.
 *
 *   3. If someone develops hardware that can compute a strong hash quickly,
 *	we want the ability to take advantage of that hardware.
 *
 * Of course, we don't want a checksum upgrade to invalidate existing
 * data, so we store the checksum *function* in eight bits of the bp.
 * This gives us room for up to 256 different checksum functions.
 *
 * When writing a block, we always checksum it with the latest-and-greatest
 * checksum function of the appropriate strength.  When reading a block,
 * we compare the expected checksum against the actual checksum, which we
 * compute via the checksum function specified by BP_GET_CHECKSUM(bp).
 */

/*ARGSUSED*/
static void
zio_checksum_off(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
	ZIO_SET_CHECKSUM(zcp, 0, 0, 0, 0);
}

zio_checksum_info_t zio_checksum_table[ZIO_CHECKSUM_FUNCTIONS] = {
	{{NULL,			NULL},			0, 0, 0, "inherit"},
	{{NULL,			NULL},			0, 0, 0, "on"},
	{{zio_checksum_off,	zio_checksum_off},	0, 0, 0, "off"},
	{{zio_checksum_SHA256,	zio_checksum_SHA256},	1, 1, 0, "label"},
	{{zio_checksum_SHA256,	zio_checksum_SHA256},	1, 1, 0, "gang_header"},
	{{fletcher_2_native,	fletcher_2_byteswap},	0, 1, 0, "zilog"},
	{{fletcher_2_native,	fletcher_2_byteswap},	0, 0, 0, "fletcher2"},
	{{fletcher_4_native,	fletcher_4_byteswap},	1, 0, 0, "fletcher4"},
	{{zio_checksum_SHA256,	zio_checksum_SHA256},	1, 0, 1, "sha256"},
	{{fletcher_4_native,	fletcher_4_byteswap},	0, 1, 0, "zilog2"},
	{{zio_checksum_SHA1CRC32,	zio_checksum_SHA1CRC32},	0, 0, 1,
		"sha1crc32"},
};

#pragma	weak zio_parallel_checksum_fsm = _zio_parallel_checksum_fsm
/* ARGSUSED */
static int
_zio_parallel_checksum_fsm(zio_t *zio, enum zio_checksum checksum,
    void *data, uint64_t size, int can_accumulate, zio_cksum_t *result,
    int *zio_progress)
{
	return (1); /* error */
}

enum zio_checksum
zio_checksum_select(enum zio_checksum child, enum zio_checksum parent)
{
	ASSERT(child < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(parent < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(parent != ZIO_CHECKSUM_INHERIT && parent != ZIO_CHECKSUM_ON);

	if (child == ZIO_CHECKSUM_INHERIT)
		return (parent);

	if (child == ZIO_CHECKSUM_ON)
		return (ZIO_CHECKSUM_ON_VALUE);

	return (child);
}

enum zio_checksum
zio_checksum_dedup_select(spa_t *spa, enum zio_checksum child,
    enum zio_checksum parent)
{
	ASSERT((child & ZIO_CHECKSUM_MASK) < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT((parent & ZIO_CHECKSUM_MASK) < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(parent != ZIO_CHECKSUM_INHERIT && parent != ZIO_CHECKSUM_ON);

	if (child == ZIO_CHECKSUM_INHERIT)
		return (parent);

	if (child == ZIO_CHECKSUM_ON)
		return (spa_dedup_checksum(spa));

	if (child == (ZIO_CHECKSUM_ON | ZIO_CHECKSUM_VERIFY))
		return (spa_dedup_checksum(spa) | ZIO_CHECKSUM_VERIFY);

	ASSERT(zio_checksum_table[child & ZIO_CHECKSUM_MASK].ci_dedup ||
	    (child & ZIO_CHECKSUM_VERIFY) || child == ZIO_CHECKSUM_OFF);

	return (child);
}

/*
 * Set the external verifier for a gang block based on <vdev, offset, txg>,
 * a tuple which is guaranteed to be unique for the life of the pool.
 */
static void
zio_checksum_gang_verifier(zio_cksum_t *zcp, blkptr_t *bp)
{
	dva_t *dva = BP_IDENTITY(bp);
	uint64_t txg = BP_PHYSICAL_BIRTH(bp);

	ASSERT(BP_IS_GANG(bp));

	ZIO_SET_CHECKSUM(zcp, DVA_GET_VDEV(dva), DVA_GET_OFFSET(dva), txg, 0);
}

/*
 * Set the external verifier for a label block based on its offset.
 * The vdev is implicit, and the txg is unknowable at pool open time --
 * hence the logic in vdev_uberblock_load() to find the most recent copy.
 */
static void
zio_checksum_label_verifier(zio_cksum_t *zcp, uint64_t offset)
{
	ZIO_SET_CHECKSUM(zcp, offset, 0, 0, 0);
}

/*
 * Generate the checksum.
 */
int
zio_checksum_compute(zio_t *zio, enum zio_checksum checksum,
	void *data, uint64_t size, int can_accumulate)
{
	blkptr_t *bp = zio->io_bp;
	uint64_t offset = zio->io_offset;
	zio_checksum_info_t *ci = &zio_checksum_table[checksum];
	zio_cksum_t cksum;
	int zio_progress = ZIO_PIPELINE_CONTINUE;

	ASSERT((uint_t)checksum < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(ci->ci_func[0] != NULL);

	if (ci->ci_eck) {
		zio_eck_t *eck;

		if (checksum == ZIO_CHECKSUM_ZILOG2) {
			zil_chain_t *zilc = data;

			size = P2ROUNDUP_TYPED(zilc->zc_nused, ZIL_MIN_BLKSZ,
			    uint64_t);
			eck = &zilc->zc_eck;
		} else {
			eck = (zio_eck_t *)((char *)data + size) - 1;
		}
		if (checksum == ZIO_CHECKSUM_GANG_HEADER)
			zio_checksum_gang_verifier(&eck->zec_cksum, bp);
		else if (checksum == ZIO_CHECKSUM_LABEL)
			zio_checksum_label_verifier(&eck->zec_cksum, offset);
		else
			bp->blk_cksum = eck->zec_cksum;
		eck->zec_magic = ZEC_MAGIC;
		ci->ci_func[0](data, size, &cksum);
		eck->zec_cksum = cksum;
	} else {
		/* ci_eck is 0 for sha1crc32, sha256 */
		if ((zfs_parallel_checksum_enabled_write == 0) ||
		    (zio_parallel_checksum_fsm(zio, checksum, data, size,
		    can_accumulate, &bp->blk_cksum, &zio_progress))) {
			/* fall back on the non-accelerated algo */
			ci->ci_func[0](data, size, &bp->blk_cksum);
		}
	}

	return (zio_progress);
}

#define	ZIO_CHECKSUM_CRC32_EQUAL(zc1, zc2) \
	((zc1).zc_word[3] == (zc2).zc_word[3])

int
zio_checksum_error(zio_t *zio, zio_bad_cksum_t *info, int *zio_progress_p)
{
	blkptr_t *bp = zio->io_bp;
	uint_t checksum = (bp == NULL ? zio->io_prop.zp_checksum :
	    (BP_IS_GANG(bp) ? ZIO_CHECKSUM_GANG_HEADER : BP_GET_CHECKSUM(bp)));
	int byteswap;
	int error;
	uint64_t size = (bp == NULL ? zio->io_size :
	    (BP_IS_GANG(bp) ? SPA_GANGBLOCKSIZE : BP_GET_PSIZE(bp)));
	uint64_t offset = zio->io_offset;
	void *data = zio->io_data;
	zio_checksum_info_t *ci = &zio_checksum_table[checksum];
	zio_cksum_t actual_cksum, expected_cksum, verifier;

	if (checksum >= ZIO_CHECKSUM_FUNCTIONS || ci->ci_func[0] == NULL)
		return (EINVAL);

	if (ci->ci_eck) {
		zio_eck_t *eck;

		if (checksum == ZIO_CHECKSUM_ZILOG2) {
			zil_chain_t *zilc = data;
			uint64_t nused;

			eck = &zilc->zc_eck;
			if (eck->zec_magic == ZEC_MAGIC)
				nused = zilc->zc_nused;
			else if (eck->zec_magic == BSWAP_64(ZEC_MAGIC))
				nused = BSWAP_64(zilc->zc_nused);
			else
				return (ECKSUM);

			if (nused > size)
				return (ECKSUM);

			size = P2ROUNDUP_TYPED(nused, ZIL_MIN_BLKSZ, uint64_t);
		} else {
			eck = (zio_eck_t *)((char *)data + size) - 1;
		}

		if (checksum == ZIO_CHECKSUM_GANG_HEADER)
			zio_checksum_gang_verifier(&verifier, bp);
		else if (checksum == ZIO_CHECKSUM_LABEL)
			zio_checksum_label_verifier(&verifier, offset);
		else
			verifier = bp->blk_cksum;

		byteswap = (eck->zec_magic == BSWAP_64(ZEC_MAGIC));
		if (byteswap)
			byteswap_uint64_array(&verifier, sizeof (zio_cksum_t));

		expected_cksum = eck->zec_cksum;
		eck->zec_cksum = verifier;
		ci->ci_func[byteswap](data, size, &actual_cksum);
		eck->zec_cksum = expected_cksum;

		if (byteswap)
			byteswap_uint64_array(&expected_cksum,
			    sizeof (zio_cksum_t));
	} else {
		boolean_t cksum_ready = B_FALSE;
		/* ci_eck is 0 for sha256, sha1crc32 */
		ASSERT(!BP_IS_GANG(bp));
		byteswap = BP_SHOULD_BYTESWAP(bp);
		expected_cksum = bp->blk_cksum;
		/*
		 * SHA1CRC32 for reading computes only CRC, but CRC
		 * is not parallel in ISA-L
		 */
		if (checksum != ZIO_CHECKSUM_SHA1CRC32 &&
		    (byteswap == 0) && (zfs_parallel_checksum_enabled_read)) {
			int zio_progress = ZIO_PIPELINE_CONTINUE;
			int rc = 0;
			int can_accumulate = (zio_progress_p != NULL);

			DTRACE_PROBE(parallel_cksum_verify_start);
			rc = zio_parallel_checksum_fsm(zio, checksum,
			    data, size, can_accumulate,
			    &zio->actual_cksum, &zio_progress);
			DTRACE_PROBE(parallel_cksum_verify_end);
			if (rc == 0) {
				ASSERT(can_accumulate);
				*zio_progress_p = zio_progress;
				/* return if not completed calculation */
				if (zio_progress ==
				    ZIO_PIPELINE_RESTART_STAGE) {
					return (0);
				} else {
					/* actual checksum calculated */
					actual_cksum = zio->actual_cksum;
					cksum_ready = B_TRUE;
				}
			}
		}

		if (!cksum_ready) {
			/* invoke standard algo because of byteswap */
#ifdef	NZA_CLOSED
			if (checksum == ZIO_CHECKSUM_SHA1CRC32) {
				/*
				 * SHA1CRC32 is special case
				 * for read path it computes only CRC32
				 */
				_zio_checksum_SHA1CRC32(data, size,
				    &actual_cksum, B_TRUE);
				cksum_ready = B_TRUE;
			}
#endif	/* NZA_CLOSED */
			if (!cksum_ready) {
				ci->ci_func[byteswap](data, size,
				    &actual_cksum);
			}
		}
	}

	info->zbc_expected = expected_cksum;
	info->zbc_actual = actual_cksum;
	info->zbc_checksum_name = ci->ci_name;
	info->zbc_byteswapped = byteswap;
	info->zbc_injected = 0;
	info->zbc_has_cksum = 1;

	/*
	 * SHA1CRC32 is special case - check only CRC32 for read path
	 */
	if (!((checksum == ZIO_CHECKSUM_SHA1CRC32) ?
	    ZIO_CHECKSUM_CRC32_EQUAL(actual_cksum, expected_cksum) :
	    ZIO_CHECKSUM_EQUAL(actual_cksum, expected_cksum))) {
		DTRACE_PROBE2(cksum_error, zio_cksum_t *, &actual_cksum,
		    zio_cksum_t *, &expected_cksum);
		return (ECKSUM);
	}

	if (zio_injection_enabled && !zio->io_error &&
	    (error = zio_handle_fault_injection(zio, ECKSUM)) != 0) {

		info->zbc_injected = 1;
		return (error);
	}

	return (0);
}
