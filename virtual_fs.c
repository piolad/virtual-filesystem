#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BLOCKSIZE 1024
#define DIRECTBLOCK_CNT 12
#define MAX_FILENAME 252
#define INODE_COUNT 128
#define INODE_SIZE 64


#define INODE_TABLE_BLOCKS ((INODE_COUNT * INODE_SIZE + BLOCKSIZE - 1) / BLOCKSIZE)

#define BGDT_OFFSET BLOCKSIZE
#define BLOCK_BITMAP_OFFSET (BGDT_OFFSET + BLOCKSIZE)
#define INODE_BITMAP_OFFSET (BLOCK_BITMAP_OFFSET + BLOCKSIZE)
#define INODE_TABLE_OFFSET (INODE_BITMAP_OFFSET + BLOCKSIZE)
#define DATA_BLOCKS_OFFSET (INODE_TABLE_OFFSET + INODE_TABLE_BLOCKS * BLOCKSIZE)
#define DIRS_PER_BLOCK (BLOCKSIZE / sizeof(DirectoryEntry))

#pragma pack(push, 1) // tight packing of structures
typedef struct
{
    uint32_t totalBlockCount;
    uint32_t totalInodeCount;
    uint32_t freeInodeCount;
    uint32_t freeBlockCount;
    uint32_t blockSize; // for compatibility
    uint32_t dataStartOffset;
} SuperBlock;

typedef struct
{
    uint32_t size; // in bytes
    uint32_t directPointers[DIRECTBLOCK_CNT]; // to data blocks
    uint32_t linkCount;
    uint32_t isDirectory; // 0 - file, 1 -dir
    uint8_t padding[4]; // make struct 64 bytes

} Inode;
typedef struct
{
    char name[MAX_FILENAME];
    uint32_t inodeIndex;
} DirectoryEntry;

typedef struct
{
    uint32_t blockBitmapBlock;
    uint32_t inodeBitmapBlock;
    uint32_t inodeTableBlock;
    uint16_t freeBlocksCount;
    uint16_t freeInodesCount;
    uint16_t usedDirsCount;
} BlockGroupDesc;
#pragma pack(pop)

SuperBlock sb; //static for simplicity

void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

FILE *open_image_rw(const char *path)
{
    FILE *fp = fopen(path, "r+b");
    if (!fp)
        die("open");
    return fp;
}

void read_at(FILE *fp, uint64_t off, void *buf, size_t n)
{
    if (fseek(fp, off, SEEK_SET) || fread(buf, 1, n, fp) != n)
        die("read_at");
        
}

void write_at(FILE *fp, uint64_t off, const void *buf, size_t n)
{
    if (fseek(fp, off, SEEK_SET) || fwrite(buf, 1, n, fp) != n)
        die("write_at");
}

// superblock
void load_super(FILE *fp)
{
    read_at(fp, 0, &sb, sizeof sb);
}

void store_super(FILE *fp)
{
    write_at(fp, 0, &sb, sizeof sb);
}

void read_inode(FILE *fp, uint32_t idx, Inode *ino)
{
    uint64_t off = INODE_TABLE_OFFSET + idx * INODE_SIZE;
    read_at(fp, off, ino, sizeof *ino);
}

void write_inode(FILE *fp, uint32_t idx, Inode *ino)
{
    uint64_t off = INODE_TABLE_OFFSET + idx * INODE_SIZE;
    write_at(fp, off, ino, sizeof *ino);
}

void read_block(FILE *fp, uint32_t blk_no, void *buf)
{
    read_at(fp, blk_no * BLOCKSIZE, buf, BLOCKSIZE);
}

void write_block(FILE *fp, uint32_t blk_no, const void *buf)
{
    write_at(fp, blk_no * BLOCKSIZE, buf, BLOCKSIZE);
}

// find first free bit in bitmap and allocate
uint32_t alloc_from_bitmap(FILE *fp, uint64_t bmp_off)
{
    uint8_t byte;
    for (uint32_t i = 0; i < sb.totalBlockCount; i++)
    {
        uint64_t off = bmp_off + i / 8;
        read_at(fp, off, &byte, 1);
        if (!(byte & (1 << (i & 7)))) //free bit
        {
            byte |= 1 << (i & 7);
            write_at(fp, off, &byte, 1);
            return i;
        }
    }
    return UINT32_MAX;
}

void free_in_bitmap(FILE *fp, uint64_t bmp_off, uint32_t idx)
{
    uint8_t byte;
    uint64_t off = bmp_off + idx / 8; // find the byte
    read_at(fp, off, &byte, 1);
    byte &= ~(1 << (idx & 7)); // clear the bit
    write_at(fp, off, &byte, 1);
}

// allocation of a block and an inode look same from bitmap perspetive - just at different offset
uint32_t alloc_block(FILE *fp) { return alloc_from_bitmap(fp, BLOCK_BITMAP_OFFSET); }
uint32_t alloc_inode(FILE *fp) { return alloc_from_bitmap(fp, INODE_BITMAP_OFFSET); }


// find the entry by name inside block of DirectoryEntrys
// returns 0 if found, -1 if not found
// and if out != null -> sets it as the found entry
int find_entry_in_block(DirectoryEntry *block,
                               char *name,
                               DirectoryEntry *out)
{
    for (uint32_t i = 0; i < BLOCKSIZE / sizeof(DirectoryEntry); i++)
    {
        if (block[i].inodeIndex && strncmp(block[i].name, name, MAX_FILENAME) == 0)
        {
            if (out)
                *out = block[i];
            return 0;
        }
    }
    return -1; // not found
}

int add_entry_to_dir(FILE *fp, Inode *parent, uint32_t parent_idx,
                            const char *name, uint32_t inodeNo)
{
    uint8_t buf[BLOCKSIZE] = {0};
    uint32_t blk_no = parent->directPointers[0];
    DirectoryEntry *dir = (DirectoryEntry *)buf;

    read_block(fp, blk_no, buf);

    // find empty slot in the block
    for (uint32_t i = 0; i < BLOCKSIZE / sizeof(DirectoryEntry); i++)
    {
        if (dir[i].inodeIndex == 0)
        {
            dir[i].inodeIndex = inodeNo;
            strncpy(dir[i].name, name, MAX_FILENAME - 1);
            dir[i].name[MAX_FILENAME - 1] = '\0';
            write_block(fp, blk_no, dir);

            parent->size += sizeof(DirectoryEntry);
            write_inode(fp, parent_idx, parent);
            return 0;
        }
    }
    return -1;
}

// returns inode's index of the component if found
// or UINT32_MAX if not found
// also:
// - if parent_out != null -> sets it as the parents inode
// - if leaf_out != null -> sets it as the final component of path (file/dir name)
// the return of upto 3 vars makes it efficient
uint32_t path_lookup(FILE *fp, const char *path, uint32_t *parent_out, char *leaf_out)
{
    if (!path || path[0] != '/')
        return UINT32_MAX;

    // root
    if (path[1] == 0) {
        if (parent_out) *parent_out = 0;
        if (leaf_out)   strcpy(leaf_out, "/");
        return 0;
    }

    char copy[1024];
    strncpy(copy, path + 1, sizeof copy); // skip first /
    copy[sizeof copy - 1] = '\0';

    uint32_t cur_idx = 0; // root inode
    Inode     cur;
    read_inode(fp, cur_idx, &cur);

    char *save = NULL;
    char *tok  = strtok_r(copy, "/", &save);
    char buf[BLOCKSIZE];

    DirectoryEntry child;

    while (tok) {
        bool last = (save == NULL || *save == '\0');

        read_block(fp, cur.directPointers[0], buf);
        int found = find_entry_in_block((DirectoryEntry *)buf, tok, &child);

        if (!last) {
            if (found != 0)
                return UINT32_MAX; // not found

            read_inode(fp, child.inodeIndex, &cur);

            if (!cur.isDirectory)
                return UINT32_MAX; // not a directory

            cur_idx = child.inodeIndex; // next index
            tok = strtok_r(NULL, "/", &save); // next token
            continue;
        }
        if (parent_out)
            *parent_out = cur_idx;
        if (leaf_out) {
            strncpy(leaf_out, tok, MAX_FILENAME - 1);
            leaf_out[MAX_FILENAME - 1] = '\0';
        }

        return (found == 0) ? child.inodeIndex : cur_idx;
    }

    return UINT32_MAX; //should not reach here
}

void cmd_mkdir(const char *img, const char *path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t parent_idx;
    char name[MAX_FILENAME];
    if (path_lookup(fp, path, &parent_idx, name) == UINT32_MAX)
        die("mkdir: component not found");
    
    Inode parent;
    read_inode(fp, parent_idx, &parent);
    if (!parent.isDirectory)
        die("mkdir: parent not dir");

    DirectoryEntry blk[BLOCKSIZE / sizeof(DirectoryEntry)];
    read_block(fp, parent.directPointers[0], blk);
    if (find_entry_in_block(blk, name, NULL) == 0)
        die("mkdir: already exists");


    // check if the parent directory has space for a new entry
    uint32_t new_ino_idx = alloc_inode(fp);
    if (new_ino_idx == UINT32_MAX)
        die("no free inodes");
    uint32_t new_blk_idx = alloc_block(fp);
    if (new_blk_idx == UINT32_MAX)
        die("no free blocks");

    sb.freeInodeCount--;
    sb.freeBlockCount--;
    store_super(fp);

    Inode nd = {0};
    nd.isDirectory = 1;
    nd.linkCount = 1;
    nd.directPointers[0] = new_blk_idx;
    write_inode(fp, new_ino_idx, &nd);


    uint8_t zero_block[BLOCKSIZE] = {0};
    DirectoryEntry *ents = (DirectoryEntry *)zero_block;
    strncpy(ents[0].name, ".", MAX_FILENAME);
    ents[0].inodeIndex = new_ino_idx;

    strncpy(ents[1].name, "..", MAX_FILENAME);
    ents[1].inodeIndex = parent_idx;

    write_block(fp, new_blk_idx, zero_block);

    if (add_entry_to_dir(fp, &parent, parent_idx, name, new_ino_idx) < 0)
        die("mkdir: parent directory full");

    fclose(fp);
    printf("mkdir: created %s\n", path);
}

void cmd_ls(const char *img, const char *path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    // find the inode of the path
    uint32_t parent_idx;
    uint32_t ino_idx = path_lookup(fp, path, &parent_idx, NULL);

    // if the path is the root, set parent_idx to 0
    if (ino_idx == UINT32_MAX ||
        (strcmp(path, "/") != 0 && ino_idx == parent_idx))
        die("ls: not found");

    Inode ino;
    read_inode(fp, ino_idx, &ino);

    // if path is file -> print its size
    if (!ino.isDirectory) {
        printf("%s  %u bytes\n", path, ino.size);
        fclose(fp);
        return;
    }

    
    DirectoryEntry dir[DIRS_PER_BLOCK];
    read_block(fp, ino.directPointers[0], dir);

    for (uint32_t i = 0; i < DIRS_PER_BLOCK; i++) {
        if (!dir[i].inodeIndex) continue;

        Inode child;
        read_inode(fp, dir[i].inodeIndex, &child);

        printf("%-30s %10u  %s\n",
               dir[i].name,
               child.size,
               child.isDirectory ? "<DIR>" : "");
    }

    fclose(fp);
}

void cmd_df(const char *img)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    printf("Total Blocks: %u\n", sb.totalBlockCount);
    printf("Free Blocks:  %u\n", sb.freeBlockCount);
    printf("Used Blocks:  %u\n", sb.totalBlockCount - sb.freeBlockCount);
    printf("Total Inodes: %u\n", sb.totalInodeCount);
    printf("Free Inodes:  %u\n", sb.freeInodeCount);
    printf("Used Inodes:  %u\n", sb.totalInodeCount - sb.freeInodeCount);

    fclose(fp);
}

void cmd_rmdir(const char *img, const char *path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t parent_idx;
    char leaf[MAX_FILENAME];
    uint32_t dir_idx = path_lookup(fp, path, &parent_idx, leaf);

    if (dir_idx == UINT32_MAX || dir_idx == parent_idx)
        die("rmdir: directory not found");

    Inode dir_ino;
    read_inode(fp, dir_idx, &dir_ino);
    if (!dir_ino.isDirectory)
        die("rmdir: not a directory");

    DirectoryEntry ents[DIRS_PER_BLOCK];
    read_block(fp, dir_ino.directPointers[0], ents);

    for (uint32_t i = 0; i < DIRS_PER_BLOCK; i++)
        if (ents[i].inodeIndex &&
            strcmp(ents[i].name, ".")  && strcmp(ents[i].name, ".."))
            die("rmdir: directory not empty");

    Inode parent_ino;
    read_inode(fp, parent_idx, &parent_ino);
    read_block(fp, parent_ino.directPointers[0], ents);

    bool removed = false;
    for (uint32_t i = 0; i < DIRS_PER_BLOCK; i++){

        if (ents[i].inodeIndex == dir_idx && strncmp(ents[i].name, leaf, MAX_FILENAME) == 0){

            ents[i].inodeIndex = 0;
            ents[i].name[0] = '\0';
            parent_ino.size -= sizeof(DirectoryEntry);
            removed = true;

            break;
        }
    }
    if (!removed)
        die("rmdir: corrupted directory");

    write_block(fp, parent_ino.directPointers[0], ents);
    write_inode(fp, parent_idx, &parent_ino);

    free_in_bitmap(fp, INODE_BITMAP_OFFSET,  dir_idx);
    free_in_bitmap(fp, BLOCK_BITMAP_OFFSET, dir_ino.directPointers[0]);

    sb.freeInodeCount++;
    sb.freeBlockCount++;
    store_super(fp);

    fclose(fp);
    printf("rmdir: removed %s\n", path);
}

void cmd_ecpt(const char *img, const char *host_path, const char *vfs_path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    FILE *hf = fopen(host_path, "rb");
    if (!hf) die("ecpt: open host file");

    //get file size
    if (fseek(hf, 0, SEEK_END))
        die("ecpt: ftell");
    uint64_t fsize = ftell(hf);
    rewind(hf);

    if (fsize > DIRECTBLOCK_CNT * BLOCKSIZE)
        die("ecpt: file too large for this FS (max 12 KiB)");

    uint32_t parent_idx;
    char leaf[MAX_FILENAME];
    uint32_t found = path_lookup(fp, vfs_path, &parent_idx, leaf);
    if (found != parent_idx)
        die("ecpt: destination already exists");

    uint32_t ino_idx = alloc_inode(fp);
    if (ino_idx == UINT32_MAX) die("ecpt: no free inodes");

    uint32_t need_blocks = (fsize + BLOCKSIZE - 1) / BLOCKSIZE;
    if (need_blocks > sb.freeBlockCount)
        die("ecpt: not enough free blocks");
    uint32_t blk[DIRECTBLOCK_CNT] = {0};
    for (uint32_t i = 0; i < need_blocks; i++)
        if ((blk[i] = alloc_block(fp)) == UINT32_MAX)
            die("ecpt: alloc_block");

    uint8_t buf[BLOCKSIZE] = {0};
    for (uint32_t i = 0; i < need_blocks; i++) {

        // read the file in chunks of upto BLOCKSIZE
        size_t chunk = (i == need_blocks - 1) ? (fsize - i * BLOCKSIZE) : BLOCKSIZE;
        memset(buf, 0, BLOCKSIZE); // clear the buffer

        fread(buf, 1, chunk, hf);
        write_block(fp, blk[i], buf);
    }
    fclose(hf);

    Inode ino = {0};
    ino.size       = (uint32_t)fsize;
    ino.linkCount  = 1;
    ino.isDirectory = 0;
    for (uint32_t i = 0; i < need_blocks; i++) ino.directPointers[i] = blk[i];
    write_inode(fp, ino_idx, &ino);

    Inode parent;
    read_inode(fp, parent_idx, &parent);
    if (add_entry_to_dir(fp, &parent, parent_idx, leaf, ino_idx) < 0)
        die("ecpt: parent directory full");

    sb.freeInodeCount--;
    sb.freeBlockCount -= need_blocks;
    store_super(fp);
    fclose(fp);
    printf("ecpt: copied \"%s\" -> \"%s\"\n", host_path, vfs_path);
}

void cmd_ecpf(const char *img, const char *vfs_path, const char *host_path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t parent_idx;
    uint32_t ino_idx = path_lookup(fp, vfs_path, &parent_idx, NULL);
    if (ino_idx == parent_idx)
        die("ecpf: source not found");

    Inode ino;
    read_inode(fp, ino_idx, &ino);
    if (ino.isDirectory)
        die("ecpf: cannot copy directories (only regular files)");

    FILE *hf = fopen(host_path, "wb");
    if (!hf) die("ecpf: create host file");

    uint32_t blocks = (ino.size + BLOCKSIZE - 1) / BLOCKSIZE;
    uint8_t buf[BLOCKSIZE];
    for (uint32_t i = 0; i < blocks; i++) {
        read_block(fp, ino.directPointers[i], buf);
        size_t chunk = (i == blocks - 1) ? (ino.size - i * BLOCKSIZE) : BLOCKSIZE;
        fwrite(buf, 1, chunk, hf);
    }
    fclose(hf);
    fclose(fp);
    printf("ecpf: copied \"%s\" -> \"%s\"\n", vfs_path, host_path);
}

void cmd_mkfs(const char *filename, size_t disk_size)
{

    uint64_t rounded_disk_size = (disk_size / BLOCKSIZE) * BLOCKSIZE;

    if (rounded_disk_size < DATA_BLOCKS_OFFSET + BLOCKSIZE)
    {
        die("Image too small");
    }

    FILE *fp = fopen(filename,"w+b");

    if (!fp)
        die("open");

    // prefill file with zeros
    uint8_t zero = 0;
    for (size_t i = 0; i < rounded_disk_size; i++)
    {
        fwrite(&zero, 1, 1, fp);
    }

    uint32_t reserved_blocks = 4 + INODE_TABLE_BLOCKS; // +4: superblock, blockgroup descriptor, blockgroup (free/used) bitmap, inode  bitmap
    // create superblock
    sb.totalBlockCount = rounded_disk_size / BLOCKSIZE;
    sb.totalInodeCount = INODE_COUNT;
    sb.freeInodeCount = INODE_COUNT - 1;                          //-1 for the root
    sb.freeBlockCount = sb.totalBlockCount - reserved_blocks - 1; // -1 for root dir blk
    sb.blockSize = BLOCKSIZE;
    sb.dataStartOffset = DATA_BLOCKS_OFFSET;

    printf("Total blocks: %u\n", sb.totalBlockCount);
    printf("Total inodes: %u\n", sb.totalInodeCount);
    printf("Free inodes: %u\n", sb.freeInodeCount);
    printf("Free blocks: %u\n", sb.freeBlockCount);
    printf("Data start offset: %u\n", sb.dataStartOffset);
    fwrite(&sb, sizeof(SuperBlock), 1, fp);

    store_super(fp);
    //===================================================================

    // block group descriptor
    BlockGroupDesc bgd = {0};
    bgd.blockBitmapBlock = BLOCK_BITMAP_OFFSET / BLOCKSIZE;
    bgd.inodeBitmapBlock = INODE_BITMAP_OFFSET / BLOCKSIZE;
    bgd.inodeTableBlock = INODE_TABLE_OFFSET / BLOCKSIZE;
    bgd.freeBlocksCount = sb.freeBlockCount;
    bgd.freeInodesCount = sb.freeInodeCount;
    bgd.usedDirsCount = 1;
    fseek(fp, BGDT_OFFSET, SEEK_SET);
    fwrite(&bgd, sizeof bgd, 1, fp);
    //===================================================================

    // bitmap of used/free blocks
    uint8_t block_bitmap[BLOCKSIZE] = {0};
    for (uint32_t i = 0; i < reserved_blocks + 1; i++)
    {
        // set bits for each position of first [reserved_blocks] bit
        block_bitmap[i / 8] |= (0x01 << (i & 7));
    }
    fseek(fp, BLOCK_BITMAP_OFFSET, SEEK_SET);
    fwrite(&block_bitmap, sizeof(block_bitmap), 1, fp);

    uint8_t inode_bitmap[BLOCKSIZE] = {0};
    inode_bitmap[0] = 0x01;
    fseek(fp, INODE_BITMAP_OFFSET, SEEK_SET);
    fwrite(&inode_bitmap, sizeof(inode_bitmap), 1, fp);
    //===================================================================

    // root inode
    Inode root = {0};
    root.isDirectory = 1;
    root.linkCount = 1; // the / itself
    root.directPointers[0] = DATA_BLOCKS_OFFSET / BLOCKSIZE;
    root.size = 0;
    fseek(fp, INODE_TABLE_OFFSET, SEEK_SET);
    fwrite(&root, sizeof(root), 1, fp);

    // fill rest with 0
    uint32_t inode_padding = INODE_TABLE_BLOCKS * BLOCKSIZE - sizeof root;
    uint8_t buf[1024] = {0};
    for (uint32_t w = 0; w < inode_padding; w += sizeof buf)
        fwrite(buf, 1, sizeof buf, fp);

    //===================================================================

    fflush(fp);
    fclose(fp);
}

void release_block(FILE *fp, uint32_t blk)
{
    free_in_bitmap(fp, BLOCK_BITMAP_OFFSET, blk);
    sb.freeBlockCount++;
}

void release_inode_and_data(FILE *fp, uint32_t ino_idx, Inode *ino)
{
    uint32_t blks = (ino->size + BLOCKSIZE - 1) / BLOCKSIZE;
    for (uint32_t i = 0; i < blks; i++)
        if (ino->directPointers[i])
            release_block(fp, ino->directPointers[i]);

    free_in_bitmap(fp, INODE_BITMAP_OFFSET, ino_idx);
    sb.freeInodeCount++;
}

uint64_t compute_usage(FILE *fp, uint32_t ino_idx)
{
    Inode ino;
    read_inode(fp, ino_idx, &ino);

    if (!ino.isDirectory)
        return ((ino.size + BLOCKSIZE - 1) / BLOCKSIZE) * BLOCKSIZE;

    uint64_t total = BLOCKSIZE;
    DirectoryEntry ent[DIRS_PER_BLOCK];
    read_block(fp, ino.directPointers[0], ent);

    for (uint32_t i = 0; i < DIRS_PER_BLOCK; i++)
        if (ent[i].inodeIndex &&
            strcmp(ent[i].name, ".")  && strcmp(ent[i].name, ".."))
            total += compute_usage(fp, ent[i].inodeIndex);
    return total;
}

void cmd_lsdf(const char *img, const char *path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t parent_idx;
    uint32_t ino_idx = path_lookup(fp, path, &parent_idx, NULL);
    if (ino_idx == parent_idx)
        die("lsdf: path not found");

    uint64_t bytes = compute_usage(fp, ino_idx);
    printf("%s: %llu bytes (%.2f KiB, %.2f MiB)\n",
           path, (unsigned long long)bytes,
           bytes / 1024.0, bytes / (1024.0 * 1024.0));

    fclose(fp);
}


void cmd_crhl(const char *img, const char *src, const char *dst)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t src_parent;
    uint32_t src_ino = path_lookup(fp, src, &src_parent, NULL);
    if (src_ino == src_parent)
        die("crhl: source not found");

    uint32_t dst_parent;
    char dst_leaf[MAX_FILENAME];
    uint32_t dst_found = path_lookup(fp, dst, &dst_parent, dst_leaf);
    if (dst_found != dst_parent)
        die("crhl: destination already exists");

    Inode parent;
    read_inode(fp, dst_parent, &parent);
    if (!parent.isDirectory) die("crhl: dest-parent not a directory");

    if (add_entry_to_dir(fp, &parent, dst_parent, dst_leaf, src_ino) < 0)
        die("crhl: parent directory full");

    Inode target;
    read_inode(fp, src_ino, &target);
    target.linkCount++;
    write_inode(fp, src_ino, &target);

    fclose(fp);
    printf("crhl: linked %s -> %s\n", dst, src);
}

void cmd_rm(const char *img, const char *path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t parent_idx;
    char leaf[MAX_FILENAME];
    uint32_t ino_idx = path_lookup(fp, path, &parent_idx, leaf);
    if (ino_idx == parent_idx)
        die("rm: path not found");

    Inode ino;
    read_inode(fp, ino_idx, &ino);
    if (ino.isDirectory)
        die("rm: use rmdir for directories");

    Inode parent;
    read_inode(fp, parent_idx, &parent);
    DirectoryEntry ent[DIRS_PER_BLOCK];
    read_block(fp, parent.directPointers[0], ent);

    bool removed = false;
    for (uint32_t i = 0; i < DIRS_PER_BLOCK; i++)
        if (ent[i].inodeIndex == ino_idx &&
            strncmp(ent[i].name, leaf, MAX_FILENAME) == 0)
        {
            ent[i].inodeIndex = 0;
            ent[i].name[0]    = '\0';
            parent.size      -= sizeof(DirectoryEntry);
            removed = true;
            break;
        }
    if (!removed) die("rm: corrupt parent directory");

    write_block(fp, parent.directPointers[0], ent);
    write_inode (fp, parent_idx, &parent);

    ino.linkCount--;
    if (ino.linkCount == 0)
        release_inode_and_data(fp, ino_idx, &ino);
    else
        write_inode(fp, ino_idx, &ino);

    store_super(fp);
    fclose(fp);
    printf("rm: removed %s\n", path);
}

void cmd_ext(const char *img, const char *path, uint32_t add)
{
    if (!add) return;

    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t pidx;
    uint32_t ino_idx = path_lookup(fp, path, &pidx, NULL);
    if (ino_idx == pidx) die("ext: path not found");

    Inode ino;
    read_inode(fp, ino_idx, &ino);
    if (ino.isDirectory) die("ext: cannot extend a directory");

    uint32_t old_size   = ino.size;
    uint32_t new_size   = old_size + add;
    uint32_t old_blocks = (old_size + BLOCKSIZE - 1) / BLOCKSIZE;
    uint32_t new_blocks = (new_size + BLOCKSIZE - 1) / BLOCKSIZE;

    if (new_blocks > DIRECTBLOCK_CNT)
        die("ext: exceeds max direct blocks (12)");

    for (uint32_t i = old_blocks; i < new_blocks; i++) {
        uint32_t b = alloc_block(fp);
        if (b == UINT32_MAX) die("ext: out of blocks");
        ino.directPointers[i] = b;
        sb.freeBlockCount--;
        uint8_t z[BLOCKSIZE]={0};
        write_block(fp, b, z);
    }

    ino.size = new_size;
    write_inode(fp, ino_idx, &ino);
    store_super(fp);
    fclose(fp);
    printf("ext: %u bytes added to %s (new size %u)\n",
           add, path, new_size);
}

void cmd_red(const char *img, const char *path, uint32_t sub)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t pidx;
    uint32_t ino_idx = path_lookup(fp, path, &pidx, NULL);
    if (ino_idx == pidx) die("red: path not found");

    Inode ino;
    read_inode(fp, ino_idx, &ino);
    if (ino.isDirectory) die("red: cannot shrink a directory");

    if (sub >= ino.size) {
        release_inode_and_data(fp, ino_idx, &ino);
        store_super(fp);
        fclose(fp);
        printf("red: %s truncated to 0\n", path);
        return;
    }

    uint32_t new_size   = ino.size - sub;
    uint32_t old_blocks = (ino.size + BLOCKSIZE - 1) / BLOCKSIZE;
    uint32_t new_blocks = (new_size + BLOCKSIZE - 1) / BLOCKSIZE;

    for (uint32_t i = new_blocks; i < old_blocks; i++)
        if (ino.directPointers[i]) {
            release_block(fp, ino.directPointers[i]);
            ino.directPointers[i] = 0;
        }

    ino.size = new_size;
    write_inode(fp, ino_idx, &ino);
    store_super(fp);
    fclose(fp);
    printf("red: %u bytes removed from %s (new size %u)\n",
           sub, path, new_size);
}
void du_walk(FILE *fp, uint32_t ino_idx, const char *path)
{
    uint64_t bytes = compute_usage(fp, ino_idx);
    printf("%llu\t%s\n", (unsigned long long)bytes, path);

    Inode ino;
    read_inode(fp, ino_idx, &ino);
    if (!ino.isDirectory)
        return;

    DirectoryEntry ent[DIRS_PER_BLOCK];
    read_block(fp, ino.directPointers[0], ent);

    for (uint32_t i = 0; i < DIRS_PER_BLOCK; i++)
        if (ent[i].inodeIndex &&
            strcmp(ent[i].name, ".")  != 0 &&
            strcmp(ent[i].name, "..") != 0)
        {
            char child_path[1024];
            if (strcmp(path, "/") == 0)
                snprintf(child_path, sizeof(child_path), "/%s", ent[i].name);
            else
                snprintf(child_path, sizeof(child_path), "%s/%s",
                         path, ent[i].name);

            du_walk(fp, ent[i].inodeIndex, child_path);
        }
}

void cmd_du(const char *img, const char *path)
{
    FILE *fp = open_image_rw(img);
    load_super(fp);

    uint32_t parent_idx;
    uint32_t ino_idx = path_lookup(fp, path, &parent_idx, NULL);
    if (ino_idx == parent_idx)
        die("du: path not found");

    du_walk(fp, ino_idx, path);

    fclose(fp);
}

void usage()
{
    printf("Usage: vfs <imagepath> <command> [args]\n");
    printf("Commands:\n");
    printf("\tmkfs <bytes>\t\t\t- create an empty image\n");
    printf("\tmkdir <path>\t\t\t- create directory at path\n");
    printf("\trmdir <path>\t\t\t- remove directory at path\n");
    printf("\tls <path>\t\t\t- list items at path\n");
    printf("\tdf\t\t\t\t- show disk usage of the image\n");
    printf("\tlsdf <path>\t\t\t- show disk usage of the pathitem\n");
    printf("\tcrhl <path> <path>\t\t- create a hard link to file or dir\n");
    printf("\trm <path>\t\t\t- remove a file or link\n");
    printf("\text <path> <n>\t\t\t- add n bytes to a file\n");
    printf("\tred <path> <n>\t\t\t- reduce n bytes from a file\n");
    printf("\tdu <path>\t\t\t- display info about disk usage\n");

    printf("\tecpt <ext_path> <path>\t\t- external copy to disk\n");
    printf("\tecpf <path> <ext_path>\t\t- external copy from disk\n");
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        usage();
        return 1;
    }

    const char *img = argv[1];
    const char *cmd = argv[2];

    if (strcmp(cmd, "mkfs") == 0)
    {
        if (argc != 4)
        {
            usage();
            return 1;
        }
        cmd_mkfs(img, strtoull(argv[3], NULL, 10));
        return 0;
    }
    else if (strcmp(cmd, "mkdir") == 0)
    {
        if (argc != 4)
        {
            usage();
            return 1;
        }
        cmd_mkdir(img, argv[3]);
        return 0;
    }
    else if (strcmp(cmd, "ls") == 0)
    {
        if (argc != 4)
        {
            usage();
            return 1;
        }
        cmd_ls(img, argv[3]);
        return 0;
    }
    else if(strcmp(cmd, "df") == 0)
    {
        if (argc != 3)
        {
            usage();
            return 1;
        }
        cmd_df(img);
        return 0;
    }
    else if (strcmp(cmd, "rmdir") == 0)
    {
        if (argc != 4)
        {
            usage();
            return 1;
        }
        cmd_rmdir(img, argv[3]);
        return 0; 
    }else if (strcmp(cmd, "ecpt") == 0)
    {
        if (argc != 5)
        {
            usage();
            return 1;
        }
        cmd_ecpt(img, argv[3], argv[4]);
        return 0;
    }
    else if (strcmp(cmd, "ecpf") == 0)
    {
        if (argc != 5)
        {
            usage();
            return 1;
        }
        cmd_ecpf(img, argv[3], argv[4]);
        return 0;
    }
    else if (strcmp(cmd, "lsdf") == 0)
    {
        if (argc != 4) { usage(); return 1; }
        cmd_lsdf(img, argv[3]);
        return 0;
    }
    else if (strcmp(cmd, "crhl") == 0)
    {
        if (argc != 5) { usage(); return 1; }
        cmd_crhl(img, argv[3], argv[4]);
        return 0;
    }
    else if (strcmp(cmd, "rm") == 0)
    {
        if (argc != 4) { usage(); return 1; }
        cmd_rm(img, argv[3]);
        return 0;
    }
    else if (strcmp(cmd, "ext") == 0)
    {
        if (argc != 5) { usage(); return 1; }
        cmd_ext(img, argv[3], (uint32_t)strtoul(argv[4], NULL, 10));
        return 0;
    }
    else if (strcmp(cmd, "red") == 0)
    {
        if (argc != 5) { usage(); return 1; }
        cmd_red(img, argv[3], (uint32_t)strtoul(argv[4], NULL, 10));
        return 0;
    }
    else if (strcmp(cmd, "du") == 0)
    {
        if(argc != 4) { usage(); return 1; }
        cmd_du(img, argv[3]);
        return 0;
    }


    usage();
    return 1;
}