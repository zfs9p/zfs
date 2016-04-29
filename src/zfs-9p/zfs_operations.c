


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

void
zfs_attach(Ixp9Req* r){
	r->fid->qid.type = P9_QTDIR;
	r->fid->qid.path = 0;
	r->fid->qid.version = 0;
	r->ofcall.rattach.qid = r->fid->qid;
	ixp_respond(r, NULL);
}

void
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

Ixp9Srv p9srv = {
//	.open=
//	.walk=
//	.read=
//	.stat=
	.write=zfs_write,
	.clunk=zfs_clunk,
//	.flush=
	.attach=zfs_attach,
	.create=zfs_create
//	.remove=
//	.freefid=
};
