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
 * Copyright 2013 Nexenta Systems, Inc.  All rights reserved.
 */

#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/cos_impl.h>
#include <sys/refcount.h>
#include <sys/dmu.h>
#include <sys/dsl_synctask.h>
#include <sys/zap.h>
#include <sys/zfeature.h>

#include <zfs_prop.h>

/*
 * Support for vdev-specific properties. Similar properties can be set
 * on per-pool basis; vdev-specific properties override per-pool ones.
 * In addition to path and fru, the supported vdev properties include
 * min_active/max_active (request queue length params), and preferred
 * read (biases reads toward this device if it is a part of a mirror).
 */

/*
 * Store vdev properties at offset 'offset' in object 'obj' in MOS
 */
static uint64_t
vdev_store_props(vdev_t *vdev, objset_t *mos, uint64_t obj, uint64_t offset,
    dmu_tx_t *tx)
{
	vdev_props_phys_hdr_t *vpph;
	char *packed = NULL;
	char *buf = NULL;
	size_t size = 0;
	size_t nvsize = 0;
	size_t bufsize = 0;
	nvlist_t *nvl;
	const char *propname;
	zio_priority_t p;

	if (!vdev->vdev_ops->vdev_op_leaf) {
		for (int c = 0; c < vdev->vdev_children; c++)
			size += vdev_store_props(vdev->vdev_child[c], mos, obj,
			    offset + size, tx);
		return (size);
	}

	VERIFY(nvlist_alloc(&nvl, NV_UNIQUE_NAME, KM_SLEEP) == 0);

	for (p = ZIO_PRIORITY_SYNC_READ; p < ZIO_PRIORITY_NUM_QUEUEABLE; p++) {
		uint64_t val = vdev->vdev_queue.vq_class[p].vqc_min_active;
		int prop_id = VDEV_ZIO_PRIO_TO_PROP_MIN(p);

		ASSERT(VDEV_PROP_MIN_VALID(prop_id));
		propname = vdev_prop_to_name(prop_id);
		VERIFY(nvlist_add_uint64(nvl, propname, val) == 0);

		val = vdev->vdev_queue.vq_class[p].vqc_max_active;
		prop_id = VDEV_ZIO_PRIO_TO_PROP_MAX(p);
		ASSERT(VDEV_PROP_MAX_VALID(prop_id));
		propname = vdev_prop_to_name(prop_id);
		VERIFY(nvlist_add_uint64(nvl, propname, val) == 0);
	}

	propname = vdev_prop_to_name(VDEV_PROP_PREFERRED_READ);
	VERIFY(0 == nvlist_add_uint64(nvl, propname,
		vdev->vdev_queue.vq_preferred_read));

	if (vdev->vdev_queue.vq_cos) {
		propname = vdev_prop_to_name(VDEV_PROP_COS);
		VERIFY(0 == nvlist_add_uint64(nvl, propname,
		    vdev->vdev_queue.vq_cos->cos_guid));
	}

	if (vdev->vdev_spare_group) {
		propname = vdev_prop_to_name(VDEV_PROP_SPAREGROUP);
		VERIFY(0 == nvlist_add_string(nvl, propname,
		    vdev->vdev_spare_group));
	}

	VERIFY(nvlist_size(nvl, &nvsize, NV_ENCODE_XDR) == 0);

	size = P2ROUNDUP(nvsize, 8);
	bufsize = sizeof (*vpph) + size;
	buf = kmem_alloc(bufsize, KM_SLEEP);
	vpph = (vdev_props_phys_hdr_t *)buf;
	packed = buf + sizeof (*vpph);
	if (size > nvsize)
		bzero(packed + nvsize, size - nvsize);
	VERIFY(nvlist_pack(nvl, &packed, &nvsize, NV_ENCODE_XDR,
		KM_SLEEP) == 0);

	vpph->vpph_guid = vdev->vdev_guid;
	vpph->vpph_nvsize = nvsize;
	vpph->vpph_size = bufsize;

	dmu_write(mos, obj, offset, bufsize, buf, tx);

	kmem_free(buf, bufsize);
	nvlist_free(nvl);

	return (bufsize);
}

/*
 * Get the properties from nvlist and put then in vdev object
 */
static void
vdev_parse_props(vdev_t *vdev, char *packed, uint64_t nvsize)
{
	uint64_t ival;
	nvlist_t *nvl;
	const char *propname;
	char *sval;
	int err;
	zio_priority_t p;

	ASSERT(vdev);

	if (!vdev->vdev_ops->vdev_op_leaf)
		return;

	if ((err = nvlist_unpack(packed, nvsize, &nvl, KM_SLEEP)) != 0) {
		cmn_err(CE_WARN, "Failed to unpack vdev props, err: %d\n", err);
		return;
	}

	for (p = ZIO_PRIORITY_SYNC_READ; p < ZIO_PRIORITY_NUM_QUEUEABLE; p++) {
		int prop_id = VDEV_ZIO_PRIO_TO_PROP_MIN(p);

		ASSERT(VDEV_PROP_MIN_VALID(prop_id));
		propname = vdev_prop_to_name(prop_id);

		if (nvlist_lookup_uint64(nvl, propname, &ival) == 0)
			vdev->vdev_queue.vq_class[p].vqc_min_active = ival;

		prop_id = VDEV_ZIO_PRIO_TO_PROP_MAX(p);
		ASSERT(VDEV_PROP_MAX_VALID(prop_id));
		propname = vdev_prop_to_name(prop_id);
		if (nvlist_lookup_uint64(nvl, propname, &ival) == 0)
			vdev->vdev_queue.vq_class[p].vqc_max_active = ival;
	}

	propname = vdev_prop_to_name(VDEV_PROP_PREFERRED_READ);
	if (nvlist_lookup_uint64(nvl, propname, &ival) == 0)
		vdev->vdev_queue.vq_preferred_read = ival;
	propname = vdev_prop_to_name(VDEV_PROP_COS);
	if (nvlist_lookup_uint64(nvl, propname, &ival) == 0) {
		/*
		 * At this time, all CoS properties have been loaded.
		 * Lookup CoS by guid and take it if found.
		 */
		cos_t *cos = NULL;
		spa_t *spa = vdev->vdev_spa;
		spa_cos_enter(spa);
		cos = spa_lookup_cos_by_guid(spa, ival);
		if (cos) {
			cos_hold(cos);
			vdev->vdev_queue.vq_cos = cos;
		} else {
			cmn_err(CE_WARN, "vdev %s refers to non-existent "
			    "CoS %" PRIu64 "\n", vdev->vdev_path, ival);
			vdev->vdev_queue.vq_cos = NULL;
		}
		spa_cos_exit(spa);
	}
	propname = vdev_prop_to_name(VDEV_PROP_SPAREGROUP);
	if (nvlist_lookup_string(nvl, propname, &sval) == 0)
		vdev->vdev_spare_group = spa_strdup(sval);

	nvlist_free(nvl);
}

/*
 * Get specific property for a leaf-level vdev by property id or name.
 */
static int
spa_vdev_get_common(spa_t *spa, uint64_t guid, char **value,
    uint64_t *oval, vdev_prop_t prop)
{
	vdev_t *vd;
	vdev_queue_class_t *vqc;
	zio_priority_t p;

	spa_vdev_state_enter(spa, SCL_ALL);

	if ((vd = spa_lookup_by_guid(spa, guid, B_TRUE)) == NULL)
		return (spa_vdev_state_exit(spa, NULL, EINVAL));

	if (!vd->vdev_ops->vdev_op_leaf)
		return (spa_vdev_state_exit(spa, NULL, EINVAL));

	vqc = vd->vdev_queue.vq_class;

	switch (prop) {
	case VDEV_PROP_PATH:
		if (vd->vdev_path != NULL) {
			*value = vd->vdev_path;
		}
		break;
	case VDEV_PROP_FRU:
		if (vd->vdev_fru != NULL) {
			*value = vd->vdev_fru;
		}
		break;

	case VDEV_PROP_READ_MINACTIVE:
	case VDEV_PROP_AREAD_MINACTIVE:
	case VDEV_PROP_WRITE_MINACTIVE:
	case VDEV_PROP_AWRITE_MINACTIVE:
	case VDEV_PROP_SCRUB_MINACTIVE:
		p = VDEV_PROP_TO_ZIO_PRIO_MIN(prop);
		ASSERT(ZIO_PRIORITY_QUEUEABLE_VALID(p));
		*oval = vqc[p].vqc_min_active;
		break;

	case VDEV_PROP_READ_MAXACTIVE:
	case VDEV_PROP_AREAD_MAXACTIVE:
	case VDEV_PROP_WRITE_MAXACTIVE:
	case VDEV_PROP_AWRITE_MAXACTIVE:
	case VDEV_PROP_SCRUB_MAXACTIVE:
		p = VDEV_PROP_TO_ZIO_PRIO_MAX(prop);
		ASSERT(ZIO_PRIORITY_QUEUEABLE_VALID(p));
		*oval = vqc[p].vqc_max_active;
		break;

	case VDEV_PROP_PREFERRED_READ:
		*oval = vd->vdev_queue.vq_preferred_read;
		break;

	case VDEV_PROP_COS:
		if (vd->vdev_queue.vq_cos != NULL) {
			*value = vd->vdev_queue.vq_cos->cos_name;
		} else {
			*value = NULL;
			return (spa_vdev_state_exit(spa, NULL, ENOENT));
		}
		break;

	case VDEV_PROP_SPAREGROUP:
		if (vd->vdev_spare_group != NULL) {
			*value = vd->vdev_spare_group;
		} else {
			*value = NULL;
			return (spa_vdev_state_exit(spa, NULL, ENOENT));
		}
		break;

	default:
		return (spa_vdev_state_exit(spa, NULL, ENOTSUP));
	}

	return (spa_vdev_state_exit(spa, NULL, 0));
}

/*
 * Update the stored property for this vdev.
 */
static int
spa_vdev_set_common(vdev_t *vd, const char *value,
    uint64_t ival, vdev_prop_t prop)
{
	spa_t *spa = vd->vdev_spa;
	cos_t *cos = NULL, *cos_to_release = NULL;
	boolean_t sync = B_FALSE;
	boolean_t reset_cos = B_FALSE;
	vdev_queue_class_t *vqc = vd->vdev_queue.vq_class;
	zio_priority_t p;

	ASSERT(spa_writeable(spa));

	spa_vdev_state_enter(spa, SCL_ALL);

	if (!vd->vdev_ops->vdev_op_leaf)
		return (spa_vdev_state_exit(spa, NULL, EINVAL));

	switch (prop) {
	case VDEV_PROP_PATH:
		if (vd->vdev_path == NULL) {
			vd->vdev_path = spa_strdup(value);
			sync = B_TRUE;
		} else if (strcmp(value, vd->vdev_path) != 0) {
			spa_strfree(vd->vdev_path);
			vd->vdev_path = spa_strdup(value);
			sync = B_TRUE;
		}
		break;

	case VDEV_PROP_FRU:
		if (vd->vdev_fru == NULL) {
			vd->vdev_fru = spa_strdup(value);
			sync = B_TRUE;
		} else if (strcmp(value, vd->vdev_fru) != 0) {
			spa_strfree(vd->vdev_fru);
			vd->vdev_fru = spa_strdup(value);
			sync = B_TRUE;
		}
		break;

	case VDEV_PROP_READ_MINACTIVE:
	case VDEV_PROP_AREAD_MINACTIVE:
	case VDEV_PROP_WRITE_MINACTIVE:
	case VDEV_PROP_AWRITE_MINACTIVE:
	case VDEV_PROP_SCRUB_MINACTIVE:
		p = VDEV_PROP_TO_ZIO_PRIO_MIN(prop);
		ASSERT(ZIO_PRIORITY_QUEUEABLE_VALID(p));
		vqc[p].vqc_min_active = ival;
		break;

	case VDEV_PROP_READ_MAXACTIVE:
	case VDEV_PROP_AREAD_MAXACTIVE:
	case VDEV_PROP_WRITE_MAXACTIVE:
	case VDEV_PROP_AWRITE_MAXACTIVE:
	case VDEV_PROP_SCRUB_MAXACTIVE:
		p = VDEV_PROP_TO_ZIO_PRIO_MAX(prop);
		ASSERT(ZIO_PRIORITY_QUEUEABLE_VALID(p));
		vqc[p].vqc_max_active = ival;
		break;

	case VDEV_PROP_PREFERRED_READ:
		vd->vdev_queue.vq_preferred_read = ival;
		break;

	case VDEV_PROP_COS:
		spa_cos_enter(spa);

		if ((value == NULL || value[0] == '\0') && ival == 0) {
			reset_cos = B_TRUE;
		} else {
			if (ival != 0)
				cos = spa_lookup_cos_by_guid(spa, ival);
			else
				cos = spa_lookup_cos_by_name(spa, value);
		}

		if (!reset_cos && cos == NULL) {
			spa_cos_exit(spa);
			return (spa_vdev_state_exit(spa, NULL, ENOENT));
		}

		cos_to_release = vd->vdev_queue.vq_cos;

		if (reset_cos) {
			vd->vdev_queue.vq_cos = NULL;
		} else {
			cos_hold(cos);
			vd->vdev_queue.vq_cos = cos;
		}

		if (cos_to_release)
			cos_rele(cos_to_release);

		spa_cos_exit(spa);
		break;

	case VDEV_PROP_SPAREGROUP:
		if (vd->vdev_spare_group == NULL) {
			vd->vdev_spare_group = spa_strdup(value);
			sync = B_TRUE;
		} else if (strcmp(value, vd->vdev_spare_group) != 0) {
			spa_strfree(vd->vdev_spare_group);
			vd->vdev_spare_group = spa_strdup(value);
			sync = B_TRUE;
		}
		break;

	default:
		return (spa_vdev_state_exit(spa, NULL, ENOTSUP));
	}

	return (spa_vdev_state_exit(spa, sync ? vd : NULL, 0));
}

/*
 * Set properties (names, values) in this nvlist, indicate if sync is needed
 */
static int
spa_vdev_prop_set_nosync(vdev_t *vd, nvlist_t *nvp, boolean_t *needsyncp)
{
	int error = 0;
	nvpair_t *elem;
	vdev_prop_t prop;
	boolean_t need_sync = B_FALSE;

	elem = NULL;
	while ((elem = nvlist_next_nvpair(nvp, elem)) != NULL) {
		uint64_t ival = 0;
		char *strval = NULL;
		zprop_type_t proptype;
		if ((prop = vdev_name_to_prop(
		    nvpair_name(elem))) == ZPROP_INVAL)
			return (ENOTSUP);

		switch (prop) {
		case VDEV_PROP_READ_MINACTIVE:
		case VDEV_PROP_READ_MAXACTIVE:
		case VDEV_PROP_AREAD_MINACTIVE:
		case VDEV_PROP_AREAD_MAXACTIVE:
		case VDEV_PROP_WRITE_MINACTIVE:
		case VDEV_PROP_WRITE_MAXACTIVE:
		case VDEV_PROP_AWRITE_MINACTIVE:
		case VDEV_PROP_AWRITE_MAXACTIVE:
		case VDEV_PROP_SCRUB_MINACTIVE:
		case VDEV_PROP_SCRUB_MAXACTIVE:
		case VDEV_PROP_PREFERRED_READ:
		case VDEV_PROP_COS:
		case VDEV_PROP_SPAREGROUP:
			need_sync = B_TRUE;
			break;
		default:
			need_sync = B_FALSE;
		}

		proptype = vdev_prop_get_type(prop);

		switch (proptype) {
		case PROP_TYPE_STRING:
			VERIFY(nvpair_value_string(elem, &strval) == 0);
			break;
		case PROP_TYPE_INDEX:
		case PROP_TYPE_NUMBER:
			VERIFY(nvpair_value_uint64(elem, &ival) == 0);
			if (proptype == PROP_TYPE_INDEX) {
				const char *unused;
				VERIFY(vdev_prop_index_to_string(
				    prop, ival, &unused) == 0);
			}
		}

		error = spa_vdev_set_common(vd, strval, ival, prop);
	}

	if (needsyncp != NULL)
		*needsyncp = need_sync;

	return (error);
}

/*
 * Store properties of vdevs in the pool in the MOS of that pool
 */
static void
spa_vdev_sync_props(void *arg1, dmu_tx_t *tx)
{
	spa_t *spa = (spa_t *)arg1;
	objset_t *mos = spa->spa_meta_objset;
	uint64_t size = 0;
	uint64_t *sizep;
	vdev_t *root_vdev;
	vdev_t *top_vdev;
	dmu_buf_t *db;

	mutex_enter(&spa->spa_vdev_props_lock);

	if (spa->spa_vdev_props_object == 0) {
		VERIFY((spa->spa_vdev_props_object =
		    dmu_object_alloc(mos, DMU_OT_VDEV_PROPS, 0,
		    DMU_OT_VDEV_PROPS_SIZE, 8, tx)) > 0);

		VERIFY(zap_update(mos,
		    DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_VDEV_PROPS,
		    8, 1, &spa->spa_vdev_props_object, tx) == 0);

		spa_feature_incr(spa, SPA_FEATURE_VDEV_PROPS, tx);
	}

	root_vdev = spa->spa_root_vdev;

	/* process regular vdevs */
	for (int c = 0; c < spa->spa_root_vdev->vdev_children; c++) {
		top_vdev = root_vdev->vdev_child[c];
		size += vdev_store_props(top_vdev, mos,
		    spa->spa_vdev_props_object, size, tx);
	}

	/* process aux vdevs */
	for (int i = 0; i < spa->spa_l2cache.sav_count; i++) {
		vdev_t *vd = spa->spa_l2cache.sav_vdevs[i];
		size += vdev_store_props(vd, mos,
		    spa->spa_vdev_props_object, size, tx);
	}

	for (int i = 0; i < spa->spa_spares.sav_count; i++) {
		vdev_t *vd = spa->spa_spares.sav_vdevs[i];
		size += vdev_store_props(vd, mos,
		    spa->spa_vdev_props_object, size, tx);
	}

	VERIFY(0 == dmu_bonus_hold(mos, spa->spa_vdev_props_object, FTAG, &db));
	dmu_buf_will_dirty(db, tx);

	sizep = db->db_data;
	*sizep = size;

	dmu_buf_rele(db, FTAG);

	mutex_exit(&spa->spa_vdev_props_lock);
}

int
spa_vdev_props_sync_task_do(spa_t *spa)
{
	return (dsl_sync_task(spa->spa_name, NULL, spa_vdev_sync_props,
	    spa, 3, ZFS_SPACE_CHECK_RESERVED));
}

/*
 * Load vdev properties from the vdev_props_object in the MOS
 */
int
spa_load_vdev_props(spa_t *spa, boolean_t load_aux)
{
	objset_t *mos = spa->spa_meta_objset;
	vdev_t *vdev;
	dmu_buf_t *db;
	vdev_props_phys_hdr_t *vpph;
	size_t bufsize = 0;
	char *buf;
	char *pbuf;
	char *bufend;

	ASSERT(spa);

	if (spa->spa_vdev_props_object == 0)
		return (ENOENT);

	mutex_enter(&spa->spa_vdev_props_lock);

	VERIFY(0 == dmu_bonus_hold(mos, spa->spa_vdev_props_object, FTAG, &db));
	bufsize = *(uint64_t *)db->db_data;
	dmu_buf_rele(db, FTAG);

	if (bufsize == 0)
		goto out;

	buf = kmem_alloc(bufsize, KM_SLEEP);
	bufend = buf + bufsize;

	/* read and unpack array of nvlists */
	VERIFY(0 == dmu_read(mos, spa->spa_vdev_props_object,
	    0, bufsize, buf, DMU_READ_PREFETCH));

	for (pbuf = buf; pbuf < bufend; pbuf += vpph->vpph_size) {
		vpph = (vdev_props_phys_hdr_t *)pbuf;
		char *packed = pbuf + sizeof (*vpph);
		uint64_t nvsize = vpph->vpph_nvsize;

		vdev = spa_lookup_by_guid(spa, vpph->vpph_guid, load_aux);
		if (vdev == NULL)
			continue;
		vdev_parse_props(vdev, packed, nvsize);
	}

	kmem_free(buf, bufsize);
out:
	mutex_exit(&spa->spa_vdev_props_lock);

	return (0);
}

/*
 * Check properties (names, values) in this nvlist
 */
int
spa_vdev_prop_validate(spa_t *spa, nvlist_t *props)
{
	nvpair_t *elem;
	int error = 0;

	if (!spa_feature_is_enabled(spa, SPA_FEATURE_VDEV_PROPS))
		return (ENOTSUP);

	elem = NULL;
	while ((elem = nvlist_next_nvpair(props, elem)) != NULL) {
		vdev_prop_t prop;
		char *propname;
		uint64_t ival;

		propname = nvpair_name(elem);

		if ((prop = vdev_name_to_prop(propname)) == ZPROP_INVAL)
			return (EINVAL);

		switch (prop) {
		case VDEV_PROP_PATH:
		case VDEV_PROP_FRU:
		case VDEV_PROP_COS:
		case VDEV_PROP_SPAREGROUP:
			break;

		case VDEV_PROP_READ_MINACTIVE:
		case VDEV_PROP_AREAD_MINACTIVE:
		case VDEV_PROP_WRITE_MINACTIVE:
		case VDEV_PROP_AWRITE_MINACTIVE:
		case VDEV_PROP_SCRUB_MINACTIVE:
		case VDEV_PROP_READ_MAXACTIVE:
		case VDEV_PROP_AREAD_MAXACTIVE:
		case VDEV_PROP_WRITE_MAXACTIVE:
		case VDEV_PROP_AWRITE_MAXACTIVE:
		case VDEV_PROP_SCRUB_MAXACTIVE:
			error = nvpair_value_uint64(elem, &ival);
			if (!error && ival > 1000)
				error = EINVAL;
			break;

		case VDEV_PROP_PREFERRED_READ:
			error = nvpair_value_uint64(elem, &ival);
			if (!error && ival > 10)
				error = EINVAL;
			break;

		default:
			error = EINVAL;
		}

		if (error)
			break;
	}

	return (error);
}

/*
 * Set properties for the vdev and its children
 */
int
spa_vdev_prop_set(spa_t *spa, uint64_t vdev_guid, nvlist_t *nvp)
{
	int error;
	vdev_t	*vd;
	boolean_t need_sync = B_FALSE;

	if ((error = spa_vdev_prop_validate(spa, nvp)) != 0)
		return (error);

	if ((vd = spa_lookup_by_guid(spa, vdev_guid, B_TRUE)) == NULL)
		return (ENOENT);

	if (!vd->vdev_ops->vdev_op_leaf) {
		int i;
		for (i = 0; i < vd->vdev_children; i++) {
			error = spa_vdev_prop_set(spa,
			    vd->vdev_child[i]->vdev_guid, nvp);
			if (error != 0)
				break;
		}

		return (error);
	}

	if ((error = spa_vdev_prop_set_nosync(vd, nvp, &need_sync)) != 0)
		return (error);

	if (need_sync)
		return (spa_vdev_props_sync_task_do(spa));

	return (error);
}

/*
 * Get properties for the vdev, put them on nvlist
 */
int
spa_vdev_prop_get(spa_t *spa, uint64_t vdev_guid, nvlist_t **nvp)
{
	int err;
	vdev_prop_t prop;

	VERIFY(nvlist_alloc(nvp, NV_UNIQUE_NAME, KM_SLEEP) == 0);

	for (prop = VDEV_PROP_PATH; prop < VDEV_NUM_PROPS; prop++) {
		uint64_t ival;
		char *strval = NULL;
		const char *propname = vdev_prop_to_name(prop);

		if ((err = spa_vdev_get_common(spa, vdev_guid, &strval,
		    &ival, prop)) != 0 && (err != ENOENT))
			return (err);

		if (strval != NULL) {
			VERIFY(nvlist_add_string(*nvp, propname, strval) == 0);
		} else if (err != ENOENT) {
			VERIFY(nvlist_add_uint64(*nvp, propname, ival) == 0);
		}
	}

	return (0);
}

int
spa_vdev_setpath(spa_t *spa, uint64_t guid, const char *newpath)
{
	vdev_t *vdev;

	if ((vdev = spa_lookup_by_guid(spa, guid, B_TRUE)) == NULL)
		return (ENOENT);

	return (spa_vdev_set_common(vdev, newpath, 0, VDEV_PROP_PATH));
}

int
spa_vdev_setfru(spa_t *spa, uint64_t guid, const char *newfru)
{
	vdev_t *vdev;

	if ((vdev = spa_lookup_by_guid(spa, guid, B_TRUE)) == NULL)
		return (ENOENT);

	return (spa_vdev_set_common(vdev, newfru, 0, VDEV_PROP_FRU));
}
