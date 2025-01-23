/**
 * \file
 *
 * =====================================================================================
 *
 *       Filename:  scriptfs.c
 *
 *    Description:  FUSE-based file system that automatically executes scripts and
 *    							returns their output instead of the actual file content
 *
 *        Version:  2.0
 *        Created:  01/22/2025
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Eric Ewanco (2.0) François Hissel (1.0)
 *        Company:  
 *
 * =====================================================================================
 */

#define FUSE_USE_VERSION 314			//!< FUSE version on which the file system is based
#define _GNU_SOURCE

#include <stdlib.h>
#include <stddef.h>
#include <fuse3/fuse.h>
#include <fuse3/fuse_opt.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sysexits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fcntl.h>
#include "operations.h"
#include "procedures.h"

extern struct Persistent persistent;

#define SFS_OPT_KEY(t,u,p) { t ,offsetof(struct options, p ), 1 } , { u ,offsetof(struct options, p ), 1 }	//!< Generate a command-line argument with short name t, long name u. p is an integer variable name and the corresponding variable will be set to 1 if it is found in the arguments
#define SFS_OPT_KEY2(t,u,p,v) { t ,offsetof(struct options, p ), v } , { u ,offsetof(struct options, p ), v }	//!< Generate a command-line argument with short name t, long name u. p is an integer or string variable name and the corresponding variable will be set to the value of the argument

uid_t uid;	//!< Current user ID
gid_t gid;	//!< Current group ID

/**
 * \brief Display a brief help about the syntax and exit the program
 *
 * The function is called when the user supplies wrong command line parameters. It displays a brief help and exits the program.
 * \param code Error code that will be returned at the termination of the program
 */
void print_usage(int code) {
	printf("Syntax: scriptfs [arguments] mirror_folder mount_point\n");
	printf("Arguments:\n");
	printf("        -l\n\t\tReport final output size for scripts instead of size of source.\n");
	printf("	-p program[;test]\n\t\tAdd a procedure which tells what to do with files\n");
	printf("	mirror_folder\n\t\tActual folder on the disk that will be the base folder of the mounted structure\n");
	printf("	mount_point\n\t\tFolder that will be used as the mount point\n");
	exit(code);
}

/**
 * \brief Transform an absolute path in the virtual file system in a path relative to the mirror file system
 *
 * This function converts an absolute path name in the virtual mounted file system in a relative pathname in the mirror underlying file system. If the pathname is "/", it replaces it with ".". Otherwise it removes the initial slash. The function returns a new string and leaves the user program the responsibility to release it.
 * \param path Absolute path in the virtual file system
 * \return Relative path in the mirror file system
 */
char *relative_path(const char *path) {
	if (path==0 || path[0]==0) return 0;
	char *res;	
	if (path[0]=='/' && path[1]==0) {
		res=(char*)malloc(2*sizeof(char));
		res[0]='.';res[1]=0;
		return res;
	}
	res=(char*)malloc(strlen(path));
	strcpy(res,path+1);
	return res;
}

/**
 * \brief Split a string in different tokens
 *
 * This function analyses the string s and splits it in tokens delimited by blank characters (spaces or tabs or newlines). When two or more delimiters appear in successive positions, they are considered as only one delimiter. Blank characters at the start or at the end of the string are ignored. At the end of the function, the tokens variable points to an array of string, with each value being one token of the initial string. The last element points to a null pointer.
 * Due to its internal algorithm, only the 254 first tokens of the string can be returned.
 * \param s String that will be splitted in tokens
 * \param tokens Pointer to an array of string. The array of string should not be initialized before the function which will create it. The calling code should take care of releasing this variable. However, the pointer should already address a valid and allocated variable. The first element of the array is not initialized and kept for future access (it will be used for the name of the program in the parameters of the exec functions).
 */
void tokenize(char *s,char ***tokens) {
	size_t num=1;
	*tokens=(char**)malloc(0xff*sizeof(char*));	
	char *ss=s;
	char *ss2;
	size_t size;
	while (ss!=0 && (*ss==' ' || *ss=='\t' || *ss=='\n')) ++ss;	// Go to the first non-blank character of the string
	while (ss!=0 && num<0xfe) {
		ss2=ss+1;
		while (ss2!=0 && *ss2!=' ' && *ss2!='\t' && *ss2!='\n') ++ss2;	// ss2 now points to the next blank character or the end of the string
		size=ss2-ss;
		(*tokens)[num]=(char*)malloc(size+1);
		strncpy((*tokens)[num],ss,size);
		(*tokens)[num][size]=0;
		++num;
		ss=ss2;
		while (ss!=0 && (*ss==' ' || *ss=='\t' || *ss=='\n')) ++ss;	// Go to the first non-blank character of the string
	}
	char **newtokens=realloc(*tokens,(num+1)*sizeof(char*));
	if (newtokens!=0) *tokens=newtokens;
	(*tokens)[num]=0;
}

/**
 * \brief Run a script and return a handle to the output
 *
 * Given a script whose "relative path" has already been ascertained, run it and return a handle
 * to the (open) temporary file containing its output. The file will be unlinked so it will disappear
 * once closed.
 * \param relative Name of script in path relative to virtual filesystem
 * \param proc Pointer to Procedure struct of relevant script
 * \param fi File info structure
 * \return Negative error code, else handle if everything went fine
 */
int run_script(const char *relative, Procedure *proc, struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"run_script(%s, %p, %p)\n", relative, proc, fi);
#endif
	char temp_filename[sizeof(persistent.tmp_template)];
	strncpy(temp_filename, persistent.tmp_template, sizeof temp_filename-1);
	temp_filename[sizeof temp_filename-1] = 0;
	int handle = mkstemp(temp_filename);
	if (handle <= 0) return -errno;
	unlink(temp_filename);
	proc->program->func(proc->program, relative, handle);
	if (fi) fi->direct_io=1;	// Force use of FUSE read on this file and do not take into account
	return handle;
}

/**
 * \brief Initialize the filesystem
 *
 * This function does all the technical stuff which has to be done before the start of the program. In particular, it allocates storage for the structures in memory and defines some of the callback functions used when a script is executed. The fuse_conn_info structure tells which capabilities FUSE provides and which one are needed and activated by the client. The function returns a pointer which will be available in all file operations later.
 * \param conn Capabilities requested by the application
 * \param cfg Fuse configuration
 * \return Pointer to a persistent structure, available in future file operations
 */
void *sfs_init(struct fuse_conn_info *conn,
	       struct fuse_config *cfg) {
#ifdef TRACE
	fprintf(stderr,"sfs_init\n");
#endif
	// Setup connection
	conn->want=0;
	return 0;
}

/**
 * \brief Unmount the filesystem
 *
 * This function is called when the filesystem is unmounted. It releases all allocated memory.
 * \param private_data Private data initialized and returned by function sfs_init
 */
void sfs_destroy(void *private_data) {
#ifdef TRACE
	fprintf(stderr,"sfs_destroy\n");
#endif
}

/**
 * \brief Get the attributes of a file on the virtual filesystem
 *
 * This function loads the attributes of a chosen file and stores it in the stbuf variable. It is used if no file handle is provided through a fuse_file_info structure.
 * \param path Virtual path of the file on the remote file system
 * \param stbuf Structure in which the attributes will be stored
 * \param fi File info structure (not used)
 * \return Error code, 0 if everything went fine
 */
int sfs_getattr(const char *path,struct stat *stbuf, struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_getattr(%s)\n",path);
#endif
	int code;
	if (path) {
		Procedure *proc = NULL;
		char *relative=relative_path(path);
		code=fstatat(persistent.mirror_fd,relative,stbuf,AT_SYMLINK_NOFOLLOW);
		if (!code && S_ISREG(stbuf->st_mode) && (proc = get_script(persistent.procs, relative))) {
			// If the file is a script, remove write access to everyone (for now we don't handle writing on scripts)
			stbuf->st_mode &= (~(S_IWUSR | S_IWGRP | S_IWOTH));
			// If we want the actual size of the output, have to run the script and look
			if (persistent.return_real_size) {
				struct stat realsize;

				int handle = run_script(relative, proc, fi);
				int rstat_code = fstat(handle, &realsize);
				if (!rstat_code) {
#ifdef TRACE
					fprintf(stderr, "sfs_getattr: Changing size from %ld to %zu\n", stbuf->st_size, realsize.st_size);
#endif
					stbuf->st_size = realsize.st_size;
				}
				close(handle);
			}
		}
		free(relative);
	} else {
		if (fi==NULL) return -EBADF;
		code=fstat(fi->fh, stbuf);
	}
	return (code==0)?0:-errno;
}

/**
 * \brief Check if a file can be accessed to with the given rights
 *
 * This function checks if the file at the path address (virtual path) can be accessed to. It returns 0 if the file exists and the user is authorized to access to it, and another value otherwise.
 * \param path Virtual path of the file
 * \param mask Required permissions mask (binary OR of values like R_OK, W_OK, X_OK)
 * \return 0 if the file can be opened, another value otherwise
 */
int sfs_access(const char *path,int mask) {
#ifdef TRACE
	fprintf(stderr,"sfs_access(%s,%o)\n",path,mask);
#endif
	char *relative=relative_path(path);
	int code=faccessat(persistent.mirror_fd,relative,mask,0);
	if (code==0 && (mask & W_OK)!=0) {	// If write-acess is requested, check if the file is a regular file and not a script, because for the moment we don't handle writing on scripts
		struct stat stbuf;
		int code2=fstatat(persistent.mirror_fd,relative,&stbuf,0);
		if (code2!=0) {free(relative);return -code2;}	// Normally, that should not happen
		if (S_ISREG(stbuf.st_mode) && get_script(persistent.procs,relative)!=0) {free(relative);return -1;}
	}
	free(relative);
	return (code==0)?0:-errno;
}

/**
 * \brief Return the target of a symbolic link
 *
 * This function reads the symbolic link which path is given as parameter and stores the target path in the buf string. The size of the buffer must be saved in the size variable (including the null character at the end of the string, and contrary to the system's readlink function). If the link target is too long, the function should truncate it. The function returns 0 if everything went fine and an error code otherwise.
 * As the file system is programmed, the mounted folder can hold symbolic links to any other file, being or not on the filesystem. Using relative links in symbolic links targets can ensure the symbolic link is still on the virtual file system and the target is read using the ScriptFS operations.
 * \param path Virtual path of the file
 * \param buf Buffer that will hold the target of the symbolic link. It should be allocated before the call to the function
 * \param size Size of buffer. If the target path (including the null character) is longer than this size, then it will be truncated (and unusable)
 * \return 0 if everything went fine, another value otherwise
 */
int sfs_readlink(const char *path,char *buf,size_t size) {
#ifdef TRACE
	fprintf(stderr,"sfs_readlink(%s,%p,%zi)\n",path,(void*)buf,size);
#endif
	char *relative=relative_path(path);
	ssize_t length=readlinkat(persistent.mirror_fd,relative,buf,size-1);
	free(relative);
	if (length<0) return -errno;
	buf[length]=0;
	return 0;
}

/**
 * \brief Open a directory for reading
 *
 * The function opens the directory to read its content. It actually looks if the opening is permitted, and stores a handle of the directory in the fi struct.
 * \param path Virtual path of the directory
 * \param fi FUSE information on the directory, which contains the file handle, filled in by the function
 * \return 0 if everything went fine, another value otherwise
 */
int sfs_opendir(const char *path, struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_opendir(%s,%p)\n",path,(fi==0)?0:(void*)(long)(fi->fh));
#endif
	char *relative=relative_path(path);
	int fd=openat(persistent.mirror_fd,relative,O_RDONLY);
	if (fd<0) {
		free(relative);
		return -errno;
	}
	DIR* handle=fdopendir(fd);
	if (handle==0) {
		if (relative) free(relative);
		return -errno;
	}
	FileStruct *fs=(FileStruct*)malloc(sizeof(FileStruct));
	fs->type=T_FOLDER;
	fs->dir_handle=(void*)handle;
	strncpy(fs->filename,relative,FILENAME_MAX_LENGTH-1);
	fs->filename[FILENAME_MAX_LENGTH-1]=0;
	fi->fh=(long)(fs);
	free(relative);
	return 0;
}

/**
 * \brief Read the content of a directory
 *
 * This function returns all directory entries to the caller. It is one of the main functions of the filesystem.
 * \param path Virtual path of the directory
 * \param buf Name of a file in the directory
 * \param filler Filler function provided by the FUSE system
 * \param offset Offset of the next directory entry, in the case files are provided one by one
 * \param fi File information structure
 * \param rf Readdir flags for plus mode (not used)
 * \return 0 if there are no more files or if the filler function returned a non-null value, another value otherwise
 */
int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
		struct fuse_file_info *fi, enum fuse_readdir_flags rf) {
#ifdef TRACE
	fprintf(stderr,"sfs_readdir(%s,%p)\n",path,(fi==0)?0:(void*)(long)(fi->fh));
#endif
	(void) rf;
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type!=T_FOLDER) return -ENOTDIR;
	DIR *handle=(DIR*)(fs->dir_handle);
	struct dirent* entry;
	do {
		errno=0;
		entry=readdir(handle);
		if (entry==0) return -errno;
		filler(buf,entry->d_name, 0, 0, 0);
	} while (entry!=0);
	return 0;
}

/**
 * \brief Release a directory structure
 *
 * This function is called when the user does not need to read a directory any longer. It is the equivalent of the release function for directories. The function releases the directory handle and frees all memory allocated to store the directoy element. 
 * \param path Directory which will be released, this variable is not used
 * \param fi FUSE information on the directory
 * \return Error code, or 0 if everything went fine
 */
int sfs_releasedir(const char *path,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_releasedir(%s,%p)\n",path,(void*)(long)(fi->fh));
#endif
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type!=T_FOLDER) return -ENOTDIR;
	DIR *handle=(DIR*)(fs->dir_handle);
	free(fs);
	int code=closedir(handle);
	return (code==0)?0:-errno;
}

/**
 * \brief Make a new directory
 *
 * This function creates a new empty directory in the virtual file system.
 * \param path Virtual path of the new directory
 * \param mode Directory permissions
 * \return Error code, or 0 if everything went fine
 */
int sfs_mkdir(const char *path,mode_t mode) {
#ifdef TRACE
	fprintf(stderr,"sfs_mkdir(%s,%X)\n",path,mode);
#endif
	char *relative=relative_path(path);
	int code=mkdirat(persistent.mirror_fd,relative,mode);
	free(relative);
	return (code==0)?0:-errno;
}

/**
 * \brief Remove a directory
 *
 * This function remove a directory in the virtual file system. This is basically the same as removing the directory in the mirror file system.
 * \param path Virtual path of the directory
 * \return Error code, 0 if everything went fine
 */
int sfs_rmdir(const char *path) {
#ifdef TRACE
	fprintf(stderr,"sfs_rmdir(%s)\n",path);
#endif
	char *relative=relative_path(path);
	int code=unlinkat(persistent.mirror_fd,relative,AT_REMOVEDIR);
	free(relative);
	return (code==0)?0:-errno;
}

/**
 * \brief Make a symbolic link in the virtual file system
 *
 * This function creates a new symbolic link which targets to another file.
 * \param to Path to the target of the symbolic link
 * \param from Name of the symbolic link
 * \return Error code, 0 if everything went fine
 */
int sfs_symlink(const char* to,const char *from) {
#ifdef TRACE
	fprintf(stderr,"sfs_symlink(%s,%s)\n",to,from);
#endif
	char *relative=relative_path(to);
	int code=symlinkat(from,persistent.mirror_fd,relative);
	free(relative);
	return (code==0)?0:-errno;
}

/**
 * \brief Remove a file from the virtual file system
 *
 * This function unlinks (removes) a file from the virtual file system. This is the same as removing the file from the mirror file system.
 * \param path Path of the file in the virtual file system
 * \return Error code, or 0 if everything went fine
 */
int sfs_unlink(const char *path) {
#ifdef  TRACE
	fprintf(stderr,"sfs_unlink(%s)\n",path);
#endif
	char *relative=relative_path(path);
	int code=unlinkat(persistent.mirror_fd,relative,0);
	free(relative);
	return (code==0)?0:-errno;
}

/**
 * \brief Create a new hard link on the virtual file system
 *
 * This function creates a hard link on the virtual file system. This is the same as creating the hard link on the mirror file system. Hard links are interesting because they can "extend" the file system by referring to files out of it. Those target files, if they are scripts, can be executed by the file system.
 * \param to Path to the target of the hard link
 * \param from Name of the hard link
 * \return Error code, or 0 if everything went fine
 */
int sfs_link(const char* from,const char *to) {
#ifdef TRACE
	fprintf(stderr,"sfs_symlink(%s,%s)\n",from,to);
#endif
	char *relative_from=relative_path(from);
	char *relative_to=relative_path(to);
	int code=linkat(persistent.mirror_fd,relative_from,persistent.mirror_fd,relative_to,0);
	free(relative_from);
	free(relative_to);
	return (code==0)?0:-errno;
}

/**
 * \brief Rename a file or a directory in the virtual file system
 *
 * This function changes the name of a file or a directory in the virtual file system. This is the same as changing the name in the mirror file system.
 * \param from Path of the source file in the virtual file system
 * \param to Path of the destination in the virtual file system
 * \param flags Governs whether should be atomic or not
 * \return Error code, or 0 if everything went fine
 */
int sfs_rename(const char *from,const char *to, unsigned int flags) {
#ifdef  TRACE
	fprintf(stderr,"sfs_rename(%s,%s)\n",from,to);
#endif
	char *relative_from=relative_path(from);
	char *relative_to=relative_path(to);
	int code=renameat2(persistent.mirror_fd, relative_from, persistent.mirror_fd, relative_to, flags);
	free(relative_from);
	free(relative_to);
	return (code==0)?0:-errno;
}

/**
 * \brief Change permissions of a file in the virtual file system
 *
 * This function changes the permissions (read, write, execute) of a file in the virtual file system.
 * \param path Virtual path of the file
 * \param mode New permissions of the file, which will replace the old ones
 * \return Error code, or 0 if everything went fine
 */
int sfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_chmod(%s,%X)\n",path,mode);
#endif
	struct stat stbuf;
	int code;
	if (path) {
		char *relative=relative_path(path);
		code=fstatat(persistent.mirror_fd, relative, &stbuf, 0);
		if (code==0 && S_ISREG(stbuf.st_mode) && (mode & (S_IWUSR | S_IWGRP | S_IWOTH))!=0 && get_script(persistent.procs,relative)!=0) mode&= (~(S_IWUSR | S_IWGRP | S_IWOTH));	// If the file is a script, remove write access to the requested permissions
		code=fchmodat(persistent.mirror_fd, relative, mode, 0);
		free(relative);
	} else {
		if (fi==NULL) return -EBADF;
		// TODO: This does NOT do the script check above b/c get_script requires a filename we do not have
		code=fchmod(fi->fh, mode);
	}

	return (code==0)?0:-errno;
}

/**
 * \brief Truncate a file in a virtual file system so that it has the desired size
 *
 * This function truncates a file in the virtual file system to the desired size given as a parameter. It also checks if the file is not a script to determine if it can be truncated, because for the moment writing on scripts is disabled.
 * \param path Virtual path of the file
 * \param size Final size of the file
 * \return Error code, or 0 if everything went fine
 */
int sfs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_truncate(%s,%li)\n",path,(long)size);
#endif
	struct stat stbuf;
	uint64_t fd;
	int code;
	if (path) {
		char *relative=relative_path(path);
		code=fstatat(persistent.mirror_fd,relative,&stbuf,0);
		if (code==0 && S_ISREG(stbuf.st_mode) && get_script(persistent.procs,relative)!=0) {
			free(relative);
			return -EACCES;
		}	// Writing on a script is forbidden
		fd=openat(persistent.mirror_fd,relative,O_WRONLY);
		free(relative);
	} else {
		if (fi == NULL) return -EBADF;
		fd = fi->fh;
		// TODO: This does NOT do the script check above b/c get_script requires a filename we do not have
	}
	if (fd<0) return -errno;
	code=ftruncate(fd,size);
	close(fd);
	return (code==0)?0:-errno;
}

/**
 * \brief Update the last access and last modification times of a file on the virtual file system
 *
 * This function updates the last access time from the first element of the array in parameter, and the last modification time from the second element of the same array. It only transfers the order to the mirror file system.
 * \param path Virtual path of the file
 * \param ts Array of two elements holding the two time structures of the last access and last modification time which the file will have after the execution
 * \param fi File information (not used)
 * \return Error code, or 0 if everything went fine
 */
int sfs_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_utimens(%s)\n",path);
#endif
	struct stat stbuf;
	int code;
	if (path) {
		char *relative=relative_path(path);
		code=fstatat(persistent.mirror_fd,relative,&stbuf,0);
		if (code==0 && S_ISREG(stbuf.st_mode) && get_script(persistent.procs,relative)!=0) {
			free(relative);
			return -EACCES;
		}	// Writing on a script is forbidden
		code=utimensat(persistent.mirror_fd, relative, ts, 0);
		free(relative);
	} else {
		code = futimens(fi->fh, ts);
		// TODO: This does NOT do the script check above b/c get_script requires a filename we do not have
	}
	return (code==0)?0:-errno;
}
 
/**
 * \brief Retrieve statistics about the file system
 *
 * This function retrieves statistics about the whole virtual file system. Since it is hosted on a mirror file system, the statistics should be the same as the latter.
 * \param path Virtual path of any file on the virtual file system, ignored
 * \param stbuf Record holding the statistics about the virtual file system after the execution of the function
 * \return Error code, or 0 if everything went fine
 */
int sfs_statfs(const char *path,struct statvfs *stbuf) {
#ifdef TRACE
	fprintf(stderr,"sfs_statfs(%s)\n",path);
#endif
	int code=statvfs("/",stbuf);
	return (code==0)?0:-errno;
}

/**
 * \brief Open a file in the virtual file system
 *
 * This function opens a file in the virtual file system. If the file is not a script file, this is the same as opening it on the mirror file system and saving its handle in the file info structure fi. If the file is a script file, the function executes the script, saves its content on a temporary file, and opens the latter for reading. The temporary file is unlinked immediately so that the user does not have to worry about erasing it when it is closed. The function also checks if the open mode (in the fi->flags variable) are allowed for this file.
 * \param path Virtual path of the file
 * \param fi File information structure, filled by the function with the handle of the mirror file
 * \return Error code, or 0 if everything went fine
 */
int sfs_open(const char *path,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_open(%s)\n",path);
#endif
	int handle=0;
	int typ=0;
	char *relative=relative_path(path);
	Procedure *proc=get_script(persistent.procs,relative);
	if (proc!=0) {	// If the file is a script, the interpreter is executed to produce the result of the script
		// If the caller requests to open the file in one of the write modes, immediately abort the opening
		if ((fi->flags & O_WRONLY)!=0 || (fi->flags & O_RDWR)!=0) {
			free(relative);
			return -EACCES;
		}

		handle = run_script(relative, proc, fi);
		if (handle <= 0) {
			free(relative);
			return handle;
		}
		typ=1;
	} else {
		handle=openat(persistent.mirror_fd,relative,fi->flags);
		if (handle<=0) {free(relative);return -errno;}
		typ=2;
		fi->direct_io=0;	// Authorize direct translation of FUSE IO calls to system calls
	}
	FileStruct *fs=(FileStruct*)malloc(sizeof(FileStruct));
	fs->type=(typ==1)?T_SCRIPT:T_FILE;
	fs->file_handle=handle;
	strncpy(fs->filename,relative,FILENAME_MAX_LENGTH-1);
	fs->filename[FILENAME_MAX_LENGTH-1]=0;
	fi->fh=(long)fs;
	free(relative);
	return 0;
}

/**
 * \brief Read content from a file on the virtual file system
 *
 * This function reads the chosen number of bytes from the file which handle is stored in the fi structure. The file must already be opened (otherwise there is no handle in fi). The reading starts at the position offset in the file. The function returns the actual number of bytes read or 0 if the offset was at or beyond the end of the file. 
 * \param path Virtual path of the file, not used because the file handle is stored in fi
 * \param buf Buffer in which the content read will be stored, must be allocated before the call to the function
 * \param size Number of bytes the caller wants to read
 * \param offset Starting position of the reading
 * \param fi FUSE file information structure, holding the handle to the mirror file
 * \return Actual number of bytes read, or a negative error code if something wrong occurred
 */
int sfs_read(const char *path,char *buf,size_t size,off_t offset,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_read(%zi,%li,%p)\n",size,(long)offset,(fi==0)?0:(void*)(long)(fi->fh));
#endif
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type==T_FOLDER) return -EISDIR;
	off_t res=lseek(fs->file_handle,offset,SEEK_SET);
	if (res<0) return -errno;
	ssize_t num=read(fs->file_handle,buf,size);
	if (num>=0) return num; else return -errno;
}

/**
 * \brief Write content in a file of the virtual file system
 *
 * This function writes a chosen number of bytes from the buffer in parameter to the file which handle is stores in the fi structure. The file must already be opened (otherwise there is no handle in fi). The output starts at the position offset into the file. The function returns the actual number of bytes written or a negative error code.
 * \param path Virtual path of the file, not used because the file handle is stored in fi
 * \param buf Buffer to write in the file
 * \param size Number of bytes the caller wants to write
 * \param offset Starting position of the output
 * \param fi FUSE file information structure, holding the handle to the mirror file
 * \return Actual number of bytes written, or a negative error code if something wrong occurred
 */
int sfs_write(const char *path,const char *buf,size_t size,off_t offset,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_write(%zi,%li,%p)\n", size, (long)offset, (fi==0)?0:(void*)(long)(fi->fh));
#endif
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type==T_FOLDER) return -EISDIR;
	off_t res=lseek(fs->file_handle,offset,SEEK_SET);
	if (res<0) return -errno;
	ssize_t num=write(fs->file_handle,buf,size);
	if (num>=0) return num; else return -errno;
}

/**
 * \brief Close a file on the virtual file system
 *
 * The function closes an opened file on the virtual file system. It erases the temporary files generated in case the virtual file was a script, releases all the structures allocated (and especially the one holding the handle) and synchronizes the content of the file with that of the mirror file.
 * \param path Virtual path of the file, not used because the file handle is stored in fi
 * \param fi FUSE file information structure, holding the handle to the mirror file
 * \return Error code, or 0 if everything went fine, but not used by FUSE
 */
int sfs_release(const char *path,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_release(%p)\n",(fi==0)?0:(void*)(long)(fi->fh));
#endif
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type==T_FOLDER) return -EISDIR;
	int code=close(fs->file_handle);
	free(fs);
	return (code==0)?0:-errno;
}

/**
 * \brief Flush any information about the file to the disk
 *
 * This function makes sure any information written on the file (on the virtual file system) is stored on the disk. It only calls the fsync method from the mirror file system.
 * \param path Virtual path of the file, not used because the file handle is stored in fi
 * \param isdatasync Strange parameter, if not null indicates that only user data should be synchronized and not meta data
 * \param fi FUSE file information structure, holding the handle to the mirror file
 * \return Error code, or 0 if everything went fine, but not used by FUSE
 */
int sfs_fsync(const char *path,int isdatasync,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_fsync(%p)\n",(fi==0)?0:(void*)(long)(fi->fh));
#endif
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type==T_FOLDER) return -EISDIR;
	int code=fsync(fs->file_handle);
	return (code==0)?0:-errno;
}

/**
 * \brief Flush modifications to a file in the virtual file system.
 *
 * The function is called before each close of a function on the virtual file system. It gives a chance to report delayed errors according to the documentation. This one also says that there can be zero, one, or several flush call for each open.
 * \param path Virtual path of the file, not used because the file handle is stored in fi
 * \param fi FUSE file information structure, holding the handle to the mirror file
 * \return Error code, or 0 if everything went fine, but not used by FUSE
 */
int sfs_flush(const char *path,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_flush(%p)\n",(fi==0)?0:(void*)(long)(fi->fh));
#endif
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type==T_FOLDER) return -EISDIR;
	if (fs->type==T_SCRIPT) return 0;
	int code=fsync(fs->file_handle);
	return (code==0)?0:-errno;
}

/**
 * \brief Create a new file on the virtual file system
 *
 * This function creates a file and opens it. It only transfers the order to the mirror system that creates the file.
 * \param path Virtual path of the new file
 * \param mode Permissions of the file (read, write, execute)
 * \param fi FUSE file information structure, that will hold the handle to the mirror file
 * \return Error code, or 0 if everything went fine, but not used by FUSE
 */
int sfs_create(const char *path,mode_t mode,struct fuse_file_info *fi) {
#ifdef TRACE
	fprintf(stderr,"sfs_create(%s,%X)\n",path,mode);
#endif
	int handle=0;
	char *relative=relative_path(path);
	handle=openat(persistent.mirror_fd,relative,O_CREAT | O_WRONLY | O_TRUNC,mode);
	if (handle<=0) {free(relative);return -errno;}
	FileStruct *fs=(FileStruct*)malloc(sizeof(FileStruct));
	fs->type=T_FILE;
	fs->file_handle=handle;
	strncpy(fs->filename,relative,FILENAME_MAX_LENGTH-1);
	fs->filename[FILENAME_MAX_LENGTH-1]=0;
	fi->fh=(long)fs;
	free(relative);
	return 0;
}

static off_t sfs_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi)
{
#ifdef TRACE
	fprintf(stderr,"sfs_lseek(%p)\n",(fi==0)?0:(void*)(long)(fi->fh));
#endif
	if (fi==0 || fi->fh==0) return -EBADF;
	FileStruct *fs=(FileStruct*)(long)(fi->fh);
	if (fs->type==T_FOLDER) return -EISDIR;
	off_t res=lseek(fs->file_handle, off, whence);
	if (res<0) return -errno;
	return 0;
	}

/**
 * \brief List of callback functions
 *
 * This constant holds the references of the callback functions used when an operation is made on a file on the FUSE filesystem.
 */
struct fuse_operations sfs_oper = {
	.init=sfs_init,
	.destroy=sfs_destroy,
	.getattr=sfs_getattr,
	.access=sfs_access,
	.readlink=sfs_readlink,
	.symlink=sfs_symlink,
	.link=sfs_link,
	.opendir=sfs_opendir,
	.releasedir=sfs_releasedir,
	.readdir=sfs_readdir,
	.mkdir=sfs_mkdir,
	.unlink=sfs_unlink,
	.rmdir=sfs_rmdir,
	.rename=sfs_rename,
	.chmod=sfs_chmod,
	.truncate=sfs_truncate,
	.utimens=sfs_utimens,
	.statfs=sfs_statfs,
	.open=sfs_open,
	.read=sfs_read,
	.write=sfs_write,
	.release=sfs_release,
	.fsync=sfs_fsync,
	.create=sfs_create,
	.flush=sfs_flush,
	.lseek=sfs_lseek,
};

/**
 * \brief Main program, mounts file system
 *
 * The main program processes command line arguments and mounts the file system. In addition to standard \e fusermount command parameters, possible command line arguments are:
 * 	Syntax: scriptfs [-l] [-p procedure|--procedure=procedure...] mirror_path mountpoint
 * 
 * 	- -p procedure
 * 		--procedure=procedure
 * 			Define an execution procedure. This procedure holds the external executable program and the test program that will be used on files. The command can be repeated as many times as needed, and each procedure will be tested in the order they appear in the command-line. For more information about the way to define a procedure, see \ref syntaxdoc "Syntax of command-line".
 *      - -l
 *              Report final output size (in `stat`/`getattr`) instead of size of source script.
 *
 * \param argc Number of command line arguments, including the name of the calling program
 * \param argv Array of command line arguments, the first one being the path to the calling program
 * \return Error code, 0 if everything went fine
 */
int main(int argc,char **argv,char **envp) {
	// Get system information
	uid=geteuid();
	gid=getegid();
	persistent.envp=envp;
	// Parse command line arguments
	init_resources();
	struct stat sb;
	int scode;
	scode = stat("/dev/shm", &sb);
	if (!scode && ((sb.st_mode & S_IFMT) == S_IFDIR)) {
		strncpy(persistent.tmp_template, "/dev/shm/sfs.XXXXXX", sizeof(persistent.tmp_template));
	} else {
		strncpy(persistent.tmp_template, "/tmp/sfs.XXXXXX", sizeof(persistent.tmp_template));
	}
#ifdef TRACE
	fprintf(stderr, "main: Using %s as a temporary file template\n", persistent.tmp_template);
#endif
	size_t i,j;
	Procedures *last=0;
	for (i=1;i<argc && argv[i][0]=='-';++i) {
		if (argv[i][1]=='o') ++i;	// Skip -o options parameters
		else if (argv[i][1]=='l') { // Parse -l option (always report real file length)
			persistent.return_real_size = 1;
			for (j=i;j<argc;++j) argv[j]=argv[j+1];
			--argc;
			--i;
		}
		else if (argv[i][1]=='p') { // Parse -p options parameters
			if (i>=argc-1) {
				fprintf(stderr, "-p needs an argument\n");
				free_resources();
				print_usage(EX_USAGE);
			}
			Procedure *proc=get_procedure_from_string(argv[i+1]);
			if (proc!=0) {
				Procedures *procs=(Procedures*)malloc(sizeof(Procedures));
				procs->procedure=proc;
				procs->next=0;
				if (last==0) persistent.procs=procs; else last->next=procs;
				last=procs;
			} else {
				free_resources();
				fprintf(stderr, "-p option failed\n");
				exit(1);
			}
			for (j=i;j<argc-2;++j) argv[j]=argv[j+2];
			argc-=2;
			--i;
#ifdef TRACE
			if (proc && proc->program)
				fprintf(stderr, "Program name is %s\n", proc->program->path);
#endif
		}
	}
	if ((argc-i)!=2) {
		fprintf(stderr, "See %ld parameters; need a mirror_folder and a mount_point, and no more\n",
		       argc-i);
		free_resources();
		print_usage(EX_USAGE);
	}
	scode = stat(argv[i], &sb);
	if (scode || ((sb.st_mode & S_IFMT) != S_IFDIR)) {
		fprintf(stderr, "mirror_folder %s doesn't exist or is not a directory\n", argv[i]);
		free_resources();
		return ENOENT;
	}
		
	scode = stat(argv[i+1], &sb);
	if (scode || ((sb.st_mode & S_IFMT) != S_IFDIR)) {
		fprintf(stderr, "mount_point %s doesn't exist or is not a directory\n", argv[i+1]);
		free_resources();
		return ENOENT;
	}
		
	persistent.mirror=realpath(argv[i],0);
	persistent.mirror_len=strlen(persistent.mirror);
	argv[i]=argv[i+1];
	argc--;
	// Open mirror directory
	persistent.mirror_fd=open(persistent.mirror,O_RDONLY);
	if (persistent.mirror_fd<0) {
		fprintf(stderr,"Can't open mirror folder: %s\n",persistent.mirror);
		free_resources();
		return EX_NOPERM;
	}
	// Check if no valid procedure was set. In that case, automatically provide a standard procedure
	if (persistent.procs==0) {
		persistent.procs=(Procedures*)malloc(sizeof(Procedures));
		persistent.procs->procedure=(Procedure*)malloc(sizeof(Procedure));
		persistent.procs->procedure->program=(Program*)malloc(sizeof(Program));
		persistent.procs->procedure->program->path=0;
		persistent.procs->procedure->program->args=0;
		persistent.procs->procedure->program->filearg=0;
		persistent.procs->procedure->program->func=&program_shell;
		persistent.procs->procedure->test=(Test*)malloc(sizeof(Test));
		persistent.procs->procedure->test->func=&test_shell_executable;
		persistent.procs->procedure->test->path=0;
		persistent.procs->procedure->test->args=0;
		persistent.procs->procedure->test->filearg=0;
		persistent.procs->procedure->test->filter=0;
		persistent.procs->procedure->test->compiled=0;
		persistent.procs->next=0;
	}
	// Daemonize the program
	int code=fuse_main(argc, argv, &sfs_oper, NULL);
	free_resources();
	close(persistent.mirror_fd);
	return code;
}
