#include <stdio.h>
#include <stdlib.h>
#include <program.h>
#include <argdata.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sched.h>
#include <sys/procdesc.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <sys/capsicum.h>
#include <cloudabi_syscalls.h>

int stdout;
int procfs;
int bootfs;
int initrd;
int reversefd;
int pseudofd;
int ifstore;

long uptime() {
	int uptimefd = openat(procfs, "kernel/uptime", O_RDONLY);
	if(uptimefd < 0) {
		dprintf(stdout, "INIT: failed to open uptime: %s\n", strerror(errno));
		return 0;
	}
	char buf[16];
	ssize_t r = read(uptimefd, buf, sizeof(buf) - 1);
	if(r <= 0) {
		dprintf(stdout, "INIT: failed to read uptime: %s\n", strerror(errno));
		return 0;
	}
	buf[r] = 0;
	close(uptimefd);
	return atol(buf);
}

argdata_t *argdata_create_string(const char *value) {
	return argdata_create_str(value, strlen(value));
}

int program_run(const char *name, int bfd, argdata_t *ad) {
	int pfd = program_spawn(bfd, ad);
	if(pfd < 0) {
		dprintf(stdout, "INIT: %s failed to start: %s\n", name, strerror(errno));
		return -1;
	}

	dprintf(stdout, "INIT: %s started.\n", name);

	siginfo_t si;
	pdwait(pfd, &si, 0);
	dprintf(stdout, "INIT: %s exited, exit status %d\n", name, si.si_status);
	dprintf(stdout, "INIT: current uptime: %ld seconds\n", uptime());

	close(pfd);
	return si.si_status;
}

void start_tmpfs() {
	int bfd = openat(bootfs, "tmpfs", O_RDONLY);
	if(bfd < 0) {
		dprintf(stdout, "Can't run tmpfs, because it failed to open: %s\n", strerror(errno));
		return;
	}

	dprintf(stdout, "Running tmpfs...\n");
	argdata_t *keys[] = {argdata_create_string("stdout"), argdata_create_string("reversefd"), argdata_create_string("deviceid")};
	argdata_t *values[] = {argdata_create_fd(stdout), argdata_create_fd(reversefd), argdata_create_int(1)};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));

	int pfd = program_spawn(bfd, ad);
	if(pfd < 0) {
		dprintf(stdout, "tmpfs failed to spawn: %s\n", strerror(errno));
	} else {
		dprintf(stdout, "tmpfs spawned, fd: %d\n", pfd);
	}
}

void start_networkd() {
	// Copy the ifstorefd for the networkd
	write(ifstore, "COPY", 10);
	char buf[20];
	buf[0] = 0;
	struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
	alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	if(recvmsg(ifstore, &msg, 0) < 0 || strncmp(buf, "OK", 2) != 0) {
		perror("Failed to retrieve ifstore-copy from ifstore");
		exit(1);
	}
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if(cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
		dprintf(stdout, "Ifstore socket requested, but not given\n");
		exit(1);
	}
	int *fdbuf = reinterpret_cast<int*>(CMSG_DATA(cmsg));
	int new_ifstorefd = fdbuf[0];

	int bfd = openat(bootfs, "networkd", O_RDONLY);
	if(bfd < 0) {
		dprintf(stdout, "Can't run tmpfs, because it failed to open: %s\n", strerror(errno));
		return;
	}

	dprintf(stdout, "Running networkd...\n");
	argdata_t *keys[] = {argdata_create_string("stdout"), argdata_create_string("rootfs"), argdata_create_string("bootfs"), argdata_create_string("ifstore")};
	argdata_t *values[] = {argdata_create_fd(stdout), argdata_create_fd(pseudofd), argdata_create_fd(bootfs), argdata_create_fd(new_ifstorefd)};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));

	int pfd = program_spawn(bfd, ad);
	if(pfd < 0) {
		dprintf(stdout, "networkd failed to spawn: %s\n", strerror(errno));
	} else {
		dprintf(stdout, "networkd spawned, fd: %d\n", pfd);
	}
}

int start_networked_binary(const char *name, bool wait = true) {
	int bfd = openat(bootfs, name, O_RDONLY);
	if(bfd < 0) {
		dprintf(stdout, "Failed to open %s: %s\n", name, strerror(errno));
		return 1;
	}

	dprintf(stdout, "Init going to program_spawn() %s...\n", name);

	int networkfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(networkfd < 0) {
		perror("networkfd");
		exit(1);
	}
	if(connectat(networkfd, pseudofd, "networkd") < 0) {
		perror("connect");
		exit(1);
	}

	argdata_t *keys[] = {argdata_create_string("stdout"),
		argdata_create_string("tmpdir"),
		argdata_create_string("initrd"),
		argdata_create_string("networkd"),
		argdata_create_string("procfs"),
		argdata_create_string("bootfs")};
	argdata_t *values[] = {argdata_create_fd(stdout),
		argdata_create_fd(pseudofd),
		argdata_create_fd(initrd),
		argdata_create_fd(networkfd),
		argdata_create_fd(procfs),
		argdata_create_fd(bootfs)};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));

	if(wait) {
		auto r = program_run(name, bfd, ad);
		close(bfd);
		return r;
	} else {
		int pfd = program_spawn(bfd, ad);
		if(pfd < 0) {
			dprintf(stdout, "%s failed to spawn: %s\n", name, strerror(errno));
			return 1;
		}
		return 0;
	}
}

void rm_rf_contents(DIR *d) {
	struct dirent *ent;
	std::vector<std::string> files;
	std::vector<std::string> directories;
	while((ent = readdir(d)) != nullptr) {
		if(ent->d_type == DT_DIR) {
			if(strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
				directories.push_back(ent->d_name);
			}
		} else {
			files.push_back(ent->d_name);
		}
	}

	for(auto &dir : directories) {
		// delete all files within
		int innerdh = openat(dirfd(d), dir.c_str(), O_RDONLY);
		DIR *innerdir = fdopendir(innerdh);
		rm_rf_contents(innerdir);
		closedir(innerdir);
		unlinkat(dirfd(d), dir.c_str(), AT_REMOVEDIR);
	}
	for(auto &f : files) {
		unlinkat(dirfd(d), f.c_str(), 0);
	}
}

void open_pseudo() {
	// get a pseudopair
	write(ifstore, "PSEUDOPAIR", 10);
	char buf[20];
	buf[0] = 0;
	struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
	alignas(struct cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))];
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	if(recvmsg(ifstore, &msg, 0) < 0 || strncmp(buf, "OK", 2) != 0) {
		perror("Failed to retrieve pseudopair from ifstore");
		exit(1);
	}
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if(cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_len != CMSG_LEN(2 * sizeof(int))) {
		dprintf(stdout, "Pseudopair requested, but not given\n");
		exit(1);
	}
	int *fdbuf = reinterpret_cast<int*>(CMSG_DATA(cmsg));
	reversefd = fdbuf[0];
	pseudofd = fdbuf[1];
	cloudabi_fdstat_t fsb = {
		.fs_rights_base =
			CLOUDABI_RIGHT_POLL_FD_READWRITE |
			CLOUDABI_RIGHT_FD_WRITE |
			CLOUDABI_RIGHT_FD_READ |
			CLOUDABI_RIGHT_SOCK_SHUTDOWN |
			CLOUDABI_RIGHT_SOCK_STAT_GET,
		.fs_rights_inheriting = 0,
	};
	if(cloudabi_sys_fd_stat_put(reversefd, &fsb, CLOUDABI_FDSTAT_RIGHTS) != 0) {
		dprintf(stdout, "Failed to limit rights on reverse FD");
	}

	fsb.fs_rights_base =
		CLOUDABI_RIGHT_FILE_CREATE_DIRECTORY |
		CLOUDABI_RIGHT_FILE_CREATE_FIFO |
		CLOUDABI_RIGHT_FILE_CREATE_FILE |
		CLOUDABI_RIGHT_FILE_LINK_SOURCE |
		CLOUDABI_RIGHT_FILE_LINK_TARGET |
		CLOUDABI_RIGHT_FILE_OPEN |
		CLOUDABI_RIGHT_FILE_READDIR |
		CLOUDABI_RIGHT_FILE_STAT_FGET |
		CLOUDABI_RIGHT_FILE_STAT_GET |
		CLOUDABI_RIGHT_FILE_UNLINK |
		CLOUDABI_RIGHT_SOCK_BIND_DIRECTORY |
		CLOUDABI_RIGHT_SOCK_CONNECT_DIRECTORY;
	fsb.fs_rights_inheriting =
		CLOUDABI_RIGHT_FD_DATASYNC |
		CLOUDABI_RIGHT_FD_READ |
		CLOUDABI_RIGHT_FD_SEEK |
		CLOUDABI_RIGHT_FD_STAT_PUT_FLAGS |
		CLOUDABI_RIGHT_FD_SYNC |
		CLOUDABI_RIGHT_FD_TELL |
		CLOUDABI_RIGHT_FD_WRITE |
		CLOUDABI_RIGHT_FILE_ADVISE |
		CLOUDABI_RIGHT_FILE_ALLOCATE |
		CLOUDABI_RIGHT_FILE_CREATE_DIRECTORY |
		CLOUDABI_RIGHT_FILE_CREATE_FIFO |
		CLOUDABI_RIGHT_FILE_CREATE_FILE |
		CLOUDABI_RIGHT_FILE_LINK_SOURCE |
		CLOUDABI_RIGHT_FILE_LINK_TARGET |
		CLOUDABI_RIGHT_FILE_OPEN |
		CLOUDABI_RIGHT_FILE_READDIR |
		CLOUDABI_RIGHT_FILE_STAT_FGET |
		CLOUDABI_RIGHT_FILE_STAT_FPUT_SIZE |
		CLOUDABI_RIGHT_FILE_STAT_FPUT_TIMES |
		CLOUDABI_RIGHT_FILE_STAT_GET |
		CLOUDABI_RIGHT_FILE_UNLINK |
		CLOUDABI_RIGHT_MEM_MAP |
		CLOUDABI_RIGHT_MEM_MAP_EXEC |
		CLOUDABI_RIGHT_POLL_FD_READWRITE |
		CLOUDABI_RIGHT_PROC_EXEC |
		CLOUDABI_RIGHT_SOCK_BIND_DIRECTORY |
		CLOUDABI_RIGHT_SOCK_CONNECT_DIRECTORY;
	if(cloudabi_sys_fd_stat_put(pseudofd, &fsb, CLOUDABI_FDSTAT_RIGHTS) != 0) {
		dprintf(stdout, "Failed to limit rights on pseudo FD");
	}
}

void program_main(const argdata_t *) {
	stdout = 0;
	procfs = 2;
	bootfs = 3;
	initrd = 4;
	ifstore = 5;

	dprintf(stdout, "Init starting up.\n");

	// reconfigure stderr
	FILE *out = fdopen(stdout, "w");
	fswap(stderr, out);

	open_pseudo();
	start_tmpfs();
	start_networkd();

	// sleep for a bit for networkd to come up
	{
		struct timespec ts = {.tv_sec = 5, .tv_nsec = 0};
		clock_nanosleep(CLOCK_MONOTONIC, 0, &ts);
	}

	start_networked_binary("httpd", false);
	while(1) {
		start_networked_binary("pythonshell", true);
	}

	pthread_mutex_t mtx;
	pthread_mutex_init(&mtx, NULL);
	pthread_cond_t cond;
	pthread_cond_init(&cond, NULL);
	pthread_mutex_lock(&mtx);
	pthread_cond_wait(&cond, &mtx);
	pthread_mutex_unlock(&mtx);
	exit(0);

	// 1. Open the init-binaries directory fd from argdata
	// 2. Read some configuration from the kernel cmdline
	// 3. Start the necessary applications (like dhcpcd)
	// 4. Keep track of their status using poll() / poll_fd()
	//    (so that the application actually always blocks)
	// 5. If needed, open an init RPC socket so that applications or the
	//    kernel can always ask for extra services
}
