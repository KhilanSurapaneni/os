#ifndef MEMORY_H
#define MEMORY_H

#include "scheduler.h"

int ValidRange(struct PCB_struct *pcb, int uaddr, int len);
void *U2K(struct PCB_struct *pcb, int uaddr);
int u_strlen_bounded(struct PCB_struct *pcb, int uaddr, int *out_len);

#endif