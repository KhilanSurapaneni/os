#ifndef PROC_SCHED_H
#define PROC_SCHED_H

#include "dllist.h"
#include "simulator_lab2.h"
#include <sys/errno.h>
#include "kt.h"
#include "jrb.h"

#define MAX_PROCS 8
#define MAX_FD 64
#define PIPE_BUF_SIZE (8*1024)

/*
    -   tells what an FD points to
    -   FD_NONE: unused slot
    -   FD_CONSOLE: stdin/stdout/stderr-style console endpoint
    -   FD_PIPE: endpoint of a pipe
*/
enum FDKind
{
    FD_NONE = 0,
    FD_CONSOLE = 1,
    FD_PIPE = 2
};

/*
    -   tells allowed direction for an FD entry (read or write)
*/
enum FDMode
{
    FD_READ = 1,
    FD_WRITE = 2
};


typedef struct Pipe
{  
    // FIFO byte ring
    char buf[PIPE_BUF_SIZE]; // where bytes are kept
    int head; // index of next byte to read
    int tail; // index of next slot to write
    int count; // how many bytes are stored in buf

    // how many FDs still reference each end
    int reader_refs; // number of open read-end FDs pointing to this pipe.
    int writer_refs; // number of open write-end FDs pointing to this pipe.
    int active_ops;  // number of do_read/do_write paths currently using p

    kt_sem lock; // mutex around pipe
    kt_sem data_sem; // readers wait here when pipe is empty
    kt_sem space_sem; // writers wait here when pipe is full
    kt_sem writer_sem; // blocking so the whole write() call can happen
} Pipe;

/*
    - slot in process FD table
*/
typedef struct FDEntry
{
    int in_use;
    int kind; // console vs. pipe
    int mode; // read or write
    Pipe *pipe;
} FDEntry;

struct PCB_struct
{
    int pid;
    int registers[NumTotalRegs];
    int brk;
    int base;
    int limit;
    int exited;
    int exit_status;
    struct PCB_struct *parent;
    Dllist waiters;
    kt_sem waiter_sem;
    JRB children;
    FDEntry fds[MAX_FD];
};

int alloc_partition(int *out_base, int *out_limit);
void free_partition(int base);

extern Dllist readyq;
extern struct PCB_struct *Current_pcb;
extern int Idle;

extern struct PCB_struct *Init_pcb;

void *InitUserProcess(void *arg);
void ScheduleProcess(void);
void SysCallReturn(struct PCB_struct *pcb, int return_val);
int PerformExecve(struct PCB_struct *pcb, const char *fn, char *argv[]);
int get_new_pid(void);
void destroy_pid(int pid);

#endif
