
#include <stdio.h>
#include <stdarg.h>
#include <pwd.h>
#include <grp.h>

extern int debug;

void
print_debug(const char *fmt, ...) {
	if(debug){
		va_list args;
		va_start(args, fmt);
		printf(fmt, args); 
		va_end(args);
	}
}

char *
getUserName(uid_t uid){
	struct passwd *pw = getpwuid(uid);

	if (pw){
		return pw->pw_name;
	}

	return "";
}

char *
getGroupName(gid_t gid){
	struct group *g = getgrgid(gid);

	if (g){
		return g->gr_name;
	}

	return "";
}
