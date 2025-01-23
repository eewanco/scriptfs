/*
 * =====================================================================================
 *
 *       Filename:  operations.c
 *
 *    Description:  Implementation of operations on script files
 *
 *        Version:  2.0
 *        Created:  01/25/2025
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Eric Ewanco (2.0) François Hissel (1.0)
 *        Company:  
 *
 * =====================================================================================
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "procedures.h"
#include "operations.h"

/********************************************/
/*         DATA TYPES AND FUNCTIONS         */
/********************************************/

struct Persistent persistent;

void init_resources() {
	persistent.mirror=0;
	persistent.mirror_len=0;
	persistent.procs=0;
}

void free_resources() {
	free(persistent.mirror);
	free_procedures(persistent.procs);
}

/********************************************/
/*             COMMON FUNCTIONS             */
/********************************************/
/**
 * \brief Make a temporary copy of a file
 *
 * The function tries to copy the file in argument in a temporary folder, on a random name. The file name is relative to the folder which descriptor is in the global variable persistent.mirror_fd. If it succeeds, it returns a newly-allocated string with the name of the temporary file. Otherwise, it returns 0.
 * \param file Source file which should be copied
 * \return New string with the name of the temporary file. The user is responsible for releasing the memory allocated for this string.
 */
char *temp_copy(const char *file) {
	int fin=openat(persistent.mirror_fd,file,O_RDONLY);
	if (fin==-1) return 0;
	char *res = strdup(persistent.tmp_template);
	int fout=mkstemp(res);
	if (fout==-1) {
		fprintf(stderr,"temp_copy: Failed to create temp file (%d)\n", errno);
		close(fin);
		return 0;
	}
	ssize_t num, total = 0;
	char buf[0x400];
	while ((num = read(fin, &buf, sizeof(buf))) > 0) {
		write(fout, &buf, num);
		total += num;
	}
	struct stat st;
	int code = fstat(fin, &st);
	if (code==-1) {
#ifdef TRACE
		fprintf(stderr,"temp_copy: Warning: Cannot stat mode for copy: %s\n", strerror(errno));
#endif
	} else {
		/* Otherwise any executable bit won't get transferred */
		fchmod(fout, st.st_mode & (S_IRUSR | S_IXUSR));
	}
	close(fin);
	close(fout);
	return res;
}

/********************************************/
/*              TEST FUNCTIONS              */
/********************************************/
int test_true(PTest test,const char *file) {return 1;}
int test_false(PTest test,const char *file) {return 0;}

int test_shell(PTest test,const char *file) {
	int fd=openat(persistent.mirror_fd,file,O_RDONLY);
	FILE *f=fdopen(fd,"r");
	if (f==0) return 0;
	char magic[2];
	size_t s=fread(magic,1,2,f);
	close(fd);
	if (s<2 || magic[0]!='#' || magic[1]!='!') return 0;
	return 1;
}

int test_executable(PTest test,const char *file) {
	return faccessat(persistent.mirror_fd,file,X_OK,0)==0;
}

int test_shell_executable(PTest test,const char *file) {
	return test_shell(test,file) || test_executable(test,file);
}

int test_pattern(PTest test,const char *file) {
	if (test->compiled==0) return 0;
	return regexec(test->compiled,file,0,0,0)==0;
}

int test_program(PTest test,const char *file) {
	// Create the array of arguments of the program by replacing the exclamation mark with the name of the file
	if (test->args!=0 && test->filearg!=0) *(test->filearg)=(char*)file;
	// If the program is a filter that requires standard input, add the name of the file in the arguments of the call to execute_program
	const char *f=(test->filter)?file:0;
	// Launch the program
	int code=execute_program(test->path,(const char**)(test->args),0,f);
	if (test->args && test->filearg)
		*(test->filearg) = strdup(""); // A hack to give it something non-zero to free() to avoid errors
	return (code==0);
}

/********************************************/
/*           EXECUTION FUNCTIONS            */
/********************************************/
int program_shell(PProgram program,const char *file,int fd) {
#ifdef TRACE
	fprintf(stderr,"program_shell(%s, %d)\n",file, fd);
#endif
	char *tmpfil=temp_copy(file);
	if (tmpfil==0) return -errno;
	const char *args[]={tmpfil,0};
	int code=execute_program(tmpfil,args,fd,0);
	unlink(tmpfil);
	free(tmpfil);
	return code;
}

int program_external(PProgram program,const char *file,int fd) {
	// Create the array of arguments of the program by replacing the exclamation mark with the name of a file with the same content
	// The actual file is not used because it may not be accessible for external programs since the host folder can be mounted over with the new file system [this is no longer true, but I am leaving the code --eje]. To prevent that case, the script file is copied in the temporary folder and this new file name is given as the argument of the external program at the location of the exclamation mark. The temporary file is deleted after the end of the procedure.
	if (program->args!=0 && program->filearg!=0) *(program->filearg)=temp_copy(file);
	// If the program is a filter that requires standard input, add the name of the file in the arguments of the call to execute_program
	const char *f=(program->filter && program->filearg==0)?file:0;
	// Launch the program
	int code=execute_program(program->path,(const char**)(program->args),fd,f);
	// Release memory, restore structure in previous state and exit
	if (program->args!=0 && program->filearg!=0) {
		if (*(program->filearg)!=0) {
			unlink(*(program->filearg));
			free(*(program->filearg));
		}
		*(program->filearg)=0;
	}
	return code;
}

/********************************************/
/*             OTHER OPERATIONS             */
/********************************************/
Procedure* get_script(const Procedures *procs,const char *file) {
#ifdef TRACE
	fprintf(stderr,"get_script(%s)\n", file);
#endif
	Procedure *res=0;
	while (res==0 && procs!=0) {
		if (procs->procedure->test!=0 && procs->procedure->test->func!=0 && procs->procedure->test->func(procs->procedure->test,file)!=0) res=procs->procedure;
		procs=procs->next;
	}
#ifdef TRACE
	fprintf(stderr,"get_script() <-- %p\n", res);
#endif
	return res;
}

void call_program(const char *file,const char **args) {
#ifdef TRACE
	fprintf(stderr,"call_program(%s)\n", file);
#endif
	// Check the nature of file
	int fd=openat(persistent.mirror_fd,file,O_RDONLY);
	if (fd <= 0) {
		// This is sort of an experimental hack. It's not clear from the original documentation,
		// but apparently filters are *supposed* to be in the mirror directory, except this didn't
		// quite work in my experience. I did some work "fixing" it (by canonicalizing filter paths)
		// except this part broke, so this hack permits absolute paths to work. --eje
		if ((fd = open(file, O_RDONLY)) <= 0) {
			fprintf(stderr, "call_program: Open of file %s failed\n", file);
			return;
		}
	}
	FILE *f=fdopen(fd,"r");
	if (f==0) return;
	char *line=0;
	size_t nn;
	ssize_t n=getline(&line,&nn,f);
	close(fd);
	if (n>=2 && line[0]=='#' && line[1]=='!') {	// file is a shell script
		// Read the path to the script interpreter
		size_t i=2;
		while (i<n && (line[i]==' ' || line[i]=='\t')) ++i;
		if (i>=n || line[i]=='\n') return;
		size_t j=i;
		while (j<n && (line[j-1]=='\\' || (line[j]!=' ' && line[j]!='\t' && line[j]!='\n'))) ++j;
		char path[j-i+1];
		strncpy(path,line+i,j-i);
		path[j-i]=0;
		free(line);
		// Prepare array of arguments
		i=0;
		const char **ar=args;
		while (*(ar++)!=0) ++i;
		const char *newargs[i+2];
		newargs[0]=path;
		j=1;
		while (j<i+2) {newargs[j]=args[j-1];++j;}
		// Launch program
		int fde=openat(persistent.mirror_fd,path,O_RDONLY);
#ifdef TRACE
		fprintf(stderr,"call_program: Executing shell script with %s\n", path);
#endif
		if (fde > 0) {
			fexecve(fde, (char* const*)newargs, persistent.envp);
		} else {
			fprintf(stderr, "call_program: Open of script %s/%s failed\n",
				persistent.mirror, path);
		}
	} else {
		int fde=openat(persistent.mirror_fd,file,O_RDONLY);
#ifdef TRACE
		fprintf(stderr, "call_program: executable file handle is %d\n", fde);
#endif
		if (fde > 0) {
			fexecve(fde, (char *const*)args, persistent.envp);
		} else {
			fprintf(stderr, "call_program: Open of executable %s/%s failed\n",
				persistent.mirror, file);
		}
	}
}

int execute_program(const char *file,const char **args,int out,const char* path_in) {
#ifdef TRACE
	fprintf(stderr,"execute_program(%s,..., %d, %s)\n", file, out, path_in);
#endif
	pid_t child;	// ID of child process executing external program
	int fds[2];	// Handles of the two ends of the pipe, only used if input has to be provided to the standard input of the external program
	int in;
	if (path_in!=0) pipe(fds);	// Prepare a pipe to feed standard input of the external program, fork and copy the file to the pipe
	child=fork();
	if (child!=0) {	// Parent process (caller)
		if (path_in!=0) {	// If a path is provided, feed the content of the file to the pipe so that it is used as the standard input of the child process
			close(fds[0]);	// Close input descriptor
			in=openat(persistent.mirror_fd,path_in,O_RDONLY);
			if (in<0) path_in=0; else {	// Copy file to standard input
				char buffer[0x400];
				ssize_t num,numw,num2;
				do {
					num=read(in, buffer, sizeof buffer);
					numw=0;
					while (numw<num) {
						num2=write(fds[1],buffer+numw,num-numw);
						if (num2<0) numw=num; else numw+=num2;
					}
				} while (num>0);
			}
			fsync(fds[1]);
			close(fds[1]);
		}
		int code;
		waitpid(child,&code,0);
		if (WIFEXITED(code)) return WEXITSTATUS(code);
	} else {	// Child process (external program)
		if (out!=0) dup2(out,STDOUT_FILENO);	// Redirect output to out descriptor
		else dup2(STDERR_FILENO,STDOUT_FILENO);	// Redirect standard output on standard error, to avoid mixing outputs from the external program and the parent process
		if (path_in==0) {
			close(STDIN_FILENO);	// We do not want the external program to use anything from the common standard input
		} else {
			close(fds[1]);	// Close output descriptor
			dup2(fds[0],STDIN_FILENO);	// Redirect standard input to pipe output
		}
		//execvp(file,(char *const *)args);
		call_program(file,args);
		fprintf(stderr,"Error '%s' calling external program : %s", strerror(errno), file);
		args++;
		while (*args!=0) fprintf(stderr," %s",*(args++));
		fprintf(stderr,"\n");
		abort();
	}
	return 1;
}
