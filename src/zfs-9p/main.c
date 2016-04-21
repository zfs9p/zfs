#include <stdio.h>
#include <ixp.h>


extern Ixp9Srv p9srv;

int
main(){
	IxpConn *conn;
	IxpServer server;
	char *address = "tcp!localhost!1441";
	int fd;
	
	fd = ixp_announce(address);
	if(fd < 0){
		printf("%s\n", ixp_errbuf());
		return 1;
	}

	conn = ixp_listen(&server, fd, &p9srv, ixp_serve9conn, NULL);

	printf("%s\n", ixp_errbuf());

	ixp_serverloop(&server);
	
	return 0;
}
