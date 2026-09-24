#ifndef PURE_STORE_PORT_H
#define PURE_STORE_PORT_H
#include "runtime.h"
#include "program.h"
/* The path remains alive for the domain's complete lifetime, including pins. */
PureHost pure_store_host(const char *path);
#endif
