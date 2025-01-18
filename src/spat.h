#ifndef SPAT_H
#define SPAT_H

#include "stdlib.h"

uint32_t hash_murmur3(const void *key, size_t len, void *arg);

#endif