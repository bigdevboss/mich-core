#ifndef SERIAL_H
#define SERIAL_H

void serial_init_port(void);
void serial_write(const char *str);
void serial_write_char(char c);
void serial_write_hex(unsigned int v);

#endif
