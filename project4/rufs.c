/*
 *  Copyright (C) 2025 CS416 Rutgers CS
 *	Rutgers Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here

static struct superblock sb;

static inline int total_disk_blocks(void) {
    return DISK_SIZE / BLOCK_SIZE;   // NOTE: DISK_SIZE is in block.c; keep consistent
}

#define INODES_PER_BLOCK   (BLOCK_SIZE / sizeof(struct inode))
#define DIRENTS_PER_BLOCK  (BLOCK_SIZE / sizeof(struct dirent))

static void zero_block(int blkno) {
    char buf[BLOCK_SIZE];
    memset(buf, 0, BLOCK_SIZE);
    bio_write(blkno, buf);
}

static void init_dir_block_empty(int blkno) {
    zero_block(blkno);
}

static int bitmap_alloc_bit(uint32_t bitmap_blkno, int max_bits) {
    uint8_t buf[BLOCK_SIZE];
    bio_read(bitmap_blkno, buf);

    for (int i = 0; i < max_bits; i++) {
        if (get_bitmap(buf, i) == 0) {
            set_bitmap(buf, i);
            bio_write(bitmap_blkno, buf);
            return i; 
        }
    }
    return -1;
}


int get_avail_ino() {
    int ino = bitmap_alloc_bit(sb.i_bitmap_blk, sb.max_inum);
    return ino; // -1 on failure
}

int get_avail_blkno() {
    int idx = bitmap_alloc_bit(sb.d_bitmap_blk, sb.max_dnum);
    if (idx < 0) return -1;
    return (int)(sb.d_start_blk + idx); // return absolute disk block number
}


/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
    if (ino >= sb.max_inum) return -EINVAL;

    uint32_t blk = sb.i_start_blk + (ino / INODES_PER_BLOCK);
    uint32_t off = (ino % INODES_PER_BLOCK) * sizeof(struct inode);

    uint8_t buf[BLOCK_SIZE];
    bio_read(blk, buf);
    memcpy(inode, buf + off, sizeof(struct inode));
    return 0;
}

int writei(uint16_t ino, struct inode *inode) {
    if (ino >= sb.max_inum) return -EINVAL;

    uint32_t blk = sb.i_start_blk + (ino / INODES_PER_BLOCK);
    uint32_t off = (ino % INODES_PER_BLOCK) * sizeof(struct inode);

    uint8_t buf[BLOCK_SIZE];
    bio_read(blk, buf);
    memcpy(buf + off, inode, sizeof(struct inode));
    bio_write(blk, buf);
    return 0;
}



/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *out) {
    struct inode dir;
    if (readi(ino, &dir) < 0) return -ENOENT;
    if (!dir.valid) return -ENOENT;

    for (int p = 0; p < 16; p++) {
        int blk = dir.direct_ptr[p];
        if (blk == 0) continue;

        uint8_t buf[BLOCK_SIZE];
        bio_read(blk, buf);
        struct dirent *ents = (struct dirent *)buf;

        for (int i = 0; i < DIRENTS_PER_BLOCK; i++) {
            if (!ents[i].valid) continue;
            if (ents[i].len == name_len && strncmp(ents[i].name, fname, name_len) == 0) {
                if (out) *out = ents[i];
                return 0;
            }
        }
    }
    return -ENOENT;
}


int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
    struct dirent tmp;
    if (dir_find(dir_inode.ino, fname, name_len, &tmp) == 0) return -EEXIST;

    for (int p = 0; p < 16; p++) {
        int blk = dir_inode.direct_ptr[p];
        if (blk == 0) continue;

        uint8_t buf[BLOCK_SIZE];
        bio_read(blk, buf);
        struct dirent *ents = (struct dirent *)buf;

        for (int i = 0; i < DIRENTS_PER_BLOCK; i++) {
            if (ents[i].valid) continue;

            ents[i].valid = 1;
            ents[i].ino   = f_ino;
            ents[i].len   = (uint16_t)name_len;
            memset(ents[i].name, 0, sizeof(ents[i].name));
            memcpy(ents[i].name, fname, name_len);

            bio_write(blk, buf);

            dir_inode.size += sizeof(struct dirent);
            writei(dir_inode.ino, &dir_inode);
            return 0;
        }
    }

    int newblk = get_avail_blkno();
    if (newblk < 0) return -ENOSPC;

    int placed = 0;
    for (int p = 0; p < 16; p++) {
        if (dir_inode.direct_ptr[p] == 0) {
            dir_inode.direct_ptr[p] = newblk;
            placed = 1;
            break;
        }
    }
    if (!placed) return -ENOSPC;

    init_dir_block_empty(newblk);

    uint8_t buf[BLOCK_SIZE];
    bio_read(newblk, buf);
    struct dirent *ents = (struct dirent *)buf;

    ents[0].valid = 1;
    ents[0].ino   = f_ino;
    ents[0].len   = (uint16_t)name_len;
    memset(ents[0].name, 0, sizeof(ents[0].name));
    memcpy(ents[0].name, fname, name_len);

    bio_write(newblk, buf);

    dir_inode.size += sizeof(struct dirent);
    writei(dir_inode.ino, &dir_inode);
    return 0;
}


/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t start_ino, struct inode *out) {
    if (strcmp(path, "/") == 0) {
        return readi(0, out);
    }

    char copy[PATH_MAX];
    strncpy(copy, path, PATH_MAX - 1);
    copy[PATH_MAX - 1] = '\0';

    uint16_t cur_ino = start_ino;
    struct inode cur;
    if (readi(cur_ino, &cur) < 0 || !cur.valid) return -ENOENT;

    char *save = NULL;
    char *tok = strtok_r(copy, "/", &save);
    while (tok) {
        struct dirent de;
        if (dir_find(cur_ino, tok, strlen(tok), &de) < 0) return -ENOENT;

        cur_ino = de.ino;
        if (readi(cur_ino, &cur) < 0 || !cur.valid) return -ENOENT;

        tok = strtok_r(NULL, "/", &save);
    }

    *out = cur;
    return 0;
}


/* 
 * Make file system
 */
int rufs_mkfs() {
    dev_init(diskfile_path);
    dev_open(diskfile_path);

    memset(&sb, 0, sizeof(sb));
    sb.magic_num    = MAGIC_NUM;
    sb.max_inum     = MAX_INUM;

    sb.i_bitmap_blk = 1;
    sb.d_bitmap_blk = 2;
    sb.i_start_blk  = 3;

    int inode_blks = (MAX_INUM + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK;
    sb.d_start_blk = sb.i_start_blk + inode_blks;

    int total = total_disk_blocks();
    int usable_data = total - (int)sb.d_start_blk;
    if (usable_data < 0) usable_data = 0;

    sb.max_dnum = (usable_data < MAX_DNUM) ? usable_data : MAX_DNUM;

    // write superblock
    uint8_t blk0[BLOCK_SIZE];
    memset(blk0, 0, BLOCK_SIZE);
    memcpy(blk0, &sb, sizeof(sb));
    bio_write(0, blk0);

    // zero bitmaps and inode region
    zero_block(sb.i_bitmap_blk);
    zero_block(sb.d_bitmap_blk);
    for (int b = 0; b < inode_blks; b++) zero_block(sb.i_start_blk + b);

    // allocate root inode (ino 0) + root data block (idx 0)
    // set inode bitmap bit 0
    uint8_t ibuf[BLOCK_SIZE];
    bio_read(sb.i_bitmap_blk, ibuf);
    set_bitmap(ibuf, 0);
    bio_write(sb.i_bitmap_blk, ibuf);

    // set data bitmap bit 0 (root dir uses first data block)
    uint8_t dbuf[BLOCK_SIZE];
    bio_read(sb.d_bitmap_blk, dbuf);
    if (sb.max_dnum > 0) set_bitmap(dbuf, 0);
    bio_write(sb.d_bitmap_blk, dbuf);

    int root_blk = (sb.max_dnum > 0) ? (int)sb.d_start_blk : 0;
    if (root_blk != 0) init_dir_block_empty(root_blk);

    struct inode root;
    memset(&root, 0, sizeof(root));
    root.ino   = 0;
    root.valid = 1;
    root.type  = S_IFDIR;
    root.link  = 2;
    root.size  = 0;
    if (root_blk != 0) root.direct_ptr[0] = root_blk;

    time(&root.vstat.st_mtime);
    time(&root.vstat.st_atime);
    root.vstat.st_mode = S_IFDIR | 0755;

    // Optional: add "." and ".." (recommended)
    if (root_blk != 0) {
        uint8_t buf[BLOCK_SIZE];
        bio_read(root_blk, buf);
        struct dirent *ents = (struct dirent *)buf;

        ents[0].valid = 1; ents[0].ino = 0; ents[0].len = 1; strcpy(ents[0].name, ".");
        ents[1].valid = 1; ents[1].ino = 0; ents[1].len = 2; strcpy(ents[1].name, "..");

        bio_write(root_blk, buf);
        root.size = 2 * sizeof(struct dirent);
    }

    writei(0, &root);
    return 0;
}



/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {
    if (access(diskfile_path, F_OK) != 0) {
        rufs_mkfs();
    } else {
        dev_open(diskfile_path);
        uint8_t buf[BLOCK_SIZE];
        bio_read(0, buf);
        memcpy(&sb, buf, sizeof(sb));
        if (sb.magic_num != MAGIC_NUM) {
            // disk exists but not formatted as RUFS
            rufs_mkfs();
        }
    }
    return NULL;
}

static void rufs_destroy(void *userdata) {
    dev_close();
}

static int rufs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(*stbuf));

    struct inode node;
    if (get_node_by_path(path, 0, &node) < 0 || !node.valid) return -ENOENT;

    stbuf->st_uid   = getuid();
    stbuf->st_gid   = getgid();
    stbuf->st_nlink = node.link;
    stbuf->st_size  = node.size;
    stbuf->st_atime = node.vstat.st_atime;
    stbuf->st_mtime = node.vstat.st_mtime;

    if (node.type == S_IFDIR) stbuf->st_mode = S_IFDIR | 0755;
    else                     stbuf->st_mode = S_IFREG | 0644;

    return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {
    struct inode node;
    if (get_node_by_path(path, 0, &node) < 0 || !node.valid) return -ENOENT;
    if (node.type != S_IFDIR) return -ENOTDIR;
    return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
    struct inode dir;
    if (get_node_by_path(path, 0, &dir) < 0 || !dir.valid) return -ENOENT;
    if (dir.type != S_IFDIR) return -ENOTDIR;

    filler(buffer, ".",  NULL, 0);
    filler(buffer, "..", NULL, 0);

    for (int p = 0; p < 16; p++) {
        int blk = dir.direct_ptr[p];
        if (blk == 0) continue;

        uint8_t buf[BLOCK_SIZE];
        bio_read(blk, buf);
        struct dirent *ents = (struct dirent *)buf;

        for (int i = 0; i < DIRENTS_PER_BLOCK; i++) {
            if (!ents[i].valid) continue;
            if (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0) continue;
            filler(buffer, ents[i].name, NULL, 0);
        }
    }

    return 0;
}



static int rufs_mkdir(const char *path, mode_t mode) {
    char path1[PATH_MAX], path2[PATH_MAX];
    strncpy(path1, path, PATH_MAX-1); path1[PATH_MAX-1] = '\0';
    strncpy(path2, path, PATH_MAX-1); path2[PATH_MAX-1] = '\0';

    char *parent = dirname(path1);
    char *name   = basename(path2);

    struct inode pnode;
    if (get_node_by_path(parent, 0, &pnode) < 0) return -ENOENT;
    if (pnode.type != S_IFDIR) return -ENOTDIR;

    struct dirent exists;
    if (dir_find(pnode.ino, name, strlen(name), &exists) == 0) return -EEXIST;

    int ino = get_avail_ino();
    if (ino < 0) return -ENOSPC;

    int blk = get_avail_blkno();
    if (blk < 0) return -ENOSPC;
    init_dir_block_empty(blk);

    struct inode node;
    memset(&node, 0, sizeof(node));
    node.ino = (uint16_t)ino;
    node.valid = 1;
    node.type = S_IFDIR;
    node.link = 2;
    node.direct_ptr[0] = blk;
    node.vstat.st_mode = S_IFDIR | 0755;
    time(&node.vstat.st_mtime);
    time(&node.vstat.st_atime);

    // "." and ".."
    uint8_t b[BLOCK_SIZE];
    bio_read(blk, b);
    struct dirent *ents = (struct dirent *)b;
    ents[0].valid = 1; ents[0].ino = node.ino; ents[0].len = 1; strcpy(ents[0].name, ".");
    ents[1].valid = 1; ents[1].ino = pnode.ino; ents[1].len = 2; strcpy(ents[1].name, "..");
    bio_write(blk, b);
    node.size = 2 * sizeof(struct dirent);

    if (dir_add(pnode, node.ino, name, strlen(name)) < 0) return -ENOSPC;
    writei(node.ino, &node);
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    char path1[PATH_MAX], path2[PATH_MAX];
    strncpy(path1, path, PATH_MAX-1); path1[PATH_MAX-1] = '\0';
    strncpy(path2, path, PATH_MAX-1); path2[PATH_MAX-1] = '\0';

    char *parent = dirname(path1);
    char *name   = basename(path2);

    struct inode pnode;
    if (get_node_by_path(parent, 0, &pnode) < 0) return -ENOENT;
    if (pnode.type != S_IFDIR) return -ENOTDIR;

    struct dirent exists;
    if (dir_find(pnode.ino, name, strlen(name), &exists) == 0) return -EEXIST;

    int ino = get_avail_ino();
    if (ino < 0) return -ENOSPC;

    struct inode node;
    memset(&node, 0, sizeof(node));
    node.ino = (uint16_t)ino;
    node.valid = 1;
    node.type = S_IFREG;
    node.link = 1;
    node.size = 0;
    node.vstat.st_mode = S_IFREG | 0644;
    time(&node.vstat.st_mtime);
    time(&node.vstat.st_atime);

    if (dir_add(pnode, node.ino, name, strlen(name)) < 0) return -ENOSPC;
    writei(node.ino, &node);
    return 0;
}


static int rufs_open(const char *path, struct fuse_file_info *fi) {
    struct inode node;
    if (get_node_by_path(path, 0, &node) < 0 || !node.valid) return -ENOENT;
    return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct inode node;
    if (get_node_by_path(path, 0, &node) < 0 || !node.valid) return -ENOENT;
    if (node.type != S_IFREG) return -EISDIR;

    if ((uint32_t)offset >= node.size) return 0;
    size_t to_read = size;
    if (offset + (off_t)to_read > (off_t)node.size) to_read = node.size - offset;

    size_t done = 0;
    while (done < to_read) {
        int file_blk_idx = (offset + done) / BLOCK_SIZE;
        int blk_off      = (offset + done) % BLOCK_SIZE;
        if (file_blk_idx >= 16) break;

        int blkno = node.direct_ptr[file_blk_idx];
        if (blkno == 0) break;

        uint8_t buf[BLOCK_SIZE];
        bio_read(blkno, buf);

        size_t chunk = BLOCK_SIZE - blk_off;
        if (chunk > (to_read - done)) chunk = (to_read - done);

        memcpy(buffer + done, buf + blk_off, chunk);
        done += chunk;
    }

    time(&node.vstat.st_atime);
    writei(node.ino, &node);
    return (int)done;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct inode node;
    if (get_node_by_path(path, 0, &node) < 0 || !node.valid) return -ENOENT;
    if (node.type != S_IFREG) return -EISDIR;

    size_t done = 0;
    while (done < size) {
        int file_blk_idx = (offset + done) / BLOCK_SIZE;
        int blk_off      = (offset + done) % BLOCK_SIZE;
        if (file_blk_idx >= 16) return (done > 0) ? (int)done : -EFBIG;

        if (node.direct_ptr[file_blk_idx] == 0) {
            int newblk = get_avail_blkno();
            if (newblk < 0) return (done > 0) ? (int)done : -ENOSPC;
            node.direct_ptr[file_blk_idx] = newblk;
            zero_block(newblk);
        }

        int blkno = node.direct_ptr[file_blk_idx];

        uint8_t buf[BLOCK_SIZE];
        bio_read(blkno, buf);

        size_t chunk = BLOCK_SIZE - blk_off;
        if (chunk > (size - done)) chunk = (size - done);

        memcpy(buf + blk_off, buffer + done, chunk);
        bio_write(blkno, buf);

        done += chunk;
    }

    uint32_t new_end = (uint32_t)(offset + done);
    if (new_end > node.size) node.size = new_end;

    time(&node.vstat.st_mtime);
    time(&node.vstat.st_atime);
    writei(node.ino, &node);

    return (int)done;
}



/* 
 * Functions you DO NOT need to implement for this project
 * (stubs provided for completeness)
 */

static int rufs_rmdir(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_unlink(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.mkdir		= rufs_mkdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,

	//Operations that you don't have to implement.
	.rmdir		= rufs_rmdir,
	.releasedir	= rufs_releasedir,
	.unlink		= rufs_unlink,
	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

