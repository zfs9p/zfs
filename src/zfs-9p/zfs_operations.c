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


//TODO: this must be inicialized at some point
vfs_t *vfs;

static void
mapFlags(int fileMode, int *mode, int *flags, int *excl){
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
	int error=0;

	if (error = VOP_ACCESS(vp, VREAD | VEXEC, 0, &cred, NULL)){
		return error;
	}

	vnode_t *old_vp = vp;

	/* TODO: review this comment from original code:*/
	/* XXX: not sure about flags */
	error = VOP_OPEN(&vp, FREAD, &cred, NULL);

	ASSERT(old_vp == vp);

	return error;
}

static int
openFile(vnode_t *vp, int mode, int flags, cred_t cred){
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
	ASSERT(vp != NULL);
	ASSERT(stbuf != NULL);

	vattr_t vattr;
	vattr.va_mask = AT_STAT | AT_NBLOCKS | AT_BLKSIZE | AT_SIZE;

	int error = VOP_GETATTR(vp, &vattr, 0, cred, NULL);
	if(error)
		return error;

	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_dev = vattr.va_fsid;
	stbuf->st_ino = vattr.va_nodeid == 3 ? 1 : vattr.va_nodeid;
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
zfs_attach(Ixp9Req* r){
	int error;

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
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

	r->fid->aux = vp;
	r->fid->qid.type = P9_QTDIR;
	r->fid->qid.path = VTOZ(vp)->z_id;
	r->fid->qid.version = 0;
	r->ofcall.rattach.qid = r->fid->qid;
	ixp_respond(r, NULL);
}

void
zfs_create(Ixp9Req* r){
	int error;

	if(r->ifcall.tcreate.name && strlen(r->ifcall.tcreate.name) >= MAXNAMELEN){
		ixp_respond(r, "zfs_nameTooLong");
		return;
	}

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
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

	r->fid->aux = new_vp;
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
zfs_clunk(Ixp9Req *r){
	int error;

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
		return;
	}

	vnode_t *vp = r->fid->aux;
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
		VN_RELE(vp);
		free(r->fid->aux);
		ixp_respond(r, NULL);
	}

	ZFS_EXIT(zfsvfs);	
}

void
zfs_write(Ixp9Req *r) {
	int error;

	vnode_t *vp = r->fid->aux;
	if(vp == NULL){
		ixp_respond(r, "filenotopen");
		return;
	}
	if(VTOZ(vp)->z_id != r->fid->qid.path){
		ixp_respond(r, "badinode");
	}

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
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
zfs_readdir(Ixp9Req *r){
	int error;

	vnode_t *vp = r->fid->aux;
	if(vp == NULL){
		ixp_respond(r, "dirnotopen");
		return;
	}
	if(VTOZ(vp)->z_id != r->fid->qid.path){
		ixp_respond(r, "badinode");
	}

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
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
	off_t next = r->ifcall.tread.offset;

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
		o+=n;
		if(o > r->ifcall.tread.count)
			break;

		ixp_pstat(&m, &s);
		o+=n;

		next = entry.dirent.d_off;
	}

	if(error){
		ixp_respond(r, errstr);
	} else {
		r->ofcall.rread.count = o;
		r->ofcall.rread.data = (char*)m.data;
		ixp_respond(r, NULL);
	}

	ZFS_EXIT(zfsvfs);
}

void
zfs_read(Ixp9Req *r) {
	if(r->fid->qid.type & P9_QTDIR){
		zfs_readdir(r);
		return;
	}

	int error;

	vnode_t *vp = r->fid->aux;
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

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
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
zfs_stat(Ixp9Req *r){
	int error;

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
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
		
		//TODO: Get name is not trivial, need more work.
		char *name = "TODO!!";
		
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
zfs_open(Ixp9Req *r){
	int error;

	zfsvfs_t *zfsvfs = vfs->vfs_data;
	ZFS_ENTER(zfsvfs, error);
	if(error){
		ixp_respond(r, "zfs_enterError");
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

	r->fid->aux = vp;
	r->fid->qid.path=VTOZ(vp)->z_id;
	r->fid->qid.version=0;
	if(r->ifcall.topen.perm & P9_DMDIR)
		r->fid->qid.type=P9_QTDIR;
	else
		r->fid->qid.type=(excl==EXCL)?P9_QTEXCL:P9_QTFILE;
	r->fid->omode=r->ifcall.topen.mode;
	r->ofcall.ropen.qid = r->fid->qid;
	ixp_respond(r, NULL);
}

Ixp9Srv p9srv = {
	.open=zfs_open,
//	.walk=
	.read=zfs_read,
	.stat=zfs_stat,
	.write=zfs_write,
	.clunk=zfs_clunk,
//	.flush=
	.attach=zfs_attach,
	.create=zfs_create
//	.remove=
//	.freefid=
};
