#ifndef _ALLIGATOR_H_
#define _ALLIGATOR_H_

#include <sys/mman.h>
#include <unistd.h>

#define min(a,b)  (((a) < (b)) ? (a) : (b))
#define max(a,b)  (((a) > (b)) ? (a) : (b))


#define PAGE_SIZE                   sysconf(_SC_PAGESIZE)
#define FREE_H_SIZE                 sizeof (_Header)
#define ALOC_H_SIZE                 2 * sizeof (size_t)


/** Different slab size to save from over allocation */
#define SLAB_SIZE_SML               PAGE_SIZE - FREE_H_SIZE
#define SLAB_SIZE_MID               1024 * PAGE_SIZE - FREE_H_SIZE
#define SLAB_SIZE_LRG               1024 * 1024 * PAGE_SIZE - FREE_H_SIZE


/** Flags bits */
#define IS_FREE                     1
#define IS_FRST_BLK                 2       // Starting block on slab
#define IS_LAST_BLK                 4       // Ending block on slab
#define IS_HUGE_BLK                 8



typedef unsigned long _align;


typedef union header {
    struct {
        size_t size;
        size_t prev_size;
        unsigned int flags;
        union header * next;
        union header * prev;                
    } head;
    _align _al;
} _Header;



void * mm_alloc (size_t size) ;
void   mm_free (void * mem)  ;


#endif /* _ALLIGATOR_H_ */