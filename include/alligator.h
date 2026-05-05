#ifndef _ALLIGATOR_H_
#define _ALLIGATOR_H_

#include <unistd.h>


typedef struct header {
    size_t size;
    int is_free;
    struct header * next;
} _Header;



void * bite (size_t size) ;
void   drop (void * mem)  ;



#endif /* _ALLIGATOR_H_ */