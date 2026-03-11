#ifndef PROC_MEM_H
#define PROC_MEM_H

#define MAX_PROCS 8

int  alloc_partition(int *out_base, int *out_limit);
void free_partition(int base);

#endif