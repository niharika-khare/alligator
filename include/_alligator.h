#ifndef _ALLIGATOR_INTERNALS_H_
#define _ALLIGATOR_INTERNALS_H_

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdalign.h>  

#define min(a,b)  (((a) < (b)) ? (a) : (b))
#define max(a,b)  (((a) > (b)) ? (a) : (b))


static inline size_t _get_page_size() {

    static size_t page_size = 0;
    if (!page_size) {
        page_size = sysconf(_SC_PAGESIZE);
    }
    return page_size;
}

#define PAGE_SIZE                   _get_page_size()
#define FREE_H_SIZE                 sizeof (_Header)
#define ALOC_H_SIZE                 sizeof (_alloc_head)
#define MAGIC_NUMBER                0xA2F5
#define ALIGNMENT                   alignof (max_align_t)


/** Different slab size to avoid over allocation */
#define SLAB_SIZE_SML               (size_t) (PAGE_SIZE - FREE_H_SIZE)
#define SLAB_SIZE_MID               (size_t) (1024 * PAGE_SIZE - FREE_H_SIZE)
#define SLAB_SIZE_LRG               (size_t) (1024 * 1024 * PAGE_SIZE - FREE_H_SIZE)


typedef max_align_t _align;

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
            size_t size         : 48;
            size_t              : 0;
            size_t pblk_size    : 48;
            size_t magic_id     : 16;
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


/** Slab Registry internals */
#define MAX_SLAB_COUNT              32 * 1024

struct slab_registry_object {
    void * slab_addr;
    size_t slab_size;
} 
slab_registry[MAX_SLAB_COUNT];

static int slab_count = 0;

#endif /* _ALLIGATOR_INTERNALS_H_ */