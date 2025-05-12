
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

#pragma pack(push, 1) // ensure structures are tightly packed

#define BLOCKSIZE 1024
#define DIRECTBLOCK_CNT 12
#define MAX_FILENAME 255

#define INODE_COUNT 128
#define INODE_SIZE 128
#define INODE_TABLE_BLOCKS (INODE_COUNT * INODE_SIZE / BLOCKSIZE)

#define SUPERBLOCK_OFFSET 0
#define BGDT_OFFSET (SUPERBLOCK_OFFSET + BLOCKSIZE)
#define BLOCK_BITMAP_OFFSET (BGDT_OFFSET + BLOCKSIZE)
#define INODE_BITMAP_OFFSET (BLOCK_BITMAP_OFFSET + BLOCKSIZE)
#define INODE_TABLE_OFFSET (INODE_BITMAP_OFFSET + BLOCKSIZE)
#define DATA_BLOCKS_OFFSET (INODE_TABLE_OFFSET + INODE_TABLE_BLOCKS * BLOCKSIZE)


typedef struct {
    uint32_t totalBlockCount;
    uint32_t totalInodeCount;
    uint32_t freeInodeCount;
    uint32_t freeBlockCount;
    uint32_t dataStartOffset;
} SuperBlock;

// index node - what is it, when the file was created, permissions, pointer to data
typedef struct {
    uint32_t size; // in bytes
    uint32_t directPointers[DIRECTBLOCK_CNT];
    uint32_t linkCount;
    bool isDirectory;

} Inode;
typedef struct  {
    char name[MAX_FILENAME];
    uint32_t inodeIndex;
} DirectoryEntry;


static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

void init_disk(const char* filename, size_t disk_size) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        die("file could not be opened!");
        return;
    }

    // Fill with zeros
    uint8_t zero = 0;
    for (size_t i = 0; i < disk_size; i++) {
        fwrite(&zero, 1, 1, fp);
    }

    // go back to first byte:
    fseek(fp, 0, SEEK_SET);
    // write superblock
    SuperBlock sb;
    sb.totalBlockCount = disk_size / BLOCKSIZE;
    sb.totalInodeCount = INODE_COUNT;
    sb.freeInodeCount = INODE_COUNT;
    sb.freeBlockCount = sb.totalBlockCount - INODE_TABLE_BLOCKS - 1; // 1 for superblock
    sb.dataStartOffset = DATA_BLOCKS_OFFSET;

    printf("Total blocks: %u\n", sb.totalBlockCount);
    printf("Total inodes: %u\n", sb.totalInodeCount);
    printf("Free inodes: %u\n", sb.freeInodeCount);
    printf("Free blocks: %u\n", sb.freeBlockCount);
    printf("Data start offset: %u\n", sb.dataStartOffset);

    fwrite(&sb, sizeof(SuperBlock), 1, fp);


    fclose(fp);
}




void usage(){
    printf("Usage: vfs <imagepath> <command> [args]\n");
    printf("Commands:\n");
    printf("\tmkfs <bytes>\t\t- create an empty image\n");
    printf("\tmkdir <path>\t\t- create directory at path\n");
    printf("\trmdir <path>\t\t- remove directory at path\n");
    printf("\tls <path>\t\t- list items at path\n");
    printf("\tdf\t\t- show disk usage of the image\n");
    printf("\tlsdf <path>\t\t- show disk usage of the pathitem\n");
    printf("\tcrhl <path> <path>\t\t- create a hard link to file or dir\n");
    printf("\trm <path>\t\t- remove a file or link\n");
    printf("\text <path> <n>\t\t- add n bytes to a file\n");
    printf("\tred <path> <n>\t\t- reduce n bytes from a file\n");
    printf("\tdu <path>\t\t- display info about disk usage\n");

    printf("\tecpt <ext_path> <path>\t\t- external copy to disk\n");
    printf("\tecpf <path> <ext_path>\t\t- external copy from disk\n");

}


int main(int argc, char* argv[] ){
    if(argc < 3){usage(); return 1;}

    if(strcmp(argv[2], "mkfs") ==0){
        init_disk(argv[1], atoi(argv[3]));
    }

    

}