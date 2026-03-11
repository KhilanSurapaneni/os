#include "simulator_lab2.h"
#include "memory.h"

/*
    - returns True if [uaddr, uaddr+len) is valid user-space memory for this process.
*/
int ValidRange(struct PCB_struct *pcb, int uaddr, int len)
{
    if (pcb == NULL)
        return 0;
    if (uaddr < 0)
        return 0;
    if (len < 0)
        return 0;

    // Prevent overflow and enforce upper bound
    if (uaddr > pcb->limit)
        return 0;
    if (len > pcb->limit)
        return 0;
    if (uaddr > pcb->limit - len)
        return 0;

    return 1;
}

/*
    - converts a user address into a kernel pointer.
 */
void *U2K(struct PCB_struct *pcb, int uaddr)
{
    return (void *)(main_memory + pcb->base + uaddr);
}

int u_strlen_bounded(struct PCB_struct *pcb, int uaddr, int *out_len)
{
    // Returns 0 on success, errno on failure. out_len includes the '\0'
    if (uaddr < 0)
        return EFAULT;
    if (!ValidRange(pcb, uaddr, 1))
        return EFAULT;

    int max = pcb->limit - uaddr;
    if (max <= 0)
        return EFAULT;

    char *k = (char *)U2K(pcb, uaddr);
    void *p = memchr(k, '\0', (size_t)max);
    if (p == NULL)
        return EFAULT;

    *out_len = (int)((char *)p - k) + 1;
    return 0;
}