#ifndef CONSOLE_BUF_H
#define CONSOLE_BUF_H

#include "dllist.h"
#include "kt.h"

// write semaphores
extern kt_sem writeok;
extern kt_sem writers;
extern kt_sem console_writer_state_lock;
extern Dllist console_writer_q;
extern int console_writer_busy;

// read semaphores
extern kt_sem consoleWait;
extern kt_sem nelem;
extern kt_sem nslots;

void *ConsoleReader(void *arg);
void DumpConsoleBuffer(void);

int ConsoleBufGetChar(void);
void ConsoleBufSetEOFPending(void);

#endif
