#include <stdlib.h>

#include "simulator_lab2.h"
#include "scheduler.h"
#include "console_buf.h"
#include "kt.h"

/*
    - starting point
*/
void KOS(void)
{
    // holds runnable user processes
    readyq = new_dllist();

    // write sempahores
    writeok = make_kt_sem(0);
    writers = make_kt_sem(1);

    // read semaphores
    consoleWait = make_kt_sem(0);
    nelem = make_kt_sem(0);
    nslots = make_kt_sem(256);

    // says that "no process is running yet"
    Current_pcb = NULL;

    // creating an "adoptive parent" process, to handle orphans, zombies, etc...
    Init_pcb = malloc(sizeof(*Init_pcb));
    bzero(Init_pcb, sizeof(*Init_pcb));
    Init_pcb->pid = 0;
    Init_pcb->parent = NULL;
    Init_pcb->waiter_sem = make_kt_sem(0);
    Init_pcb->waiters = new_dllist();

    kt_fork(ConsoleReader, NULL);               // start kernel thread that moves console input from device to circular buffer
    kt_fork(InitUserProcess, (void *)kos_argv); // create first user process
    kt_joinall();

    ScheduleProcess();
}