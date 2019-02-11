/*
 * This file is part of the Green End SFTP Server.
 * Copyright (C) 2007, 2011, 2016 Richard Kettlewell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include "sftpserver.h"
#include "alloc.h"
#include "users.h"
#include "debug.h"
#include "sftp.h"
#include "handle.h"
#include "send.h"
#include "parse.h"
#include "types.h"
#include "globals.h"
#include "stat.h"
#include "utils.h"
#include "serialize.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/statvfs.h>

int reverse_symlink;

/* Callbacks */

/* Encode/decode path names.  v3 does not know what encoding filenames use.  I
 * assume that the client and the server use the same encoding and so don't
 * perform any translation. */
int sftp_v3_encode(struct sftpjob attribute((unused)) * job,
                   char attribute((unused)) * *path) {
  return 0;
}

static uint32_t v3_decode(struct sftpjob attribute((unused)) * job,
                          char **path) {
  /* Empty path means default directory */
  if(!**path)
    *path = (char *)".";
  return SSH_FX_OK;
}

/* Send a filename list as found in an SSH_FXP_NAME response.  The response
 * header and so on must be generated by the caller. */
static void v3_sendnames(struct sftpjob *job, int nnames,
                         const struct sftpattr *names) {
  time_t now;
  struct tm nowtime;

  /* We'd like to know what year we're in for dates in longname */
  time(&now);
  gmtime_r(&now, &nowtime);
  sftp_send_uint32(job->worker, nnames);
  while(nnames > 0) {
    sftp_send_path(job, job->worker, names->name);
    sftp_send_string(job->worker,
                     sftp_format_attr(job->a, names, nowtime.tm_year, 0));
    protocol->sendattrs(job, names);
    ++names;
    --nnames;
  }
}

static void v3_sendattrs(struct sftpjob *job, const struct sftpattr *attrs) {
  uint32_t v3bits, m, a;

  /* The timestamp flags change between v3 and v4.  In the structure we always
   * use the v4+ bits, so we must translate. */
  if((attrs->valid &
      (SSH_FILEXFER_ATTR_ACCESSTIME | SSH_FILEXFER_ATTR_MODIFYTIME)) ==
     (SSH_FILEXFER_ATTR_ACCESSTIME | SSH_FILEXFER_ATTR_MODIFYTIME))
    v3bits =
        ((attrs->valid & (SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID |
                          SSH_FILEXFER_ATTR_PERMISSIONS)) |
         SSH_FILEXFER_ACMODTIME);
  else
    v3bits =
        (attrs->valid & (SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID |
                         SSH_FILEXFER_ATTR_PERMISSIONS));
  sftp_send_uint32(job->worker, v3bits);
  if(v3bits & SSH_FILEXFER_ATTR_SIZE)
    sftp_send_uint64(job->worker, attrs->size);
  if(v3bits & SSH_FILEXFER_ATTR_UIDGID) {
    sftp_send_uint32(job->worker, attrs->uid);
    sftp_send_uint32(job->worker, attrs->gid);
  }
  if(v3bits & SSH_FILEXFER_ATTR_PERMISSIONS)
    sftp_send_uint32(job->worker, attrs->permissions);
  if(v3bits & SSH_FILEXFER_ACMODTIME) {
    m = attrs->mtime.seconds;
    a = attrs->atime.seconds;
    /* Check that the conversion was sound.  SFTP v3 becomes unsound in 2038CE.
     * If you're looking at this code then, I suggest using a later protocol
     * version.  If that's not acceptable, and you either don't care about
     * bogus timestamps or have some other workaround, then delete the
     * checks. */
    if(m != attrs->mtime.seconds)
      fatal("sending out-of-range mtime");
    if(a != attrs->atime.seconds)
      fatal("sending out-of-range mtime");
    sftp_send_uint32(job->worker, a);
    sftp_send_uint32(job->worker, m);
  }
  /* Note that we just discard unknown bits rather than reporting errors. */
}

static uint32_t v3_parseattrs(struct sftpjob *job, struct sftpattr *attrs) {
  uint32_t n, rc;

  memset(attrs, 0, sizeof *attrs);
  if((rc = sftp_parse_uint32(job, &attrs->valid)) != SSH_FX_OK)
    return rc;
  if((attrs->valid & protocol->attrmask) != attrs->valid) {
    D(("received attrs %#x but protocol %d only supports %#x", attrs->valid,
       protocol->version, protocol->attrmask));
    attrs->valid = 0;
    return SSH_FX_BAD_MESSAGE;
  }
  /* Translate v3 bits t v4+ bits */
  if(attrs->valid & SSH_FILEXFER_ACMODTIME)
    attrs->valid |=
        (SSH_FILEXFER_ATTR_ACCESSTIME | SSH_FILEXFER_ATTR_MODIFYTIME);
  /* Read the v3 fields */
  if(attrs->valid & SSH_FILEXFER_ATTR_SIZE)
    if((rc = sftp_parse_uint64(job, &attrs->size)) != SSH_FX_OK)
      return rc;
  if(attrs->valid & SSH_FILEXFER_ATTR_UIDGID) {
    if((rc = sftp_parse_uint32(job, &attrs->uid)) != SSH_FX_OK)
      return rc;
    if((rc = sftp_parse_uint32(job, &attrs->gid)) != SSH_FX_OK)
      return rc;
  }
  if(attrs->valid & SSH_FILEXFER_ATTR_PERMISSIONS) {
    if((rc = sftp_parse_uint32(job, &attrs->permissions)) != SSH_FX_OK)
      return rc;
    /* Fake up type field */
    switch(attrs->permissions & S_IFMT) {
    case S_IFIFO:
      attrs->type = SSH_FILEXFER_TYPE_FIFO;
      break;
    case S_IFCHR:
      attrs->type = SSH_FILEXFER_TYPE_CHAR_DEVICE;
      break;
    case S_IFDIR:
      attrs->type = SSH_FILEXFER_TYPE_DIRECTORY;
      break;
    case S_IFBLK:
      attrs->type = SSH_FILEXFER_TYPE_BLOCK_DEVICE;
      break;
    case S_IFREG:
      attrs->type = SSH_FILEXFER_TYPE_REGULAR;
      break;
    case S_IFLNK:
      attrs->type = SSH_FILEXFER_TYPE_SYMLINK;
      break;
    case S_IFSOCK:
      attrs->type = SSH_FILEXFER_TYPE_SOCKET;
      break;
    default:
      attrs->type = SSH_FILEXFER_TYPE_UNKNOWN;
      break;
    }
  } else
    attrs->type = SSH_FILEXFER_TYPE_UNKNOWN;
  if(attrs->valid & SSH_FILEXFER_ATTR_ACCESSTIME) {
    if((rc = sftp_parse_uint32(job, &n)) != SSH_FX_OK)
      return rc;
    attrs->atime.seconds = n;
  }
  if(attrs->valid & SSH_FILEXFER_ATTR_MODIFYTIME) {
    if((rc = sftp_parse_uint32(job, &n)) != SSH_FX_OK)
      return rc;
    attrs->mtime.seconds = n;
  }
  if(attrs->valid & SSH_FILEXFER_ATTR_EXTENDED) {
    if((rc = sftp_parse_uint32(job, &n)) != SSH_FX_OK)
      return rc;
    while(n-- > 0) {
      if((rc = sftp_parse_string(job, 0, 0)) != SSH_FX_OK ||
         (rc = sftp_parse_string(job, 0, 0)) != SSH_FX_OK)
        return rc;
    }
  }
  return SSH_FX_OK;
}

/* Command implementations */

uint32_t sftp_vany_already_init(struct sftpjob attribute((unused)) * job) {
  /* Cannot initialize more than once */
  return SSH_FX_FAILURE;
}

uint32_t sftp_vany_remove(struct sftpjob *job) {
  char *path;
  struct stat sb;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_path(job, &path));
  D(("sftp_vany_remove %s", path));
  if(unlink(path) < 0) {
    if(errno == EPERM || errno == EINVAL) {
      int save_errno = errno;
      if(lstat(path, &sb) == 0 && S_ISDIR(sb.st_mode))
        return SSH_FX_FILE_IS_A_DIRECTORY;
      errno = save_errno;
    }
    return HANDLER_ERRNO;
  } else
    return SSH_FX_OK;
}

uint32_t sftp_vany_rmdir(struct sftpjob *job) {
  char *path;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_path(job, &path));
  D(("sftp_vany_rmdir %s", path));
  if(rmdir(path) < 0)
    if(errno == EEXIST)
      return SSH_FX_DIR_NOT_EMPTY;
    else
      return HANDLER_ERRNO;
  else
    return SSH_FX_OK;
}

uint32_t sftp_v34_rename(struct sftpjob *job) {
  char *oldpath, *newpath;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_path(job, &oldpath));
  pcheck(sftp_parse_path(job, &newpath));
  D(("sftp_v34_rename %s %s", oldpath, newpath));
  /* newpath is not allowed to exist.  We enforce this atomically by attempting
     to link() from oldpath to newpath and unlinking oldpath if it succeeds. */
  if(link(oldpath, newpath) < 0) {
    if(errno != EEXIST) {
      /* On Linux we always get EEXIST if the destination already exists.  We
       * get EPERM if we're trying to link a directory (and the destination
       * doesn't exist) or we're trying to use a non-link-capable filesytem
       * (and the destination doesn't exist).
       *
       * On BSD we get EPERM if we're trying to link a directory even if the
       * destination does exist.
       *
       * So all we can be sure is that EEXIST means the destination definitely
       * exists.  Other errors don't mean it doesn't exist.
       *
       * We give up on atomicity for such cases (v3/v4 drafts do not state a
       * requirement for it) and have other useful semantics instead.
       *
       * This has the slightly odd effect of giving rename(2) semantics only
       * for directories and on primitive filesystems.  If you want such
       * semantics reliably you need SFTP v5 or better.
       *
       * TODO: do a configure check for the local link() semantics.
       */
#ifndef __linux__
      {
        struct stat sb;

        if(lstat(newpath, &sb) == 0)
          return SSH_FX_FILE_ALREADY_EXISTS;
      }
#endif
      if(rename(oldpath, newpath) < 0)
        return HANDLER_ERRNO;
      else
        return SSH_FX_OK;
    } else
      return HANDLER_ERRNO;
  } else if(unlink(oldpath) < 0) {
    const int save_errno = errno;
    unlink(newpath);
    errno = save_errno;
    return HANDLER_ERRNO;
  } else
    return SSH_FX_OK;
}

uint32_t sftp_v345_symlink(struct sftpjob *job) {
  char *targetpath, *linkpath;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  /* The spec is fairly clear.  linkpath is first, targetpath is second.
   * linkpath is the name of the symlink to be created and targetpath is the
   * contents.  This is the reverse of the symlink() call and the ln command,
   * where the first argument is the contents and the second argument the path
   * of the symlink to be created.
   *
   *
   * Implementations that get it right:
   * - Gnome's Nautilus gets the arguments the right way round.
   * - The sshtools.com Java SFTP talks the talk but obviously depends on its
   *   caller getting the arguments in the right order.  CyberDuck (the client I
   *   have to hand that uses it) doesn't seem to have a way to make links.
   *
   * Implementations that get it wrong:
   * - The OpenSSH server and client both this wrong (at the time of writing)
   *   and don't seem to be immediately interested in fixing it - see
   *   http://bugzilla.mindrot.org/show_bug.cgi?id=861 for further details.
   * - WinSCP knows the right way round but if it thinks it's talking to an
   *   OpenSSH SSH server (NB not necessarily the OpenSSH SFTP server) then it
   *   reverses them as a workaround.
   * - Paramiko's client symlink command sends source then dest, which
   *   is the wrong way around (at least as of revno 434/Feb 07).
   * - Paramiko's server also gets it wrong.
   *
   * Implementations that apparently can't make links:
   * - lftp and Konqueror don't seem to be able to create remote symlinks.
   *
   *
   * So what to do?  There's a configure option to select the desired
   * behaviour, and an extension to report to clients what was chosen, but we
   * need to pick a default.  The option of "follow the implementation" doesn't
   * yield a definitive answer as some implementations get it right and some
   * wrong.
   *
   * I go with the specification for the time being but this may be revisited
   * in the light of experience.
   *
   * Currently I assume that any v6 implementations (which has a different link
   * command) will follow the spec but of course I may yet be disappointed.  An
   * extension documenting server behaviour is sent in that case too.
   */
  if(reverse_symlink) {
    pcheck(sftp_parse_string(job, &targetpath, 0));
    pcheck(sftp_parse_path(job, &linkpath));
  } else {
    pcheck(sftp_parse_path(job, &linkpath));
    pcheck(sftp_parse_string(job, &targetpath, 0));
  }
  D(("sftp_v345_symlink %s %s", targetpath, linkpath));
  if(strlen(targetpath) == 0) {
    /* Empty paths are supposed to refer to the default directory.  For a
     * symbolic link target this could mean "." or it could mean the full path
     * to it.  Rather than make a decision we reject this case. */
    D(("sftp_v345_symlink rejecting empty targetpath"));
    sftp_send_status(job, SSH_FX_FAILURE, "link target too short");
    return HANDLER_RESPONDED;
  }
  pcheck(protocol->decode(job, &targetpath));
  if(symlink(targetpath, linkpath) < 0)
    return HANDLER_ERRNO;
  else
    return SSH_FX_OK;
}

uint32_t sftp_vany_readlink(struct sftpjob *job) {
  char *path, *result;
  struct sftpattr attr;

  pcheck(sftp_parse_path(job, &path));
  D(("sftp_vany_readlink %s", path));
  if(!(result = sftp_do_readlink(job->a, path))) {
    if(errno == E2BIG) {
      sftp_send_status(job, SSH_FX_FAILURE, "link name is too long");
      return HANDLER_RESPONDED;
    }
    return HANDLER_ERRNO;
  }
  memset(&attr, 0, sizeof attr);
  attr.name = result;
  sftp_send_begin(job->worker);
  sftp_send_uint8(job->worker, SSH_FXP_NAME);
  sftp_send_uint32(job->worker, job->id);
  protocol->sendnames(job, 1, &attr);
  sftp_send_end(job->worker);
  return HANDLER_RESPONDED;
}

uint32_t sftp_vany_opendir(struct sftpjob *job) {
  char *path;
  DIR *dp;
  struct handleid id;

  pcheck(sftp_parse_path(job, &path));
  D(("sftp_vany_opendir %s", path));
  if(!(dp = opendir(path)))
    return HANDLER_ERRNO;
  sftp_handle_new_dir(&id, dp, path);
  D(("...handle is %" PRIu32 " %" PRIu32, id.id, id.tag));
  sftp_send_begin(job->worker);
  sftp_send_uint8(job->worker, SSH_FXP_HANDLE);
  sftp_send_uint32(job->worker, job->id);
  sftp_send_handle(job->worker, &id);
  sftp_send_end(job->worker);
  return HANDLER_RESPONDED;
}

uint32_t sftp_vany_readdir(struct sftpjob *job) {
  struct handleid id;
  DIR *dp;
  uint32_t rc;
  struct sftpattr d[MAXNAMES];
  int n;
  struct dirent *de;
  const char *path;
  char *childpath, *fullpath;
  struct stat sb;

  pcheck(sftp_parse_handle(job, &id));
  D(("sftp_vany_readdir %" PRIu32 " %" PRIu32, id.id, id.tag));
  if((rc = sftp_handle_get_dir(&id, &dp, &path))) {
    sftp_send_status(job, rc, "invalid directory handle");
    return HANDLER_RESPONDED;
  }
  memset(d, 0, sizeof d);
  for(n = 0; n < MAXNAMES;) {
    /* readdir() has a slightly shonky interface - a null return can mean EOF
     * or error, and there is no guarantee that errno is reset to 0 on EOF. */
    errno = 0;
    de = readdir(dp);
    if(!de)
      break;
    /* We include . and .. in the list - if the cliient doesn't like them it
     * can filter them out itself. */
    childpath = strcpy(sftp_alloc(job->a, strlen(de->d_name) + 1), de->d_name);
    /* We need the full path to be able to stat the file */
    fullpath = sftp_alloc(job->a, strlen(path) + strlen(childpath) + 2);
    strcpy(fullpath, path);
    strcat(fullpath, "/");
    strcat(fullpath, childpath);
    if(lstat(fullpath, &sb))
      return HANDLER_ERRNO;
    sftp_stat_to_attrs(job->a, &sb, &d[n], 0xFFFFFFFF, childpath);
    d[n].name = childpath;
    ++n;
    errno = 0; /* avoid error slippage from
                * e.g. getpwuid() failure */
  }

  if(errno)
    return HANDLER_ERRNO;
  if(n) {
    sftp_send_begin(job->worker);
    sftp_send_uint8(job->worker, SSH_FXP_NAME);
    sftp_send_uint32(job->worker, job->id);
    protocol->sendnames(job, n, d);
    sftp_send_end(job->worker);
    return HANDLER_RESPONDED;
  } else
    return SSH_FX_EOF;
}

uint32_t sftp_vany_close(struct sftpjob *job) {
  struct handleid id;

  pcheck(sftp_parse_handle(job, &id));
  D(("sftp_vany_close %" PRIu32 " %" PRIu32, id.id, id.tag));
  return sftp_handle_close(&id);
}

uint32_t sftp_v345_realpath(struct sftpjob *job) {
  char *path;
  struct sftpattr attr;

  pcheck(sftp_parse_path(job, &path));
  D(("sftp_v345_realpath %s", path));
  memset(&attr, 0, sizeof attr);
  attr.name = sftp_find_realpath(job->a, path, RP_READLINK);
  if(attr.name) {
    D(("...real path is %s", attr.name));
    sftp_send_begin(job->worker);
    sftp_send_uint8(job->worker, SSH_FXP_NAME);
    sftp_send_uint32(job->worker, job->id);
    protocol->sendnames(job, 1, &attr);
    sftp_send_end(job->worker);
    return HANDLER_RESPONDED;
  } else
    return HANDLER_ERRNO;
}

/* Command code for the various _*STAT calls.  rc is the return value
 * from *stat() and SB is the buffer. */
static uint32_t sftp_v3_stat_core(struct sftpjob *job, int rc,
                                  const struct stat *sb) {
  struct sftpattr attrs;

  if(!rc) {
    /* We suppress owner/group name lookup since there is no way to communicate
     * it in protocol version 3 */
    sftp_stat_to_attrs(job->a, sb, &attrs,
                       ~(uint32_t)SSH_FILEXFER_ATTR_OWNERGROUP, 0);
    sftp_send_begin(job->worker);
    sftp_send_uint8(job->worker, SSH_FXP_ATTRS);
    sftp_send_uint32(job->worker, job->id);
    protocol->sendattrs(job, &attrs);
    sftp_send_end(job->worker);
    return HANDLER_RESPONDED;
  } else
    return HANDLER_ERRNO;
}

static uint32_t sftp_v3_lstat(struct sftpjob *job) {
  char *path;
  struct stat sb;

  pcheck(sftp_parse_path(job, &path));
  D(("sftp_v3_lstat %s", path));
  return sftp_v3_stat_core(job, lstat(path, &sb), &sb);
}

static uint32_t sftp_v3_stat(struct sftpjob *job) {
  char *path;
  struct stat sb;

  pcheck(sftp_parse_path(job, &path));
  D(("sftp_v3_stat %s", path));
  return sftp_v3_stat_core(job, stat(path, &sb), &sb);
}

static uint32_t sftp_v3_fstat(struct sftpjob *job) {
  int fd;
  struct handleid id;
  struct stat sb;
  uint32_t rc;

  pcheck(sftp_parse_handle(job, &id));
  D(("sftp_v3_fstat %" PRIu32 " %" PRIu32, id.id, id.tag));
  if((rc = sftp_handle_get_fd(&id, &fd, 0)))
    return rc;
  return sftp_v3_stat_core(job, fstat(fd, &sb), &sb);
}

uint32_t sftp_vany_setstat(struct sftpjob *job) {
  char *path;
  struct sftpattr attrs;
  uint32_t rc;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_path(job, &path));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_vany_setstat %s", path));
  /* Check owner/group */
  if((rc = sftp_normalize_ownergroup(job->a, &attrs)) != SSH_FX_OK)
    return rc;
  return sftp_set_status(job->a, path, &attrs, 0);
}

uint32_t sftp_vany_fsetstat(struct sftpjob *job) {
  struct handleid id;
  struct sftpattr attrs;
  int fd;
  uint32_t rc;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_handle(job, &id));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_vany_fsetstat %" PRIu32 " %" PRIu32, id.id, id.tag));
  /* Check owner/group */
  if((rc = sftp_normalize_ownergroup(job->a, &attrs)) != SSH_FX_OK)
    return rc;
  if((rc = sftp_handle_get_fd(&id, &fd, 0)))
    return rc;
  return sftp_set_fstatus(job->a, fd, &attrs, 0);
}

uint32_t sftp_vany_mkdir(struct sftpjob *job) {
  char *path;
  struct sftpattr attrs;
  uint32_t rc;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_path(job, &path));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_vany_mkdir %s", path));
  attrs.valid &= ~SSH_FILEXFER_ATTR_SIZE; /* makes no sense */
  if(attrs.valid & SSH_FILEXFER_ATTR_PERMISSIONS) {
    D(("initial permissions are %#o (%d decimal)", attrs.permissions,
       attrs.permissions));
    /* If we're given initial permissions, use them  */
    if(mkdir(path, attrs.permissions & 07777) < 0)
      return HANDLER_ERRNO;
    /* Don't modify permissions later unless necessary */
    if(attrs.permissions == (attrs.permissions & 0777))
      attrs.valid ^= SSH_FILEXFER_ATTR_PERMISSIONS;
  } else {
    /* Otherwise be conservative */
    if(mkdir(path, DEFAULT_PERMISSIONS) < 0)
      return HANDLER_ERRNO;
  }
  if((rc = sftp_set_status(job->a, path, &attrs, 0))) {
    const int save_errno = errno;
    /* If we can't have the desired permissions, don't have the directory at
     * all */
    rmdir(path);
    errno = save_errno;
    return rc;
  }
  return SSH_FX_OK;
}

uint32_t sftp_v34_open(struct sftpjob *job) {
  char *path;
  uint32_t pflags;
  struct sftpattr attrs;
  uint32_t desired_access = 0;
  uint32_t flags;

  pcheck(sftp_parse_path(job, &path));
  pcheck(sftp_parse_uint32(job, &pflags));
  pcheck(protocol->parseattrs(job, &attrs));
  D(("sftp_v34_open %s %#" PRIx32, path, pflags));
  /* Translate to v5/6 bits */
  switch(pflags & (SSH_FXF_CREAT | SSH_FXF_TRUNC | SSH_FXF_EXCL)) {
  case 0:
    flags = SSH_FXF_OPEN_EXISTING;
    break;
  case SSH_FXF_TRUNC:
    /* The drafts demand that SSH_FXF_CREAT also be sent making this formally
     * invalid, though there doesn't seem any good reason for them to do so:
     * the client intent seems clear.*/
    flags = SSH_FXF_TRUNCATE_EXISTING;
    break;
  case SSH_FXF_CREAT:
    flags = SSH_FXF_OPEN_OR_CREATE;
    break;
  case SSH_FXF_CREAT | SSH_FXF_TRUNC:
    flags = SSH_FXF_CREATE_TRUNCATE;
    break;
  case SSH_FXF_CREAT | SSH_FXF_EXCL:
  case SSH_FXF_CREAT | SSH_FXF_TRUNC | SSH_FXF_EXCL: /* nonsensical */
    flags = SSH_FXF_CREATE_NEW;
    break;
  default:
    return SSH_FX_BAD_MESSAGE;
  }
  if(pflags & SSH_FXF_TEXT)
    flags |= SSH_FXF_TEXT_MODE;
  if(pflags & SSH_FXF_READ)
    desired_access |= ACE4_READ_DATA | ACE4_READ_ATTRIBUTES;
  if(pflags & SSH_FXF_WRITE)
    desired_access |= ACE4_WRITE_DATA | ACE4_WRITE_ATTRIBUTES;
  if(pflags & SSH_FXF_APPEND) {
    flags |= SSH_FXF_APPEND_DATA;
    desired_access |= ACE4_APPEND_DATA;
  }
  return sftp_generic_open(job, path, desired_access, flags, &attrs);
}

uint32_t sftp_vany_read(struct sftpjob *job) {
  struct handleid id;
  uint64_t offset;
  uint32_t len, rc;
  ssize_t n;
  int fd;
  unsigned flags;

  pcheck(sftp_parse_handle(job, &id));
  pcheck(sftp_parse_uint64(job, &offset));
  pcheck(sftp_parse_uint32(job, &len));
  D(("sftp_vany_read %" PRIx32 " %" PRIu32 " %" PRIu32 ": %" PRIu32
     " bytes at %" PRIu64,
     job->id, id.id, id.tag, len, offset));
  if(len > MAXREAD)
    len = MAXREAD;
  if((rc = sftp_handle_get_fd(&id, &fd, &flags)))
    return rc;
  /* We read straight into our own output buffer to save a copy. */
  sftp_send_begin(job->worker);
  sftp_send_uint8(job->worker, SSH_FXP_DATA);
  sftp_send_uint32(job->worker, job->id);
  sftp_send_need(job->worker, len + 4);
  if(flags & (HANDLE_TEXT | HANDLE_APPEND))
    n = read(fd, job->worker->buffer + job->worker->bufused + 4, len);
  else
    n = pread(fd, job->worker->buffer + job->worker->bufused + 4, len, offset);
  /* Short reads are allowed so we don't try to read more */
  if(n > 0) {
    /* Fix up the buffer */
    sftp_send_uint32(job->worker, n);
    job->worker->bufused += n;
    sftp_send_end(job->worker);
    return HANDLER_RESPONDED;
  }
  /* The error-sending code calls sftp_send_begin(), so we don't get half a
   * SSH_FXP_DATA response first */
  if(n == 0)
    return SSH_FX_EOF;
  else
    return HANDLER_ERRNO;
}

uint32_t sftp_vany_write(struct sftpjob *job) {
  struct handleid id;
  uint64_t offset;
  uint32_t len, rc;
  ssize_t n;
  int fd;
  unsigned flags;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_handle(job, &id));
  pcheck(sftp_parse_uint64(job, &offset));
  pcheck(sftp_parse_uint32(job, &len));
  if(len > job->left)
    return SSH_FX_BAD_MESSAGE;
  D(("sftp_vany_write %" PRIu32 " %" PRIu32 ": %" PRIu32 " bytes at %" PRIu64,
     id.id, id.tag, len, offset));
  if((rc = sftp_handle_get_fd(&id, &fd, &flags)))
    return rc;
  while(len > 0) {
    /* Short writes aren't allowed so we loop around writing more */
    if(flags & (HANDLE_TEXT | HANDLE_APPEND))
      n = write(fd, job->ptr, len);
    else
      n = pwrite(fd, job->ptr, len, offset);
    if(n < 0)
      return HANDLER_ERRNO;
    job->ptr += n;
    job->left += n;
    len -= n;
    offset += n;
  }
  return SSH_FX_OK;
}

uint32_t sftp_vany_posix_rename(struct sftpjob *job) {
  char *oldpath, *newpath;

  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_path(job, &oldpath));
  pcheck(sftp_parse_path(job, &newpath));
  D(("sftp_vany_posix_rename %s %s", oldpath, newpath));
  if(rename(oldpath, newpath) < 0)
    return HANDLER_ERRNO;
  else
    return SSH_FX_OK;
}

uint32_t sftp_vany_statfs(struct sftpjob *job) {
  char *path;
  struct statvfs fs;

  pcheck(sftp_parse_path(job, &path));
  D(("sftp_vany_statfs %s", path));
  if(statvfs(path, &fs) < 0)
    return HANDLER_ERRNO;
  sftp_send_begin(job->worker);
  sftp_send_uint8(job->worker, SSH_FXP_EXTENDED_REPLY);
  sftp_send_uint32(job->worker, job->id);
  /* FUSE's version uses 'struct statfs', we use 'struct statvfs'.  So some of
   * the names are a little different.  However the overall semantics should be
   * the same. */
  sftp_send_uint32(job->worker, fs.f_frsize);
  sftp_send_uint64(job->worker, fs.f_blocks);
  sftp_send_uint64(job->worker, fs.f_bfree);
  sftp_send_uint64(job->worker, fs.f_bavail);
  sftp_send_uint64(job->worker, fs.f_files);
  sftp_send_uint64(job->worker, fs.f_ffree);
  sftp_send_end(job->worker);
  return HANDLER_RESPONDED;
}

static uint32_t sftp_vany_statvfs_send(struct sftpjob *job, int rc,
                                       struct statvfs *fs) {
  if(rc < 0)
    return HANDLER_ERRNO;
  sftp_send_begin(job->worker);
  sftp_send_uint8(job->worker, SSH_FXP_EXTENDED_REPLY);
  sftp_send_uint32(job->worker, job->id);
  sftp_send_uint64(job->worker, fs->f_bsize);
  sftp_send_uint64(job->worker, fs->f_frsize);
  sftp_send_uint64(job->worker, fs->f_blocks);
  sftp_send_uint64(job->worker, fs->f_bfree);
  sftp_send_uint64(job->worker, fs->f_bavail);
  sftp_send_uint64(job->worker, fs->f_files);
  sftp_send_uint64(job->worker, fs->f_ffree);
  sftp_send_uint64(job->worker, fs->f_favail);
  sftp_send_uint64(job->worker, fs->f_fsid);
  sftp_send_uint64(job->worker, (fs->f_flag & ST_RDONLY ? 1 : 0) |
                                    (fs->f_flag & ST_NOSUID ? 2 : 0));
  sftp_send_uint64(job->worker, fs->f_namemax);
  sftp_send_end(job->worker);
  return HANDLER_RESPONDED;
}

uint32_t sftp_vany_statvfs(struct sftpjob *job) {
  char *path;
  struct statvfs fs;

  pcheck(sftp_parse_path(job, &path));
  D(("sftp_vany_statfs %s", path));
  return sftp_vany_statvfs_send(job, statvfs(path, &fs), &fs);
}

uint32_t sftp_vany_fstatvfs(struct sftpjob *job) {
  int fd;
  struct handleid id;
  struct statvfs fs;
  uint32_t rc;

  pcheck(sftp_parse_handle(job, &id));
  D(("sftp_vany_fstatvfs %" PRIu32 " %" PRIu32, id.id, id.tag));
  if((rc = sftp_handle_get_fd(&id, &fd, 0)))
    return rc;
  return sftp_vany_statvfs_send(job, fstatvfs(fd, &fs), &fs);
}

uint32_t sftp_vany_fsync(struct sftpjob *job) {
  int fd;
  struct handleid id;
  uint32_t rc;

  pcheck(sftp_parse_handle(job, &id));
  D(("sftp_v3_fstat %" PRIu32 " %" PRIu32, id.id, id.tag));
  if((rc = sftp_handle_get_fd(&id, &fd, 0)))
    return rc;
  if(fsync(fd) < 0)
    return HANDLER_ERRNO;
  return SSH_FX_OK;
}

uint32_t sftp_vany_hardlink(struct sftpjob *job) {
  char *oldpath, *newlinkpath;

  /* See also comment in v3.c for SSH_FXP_SYMLINK */
  if(readonly)
    return SSH_FX_PERMISSION_DENIED;
  pcheck(sftp_parse_path(job, &oldpath)); /* aka existing-path/target-paths */
  pcheck(sftp_parse_path(job, &newlinkpath));
  D(("sftp_hardlink %s %s", oldpath, newlinkpath));
  if(link(oldpath, newlinkpath) < 0)
    return HANDLER_ERRNO;
  return SSH_FX_OK;
}

static const struct sftpcmd sftpv3tab[] = {
    {SSH_FXP_INIT, sftp_vany_already_init},
    {SSH_FXP_OPEN, sftp_v34_open},
    {SSH_FXP_CLOSE, sftp_vany_close},
    {SSH_FXP_READ, sftp_vany_read},
    {SSH_FXP_WRITE, sftp_vany_write},
    {SSH_FXP_LSTAT, sftp_v3_lstat},
    {SSH_FXP_FSTAT, sftp_v3_fstat},
    {SSH_FXP_SETSTAT, sftp_vany_setstat},
    {SSH_FXP_FSETSTAT, sftp_vany_fsetstat},
    {SSH_FXP_OPENDIR, sftp_vany_opendir},
    {SSH_FXP_READDIR, sftp_vany_readdir},
    {SSH_FXP_REMOVE, sftp_vany_remove},
    {SSH_FXP_MKDIR, sftp_vany_mkdir},
    {SSH_FXP_RMDIR, sftp_vany_rmdir},
    {SSH_FXP_REALPATH, sftp_v345_realpath},
    {SSH_FXP_STAT, sftp_v3_stat},
    {SSH_FXP_RENAME, sftp_v34_rename},
    {SSH_FXP_READLINK, sftp_vany_readlink},
    {SSH_FXP_SYMLINK, sftp_v345_symlink},
    {SSH_FXP_EXTENDED, sftp_vany_extended}};

static const struct sftpextension v3_extensions[] = {
    {"fsync@openssh.com", "1", sftp_vany_fsync},
    {"hardlink@openssh.com", "1", sftp_vany_hardlink},
    {"posix-rename@openssh.com", "1", sftp_vany_posix_rename},
    {"posix-rename@openssh.org", "", sftp_vany_posix_rename},
    {"space-available", "", sftp_vany_space_available},
    {"statfs@openssh.org", "", sftp_vany_statfs},
    {"statvfs@openssh.com", "2", sftp_vany_statvfs},
    {"fstatvfs@openssh.com", "2", sftp_vany_fstatvfs},
};

const struct sftpprotocol sftp_v3 = {
    sizeof sftpv3tab / sizeof(struct sftpcmd), /* ncommands */
    sftpv3tab,                                 /* commands */
    3,                                         /* version */
    (SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS |
     SSH_FILEXFER_ATTR_ACCESSTIME | SSH_FILEXFER_ATTR_MODIFYTIME |
     SSH_FILEXFER_ATTR_UIDGID), /* attrmask */
    SSH_FX_OP_UNSUPPORTED,      /* maxstatus */
    v3_sendnames,
    v3_sendattrs,
    v3_parseattrs,
    sftp_v3_encode,
    v3_decode,
    sizeof v3_extensions / sizeof(struct sftpextension),
    v3_extensions, /* extensions */
};

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
