#pragma once
#ifdef _WIN32
#ifndef WNOHANG
#define WNOHANG 1
#endif
inline int waitpid(int, int*, int) { return -1; }
#else
#include_next <sys/wait.h>
#endif
