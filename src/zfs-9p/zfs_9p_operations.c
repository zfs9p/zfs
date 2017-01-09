#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/debug.h>
#include <sys/vnode.h>
#include <sys/cred_impl.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_znode.h>
#include <sys/mode.h>
#include <attr/xattr.h>
#include <sys/fcntl.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>

#include <ixp.h>
#include "util.h"

vfs_t *vfs_9p;

typedef struct zfsAux zfsAux;

struct zfsAux{
	vnode_t *vp;
	char *name;
	off_t next; //offset reading directories
};

static void
mapFlags(int fileMode, int *mode, int *flags, int *excl){
	print_debug("mapFlags\n");
	*excl=NONEXCL;
	if(fileMode & P9_OWRITE) {
		*mode = VWRITE;
		*flags = FWRITE;
	} else if(fileMode & P9_ORDWR) {
		*mode = VREAD | VWRITE;
		*flags = FREAD | FWRITE;
	} else {
		*mode = VREAD;
		*flags = FREAD;
	}

	if(fileMode & P9_OAPPEND)
		*flags |= FAPPEND;
//	if(r->fid->omode & O_LARGEFILE)
//		*flags |= FOFFMAX;
	if(fileMode & P9_OTRUNC)
		*flags |= FTRUNC;
	if(fileMode & P9_OEXCL)
		*excl=EXCL;
}

static int
openDir(vnode_t *vp, int mode, int flags, cred_t cred){
	print_debug("openDir\n");
	int error = 0;

	if (error = VOP_ACCESS(vp, mode, 0, &cred, NULL)){
		return error;
	}

	vnode_t *old_vp = vp;

	/* TODO: review this comment from original code:*/
	/* XXX: not sure about flags */
	error = VOP_OPEN(&vp, flags, &cred, NULL);

	ASSERT(old_vp == vp);

	return error;
}

static int
openFile(vnode_t *vp, int mode, int flags, cred_t cred){
	print_debug("openFile\n");
	int error=0;
	if (!(flags & FOFFMAX) && (vp->v_type == VREG)) {
		vattr_t vattr;
		vattr.va_mask = AT_SIZE;
		if ((error = VOP_GETATTR(vp, &vattr, 0, &cred, NULL)))
			return error;

		if (vattr.va_size > (u_offset_t) MAXOFF32_T) {
			/*
			 * Large File API - regular open fails
			 * if FOFFMAX flag is set in file mode
			 */
			error = EOVERFLOW;
			return error;
		}
	}

	/*
	 * Check permissions.
	 */
	if (error = VOP_ACCESS(vp, mode, 0, &cred, NULL)) {
		print_debug("open fails on access\n");
		return error;
	}

	if ((flags & FNOFOLLOW) && vp->v_type == VLNK) {
		error = ELOOP;
		return error;
	}

	vnode_t *old_vp = vp;
	error = VOP_OPEN(&vp, flags, &cred, NULL);
	ASSERT(old_vp == vp);

	return error;
}

static void
dostat(IxpStat *s, char *name, struct stat *buf) {
	print_debug("dostat\n");
	s->type = 0;
	s->dev = 0;
	s->qid.type = buf->st_mode&S_IFMT;
	s->qid.path = buf->st_ino;
	s->qid.version = 0;
	s->mode = buf->st_mode & 0777;
	if (S_ISDIR(buf->st_mode)) {
		s->mode |= P9_DMDIR;
		s->qid.type |= P9_QTDIR;
	}
	s->atime = buf->st_atime;
	s->mtime = buf->st_mtime;
	s->length = buf->st_size;
	s->name =name;
	s->uid = getUserName(buf->st_uid);
	s->gid = getGroupName(buf->st_gid);
	s->muid = "";
}

static int
zStat(vnode_t *vp, struct stat *stbuf, cred_t *cred){
	print_debug("zStat\n");
	ASSERT(vp != NULL);
	ASSERT(stbuf != NULL);

	vattr_t vattr;
	vattr.va_mask = AT_STAT | AT_NBLOCKS | AT_BLKSIZE | AT_SIZE;

	int error = VOP_GETATTR(vp, &vattr, 0, cred, NULL);
	if(error)
		return error;

	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_dev = vattr.va_fsid;
	stbuf->st_ino = vattr.va_nodeid; //vattr.va_nodeid == 3 ? 1 : vattr.va_nodeid;
	stbuf->st_mode = VTTOIF(vattr.va_type) | vattr.va_mode;
	stbuf->st_nlink = vattr.va_nlink;
	stbuf->st_uid = vattr.va_uid;
	stbuf->st_gid = vattr.va_gid;
	stbuf->st_rdev = vattr.va_rdev;
	stbuf->st_size = vattr.va_size;
	stbuf->st_blksize = vattr.va_blksize;
	stbuf->st_blocks = vattr.va_nblocks;
	TIMESTRUC_TO_TIME(vattr.va_atime, &stbuf->st_atime);
	TIMESTRUC_TO_TIME(vattr.va_mtime, &stbuf->st_mtime);
	TIMESTRUC_TO_TIME(vattr.va_ctime, &stbuf->st_ctime);

	return 0;
}

void
p9_zfs_attach(Ixp9Req* r){
	print_debug("p9_zfs_attach\n");
	int error = 0;

	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}

	znode_t *znode;

	error = zfs_zget(zfsvfs, 3, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		char aux[6];
		sprintf(aux, "%d", error);
		ixp_respond(r, error == EEXIST ? "ENOENT" : aux);
		return;
	}

	if(znode==NULL){
		ixp_respond(r,"znodenil");
		return;
	}
	vnode_t *vp = ZTOV(znode);

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	/* Map flags */
	int mode, flags, excl;
	mapFlags(P9_ORDWR|P9_OEXEC, &mode, &flags, &excl);

	error = openDir(vp, mode, flags, cred);

	if(error)
		VN_RELE(vp);

	ZFS_EXIT(zfsvfs);
	
	zfsAux *z = ixp_emalloc(sizeof(zfsAux));
	z->vp=vp;
	z->name="/";
	r->fid->aux = z;
	r->fid->qid.type = P9_QTDIR;
	r->fid->qid.path = VTOZ(vp)->z_id;
	r->fid->qid.version = 0;
	r->ofcall.rattach.qid = r->fid->qid;
	ixp_respond(r, NULL);
}

void
p9_zfs_create(Ixp9Req* r){
	printf("p9_zfs_create\n");
	int error = 0;

	if(r->ifcall.tcreate.name && strlen(r->ifcall.tcreate.name) >= MAXNAMELEN){
		ixp_respond(r, "zfs_nameTooLong");
		return;
	}

	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	/* Map flags */
	int mode, flags, excl;
	mapFlags(r->ifcall.tcreate.mode, &mode, &flags, &excl);

	znode_t *znode;

	error = zfs_zget(zfsvfs, r->fid->qid.path, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		char aux[6];
		sprintf(aux, "%d", error);
		ixp_respond(r, error == EEXIST ? "ENOENT" : aux);
		return;
	}

	if(znode==NULL){
		ixp_respond(r,"znodenil");
		return;
	}
	vnode_t *vp = ZTOV(znode);
	vnode_t *new_vp;

	vattr_t vattr = { 0 };
	if(r->ifcall.tcreate.perm & P9_DMDIR){
		vattr.va_type = VDIR;
		vattr.va_mode = PERMMASK;
		vattr.va_mask = AT_TYPE | AT_MODE;
		error = VOP_MKDIR(vp, r->ifcall.tcreate.name, &vattr, &new_vp, &cred, NULL, 0, NULL);
	} else {
		vattr.va_type = VREG;
		//TODO: review the va_mode value
		vattr.va_mode = 0;
		vattr.va_mask = AT_TYPE|AT_MODE;
		if (flags & FTRUNC) {
			vattr.va_size = 0;
			vattr.va_mask |= AT_SIZE;
		}

		/* FIXME: check filesystem boundaries */
		error = VOP_CREATE(vp, r->ifcall.tcreate.name, &vattr, excl, mode, &new_vp, &cred, 0, NULL, NULL);

		if(!error){
			error=openFile(new_vp, mode, flags, cred);
		}
	}

	if(error) {
		ASSERT(vp->v_count > 0);
		VN_RELE(vp);
	}

	zfsAux *z = ixp_emalloc(sizeof(zfsAux));
	z->vp = new_vp;
	z->name = (char *) ixp_emalloc(strlen(r->ifcall.tcreate.name) * sizeof(char));
	memcpy(z->name, r->ifcall.tcreate.name, strlen(r->ifcall.tcreate.name) + 1);

	r->fid->aux = z;
	r->fid->qid.path=VTOZ(new_vp)->z_id;
	r->fid->qid.version=0;
	if(r->ifcall.tcreate.perm & P9_DMDIR)
		r->fid->qid.type=P9_QTDIR;
	else
		r->fid->qid.type=(excl==EXCL)?P9_QTEXCL:P9_QTFILE;
	r->fid->omode=r->ifcall.tcreate.mode;
	r->ofcall.rcreate.qid = r->fid->qid;

	ZFS_EXIT(zfsvfs);

	ixp_respond(r, NULL);
}

void
p9_zfs_clunk(Ixp9Req *r){
	print_debug("p9_zfs_clunk\n");
	int error = 0;
	printf("clunck ID <%u>\n", r->fid->qid.path);
	zfsAux *z = r->fid->aux;
	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}
	vnode_t *vp = z->vp;
	if(vp == NULL){
		ixp_respond(r, "filenotopen");
		return;
	}
	if(VTOZ(vp)->z_id != r->fid->qid.path){
		ixp_respond(r, "badinode");
	}
	/* Map flags */
	int mode, flags, excl;
	mapFlags(r->fid->omode, &mode, &flags, &excl);

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	error = VOP_CLOSE(vp, flags, 1, (offset_t) 0, &cred, NULL);

	if(error){
		ixp_respond(r, "clunkFailed");
	}else{
		//z->vp=NULL;
		VN_RELE(vp);
		ixp_respond(r, NULL);
	}

	ZFS_EXIT(zfsvfs);	
}

void
p9_zfs_write(Ixp9Req *r) {
	print_debug("p9_zfs_write\n");
	int error = 0;
	zfsAux *z = r->fid->aux;

	vnode_t *vp = z->vp;
	if(vp == NULL){
		ixp_respond(r, "filenotopen");
		return;
	}
	if(VTOZ(vp)->z_id != r->fid->qid.path){
		ixp_respond(r, "badinode");
	}

	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}

	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;

	iovec.iov_base = r->ifcall.twrite.data;
	iovec.iov_len = r->ifcall.twrite.count;
	uio.uio_resid = iovec.iov_len;
	uio.uio_loffset = r->ifcall.twrite.offset;

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	/* Map flags */
	int mode, flags, excl;
	mapFlags(r->fid->omode, &mode, &flags, &excl);

	error = VOP_WRITE(vp, &uio, flags, &cred, NULL);
	r->ofcall.rwrite.count = r->ifcall.twrite.count - uio.uio_resid;

	if(error){
		ixp_respond(r, "writtingError");
	} else {
		ixp_respond(r, NULL);
	}

	ZFS_EXIT(zfsvfs);
}

void
p9_zfs_readdir(Ixp9Req *r){
	print_debug("p9_zfs_readdir\n");
	int error = 0;
	
	zfsAux *z=r->fid->aux;
	vnode_t *vp = z->vp;
	if(vp == NULL){
		ixp_respond(r, "dirnotopen");
		return;
	}
	if(VTOZ(vp)->z_id != r->fid->qid.path){
		ixp_respond(r, "badinode");
	}

	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	union {
		char buf[DIRENT64_RECLEN(MAXNAMELEN)];
		struct dirent64 dirent;
	} entry;

	struct stat fstat = { 0 };

	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;

	int eofp = 0;
	int outbuf_off = 0;
	int outbuf_resid = r->ifcall.tread.count;
	off_t next;
	if(r->ifcall.tread.offset == 0){
		z->next = next = 0;
	} else {
		next = z->next;
	}

	znode_t *znode;
	char * errstr=NULL;

	int n, o=0;
	IxpStat s;
	IxpMsg m;
	char *mbuf = ixp_emallocz(r->ifcall.tread.count);
	m = ixp_message(mbuf, r->ifcall.tread.count, MsgPack);

	for(;;) {
		iovec.iov_base = entry.buf;
		iovec.iov_len = sizeof(entry.buf);
		uio.uio_resid = iovec.iov_len;
		uio.uio_loffset = next;

		error = VOP_READDIR(vp, &uio, &cred, &eofp, NULL, 0);
		if(error){
			errstr="errorReadDir";
			break;
		}
		/* No more directory entries */
		if(iovec.iov_base == entry.buf)
			break;

		error = zfs_zget(zfsvfs, entry.dirent.d_ino, &znode, B_TRUE);
		if(error){
			errstr="errorZGet";
			break;
		}

		ASSERT(znode != NULL);
		vnode_t *vp = ZTOV(znode);
		ASSERT(vp != NULL);

		error = zStat(vp, &fstat, &cred);
		VN_RELE(vp);

		if(error){
			errstr="errorZStat";
			break;
		}

		dostat(&s, entry.dirent.d_name, &fstat);
		n = ixp_sizeof_stat(&s);
		//o+=n;
		if(o + n > r->ifcall.tread.count)
			break;

		ixp_pstat(&m, &s);
		o+=n;

		next = entry.dirent.d_off;
	}

	if(error){
		ixp_respond(r, errstr);
	} else {
		z->next = next;
		r->ofcall.rread.count = o;
		r->ofcall.rread.data = (char*)m.data;
		ixp_respond(r, NULL);
	}

	ZFS_EXIT(zfsvfs);
}

void
p9_zfs_read(Ixp9Req *r) {
	printf("p9_zfs_read ID <%u>\n", r->fid->qid.path);
	if(r->fid->qid.type & P9_QTDIR){
		p9_zfs_readdir(r);
		return;
	}

	int error = 0;
	
	zfsAux *z = r->fid->aux;
	vnode_t *vp = z->vp;
	if(vp == NULL){
		ixp_respond(r, "filenotopen");
		return;
	}
	if(VTOZ(vp)->z_id != r->fid->qid.path){
		ixp_respond(r, "badinode");
	}

	char *outbuf = ixp_emallocz(r->ifcall.tread.count);
	if(outbuf == NULL){
		ixp_respond(r, "nomemory");
		return;
	}

	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}

	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;

	iovec.iov_base = outbuf;
	iovec.iov_len = r->ifcall.tread.count;
	uio.uio_resid = iovec.iov_len;
	uio.uio_loffset = r->ifcall.tread.offset;

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	/* Map flags */
	int mode, flags, excl;
	mapFlags(r->fid->omode, &mode, &flags, &excl);

	error = VOP_READ(vp, &uio, flags, &cred, NULL);
	r->ofcall.rread.data=outbuf;
	r->ofcall.rread.count=uio.uio_loffset - r->ifcall.tread.offset;

	if(error){
		ixp_respond(r,"readingError");
	} else {
		ixp_respond(r, NULL);
	}

	ZFS_EXIT(zfsvfs);
}

void
p9_zfs_stat(Ixp9Req *r){
	print_debug("p9_zfs_stat\n");
	printf("stat ID <%u>\n", r->fid->qid.path);
	int error = 0;
	zfsAux *z = r->fid->aux;
	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}

	znode_t *znode;

	error = zfs_zget(zfsvfs, r->fid->qid.path, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		char aux[6];
		sprintf(aux, "%d", error);
		ixp_respond(r, error == EEXIST ? "ENOENT" : aux);
		return;
	}

	if(znode==NULL){
		ixp_respond(r,"znodenil");
		return;
	}
	vnode_t *vp = ZTOV(znode);

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	struct stat stbuf;
	error = zStat(vp, &stbuf, &cred);

	VN_RELE(vp);
	ZFS_EXIT(zfsvfs);

	if(error){
		ixp_respond(r, "zStatError");
	} else {
		IxpStat s;
		
		char *name = z->name;
		
		dostat(&s, name, &stbuf);
		r->fid->qid = s.qid;
		r->ofcall.rstat.nstat = ixp_sizeof_stat(&s);

		char *buf = ixp_emallocz(r->ofcall.rstat.nstat);

		IxpMsg m = ixp_message(buf, r->ofcall.rstat.nstat, MsgPack);
		ixp_pstat(&m, &s);

		r->ofcall.rstat.stat = m.data;
		ixp_respond(r, NULL);
	}
}

void
p9_zfs_open(Ixp9Req *r){
	printf("p9_zfs_open ID <%u>\n", r->fid->qid.path);
	int error = 0;

	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}

	znode_t *znode;

	error = zfs_zget(zfsvfs, r->fid->qid.path, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		char aux[6];
		sprintf(aux, "%d", error);
		ixp_respond(r, error == EEXIST ? "ENOENT" : aux);
		return;
	}

	if(znode==NULL){
		ixp_respond(r,"znodenil");
		return;
	}
	vnode_t *vp = ZTOV(znode);

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	/* Map flags */
	int mode, flags, excl;
	mapFlags(r->ifcall.tcreate.mode, &mode, &flags, &excl);

	if(r->fid->qid.type & P9_QTDIR){
		error = openDir(vp, mode, flags, cred); 
	} else {
		error = openFile(vp, mode, flags, cred); 
	}

	if(error)
		VN_RELE(vp);

	ZFS_EXIT(zfsvfs);
	
	zfsAux *z = r->fid->aux;
	z->vp = vp;
	r->fid->qid.path=VTOZ(vp)->z_id;
	r->fid->qid.version=0;
	/*if(r->ifcall.topen.perm & P9_DMDIR)
		r->fid->qid.type=P9_QTDIR;
	else
		r->fid->qid.type=(excl==EXCL)?P9_QTEXCL:P9_QTFILE;*/
	r->fid->omode=r->ifcall.topen.mode;
	r->ofcall.ropen.qid = r->fid->qid;
	ixp_respond(r, NULL);
}


znode_t *
getZNode(uint64_t inode){
	int error = 0;
	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	znode_t * aux;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		return NULL;
	}
	error = zfs_zget(zfsvfs, inode, &aux, B_FALSE);
	if((error) || (aux==NULL)) {
		ZFS_EXIT(zfsvfs);
		return NULL;
	}

	ZFS_EXIT(zfsvfs);
	return aux;
}

int getInode(uint64_t parentID, char * name){
	int error = 0;
	uint64_t ino;
	
	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		return 0;
	}
	znode_t *znode;

	error = zfs_zget(zfsvfs, parentID, &znode, B_FALSE);
	if((error) || (znode==NULL)) {
		ZFS_EXIT(zfsvfs);
		return 0;
	}
	
	vnode_t *vp = ZTOV(znode);
	
	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	int mode, flags, excl;
	mode = VREAD;
	flags = FREAD;
	excl=NONEXCL;
	
	if(openDir(vp, mode, flags, cred)){
		return 0;
	}
	
	union {
		char buf[DIRENT64_RECLEN(MAXNAMELEN)];
		struct dirent64 dirent;
	} entry;
	
	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;
	
	int eofp = 0;
	int outbuf_off = 0;
	int outbuf_resid = 10240;
	off_t next = 0;
	
	for(;;) {
		iovec.iov_base = entry.buf;
		iovec.iov_len = sizeof(entry.buf);
		uio.uio_resid = iovec.iov_len;
		uio.uio_loffset = next;
		
		if(VOP_READDIR(vp, &uio, &cred, &eofp, NULL, 0)){
			ino = 0;
			break;
		}
		
		/* No more directory entries */
		if(iovec.iov_base == entry.buf){
			ino = 1;
			break;
		}
		
		if(strcmp(entry.dirent.d_name, name) == 0 ){
			ino = entry.dirent.d_ino;
			break;
		}

		next = entry.dirent.d_off;
	}
	if(!VOP_CLOSE(vp, flags, 1, (offset_t) 0, &cred, NULL))
		VN_RELE(vp);
	
	ZFS_EXIT(zfsvfs);
	
	return ino;
}


int
getZID(znode_t *parentIno, char *name, enum vtype *nodeType){
	int error = 0;
	uint64_t ino;

	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		return 0;
	}
	//znode_t *znode;

	/*error = zfs_zget(zfsvfs, parentIno->z_id, &znode, B_FALSE);
	if((error) || (znode==NULL)) {
		ZFS_EXIT(zfsvfs);
		return 0;
	}*/

	vnode_t *vp = ZTOV(parentIno);

	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;

	int mode, flags, excl;
	mode = VREAD;
	flags = FREAD;
	excl=NONEXCL;
	
	if(openDir(vp, mode, flags, cred)){
		return 0;
	}
	
	union {
		char buf[DIRENT64_RECLEN(MAXNAMELEN)];
		struct dirent64 dirent;
	} entry;
	
	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;
	
	int eofp = 0;
	int outbuf_off = 0;
	int outbuf_resid = 10240;
	off_t next = 0;
	
	for(;;) {
		iovec.iov_base = entry.buf;
		iovec.iov_len = sizeof(entry.buf);
		uio.uio_resid = iovec.iov_len;
		uio.uio_loffset = next;
		
		if(VOP_READDIR(vp, &uio, &cred, &eofp, NULL, 0)){
			return 0;
		}
		
		/* No more directory entries */
		if(iovec.iov_base == entry.buf){
			return 1;
		}
		
		if(strcmp(entry.dirent.d_name, name) == 0 ){
			ino = entry.dirent.d_ino;
			break;
		}

		next = entry.dirent.d_off;
	}

	if(!VOP_CLOSE(vp, flags, 1, (offset_t) 0, &cred, NULL))
		VN_RELE(vp);

	znode_t *va;
	error = zfs_zget(zfsvfs, ino, &va, B_FALSE);
	if((error) || (va==NULL)) {
		ZFS_EXIT(zfsvfs);
		return 0;
	}
	
	*nodeType=ZTOV(va)->v_type;
	ZFS_EXIT(zfsvfs);
	
	return ino;
}

void
p9_zfs_walk(Ixp9Req *r){
	printf("p9_zfs_walk\n");
	printf("walk ID <%u>\n", r->fid->qid.path);
	int i;
	uint64_t id;
	zfsAux *z = r->fid->aux;
	vnode_t *vp = z->vp;
	znode_t *zn;
	id = r->fid->qid.path;

	for(i=0; i < r->ifcall.twalk.nwname; i++) {
		id = getInode(id, r->ifcall.twalk.wname[i]);
		switch(id){
			case 0:
				ixp_respond(r, "ErrorWalk");
				return;
			case 1:
				//Not exist
				ixp_respond(r, "file not found");
				return;
			default:
				zn = getZNode(id);
				if (ZTOV(zn)->v_type == VDIR){
					r->ofcall.rwalk.wqid[i].type = P9_QTDIR;
				}
				r->ofcall.rwalk.wqid[i].path = id;
				break;
		}
	}
	zfsAux *znew = ixp_emalloc(sizeof(zfsAux));
	if(i == 0){
		memcpy(znew, z, sizeof(zfsAux));
	} else {
		znew->vp = ZTOV(zn);
		znew->name = ixp_estrdup(r->ifcall.twalk.wname[i - 1]);
	}
	r->newfid->aux = znew;
	r->ofcall.rwalk.nwqid = i;

	ixp_respond(r, NULL);
}

static void
p9_zfs_freefid(IxpFid* fid){
	printf("p9_zfs_freefid\n");
	printf("fid ID <%u>\n", fid->qid.path);
	zfsAux *z = fid->aux;
	fid->aux = NULL;
	free(z);
	printf("Fin freefid\n");
}

static void
p9_zfs_remove(Ixp9Req *r){
	printf("p9_zfs_remove ID <%u>\n", r->fid->qid.path);
	int error;
	zfsAux *z = r->fid->aux;
	vnode_t *vp = z->vp;
	
	zfsvfs_t *zfsvfs = vfs_9p->vfs_data;
	ZFS_ENTER_9P(zfsvfs, error);
	if(error){
		ixp_respond(r, "ZFS_ENTER_9PError");
		return;
	}
	
	znode_t *znode;

	error = zfs_zget(zfsvfs, r->fid->qid.path, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		char aux[6];
		sprintf(aux, "%d", error);
		ixp_respond(r, error == EEXIST ? "ENOENT" : aux);
		return;
	}
	
	if(znode==NULL){
		ixp_respond(r,"znodenil");
		return;
	}
	
	vnode_t *dvp = ZTOV(znode);
	
	//Set credentials
	cred_t cred;
	cred.cr_uid=0;
	cred.cr_gid=0;
	
	IxpQid qid = r->fid->qid;
	char *msg=NULL;
	
	if (qid.type & P9_QTDIR){
		error = VOP_RMDIR(dvp, z->name, NULL, &cred, NULL, 0);
		if(error == EEXIST){
			error = ENOTEMPTY;
			msg = "Dir not empty";
		} else if (error) {
			msg = "Error removing directory";
		}
	} else {
		error = VOP_REMOVE(dvp, z->name, &cred, NULL, 0);
		msg = "Error removing file";
	}
	VN_RELE(dvp);
	ZFS_EXIT(zfsvfs);
	if (!error){
		free(z);
		r->fid->aux=NULL;
	}
	
	ixp_respond(r, msg);
	

	return;

}


Ixp9Srv p9srv = {
	.open=p9_zfs_open,
	.walk=p9_zfs_walk,
	.read=p9_zfs_read,
	.stat=p9_zfs_stat,
	.write=p9_zfs_write,
	.clunk=p9_zfs_clunk,
//	.flush=
	.attach=p9_zfs_attach,
	.create=p9_zfs_create,
	.remove=p9_zfs_remove,
	.freefid=p9_zfs_freefid
};
