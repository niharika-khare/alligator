#include "alligator.h"

_Header * slab_list_1 = NULL;
_Header * slab_list_2 = NULL;
_Header * slab_list_3 = NULL;

_Header * free_list_small = NULL;
_Header * free_list_medium = NULL;
_Header * free_list_large = NULL;

static void _init () {


}


void * bite ( unsigned int t ) {
    
    return (void *) -1;
}


void drop ( void * mem ) {

}