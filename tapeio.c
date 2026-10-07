/*
  Routines to do magtape I/O to local tapes, remote tapes, and tape image
  files.

  Copyright 1998, 1999 John Wilson and Eric Smith

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License version 2 as published
  by the Free Software Foundation.  Note that permission is not granted
  to redistribute this program under the terms of any other version of the
  General Public License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  08/10/93  JMBW  IBM mainframe TCP socket stuff (was using many files).
  07/08/94  JMBW  Local magtape code.
  03/13/95  JMBW  Converted to separate routines.
  07/19/98  JMBW  Added support for "rmt" remote tape protocol.
  11/16/98  ELS   Provide struct for per-instance variables.
  02/06/98  ELS   Reorganization, and added skiprec and skipfile.
*/


#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>	/* for lseek() SEEK_SET, SEEK_END under Linux */
#include <errno.h>

#ifdef _AIX /* maybe this will be enough to make it compile on AIX */
#include <sys/tape.h>
#define MTWEOF STWEOF
#define MTREW STREW
#define MTFSR STFSR
#define MTFSF STFSF
#define MTBSR STRSR
#define MTIOCTOP STIOCTOP
/* not sure about these two (SCSI only): */
#define MTSETBLK STSETBLK
#define MTSETDENSITY STSETDENSITY
#define mtop stop
#elif __APPLE__
#define MTWEOF 0
#define MTREW 0
#define MTFSR 0
#define MTFSF 0
#define MTBSR 0
#define MTIOCTOP 0
#define MTSETBLK 0
#define MTSETDENSITY 0
struct mtop { int mt_op; int mt_count; };
#else
#include <sys/mtio.h>
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define MTSETBLK MTSETBSIZ
#define MTSETDENSITY MTSETDNSTY
#endif
#endif

#include "tapeio.h"


#define TT_UNK   0
#define TT_TAPE  1  /* honest to god tape drive */
#define TT_IMAGE 2  /* file containing image of a tape */
#define TT_RMT   3  /* rmt tape server */


struct mtape_t
{
  int tape_type;
  int tapefd;		/* tape drive, file, or socket file descriptor */
  int seek_ok;
  int flags;

  unsigned long bpi;	/* tape density (for tape length msg) */
  int waccess;		/* NZ => tape opened for write access access */
  unsigned long count;	/* count of frames written to tape */

  char netbuf[80];	/* buffer for net commands and responses */
};


#ifndef O_BINARY
#define O_BINARY 0
#endif

/* default tape drive device name */
#define TAPE "/dev/nst0"
/* default tape density */
#define BPI 1600


/* magtape commands */
static struct mtop mt_weof={ MTWEOF, 1 }; /* operation, count */
static struct mtop mt_rew={ MTREW, 1 };
static struct mtop mt_fsr={ MTFSR, 1 };
static struct mtop mt_fsf={ MTFSF, 1 };
static struct mtop mt_bsr={ MTBSR, 1 };
/* SCSI only: */
static struct mtop mt_setblk={ MTSETBLK, 0 };  /* blockize = 0 (variable) */
static struct mtop mt_setden={ MTSETDENSITY, 0x02 };  /* density = 1600 */


#define FAIL(msg) do { fprintf (stderr, msg); goto fail; } while (0)

#define SCRATCH_BUF_SIZE    4096

/* do a write and check the return status, punt on error */
static void dowrite (int handle, void *buf, int len)
{
  if (write (handle, buf, len) != len)
    {
      perror ("?Error on write");
      exit (1);
    }
}


/* do a read and keep trying until we get all bytes */
static void doread (int handle, void *buf, int len)
{
  int n;
  while(len)
    {
      n = read (handle, buf, len);
      if (n < 0)
	{
	  perror("?Error on read");
	  exit (1);
	}
      if (n == 0)
	{
	  fprintf (stderr, "?Unexpected end of file\n");
	  exit (1);
	}
      buf += n;
      len -= n;
    }
}
static unsigned long doread_u32(int handle)
{
  unsigned char bytes[4];	/* 32 bits for length field(s) */
  unsigned long ul;		/* at least 32 bits */

  doread (handle, &bytes[0], 4);  /* get next 4 bytes */
  ul =	((unsigned long)bytes[3]<<24L) | ((unsigned long)bytes[2]<<16L)|
        ((unsigned long)bytes[1]<< 8L) |  (unsigned long)bytes[0];
  return ul;
}


/* get response from "rmt" server */
static int response (tape_handle_t mtape)
{
  char c, rc;
  int n;

  doread (mtape->tapefd, &rc, 1);	/* get success/error code */
  if (rc != 'A' && rc != 'E')
    {	/* must be Acknowledge or Error */
      fprintf (stderr, "?Invalid rmt response code:  %c\n",rc);
      exit (1);
    }

  /* get numeric value (returned by both A and E responses) */
  for (n=0;;)
    {
      doread (mtape->tapefd, &c, 1);  /* get next digit */
      if (c < '0' || c > '9')
	break;  /* not a digit */
      n = n * 10 + (c - '0');	/* add new digit in */
      /* ideally would check for overflow */
    }
  if (c != '\n')
    {		/* first non-digit char must be <LF> */
      fprintf (stderr, "?Invalid rmt response terminator:  %3.3o\n",
	       ((int) c) & 0377);
      exit (1);
    }
  if (rc == 'A')
    return (n);	/* success, return value >=0 */
				/* (unless overflowed) */
  do
    doread (mtape->tapefd, &c, 1);
  while (c != '\n');		/* ignore until next LF */
  errno = n;		/* set error number */
  return (-1);
}


/* send ioctl() command to local or remote tape drive */
static int doioctl (tape_handle_t mtape, struct mtop *op)
{
  int len;

  if (mtape->tape_type == TT_TAPE)
    return (ioctl (mtape->tapefd, MTIOCTOP, op));
  else
    {	/* "rmt" tape server */
      /* form cmd (better hope remote MT_OP values are the same) */
      len = sprintf (mtape->netbuf, "I%d\n%d\n", op->mt_op, op->mt_count);
      dowrite (mtape->tapefd, mtape->netbuf, len);
      return (response (mtape));
    }
}


/* open the tape drive (or whatever) */
/* "create" =1 to create if file, "writable" =1 to open with write access */
tape_handle_t opentape (char *name, int create, int writable)
{
  char *p, *user, *port;
  int len;
  char *host = NULL;

  tape_handle_t mtape = NULL;

  mtape = (tape_handle_t) calloc (1, sizeof (struct mtape_t));
  if (! mtape)
    FAIL ("?can't allocate mtape struct\n");

  mtape->bpi = BPI;

  mtape->waccess = writable;		/* remember if we're writing */
  mtape->count = 0;			/* nothing transferred yet */

  /* get tape filename */
  if (name == NULL)
    name = getenv("TAPE");	/* get from environment */
  if (name == NULL)
    name = TAPE;		/* or use our default */

  /* just a file if no colon in filename */
  if ((p = index (name, ':')) == NULL)
    {
      /* there's probably a better way to handle this, in case a file is really
	 a link to a tape drive -- handler index or something? */
      if (strncmp (name, "/dev/", 5) == 0)
	{
	  /* assume tape if starts with /dev/ */
	  mtape->tape_type = TT_TAPE;
	  mtape->tapefd = open (name, (writable ? O_RDWR : O_RDONLY), 0);
	}
      else
	{	/* otherwise file */
	  mtape->tape_type = TT_IMAGE;
	  if (strcmp (name, "-") ==0 )
	    { /* stdin/stdout */
	      if (writable)
		mtape->tapefd=1;
	      else
		mtape->tapefd=0;
	    }
	  else
	    {
	      if (create)
		mtape->tapefd = open (name, O_CREAT | O_TRUNC |
				      O_WRONLY | O_BINARY, 0644);
	      else
		{
		  mtape->tapefd = open (name, (writable ? O_RDWR : O_RDONLY) |
					O_BINARY, 0);
		  mtape->seek_ok = 1;
		}
	    }
	}
      if (mtape->tapefd < 0)
	FAIL ("?can't open device or file\n");
    }
  else
    {	/* "rmt" tape server on remote host */
      mtape->tape_type = TT_RMT;
      /* split filename around ':' */
      len = p-name;
      port = p+1;

      /* can't necessarily modify tape[] so copy it first */
      if ((host = malloc (len + 1)) == NULL)
	FAIL ("?can't allocate string for hostname\n");
      strncpy (host, name, len);		/* copy hostname */
      host [len] = 0;			/* tack on null */

      /* connect to "rexec" server */
      if ((p = index (host, '@')) == NULL)
	{
	  p = host;	/* no @, point at hostname */
	  user = NULL;
	}
      else
	{
	  *p++ = '\0';	/* shoot out @, point at host name */
	  user = (*p != '\0') ? host : NULL;  /* keep non-null user */
	}
#if !defined(__APPLE__) && !defined(__OpenBSD__)
      if ((mtape->tapefd = rexec (&p, htons (512), user, NULL, "/etc/rmt",
				  (int *) NULL)) < 0)
        FAIL ("?Connection failed\n");
#endif

      /* build rmt "open device" command */
      if ((1 + strlen (port) + 1 + 1 + 1 + 1) > sizeof (mtape->netbuf))
	FAIL ("?Device name too long\n");
      len = sprintf (mtape->netbuf, "O%s\n%d\n", port, writable ? O_RDWR : O_RDONLY);
      dowrite (mtape->tapefd, mtape->netbuf, len);
      if (response (mtape) < 0)
	FAIL ("?Error opening tape drive");
    }

  /* SCSI setup for local/remote tape drive */
  if ((mtape->tape_type == TT_TAPE) ||
      (mtape->tape_type == TT_RMT))
    {
      /* (ignore errors in case not SCSI) */
      /* set variable record length mode */
      doioctl (mtape, & mt_setblk);
      /* set density to 1600 */
      doioctl (mtape, & mt_setden);
    }

  if (host)
    free (host);

  return (mtape);

 fail:
  if (mtape)
    {
      if (host)
	free (host);
      free (mtape);
    }
  return (NULL);
}


/* close the tape drive */
void closetape (tape_handle_t mtape)
{
  if (mtape->waccess)
    {				/* opened for create/append */
      tapemark (mtape);		/* add one more tape mark */
      				/* (should have one already) */
    }
  if (mtape->tape_type == TT_RMT)
    {
      dowrite (mtape->tapefd, "C\n", 2);
      if (response (mtape) < 0)
	{
	  perror("?Error closing remote tape");
	  exit(1);
	}
    }
  if (close (mtape->tapefd) < 0)
    {
      perror("?Error closing tape");
      exit(1);
    }
  free (mtape);
}


/* rewind tape */
void posnbot (tape_handle_t mtape)
{
  if (mtape->tape_type == TT_IMAGE)
    {		/* image file */
      if (lseek (mtape->tapefd, 0L, SEEK_SET) < 0)
	{
	  perror("?Seek failed");
	  exit(1);
	}
    }
  else
    {				/* local/remote tape drive */
      if (doioctl (mtape, & mt_rew) < 0)
	{
	  perror("?Rewind failed");
	  exit(1);
	}
    }
}


/* position tape at EOT (between the two tape marks) */
void posneot (tape_handle_t mtape)
{
  if (mtape->tape_type == TT_IMAGE)
    {		/* image file */
      if (lseek (mtape->tapefd, -4L, SEEK_END) < 0)
	{
	  perror("?Seek failed");
	  exit(1);
	}
    }
  else
    {				/* local/remote tape drive */
      doioctl (mtape, & mt_bsr);	/* in case already at LEOT */
      while (1)
	{
	  /* space forward a file */
	  if (doioctl (mtape, & mt_fsf) < 0)
	    {
	      perror("?Error spacing to EOT");
	      exit(1);
	    }
	  /* space one record more to see if double EOF */
	  if (doioctl (mtape, & mt_fsr) < 0)
	    break;
/* might want to check errno to make sure it's the right error */
	}
#if 1
      /* "man mtio" doesn't say whether MTFSR actually moves past */
      /* the tape mark, let's assume it does */
      if (doioctl (mtape, & mt_bsr) < 0)
	{  /* get between them */
	  perror("?Error backspacing at EOT");
	  exit(1);
	}
#endif
    }
}


/* read a tape record, return actual length (0=tape mark) */
int getrec (tape_handle_t mtape, void *buf, int len)
{
  unsigned long curlen;		/* at least 32 bits */
  unsigned long reclen;		/* at least 32 bits */
  int i;

  if (mtape->tape_type == TT_IMAGE)
    {		/* image file */
      reclen = doread_u32(mtape->tapefd );    /* get record length */
      if (reclen > len)
	goto toolong;	/* don't read if too long for buf */
      if (reclen != 0)
	{		/* get data unless tape mark */
	  char x;
	  doread (mtape->tapefd, buf, reclen);  /* read data */
	  if ((reclen & 1) != 0 && (mtape->flags & TF_SIMH) != 0)
	    {
	      doread (mtape->tapefd, &x, 1);
	    }
	  curlen = doread_u32(mtape->tapefd );  /* get trailing record length */
	  if(curlen != reclen)
	    {	/* should match */
	      fprintf (stderr,"?Corrupt tape image");
	      fprintf (stderr," reclen != curlen %lu != %lu\n", reclen, curlen);
	      exit(1);
	    }
	}
    }
  else if (mtape->tape_type == TT_RMT)
    {		/* rmt tape server */
      len = sprintf (mtape->netbuf, "R%d\n", len);
      dowrite (mtape->tapefd, mtape->netbuf, len);
      if ((i = response (mtape)) < 0)
	{
	  perror("?Error reading rmt tape");
	  exit(1);
	}
      reclen = i;
      if (reclen)
	{
	  doread (mtape->tapefd, buf, reclen);
	}
    }
  else
    {				/* local tape drive */
      if ((i = read (mtape->tapefd, buf, len)) < 0)
	{
	  perror("?Error reading local tape");
	  exit(1);
	}
      reclen = i;
    }
  return(reclen);

 toolong:
  fprintf(stderr,"?%ld byte tape record too long for %d byte buffer\n",
	  reclen,len);
  exit(1);
}


/* write a tape record */
void putrec (tape_handle_t mtape, void *buf, int len)
{
  unsigned char l [4];

  if (mtape->tape_type == TT_IMAGE)
    {		/* image file */
      l [0] = len & 0377;		/* PDP-11 byte order */
      l [1] = (len >> 8) &0377;
      l [2] = 0;			/* our recs are always < 64 KB */
      l [3] = 0;
      dowrite (mtape->tapefd, l, 4);	/* write longword length */
      dowrite (mtape->tapefd, buf, len);  /* write data */
      dowrite (mtape->tapefd, l, 4);	/* write length again */
    }
  else if (mtape->tape_type == TT_RMT)
    {		/* rmt tape */
      int n;
      n = sprintf (mtape->netbuf, "W%d\n", len);
      dowrite (mtape->tapefd, mtape->netbuf, n);
      dowrite (mtape->tapefd, buf, len);
    }
  else
    {
      dowrite (mtape->tapefd, buf, len);	/* just write the data if tape */
    }

  mtape->count += len + (mtape->bpi * 3 /5);  /* add to byte count
						 (+0.6" tape gap) */
}


/* write a tape mark */
void tapemark (tape_handle_t mtape)
{
  static char zero [4] = { 0, 0, 0, 0 };

  if (mtape->tape_type == TT_IMAGE)
    {		/* image file */
      dowrite (mtape->tapefd, zero, 4);	/* write longword length */
    }
  else
    {				/* local/remote tape drive */
      if (doioctl (mtape, & mt_weof) < 0)
	{
	  perror ("?Failed writing tape mark");
	  exit (1);
	}
    }
  mtape->count += 3 * mtape->bpi;	/* 3" of tape */
}


/* skip records (negative for reverse) */
void skiprec (tape_handle_t mtape, int count)
{
  unsigned long reclen;		/* at least 32 bits */
  unsigned long traillen;	/* at least 32 bits */

  if (mtape->tape_type != TT_IMAGE)
    {
      fprintf (stderr, "?Record skip only implemented for image files");
      exit (1);
    }

  if (count < 0)
    {
      fprintf (stderr, "?Record skip reverse not yet implemented");
      exit (1);
    }

  while (count--)
    {
      reclen = doread_u32(mtape->tapefd );    /* get record length */
      if (reclen == 0)  /* hit tape mark? */
	return;  /* note that we've effectively skipped over the tape mark */

      /* skip record */
      if (lseek (mtape->tapefd, reclen, SEEK_CUR) < 0)
	{
	  perror ("?Seek failed");
	  exit (1);
	}

      traillen = doread_u32(mtape->tapefd );  /* get trailing record length */
      if(traillen != reclen)
	{	/* should match */
	  fprintf (stderr,"?Corrupt tape image");
	  fprintf (stderr," reclen != traillen %lu != $%lu\n", reclen, traillen);
	  exit(1);
	}
    }
}


/* skip forward to the next file mark, and leave the tape positioned
   after the mark */
static void skip_to_mark (tape_handle_t mtape)
{
  unsigned long reclen;		/* at least 32 bits */
  unsigned long traillen;	/* at least 32 bits */

  static char scratch_buf [SCRATCH_BUF_SIZE];

  if (mtape->tape_type != TT_IMAGE)
    {
      fprintf (stderr, "?Record skip only implemented for image files");
      exit (1);
    }

  for (;;)
    {
      reclen = doread_u32(mtape->tapefd );    /* get record length */
      if (reclen == 0)  /* hit tape mark? */
	return;  /* note that we've effectively skipped over the tape mark */

      /* skip record */
      if (mtape->seek_ok)
	{
	  if (lseek (mtape->tapefd, reclen, SEEK_CUR) < 0)
	    {
	      perror ("?Seek failed");
	      exit (1);
	    }
	}
      else
	{
	  int len, len2;
	  len = reclen;
	  while (len > 0)
	    {
	      len2 = len;
	      if (len2 > sizeof (scratch_buf))
		len2 = sizeof (scratch_buf);
	      doread (mtape->tapefd, scratch_buf, len2);  /* read data */
	      len -= len2;
	    }
	}

      traillen = doread_u32(mtape->tapefd );  /* get trailing record length */
      if(traillen != reclen)
	{	/* should match */
	  fprintf (stderr,"?Corrupt tape image");
	  fprintf (stderr," reclen != traillen %lu != $%lu\n", reclen, traillen);
	  exit(1);
	}
    }
}


/* skip files (negative for reverse) */
void skipfile (tape_handle_t mtape, int count)
{
  if (mtape->tape_type != TT_IMAGE)
    {
      fprintf (stderr, "?File skip only implemented for image files");
      exit (1);
    }

  if (count < 0)
    {
      fprintf (stderr, "?File skip reverse not yet implemented");
      exit (1);
    }

  while (count--)
    {
      skip_to_mark (mtape);
    }
}

/* set tape flags */
void tapeflags (tape_handle_t mtape, int flags)
{
  mtape->flags = flags;
}
