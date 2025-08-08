/*
  Name: kernel heap management module (high level memory management)
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 05/03/04 06:54
  Description: This module handles calls to malloc, free and realloc, it does not
  actually does the memory allocation but serves as a bridge to the custom
  malloc function that the user wants to use.
*/

#ifndef KHEAP_H_GUARD
#define KHEAP_H_GUARD

extern DWORD auxillary_malloc_base;


/*==============================Prototyp Definitions here=====================================*/




void *malloc(unsigned int size);
void *realloc(void *ptr,unsigned int size);
void free(void *ptr);
void alloc_init();

#endif /* KHEAP_H_GUARD */

