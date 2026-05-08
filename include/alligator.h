#ifndef _ALLIGATOR_H_
#define _ALLIGATOR_H_

#include <sys/mman.h>
#include <unistd.h>



#define PAGE_SIZE                   sysconf(_SC_PAGESIZE)
#define H_SIZE                      sizeof (_Header)


/** Different slab size to save from over allocation */
#define SLAB_SIZE_SMALL             PAGE_SIZE - H_SIZE
#define SLAB_SIZE_MEDIUM            1024 * PAGE_SIZE - H_SIZE
#define SLAB_SIZE_LARGE             1024 * 1024 * PAGE_SIZE - H_SIZE


/** Flags bits */
#define IS_FREE                     1
#define IS_FIRST_BLOCK              2       // Starting block on slab
#define IS_LAST_BLOCK               4       // Ending block on slab


typedef unsigned long _align;


typedef union header {
    struct {
        unsigned int size;
        unsigned int flags;
        union header * next;
        union header * prev;
    } head;
    _align _al;
} _Header;



void * bite (unsigned int size) ;
void   drop (void * mem)  ;


#endif /* _ALLIGATOR_H_ */