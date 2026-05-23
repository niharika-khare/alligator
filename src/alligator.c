#include "_alligator.h"
#include "alligator.h"

#include <stdio.h>

static _Header * fl_sml = NULL;
static _Header * fl_mid = NULL;
static _Header * fl_lrg = NULL;


static _Header * _mmap (size_t size) {

    size_t tt_size = size + FREE_H_SIZE;

    _Header * slab = mmap (NULL, tt_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (slab == MAP_FAILED) return NULL;
    
    slab->head.ah.is_free       = 1;
    slab->head.ah.is_last       = 1;
    slab->head.ah.magic_id      = MAGIC_NUMBER;
    slab->head.ah.size          = size;
    slab->head.ah.pblk_size     = 0;
    slab->head.prev             = NULL;
    slab->head.next             = NULL;

    return slab;

}

/**
 * Check Header validity via the following methods (in sequence):
 *  1. Range check of the blk address (TODO)
 *  2. Alignment check (TODO)
 *  3. Magic number check 
 *  4. Check if the the block is not already free.
 */
static int _is_valid_alloc_blk (_Header * blk) {

    if (blk->head.ah.magic_id != MAGIC_NUMBER) {
        const char * err_msg = "err: memory was not allocated!\n";
        write (STDERR_FILENO, err_msg, strlen (err_msg));
        return -1;
    }
    if (blk->head.ah.is_free) {
        const char * err_msg = "err: memory is free!\n";
        write (STDERR_FILENO, err_msg, strlen (err_msg));
        return -1;
    }
    return 0;
}


static _Header * _fl_add (_Header * fl, _Header * chunk) {

    if (!chunk) return fl;

    if (!chunk->head.ah.is_free) {

        chunk->head.ah.is_free = 1;
        size_t chunk_tt_size = chunk->head.ah.size + ALOC_H_SIZE;
        chunk->head.ah.size = chunk_tt_size - FREE_H_SIZE;

        if (!chunk->head.ah.is_last) {
            _Header * n_blk = (_Header *) ((char *) chunk + chunk_tt_size);
            n_blk->head.ah.pblk_size = chunk->head.ah.size;
        }
    }

    if (!fl) return chunk;

    chunk->head.next = fl->head.next;
    fl->head.next->head.prev = chunk;
    fl->head.next = chunk;
    chunk->head.prev = fl;

    if (fl == fl_sml) fl_sml = fl = chunk; 
    else if (fl == fl_mid) fl_mid = fl = chunk; 
    else if (fl == fl_lrg) fl_lrg = fl = chunk; 

    return fl;
}


/**
 * If the blk to be freed equals slab size or blk allocation follows whole block path then:
 *  1. If it's the only remaining slab on the free list, then bring the free list back to 
 *     the initial state for future allocation.
 *  2. If not the only slab, move the corresponding free list pointer to next slab
 */
static void _fl_dereference (size_t blk_tt_size, _Header * blk) {

    if (!blk) return;
    
    if ((fl_sml) && (fl_sml == blk)) {
        
        fl_sml = ((blk_tt_size == (SLAB_SIZE_SML + FREE_H_SIZE)) && (fl_sml == fl_sml->head.next)) 
               ? NULL : fl_sml->head.next;
        return;
    }
    if ((fl_mid) && (fl_mid == blk)) {
        
        fl_mid = ((blk_tt_size == (SLAB_SIZE_MID + FREE_H_SIZE)) && (fl_mid == fl_mid->head.next)) 
               ? NULL : fl_mid->head.next;
        return;
    }
    if ((fl_lrg) && (fl_lrg == blk)) {  
        
        fl_lrg = ((blk_tt_size == (SLAB_SIZE_LRG + FREE_H_SIZE)) && (fl_lrg == fl_lrg->head.next)) 
               ? NULL : fl_lrg->head.next;
        return;
    }
}


static _Header * _find_f_blk (_Header * fl, size_t size) {
    
    if (!fl) return NULL;

    _Header * st = fl;
    do {
        size_t tt_size = max (FREE_H_SIZE, size + ALOC_H_SIZE);

        if (st->head.ah.size > tt_size) {

            
            st->head.ah.size = st->head.ah.size - tt_size ;
            _Header * f_blk = ((_Header *) ((char *) st + FREE_H_SIZE + st->head.ah.size));

            if (st->head.ah.is_last) {
                st->head.ah.is_last = 0;
                f_blk->head.ah.is_last = 1;
            } 
            else {
                _Header * n_blk = ((_Header *) ((char *) f_blk + tt_size));
                n_blk->head.ah.pblk_size = tt_size - ALOC_H_SIZE;
            }

            f_blk->head.ah.is_free = 0;
            f_blk->head.ah.magic_id = MAGIC_NUMBER;
            f_blk->head.ah.size = tt_size - ALOC_H_SIZE;
            f_blk->head.ah.pblk_size = st->head.ah.size;
                
            return f_blk;
        }
        else if (st->head.ah.size + FREE_H_SIZE >= tt_size) {

            /* If the current free list block's size (combined with header size) is able to 
             * accomodate the new request then assign the whole block and re-link the pointers */

            _Header * f_blk = st;

            f_blk->head.ah.is_free = 0;
            f_blk->head.ah.is_last = st->head.ah.is_last;
            f_blk->head.ah.size = f_blk->head.ah.size + FREE_H_SIZE - ALOC_H_SIZE;

            size_t blk_tt_size = f_blk->head.ah.size + ALOC_H_SIZE;
            _fl_dereference (blk_tt_size, f_blk);

            st->head.next->head.prev = st->head.prev;
            st->head.prev->head.next = st->head.next;

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
 * 6. If a free blk is found (first - fit ) then split the blk, the rear end of the splitted block 
 *    is return to the user (with striped off pointers for prev and next and the flags) and the 
 *    front end's size and pointers are adjusted and it remains on the free list.
 * 7. mm_alloc(0) would return a pointer to address whose ptr->head.ah.size = 0
 */
void * mm_alloc (size_t size) {

    _Header * fl;
    _Header * f_blk = NULL;
    size_t slab_size = 0;
    size_t tt_size = size + ALOC_H_SIZE;

    if (tt_size > SLAB_SIZE_LRG + FREE_H_SIZE) {
        
        f_blk = _mmap (size);
        if (f_blk) f_blk->head.ah.is_free = 0;

        return f_blk ? (void *) ((char *) f_blk + ALOC_H_SIZE) : NULL;
    }

    if ( tt_size <= SLAB_SIZE_SML + FREE_H_SIZE) {

        slab_size = SLAB_SIZE_SML;
        if (!fl_sml ) {
            fl_sml = _mmap (slab_size);
            fl_sml->head.next = fl_sml->head.prev = fl_sml;
        }  
        fl = fl_sml;
    } 
    else if (tt_size <= SLAB_SIZE_MID + FREE_H_SIZE) {

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
 * 10. mm_free(NULL) is valid and would do nothing
 */
void mm_free ( void * restrict mem ) {

    if (!mem) return;

    _Header * restrict blk = (_Header *) ((char *) mem - ALOC_H_SIZE);
    
    if (_is_valid_alloc_blk(blk) == -1) abort();
    

    size_t blk_tt_size = blk->head.ah.size + ALOC_H_SIZE;

    if (blk_tt_size == SLAB_SIZE_SML + FREE_H_SIZE || 
        blk_tt_size == SLAB_SIZE_MID + FREE_H_SIZE ||
        blk_tt_size >= SLAB_SIZE_LRG + FREE_H_SIZE) {

            munmap (blk, blk_tt_size);
            return;
    }

    _Header * fl = NULL;
    size_t slab_size = 0;

    if (blk_tt_size < SLAB_SIZE_SML + FREE_H_SIZE) {

        slab_size = SLAB_SIZE_SML;
        fl = fl_sml ? fl_sml 
            : (fl_sml = blk->head.next = blk->head.prev = blk) ;
    } 
    else if (blk_tt_size < SLAB_SIZE_MID + FREE_H_SIZE) {

        slab_size = SLAB_SIZE_MID;
        fl = fl_mid ? fl_mid
            : (fl_mid = blk->head.next = blk->head.prev = blk) ;
    }
    else {
        slab_size = SLAB_SIZE_LRG;
        fl = fl_lrg ? fl_lrg
            : (fl_lrg = blk->head.next = blk->head.prev = blk) ;
    }

    if (fl == blk) return;

    int is_on_fl = 0;

    /* Coalesc with next blk if it exists and is free. */
    if (!blk->head.ah.is_last) {

        _Header * n_blk = (_Header *) ((char *) blk + blk_tt_size);
        if (n_blk->head.ah.is_free) {
            
            blk->head.ah.is_free        = 1;
            blk->head.ah.is_last        = n_blk->head.ah.is_last;
            blk->head.ah.size           = blk_tt_size + n_blk->head.ah.size;
            blk->head.next              = n_blk->head.next;
            blk->head.prev              = n_blk->head.prev;

            n_blk->head.next->head.prev = blk;
            n_blk->head.prev->head.next = blk;

            is_on_fl = 1;
            blk_tt_size = blk->head.ah.size + FREE_H_SIZE;

            if (fl_sml == n_blk) fl_sml = blk; 
            else if (fl_mid == n_blk) fl_mid = blk; 
            else if (fl_lrg == n_blk) fl_lrg = blk; 
        }

    }

    /* Coalesc with prev blk if it exists and is free */
    if (blk->head.ah.pblk_size != 0) {

        _Header * p_blk = (_Header *) ((char *) blk - (blk->head.ah.pblk_size + FREE_H_SIZE));

        if ( (p_blk->head.ah.magic_id == MAGIC_NUMBER) && p_blk->head.ah.is_free ) {
        
            p_blk->head.ah.size         = p_blk->head.ah.size + blk_tt_size;
            p_blk->head.ah.is_last      = blk->head.ah.is_last;
            
            if (!p_blk->head.ah.is_last) {
                _Header * n_blk = (_Header *) ((char *) blk + blk_tt_size);
                n_blk->head.ah.pblk_size = p_blk->head.ah.size;
            }

            /* If the blk was already added to the free list then that link needs to be removed 
             * (can happen in case of next blk coalescing) */
            if (is_on_fl) {
                blk->head.next->head.prev = blk->head.prev;
                blk->head.prev->head.next = blk->head.next;

                if (fl_sml == blk) fl_sml = p_blk; 
                else if (fl_mid == blk) fl_mid = p_blk; 
                else if (fl_lrg == blk) fl_lrg = p_blk; 
            }
            
            blk = p_blk;
            blk_tt_size = blk->head.ah.size + FREE_H_SIZE;
            is_on_fl = 1;
        }
    }

    /* If blk is coalesced into a slab, re-link it's pointers and free it */
    if (blk->head.ah.size == slab_size) {

        _fl_dereference(blk_tt_size, blk);

        blk->head.next->head.prev = blk->head.prev;
        blk->head.prev->head.next = blk->head.next;

        munmap (blk, blk_tt_size);
        return;
    }

    /* Add to free list if the block is not coalesced and hence not on free list */ 
    if (!is_on_fl) {
        fl = _fl_add (fl, blk);
    }
}

/**
 * Basic realloc implementation which always allocates a new block, copies 
 * `min(old_size, new_size)` bytes, and frees the original.
 */
void * mm_realloc(void * mem, size_t size) {

    if (!mem) return mm_alloc(size);

    _Header * blk = (_Header *) ((char *) mem - ALOC_H_SIZE) ;
    if (_is_valid_alloc_blk(blk) == -1) abort();

    size_t blk_size = blk->head.ah.size;
    
    void * new_mem = mm_alloc(size);
    if (!new_mem) return NULL;

    memcpy (new_mem, mem, min (blk_size, size));
    mm_free(mem);

    return new_mem;
}