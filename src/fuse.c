

/*
   FUSE: Filesystem in Userspace
   Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

   This program can be distributed under the terms of the GNU GPL.
   See the file COPYING.

   gcc -Wall hello.c `pkg-config fuse --cflags --libs` -o hello
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#ifndef NDEBUG
#include <execinfo.h>
#include <assert.h>
#endif

#include "proto/ClientNamenodeProtocol.pb-c.h"
#include "proto/datatransfer.pb-c.h"
#include "hadooprpc.h"

#ifndef NDEBUG
static void dump_trace() {
  void * buffer[255];
  const int calls = backtrace(buffer,
                              sizeof(buffer) / sizeof(void *));
  backtrace_symbols_fd(buffer, calls, 1);
  exit(EXIT_FAILURE);
}

#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define CALL_NN(method, request, response) \
  hadoop_rpc_call_namenode( \
    (struct connection_state *) fuse_get_context()->private_data, \
    (const ProtobufCServiceDescriptor *) &hadoop__hdfs__client_namenode_protocol__descriptor, \
    method, \
    (const ProtobufCMessage *) &request, \
    (ProtobufCMessage **) &response);

static
int hadoop_fuse_truncate(const char * path, off_t offset);

static
void unpack_filestatus(Hadoop__Hdfs__HdfsFileStatusProto * fs, struct stat *stbuf)
{
  struct passwd * owner;
  struct group * group;

  assert(fs);
  assert(stbuf);

  stbuf->st_size = fs->length;
  switch(fs->filetype)
  {
  case HADOOP__HDFS__HDFS_FILE_STATUS_PROTO__FILE_TYPE__IS_DIR:
  {
    stbuf->st_mode = S_IFDIR;
    break;
  }
  case HADOOP__HDFS__HDFS_FILE_STATUS_PROTO__FILE_TYPE__IS_FILE:
  {
    stbuf->st_mode = S_IFREG;
    break;
  }
  case HADOOP__HDFS__HDFS_FILE_STATUS_PROTO__FILE_TYPE__IS_SYMLINK:
  {
    stbuf->st_mode = S_IFLNK;
    break;
  }
  }
  if(fs->permission)
  {
    stbuf->st_mode |= fs->permission->perm;
  }
  if(fs->has_blocksize)
  {
    stbuf->st_blksize = fs->blocksize;
  }
  stbuf->st_mtime = fs->modification_time;
  stbuf->st_atime = fs->access_time;

  owner = getpwnam(fs->owner);
  if(owner)
  {
    stbuf->st_uid = owner->pw_uid;
  }
  group = getgrnam(fs->group);
  if(group)
  {
    stbuf->st_gid = group->gr_gid;
  }
}

static
int hadoop_fuse_getattr(const char *path, struct stat *stbuf)
{
  int res;
  Hadoop__Hdfs__GetFileInfoRequestProto request = HADOOP__HDFS__GET_FILE_INFO_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetFileInfoResponseProto *response = NULL;

  memset(stbuf, 0, sizeof(struct stat));

  request.src = (char *) path;
  res = CALL_NN("getFileInfo", request, response);
  if(res < 0)
  {
    return res;
  }
  else if(!response->fs)
  {
    hadoop__hdfs__get_file_info_response_proto__free_unpacked(response, NULL);
    return -ENOENT;
  }
  else
  {
    unpack_filestatus(response->fs, stbuf);
    hadoop__hdfs__get_file_info_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_readlink(const char * path, char * buf, size_t len)
{
  int res;
  Hadoop__Hdfs__GetLinkTargetRequestProto request = HADOOP__HDFS__GET_LINK_TARGET_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetLinkTargetResponseProto *response = NULL;

  request.path = (char *) path;
  res = CALL_NN("getLinkTarget", request, response);
  if(res < 0)
  {
    return res;
  }
  else if (!response->targetpath)
  {
    hadoop__hdfs__get_link_target_response_proto__free_unpacked(response, NULL);
    return -ENOENT;
  }
  else
  {
    strncpy(buf, response->targetpath, len);
    buf[len - 1] = '\0';
    hadoop__hdfs__get_link_target_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_utimens(const char * src, const struct timespec tv[2])
{
  int res;
  Hadoop__Hdfs__SetTimesRequestProto request = HADOOP__HDFS__SET_TIMES_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__SetTimesResponseProto *response = NULL;

  request.src = (char *) src;
  request.atime = tv[0].tv_sec * 1000 + tv[0].tv_nsec / 1000000;
  request.mtime = tv[1].tv_sec * 1000 + tv[1].tv_nsec / 1000000;
  res = CALL_NN("setTimes", request, response);
  if(res < 0)
  {
    return res;
  }
  else
  {
    hadoop__hdfs__set_times_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_chown(const char * src, uid_t uid, gid_t gid)
{
  int res;
  struct passwd * owner;
  struct group * group;
  Hadoop__Hdfs__SetOwnerRequestProto request = HADOOP__HDFS__SET_OWNER_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__SetOwnerResponseProto *response = NULL;

  request.src = (char *) src;
  owner = getpwuid(uid);
  if(owner)
  {
    request.username = owner->pw_name;
  }
  group = getgrgid(gid);
  if(group)
  {
    request.groupname = group->gr_name;
  }

  res = CALL_NN("setOwner", request, response);
  if(res < 0)
  {
    return res;
  }
  else
  {
    hadoop__hdfs__set_owner_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_symlink(const char * target, const char * link)
{
  int res;
  Hadoop__Hdfs__CreateSymlinkRequestProto request = HADOOP__HDFS__CREATE_SYMLINK_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__FsPermissionProto permission = HADOOP__HDFS__FS_PERMISSION_PROTO__INIT;
  Hadoop__Hdfs__CreateSymlinkResponseProto *response = NULL;

  request.link = (char *) link;
  request.target = (char *) target;
  request.dirperm = &permission;
  request.createparent = false;
  permission.perm = 0777; // don't actually matter as we never create parents
  res = CALL_NN("createSymlink", request, response);
  if(res < 0)
  {
    return res;
  }
  else
  {
    hadoop__hdfs__create_symlink_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_rename(const char * src, const char * dst)
{
  int res;
  Hadoop__Hdfs__RenameRequestProto request = HADOOP__HDFS__RENAME_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__RenameResponseProto *response = NULL;

  request.src = (char *) src;
  request.dst = (char *) dst;
  res = CALL_NN("rename", request, response);
  if(res < 0)
  {
    return res;
  }
  else if(!response->result)
  {
    hadoop__hdfs__rename_response_proto__free_unpacked(response, NULL);
    return -EINVAL;
  }
  else
  {
    hadoop__hdfs__rename_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_chmod(const char * src, mode_t perm)
{
  int res;
  Hadoop__Hdfs__SetPermissionRequestProto request = HADOOP__HDFS__SET_PERMISSION_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__FsPermissionProto permission = HADOOP__HDFS__FS_PERMISSION_PROTO__INIT;
  Hadoop__Hdfs__SetPermissionResponseProto *response = NULL;

  request.src = (char *) src;
  request.permission = &permission;
  permission.perm = perm;
  res = CALL_NN("setPermission", request, response);
  if(res < 0)
  {
    return res;
  }
  else
  {
    hadoop__hdfs__set_permission_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_statfs(const char * src, struct statvfs * stat)
{
  int res;
  uint64_t block_size;
  (void) src; // HDFS only supports getPreferredBlockSize for files
  Hadoop__Hdfs__GetFsStatusRequestProto fs_request = HADOOP__HDFS__GET_FS_STATUS_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetServerDefaultsRequestProto block_request = HADOOP__HDFS__GET_SERVER_DEFAULTS_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetFsStatsResponseProto * fs_response = NULL;
  Hadoop__Hdfs__GetServerDefaultsResponseProto * block_response = NULL;

  res = CALL_NN("getFsStats", fs_request, fs_response);
  if(res < 0)
  {
    return res;
  }
  res = CALL_NN("getServerDefaults", block_request, block_response);
  if(res < 0)
  {
    return res;
  }

  stat->f_bsize = stat->f_frsize = block_size = block_response->serverdefaults->blocksize;
  stat->f_blocks = fs_response->capacity / block_size;
  stat->f_bfree = stat->f_bavail = fs_response->remaining / block_size;
  stat->f_namemax = 0xFFFFFFFF; // java String max length

  hadoop__hdfs__get_fs_stats_response_proto__free_unpacked(fs_response, NULL);
  hadoop__hdfs__get_server_defaults_response_proto__free_unpacked(block_response, NULL);
  return 0;
}

static
int hadoop_fuse_delete(const char * src)
{
  int res;
  Hadoop__Hdfs__DeleteRequestProto request = HADOOP__HDFS__DELETE_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__DeleteResponseProto *response = NULL;

  request.src = (char *) src;
  request.recursive = false;
  res = CALL_NN("delete", request, response);
  if(res < 0)
  {
    return res;
  }
  else if(!response->result)
  {
    hadoop__hdfs__delete_response_proto__free_unpacked(response, NULL);
    return -EINVAL;
  }
  else
  {
    hadoop__hdfs__delete_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_mkdir(const char * src, mode_t perm)
{
  int res;
  Hadoop__Hdfs__MkdirsRequestProto request = HADOOP__HDFS__MKDIRS_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__FsPermissionProto permission = HADOOP__HDFS__FS_PERMISSION_PROTO__INIT;
  Hadoop__Hdfs__MkdirsResponseProto *response = NULL;

  request.src = (char *) src;
  request.masked = &permission;
  request.createparent = false;
  permission.perm = perm;
  res = CALL_NN("mkdirs", request, response);
  if(res < 0)
  {
    return res;
  }
  else if(!response->result)
  {
    hadoop__hdfs__mkdirs_response_proto__free_unpacked(response, NULL);
    return -EINVAL;
  }
  else
  {
    hadoop__hdfs__mkdirs_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
  (void) offset;
  (void) fi;
  int res;
  Hadoop__Hdfs__GetListingRequestProto request = HADOOP__HDFS__GET_LISTING_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetListingResponseProto *response = NULL;

  request.src = (char *) path;
  request.startafter.len = 0;
  request.startafter.data = NULL;
  request.needlocation = false;

  res = CALL_NN("getListing", request, response);
  if(res < 0)
  {
    return res;
  }

  for(int i = 0, max = response->dirlist->n_partiallisting; i < max; ++i)
  {
    struct stat stbuf;
    Hadoop__Hdfs__HdfsFileStatusProto * file = response->dirlist->partiallisting[i];
    struct stat * forfuse;
    char * filename;

    if(file)
    {
      memset(&stbuf, 0, sizeof(struct stat));
      unpack_filestatus(file, &stbuf);
      forfuse = &stbuf;
    }
    else
    {
      forfuse = NULL;
    }

    filename = malloc(file->path.len);
    if(!filename)
    {
      return -ENOMEM;
    }
    strncpy(filename, (const char *) file->path.data, file->path.len);
    int filled = filler(buf, filename, forfuse, 0);
    if(filled != 0)
    {
      return -ENOMEM;
    }
  }

  hadoop__hdfs__get_listing_response_proto__free_unpacked(response, NULL);
  return 0;
}

static
int hadoop_fuse_fsync(const char * src, int datasync, struct fuse_file_info * fi)
{
  (void) datasync;
  (void) fi;
  int res;
  Hadoop__Hdfs__FsyncRequestProto request = HADOOP__HDFS__FSYNC_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__FsyncResponseProto *response = NULL;

  request.src = (char *) src;
  request.client = (char *) ((struct connection_state *) fuse_get_context()->private_data)->clientname;
  res = CALL_NN("fsync", request, response);
  if(res < 0)
  {
    return res;
  }
  else
  {
    hadoop__hdfs__fsync_response_proto__free_unpacked(response, NULL);
    return 0;
  }
}

static
int hadoop_fuse_do_release(const char * src)
{
  int res;
  Hadoop__Hdfs__CompleteRequestProto request = HADOOP__HDFS__COMPLETE_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__CompleteResponseProto *response = NULL;

  request.src = (char *) src;
  request.clientname = (char *) ((struct connection_state *) fuse_get_context()->private_data)->clientname;
  res = CALL_NN("complete", request, response);
  if(res < 0)
  {
    return res;
  }
  else
  {
    res = response->result ? 0 : -EBUSY;
    hadoop__hdfs__complete_response_proto__free_unpacked(response, NULL);
    return res;
  }
}

static
int hadoop_fuse_release(const char * src, struct fuse_file_info * fi)
{
  if(fi->flags & (O_WRONLY | O_RDWR))
  {
    return hadoop_fuse_do_release(src);
  }
  else
  {
    // read-only - since HDFS doesn't lock to read a file we
    // can ignore
    return 0;
  }
}

static
int hadoop_fuse_mknod(const char * src, mode_t perm, dev_t dev)
{
  (void) dev;
  int res;
  Hadoop__Hdfs__CreateRequestProto request = HADOOP__HDFS__CREATE_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__FsPermissionProto permission = HADOOP__HDFS__FS_PERMISSION_PROTO__INIT;
  Hadoop__Hdfs__CreateResponseProto *response = NULL;
  Hadoop__Hdfs__GetServerDefaultsRequestProto defaults_request = HADOOP__HDFS__GET_SERVER_DEFAULTS_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetServerDefaultsResponseProto * defaults_response = NULL;

  // just use default block size & replication factors
  res = CALL_NN("getServerDefaults", defaults_request, defaults_response);
  if(res < 0)
  {
    return res;
  }

  request.src = (char *) src;
  request.clientname = (char *) ((struct connection_state *) fuse_get_context()->private_data)->clientname;
  request.createparent = false;
  request.masked = &permission;
  permission.perm = perm;
  request.createflag = HADOOP__HDFS__CREATE_FLAG_PROTO__CREATE; // must not already exist
  request.replication = defaults_response->serverdefaults->replication;
  request.blocksize = defaults_response->serverdefaults->blocksize;

  hadoop__hdfs__get_server_defaults_response_proto__free_unpacked(defaults_response, NULL);

  res = CALL_NN("create", request, response);
  if(res < 0)
  {
    return res;
  }

  hadoop__hdfs__create_response_proto__free_unpacked(response, NULL);

  return hadoop_fuse_do_release(src); // release our lease on this file
}

static
int hadoop_fuse_open(const char * path, struct fuse_file_info * fi)
{
  int res;
  Hadoop__Hdfs__GetFileInfoRequestProto file_request = HADOOP__HDFS__GET_FILE_INFO_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetFileInfoResponseProto *file_response = NULL;
  Hadoop__Hdfs__FsPermissionProto permission = HADOOP__HDFS__FS_PERMISSION_PROTO__INIT;
  bool exists;

  file_request.src = (char *) path;
  res = CALL_NN("getFileInfo", file_request, file_response);
  if(res < 0)
  {
    return res;
  }
  exists = file_response->fs;

  if(exists)
  {
    permission.perm = file_response->fs->permission->perm;
    hadoop__hdfs__get_file_info_response_proto__free_unpacked(file_response, NULL);

    if((fi->flags & O_EXCL) == O_EXCL)
    {
      // exclusive - but the file already exists? fail:
      return -EEXIST;
    }
  }
  else
  {
    hadoop__hdfs__get_file_info_response_proto__free_unpacked(file_response, NULL);
    if(fi->flags & O_CREAT)
    {
      permission.perm = 0755; // defaults
    }
    else
    {
      // not there, and we don't want to create it
      return -ENOENT;
    }
  }

  if ((fi->flags & O_ACCMODE) == O_RDWR || (fi->flags & O_ACCMODE) == O_WRONLY)
  {
    // we need the lease on this file
    bool truncate = (fi->flags & O_TRUNC) == O_TRUNC;

    if(exists && !truncate)
    {
      // hadoop sementics means we must "append"
      Hadoop__Hdfs__AppendRequestProto request = HADOOP__HDFS__APPEND_REQUEST_PROTO__INIT;
      Hadoop__Hdfs__AppendResponseProto *response = NULL;

      request.src = (char *) path;
      request.clientname = (char *) ((struct connection_state *) fuse_get_context()->private_data)->clientname;

      res = CALL_NN("append", request, response);
      if(res < 0)
      {
        return res;
      }

      hadoop__hdfs__append_response_proto__free_unpacked(response, NULL);
    }
    else
    {
      // "create" a new file
      Hadoop__Hdfs__GetServerDefaultsRequestProto defaults_request = HADOOP__HDFS__GET_SERVER_DEFAULTS_REQUEST_PROTO__INIT;
      Hadoop__Hdfs__GetServerDefaultsResponseProto * defaults_response = NULL;
      Hadoop__Hdfs__CreateRequestProto request = HADOOP__HDFS__CREATE_REQUEST_PROTO__INIT;
      Hadoop__Hdfs__CreateResponseProto *response = NULL;

      // just use default block size & replication factors
      res = CALL_NN("getServerDefaults", defaults_request, defaults_response);
      if(res < 0)
      {
        return res;
      }

      request.src = (char *) path;
      request.clientname = (char *) ((struct connection_state *) fuse_get_context()->private_data)->clientname;
      request.createparent = false;
      request.masked = &permission;
      if(truncate)
      {
        request.createflag = HADOOP__HDFS__CREATE_FLAG_PROTO__CREATE | HADOOP__HDFS__CREATE_FLAG_PROTO__OVERWRITE;
      }
      else
      {
        request.createflag = HADOOP__HDFS__CREATE_FLAG_PROTO__CREATE | HADOOP__HDFS__CREATE_FLAG_PROTO__APPEND;
      }
      request.replication = defaults_response->serverdefaults->replication;
      request.blocksize = defaults_response->serverdefaults->blocksize;
      hadoop__hdfs__get_server_defaults_response_proto__free_unpacked(defaults_response, NULL);

      res = CALL_NN("create", request, response);
      if(res < 0)
      {
        return res;
      }

      hadoop__hdfs__create_response_proto__free_unpacked(response, NULL);
    }

    return 0;
  }
  else if ((fi->flags & O_ACCMODE) == O_RDONLY)
  {
    // HDFS doesn't need to lock to read a file
    return 0;
  }
  else
  {
    return -ENOSYS;
  }
}

static
int hadoop_fuse_truncate(const char * path, off_t offset)
{
  int res;
  Hadoop__Hdfs__GetFileInfoRequestProto file_request = HADOOP__HDFS__GET_FILE_INFO_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetFileInfoResponseProto *file_response = NULL;
  Hadoop__Hdfs__GetBlockLocationsResponseProto *location_response = NULL;
  Hadoop__Hdfs__LocatedBlocksProto * locations;
  struct connection_state * state = (struct connection_state *) fuse_get_context()->private_data;

  if(offset != 0)
  {
    // not sure how to truncate half blocks
    return -ENOSYS;
  }

  file_request.src = (char *) path;
  res = CALL_NN("getFileInfo", file_request, file_response);
  if(res < 0)
  {
    goto end;
  }
  if(!file_response->fs)
  {
    res = -ENOENT;
    goto end;
  }
  if(file_response->fs->locations)
  {
    locations = file_response->fs->locations;
  }
  else
  {
    // just have to ask for them
    Hadoop__Hdfs__GetBlockLocationsRequestProto location_request = HADOOP__HDFS__GET_BLOCK_LOCATIONS_REQUEST_PROTO__INIT;
    Hadoop__Hdfs__GetBlockLocationsResponseProto *location_response = NULL;

    location_request.src = (char *) path;
    location_request.offset = 0;
    location_request.length = file_response->fs->length;
    res = CALL_NN("getBlockLocations", location_request, location_response);
    if(res < 0)
    {
      goto end;
    }
    locations = location_response->locations;
  }

  for(uint32_t b = 0; b < locations->n_blocks; ++b)
  {
    Hadoop__Hdfs__AbandonBlockRequestProto abandon_request = HADOOP__HDFS__ABANDON_BLOCK_REQUEST_PROTO__INIT;
    Hadoop__Hdfs__AbandonBlockResponseProto * abandon_response = NULL;

    abandon_request.src = (char *) path;
    abandon_request.holder = (char *) state->clientname;
    abandon_request.b = locations->blocks[b]->b;

    res = CALL_NN("abandonBlock", abandon_request, abandon_response);
    if(res < 0)
    {
      goto end;
    }

    hadoop__hdfs__abandon_block_response_proto__free_unpacked(abandon_response, NULL);
  }

  res = 0;
end:
  if(file_response)
  {
    hadoop__hdfs__get_file_info_response_proto__free_unpacked(file_response, NULL);
  }
  if(location_response)
  {
    hadoop__hdfs__get_block_locations_response_proto__free_unpacked(location_response, NULL);
  }
  return res;
}

static
int hadoop_fuse_write(const char * src, const char * data, size_t len, off_t offset, struct fuse_file_info * fi)
{
  (void) data;
  (void) len;
  (void) offset;
  (void) fi;
  int res;
  struct connection_state * state = (struct connection_state *) fuse_get_context()->private_data;
  Hadoop__Hdfs__ClientOperationHeaderProto clientheader = HADOOP__HDFS__CLIENT_OPERATION_HEADER_PROTO__INIT;
  Hadoop__Hdfs__BaseHeaderProto baseheader = HADOOP__HDFS__BASE_HEADER_PROTO__INIT;
  Hadoop__Hdfs__OpWriteBlockProto op = HADOOP__HDFS__OP_WRITE_BLOCK_PROTO__INIT;
  Hadoop__Hdfs__BlockOpResponseProto * opresponse = NULL;
  Hadoop__Hdfs__AddBlockRequestProto block_request = HADOOP__HDFS__ADD_BLOCK_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__AddBlockResponseProto * block_response = NULL;
  Hadoop__Hdfs__LocatedBlockProto * block;
  Hadoop__Hdfs__ChecksumProto checksum = HADOOP__HDFS__CHECKSUM_PROTO__INIT;

  bool written = false;

  block_request.src = (char *) src;
  block_request.clientname = (char *) state->clientname;
  block_request.previous = NULL;
  block_request.has_fileid = false;
  block_request.n_excludenodes = 0;
  block_request.n_favorednodes = 0;
  res = CALL_NN("addBlock", block_request, block_response);
  if(res < 0)
  {
    return res;
  }
  block = block_response->block;

  // find a DN to write it to. Assume it fits in one block for now.

  checksum.type = HADOOP__HDFS__CHECKSUM_TYPE_PROTO__CHECKSUM_NULL;
  checksum.bytesperchecksum = 65536;
  clientheader.clientname = (char *) state->clientname;
  baseheader.block = block->b;
  clientheader.baseheader = &baseheader;
  op.header = &clientheader;
  op.n_targets = block->n_locs;
  op.targets = block->locs;
  op.stage = HADOOP__HDFS__OP_WRITE_BLOCK_PROTO__BLOCK_CONSTRUCTION_STAGE__PIPELINE_SETUP_CREATE;
  op.requestedchecksum = &checksum;

  // for now, don't care about which locatoin we get it from
  for (size_t l = 0; l < block->n_locs; ++l)
  {
    Hadoop__Hdfs__DatanodeInfoProto * location = block->locs[l];
    struct connection_state dn_state;

    memset(&dn_state, 0, sizeof(dn_state));
    res = hadoop_rpc_connect_datanode(&dn_state, location->id->ipaddr, location->id->xferport);
    if(res < 0)
    {
      continue;
    }

    res = hadoop_rpc_call_datanode(&dn_state, 80, (const ProtobufCMessage *) &op, &opresponse);
    if(res < 0)
    {
      hadoop_rpc_disconnect(&dn_state);
      continue;
    }
    hadoop__hdfs__block_op_response_proto__free_unpacked(opresponse, NULL);

    res = 0;
    if(res < 0)
    {
      hadoop_rpc_disconnect(&dn_state);
      continue;
    }

    written = true;
    hadoop_rpc_disconnect(&dn_state);
    break;
  }

  hadoop__hdfs__add_block_response_proto__free_unpacked(block_response, NULL);
  if(!written)
  {
    return -EIO;
  }

  return len;
}

static
int hadoop_fuse_read(const char * src, char * buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
  (void) buf;
  (void) fi;
  int res;
  Hadoop__Hdfs__GetBlockLocationsRequestProto request = HADOOP__HDFS__GET_BLOCK_LOCATIONS_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__GetBlockLocationsResponseProto *response = NULL;

  request.src = (char *) src;
  request.offset = offset;
  request.length = size;
  res = CALL_NN("getBlockLocations", request, response);
  if(res < 0)
  {
    return res;
  }
  else
  {
    Hadoop__Hdfs__LocatedBlocksProto * locations = response->locations;

    if(!locations)
    {
      res = -ENOENT;
    }
    else if(locations->underconstruction)
    {
      res = -EBUSY;
    }
    else
    {
      Hadoop__Hdfs__ClientOperationHeaderProto clientheader = HADOOP__HDFS__CLIENT_OPERATION_HEADER_PROTO__INIT;
      Hadoop__Hdfs__BaseHeaderProto baseheader = HADOOP__HDFS__BASE_HEADER_PROTO__INIT;
      Hadoop__Hdfs__OpReadBlockProto op = HADOOP__HDFS__OP_READ_BLOCK_PROTO__INIT;
      struct connection_state * state = (struct connection_state *) fuse_get_context()->private_data;

      clientheader.clientname = (char *) state->clientname;

      for (size_t b = 0; b < locations->n_blocks; ++b)
      {
        Hadoop__Hdfs__LocatedBlockProto * block = locations->blocks[b];

        if(block->corrupt)
        {
          res = -EBADMSG;
        }
        else
        {
          Hadoop__Hdfs__BlockOpResponseProto *opresponse = NULL;
          bool written = false;

          baseheader.block = block->b;
          clientheader.baseheader = &baseheader;
          op.header = &clientheader;
          op.has_sendchecksums = true;
          op.sendchecksums = false;
          op.offset = min(offset - block->offset, 0);
          op.len = min(block->b->numbytes - op.offset, size);

          // for now, don't care about which locatoin we get it from
          for (size_t l = 0; l < block->n_locs; ++l)
          {
            Hadoop__Hdfs__DatanodeInfoProto * location = block->locs[l];
            struct connection_state dn_state;

            memset(&dn_state, 0, sizeof(dn_state));
            res = hadoop_rpc_connect_datanode(&dn_state, location->id->ipaddr, location->id->xferport);
            if(res < 0)
            {
              continue;
            }

            res = hadoop_rpc_call_datanode(&dn_state, 81, (const ProtobufCMessage *) &op, &opresponse);
            if(res < 0)
            {
              hadoop_rpc_disconnect(&dn_state);
              continue;
            }
            hadoop__hdfs__block_op_response_proto__free_unpacked(opresponse, NULL);

            res = hadoop_rpc_receive_packets(
              &dn_state,
              (uint8_t *) buf);
            if(res < 0)
            {
              hadoop_rpc_disconnect(&dn_state);
              continue;
            }

            written = true;
            hadoop_rpc_disconnect(&dn_state);
            break;
          }

          if(!written)
          {
            return -EIO;
          }
        }
      }

      res = size;
    }

    hadoop__hdfs__get_block_locations_response_proto__free_unpacked(response, NULL);
    return res;
  }
}

static
void * hadoop_fuse_init(struct fuse_conn_info *conn)
{
  conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;
  conn->capable |= FUSE_CAP_ATOMIC_O_TRUNC;
  return fuse_get_context()->private_data;
}

static
struct fuse_operations hello_oper = {
  .getattr = hadoop_fuse_getattr,
  .readdir = hadoop_fuse_readdir,
  .readlink = hadoop_fuse_readlink,
  .chmod = hadoop_fuse_chmod,
  .rename = hadoop_fuse_rename,
  .unlink = hadoop_fuse_delete,
  .rmdir = hadoop_fuse_delete,
  .mkdir = hadoop_fuse_mkdir,
  .symlink = hadoop_fuse_symlink,
  .statfs = hadoop_fuse_statfs,
  .chown = hadoop_fuse_chown,
  .utimens = hadoop_fuse_utimens,
  .open = hadoop_fuse_open,
  .read = hadoop_fuse_read,
  .fsync = hadoop_fuse_fsync,
  .release = hadoop_fuse_release,
  .mknod = hadoop_fuse_mknod,
  .write = hadoop_fuse_write,
  .truncate = hadoop_fuse_truncate,
  .init = hadoop_fuse_init
};

int main(int argc, char *argv[])
{
  struct connection_state state;
  uint16_t port;
  int result;

  if (argc < 3)
  {
    fprintf(stderr, "need namenode host & namenode port as first two argumnets\n");
    return 1;
  }

#ifndef NDEBUG
  signal(SIGSEGV, dump_trace);
#endif

  port = atoi(argv[2]);
  memset(&state, 0, sizeof(state));
  state.clientname = "hadoop_fuse";
  pthread_mutex_init(&state.mutex, NULL);
  result = hadoop_rpc_connect_namenode(&state, argv[1], port);
  if(result < 0)
  {
    fprintf(stderr, "connection to hdfs://%s:%d failed: %s\n", argv[1], port, strerror(-result));
    return 1;
  }

  for(int i = 3; i < argc; ++i)
  {
    argv[i - 2] = argv[i];
  }
  return fuse_main(argc - 2, argv, &hello_oper, &state);
}

