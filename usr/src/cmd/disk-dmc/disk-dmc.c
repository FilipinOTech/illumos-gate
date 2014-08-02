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
 * Copyright 2012 Nexenta Systems, Inc.  All rights reserved.
 */

#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <stddef.h>
#include <strings.h>
#include <sys/list.h>
#include <libdevinfo.h>
#include <sys/stat.h>
#include <sys/scsi/impl/uscsi.h>
#include <sys/scsi/generic/inquiry.h>
#include <fcntl.h>
#include <sys/scsi/scsi.h>
#include <assert.h>
#include <libdiskmgt.h>
#include <libintl.h>

#define	VERSION "1.1"

#define	DISK_DRIVER "sd"

#define	DEFAULT_WAIT 5

/* will set timeout to 30 sec for all USCSI commands that we will issue */
#define	DISK_CMD_TIMEOUT 30

#define	IMPOSSIBLE_RQ_STATUS 0xff

/* length bigger then this can not be represented by 3 bytes */
#define	MAX_FW_SIZE_IN_BYTES 16777215

/* 10 is strlen("/dev/rdsk/") */
#define	CTD_START_IN_PATH(path) (path + 10)

#define	MAX_DISK_LEN 9 /* enough space for a million disk names: sd0-sd999999 */
#define	INQ_VENDOR_LEN 9 /* struct scsi_inquiry defines these fields at: */
#define	INQ_MODEL_LEN 17 /* 8, 16 and 4 chars */
#define	INQ_REV_LEN 5 /* without string terminating zeros */

/* to reduce clutter and fprintf length */
#define	_(x) (gettext(x))

/* struct to hold relevant disk info */
typedef struct disk_info {
	list_node_t link;
	char device[MAX_DISK_LEN];
	char vendor[INQ_VENDOR_LEN];
	char model[INQ_MODEL_LEN];
	char rev[INQ_REV_LEN];
	char path[MAXPATHLEN];
	boolean_t inuse;
	int bad_info;
} disk_info_t;

/* prints sense data and status */
void
print_status_and_sense(char *cmd, int rc, int status,
		struct scsi_extended_sense *sense)
{
	(void) fprintf(stdout, _("%s ioctl() returned: %d status: 0x%02x, sense - "
	    "skey: 0x%02x, asc: 0x%02x, ascq: 0x%02x\n"), cmd, rc,
		status & STATUS_MASK, sense->es_key, sense->es_add_code,
		sense->es_qual_code);
}

/* determine if the uscsi cmd failed or not */
boolean_t
uscsi_parse_status(struct uscsi_cmd *ucmd, int rc, boolean_t verbose)
{
	struct scsi_extended_sense *sense;

	if (rc == -1 && errno == EAGAIN) {
		if (verbose)
			(void) fprintf(stderr, _("Disk is temporarily unavailable.\n"));
		return (B_FALSE); /* unavailable */
	}

	if ((ucmd->uscsi_status & STATUS_MASK) == STATUS_RESERVATION_CONFLICT) {
		if (verbose)
			(void) fprintf(stderr, _("Disk is reserved.\n"));
		return (B_FALSE); /* reserved by another system */
	}

	if (rc == -1 && ucmd->uscsi_status == 0 && errno == EIO) {
		if (verbose)
			(void) fprintf(stderr, _("Disk is unavailable.\n"));
		return (B_FALSE); /* unavailable */
	}

	/* check if we have valid sense status */
	if (ucmd->uscsi_rqstatus == IMPOSSIBLE_RQ_STATUS) {
		if (verbose) {
			(void) fprintf(stderr, _("No sense data for command 0x%02x.\n"),
			    ucmd->uscsi_cdb[0]);
		}
		return (B_FALSE);
	}

	if (ucmd->uscsi_rqstatus != STATUS_GOOD) {
		if (verbose) {
			(void) fprintf(stderr, _("Sense status for command 0x%02x: "
			    "0x%02x.\n"), ucmd->uscsi_cdb[0], ucmd->uscsi_rqstatus);
		}
		return (B_FALSE);
	}

	sense = (struct scsi_extended_sense *)ucmd->uscsi_rqbuf;
	if ((sense->es_key != KEY_RECOVERABLE_ERROR) &&
			(sense->es_key != KEY_NO_SENSE)) {
		if (verbose && sense->es_key == KEY_ILLEGAL_REQUEST &&
				sense->es_add_code == 0x2C && sense->es_qual_code == 0x0) {
			(void) fprintf(stderr, _(" Illegal Request - Command 0x%02x sequence "
			    "error.\n"), ucmd->uscsi_cdb[0]);
		} else if (verbose && sense->es_key == KEY_ILLEGAL_REQUEST &&
				sense->es_add_code == 0x24 && sense->es_qual_code == 0x0) {
			(void) fprintf(stderr, _(" cmd 0x%02x: Illegal Request - Invalid "
			    "field in CDB for  command.\n"), ucmd->uscsi_cdb[0]);
		} else if (verbose && sense->es_key == KEY_UNIT_ATTENTION &&
				sense->es_add_code == 0x3F && sense->es_qual_code == 0x01) {
			(void) fprintf(stderr, _(" cmd 0x%02x: Unit Attention - Microcode "
			    "changed.\n"), ucmd->uscsi_cdb[0]);
		} else if (verbose && sense->es_key == KEY_UNIT_ATTENTION &&
				sense->es_add_code == 0x29 && sense->es_qual_code == 0x0) {
			(void) fprintf(stderr, _(" cmd 0x%02x: Unit Attention - Reset "
			    "Occurred.\n"), ucmd->uscsi_cdb[0]);
		} else if (verbose) {
			(void) fprintf(stderr, _("Command 0x%02x produced sense data that "
			    "indicated an error.\n"), ucmd->uscsi_cdb[0]);
		}
		return (B_FALSE);
	}

	if (rc == -1 && errno == EIO) {
		if (verbose) {
			(void) fprintf(stderr, _("Command 0x%02x resulted in I/O error.\n"),
			    ucmd->uscsi_cdb[0]);
		}
		return (B_FALSE);
	}

	if (rc != 0) {
		if (verbose) {
			(void) fprintf(stderr, _("cmd 0x%02x: Unknown error.\n"),
			    ucmd->uscsi_cdb[0]);
		}
		return (B_FALSE);
	}

	if (verbose) {
		(void) fprintf(stderr, _("USCSI command 0x%02x completed "
		    "successfully.\n"), ucmd->uscsi_cdb[0]);
	}
	return (B_TRUE);
}

/* dump raw hex cdb */
void
print_cdb(union scsi_cdb *cdb, const char *cmd, int cdb_len)
{
	int i;

	(void) fprintf(stderr, "\n%s CDB (hex): ", cmd);
	for (i = 0; i < cdb_len; i++)
		(void) fprintf(stderr, "%02x ", cdb->cdb_opaque[i]);
	(void) fprintf(stderr, "\n");
}

/* will write microcode located in fw_img to the disk_fd */
boolean_t
uscsi_write_buffer_dmc(int disk_fd, uint8_t mode, void * fw_img,
		size_t fw_len, struct scsi_extended_sense *sense, boolean_t verbose)
{
	struct uscsi_cmd ucmd;
	union scsi_cdb cdb;
	int rc;

	(void) memset(&ucmd, 0, sizeof (struct uscsi_cmd));
	(void) memset(sense, 0, sizeof (struct scsi_extended_sense));
	(void) memset(&cdb, 0, sizeof (union scsi_cdb));

	cdb.scc_cmd = SCMD_WRITE_BUFFER;
	/* bits 0-4 (inclusive) of byte 1 contains mode field */
	cdb.cdb_opaque[1] = (mode & 0x1f);
	/* bytes 6-8 contain fw file length */
	cdb.cdb_opaque[6] = (uint8_t)((fw_len >> 16) & 0xff);
	cdb.cdb_opaque[7] = (uint8_t)((fw_len >> 8) & 0xff);
	cdb.cdb_opaque[8] = (uint8_t)(fw_len & 0xff);

	ucmd.uscsi_cdb = (caddr_t)&cdb;
	ucmd.uscsi_cdblen = CDB_GROUP1;
	ucmd.uscsi_flags = USCSI_SILENT | USCSI_WRITE | USCSI_ISOLATE;
	ucmd.uscsi_flags |= USCSI_RQENABLE | USCSI_DIAGNOSE;
	ucmd.uscsi_bufaddr = fw_img;
	ucmd.uscsi_buflen = fw_len;
	ucmd.uscsi_timeout = DISK_CMD_TIMEOUT;
	ucmd.uscsi_rqbuf = (caddr_t)sense;
	ucmd.uscsi_rqlen = sizeof (struct  scsi_extended_sense);
	ucmd.uscsi_rqstatus = IMPOSSIBLE_RQ_STATUS;

	if (verbose)
		print_cdb(&cdb, "write buffer", CDB_GROUP1);

	rc = ioctl(disk_fd, USCSICMD, &ucmd);
	if (verbose)
		print_status_and_sense("write buf", rc, ucmd.uscsi_status, sense);

	rc = uscsi_parse_status(&ucmd, rc, verbose);

	if (sense->es_key == KEY_ILLEGAL_REQUEST && sense->es_add_code == 0x2C &&
			sense->es_qual_code == 0x0) {
		(void) fprintf(stderr, _(" Downloading of microcode has failed - "
		    "Command sequence error.\n"));
		rc = B_FALSE;
	} else if (sense->es_key == KEY_ILLEGAL_REQUEST &&
			sense->es_add_code == 0x24 && sense->es_qual_code == 0x0) {
		(void) fprintf(stderr, _(" Downloading of microcode has failed - "
		    "Invalid field in CDB.\n"));
		rc = B_FALSE;
	} else if (sense->es_key == KEY_UNIT_ATTENTION &&
			sense->es_add_code == 0x3F && sense->es_qual_code == 0x01) {
		(void) fprintf(stderr, _(" Microcode download successful\n"));
		rc = B_TRUE;
	}

	return (rc);
}

/* Issue TUR command to device identified by file descriptor disk_fd */
boolean_t
uscsi_test_unit_ready(int disk_fd, struct scsi_extended_sense *sense,
		boolean_t verbose)
{
	struct uscsi_cmd ucmd;
	int rc;
	union scsi_cdb cdb;

	(void) memset(&ucmd, 0, sizeof (struct uscsi_cmd));
	(void) memset(sense, 0, sizeof (struct scsi_extended_sense));
	(void) memset(&cdb, 0, sizeof (union scsi_cdb));

	cdb.scc_cmd = SCMD_TEST_UNIT_READY;

	ucmd.uscsi_cdb = (caddr_t)&cdb;
	ucmd.uscsi_cdblen = CDB_GROUP0;
	ucmd.uscsi_flags = USCSI_SILENT | USCSI_DIAGNOSE | USCSI_RQENABLE;
	ucmd.uscsi_timeout = DISK_CMD_TIMEOUT;
	ucmd.uscsi_rqbuf = (caddr_t)sense;
	ucmd.uscsi_rqlen = sizeof (struct scsi_extended_sense);
	ucmd.uscsi_rqstatus = IMPOSSIBLE_RQ_STATUS;

	if (verbose)
	    print_cdb(&cdb, "test unit ready", CDB_GROUP0);

	rc = ioctl(disk_fd, USCSICMD, &ucmd);
	if (verbose)
		print_status_and_sense("test unit ready", rc, ucmd.uscsi_status, sense);

	return (uscsi_parse_status(&ucmd, rc, verbose));
}

/* Execute a uscsi inquiry command and put the resulting data into inqbuf. */
boolean_t
uscsi_inquiry(int disk_fd, struct scsi_inquiry *inqbuf,
		struct scsi_extended_sense *sense, boolean_t verbose)
{
	struct uscsi_cmd ucmd;
	union scsi_cdb cdb;
	int rc;

	(void) memset(inqbuf, 0, sizeof (struct scsi_inquiry));
	(void) memset(sense, 0, sizeof (struct scsi_extended_sense));
	(void) memset(&ucmd, 0, sizeof (ucmd));
	(void) memset(&cdb, 0, sizeof (union scsi_cdb));

	cdb.scc_cmd = SCMD_INQUIRY;
	/* bytes 3-4 contain data-in buf len */
	cdb.cdb_opaque[3] = (uint8_t)((sizeof (struct scsi_inquiry) >> 8) & 0xff);
	cdb.cdb_opaque[4] = (uint8_t)((sizeof (struct scsi_inquiry)) & 0xff);

	ucmd.uscsi_cdb = (caddr_t)&cdb;
	ucmd.uscsi_cdblen = CDB_GROUP0;
	ucmd.uscsi_flags = USCSI_READ | USCSI_SILENT | USCSI_ISOLATE;
	ucmd.uscsi_flags |= USCSI_RQENABLE | USCSI_DIAGNOSE;
	ucmd.uscsi_bufaddr = (caddr_t)inqbuf;
	ucmd.uscsi_buflen = sizeof (struct scsi_inquiry);
	ucmd.uscsi_timeout = DISK_CMD_TIMEOUT;
	ucmd.uscsi_rqbuf = (caddr_t)sense;
	ucmd.uscsi_rqlen = sizeof (struct scsi_extended_sense);
	ucmd.uscsi_rqstatus = IMPOSSIBLE_RQ_STATUS;

	if (verbose)
		print_cdb(&cdb, "inquiry", CDB_GROUP1);

	rc = ioctl(disk_fd, USCSICMD, &ucmd);
	if (verbose)
		print_status_and_sense("inquiry", rc, ucmd.uscsi_status, sense);

	return (uscsi_parse_status(&ucmd, rc, verbose));
}

/* Issue start command to device identified by file descriptor disk_fd */
boolean_t
uscsi_start_unit(int disk_fd, struct scsi_extended_sense *sense,
    boolean_t verbose)
{
	struct uscsi_cmd ucmd;
	union scsi_cdb cdb;
	int rc;

	(void) memset(&ucmd, 0, sizeof (struct uscsi_cmd));
	(void) memset(&cdb, 0, sizeof (union scsi_cdb));
	(void) memset(sense, 0, sizeof (struct scsi_extended_sense));

	cdb.scc_cmd = SCMD_START_STOP;
	/* bit 0 of byte 4 - start bit value. bits 4-7 - power condition field */
	cdb.cdb_opaque[4] = 0x01;

	ucmd.uscsi_cdb = (caddr_t)&cdb;
	ucmd.uscsi_cdblen = CDB_GROUP0;
	ucmd.uscsi_flags = USCSI_SILENT | USCSI_DIAGNOSE | USCSI_RQENABLE;
	ucmd.uscsi_flags |= USCSI_ISOLATE;
	ucmd.uscsi_timeout = DISK_CMD_TIMEOUT;
	ucmd.uscsi_rqbuf = (caddr_t)sense;
	ucmd.uscsi_rqlen = sizeof (struct scsi_extended_sense);
	ucmd.uscsi_rqstatus = IMPOSSIBLE_RQ_STATUS;

	if (verbose)
		print_cdb(&cdb, "start", CDB_GROUP0);

	rc = ioctl(disk_fd, USCSICMD, &ucmd);
	if (verbose)
		print_status_and_sense("start", rc, ucmd.uscsi_status, sense);

	return (uscsi_parse_status(&ucmd, rc, verbose));
}

/* copy chars to out without trailing and leading "space" chars */
void
mem_trim_and_cpy(char *out, const char *buf, size_t buf_len) {

	while (buf_len != 0 && isspace(*buf) != 0) { /* ignore leading space(s) */
		buf++;
		buf_len--;
	}
	while (buf_len && isspace(buf[buf_len - 1]))
		buf_len--; /* ignore trailing space(s) */

	(void) memcpy(out, buf, buf_len);
	out[buf_len] = 0; /* stringify */
}

/* given a disk - check if it's in use */
/* define NOINUSE_CHECK environment variable to turn of disk inuse check. */
void
set_disk_inuse(disk_info_t *disk, boolean_t verbose)
{
	char *msg, *slice, *character;
	char dev[MAXPATHLEN];
	int error = 0;
	int i;
	dm_descriptor_t	*slices = NULL;
	/*
	 * Let libdiskmgt think we are the format consumer, meaning slices will not
	 * be considered "inuse" if they have a filesystem or an exported zpool
	 */
	dm_who_type_t who = DM_WHO_ZPOOL;

	/* need to give dm_get_slices() a "whole" disk name, ie - c#t...d#p0 */
	(void) strncpy(dev, CTD_START_IN_PATH(disk->path), MAXPATHLEN);
	if ((character = strrchr(dev, 'd')) != NULL) {
		character++;
		while (isdigit(*character))
			character++;
		*character = 0; /* terminate the string after d# */
	}

	disk->inuse = B_FALSE;
	dm_get_slices(dev, &slices, &error);
	if (error != 0) {
		if (verbose) {
			(void) fprintf(stderr, _("dm_get_slices() failed: %s.\n Marking disk "
			    "inuse.\n"), strerror(error));
		}
		/*
		 * Marking disk as "in use" on errors since it's
		 * better to not touch a disk that MAY be in use by something.
		 */
		disk->inuse = B_TRUE;
		return;
	}

	/* loop through slices - set inuse */
	for (i = 0; slices[i]; i++) {
		slice = dm_get_name(slices[i], &error);
		if (dm_inuse(slice, &msg, who, &error) != 0 || error != 0) {
			/*
			 * Marking disk as "in use" even if we got here by way of
			 * (error != 0) since it's better to not touch a disk that MAY
			 * be in use by something. User can still use the disk by defining
			 * the NOINUSE_CHECK environment variable which will skip
			 * "in use" checking.
			 */
			disk->inuse = B_TRUE;
			if (error == 0) {
				if (verbose) {
					(void) fprintf(stderr, _("Disk '%s' is in use: %s"),
					    disk->device, msg);
				}
				free(msg);
			} else if (verbose) {
				(void) fprintf(stderr, _("dm_inuse() failed: %s.\n"
				    "Marking disk inuse.\n"), strerror(error));
			}
			dm_free_name(slice);
			break;
		}
		dm_free_name(slice);
	}
	dm_free_descriptors(slices);
}

/* obtain devlink (c*d*t* path) from /device path */
int
devlink_walk_cb(di_devlink_t devlink, void *arg)
{
	const char *path;
	if ((path = di_devlink_path(devlink)) != NULL) {
		assert(strlen(path) < MAXPATHLEN);
		(void) strncpy(arg, path, strlen(path)+1);
	} else {
		(void) strcpy(arg, "unknown_device_path");
	}

	return (DI_WALK_TERMINATE);
}

/*  get relevant disk info for char devices */
boolean_t
set_disk_info(const di_node_t *node, disk_info_t *disk, boolean_t verbose)
{
	int instance;
	char *m_path = NULL;
	di_minor_t di_minor;
	di_devlink_handle_t devlink_h;
	struct scsi_inquiry inq;
	int disk_fd;
	struct scsi_extended_sense sense;

	disk->bad_info = 1;

	/* populate device field with sd */
	assert(MAX_DISK_LEN > strlen(DISK_DRIVER) + sizeof (int) + 1);
	(void) strncpy(disk->device, DISK_DRIVER, strlen(DISK_DRIVER)+1);

	instance = di_instance(*node);
	if (instance == -1) {
		(void) fprintf(stderr, _("Could not get the instance number of the "
		    "device - di_instance() failed\n"));
		(void) strcpy(disk->device + strlen(DISK_DRIVER), "?");
		disk->bad_info = -1;
		return (B_FALSE);
	}

	/* append instance # to device name making sd# */
	(void) snprintf(disk->device + strlen(DISK_DRIVER), MAX_DISK_LEN -
			strlen(DISK_DRIVER), "%d", instance);

	/* take a snapshot of devlinks bound to sd driver */
	if ((devlink_h = di_devlink_init(DISK_DRIVER, DI_MAKE_LINK)) ==
		    DI_LINK_NIL) {
		(void) fprintf(stderr, _("di_link_init() failed for disk %s.\n"),
		    disk->device);
		return (B_FALSE);
	}
	/* traverse devlink minor nodes */
	di_minor = di_minor_next(*node, DI_MINOR_NIL);
	for (; di_minor != DI_MINOR_NIL;
		di_minor = di_minor_next(*node, di_minor)) {
		if (di_minor_spectype(di_minor) != S_IFCHR)
			continue; /* skip minor nodes that are not char devs */

		/* phys path to minor node (/devices/...) */
		if ((m_path = di_devfs_minor_path(di_minor)) == NULL) {
			(void)  fprintf(stderr, _("couldn't get path for a minor node of "
			    "disk '%s' - di_devfs_minor_path() failed.\n"), disk->device);
			break;
		}
		if (strstr(m_path, ":q,raw") == NULL) {
			di_devfs_path_free(m_path);
			continue; /* skips minor nodes that are not slice 0 */
		}

		/*
		 * Lookup /dev/rdsk/ path for minor node defined by m_path
		 * The reason we're not just using the /devices m_path for WRITE_BUFFER
		 * is we need the c#t...d#p0 lun to display to the user anyway
		 */
		if (di_devlink_walk(devlink_h, "^rdsk/", m_path, DI_PRIMARY_LINK,
				disk->path, devlink_walk_cb) == -1) {
			(void) fprintf(stderr, _("di_devlink_walk() failed for disk "
			    "'%s'\n"), disk->device);
			di_devfs_path_free(m_path);
			break;
		}
		di_devfs_path_free(m_path);

		/* check that the devlink (/dev/rdsk/) path was found by the above */
		if (strcmp(disk->path, "unknown_device_path") == 0) {
			(void) fprintf(stderr, _("di_devlink_path() for '%s' failed"),
			    disk->device);
			break;
		}

		disk_fd = open(disk->path, O_RDONLY | O_NDELAY);
		if (disk_fd < 0) {
			(void) fprintf(stderr, _("open() on disk '%s', dev path '%s' "
			    "failed: %s\n"), disk->device, disk->path, strerror(errno));
			break;
		}

		/* found p0 for a char disk, now get vendor, model, and rev */
		if (uscsi_inquiry(disk_fd, &inq, &sense, verbose)) {
			disk->bad_info = 0;
			mem_trim_and_cpy(disk->vendor, inq.inq_vid, sizeof (inq.inq_vid));
			mem_trim_and_cpy(disk->model, inq.inq_pid, sizeof (inq.inq_pid));
			mem_trim_and_cpy(disk->rev, inq.inq_revision,
					sizeof (inq.inq_revision));
			set_disk_inuse(disk, verbose);
		} else {
			if (verbose) {
				(void) fprintf(stderr, _("uscsi_inquiry() failed, disk '%s' will "
				    "be skipped\n"), disk->device);
			}
		}
		(void) close(disk_fd);
		break; /* have found slice 0 for a char device, can now exit */
	}
	(void) di_devlink_fini(&devlink_h);

	return (disk->bad_info == 0 ? B_TRUE : B_FALSE);
}

/* kernel device tree walk */
int
walk_nodes(di_node_t *root_node, list_t *disks, boolean_t verbose)
{
	int *dtype;
	int disks_found = 0;
	disk_info_t *disk;
	di_node_t node;

	(void) fprintf(stdout, _("Searching for disks, found: "));
	/* start at root node and walk all sd nodes */
	node = di_drv_first_node(DISK_DRIVER, *root_node);
	for (; node != DI_NODE_NIL; node = di_drv_next_node(node)) {
		(void) fflush(stdout);
		if (di_prop_lookup_ints(DDI_DEV_T_ANY, node, "inquiry-device-type",
				&dtype) >= 0) {
			if (((*dtype) & DTYPE_MASK) != DTYPE_DIRECT)
				continue; /* skip dev types that are not type 0 (not disks) */
		} else {
			if (verbose) {
				(void) fprintf(stderr, _("di_prop_lookup_ints(inquiry-device-type) "
				    "failed, ignoring this node\n"));
			}
			continue;
		}
		disks_found++;
		if ((disk = (disk_info_t *) malloc(sizeof (disk_info_t))) == NULL) {
			if (verbose) {
				(void)  fprintf(stderr, _("malloc(%d) failed\n"),
				    sizeof (disk_info_t));
			}
			return (-disks_found);
		}
		list_insert_tail(disks, disk); /* preserves discovery order */
		if (set_disk_info(&node, disk, verbose) || verbose) {
			(void) fprintf(stdout, "%s ", disk->device);
			if ((disks_found % 10) == 0)
				(void) fprintf(stdout, "\n");
		} else {
			disks_found--;
		}
	}
	(void) fprintf(stdout, _("(%d total).\n"), disks_found);

	return (disks_found);
}

/* allocate space and read into it the fw image */
char *
get_fw_image(const char *fw_file, size_t *fw_len, boolean_t verbose)
{
	struct stat stat_buf;
	char *fw_buf = NULL;
	FILE * fw_stream = fopen(fw_file, "r");

	*fw_len = 0;
	if (fw_stream != NULL) {
		if (fstat(fileno(fw_stream), &stat_buf) == -1) { /* fstat failed */
			if (verbose) {
				(void) fprintf(stderr, _("fstat() on file '%s' failed\n"),
				    fw_file);
			}
		} else if (stat_buf.st_size < 1 ||
		    stat_buf.st_size > MAX_FW_SIZE_IN_BYTES) {
			/* fw file empty or too big to fit into 1 SCSI WRITEBUFFER buffer */
			(void) fprintf(stderr, _("'%s' is of size %lld bytes.\ndisk-dmc v"
			    VERSION " does not support fw files bigger then %d bytes or "
				"smaller then 1 byte.\n"), fw_file, (intmax_t) stat_buf.st_size,
				MAX_FW_SIZE_IN_BYTES);
		} else { /* alloc buffer to hold fw img */
			if ((fw_buf = malloc(stat_buf.st_size)) != NULL)
				*fw_len = stat_buf.st_size;
			else if (verbose)
				(void) fprintf(stderr, _("malloc() failed\n"));
		}
	} else { /* fopen failed */
		if (verbose) {
			(void) fprintf(stderr, _("fopen() on fw image '%s' failed: %s\n"),
			    fw_file, strerror(errno));
		}
	}

	if (fw_buf != NULL && fread(fw_buf, 1, *fw_len, fw_stream) != *fw_len) {
		free(fw_buf);
		fw_buf = NULL;
		*fw_len = 0;
		if (verbose)
			(void) fprintf(stderr, _("fread() failed\n"));
	}

	if (fw_stream != NULL)
		(void) fclose(fw_stream);

	return (fw_buf);
}

/* print a dot for every second we wait as to not look like process is hanging */
void
wait_print(int sec)
{
	(void) fprintf(stdout, _(" waiting for %d seconds:\n"), sec);
	while (sec--) {
		(void) fprintf(stdout, " .");
		(void) fflush(stdout);
		if (! (sec % 30))
			(void) fprintf(stdout, "\n");
		(void) sleep(1);
	}
}

/* perform some housekeeping before write buffer */
boolean_t
prep_disk_for_fw_dl(int disk_fd, int wait, boolean_t verbose)
{
	struct scsi_extended_sense sense;

	if (verbose)
		(void) fprintf(stderr, _("  before dl prep\n"));

	sync(); /* schedule in flight stuff to be put onto disk before dmc */

	if (uscsi_test_unit_ready(disk_fd, &sense, verbose)) {
		return (B_TRUE);
	} else {
		if (sense.es_key == KEY_NOT_READY && sense.es_add_code == 0x04 &&
				sense.es_qual_code == 0x02) {
			/* above sense data indicates disk needs a start command */
			(void) uscsi_start_unit(disk_fd, &sense, verbose);
			wait_print(wait);
		} else if (sense.es_key == KEY_UNIT_ATTENTION &&
				sense.es_add_code == 0x29 && sense.es_qual_code == 0x0) {
			wait_print(wait); /* reset occurred */
		} else if (sense.es_key == KEY_NOT_READY && sense.es_add_code == 0x04 &&
				sense.es_qual_code == 0x0) {
			wait_print(wait); /* becoming ready */
		}
	}

	return (uscsi_test_unit_ready(disk_fd, &sense, verbose)); /* trying again */
}

/* test disk after buffer write to make sure it's ready for operation */
boolean_t
after_dl_prep(int disk_fd, int wait, boolean_t verbose)
{
	struct scsi_extended_sense sense;

	if (verbose)
		(void) fprintf(stderr, _(" after dl prep\n"));
	wait_print(wait);	/* give disk time to become ready after dmc */

	if (uscsi_test_unit_ready(disk_fd, &sense, verbose)) {
		return (B_TRUE);
	} else {
		if (sense.es_key == KEY_NOT_READY && sense.es_add_code == 0x04 &&
				sense.es_qual_code == 0x02) {
			/* above sense data indicates disk needs a start command */
			(void) uscsi_start_unit(disk_fd, &sense, verbose);
			wait_print(wait);
		} else if (sense.es_key == KEY_UNIT_ATTENTION &&
				sense.es_add_code == 0x63 && sense.es_qual_code == 0x01) {
			wait_print(wait); /* microcode changed */
		} else if (sense.es_key == KEY_UNIT_ATTENTION &&
				sense.es_add_code == 0x29 && sense.es_qual_code == 0x0) {
			wait_print(wait); /* reset occurred */
		} else if (sense.es_key == KEY_NOT_READY && sense.es_add_code == 0x04 &&
				sense.es_qual_code == 0x0) {
			wait_print(wait); /* becoming ready */
		}
	}

	return (uscsi_test_unit_ready(disk_fd, &sense, verbose)); /* trying again */
}

/* read through fw image and look for model string */
boolean_t
match_fw_image_to_model(const char *fw_image, size_t fw_len, const char *model)
{
	size_t model_len = strlen(model);
	size_t limit = fw_len - model_len + 1;
	size_t i;

	assert(model_len > 0);
	for (i = 0; i < limit; i++)
		if (memcmp(fw_image + i, model, model_len) == 0)
			return (B_TRUE);

	return (B_FALSE);
}

/* check if the bad_info field is set, if so print the error */
boolean_t
has_bad_info(disk_info_t *disk, boolean_t verbose)
{
	if (disk->bad_info == -1) {
		if (verbose) {
			(void)  fprintf(stderr, _("Encountered a device without an "
			    "instance, it will be skipped.\n"));
		}
		return (B_TRUE);
	} else if (disk->bad_info == 1) {
		if (verbose) {
			(void) fprintf(stderr, _("Was not able to get all of the needed "
			    "info for disk '%s', it will be skipped.\n"), disk->device);
		}
		return (B_TRUE);
	}

	return (B_FALSE);
}

/* print info for a single disk entry */
void
print_disk(const disk_info_t *disk)
{
	(void) fprintf(stdout, "%-8s %-35s %-9s %-17s %6s\n",
	    disk->device, CTD_START_IN_PATH(disk->path), disk->vendor, disk->model,
	    disk->rev);
}

/* print header and info for all disks */
void
print_disks(list_t *disks, boolean_t verbose)
{
	disk_info_t *disk;

	(void) fprintf(stdout, "%-8s %-35s %-9s %-17s %6s\n"
	    "========================================"
	    "=======================================\n",
		"DEVICE", "LUN", "VENDOR", "MODEL", "FW REV");
	for (disk = list_head(disks); disk; disk = list_next(disks, disk))
		if (!has_bad_info(disk, verbose))
			print_disk(disk);
}

/* perform fw update on one disk */
boolean_t
process_disk(disk_info_t *disk, uint8_t mode, char * fw_buf,
		size_t fw_len, int wait, boolean_t ignore_mismatch, boolean_t verbose)
{
	int disk_fd;
	struct scsi_extended_sense sense;
	boolean_t status;
	struct scsi_inquiry inq;

	(void) fprintf(stdout, _("Processing disk %s:\n\n"), disk->device);
	status = match_fw_image_to_model(fw_buf, fw_len, disk->model);

	if (verbose) {
		(void) fprintf(stderr, _(" fw img matched to model %s? %s\n"),
		    disk->model, status ? "yes" : "no");
	}

	if (!ignore_mismatch && !status) {
		(void) fprintf(stdout, _("Could not match fw image to disk (%s) model "
		    "(%s), will not proceed without ignore mismatch (-i) "
		    "argument specified.\n"), disk->device, disk->model);
		return (B_FALSE);
	}

	if (disk->inuse) {
		(void) fprintf(stdout, _("Disk '%s' is in use, will not continue.\n"),
		    disk->device);
		return (B_FALSE);
	}

	disk_fd = open(disk->path, O_RDWR | O_NONBLOCK);
	if (disk_fd < 0) {
		(void) fprintf(stderr, _("open() on disk '%s' failed: %s\n"), disk->path,
		    strerror(errno));
		return (B_FALSE);
	}

	status = prep_disk_for_fw_dl(disk_fd, wait, verbose);
	if (verbose) {
		(void) fprintf(stderr, _(" Prep disk status: %s\n"),
		    status ? _("success") : _("failure"));
	}

	if (!status) {
		(void) close(disk_fd);
		return (B_FALSE);
	}

	/* write the fw onto the disk */
	status = uscsi_write_buffer_dmc(disk_fd, mode, fw_buf, fw_len, &sense,
			verbose);

	if (after_dl_prep(disk_fd, wait, verbose) && verbose) {
		(void) fprintf(stderr, _(" After fw download disk seems to be "
		    "online.\n"));
	} else if (verbose) {
		(void) fprintf(stderr, _(" After fw download disk doesn't seem to be "
		    "online.\n"));
	}

	(void) fprintf(stdout, _("\nFlashing %s firmware: %s\n"), disk->device,
	    status ? _("successful") : _("failed"));

	if (verbose) { /* get and display the "new" disk revision */
		if (uscsi_inquiry(disk_fd, &inq, &sense, verbose)) {
			mem_trim_and_cpy(disk->rev, inq.inq_revision,
					sizeof (inq.inq_revision));
			print_disk(disk);
		} else {
			(void) fprintf(stderr, _("uscsi_inquiry() failed on disk '%s', "
			    "can't get revision.\n"), disk->device);
		}
	}
	(void) close(disk_fd);

	return (status);
}

void
usage(const char * prog_name)
{
	(void) fprintf(stdout, _("%s v" VERSION "\n"
	    "\nUsage: %s <-d (c#t...d#p0 | sd#) | "
	    "-m model_string> <-p /path/to/fw/img> <-h> <-l> <-i> <-v> "
	    "<-w #sec>\n"
	    "\t-h\tPrint this help message and exit.\n"
	    "\t-l\tList discovered drives.\n"
	    "\t-d dsk\tSpecify single drive to use for firmware "
	    "download in c#t...d#p0 or sd# format.\n"
	    "\t-m str\tSpecify model of drive(s) to download "
	    "firmware to. Drives whose model exactly matches model str provided "
		"will be upgraded.\n\t-p img\tPath to the firmware file that will be "
	    "downloaded onto the specified drive(s).\n"
	    "\t-i\tIgnore drive and fw model mismatch.\n"
	    "\t-v\tVerbose mode: turns on extra debug messages.\n"
	    "\t-w #sec\tNumber of seconds to delay checking "
	    "for drive readiness after downloading the firmware. Also "
	    "used for drive preparation timeouts. Default is %d.\n"),
	    prog_name, prog_name, DEFAULT_WAIT);
	exit(1);
}

int
main(int argc, char *argv[])
{
	di_node_t root_node;
	int opt, i, wait = DEFAULT_WAIT;
	uint8_t mode = 0x05; /* download, save and activate microcode */
	boolean_t ctd = B_FALSE, list = B_FALSE, model = B_FALSE, verbose = B_FALSE;
	boolean_t ignore_mismatch = B_FALSE, user_wait = B_FALSE;
	char *fw_img = NULL;
	size_t fw_len;
	char dl_fw_to_disk[MAXPATHLEN];
	char dl_fw_to_model[INQ_MODEL_LEN];
	list_t disks; /* list of disk_info_t structures */
	disk_info_t *disk = NULL;

	if (geteuid() > 1) {
		(void) fprintf(stderr, _("This utility requires root level permissions "
			"due to the use of USCSI(7i).\n"));
		return (1);
	}

	/* if only verbose arg or no command line args received do a disk list */
	if (argc == 1 || (argc == 2 && strcmp("-v", argv[1]) == 0))
		list = B_TRUE;

	while ((opt = getopt(argc, argv, "d:p:hilm:vw:")) != -1) {
		switch (opt) {
		case 'd': /* single disk mode */
			if (strlen(optarg) < MAXPATHLEN) {
				(void) strncpy(dl_fw_to_disk, optarg, strlen(optarg)+1);
				ctd = B_TRUE;
			} else {
				(void) fprintf(stderr, _("disk name given to '-%c' is too "
				    "long (%d)\n"), optopt, MAXPATHLEN);
				usage(argv[0]);
			}
			break;
		case 'p': /* fw file */
			if (strlen(optarg) < MAXPATHLEN) {
				fw_img = get_fw_image(optarg, &fw_len, verbose);
				if (fw_img == NULL) {
					(void) fprintf(stderr, _("could not use fw file '%s', check"
					    " file size, permissions and/or existence.\n"), optarg);
					usage(argv[0]);
				}
			} else {
				(void) fprintf(stderr, _("fw path provided to '-%c' is too "
				    "long.\n"), optopt);
				usage(argv[0]);
			}
			break;
		case 'h': /* print usage and exit */
			usage(argv[0]);
			break;
		case 'i': /* ignore fw and disk model mismatch */
			ignore_mismatch = B_TRUE;
			break;
		case 'l': /* list drives */
			list = B_TRUE;
			break;
		case 'm': /* only flash this model */
			if (strlen(optarg) < INQ_MODEL_LEN) {
				(void) strncpy(dl_fw_to_model, optarg, strlen(optarg)+1);
				model = B_TRUE;
			} else {
				(void) fprintf(stderr, _("model provided to '-%c' is too long "
				    "(%d)\n"), optopt, MAXPATHLEN);
				usage(argv[0]);
			}
			break;
		case 'v': /* verbose mode */
			verbose = B_TRUE;
			break;
		case 'w': /* wait time after dl */
			wait = atoi(optarg);
			if (wait < 0 || wait > 300) {
				(void) fprintf(stderr, _("'-%c %d' - time to wait after "
				    "microcode download is too long ( > 5min ) or negative.\n"),
				    optopt, wait);
				usage(argv[0]);
			}
			user_wait = B_TRUE;
			break;
		case ':':
		case '?':
			usage(argv[0]);
			break;
		}
	}

	for (i = optind; i < argc; i++) {
		(void) printf(_("Unexpected argument '%s'\n"), argv[i]);
		usage(argv[0]);
	}

	if (fw_img && (!ctd && !model)) {
		/* if fw file is given, need to associate it with a disk or model */
		(void) fprintf(stderr, _("Expected a disk specification "
		    "('-m model' or '-d dsk') to associate fw file with.\n"));
		usage(argv[0]);
	}
	if (!fw_img && (ctd || model)) {
		/* if no fw file is given, having model or disk doesn't make sense */
		(void) fprintf(stderr, _("Expected a fw file ('-p /path/to/fw/file') to"
		    " associate '-m model' or '-d dsk' with.\n"));
		usage(argv[0]);
	}

	if (model && ctd) {
		/* can't have both disk and model for fw download specified */
		(void) fprintf(stderr, _("-m and -d are mutually exclusive.\n"));
		usage(argv[0]);
	}

	if ((ignore_mismatch || user_wait) && !fw_img) {
		/* need firmware to work with if above args are given */
		(void) fprintf(stderr, _("Expected '-p /path/to/fw.'\n"));
		usage(argv[0]);
	}

	/*
	 * create a snapshot of kernel device tree include minor nodes,
	 * properties and subtree
	 */
	if ((root_node = di_init("/", DINFOCPYALL)) == DI_NODE_NIL) {
		(void) fprintf(stderr, _("di_init() failed\n"));
		return (1);
	}

	list_create(&disks, sizeof (disk_info_t), offsetof(disk_info_t, link));
	i = walk_nodes(&root_node, &disks, verbose); /* walk device tree */
	di_fini(root_node);

	if (i == 0) {
		(void) fprintf(stdout, _("No disk(s) serviced by '" DISK_DRIVER "' "
		    "driver were found.\n)"));
		return (1);
	}
	if (i < 0) {
		(void) fprintf(stderr, _("malloc() failed so not all disk(s) serviced "
		    "by '" DISK_DRIVER "' driver were walked.\n"
		    "Encountered %d disk(s) before failure.\n"), -i);
		return (1);
	}
	if (verbose) {
		(void) fprintf(stderr, _("Found %d disk(s) serviced by '" DISK_DRIVER
		    "' driver.\n"), i);
	}

	if (list)
		print_disks(&disks, verbose);

	if (ctd) { /* if we were given a disk */
		for (disk = list_head(&disks); disk; disk = list_next(&disks, disk)) {
			if (has_bad_info(disk, verbose))
				continue;
			if (strcmp(CTD_START_IN_PATH(disk->path), dl_fw_to_disk) == 0 ||
					strcmp(disk->device, dl_fw_to_disk) == 0) {
				if (verbose) {
					(void) fprintf(stderr, _("matched provided disk '%s' to "
					    "'%s' or '%s'.\n"), dl_fw_to_disk,
					    CTD_START_IN_PATH(disk->path), disk->device);
				}
				break;
			} else if (verbose) {
				(void) fprintf(stderr, _("did not match provided disk '%s' to "
				    "'%s' or '%s'.\n"), dl_fw_to_disk,
				    CTD_START_IN_PATH(disk->path), disk->device);
			}
		}
		if (disk == NULL) {
			(void) fprintf(stdout, _("disk '%s' was not found.\n"),
		    dl_fw_to_disk);
		} else {
			(void) process_disk(disk, mode, fw_img, fw_len, wait,
			    ignore_mismatch, verbose);
		}
	}

	if (model) { /* if we were given a model */
		i = 0;
		for (disk = list_head(&disks); disk; disk = list_next(&disks, disk)) {
			if (has_bad_info(disk, verbose))
				continue;
			if (strcmp(dl_fw_to_model, disk->model) == 0) {
				i++;
				if (verbose) {
					(void) fprintf(stderr, _("matched provided model: '%s' to "
					    "disk '%s'.\n"), dl_fw_to_model, disk->device);
				}
				(void) process_disk(disk, mode, fw_img, fw_len, wait,
				    ignore_mismatch, verbose);
			} else if (verbose) {
				(void) fprintf(stderr, _("did not match provided model: '%s' to"
				    " disk '%s'.\n"), dl_fw_to_model, disk->device);
			}
		}
		if (i == 0) {
			(void) fprintf(stdout, _("disk model '%s' was not found.\n"),
			    dl_fw_to_model);
		}
	}

	/* cleanup start */
	if (fw_img)
		free(fw_img);
	while ((disk = (list_head(&disks))) != NULL) {
		list_remove(&disks, disk);
		free(disk);
	}
	list_destroy(&disks);

	return (0);
}
