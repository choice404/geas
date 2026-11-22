/* hardening_module.c: a module that registers nothing, which the tables
 * gate copies to many paths so each copy loads as a module of its own */

#include <geas/geas.h>

GeasStatus geas_module_register(GeasRuntime* rt);

GeasStatus geas_module_register(GeasRuntime* rt) {
    (void)rt;
    return GEAS_OK;
}
