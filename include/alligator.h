#ifndef _ALLIGATOR_H_
#define _ALLIGATOR_H_

#include <sys/mman.h>
#include <unistd.h>



#define PAGE_SIZE                   sysconf(_SC_PAGESIZE)
#define H_SIZE                      sizeof (_Header)


/** Different slab size to save from over allocation */
#define SLAB_SIZE_SML               PAGE_SIZE - H_SIZE
#define SLAB_SIZE_MID               1024 * PAGE_SIZE - H_SIZE
#define SLAB_SIZE_LRG               1024 * 1024 * PAGE_SIZE - H_SIZE


/** Flags bits */
#define IS_FREE                     1
#define IS_FRST_BLK                 2       // Starting block on slab
#define IS_LAST_BLK                 4       // Ending block on slab
#define IS_HUGE_BLK                 8


typedef unsigned long _align;


typedef union header {
    struct {
        unsigned int flags;
        size_t size;
        size_t prev_size;
        union header * next;
        union header * prev;                // points to the last block 
    } head;
    _align _al;
} _Header;



void * mm_alloc (size_t size) ;
void   mm_free (void * mem)  ;


#endif /* _ALLIGATOR_H_ */