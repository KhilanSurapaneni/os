/*
 * exception.c -- stub to handle user mode exceptions, including system calls
 *
 * Everything else core dumps.
 *
 * Copyright (c) 1992 The Regents of the University of California. All rights
 * reserved.  See copyright.h for copyright notice and limitation of
 * liability and disclaimer of warranty provisions.
 */
#include <stdlib.h>

#include "simulator_lab2.h"
#include "scheduler.h"
#include "kt.h"
#include "syscall.h"
#include "console_buf.h"

/*
	- runs when user code causes an exception, including syscalls
*/
void exceptionHandler(ExceptionType which)
{
	int type = 0; // syscall number
	int r5 = 0;

	switch (which)
	{
	case SyscallException:

		if (Current_pcb == NULL)
		{
			DEBUG('e', "Unknown system call %d\n", type);
			SYSHalt();
		}

		// saving user register
		examine_registers(Current_pcb->registers);

		// extracting syscall
		type = Current_pcb->registers[4];

		// Debug
		int a0 = Current_pcb->registers[5];
		int a1 = Current_pcb->registers[6];
		int a2 = Current_pcb->registers[7];
		DEBUG('e', "Syscall %d a0=%d a1=%d a2=%d\n", type, a0, a1, a2);

		r5 = Current_pcb->registers[5]; // r5

		switch (type)
		{
		case SYS_halt:
			DEBUG('e', "Halt initiated by user program\n");
			SYSHalt();
			break;

		case SYS_exit:
			DEBUG('e', "_exit() system call\n");

			// // debug function to print the console
			// DumpConsoleBuffer();

			kt_fork(do_exit, (void *)Current_pcb);
			break;

		case SYS_write:
			// fork the handler thread
			kt_fork(do_write, (void *)Current_pcb);
			break;

		case SYS_read:
			// fork the handler thread
			kt_fork(do_read, (void *)Current_pcb);
			break;

		case SYS_ioctl:
			// fork the handler thread
			kt_fork(do_ioctl, (void *)Current_pcb);
			break;

		case SYS_fstat:
			// fork the handler thread
			kt_fork(do_fstat, (void *)Current_pcb);
			break;

		case SYS_getpagesize:
			// fork the handler thread
			kt_fork(do_getpagesize, (void *)Current_pcb);
			break;

		case SYS_sbrk:
			kt_fork(do_sbrk, (void *)Current_pcb);
			break;

		case SYS_execve:
			kt_fork(do_execve, (void *)Current_pcb);
			break;

		case SYS_getpid:
			kt_fork(do_getpid, (void *)Current_pcb);
			break;

		case SYS_fork:
			kt_fork(do_fork, (void *)Current_pcb);
			break;

		case SYS_getdtablesize:
			kt_fork(do_getdtablesize, (void *)Current_pcb);
			break;

		case SYS_close:
			kt_fork(do_close, (void *)Current_pcb);
			break;

		case SYS_wait:
			kt_fork(do_wait, (void *)Current_pcb);
			break;

		case SYS_getppid:
			kt_fork(do_getppid, (void *)Current_pcb);
			break;
		
		case SYS_pipe:
			kt_fork(do_pipe, (void *)Current_pcb);
			break;

		case SYS_dup:
			kt_fork(do_dup, (void *)Current_pcb);
			break;
		
		case SYS_dup2:
			kt_fork(do_dup2, (void *)Current_pcb);
			break;

		default:
			DEBUG('e', "Unknown system call %d\n", type);
			SYSHalt();
			break;
		}
		break;

	case PageFaultException:
		DEBUG('e', "Exception PageFaultException\n");
		break;
	case BusErrorException:
		DEBUG('e', "Exception BusErrorException\n");
		break;
	case AddressErrorException:
		DEBUG('e', "Exception AddressErrorException\n");
		break;
	case OverflowException:
		DEBUG('e', "Exception OverflowException\n");
		break;
	case IllegalInstrException:
		DEBUG('e', "Exception IllegalInstrException\n");
		break;
	default:
		printf("Unexpected user mode exception %d %d\n", which, type);
		exit(1);
	}

	// wait for kernel threads to finish executing
	kt_joinall();
	// return back to user process
	ScheduleProcess();
}

/*
	- runs when a device interrupt fires
	- for async events
*/
void interruptHandler(IntType which)
{
	if (Current_pcb != NULL)
	{
		examine_registers(Current_pcb->registers);			 // save user regs
		dll_append(readyq, new_jval_v((void *)Current_pcb)); // put back on readyq
		Current_pcb = NULL;									 // no longer “running”
	}

	switch (which)
	{
	case ConsoleReadInt:
		DEBUG('e', "ConsoleReadInt interrupt\n");
		V_kt_sem(consoleWait); // signals that a char is ready
		break;
	case ConsoleWriteInt:
		DEBUG('e', "ConsoleWriteInt interrupt\n");
		V_kt_sem(writeok); // signals that console finished writing a character
		break;
	case TimerInt:
		DEBUG('e', "TimerInt interrupt\n");
		break;
	default:
		DEBUG('e', "Unknown interrupt\n");
		break;
	}

	// wait for kernel threads to finish executing
	kt_joinall();
	// return back to user process
	ScheduleProcess();
}
