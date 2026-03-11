#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <sys/errno.h>
#include "dllist.h"
#include "kt.h"
#include "jrb.h"
#include "jval.h"
#include "simulator_lab2.h"
#include "scheduler.h"
#include "proc_mem.h"

Dllist readyq;            // runnable user processes
Dllist init_reapq = NULL; // zombie children of Init waiting to be freed by the scheduler
extern char *Argv[];
struct PCB_struct *Current_pcb; // process currently running on CPU
int Idle;
static JRB pid_tree = NULL;         // tracks allocated PIDs to reuse the lowest available
struct PCB_struct *Init_pcb = NULL; // pid-0 process used for orphans and shutdown detection
static int part_inuse[MAX_PROCS];   // 8-partition memory allocation table

/*
   - scans partition table for a free slot and gies back the base and limit
   - returns -1 otherwise
*/
int alloc_partition(int *out_base, int *out_limit)
{
    int part_size = MemorySize / MAX_PROCS;

    // finidng a valid partition for fork
    for (int i = 0; i < MAX_PROCS; i++)
    {
        if (!part_inuse[i])
        {
            part_inuse[i] = 1;
            *out_base = i * part_size;
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
    if (idx < 0 || idx >= MAX_PROCS)
        return;
    part_inuse[idx] = 0;
}

/*
    - returns the lowest unused PID
    - inserts into the rb-tree
*/
int get_new_pid(void)
{
    int pid = 1;

    if (pid_tree == NULL)
        pid_tree = make_jrb();

    while (jrb_find_int(pid_tree, pid) != NULL)
        pid++;

    jrb_insert_int(pid_tree, pid, new_jval_i(pid));
    return pid;
}

/*
    - removes PID from tree if present
*/
void destroy_pid(int pid)
{
    if (pid_tree == NULL)
        return;

    JRB n = jrb_find_int(pid_tree, pid);
    if (n != NULL)
        jrb_delete_node(n);
}

/*
    - initializes the FD table defaults
    - sets FDs 0/1/2 to stdin/stdout/stderr
    - leaves the others as free
*/
static void InitFDTable(struct PCB_struct *pcb)
{   
    // initialize all FDs as free within the FD table
    int i;
    for (i = 0; i < MAX_FD; i++) {
        pcb->fds[i].in_use = 0;
        pcb->fds[i].kind = FD_NONE;
        pcb->fds[i].mode = 0;
        pcb->fds[i].pipe = NULL;
    }

    // stdin
    pcb->fds[0].in_use = 1;
    pcb->fds[0].kind = FD_CONSOLE;
    pcb->fds[0].mode = FD_READ;

    // stdout
    pcb->fds[1].in_use = 1;
    pcb->fds[1].kind = FD_CONSOLE;
    pcb->fds[1].mode = FD_WRITE;

    // stderr
    pcb->fds[2].in_use = 1;
    pcb->fds[2].kind = FD_CONSOLE;
    pcb->fds[2].mode = FD_WRITE;
}


/*
    - takes an existing PCB and turns it into a brand new program
    - destroys the current program
    - loads the new executable
    - rebuilds stack
    - resets CPU state
    - starts execution at instr. 0
*/
int PerformExecve(struct PCB_struct *pcb, const char *fn, char *argv[])
{
    int i;
    int sz;
    int *user_argv;
    int init_sp, new_sp, final_sp;

    // input validation
    if (pcb == NULL || fn == NULL || argv == NULL || argv[0] == NULL)
    {
        return EINVAL;
    }

    // initializing memory
    User_Base = pcb->base;
    User_Limit = pcb->limit;

    // clear the RAM of just this process
    bzero(main_memory + pcb->base, pcb->limit);

    sz = load_user_program((char *)fn);
    if (sz < 0)
    {
        // fails if file is too big or space is missing
        return ENOENT;
    }

    // set heap end to end of program
    pcb->brk = sz;

    // reset/clear registers from old PCB
    for (i = 0; i < NumTotalRegs; i++)
        pcb->registers[i] = 0;

    // start executing at instr. 0
    pcb->registers[PCReg] = 0;
    pcb->registers[NextPCReg] = 0;

    // stack pointer intialization
    init_sp = pcb->limit - 12;

    // building args + runtime
    user_argv = InitArgsOnStack(init_sp, argv, pcb->base, &new_sp);
    final_sp = InitCRuntime(new_sp, user_argv, argv, pcb->base);

    // sets the final stack pointer
    pcb->registers[StackReg] = final_sp;

    return 0;
}

/*
    - creates the first real user process at boot, from kos_argv
*/
void *InitUserProcess(void *arg)
{
    struct PCB_struct *pcb;
    char **argv = (char **)arg;
    char *fname = argv[0];

    // allocate new PCB
    pcb = (struct PCB_struct *)malloc(sizeof(struct PCB_struct));
    if (pcb == NULL)
        exit(1);

    // assigning PID to process
    pcb->pid = get_new_pid();

    // initializing FD table w/ defaults
    InitFDTable(pcb);

    // initializing exit status
    pcb->exited = 0;
    pcb->exit_status = 0;

    // allocate the memory partition
    int base, limit;
    if (alloc_partition(&base, &limit) < 0)
        exit(1);

    // assigning a chunk of physical memory to the process
    pcb->base = base;
    pcb->limit = limit;

    // initializing the wait structures
    pcb->waiter_sem = make_kt_sem(0);
    pcb->waiters = new_dllist();
    pcb->children = make_jrb();

    // making the parent the initial process
    pcb->parent = Init_pcb;

    // initializing the list to keep track of zombie children
    if (init_reapq == NULL)
        init_reapq = new_dllist();

    // make sure Init has a children tree
    if (Init_pcb->children == NULL)
        Init_pcb->children = make_jrb();

    // add the first user process as a child of Init
    jrb_insert_int(Init_pcb->children, pcb->pid, new_jval_v(pcb));

    // load + initialize the new program
    int err = PerformExecve(pcb, fname, argv);
    if (err != 0)
    {
        fprintf(stderr, "Can't exec %s (errno=%d)\n", fname, err);
        exit(1);
    }

    // Initial booted process does not return through SysCallReturn(), so it
    // must start with the simulator's expected PC/NextPC pair (0, 4).
    pcb->registers[NextPCReg] = 4;

    // queue the PCB
    dll_append(readyq, new_jval_v((void *)pcb));

    // strart the timer for periodic preemption
    start_timer(10);

    kt_exit(NULL);
    return NULL;
}

/*
    - kernel scheduler
    - decideds what runs next
    - only place that calls run_user_code()
*/
void ScheduleProcess(void)
{
    // drains the Init zombies and frees the PID and PCB
    if (init_reapq != NULL)
    {
        while (!dll_empty(init_reapq))
        {
            Dllist d = dll_first(init_reapq);
            struct PCB_struct *z = (struct PCB_struct *)jval_v(dll_val(d));
            dll_delete_node(d);

            destroy_pid(z->pid);
            free(z);
        }
    }

    // no runnable processes
    if (readyq == NULL || dll_empty(readyq))
    {

        Current_pcb = NULL;

        // If Init has no children, the system is done -> halt instead of noop
        if (Init_pcb != NULL && Init_pcb->children != NULL && jrb_empty(Init_pcb->children))
        {
            SYSHalt();
        }

        // system is idle but processes (or future interrupts) may stil exist
        noop();
        return;
    }

    // choose next process
    Dllist n = dll_first(readyq);
    Current_pcb = (struct PCB_struct *)jval_v(dll_val(n));
    dll_delete_node(n);

    // put the memory window into the simulator
    User_Base = Current_pcb->base;
    User_Limit = Current_pcb->limit;

    // run user code
    run_user_code(Current_pcb->registers);

    // should never reach here
    Current_pcb = NULL;
    noop();
}

/*
    - returns exectution from kernel mode to user mode
    - runs after system call finishes
*/
void SysCallReturn(struct PCB_struct *pcb, int return_val)
{
    // advancing PC so the syscall does not run again
    pcb->registers[PCReg] = pcb->registers[NextPCReg];
    pcb->registers[NextPCReg] = pcb->registers[PCReg] + 4;

    // putting the return value in r2
    pcb->registers[2] = return_val;

    // put the process back on the ready queue
    dll_append(readyq, new_jval_v((void *)pcb));

    // syscallreturn code goes here
    kt_exit(NULL);
}
