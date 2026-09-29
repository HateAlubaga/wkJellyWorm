#ifndef WKJELLYWORM_MEMACCESS_H
#define WKJELLYWORM_MEMACCESS_H

#include <cstddef>

bool jellyIsMemReadable(unsigned long addr, size_t size);
bool jellyIsMemWritable(unsigned long addr, size_t size);
bool jellyProbeRead(unsigned long addr, size_t size);

#endif //WKJELLYWORM_MEMACCESS_H
