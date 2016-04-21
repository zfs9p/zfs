
#include <stdio.h>
#include <stdarg.h>

extern int debug;

void print_debug(const char *fmt, ...) {
	if(debug){
		va_list args;
		va_start(args, fmt);
		printf(fmt, args); 
		va_end(args);
	}
}
