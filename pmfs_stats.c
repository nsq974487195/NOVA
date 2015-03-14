#include "pmfs.h"

const char *Timingstring[TIMING_NUM] = 
{
	"ioremap",
	"xip_read",
	"cow_write",
	"xip_write",
	"xip_write_fast",
	"memcpy_read",
	"memcpy_write",
	"logging",
	"new_meta_blocks",
	"new_data_blocks",
	"assign_blocks",
	"free_data_blocks",
	"free_meta_blocks",
	"evict_inode",
	"malloc_test",
};

unsigned long long Timingstats[TIMING_NUM];
u64 Countstats[TIMING_NUM];
unsigned long alloc_steps;
unsigned long free_steps;
unsigned long write_breaks;

void pmfs_print_blocknode_list(struct super_block *sb)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct list_head *head = &(sbi->block_inuse_head);
	struct pmfs_blocknode *i;
	unsigned long count = 0;

	printk("=========== PMFS blocknode stats ===========\n");
	mutex_lock(&sbi->s_lock);
	list_for_each_entry(i, head, link) {
		count++;
		pmfs_dbg_verbose("node low %lu, high %lu, size %lu\n",
			i->block_low, i->block_high,
			i->block_high - i->block_low + 1);
	}
	mutex_unlock(&sbi->s_lock);
	printk("All: %lu nodes\n", count);
	printk("alloc %llu, alloc steps %lu, average %llu\n",
		Countstats[new_data_blocks_t], alloc_steps,
		Countstats[new_data_blocks_t] ?
			alloc_steps / Countstats[new_data_blocks_t] : 0);
	printk("free %llu, free steps %lu, average %llu\n",
		Countstats[free_data_t], free_steps,
		Countstats[free_data_t] ?
			free_steps / Countstats[free_data_t] : 0);
	printk("write %llu, write breaks %lu, average %llu\n",
		Countstats[cow_write_t], write_breaks,
		Countstats[cow_write_t] ?
			write_breaks / Countstats[cow_write_t] : 0);
}

void pmfs_print_timing_stats(struct super_block *sb)
{
	int i;

	printk("======== PMFS kernel timing stats ========\n");
	for (i = 0; i < TIMING_NUM; i++) {
		if (measure_timing) {
			printk("%s: count %llu, timing %llu, average %llu\n",
				Timingstring[i],
				Countstats[i],
				Timingstats[i],
				Countstats[i] ?
				Timingstats[i] / Countstats[i] : 0);
		} else {
			printk("%s: count %llu\n",
				Timingstring[i],
				Countstats[i]);
		}
	}

	pmfs_print_blocknode_list(sb);
}

void pmfs_clear_stats(void)
{
	int i;

	printk("======== Clear PMFS kernel timing stats ========\n");
	for (i = 0; i < TIMING_NUM; i++) {
		Countstats[i] = 0;
		Timingstats[i] = 0;
	}
}

void pmfs_print_inode_log(struct super_block *sb, struct inode *inode)
{
	struct pmfs_inode *pi;
	size_t entry_size = sizeof(struct pmfs_inode_entry);
	u64 curr;

	pi = pmfs_get_inode(sb, inode->i_ino);
	if (pi->log_tail == 0)
		return;

	curr = pi->log_head;
	pmfs_dbg("Pi %lu: log head block @ %llu, tail @ block %llu, %llu\n",
			inode->i_ino, curr >> PAGE_SHIFT,
			pi->log_tail >> PAGE_SHIFT, pi->log_tail);
	while (curr != pi->log_tail) {
		if ((curr & (PAGE_SIZE - 1)) == LAST_ENTRY) {
			struct pmfs_inode_page_tail *tail =
					pmfs_get_block(sb, curr);
			pmfs_dbg("Log tail. Next page @ block %llu\n",
					tail->next_page >> PAGE_SHIFT);
			curr = tail->next_page;
		} else {
			struct pmfs_inode_entry *entry =
					pmfs_get_block(sb, curr);
			pmfs_dbg("entry @ %llu: offset %u, size %u, "
				"blocknr %llu, invalid count %llu\n",
				(curr & (PAGE_SIZE - 1)) / entry_size,
				entry->pgoff, entry->num_pages,
				entry->block >> PAGE_SHIFT,
				GET_INVALID(entry->block));
			curr += entry_size;
		}
	}
}

void pmfs_print_inode_log_page(struct super_block *sb, struct inode *inode)
{
	struct pmfs_inode *pi;
	struct pmfs_inode_log_page *curr_page;
	u64 curr, next;
	int count = 1;

	pi = pmfs_get_inode(sb, inode->i_ino);
	if (pi->log_tail == 0)
		return;

	curr = pi->log_head;
	pmfs_dbg("Pi %lu: log head block @ %llu, tail @ block %llu, %llu\n",
			inode->i_ino, curr >> PAGE_SHIFT,
			pi->log_tail >> PAGE_SHIFT, pi->log_tail);
	curr_page = (struct pmfs_inode_log_page *)pmfs_get_block(sb, curr);
	while ((next = curr_page->page_tail.next_page) != 0) {
		pmfs_dbg("Current page %llu, next page %llu\n",
			curr >> PAGE_SHIFT, next >> PAGE_SHIFT);
		curr = next;
		curr_page = (struct pmfs_inode_log_page *)
			pmfs_get_block(sb, curr);
		count++;
	}
	pmfs_dbg("Pi %lu: log has %d pages\n", inode->i_ino, count);
}

void pmfs_print_inode_log_blocknode(struct super_block *sb,
		struct inode *inode)
{
	struct pmfs_inode *pi;
	struct pmfs_inode_page_tail *tail;
	size_t entry_size = sizeof(struct pmfs_inode_entry);
	u64 curr;
	unsigned long count = 0;

	pi = pmfs_get_inode(sb, inode->i_ino);

	if (pi->log_tail == 0)
		goto out;

	curr = pi->log_head;
	pmfs_dbg("Pi %lu: log head @ %llu, tail @ %llu\n", inode->i_ino,
			curr >> PAGE_SHIFT, pi->log_tail >> PAGE_SHIFT);
	do {
		tail = pmfs_get_block(sb, curr +
					entry_size * ENTRIES_PER_PAGE);
		pmfs_dbg("log block @ %llu\n", curr >> PAGE_SHIFT);
		curr = tail->next_page;
		count++;
		if ((curr >> PAGE_SHIFT) == 0)
			break;
	} while ((curr >> PAGE_SHIFT) != (pi->log_tail >> PAGE_SHIFT));

out:
	pmfs_dbg("All %lu pages\n", count);
}
