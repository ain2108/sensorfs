/* inode.c: sensorfs inode implementations
 *
 * W4118 Homework 6
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/sensorfs.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/idr.h>
#include <linux/uaccess.h>
#include <linux/stat.h>

#include "internal.h"

#define SENSORFS_DEFAULT_MODE	0444
#define SENSORFS_DYNAMIC_FIRST 0xF0000000U

#define SENSORFS_MAGIC (RAMFS_MAGIC + 23)
#define SENSORFS_ROOT_INO 0

static const struct super_operations sensorfs_ops;

static DEFINE_IDA(sensorfs_inum_ida);
static DEFINE_SPINLOCK(sensorfs_inum_lock);
DEFINE_SPINLOCK(sensorfs_biglock);
DEFINE_SPINLOCK(file_sde_array_lock);
struct sensorfs_dir_entry *file_sde_array[NUMBER_OF_FILES];
static struct kmem_cache *sensorfs_inode_cache;

static struct sensorfs_dir_entry sensorfs_root = {

	.low_ino	= SENSORFS_ROOT_INO,
	.namelen	= 9,
	.mode		= S_IFDIR | S_IRUGO | S_IXUGO,
	.nlink		= 2,
	.count		= ATOMIC_INIT(1),
	.sensorfs_iops	= &sensorfs_dir_inode_operations,
	.sensorfs_fops	= &sensorfs_dir_operations,
	.parent		= &sensorfs_root,
	.name		= "/sensorfs",

};

int sensorfs_alloc_inum(unsigned int *inum)
{
	unsigned int i;
	int error;

retry:
	if (!ida_pre_get(&sensorfs_inum_ida, GFP_KERNEL))
		return -ENOMEM;

	spin_lock_irq(&sensorfs_inum_lock);
	error = ida_get_new(&sensorfs_inum_ida, &i);
	spin_unlock_irq(&sensorfs_inum_lock);
	if (error == -EAGAIN)
		goto retry;
	else if (error)
		return error;

	if (i > UINT_MAX - SENSORFS_DYNAMIC_FIRST) {
		spin_lock_irq(&sensorfs_inum_lock);
		ida_remove(&sensorfs_inum_ida, i);
		spin_unlock_irq(&sensorfs_inum_lock);
		return -ENOSPC;
	}
	*inum = SENSORFS_DYNAMIC_FIRST + i;
	return 0;
}

static struct sensorfs_inode *inode_to_sensorfs_inode(struct inode *inode)
{
	return container_of(inode, struct sensorfs_inode, vfs_inode);
}

struct inode *sensorfs_get_inode(struct super_block *sb,
	const struct inode *dir,
	struct sensorfs_dir_entry *de)
{

	struct inode *inode;
	inode = new_inode(sb);
	if (inode == NULL)
		return NULL;

	/* Get the next inode number */
	inode->i_ino = get_next_ino();

	/* Initialize the owner */
	inode_init_owner(inode, dir, SENSORFS_DEFAULT_MODE);

	/* Time of creation and etc */
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;

	/* Associate the inode with the dir_entry */
	SENSORFS_I(inode)->sde = de;

	/* Set the mode and the ownership */
	if (de->mode) {
		inode->i_mode = de->mode;
		inode->i_uid = de->uid;
		inode->i_gid = de->gid;
	}

	/* Set the size */
	if (de->size)
		inode->i_size = de->size;

	if (de->nlink)
		set_nlink(inode, de->nlink);

	/* Setting the ops for inode */
	if (dir == NULL) {
		inode->i_op		= &sensorfs_dir_inode_operations;
		inode->i_fop	= &sensorfs_dir_operations;
	} else {
		inode->i_op		= &sensorfs_file_inode_operations;
		inode->i_fop	= &sensorfs_file_operations;
	}

	return inode;
}

static int sensorfs_delete_dentry(const struct dentry *dentry)
{
	return 1;
}

static const struct dentry_operations sensorfs_dentry_operations = {
	.d_delete	   = sensorfs_delete_dentry,
};


static void init_once(void *toinit)
{
	struct sensorfs_inode *sinode = (struct sensorfs_inode *)toinit;

	inode_init_once(&sinode->vfs_inode);
}

static void sensorfs_init_nodecache(void)
{
	sensorfs_inode_cache = kmem_cache_create("sensorfs_inode_cache",
		sizeof(struct sensorfs_inode),
		0, (SLAB_RECLAIM_ACCOUNT|
		SLAB_MEM_SPREAD|SLAB_PANIC), init_once);
}

static struct inode *sensorfs_alloc_inode(struct super_block *sb)
{
	struct sensorfs_inode *sinode =
		kmem_cache_alloc(sensorfs_inode_cache, GFP_KERNEL);
	if (!sinode)
		return NULL;

	sinode->sde = NULL;
	return &(sinode->vfs_inode);
}


static void sensorfs_destroy_inode(struct inode *inode)
{
	struct sensorfs_inode *sinode = inode_to_sensorfs_inode(inode);

	kmem_cache_free(sensorfs_inode_cache, sinode);
}

static int sensorfs_register(struct sensorfs_dir_entry *dir,
	struct sensorfs_dir_entry *dp)
{
	struct sensorfs_dir_entry *tmp;
	int ret;

	ret = sensorfs_alloc_inum(&dp->low_ino);
	if (ret)
		return ret;

	if (S_ISDIR(dp->mode)) {
		dp->sensorfs_fops = &sensorfs_dir_operations;
		dp->sensorfs_iops = &sensorfs_dir_inode_operations;
		dir->nlink++;
	} else if (S_ISLNK(dp->mode)) {
		dp->sensorfs_iops = &sensorfs_dir_inode_operations;
	} else if (S_ISREG(dp->mode)) {
		BUG_ON(dp->sensorfs_fops == NULL);
		dp->sensorfs_iops = &sensorfs_file_inode_operations;
	} else {
		WARN_ON(1);
	}

	spin_lock(&sensorfs_biglock);

	for (tmp = dir->subdir; tmp; tmp = tmp->next)
		if (strcmp(tmp->name, dp->name) == 0) {
			WARN(1, "sensorfs_dir_entry '%s/%s' already registered\n",
				dir->name, dp->name);
			break;
		}

	dp->next = dir->subdir;
	dp->parent = dir;
	dir->subdir = dp;
	spin_unlock(&sensorfs_biglock);

	return 0;
}

struct sensorfs_dir_entry *sensorfs_create_sfile(
	struct sensorfs_dir_entry *parent, const char *name)
{
	struct sensorfs_dir_entry *ent = NULL;
	const char *fn = name;
	unsigned int len;

	/* Validating the name */
	if (!name || !strlen(name))
		return NULL;


	len = strlen(fn);

	ent = kzalloc(sizeof(struct sensorfs_dir_entry), GFP_KERNEL);
	if (!ent)
		return NULL;
	/* ent is the return value of __proc_create */

	/* Add the name of the file */
	memcpy(ent->name, fn, len + 1);
	ent->namelen = len;

	atomic_set(&ent->count, 1);
	spin_lock_init(&ent->contents_lock);

	if (sensorfs_register(parent, ent) < 0) {
		kfree(ent);
		ent = NULL;
		return NULL;
	}

	return ent;
}


struct dentry *sensorfs_lookup(struct inode *dir, struct dentry *dentry,
	unsigned int flags)
{

	struct inode *inode;
	struct sensorfs_dir_entry *de;


	spin_lock(&sensorfs_biglock);

	de = SDE(dir);
	for (de = de->subdir; de ; de = de->next) {
		if (de->namelen != dentry->d_name.len)
			continue;
		if (!memcmp(dentry->d_name.name, de->name, de->namelen)) {

			spin_unlock(&sensorfs_biglock);
			inode = sensorfs_get_inode(dir->i_sb, dir, de);
			if (!inode)
				return ERR_PTR(-ENOMEM);
			d_set_d_op(dentry, &sensorfs_dentry_operations);
			d_add(dentry, inode);
			return NULL;
		}
	}

	spin_unlock(&sensorfs_biglock);

	return ERR_PTR(-ENOENT);
}

/* Function called during mounting. Fills in the superblock structure,
 * creates the root node by calling sensorfs_get_inode, makes the
 * returned inode the root */

int sensorfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_CACHE_SIZE;
	sb->s_blocksize_bits	= PAGE_CACHE_SHIFT;
	sb->s_magic		= SENSORFS_MAGIC; /* MAGIC NUMBER HERE! */
	sb->s_op		= &sensorfs_ops;
	sb->s_time_gran		= 1;

	/* Creates an inode registered under sensorfs superblock. Notice,
	 * the sensorfs_root argument */
	inode = sensorfs_get_inode(sb, NULL, &sensorfs_root);

	/* Make the inode the root */
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

struct dentry *sensorfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	/*TODO: Probably have to do a little thing here. */
	return mount_nodev(fs_type, flags, data, sensorfs_fill_super);
}

static void sensorfs_kill_sb(struct super_block *sb)
{
	kill_litter_super(sb);
}

static int __init init_sensorfs_fs(void)
{
	sensorfs_init_nodecache();

	spin_lock(&file_sde_array_lock);
	file_sde_array[GPS_FILE_POS] =
		sensorfs_create_sfile(&sensorfs_root, SENSORFS_GPS_FILENAME);
	file_sde_array[LUMMI_FILE_POS] =
		sensorfs_create_sfile(&sensorfs_root, SENSORFS_LUMI_FILENAME);
	file_sde_array[PROX_FILE_POS] =
		sensorfs_create_sfile(&sensorfs_root, SENSORFS_PROX_FILENAME);
	file_sde_array[LINACCEL_FILE_POS] =
		sensorfs_create_sfile(&sensorfs_root,
			SENSORFS_LINACCEL_FILENAME);
	spin_unlock(&file_sde_array_lock);
	return register_filesystem(&sensorfs_fs_type);
}
module_init(init_sensorfs_fs)

struct file_system_type sensorfs_fs_type = {
	.name		= "sensorfs",
	.mount		= sensorfs_mount,
	.kill_sb	= sensorfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

static const struct super_operations sensorfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= generic_show_options,
	.alloc_inode	= sensorfs_alloc_inode,
	.destroy_inode	= sensorfs_destroy_inode,
};

