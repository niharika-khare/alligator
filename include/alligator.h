#ifndef _ALLIGATOR_H_
#define _ALLIGATOR_H_

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define min(a,b)  (((a) < (b)) ? (a) : (b))
#define max(a,b)  (((a) > (b)) ? (a) : (b))


#define PAGE_SIZE                   sysconf(_SC_PAGESIZE)
#define FREE_H_SIZE                 sizeof (_Header)
#define ALOC_H_SIZE                 sizeof (_alloc_head)
#define MAGIC_NUMBER                0xFF


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

/**
 * The use of bitfields take away the scope for multi-threding 
 * as bitfields are not thread-safe. 
 * For now moving ahead with this implementation makes sense as 
 * this allocator is aimed to be a small learning project and 
 * not a production level library.
 */
typedef struct _aloc_h {
    struct {
            size_t is_free      : 1;
            size_t is_last      : 1;
            size_t magic_id     : 8;
            size_t size         : 48;
            size_t pblk_size    : 48;
            size_t              : 0;
    };
} _alloc_head;


typedef union header {
    struct {
        _alloc_head ah;
        union header * next;
        union header * prev;                
    } head;
    _align _al;
} _Header;



void * mm_alloc (size_t size) ;
void   mm_free (void * mem)  ;


#endif /* _ALLIGATOR_H_ */