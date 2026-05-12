#include "alligator.h"

#include <stdio.h>

static _Header * fl_sml = NULL;
static _Header * fl_mid = NULL;
static _Header * fl_lrg = NULL;



static _Header * _mmap (size_t size) {

    size_t tt_size = size + sizeof (_Header);

    _Header * slab = mmap (NULL, tt_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (slab == MAP_FAILED) {
        return NULL;
    }
    
    slab->head.ah.is_free       = 1;
    slab->head.ah.is_last       = 1;
    slab->head.ah.magic_id      = MAGIC_NUMBER;
    slab->head.ah.size          = size;
    slab->head.ah.pblk_size     = 0;
    slab->head.prev             = NULL;
    slab->head.next             = NULL;

    return slab;

}

static _Header * _fl_add (_Header * fl, _Header * chunk) {

    if (!fl) {
        return chunk;
    }

    chunk->head.next = fl->head.next;
    fl->head.next->head.prev = chunk;
    fl->head.next = chunk;
    chunk->head.prev = fl;

    fl = chunk;

    return fl;
}

static _Header * _find_f_blk (_Header * fl, size_t size) {
    
    if (!fl) {
        return NULL;
    }
    _Header * st = fl;

    do {
        size_t min_size = max (FREE_H_SIZE, size + ALOC_H_SIZE);
        if ((st->head.ah.size + FREE_H_SIZE) > min_size) {

            st->head.ah.size = st->head.ah.size - min_size ;
            _Header * f_blk = ((_Header *) ((char *) st + FREE_H_SIZE + st->head.ah.size ));

            if (st->head.ah.is_last) {
                st->head.ah.is_last = 0;
                f_blk->head.ah.is_last = 1;
            }

            f_blk->head.ah.is_free = 0;
            f_blk->head.ah.magic_id = MAGIC_NUMBER;
            f_blk->head.ah.size = min_size - ALOC_H_SIZE;
            f_blk->head.ah.pblk_size = st->head.ah.size;
                
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

    _Header * fl;
    _Header * f_blk = NULL;
    size_t slab_size = 0;

    if ( size > SLAB_SIZE_LRG ) {
        f_blk = _mmap (size);
        return f_blk ? (void *) ((char *) f_blk + ALOC_H_SIZE) : NULL;
    }

    if ( size <= SLAB_SIZE_SML ) {

        slab_size = SLAB_SIZE_SML;
        if (!fl_sml ) {
            fl_sml = _mmap (slab_size);
            fl_sml->head.next = fl_sml->head.prev = fl_sml;
        }  
        fl = fl_sml;
    } 
    else if (size <= SLAB_SIZE_MID ) {

        slab_size = SLAB_SIZE_MID;
        if (!fl_mid) {
            fl_mid = _mmap (slab_size);
            fl_mid->head.next = fl_mid->head.prev = fl_mid;
        }  
        fl = fl_mid;
    }
    else {

        slab_size = SLAB_SIZE_LRG;
        if (!fl_lrg) {
            fl_lrg = _mmap (slab_size);
            fl_lrg->head.next = fl_lrg->head.prev = fl_lrg;
        } 
        fl = fl_lrg;
    }

    f_blk = _find_f_blk (fl, size);

    if (!f_blk) {
         _Header * new_slab = _mmap (slab_size);
         if (new_slab) {
            fl = _fl_add (fl, new_slab);
            f_blk = _find_f_blk (fl, size);
        }
    }

    return f_blk ? ((void *) ((char *) f_blk + ALOC_H_SIZE)) : NULL;
}

/**
 * 1. Get the pointer of the header from the blk.
 * 2. Check if the header is valid or not.
 * 3. If not valid, generate trap. If valid, continue with below.
 * 4. Check if the header is of size greater than the largest slab, 
 *    if yes, free with munmap directly else continue.
 * 5. Determine the slab list to put the freed blk.
 * 6. If next block is free -> coalesc and update size, adjust pointers
 * 7. If prev block is free -> coalesc and update size, adjust pointers
 * 8. If the final block is equal to the slab size, free it.
 * 9. If a new free blk, put on the free_list, adjust pointers
 * 
 */
void mm_free ( void * mem ) {

    _Header * blk = (_Header *) ((char *) mem - ALOC_H_SIZE);

    // Check Header validity
    // Range check, alignment check, magic number check 
    // how to do above without increasing header size?
    // For now assuming that these are in place and that only 
    // valid memory allocations would be calling mm_free()
    
    if ( (blk->head.ah.magic_id & ~MAGIC_NUMBER) != 0 ) {
        const char * err_msg = "err: memory requested to free was not allocated!\n";
        write (STDERR_FILENO, err_msg, strlen (err_msg));
        abort();
    }

    if (blk->head.ah.is_free) {
        const char * err_msg = "err: memory requested is already free!\n";
        write (STDERR_FILENO, err_msg, strlen (err_msg));
        abort();
    }

    if (blk->head.ah.size > SLAB_SIZE_LRG) {
        munmap (blk, blk->head.ah.size + ALOC_H_SIZE);
        return;
    }

    _Header *fl = NULL;
    blk->head.ah.is_free = 1;

    if (blk->head.ah.size <= SLAB_SIZE_SML) {
        fl = fl_sml ? fl_sml 
            : (blk->head.next = blk->head.prev = blk) ;
    }
    else if (blk->head.ah.size <= SLAB_SIZE_MID) {
        fl = fl_mid ? fl_mid
            : (blk->head.next = blk->head.prev = blk) ;
    }
    else {
        fl = fl_lrg ? fl_lrg
            : (blk->head.next = blk->head.prev = blk) ;
    }

    if (fl == blk) {
        return;
    }

    size_t min_size = max (FREE_H_SIZE, ALOC_H_SIZE + blk->head.ah.size);
    int is_on_fl = 0;

    /* Coalesc with next blk if it exists and is free. */
    if (!blk->head.ah.is_last) {

        _Header * n_blk = blk + min_size;
        if (n_blk->head.ah.is_free) {
            
            blk->head.ah.is_last        = n_blk->head.ah.is_last;
            blk->head.ah.size           = blk->head.ah.size 
                                            + ALOC_H_SIZE 
                                            + n_blk->head.ah.size;
            blk->head.next              = n_blk->head.next;
            blk->head.prev              = n_blk->head.prev;

            n_blk->head.next->head.prev = blk;
            n_blk->head.prev->head.next = blk;

            is_on_fl = 1;
        }

    }

    /* Coalesc with prev blk if it exists and is free */
    if (blk->head.ah.pblk_size != 0) {

        _Header * p_blk = (_Header *) ((char *) blk - (blk->head.ah.pblk_size + FREE_H_SIZE));

        if ( !(p_blk->head.ah.magic_id & ~MAGIC_NUMBER) && (p_blk->head.ah.is_free & 1) ) {
        
            p_blk->head.ah.size         = p_blk->head.ah.size + min_size;
            p_blk->head.ah.is_last      = blk->head.ah.is_last;
            
            if (!p_blk->head.ah.is_last) {
                _Header *n_blk = blk + min_size;
                n_blk->head.ah.pblk_size = p_blk->head.ah.size;
            }

            is_on_fl = 1;
        }
    }

    /* Add to free list if the block is not coalesced and hence not on free list */ 
    if (!is_on_fl) {
        fl = _fl_add (fl, blk);
    }

}