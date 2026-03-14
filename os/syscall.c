#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/errno.h>
#include "simulator_lab2.h"
#include "scheduler.h"
#include "kt.h"
#include "memory.h"
#include "console_buf.h"
#include "proc_mem.h"

#define PIPE_WAKE_BROADCAST MAX_PROCS

typedef struct PipeWaiter
{
    kt_sem sem;
} PipeWaiter;

typedef struct ConsoleWriterWaiter
{
    kt_sem sem;
} ConsoleWriterWaiter;

static int pipe_push_write_record(Pipe *p, int n)
{
    if (n <= 0)
        return 0;

    if (p->ws_count >= PIPE_MAX_WRITES)
        return -1;

    p->write_sizes[p->ws_tail] = n;
    p->ws_tail = (p->ws_tail + 1) % PIPE_MAX_WRITES;
    p->ws_count++;
    p->committed_bytes += n;

    return 0;
}

static int pipe_front_write_remaining(Pipe *p)
{
    if (p->ws_count == 0)
        return 0;

    return p->write_sizes[p->ws_head];
}

static void pipe_pop_front_write(Pipe *p)
{
    if (p->ws_count == 0)
        return;

    p->ws_head = (p->ws_head + 1) % PIPE_MAX_WRITES;
    p->ws_count--;
}

static void pipe_hold(Pipe *p)
{
    P_kt_sem(p->lock);
    p->active_ops++;
    V_kt_sem(p->lock);
}

static void pipe_release(Pipe *p)
{
    int free_now = 0;

    P_kt_sem(p->lock);
    p->active_ops--;
    if (p->active_ops == 0 && p->reader_refs == 0 && p->writer_refs == 0)
        free_now = 1;
    V_kt_sem(p->lock);

    if (free_now)
    {
        if (p->blocked_readers != NULL)
            free(p->blocked_readers);
        if (p->blocked_writers != NULL)
            free(p->blocked_writers);
        free(p);
    }
}

static void pipe_enqueue_waiter(Dllist q, PipeWaiter *w)
{
    dll_append(q, new_jval_v((void *)w));
}

static void pipe_wake_one_waiter(Dllist q)
{
    if (q == NULL || dll_empty(q))
        return;

    Dllist n = dll_first(q);
    PipeWaiter *w = (PipeWaiter *)jval_v(dll_val(n));
    dll_delete_node(n);
    V_kt_sem(w->sem);
}

static void pipe_wake_all_waiters(Dllist q)
{
    while (q != NULL && !dll_empty(q))
        pipe_wake_one_waiter(q);
}

static void *FinishFork(void *arg)
{
    struct PCB_struct *child = (struct PCB_struct *)arg;
    SysCallReturn(child, 0);
    return NULL;
}

/*
    - output to console
*/
void *do_write(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    int fd = pcb->registers[5];    // arg1
    int buf_u = pcb->registers[6]; // arg2 (user addr)
    int n = pcb->registers[7];     // arg3 (count)

    // validating user buffer
    if (buf_u < 0)
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // validating count
    if (n < 0)
    {
        SysCallReturn(pcb, -EINVAL);
        return NULL;
    }

    // validating user buffer
    if (!ValidRange(pcb, buf_u, n))
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // validating fd range and making sure it is not in use
    if (fd < 0 || fd >= MAX_FD || pcb->fds[fd].in_use == 0)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }

    FDEntry *f = &pcb->fds[fd];
    char *kbuf = (char *)U2K(pcb, buf_u);

    // console path
    if (f->kind == FD_CONSOLE)
    {   
        // making sure it is write mode
        if (f->mode != FD_WRITE)
        {
            SysCallReturn(pcb, -EBADF);
            return NULL;
        }

        ConsoleWriterWaiter waiter;
        int waiter_init = 0;

        // Preserve FIFO order among console writers explicitly instead of
        // relying on kt_sem wake ordering.
        P_kt_sem(console_writer_state_lock);
        if (console_writer_busy || !dll_empty(console_writer_q))
        {
            waiter.sem = make_kt_sem(0);
            waiter_init = 1;
            dll_append(console_writer_q, new_jval_v((void *)&waiter));
            V_kt_sem(console_writer_state_lock);
            P_kt_sem(waiter.sem);
        }
        else
        {
            console_writer_busy = 1;
            V_kt_sem(console_writer_state_lock);
        }

        // Keep the existing writer semaphore as the actual device exclusion.
        P_kt_sem(writers);
        int written = 0;

        // write one byte at a time
        while (written < n)
        {
            console_write(kbuf[written]);
            P_kt_sem(writeok);
            written++;
        }

        // release writer block
        V_kt_sem(writers);

        P_kt_sem(console_writer_state_lock);
        if (!dll_empty(console_writer_q))
        {
            Dllist n = dll_first(console_writer_q);
            ConsoleWriterWaiter *next = (ConsoleWriterWaiter *)jval_v(dll_val(n));
            dll_delete_node(n);
            V_kt_sem(next->sem);
        }
        else
        {
            console_writer_busy = 0;
        }
        V_kt_sem(console_writer_state_lock);

        if (waiter_init)
            kill_kt_sem(waiter.sem);

        SysCallReturn(pcb, written);
        return NULL;
    }

    // pipe path
    if (f->kind == FD_PIPE)
    {
        // making sure it is in write mode
        if (f->mode != FD_WRITE || f->pipe == NULL)
        {
            SysCallReturn(pcb, -EBADF);
            return NULL;
        }

        Pipe *p = f->pipe;
        int written = 0;
        int rc = 0;
        PipeWaiter writer_waiter;
        int writer_waiter_init = 0;
        int writer_waiter_queued = 0;

        // marks this pipe as active so it does not get freed while this
        // write() system call is still using it
        pipe_hold(p);

        // blocking so the whole write() call can happen
        P_kt_sem(p->writer_sem);

        while (written < n)
        {
            // check if readers still exist before blocking/writing.
            P_kt_sem(p->lock);
            if (p->reader_refs == 0)
            {
                V_kt_sem(p->lock);
                if (written == 0)
                    pipe_wake_one_waiter(p->blocked_writers);
                rc = (written == 0) ? -EPIPE : written;
                break;
            }
            if (p->count >= PIPE_BUF_SIZE)
            {
                if (!writer_waiter_init)
                {
                    writer_waiter.sem = make_kt_sem(0);
                    writer_waiter_init = 1;
                }
                if (!writer_waiter_queued)
                {
                    pipe_enqueue_waiter(p->blocked_writers, &writer_waiter);
                    writer_waiter_queued = 1;
                }
                V_kt_sem(p->lock);
                P_kt_sem(writer_waiter.sem);
                writer_waiter_queued = 0;
                continue;
            }

            // check if readers disappeared while blocked
            if (p->reader_refs == 0)
            {
                V_kt_sem(p->lock);
                if (written == 0)
                    pipe_wake_one_waiter(p->blocked_writers);
                rc = (written == 0) ? -EPIPE : written;
                break;
            }
            
            // write one byte into pipe buffer
            p->buf[p->tail] = kbuf[written]; // store at current tail
            p->tail = (p->tail + 1) % PIPE_BUF_SIZE; // move tail forward
            p->count++; // increment byte count

            // unlock pipe
            V_kt_sem(p->lock);

            written++; // update local progress
        }

        if (written > 0)
        {
            P_kt_sem(p->lock);
            if (pipe_push_write_record(p, written) < 0)
            {
                V_kt_sem(p->lock);
                rc = -EIO;
                V_kt_sem(p->writer_sem);
                pipe_release(p);
                SysCallReturn(pcb, rc);
                return NULL;
            }
            V_kt_sem(p->lock);
            pipe_wake_one_waiter(p->blocked_readers);
        }

        // returning success unless the loop broke on a pipe error/EOF case
        if (written == n)
            rc = written;

        // release writer block
        V_kt_sem(p->writer_sem);

        if (writer_waiter_init)
            kill_kt_sem(writer_waiter.sem);

        // marks this pipe as no longer active for this write() path
        pipe_release(p);

        SysCallReturn(pcb, rc);
        return NULL;
    }

    SysCallReturn(pcb, -EBADF);
    return NULL;
}

/*
    - input from console
*/
void *do_read(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    int fd = pcb->registers[5];    // arg1
    int buf_u = pcb->registers[6]; // arg2 (user addr)
    int n = pcb->registers[7];     // arg3 (count)

    // validating user buffer
    if (buf_u < 0)
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // validating count
    if (n < 0)
    {
        SysCallReturn(pcb, -EINVAL);
        return NULL;
    }

    // validating user buffer
    if (!ValidRange(pcb, buf_u, n))
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // validating file descriptor range + making sure fd is not in use
    if (fd < 0 || fd >= MAX_FD || pcb->fds[fd].in_use == 0)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }

    FDEntry *f = &pcb->fds[fd];
    char *kbuf = (char *)U2K(pcb, buf_u);

    // console path
    if (f->kind == FD_CONSOLE)
    {
        // make sure mode of FD is in read
        if (f->mode != FD_READ)
        {
            SysCallReturn(pcb, -EBADF);
            return NULL;
        }

        // read character 1 by 1
        int i = 0;
        while (i < n)
        {
            int ch = ConsoleBufGetChar(); // blocks if empty
            if (ch == -1) // CTRL-D / EOF
            {
                // if some bytes were already read, defer EOF to the next
                // read() so this one can return the buffered bytes first
                if (i > 0)
                    ConsoleBufSetEOFPending();
                break;
            }
            kbuf[i] = (char)ch;
            i++;
        }

        // return number of bytes reads
        SysCallReturn(pcb, i);
        return NULL;
    }

    // pipe path
    if (f->kind == FD_PIPE)
    {   
        // making sure mode is read and pipe is not null
        if (f->mode != FD_READ || f->pipe == NULL)
        {
            SysCallReturn(pcb, -EBADF);
            return NULL;
        }

        Pipe *p = f->pipe;
        int got = 0; // local counter
        int rc = 0;
        PipeWaiter reader_waiter;
        int reader_waiter_init = 0;
        int reader_waiter_queued = 0;

        // marks this pipe as active so it does not get freed while this
        // read() system call is still using it
        pipe_hold(p);

        while (got < n)
        {
            P_kt_sem(p->lock);

            // Only consume bytes that belong to completed write() calls.
            if (p->committed_bytes > 0 && p->ws_count > 0)
            {
                int freed = 0;

                while (got < n && p->committed_bytes > 0 && p->ws_count > 0)
                {
                    int remaining = pipe_front_write_remaining(p);

                    while (got < n && remaining > 0)
                    {
                        kbuf[got] = p->buf[p->head];
                        p->head = (p->head + 1) % PIPE_BUF_SIZE;
                        p->count--;
                        p->committed_bytes--;
                        got++;
                        freed++;
                        remaining--;
                    }

                    if (remaining == 0)
                    {
                        pipe_pop_front_write(p);
                    }
                    else
                    {
                        p->write_sizes[p->ws_head] = remaining;
                        break;
                    }
                }

                V_kt_sem(p->lock);
                while (freed > 0)
                {
                    pipe_wake_one_waiter(p->blocked_writers);
                    freed--;
                }

                rc = got;
                break;
            }

            // Empty pipe and no writers means EOF. If we already copied some
            // bytes, return them; otherwise return 0 immediately.
            if (p->writer_refs == 0)
            {
                V_kt_sem(p->lock);
                if (got == 0)
                    pipe_wake_one_waiter(p->blocked_readers);
                rc = got;
                break;
            }

            if (!reader_waiter_init)
            {
                reader_waiter.sem = make_kt_sem(0);
                reader_waiter_init = 1;
            }
            if (!reader_waiter_queued)
            {
                pipe_enqueue_waiter(p->blocked_readers, &reader_waiter);
                reader_waiter_queued = 1;
            }
            V_kt_sem(p->lock);
            P_kt_sem(reader_waiter.sem);
            reader_waiter_queued = 0;
        }

        if (got == n)
            rc = got;

        if (reader_waiter_init)
            kill_kt_sem(reader_waiter.sem);

        // marks this pipe as no longer active for this read() path
        pipe_release(p);

        SysCallReturn(pcb, rc);
        return NULL;
    }

    SysCallReturn(pcb, -EBADF);
    return NULL;
}

/*
    - makes stdio believe that stdout is a real terminal
    - ioctl = I/O control
*/
void *do_ioctl(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    int fd = pcb->registers[5];     // arg1 (fd)
    int req = pcb->registers[6];    // arg2 (request code)
    int addr_u = pcb->registers[7]; // arg3 (user address of struct)

    // validating fd range + making sure fd is not in use
    if (fd < 0 || fd >= MAX_FD || pcb->fds[fd].in_use == 0)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }

    FDEntry *f = &pcb->fds[fd];

    // only support ioctl() on console-backed descriptors
    if (f->kind != FD_CONSOLE || req != JOS_TCGETP)
    {
        SysCallReturn(pcb, -EINVAL);
        return NULL;
    }

    // make sure arg3 is >= 0
    if (addr_u < 0)
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // make sure the struct fits in the user address space
    if (!ValidRange(pcb, addr_u, (int)sizeof(struct JOStermios)))
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // translate user address -> simulator memory (kernel pointer)
    struct JOStermios *kaddr = (struct JOStermios *)U2K(pcb, addr_u);

    // fill the user's JOStermios struct
    ioctl_console_fill(kaddr);

    // success
    SysCallReturn(pcb, 0);
    return NULL;
}

/*
    - fstat system call
    - fakes file metadata so stdio can decide how to buffer input/output on stdin/stdout/stderr
*/
void *do_fstat(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    int fd = pcb->registers[5];     // arg1 (fd)
    int stat_u = pcb->registers[6]; // arg2 (user addr of struct KOSstat)

    // validating fd range + making sure fd is not in use
    if (fd < 0 || fd >= MAX_FD || pcb->fds[fd].in_use == 0)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }

    FDEntry *f = &pcb->fds[fd];

    // only support fstat on console-backed descriptors
    if (f->kind != FD_CONSOLE)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }

    // validate user pointer
    if (stat_u < 0)
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // must fit in this process's user memory window
    if (!ValidRange(pcb, stat_u, (int)sizeof(struct KOSstat)))
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // choose buffering size
    int blk_size = (f->mode == FD_READ) ? 1 : 256;

    // translate user addr to simulator memory
    struct KOSstat *kstat = (struct KOSstat *)U2K(pcb, stat_u);

    // fill the stat buffer
    stat_buf_fill(kstat, blk_size);

    // success
    SysCallReturn(pcb, 0);
    return NULL;
}

/*
    - returns PageSize(memory block size) from simulator
*/
void *do_getpagesize(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    // return pagesize constant
    SysCallReturn(pcb, PageSize);
    return NULL;
}

/*
    - grows or shrinks a process's heap
*/
void *do_sbrk(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    int incr = pcb->registers[5]; // arg1: how much to increment

    // old break is what sbrk returns on success, to tell malloc where the new block starts
    int old_brk = pcb->brk;
    long new_brk_long = (long)pcb->brk + (long)incr; // avoiding overflow

    // enforcing 0 < new_brk_long < pcb->Limit

    if (new_brk_long < 0)
    {
        SysCallReturn(pcb, -EINVAL);
        return NULL;
    }

    if (new_brk_long > (long)pcb->limit)
    {
        SysCallReturn(pcb, -ENOMEM);
        return NULL;
    }

    pcb->brk = (int)new_brk_long;

    // returning prev. break
    SysCallReturn(pcb, old_brk);
    return NULL;
}

/*
    - replaces current process image with new program
*/
void *do_execve(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    // extracting arguments
    int path_u = pcb->registers[5];
    int argv_u = pcb->registers[6];

    // pointer validity
    if (path_u < 0 || argv_u < 0)
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // validate that we can read at least 1 byte of the filename
    if (!ValidRange(pcb, path_u, 1) || !ValidRange(pcb, argv_u, (int)sizeof(int)))
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // copying path into kernel memory
    int path_len = 0;
    int err = u_strlen_bounded(pcb, path_u, &path_len);
    if (err != 0)
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // copy filename into kernel memory
    char *kpath_copy = (char *)malloc((size_t)path_len);
    if (!kpath_copy)
    {
        SysCallReturn(pcb, -ENOMEM);
        return NULL;
    }
    memcpy(kpath_copy, (char *)U2K(pcb, path_u), (size_t)path_len);

    // copying argv pointers into kernel memory
    int max_args = 256; // bounding the max args to 256
    char **kargv = (char **)malloc((size_t)(max_args + 1) * sizeof(char *));
    if (!kargv)
    {
        free(kpath_copy);
        SysCallReturn(pcb, -ENOMEM);
        return NULL;
    }

    int i;
    for (i = 0; i <= max_args; i++)
        kargv[i] = NULL;


    // allocate/copy argv vector into kernel memory
    int argc = 0;
    while (argc < max_args)
    {
        int ptr_u_addr = argv_u + argc * (int)sizeof(int);
        if (!ValidRange(pcb, ptr_u_addr, (int)sizeof(int)))
        {
            err = EFAULT;
            break;
        }

        int arg_u = *(int *)U2K(pcb, ptr_u_addr);
        if (arg_u == 0)
        {
            kargv[argc] = NULL;
            err = 0;
            break;
        }

        int arg_len = 0;
        err = u_strlen_bounded(pcb, arg_u, &arg_len);
        if (err != 0)
        {
            err = EFAULT;
            break;
        }

        kargv[argc] = (char *)malloc((size_t)arg_len);
        if (!kargv[argc])
        {
            err = ENOMEM;
            break;
        }

        memcpy(kargv[argc], (char *)U2K(pcb, arg_u), (size_t)arg_len);
        argc++;
    }

    if (argc == max_args && err == 0)
    {
        err = E2BIG;
    }

    if (err != 0)
    {
        for (int j = 0; j < max_args; j++)
        {
            if (kargv[j])
                free(kargv[j]);
        }
        free(kargv);
        free(kpath_copy);
        SysCallReturn(pcb, -err);
        return NULL;
    }

    // debug logging
    DEBUG('e', "execve(copy done): path=\"%s\"\n", kpath_copy);
    for (int j = 0; kargv[j] != NULL; j++)
    {
        DEBUG('e', "execve(copy done): argv[%d]=\"%s\"\n", j, kargv[j]);
    }

    // save the registers for error rollback in the case that PerformExecve fails
    int saved_regs[NumTotalRegs];
    memcpy(saved_regs, pcb->registers, sizeof(saved_regs));

    // make SysCallReturn leave PC at 0
    pcb->registers[NextPCReg] = 0;

    // try the path exactly as provided first
    int perr = PerformExecve(pcb, kpath_copy, kargv);

    // executable path fallback
    char relpath[512];
    if (perr != 0 && strchr(kpath_copy, '/') == NULL)
    {
        snprintf(relpath, sizeof(relpath), "./%s", kpath_copy);
        perr = PerformExecve(pcb, relpath, kargv);
    }
    
    // error path if PerformExecve fails
    if (perr != 0)
    {
        // exec failed: restore old context so caller continues
        memcpy(pcb->registers, saved_regs, sizeof(saved_regs));

        // free mallocs from step 8
        for (int j = 0; kargv[j] != NULL; j++)
            free(kargv[j]);
        free(kargv);
        free(kpath_copy);

        SysCallReturn(pcb, -perr);
        return NULL;
    }

    // free before returning
    for (int j = 0; kargv[j] != NULL; j++)
        free(kargv[j]);
    free(kargv);
    free(kpath_copy);

    // success path
    SysCallReturn(pcb, 0);
    return NULL;
}

/*
    - returns PID
*/
void *do_getpid(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;
    SysCallReturn(pcb, pcb->pid);
    return NULL;
}

/*
    - creates a child process by cloning the current one
*/
void *do_fork(void *arg)
{
    struct PCB_struct *parent = (struct PCB_struct *)arg;

    // finds a free partion for the child
    int child_base, child_limit;
    if (alloc_partition(&child_base, &child_limit) < 0)
    {
        SysCallReturn(parent, -EAGAIN);
        return NULL;
    }

    // create a new PCB for the child
    struct PCB_struct *child = malloc(sizeof(*child));
    if (!child)
    {
        free_partition(child_base);
        SysCallReturn(parent, -ENOMEM);
        return NULL;
    }

    child->base = child_base;
    child->limit = child_limit;

    child->parent = parent;

    // initializing child exit status
    child->exited = 0;
    child->exit_status = 0;

    child->waiter_sem = make_kt_sem(0);
    child->waiters = new_dllist();

    // copying over program state
    memcpy(child->registers, parent->registers, sizeof(child->registers));
    child->brk = parent->brk;

    child->pid = get_new_pid();

    child->children = make_jrb();

    jrb_insert_int(parent->children, child->pid, new_jval_v(child));

    memcpy(main_memory + child->base,
           main_memory + parent->base,
           (size_t)child->limit);
    
    memcpy(child->fds, parent->fds, sizeof(child->fds));

    // bumping pipe refcounts for each duplicated child FD
    for (int i = 0; i < MAX_FD; i++)
    {
        FDEntry *f = &child->fds[i];
        if (!f->in_use) continue; // if fd slot is not allocated skip it
        if (f->kind != FD_PIPE || f->pipe == NULL) continue; // skip if it is not a pipe

        Pipe *p = f->pipe;
        P_kt_sem(p->lock);

        // bumping reader ref
        if (f->mode == FD_READ)
        {
            p->reader_refs++;
        }
        // bumping righter refs
        else if (f->mode == FD_WRITE)
        {
            p->writer_refs++;
        }

        V_kt_sem(p->lock);
    }

    kt_fork(FinishFork, (void *)child);
    SysCallReturn(parent, child->pid);

    return NULL;
}

/*
    - wakes up all readers up to MAX_PROCS(8)
*/
static void pipe_broadcast_readers(Pipe *p)
{
    pipe_wake_one_waiter(p->blocked_readers);
}

/*
    - wakes up all writers up to MAX_PROCS(8)
*/
static void pipe_broadcast_writers(Pipe *p)
{
    pipe_wake_one_waiter(p->blocked_writers);
}

/*
    - internal close primitive: no SysCallReturn, reusable by do_close/do_exit/dup2
*/
static int close_fd_internal(struct PCB_struct *pcb, int fd)
{
    // validating pcb and fds
    if (pcb == NULL || fd < 0 || fd >= MAX_FD || pcb->fds[fd].in_use == 0)
        return -EBADF;

    FDEntry *f = &pcb->fds[fd];

    // console close, just clear the entry
    if (f->kind == FD_CONSOLE)
    {
        f->in_use = 0;
        f->kind = FD_NONE;
        f->mode = 0;
        f->pipe = NULL;
        return 0;
    }

    // pipe close
    if (f->kind == FD_PIPE)
    {
        Pipe *p = f->pipe;
        
        // returning error if no pipe
        if (p == NULL)
        {
            f->in_use = 0;
            f->kind = FD_NONE;
            f->mode = 0;
            f->pipe = NULL;
            return -EBADF;
        }

        // flags
        int became_no_writers = 0;
        int became_no_readers = 0;
        int free_pipe = 0;

        P_kt_sem(p->lock);
        
        // decrementing reader refs
        if (f->mode == FD_READ)
        {
            if (p->reader_refs > 0) p->reader_refs--;
            if (p->reader_refs == 0) became_no_readers = 1;
        }
        // decrementing writer refs
        else if (f->mode == FD_WRITE)
        {
            if (p->writer_refs > 0) p->writer_refs--;
            if (p->writer_refs == 0) became_no_writers = 1;
        }
        else
        {
            V_kt_sem(p->lock);

            // clearing entry if invalid mode
            f->in_use = 0;
            f->kind = FD_NONE;
            f->mode = 0;
            f->pipe = NULL;
            return -EBADF;
        }

        // only free pipe if no refs and active ops remain
        if (p->reader_refs == 0 && p->writer_refs == 0 && p->active_ops == 0)
            free_pipe = 1;

        V_kt_sem(p->lock);

        // clearing caller's fd slot
        f->in_use = 0;
        f->kind = FD_NONE;
        f->mode = 0;
        f->pipe = NULL;

        // wake blocked sides if one end disappears
        if (became_no_writers) pipe_broadcast_readers(p);
        if (became_no_readers) pipe_broadcast_writers(p);
        if (free_pipe) free(p);

        return 0;
    }

    return -EBADF;
}

/*
    - process termination
    - zombie/orphan management
*/
void *do_exit(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;

    int status = pcb->registers[5]; // exit(status)

    pcb->exit_status = status;
    pcb->exited = 1;

    // close all open FDs so pipe refs/wakeups/free happen on process death
    for (int fd = 0; fd < MAX_FD; fd++)
    {
        if (pcb->fds[fd].in_use)
        {
            close_fd_internal(pcb, fd);   // ignore rc during exit
        }
    }

    // free process memory partition immediately
    // process is dead, memory should be resusuable immediately
    free_partition(pcb->base);
    
    // reparent live children to Init process
    if (pcb->children != NULL)
    {
        while (!jrb_empty(pcb->children))
        {

            // grab node (first in sorted order)
            JRB n = jrb_first(pcb->children);
            struct PCB_struct *ch = (struct PCB_struct *)jval_v(n->val);

            // remove from old parent tree
            jrb_delete_node(n);

            // reparent to Init
            ch->parent = Init_pcb;

            if (Init_pcb->children == NULL)
                Init_pcb->children = make_jrb();
            jrb_insert_int(Init_pcb->children, ch->pid, new_jval_v(ch));
        }
    }

    // zombie cleanup
    if (pcb->waiters != NULL)
    {
        int zombie_count = 0;
        Dllist tmp;
        dll_traverse(tmp, pcb->waiters) { zombie_count++; }
        while (!dll_empty(pcb->waiters))
        {
            Dllist d = dll_first(pcb->waiters);
            struct PCB_struct *z = (struct PCB_struct *)jval_v(dll_val(d));
            dll_delete_node(d);

            // z is already a zombie: its partition was freed in z's do_exit()
            destroy_pid(z->pid);
            free(z);
        }
    }

    // remove self from parent's live-children tree
    if (pcb->parent != NULL)
    {
        JRB n = jrb_find_int(pcb->parent->children, pcb->pid);
        if (n != NULL)
            jrb_delete_node(n);

        extern Dllist init_reapq;
        
        // if the parent is Init, put it onto the init_reapq to be cleaned up later
        if (pcb->parent == Init_pcb)
        {
            if (init_reapq == NULL)
                init_reapq = new_dllist();
            dll_append(init_reapq, new_jval_v(pcb));

            kt_exit(NULL);
            return NULL;
        }

        // normal zombie path
        dll_append(pcb->parent->waiters, new_jval_v(pcb));
        V_kt_sem(pcb->parent->waiter_sem);
    }

    kt_exit(NULL);
    return NULL;
}

/*
    - says how many file descriptors can be open at once
*/
void *do_getdtablesize(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;
    SysCallReturn(pcb, 64);
    return NULL;
}

/*
    - closes a file descriptor
    - if the fd s a console fd, it just clears the slot
    - if the fd is a pipe fd, it updates the shared pipe state, wakes blocked readers/writers if needed, and possibly frees the pipe
*/
void *do_close(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;
    int fd = pcb->registers[5];

    int rc = close_fd_internal(pcb, fd);
    SysCallReturn(pcb, rc);
    return NULL;
}

/*
    - waits for a child process to exit and reaps its zombie metadata
*/
void *do_wait(void *arg)
{
    struct PCB_struct *parent = (struct PCB_struct *)arg;
    int status_u = parent->registers[5]; // how the child tells parent how it exited

    // validation
    if (status_u != 0)
    {
        if (status_u < 0 || !ValidRange(parent, status_u, (int)sizeof(int)))
        {
            SysCallReturn(parent, -EFAULT);
            return NULL;
        }
    }

    // snapshot of child state
    int has_live_children = (parent->children != NULL && !jrb_empty(parent->children));
    int has_zombies = (parent->waiters != NULL && !dll_empty(parent->waiters));

    // if there are no children at all return
    if (!has_live_children && !has_zombies)
    {
        SysCallReturn(parent, -ECHILD);
        return NULL;
    }

    // consume one zombie-arrival token per successful wait(), regardless of whether a zombie was already queued when wait() was called.
    P_kt_sem(parent->waiter_sem);

    // there should now be at least one zombie queued.
    if (dll_empty(parent->waiters))
    {
        SysCallReturn(parent, -ECHILD);
        return NULL;
    }

    // grab the first zombie child from parents waiters list
    Dllist n = dll_first(parent->waiters);
    struct PCB_struct *child = (struct PCB_struct *)jval_v(dll_val(n));
    dll_delete_node(n);

    // save child PID
    int child_pid = child->pid;

    // write status into parent's memory
    if (status_u != 0)
    {
        int *kstatus = (int *)U2K(parent, status_u);
        *kstatus = (child->exit_status & 0xff) << 8;
    }

    // reclaim zombie metadata (memory partition was freed in do_exit)
    destroy_pid(child_pid);
    free(child);

    SysCallReturn(parent, child_pid);
    return NULL;
}

/*
    - returns parent PID if it exists
*/
void *do_getppid(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;
    int ppid = (pcb->parent ? pcb->parent->pid : 0);
    SysCallReturn(pcb, ppid);
    return NULL;
}

/*
    - returns the lowest free fd index in this process, or -1 if full
*/
static int fd_alloc_lowest_free(struct PCB_struct *pcb)
{
    int i;
    // scanning FD table
    for (i = 0; i < MAX_FD; i++)
    {   
        // returning lowest free index
        if (pcb->fds[i].in_use == 0)
        {
            return i;
        }
    }
    // FD table is full
    return -1;
}

/*
    - allocates and initializes a fresh pipe object
    - returns NULL on allocation failure
*/
static Pipe *pipe_create(void)
{
    // allocating the pipe
    Pipe *p = (Pipe *)malloc(sizeof(Pipe));
    if (p == NULL)
    {
        return NULL;
    }

    // both head and tail point to the start of the buffer
    p->head = 0;
    p->tail = 0;

    // zero bytes in pipe initially
    p->count = 0;
    p->committed_bytes = 0;
    p->ws_head = 0;
    p->ws_tail = 0;
    p->ws_count = 0;

    // only one read and write FD right now
    p->reader_refs = 1;
    p->writer_refs = 1;

    // no active operations initially
    p->active_ops = 0;

    p->lock = make_kt_sem(1); // starts unlocked
    p->data_sem = make_kt_sem(0); // no bytes available for readers yet to read, signals for readers to block
    p->record_sem = make_kt_sem(0); // no completed write() records available initially
    p->space_sem = make_kt_sem(PIPE_BUF_SIZE); // all slots are free initially, writers can write to full capacity
    p->writer_sem = make_kt_sem(1); // only 1 writer allowed at a time, no writers currently
    p->blocked_readers = new_dllist();
    p->blocked_writers = new_dllist();

    return p;
}

/*
    - Creates a unidirectional communication channel between processes.
    - allocates 2 file descriptors at read-end and write-end of pipe
*/
void *do_pipe(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;
    int pd_u = pcb->registers[5]; // arr of two file descriptors for user

    // validate user pointer if it has space for two ints
    if (pd_u < 0 || !ValidRange(pcb, pd_u, (int)(2 * sizeof(int))))
    {
        SysCallReturn(pcb, -EFAULT);
        return NULL;
    }

    // finding two free file descriptors
    int fd_r = fd_alloc_lowest_free(pcb);
    if (fd_r < 0)
    {
        SysCallReturn(pcb, -EMFILE);
        return NULL;
    }

    // updating the read FD within the FD table
    pcb->fds[fd_r].in_use = 1;
    pcb->fds[fd_r].kind = FD_NONE;
    pcb->fds[fd_r].mode = 0;
    pcb->fds[fd_r].pipe = NULL;

    // allocating the write FD
    int fd_w = fd_alloc_lowest_free(pcb);

    // if the allocation failed then cleanup the read FD and return
    if (fd_w < 0)
    {
        pcb->fds[fd_r].in_use = 0;
        pcb->fds[fd_r].kind = FD_NONE;
        pcb->fds[fd_r].mode = 0;
        pcb->fds[fd_r].pipe = NULL;
        SysCallReturn(pcb, -EMFILE);
        return NULL;
    }

    // updating the write FD within the FD table
    pcb->fds[fd_w].in_use = 1;
    pcb->fds[fd_w].kind = FD_NONE;
    pcb->fds[fd_w].mode = 0;
    pcb->fds[fd_w].pipe = NULL;

    // creating the actual pipe object
    Pipe *p = pipe_create();

    // cleanup if allocating pipe failed
    if (p == NULL)
    {
        pcb->fds[fd_w].in_use = 0;
        pcb->fds[fd_w].kind = FD_NONE;
        pcb->fds[fd_w].mode = 0;
        pcb->fds[fd_w].pipe = NULL;

        pcb->fds[fd_r].in_use = 0;
        pcb->fds[fd_r].kind = FD_NONE;
        pcb->fds[fd_r].mode = 0;
        pcb->fds[fd_r].pipe = NULL;

        SysCallReturn(pcb, -ENOMEM);
        return NULL;
    }

    // updating read FD table entry w/ pipe
    pcb->fds[fd_r].in_use = 1;
    pcb->fds[fd_r].kind = FD_PIPE;
    pcb->fds[fd_r].mode = FD_READ;
    pcb->fds[fd_r].pipe = p;

    // updating write FD table entry w/ pipe
    pcb->fds[fd_w].in_use = 1;
    pcb->fds[fd_w].kind = FD_PIPE;
    pcb->fds[fd_w].mode = FD_WRITE;
    pcb->fds[fd_w].pipe = p;

    // returning fd pair to user: pd[0]=read end, pd[1]=write end.
    int *pd_k = (int *)U2K(pcb, pd_u);
    pd_k[0] = fd_r;
    pd_k[1] = fd_w;

    SysCallReturn(pcb, 0);
    return NULL;
}

/*
    - Creates a duplicate of the file descriptor fd at lowest available fd
    - refers to same open file as fd
*/
void *do_dup(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;
    int oldfd = pcb->registers[5];

    // validating oldfd
    if (oldfd < 0 || oldfd >= MAX_FD || pcb->fds[oldfd].in_use == 0)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }
    
    // finding lowest free fd
    int newfd = fd_alloc_lowest_free(pcb);
    if (newfd < 0)
    {
        SysCallReturn(pcb, -EMFILE);
        return NULL;
    }

    FDEntry *src = &pcb->fds[oldfd];
    FDEntry *dst = &pcb->fds[newfd];

    // copying descriptor entry
    dst->in_use = 1;
    dst->kind = src->kind;
    dst->mode = src->mode;
    dst->pipe = src->pipe;

    // increasing pipe refs if needed
    if (dst->kind == FD_PIPE && dst->pipe != NULL)
    {
        Pipe *p = dst->pipe;
        P_kt_sem(p->lock);

        // increasing reader refs under lock
        if (dst->mode == FD_READ)
            p->reader_refs++;
        else if (dst->mode == FD_WRITE) // increasing writer refs under lock
            p->writer_refs++;
        else
        {
            V_kt_sem(p->lock); // releasing lock
            
            // invalid mode, so clearing fd entry -> returning error
            dst->in_use = 0;
            dst->kind = FD_NONE;
            dst->mode = 0;
            dst->pipe = NULL;

            SysCallReturn(pcb, -EBADF);
            return NULL;
        }
        V_kt_sem(p->lock);
    }

    // returning newfd
    SysCallReturn(pcb, newfd);
    return NULL;
}

/*
    - Duplicates the file descriptor oldfd onto newfd.
    - if newfd is already opened it closes it first
    - After duplication, both oldfd and newfd refer to the same open file description
*/
void *do_dup2(void *arg)
{
    struct PCB_struct *pcb = (struct PCB_struct *)arg;
    int oldfd = pcb->registers[5];
    int newfd = pcb->registers[6];

    // validating fd ranges
    if (oldfd < 0 || oldfd >= MAX_FD || newfd < 0 || newfd >= MAX_FD)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }

    // oldfd must be open
    if (pcb->fds[oldfd].in_use == 0)
    {
        SysCallReturn(pcb, -EBADF);
        return NULL;
    }

    // dup2(x, x) is trivial
    if (oldfd == newfd)
    {
        SysCallReturn(pcb, newfd);
        return NULL;
    }

    // if newfd is open, close it first
    if (pcb->fds[newfd].in_use)
    {
        int cerr = close_fd_internal(pcb, newfd);
        if (cerr < 0)
        {
            SysCallReturn(pcb, cerr);
            return NULL;
        }
    }
    
    FDEntry *src = &pcb->fds[oldfd];
    FDEntry *dst = &pcb->fds[newfd];

    // copying oldfd -> newfd
    dst->in_use = 1;
    dst->kind = src->kind;
    dst->mode = src->mode;
    dst->pipe = src->pipe;

    // increasing pipe refs if needed
    if (dst->kind == FD_PIPE && dst->pipe != NULL)
    {
        Pipe *p = dst->pipe;
        P_kt_sem(p->lock);

        // increasing reader ref count
        if (dst->mode == FD_READ)
            p->reader_refs++;
        // increasing writer ref count
        else if (dst->mode == FD_WRITE)
            p->writer_refs++;
        else
        {
            V_kt_sem(p->lock);
            
            // invalid mode, freeing fd entry -> returning error
            dst->in_use = 0;
            dst->kind = FD_NONE;
            dst->mode = 0;
            dst->pipe = NULL;
            SysCallReturn(pcb, -EBADF);

            return NULL;
        }
        V_kt_sem(p->lock);
    }

    // returning newfd
    SysCallReturn(pcb, newfd);
    return NULL;
}
