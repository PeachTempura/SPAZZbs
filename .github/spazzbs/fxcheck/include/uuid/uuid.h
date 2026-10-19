#pragma once
/* stand-in so the harness builds without the uuid-dev package; the
 * functions are never called (see stubs.c) */
typedef unsigned char uuid_t[16];
void uuid_generate(uuid_t out);
void uuid_unparse_lower(const uuid_t uu, char *out);
