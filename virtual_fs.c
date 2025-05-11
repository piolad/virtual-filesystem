
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

struct SuperBlock {
    uint32_t totalBlockCount;
    uint32_t totalInodeCount;
    uint32_t freeInodeCount;
    uint32_t freeBlockCount;
    uint32_t dataStartOffset;
};

// index node - what is it, when the file was created, permissions, pointer to data
struct Inode {
    uint32_t size; // in bytes
    uint32_t directPointers[12];
    uint32_t linkCount;
    bool isDirectory;

};
struct DirectoryEntry {
    char name[256];
    uint32_t inodeIndex;
};



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


    

}