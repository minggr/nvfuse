/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 30/10/2016
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
//#define NDEBUG
#include <assert.h>

#ifdef __linux__
#include <sys/uio.h>
#endif

#include "spdk/env.h"

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_mempool.h>

#include "nvfuse_dep.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_core.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_gettimeofday.h"
#include "nvfuse_indirect.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_config.h"
#include "nvfuse_malloc.h"
#include "nvfuse_api.h"
#include "nvfuse_dirhash.h"
#include "nvfuse_ipc_ring.h"
#include "nvfuse_dirhash.h"

struct nvfuse_inode_ctx *nvfuse_read_inode(struct nvfuse_superblock *sb,
		struct nvfuse_inode_ctx *ictx, inode_t ino)
{
	struct nvfuse_inode_ctx *alloc_ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_head *bh;
	lbno_t block;
	lbno_t offset;

	if (ino < ROOT_INO)
		return NULL;

	if (ictx == NULL) {
		alloc_ictx = nvfuse_get_ictx(sb, ino);
		if (alloc_ictx == NULL)
			return NULL;
	} else {
		alloc_ictx = ictx;
	}

	block = ino / INODE_ENTRY_NUM;
	offset = ino % INODE_ENTRY_NUM;
	bh = nvfuse_get_bh(sb, alloc_ictx, ITABLE_INO, block, READ, NVFUSE_TYPE_META);
	if (bh == NULL) {
		printf(" Error: get_bh() for read inode()\n");
		return NULL;
	}
	inode = (struct nvfuse_inode *)bh->bh_buf;
	inode += offset;
	assert(ino == inode->i_ino);

	alloc_ictx->ictx_ino = ino;
	alloc_ictx->ictx_inode = inode;
	alloc_ictx->ictx_bh = bh;
	alloc_ictx->ictx_ref++;

	assert(inode->i_ino);

	return alloc_ictx;
}

void nvfuse_mark_inode_dirty(struct nvfuse_inode_ctx *ictx)
{
	set_bit(&ictx->ictx_status, BUFFER_STATUS_DIRTY);
	clear_bit(&ictx->ictx_status, BUFFER_STATUS_CLEAN);
}

void nvfuse_release_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty)
{
	struct nvfuse_buffer_head *bh;

	if (ictx == NULL)
		return;

	bh = ictx->ictx_bh;
	if (bh) {
		nvfuse_set_bh_status(bh, BUFFER_STATUS_META);
		nvfuse_release_bh(sb, bh, 0/*head*/, dirty);
	}

	ictx->ictx_bh = NULL;
	nvfuse_release_ictx(sb, ictx, dirty);
}

s32 nvfuse_relocate_delete_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	struct nvfuse_inode *inode;
	inode_t ino;
	u32 bg_id;
	ino = ictx->ictx_ino;
	inode = ictx->ictx_inode;
	inode->i_deleted = 1;
	inode->i_ino = 0;
	inode->i_size = 0;

	nvfuse_release_inode(sb, ictx, DIRTY);
	nvfuse_inc_free_inodes(sb, ino);

	bg_id = ino / sb->sb_no_of_inodes_per_bg;
	nvfuse_release_ibitmap(sb, bg_id, ino);
	return 0;
}

u32 nvfuse_find_free_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, u32 last_ino)
{
	u32 new_ino = 0;
	u32 hint_ino;
	u32 bg_id;
	u32 last_id;
	/* debug info */
	s32 count = 0;
	s32 start_bg = 0;

	if (!spdk_process_is_primary())
		bg_id = nvfuse_get_curr_bg_id(sb, 1 /*inode type*/);
	else
		bg_id = last_ino / sb->sb_no_of_inodes_per_bg;

	last_id = bg_id;

	hint_ino = last_ino % sb->sb_no_of_inodes_per_bg;

	start_bg = bg_id;
	do {
		new_ino = nvfuse_scan_free_ibitmap(sb, ictx, bg_id, hint_ino);
		if (new_ino)
			break;

		if (nvfuse_process_model_is_dataplane())
			bg_id = nvfuse_get_next_bg_id(sb, 1 /*inode type*/);
		else
			bg_id = (bg_id + 1) % sb->sb_bg_num;

		hint_ino = 0;
		count++;
		//printf(" bg_id = %d \n", bg_id);
	} while (bg_id != last_id);

	if (new_ino) {
		//printf(" Found free inode = %d, bg = %d \n", new_ino, new_ino / sb->sb_no_of_inodes_per_bg);
	} else {
		printf(" Unavailable free inodes!!! app free inode = %d\n", sb->asb.asb_free_inodes);
		printf(" loop count = %d \n", count);
		printf(" bg list count = %d \n", sb->sb_bg_list_count);
		printf(" start bg = %d cur bg = %d \n", start_bg, bg_id);
		nvfuse_print_bg_list(sb);

		bg_id = 1;
		new_ino = nvfuse_scan_free_ibitmap(sb, ictx, bg_id, hint_ino);
		printf(" alloc inode = %d \n", new_ino);

		assert(0);
	}

	return new_ino;
}

void nvfuse_free_inode_size(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s64 size)
{
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_cache *bc;
	lbno_t offset;
	u32 num_block, trun_num_block;
	s32 res;
	u64 key;
	s32 unused_count = 0;

	inode = ictx->ictx_inode;

	num_block = NVFUSE_SIZE_TO_BLK(inode->i_size);
	trun_num_block = NVFUSE_SIZE_TO_BLK(size);
	if (inode->i_size & (CLUSTER_SIZE - 1))
		num_block++;

	/* debug code
	*if(trun_num_block)
	*	printf(" trun num block = %d\n", trun_num_block);
	*/

	if (!num_block || num_block <= trun_num_block)
		return;

	/*
	 * Too slow when large inode is deleted.
	 * FIXME: buffers will be managed by RBtree or other structures.
	 */

	for (offset = num_block - 1; offset >= trun_num_block; offset--) {
		nvfuse_make_pbno_key(inode->i_ino, offset, &key, NVFUSE_BP_TYPE_DATA);
		bc = (struct nvfuse_buffer_cache *)nvfuse_hash_lookup(sb->sb_bm, key);
		if (bc) {
			nvfuse_remove_bhs_in_bc(sb, bc);

			/* FIXME: reinitialization is necessary */
			bc->bc_load = 0;
			bc->bc_pno = 0;
			bc->bc_dirty = 0;
			bc->bc_ref = 0;

			nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_UNUSED, INSERT_HEAD);
			unused_count++;
		}

		if (offset == 0)
			break;
	}

	if (!sb->sb_nvh->nvh_params.preallocation && nvfuse_process_model_is_dataplane()) {
		while (unused_count--) {
			if (sb->sb_bm->bm_list_count[BUFFER_TYPE_UNUSED] >= NVFUSE_BUFFER_DEFAULT_ALLOC_SIZE_PER_MSG) {
				res = nvfuse_remove_buffer_cache(sb, NVFUSE_BUFFER_DEFAULT_ALLOC_SIZE_PER_MSG);
				if (res == 0) {
					nvfuse_send_dealloc_buffer_req(sb->sb_nvh, NVFUSE_BUFFER_DEFAULT_ALLOC_SIZE_PER_MSG);
				}
			} else {
				break;
			}
		}
	}

	/* truncate blocks */
	nvfuse_truncate_blocks(sb, ictx, size);
}

inode_t nvfuse_alloc_new_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx)
{
	struct nvfuse_buffer_head *bh;
	struct nvfuse_inode *ip;

	lbno_t search_block = 0, search_entry = 0;
	inode_t alloc_ino = 0;
	inode_t hint_ino = 0;
	inode_t last_allocated_ino = 0;
	s32 container_id;

	if (nvfuse_process_model_is_dataplane() && !nvfuse_check_free_inode(sb)) {
		container_id = nvfuse_alloc_container_from_primary_process(sb->sb_nvh, CONTAINER_NEW_ALLOC);
		if (container_id > 0) {
			/* insert allocated container to process */
			nvfuse_add_bg(sb, container_id);
			assert(nvfuse_check_free_inode(sb) == 1);
		} else {
			assert(0);
		}
	}

	last_allocated_ino = sb->sb_last_allocated_ino;
	hint_ino = nvfuse_find_free_inode(sb, ictx, last_allocated_ino);
	if (hint_ino) {
		search_block = hint_ino / INODE_ENTRY_NUM;
		search_entry = hint_ino % INODE_ENTRY_NUM;
	} else {
		printf(" no more inodes in the file system.");
		return 0;
	}


	bh = nvfuse_get_bh(sb, ictx, ITABLE_INO, search_block, READ, NVFUSE_TYPE_META);
	ip = (struct nvfuse_inode *)bh->bh_buf;
#ifdef NVFUSE_USE_MKFS_INODE_ZEROING
	for (j = 0; j < INODE_ENTRY_NUM; j++) {
		if (ip[search_entry].i_ino == 0 &&
		    (search_entry + search_block * INODE_ENTRY_NUM) >= NUM_RESV_INO) {
			alloc_ino = search_entry + search_block * INODE_ENTRY_NUM;
			goto RES;
		}
		search_entry = (search_entry + 1) % INODE_ENTRY_NUM;
	}

	/* FIXME: need to put error handling code  */
	printf(" Warning: it runs out of free inodes = %d \n", sb->sb_free_inodes);
	printf(".");
	while (1);

RES:
	;

#else
	alloc_ino = search_entry + search_block * INODE_ENTRY_NUM;
#endif

	nvfuse_dec_free_inodes(sb, alloc_ino);

	ip += search_entry;

	/* initialization of inode entry */
	memset(ip, 0x00, INODE_ENTRY_SIZE);

	ip->i_ino = alloc_ino;
	ip->i_deleted = 0;
	ip->i_version++;

	nvfuse_release_bh(sb, bh, 0, DIRTY);

	/* keep hit information to rapidly find a free inode */
	if (!spdk_process_is_primary() || nvfuse_process_model_is_standalone()) {
		sb->sb_last_allocated_ino = alloc_ino + 1;
	}

	if (spdk_process_is_primary() && nvfuse_process_model_is_dataplane()) {
		printf(" allocated ino = %d\n", alloc_ino);
	}

	return alloc_ino;
}

void nvfuse_release_ibitmap(struct nvfuse_superblock *sb, u32 bg_id, u32 ino)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;
	struct nvfuse_buffer_head *bh;
	void *buf;

	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	bh = nvfuse_get_bh(sb, NULL, IBITMAP_INO, bg_id, READ, NVFUSE_TYPE_META);
	buf = bh->bh_buf;

	if (ext2fs_test_bit(ino % bd->bd_max_inodes, buf)) {
		ext2fs_clear_bit(ino % bd->bd_max_inodes, buf);
	} else {
		printf(" Warning: ino was already released \n");
	}

	nvfuse_release_bh(sb, bd_bh, 0, CLEAN);
	nvfuse_release_bh(sb, bh, 0, DIRTY);
}

void nvfuse_inc_free_inodes(struct nvfuse_superblock *sb, inode_t ino)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;
	u32 bg_id;

	bg_id = ino / sb->sb_no_of_inodes_per_bg;
	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	bd->bd_free_inodes++;
	sb->sb_free_inodes++;
	if (!spdk_process_is_primary()) {
		sb->asb.asb_free_inodes++;
	}
	assert(bd->bd_free_inodes <= bd->bd_max_inodes);
	nvfuse_release_bh(sb, bd_bh, 0, DIRTY);

	/* release bg to the control plane */
	if (!sb->sb_nvh->nvh_params.preallocation && nvfuse_process_model_is_dataplane()) {
		//printf(" %s bd free blocks = %d(/%d) inode = %d(/%d)\n", __FUNCTION__, bd->bd_free_blocks + (bd->bd_dtable_start % sb->sb_no_of_blocks_per_bg), bd->bd_max_blocks, bd->bd_free_inodes, bd->bd_max_inodes);
		if (bd->bd_free_blocks + (bd->bd_dtable_start % sb->sb_no_of_blocks_per_bg) == bd->bd_max_blocks &&
		    bd->bd_free_inodes == bd->bd_max_inodes) {
			//printf(" %s Deallocate bg = %d \n", __FUNCTION__, bg_id);
			nvfuse_remove_bg(sb, bg_id);
		}
	}

}

void nvfuse_dec_free_inodes(struct nvfuse_superblock *sb, inode_t ino)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;
	u32 bg_id;

	bg_id = ino / sb->sb_no_of_inodes_per_bg;
	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	bd->bd_free_inodes--;
	sb->sb_free_inodes--;
	if (!spdk_process_is_primary()) {
		sb->asb.asb_free_inodes--;
	}
	assert(bd->bd_free_inodes >= 0);
	nvfuse_release_bh(sb, bd_bh, 0, DIRTY);
}

void nvfuse_inc_free_blocks(struct nvfuse_superblock *sb, u32 blockno, u32 cnt)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;
	u32 bg_id;

	bg_id = blockno / sb->sb_no_of_blocks_per_bg;
	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	bd->bd_free_blocks += cnt;
	sb->sb_free_blocks += cnt;
	sb->sb_no_of_used_blocks -= cnt;
	if (!spdk_process_is_primary())
		sb->asb.asb_free_blocks += cnt;

	assert(bd->bd_free_blocks <= bd->bd_max_blocks);
	assert(sb->sb_free_blocks <= sb->sb_no_of_blocks);
	if (!spdk_process_is_primary())
		assert(sb->asb.asb_free_blocks <= sb->sb_no_of_blocks);
	assert(sb->sb_no_of_used_blocks >= 0);

	/* removal of unused bg to primary process (e.g., control plane)*/
	if (!sb->sb_nvh->nvh_params.preallocation && nvfuse_process_model_is_dataplane()) {
		//printf(" %s bd free blocks = %d(/%d) inode = %d(/%d)\n", __FUNCTION__, bd->bd_free_blocks + (bd->bd_dtable_start % sb->sb_no_of_blocks_per_bg), bd->bd_max_blocks, bd->bd_free_inodes, bd->bd_max_inodes);
		if (bd->bd_free_blocks + (bd->bd_dtable_start % sb->sb_no_of_blocks_per_bg) == bd->bd_max_blocks &&
		    bd->bd_free_inodes == bd->bd_max_inodes) {
			//printf(" %s Deallocate bg = %d \n", __FUNCTION__, bg_id);
			nvfuse_remove_bg(sb, bg_id);
		}
	}

	nvfuse_release_bh(sb, bd_bh, 0, DIRTY);
}

void nvfuse_update_owner_in_bd_info(struct nvfuse_superblock *sb, s32 bg_id)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;

	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	bd->bd_owner = sb->asb.asb_core_id;

	//printf(" Update bg id = %d, owner = %d\n", bg_id, bd->bd_owner);

	nvfuse_release_bh(sb, bd_bh, 0, DIRTY);
}

u32 nvfuse_scan_free_ibitmap(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, 
								u32 bg_id, u32 hint_free_inode)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;
	struct nvfuse_buffer_head *bh;
	void *buf;
	u32 count = 0;
	u32 free_inode = hint_free_inode;
	u32 found = 0;

	bd_bh = nvfuse_get_bh(sb, ictx, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	bh = nvfuse_get_bh(sb, ictx, IBITMAP_INO, bg_id, READ, NVFUSE_TYPE_META);
	buf = bh->bh_buf;

	while (bd->bd_free_inodes && count < sb->sb_no_of_inodes_per_bg) {
		if (!ext2fs_test_bit(free_inode, buf)) {
			//printf(" bg = %d free block %d found \n", bg_id, free_inode);
			found = 1;
			break;
		}
		free_inode = (free_inode + 1) % sb->sb_no_of_inodes_per_bg;
		count++;
	}

	if (found && free_inode < sb->sb_no_of_inodes_per_bg) {
		ext2fs_set_bit(free_inode, buf);
		free_inode += (bg_id * bd->bd_max_inodes);
	} else {
		free_inode = 0;
	}

	nvfuse_release_bh(sb, bd_bh, 0, CLEAN);
	if (found)
		nvfuse_release_bh(sb, bh, 0, DIRTY);
	else
		nvfuse_release_bh(sb, bh, 0, CLEAN);

	return free_inode;
}

u32 nvfuse_get_next_bg_id(struct nvfuse_superblock *sb, s32 is_inode)
{
	u32 next_bg_id;

	if (!spdk_process_is_primary()) {
		struct list_head *first_node;
		struct bg_node *bg_node;

		if (is_inode) {
			first_node = sb->sb_bg_search_ptr_for_inode;
		} else {
			first_node = sb->sb_bg_search_ptr_for_data;
		}

		if (list_is_last(first_node, &sb->sb_bg_list)) {
			printf(" Debug: list_is_last ..!! %s\n", is_inode ? "inode" : "data");
			first_node = &sb->sb_bg_list;
		}

		if (is_inode) {
			sb->sb_bg_search_ptr_for_inode = first_node->next;
			first_node = sb->sb_bg_search_ptr_for_inode;
		} else {
			sb->sb_bg_search_ptr_for_data = first_node->next;
			first_node = sb->sb_bg_search_ptr_for_data;
		}

		bg_node = container_of(first_node, struct bg_node, list);

		next_bg_id = bg_node->bg_id;
		assert(next_bg_id != 0);
	} else {
		next_bg_id = 0; /* primary process only utilizes the first container. */
	}

	return next_bg_id;
}

void nvfuse_move_curr_bg_id(struct nvfuse_superblock *sb, s32 bg_id, s32 is_inode)
{
	if (!spdk_process_is_primary()) {
		struct list_head *first_node;
		struct bg_node *bg_node;

		if (is_inode)
			first_node = sb->sb_bg_search_ptr_for_inode;
		else
			first_node = sb->sb_bg_search_ptr_for_data;

		assert(first_node != NULL);
		assert(first_node != &sb->sb_bg_list);

		do {
			//if (is_inode == 0)
			//	nvfuse_print_bg_list(sb);
			bg_node = container_of(first_node, struct bg_node, list);
			if (bg_node->bg_id == bg_id) {
				if (is_inode)
					sb->sb_bg_search_ptr_for_inode = first_node;
				else
					sb->sb_bg_search_ptr_for_data = first_node;

				//printf(" set bg id = %d for %s\n", bg_id, is_inode ? "inode" : "data");

				assert(bg_id != 0);
				break;
			}

			/* bg 0 is reserved for control plane */
			assert(bg_id != 0);

			/* skip list head */
			do {
				first_node = first_node->next;
			} while (first_node == &sb->sb_bg_list);

		} while (1);
	} else {
		assert(0);
	}
}

u32 nvfuse_get_curr_bg_id(struct nvfuse_superblock *sb, s32 is_inode)
{
	u32 curr_bg_id;

	if (!spdk_process_is_primary()) {
		struct list_head *first_node;
		struct bg_node *bg_node;

		if (is_inode) {
			first_node = sb->sb_bg_search_ptr_for_inode;
		} else {
			first_node = sb->sb_bg_search_ptr_for_data;
		}

		assert(first_node != NULL);
		assert(first_node != &sb->sb_bg_list);

		bg_node = container_of(first_node, struct bg_node, list);
		curr_bg_id = bg_node->bg_id;
		assert(curr_bg_id != 0);
	} else {
		curr_bg_id = 0; /* primary process only utilizes the first container. */
	}

	return curr_bg_id;
}

void nvfuse_dec_free_blocks(struct nvfuse_superblock *sb, u32 blockno, u32 cnt)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;
	u32 bg_id;

	bg_id = blockno / sb->sb_no_of_blocks_per_bg;
	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	bd->bd_free_blocks -= cnt;
	sb->sb_free_blocks -= cnt;

	if (!spdk_process_is_primary())
		sb->asb.asb_free_blocks -= cnt;

	sb->sb_no_of_used_blocks += cnt;
	assert(bd->bd_free_blocks >= 0);
	assert(sb->sb_free_blocks >= 0);
	if (!spdk_process_is_primary())
		assert(sb->asb.asb_free_blocks >= 0);
	assert(sb->sb_no_of_used_blocks <= sb->sb_no_of_blocks);

	nvfuse_release_bh(sb, bd_bh, 0, DIRTY);
}

u32 nvfuse_get_free_blocks(struct nvfuse_superblock *sb, u32 bg_id)
{
	struct nvfuse_bg_descriptor *bd = NULL;
	struct nvfuse_buffer_head *bd_bh;
	u32 free_blocks;

	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	if (bd_bh == NULL) {
		printf(" Error: nvfuse_get_bh\n");
		return 0;
	}
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	free_blocks = bd->bd_free_blocks;

	assert(bd->bd_free_blocks >= 0);
	assert(sb->sb_free_blocks >= 0);
	nvfuse_release_bh(sb, bd_bh, 0, CLEAN);

	return free_blocks;
}

s32 nvfuse_check_free_inode(struct nvfuse_superblock *sb)
{
	if (!spdk_process_is_primary())
		return sb->asb.asb_free_inodes ? 1 : 0;
	else
		return sb->sb_free_inodes ? 1 : 0;
}

s32 nvfuse_check_free_block(struct nvfuse_superblock *sb, u32 num_blocks)
{
	s32 ret;

	if (spdk_process_is_primary())
		ret = (sb->sb_free_blocks >= (s64)num_blocks) ? 1 : 0;
	else
		ret = (sb->asb.asb_free_blocks >= (s64)num_blocks) ? 1 : 0;

	return ret;
}

void nvfuse_free_blocks(struct nvfuse_superblock *sb, u32 block_to_delete, u32 count)
{
	u32 end_blk = block_to_delete + count;
	u32 start_blk = block_to_delete;
	u32 bg_id;
	u32 offset;
	u32 next_bg_id;
	u32 length = 0;

	start_blk--;
	goto RESET;

	while (start_blk < end_blk) {

		length++;

		next_bg_id = (start_blk + 1) / sb->sb_no_of_blocks_per_bg;
		if (bg_id != next_bg_id || start_blk + 1 == end_blk) {
			nvfuse_free_dbitmap(sb, bg_id, offset, length);

RESET:
			;
			length = 0;
			bg_id = (start_blk + 1) / sb->sb_no_of_blocks_per_bg;
			offset = (start_blk + 1) % sb->sb_no_of_blocks_per_bg;
		}
		start_blk ++;
	}
}

s32 nvfuse_set_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename,
			    u32 offset)
{
	bkey_t key;
	u32 dir_hash[2];
	u32 collision = ~0;
	u32 cur_offset;
	u64 start_tsc = spdk_get_ticks();
	u64 end_tsc;
	master_node_t *master;

	assert(inode->i_bpino);

	master = bp_init_master(sb);
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	collision >>= NVFUSE_BP_COLLISION_BITS;
	offset &= collision;

	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);
	key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;
	if (B_INSERT(master, &key, &offset, &cur_offset, 0) < 0) {
		u32 c = cur_offset >> (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
		c++;

		printf(" file name collision = %08x%08x, %d\n", dir_hash[0], dir_hash[1], c);

		c <<= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
		/* if collision occurs, offset is set to 0 */
		cur_offset = 0;
		cur_offset = c | cur_offset;
		//collision
		B_UPDATE(master, &key, &cur_offset);
	}
	bp_write_master(master);
	bp_deinit_master(master);

	end_tsc = spdk_get_ticks();
	assert((end_tsc - start_tsc) > 0);
	sb->bp_set_index_tsc += (end_tsc - start_tsc);
	sb->bp_set_index_count++;

	return 0;
}

s32 nvfuse_get_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename,
			    bitem_t *offset)
{
	bkey_t key;
	u32 dir_hash[2];
	u32 collision = ~0;
	u32 c;
	int res = 0;
	master_node_t *master;

	assert(inode->i_bpino);

	master = bp_init_master(sb);
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	if (!strcmp(filename, ".") || !strcmp(filename, "..")) {
		*offset = 0;
		return 0;
	}

	collision >>= NVFUSE_BP_COLLISION_BITS;
	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);
	key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;
	if (bp_find_key(master, &key, offset) < 0) {
		res = -1;
		goto RES;
	}

	c = *offset;
	c >>= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
	if (c)
		*offset = 0;
	else
		*offset &= collision;
RES:
	;
	B_RELEASE_BH(master, master->m_bh);
	bp_deinit_master(master);
	return res;
}

s32 nvfuse_update_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode,
						   s8 *filename, bitem_t *offset)
{
	bkey_t key;
	u32 dir_hash[2];
	u32 collision = ~0;
	u32 c;
	int res = 0;

	master_node_t *master;

	assert(inode->i_bpino);

	master = bp_init_master(sb);
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	if (!strcmp(filename, ".") || !strcmp(filename, "..")) {
		*offset = 0;
		return 0;
	}

	collision >>= NVFUSE_BP_COLLISION_BITS;
	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);
	key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;
	if (bp_find_key(master, &key, offset) < 0) {
		res = -1;
		goto RES;
	}

	c = *offset;
	c >>= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
	if (c)
		*offset = 0;
	else
		*offset &= collision;
RES:
	;

	bp_deinit_master(master);
	return res;
}

s32 nvfuse_del_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode, s8 *filename)
{
	u32 dir_hash[2];
	bkey_t key;
	bitem_t offset = 0;
	u32 collision = ~0;
	u32 c;
	master_node_t *master = NULL;

	assert(inode->i_bpino);

	master = bp_init_master(sb);
	master->m_ino = inode->i_bpino;
	master->m_sb = sb;
	bp_read_master(master);

	collision >>= NVFUSE_BP_COLLISION_BITS;

	nvfuse_dir_hash(filename, dir_hash, dir_hash + 1);
	key = (u64)dir_hash[0] | ((u64)dir_hash[1]) << 32;

	if (bp_find_key(master, &key, &offset) < 0) {
		printf(" find key %lu \n", (unsigned long)key);
		return -1;
	}

	c = offset;
	c >>= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
	if (c == 0)
		B_REMOVE(master, &key);
	else {
		c--;
		c <<= (NVFUSE_BP_LOW_BITS - NVFUSE_BP_COLLISION_BITS);
		offset &= collision;
		offset = c | offset;
		//collision
		B_UPDATE(master, &key, &offset);
	}

	bp_write_master(master);
	bp_deinit_master(master);
	return 0;
}

void io_cancel_incomplete_ios(struct nvfuse_superblock *sb, struct io_job **jobq, int job_cnt)
{
	struct io_job *job;
	int i;

	for (i = 0; i < job_cnt; i++) {
		job = jobq[i];

		if (job && job->complete)
			continue;

		nvfuse_aio_cancel(job, sb->io_manager);
	}
}

s32 nvfuse_wait_aio_completion(struct nvfuse_superblock *sb, struct io_job **jobq, int job_cnt)
{
	struct io_job *job;
	int cc; // completion count

	//nvfuse_aio_resetnextcjob(sb->io_manager);
	while (sb->io_manager->queue_cur_count) {

		cc = nvfuse_aio_complete(sb->io_manager);
		sb->io_manager->queue_cur_count -= cc;
		assert(sb->io_manager->queue_cur_count >= 0);

		while (cc--) {
			job = nvfuse_aio_getnextcjob(sb->io_manager);

			if (job->ret != job->bytes) {
				printf(" Error: IO \n");
			}

			job->complete = 1;
		}

		//printf(" spdk cjob size = %d, cnt = %d\n", cjob_size(sb->io_manager), sb->io_manager->cjob_cnt);
		//printf(" queue count = %d \n", sb->io_manager->queue_cur_count);
	}

	// TODO: io cancel
	io_cancel_incomplete_ios(sb, jobq, job_cnt);

	return 0;
}

s32 nvfuse_make_jobs(struct nvfuse_superblock *sb, struct io_job **jobs, int numjobs)
{
	s32 res;

	assert(numjobs <= AIO_MAX_QDEPTH);

	res = rte_mempool_get_bulk((struct rte_mempool *)sb->io_job_mempool, (void **)jobs, numjobs);
	if (res != 0) {
		fprintf(stderr, "mempool get error for io job \n");
		/* FIXME: how can we handle this error? */
		assert(0);
	}
	return 0;
}

void nvfuse_release_jobs(struct nvfuse_superblock *sb, struct io_job **jobs, int numjobs)
{
	rte_mempool_put_bulk((struct rte_mempool *)sb->io_job_mempool, (void **)jobs, numjobs);
}

s32 nvfuse_sync_dirty_data(struct nvfuse_superblock *sb, struct list_head *head, s32 num_blocks)
{
	struct list_head *ptr, *temp;
	struct nvfuse_buffer_cache *bc;

	struct io_job *jobs[AIO_MAX_QDEPTH];
	struct iocb *iocb[AIO_MAX_QDEPTH];
	s32 count = 0;
	s32 res = 0;

	assert(num_blocks <= AIO_MAX_QDEPTH);

#if (NVFUSE_OS==NVFUSE_OS_LINUX)
	if (sb->io_manager->type == IO_MANAGER_SPDK || sb->io_manager->type == IO_MANAGER_BLKDEVIO) {

		res = nvfuse_make_jobs(sb, jobs, num_blocks);
		if (res != 0) {
			/* FIXME: */
			fprintf(stderr, "mempool get error for io job \n");
		}

		list_for_each_safe(ptr, temp, head) {
			bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
			assert(bc->bc_dirty);

			jobs[count]->offset = (s64)bc->bc_pno * CLUSTER_SIZE;
			jobs[count]->bytes = (size_t)CLUSTER_SIZE;
			jobs[count]->ret = 0;
			jobs[count]->req_type = WRITE;
			jobs[count]->buf = bc->bc_buf;
			jobs[count]->complete = 0;
			iocb[count] = &jobs[count]->iocb;
			count++;

		}

		count = 0;
		while (count < num_blocks) {
			nvfuse_aio_prep(jobs[count], sb->io_manager);
			count++;
		}

		nvfuse_aio_submit(iocb, num_blocks, sb->io_manager);
		sb->io_manager->queue_cur_count = num_blocks;

		nvfuse_wait_aio_completion(sb, jobs, num_blocks);

		nvfuse_release_jobs(sb, jobs, num_blocks);
	} else
#endif
	{	/* in case of ramdisk or filedisk */
		list_for_each_safe(ptr, temp, head) {
			bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
			assert(bc->bc_dirty);
			nvfuse_write_cluster(bc->bc_buf, bc->bc_pno, sb->io_manager);
		}
	}

	list_for_each_safe(ptr, temp, head) {
		bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);

		nvfuse_remove_bhs_in_bc(sb, bc);

		assert(bc->bc_dirty);
		bc->bc_dirty = 0;

		/*if (bc->bc_ref) {
			printf("debug\n");
		}*/
		nvfuse_move_buffer_list(sb, bc, BUFFER_TYPE_CLEAN, INSERT_HEAD);
	}

	return 0;
}

void nvfuse_update_sb_with_bd_info(struct nvfuse_superblock *sb, s32 bg_id, s32 is_root_container,
				   s32 increament)
{
	if (!is_root_container) {
		struct nvfuse_bg_descriptor *bd = nvfuse_get_bd(sb, bg_id);
		if (increament) {
			sb->asb.asb_free_blocks += bd->bd_free_blocks;
			sb->asb.asb_free_inodes += bd->bd_free_inodes;
			sb->asb.asb_no_of_used_blocks += 0;
		} else {
			sb->asb.asb_free_blocks -= bd->bd_free_blocks;
			sb->asb.asb_free_inodes -= bd->bd_free_inodes;
			sb->asb.asb_no_of_used_blocks += 0;
		}
	} else {
		struct nvfuse_bg_descriptor *bd;
		struct nvfuse_buffer_head *bd_bh;

		bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
		bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;

		if (increament) {
			sb->asb.asb_free_blocks += bd->bd_free_blocks;
			sb->asb.asb_free_inodes += bd->bd_free_inodes;
			sb->asb.asb_no_of_used_blocks += (sb->sb_no_of_blocks_per_bg - bd->bd_free_blocks);
		} else {
			sb->asb.asb_free_blocks -= bd->bd_free_blocks;
			sb->asb.asb_free_inodes -= bd->bd_free_inodes;
			sb->asb.asb_no_of_used_blocks -= (sb->sb_no_of_blocks_per_bg - bd->bd_free_blocks);
		}
		nvfuse_release_bh(sb, bd_bh, 0, CLEAN);
	}

	//printf(" %s: sb_free_blocks = %ld, sb_free_inodes = %d\n",
	//		__FUNCTION__, sb->asb.asb_free_blocks, sb->asb.asb_free_inodes);
}

void nvfuse_add_bg(struct nvfuse_superblock *sb, u32 bg_id)
{
	struct list_head *head = &sb->sb_bg_list;
	struct bg_node *new_node;
	s32 root_container = 0;

	new_node = spdk_mempool_get(sb->bg_mempool);

	new_node->bg_id = bg_id;
	list_add_tail(&new_node->list, head);
	sb->sb_bg_list_count++;

	if (sb->sb_bg_list_count == 1) {
		sb->sb_bg_search_ptr_for_inode = sb->sb_bg_list.next;
		sb->sb_bg_search_ptr_for_data = sb->sb_bg_list.next;
		root_container = 1;
	}

	if (nvfuse_process_model_is_dataplane()) {
		if (!spdk_process_is_primary()) {
			nvfuse_update_sb_with_bd_info(sb, bg_id, root_container, 1 /* inc*/);
		}

		nvfuse_update_owner_in_bd_info(sb, bg_id);
	}

	//printf(" core %d adds bg %d (total %d)\n", rte_lcore_id(), bg_id, sb->sb_bg_list_count);
}

s32 nvfuse_remove_bg(struct nvfuse_superblock *sb, u32 bg_id)
{
	struct list_head *head = &sb->sb_bg_list;
	struct list_head *next;
	struct bg_node *node, *temp;
	s32 root_container = 0;
	s32 ret;

	if (bg_id == sb->asb.asb_root_bg_id)
		return 0;

	list_for_each_entry_safe(node, temp, head, list) {
		if (node->bg_id == bg_id) {
			//printf(" found bg = %d %d %p\n", bg_id, node->bg_id, node);
			if (&node->list == sb->sb_bg_search_ptr_for_inode) {
				next = node->list.next;
				while (next == &sb->sb_bg_list)
					next = next->next;
				sb->sb_bg_search_ptr_for_inode = next;
			}

			if (&node->list == sb->sb_bg_search_ptr_for_data) {
				next = node->list.next;
				while (next == &sb->sb_bg_list)
					next = next->next;
				sb->sb_bg_search_ptr_for_data = next;
			}

			list_del(&node->list);
			break;
		}
	}
	//printf(" found bg = %d %p\n", node->bg_id, node);
	assert(node->bg_id == bg_id);

	/* deallocation of memory */
	spdk_mempool_put(sb->bg_mempool, node);

	sb->sb_bg_list_count--;

	if (!spdk_process_is_primary()) {
		nvfuse_update_sb_with_bd_info(sb, bg_id, root_container, 0/* dec */);
	}

	ret = nvfuse_dealloc_container_from_primary_process(sb, bg_id);
	if (ret < 0)
		return ret;

	//printf(" core %d removes bg %d (total %d)\n", rte_lcore_id(), bg_id, sb->sb_bg_list_count);
	return 0;
}

void nvfuse_print_bg_list(struct nvfuse_superblock *sb)
{
	struct list_head *head = &sb->sb_bg_list;
	struct bg_node *node;

	list_for_each_entry(node, head, list) {
		struct nvfuse_bg_descriptor *bd = NULL;
		struct nvfuse_buffer_head *bd_bh;
		s32 bg_id = node->bg_id;

		bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
		bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
		assert(bd->bd_id == bg_id);
		printf(" bg = %d, free inodes = %d blocks = %d \n", bg_id, bd->bd_free_inodes,
		       bd->bd_free_blocks);
		nvfuse_release_bh(sb, bd_bh, 0, CLEAN);
	}
}

s32 nvfuse_alloc_container_from_primary_process(struct nvfuse_handle *nvh, s32 type)
{
	struct nvfuse_superblock *sb = &nvh->nvh_sb;
	union nvfuse_ipc_msg *ipc_msg;
	struct rte_ring *send_ring, *recv_ring;
	struct rte_mempool *mempool;
	s32 ret;
	s32 container_id;

	/* INITIALIZATION OF TX/RX RING BUFFERS */
	send_ring = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);
	recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

	/* INITIALIZATION OF MEMORY POOL */
	mempool = nvfuse_ipc_mempool(&nvh->nvh_ipc_ctx);
	/*
	* ALLOCATION OF CONTAINER
	*/
	if (rte_mempool_get(mempool, (void **)&ipc_msg) < 0) {
		rte_panic("Failed to get message buffer\n");
		return -1;
	}
	memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
	ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
	ipc_msg->container_alloc_req.type = type;
	{
		u64 start_tsc = spdk_get_ticks();

		/* SEND CONTAINER_ALLOC_REQ TO PRIMARY CORE */
		ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, CONTAINER_ALLOC_REQ);
		if (ret == 0) {
			fprintf(stderr, "Failed to get new container (lcore = %d)\n", rte_lcore_id());
			return 0;
		} else {
			//printf(" allocated container = %d \n", ret);
			container_id = ret;
		}

		sb->perf_stat_ipc.stat_ipc.total_tsc[CONTAINER_ALLOC_REQ] += (spdk_get_ticks() - start_tsc);
		sb->perf_stat_ipc.stat_ipc.total_count[CONTAINER_ALLOC_REQ]++;
	}
	rte_mempool_put(mempool, ipc_msg);

	return container_id;
}

s32 nvfuse_dealloc_container_from_primary_process(struct nvfuse_superblock *sb, u32 bg_id)
{
	struct nvfuse_handle *nvh = sb->sb_nvh;
	struct rte_ring *send_ring, *recv_ring;
	struct rte_mempool *mempool;
	union nvfuse_ipc_msg *ipc_msg;
	s32 ret;

	/* INITIALIZATION OF TX/RX RING BUFFERS */
	send_ring = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);
	recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

	/* initialization of memory pool */
	mempool = nvfuse_ipc_mempool(&nvh->nvh_ipc_ctx);

	/*
	* DEALLOCATION OF CONTAINER
	*/
	if (rte_mempool_get(mempool, (void **)&ipc_msg) < 0) {
		rte_panic("Failed to get message buffer\n");
		return -1;
	}
	memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
	ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
	ipc_msg->container_release_req.container_id = bg_id;
	{
		u64 start_tsc = spdk_get_ticks();

		/* SEND CONTAINER_RELEASE_REQ TO PRIMARY CORE */
		ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, CONTAINER_RELEASE_REQ);
		if (ret < 0) {
			rte_panic("Failed to get new container\n");
			return -1;
		}

		sb->perf_stat_ipc.stat_ipc.total_tsc[CONTAINER_RELEASE_REQ] += (spdk_get_ticks() - start_tsc);
		sb->perf_stat_ipc.stat_ipc.total_count[CONTAINER_RELEASE_REQ]++;
	}

	rte_mempool_put(mempool, ipc_msg);

	return 0;
}

void nvfuse_send_health_check_msg_to_primary_process(struct nvfuse_handle *nvh)
{
	struct rte_ring *send_ring, *recv_ring;
	struct rte_mempool *mempool;
	union nvfuse_ipc_msg *ipc_msg;
	u64 tsc_rate = spdk_get_ticks_hz();
	u64 start_tsc, end_tsc;
	u64 sum_tsc = 0;
	s32 count;
	s32 ret;

	/* INITIALIZATION OF TX/RX RING BUFFERS */
	send_ring = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);
	recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

	/* initialization of memory pool */
	mempool = nvfuse_ipc_mempool(&nvh->nvh_ipc_ctx);
	/*
	* UNREGISTRATION OF HOST ID
	*/
	if (rte_mempool_get(mempool, (void **)&ipc_msg) < 0) {
		rte_panic("Failed to get message buffer\n");
		return;
	}
	memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
	ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
	count = 1000;
	while (count--) {
		start_tsc = spdk_get_ticks();
		/* SEND APP_UNREGISTER_REQ TO PRIMARY CORE */
		ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, HEALTH_CHECK_REQ);
		end_tsc = spdk_get_ticks();
		sum_tsc += (end_tsc - start_tsc);
	}
	if (ret) {
		return;
	}

	printf(" IPC latency = %f usec (from secondary to primary). \n",
	       (double)(sum_tsc / 1000) * 1000 * 1000 / tsc_rate);

	rte_mempool_put(mempool, ipc_msg);
}

s32 nvfuse_mount(struct nvfuse_handle *nvh)
{
	struct nvfuse_superblock *sb;

	void *buf;
	s32 i, res = 0;
	s8 mempool_name[32];
	s32 mempool_size;

	fprintf(stdout, " start %s\n", __FUNCTION__);

	sb = nvfuse_read_super(nvh);

	sb->io_manager = &nvh->nvh_iom;
	sb->io_manager->ipc_ctx = &nvh->nvh_ipc_ctx;
	sb->sb_nvh = nvh;

	if (!spdk_process_is_primary()) {
		nvh->nvh_ipc_ctx.my_channel_id = nvfuse_get_channel_id(&nvh->nvh_ipc_ctx);
		printf(" Obtained Channel ID = %d \n", nvh->nvh_ipc_ctx.my_channel_id);
	}

	{
		s32 type;

		for (type = 0; type < BP_MEMPOOL_NUM; type++) {
			u32 fanout = (CLUSTER_SIZE - BP_NODE_HEAD_SIZE) / (BP_PAIR_SIZE) * 2 + 1;
			sprintf(mempool_name, "nvfuse_bp_%d_%d", type, rte_lcore_id());

			switch (type) {
			case BP_MEMPOOL_MASTER:
				printf(" mempool size for master: %d",
				       (int)(NVFUSE_BPTREE_MEMPOOL_MASTER_TOTAL_SIZE * sizeof(master_node_t)));
				sb->bp_mempool[type] = spdk_mempool_create(mempool_name, NVFUSE_BPTREE_MEMPOOL_MASTER_TOTAL_SIZE,
						       sizeof(master_node_t), NVFUSE_BPTREE_MEMPOOL_MASTER_CACHE_SIZE, SPDK_ENV_SOCKET_ID_ANY);
				break;
			case BP_MEMPOOL_INDEX:
				printf(" mempool size for index: %d",
				       (int)(NVFUSE_BPTREE_MEMPOOL_INDEX_TOTAL_SIZE * sizeof(index_node_t)));
				sb->bp_mempool[type] = spdk_mempool_create(mempool_name, NVFUSE_BPTREE_MEMPOOL_INDEX_TOTAL_SIZE,
						       sizeof(index_node_t), NVFUSE_BPTREE_MEMPOOL_INDEX_CACHE_SIZE, SPDK_ENV_SOCKET_ID_ANY);
				break;
			case BP_MEMPOOL_PAIR:
				printf(" mempool size for pair: %d",
				       (int)(NVFUSE_BPTREE_MEMPOOL_PAIR_TOTAL_SIZE * sizeof(key_pair_t)));
				sb->bp_mempool[type] = spdk_mempool_create(mempool_name, NVFUSE_BPTREE_MEMPOOL_PAIR_TOTAL_SIZE,
						       sizeof(key_pair_t), NVFUSE_BPTREE_MEMPOOL_PAIR_CACHE_SIZE, SPDK_ENV_SOCKET_ID_ANY);
				break;
			case BP_MEMPOOL_KEY:
				printf(" mempool size for key: %d",
				       (int)(NVFUSE_BPTREE_MEMPOOL_TOTAL_SIZE * sizeof(bkey_t) * fanout));
				sb->bp_mempool[type] = spdk_mempool_create(mempool_name, NVFUSE_BPTREE_MEMPOOL_TOTAL_SIZE,
						       sizeof(bkey_t) * fanout, NVFUSE_BPTREE_MEMPOOL_CACHE_SIZE, SPDK_ENV_SOCKET_ID_ANY);
				break;
			case BP_MEMPOOL_VALUE:
				printf(" mempool size for value: %d",
				       (int)(NVFUSE_BPTREE_MEMPOOL_TOTAL_SIZE * sizeof(bitem_t) * fanout));
				sb->bp_mempool[type] = spdk_mempool_create(mempool_name, NVFUSE_BPTREE_MEMPOOL_TOTAL_SIZE,
						       sizeof(bitem_t) * fanout, NVFUSE_BPTREE_MEMPOOL_CACHE_SIZE, SPDK_ENV_SOCKET_ID_ANY);
				break;
			default:
				fprintf(stderr, " Invalid mempool type = %d \n", type);
				assert(0);
			}

			if (sb->bp_mempool[type] == NULL) {
				fprintf(stderr, " Error: allocation of mempool type = %d \n", type);
				exit(0);
			}
			printf(" Allocation of BP_MEMPOOL type = %d %p \n", type, sb->bp_mempool[type]);
		}
	}

	sprintf(mempool_name, "nvfuse_iojob_%d", rte_lcore_id());
	printf(" mempool size for value: %d", (int)(sizeof(struct io_job) * AIO_MAX_QDEPTH * 2));
	sb->io_job_mempool = spdk_mempool_create(mempool_name,
			     (sizeof(struct io_job) * AIO_MAX_QDEPTH * 32),
			     sizeof(struct io_job), 128, SPDK_ENV_SOCKET_ID_ANY);
	if (sb->io_job_mempool == NULL) {
		fprintf(stderr, " Error: allocation of mempool io_jobs \n");
		exit(0);
	}

	{
		s32 buf_size; /* in MB unit */

		if (nvfuse_process_model_is_standalone()) {
			buf_size = nvh->nvh_params.buffer_size;
		} else {
			if (spdk_process_is_primary())
				buf_size = NVFUSE_INITIAL_BUFFER_SIZE_CONTROL;
			else {
				if (nvh->nvh_params.preallocation)
					buf_size = NVFUSE_MAX_BUFFER_SIZE_DATA;
				else
					buf_size = NVFUSE_INITIAL_BUFFER_SIZE_DATA;
			}
		}

		res =  nvfuse_init_buffer_cache(sb, buf_size);
		if (res < 0) {
			printf(" Error: initialization of buffer cache \n");
			return -1;
		}
	}

	res = nvfuse_init_ictx_cache(sb);
	if (res < 0) {
		printf(" Error: initialization of inode context cache \n");
		return -1;
	}

	if (nvh->nvh_mounted) {
		printf(" nvfuse is already mounted.\n");
		return -1;
	}

	sb->sb_file_table = (struct nvfuse_file_table *)spdk_malloc(sizeof(struct nvfuse_file_table) *
			    MAX_OPEN_FILE, 0, NULL);
	if (sb->sb_file_table == NULL) {
		printf(" %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
		return -1;
	}
	memset(sb->sb_file_table, 0x00, sizeof(struct nvfuse_file_table) * MAX_OPEN_FILE);

	if (spdk_process_is_primary() || nvfuse_process_model_is_standalone()) {
		res = nvfuse_scan_superblock(sb);
		if (res < 0) {
			printf(" Invalid signature in superblock !!\n");
			printf(" Please, try mkfs.nvfuse\n");
			return res;
		}
	} else {
		/* App Registration Process*/
		struct rte_ring *send_ring, *recv_ring;
		struct rte_mempool *mempool;
		union nvfuse_ipc_msg *ipc_msg;
		s32 ret;

		/* INITIALIZATION OF TX/RX RING BUFFERS */
		send_ring = nvfuse_ipc_get_sendq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);
		recv_ring = nvfuse_ipc_get_recvq(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

		/* initialization of memory pool */
		mempool = nvfuse_ipc_mempool(&nvh->nvh_ipc_ctx);

		/*
		 * REGISTRATION OF HOST ID
		 */
		if (rte_mempool_get(mempool, (void **)&ipc_msg) < 0) {
			rte_panic("Failed to get message buffer\n");
			return -1;
		}

		memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
		ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
		sprintf(ipc_msg->app_register_req.name, "%s_%d", nvh->nvh_params.appname,
			nvh->nvh_ipc_ctx.my_channel_id);

		printf(" app name = %s (%s_%d) \n", ipc_msg->app_register_req.name, nvh->nvh_params.appname,
		       nvh->nvh_ipc_ctx.my_channel_id);

		/* SEND APP_REGISTER_REQ TO PRIMARY CORE */
		ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, APP_REGISTER_REQ);
		if (ret) {
			return ret;
		}

		memset(ipc_msg->bytes, 0x00, NVFUSE_IPC_MSG_SIZE);
		ipc_msg->chan_id = nvh->nvh_ipc_ctx.my_channel_id;
		sprintf(ipc_msg->superblock_copy_req.name, "%s_%d", nvh->nvh_params.appname,
			nvh->nvh_ipc_ctx.my_channel_id);

		ret = nvfuse_send_msg_to_primary_core(send_ring, recv_ring, ipc_msg, SUPERBLOCK_COPY_REQ);
		if (ret) {
			return ret;
		}

		rte_memcpy(sb, &ipc_msg->superblock_copy_cpl.superblock_common,
			   sizeof(struct nvfuse_superblock_common));

		printf(" Copied superblock info from primary process!\n");
		printf(" no_of_sectors = %ld\n", sb->sb_no_of_sectors);
		printf(" no_of_inodes_per_bg = %d\n", sb->sb_no_of_inodes_per_bg);
		printf(" no_of_blocks_per_bg = %d\n", sb->sb_no_of_blocks_per_bg);
		printf(" no_of_bgs = %d\n", sb->sb_bg_num);
		printf(" free inodes = %d\n", sb->sb_free_inodes);
		printf(" free blocks = %ld\n", sb->sb_free_blocks);
		printf(" App Super Block Info \n");
		printf(" fee inodes = %d\n", sb->asb.asb_free_inodes);
		printf(" free blocks = %ld\n", sb->asb.asb_free_blocks);
		printf(" used blocks = %ld\n", sb->asb.asb_no_of_used_blocks);
		printf(" root bg = %d\n", sb->asb.asb_root_bg_id);

		/* RELEASE MEMORY */
		rte_mempool_put(mempool, ipc_msg);

		assert(sb->asb.asb_root_bg_id != 0);
	}

	sprintf(mempool_name, "nvfuse_bg");
	if (spdk_process_is_primary() || nvfuse_process_model_is_standalone()) {
		mempool_size = sb->sb_bg_num;
		assert(mempool_size);

		printf(" no_of_bgs = %d\n", sb->sb_bg_num);
		printf(" mempool for bg node size = %d \n", (int)(mempool_size * sizeof(struct bg_node)));
		sb->bg_mempool = (struct spdk_mempool *)spdk_mempool_create(mempool_name, mempool_size,
				  sizeof(struct bg_node), 0x10, SPDK_ENV_SOCKET_ID_ANY);
		if (sb->bg_mempool == NULL) {
			fprintf(stderr, " Error: allocaNVFUSE_BUFFER_RATIO_TO_DATAtion of bc mempool \n");
			exit(0);
		}
	} else {
		printf(" mempool for bg node size = %d \n", (int)(mempool_size * sizeof(struct bg_node)));
		sb->bg_mempool = (struct spdk_mempool *)rte_mempool_lookup(mempool_name);
		if (sb->bg_mempool == NULL) {
			fprintf(stderr, " Error: allocation of bc mempool \n");
			exit(0);
		}
	}

	gettimeofday(&sb->sb_last_update, NULL);

	nvfuse_set_cwd_ino(nvh, sb->sb_root_ino);
	nvfuse_set_root_ino(nvh, sb->sb_root_ino);

	gettimeofday(&sb->sb_sync_time, NULL);

	if (!sb->sb_umount) {
		sb->sb_cur_bg = 0;
		sb->sb_next_bg = 0;
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	} else {
		sb->sb_umount = 0;
	}

	sb->sb_bd = (struct nvfuse_bg_descriptor *)spdk_malloc(sizeof(struct nvfuse_bg_descriptor) *
			sb->sb_bg_num, 0, NULL);
	if (sb->sb_bd == NULL) {
		printf("nvfuse_malloc error = %d\n", __LINE__);
		return -1;
	}
	memset(sb->sb_bd, 0x00, sizeof(struct nvfuse_bg_descriptor) * sb->sb_bg_num);

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL) {
		printf(" malloc error \n");
		return NVFUSE_ERROR;
	}

	// load bds in memory
	for (i = 0; i < sb->sb_bg_num; i++) {
		u32 cno = NVFUSE_BD_OFFSET + i * sb->sb_no_of_blocks_per_bg;
		nvfuse_read_cluster(buf, cno, sb->io_manager);
		rte_memcpy(sb->sb_bd + i, buf, sizeof(struct nvfuse_bg_descriptor));
		//printf("b %d ibitmap start = %d \n", i, g_nvfuse_sb->sb_bd[i].bd_ibitmap_start);
		assert(sb->sb_bd[i].bd_id == i);
	}
	nvfuse_free_aligned_buffer(buf);

	/* initilization of bg list */
	INIT_LIST_HEAD(&sb->sb_bg_list);
	sb->sb_bg_list_count = 0;
	sb->sb_bg_search_ptr_for_inode = &sb->sb_bg_list;
	sb->sb_bg_search_ptr_for_data = &sb->sb_bg_list;

	/* Effective for only multiple dataplane model */
	if (spdk_process_is_primary()) {
		/* the primary process makes use of all bgs */
		if (nvfuse_process_model_is_standalone()) {
			s32 bg_id;

			for (bg_id = 0; bg_id < sb->sb_bg_num; bg_id++)
				nvfuse_add_bg(sb, bg_id);
		} else {
			/*add root container */
			nvfuse_add_bg(sb, sb->asb.asb_root_bg_id);
		}
	}
	/* fetch allocated container list, which was already reserved
	   in the previous execution from primary process */
	else {
		s32 container_id;
		s32 bg_count = 0;

		do {
			container_id = nvfuse_alloc_container_from_primary_process(nvh, CONTAINER_ALLOCATED_ALLOC);
			if (container_id) {
				/* insert allocated container to process */
				nvfuse_add_bg(sb, container_id);
			}
			bg_count++;
		} while (container_id);

		if (nvh->nvh_params.preallocation) {
			/* fixed allocation (128G = 128M * 1024) */
			while (bg_count < NVFUSE_CONTAINER_PERALLOCATION_SIZE) {
				container_id = nvfuse_alloc_container_from_primary_process(nvh, CONTAINER_NEW_ALLOC);
				if (container_id) {
					/* insert allocated container to process */
					nvfuse_add_bg(sb, container_id);
				}
				bg_count++;
			}
		}
	}

	/* create b+tree index for root directory at first mount after formattming */
	if (sb->sb_mount_cnt == 0 && spdk_process_is_primary()) {
		struct nvfuse_inode_ctx *root_ictx;

		/* read root inode */
		root_ictx = nvfuse_read_inode(sb, NULL, sb->sb_root_ino);

		/* create bptree related nodes */
		nvfuse_create_bptree(sb, root_ictx->ictx_inode);

		/* mark dirty and copy */
		nvfuse_release_inode(sb, root_ictx, DIRTY);

		/* sync dirty data to storage medium */
		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}
	nvh->nvh_mounted = 1;

	sb->sb_dirty_sync_policy = NVFUSE_META_DIRTY_POLICY;

	switch (sb->sb_dirty_sync_policy) {
	case DIRTY_FLUSH_DELAY:
		printf(" DIRTY_FLUSH_POLICY: DELAY \n");
		break;
	case DIRTY_FLUSH_FORCE:
		printf(" DIRTY_FLUSH_POLICY: FORCE \n");
		break;
	default:
		printf(" DIRTY_FLUSH_POLICY: UNKNOWN\n");
		break;
	}

	gettimeofday(&sb->sb_time_start, NULL);

	if (!spdk_process_is_primary()) {
		nvfuse_send_health_check_msg_to_primary_process(nvh);
	}

	printf(" NVFUSE has been successfully mounted. \n");
	if (0) { /* for debugging */
		struct perf_stat_ipc *stat = &sb->perf_stat_ipc.stat_ipc;
		printf(" Container Alloc Latency = %f us\n",
		       (double)stat->total_tsc[CONTAINER_ALLOC_REQ] / stat->total_count[CONTAINER_ALLOC_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" Container Free Latency = %f us\n",
		       (double)stat->total_tsc[CONTAINER_RELEASE_REQ] / stat->total_count[CONTAINER_RELEASE_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" BUFFER Alloc Latency = %f us\n",
		       (double)stat->total_tsc[BUFFER_ALLOC_REQ] / stat->total_count[BUFFER_ALLOC_REQ] /
		       spdk_get_ticks_hz() * 1000000);
		printf(" BUFFER Free Latency = %f us\n",
		       (double)stat->total_tsc[BUFFER_FREE_REQ] / stat->total_count[BUFFER_FREE_REQ] / spdk_get_ticks_hz()
		       * 1000000);
		memset(stat, 0x00, sizeof(struct perf_stat_ipc));
	}

	return NVFUSE_SUCCESS;
}

s32 nvfuse_umount(struct nvfuse_handle *nvh)
{
	struct nvfuse_superblock *sb;
	s8 *buf;

	if (!nvh->nvh_mounted)
		return -1;

	fprintf(stdout, " start %s\n", __FUNCTION__);

	buf = nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL) {
		printf(" Error: alloc aligned buffer \n");
		return -1;
	}

	sb = nvfuse_read_super(nvh);

	gettimeofday(&sb->sb_time_end, NULL);
	timeval_subtract(&sb->sb_time_total, &sb->sb_time_end, &sb->sb_time_start);

	nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);

	if (spdk_process_is_primary() || nvfuse_process_model_is_standalone()) {
		nvfuse_copy_mem_sb_to_disk_sb((struct nvfuse_superblock *)buf, sb);
		nvfuse_write_cluster(buf, INIT_NVFUSE_SUPERBLOCK_NO, sb->io_manager);
	}

	/* deallocation of mempool for bptree*/
	{
		s32 type;

		for (type = 0; type < BP_MEMPOOL_NUM; type++) {
			spdk_mempool_free(sb->bp_mempool[type]);
			printf(" free bp mempool %d (num = %d)\n", type, BP_MEMPOOL_NUM);
		}

		if (spdk_process_is_primary()) {
			spdk_mempool_free(sb->bg_mempool);
		}

		spdk_mempool_free(sb->io_job_mempool);
	}

	nvfuse_deinit_buffer_cache(sb);
	nvfuse_deinit_ictx_cache(sb);

	if (nvfuse_process_model_is_dataplane()) {
		if (!spdk_process_is_primary()) {
			/* app unregistration with keeping allocated containers permanently */
			nvfuse_send_app_unregister_req(nvh, APP_UNREGISTER_WITHOUT_DESTROYING_CONTAINERS);
			nvfuse_put_channel_id(&nvh->nvh_ipc_ctx, nvh->nvh_ipc_ctx.my_channel_id);

			printf(" Release channel = %d \n", nvh->nvh_ipc_ctx.my_channel_id);
		}

		{
			struct perf_stat_ipc *stat = &sb->perf_stat_ipc.stat_ipc;
			double us_ticks = (double)spdk_get_ticks_hz() / 1000000;

			printf(" Container Alloc Latency = %f us\n", (double)stat->total_tsc[CONTAINER_ALLOC_REQ] /
			       stat->total_count[CONTAINER_ALLOC_REQ] /
			       us_ticks);
			printf(" Container Free Latency = %f us\n", (double)stat->total_tsc[CONTAINER_RELEASE_REQ] /
			       stat->total_count[CONTAINER_RELEASE_REQ] /
			       us_ticks);
			printf(" BUFFER Alloc Latency = %f us\n", (double)stat->total_tsc[BUFFER_ALLOC_REQ] /
			       stat->total_count[BUFFER_ALLOC_REQ] /
			       us_ticks);
			printf(" BUFFER Free Latency = %f us\n", (double)stat->total_tsc[BUFFER_FREE_REQ] /
			       stat->total_count[BUFFER_FREE_REQ] /
			       us_ticks);
		}
	}

	spdk_free(sb->sb_bd);
	spdk_free(sb->sb_file_table);

	nvh->nvh_mounted = 0;

	nvfuse_free_aligned_buffer(buf);

	printf(" NVFUSE has been successfully unmounted. \n");
	return 0;
}


void nvfuse_copy_mem_sb_to_disk_sb(struct nvfuse_superblock *disk_sb,
				   struct nvfuse_superblock *memory_sb)
{
	rte_memcpy(disk_sb, memory_sb, sizeof(struct nvfuse_superblock_common));
}


void nvfuse_copy_disk_sb_to_sb(struct nvfuse_superblock *memory, struct nvfuse_superblock *disk)
{
	rte_memcpy(memory, disk, sizeof(struct nvfuse_superblock_common));
}

s32 nvfuse_is_sb(s8 *buf)
{
	struct nvfuse_superblock *sb = (struct nvfuse_superblock *)buf;

	if (sb->sb_signature == NVFUSE_SB_SIGNATURE)
		return 1;
	else
		return 0;
}

s32 nvfuse_is_bd(s8 *buf)
{
	struct nvfuse_bg_descriptor *bd = (struct nvfuse_bg_descriptor *)buf;

	if (bd->bd_magic == NVFUSE_BD_SIGNATURE)
		return 1;
	else
		return 0;

}

#if NVFUSE_OS == NVFUSE_OS_LINUX
u32 get_part_size(s32 fd)
{
	u32 size, blksize;

#ifndef BLKSSZGET
	blksize = 512;
#else
	ioctl(fd, BLKSSZGET, &blksize);
#endif

	ioctl(fd, BLKGETSIZE, &size);

	return size * blksize;
}
u32 get_sector_size(s32 fd)
{
	u32 sector_size;

#ifndef BLKSSZGET
	sector_size = 512;
#else
	ioctl(fd, BLKSSZGET, &sector_size);
#endif
	return sector_size;
}

u64 get_no_of_sectors(s32 fd)
{
	u64 no_of_sectors;

	ioctl(fd, BLKGETSIZE, &no_of_sectors);

	printf(" no of sectors = %lu\n", no_of_sectors);
	return no_of_sectors;
}
#endif

s32 nvfuse_scan_superblock(struct nvfuse_superblock *cur_sb)
{
	s32 res = -1;
	s8 *buf;
	u64 num_clu, num_sectors, num_bg;
	pbno_t cno;
	struct nvfuse_superblock *read_sb;

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
	num_sectors = NO_OF_SECTORS;
	num_clu = (u32)NVFUSE_NUM_CLU;
#else
#	ifdef __USE_FUSE__
	num_sectors = get_no_of_sectors(cur_sb->io_manager->dev);
	num_clu = (u32)num_sectors / (u32)SECTORS_PER_CLUSTER;
#	else
#		if USE_RAMDISK == 1 || USE_FILEDISK == 1
	num_sectors = NO_OF_SECTORS;
	num_clu = (u32)NVFUSE_NUM_CLU;
#		elif USE_BLKDEVIO == 1
	num_sectors = get_no_of_sectors(cur_sb->io_manager->dev);
	num_clu = num_sectors / (u32)SECTORS_PER_CLUSTER;
#		elif USE_SPDK == 1
	num_sectors = cur_sb->io_manager->total_blkcount;
	num_clu = num_sectors / SECTORS_PER_CLUSTER;
#		endif
#	endif
	num_sectors = cur_sb->io_manager->total_blkcount;
	num_clu = num_sectors / SECTORS_PER_CLUSTER;
	/* FIXME: total_blkcount must be set to when io_manager is initialized. */
#endif

	printf(" sectors = %lu, blocks = %lu\n", (unsigned long)num_sectors, (unsigned long)num_clu);

	num_bg = NVFUSE_BG_NUM(num_clu, NVFUSE_BG_SIZE_BITS - CLUSTER_SIZE_BITS);
	num_clu = num_bg << (NVFUSE_BG_SIZE_BITS -
			      CLUSTER_SIZE_BITS);

	buf = (s8 *)nvfuse_alloc_aligned_buffer(CLUSTER_SIZE);
	if (buf == NULL)	{
		printf(" %s:%d: nvfuse_malloc error \n", __FUNCTION__, __LINE__);
	}

	memset(buf, 0x00, CLUSTER_SIZE);
	cno = INIT_NVFUSE_SUPERBLOCK_NO;

	nvfuse_read_cluster(buf, cno, cur_sb->io_manager);
	read_sb = (struct nvfuse_superblock *)buf;

	if (read_sb->sb_signature == NVFUSE_SB_SIGNATURE) {
		nvfuse_copy_disk_sb_to_sb(cur_sb, read_sb);
		res = 0;
	} else {
		printf(" super block signature is mismatched. \n");
		res = -1;
	}

	printf(" root ino = %d \n", cur_sb->sb_root_ino);
	printf(" no of sectors = %ld \n", (unsigned long)cur_sb->sb_no_of_sectors);
	printf(" no of blocks = %ld \n", (unsigned long)cur_sb->sb_no_of_blocks);
	printf(" no of used blocks = %ld \n", (unsigned long)cur_sb->sb_no_of_used_blocks);
	printf(" no of inodes per bg = %d \n", cur_sb->sb_no_of_inodes_per_bg);
	printf(" no of blocks per bg = %d \n", cur_sb->sb_no_of_blocks_per_bg);
	printf(" no of free inodes = %d \n", cur_sb->sb_free_inodes);
	printf(" no of free blocks = %ld \n", (unsigned long)cur_sb->sb_free_blocks);

	nvfuse_free_aligned_buffer(buf);
	return res;
}

u32 nvfuse_create_bptree(struct nvfuse_superblock *sb, struct nvfuse_inode *inode)
{
	master_node_t *master;
	int ret;

	/* make b+tree master and root nodes */
	master = bp_init_master(sb);
	ret = bp_alloc_master(sb, master);
	if (ret < 0) {
		return -1;
	}

	bp_init_root(master);
	bp_write_master(master);

	/* update bptree inode */
	inode->i_bpino = master->m_ino;
	/* deinit b+tree memory structure */
	bp_deinit_master(master);

	return 0;
}

s32 nvfuse_dir(struct nvfuse_handle *nvh)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_superblock *sb;
	s64 read_bytes;
	s64 dir_size;

	sb = nvfuse_read_super(nvh);

	dir_ictx = nvfuse_read_inode(sb, NULL, nvfuse_get_cwd_ino(nvh));
	dir_inode = dir_ictx->ictx_inode;

	dir_size = dir_inode->i_size;

	for (read_bytes = 0; read_bytes < dir_size ; read_bytes += DIR_ENTRY_SIZE) {
		if (!(read_bytes & (CLUSTER_SIZE - 1))) {
			if (dir_bh != NULL) {
				nvfuse_release_bh(sb, dir_bh, 0, 0);
				dir_bh = NULL;
			}
			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(read_bytes), READ,
					       NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
			inode = ictx->ictx_inode;

			nvfuse_print_inode(inode, dir->d_filename);

			nvfuse_release_inode(sb, ictx, CLEAN);
		}
		dir++;
	}

	nvfuse_release_bh(sb, dir_bh, 0, 0);
	nvfuse_release_inode(sb, dir_ictx, CLEAN);
	nvfuse_release_super(sb);

	printf("\nfree blocks	  = %ld, num  blocks  = %ld\n", (unsigned long)sb->sb_free_blocks,
	       (unsigned long)sb->sb_no_of_blocks);
	printf("Disk Util     = %2.2f %%\n",
	       ((double)(sb->sb_no_of_blocks - sb->sb_free_blocks) / (double)sb->sb_no_of_blocks) * 100);

	return NVFUSE_SUCCESS;
}


s32 nvfuse_allocate_open_file_table(struct nvfuse_superblock *sb)
{
	s32 i = 0, fid = -1;

	for (i = START_OPEN_FILE; i < MAX_OPEN_FILE; i++) {
		if (sb->sb_file_table[i].used == FALSE) {
			fid = i;
			break;
		}
	}

	return fid;
}

s32 nvfuse_truncate(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename,
		    nvfuse_off_t trunc_size)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;

	s64 read_bytes = 0;
	s64 start = 0;
	u32 offset = 0;
	s64 dir_size = 0;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;
	dir_size = dir_inode->i_size;

#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) {
		printf(" fixme: filename (%s) is not in the index.\n", filename);
		offset = 0;
	}
#endif

	start = (s64)offset * DIR_ENTRY_SIZE;
	if ((start & (CLUSTER_SIZE - 1))) {
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(start), READ,
				       NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) {
		if (!(read_bytes & (CLUSTER_SIZE - 1))) {
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0/*tail*/, 0/*dirty*/);

			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(read_bytes), READ,
					       NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			if (!strcmp(dir->d_filename, filename)) {
				ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
				inode = ictx->ictx_inode;
				break;
			}
		}
		dir++;
	}

	if (inode == NULL || inode->i_ino == 0) {
		printf(" file (%s) is not found this directory\n", filename);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
		return NVFUSE_ERROR;
	}

	if (inode->i_type == NVFUSE_TYPE_DIRECTORY) {
		return error_msg(" rmfile() is supported for a file.");
	}

	nvfuse_free_inode_size(sb, ictx, trunc_size);
	inode->i_size = trunc_size;
	assert(inode->i_size < MAX_FILE_SIZE);
	nvfuse_release_inode(sb, ictx, DIRTY);

	nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
	nvfuse_release_inode(sb, dir_ictx, CLEAN);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);
	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_truncate_ino(struct nvfuse_superblock *sb, inode_t ino, s64 trunc_size)
{
	struct nvfuse_inode_ctx *ictx;

	ictx = nvfuse_read_inode(sb, NULL, ino);
	nvfuse_free_inode_size(sb, ictx, trunc_size);
	nvfuse_release_inode(sb, ictx, DIRTY);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_chmod(struct nvfuse_handle *nvh, inode_t par_ino, s8 *filename, mode_t mode)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	u64 start = 0;
	s64 read_bytes = 0;
	u32 offset = 0;
	s32 mask;
	s64 dir_size = 0;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;

#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) {
		printf(" The smae file eixts = %s \n", filename);
	}
#endif

	dir_size = dir_inode->i_size;
	start = (s64)offset * DIR_ENTRY_SIZE;

	if (start & (CLUSTER_SIZE - 1)) {
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(start), READ,
				       NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) {
		if (!(read_bytes & (CLUSTER_SIZE - 1))) {
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0, 0);

			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(read_bytes), READ,
					       NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			if (!strcmp(dir->d_filename, filename)) {
				ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
				inode = ictx->ictx_inode;
				break;
			}
		}
		dir++;
	}

	if (inode->i_ino == 0)
		return NVFUSE_ERROR;

	mask = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
	inode->i_mode = (inode->i_mode & ~mask) | (mode & mask);

	nvfuse_release_inode(sb, ictx, DIRTY);

	nvfuse_release_bh(sb, dir_bh, 0, CLEAN);
	nvfuse_release_inode(sb, dir_ictx, CLEAN);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_is_directio(struct nvfuse_superblock *sb, s32 fid)
{
	struct nvfuse_file_table *ft;

	ft = sb->sb_file_table + fid;

	if (ft->flags & O_DIRECT)
		return 1;

	return 0;
}

s32 nvfuse_path_open(struct nvfuse_handle *nvh, s8 *path, s8 *filename,
		     struct nvfuse_dir_entry *get)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_superblock *sb = nvfuse_read_super(nvh);
	s8 *token;
	s8 b[256];
	u32 local_dir_ino;

	strcpy(b, path);

	if (path[0] == '/')
		local_dir_ino = nvfuse_get_root_ino(nvh);
	else
		local_dir_ino = nvfuse_get_cwd_ino(nvh);

	token = strtok(b, "/");

	if (token != NULL) {
		if (nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino) < 0) {
			return NVFUSE_ERROR;
		}

		rte_memcpy(get, &dir_entry, DIR_ENTRY_SIZE);

		while ((token = strtok(NULL, "/")) != NULL) {
			local_dir_ino = dir_entry.d_ino;
			if (nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino) < 0)
				return NVFUSE_ERROR;
			rte_memcpy(get, &dir_entry, DIR_ENTRY_SIZE);
		}
	} else if (token == NULL) {
		get->d_ino = local_dir_ino;
	}

	nvfuse_release_super(sb);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_path_open2(struct nvfuse_handle *nvh, s8 *path, s8 *filename,
		      struct nvfuse_dir_entry *get)
{
	struct nvfuse_superblock *sb;
	struct nvfuse_dir_entry dir_entry;
	s8 *token;
	s8 b[256];
	u32 local_dir_ino;
	s32 i;
	s32 count = 0;
	s32 res = NVFUSE_SUCCESS;

	sb = nvfuse_read_super(nvh);

	strcpy(b, path);
	i = strlen(b);

	if (b[i - 1] == '/') {
		res = NVFUSE_ERROR;
		goto RES;
	}

	while (b[--i] != '/' && i > 1);

	if (b[i] == '/')
		b[i] = '\0';

	if (path[0] == '/')
		local_dir_ino = nvfuse_get_root_ino(nvh);
	else
		local_dir_ino = nvfuse_get_cwd_ino(nvh);

	for (count = 0, i = 0; i < strlen(path); i++)
		if (path[i] == '/')
			count++;

	if (count == 0) {
		get->d_ino = local_dir_ino;
		goto RES;
	}

	token = strtok(b, "/");
	if (token == NULL) {
		get->d_ino = local_dir_ino;
	} else if (token != NULL) {
		if (nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino) < 0) {
			//not found
			res = NVFUSE_ERROR;
			goto RES;
		}

		rte_memcpy(get, &dir_entry, DIR_ENTRY_SIZE);

		while ((token = strtok(NULL, "/")) != NULL) {
			local_dir_ino = dir_entry.d_ino;

			if (nvfuse_lookup(sb, NULL, &dir_entry, token, local_dir_ino) < 0)
				return NVFUSE_ERROR;
			rte_memcpy(get, &dir_entry, DIR_ENTRY_SIZE);
		}
	}

RES:
	;

	nvfuse_release_super(sb);

	return res;
}

s32 nvfuse_lseek(struct nvfuse_handle *nvh, s32 fd, u32 offset, s32 position)
{
	struct nvfuse_file_table *of;
	struct nvfuse_superblock *sb;

	sb = nvfuse_read_super(nvh);

	of = sb->sb_file_table + fd;

	if (position == SEEK_SET)             /* SEEK_SET */
		of->rwoffset = offset;
	else if (position == SEEK_CUR)        /* SEEK_CUR */
		of->rwoffset += offset;
	else if (position == SEEK_END)        /* SEEK_END */
		of->rwoffset = of->size - offset;

	nvfuse_release_super(sb);

	return (NVFUSE_SUCCESS);
}

s32 nvfuse_seek(struct nvfuse_superblock *sb, struct nvfuse_file_table *of, s64 offset, s32 position)
{
	if (position == SEEK_SET)             /* SEEK_SET */
		of->rwoffset = offset;
	else if (position == SEEK_CUR)        /* SEEK_CUR */
		of->rwoffset += offset;
	else if (position == SEEK_END)        /* SEEK_END */
		of->rwoffset = of->size - offset;

	return (NVFUSE_SUCCESS);
}

u32 nvfuse_alloc_dbitmap(struct nvfuse_superblock *sb, u32 bg_id, u32 *alloc_blks, u32 num_blocks)
{
	struct nvfuse_bg_descriptor *bd;
	struct nvfuse_buffer_head *bd_bh, *bh;
	u32 free_block = 0;
	u32 cnt = 0;
	void *buf;
	u32 flag = 0;
	u32 alloc_cnt = 0;

	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;

	bh = nvfuse_get_bh(sb, NULL, DBITMAP_INO, bg_id, READ, NVFUSE_TYPE_META);
	buf = bh->bh_buf;

	free_block = bd->bd_next_block % sb->sb_no_of_blocks_per_bg;

	while (cnt++ < sb->sb_no_of_blocks_per_bg) {
		if (!free_block) {
			free_block = bd->bd_dtable_start % sb->sb_no_of_blocks_per_bg;
			cnt += (bd->bd_dtable_start % sb->sb_no_of_blocks_per_bg - 1);
		}

		if (!ext2fs_test_bit(free_block, buf)) {
			//printf(" free block %d found \n", free_block);
			bd->bd_next_block = free_block; // keep track of hit information to quickly lookup free blocks.
			ext2fs_set_bit(free_block, buf); // right approach?
			flag = 1;

			*alloc_blks = bd->bd_bg_start + free_block;
			alloc_blks++;
			num_blocks--;
			alloc_cnt++;
			if (num_blocks == 0)
				break;
		}
		free_block = (free_block + 1) % sb->sb_no_of_blocks_per_bg;
	}

	if (flag) {
		nvfuse_release_bh(sb, bh, 0, DIRTY);
		nvfuse_release_bh(sb, bd_bh, 0, DIRTY);
#if 0
		if (spdk_process_is_primary()) {
			printf(" allocated bgid = %d blks = %d\n", bg_id, alloc_cnt);
		}
#endif
		nvfuse_dec_free_blocks(sb, bd->bd_bg_start + free_block, alloc_cnt);

		return alloc_cnt;
	}

	nvfuse_release_bh(sb, bh, 0, CLEAN);
	nvfuse_release_bh(sb, bd_bh, 0, CLEAN);
	return 0;
}

u32 nvfuse_free_dbitmap(struct nvfuse_superblock *sb, u32 bg_id, nvfuse_loff_t offset, u32 count)
{
	struct nvfuse_bg_descriptor *bd;
	struct nvfuse_buffer_head *bd_bh, *bh;
	void *buf;
	int flag = 0;
	int i;

	bd_bh = nvfuse_get_bh(sb, NULL, BD_INO, bg_id, READ, NVFUSE_TYPE_META);
	if (bd_bh == NULL) {
		printf(" Error: get_bh\n");
		assert(0);
	}

	bd = (struct nvfuse_bg_descriptor *)bd_bh->bh_buf;
	assert(bd->bd_id == bg_id);

	bh = nvfuse_get_bh(sb, NULL, DBITMAP_INO, bg_id, READ, NVFUSE_TYPE_META);
	if (bh == NULL) {
		printf(" Error: get_bh\n");
		assert(0);
	}
	buf = bh->bh_buf;

	for (i = 0; i < count; i++) {
		if (ext2fs_test_bit(offset + i, buf)) {
			ext2fs_clear_bit(offset + i, buf);

			/* keep track of hit information to quickly lookup free blocks. */
			bd->bd_next_block = offset;
			flag = 1;
		} else {
			printf(" ERROR: block was already cleared. ");
			assert(0);
		}
	}

	if (flag) {
		nvfuse_release_bh(sb, bh, 0, DIRTY);
		nvfuse_release_bh(sb, bd_bh, 0, DIRTY);
		nvfuse_inc_free_blocks(sb, bd->bd_bg_start + offset, count);
	} else {
		nvfuse_release_bh(sb, bh, 0, CLEAN);
		nvfuse_release_bh(sb, bd_bh, 0, CLEAN);
	}
	return 0;
}

s32 nvfuse_link(struct nvfuse_superblock *sb, u32 newino, s8 *new_filename, s32 ino)
{
	struct nvfuse_dir_entry *dir;
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode, *inode;
	struct nvfuse_buffer_head *dir_bh = NULL;
	s32 search_lblock = 0, search_entry = 0;
	u32 empty_dentry;

	if (strlen(new_filename) < 1 || strlen(new_filename) >= FNAME_SIZE)
		return error_msg("mkdir [dir name]\n");

	if (!nvfuse_lookup(sb, NULL, NULL, new_filename, newino)) {
		printf(" file exists = %s\n", new_filename);
		return -1;
	}

	dir_ictx = nvfuse_read_inode(sb, NULL, newino);
	dir_inode = dir_ictx->ictx_inode;

	if (dir_inode->i_links_count == MAX_FILES_PER_DIR) {
		printf(" The number of files exceeds %d\n", MAX_FILES_PER_DIR);
		return -1;
	}

	/* find an empty directory */
	empty_dentry = nvfuse_find_empty_dentry(sb, dir_ictx, dir_inode);
	if (empty_dentry < 0) {
		return -1;
	}
	search_lblock = dir_inode->i_ptr / DIR_ENTRY_NUM;
	search_entry = dir_inode->i_ptr % DIR_ENTRY_NUM;

	dir_inode->i_ptr = search_lblock * DIR_ENTRY_NUM + search_entry;
	dir_inode->i_links_count++;

	ictx = nvfuse_read_inode(sb, NULL, ino);
	inode = ictx->ictx_inode;
	inode->i_links_count++;

	dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ, NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir[search_entry].d_flag = DIR_USED;
	dir[search_entry].d_ino = ino;
	dir[search_entry].d_version = inode->i_version;
	strcpy(dir[search_entry].d_filename, new_filename);

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_set_dir_indexing(sb, dir_inode, new_filename, dir_inode->i_ptr);
#endif

	nvfuse_release_bh(sb, dir_bh, 0, DIRTY);

	nvfuse_release_inode(sb, dir_ictx, DIRTY);
	nvfuse_release_inode(sb, ictx, DIRTY);

	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

	return NVFUSE_SUCCESS;
}

s32 nvfuse_find_empty_dentry(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *dir_ictx, struct nvfuse_inode *dir_inode)
{
	struct nvfuse_buffer_head *dir_bh = NULL;
	struct nvfuse_dir_entry *dir;
	u32 search_lblock, search_entry;
	u32 dir_num;
	s32 num_block;
	u32 new_entry, flag = 0;
	s32 i;

	search_lblock = (dir_inode->i_links_count - 1) / DIR_ENTRY_NUM;
	search_entry = (dir_inode->i_links_count - 1) % DIR_ENTRY_NUM;

RETRY:

	dir_num = (dir_inode->i_size / DIR_ENTRY_SIZE);
	if (dir_num == dir_inode->i_links_count) {
		search_entry = -1;
		num_block = 0;
	} else {
		if (search_entry == DIR_ENTRY_NUM - 1) {
			search_entry = -1;
		}
		num_block = dir_num / DIR_ENTRY_NUM;
	}

	// find an empty dentry
	for (i = 0; i < num_block; i++) {
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ, NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;

		for (new_entry = 0; new_entry < DIR_ENTRY_NUM; new_entry++) {
			search_entry++;
			if (search_entry == DIR_ENTRY_NUM) {
				search_entry = 0;
			}

			if (nvfuse_dir_is_invalid(dir + search_entry)) {
				flag = 1;
				goto FIND;
			}
		}
		nvfuse_release_bh(sb, dir_bh, 0, 0);
		dir_bh = NULL;
		search_entry = -1;
		search_lblock++;
		if (search_lblock == NVFUSE_SIZE_TO_BLK(dir_inode->i_size))
			search_lblock = 0;
	}

	dir_num = (dir_inode->i_size / DIR_ENTRY_SIZE);
	num_block = dir_num / DIR_ENTRY_NUM;
	search_lblock = num_block;

	if (!flag) { // allocate new direcct block
		s32 ret;
		nvfuse_release_bh(sb, dir_bh, 0, 0);
		ret = nvfuse_get_block(sb, dir_ictx, NVFUSE_SIZE_TO_BLK(dir_inode->i_size), 1/* num block */, NULL,
				       NULL, 1);
		if (ret) {
			printf(" data block allocation fails.");
			return NVFUSE_ERROR;
		}

		dir_bh = nvfuse_get_new_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(dir_inode->i_size),
					   NVFUSE_TYPE_META);
		nvfuse_release_bh(sb, dir_bh, INSERT_HEAD, DIRTY);
		assert(dir_inode->i_size < MAX_FILE_SIZE);
		dir_inode->i_size += CLUSTER_SIZE;
		goto RETRY;
	}

FIND:

	return search_lblock * DIR_ENTRY_NUM + search_entry;
}


s32 nvfuse_find_existing_dentry(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *dir_ictx, struct nvfuse_inode *dir_inode, s8 *filename)
{
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;

	s64 read_bytes = 0;
	s64 start = 0;
	u32 offset = 0;
	s64 dir_size = 0;
	u32 found_entry = -1;

#if NVFUSE_USE_DIR_INDEXING == 1
	if (nvfuse_get_dir_indexing(sb, dir_inode, filename, &offset) < 0) {
		printf(" dir (%s) is not in the index.\n", filename);
		offset = 0;
		/* linear search */
	} else {
		/* quick search */
	}
#endif

	dir_size = dir_inode->i_size;
	start = (s64)offset * DIR_ENTRY_SIZE;
	if ((start & (CLUSTER_SIZE - 1))) {
		dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(start), READ,
				       NVFUSE_TYPE_META);
		dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		dir += (offset % DIR_ENTRY_NUM);
	}

	for (read_bytes = start; read_bytes < dir_size; read_bytes += DIR_ENTRY_SIZE) {
		if (!(read_bytes & (CLUSTER_SIZE - 1))) {
			if (dir_bh)
				nvfuse_release_bh(sb, dir_bh, 0/*tail*/, 0/*dirty*/);
			dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, NVFUSE_SIZE_TO_BLK(read_bytes), READ,
					       NVFUSE_TYPE_META);
			dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
		}

		if (dir->d_flag == DIR_USED) {
			if (!strcmp(dir->d_filename, filename)) {
				found_entry = read_bytes / DIR_ENTRY_SIZE;
				nvfuse_release_bh(sb, dir_bh, 0/*tail*/, 0/*dirty*/);
				break;
			}
		}
		dir++;
	}

	return found_entry;
}


s32 nvfuse_rm_direntry(struct nvfuse_superblock *sb, inode_t par_ino, s8 *name, u32 *ino)
{
	struct nvfuse_inode_ctx *dir_ictx, *ictx;
	struct nvfuse_inode *dir_inode = NULL;
	struct nvfuse_inode *inode = NULL;
	struct nvfuse_dir_entry *dir = NULL;
	struct nvfuse_buffer_head *dir_bh = NULL;
	u32 found_entry;
	u32 search_lblock, search_entry;

	dir_ictx = nvfuse_read_inode(sb, NULL, par_ino);
	dir_inode = dir_ictx->ictx_inode;

	/* find an existing dentry */
	found_entry = nvfuse_find_existing_dentry(sb, dir_ictx, dir_inode, name);
	if (found_entry < 0)
		return 0;

	search_lblock = found_entry / DIR_ENTRY_NUM;
	search_entry = found_entry % DIR_ENTRY_NUM;
	dir_bh = nvfuse_get_bh(sb, dir_ictx, dir_inode->i_ino, search_lblock, READ,
						   NVFUSE_TYPE_META);
	dir = (struct nvfuse_dir_entry *)dir_bh->bh_buf;
	dir += search_entry;

	ictx = nvfuse_read_inode(sb, NULL, dir->d_ino);
	inode = ictx->ictx_inode;

	if (inode == NULL || inode->i_ino == 0) {
		printf(" file (%s) is not found this directory\n", name);
		nvfuse_release_bh(sb, dir_bh, 0/*tail*/, CLEAN);
		return NVFUSE_ERROR;
	}

	if (ino)
		*ino = dir->d_ino;

	/* link count decrement */
	inode->i_links_count--;

#if NVFUSE_USE_DIR_INDEXING == 1
	nvfuse_del_dir_indexing(sb, dir_inode, name);
#endif

	dir->d_flag = DIR_DELETED;

	nvfuse_release_bh(sb, dir_bh, 0, DIRTY);
	nvfuse_release_inode(sb, dir_ictx, DIRTY);

	nvfuse_release_inode(sb, ictx, DIRTY);
	nvfuse_check_flush_dirty(sb, sb->sb_dirty_sync_policy);

	return 0;
}


u32 nvfuse_get_pbn(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino,
		   lbno_t offset)
{
	bitem_t value = 0;
	int ret;

	//printf(" %s ino = %d, offset = %d, res = %d \n", __FUNCTION__, ino, offset, value);

	if (ino < ROOT_INO) {
		printf(" Received invalid ino = %d", ino);
		assert(0);
		return 0;
	}

	switch (ino) {
	case BLOCK_IO_INO: // direct translation lblk to pblk
		return offset;
	case ITABLE_INO: {
		u32 bg_id = offset / (sb->sb_no_of_inodes_per_bg / INODE_ENTRY_NUM);
		struct nvfuse_bg_descriptor *bd = nvfuse_get_bd(sb, bg_id);
		value = bd->bd_itable_start + (offset % bd->bd_itable_size);
		return value;
	}
	case DBITMAP_INO: {
		u32 bg_id = offset;
		struct nvfuse_bg_descriptor *bd = nvfuse_get_bd(sb, bg_id);
		value = bd->bd_dbitmap_start;
		return value;
	}
	case IBITMAP_INO: {
		u32 bg_id = offset;
		struct nvfuse_bg_descriptor *bd = nvfuse_get_bd(sb, bg_id);
		value = bd->bd_ibitmap_start;
		return value;
	}
	case BD_INO: {
		value = offset * NVFUSE_CLU_P_BG(sb);
		value += NVFUSE_BD_OFFSET;
		return value;
	}
	default:
		;
	}

	assert(ictx->ictx_ino == ino);

	ret = nvfuse_get_block(sb, ictx, offset, 1/* num block */, NULL, &value, 0);
	if (ret) {
		printf(" Warning: block is not allocated.");
	}

	return value;
}

s32 error_msg(s8 *str)
{
	printf("ERROR_MSG : %s \n", str);
	return NVFUSE_ERROR;
}

void nvfuse_dir_hash(s8 *filename, u32 *hash1, u32 *hash2)
{
#ifdef USE_INTEL_CRC32C
	s32 size = strlen(filename);
	s32 half = size / 2;

	*hash1 = crc32c_intel((unsigned char *)filename, half);
	*hash2 = crc32c_intel((unsigned char *)filename + half, size - half);
#else
	ext2fs_dirhash(EXT2_HASH_TEA, filename, strlen(filename), 0, hash1, hash2);
#endif
}

int nvfuse_read_block(char *buf, unsigned long block, struct nvfuse_io_manager *io_manager)
{
	return nvfuse_read_cluster(buf, block, io_manager);
}

void nvfuse_check_flush_dirty(struct nvfuse_superblock *sb, s32 force)
{
	struct list_head *dirty_head, *flushing_head;
	struct list_head *temp, *ptr;
	struct nvfuse_buffer_cache *bc;
	s32 dirty_count = 0;
	s32 flushing_count = 0;
	s32 res;
	u64 start_tsc;

	if (spdk_process_is_primary() && nvfuse_process_model_is_dataplane()) {
		force = DIRTY_FLUSH_FORCE;
	}

	dirty_count = nvfuse_get_dirty_count(sb);
	/* check dirty flush with force option */
	if (force != DIRTY_FLUSH_FORCE && dirty_count < NVFUSE_SYNC_DIRTY_COUNT)
		goto RES;

	/* no more dirty data */
	if (dirty_count == 0)
		goto RES;

	// if (!spdk_process_is_primary())
	// {
	// 	printf(" flush dirty = %d \n", dirty_count);
	// }

	start_tsc = spdk_get_ticks();

	while ((dirty_count = nvfuse_get_dirty_count(sb)) != 0) {
		dirty_head = &sb->sb_bm->bm_list[BUFFER_TYPE_DIRTY];
		flushing_head = &sb->sb_bm->bm_list[BUFFER_TYPE_FLUSHING];
		flushing_count = 0;

		list_for_each_safe(ptr, temp, dirty_head) {
			bc = (struct nvfuse_buffer_cache *)list_entry(ptr, struct nvfuse_buffer_cache, bc_list);
			assert(bc->bc_dirty);
			list_move(&bc->bc_list, flushing_head);
			flushing_count++;
			if (flushing_count >= AIO_MAX_QDEPTH)
				break;
		}

		res = nvfuse_sync_dirty_data(sb, flushing_head, flushing_count);
		if (res)
			break;
	}

	/* flush cmd to nvme ssd */
	nvfuse_dev_flush(sb->io_manager);

	sb->nvme_io_tsc += (spdk_get_ticks() - start_tsc);
	sb->nvme_io_count ++;

	/* FIXME: it is necessary to analyze why dirties are left here. */
	//if (sb->sb_ictxc->ictxc_list_count[BUFFER_TYPE_DIRTY])
	//	printf(" Warning: inode dirty count = %d \n", sb->sb_ictxc->ictxc_list_count[BUFFER_TYPE_DIRTY]);
RES:
	;

	return;
}

struct nvfuse_superblock *nvfuse_read_super(struct nvfuse_handle *nvh)
{
	return &nvh->nvh_sb;
}

void nvfuse_release_super(struct nvfuse_superblock *sb)
{
	return;
}

s32 nvfuse_dir_is_invalid(struct nvfuse_dir_entry *dir)
{
	if (dir->d_flag == DIR_EMPTY || dir->d_flag == DIR_DELETED)
		return 1;

	return 0;
}

void nvfuse_print_inode(struct nvfuse_inode *inode, s8 *str)
{
	if (inode->i_type == NVFUSE_TYPE_DIRECTORY) {
		printf("%-13s/ [%ld] ino : %3d\n", str, (long)inode->i_size, inode->i_ino);
	} else if (inode->i_type == NVFUSE_TYPE_FILE) {
		printf("%-14s [%ld] ino : %3d\n", str, (long)inode->i_size, inode->i_ino);
	}
}

struct nvfuse_bg_descriptor *nvfuse_get_bd(struct nvfuse_superblock *sb, u32 bg_id)
{
	assert(bg_id == sb->sb_bd[bg_id].bd_id);
	return sb->sb_bd + bg_id;
}
