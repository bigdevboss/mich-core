#ifndef EXCEPTION_H
#define EXCEPTION_H

void exception_dispatch(unsigned int vec, unsigned int err,
                        unsigned int cr2, unsigned int eip,
                        unsigned int cs);

#endif
