#ifndef CONSOLE_BUF_H
#define CONSOLE_BUF_H

#include "kt.h"

// write semaphores
extern kt_sem writeok;
extern kt_sem writers;

// read semaphores
extern kt_sem consoleWait;
extern kt_sem nelem;
extern kt_sem nslots;

void *ConsoleReader(void *arg);
void DumpConsoleBuffer(void);

int ConsoleBufGetChar(void);
void ConsoleBufSetEOFPending(void);

#endif
