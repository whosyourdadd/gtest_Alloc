#include <stdlib.h>
#include <stdio.h>
#include "chunk.h"
#include "memory.h"
#define TEST_SIZE 4
#define SHRINL_SIZE   2
//#define EXPAND_SIZE   1024*128
#define EXPAND_SIZE   2
/*
test MAX_LARGE (CHUNK_SIZE - (LARGE_CHUNK_HEADER + sizeof(struct large)))
test MAX_LARGE(4194240) = (4194304 - (32+ 32))

*/
int main(void) {
    // mmap(NULL, CHUNK_SIZE * 4, ...)
    void *p = malloc(CHUNK_SIZE * TEST_SIZE);
    if (!p) return 1;
    {
        // no change to the allocation
        void *q = realloc(p, CHUNK_SIZE * TEST_SIZE);
        if (q != p){
            return 1;  
        } 
        // no change to the allocation
        q = realloc(p, CHUNK_SIZE * TEST_SIZE - (CHUNK_SIZE / SHRINL_SIZE));
        if (q != p){
            return 1;  
        } 
        // in-place shrink, madvise purge
        q = realloc(p, CHUNK_SIZE * SHRINL_SIZE);
        if (q != p){

            return 1;  
        } 
        // in-place shrink, madvise purge
        q = realloc(p, CHUNK_SIZE);
        if (q != p){
            return 1;  
        } 
        // in-place expand, no syscall
        //q = realloc(p, CHUNK_SIZE * EXPAND_SIZE);
        q = realloc(p, CHUNK_SIZE * 2);
        if (q != p){
            return 1;  
        } 

        // in-place expand, no syscall
        q = realloc(p, CHUNK_SIZE * EXPAND_SIZE*2);
        //q = realloc(p, CHUNK_SIZE * 123456);
        if (q != p){
            return 1;  
        } 


    }

    // extended/moved by mremap(..., CHUNK_SIZE * 8, MREMAP_MAYMOVE)
    //
    // if it is moved, the source is mapped back in (MREMAP_RETAIN landing would be nicer)
    p = realloc(p, CHUNK_SIZE * 8);
    if (!p){ 
        return 1;
    }

    // mmap(NULL, CHUNK_SIZE * 16, ...)
    void *dest = malloc(CHUNK_SIZE * 16);
    if (!dest){ 
        return 1;
    }
    // madvise purge
    free(dest);

    // moved via MREMAP_MAYMOVE|MREMAP_FIXED to dest
    //
    // the source is mapped back in (MREMAP_RETAIN landing would be nicer)
    p = realloc(p, CHUNK_SIZE * 16);
    if (p != dest){

        //unin test for huge_move_expand function
        void *q = realloc(p, CHUNK_SIZE * EXPAND_SIZE*1024*64);
        if (q != p){
            printf("q != p \n ");
        } 
      return 1;
    }
    // madvise purge
    free(p);
    return 0;
}
