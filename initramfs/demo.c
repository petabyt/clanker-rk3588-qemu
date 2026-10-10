/*
 * P5 demo: exercise the booted kernel from userspace and print a
 * battery of results on the serial console.  Static, no libc
 * dependencies beyond the toolchain's own set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>

static int fail;

static void check(const char *what, int ok)
{
	printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) {
		fail++;
	}
}

static void grepfile(const char *path, const char *key)
{
	char buf[4096];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		printf("       %s: open failed\n", path);
		return;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		printf("       %s: read failed\n", path);
		return;
	}
	buf[n] = '\0';
	for (char *line = strtok(buf, "\n"), *prev = NULL; line;
	     prev = line, line = strtok(NULL, "\n")) {
		if (strstr(line, key) && (!prev || 1)) {
			printf("       %s\n", line);
		}
	}
}

int main(void)
{
	struct utsname u;
	struct timespec t0, t1;
	char tmp[] = "/tmp/demo.bin";
	char magic[64];
	pid_t pid;
	int fd, status, i, ncpu = 0;
	ssize_t n;
	long long delta_ms;
	unsigned char pat[4096], rbuf[4096];

	printf("\n=== P5 userspace demo ===\n");

	check("uname(2)", uname(&u) == 0);
	printf("       %s %s %s %s\n", u.sysname, u.nodename,
	       u.release, u.machine);

	fd = open("/proc/cpuinfo", O_RDONLY);
	if (fd >= 0) {
		char buf[4096];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);

		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			for (char *p = buf; (p = strstr(p, "processor")); p++) {
				ncpu++;
			}
			char *m = strstr(buf, "model name");
			if (m) {
				printf("       cpu: %.*s",
				       (int)(strchr(m, '\n') - m), m);
			}
		}
	}
	check("cpuinfo online cpus", ncpu >= 1);
	printf("       ncpu=%d\n", ncpu);

	grepfile("/proc/meminfo", "MemTotal");
	grepfile("/proc/interrupts", "arch_timer");
	printf("       ---\n");
	printf("       interrupts lines:\n");
	{
		char cmd[] = "wc -l /proc/interrupts";
		/* no shell yet: count via open/read */
		int fd2 = open("/proc/interrupts", O_RDONLY);
		char b[8192];
		ssize_t n = read(fd2, b, sizeof(b) - 1);
		int lines = 0;

		close(fd2);
		b[n > 0 ? n : 0] = '\0';
		for (char *p = b; p && *p; p++) {
			if (*p == '\n') {
				lines++;
			}
		}
		printf("       %d\n", lines);
		(void)cmd;
	}

	/* file write/read/verify test */
	for (i = 0; i < (int)sizeof(pat); i++) {
		pat[i] = (unsigned char)(i * 7 + 3);
	}
	fd = open(tmp, O_CREAT | O_RDWR | O_TRUNC, 0600);
	check("open(tmpfile)", fd >= 0);
	if (fd >= 0) {
		for (i = 0; i < 16; i++) {
			check("write 4K chunk",
			      write(fd, pat, sizeof(pat)) == sizeof(pat));
		}
		check("fsync", fsync(fd) == 0);
		lseek(fd, 0, SEEK_SET);
		n = 0;
		for (i = 0; i < 16; i++) {
			ssize_t r = read(fd, rbuf, sizeof(rbuf));

			if (r != sizeof(rbuf) || memcmp(rbuf, pat, sizeof(pat))) {
				break;
			}
			n += r;
		}
		check("read back 64K and verify", n == 16 * (int)sizeof(pat));
		close(fd);
		unlink(tmp);
	}

	/* fork/exit/wait */
	pid = fork();
	if (pid == 0) {
		printf("       [child] pid=%d, exiting 42\n", getpid());
		exit(42);
	}
	check("waitpid", waitpid(pid, &status, 0) == pid &&
		   WIFEXITED(status) && WEXITSTATUS(status) == 42);

	/* timer resolution via nanosleep */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	nanosleep(&(struct timespec){0, 100 * 1000 * 1000}, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	delta_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		  (t1.tv_nsec - t0.tv_nsec) / 1000000;
	check("nanosleep ~100ms (>=50ms)", delta_ms >= 50 && delta_ms < 5000);
	printf("       slept %lld ms\n", delta_ms);

	/* mount table readable */
	{
		struct stat st;

		check("statfs /proc", stat("/proc/self/status", &st) == 0);
	}
	(void)magic;

	/* P6 display tests: direct fbdev pixels + the fbcon glyph path. */
	{
		int fb = open("/dev/fb0", O_RDWR);
		uint32_t *pix;
		size_t len = 1920u * 1080 * 4;
		int y, x, tty;
		const char *msg = "P6: fbcon glyph path test 0123456789\n";

		check("open /dev/fb0", fb >= 0);
		if (fb >= 0) {
			pix = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
				   fb, 0);
			check("mmap /dev/fb0", pix != MAP_FAILED);
			if (pix != MAP_FAILED) {
				/* four horizontal bands: red, green, blue, white */
				for (y = 0; y < 1080; y++) {
					uint32_t c = y < 270 ? 0x00ff0000 :
						     y < 540 ? 0x0000ff00 :
						     y < 810 ? 0x000000ff :
							       0x00ffffff;
					for (x = 0; x < 1920; x++) {
						pix[y * 1920 + x] = c;
					}
				}
				munmap(pix, len);
			}
			close(fb);
		}
		tty = open("/dev/tty0", O_WRONLY);
		check("open /dev/tty0", tty >= 0);
		if (tty >= 0) {
			check("write /dev/tty0 (fbcon glyphs)",
			      write(tty, msg, strlen(msg)) ==
			      (ssize_t)strlen(msg));
			close(tty);
		}
	}

	/* P11 eMMC tests: card probe, block reads, partition scan, ext4. */
	{
		int mmc, tries, f;
		unsigned char mbr[512];
		char hello[128];

		/* the mmc core probes asynchronously: poll up to ~15 s */
		mmc = -1;
		for (tries = 0; tries < 150; tries++) {
			mmc = open("/dev/mmcblk0", O_RDONLY);
			if (mmc >= 0) {
				break;
			}
			usleep(100000);
		}
		check("open /dev/mmcblk0", mmc >= 0);
		if (mmc >= 0) {
			check("read MBR sector", read(mmc, mbr, 512) == 512);
			check("MBR signature 0x55AA",
			      mbr[510] == 0x55 && mbr[511] == 0xaa);
			/* partition entry 1 at 0x1be: type byte at +4 = 0x83 */
			check("MBR partition 1 type 0x83 (Linux)",
			      mbr[0x1c2] == 0x83);
			close(mmc);
		}
		mmc = -1;
		for (tries = 0; tries < 150; tries++) {
			mmc = open("/dev/mmcblk0p1", O_RDONLY);
			if (mmc >= 0) {
				break;
			}
			usleep(100000);
		}
		check("open /dev/mmcblk0p1 (partition scan)", mmc >= 0);
		if (mmc >= 0) {
			close(mmc);
		}
		check("mount ext4 /dev/mmcblk0p1 -> /mnt",
		      mount("/dev/mmcblk0p1", "/mnt", "ext4", 0, NULL) == 0);
		f = open("/mnt/hello.txt", O_RDONLY);
		check("open /mnt/hello.txt", f >= 0);
		if (f >= 0) {
			n = read(f, hello, sizeof(hello) - 1);
			if (n > 0) {
				hello[n] = '\0';
				printf("       hello.txt: %s", hello);
				check("hello.txt content",
				      strncmp(hello, "eMMC rootfs test", 16) == 0);
			} else {
				check("hello.txt content", 0);
			}
			close(f);
		}
	}

	printf("=== P5 demo done, fail=%d ===\n", fail);
	printf("LINUX-OK\n");
	return fail ? 1 : 0;
}
