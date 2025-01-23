# ScriptFS
(This fork of [a previous
ScriptFS](https://github.com/frodonh/scriptfs) has been updated to
work with recent versions of Linux.)

ScriptFS allows script output to appear as ordinary, readable
files. It's similar to input process substitution (`<(...)`), but it's
launched transparently and appears in a virtual filesystem with a
filename, and lives until it is closed.

In a nutshell, it allows ordinary file content to be generated
on-the-fly (like a web page) such that other programs can read it and
won't even notice! For example, you can use ScriptFS to generate
system configuration files from a database. Output is temporarily
stored in RAM for security and seekability (unlike pipes).

ScriptFS can also apply a script to all or select files in a
subdirectory, like a filter.

ScriptFS can work with any executable script or program. It uses the
Linux FUSE3 virtual filesystem interface, and does not require root
permissions to use. ScriptFS is designed for Linux.

This release has been updated to work with the Linux 4.2+ kernels. It
has been tested under Ubuntu Linux 24.04.

## Concepts

Understanding how to use ScriptFS requires knowing a few key concepts.
ScriptFS works by "mirroring" a existing, populated subdirectory into
a FUSE "mount point". (The mount point is simply an empty directory
where you want the script output to appear after invocation.) If you
then list the two directories, they will have the same filenames (but
will behave differently!).

In `auto` mode, when you attempt to read the files from the mount
point, files ScriptFS identifies as scripts, it executes, and the
contents of the filename listed in the mount point will then contain
the output of the script with the corresponding name in the mirror
subdirectory.

Instead of `auto`, you can also specify a `program` and an optional
`test` script. The `test` script is applied to each file upon access;
if it returns true (exit code 0), the file in the mirror directory is
piped through `program` and the output appears in the mount point
under the same name.

Unlike in prior releases, the mount point must be empty; this is due
to changes in how `fuse3` works.

## Setup
### Manual using Makefile

Building ScriptFS requires appropriate C buildtools (`make` and `gcc`)
as well as `fuse3` and `libfuse3-dev` to be installed.

`make`

## Usage
### Command line

The virtual filesystem is mounted with the following command: 
`scriptfs [options] mirror_path mountpoint`

#### Mandatory arguments

`mirror_path`

Path of the original folder which will be replicated in the virtual file system

`mountpoint`

Path of the mount point folder. The folder must be empty before using
the command, otherwise the command will fail.

#### Optional arguments

Optional arguments are used to identify scripts and what to do with
them. Two types of executable files should be provided:

*   **Program files** specify the program that to be called on each
    identified script file to process its contents;
*   **Test files** are executable files that identify which files are
    scripts; this gives the user flexibility for selecting files to
    execute.

For each identified script file, the program file is executed and its
standard output is captured and appears as the content of the virtual
file with the same name as the executed script. Standard error is left
unchanged and written to standard error for logging purposes (you will
need to pass the `-f` or "foreground" fuse3 option to see them).

`-p program[;test]`

Specify an executable filter program and a corresponding test program
to use. The command may be repeated several times to specify
additional executable programs, in which case processing stops with
the first `program` executed (i.e. the first `test` that positively
identifies a script).

`program` can be either of the following:

*   A full command line. If `program` is a full shell command-line
    (starting with the name of an executable program, with arguments),
    the corresponding program (identified by the first word on the
    command line) is invoked on each script file, as identified by the
    `test` program. All supplied arguments are passed to it. If the
    command-line contains the "!" character, it is replaced by the
    name of the script file. If no such character is found, the
    content of the script file is provided as the standard input of
    the external program.
*   `auto`. When the `auto` string is specified, any script starting
    with a shebang `#!`) or executable program is invoked. No `test`
    program has to be provided. If the file is a shell script, the
    string after `#!` defines the path of the executable program that
    will be launched to execute the content of the script.

`test` is optional, and can be one of the following:

*   A full command line. This command-line is invoked on each file to
    determine if it is a script. As in the `program` command line, all
    supplied arguments are passed. If the command-line contains the
    "!" character, it is replaced by the name of the file. Otherwise,
    the content of the file is provided as the standard input of the
    test program. The standard output of the test program is
    discarded. Since the test program is executed on every file on the
    filesystem, it must be fast. A file is identified as a script if
    the exit code of the test program is zero (normal
    exit). Otherwise, it is not considered a script.
*   `always`. When the `always` string is specified, all the files in
    the virtual file system are considered script files.
*   `executable`. When the `executable` string is found, only files
    that the current user can execute are treated as script files.
*   Pattern. A pattern is a POSIX case-sensitive basic regular
    expression which starts with the '&' character. The full name of
    the file (including the path) is tested against the pattern and if
    it matches, the file is identified as a script file.

If no test procedure is provided and the program procedure is a full
command-line, the same command-line will be used for the test
program. Thus every file will first be executed to detect if they
should be regarded as script files. If the program procedure is
`self`, and no test procedure is provided, the `executable` mode will
be used for the test procedure, and only executable files will be
considered as script files.

When no procedure (`-p`) is set, the program behaves as if `-p auto`
was specified.

`-l`

The `-l` option (for "length" or "lseek") causes any queries of the
file size of a script to run the script to ascertain and report the
actual output size, at the expense of latency and an idempotency
requirement (see [Caveats](#caveats)). This also makes seeking work as
expected (if your application does that). Otherwise, the size of the
source script is reported.

`-f`

The `-f` option (which is a FUSE option, not a ScriptFS option) puts
the driver in the foreground so you can see the error messages (and
trace messages, if you did a `make TRACE=1` in building). Ctrl-C
terminates and unmounts. Also, `-d` (debug), which implies `-f`.

# Caveats

The tool looks for a ramdisk in `/dev/shm` to store output temporarily
(this ensure security); if it doesn't exist, `/tmp` is used instead.

Applications (such as `ls`) that consult `stat` to get a file length
(e.g. before opening a script file) will not get a correct size by
default, because the script has not yet been opened and
executed. (Instead, `stat` will return the length of the _script_.)
This will affect any attempted seeks (`lseek()`). It will not affect
applications that simply read from beginning to EOF.  To fix this,
pass the `-l` option; when the file size is queried (including on
seeks), it will cause the script to run and its output size to be
calculated and returned, discarding the output. Note that depending on
the varigations of your script, when the file is subsequently opened,
the contents and size may have changed in the meantime (no caching is
done). If you use this option, scripts should be idempotent for best
results. See the examples.

`test` programs may be executed under unexpected circumstances
(e.g. on an `ls` or any `stat`-related call). Furthermore, remember
that `-p prog` is the same as `-p prog;prog`, so `prog` in this
scenario will be executed as a `test` program. This is why you want to
avoid scripts (in this case, `test` scripts, or `program` scripts
acting as `test` scripts) having side-effects.

When a script specified on the command line as a `test` script is
called with an argument (`!`), the argument supplied is the name of
the file in the virtual filesystem, relative to the filesystem mount
point. When a `program` script is called, the argument is the name of
the file in the temporary filesystem containing the original contents
of the file (from the mirror subdirectory).

If you get a "Transport end not connected" error when accessing
virtual files (probably due to a crash), use `fusermount -u
mount_point` to fix this and re-mount.

# Examples

## Example #1; simple script substitution

We have three files of interest:

* `hello_script`: A `bash` script, whose standard output will appear in the mount point
* `hello_c`/`hello.c`: An executable C program, whose standard output will appear in the mount point
* `hello_text`: A plain text file, which will appear verbatim

The files in the mirror directory, and the empty mount point:
```
$ ls -l examples/test
total 28
-rw-rw-r-- 1 eewanco eewanco    63 Jan 22 11:58 hello.c
-rwxrwxr-x 1 eewanco eewanco 15960 Jan 22 11:58 hello_c
-rwxr-xr-x 1 eewanco eewanco    38 Jan 22 14:55 hello_script
-rw-rw-r-- 1 eewanco eewanco    16 Jan 22 11:57 hello_text
$ ls -R /tmp/scriptfs
/tmp/scriptfs:
$
```
Now we do the mount, using the `auto` program option, with
`examples/test` as the mirror directory and `/tmp/scriptfs` as the mount
point.
```
$ ./scriptfs -p auto examples/test /tmp/scriptfs
$ ls -R /tmp/scriptfs
/tmp/scriptfs:
hello.c  hello_c  hello_script  hello_text
```

Note that the mount point now mirrors `examples/test`.

```
$ cat examples/test/hello_script
#!/bin/bash

echo "Hello, from bash"
$ cat /tmp/scriptfs/hello_script
Hello, from bash
```

So here the file `/tmp/scriptfs/hello_script` executes
`examples/test/hello_script` and presents the `stdout` as the same
filename.

The same is true of the C executable:
```
$ cat examples/test/hello.c
#include <stdio.h>

int main() {
  printf("hello, world\n");
}
$ cat /tmp/scriptfs/hello_c
hello, world
```
But the text file is untouched:
```
$ cat examples/test/hello_text 
Hello everybody
$ cat /tmp/scriptfs/hello_text 
Hello everybody
$ 
```
## Example #2; program as a filter
You can apply a specific program (filter) to some or all of the files
in the mirror directory. The content of each read file will be applied
to the standard input of the filter, and the contents of the file in
the mount point will contain the output.

```
$ cat examples/test/filter
#!/bin/bash

awk -F "\n" '{print FNR,$0;}' $1
$ ./scriptfs -p examples/test/filter examples/test /tmp/scriptfs
$ cat /tmp/scriptfs/hello_text
1 Hello everybody
$ cat /tmp/scriptfs/hello_script
1 #!/bin/bash
2 
3 echo "Hello, from bash"
$ fusermount -u /tmp/scriptfs
```
## Example #3; filter arguments
As the [Caveat](#caveat) section explains, when passing a filename
argument (`!`) to a script, the behavior varies depending on whether
the script is running as a `test` or as a `program`. Recall that the
syntax of `-p` is `program;test` and that if only `program` is listed,
it is _also_ used as a test, returning success if the file in the
virtual filesystem is a script to process, and failure if it should
not be processed. Let's illustrate this.

First, let's show the simple `-p program` format, where `program` has an `!` argument:
```
$ cat examples/test/args
#!/bin/bash

echo I was passed: "$@"
echo "$(date) $1" >> /tmp/args.log
$ rm -f /tmp/args.log
$ ls -lB examples/test
total 44
-rwxr-xr-x 1 eewanco eewanco    72 Jan 24 12:48 args
-rwxr-xr-x 1 eewanco eewanco    46 Jan 23 15:05 filter
-rw-rw-r-- 1 eewanco eewanco    63 Jan 22 11:58 hello.c
-rwxrwxr-x 1 eewanco eewanco 15960 Jan 22 11:58 hello_c
-rwxr-xr-x 1 eewanco eewanco    37 Jan 22 15:13 hello_script
-rw-rw-r-- 1 eewanco eewanco    16 Jan 22 11:57 hello_text
-rw-rw-r-- 1 eewanco eewanco    23 Jan 22 16:46 seq
drwxrwxr-x 2 eewanco docker   4096 Jan 24 12:20 subdir
$ ./scriptfs -p $PWD/'examples/test/args !' examples/test /tmp/scriptfs
$ ls -lB /tmp/scriptfs
total 44
-r-xr-xr-x 1 eewanco eewanco    72 Jan 24 12:48 args
-r-xr-xr-x 1 eewanco eewanco    46 Jan 23 15:05 filter
-r--r--r-- 1 eewanco eewanco    63 Jan 22 11:58 hello.c
-r-xr-xr-x 1 eewanco eewanco 15960 Jan 22 11:58 hello_c
-r-xr-xr-x 1 eewanco eewanco    37 Jan 22 15:13 hello_script
-r--r--r-- 1 eewanco eewanco    16 Jan 22 11:57 hello_text
-r--r--r-- 1 eewanco eewanco    23 Jan 22 16:46 seq
drwxrwxr-x 2 eewanco docker   4096 Jan 24 12:20 subdir
$ cat /tmp/args.log
Fri Jan 24 12:53:52 PM EST 2025 hello.c
Fri Jan 24 12:53:52 PM EST 2025 hello_c
Fri Jan 24 12:53:52 PM EST 2025 hello_script
Fri Jan 24 12:53:52 PM EST 2025 filter
Fri Jan 24 12:53:52 PM EST 2025 hello_text
Fri Jan 24 12:53:52 PM EST 2025 args
Fri Jan 24 12:53:52 PM EST 2025 seq
```

This illustrates the non-obvious principle that the test script is
called and passed the virtual filename on an `ls` (or any `stat`
operation) on a _file_ (note that the directory is skipped). Now let's
show that the temporary file is passed instead of the virtual filename
when the program is executed as a `program` rather than an (implicit)
`test`:

```
$ cat /tmp/scriptfs/hello_text
I was passed: /dev/shm/sfs.jWTgLl
$ cat /tmp/args.log
Fri Jan 24 12:53:52 PM EST 2025 hello.c
Fri Jan 24 12:53:52 PM EST 2025 hello_c
Fri Jan 24 12:53:52 PM EST 2025 hello_script
Fri Jan 24 12:53:52 PM EST 2025 filter
Fri Jan 24 12:53:52 PM EST 2025 hello_text
Fri Jan 24 12:53:52 PM EST 2025 args
Fri Jan 24 12:53:52 PM EST 2025 seq
Fri Jan 24 12:54:16 PM EST 2025 hello_text
Fri Jan 24 12:54:16 PM EST 2025 hello_text
Fri Jan 24 12:54:16 PM EST 2025 /dev/shm/sfs.jWTgLl
$ cat /tmp/scriptfs/hello_script
I was passed: /dev/shm/sfs.n566BM
$ fusermount -u /tmp/scriptfs
```

If we specify an explicit `test` program that returns success, the
filter will be applied to every file (and will not be executed for
each test) (`/bin/true` is functionally the same as the keyword
`always`):

```
$ rm -f /tmp/args.log
$ ./scriptfs -p 'examples/test/args !;/bin/true' examples/test /tmp/scriptfs
$ ls -lB /tmp/scriptfs
total 44
-r-xr-xr-x 1 eewanco eewanco    72 Jan 24 12:48 args
-r-xr-xr-x 1 eewanco eewanco    46 Jan 23 15:05 filter
-r--r--r-- 1 eewanco eewanco    63 Jan 22 11:58 hello.c
-r-xr-xr-x 1 eewanco eewanco 15960 Jan 22 11:58 hello_c
-r-xr-xr-x 1 eewanco eewanco    37 Jan 22 15:13 hello_script
-r--r--r-- 1 eewanco eewanco    16 Jan 22 11:57 hello_text
-r--r--r-- 1 eewanco eewanco    23 Jan 22 16:46 seq
drwxrwxr-x 2 eewanco docker   4096 Jan 24 12:20 subdir
$ cat /tmp/args.log
cat: /tmp/args.log: No such file or directory
$ cat /tmp/scriptfs/hello_text
I was passed: /dev/shm/sfs.TuOg8T
$ cat /tmp/args.log
Fri Jan 24 01:22:14 PM EST 2025 /dev/shm/sfs.TuOg8T
$ fusermount -u /tmp/scriptfs
```

If the `test` program always fails, all file content is verbatim:

```
$ rm -f /tmp/args.log
$ ./scriptfs -p 'examples/test/args !;/bin/false' examples/test /tmp/scriptfs
$ cat /tmp/scriptfs/hello_text
Hello everybody
$ fusermount -u /tmp/scriptfs
$ cat /tmp/args.log
cat: /tmp/args.log: No such file or directory
```

Let's come full circle and use `/bin/true` as the `program` and `args`
as the `test`. `args` will always succeed (return an exit code of 0)
and `/bin/true` will run as a filter on each file. Since `/bin/true`
never outputs anything or reads anything from `stdin`, each file will
have no content:

```
$ ./scriptfs -p '/bin/true;examples/test/args !' examples/test /tmp/scriptfs
$ ls -B /tmp/scriptfs
args  filter  hello.c  hello_c  hello_script  hello_text  seq  subdir
$ cat /tmp/args.log
Fri Jan 24 01:28:46 PM EST 2025 hello.c
Fri Jan 24 01:28:46 PM EST 2025 hello_c
Fri Jan 24 01:28:46 PM EST 2025 hello_script
Fri Jan 24 01:28:46 PM EST 2025 filter
Fri Jan 24 01:28:46 PM EST 2025 hello_text
Fri Jan 24 01:28:46 PM EST 2025 args
Fri Jan 24 01:28:46 PM EST 2025 seq
$ cat /tmp/scriptfs/hello_script
$ cat /tmp/scriptfs/hello_text
$ fusermount -u /tmp/scriptfs
```

## Example #4; regex matching

```
$ ls -l examples/test/subdir/
total 40
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_1
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_2
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_3
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_4
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_5
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_6
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_7
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_8
-rw-rw-r-- 1 eewanco eewanco  2 Jan 24 13:35 file_9
-rw-rw-r-- 1 eewanco docker  21 Jan 24 12:20 foo.txt
$ cat examples/test/subdir/file_?
1
2
3
4
5
6
7
8
9
$ ./scriptfs -p '/bin/echo !;&file_[0-4]' examples/test /tmp/scriptfs
$ cat /tmp/scriptfs/subdir/file_?
/dev/shm/sfs.arS8hN
/dev/shm/sfs.VIAJRJ
/dev/shm/sfs.Fq3Nll
/dev/shm/sfs.I6gy1l
5
6
7
8
9
$ fusermount -u /tmp/scriptfs
```

## Example #5; seek and you shall find
You can choose two modes for reporting file sizes (which affects the
success of seeks, which some, but not all, applications may
attempt). The default mode does not run the script when the size is
queried, but returns the size of the script. This sounds pretty
useless, but it prevents scripts with side-effects from going awry,
and it does slice off some time. Or you can specify the `-l` option,
which runs the script, calculates the length of its output, and
discards the output, returning the length. The difference is
illustrated below.

```
$ mkdir /tmp/scriptfs
$ ./scriptfs examples/test /tmp/scriptfs
$ cat examples/test/seq
#!/bin/bash

seq 0 255
$ tail -256 /tmp/scriptfs/seq | wc -c
23
$ tail -3 /tmp/scriptfs/seq
8
9
10
$ cat /tmp/scriptfs/seq | tail -3
253
254
255
$ ls -l examples/test/seq /tmp/scriptfs/seq
-rw-rw-r-- 1 eewanco eewanco 23 Jan 22 16:46 examples/test/seq
-r--r--r-- 1 eewanco eewanco 23 Jan 22 16:46 /tmp/scriptfs/seq
```
Note that `tail`, when using an offset from the end, only sees 23 bytes, the length of the script.
```
$ cat /tmp/scriptfs/seq | wc -c
914
$ fusermount -u /tmp/scriptfs
```
... but there are 914 bytes in the output.

Now let's remount using `-l`.
```
$ ./scriptfs -l examples/test /tmp/scriptfs
$ tail -3 /tmp/scriptfs/seq
253
254
255
$ ls -l examples/test/seq /tmp/scriptfs/seq
-rw-rw-r-- 1 eewanco eewanco  23 Jan 22 16:46 examples/test/seq
-r--r--r-- 1 eewanco eewanco 914 Jan 22 16:46 /tmp/scriptfs/seq
$ fusermount -u /tmp/scriptfs
```
`tail` behaves as expected and the length reported is the length of the output.

## Example #6; mutual mirroring
Files written to the mount point are mirrored to the mirror directory:
```
$ ./scriptfs -l examples/test /tmp/scriptfs
$ echo "Goodbye" > /tmp/scriptfs/goodbye
$ ls -l /tmp/scriptfs examples/test/
examples/test/:
total 36
-rw-rw-r-- 1 eewanco eewanco     8 Jan 22 15:23 goodbye
-rw-rw-r-- 1 eewanco eewanco    63 Jan 22 11:58 hello.c
-rwxrwxr-x 1 eewanco eewanco 15960 Jan 22 11:58 hello_c
-rwxr-xr-x 1 eewanco eewanco    37 Jan 22 15:13 hello_script
-rw-rw-r-- 1 eewanco eewanco    16 Jan 22 11:57 hello_text

/tmp/scriptfs:
total 36
-rw-rw-r-- 1 eewanco eewanco     8 Jan 22 15:23 goodbye
-rw-rw-r-- 1 eewanco eewanco    63 Jan 22 11:58 hello.c
-rwxrwxr-x 1 eewanco eewanco 15960 Jan 22 11:58 hello_c
-rwxr-xr-x 1 eewanco eewanco    37 Jan 22 15:13 hello_script
-rw-rw-r-- 1 eewanco eewanco    16 Jan 22 11:57 hello_text
$ rm examples/test/goodbye
$ ls -l /tmp/scriptfs
total 32
-rw-rw-r-- 1 eewanco eewanco    63 Jan 22 11:58 hello.c
-rwxrwxr-x 1 eewanco eewanco 15960 Jan 22 11:58 hello_c
-rwxr-xr-x 1 eewanco eewanco    37 Jan 22 15:13 hello_script
-rw-rw-r-- 1 eewanco eewanco    16 Jan 22 11:57 hello_text
$ fusermount -u /tmp/scriptfs
```
