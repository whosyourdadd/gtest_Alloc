#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "chunk.h"
#include "memory.h"
#include "gtest/gtest.h"

void *do_work(void *N) {
	if(!N)return NULL;
	unsigned long MemPoolSize = *(unsigned long*)N;
    void **p = (void **)malloc(MemPoolSize * sizeof(void *));
    for (size_t i = 0; i < MemPoolSize; i++) {
        p[i] = malloc(16);
        if (!p[i]) {
            exit(1);
        }
    }
    for (size_t i = 0; i < MemPoolSize; i++) {
        free(p[i]);
    }
	return NULL;
    //return ptr;
}

//int main(void) {
int test_small(unsigned long N){
    pthread_t thread;
    pthread_create(&thread, NULL, do_work, (void *)&N);
    pthread_join(thread, NULL);
    return 0;
}

//int main(void) {
int test_huge(unsigned int alloc_size,unsigned int  shrink_size,unsigned int expand_size){
    // mmap(NULL, CHUNK_SIZE * 4, ...)
    void *p = malloc(CHUNK_SIZE * alloc_size);
    if (!p) return 1;
    {
        // no change to the allocation
        void *q = realloc(p, CHUNK_SIZE * alloc_size);
        if (q != p){
            return 1;  
        } 
        // no change to the allocation
        q = realloc(p, CHUNK_SIZE * alloc_size - (CHUNK_SIZE / shrink_size));
        if (q != p){
            return 1;  
        } 
        // in-place shrink, madvise purge
        q = realloc(p, CHUNK_SIZE * shrink_size);
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
        q = realloc(p, CHUNK_SIZE * expand_size);
        if (q != p){
            return 1;  
        } 

        // in-place expand, no syscall
        q = realloc(p, CHUNK_SIZE * expand_size*2);
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
        void *q = realloc(p, CHUNK_SIZE * expand_size*1024*64);
      //  if (q != p){
      //   printf("q != p \n ");
       // } 
      return 1;
    }
    // madvise purge
    free(p);
    return 0;
}

int test_large(unsigned int test_size) {
    void *p = malloc(test_size * 4);
    if (!p) return 1;

    {
        // in-place shrink
        void *q = realloc(p, test_size * 2);
        if (q != p) return 1;

        // in-place shrink
        q = realloc(p, test_size);
        if (q != p) return 1;

        // in-place expand
        q = realloc(p, test_size * 2);
        if (q != p) return 1;

        // in-place expand
        q = realloc(p, test_size * 4);
        if (q != p) return 1;

        // in-place expand
        q = realloc(p, test_size * 8);
        if (q != p) return 1;

        // in-place expand
        q = realloc(p, test_size * 64);
        if (q != p) return 1;
    }

    free(p);
    return 0;
}


TEST(MemoryTest, test_small) {
    EXPECT_FALSE(test_small(10000000));
}
TEST(MemoryTest, test_huge) {
    EXPECT_TRUE(test_huge(4,2,2));
}
TEST(MemoryTest, test_large) {
    EXPECT_FALSE(test_large(4096));
}
/*
int main(void){
	printf("test small\n");
	test_small(10000000);
	printf("test huge\n");
	test_huge(4,2,2);
	printf("test large\n");
	test_large(4096);
	return 1;

}
*/
