#pragma once

#include <libfdt.h>

/* Helper function to simplify device tree creation */
#define _FDT(exp)                                                         \
    do {                                                                  \
        int __ret = (exp);                                                \
        if (ret < 0)                                                      \
            return throw_err("Failed to create device tree:\n %s\n %s\n", \
                             #exp, fdt_strerror(ret));                    \
    } while (0)
