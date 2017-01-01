/* file.c: sensorfs file implementation
 *
 * W4118 Homework 6
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sensorfs.h>

#include "internal.h"

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/idr.h>
#include <linux/uaccess.h>
#include <linux/stat.h>


int sensorfs_readdir_de(struct sensorfs_dir_entry *de,
	struct file *filp, void *dirent, filldir_t filldir)
{
	unsigned int ino;
	int i;
	struct inode *inode = file_inode(filp);
	int ret = 0;

	ino = inode->i_ino;
	i = filp->f_pos;
	switch (i) {
	case 0:
		if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
			goto out;
		i++;
		filp->f_pos++;
		/* fall through */
	case 1:
		if (filldir(dirent, "..", 2, i,
			    parent_ino(filp->f_path.dentry),
			    DT_DIR) < 0)
			goto out;
		i++;
		filp->f_pos++;
		/* fall through */
	default:
		spin_lock(&sensorfs_biglock);
		de = de->subdir;
		i -= 2;
		for (;;) {
			if (!de) {
				ret = 1;
				spin_unlock(&sensorfs_biglock);
				goto out;
			}
			if (!i)
				break;

			de = de->next;
			i--;
		}

		do {
			struct sensorfs_dir_entry *next;

			/* filldir passes info to user space */
			/* TODO: fix counters here and elsewhere */
			/* pde_get(de); */
			spin_unlock(&sensorfs_biglock);

			if (filldir(dirent, de->name,
					de->namelen, filp->f_pos,
				    de->low_ino, de->mode >> 12) < 0) {
				/* pde_put(de); */
				goto out;
			}
			spin_lock(&sensorfs_biglock);
			filp->f_pos++;
			next = de->next;
			/* pde_put(de);*/
			de = next;
		} while (de);
		spin_unlock(&sensorfs_biglock);
	}
	ret = 1;
out:
	return ret;
}


int sensorfs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = file_inode(filp);

	return sensorfs_readdir_de(SDE(inode), filp, dirent, filldir);
}

/*
static void process_contents(char *processed_buffer,
	struct sensorfs_dir_entry *sde)
{
	int oldest_message_pos;
	int dif_older_entries;
	int dif_younger_entries;

	oldest_message_pos = sde->next_relative_pos;
	dif_older_entries =
		CONTENTS_BUFFER_SIZE - oldest_message_pos;
	dif_younger_entries = oldest_message_pos;

	memcpy(processed_buffer, sde->contents + oldest_message_pos,
		dif_older_entries);
	memcpy(processed_buffer + dif_older_entries,
		sde->contents, dif_younger_entries);
	return;
}
*/

static ssize_t sensorfs_read(struct file *file, char __user *buf,
	size_t count, loff_t *ppos)
{
	struct sensorfs_dir_entry *sde = SDE(file_inode(file));
	ssize_t rv = -EIO;

	/*char *processed_buffer = kzalloc(CONTENTS_BUFFER_SIZE, GFP_KERNEL);
	if (processed_buffer == NULL)
		return -ENOMEM;*/

	spin_lock(&sde->contents_lock);

	/* Make sure youngest entries appear last in the file */
	/*process_contents(processed_buffer, sde);*/

	/* Do the "reading" */
	rv = simple_read_from_buffer(buf, count, ppos,
		sde->contents, CONTENTS_BUFFER_SIZE);

	spin_unlock(&sde->contents_lock);


	/*kfree(processed_buffer);*/
	return rv;
}


const struct file_operations sensorfs_file_operations = {
	.read		= sensorfs_read,
};

const struct inode_operations sensorfs_file_inode_operations = {
	.lookup         = sensorfs_lookup,
};

const struct file_operations sensorfs_dir_operations = {
	.readdir        = sensorfs_readdir,
};

const struct inode_operations sensorfs_dir_inode_operations = {
	.lookup         = sensorfs_lookup,
};

