/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2014 Nexenta Systems, Inc.  All rights reserved.
 */

#include <sys/conf.h>
#include <sys/file.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/scsi/scsi.h>
#include <sys/scsi/impl/scsi_reset_notify.h>
#include <sys/scsi/generic/mode.h>
#include <sys/disp.h>
#include <sys/byteorder.h>
#include <sys/atomic.h>
#include <sys/sdt.h>
#include <sys/dkio.h>

#include <sys/dmu.h>
#include <sys/txg.h>
#include <sys/refcount.h>
#include <sys/zvol.h>

#include <sys/stmf.h>
#include <sys/lpif.h>
#include <sys/portif.h>
#include <sys/stmf_ioctl.h>
#include <sys/stmf_sbd_ioctl.h>

#include "stmf_sbd.h"
#include "sbd_impl.h"

/* ATS routines. */
#define	SBD_ATS_MAX_NBLKS	32

uint8_t
sbd_ats_max_nblks(void)
{
	return (SBD_ATS_MAX_NBLKS);
}

#define	is_overlapping(start1, len1, start2, len2) \
	((start2) > (start1) ? ((start2) - (start1)) < (len1) : \
	((start1) - (start2)) < (len2))

static sbd_status_t
sbd_alloc_ats_handle(scsi_task_t *task, uint64_t lba, uint64_t count,
	ats_state_t **ats_state_ret);

sbd_status_t
sbd_ats_handling_before_io(scsi_task_t *task, struct sbd_lu *sl,
	uint64_t lba, uint64_t count, ats_state_t **ats_handle)
{
	sbd_status_t ret = SBD_SUCCESS;
	ats_state_t *ats_state, *ats_state_ret;
	uint64_t lbaend;

	mutex_enter(&sl->sl_lock);
	/*
	 * if the list is empty then just add the element to the list and
	 * return success. There is no overlap.  This is done for every
	 * read, write or compare and write.
	 */
	if (list_is_empty(&sl->sl_ats_io_list)) {
		ret = sbd_alloc_ats_handle(task, lba, count, &ats_state_ret);
		list_insert_head(&sl->sl_ats_io_list, ats_state_ret);
		*ats_handle = ats_state_ret;
		mutex_exit(&sl->sl_lock);
		return (ret);
	}

	/*
	 * There are inflight operations.  As a result the list must be scanned
	 * and if there are any overlaps then SBD_BUSY should be returned.
	 *
	 * Duplicate reads and writes are allowed and kept on the list
	 * since there is no reason that overlapping IO operations should
	 * be blocked.
	 *
	 * A command the conflicts with a running compare and write will
	 * be rescheduled and rerun.  This is handled by stmf_task_poll_lu.
	 * There is a possibility that a command can be starved and still
	 * return busy, which is valid in the SCSI protocol.
	 */

	lbaend = lba + count - 1;
	for (ats_state = list_head(&sl->sl_ats_io_list); ats_state != NULL;
			ats_state = list_next(&sl->sl_ats_io_list, ats_state)) {
		/*
		 * the current command is a compare and write, if there is any
		 * overlap return error
		 */
		if (task->task_cdb[0] == SCMD_COMPARE_AND_WRITE) {
			if (lba >= ats_state->as_cur_ats_lba &&
				lba <= ats_state->as_cur_ats_lba_end) {
				ret = SBD_BUSY;
				goto exit;
			}
			if (lbaend >= ats_state->as_cur_ats_lba &&
				lbaend <= ats_state->as_cur_ats_lba_end) {
				ret =  SBD_BUSY;
				goto exit;
			}
			continue;
		}

		/*
		 * The current command is a read or write or variant.
		 * Only return an error if the current command conflicts
		 * with a compare and write that is in progress
		 */
		if (ats_state->as_scmd != SCMD_COMPARE_AND_WRITE)
			continue;
		if (lba >= ats_state->as_cur_ats_lba &&
			lba <= ats_state->as_cur_ats_lba_end) {
			ret = SBD_BUSY;
			goto exit;
		}
		if (lbaend >= ats_state->as_cur_ats_lba &&
			lbaend <= ats_state->as_cur_ats_lba_end) {
			ret = SBD_BUSY;
			goto exit;
		}
	}

	ret = sbd_alloc_ats_handle(task, lba, count, &ats_state_ret);
	list_insert_tail(&sl->sl_ats_io_list, ats_state_ret);
	*ats_handle = ats_state_ret;
exit:
	mutex_exit(&sl->sl_lock);
	return (ret);
}

void
sbd_ats_handling_after_io(sbd_lu_t *sl, ats_state_t *ats_state)
{
	/*
	 * Remove a item from the list by using the state structure.
	 * This is probably the most common way that an item is
	 * discarded.
	 */
	ASSERT(ats_state != NULL);
	ASSERT(sl != NULL);
	mutex_enter(&sl->sl_lock);
	list_remove(&sl->sl_ats_io_list, ats_state);
	kmem_free(ats_state, sizeof (ats_state_t));
	mutex_exit(&sl->sl_lock);
}

/*
 * just like sbd_ats_handling_after_io() but also manages a sbd_cmd
 */
void
sbd_scmd_ats_handling_after_io(sbd_lu_t *sl, sbd_cmd_t *scmd)
{
	sbd_ats_handling_after_io(sl, scmd->ats_state);
}

void
sbd_ats_remove_by_task(scsi_task_t *task, sbd_lu_t *sl)
{
	ats_state_t *ats_state;

	/*
	 * When an abort or some other condition occurs that may need to
	 * remove an element from the list this is the command that is
	 * used.  There are cases where an IO is aborted that may be in
	 * the clean up phase.  sbd_cmd_t may have been freed, or the
	 * other elements may have been freed so the list is scanned
	 * to find the IO task and the element is removed.
	 */

	mutex_enter(&sl->sl_lock);
	for (ats_state = list_head(&sl->sl_ats_io_list); ats_state != NULL;
			ats_state = list_next(&sl->sl_ats_io_list, ats_state)) {
		if (ats_state->as_cur_ats_task == task) {
			/*
			 * abort can call this routine multiple
			 * times, so search the list and then remove the
			 * io state without dropping the lock
			 */
			list_remove(&sl->sl_ats_io_list, ats_state);
			kmem_free(ats_state, sizeof (ats_state_t));
			break;
		}
	}
	mutex_exit(&sl->sl_lock);
}

static sbd_status_t
sbd_alloc_ats_handle(scsi_task_t *task, uint64_t lba, uint64_t count,
		ats_state_t **ats_state_ret)
{
	ats_state_t *ats_state;

	ats_state = (ats_state_t *)kmem_zalloc(sizeof (ats_state_t), KM_SLEEP);
	ats_state->as_scmd = task->task_cdb[0];
	ats_state->as_cur_ats_lba = lba;
	ats_state->as_cur_ats_len = count;
	ats_state->as_cur_ats_lba_end = lba + count - 1;
	ats_state->as_cur_ats_task = task;
	*ats_state_ret = ats_state;

	return (SBD_SUCCESS);
}

void
sbd_free_ats_handle(struct scsi_task *task, sbd_cmd_t *scmd)
{
	sbd_lu_t *sl = (sbd_lu_t *)task->task_lu->lu_provider_private;

	ASSERT(scmd->ats_state != NULL);
	sbd_scmd_ats_handling_after_io(sl, scmd);
}

static sbd_status_t
sbd_compare_and_write(struct scsi_task *task, sbd_cmd_t *scmd,
    uint32_t *ret_off)
{
	sbd_lu_t *sl = (sbd_lu_t *)task->task_lu->lu_provider_private;
	uint8_t *buf;
	sbd_status_t ret;
	uint64_t addr;
	uint32_t len, i;

	addr = READ_SCSI64(&task->task_cdb[2], uint64_t);
	len = (uint32_t)task->task_cdb[13];

	addr <<= sl->sl_data_blocksize_shift;
	len <<= sl->sl_data_blocksize_shift;
	buf = kmem_alloc(len, KM_SLEEP);
	ret = sbd_data_read(sl, task, addr, (uint64_t)len, buf);
	if (ret != SBD_SUCCESS)
		goto compare_and_write_done;
	/*
	 * Can't use bcmp here. We need mismatch offset.
	 */
	for (i = 0; i < len; i++) {
		if (buf[i] != scmd->trans_data[i])
			break;
	}
	if (i != len) {
		*ret_off = i;
		ret = SBD_COMPARE_FAILED;
		goto compare_and_write_done;
	}
	ret = sbd_data_write(sl, task, addr, (uint64_t)len,
	    scmd->trans_data + len);

compare_and_write_done:
	kmem_free(buf, len);
	return (ret);
}

static void
sbd_send_miscompare_status(struct scsi_task *task, uint32_t miscompare_off)
{
	uint8_t sd[18];

	task->task_scsi_status = STATUS_CHECK;
	bzero(sd, 18);
	sd[0] = 0xF0;
	sd[2] = 0xe;
	SCSI_WRITE32(&sd[3], miscompare_off);
	sd[7] = 10;
	sd[12] = 0x1D;
	task->task_sense_data = sd;
	task->task_sense_length = 18;
	(void) stmf_send_scsi_status(task, STMF_IOF_LU_DONE);
}

void
sbd_handle_ats_xfer_completion(struct scsi_task *task, sbd_cmd_t *scmd,
    struct stmf_data_buf *dbuf, uint8_t dbuf_reusable)
{
	// sbd_lu_t *sl = (sbd_lu_t *)task->task_lu->lu_provider_private;
	uint64_t laddr;
	uint32_t buflen, iolen, miscompare_off;
	int ndx;
	sbd_status_t ret;

	if (dbuf->db_xfer_status != STMF_SUCCESS) {
		sbd_free_ats_handle(task, scmd);
		stmf_abort(STMF_QUEUE_TASK_ABORT, task,
		    dbuf->db_xfer_status, NULL);
		return;
	}

	if (ATOMIC8_GET(scmd->nbufs) > 0) {
		atomic_add_8(&scmd->nbufs, -1);
	}

	if (scmd->flags & SBD_SCSI_CMD_XFER_FAIL) {
		goto ATS_XFER_DONE;
	}

	if (ATOMIC32_GET(scmd->len) != 0) {
		/*
		 * Initiate the next port xfer to occur in parallel
		 * with writing this buf.  A side effect of sbd_do_ats_xfer is
		 * it may set scmd_len to 0.  This means all the data
		 * transfers have been started, not that they are done.
		 */
		sbd_do_ats_xfer(task, scmd, NULL, 0);
	}

	/*
	 * move the most recent data transfer to the temporary buffer
	 * used for the compare and write function.
	 */
	if (scmd->trans_data == NULL)
		cmn_err(CE_PANIC, "compare and write null buffer");
	laddr = dbuf->db_relative_offset;
	for (buflen = 0, ndx = 0; (buflen < dbuf->db_data_size) &&
	    (ndx < dbuf->db_sglist_length); ndx++) {
		iolen = min(dbuf->db_data_size - buflen,
		    dbuf->db_sglist[ndx].seg_length);
		if (iolen == 0)
			break;
		bcopy(dbuf->db_sglist[ndx].seg_addr, &scmd->trans_data[laddr],
		    iolen);
		buflen += iolen;
		laddr += (uint64_t)iolen;
	}
	task->task_nbytes_transferred += buflen;

ATS_XFER_DONE:
	if (ATOMIC32_GET(scmd->len) == 0 ||
			scmd->flags & SBD_SCSI_CMD_XFER_FAIL) {
		stmf_free_dbuf(task, dbuf);
		/*
		 * if this is not the last buffer to be transfered then exit
		 * and wait for the next buffer.  Once nbufs is 0 then all the
		 * data has arrived and the compare can be done.
		 */
		if (ATOMIC8_GET(scmd->nbufs) > 0)
			return;
		scmd->flags &= ~SBD_SCSI_CMD_ACTIVE;
		if (scmd->flags & SBD_SCSI_CMD_XFER_FAIL) {
			sbd_free_ats_handle(task, scmd);
			stmf_scsilib_send_status(task, STATUS_CHECK,
			    STMF_SAA_WRITE_ERROR);
		} else {
			ret = sbd_compare_and_write(task, scmd,
			    &miscompare_off);
			sbd_free_ats_handle(task, scmd);
			if (ret != SBD_SUCCESS) {
				if (ret != SBD_COMPARE_FAILED) {
					stmf_scsilib_send_status(task,
					    STATUS_CHECK, STMF_SAA_WRITE_ERROR);
				} else {
					sbd_send_miscompare_status(task,
					    miscompare_off);
				}
			} else {
				stmf_scsilib_send_status(task, STATUS_GOOD, 0);
			}
		}
		ASSERT(scmd->nbufs == 0 &&
			(scmd->flags & SBD_SCSI_CMD_TRANS_DATA) &&
			scmd->trans_data != NULL);
		kmem_free(scmd->trans_data, scmd->trans_data_len);
		scmd->trans_data = NULL; /* force panic later if re-entered */
		scmd->trans_data_len = 0;
		scmd->flags &= ~SBD_SCSI_CMD_TRANS_DATA;
		return;
	}
	sbd_do_ats_xfer(task, scmd, dbuf, dbuf_reusable);
}

void
sbd_do_ats_xfer(struct scsi_task *task, sbd_cmd_t *scmd,
    struct stmf_data_buf *dbuf, uint8_t dbuf_reusable)
{
	uint32_t len;

	if (ATOMIC32_GET(scmd->len) == 0) {
		if (dbuf != NULL) {
			stmf_free_dbuf(task, dbuf);
		}
		return;
	}

	if ((dbuf != NULL) &&
	    ((dbuf->db_flags & DB_DONT_REUSE) || (dbuf_reusable == 0))) {
		/* free current dbuf and allocate a new one */
		stmf_free_dbuf(task, dbuf);
		dbuf = NULL;
	}
	if (dbuf == NULL) {
		uint32_t maxsize, minsize, old_minsize;

		maxsize = (ATOMIC32_GET(scmd->len) > (128*1024)) ? 128*1024 :
		    ATOMIC32_GET(scmd->len);
		minsize = maxsize >> 2;
		do {
			old_minsize = minsize;
			dbuf = stmf_alloc_dbuf(task, maxsize, &minsize, 0);
		} while ((dbuf == NULL) && (old_minsize > minsize) &&
		    (minsize >= 512));
		if (dbuf == NULL) {
			if (ATOMIC8_GET(scmd->nbufs) == 0) {
				sbd_free_ats_handle(task, scmd);
				stmf_abort(STMF_QUEUE_TASK_ABORT, task,
				    STMF_ALLOC_FAILURE, NULL);
			}
			return;
		}
	}

	len = ATOMIC32_GET(scmd->len) > dbuf->db_buf_size ? dbuf->db_buf_size :
	    ATOMIC32_GET(scmd->len);

	dbuf->db_relative_offset = scmd->current_ro;
	dbuf->db_data_size = len;
	dbuf->db_flags = DB_DIRECTION_FROM_RPORT;
	(void) stmf_xfer_data(task, dbuf, 0);
	/*
	 * scmd->nbufs is the outstanding transfers
	 * scmd->len is the number of bytes that are remaing for requests
	 */
	atomic_add_8(&scmd->nbufs, 1);
	atomic_add_32(&scmd->len, -len);
	scmd->current_ro += len;
}

void
sbd_handle_ats(scsi_task_t *task, struct stmf_data_buf *initial_dbuf)
{
	sbd_lu_t *sl = (sbd_lu_t *)task->task_lu->lu_provider_private;
	uint64_t addr, len;
	sbd_cmd_t *scmd;
	stmf_data_buf_t *dbuf;
	ats_state_t *ats_handle;
	uint8_t do_immediate_data = 0;
	/* int ret; */

	task->task_cmd_xfer_length = 0;
	if (task->task_additional_flags &
	    TASK_AF_NO_EXPECTED_XFER_LENGTH) {
		task->task_expected_xfer_length = 0;
	}
	if (sl->sl_flags & SL_WRITE_PROTECTED) {
		stmf_scsilib_send_status(task, STATUS_CHECK,
		    STMF_SAA_WRITE_PROTECTED);
		return;
	}
	addr = READ_SCSI64(&task->task_cdb[2], uint64_t);
	len = (uint64_t)task->task_cdb[13];
	if ((task->task_cdb[1]) || (len > SBD_ATS_MAX_NBLKS)) {
		stmf_scsilib_send_status(task, STATUS_CHECK,
		    STMF_SAA_INVALID_FIELD_IN_CDB);
		return;
	}
	if (len == 0) {
		stmf_scsilib_send_status(task, STATUS_GOOD, 0);
		return;
	}

	/*
	 * This can be called again. It will return the same handle again.
	 */
	if (sbd_ats_handling_before_io(task, sl, addr, len,
						&ats_handle) != SBD_SUCCESS) {
		if (stmf_task_poll_lu(task, 10) != STMF_SUCCESS) {
			stmf_scsilib_send_status(task, STATUS_BUSY, 0);
		}
		return;
	}

	addr <<= sl->sl_data_blocksize_shift;
	len <<= sl->sl_data_blocksize_shift;

	task->task_cmd_xfer_length = len << 1;	/* actual amt of data is 2x */
	if (task->task_additional_flags &
	    TASK_AF_NO_EXPECTED_XFER_LENGTH) {
		task->task_expected_xfer_length = task->task_cmd_xfer_length;
	}
	if ((addr + len) > sl->sl_lu_size) {
		sbd_ats_handling_after_io(sl, ats_handle);
		stmf_scsilib_send_status(task, STATUS_CHECK,
		    STMF_SAA_LBA_OUT_OF_RANGE);
		return;
	}

	len <<= 1;

	if (len != task->task_expected_xfer_length) {
		sbd_ats_handling_after_io(sl, ats_handle);
		stmf_scsilib_send_status(task, STATUS_CHECK,
		    STMF_SAA_INVALID_FIELD_IN_CDB);
		return;
	}

	if ((initial_dbuf != NULL) && (task->task_flags & TF_INITIAL_BURST)) {
		if (initial_dbuf->db_data_size > len) {
			if (initial_dbuf->db_data_size >
			    task->task_expected_xfer_length) {
				/* protocol error */
				sbd_ats_handling_after_io(sl, ats_handle);
				stmf_abort(STMF_QUEUE_TASK_ABORT, task,
				    STMF_INVALID_ARG, NULL);
				return;
			}
			ASSERT(len <= 0xFFFFFFFFull);
			initial_dbuf->db_data_size = (uint32_t)len;
		}
		do_immediate_data = 1;
	}
	dbuf = initial_dbuf;

	if (task->task_lu_private) {
		scmd = (sbd_cmd_t *)task->task_lu_private;
	} else {
		scmd = (sbd_cmd_t *)kmem_alloc(sizeof (sbd_cmd_t), KM_SLEEP);
		task->task_lu_private = scmd;
	}

	/* We dont set the ATS_RELATED flag here */
	scmd->flags = SBD_SCSI_CMD_ACTIVE | SBD_SCSI_CMD_TRANS_DATA;
	scmd->cmd_type = SBD_CMD_SCSI_WRITE;
	scmd->nbufs = 0;
	ASSERT(len <= 0xFFFFFFFFull);
	scmd->len = (uint32_t)len;
	scmd->trans_data_len = (uint32_t)len;
	scmd->trans_data = kmem_alloc((size_t)len, KM_SLEEP);
	scmd->current_ro = 0;
	scmd->ats_state = ats_handle;

	if (do_immediate_data) {
		/*
		 * Account for data passed in this write command
		 */
		(void) stmf_xfer_data(task, dbuf, STMF_IOF_STATS_ONLY);
		atomic_add_32(&scmd->len, -dbuf->db_data_size);
		scmd->current_ro += dbuf->db_data_size;
		dbuf->db_xfer_status = STMF_SUCCESS;
		sbd_handle_ats_xfer_completion(task, scmd, dbuf, 0);
	} else {
		sbd_do_ats_xfer(task, scmd, dbuf, 0);
	}
}

/*
 * SCSI Copy Manager
 *
 * SCSI copy manager is the state machine which implements
 * SCSI extended copy functionality (SPC). There is one
 * cpmgr instance per extended copy command.
 *
 * Exported block-copy functions:
 *   cpmgr_create()  - Creates the state machine.
 *   cpmgr_destroy() - Cleans up a completed cpmgr.
 *   cpmgr_run()     - Performs time bound copy.
 *   cpmgr_abort()   - Aborts a cpmgr(if not already completed).
 *   cpmgr_done()    - Tests if the copy is done.
 */

static void cpmgr_completion_cleanup(cpmgr_t *cm);
int sbd_check_reservation_conflict(sbd_lu_t *sl, scsi_task_t *task);

static uint8_t sbd_recv_copy_results_op_params[] = {
    0, 0, 0, 42, 1, 0, 0, 0,
    0, 2, 0, 1, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0xFF, 0xFF, 0, 9, 0, 0, 0, 0, 0,
    2, 2, 0xE4
};

cpmgr_handle_t
cpmgr_create(scsi_task_t *task, uint8_t *params)
{
	cpmgr_t *cm = NULL;
	uint8_t *p;
	uint32_t plist_len;
	uint32_t dbl;
	int i;
	uint16_t tdlen;
	uint16_t n;

	cm = kmem_zalloc(sizeof (*cm), KM_NOSLEEP);
	if (cm == NULL)
		return (CPMGR_INVALID_HANDLE);

	cm->cm_task = task;
	p = task->task_cdb;
	plist_len = READ_SCSI32(&p[10], uint32_t);

	/*
	 * In case of error. Otherwise we will change this to CM_COPYING.
	 */
	cm->cm_state = CM_COMPLETE;

	if (plist_len == 0) {
		cm->cm_status = 0;
		goto cpmgr_create_done;
	}

	if (plist_len < CPMGR_PARAM_HDR_LEN) {
		cm->cm_status = CPMGR_PARAM_LIST_LEN_ERROR;
		goto cpmgr_create_done;
	} else if ((params[0] != 0) || ((params[1] & 0x18) != 0x18)) {
		/*
		 * Current implementation does not allow the use
		 * of list ID field.
		 */
		cm->cm_status = CPMGR_INVALID_FIELD_IN_PARAM_LIST;
		goto cpmgr_create_done;
	}
	/* No inline data either */
	if (*((uint32_t *)(&params[12])) != 0) {
		cm->cm_status = CPMGR_INVALID_FIELD_IN_PARAM_LIST;
		goto cpmgr_create_done;
	}

	tdlen = READ_SCSI16(&params[2], uint16_t);
	if ((tdlen == 0) || (tdlen % CPMGR_TARGET_DESCRIPTOR_SIZE) ||
	    (plist_len < (CPMGR_PARAM_HDR_LEN + tdlen))) {
		cm->cm_status = CPMGR_PARAM_LIST_LEN_ERROR;
		goto cpmgr_create_done;
	}
	cm->cm_td_count = tdlen / CPMGR_TARGET_DESCRIPTOR_SIZE;
	if (cm->cm_td_count > CPMGR_MAX_TARGET_DESCRIPTORS) {
		cm->cm_status = CPMGR_TOO_MANY_TARGET_DESCRIPTORS;
		goto cpmgr_create_done;
	}
	if (plist_len != (CPMGR_PARAM_HDR_LEN + tdlen +
	    CPMGR_B2B_SEGMENT_DESCRIPTOR_SIZE)) {
		cm->cm_status = CPMGR_PARAM_LIST_LEN_ERROR;
		goto cpmgr_create_done;
	}
	for (i = 0; i < cm->cm_td_count; i++) {
		p = params + CPMGR_PARAM_HDR_LEN;
		p += i * CPMGR_TARGET_DESCRIPTOR_SIZE;
		if ((p[0] != CPMGR_IDENT_TARGET_DESCRIPTOR) ||
		    ((p[5] & 0x30) != 0) || (p[7] != 16)) {
			cm->cm_status = CPMGR_UNSUPPORTED_TARGET_DESCRIPTOR;
			goto cpmgr_create_done;
		}
		/*
		 * stmf should be able to find this LU and lock it. Also
		 * make sure that is indeed a sbd lu.
		 */
		if (((cm->cm_tds[i].td_lu =
		    stmf_check_and_hold_lu(task, &p[8])) == NULL) ||
		    (!sbd_is_valid_lu(cm->cm_tds[i].td_lu))) {
			cm->cm_status = CPMGR_COPY_TARGET_NOT_REACHABLE;
			goto cpmgr_create_done;
		}
		dbl = p[29];
		dbl <<= 8;
		dbl |= p[30];
		dbl <<= 8;
		dbl |= p[31];
		cm->cm_tds[i].td_disk_block_len = dbl;
		cm->cm_tds[i].td_lbasize_shift =
		    sbd_get_lbasize_shift(cm->cm_tds[i].td_lu);
	}
	/* p now points to segment descriptor */
	p += CPMGR_TARGET_DESCRIPTOR_SIZE;

	if (p[0] != CPMGR_B2B_SEGMENT_DESCRIPTOR) {
		cm->cm_status = CPMGR_UNSUPPORTED_SEGMENT_DESCRIPTOR;
		goto cpmgr_create_done;
	}
	n = READ_SCSI16(&p[2], uint16_t);
	if (n != (CPMGR_B2B_SEGMENT_DESCRIPTOR_SIZE - 4)) {
		cm->cm_status = CPMGR_INVALID_FIELD_IN_PARAM_LIST;
		goto cpmgr_create_done;
	}

	n = READ_SCSI16(&p[4], uint16_t);
	if (n >= cm->cm_td_count) {
		cm->cm_status = CPMGR_INVALID_FIELD_IN_PARAM_LIST;
		goto cpmgr_create_done;
	}
	cm->cm_src_td_ndx = n;

	n = READ_SCSI16(&p[6], uint16_t);
	if (n >= cm->cm_td_count) {
		cm->cm_status = CPMGR_INVALID_FIELD_IN_PARAM_LIST;
		goto cpmgr_create_done;
	}
	cm->cm_dst_td_ndx = n;

	cm->cm_copy_size = READ_SCSI16(&p[10], uint64_t);
	cm->cm_copy_size *= (uint64_t)(cm->cm_tds[(p[1] & 2) ?
	    cm->cm_dst_td_ndx : cm->cm_src_td_ndx].td_disk_block_len);
	cm->cm_src_offset = (READ_SCSI64(&p[12], uint64_t)) <<
	    cm->cm_tds[cm->cm_src_td_ndx].td_lbasize_shift;
	cm->cm_dst_offset = (READ_SCSI64(&p[20], uint64_t)) <<
	    cm->cm_tds[cm->cm_dst_td_ndx].td_lbasize_shift;

	/* Allocate the xfer buffer. */
	cm->cm_xfer_buf = kmem_alloc(CPMGR_XFER_BUF_SIZE, KM_NOSLEEP);
	if (cm->cm_xfer_buf == NULL) {
		cm->cm_status = CPMGR_INSUFFICIENT_RESOURCES;
		goto cpmgr_create_done;
	}

	/*
	 * No need to check block limits. cpmgr_run() will
	 * take care of that.
	 */

	/* All checks passed */
	cm->cm_state = CM_COPYING;

cpmgr_create_done:
	if (cm->cm_state == CM_COMPLETE) {
		cpmgr_completion_cleanup(cm);
	}
	return (cm);
}

void
cpmgr_destroy(cpmgr_handle_t h)
{
	cpmgr_t *cm = (cpmgr_t *)h;

	ASSERT(cm->cm_state == CM_COMPLETE);
	kmem_free(cm, sizeof (*cm));
}

static void
cpmgr_completion_cleanup(cpmgr_t *cm)
{
	int i;

	for (i = 0; i < cm->cm_td_count; i++) {
		if (cm->cm_tds[i].td_lu) {
			stmf_release_lu(cm->cm_tds[i].td_lu);
			cm->cm_tds[i].td_lu = NULL;
		}
	}
	if (cm->cm_xfer_buf) {
		kmem_free(cm->cm_xfer_buf, CPMGR_XFER_BUF_SIZE);
		cm->cm_xfer_buf = NULL;
	}
}

void
cpmgr_run(cpmgr_t *cm, clock_t preemption_point)
{
	stmf_lu_t *lu;
	sbd_lu_t *src_slu, *dst_slu;
	uint64_t xfer_size, start, end;
	sbd_status_t ret;

	/*
	 * XXX: Handle reservations and read-only LU here.
	 */
	ASSERT(cm->cm_state == CM_COPYING);
	lu = cm->cm_tds[cm->cm_src_td_ndx].td_lu;
	src_slu = (sbd_lu_t *)lu->lu_provider_private;
	if (sbd_check_reservation_conflict(src_slu, cm->cm_task)) {
		cpmgr_abort(cm, CPMGR_RESERVATION_CONFLICT);
		return;
	}

	lu = cm->cm_tds[cm->cm_dst_td_ndx].td_lu;
	dst_slu = (sbd_lu_t *)lu->lu_provider_private;
	if (sbd_check_reservation_conflict(dst_slu, cm->cm_task)) {
		cpmgr_abort(cm, CPMGR_RESERVATION_CONFLICT);
		return;
	}
	if (dst_slu->sl_flags & SL_WRITE_PROTECTED) {
		cpmgr_abort(cm, STMF_SAA_WRITE_PROTECTED);
		return;
	}

	while (cm->cm_size_done < cm->cm_copy_size) {
		xfer_size = ((cm->cm_copy_size - cm->cm_size_done) >
		    CPMGR_XFER_BUF_SIZE) ? CPMGR_XFER_BUF_SIZE :
		    (cm->cm_copy_size - cm->cm_size_done);
		start = cm->cm_src_offset + cm->cm_size_done;
		ret = sbd_data_read(src_slu, cm->cm_task, start, xfer_size,
		    cm->cm_xfer_buf);
		if (ret != SBD_SUCCESS) {
			if (ret == SBD_IO_PAST_EOF) {
				cpmgr_abort(cm, CPMGR_LBA_OUT_OF_RANGE);
			} else {
				cpmgr_abort(cm,
				    CPMGR_THIRD_PARTY_DEVICE_FAILURE);
			}
			break;
		}
		end = cm->cm_dst_offset + cm->cm_size_done;
		ret = sbd_data_write(dst_slu, cm->cm_task, end, xfer_size,
		    cm->cm_xfer_buf);
		if (ret != SBD_SUCCESS) {
			if (ret == SBD_IO_PAST_EOF) {
				cpmgr_abort(cm, CPMGR_LBA_OUT_OF_RANGE);
			} else {
				cpmgr_abort(cm,
				    CPMGR_THIRD_PARTY_DEVICE_FAILURE);
			}
			break;
		}
		cm->cm_size_done += xfer_size;
		if (ddi_get_lbolt() >= preemption_point)
			break;
	}
	if (cm->cm_size_done == cm->cm_copy_size) {
		cm->cm_state = CM_COMPLETE;
		cm->cm_status = 0;
		cpmgr_completion_cleanup(cm);
	}
}

void
cpmgr_abort(cpmgr_t *cm, uint32_t s)
{
	if (cm->cm_state == CM_COPYING) {
		cm->cm_state = CM_COMPLETE;
		cm->cm_status = s;
		cpmgr_completion_cleanup(cm);
	}
}

void
sbd_handle_xcopy(scsi_task_t *task, stmf_data_buf_t *dbuf)
{
	uint32_t cmd_xfer_len;

	cmd_xfer_len = READ_SCSI32(&task->task_cdb[10], uint32_t);

	if (cmd_xfer_len == 0) {
		task->task_cmd_xfer_length = 0;
		if (task->task_additional_flags &
		    TASK_AF_NO_EXPECTED_XFER_LENGTH) {
			task->task_expected_xfer_length = 0;
		}
		stmf_scsilib_send_status(task, STATUS_GOOD, 0);
		return;
	}

	sbd_handle_short_write_transfers(task, dbuf, cmd_xfer_len);
}

void
sbd_handle_xcopy_xfer(scsi_task_t *task, uint8_t *buf)
{
	cpmgr_handle_t h;
	uint32_t s;
	clock_t tic, end;

	/*
	 * No need to pass buf size. Its taken from cdb.
	 */
	h = cpmgr_create(task, buf);
	if (h == CPMGR_INVALID_HANDLE) {
		stmf_scsilib_send_status(task, STATUS_CHECK,
		    CPMGR_INSUFFICIENT_RESOURCES);
		return;
	}
	tic = drv_usectohz(1000000);
	end = ddi_get_lbolt() + (CPMGR_DEFAULT_TIMEOUT * tic);
	while (!cpmgr_done(h)) {
		if (stmf_is_task_being_aborted(task) || (ddi_get_lbolt() > end))
			cpmgr_abort(h, CPMGR_THIRD_PARTY_DEVICE_FAILURE);
		else
			cpmgr_run(h, ddi_get_lbolt() + tic);
	}
	s = cpmgr_status(h);
	if (s) {
		if (s == CPMGR_RESERVATION_CONFLICT) {
			stmf_scsilib_send_status(task,
			    STATUS_RESERVATION_CONFLICT, 0);
		} else {
			stmf_scsilib_send_status(task, STATUS_CHECK, s);
		}
	} else {
		stmf_scsilib_send_status(task, STATUS_GOOD, 0);
	}
	cpmgr_destroy(h);
}

void
sbd_handle_recv_copy_results(struct scsi_task *task,
    struct stmf_data_buf *initial_dbuf)
{
	uint32_t cdb_len;

	cdb_len = READ_SCSI32(&task->task_cdb[10], uint32_t);
	if ((task->task_cdb[1] & 0x1F) != 3) {
		stmf_scsilib_send_status(task, STATUS_CHECK,
		    STMF_SAA_INVALID_FIELD_IN_CDB);
		return;
	}
	sbd_handle_short_read_transfers(task, initial_dbuf,
	    sbd_recv_copy_results_op_params, cdb_len,
	    sizeof (sbd_recv_copy_results_op_params));
}
