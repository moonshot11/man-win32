/* A simple `man' clone.

   Features:

       1. Doesn't care about the file name extension of the man pages
	  (unless you specify a man section).  Thus, you can have it
	  find files with extensions like .doc, .hlp, .txt, etc.
       2. Supports flat man directories, i.e., you could dump all the
	  pages into a single directory, for example.
       3. Supports both SysV-like -s SECTION and BSD-like SECTION methods
          of specifying the man sections.
       4. Finds pages for topics longer than 8 characters on DOS 8+3
	  filesystems.

   Bugs:

       1. Requires Groff or a very close work-alike to display unformatted
	  pages which require preprocessing.
       2. Doesn't support preprocessing with vgrind (since Groff doesn't).
       3. Supports only a subset of options.  In particular, `whatis'
	  database and related options (-f, -k) aren't supported.
       4. Multiple sections (-s 2,3,8) aren't supported directly.  (Use
	  multiple requests like "-s 2 foo -s 3 foo" to work around.)
       5. "man foo" will happily try to display foo.tgz, if found (because
	  it doesn't care about the extension too much).
       6. Relatively slow on large directories.

   Tested in interactive use, with Emacs, and with stand-alone Info.
   Memory usage checked with YAMD v0.32.

   Author: Eli Zaretskii <eliz@gnu.org>

   Ported to Borland C, 16 bit DOS, 10 feb 2000
   by Erwin Waterlander, waterlan@xs4all.nl or erwin.waterlander@philips.com

   Ported to MinGW, Nov 2005

   Last updated: November 5, 2005

   -----------------------------------------------------------------------
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GNU Emacs; see the file COPYING.  If not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
   ------------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#ifdef __TURBOC__
# include <io.h>
# include <dir.h>
# include <malloc.h>
# include <dirent.h>
# include "limits.h"
# include "unistd.h"
# include "fnmatch.h"
#else
# include <fnmatch.h>
# include <unistd.h>
#endif

#ifdef __DJGPP__
# include <io.h>
# include <fcntl.h>
#endif

#ifdef __WIN32__
# include <windows.h>
# include <malloc.h>
#endif	 /* __WIN32__ */

#ifdef __TURBOC__
# define MATCHFLAGS FNM_CASEFOLD
#else
# define MATCHFLAGS 0
#endif /* TURBOC */

static const char *version = "1.4";

/* Defaults: the pager, directories to look for man pages, groff, etc.  */
#ifdef MSDOS

/* The .exe suffix in program names is so we get better diagnostics
   from `system' library function when the shell is COMMAND.COM.  */
static char *pager    = "less.exe -c";
static char *groff    = "groff.exe -man -Tascii";
# ifdef __DJGPP__
static char *manpath  = "c:/djgpp/man;c:/djgpp/info;/usr/man";
# else
static char *manpath  = "/man;/usr/man";
# endif
# define PATH_SEP	';'
# define IS_DIR_SEP(x)	((x) == '/' || (x) == '\\')

#else  /* not MSDOS */

# ifdef __WIN32__
static char *pager    = "less.exe -c";
static char *groff    = "groff.exe -man -Tascii";
static char *manpath  = "c:/usr/man;c:/usr/info;/usr/man";
#  define PATH_SEP	';'
#  define IS_DIR_SEP(x)	((x) == '/' || (x) == '\\')

# else	/* not __WIN32__ */
static char *pager    = "less -c";
static char *groff    = "groff -man -Tascii";
static char *manpath  = "/usr/local/man:/usr/share/man:/usr/man";
#  define PATH_SEP   ':'
#  define IS_DIR_SEP(x)	((x) == '/')

# endif	 /* not __WIN32__ */
#endif /* not MSDOS */

static char section_letters[] = "123456789lnop";

/* The name we were invoked.  */
char *progname;

/* If non-zero, display all manual pages which match a given NAME.  */
int show_all_option;

/* If non-zero, just list all the matching pages, but don't display them.  */
int list_all_option;

/* If non-zero, list all the file paths of matching pages.  */
int list_fpaths_option;

/* If non-zero, print diagnostic messages.  */
int verbose_option;

/* If non-zero, prints debugging messages during operation.  */
int debugging_output;

/* If non-zero, output is not piped through the pager.  */
int direct_output;

/* Utility functions.  */
void *
xmalloc (size_t size)
{
  void *p = malloc (size);

  if (size && p == (void *)0)
    {
      fprintf (stderr, "Memory exhausted!\n");
      exit (4);
    }
  return p;
}

void *
xrealloc (void *ptr, size_t new_size)
{
  void *p = realloc (ptr, new_size);

  if (new_size && p == (void *)0)
    {
      fprintf (stderr, "Memory exhausted!\n");
      exit (4);
    }
  return p;
}

/* Manipulating the stored man pages.  */
#define FMT_MASK		0x3f
#define FLAG_SOELIM		0x01
#define FLAG_EQN		0x02
#define FLAG_TBL		0x04
#define FLAG_REFER		0x08
#define FLAG_VGRIND		0x10
#define FLAG_UNFORMATTED	0x20
#define FLAG_FORMATTED		0x40
#define FLAG_CANT_OPEN		0x80

/* The info we store about each man page.  */
typedef struct {
  char *path;		 /* the full pathname of the file */
  char *name;		 /* pointer into PATH where its basename begins */
  int section;		 /* numerical section code */
  unsigned flags;	 /* various prperties, see definitions above */
} Man_page;

static Man_page **pages; /* array that holds man pages we've found so far */
static int max_pages;	 /* how much slots do we have in PAGES array? */
static int next_slot;	 /* the index of the next slot to be used */

/* Add a page to the list of stored pages.  */
void
add_page (Man_page *page)
{
  if (next_slot >= max_pages)
    {
      /* Time to realloc.  I believe this won't be called more than once.  */
      max_pages += 2 * 3;  /* catN + manN times 3 pages per topic */
      pages = (Man_page **)xrealloc (pages, max_pages * sizeof (Man_page *));
    }
  pages[next_slot++] = page;
  if (debugging_output)
    fprintf (stderr, "Added page `%s'\n", page->path);
}

/* Delete a page from the list.  This also compacts the list.  */
void
remove_page (int idx)
{
  if (idx < 0 || idx >= next_slot)
    {
      fprintf (stderr, "impossible index in `remove_page'\n");
      exit (3);
    }
  else
    {
      Man_page *p = pages[idx];

      free (p->path);
      free (p);
      if (idx < next_slot - 1)	/* if not last */
	memmove (pages + idx, pages + idx + 1,
		 (next_slot - idx - 1)*sizeof (Man_page *));
    }
  next_slot--;
}

/* A helper function for sorting stored pages.  */
int
compare_pages (const void *p1, const void *p2)
{
  /* FIXME: do we need name comparison as well?  I don't think so.  */
  const Man_page *t1 = *(const Man_page **)p1, *t2 = *(const Man_page **)p2;

  /* First section numbers, then formatting requirements.
     Sort into descending order, so we could unwind the list from the end.  */
  return (t2->section - t1->section) * 100
	 + (t2->flags & FMT_MASK) - (t1->flags & FMT_MASK);
}

void
sort_pages (void)
{
  qsort (pages, next_slot, sizeof (Man_page *), compare_pages);
}

/* Page display stuff.  */

/* Pipe the page through a formatter (if needed) to a pager.  */
int
display_page (const char *file, const char *formatter)
{
  size_t cmdlen = (formatter ? strlen (formatter) + 3 : 0) +
		  strlen (file) + 1 + strlen (pager) + 1;
  char *cmd = (char *)alloca (cmdlen);
  int status;

  if (formatter)
    {
      /* "groff -man -Tascii /usr/man/foo.1 | less -c"  */
      sprintf (cmd, "%s \"%s\"%s%s%s", formatter, file,
	       direct_output ? "" : " | ",
	       direct_output ? "" : pager, "");
    }
  else
    /* "less -c /usr/man/foo.1"
        Don't require `cat' unless we really need it.  Pagers usually
	disable all screen effects when stdout is not a terminal, so
	their pager could serve as `cat' also.  */
    sprintf (cmd, "%s \"%s\"", !direct_output || !isatty (fileno (stdout))
				   ? pager : "cat", file);

  if (debugging_output)
    fprintf (stderr, "Running `%s'\n", cmd);
  status = system (cmd);

  if (status == -1)
    fprintf (stderr, "%s: %s%s%s: %s\n", progname,
	     !direct_output || !isatty (fileno (stdout)) ? pager : "cat",
	     formatter ? " or " : "", formatter ? formatter : "",
	     strerror (errno));
  else if (verbose_option && status)
    fprintf (stderr, "%s: `%s' returned %d\n", progname, cmd, status);

  return status;
}

/* Print the page, its section, and the part of MANPATH where it was found. */
void
list_page (const Man_page *page)
{
  char *topic = page->name;
  char *subdir = page->name - 5;
  char *section_name = strchr (topic, '.');

  if (subdir > page->path && subdir[-1] == '/'
      && (strncmp (subdir, "cat", 3) == 0 || strncmp (subdir, "man", 3) == 0)
      && strchr (section_letters, subdir[3]))
    {
      if ((subdir == page->path + 1 && IS_DIR_SEP (subdir[-1]))
#if defined(MSDOS) || defined(__WIN32__)
	  /* DOS-style absolute pathname with a drive letter.  */
	  || (subdir == page->path + 3
	      && IS_DIR_SEP (subdir[-1]) && subdir[-2] == ':')
#endif
	  )
	subdir[0] = '\0';
      else
	subdir[-1] = '\0';
    }
  else
    topic[-1] = '\0';

    if (section_name)
        *section_name++ = '\0';	/* remove the dot and get past it */

    if (list_all_option)
    {
        printf ("%s", topic);
        if (section_name)
            printf (" (%s)\t", section_name);
        else
            printf ("     \t");
        printf ("-M %s\n", page->path); /* say which -M argument will find it */
    }
    else if (list_fpaths_option)
    {
        char last = page->path[ strlen(page->path)-1 ];
        char *sep = (last == '\\') ? "" : "\\";
        printf("%s%sman%s\\%s.%s\n",
                page->path, sep, section_name, page->name, section_name);
    }
}

/* Searching for man pages along MANPATH.  */

/* Compute the section numeric code from the basename of the page file.
   The numeric code is used to sort the pages we find.  */
int
set_section (const char *basename)
{
  size_t baselen = strlen (basename);
  int secno = 300;	/* for non-standard extensions, should be > 26*10 */
  const char *s = 0;

  if (baselen > 2)
    {
      if (basename[baselen - 2] == '.')
	s = basename + baselen - 1;
      else if (baselen > 3 && basename[baselen - 3] == '.')
	s = basename + baselen - 2;
      if (s && isdigit (*s))
	{
	  secno = (*s - '0') * 26;
	  if (s[1])
	    secno += (s[1] & 0x1f) - 'A';
	}
    }
  return secno;
}

/* Compute the formatting flags for a page by reading its first line.  */
unsigned
set_flags (const char *file)
{
  char line[10];
  FILE *fp = fopen (file, "rt");
  unsigned retval = 0;

  if (!fp || !fgets (line, 10, fp))
    {
      retval = FLAG_CANT_OPEN;
      if (verbose_option)
	fprintf (stderr, "%s: `%s' is unreadable: %s\n", progname, file,
		 strerror (errno));
    }
  else if (strncmp (line, "'\\\" ", 4) == 0)
    {
      char *p;

      retval = FLAG_UNFORMATTED;
      for (p = line + 4; *p; p++)
	switch (*p)
	  {
	    case  'e':
	      retval |= FLAG_EQN;
	      break;
	    case 'r':
	      retval |= FLAG_REFER;
	      break;
	    case 't':
	      retval |= FLAG_TBL;
	      break;
	    case 'v':
	      retval |= FLAG_VGRIND;
	      break;
	    default:
	      if (!isspace (*p) && verbose_option)
		fprintf (stderr, "%s: unknown preprocessing specifier %c\n",
			 progname, *p);
	      break;
	  }
    }
  else if (strncmp (line, ".so ", 4) == 0)
    retval = (FLAG_UNFORMATTED | FLAG_SOELIM);
  else if (line[0] != '.' && line[0] != '\'')
    retval = FLAG_FORMATTED;
  else
    retval = FLAG_UNFORMATTED;

  if (fclose (fp))
    retval = FLAG_CANT_OPEN;	/* CANT_OPEN is a misnomer, actually */
  return retval;
}

int
isadir(char *fn)
{
#if defined(__WIN32__)
  /* Windows runtime doesn't support D_OK.  */
  DWORD attrs = GetFileAttributes (fn);
  return (attrs != INVALID_FILE_ATTRIBUTES
	  && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
  return (access (fn, D_OK) == 0);
#endif
}

char dir_pattern1[5], dir_pattern2[5];

/*  Find all man page files in directory DIR and, if RECURSE_OK is
    set, in its first-level subdirectories man* and cat*.
    Returns the number of found pages, or -1 in case of fatal errors.  */
int
try_directory (const char *dir, const char *file_pattern, int recurse_ok)
{
  size_t dirlen = strlen (dir);
  DIR *dp = opendir (dir);
  struct dirent *de;
  int found = 0;
  char cat_name[PATH_MAX];
  int try_cat_dir = 0;
  char entry_name[PATH_MAX];

  if (!dp)
    {
      if (verbose_option)
	fprintf (stderr, "%s: cannot look inside %s: %s\n",
		 progname, dir, strerror (errno));
      return -1;
    }

  if (debugging_output)
    fprintf (stderr, "Looking in `%s' for `%s'\n", dir, file_pattern);

  if (!recurse_ok
#ifdef __TURBOC__
      && strnicmp (dir + dirlen - 5, "/man", 4) == 0
#else
      && strncmp (dir + dirlen - 5, "/man", 4) == 0
#endif
      && strchr (section_letters, dir[dirlen-1]))
    {
      try_cat_dir = 1;
      /* Create a name of the sibling catN directory.  */
      memcpy (cat_name, dir, dirlen - 4);
      memcpy (cat_name + dirlen - 4, "cat", 3);
      cat_name[dirlen - 1] = dir[dirlen - 1];
      cat_name[dirlen] = '/';
    }

  strcat (strcpy (entry_name, dir), "/");
  while ((de = readdir (dp)) != 0)
    {
      strcpy (entry_name + dirlen + 1, de->d_name);

      /* If found a subdirectory like manN or catN, recurse into it.  */
      if (recurse_ok &&
#ifdef __TURBOC__
	  (de->d_name[0] == 'm' || de->d_name[0] == 'c'
	   || de->d_name[0] == 'M' || de->d_name[0] == 'C')
#else
	  (de->d_name[0] == 'm' || de->d_name[0] == 'c')
#endif
	  && (fnmatch (dir_pattern1, de->d_name, 0) == 0
	      || fnmatch (dir_pattern2, de->d_name, MATCHFLAGS) == 0))
	{
	  if (isadir (entry_name))
	    {
	      if (debugging_output)
		fprintf (stderr, "`%s': a directory, recursing\n", entry_name);
	      found += try_directory (entry_name, file_pattern, 0);
	    }
	}
      else if (fnmatch (file_pattern, de->d_name, MATCHFLAGS) == 0)
	{
	  char *full_name;
	  Man_page *page;

	  /* If a file by the same name exists in a sibling catN directory,
	     don't add the file from manN directory to the list, because
	     the formatted file from the catN directory will be used.  */
	  if (try_cat_dir)
	    {
	      strcpy (cat_name + dirlen + 1, de->d_name);
	      if (access (cat_name, R_OK) == 0 && isadir (cat_name))
		{
		  if (debugging_output)
		    fprintf (stderr,
			     "`%s': rejected (formatted version found)\n",
			     entry_name);
		  continue;
		}
	    }

#ifdef __TURBOC__
	  full_name = (char *)xmalloc (dirlen + strlen(de->d_name) + 2);
#else
	  full_name = (char *)xmalloc (dirlen + de->d_namlen + 2);
#endif
	  page  = (Man_page *)xmalloc (sizeof (Man_page));

	  if (debugging_output)
	    fprintf (stderr, "`%s': accepted\n", entry_name);
	  strcpy (full_name, entry_name);
	  page->path = full_name;
	  page->name = full_name + dirlen + 1;
	  page->section = set_section (page->name);
	  page->flags = set_flags (full_name);
	  found++;
	  add_page (page);
	}
    }
  closedir (dp);
  return found;
}

int
find_pages (const char *section, const char *name)
{
  char this_dir[FILENAME_MAX], file_pattern[FILENAME_MAX];
  char base[FILENAME_MAX], ext[10];
  const char *dstart, *dend;
  int found_pages = 0;
  size_t namelen = strlen (name);
  size_t extlen = 0;
#ifdef MSDOS
  int truncate_long_names = 1;
#else  /* not MSDOS */
  int truncate_long_names = 0;
#endif /* not MSDOS */

  /* Look in either "manN" and "catN" or "man?" and "cat?" subdirs.  */
  dir_pattern1[3] = dir_pattern2[3] = (*section == '*' ? '?' :  *section);

  /* Generate the pattern "name.section"  */
  memcpy (base, name, namelen + 1);
  ext[extlen++] = '.';

  /* "foo" + "3"  -> "foo.3*"
     "foo" + "3v" -> "foo.3v"
     "foo" + "new" -> "foo.[1-9]?"
     "foo" + "*"  -> "foo.[!iz]*"  (so we don't find .info and .zip files)  */
  if (isdigit (*section))
    {
      ext[extlen++] = *section++;
      if (*section)
	ext[extlen++] = *section;
      else
	ext[extlen++] = '*';
      ext[extlen++] = '\0';
    }
  else if (strchr (section_letters, *section))
    strcpy (ext + extlen, "[1-9]?");
  else
    strcpy (ext + extlen, "[!iz]*");

  /* Try each directory in MANPATH.  */
  for (dstart = manpath; dstart; dstart = dend)
    {
      this_dir[0] = '\0';	/* so we could use `strncat' */
      if (*dstart == PATH_SEP)
	dstart++;
      dend = strchr (dstart, PATH_SEP);
      if (dend)
	/* `strncat' frees us from worrying about terminating null char.  */
	strncat (this_dir, dstart, dend - dstart);
      else if (strlen (dstart) > 0)
	strcpy (this_dir, dstart);

      if (this_dir[0])
	{
	  int this_found;

#ifdef __DJGPP__
	  /* DJGPP's support of long file names depends on whether the
	     filesystem where THIS_DIR resides supports long names.  */
	  truncate_long_names = !_use_lfn (this_dir);
#endif
	  /* If NAME is longer than 8 characters, we will never find it using
	     `fnmatch' if file names are truncated by the filesystem.  We
	     need to truncate the topic name as well.  */
	  if (truncate_long_names && namelen > 8)
	    {
	      file_pattern[0] = '\0';
	      strncat (file_pattern, base, 8);
	    }
	  else
	    strcpy (file_pattern, base);

	  strcat (file_pattern, ext);

	  this_found = try_directory (this_dir, file_pattern, 1);
	  if (this_found > 0)
	    found_pages += this_found;
	}
    }
  return found_pages;
}

/* Given a page with its formatting flags, build a Groff command line
   to format that page.  */
char *
build_formatter_cmd (const Man_page *page)
{
  static char fmt_cmd[FILENAME_MAX + 3*5 + 1]; /* 5: known preprocessors */
  unsigned flags = page->flags;

  if (flags & FLAG_CANT_OPEN)	/* file couldn't be accessed */
    {
      fprintf (stderr, "%s: Impossible: %s is inaccessible\n",
	       progname, page->path);
      exit (3);
    }
  else if (flags & FLAG_FORMATTED)
    return (char *)0;	/* no formatter required */

  strcpy (fmt_cmd, groff);

  if (flags & FLAG_SOELIM)
    strcat (fmt_cmd, " -s");	/* I think Groff doesn't need this... */
  if (flags & FLAG_EQN)
    strcat (fmt_cmd, " -e");
  if (flags & FLAG_REFER)
    strcat (fmt_cmd, " -R");
  if (flags & FLAG_TBL)
    strcat (fmt_cmd, " -t");
  if (flags & FLAG_VGRIND)
    fprintf (stderr, "%s: vgrind preprocessing not supported!\n",
	     progname);

  return fmt_cmd;
}

/* Display man page(s) for a topic NAME in section SECTION.  */
int
man_entry (const char *section, const char *name)
{
  int count = find_pages (section, name);

  if (count > 0)
    {
      if (count > 1)
	/*  Strictly speaking, we don't need to sort the pages, but doing so
	    makes the ``first'' page (displayed by default) predictable.  */
	sort_pages ();

      while (count--)
	{
	  /* Examining the list from the end makes removing the
	     pages easier.  The list is sorted in descending order.  */
	  Man_page *page = pages[next_slot - 1];

	  if (list_all_option || list_fpaths_option)
	    list_page (page);
	  else
	    {
	      char *curdir = NULL, *formatter_cmd = NULL;
	      char man_dir[PATH_MAX];

	      formatter_cmd = build_formatter_cmd (page);
	      if (formatter_cmd
		  && (page->path[0] != '.'  /* not relative */
		      || (page->path[0]
			  && IS_DIR_SEP (page->path[1]))))
		{
		  size_t mandir_len = page->name - page->path - 1;
		  /* We need to chdir into the root of the manual page
		     directory subtree, because .so directives name
		     files relative to that.  */
		  curdir = getcwd (0, PATH_MAX);
		  memcpy (man_dir, page->path, mandir_len);
		  man_dir[mandir_len] = '\0'; /* dirname */

		  /* If the pathname includes "/catN" or "/manN", exclude
		     that from the directory where we are going.  */
		  if ((strncmp (man_dir + mandir_len - 5, "/cat", 4) == 0
		       || strncmp (man_dir + mandir_len - 5, "/man", 4) == 0)
		      && strchr (section_letters, man_dir[mandir_len - 1]))
		    man_dir[mandir_len - 5] = '\0';
		  if (debugging_output)
		    fprintf (stderr, "Chdir to `%s'\n", man_dir);
		  if (chdir (man_dir))
		    {
		      if (verbose_option)
			fprintf (stderr, "%s: cannot chdir to %s: %s\n",
				 progname, man_dir, strerror (errno));
		    }
		  else
		    {
		      display_page (page->path, formatter_cmd);
		      chdir (curdir); /* return to original directory */
		      if (curdir)
			free (curdir);
		    }
		}
	      else
		display_page (page->path, formatter_cmd);

	      if (!show_all_option)
		break;
	    }
	  /* Now delete the page, so if we have another topic on the
	     command line, the pages from this topic won't be considered.  */
	  remove_page (next_slot - 1);
	}
      return 0;
    }
  else
    {
      printf ("No manual entry for %s%s%s.\n",
	      name, *section == '*' ? "" : " in section(s) ",
	      *section == '*' ? "" : section);
      return 2;
    }
}

int
usage (void)
{
  printf ("\t\tman version %s\n\n", version);
  printf ("`man' finds and displays documentation from manual pages.\n\
\n\
Usage:\tman [-] [-al] [-M path] [[-s] section] topic ...\n\
\n\
If no options are given, looks for a manual page which describes TOPIC\n\
in directories specified by MANPATH environment variable and displays\n\
that page.  If the standard output is not a terminal, or if the `-'\n\
option is given, `man' writes the manual page(s) to standard output;\n\
otherwise, it pipes the page(s) through a program defined by the PAGER\n\
environment variable, to allow browsing and bold/underlined words to be\n\
displayed on the screen.\n\
\n\
MANPATH is a list of directories separated by `%c'.  Each directory in\n\
this list is assumed to contain either manual pages (files) or\n\
subdirectories named `manN' or `catN' with manual pages.  `man' does NOT\n\
look more than one level deep into the subdirectories.\n\
\n\
The command to display pages is `%s'.\n\
The path to look for man pages is:\n\
\n\
   `%s'\n\
\n\
  Options:\n\
\n\
  -          Write the output to standard output, even if it is a terminal.\n\
             By default, when stdout is a terminal, `man' pipes its output\n\
             through a pager program.\n\
\n\
  -a         Display all manual pages which match TOPIC.\n\
             By default, `man' displays the first page it finds.\n\
\n\
  -l         List all the manual pages which match TOPIC, but don't display\n\
             them.  Each page is listed together with the -M argument which,\n\
             if used, will cause `man' display that page alone.\n\
\n\
  -W         Print the full paths of each man page that is found, in order.\n\
\n\
  -M path    Specifies an alternate search path for manual pages.  PATH is\n\
             a list of directories separated by `%c', just like the value\n\
             of MANPATH.  This option overrides the MANPATH environment\n\
             variable.\n\
\n\
  -s section Specifies the section of the manual for `man' to search.  `man'\n\
             will only display pages from these sections.  SECTION may be\n\
             a digit perhaps followed by a letter, a letter from the set\n\
             [onlp], or one of the words `old', `new', `local' or `public'.\n\
             You cannot specify more than a single section at a time, but\n\
             you can specify multiple -s options (\"-s 1 foo -s 5 foo\").\n\
\n\
             A section name in short form only (1v, n, etc.) can also be\n\
	     given without preceeding it with the -s switch.\n\
\n\
  -v         Causes `man' to print messages about non-fatal errors it\n\
             encounters during the run.\n\
\n\
  -d         Causes `man' to display debugging trace of its run.\n\n",
	  PATH_SEP, pager, manpath, PATH_SEP);
  return 1;
}

int
main (int argc, char *argv[])
{
  char *use_pager = getenv ("PAGER");
  char *use_manpath = getenv ("MANPATH");

  if (use_pager)
    pager = use_pager;
  if (use_manpath)
    manpath = use_manpath;
#ifdef __DJGPP__
  else
    {
      char *djdir = getenv ("DJDIR");

      if (djdir && __file_exists (djdir))
	{
	  char *djmanpath = (char *)xmalloc (2*strlen (djdir) + 10 + 1);

	  strcat (strcpy (djmanpath, djdir), "/man;");
	  strcat (strcat (djmanpath, djdir), "/info");
	  manpath = djmanpath;
	}
    }
#endif
  progname = argv[0];
  if (argc == 1)
    return usage ();
  else
    {
      char *section = "*";
      int status = 0;
      int last_arg_was_section = 0;

      strcpy (dir_pattern1, "man?");
      strcpy (dir_pattern2, "cat?");
      if (!isatty (fileno (stdout)))
	direct_output = 1;

      while (--argc)
	{
	  char *arg = *++argv;

	  if (arg[0] == '-')
	    {
	      switch (arg[1])
		{
		  case '\0':
		    direct_output = 1;
		    break;
		  case 'a':
		    show_all_option = 1;
		    break;
		  case 'l':
		    list_all_option = 1;
		    break;
          case 'W':
            list_fpaths_option = 1;
            break;
		  case 'v':
		    verbose_option = 1;
		    break;
		  case 'd':
		    debugging_output = 1;
		    fprintf (stderr, "man version %s: debugging output ON\n",
			     version);
		    break;
		  case 'M':
		    if (argc <= 0)
		      {
			fprintf (stderr, "%s: missing argument to -M\n",
				 progname);
			return 2;
		      }
		    manpath = argv[1];
		    --argc; ++argv;
		    break;
		  case 's':
		    /* FIXME: handle multiple sections like "-s 3,5,n" etc.  */
		    if (argc <= 0)
		      {
			fprintf (stderr, "%s: missing argument to -s\n",
				 progname);
			return 2;
		      }
		    section = argv[1];
		    /* Better ignore unsupported feature than crash.  */
		    if (section[1] == ',')
		      section[1] = '\0';
		    --argc; ++argv;

		    /* Remember the last arg seen was a section name.  */
		    last_arg_was_section = 1;
		    break;
		  default:
		    last_arg_was_section = 0;
		    status |= man_entry (section, arg);
		}
	    }
	  /* Treat digit arguments, single letters and reserved words
	     as section specifications, even without explicit -s.  */
	  else if ((strchr (section_letters, arg[0]) && !arg[1])
		   || (isdigit (arg[0]) && arg[1] && !arg[2])
		   || !strcmp (arg, "old") || !strcmp (arg, "new")
		   || !strcmp (arg, "local") || !strcmp (arg, "public"))
	    {
	      section = arg;
	      last_arg_was_section = 1;
	    }
	  else
	    {
	      last_arg_was_section = 0;
	      status |= man_entry (section, arg);
	    }
	}
      if (pages)
	free (pages);
      if (last_arg_was_section)
	{
	  fprintf (stderr, "But what do you want from section `%s'?\n",
		   section);
	  return 1;
	}
      return status;
    }
}
