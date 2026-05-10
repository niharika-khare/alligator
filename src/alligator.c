#include "alligator.h"

#include <stdio.h>

_Header * fl_sml = NULL;
_Header * fl_mid = NULL;
_Header * fl_lrg = NULL;


static _Header * _fl_add (_Header * fl, _Header * slab) {

    if (!fl) {
        return slab;
    }

    slab->head.next = fl;
    fl->head.prev = slab;
    fl->head.prev_size = slab->head.size;

    return slab;
}

static _Header * _find_f_blk (_Header * fl, size_t size) {

    _Header * st = fl;

    do {
        if (st->head.size == size) {

            if (st->head.next) {
                st->head.next->head.prev = st->head.prev;
                st->head.next->head.prev_size = st->head.prev ? st->head.prev->head.size : 0;
            }            

            if (st->head.prev) {
                st->head.prev->head.next = st->head.next;
            }
            
            st->head.flags = st->head.flags & ~IS_FREE;
            
            return st;
        }
        else if (st->head.size >= size + ALOC_H_SIZE) {

            st->head.size = st->head.size - (size + ALOC_H_SIZE) ;
            _Header * f_blk = ((_Header *) ((char *) st + ALOC_H_SIZE + size ));

            f_blk->head.size = size;
            f_blk->head.prev_size = st->head.size;
                
            return f_blk;
        }
        st = st->head.next;
    }
    while (st && st != fl);

    return NULL;
}

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

void * mm_alloc (size_t size) {

    unsigned int init_flags = IS_FREE | IS_FRST_BLK | IS_LAST_BLK;
    _Header * fl;
    _Header * f_blk = NULL;

    if ( size <= SLAB_SIZE_SML ) {

        if (!fl_sml || !(f_blk = _find_f_blk(fl_sml, size)) ) {

            f_blk = _mmap (SLAB_SIZE_SML, init_flags);
            fl_sml = !fl_sml ? f_blk : _fl_add(fl_sml, f_blk);
        }  
        fl = fl_sml;
    } 
    else if (size <= SLAB_SIZE_MID ) {

        if (!fl_mid || !(f_blk = _find_f_blk(fl_mid, size)) ) {

            f_blk = _mmap (SLAB_SIZE_MID, init_flags);
            fl_mid = !fl_mid ? f_blk : _fl_add(fl_mid, f_blk);
        }  
        fl = fl_mid;
    }
    else if (size <= SLAB_SIZE_LRG ) {

        if (!fl_lrg || !(f_blk = _find_f_blk(fl_lrg, size)) ) {

            f_blk = _mmap (SLAB_SIZE_LRG, init_flags);
            fl_lrg = !fl_lrg ? f_blk : _fl_add(fl_lrg, f_blk);
        } 

        fl = fl_lrg;
    } 
    else {

        f_blk = _mmap(size, IS_HUGE_BLK);
        return f_blk ? (void *) (f_blk + 1) : NULL;
    }

    if (!f_blk) {
        f_blk = _find_f_blk(fl, size);
    }
    
    return f_blk ? (char *) f_blk + ALOC_H_SIZE : NULL;
}


void mm_free ( void * mem ) {

    _Header * blk = (_Header *) ((char *) mem - ALOC_H_SIZE);

    printf("Prev size: %ld\n\n", blk->head.prev_size);

    if (blk->head.flags & IS_HUGE_BLK) {
        munmap(blk, blk->head.size + FREE_H_SIZE);
    }

}