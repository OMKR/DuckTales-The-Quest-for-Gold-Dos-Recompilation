/* Log file (settings.log), log86.c */
#ifndef LOG86_H
#define LOG86_H

void log_open(void);
int log_active(void);
void log_printf(const char *fmt, ...);

#endif
