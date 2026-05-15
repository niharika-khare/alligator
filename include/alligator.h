#ifndef _ALLIGATOR_H_
#define _ALLIGATOR_H_

#include <stdio.h>


void * mm_alloc (size_t size) ;
void * mm_realloc (void * mem, size_t size) ;
void   mm_free (void * mem)  ;


#endif /* _ALLIGATOR_H_ */