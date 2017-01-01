#ifndef _INTERNAL_H_
#define _INTERNAL_H_

/* internal.h: sensorfs internal definitions
 *
 * W4118 Homework 6
 */

extern const struct address_space_operations sensorfs_aops;
extern const struct inode_operations sensorfs_file_inode_operations;
extern const struct file_operations sensorfs_dir_operations;
extern const struct inode_operations sensorfs_dir_inode_operations;
extern struct file_system_type sensorfs_fs_type;

extern spinlock_t sensorfs_biglock;

#define DEFAULT_NAME_LENGTH 128
#define SENSORFS_GPS_FILENAME "gps"
#define SENSORFS_LUMI_FILENAME "lumi"
#define SENSORFS_PROX_FILENAME "prox"
#define SENSORFS_LINACCEL_FILENAME "linaccel"


struct sensorfs_inode {
	struct sensorfs_dir_entry *sde;
	struct inode vfs_inode;
};


/* Taken from proc pretty much directly */
static inline struct sensorfs_inode *SENSORFS_I(const struct inode *inode)
{
	return container_of(inode, struct sensorfs_inode, vfs_inode);
}

static inline struct sensorfs_dir_entry *SDE(const struct inode *inode)
{
	return SENSORFS_I(inode)->sde;
}

struct dentry *sensorfs_lookup(struct inode *dir, struct dentry *dentry,
	unsigned int flags);

#endif
