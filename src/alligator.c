#include "alligator.h"

#include <stdio.h>

_Header * fl_sml = NULL;
_Header * fl_mid = NULL;
_Header * fl_lrg = NULL;



static _Header * _mmap (size_t size, int flags) {

    size_t tt_size = size + sizeof (_Header);

    _Header * slab = mmap (NULL, tt_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (slab == MAP_FAILED) {
        return NULL;
    }
    
    slab->head.flags        = flags;
    slab->head.size         = size;
    slab->head.prev_size    = 0;
    slab->head.prev         = NULL;
    slab->head.next         = NULL;

    return slab;

}

static _Header * _fl_add (_Header * fl, _Header * slab) {

    if (!fl) {
        return slab;
    }

    slab->head.next = fl->head.next;
    fl->head.next->head.prev = slab;
    fl->head.next = slab;
    slab->head.prev = fl;

    slab->head.next->head.prev_size = slab->head.size;
    slab->head.prev_size = fl->head.size;

    return slab;
}

static _Header * _find_f_blk (_Header * fl, size_t size) {
    
    if (!fl) {
        return NULL;
    }
    _Header * st = fl;

    do {
        size_t min_size = max (FREE_H_SIZE, size + ALOC_H_SIZE);
        if ((st->head.size + FREE_H_SIZE) > min_size) {

            st->head.size = st->head.size - min_size ;
            _Header * f_blk = ((_Header *) ((char *) st + FREE_H_SIZE + st->head.size ));

            f_blk->head.size = size;
            f_blk->head.prev_size = st->head.size;
                
            return f_blk;
        }
        st = st->head.next;
    }
    while (st != fl);

    return NULL;
}



/**
 * 1. Find the slab tier
 * 2. If the corresponding slab list is NULL, allocate a new slab via _mmap. -> Lazy allocation
 * 3. Try to find the free blk (f_blk). 
 * 4. If a free blk is not found on the slab_list (could be due to unavaialble size or due to no free space), 
 *    then create a new slab for the concerned list and add it to the front of the free list.
 * 5. After adding the slab, search the list again, this time the blk should be found.
 * 6. If a free blk is found (first -fit ) then split the blk, the rear end of the splitted block 
 *    is return to the user (with striped off pointers for prev and next and the flags) and the 
 *    front end's size and pointers are adjusted and it remains on the free list.
 */
void * mm_alloc (size_t size) {

    unsigned int init_flags = IS_FREE | IS_FRST_BLK | IS_LAST_BLK;
    _Header * fl;
    _Header * f_blk = NULL;
    size_t slab_size = 0;

    if ( size <= SLAB_SIZE_SML ) {

        slab_size = SLAB_SIZE_SML;
        if (!fl_sml ) {
            fl_sml = _mmap (slab_size, init_flags);
            fl_sml->head.next = fl_sml->head.prev = fl_sml;
        }  
        fl = fl_sml;
    } 
    else if (size <= SLAB_SIZE_MID ) {

        slab_size = SLAB_SIZE_MID;
        if (!fl_mid) {
            fl_mid = _mmap (slab_size, init_flags);
            fl_mid->head.next = fl_mid->head.prev = fl_mid;
        }  
        fl = fl_mid;
    }
    else if (size <= SLAB_SIZE_LRG ) {

        slab_size = SLAB_SIZE_LRG;
        if (!fl_lrg) {
            fl_lrg = _mmap (slab_size, init_flags);
            fl_lrg->head.next = fl_lrg->head.prev = fl_lrg;
        } 
        fl = fl_lrg;
    } 
    else {
        f_blk = _mmap(size, IS_HUGE_BLK);
        return f_blk ? (void *) ((char *) f_blk + ALOC_H_SIZE) : NULL;
    }

    f_blk = _find_f_blk(fl, size);

    if (!f_blk) {
         _Header * new_slab = _mmap (slab_size, init_flags);
         if (new_slab) {
            fl = _fl_add(fl, new_slab);
            f_blk = _find_f_blk(fl, size);
        }
    }

    return f_blk ? ((void *) ((char *) f_blk + ALOC_H_SIZE)) : NULL;
}


void mm_free ( void * mem ) {

    _Header * blk = (_Header *) ((char *) mem - ALOC_H_SIZE);

    if (blk->head.size > SLAB_SIZE_LRG) {
        munmap(blk, blk->head.size + ALOC_H_SIZE);
    }

}