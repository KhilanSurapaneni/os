#include "simulator_lab2.h"
#include "proc_mem.h"

static int part_inuse[MAX_PROCS]; // 8-partition memory allocation table

/*
   - scans partition table for a free slot and gies back the base and limit
   - returns -1 otherwise
*/
int alloc_partition(int *out_base, int *out_limit)
{
    int part_size = MemorySize / MAX_PROCS;

    // finidng a valid partition for fork
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!part_inuse[i]) {
            part_inuse[i] = 1;
            *out_base  = i * part_size;
            *out_limit = part_size;
            return 0;
        }
    }
    return -1;
}

/*
    - given a base value, computes the partition index and marks it as "freed"
*/
void free_partition(int base)
{
    int part_size = MemorySize / MAX_PROCS;
    int idx = base / part_size;
    if (idx < 0 || idx >= MAX_PROCS) return;
    part_inuse[idx] = 0;
}