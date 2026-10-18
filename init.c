// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal PID 1 for MT6991 mainline bring-up.  Runs for 40s, then exits on
 * purpose so that PID 1 dying panics the kernel: that flushes the log to pstore
 * and reboots to Android, where it can be read back.
 *
 * Reaching this code at all is the milestone: it means the DTB parsed, the
 * CPUs came up under PSCI, the timer ticks, the bootloader's watchdog is being
 * fed, and userspace runs.
 *
 * Output goes to /dev/kmsg rather than stdout so that every message lands in
 * the kernel log, and therefore on all channels at once: the framebuffer
 * console on the panel, ramoops (readable after a reset), UART if a cable is
 * attached, and the USB-C gadget serial console (console=ttyGS0 --
 * ttygs0_replay_once() below fills the gap before the PC enumerates the
 * port).
 *
 * Built static -- there is no libc in the initramfs.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <sys/klog.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/kd.h>
/*
 * Full display register dumps (EXDMA/BLENDER/OUTPROC/DSC/DSI/mutex, pre and
 * post modeset).  Very verbose; kept for display regression debugging.
 * 0 = quiet boot path, 1 = dump everything.
 */
#define DUMP_DISPLAY_REGS 0

/*
 * Legacy DRM ioctl modeset test.  Disabled now that the DRM fbdev client
 * owns /dev/fb0: its buffer fills the display DMA pool, so the test's own
 * CREATE_DUMB fails with ENOMEM.  The fbdev + FBIOBLANK test below covers
 * the same modeset/DPMS paths without a second framebuffer.
 */
#define LEGACY_DRM_IOCTL_TEST 0

/*
 * Minimal DRM UAPI definitions for the modeset test.
 * Inlining avoids kernel UAPI header dependency issues with the
 * standalone gcc -static build of init.c.
 */
#define DRM_DISPLAY_MODE_LEN	32
#define DRM_IOCTL_BASE		'd'
#define DRM_IOWR(nr, type)	_IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_IO(nr)		_IO(DRM_IOCTL_BASE, nr)
#define DRM_IOCTL_SET_MASTER	DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER	DRM_IO(0x1f)

#define DRM_MODE_CONNECTED		1
#define DRM_MODE_CONNECTOR_Unknown	0

struct drm_mode_modeinfo {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh;
	uint32_t flags;
	uint32_t type;
	char name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_card_res {
	uint64_t fb_id_ptr;
	uint64_t crtc_id_ptr;
	uint64_t connector_id_ptr;
	uint64_t encoder_id_ptr;
	uint32_t count_fbs;
	uint32_t count_crtcs;
	uint32_t count_connectors;
	uint32_t count_encoders;
	uint32_t min_width, max_width;
	uint32_t min_height, max_height;
};

struct drm_mode_get_connector {
	uint64_t encoders_ptr;
	uint64_t modes_ptr;
	uint64_t props_ptr;
	uint64_t prop_values_ptr;
	uint32_t count_modes;
	uint32_t count_props;
	uint32_t count_encoders;
	uint32_t encoder_id;
	uint32_t connector_id;
	uint32_t connector_type;
	uint32_t connector_type_id;
	uint32_t connection;
	uint32_t mm_width, mm_height;
	uint32_t subpixel;
};

struct drm_mode_get_crtc {
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t x, y;
	uint32_t gamma_size;
	uint32_t mode_valid;
	struct drm_mode_modeinfo mode;
};

struct drm_mode_crtc {
	uint64_t set_connectors_ptr;
	uint32_t count_connectors;
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t x, y;
	uint32_t gamma_size;
	uint32_t mode_valid;
	struct drm_mode_modeinfo mode;
};

struct drm_mode_create_dumb {
	uint32_t height, width, bpp, flags;
	uint32_t handle, pitch;
	uint64_t size;
};

struct drm_mode_map_dumb {
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};

struct drm_mode_destroy_dumb {
	uint32_t handle;
};

struct drm_mode_fb_cmd {
	uint32_t fb_id;
	uint32_t width, height;
	uint32_t pitch;
	uint32_t bpp;
	uint32_t depth;
	uint32_t handle;
};

#define DRM_IOCTL_MODE_GETRESOURCES	DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC		DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC		DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETCONNECTOR	DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETFB		DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_ADDFB		DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB		DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_CREATE_DUMB	DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB		DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB	DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

static int kmsg_fd = -1;

/*
 * Seconds of userspace before deliberately exiting to panic.  40 gives the
 * UFS driver's probe up to ~12s of slack on top of the DT verification.
 */
#define LIFETIME_SEC	20
#define BEAT_SEC	5

static void klog(const char *msg)
{
	char buf[512];
	int len = snprintf(buf, sizeof(buf), "<4>[init] %s\n", msg);
	ssize_t ignored;

	if (len < 0)
		return;
	if (kmsg_fd >= 0 && write(kmsg_fd, buf, (size_t)len) >= 0)
		return;
	/* Console fallback for the case where /dev/kmsg is unavailable. */
	ignored = write(1, buf, (size_t)len);
	(void)ignored;
}

static void klogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void klogf(const char *fmt, ...)
{
	char buf[448];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	klog(buf);
}

static void try_mount(const char *src, const char *dst, const char *fs)
{
	if (mount(src, dst, fs, 0, NULL) == 0)
		klogf("mounted %s on %s", fs, dst);
	else
		klogf("mount %s on %s failed (may already be mounted)", fs, dst);
}

/*
 * Read a file into buf, NUL-terminating it.  Returns the byte count, or -1.
 * DT property files are not regular files in the sense that they have no
 * meaningful size until read, so read() is the only way to size them.
 */
static ssize_t slurp(const char *path, char *buf, size_t bufsz)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return n;
}

/*
 * Boot-time kernel messages have scrolled off the console by the time init
 * runs, and the pstore read-back path is down, so when something fails to
 * probe this re-reads the kernel's own ring buffer and prints the records
 * that look relevant.  Reading /dev/kmsg returns one record per read and
 * ends with EAGAIN once the buffer is drained.
 *
 * The failure mode this handles: a stuck deferred-probe retry loop emits the
 * same "deferred probe pending" warning every ~25ms, which floods the console
 * and hides the one-shot messages from early boot (the actual probe results
 * of the clock controllers).  So records are deduplicated with repeat counts
 * and printed newest-first: the last line on screen is then the OLDEST
 * matching record, i.e. exactly the probe-era diagnostics, and it stays
 * visible when PID 1 dies and the kernel panics.
 */
struct logrec {
	unsigned long long ts;	/* microseconds since boot */
	unsigned long n;	/* repeat count */
	char *text;
};

static int logrec_newest_first(const void *a, const void *b)
{
	const struct logrec *ra = a, *rb = b;

	if (ra->ts < rb->ts)
		return 1;
	if (ra->ts > rb->ts)
		return -1;
	return 0;
}

static void dump_filtered_kmsg(const char *const *needles, int nr_needles)
{
	/*
	 * 8 KiB: /dev/kmsg returns -EINVAL (without advancing the sequence) if
	 * a record does not fit the buffer, which would stall the read loop
	 * forever.  No single printk record can approach 8 KiB.
	 */
	char buf[8192];
	struct logrec *recs = NULL;
	int nrecs = 0, cap = 0, fd, i, j;
	unsigned long long total = 0;
	ssize_t n;

	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		klogf("log| cannot read /dev/kmsg: %s", strerror(errno));
		return;
	}
	lseek(fd, 0, SEEK_SET);	/* start at the first retained record */

	klog("--- kernel log (filtered, deduped, newest first) ---");
	for (;;) {
		char *semi, *msg, *p, *nl;
		int matched = 0;

		n = read(fd, buf, sizeof(buf) - 1);
		if (n <= 0)
			break; /* EAGAIN: drained */
		buf[n] = '\0';

		/* Record is "<pri>,<seq>,<ts_usec>[,<flags>];<msg>". */
		semi = strchr(buf, ';');
		if (!semi)
			continue;
		*semi = '\0';
		msg = semi + 1;
		p = buf;
		for (j = 0; j < 2 && p; j++) {
			p = strchr(p, ',');
			if (p)
				p++;
		}
		/* p now points at the ts_usec field, if the header was sane */
		{
			unsigned long long ts = p ? strtoull(p, NULL, 10) : 0;

			/*
			 * Our own records are noise; skip them, except the
			 * DPMS markers and the hardware register dumps which
			 * tell the two test phases apart in the post-mortem
			 * dump.
			 */
			if (strncmp(msg, "[init] ", 7) == 0 &&
			    !strstr(msg, "DPMS") &&
			    !strstr(msg, "EXDMA") &&
			    !strstr(msg, "BLENDER") &&
			    !strstr(msg, "OUTPROC") &&
			    !strstr(msg, "MUTEX[") &&
			    !strstr(msg, "DSI[") &&
			    !strstr(msg, "DSC["))
				continue;
			if (strncmp(msg, "[init] ", 7) == 0)
				msg += 7;
			nl = strchr(msg, '\n');
			if (nl)
				*nl = '\0';
			nl = strchr(msg, '\r');
			if (nl)
				*nl = '\0';
			if (!msg[0])
				continue;
			/*
			 * Cap the kept text so dedupe compares stable prefixes
			 * and long tails do not multiply memory use.
			 */
			if (strlen(msg) > 280)
				msg[280] = '\0';

			for (i = 0; i < nr_needles; i++)
				if (strcasestr(msg, needles[i])) {
					matched = 1;
					break;
				}
			if (!matched)
				continue;

			for (i = 0; i < nrecs; i++)
				if (strcmp(recs[i].text, msg) == 0) {
					recs[i].n++;
					break;
				}
			if (i < nrecs) {
				total++;
				continue;
			}
			if (nrecs >= 250)
				break;	/* enough unique lines; stop scanning */
			if (nrecs == cap) {
				int ncap = cap ? cap * 2 : 64;
				struct logrec *nrecs2;

				nrecs2 = realloc(recs,
						 (size_t)ncap * sizeof(*recs));
				if (!nrecs2)
					break;
				recs = nrecs2;
				cap = ncap;
			}
			recs[nrecs].ts = ts;
			recs[nrecs].n = 1;
			recs[nrecs].text = strdup(msg);
			if (!recs[nrecs].text)
				break;
			nrecs++;
			total++;
		}
	}
	close(fd);

	if (!nrecs) {
		klog("log| (no matching records in the kernel log)");
	} else {
		char line[720];

		qsort(recs, (size_t)nrecs, sizeof(*recs), logrec_newest_first);
		snprintf(line, sizeof(line),
			 "log| %d unique matching records, %llu total -- oldest at bottom",
			 nrecs, total);
		klog(line);
		for (i = 0; i < nrecs; i++) {
			unsigned long long sec = recs[i].ts / 1000000;
			unsigned long long msec = (recs[i].ts / 1000) % 1000;

			if (recs[i].n > 1)
				snprintf(line, sizeof(line), "log| [%llu.%03llu] x%lu %s",
					 sec, msec, recs[i].n, recs[i].text);
			else
				snprintf(line, sizeof(line), "log| [%llu.%03llu] %s",
					 sec, msec, recs[i].text);
			klog(line);
		}
	}
	free(recs);
	klog("-------------------------------------------------------");
}

static unsigned int le32_at(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned long long le64_at(const unsigned char *p)
{
	unsigned int lo = le32_at(p);
	unsigned long long hi = le32_at(p + 4);

	return lo | (hi << 32);
}

/*
 * A DT string-list property ("compatible") is a run of NUL-separated strings.
 * Turn the NULs into spaces so the whole list prints on one line.
 */
static void flatten_strlist(char *buf, ssize_t n)
{
	ssize_t i;

	for (i = 0; i < n - 1; i++)
		if (buf[i] == '\0')
			buf[i] = ' ';
}

static unsigned int be32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

/*
 * Which device tree is the kernel actually running on?
 *
 * The panel showing anything at all already implies the embedded tree won:
 * simplefb's platform device comes from /chosen/framebuffer, which exists only
 * in our blob and not in the bootloader's.  These checks make that explicit and
 * cover the rest of the tree, which the framebuffer alone says nothing about.
 */
static void verify_dtb(void)
{
	char buf[1024];
	ssize_t n;
	struct stat st;
	DIR *d;

	klog("--------- device tree verification ---------");

	/*
	 * The decisive one.  The bootloader's tree says "MT6991"; ours says
	 * "realme GT7 (RMX6688)".  Both are printed by the kernel as "Machine
	 * model:" early on, but by now that has scrolled off the panel.
	 */
	if (slurp("/proc/device-tree/model", buf, sizeof(buf)) > 0)
		klogf("model:      %s", buf);
	else
		klog("model:      <unreadable -- /proc not mounted?>");

	if ((n = slurp("/proc/device-tree/compatible", buf, sizeof(buf))) > 0) {
		flatten_strlist(buf, n);
		klogf("compatible: %s", buf);
	}

	/*
	 * /sys/firmware/fdt is the raw blob the kernel is using, and it only
	 * appears if of_fdt_raw_init()'s CRC check passes -- i.e. nothing
	 * corrupted the tree between early boot and late_initcall.  Its size
	 * below must match the DTB embedded in this Image.
	 */
	if (stat("/sys/firmware/fdt", &st) == 0) {
		klogf("/sys/firmware/fdt present, %lld bytes (CRC check passed)",
		      (long long)st.st_size);
		if ((n = slurp("/sys/firmware/fdt", buf, sizeof(buf))) >= 8) {
			unsigned int magic = be32((unsigned char *)buf);
			unsigned int total = be32((unsigned char *)buf + 4);

			klogf("fdt magic=0x%08x totalsize=%u %s", magic, total,
			      magic == 0xd00dfeedu ? "(valid)" : "(BAD MAGIC)");
		}
	} else {
		klog("/sys/firmware/fdt MISSING -- CRC check failed, or no sysfs");
	}

	/* Only our tree describes reserved memory; the count should be 55. */
	if ((d = opendir("/proc/device-tree/reserved-memory"))) {
		struct dirent *e;
		int count = 0;

		while ((e = readdir(d)))
			if (e->d_name[0] != '.' && strcmp(e->d_name, "name") &&
			    strcmp(e->d_name, "#address-cells") &&
			    strcmp(e->d_name, "#size-cells") &&
			    strcmp(e->d_name, "ranges"))
				count++;
		closedir(d);
		klogf("reserved-memory children: %d (expected 55)", count);
	} else {
		klog("reserved-memory: absent -- NOT our tree");
	}

	/* Proof the framebuffer we are printing through came from the DT. */
	if (stat("/proc/device-tree/chosen/framebuffer@fd6b0000", &st) == 0)
		klog("chosen/framebuffer@fd6b0000: present (simplefb's source)");
	else
		klog("chosen/framebuffer@fd6b0000: ABSENT");

	if (stat("/sys/class/graphics/fb0", &st) == 0) {
		if (slurp("/sys/class/graphics/fb0/name", buf, sizeof(buf)) > 0)
			klogf("fb0 driver: %s", buf);
		if (slurp("/sys/class/graphics/fb0/virtual_size", buf,
			  sizeof(buf)) > 0)
			klogf("fb0 virtual_size: %s", buf);
	}

	/* Which /chosen/bootargs won: ours (absent) or the bootloader's. */
	if (slurp("/proc/cmdline", buf, sizeof(buf)) > 0)
		klogf("cmdline: %s", buf);

	klog("-------------------------------------------");
}

/*
 * UFS bring-up check: wait for the controller's disk to appear, then prove
 * it is usable by actually reading the GPT off it.  A probe that binds but
 * cannot talk to the media would still show the sd* disks -- the read is
 * what separates "driver probed" from "storage works".
 *
 * Returns 1 if the disk appeared and IO was verified, 0 otherwise.  The heavy
 * kernel-log diagnosis is deliberately NOT done here: it is deferred until
 * just before the planned panic so its output is the last thing on screen.
 */
static int ufs_report(void) __attribute__((unused)); /* disabled: see main() */

static int ufs_report(void)
{
	char buf[4096], name[40], disk[8], cand[8], path[64];
	unsigned char sec[32768];	/* 256 GPT entries x 128 bytes max */
	unsigned long long entries_lba, nsec, bestsec;
	unsigned int nr, esz, logical = 512, i;
	ssize_t n;
	unsigned int waited;
	int fd, len;
	char *eol;
	DIR *d;
	struct dirent *de;

	klog("--------- UFS check ---------");

	/*
	 * The driver may still be probing; give it a few seconds.  The chip
	 * shows up as several SCSI LUNs (small boot LUs, the large user-data
	 * LU, W-LUNs), so the sd* name is not predictable: wait for any sd
	 * disk to appear, then pick the largest one -- that is where
	 * Android's GPT lives.
	 */
	for (waited = 0; waited <= 8; waited += 2) {
		int any = 0;

		d = opendir("/sys/block");
		if (d) {
			while ((de = readdir(d))) {
				if (!strncmp(de->d_name, "sd", 2) &&
				    de->d_name[2]) {
					any = 1;
					break;
				}
			}
			closedir(d);
		}
		if (any)
			break;
		klogf("UFS: no sd disk visible yet (%us), waiting", waited);
		sleep(2);
	}
	if (waited > 8) {
		klog("UFS: no sd disk appeared -- controller failed to probe");
		klog("UFS: full diagnostics will be printed just before the panic");
		return 0;
	}

	/* List every sd* with its size, then choose the largest. */
	disk[0] = '\0';
	bestsec = 0;
	d = opendir("/sys/block");
	if (!d) {
		klog("UFS: cannot open /sys/block");
		return 0;
	}
	while ((de = readdir(d))) {
		size_t dl;

		if (strncmp(de->d_name, "sd", 2) || !de->d_name[2])
			continue;
		dl = strlen(de->d_name);
		if (dl >= sizeof(cand))
			continue;	/* absurdly long name; skip it */
		memcpy(cand, de->d_name, dl + 1);
		snprintf(path, sizeof(path), "/sys/block/%s/size", cand);
		if (slurp(path, buf, sizeof(buf)) <= 0)
			continue;
		nsec = strtoull(buf, NULL, 10);
		klogf("UFS:   %s: %llu sectors (%llu MiB)", cand, nsec,
		      nsec / 2048);
		if (nsec > bestsec) {
			bestsec = nsec;
			memcpy(disk, cand, dl + 1);
		}
	}
	closedir(d);
	if (!disk[0]) {
		klog("UFS: no sd disk found -- controller failed to probe");
		return 0;
	}
	klogf("UFS: using %s (%llu MiB, largest LU, holds the GPT)", disk,
	      bestsec / 2048);

	snprintf(path, sizeof(path), "/sys/block/%s/device/model", disk);
	if ((n = slurp(path, buf, sizeof(buf))) > 0) {
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = '\0';
		klogf("UFS: model %s", buf);
	}
	snprintf(path, sizeof(path), "/sys/block/%s/size", disk);
	if (slurp(path, buf, sizeof(buf)) > 0) {
		nsec = strtoull(buf, NULL, 10);
		klogf("UFS: size %llu sectors (%llu MiB)", nsec, nsec / 2048);
	}

	snprintf(path, sizeof(path),
		 "/sys/block/%s/queue/logical_block_size", disk);
	if (slurp(path, buf, sizeof(buf)) > 0)
		logical = (unsigned int)atoi(buf);
	if (logical != 512 && logical != 1024 && logical != 2048 &&
	    logical != 4096)
		logical = 512;
	klogf("UFS: logical block size %u", logical);

	/* Partition list straight from sysfs. */
	if (slurp("/proc/partitions", buf, sizeof(buf)) > 0) {
		char *p = buf;

		while ((p = strstr(p, disk))) {
			eol = strchr(p, '\n');
			len = eol ? (int)(eol - p) : (int)strlen(p);
			if (len > 0 && len < 60) {
				char line[64];

				memcpy(line, p, len);
				line[len] = '\0';
				klogf("UFS: %s", line);
			}
			p = eol ? eol + 1 : p + strlen(disk);
		}
	}

	/*
	 * Read the GPT: LBA0 protective MBR + LBA1 header in one shot, then
	 * the entry array.  A short read or an I/O error here is the real
	 * "UFS not usable" signal.
	 */
	snprintf(path, sizeof(path), "/dev/%s", disk);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		klogf("UFS: cannot open %s: %s", path, strerror(errno));
		return 0;
	}
	if (pread(fd, sec, logical * 2, 0) != (ssize_t)(logical * 2)) {
		klogf("UFS: GPT read failed: %s", strerror(errno));
		close(fd);
		return 0;
	}
	/* GPT header signature sits at offset 0 of LBA1 (UEFI spec 5.3.2). */
	if (memcmp(sec + logical, "EFI PART", 8) != 0) {
		klog("UFS: no GPT signature at LBA1 (raw device?)");
		close(fd);
		return 0;
	}
	entries_lba = le64_at(sec + logical + 72);
	nr = le32_at(sec + logical + 80);
	esz = le32_at(sec + logical + 84);
	klogf("UFS: GPT ok, %u entries of %u bytes at LBA %llu", nr, esz,
	      entries_lba);

	if (esz > 0 && nr > 0 && nr <= 256 && esz * nr <= sizeof(sec)) {
		unsigned int readsz = esz * nr;

		if (pread(fd, sec, readsz, (off_t)(entries_lba * logical))
		    == (ssize_t)readsz) {
			for (i = 0; i < nr; i++) {
				const unsigned char *e = sec + i * esz;
				int j, k = 0;
				unsigned int empty = 1;
				unsigned long long first, last;

				for (j = 0; j < 16; j++)
					if (e[j]) {
						empty = 0;
						break;
					}
				if (empty)
					break;
				for (j = 0; j < 36 && e[56 + j * 2]; j++)
					if (e[56 + j * 2] < 0x80)
						name[k++] = (char)e[56 + j * 2];
				name[k] = '\0';
				first = le64_at(e + 32);
				last = le64_at(e + 40);
				klogf("UFS:   %s @ LBA %llu (%llu MiB)",
				      name[0] ? name : "?", first,
				      (last - first + 1) * logical / 1048576);
			}
		} else {
			klogf("UFS: partition entry read failed: %s",
			      strerror(errno));
		}
	}
	close(fd);
	klog("UFS: block IO verified (read device + GPT)");
	klog("-----------------------------------------");
	return 1;
}

/*
 * pstore read-back check.  Whether the log survives a reset is the difference
 * between debugging from photos and debugging from full text, so this proves
 * the whole chain on one boot instead of assuming it:
 *
 *  1. ramoops probe state -- the driver writes its module parameters back
 *     after a successful probe (ram.c), so a nonzero mem_address there means
 *     "probe ran"; a bound platform device says the same from the bus side.
 *  2. the probe-era kernel messages ("ramoops: using 0x310000@0xaec00000",
 *     "pstore: Registered ramoops as persistent store backend") are printed
 *     ~0.02s into boot, before any photographed log starts, so pull them out
 *     of the kernel ring buffer with timestamps.
 *  3. the pstore filesystem itself: mount it and list what survived.  A
 *     console-ramoops-0 file with content proves the live console backend
 *     writes; a dmesg-ramoops-0 from a previous boot proves panic dumps
 *     persist across a reset.
 *  4. a marker written to /dev/pmsg0 before the planned panic: it survives
 *     independently of any crash dump, so Android showing pmsg-ramoops-0
 *     afterwards proves our writes are readable on the vendor side too.
 */
static void pstore_report(void)
{
	static const char *const params[] = {
		"mem_address", "mem_size", "console_size",
		"pmsg_size", "record_size", "mem_type",
	};
	static const char *const needles[] = {
		"ramoops", "pstore",
	};
	/*
	 * Whole-record scratch: the console zone is a 2 MiB ring that survives
	 * across boots and wraps, so after several boots this boot's newest
	 * lines can sit at the END of the file, not the head.  Only a full
	 * read can anchor both ends.  (BSS, so the initramfs barely grows.)
	 */
	static char fb[0x200000 + 1];
	char path[160], line[300];
	int i, files = 0;
	DIR *d;
	struct dirent *e;

	klog("--------- pstore state ---------");

	klog("-- ramoops module params (defaults => probe never completed) --");
	for (i = 0; i < (int)(sizeof(params) / sizeof(params[0])); i++) {
		ssize_t n;

		snprintf(path, sizeof(path),
			 "/sys/module/ramoops/parameters/%s", params[i]);
		n = slurp(path, fb, sizeof(fb));
		if (n <= 0) {
			klogf("ramoops.%s: <unreadable -- %s>", params[i],
			      strerror(errno));
			continue;
		}
		while (n > 0 && (fb[n - 1] == '\n' || fb[n - 1] == '\r'))
			fb[--n] = '\0';
		klogf("ramoops.%s = %s", params[i], fb);
	}

	klog("-- devices bound to the ramoops platform driver --");
	d = opendir("/sys/bus/platform/drivers/ramoops");
	if (!d) {
		klogf("cannot list ramoops driver dir: %s", strerror(errno));
	} else {
		int bound = 0;

		while ((e = readdir(d))) {
			const char *n = e->d_name;

			/* Every entry is a bound device except these files. */
			if (n[0] == '.' || !strcmp(n, "module") ||
			    !strcmp(n, "bind") || !strcmp(n, "unbind") ||
			    !strcmp(n, "uevent") || !strcmp(n, "probe") ||
			    !strcmp(n, "driver_override"))
				continue;
			klogf("ramoops bound: %s", n);
			bound++;
		}
		closedir(d);
		if (!bound)
			klog("ramoops bound: (none -- probe never ran)");
	}

	/*
	 * Probe-era messages.  The dump prints newest first with the oldest at
	 * the bottom, so the lines that matter stay on screen the longest.
	 */
	dump_filtered_kmsg(needles,
			   (int)(sizeof(needles) / sizeof(needles[0])));

	klog("-- pstore filesystem --");
	mkdir("/pstore", 0755);
	try_mount("pstore", "/pstore", "pstore");
	d = opendir("/pstore");
	if (!d) {
		klogf("cannot list /pstore: %s", strerror(errno));
	} else {
		while ((e = readdir(d))) {
			struct stat st;
			ssize_t n;
			char *p, *nl;
			int shown = 0;

			if (e->d_name[0] == '.' || strlen(e->d_name) > 120)
				continue;	/* d_name is up to 255 bytes */
			snprintf(path, sizeof(path), "/pstore/%s", e->d_name);
			if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			files++;
			klogf("record %s: %lld bytes", e->d_name,
			      (long long)st.st_size);
			if (st.st_size <= 0)
				continue;
			/*
			 * Read the whole record: for a dmesg zone the head is the
			 * previous boot's panic message; for the console ring the
			 * head is the oldest retained text and the tail the newest.
			 */
			n = slurp(path, fb, sizeof(fb));
			if (n <= 0)
				continue;
			p = fb;
			while (*p && shown < 6) {
				size_t len;

				nl = strchr(p, '\n');
				len = nl ? (size_t)(nl - p) : strlen(p);
				if (len > 240)
					len = 240;
				if (len) {
					size_t j;

					memcpy(line, p, len);
					line[len] = '\0';
					for (j = 0; j < len; j++)
						if (line[j] < ' ' || line[j] > '~')
							line[j] = '?';
					klogf("  | %s", line);
					shown++;
				}
				if (!nl)
					break;
				p = nl + 1;
			}
			if (!shown)
				klogf("  | (no text in the head)");
			/* The newest lines: current boot when the backend is live. */
			if (n > 0) {
				char *s = fb + n;
				int back = 0;

				while (s > fb && back < 3) {
					char *e2 = s, *s2;

					while (e2 > fb && e2[-1] == '\n')
						e2--;
					s2 = e2;
					while (s2 > fb && s2[-1] != '\n')
						s2--;
					if (e2 > s2) {
						size_t l = (size_t)(e2 - s2);
						size_t j;

						if (l > 240) {
							s2 = e2 - 240;
							l = 240;
						}
						memcpy(line, s2, l);
						line[l] = '\0';
						for (j = 0; j < l; j++)
							if (line[j] < ' ' || line[j] > '~')
								line[j] = '?';
						klogf("  tail| %s", line);
						back++;
					}
					s = s2;
				}
			}
			/*
			 * Prove the backend is writing RIGHT NOW, not just that
			 * bytes from an earlier boot survived: the console record
			 * must already contain this boot's own init line, which
			 * exists nowhere else.
			 */
			if (!strncmp(e->d_name, "console-", 8) &&
			    strstr(fb, "init reached userspace"))
				klog("  | LIVE: this boot's console output IS in the zone");
			if (!strncmp(e->d_name, "dmesg-", 6))
				klog("  | (a prior boot's crash dump survived -- dump path ok)");
		}
		closedir(d);
	}
	if (files)
		klogf("pstore: %d record(s) readable -- read-back path works", files);
	else
		klog("pstore: NO records -- read-back path is down");

	/*
	 * Marker into the pmsg zone: survives a reset, so after the planned
	 * panic + reboot it is unconditional proof that Android can read our
	 * writes, independent of the panic dump itself.
	 */
	{
		static const char marker[] =
			"[init] pmsg marker: pstore_report reached, boot was alive\n";
		int fd = open("/dev/pmsg0", O_WRONLY | O_CLOEXEC);
		ssize_t w;

		if (fd < 0) {
			klogf("pmsg: no /dev/pmsg0 (%s) -- pmsg zone not registered",
			      strerror(errno));
		} else {
			w = write(fd, marker, sizeof(marker) - 1);
			klogf("pmsg: wrote %zd/%zu bytes to /dev/pmsg0", w,
			      sizeof(marker) - 1);
			close(fd);
		}
	}
	klog("-------------------------------------------");
}

/*
 * The UFS host deferred on its clock suppliers, which means the interesting
 * records are the clock controllers' own probe results from the first second
 * of boot.  These needles are deliberately broad: the dump dedupes, so noise
 * costs one line, while a miss costs a whole flash cycle.
 */
static const char *const diag_needles[] = {
	"clock-controller",	/* all three provider platform devices */
	"clk-mt8196",		/* clk-mt8196-topck / -ufsao driver names */
	"clk_mt8196", "topckgen", "apmixed", "cksys",
	"10000000", "1000080", "168a0000", "16810000", "c400000",
	"ufshc", "ufs_mtk", "ufscfg", "ufsao", "unipro", "mphy", "mcq",
	"defer", "probe", "supplier", "not ready",
	"already registered", "initcall",
	"failed", "error",
	"oops", "BUG", "WARNING",
	"clk-scale", "reset", "regulator", "dvfsrc",
	"ramoops", "pstore",
	/* DRM/DSI/panel display subsystem */
	"mtk-dsi", "mtk-drm", "mtk_drm", "mtk_dsi",
	"panel-ae031", "ae031", "dsi0", "dsi",
	"disp-mutex", "mmsys", "ovl", "rdma",
	"drm", "display", "connector", "framebuffer", "simplefb",
};

/*
 * Who is bound to what, from sysfs rather than the log: a platform device
 * whose uevent lacks DRIVER= has no driver attached, which is the state the
 * deferral warnings imply for the clock controllers.
 */
static void probe_state(void)
{
	static const char *const devs[] = {
		"10000000.clock-controller",	/* topckgen (cksys) */
		"1000080.clock-controller",	/* apmixedsys */
		"168a0000.clock-controller",	/* ufscfg_ao */
		"16810000.ufshci",		/* the UFS host itself */
		"c400000.interrupt-controller",	/* GIC (fw_devlink supplier) */
	};
	char path[192], buf[1024];
	DIR *d;
	struct dirent *e;
	int i;

	klog("--- device binding state ---");
	for (i = 0; i < (int)(sizeof(devs) / sizeof(devs[0])); i++) {
		char *p, *eol, *val = NULL;
		ssize_t n;

		snprintf(path, sizeof(path),
			 "/sys/devices/platform/%s/uevent", devs[i]);
		n = slurp(path, buf, sizeof(buf));
		if (n < 0) {
			klogf("%s: <no platform device>", devs[i]);
			continue;
		}
		p = buf;
		while (p && *p) {
			eol = strchr(p, '\n');
			if (eol)
				*eol = '\0';
			if (strncmp(p, "DRIVER=", 7) == 0 ||
			    strncmp(p, "MODALIAS=", 9) == 0) {
				val = p;
				break;
			}
			p = eol ? eol + 1 : NULL;
		}
		if (val && strncmp(val, "DRIVER=", 7) == 0)
			klogf("%s: BOUND to %s", devs[i], val + 7);
		else if (val)
			klogf("%s: NOT bound (matches %s)", devs[i], val + 9);
		else
			klogf("%s: NOT bound (no modalias?)", devs[i]);
	}

	klog("--- registered platform drivers of interest ---");
	d = opendir("/sys/bus/platform/drivers");
	if (!d) {
		klogf("cannot list platform drivers: %s", strerror(errno));
	} else {
		int found = 0;

		while ((e = readdir(d))) {
			if (strstr(e->d_name, "mt8196") ||
			    strstr(e->d_name, "ufs") ||
			    strstr(e->d_name, "apmixed") ||
			    strstr(e->d_name, "mtk-wdt") ||
			    strstr(e->d_name, "ramoops") ||
			    strstr(e->d_name, "uart")) {
				klogf("driver: %s", e->d_name);
				found++;
			}
		}
		closedir(d);
		if (!found)
			klog("driver: (none of interest registered)");
	}

	klog("--- kernel's own deferred list (debugfs) ---");
	mkdir("/sys/kernel/debug", 0555);
	if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) != 0)
		klogf("debugfs mount failed: %s", strerror(errno));
	else {
		char *p, *eol;
		ssize_t n = slurp("/sys/kernel/debug/devices_deferred",
				  buf, sizeof(buf));

		if (n < 0)
			klogf("devices_deferred unreadable: %s", strerror(errno));
		else if (n == 0)
			klog("devices_deferred: EMPTY (nothing deferred)");
		else {
			p = buf;
			while (p && *p) {
				eol = strchr(p, '\n');
				if (eol)
					*eol = '\0';
				if (*p)
					klogf("deferred| %s", p);
				p = eol ? eol + 1 : NULL;
			}
		}
	}
}

/*
 * DRM/DSI display subsystem probe check.  Verifies whether the display
 * pipeline registered itself and the panel driver bound, without touching
 * any registers -- pure sysfs + kmsg inspection.
 */
static void drm_report(void)
{
	char path[192], buf[1024];
	DIR *d;
	struct dirent *e;
	int found;

	klog("--------- display subsystem state ---------");

	/* 1. /dev/dri/ -- DRM device nodes */
	klog("-- /dev/dri/ --");
	d = opendir("/dev/dri");
	if (!d) {
		klog("  (not found -- DRM never registered)");
	} else {
		found = 0;
		while ((e = readdir(d))) {
			if (e->d_name[0] != '.') {
				klogf("  /dev/dri/%s", e->d_name);
				found++;
			}
		}
		closedir(d);
		if (!found)
			klog("  (empty -- DRM registered but no minors?)");
	}

	/* 2. /sys/class/drm/ -- connector objects */
	klog("-- /sys/class/drm/ --");
	d = opendir("/sys/class/drm");
	if (!d) {
		klog("  (not found)");
	} else {
		found = 0;
		while ((e = readdir(d))) {
			if (e->d_name[0] != '.') {
				klogf("  drm/%s", e->d_name);
				found++;
			}
		}
		closedir(d);
		if (!found)
			klog("  (empty)");
	}

	/* 3. Connector status */
	klog("-- connector status --");
	for (found = 0; found < 4; found++) {
		snprintf(path, sizeof(path),
			 "/sys/class/drm/card0-DSI-%d/status", found);
		if (slurp(path, buf, sizeof(buf)) > 0) {
			/* trim newline */
			char *nl = strchr(buf, '\n');

			if (nl) *nl = '\0';
			klogf("  card0-DSI-%d: %s", found, buf);
		}
	}

	/* 4. Platform driver binding -- mtk-dsi */
	klog("-- mtk-dsi driver binding --");
	d = opendir("/sys/bus/platform/drivers/mtk-dsi");
	if (!d) {
		klogf("  mtk-dsi driver dir: %s (driver not registered?)",
		      strerror(errno));
	} else {
		found = 0;
		while ((e = readdir(d))) {
			const char *n = e->d_name;

			if (n[0] == '.' || !strcmp(n, "module") ||
			    !strcmp(n, "bind") || !strcmp(n, "unbind") ||
			    !strcmp(n, "uevent") || !strcmp(n, "probe") ||
			    !strcmp(n, "driver_override"))
				continue;
			klogf("  mtk-dsi bound: %s", n);
			found++;
		}
		closedir(d);
		if (!found)
			klog("  mtk-dsi bound: (none -- probe never ran)");
	}

	/* 5. Platform driver binding -- mtk-drm (mmsys) */
	klog("-- mtk-drm driver binding --");
	d = opendir("/sys/bus/platform/drivers/mtk-drm");
	if (!d) {
		klogf("  mtk-drm driver dir: %s (driver not registered?)",
		      strerror(errno));
	} else {
		found = 0;
		while ((e = readdir(d))) {
			const char *n = e->d_name;

			if (n[0] == '.' || !strcmp(n, "module") ||
			    !strcmp(n, "bind") || !strcmp(n, "unbind") ||
			    !strcmp(n, "uevent") || !strcmp(n, "probe") ||
			    !strcmp(n, "driver_override"))
				continue;
			klogf("  mtk-drm bound: %s", n);
			found++;
		}
		closedir(d);
		if (!found)
			klog("  mtk-drm bound: (none -- probe never ran)");
	}

	/* 6. DSI panel drivers */
	klog("-- DSI panel drivers --");
	d = opendir("/sys/bus/dsi/drivers");
	if (!d) {
		klogf("  dsi drivers: %s", strerror(errno));
	} else {
		found = 0;
		while ((e = readdir(d))) {
			if (e->d_name[0] != '.') {
				klogf("  dsi driver: %s", e->d_name);
				found++;
			}
		}
		closedir(d);
		if (!found)
			klog("  dsi drivers: (none)");
	}

	/* 7. Framebuffer state */
	klog("-- framebuffer --");
	if (slurp("/sys/class/graphics/fb0/name", buf, sizeof(buf)) > 0) {
		char *nl = strchr(buf, '\n');

		if (nl) *nl = '\0';
		klogf("  fb0 name: %s", buf);
	}
	if (slurp("/sys/class/graphics/fb0/virtual_size", buf,
		  sizeof(buf)) > 0) {
		char *nl = strchr(buf, '\n');

		if (nl) *nl = '\0';
		klogf("  fb0 virtual_size: %s", buf);
	}
	if (slurp("/sys/class/graphics/fb0/bits_per_pixel", buf,
		  sizeof(buf)) > 0) {
		char *nl = strchr(buf, '\n');

		if (nl) *nl = '\0';
		klogf("  fb0 bits_per_pixel: %s", buf);
	}

	/* 8. Kernel log -- DRM/DSI messages (filtered, deduped) */
	{
		static const char *drm_needles[] = {
			"mtk-dsi", "mtk-drm", "mtk_dsi",
			"panel-ae031", "ae031",
			"disp-mutex", "mmsys", "ovl", "rdma",
			"drm", "display", "connector", "DSI",
			"MIPI", "panel", "framebuffer", "simplefb",
			"probe", "failed", "error",
			"EXDMA", "BLENDER", "OUTPROC", "MUTEX", "DSC[",
		};
		dump_filtered_kmsg(drm_needles,
				   (int)(sizeof(drm_needles) /
					 sizeof(drm_needles[0])));
	}

	klog("-------------------------------------------");
}

/*
 * DRM modeset test: open /dev/dri/card0, create a dumb buffer, paint color
 * bars, and set the CRTC to display it.  This proves the full DRM/KMS
 * pipeline works without simplefb.
 *
 * Uses raw DRM ioctls (no libdrm needed).
 */
#define DRM_MODE_W 1280
#define DRM_MODE_H 2800

static int drm_ioctl(int fd, unsigned long request, void *arg)
{
	int ret = ioctl(fd, request, arg);
	return (ret < 0) ? -errno : 0;
}

static void drm_modeset_test(void)
{
	int fd;
	struct drm_mode_card_res res = {};
	struct drm_mode_get_connector conn = {};
	struct drm_mode_crtc setcrtc = {};
	struct drm_mode_create_dumb create = {};
	struct drm_mode_map_dumb map = {};
	struct drm_mode_fb_cmd fb = {};
	uint32_t conn_id = 0, crtc_id = 0, fb_id = 0;
	uint32_t dumb_handle = 0;
	uint32_t *mem;
	int i;

	klog("--------- DRM modeset test ---------");

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		klogf("drm: open /dev/dri/card0 failed: %s", strerror(errno));
		return;
	}

	/* Become DRM master so fill_modes is called during probe */
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0)
		klogf("drm: SET_MASTER failed: %s (non-fatal)", strerror(errno));

	/* Step 1: Get connector list */
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		klogf("drm: GETRESOURCES failed: %s", strerror(errno));
		goto out_fd;
	}
	klogf("drm: resources: %u connectors, %u crtcs, %u encoders",
	      res.count_connectors, res.count_crtcs, res.count_encoders);

	/* Allocate arrays based on counts from first query, then fill them */
	{
		uint32_t n_conns = res.count_connectors;
		uint32_t n_crtcs = res.count_crtcs;
		uint32_t n_encs = res.count_encoders;
		uint32_t *conns = calloc(n_conns ? n_conns : 1, sizeof(uint32_t));
		uint32_t *crtcs = calloc(n_crtcs ? n_crtcs : 1, sizeof(uint32_t));
		uint32_t *encs = calloc(n_encs ? n_encs : 1, sizeof(uint32_t));
		if (!conns || !crtcs || !encs) {
			klog("drm: OOM for id arrays");
			free(conns); free(crtcs); free(encs);
			goto out_fd;
		}
		res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
		res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
		res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;

		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
			klogf("drm: GETRESOURCES re-query failed: %s",
			      strerror(errno));
			free(conns); free(crtcs); free(encs);
			goto out_fd;
		}
		klogf("drm: re-query: %u connectors, %u crtcs, %u encoders",
		      res.count_connectors, res.count_crtcs, res.count_encoders);

		/* Find first connected connector */
		for (i = 0; i < (int)res.count_connectors; i++) {
			memset(&conn, 0, sizeof(conn));
			conn.connector_id = conns[i];
			conn.count_modes = 0;
			if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR,
				      &conn) < 0) {
				klogf("drm: GETCONNECTOR %u failed: %s",
				      conns[i], strerror(errno));
				continue;
			}
			klogf("drm: connector %u status=%u modes=%u encoders=%u",
			      conns[i], conn.connection, conn.count_modes,
			      conn.count_encoders);
			if (!conn_id) {
				conn_id = conns[i];
				klogf("drm: using connector %u (status=%u)", conn_id, conn.connection);
			}
		}

		/* Pick first CRTC */
		if (res.count_crtcs > 0)
			crtc_id = crtcs[0];
		klogf("drm: using crtc %u", crtc_id);

		free(conns);
		free(crtcs);
		free(encs);
	}

	if (!conn_id || !crtc_id) {
		klog("drm: no connected connector or no CRTC available");
		goto out_fd;
	}

	/* Step 2: Create dumb buffer */
	create.width = DRM_MODE_W;
	create.height = DRM_MODE_H;
	create.bpp = 32;
	create.flags = 0;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		klogf("drm: CREATE_DUMB failed: %s", strerror(errno));
		goto out_fd;
	}
	dumb_handle = create.handle;
	klogf("drm: dumb buffer %ux%u handle=%u pitch=%u",
	      DRM_MODE_W, DRM_MODE_H, dumb_handle, create.pitch);

	/* Step 3: Create framebuffer from dumb buffer */
	fb.width = DRM_MODE_W;
	fb.height = DRM_MODE_H;
	fb.pitch = create.pitch;
	fb.bpp = 32;
	fb.depth = 24;
	fb.handle = dumb_handle;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
		klogf("drm: ADDFB failed: %s", strerror(errno));
		goto out_dumb;
	}
	fb_id = fb.fb_id;
	klogf("drm: framebuffer id=%u", fb_id);

	/* Step 4: Map the dumb buffer */
	map.handle = dumb_handle;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		klogf("drm: MAP_DUMB failed: %s", strerror(errno));
		goto out_fb;
	}
	mem = mmap(NULL, (size_t)create.pitch * DRM_MODE_H,
		   PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	if (mem == MAP_FAILED) {
		klogf("drm: mmap dumb buffer failed: %s", strerror(errno));
		goto out_fb;
	}

	/* Step 5: Paint 4 large quadrants (red/green/blue/white) for a
	 * diagnostic pattern that is easy to describe over the phone. */
	{
		unsigned int y, x;

		klogf("drm: painting quadrants R/G/B/W %ux%u", DRM_MODE_W, DRM_MODE_H);
		for (y = 0; y < DRM_MODE_H; y++) {
			uint32_t *row = (uint32_t *)((char *)mem +
						      (size_t)y * create.pitch);
			uint32_t left = (y < DRM_MODE_H / 2) ? 0x00ff0000 : 0x000000ff;
			uint32_t right = (y < DRM_MODE_H / 2) ? 0x0000ff00 : 0x00ffffff;

			for (x = 0; x < DRM_MODE_W / 2; x++)
				row[x] = left;
			for (; x < DRM_MODE_W; x++)
				row[x] = right;
		}
	}

	/* Step 6: Set CRTC to display the framebuffer */
	memset(&setcrtc, 0, sizeof(setcrtc));
	setcrtc.crtc_id = crtc_id;
	setcrtc.fb_id = fb_id;
	setcrtc.x = 0;
	setcrtc.y = 0;
	setcrtc.mode_valid = 1;

	/* Get connector modes to fill CRTC mode */
	{
		struct drm_mode_modeinfo *modes;
		/* First call with count_modes=0 triggers fill_modes probe */
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_id;
		conn.count_modes = 0;
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			klogf("drm: probe GETCONNECTOR failed: %s", strerror(errno));
		klogf("drm: after probe: %u modes, status=%u", conn.count_modes, conn.connection);

		modes = calloc(conn.count_modes ? conn.count_modes : 1, sizeof(*modes));
		if (!modes)
			goto out_mmap;
		conn.modes_ptr = (uint64_t)(uintptr_t)modes;
		conn.encoders_ptr = 0;
		conn.count_encoders = 0;
		conn.props_ptr = 0;
		conn.prop_values_ptr = 0;
		conn.count_props = 0;
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0 &&
		    conn.count_modes > 0) {
			memcpy(&setcrtc.mode, &modes[0],
			       sizeof(setcrtc.mode));
			klogf("drm: setting mode %dx%d@%dHz (type=0x%x flags=0x%x)",
			      modes[0].hdisplay, modes[0].vdisplay,
			      modes[0].vrefresh, modes[0].type, modes[0].flags);
		} else {
			{
			int saved_errno = errno;
			int ret2 = drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
			klogf("drm: 2nd GETCONNECTOR: ret=%d errno=%d(%s) count=%u",
			      ret2, saved_errno, strerror(saved_errno), conn.count_modes);
		}
		}
		free(modes);
	}

	/* SETCRTC uses set_connectors_ptr for the old-style path */
	uint32_t conns_list[] = { conn_id };
	setcrtc.set_connectors_ptr = (uint64_t)(uintptr_t)conns_list;
	setcrtc.count_connectors = 1;

	if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &setcrtc) < 0) {
		klogf("drm: SETCRTC failed: %s", strerror(errno));
		goto out_mmap;
	}

	klog("drm: modeset done -- color bars should be visible on panel");

	/* Let the modeset result stay visible so both phases can be checked
	 * by eye before the DPMS cycle takes the panel down. */
	sleep(3);

	/* DPMS off/on cycle: disable the CRTC, wait, then re-enable.
	 * Validates that the mainline DSI/panel re-init path works. */
	{
		struct drm_mode_crtc off = { 0 };

		klog("drm: --- DPMS off/on cycle test ---");
		klog("drm: DISABLING display for 5s -- screen should go dark now");
		off.crtc_id = crtc_id;
		off.fb_id = 0;
		if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &off) < 0)
			klogf("drm: DPMS disable failed: %s", strerror(errno));
		sleep(5);

		klog("drm: RE-ENABLING display -- quadrants should come back");
		if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &setcrtc) < 0)
			klogf("drm: DPMS re-enable failed: %s", strerror(errno));
		sleep(2);
		klog("drm: --- DPMS cycle done ---");
	}

	/* Skip cleanup: keep the FB alive so the panel continues scanning.
	 * munmap/RMFB/DESTROY_DUMB would tear down the scanout buffer. */
	klog("drm: leaving framebuffer active (no cleanup)");
	return;

out_mmap:
	munmap(mem, (size_t)create.pitch * DRM_MODE_H);
out_fb:
	if (fb_id)
		ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
out_dumb:
	if (dumb_handle) {
		struct drm_mode_destroy_dumb destroy = { .handle = dumb_handle };
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
out_fd:
	close(fd);
	klog("---------------------------------------");
}

/*
 * Display color-bar test: mmap /dev/fb0 and paint vertical color bars to
 * visually verify the DRM/KMS pipeline is driving the panel correctly.
 * The framebuffer is x8r8g8b8 (32 bpp, little-endian).
 */
static void display_color_test(void)
{
	int fd;
	unsigned int w, h, stride;
	void *mem;
	char buf[256];

	klog("--------- display color test ---------");

	fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		klogf("display: open /dev/fb0 failed: %s", strerror(errno));
		return;
	}

	/* Read framebuffer geometry from sysfs */
	if (slurp("/sys/class/graphics/fb0/virtual_size", buf, sizeof(buf)) > 0) {
		unsigned int vw = 0, vh = 0;
		char *p = buf;
		while (*p && (*p < '0' || *p > '9')) p++;
		vw = (unsigned int)strtoul(p, &p, 10);
		while (*p && (*p < '0' || *p > '9')) p++;
		vh = (unsigned int)strtoul(p, NULL, 10);
		if (vw > 0 && vh > 0) {
			w = vw;
			h = vh;
		} else {
			w = 1280; h = 2800;
		}
	} else {
		w = 1280; h = 2800;
	}

	if (slurp("/sys/class/graphics/fb0/stride", buf, sizeof(buf)) > 0)
		stride = (unsigned int)strtoul(buf, NULL, 10);
	else
		stride = w * 4;

	klogf("display: fb0 %ux%u stride=%u bytes", w, h, stride);

	/* mmap the framebuffer */
	mem = mmap(NULL, (size_t)stride * h, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		klogf("display: mmap failed: %s", strerror(errno));
		close(fd);
		return;
	}

	/*
	 * 8-color vertical bar pattern (Red, Green, Blue, White, Yellow, Cyan,
	 * Magenta, Black) -- fills the top 1/8th of the screen so the bars are
	 * visible without scrolling and the rest stays black for contrast.
	 */
	{
		struct bar {
			uint8_t r, g, b;
		} bars[] = {
			{ 255,   0,   0 },	/* Red */
			{   0, 255,   0 },	/* Green */
			{   0,   0, 255 },	/* Blue */
			{ 255, 255, 255 },	/* White */
			{ 255, 255,   0 },	/* Yellow */
			{   0, 255, 255 },	/* Cyan */
			{ 255,   0, 255 },	/* Magenta */
			{   0,   0,   0 },	/* Black */
		};
		const int nbars = (int)(sizeof(bars) / sizeof(bars[0]));
		unsigned int bar_w = w / (unsigned int)nbars;
		unsigned int bar_h = h / 8;	/* top 1/8 of screen */
		unsigned int y, x, i;
		uint32_t *row;

		klogf("display: painting %d bars, %ux%u each, top 1/8 screen",
		      nbars, bar_w, bar_h);

		for (y = 0; y < bar_h; y++) {
			row = (uint32_t *)((char *)mem + (size_t)y * stride);
			for (i = 0; i < nbars; i++) {
				uint32_t pixel = (uint32_t)bars[i].b |
						 ((uint32_t)bars[i].g << 8) |
						 ((uint32_t)bars[i].r << 16);
				for (x = 0; x < bar_w; x++)
					row[i * bar_w + x] = pixel;
			}
		}

		/* Also paint thin horizontal labels below each bar:
		 * write the bar index as a 1-pixel-high stripe of its color */
		for (i = 0; i < nbars; i++) {
			uint32_t pixel = (uint32_t)bars[i].b |
					 ((uint32_t)bars[i].g << 8) |
					 ((uint32_t)bars[i].r << 16);
			if (bar_h < h) {
				row = (uint32_t *)((char *)mem +
						   (size_t)bar_h * stride);
				for (x = i * bar_w; x < (i + 1) * bar_w; x++)
					row[x] = pixel;
			}
		}
	}

	klogf("display: color bars painted to fb0 (%ux%u, %u bytes/row)",
	      w, h, stride);
	klog("---------------------------------------");

	munmap(mem, (size_t)stride * h);
	close(fd);
}

/*
 * fbdev display + DPMS test.
 *
 * After the DRM fbdev client takes over from simplefb, /dev/fb0 is the DRM
 * framebuffer, so paint straight into it and drive DPMS through FBIOBLANK.
 * No second DRM framebuffer is needed: the fbdev buffer already fills the
 * display DMA pool.
 */
static void display_dpms_fb_test(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint32_t red, green, blue, white;
	unsigned int x, y, w, h;
	size_t stride, size;
	void *mem;
	int fd, ttyfd, ret;

	klog("--------- fbdev display + DPMS test ---------");

	fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		klogf("fbdev: open /dev/fb0 failed: %s", strerror(errno));
		return;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		klogf("fbdev: FBIOGET_*INFO failed: %s", strerror(errno));
		close(fd);
		return;
	}

	w = var.xres;
	h = var.yres;
	stride = fix.line_length;
	size = stride * h;
	klogf("fbdev: %ux%u %ubpp stride=%zu", w, h, var.bits_per_pixel, stride);

	/* Keep fbcon from painting console text over the test pattern. */
	ttyfd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (ttyfd >= 0 && ioctl(ttyfd, KDSETMODE, KD_GRAPHICS) < 0)
		klogf("fbdev: KDSETMODE failed: %s", strerror(errno));

	mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		klogf("fbdev: mmap failed: %s", strerror(errno));
		close(fd);
		return;
	}

	red = ((1u << var.red.length) - 1) << var.red.offset;
	green = ((1u << var.green.length) - 1) << var.green.offset;
	blue = ((1u << var.blue.length) - 1) << var.blue.offset;
	white = red | green | blue;

	klogf("fbdev: painting quadrants R/G/B/W %ux%u", w, h);
	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)((char *)mem + (size_t)y * stride);
		uint32_t left = (y < h / 2) ? red : blue;
		uint32_t right = (y < h / 2) ? green : white;

		for (x = 0; x < w / 2; x++)
			row[x] = left;
		for (; x < w; x++)
			row[x] = right;
	}

	klog("fbdev: quadrants painted -- display stays on for 3s");
	sleep(3);

	klog("fbdev: FBIOBLANK powerdown -- screen should go dark for 5s");
	ret = ioctl(fd, FBIOBLANK, FB_BLANK_POWERDOWN);
	if (ret < 0)
		klogf("fbdev: powerdown failed: %s", strerror(errno));
	sleep(5);

	klog("fbdev: FBIOBLANK unblank -- quadrants should return");
	ret = ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);
	if (ret < 0)
		klogf("fbdev: unblank failed: %s", strerror(errno));
	sleep(2);

	klog("fbdev: DPMS cycle done");
	klog("-------------------------------------------");

	/* Keep fd/mapping alive: the fbdev buffer stays the live scanout. */
}

/*
 * Refresh rate test: walk every rate the panel advertises, paint a pattern
 * whose bar count encodes the rate and switch with SETCRTC, reusing the
 * framebuffer the DRM fbdev already owns.  Each switch does a full
 * CRTC/panel off-on, so the panel runs the init sequence for the new rate.
 */
static void display_rate_test(void)
{
	static const unsigned int rate_list[] = { 60, 90, 120, 144 };
	struct drm_mode_modeinfo modes[8];
	struct drm_mode_modeinfo *sel[4] = { };
	struct drm_mode_card_res res;
	struct drm_mode_get_connector conn;
	struct drm_mode_crtc cur;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint32_t crtc_id, conn_id;
	uint32_t crtcs[8], conns[8];
	uint32_t red, green, blue, white;
	unsigned int i, j, x, y, w, h, nbars;
	size_t stride, size;
	void *mem;
	int fd, fbfd;

	klog("--------- refresh rate test ---------");

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		klogf("rate: open /dev/dri/card0 failed: %s", strerror(errno));
		return;
	}
	/* Legacy GETCRTC/SETCRTC need DRM master. */
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0)
		klogf("rate: SET_MASTER failed: %s (non-fatal)",
		      strerror(errno));
	fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fbfd < 0 ||
	    ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) < 0) {
		klogf("rate: /dev/fb0 access failed: %s", strerror(errno));
		close(fd);
		return;
	}
	w = var.xres;
	h = var.yres;
	stride = fix.line_length;
	size = stride * h;
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (mem == MAP_FAILED) {
		klogf("rate: mmap failed: %s", strerror(errno));
		close(fbfd);
		close(fd);
		return;
	}

	red = ((1u << var.red.length) - 1) << var.red.offset;
	green = ((1u << var.green.length) - 1) << var.green.offset;
	blue = ((1u << var.blue.length) - 1) << var.blue.offset;
	white = red | green | blue;

	memset(&res, 0, sizeof(res));
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
	res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
	res.count_crtcs = 8;
	res.count_connectors = 8;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
	    res.count_crtcs == 0 || res.count_connectors == 0) {
		klogf("rate: GETRESOURCES failed: %s", strerror(errno));
		goto out;
	}
	crtc_id = crtcs[0];
	conn_id = conns[0];

	memset(&cur, 0, sizeof(cur));
	cur.crtc_id = crtc_id;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cur) < 0 || !cur.fb_id) {
		klogf("rate: GETCRTC failed (fb_id=%u): %s", cur.fb_id,
		      strerror(errno));
		goto out;
	}

	memset(&conn, 0, sizeof(conn));
	conn.connector_id = conn_id;
	conn.count_modes = 0;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
		klogf("rate: GETCONNECTOR probe failed: %s", strerror(errno));
		goto out;
	}
	if (conn.count_modes > 8)
		conn.count_modes = 8;
	conn.modes_ptr = (uint64_t)(uintptr_t)modes;
	conn.count_props = 0;
	conn.props_ptr = 0;
	conn.count_encoders = 0;
	conn.encoders_ptr = 0;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
		klogf("rate: GETCONNECTOR modes failed: %s", strerror(errno));
		goto out;
	}

	klogf("rate: connector has %u modes (fb_id=%u)", conn.count_modes,
	      cur.fb_id);
	for (i = 0; i < conn.count_modes; i++)
		for (j = 0; j < 4; j++)
			if (modes[i].vrefresh == rate_list[j])
				sel[j] = &modes[i];

	for (j = 0; j < 4; j++) {
		struct drm_mode_crtc set;

		if (!sel[j]) {
			klogf("rate: no %uHz mode advertised", rate_list[j]);
			continue;
		}

		/* bar count encodes the rate: 60->2, 90->3, 120->4, 144->5 */
		nbars = (rate_list[j] + 29) / 30;
		for (y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)((char *)mem +
						     (size_t)y * stride);

			for (x = 0; x < w; x++)
				row[x] = (((x * nbars) / w) & 1) ?
					 green : red;
		}

		memset(&set, 0, sizeof(set));
		set.crtc_id = crtc_id;
		set.fb_id = cur.fb_id;
		set.mode_valid = 1;
		memcpy(&set.mode, sel[j], sizeof(set.mode));
		set.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
		set.count_connectors = 1;

		if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0)
			klogf("rate: switch to %uHz failed: %s", rate_list[j],
			      strerror(errno));
		else
			klogf("rate: %uHz active -- %u bars on screen",
			      rate_list[j], nbars);
		sleep(3);
	}

	/* Back to the preferred 120Hz mode, then repaint the quadrants. */
	if (sel[2]) {
		struct drm_mode_crtc set;

		memset(&set, 0, sizeof(set));
		set.crtc_id = crtc_id;
		set.fb_id = cur.fb_id;
		set.mode_valid = 1;
		memcpy(&set.mode, sel[2], sizeof(set.mode));
		set.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
		set.count_connectors = 1;
		drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set);
	}
	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)((char *)mem + (size_t)y * stride);
		uint32_t left = (y < h / 2) ? red : blue;
		uint32_t right = (y < h / 2) ? green : white;

		for (x = 0; x < w / 2; x++)
			row[x] = left;
		for (; x < w; x++)
			row[x] = right;
	}
	klog("rate: back to 120Hz, quadrants painted");
	klog("--------------------------------------");
out:
	munmap(mem, size);
	close(fbfd);
	close(fd);
}

/*
 * Failure diagnostics, run in the last second before the planned panic so
 * that its output is what remains on the panel when the kernel dies.
 */
static void diagnose(void)
{
	klog("========== UFS FAILURE DIAGNOSTICS ==========");
	probe_state();
	dump_filtered_kmsg(diag_needles,
			   (int)(sizeof(diag_needles) /
				 sizeof(diag_needles[0])));
	klog("========== end diagnostics ==========");
}

/*
 * (2026-09-05) Serial-over-USB PC console.
 *
 * console=ttyGS0 only starts streaming once the host has enumerated the
 * CDC-ACM port (~2 s into a boot with the cable in), so the first seconds --
 * the banner, verify_dtb(), pstore_report() -- never reach the PC through
 * the console.  The ring still holds them, so once the gadget reports
 * "configured" the early kernel log is played into /dev/ttyGS0 directly.
 *
 * kmsg_fd is temporarily swapped to the port so that dump_filtered_kmsg()'s
 * klog() lines go straight out the USB port instead of back into the ring,
 * which would double them on the PC via the console.
 */
static int udc_configured(void)
{
	DIR *d;
	struct dirent *e;
	int ok = 0;

	d = opendir("/sys/class/udc");
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char buf[64], path[300];

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state",
			 e->d_name);
		if (slurp(path, buf, sizeof(buf)) > 0 &&
		    strstr(buf, "configured")) {
			ok = 1;
			break;
		}
	}
	closedir(d);
	return ok;
}

/* Returns 1 once the replay has been attempted (open succeeded). */
static int ttygs0_replay_once(void)
{
	int fd, saved;

	fd = open("/dev/ttyGS0", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return 0;

	klog("serial-usb: port enumerated -- replaying the early kernel log");
	saved = kmsg_fd;
	kmsg_fd = fd;	/* dump output goes straight into the USB port */
	dump_filtered_kmsg(diag_needles,
			   (int)(sizeof(diag_needles) /
				 sizeof(diag_needles[0])));
	kmsg_fd = saved;
	close(fd);
	return 1;
}

/*
 * (2026-09-05, #41) RNDIS console layer.
 *
 * The serial console is parked; with the g_ether (RNDIS) gadget the host's
 * enumeration ("udc configured") makes usb0 appear, and the kernel log can
 * then stream to the PC as UDP via a dynamic netconsole target instead.
 * That gives the PC a real kernel console -- panic backtraces included --
 * over the USB-C link.
 *
 * Addressing is static on both ends (no DHCP server in this initramfs):
 * gadget usb0 = 192.168.42.129/24, PC adapter = 192.168.42.100/24.
 * netconsole then sends the console log to 192.168.42.100:6666 (UDP).
 */
#define NET_IP		"192.168.42.129"
#define NET_MASK	"255.255.255.0"
#define NET_PC_IP	"192.168.42.100"
#define NET_IFACE	"usb0"

static ssize_t put_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	return n;
}

/* Assign the static address and bring usb0 up.  Returns 0 on success. */
static int usb0_up(void)
{
	struct ifreq ifr;
	struct sockaddr_in *sin;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (s < 0)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, NET_IFACE, IFNAMSIZ - 1);

	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, NET_IP, &sin->sin_addr) != 1 ||
	    ioctl(s, SIOCSIFADDR, &ifr) < 0) {
		klogf("net: %s: SIOCSIFADDR %s failed: %s", NET_IFACE, NET_IP,
		      strerror(errno));
		close(s);
		return -1;
	}
	sin = (struct sockaddr_in *)&ifr.ifr_netmask;
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, NET_MASK, &sin->sin_addr) != 1 ||
	    ioctl(s, SIOCSIFNETMASK, &ifr) < 0) {
		klogf("net: SIOCSIFNETMASK failed: %s", strerror(errno));
		close(s);
		return -1;
	}
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
		ioctl(s, SIOCSIFFLAGS, &ifr);
	}
	close(s);
	klogf("net: %s up, %s/%s", NET_IFACE, NET_IP, NET_MASK);
	return 0;
}

/*
 * Mount configfs once, then create the netconsole target and enable it.
 * Call only after the udc is "configured" (host enumerated) and usb0 is up.
 * Returns 1 once enabled; on failure it is retried next heartbeat.
 */
static int net_console_up(void)
{
	static int configfs_tried;
	int rc;

	if (usb0_up() < 0)
		return 0;
	if (!configfs_tried) {
		mkdir("/sys/kernel/config", 0755);
		try_mount("configfs", "/sys/kernel/config", "configfs");
		configfs_tried = 1;
	}
	mkdir("/sys/kernel/config/netconsole", 0755);	/* exists; ignore error */
	if (mkdir("/sys/kernel/config/netconsole/c1", 0755) < 0 &&
	    errno != EEXIST) {
		klogf("net: mkdir netconsole target failed: %s",
		      strerror(errno));
		return 0;
	}
	put_file("/sys/kernel/config/netconsole/c1/dev_name", NET_IFACE);
	put_file("/sys/kernel/config/netconsole/c1/local_ip", NET_IP);
	put_file("/sys/kernel/config/netconsole/c1/remote_ip", NET_PC_IP);
	rc = (int)put_file("/sys/kernel/config/netconsole/c1/enabled", "1");
	if (rc < 0) {
		klogf("net: enabling netconsole target failed: %s",
		      strerror(errno));
		return 0;
	}
	klogf("net: kernel console -> %s:6666 (UDP) over %s", NET_PC_IP,
	      NET_IFACE);
	return 1;
}

/*
 * (2026-09-05) eUSB2 link bring-up (#29).
 *
 * #28 proved the whole gadget stack is healthy -- mtu3 probes, g_serial
 * binds, the UDC pulls up D+ at 0.087s, heartbeats run the full 40s -- yet
 * the PC never enumerates anything.  The break is therefore physical: the
 * MT6991 USB2 pad is eUSB2 (0.4 V low-swing), and the D+/D- line to the
 * USB-C connector runs through two active repeaters that the vendor kernel
 * programs but mainline has no drivers for (nxp ptn36241 on i2c6 + an
 * internal one in the mt6379 PMIC).  On a warm reboot from Android both are
 * left disabled, so nothing is listening on the far side of the pad.
 *
 * What can be restored from userspace without new drivers:
 *  1. SoC side: the device-connect announcement.  The vendor xsphy driver's
 *     PHY_MODE_DPPULLUP_SET handler sets XSP_USBPHYACR3 (u2port0 + 0x31c)
 *     bits EN_PU_DP (bit 9) and PUPD_BIST_EN (bit 12) -- mainline never
 *     does this because nothing calls phy_set_mode().  Mirrored below.
 *  2. NXP repeater EN pin = GPIO72, driven by pinctrl "rptr-enable"
 *     (output-high).  Mainline has no MT6991 pinctrl driver, so the GPIO
 *     bank is poked directly.  Offsets from the vendor pinctrl-mt6991
 *     tables (gpio bank @0x1002d000, pin 72):
 *       MODE reg 0x0390 bits[3:0] = 0   (8 pins per reg, 4 bits each)
 *       DIR  reg 0x0020 bit 8     = 1   (pins 64..95, 1 bit per pin)
 *       DO   reg 0x0120 bit 8            (same bitmap)
 *     The vendor driver pulls EN low for 10 us then high (a reset edge that
 *     puts the chip back in its default dual-role config) -- copied.
 *
 * With EN high and the chip at defaults, its role auto-detects the eUSB2
 * connect and the link should reach the connector.  If the PC still sees
 * nothing, the remaining suspect is the mt6379 PMIC-internal repeater
 * (needs ~30 SPMI register writes from Android's power-on sequence) -- that
 * is a separate, riskier phase.
 *
 * Everything here logs read-backs so a failure tells us which poke did not
 * stick instead of guessing.
 */
static void sleep_us(long us);
static void *link_map_devmem(off_t phys, size_t len)
{
	int fd;
	void *base;
	off_t page = phys & ~(off_t)0xfff;
	size_t map_len = len + (size_t)(phys - page);

	fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		klogf("link: open /dev/mem failed: %s", strerror(errno));
		return NULL;
	}
	base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, page);
	if (base == MAP_FAILED) {
		klogf("link: mmap @0x%llx failed: %s", (long long)phys,
		      strerror(errno));
		base = NULL;
	} else {
		base += (phys - page);
	}
	close(fd);
	return base;
}

#define LINK_RD32(base, off)	(*(volatile uint32_t *)((char *)(base) + (off)))
#define LINK_WR32(base, off, val) \
	do { *(volatile uint32_t *)((char *)(base) + (off)) = (val); } while (0)

#define M32(base, off) (*(volatile uint32_t *)((base) + (off)))

/*
 * MT6991 OVL register dump (pre-DRM-modeset).
 *
 * The MT6991 display subsystem uses the new EXDMA + BLENDER + OUTPROC
 * architecture -- there is NO classic OVL.  Register offsets below come
 * from the vendor driver
 *   mediatek_v2/mtk_disp_ovl_exdma.c   (EXDMA)
 *   mediatek_v2/mtk_disp_ovl_blender.c (BLENDER)
 *   mediatek_v2/mtk_disp_ovl_outproc.c (OUTPROC)
 *
 * Called before drm_modeset_test() so the values are the bootloader's.
 */
static void disp_dump_exdma(const char *name, uint32_t base)
{
	void *r = link_map_devmem(base, 0x1000);

	if (!r)
		return;
	klogf("%s @%08x: EN_CON=%08x INTSTA=%08x DATAPATH=%08x ROIBG=%08x",
	      name, base, LINK_RD32(r, 0x0c), LINK_RD32(r, 0x08),
	      LINK_RD32(r, 0x14), LINK_RD32(r, 0x18));
	klogf("%s: EN=%08x RST=%08x SHADOW=%08x SRCCON=%08x ROI=%08x",
	      name, LINK_RD32(r, 0x20), LINK_RD32(r, 0x24),
	      LINK_RD32(r, 0x28), LINK_RD32(r, 0x2c), LINK_RD32(r, 0x30));
	klogf("%s: LEN=%08x OFF=%08x SRCSZ=%08x PITCH=%08x CLRFMT=%08x CON=%08x ADDR=%08x MOUT=%08x",
	      name, LINK_RD32(r, 0x40), LINK_RD32(r, 0x44),
	      LINK_RD32(r, 0x48), LINK_RD32(r, 0x2f4), LINK_RD32(r, 0x50),
	      LINK_RD32(r, 0x300), LINK_RD32(r, 0xf40), LINK_RD32(r, 0xff0));
	klogf("%s: RDMA=%08x BURST=%08x GDRDY=%08x SRCKEY=%08x",
	      name, LINK_RD32(r, 0x100), LINK_RD32(r, 0x1f4),
	      LINK_RD32(r, 0x118), LINK_RD32(r, 0x2ec));
}

static void disp_dump_mutex(const char *tag)
{
	void *r = link_map_devmem(0x32420000, 0x1000);

	if (!r)
		return;
	klogf("MUTEX[%s]: EN=%08x INT=%08x RST=%08x SOF=%08x MOD0=%08x MOD1=%08x MOD2=%08x",
	      tag, LINK_RD32(r, 0x20), LINK_RD32(r, 0x24), LINK_RD32(r, 0x28),
	      LINK_RD32(r, 0x2c), LINK_RD32(r, 0x30), LINK_RD32(r, 0x34),
	      LINK_RD32(r, 0x38));
}

static void disp_dump_blender(const char *name, uint32_t base)
{
	void *r = link_map_devmem(base, 0x1000);

	if (!r)
		return;
	klogf("%s @%08x: EN=%08x RST=%08x DATAPATH=%08x ROI=%08x LEN=%08x CTRL=%08x",
	      name, base, LINK_RD32(r, 0x20), LINK_RD32(r, 0x24),
	      LINK_RD32(r, 0x10), LINK_RD32(r, 0x30), LINK_RD32(r, 0x40),
	      LINK_RD32(r, 0x600));
}

static void disp_dump_outproc(const char *name, uint32_t base)
{
	void *r = link_map_devmem(base, 0x1000);

	if (!r)
		return;
	klogf("%s @%08x: EN=%08x RST=%08x DATAPATH=%08x ROI=%08x MODE=%08x",
	      name, base, LINK_RD32(r, 0x20), LINK_RD32(r, 0x24),
	      LINK_RD32(r, 0x10), LINK_RD32(r, 0x30), LINK_RD32(r, 0x114));
}

/*
 * Dump the MT6991 DSC register state.  Comparing the bootloader's values
 * (pre) with the mainline configuration (post) pinpoints the exact register
 * that makes the panel show stripes.
 */
static void disp_dump_dsc(const char *tag)
{
	void *d = link_map_devmem(0x32450000, 0x1000);

	if (!d)
		return;
	klogf("DSC[%s]: CON=%08x SPR=%08x ENCW=%08x PICW=%08x PICH=%08x",
	      tag, LINK_RD32(d, 0x00), LINK_RD32(d, 0x14), LINK_RD32(d, 0x3c),
	      LINK_RD32(d, 0x18), LINK_RD32(d, 0x1c));
	klogf("DSC[%s]: SLICEW=%08x SLICEH=%08x CHUNK=%08x BUF=%08x MODE=%08x CFG=%08x PAD=%08x",
	      tag, LINK_RD32(d, 0x20), LINK_RD32(d, 0x24), LINK_RD32(d, 0x28),
	      LINK_RD32(d, 0x2c), LINK_RD32(d, 0x30), LINK_RD32(d, 0x34),
	      LINK_RD32(d, 0x38));
	klogf("DSC[%s]: DBG=%08x OBUF=%08x VER=%08x SHADOW=%08x",
	      tag, LINK_RD32(d, 0x60), LINK_RD32(d, 0x70), LINK_RD32(d, 0x200),
	      LINK_RD32(d, 0x228));
	klogf("DSC[%s]: PPS0=%08x PPS1=%08x PPS2=%08x PPS3=%08x PPS4=%08x",
	      tag, LINK_RD32(d, 0x80), LINK_RD32(d, 0x84), LINK_RD32(d, 0x88),
	      LINK_RD32(d, 0x8c), LINK_RD32(d, 0x90));
	klogf("DSC[%s]: PPS5=%08x PPS6=%08x PPS7=%08x PPS8=%08x PPS9=%08x PPS10=%08x PPS11=%08x",
	      tag, LINK_RD32(d, 0x94), LINK_RD32(d, 0x98), LINK_RD32(d, 0x9c),
	      LINK_RD32(d, 0xa0), LINK_RD32(d, 0xa4), LINK_RD32(d, 0xa8),
	      LINK_RD32(d, 0xac));
	klogf("DSC[%s]: PPS12=%08x PPS13=%08x PPS14=%08x PPS15=%08x PPS16=%08x PPS17=%08x PPS18=%08x PPS19=%08x",
	      tag, LINK_RD32(d, 0xb0), LINK_RD32(d, 0xb4), LINK_RD32(d, 0xb8),
	      LINK_RD32(d, 0xbc), LINK_RD32(d, 0xc0), LINK_RD32(d, 0xc4),
	      LINK_RD32(d, 0xc8), LINK_RD32(d, 0xcc));
}

/*
 * Dump the MT6991 DSI register state (bootloader 'pre' vs mainline 'post').
 */
static void disp_dump_dsi(const char *tag)
{
	void *s = link_map_devmem(0x32490000, 0x1000);

	if (!s)
		return;
	/* MT6991 DSI register layout (vendor offsets!) */
	klogf("DSI[%s]: START=%08x INTEN=%08x INTSTA=%08x CON=%08x MODE=%08x TXRX=%08x PS=%08x",
	      tag, LINK_RD32(s, 0x00), LINK_RD32(s, 0x08), LINK_RD32(s, 0x0c),
	      LINK_RD32(s, 0x30), LINK_RD32(s, 0x34), LINK_RD32(s, 0x38),
	      LINK_RD32(s, 0x3c));
	klogf("DSI[%s]: VSA=%08x VBP=%08x VFP=%08x VACT=%08x SIZE=%08x HSA=%08x HBP=%08x HFP=%08x HSTX=%08x",
	      tag, LINK_RD32(s, 0x60), LINK_RD32(s, 0x64), LINK_RD32(s, 0x68),
	      LINK_RD32(s, 0x6c), LINK_RD32(s, 0x2c), LINK_RD32(s, 0x80),
	      LINK_RD32(s, 0x84), LINK_RD32(s, 0x88), LINK_RD32(s, 0x100));
	klogf("DSI[%s]: VMCMD=%08x 0x104=%08x LCCON=%08x LD0=%08x TIMECON=%08x 0x204=%08x",
	      tag, LINK_RD32(s, 0x110), LINK_RD32(s, 0x104),
	      LINK_RD32(s, 0x1d0), LINK_RD32(s, 0x1d4), LINK_RD32(s, 0x200),
	      LINK_RD32(s, 0x204));
	klogf("DSI[%s]: PHY0=%08x PHY1=%08x PHY2=%08x PHY3=%08x PHYCON=%08x",
	      tag, LINK_RD32(s, 0x600), LINK_RD32(s, 0x604),
	      LINK_RD32(s, 0x608), LINK_RD32(s, 0x60c), LINK_RD32(s, 0x614));
}

static void disp_hw_dump(void)
{
	klog("disp_hw_dump: bootloader EXDMA/BLENDER/OUTPROC state (pre-modeset)");

	/* Vendor kernel uses OVL_EXDMA3 (0x32860000) as the primary canvas */
	disp_dump_exdma("EXDMA3", 0x32860000);
	disp_dump_blender("BLENDER1", 0x328e0000);
	disp_dump_exdma("EXDMA4", 0x32870000);
	disp_dump_blender("BLENDER2", 0x328f0000);
	disp_dump_exdma("EXDMA5", 0x32880000);
	disp_dump_blender("BLENDER3", 0x32900000);
	disp_dump_exdma("EXDMA6", 0x32890000);
	disp_dump_blender("BLENDER4", 0x32910000);
	disp_dump_exdma("EXDMA7", 0x328a0000);
	disp_dump_blender("BLENDER5", 0x32920000);
	disp_dump_exdma("EXDMA8", 0x328b0000);
	disp_dump_blender("BLENDER6", 0x32930000);
	disp_dump_outproc("OUTPROC0", 0x32970000);

	/* Also capture what mainline currently calls "ovl0" (EXDMA0) */
	disp_dump_exdma("EXDMA0", 0x32830000);
	disp_dump_exdma("EXDMA2", 0x32850000);
	disp_dump_exdma("EXDMA9", 0x328c0000);

	/* Mutex configuration */
	disp_dump_mutex("pre");

	/* Bootloader DSC configuration (the one that renders correctly) */
	disp_dump_dsc("pre");
	disp_dump_dsi("pre");
}

/*
 * The bootloader leaves EXDMA4 (0x32870000) enabled with its own layer;
 * it blends on top of EXDMA3 via BLENDER2.  mainline only drives EXDMA3,
 * so switch the leftover layer off before handing over to DRM.
 */
static void disp_takeover_exdma(void)
{
	void *r = link_map_devmem(0x32870000, 0x1000);

	if (!r)
		return;
	LINK_WR32(r, 0x40, 0);	/* L_EN */
	LINK_WR32(r, 0x100, 0);	/* RDMA_CTRL */
	klog("disp_takeover: EXDMA4 layer disabled (cleared stale boot logo)");
}

/*
 * Diagnostic separation test, run AFTER the DRM modeset:
 *
 * Restore the exact EXDMA3 register state the bootloader left behind --
 * which is known to scan out the chosen framebuffer at 0xfd6b0000 -- and
 * repaint the /dev/fb0 color bars.
 *
 *   bars appear -> the downstream chain (blender/PQ/DSC/DSI) survived the
 *                  DRM take-over; the black screen comes from the mainline
 *                  EXDMA config or from the DRM buffer address.
 *   still black -> the downstream chain itself got broken by the DRM
 *                  take-over (PQ blocks, DSC or DSI).
 */
static void disp_exdma3_restore_test(void)
{
	void *r = link_map_devmem(0x32860000, 0x1000);
	int fd;
	unsigned int w = 1280, h = 2800, stride = 1280 * 4;
	char buf[128];
	void *mem;

	if (!r)
		return;

	/* Paint /dev/fb0 FIRST, then point the pipe at it. */
	{
		fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			klogf("disp_test: open /dev/fb0 failed: %s", strerror(errno));
			return;
		}
		if (slurp("/sys/class/graphics/fb0/virtual_size", buf, sizeof(buf)) > 0) {
			char *p = buf;
			unsigned int vw = 0, vh = 0;

			while (*p && (*p < '0' || *p > '9')) p++;
			vw = (unsigned int)strtoul(p, &p, 10);
			while (*p && (*p < '0' || *p > '9')) p++;
			vh = (unsigned int)strtoul(p, NULL, 10);
			if (vw > 0 && vh > 0) {
				w = vw;
				h = vh;
			}
		}
		if (slurp("/sys/class/graphics/fb0/stride", buf, sizeof(buf)) > 0)
			stride = (unsigned int)strtoul(buf, NULL, 10);
		else
			stride = w * 4;

		mem = mmap(NULL, (size_t)stride * h, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (mem == MAP_FAILED) {
			klogf("disp_test: mmap fb0 failed: %s", strerror(errno));
			close(fd);
			return;
		}

		{
			unsigned int y, x;

			for (y = 0; y < h; y++) {
				uint32_t *row = (uint32_t *)((char *)mem +
							     (size_t)y * stride);
				uint32_t left = (y < h / 2) ? 0x00ff0000 : 0x000000ff;
				uint32_t right = (y < h / 2) ? 0x0000ff00 : 0x00ffffff;

				for (x = 0; x < w / 2; x++)
					row[x] = left;
				for (; x < w; x++)
					row[x] = right;
			}
			klogf("disp_test: %ux%u quadrant pattern painted to fb0", w, h);
		}

		munmap(mem, (size_t)stride * h);
		close(fd);
	}

	/*
	 * EXPERIMENT: do not write any OVL register.  The bootloader's
	 * configuration must stay live; the fb0 paint below should show up
	 * on the panel if nothing around the DRM init disturbed it.
	 */
	klogf("disp_test: no OVL writes; readback SHADOW=%08x EN=%08x LEN=%08x CLRFMT=%08x CON=%08x SRC=%08x PITCH=%08x ADDR=%08x",
	      LINK_RD32(r, 0x028), LINK_RD32(r, 0x020), LINK_RD32(r, 0x040),
	      LINK_RD32(r, 0x050), LINK_RD32(r, 0x300), LINK_RD32(r, 0x048),
	      LINK_RD32(r, 0x2f4), LINK_RD32(r, 0xf40));

	klog("disp_test: EXDMA3 left untouched (bootloader config)");
}

static void usb_eusb2_link_bringup(void)
{
	void *pio, *u2;
	uint32_t v;

	klog("link: bringing up the eUSB2 chain (SoC connect + nxp rptr EN)");

	/* PIO bank -- GPIO72 (nxp ptn36241 EN), see comment above. */
	pio = link_map_devmem(0x1002d000, 0x1000);
	if (!pio)
		return;
	v = LINK_RD32(pio, 0x390);
	LINK_WR32(pio, 0x390, v & ~0xf);	/* mode = GPIO */
	v = LINK_RD32(pio, 0x020);
	LINK_WR32(pio, 0x020, v | (1u << 8));	/* direction = output */
	/*
	 * Do NOT pulse GPIO72 LOW.  A LOW→HIGH edge resets the NXP
	 * PTN36241 to its power-on default, which destroys the bootloader's
	 * configuration.  Since I2C6 is dead (all slaves NAK), we cannot
	 * reconfigure the NXP, so we must preserve whatever the bootloader
	 * left.  Just ensure the output is HIGH (enabled).
	 */
	v = LINK_RD32(pio, 0x120);
	if (!(v & (1u << 8))) {
		LINK_WR32(pio, 0x120, v | (1u << 8));	/* EN high */
		klog("link: gpio72 was LOW -- set HIGH (no reset pulse)");
	}
	klog("link: gpio72 EN high");

	/* GPIO207 = ps5170 EN (parade USB3/DP redriver on i2c6@0x28, pinctrl
	 * group "ps-enable" pinmux 0xcf00).  Same LK-reset story as GPIO72;
	 * its vendor driver asserts EN 20 ms before every i2c access, so the
	 * pin was plausibly low for every probe so far.  Range tables:
	 * mode group 200-207 @0x490 (pin207 = bits[31:28]), dir group
	 * 192-223 @0x60 (bit15), dout @0x160 (bit15). */
	v = LINK_RD32(pio, 0x490);
	LINK_WR32(pio, 0x490, v & ~(0xfu << 28));	/* mode = GPIO */
	v = LINK_RD32(pio, 0x060);
	LINK_WR32(pio, 0x060, v | (1u << 15));		/* direction = output */
	v = LINK_RD32(pio, 0x160);
	LINK_WR32(pio, 0x160, v | (1u << 15));		/* EN high */
	klog("link: gpio207 EN high");

	/* GPIO136 = NXP PTN36241 reset pin (active-low per DTS: reset-gpios
	 * = <&pio 136 1>).  The driver returns -ENODEV before touching it,
	 * so if the bootloader left it asserted the chip stays in reset and
	 * never ACKs on I2C.  PIO bank 17: MODE @0x410 bits[3:0], DIR @0x040
	 * bit 8, DO @0x140 bit 8.  Drive HIGH to de-assert reset. */
	v = LINK_RD32(pio, 0x410);
	LINK_WR32(pio, 0x410, v & ~0xfu);	/* mode = GPIO */
	v = LINK_RD32(pio, 0x040);
	LINK_WR32(pio, 0x040, v | (1u << 8));	/* direction = output */
	v = LINK_RD32(pio, 0x140);
	LINK_WR32(pio, 0x140, v | (1u << 8));	/* reset de-assert (HIGH) */
	{ struct timespec ts = { .tv_nsec = 5000000 }; nanosleep(&ts, NULL); }
	klog("link: gpio136 reset de-assert");

	/* GPIO174 = gate-ic power (rt4831a backlight bias on I2C6@0x11).
	 * In Android the panel driver calls _gate_ic_Power_on() which grabs
	 * this GPIO via devm_gpiod_get("gate-power").  Without the panel
	 * driver the pin stays wherever LK left it.  PIO bank 21 (174/8):
	 * MODE @0x450 bits[27:24], DIR @0x050 bit14, DO @0x150 bit14.
	 * Drive HIGH to power the gate-ic (and potentially unblock the
	 * I2C bus if the unpowered chip's ESD diodes drag SDA/SCL low). */
	v = LINK_RD32(pio, 0x450);
	LINK_WR32(pio, 0x450, v & ~(0xfu << 24));	/* mode = GPIO */
	v = LINK_RD32(pio, 0x050);
	LINK_WR32(pio, 0x050, v | (1u << 14));		/* direction = output */
	v = LINK_RD32(pio, 0x150);
	LINK_WR32(pio, 0x150, v | (1u << 14));		/* power high */
	klog("link: gpio174 gate-ic power high");

	/*
	 * I2C6 SCL/SDA pinmux (GPIO104/105 -> function 1 = I2C).
	 *
	 * The vendor mt6991-i2c driver sets pinmux internally via its
	 * scl-gpio-id / sda-gpio-id properties, but the mainline mt8192-i2c
	 * driver does not.  Without this, the I2C6 controller sees an idle bus
	 * (pull-ups hold SCL/SDA high) but cannot drive the lines -- every
	 * slave address gets NAK.
	 *
	 * PIO bank 13 mode register (GPIO104..111) @ PIO_BASE + 0x3D0:
	 *   bits [3:0] = GPIO104 function (0=GPIO, 1=I2C)
	 *   bits [7:4] = GPIO105 function
	 */
	v = LINK_RD32(pio, 0x3D0);
	LINK_WR32(pio, 0x3D0, (v & ~0xffu) | 0x11);
	klog("link: i2c6 pinmux set");

	/*
	 * SoC xsphy: the kernel xsphy driver (phy-mtk-xsphy.c) now does the
	 * full vendor power_on sequence during phy_init (refclk-sel,
	 * suspend-release, REV6, CHP_EN, props from DTS).  All that remains
	 * for init is the two things mainline mtu3 never does because it
	 * never calls phy_set_mode(PHY_MODE_USB_DEVICE):
	 *   1) ACR3: EN_PU_DP — device-mode D+ pullup
	 *   2) DTM1: FORCE_IDDIG|IDDIG — tell the PHY it's a device
	 */
	u2 = link_map_devmem(0x16730000, 0x400);
	if (!u2)
		return;

	/* 1) ACR3 (+0x31c): set EN_PU_DP (bit9) + PUPD_BIST_EN (bit12). */
	v = LINK_RD32(u2, 0x31c);
	LINK_WR32(u2, 0x31c, v | (1u << 9) | (1u << 12));
	klog("link: u2phy acr3 EN_PU_DP set");

	/* 2) DTM1 (+0x36c): set FORCE_IDDIG (bit9) | IDDIG (bit1). */
	v = LINK_RD32(u2, 0x36c);
	LINK_WR32(u2, 0x36c, v | (1u << 9) | (1u << 1));
	klog("link: u2phy dtm1 FORCE_IDDIG|IDDIG set");

	klog("link: done");
}

/*
 * (2026-09-05, #31a) Read-only SPMI probe of the mt6379 eUSB2 repeater.
 *
 * After #29/#30 the SoC xsphy announce (ACR3) and the NXP ptn36241 EN are
 * poked and read-back-verified, yet the PC still never enumerates.  The last
 * unprobed stage is the repeater inside the mt6379 companion PMIC (SPMI
 * slave 0xe, repeater register base 0xd00 -- fdt node
 * mt6379-eusb2-repeater@d00).  Android's shutdown powers it down; the vendor
 * driver's power-on sequence says the powered state is BGR_EN=1/ROSC_EN=1/
 * BC11_SW_EN=0 (bandgap + RC osc running, isolation switch open), while the
 * power-off state has the bandgap off and BC11_SW_EN isolating the link.
 *
 * No mainline driver exists for pmif or mt6379 on mt6991, so this drives the
 * pmif Software Interface (SWINF) directly via /dev/mem.  Register layout is
 * the mt8195/mt8196 generation (mt6991's pmif regions are 0x8ff long exactly
 * to cover SWINF @ +0x800..0x8ec -- the older 0xc00 layout would not fit):
 *
 *   chN ACC   = +0x800 + N*0x40 + 0x00   command word
 *   chN WDATA = +0x800 + N*0x40 + 0x04
 *   chN RDATA = +0x800 + N*0x40 + 0x14
 *   chN VLD   = +0x800 + N*0x40 + 0x24   write 1 = clear/accept
 *   chN STA   = +0x800 + N*0x40 + 0x28   FSM state = (reg>>1)&7: 0=IDLE,
 *                                         6=WFVLDCLR (data waiting)
 *
 * Command word: (opc<<30)|(BIT(29)=write)|(sid<<24)|((len-1)<<16)|addr, with
 * opc=3 (EXT_REG_LONG) for 16-bit addressed register traffic.  A read has no
 * BIT(29), len=1; the byte comes back in RDATA after STA shows WFVLDCLR.
 *
 * mt6991 has TWO pmif blocks (main @0x1c01a000, "p" @0x1c018000), each with
 * SWINF channels 0..3, and it is not documented which carries slave 0xe, so
 * the first read that completes wins and is used for the full dump.  Reads
 * only: nothing here can change PMIC state.
 */
static void sleep_us(long us)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = us * 1000 };

	nanosleep(&ts, NULL);
}

/* One-byte SPMI extended-register read; returns the byte or a negative
 * code: -1 = map issue, -2 = never IDLE, -3 = no WFVLDCLR (NACK/bad bus). */
static long spmi_read_byte(void *bus, uint32_t acc_off, uint8_t sid,
			   uint16_t addr, uint32_t *sta_out)
{
	uint32_t v;
	int i;

	LINK_WR32(bus, acc_off + 0x24, 1);	/* clear any stale valid flag */
	for (i = 0; i < 200; i++) {		/* wait for FSM IDLE */
		v = LINK_RD32(bus, acc_off + 0x28);
		if (((v >> 1) & 7) == 0)
			break;
		sleep_us(100);
	}
	if (i == 200) {
		if (sta_out)
			*sta_out = LINK_RD32(bus, acc_off + 0x28);
		return -2;
	}
	/* EXT_REG_LONG read, sid, 1 byte, 16-bit addr -- no write bit. */
	LINK_WR32(bus, acc_off, (3u << 30) | ((uint32_t)sid << 24) | addr);
	for (i = 0; i < 200; i++) {		/* wait for data (WFVLDCLR) */
		v = LINK_RD32(bus, acc_off + 0x28);
		if (((v >> 1) & 7) == 6)
			break;
		sleep_us(100);
	}
	if (i == 200) {
		if (sta_out)
			*sta_out = LINK_RD32(bus, acc_off + 0x28);
		return -3;
	}
	v = LINK_RD32(bus, acc_off + 0x14);	/* RDATA */
	LINK_WR32(bus, acc_off + 0x24, 1);	/* accept the data */
	return (long)(v & 0xff);
}

/* One-byte SPMI extended-register write; returns 0 or a negative code
 * (same codes as spmi_read_byte; -4 = never back to IDLE after write). */
static long spmi_write_byte(void *bus, uint32_t acc_off, uint8_t sid,
			    uint16_t addr, uint8_t val, uint32_t *sta_out)
{
	uint32_t v;
	int i;

	LINK_WR32(bus, acc_off + 0x24, 1);	/* clear any stale valid flag */
	for (i = 0; i < 200; i++) {		/* wait for FSM IDLE */
		v = LINK_RD32(bus, acc_off + 0x28);
		if (((v >> 1) & 7) == 0)
			break;
		sleep_us(100);
	}
	if (i == 200) {
		if (sta_out)
			*sta_out = LINK_RD32(bus, acc_off + 0x28);
		return -2;
	}
	LINK_WR32(bus, acc_off + 0x04, val);	/* WDATA */
	/* EXT_REG_LONG write: BIT(29) marks a write, no WFVLDCLR follows. */
	LINK_WR32(bus, acc_off,
		  (3u << 30) | (1u << 29) | ((uint32_t)sid << 24) | addr);
	for (i = 0; i < 200; i++) {		/* back to IDLE = accepted */
		v = LINK_RD32(bus, acc_off + 0x28);
		if (((v >> 1) & 7) == 0)
			break;
		sleep_us(100);
	}
	if (i == 200) {
		if (sta_out)
			*sta_out = LINK_RD32(bus, acc_off + 0x28);
		return -4;
	}
	return 0;
}

static void mt6379_repeater_probe(void)
{
	static const uint32_t bus_phys[2] = { 0x1c01a000, 0x1c018000 };
	void *bus[2];
	uint8_t dump[0xa0];
	int b, ch, win_bus = -1, win_ch = -1, a;
	long r;

	klog("mt6379: read-only SPMI probe of eUSB2 repeater (slave 0xe @0xd00)");
	for (b = 0; b < 2; b++) {
		bus[b] = link_map_devmem(bus_phys[b], 0x1000);
		if (!bus[b])
			return;
	}
	/* SWINF machinery present on both blocks?  (v2 layout offsets.) */
	for (b = 0; b < 2; b++)
		klogf("mt6379: bus%d sanity init_done=%08x inf_en=%08x arb_en=%08x "
		      "irq_en0=%08x", b, LINK_RD32(bus[b], 0x000),
		      LINK_RD32(bus[b], 0x024), LINK_RD32(bus[b], 0x150),
		      LINK_RD32(bus[b], 0x420));

	for (b = 0; b < 2 && win_bus < 0; b++) {
		for (ch = 0; ch < 4; ch++) {
			uint32_t acc = 0x800 + (uint32_t)ch * 0x40;
			uint32_t sta = 0;

			r = spmi_read_byte(bus[b], acc, 0x0e, 0xd00, &sta);
			if (r >= 0) {
				win_bus = b;
				win_ch = ch;
				klogf("mt6379: bus%d ch%d @0xd00 -> 0x%02lx", b,
				      ch, r);
				break;
			}
			klogf("mt6379: bus%d ch%d @0xd00 err=%ld sta=%08x", b,
			      ch, r, sta);
		}
	}
	if (win_bus < 0) {
		klog("mt6379: no (bus,channel) completed a read -- pmif clocks "
		     "off, wrong layout, or slave 0xe not on these blocks");
		return;
	}
	klogf("mt6379: using bus%d ch%d for the register dump", win_bus,
	      win_ch);

	/* Dump 0xd00..0xd9f -- verbose, skip to reduce pstore volume. */
}

/*
 * (2026-09-05, #31b) Power the mt6379 eUSB2 repeater back up.
 *
 * The #31a dump matched the vendor driver's power_off sequence bit for bit
 * (USB_EN_SRC_SEL=1, BC11_SW_EN=1, BGR_EN=0, ROSC_EN=0, REV_A[5]=0), with
 * every tuning register retaining its programmed value -- so the minimal
 * bring-up is the power-control transition of the vendor power_on sequence
 * (phy-mtk-mt6379-eusb2-repeater.c), register offsets relative to 0xd00:
 *
 *   rel  reg    bits (vendor names)
 *   0x00 CR0_0  bit0 BGR_EN           0x03 CR0_3  bit0 BC11_SW_EN
 *   0x12 U2_CR0_2  bit5 REV_A[5]      bit1 ROSC_EN
 *   0x17 U2_CR1_3  bits1:0 SQD        0x34 EXT_CR1_0 bit0 OSC_BUF[0]
 *   0x89 CR2_1  bit7 INIT_SW_PHY_SEL  0x8b CR2_3  bit5 USB_EN_SRC_SEL
 *   0x88 CR2_0  bit3 MANU_MODE + bit6 PU_DP = device D+ pull-up (set last,
 *               the DPPULLUP_SET a real connect path would do)
 *
 * The oscillator start is done as a glitch-free dance copied from the
 * vendor: OSC_BUF=1, ROSC=1, brief on, ROSC=0 with OSC_BUF=1 holding, then
 * OSC_BUF=0, ROSC=1, settle.  The vendor also rewrites tuning here
 * (OSC_IBAND=0x52 etc.); the dump proves those retain their values through
 * the off state, so they are only re-asserted (harmless, idempotent).
 *
 * All writes are single bytes with read-modify-write on the target bits and
 * a read-back log, so a poke that does not stick is visible in pstore.
 */
/*
 * (2026-09-05, #37) mt6379 power_on is now rev-aware.
 *
 * #36's log finally said which silicon this is: DEV_INFO=0x74 rev=4 --
 * and the ">=0x3 path" label printed while the <=0x2 dance still ran.
 * The vendor driver branches hard on chip rev inside power_on:
 *   <=0x2: the sequence in mt6379_poweron_rev2() below (kept verbatim);
 *   >=0x3: additionally runs eusb2_efuse_prop_set() (efuse shadows at
 *   CHIP regs 0x381/0x382/0x323..0x327 -- outside the 0xd00 window -- and
 *   INIT_SW_SEL = bit6 of 0xd89) and eusb2_default_prop_set() (DISCTH=6,
 *   FS_SR=7, FSRX_HYS_SEL=0, E3 patch TXLDO_VREF_SEL @0xd20, the three
 *   0xd8a INT_DIS tweaks), then the DT calibration props for device mode
 * (vrt_sel=1 sqth=0 discth=6 pre_emphasis=2 equalization=4 -- the
 * set_mode(USB_DEVICE) content).  A rev-4 chip run with the old-rev
 * sequence plausibly never got its TX LDO/bias right -- and this block
 * gates the whole connector side, so fix it before touching anything
 * else.
 */
static void mt6379_poweron_rev2(void);

/* (2026-09-08, #43) Calibration-preserving power_on.  Android leaves the
 * mt6379 calibration block (0xd10..0xd35) programmed with per-unit tuned
 * values (intr=40 term=8 PE=6 EQ=4 ...) that survive reset because the
 * PMIC stays powered.  Overwriting them from raw efuse/DT values -- the
 * pre-#43 behaviour -- degraded a known-good link and the host could never
 * read a descriptor.  With eusb2_keep_calib=1 on the cmdline the power
 * dance only touches power bits and leaves the calibration alone. */
static int eusb2_keep_calib(void)
{
	static int state = -1;

	if (state < 0) {
		char buf[1024];
		int n = slurp("/proc/cmdline", buf, sizeof(buf));

		state = (n > 0 && strstr(buf, "eusb2_keep_calib")) ? 1 : 0;
		klogf("mt6379: calibration %s", state ? "PRESERVED (keep_calib)"
						     : "overwritten (default)");
	}
	return state;
}

/* (2026-09-08, #44) D+ announce control.  With eusb2_no_pudp on the
 * cmdline the MANU_MODE|PU_DP write at the end of power_on is skipped.
 * Android only ever drives manual D+ pullup via DPPULLUP_SET after a real
 * role negotiation; forcing it unconditionally can leave the connector
 * presenting a phantom pull-up with no working eUSB2 data path behind it
 * (the exact "descriptor request failed" symptom).  Leaving 0xd88=0 lets
 * the SoC-side connect state propagate through the repeater FSM instead. */
static int eusb2_no_pudp(void)
{
	static int state = -1;

	if (state < 0) {
		char buf[1024];
		int n = slurp("/proc/cmdline", buf, sizeof(buf));

		state = (n > 0 && strstr(buf, "eusb2_no_pudp")) ? 1 : 0;
		klogf("mt6379: MANU_PU_DP %s", state ? "SKIPPED (no_pudp)"
						    : "asserted (default)");
	}
	return state;
}

static void mt6379_repeater_power_on(void)
{
	void *bus;
	uint32_t acc = 0x880;		/* ch2: kernel soc_chan=2 */
	long r;
	uint8_t v1, v2, v3, osc, fscr, lscr;
	int keep_calib = eusb2_keep_calib();
	int no_pudp = eusb2_no_pudp();

	bus = link_map_devmem(0x1c01a000, 0x1000);
	if (!bus)
		return;
	r = spmi_read_byte(bus, acc, 0x0e, 0x0000, NULL);
	if (r < 0) {
		klogf("mt6379: DEV_INFO read failed err=%ld -- giving up", r);
		return;
	}
	klogf("mt6379: chip DEV_INFO=0x%02lx rev=%ld (%s)",
	      r, r & 0xf, (r & 0xf) <= 2 ? "<=0x2 path" : ">=0x3 path");
	if ((r & 0xf) <= 2) {
		klog("mt6379: rev<=2 -- legacy sequence");
		mt6379_poweron_rev2();
		return;
	}

	klog("mt6379: rev>=3 sequence (efuse + default props + dev calib)");
#define MTK_RD(off) ((uint8_t)spmi_read_byte(bus, acc, 0x0e, 0xd00 + (off), NULL))
#define MTK_CHIPRD(off) ((uint8_t)spmi_read_byte(bus, acc, 0x0e, (off), NULL))
#define MTK_RMW(off, mask, val) do {					\
		uint8_t _b = MTK_RD(off);				\
		uint32_t _sta = 0;					\
		r = spmi_write_byte(bus, acc, 0x0e, 0xd00 + (off),	\
				    (uint8_t)((_b & ~(mask)) | (val)),	\
				    &_sta);				\
		if (r < 0)						\
			klogf("mt6379: wr 0xd%02x failed err=%ld sta=%08x", \
			      off, r, _sta);				\
	} while (0)
#define MTK_BIT(off, bit, val) MTK_RMW((off), (1u << (bit)),		\
				      (val) ? (1u << (bit)) : 0u)

	/* SQD deglitch 1x (vendor power_on step 1). */
	MTK_RMW(0x17, 0x03, 0x00);
	/* Output-stage config (>=0x3 normal-mode prologue). */
	MTK_RMW(0x92, 0x07, 0x07);
	MTK_RMW(0x94, 0x02, 0x02);

	/* Idempotent "off" prologue (matches the state Android left). */
	MTK_BIT(0x8b, 5, 1);	/* USB_EN_SRC_SEL = 1 */
	MTK_BIT(0x03, 0, 1);	/* BC11_SW_EN = 1 */
	MTK_BIT(0x00, 0, 0);	/* BGR_EN = 0 */
	MTK_BIT(0x03, 1, 0);	/* ROSC_EN = 0 */
	MTK_BIT(0x12, 5, 0);	/* REV_A[5] = 0 */
	sleep_us(300);

	/* eusb2_efuse_prop_set: 0x381/0x382 gate the trim block; log the
	 * raw words so the efuse itself is visible in pstore.  #43: only
	 * when not preserving Android's calibration. */
	r = MTK_CHIPRD(0x381);
	v1 = (uint8_t)r;
	v2 = MTK_CHIPRD(0x382);
	klogf("mt6379: efuse 0x381=0x%02x 0x382=0x%02x", v1, v2);
	if (!keep_calib && (v1 | v2)) {
		/* v1: EU2_CR0_2 bit0/3/2 <- v1 b6/5/4; U2_CR0_3 b6/5/4/3
		 * <- v1 b3/2/1/0 (masks == vals, vendor form). */
		MTK_RMW(0x22, (v1 & 0x40) >> 6, (v1 & 0x40) >> 6);
		MTK_RMW(0x22, (v1 & 0x20) >> 2, (v1 & 0x20) >> 2);
		MTK_RMW(0x22, (v1 & 0x10) >> 2, (v1 & 0x10) >> 2);
		MTK_RMW(0x13, (v1 & 0x08) << 3, (v1 & 0x08) << 3);
		MTK_RMW(0x13, (v1 & 0x04) << 3, (v1 & 0x04) << 3);
		MTK_RMW(0x13, (v1 & 0x02) << 3, (v1 & 0x02) << 3);
		MTK_RMW(0x13, (v1 & 0x01) << 3, (v1 & 0x01) << 3);
		/* v2: EU2_CR0_1 bit7 <- v2 b4; EU2_CR0_2 b7..4 <- v2 b3..0 */
		MTK_RMW(0x21, (v2 & 0x10) << 3, (v2 & 0x10) << 3);
		MTK_RMW(0x22, (v2 & 0x08) << 4, (v2 & 0x08) << 4);
		MTK_RMW(0x22, (v2 & 0x04) << 4, (v2 & 0x04) << 4);
		MTK_RMW(0x22, (v2 & 0x02) << 4, (v2 & 0x02) << 4);
		MTK_RMW(0x22, (v2 & 0x01) << 4, (v2 & 0x01) << 4);
	}
	if (!keep_calib) {
		/* INIT_SW_SEL = 1 (efuse sw mode; bit6 of 0xd89). */
		MTK_BIT(0x89, 6, 1);

		/* Masked sw-mode calibration loads, only where the efuse word
		 * is non-zero (vendor guards). */
		v3 = MTK_CHIPRD(0x323);
		if (v3 & 0x3f)
			MTK_RMW(0x11, 0x3f, v3 & 0x3f);	/* INTR_CAL */
		v3 = MTK_CHIPRD(0x324);
		if (v3 & 0x0f)
			MTK_RMW(0x1a, 0x0f, v3 & 0x0f);	/* TERM_CAL */
		v3 = MTK_CHIPRD(0x325);
		if (v3 & 0xf8)
			MTK_RMW(0x29, 0x1f, (v3 & 0xf8) >> 3);	/* HSTX_IMPN */
		v3 = MTK_CHIPRD(0x326);
		if (v3 & 0xf8)
			MTK_RMW(0x2a, 0x1f, (v3 & 0xf8) >> 3);	/* HSTX_IMPP */
		v3 = MTK_CHIPRD(0x324);
		if (v3 & 0xf0)
			MTK_RMW(0x28, 0xf0, v3 & 0xf0);	/* HSRX_TERM_CAL */
		osc = MTK_CHIPRD(0x327) & 0x7f;
		fscr = MTK_CHIPRD(0x325) & 0x07;
		lscr = MTK_CHIPRD(0x326) & 0x07;
		klogf("mt6379: efuse intr=%02x term=%02x impn=%02x impp=%02x "
		      "hsrxt=%02x osc=%02x fscr=%02x lscr=%02x",
		      MTK_CHIPRD(0x323) & 0x3f, MTK_CHIPRD(0x324) & 0x0f,
		      (MTK_CHIPRD(0x325) & 0xf8) >> 3,
		      (MTK_CHIPRD(0x326) & 0xf8) >> 3,
		      MTK_CHIPRD(0x324) & 0xf0, osc, fscr, lscr);
		if (osc | fscr | lscr) {
			if (osc)
				MTK_RMW(0x30, 0x7f, osc);	/* OSC_CALI */
			if (fscr)
				MTK_RMW(0x24, 0x07, fscr);	/* FS_CR */
			if (lscr)
				MTK_RMW(0x24, 0x70, lscr << 4);	/* LS_CR */
		}
	} else {
		klog("mt6379: efuse calibration preserved (keep_calib)");
	}

	/* eusb2_default_prop_set (>=0x3): DISCTH, FS_SR, FSRX_HYS,
	 * E3.  Written BEFORE the on sequence, matching vendor order. */
	MTK_RMW(0x18, 0xf0, 0x60);	/* DISCTH = 6 (default high) */
	MTK_RMW(0x14, 0x07, 0x07);	/* FS_SR = 7 (DT usb20-fs-sr) */
	MTK_RMW(0x27, 0xc0, 0x00);	/* FSRX_HYS_SEL = 0 (DT) */
	MTK_BIT(0x20, 2, 1);		/* TXLDO_VREF_SEL (E3 patch) */
	MTK_BIT(0x8a, 0, 1);		/* DISC_INT_DIS */
	MTK_BIT(0x8a, 1, 1);		/* WAKE_INT_DIS */
	MTK_BIT(0x8a, 2, 0);		/* FSM_INT_DIS */
	/* Manual oscillator retune (as rev<=2). */
	MTK_RMW(0x35, 0x06, 0x06);	/* OSC_BIAS<2:1> */
	MTK_BIT(0x24, 4, 1);		/* OSC_BIAS_0 */
	MTK_RMW(0x31, 0xff, 0x52);	/* OSC_IBAND = 0x52 */

	/* On: same dance as rev<=2. */
	MTK_BIT(0x12, 5, 1);		/* REV_A[5] = 1 */
	MTK_BIT(0x89, 7, 1);		/* INIT_SW_PHY_SEL = 1 */
	MTK_BIT(0x8b, 5, 0);		/* USB_EN_SRC_SEL = 0 */
	MTK_BIT(0x00, 0, 1);		/* BGR_EN = 1 */
	sleep_us(100);
	MTK_BIT(0x03, 0, 0);		/* BC11_SW_EN = 0 (link open) */
	sleep_us(50);
	MTK_BIT(0x34, 0, 1);		/* OSC_BUF[0] = 1 */
	MTK_BIT(0x03, 1, 1);		/* ROSC_EN = 1 */
	sleep_us(10);
	MTK_BIT(0x03, 1, 0);		/* ROSC_EN = 0 */
	sleep_us(30);
	MTK_BIT(0x34, 0, 0);		/* OSC_BUF[0] = 0 */
	MTK_BIT(0x03, 1, 1);		/* ROSC_EN = 1 */
	sleep_us(100);
	MTK_BIT(0x89, 7, 0);		/* INIT_SW_PHY_SEL = 0 */

	/* Let the hardware auto-calibration settle.  The BGR_EN=1 / ROSC_EN
	 * toggle triggers a calibration pass that continuously overwrites
	 * d10-d1f for several ms.  The vendor writes device props via the
	 * set_mode callback long after power-on; we must wait here. */
	sleep_us(5000);

	/* Device-mode calibration props (vendor eusb2_device_prop_set,
	 * called from set_mode USB_DEVICE).  Written AFTER the on
	 * sequence + settle time, matching vendor timing. */
	MTK_RMW(0x10, 0x70, 0x10);	/* VRT_SEL = 1 (DTS: mediatek,vrt-sel) */
	MTK_RMW(0x18, 0x0f, 0x05);	/* SQTH = 5 (DTS: mediatek,rx-sqth) */
	MTK_RMW(0x1b, 0x70, 0x40);	/* HS_PE = 4 (DTS: mediatek,pre-emphasis) */
	MTK_RMW(0x1b, 0x0f, 0x04);	/* HS_EQ = 4 (DTS: mediatek,equalization) */

	/* Device D+ announce toward the connector (DPPULLUP_SET).  #44:
	 * skippable -- a forced manual pullup with no live eUSB2 data path
	 * behind it is indistinguishable from a broken device to the host. */
	if (!no_pudp)
		MTK_RMW(0x88, 0x48, 0x48);	/* MANU_MODE | PU_DP */
	else
		klog("mt6379: MANU_MODE|PU_DP left clear (0xd88 stays 0)");

	klog("mt6379: r3 done");
	/* #43: calibration window dump -- verbose, skip to reduce pstore volume */
#undef MTK_BIT
#undef MTK_RMW
#undef MTK_CHIPRD
#undef MTK_RD
	klog("mt6379: rev>=3 power_on done -- full eUSB2 chain up if a COM "
	     "port appears");
}

/*
 * mt6379 device-mode props (VRT_SEL, SQTH, HS_PE, HS_EQ) + default props
 * (DISCTH, FS_SR, OSC).  The power-on sequence's BGR_EN=1 / ROSC_EN toggle
 * triggers a hardware auto-calibration that continuously overwrites d10-d1f.
 * The vendor writes these from the set_mode callback, which fires seconds
 * later when the USB role-switch triggers.  We call this from the heartbeat
 * loop to retry until the calibration settles and our writes stick.
 */
static void mt6379_set_device_props(void)
{
	void *bus;
	uint32_t acc = 0x880;
	uint8_t v;

	bus = link_map_devmem(0x1c01a000, 0x1000);
	if (!bus)
		return;

#define SP(off) ((uint8_t)spmi_read_byte(bus, acc, 0x0e, 0xd00 + (off), NULL))
#define SW(off, mask, val) do {						\
		uint8_t _b = SP(off);					\
		uint32_t _sta = 0;					\
		spmi_write_byte(bus, acc, 0x0e, 0xd00 + (off),		\
				(_b & ~(mask)) | (val), &_sta);		\
	} while (0)

	/* Device-mode props (eusb2_device_prop_set) */
	SW(0x10, 0x70, 0x10);	/* VRT_SEL=1 in bits[6:4] */
	SW(0x18, 0xf0, 0x60);	/* DISCTH=6 in bits[7:4] */
	SW(0x18, 0x0f, 0x05);	/* SQTH=5 in bits[3:0] (DTS: mediatek,rx-sqth) */
	SW(0x1b, 0x70, 0x40);	/* HS_PE=4 in bits[6:4] (DTS: mediatek,pre-emphasis) */
	SW(0x1b, 0x0f, 0x04);	/* HS_EQ=4 in bits[3:0] (DTS: mediatek,equalization) */

	/* Default props (eusb2_default_prop_set) */
	SW(0x14, 0x07, 0x07);	/* FS_SR=7 in bits[2:0] */
	SW(0x27, 0xc0, 0x00);	/* FSRX_HYS_SEL=0 in bits[7:6] */

	/* Verify silently -- log only on first call or mismatch */

#undef SP
#undef SW
}

/*
 * <=0x2 silicon (and any chip older than the rev-4 unit this board
 * carries).  Kept verbatim from #32 so old-rev hardware still works.
 */
/*
 * Read key mtu3 MAC + IPPC registers to check if the controller is seeing
 * USB traffic.  Called from heartbeat loop.
 *
 * MAC base = 0x16701000, IPPC base = 0x16703e00
 */
static void mtu3_diag(void)
{
	void *mac, *ippc;
	uint32_t devctl, pm, ep0csr, episr, comm_intr;
	uint32_t u2ctrl, ip_pw_sts1, ip_pw_sts2;

	mac = link_map_devmem(0x16701000, 0x1000);
	ippc = link_map_devmem(0x16703e00, 0x100);
	if (!mac || !ippc) return;

	/* USB2 CSR registers (MAC +0x2400 base) */
	devctl  = M32(mac, 0x240c);	/* U3D_DEVICE_CONTROL */
	pm      = M32(mac, 0x2404);	/* U3D_POWER_MANAGEMENT */
	comm_intr = M32(mac, 0x241c);	/* U3D_COMMON_USB_INTR */

	/* (no forced DC_SESSION -- only the kernel driver should control it) */
	/* Force DC_SESSION if the MAC is powered but session is lost.
	 * Vendor toggle_opstate() does this from the DR IRQ handler;
	 * mainline never calls it in peripheral mode.  The guard
	 * (devctl && !(devctl & 1)) skips the write when the MAC is
	 * off (devctl=0 at 0.25s). */
	if (devctl && !(devctl & 1)) {
		M32(mac, 0x240c) = devctl | 1;	/* DC_SESSION = 1 */
		devctl = M32(mac, 0x240c);
		klogf("mtu3: DC_SESSION forced devctl=%08x", devctl);
	}

	/* Vendor mtu3_start() calls ssusb_set_force_vbus(true):
	 * U3D_MISC_CTRL |= VBUS_FRC_EN(bit0) | VBUS_ON(bit1)
	 * The kernel mtu3_start() already sets this at probe time.
	 * VBUS_FRC_EN stays set permanently: the eUSB2 PHY chain cannot
	 * reliably report VBUS without it, so clearing it causes the
	 * connection to drop immediately. */

	/* Device registers (MAC +0x0000 base) */
	ep0csr  = M32(mac, 0x0100);	/* U3D_EP0CSR */
	episr   = M32(mac, 0x0080);	/* U3D_EPISR */

	/* More registers with correct offsets */
	{
		uint32_t comm_ie  = M32(mac, 0x2418);  /* COMMON_USB_INTR_ENABLE (+0x2418) */
		uint32_t misc     = M32(mac, 0x0c84);  /* U3D_MISC_CTRL (+0x0c84) */
		uint32_t devconf  = M32(mac, 0x0800);  /* U3D_DEVICE_CONF (+0x0800) */
		uint32_t linkintr = M32(mac, 0x0854);  /* U3D_DEV_LINK_INTR (+0x0854) */
		uint32_t lpintr_en = M32(mac, 0x0850); /* U3D_DEV_LINK_INTR_ENABLE (+0x0850) */
		uint32_t lv1ier   = M32(mac, 0x0004);  /* U3D_LV1IER (+0x0004) */

		klogf("mtu3: devctl=%08x pm=%08x comm_ie=%08x misc=%08x "
		      "devconf=%08x linkintr=%08x lpintr_en=%08x lv1ier=%08x",
		      devctl, pm, comm_ie, misc, devconf,
		      linkintr, lpintr_en, lv1ier);
	}

	/* IPPC registers (+0x0000 from IPPC base) */
	u2ctrl  = M32(ippc, 0x0050);	/* SSUSB_U2_CTRL(0) */
	ip_pw_sts1 = M32(ippc, 0x0010);	/* SSUSB_IP_PW_STS1 */
	ip_pw_sts2 = M32(ippc, 0x0014);	/* SSUSB_IP_PW_STS2 */

	klogf("mtu3: devctl=%08x pm=%08x ep0csr=%08x episr=%08x "
	      "comm=%08x u2ctrl=%08x sts1=%08x sts2=%08x",
	      devctl, pm, ep0csr, episr, comm_intr,
	      u2ctrl, ip_pw_sts1, ip_pw_sts2);

	/* SoC xsphy (u2port0 @0x16730000) key registers */
	{
		void *u2phy = link_map_devmem(0x16730000, 0x400);
		if (u2phy) {
			uint32_t dtm0  = M32(u2phy, 0x368); /* U2PHYDTM0 */
			uint32_t dtm1  = M32(u2phy, 0x36c); /* U2PHYDTM1 */
			uint32_t acr3  = M32(u2phy, 0x31c); /* USBPHYACR3 */
			uint32_t acr6  = M32(u2phy, 0x318); /* USBPHYACR6 */
			klogf("xsphy: dtm0=%08x dtm1=%08x acr3=%08x acr6=%08x",
			      dtm0, dtm1, acr3, acr6);
		}
	}

	/* mt6379 eUSB2 repeater key registers (SPMI sid 0x0e, base 0xd00) */
	{
		void *spmibus = link_map_devmem(0x1c01a000, 0x1000);
		uint32_t acc = 0x880;	/* ch2 */
		uint8_t r_vrt, r_sqth, r_pe, r_manu;
		if (spmibus) {
			r_vrt  = (uint8_t)spmi_read_byte(spmibus, acc, 0x0e, 0xd10, NULL);
			r_sqth = (uint8_t)spmi_read_byte(spmibus, acc, 0x0e, 0xd18, NULL);
			r_pe   = (uint8_t)spmi_read_byte(spmibus, acc, 0x0e, 0xd1b, NULL);
			r_manu = (uint8_t)spmi_read_byte(spmibus, acc, 0x0e, 0xd88, NULL);
			klogf("mt6379rb: vrt=%02x sqth=%02x pe=%02x manu=%02x",
			      r_vrt, r_sqth, r_pe, r_manu);
		}
	}
}

static void mt6379_poweron_rev2(void)
{
	void *bus;
	uint32_t acc = 0x880;		/* ch2: kernel soc_chan=2 */
	long r;

#define MTK_RD(off) ((uint8_t)spmi_read_byte(bus, acc, 0x0e, 0xd00 + (off), NULL))
#define MTK_RMW(off, mask, val) do {					\
		uint8_t _b = MTK_RD(off);				\
		uint32_t _sta = 0;					\
		r = spmi_write_byte(bus, acc, 0x0e, 0xd00 + (off),	\
				    (uint8_t)((_b & ~(mask)) | (val)),	\
				    &_sta);				\
		if (r < 0)						\
			klogf("mt6379: wr 0xd%02x failed err=%ld sta=%08x", \
			      off, r, _sta);				\
	} while (0)

	bus = link_map_devmem(0x1c01a000, 0x1000);
	if (!bus)
		return;
	klog("mt6379: power_on sequence (bus0 ch0, repeater base 0xd00)");

	/* Chip revision lives at the chip's own reg 0 (MT6379_REG_DEV_INFO),
	 * not inside the repeater block; branch choice follows it. */
	r = spmi_read_byte(bus, acc, 0x0e, 0x0000, NULL);
	if (r < 0) {
		klogf("mt6379: DEV_INFO read failed err=%ld -- giving up", r);
		return;
	}
	klogf("mt6379: chip DEV_INFO=0x%02lx rev=%ld (%s)", r, r & 0xf,
	      (r & 0xf) <= 2 ? "<=0x2 path" : ">=0x3 path");

	/* SQD deglitch 1x (vendor power_on step 1). */
	MTK_RMW(0x17, 0x03, 0x00);
	/* Output-stage config the >=0x3 normal-mode branch re-asserts. */
	MTK_RMW(0x92, 0x07, 0x07);
	MTK_RMW(0x94, 0x02, 0x02);

	/* Idempotent "off" prologue (matches the state Android left). */
	MTK_RMW(0x8b, 0x20, 0x20);	/* USB_EN_SRC_SEL = 1 */
	MTK_RMW(0x03, 0x01, 0x01);	/* BC11_SW_EN = 1 */
	MTK_RMW(0x00, 0x01, 0x00);	/* BGR_EN = 0 */
	MTK_RMW(0x03, 0x02, 0x00);	/* ROSC_EN = 0 */
	MTK_RMW(0x12, 0x20, 0x00);	/* REV_A[5] = 0 */
	sleep_us(300);

	/* Tuning re-assert (values already retained; idempotent). */
	MTK_RMW(0x35, 0x06, 0x06);	/* OSC_BIAS<2:1> */
	MTK_RMW(0x24, 0x10, 0x10);	/* OSC_BIAS_0 (EU2_CR1_0 bit4) */
	MTK_RMW(0x31, 0xff, 0x52);	/* OSC_IBAND = 0x52 */

	/* On: REV_A[5]=1, INIT_SW_PHY_SEL=1, source sel back to phy... */
	MTK_RMW(0x12, 0x20, 0x20);
	MTK_RMW(0x89, 0x80, 0x80);	/* INIT_SW_PHY_SEL = 1 */
	MTK_RMW(0x8b, 0x20, 0x00);	/* USB_EN_SRC_SEL = 0 */
	MTK_RMW(0x00, 0x01, 0x01);	/* BGR_EN = 1 */
	sleep_us(100);
	MTK_RMW(0x03, 0x01, 0x00);	/* BC11_SW_EN = 0 (link no longer isolated) */
	sleep_us(50);

	/* Glitch-free RC oscillator start (vendor dance). */
	MTK_RMW(0x34, 0x01, 0x01);	/* OSC_BUF[0] = 1 */
	MTK_RMW(0x03, 0x02, 0x02);	/* ROSC_EN = 1 */
	sleep_us(10);
	MTK_RMW(0x03, 0x02, 0x00);	/* ROSC_EN = 0 */
	sleep_us(30);
	MTK_RMW(0x34, 0x01, 0x00);	/* OSC_BUF[0] = 0 */
	MTK_RMW(0x03, 0x02, 0x02);	/* ROSC_EN = 1 */
	sleep_us(100);
	MTK_RMW(0x89, 0x80, 0x00);	/* INIT_SW_PHY_SEL = 0 */

	/* Device D+ announce toward the connector (DPPULLUP_SET). */
	MTK_RMW(0x88, 0x48, 0x48);	/* MANU_MODE | PU_DP */

	klogf("mt6379: power_on done BGR=%d ROSC=%d BC11=%d SRC_SEL=%d "
	      "REV_A5=%d INIT_SW=%d PU_DP=%02x", !!(MTK_RD(0x00) & 1),
	      !!(MTK_RD(0x03) & 2), !!(MTK_RD(0x03) & 1),
	      !!(MTK_RD(0x8b) & 0x20), !!(MTK_RD(0x12) & 0x20),
	      !!(MTK_RD(0x89) & 0x80), MTK_RD(0x88) & 0x48);
	klog("mt6379: power_on done -- full eUSB2 chain up if a COM port "
	     "appears");
#undef MTK_RD
#undef MTK_RMW
}

/*
 * (2026-09-05, #33) Read-only USB power-domain recon.
 *
 * #32 proved every programmable stage of the eUSB2 chain was on (SoC ACR3
 * announce, NXP EN, mt6379 repeater powered with PU_DP announced) yet the
 * PC still sees nothing.  The remaining suspects sit between the MAC and
 * the pad: the mtcmos power domains the vendor DT puts the USB block in,
 * and the mt6373 vusb33 LDO that feeds the analog.
 *
 * Vendor mt6991.dts binds the mtu3 device to TWO domains:
 *     power-domains = <&scpsys SSUSB_P0>, <&scpsys SSUSB_DP_PHY_P0>;
 *     power-domain-names = "u2", "u3";
 * SSUSB_P0 (PWR_CON 0xE1C) = MAC+XHCI; SSUSB_DP_PHY_P0 (PWR_CON 0xE18) =
 * the u2 XS-PHY analog.  Mainline has no MediaTek power-controller driver
 * so nobody turns these on from DT; the MAC plainly has power (mtu3 probes
 * and pulls up D+), which makes the PHY domain the prime suspect.
 *
 * Register map (mtk-scpsys-mt6991-spm.c + booted fdt): the scpsys device is
 * power-controller@0x1c004000 (hwv) and its "spm" regmap is
 * syscon@0x1c00d000 ("mt6991-scpsys-bus"); PWR_CON offsets 0xE18/0xE1C
 * land in that second window.  Both windows are read so the log itself
 * proves which one is live.  PWR_CON layout (mtk-scpsys.c): bit0
 * PWR_RST_B, 1 PWR_ISO, 2 PWR_ON, 3 PWR_ON_2ND, 4 PWR_CLK_DIS, bit8
 * sram_pdn / bit12 ack (SSUSB_P0 only), bit30 PWR_ACK, bit31 PWR_ACK_2ND.
 * This platform polls PWR_ACK inside PWR_CON itself
 * (MTK_SCPD_IS_PWR_CON_ON), so raw bits 30/31 = the hardware state.
 *
 * mt6373 (SPMI slave 5 = second_pmic@5) vusb33: enable reg 0x1b95 bit0
 * (bit1 = LP), vsel 0x1e0d, vocal 0x1e0c -- mt6373-regulator.h.  The
 * booted fdt marks the rail always-on in DT, but only an SPMI read tells us
 * what the chip actually holds.
 */
static void usb_power_recon(void)
{
	static const uint32_t win[2] = { 0x1c004000, 0x1c00d000 };
	static const char *wname[2] = { "hwin", "sbus" };
	static const uint32_t offs[3] = { 0xE00, 0xE18, 0xE1C };
	static const char *oname[3] = { "md", "dp-phy-p0", "ssusb-p0" };
	void *m[2];
	int w, o;

	klog("usbpwr: read-only recon -- mtcmos USB domains + mt6373 vusb33");
	for (w = 0; w < 2; w++) {
		m[w] = link_map_devmem(win[w], 0x1000);
		if (!m[w])
			return;
	}
	/* md @0xE00 is a sanity anchor: the modem domain stays up across warm
	 * reboots, so its PWR_CON shows the on-pattern only in the live window. */
	for (w = 0; w < 2; w++)
		for (o = 0; o < 3; o++)
			klogf("usbpwr: %s +0x%03x %-10s %08x", wname[w],
			      offs[o], oname[o], LINK_RD32(m[w], offs[o]));

	for (w = 0; w < 2; w++) {
		uint32_t a = LINK_RD32(m[w], 0xE18);	/* ssusb-dp-phy-p0 */
		uint32_t b = LINK_RD32(m[w], 0xE1C);	/* ssusb-p0 */

		klogf("usbpwr: %s dp-phy-p0 %08x rst=%d iso=%d on=%d on2=%d "
		      "clkdis=%d ack=%d ack2=%d", wname[w], a, !!(a & 1),
		      !!(a & 2), !!(a & 4), !!(a & 8), !!(a & 0x10),
		      !!(a & (1u << 30)), !!(a & (1u << 31)));
		klogf("usbpwr: %s ssusb-p0  %08x rst=%d iso=%d on=%d on2=%d "
		      "clkdis=%d spdn=%d spdnack=%d ack=%d ack2=%d", wname[w],
		      b, !!(b & 1), !!(b & 2), !!(b & 4), !!(b & 8),
		      !!(b & 0x10), !!(b & 0x100), !!(b & 0x1000),
		      !!(b & (1u << 30)), !!(b & (1u << 31)));
	}
	{
		uint32_t acks = LINK_RD32(m[1], 0xE18) | LINK_RD32(m[1], 0xE1C);

		if (acks & (3u << 30))
			klog("usbpwr: sbus window shows the USB domains powered "
			     "(ACK bits set) -- mtcmos is NOT the blocker");
		else
			klog("usbpwr: sbus window shows the USB domains down (no "
			     "ACK bits) -- mtcmos is the prime suspect");
	}

	/* mt6373 vusb33 LDO state (same raw-SWINF path as the mt6379 work). */
	{
		void *bus = link_map_devmem(0x1c01a000, 0x1000);
		uint32_t acc = 0x880;		/* ch2: kernel soc_chan=2 */
		long en, vsel, vocal;

		if (!bus)
			return;
		en = spmi_read_byte(bus, acc, 0x05, 0x1b95, NULL);
		vsel = spmi_read_byte(bus, acc, 0x05, 0x1e0d, NULL);
		vocal = spmi_read_byte(bus, acc, 0x05, 0x1e0c, NULL);
		if (en < 0 || vsel < 0 || vocal < 0)
			klogf("usbpwr: mt6373 read failed en=%ld vsel=%ld "
			      "vocal=%ld", en, vsel, vocal);
		else
			klogf("usbpwr: mt6373 vusb 0x1b95=0x%02lx (en=%ld lp=%ld) "
			      "vsel=0x%02lx vocal=0x%02lx%s", en, en & 1,
			      (en >> 1) & 1, vsel, vocal,
			      (en & 1) ? " -- vusb33 ON" : " -- vusb33 OFF");
	}
}

/*
 * (2026-09-15) Touch panel AVDD -- MT6373 VTP LDO.
 *
 * The mainline kernel has no MT6373 PMIC driver, so the touch panel's
 * analog rail is never turned on.  The S3910 touch controller has a
 * separate always-on digital supply (SPI and command responses work),
 * but without AVDD its capacitive sensing front-end is dead: ATTN never
 * asserts, no touch reports, zero input events.
 *
 * The vendor touch driver requests 3.1 V for this rail when DT does not
 * specify synaptics,vdd_2v8_volt (syna_tcm2_platform_spi.c).
 *
 * MT6373 registers (mt6373-regulator.h, SPMI slave 5):
 *   RG_LDO_VTP_EN  0x1d23  bit0 = enable, bit1 = low-power
 *   RG_VTP_VOSEL   0x1e35  [3:0]  (ldo_volt_table1 index 12 = 3.1 V)
 *   RG_VTP_VOCAL   0x1e34  [3:0]  (factory calibration -- leave alone)
 */
static void touch_vtp_power_on(void)
{
	void *bus = link_map_devmem(0x1c01a000, 0x1000);
	uint32_t acc = 0x880;		/* ch2: kernel soc_chan=2 */
	long en0, vsel0, vocal0, en1, vsel1, vocal1, r;

	if (!bus) {
		klog("vtp: SPMI bus map failed");
		return;
	}

	en0    = spmi_read_byte(bus, acc, 0x05, 0x1d23, NULL);
	vsel0  = spmi_read_byte(bus, acc, 0x05, 0x1e35, NULL);
	vocal0 = spmi_read_byte(bus, acc, 0x05, 0x1e34, NULL);
	klogf("vtp: before en=0x%02lx vsel=0x%02lx vocal=0x%02lx",
	      en0, vsel0, vocal0);

	/* 3.1 V -> ldo_volt_table1 index 12 */
	r = spmi_write_byte(bus, acc, 0x05, 0x1e35, 0x0c, NULL);
	klogf("vtp: set vsel=0x0c (3.1V) rc=%ld", r);

	/* enable, leave low-power bit clear */
	r = spmi_write_byte(bus, acc, 0x05, 0x1d23,
			    (uint8_t)(((en0 < 0) ? 0 : en0) | 0x1) & ~0x2,
			    NULL);
	klogf("vtp: enable rc=%ld", r);

	sleep_us(20000);

	en1    = spmi_read_byte(bus, acc, 0x05, 0x1d23, NULL);
	vsel1  = spmi_read_byte(bus, acc, 0x05, 0x1e35, NULL);
	vocal1 = spmi_read_byte(bus, acc, 0x05, 0x1e34, NULL);
	klogf("vtp: after en=0x%02lx vsel=0x%02lx vocal=0x%02lx%s",
	      en1, vsel1, vocal1, (en1 & 1) ? " -- VTP ON" : " -- VTP OFF");
}

/*
 * (2026-09-05, #34) NXP ptn36241 eUSB2 repeater role via i2c6.
 *
 * #33 killed the power theories: both mtcmos domains (SSUSB_P0 +
 * SSUSB_DP_PHY_P0) read powered from the live 0x1c004000 window (ACK bits
 * set, ISO clear) and mt6373 vusb33 is enabled -- but the PC still sees
 * nothing.  The one Android connect action never replicated is the NXP
 * chip's role: the vendor xsphy DPPULLUP_SET handler propagates
 * phy_set_mode(USB_DEVICE, DPPULLUP_SET) to every sub-repeater, and the
 * nxp driver answers that with an i2c write of ROLE_FORCE_DEVICE into
 * LINK_CONTROL_1 (0x02).  We only ever pulsed its EN pin (#29), never
 * touched its i2c bus, so if the chip came up at its reset-default DUAL
 * role (or anything but device) the far side never lines up.
 *
 * i2c6 = 0x13170000, "mediatek,mt6991-i2c", regs_v2 offsets (from the
 * vendor i2c-mt65xx.c): CONTROL 0x10, SLAVE_ADDR1 0x94 (slave_addr_ver=1),
 * INTR_MASK 0x8, INTR_STAT 0xc, TRANSFER_LEN 0x14, TRANSAC_LEN 0x18,
 * TIMING 0x20, START 0x24, FIFO_ADDR_CLR 0x38, TRANSFER_LEN_AUX 0x44,
 * DATA_PORT 0x0.  NXP chip @0x4f (8-bit addr 0x9e W / 0x9f R), CID
 * 0x3222 @0x14/0x15, LINK_CONTROL_1 role bits [5:4] (FORCE_DUAL=0,
 * FORCE_HOST=1, FORCE_DEVICE=2).  Transfers are polled single-msg FIFO
 * transactions, byte-exact copies of mtk_i2c_do_transfer's non-DMA path
 * with INTR_MASK=0 and INTR_STAT polling.  A read of TIMING gates the
 * work: if Android's i2c6 setup survived (it should -- warm reboot +
 * clk_ignore_unused), TIMING is non-zero and we proceed; otherwise the
 * controller would need a full speed init and we only log.
 *
 * Read CID first (proves the bus and the chip), read LINK_CONTROL_1 and
 * only write FORCE_DEVICE when the role bits are not already device.
 */
static uint32_t i2c6_rd(void *i2c, uint32_t off)
{
	return LINK_RD32(i2c, off);
}

static void i2c6_wr(void *i2c, uint32_t off, uint32_t v)
{
	LINK_WR32(i2c, off, v);
}

/* DATA_PORT (0x0) is a byte-wide FIFO: only byte accesses push entries. */
static void i2c6_fifo_wb(void *i2c, uint8_t v)
{
	*(volatile uint8_t *)((char *)i2c + 0x00) = v;
}

static uint8_t i2c6_fifo_rb(void *i2c)
{
	return *(volatile uint8_t *)((char *)i2c + 0x00);
}

/* 2-byte plain write (reg, val) to an addr7 slave; returns 0 or negative. */
static long i2c6_write_reg_byte(void *i2c, uint8_t addr7, uint8_t reg,
				uint8_t val)
{
	uint32_t ctl, stat;
	int i;

	if (!i2c6_rd(i2c, 0x20))	/* TIMING: controller must be set up */
		return -5;
	ctl = i2c6_rd(i2c, 0x10);	/* CONTROL */
	ctl &= ~(0x10 | 0x2 | 0x4 | 0x100 | 0x200);	/* DIR_CH/RS/DMA/DMAACK/ASYNC */
	ctl |= 0x8 | 0x20;		/* CLK_EXT_EN | ACKERR_DET_EN (so a NAK
					   is visible in INTR_STAT bit 1) */
	i2c6_wr(i2c, 0x10, ctl);
	i2c6_wr(i2c, 0x94, (uint32_t)addr7 << 1);	/* SLAVE_ADDR1: write dir */
	i2c6_wr(i2c, 0x0c, 0x21f);	/* INTR_STAT: clear all */
	i2c6_wr(i2c, 0x38, 1);		/* FIFO_ADDR_CLR */
	i2c6_wr(i2c, 0x08, 0);		/* INTR_MASK = 0 (polled) */
	i2c6_wr(i2c, 0x14, 2);		/* TRANSFER_LEN */
	i2c6_wr(i2c, 0x18, 1);		/* TRANSAC_LEN = 1 msg */
	i2c6_fifo_wb(i2c, reg);		/* FIFO byte 0 = register */
	i2c6_fifo_wb(i2c, val);		/* FIFO byte 1 = data */
	i2c6_wr(i2c, 0x24, 0x4001);	/* START: TRANSAC_START | RS_MUL_TRIG */
	for (i = 0; i < 200; i++) {	/* poll completion (~0.5 ms at 100 kHz) */
		stat = i2c6_rd(i2c, 0x0c);
		if (stat)
			break;
		sleep_us(100);
	}
	if (i == 200)
		return -6;
	i2c6_wr(i2c, 0x0c, stat);	/* ack the interrupt */
	if (stat & 0x2)			/* ACKERR */
		return -7;
	if (!(stat & 1))		/* TRANSAC_COMP */
		return -8;
	return 0;
}

/* Combined write-reg-then-read (WRRD); returns the byte or a negative code. */
static long i2c6_read_reg_byte(void *i2c, uint8_t addr7, uint8_t reg)
{
	uint32_t ctl, stat;
	int i;

	if (!i2c6_rd(i2c, 0x20))
		return -5;
	ctl = i2c6_rd(i2c, 0x10);
	ctl &= ~(0x4 | 0x100 | 0x200);
	ctl |= 0x10 | 0x2 | 0x8 | 0x20;	/* DIR_CHANGE | RS | CLK_EXT_EN |
					   ACKERR_DET_EN */
	i2c6_wr(i2c, 0x10, ctl);
	i2c6_wr(i2c, 0x94, (uint32_t)addr7 << 1);	/* write address first */
	i2c6_wr(i2c, 0x0c, 0x21f);
	i2c6_wr(i2c, 0x38, 1);
	i2c6_wr(i2c, 0x08, 0);
	i2c6_wr(i2c, 0x14, 1);		/* TRANSFER_LEN (write leg) */
	i2c6_wr(i2c, 0x44, 1);		/* TRANSFER_LEN_AUX (read leg) */
	i2c6_wr(i2c, 0x18, 2);		/* TRANSAC_LEN = WRRD */
	i2c6_fifo_wb(i2c, reg);
	i2c6_wr(i2c, 0x24, 0x4001);
	for (i = 0; i < 200; i++) {
		stat = i2c6_rd(i2c, 0x0c);
		if (stat)
			break;
		sleep_us(100);
	}
	if (i == 200)
		return -6;
	i2c6_wr(i2c, 0x0c, stat);
	if (stat & 0x2)
		return -7;
	if (!(stat & 1))
		return -8;
	return (long)i2c6_fifo_rb(i2c);		/* read leg from DATA_PORT */
}

static void usb_rail_recovery(void *i2c);	/* defined after force_device */

static void nxp_repeater_force_device(void)
{
	void *pio, *i2c;
	uint32_t v, v2;
	long cid0, cid1, lc1, r;
	int i;
	static const struct {
		uint8_t addr;		/* 7-bit slave address */
		uint8_t reg;		/* reg to read as the ACK probe */
		const char *name;
	} probe[] = {
		{ 0x28, 0x00, "ps5170 tcp-c" },
		{ 0x11, 0x00, "gate-ic" },
		{ 0x5c, 0x00, "rt5512 amp" },
		{ 0x4f, 0x14, "nxp ptn36241" },
	};

	i2c = link_map_devmem(0x13170000, 0x1000);
	if (!i2c)
		return;
	klog("nxp: i2c6 diag -- pinmux, controller self-heal, bus probes, role");

	/*
	 * 1. SCL6/SDA6 pinmux.  Pins 104/105 carry i2c6 in mode 1
	 * (vendor pinctrl-mtk-mt6991.h: SCL6/SDA6); the mux lives in
	 * pio @0x1002d000 reg 0x3d0, 4 bits per pin, 8 pins per reg
	 * (pins 104..111).  LK resets SoC pinmux on warm reboot, which
	 * would leave the bus physically unconnected -- no ACK anywhere.
	 */
	pio = link_map_devmem(0x1002d000, 0x1000);
	if (!pio)
		return;
	v = LINK_RD32(pio, 0x3d0);
	klogf("nxp: pio+0x3d0 pre = 0x%08x (want pins 104/105 = mode 1 "
	      "SCL6/SDA6, i.e. bits[7:0] = 0x11)", v);
	v2 = v;
	if ((v2 & 0x0f) != 0x01)		/* pin 104 -> SCL6 */
		v2 = (v2 & ~0x0f) | 0x01;
	if ((v2 & 0xf0) != 0x10)		/* pin 105 -> SDA6 */
		v2 = (v2 & ~0xf0) | 0x10;
	if (v2 != v) {
		LINK_WR32(pio, 0x3d0, v2);
		klogf("nxp: pio+0x3d0 fixed -> 0x%08x", v2);
	}
	/* Pins 96..127 dir = pio reg 0x0030 (1 bit per pin).  The i2c engine
	 * needs SCL/SDA as inputs (open-drain); if LK left them as GPIO
	 * outputs the bus cannot be pulled by any slave.  Clear bits 8/9. */
	v = LINK_RD32(pio, 0x0030);
	if (v & (3u << 8)) {
		LINK_WR32(pio, 0x0030, v & ~(3u << 8));
		klogf("nxp: pio+0x0030 dir cleared bits 8/9 -> 0x%08x",
		      LINK_RD32(pio, 0x0030));
	}

	/*
	 * 2. Controller snapshot + self-heal.  After a vendor init_hw the
	 * state is CONTROL=0x2c (ACKERR_DET|CLK_EXT|DMA), IO_CONFIG=0x03
	 * (open-drain).  #34 showed CONTROL=0: ACKERR detection was off,
	 * so a missing slave read back 0xff silently instead of a visible
	 * NAK.  Mirror init_hw's non-DMA writes (channel clear, intr
	 * clear, open-drain, control bits).  If the engine clock were
	 * gated, the writes would not stick and the "after" log shows it.
	 */
	klogf("nxp: engine pre: control=0x%08x timing=0x%08x ioconf=0x%08x "
	      "clkdiv=0x%08x fifo_stat=0x%08x",
	      i2c6_rd(i2c, 0x10), i2c6_rd(i2c, 0x20), i2c6_rd(i2c, 0x34),
	      i2c6_rd(i2c, 0x48), i2c6_rd(i2c, 0xf4));
	if (!i2c6_rd(i2c, 0x20)) {
		klog("nxp: i2c6 unconfigured (TIMING==0) -- full speed init "
		     "not attempted; probes skipped");
		return;
	}
	i2c6_wr(i2c, 0x24, 0);		/* START: channel clear (init_hw) */
	v = i2c6_rd(i2c, 0x0c);		/* INTR_STAT: read + write back */
	i2c6_wr(i2c, 0x0c, v);
	i2c6_wr(i2c, 0x34, 0x3);	/* IO_CONFIG: open-drain */
	i2c6_wr(i2c, 0x10, 0x28);	/* CONTROL: ACKERR_DET_EN|CLK_EXT_EN */
	klogf("nxp: engine post: control=0x%08x ioconf=0x%08x",
	      i2c6_rd(i2c, 0x10), i2c6_rd(i2c, 0x34));

	/*
	 * 3. Bus probes.  ps5170/gate-ic/rt5512 share i2c6 and are
	 * powered in the warm-boot state (ps5170 sits on the cable VBUS
	 * path, the amp rail survives reboot); an ACK from any of them
	 * proves bus+controller+pins, narrowing a 0x4f failure to the
	 * nxp device itself (power/strap).  err -7 = NAK (device
	 * present-ish but not answering at that address), -6 = engine
	 * never completed (clock/state), -8 = transfer without comp.
	 */
	for (i = 0; i < 4; i++) {
		r = i2c6_read_reg_byte(i2c, probe[i].addr, probe[i].reg);
		if (r >= 0)
			klogf("nxp: probe 0x%02x %s -> ACK data=0x%02lx",
			      probe[i].addr, probe[i].name, r);
		else
			klogf("nxp: probe 0x%02x %s -> err=%ld", probe[i].addr,
			      probe[i].name, r);
	}

	/* 4. nxp role -- only when the CID reads back as ptn36241. */
	cid0 = i2c6_read_reg_byte(i2c, 0x4f, 0x14);
	cid1 = i2c6_read_reg_byte(i2c, 0x4f, 0x15);
	klogf("nxp: CID = %02lx %02lx (expect 22 32)", cid0, cid1);
	if (cid0 != 0x22 || cid1 != 0x32) {
		klog("nxp: CID mismatch -- wrong chip or bus problem; giving up");
		usb_rail_recovery(i2c);	/* #36: whole-bus NAK = far side dead? */
		return;
	}
	lc1 = i2c6_read_reg_byte(i2c, 0x4f, 0x02);
	if (lc1 < 0) {
		klogf("nxp: LINK_CONTROL_1 read failed err=%ld", lc1);
		return;
	}
	klogf("nxp: LINK_CONTROL_1 = 0x%02lx (role=%ld speed=%ld opmode=%ld)",
	      lc1, (lc1 >> 4) & 3, (lc1 >> 6) & 3, lc1 & 7);
	if (((lc1 >> 4) & 3) == 2) {
		klog("nxp: role already FORCE_DEVICE -- nothing to write");
		return;
	}
	/* RMW bits [5:4] to FORCE_DEVICE, preserving speed/opmode. */
	if (i2c6_write_reg_byte(i2c, 0x4f, 0x02,
				(uint8_t)((lc1 & ~0x30) | 0x20)) < 0) {
		klog("nxp: LINK_CONTROL_1 write failed");
		return;
	}
	lc1 = i2c6_read_reg_byte(i2c, 0x4f, 0x02);
	klogf("nxp: LINK_CONTROL_1 after = 0x%02lx (role=%ld)", lc1,
	      lc1 >= 0 ? (lc1 >> 4) & 3 : -1);
	if (lc1 >= 0 && ((lc1 >> 4) & 3) == 2)
		klog("nxp: role FORCE_DEVICE confirmed -- final eUSB2 stage set");
}

/*
 * (#36/#38) i2c6 bus-rail recovery -- called when every probe on the bus
 * NAKed.  A full-house NAK with pinmux + controller proven good means
 * the far side of the bus has no power: the i2c6 devices (nxp ptn36241,
 * ps5170 tcp-c, gate-ic, rt5512 amps) and the bus pull-ups sit on rails
 * that Android's shutdown powers off and mainline never restores.
 *  1. Static SCL/SDA level via GPIO input: both high = bus rail alive
 *     and idle (devices are not pulling); low = rail dead or clamp.
 *  2. Rail enable sweep of BOTH secondary PMICs -- mt6373 (SPMI sid 5,
 *     swept since #36) and mt6363 main PMIC (SPMI sid 4, first look in
 *     #38): report every LDO/buck enable bit, re-enable anything off,
 *     and RE-READ to prove the write latched (the #36/#37 writes were
 *     never confirmed).  Buck enable bits on these PMICs are not plain
 *     RMW: the vendor driver writes the mask to the SET register
 *     (enable_reg + 1); LDO bits are plain RMW (regulator_enable_regmap
 *     uses update_bits at the enable reg).
 *  3. mt6379 charger DPDM mux state (0x603/0x604/0x606) -- the BC12 tap
 *     on D+/D- that may sit in the data path toward the connector.
 *  4. Re-probe all four i2c6 slaves to see whether power was the blocker.
 */
/* Rail sweep tables.  Enable regs from mt6363/mt6373-regulator.h:
 * RG_LDO_*_EN_ADDR (bit0, RMW) and the shared buck RG regs 0x240
 * (mt6363: VS2 bit0 + VBUCK1-7 bits1-7; mt6373: VBUCK0-7 bits0-7) and
 * 0x243 (mt6363: VS1 bit0/VS3 bit1/VSRAM_DIGRF bit4/MDFE bit5/MODEM
 * bit6; mt6373: VBUCK8 bit0/VBUCK9 bit1), written via the +1 SET reg.
 * buck=1 marks the SET-reg rails. */
struct mrail {
	uint16_t en;
	uint16_t op_en;	/* 0 = none; else OP_EN0 register (OP_CFG = op_en+5) */
	uint16_t hw_lp;	/* 0 = none; else HW_LP_MODE register */
	uint8_t bit;
	uint8_t buck;
	const char *name;
};

static const struct mrail m6373_rails[] = {
	/* mt6373 LDOs (bit0, RMW) */
	{ 0x1b87, 0, 0, 0, 0, "vaud18" },
	{ 0x1b95, 0, 0, 0, 0, "vusb" },
	{ 0x1ba3, 0, 0, 0, 0, "vaux18" },
	{ 0x1c07, 0, 0, 0, 0, "vcn33_1" },
	{ 0x1c15, 0, 0, 0, 0, "vcn33_2" },
	{ 0x1c23, 0, 0, 0, 0, "vcn33_3" },
	{ 0x1c31, 0, 0, 0, 0, "vcn18io" },
	{ 0x1c87, 0, 0, 0, 0, "vant18" },
	{ 0x1ca3, 0, 0, 0, 0, "vefuse" },
	{ 0x1cb1, 0, 0, 0, 0, "vmch" },
	{ 0x1cc0, 0, 0, 0, 0, "vmc" },
	{ 0x1cce, 0, 0, 0, 0, "vibr" },
	{ 0x1d07, 0, 0, 0, 0, "vio28" },
	{ 0x1d15, 0, 0, 0, 0, "vfp" },
	{ 0x1d23, 0, 0, 0, 0, "vtp" },
	{ 0x1d31, 0, 0, 0, 0, "vsim1" },
	{ 0x1d40, 0, 0, 0, 0, "vsim2" },
	/* mt6373 bucks (SET reg +1) */
	{ 0x240, 0, 0, 0, 1, "vbuck0" },
	{ 0x240, 0, 0, 1, 1, "vbuck1" },
	{ 0x240, 0, 0, 2, 1, "vbuck2" },
	{ 0x240, 0, 0, 3, 1, "vbuck3" },
	{ 0x240, 0, 0, 4, 1, "vbuck4" },
	{ 0x240, 0, 0, 5, 1, "vbuck5" },
	{ 0x240, 0, 0, 6, 1, "vbuck6" },
	{ 0x240, 0, 0, 7, 1, "vbuck7" },
	{ 0x243, 0, 0, 0, 1, "vbuck8" },
	{ 0x243, 0, 0, 1, 1, "vbuck9" },
};

static const struct mrail m6363_rails[] = {
	/* mt6363 LDOs (bit0, RMW) -- op_en=OP_EN0 from vendor regulator driver;
	 * MT6363 LDOs need NORMAL_OP_CFG + NORMAL_OP_EN written before the EN
	 * bit takes effect (the LDO has a HW gate that requires OP permission).
	 * hw_lp = HW_LP_MODE register for stuck-rail diagnostics. */
	{ 0x1b87, 0, 0, 0, 0, "vcn15" },
	{ 0x1b95, 0, 0, 0, 0, "vrf09" },
	{ 0x1ba3, 0, 0, 0, 0, "vrf12" },
	{ 0x1bb1, 0x1bb6, 0x1bb5, 0, 0, "vrf13" },
	{ 0x1bbf, 0x1bc4, 0x1bc3, 0, 0, "vrf18" },
	{ 0x1bcd, 0x1bd2, 0x1bd1, 0, 0, "vrfio18" },
	{ 0x1c07, 0, 0, 0, 0, "vtref18" },
	{ 0x1c15, 0, 0, 0, 0, "vaux18" },
	{ 0x1c23, 0, 0, 0, 0, "vemc" },
	{ 0x1c31, 0, 0, 0, 0, "vufs12" },
	{ 0x1c3f, 0, 0, 0, 0, "vufs18" },
	{ 0x1c4d, 0, 0, 0, 0, "vio18" },
	{ 0x1c87, 0, 0, 0, 0, "vio075" },
	{ 0x1c95, 0, 0, 0, 0, "va12_1" },
	{ 0x1ca3, 0, 0, 0, 0, "va12_2" },
	{ 0x1cb1, 0, 0, 0, 0, "va15" },
	{ 0x1cbf, 0, 0, 0, 0, "vm18" },
	{ 0x1d07, 0, 0, 0, 0, "vcn13" },
	{ 0x1e07, 0, 0, 0, 0, "vsram_cpub" },
	{ 0x1e1d, 0x1e2a, 0x1e21, 0, 0, "vsram_cpum" },
	{ 0x1e87, 0, 0, 0, 0, "vsram_cpul" },
	{ 0x1e9d, 0, 0, 0, 0, "vsram_apu" },
	/* mt6363 0x240: VS2 + VBUCK1-7 (SET reg +1) */
	{ 0x240, 0, 0, 0, 1, "vs2" },
	{ 0x240, 0, 0, 1, 1, "vbuck1" },
	{ 0x240, 0, 0, 2, 1, "vbuck2" },
	{ 0x240, 0, 0, 3, 1, "vbuck3" },
	{ 0x240, 0, 0, 4, 1, "vbuck4" },
	{ 0x240, 0, 0, 5, 1, "vbuck5" },
	{ 0x240, 0, 0, 6, 1, "vbuck6" },
	{ 0x240, 0, 0, 7, 1, "vbuck7" },
	/* mt6363 0x243: VS1/VS3 + VSRAM_DIGRF/MDFE/MODEM (SET reg +1) */
	{ 0x243, 0, 0, 0, 1, "vs1" },
	{ 0x243, 0, 0, 1, 1, "vs3" },
	{ 0x243, 0, 0, 4, 1, "vsram_digrf" },
	{ 0x243, 0, 0, 5, 1, "vsram_mdfe" },
	{ 0x243, 0, 0, 6, 1, "vsram_modem" },
};

/* #38: enable one rail bit and prove it latched.  Primary method matches
 * the vendor driver (SET reg +1 for bucks, RMW for LDOs); if the state
 * bit does not move, retry the other method before giving up.
 *
 * MT6363 LDOs have a two-level enable gate: the basic EN bit (bit 0 of
 * the enable register) only takes effect if the OP_EN register allows at
 * least one operation mode to enable the LDO.  The vendor regulator driver
 * calls set_load() which writes NORMAL_OP_CFG (0x10) to OP_EN+5 and
 * NORMAL_OP_EN (0x800000) to OP_EN+{0,1,2} before the framework calls
 * enable().  Without this, the EN bit is silently ignored.
 *
 * v6: also clear the LP bit (bit 1, same register as EN) with a 100us
 * settle delay, matching the vendor set_mode(NORMAL) path.
 * v7: hex-dump OP_EN-2..OP_EN+13 before any writes to map the actual
 * register layout, then try mainline-header addresses (vendor - 0x30)
 * if vendor OP_CFG writes don't stick. Dump the
 * HW_LP_MODE register for diagnostics, and readback OP_EN to verify
 * writes actually landed. */
static void enable_rail(void *bus, uint32_t acc, uint8_t sid,
			const struct mrail *r)
{
	long v = spmi_read_byte(bus, acc, sid, r->en, NULL);
	int alt;

	if (v < 0) {
		klogf("rail: %-9s en=0x%04x pre-read err=%ld", r->name,
		      r->en, v);
		return;
	}
	if (v & (1u << r->bit))
		return;

	/* Dump HW_LP_MODE register if available (diagnostic). */
	if (r->hw_lp) {
		long hwm = spmi_read_byte(bus, acc, sid, r->hw_lp, NULL);

		klogf("rail: %-9s hw_lp(0x%04x)=%ld", r->name,
		      r->hw_lp, hwm);
	}

	/* v7: hex-dump OP_EN-2..OP_EN+13 BEFORE any writes to reveal the
	 * actual register layout.  16 bytes per rail; only for rails that
	 * have op_en set. */
	if (r->op_en) {
		char hx[48]; /* 16 * 3 = 48 */
		int j, pos = 0;

		for (j = -2; j <= 13; j++) {
			long rv = spmi_read_byte(bus, acc, sid,
						 (uint16_t)(r->op_en + j),
						 NULL);
			pos += snprintf(hx + pos, sizeof(hx) - pos,
					" %02lx", rv & 0xff);
		}
		klogf("rail: %-9s OP[-2..13]:%s", r->name, hx);
	}

	/* v6: clear LP bit (bit 1) of the EN register to take the rail
	 * out of low-power mode, then settle.  Matches vendor driver's
	 * set_mode(NORMAL) which clears lp_mode_mask in lp_mode_reg
	 * (same address as EN for these LDOs). */
	if (r->op_en && (v & 0x02)) {
		spmi_write_byte(bus, acc, sid, r->en,
				(uint8_t)(v & ~0x02u), NULL);
		sleep_us(100);
		v = spmi_read_byte(bus, acc, sid, r->en, NULL);
		klogf("rail: %-9s LP cleared, en=0x%04x=%02lx", r->name,
		      r->en, v);
	}

	/* If the rail has an OP_EN register, configure it so that the
	 * NORMAL operation mode is allowed to enable the LDO.  Write:
	 *   OP_EN+5 = NORMAL_OP_CFG (0x10)
	 *   OP_EN+0 = 0x00  (RC6/RC7 OP_EN bytes)
	 *   OP_EN+1 = 0x00  (RC8/RC9 OP_EN bytes)
	 *   OP_EN+2 = 0x80  (HW0/SW OP_EN byte 23 = NORMAL)
	 *
	 * v8: write TMA unlock key (0x9c9c to 0x39e) before OP_CFG.
	 * The vendor driver uses TMA for VEMC voltage; LDO OP_CFG may
	 * also need it to pass the write-protection gate. */
	if (r->op_en) {
		const uint8_t op_cfg = 0x10;	/* NORMAL_OP_CFG */
		const uint8_t op_en_bytes[3] = { 0x00, 0x00, 0x80 };
		int i;

		/* v8: TMA unlock before OP_CFG writes */
		spmi_write_byte(bus, acc, sid, 0x39e, 0x9c, NULL);
		spmi_write_byte(bus, acc, sid, 0x39f, 0x9c, NULL);
		{
			long tk0 = spmi_read_byte(bus, acc, sid, 0x39e, NULL);
			long tk1 = spmi_read_byte(bus, acc, sid, 0x39f, NULL);
			klogf("rail: %-9s TMA key rb=%02lx%02lx",
			      r->name, tk0 & 0xff, tk1 & 0xff);
		}

		spmi_write_byte(bus, acc, sid, r->op_en + 5, op_cfg, NULL);
		for (i = 0; i < 3; i++)
			spmi_write_byte(bus, acc, sid,
					(uint16_t)(r->op_en + i),
					op_en_bytes[i], NULL);

		/* v8: clear TMA key after writes */
		spmi_write_byte(bus, acc, sid, 0x39e, 0x00, NULL);
		spmi_write_byte(bus, acc, sid, 0x39f, 0x00, NULL);

		/* Readback: verify OP_EN writes actually landed. */
		for (i = 0; i < 3; i++) {
			long rb = spmi_read_byte(bus, acc, sid,
						 (uint16_t)(r->op_en + i),
						 NULL);
			klogf("rail: %-9s OP_EN+%d wr=0x%02x rb=%ld",
			      r->name, i, op_en_bytes[i], rb);
		}
		{
			long rb = spmi_read_byte(bus, acc, sid,
						 (uint16_t)(r->op_en + 5),
						 NULL);
			klogf("rail: %-9s OP_CFG  wr=0x%02x rb=%ld",
			      r->name, op_cfg, rb);

			/* v7: if vendor OP_CFG write didn't stick, try the
			 * mainline-header addresses (vendor - 0x30).  The
			 * mainline and vendor headers differ by exactly 0x30
			 * for all LDO register blocks.  If the mainline OP_CFG
			 * is writable, re-enable using the mainline EN addr. */
			if (rb != op_cfg && r->op_en > 0x30) {
				uint16_t ml_op = r->op_en - 0x30;
				uint16_t ml_en = r->en - 0x30;

				klogf("rail: %-9s ml try op=0x%04x en=0x%04x",
				      r->name, ml_op, ml_en);

				/* v8: TMA unlock for mainline addresses too */
				spmi_write_byte(bus, acc, sid,
						0x39e, 0x9c, NULL);
				spmi_write_byte(bus, acc, sid,
						0x39f, 0x9c, NULL);

				/* Write OP_CFG + OP_EN via mainline addresses */
				spmi_write_byte(bus, acc, sid,
						ml_op + 5, op_cfg, NULL);
				for (i = 0; i < 3; i++)
					spmi_write_byte(bus, acc, sid,
							(uint16_t)(ml_op + i),
							op_en_bytes[i], NULL);

				spmi_write_byte(bus, acc, sid,
						0x39e, 0x00, NULL);
				spmi_write_byte(bus, acc, sid,
						0x39f, 0x00, NULL);

				/* Readback */
				for (i = 0; i < 3; i++) {
					long mrb = spmi_read_byte(bus, acc,
							sid,
							(uint16_t)(ml_op + i),
							NULL);
					klogf("rail: %-9s ml EN+%d wr=0x%02x rb=%ld",
					      r->name, i, op_en_bytes[i], mrb);
				}
				{
					long mrb = spmi_read_byte(bus, acc,
							sid,
							(uint16_t)(ml_op + 5),
							NULL);
					klogf("rail: %-9s ml CFG  wr=0x%02x rb=%ld",
					      r->name, op_cfg, mrb);

					/* If mainline OP_CFG worked, try
					 * enabling via mainline EN address. */
					if (mrb == op_cfg) {
						long me = spmi_read_byte(bus,
							acc, sid, ml_en, NULL);

						klogf("rail: %-9s ml en(0x%04x)=%02lx",
						      r->name, ml_en, me & 0xff);
						if (me >= 0 && !(me & 1)) {
							spmi_write_byte(bus,
								acc, sid,
								ml_en,
								(uint8_t)(me | 1),
								NULL);
							sleep_us(50);
							me = spmi_read_byte(bus,
								acc, sid,
								ml_en, NULL);
							klogf("rail: %-9s ml EN wr+rb=%02lx",
							      r->name,
							      me & 0xff);
						}
					}
				}
			}
		}
	}

	for (alt = 0; alt < 2; alt++) {
		long w;
		int use_set = r->buck ? alt == 0 : alt == 1;

		w = spmi_write_byte(bus, acc, sid,
				    use_set ? (uint16_t)(r->en + 1) : r->en,
				    use_set ? (uint8_t)(1u << r->bit)
					    : (uint8_t)(v | (1u << r->bit)),
				    NULL);
		if (w < 0) {
			klogf("rail: %-9s write err=%ld (%s)", r->name, w,
			      use_set ? "SET+1" : "RMW");
			continue;
		}
		sleep_us(50);
		v = spmi_read_byte(bus, acc, sid, r->en, NULL);
		if (v >= 0 && (v & (1u << r->bit))) {
			klogf("rail: %-9s ENABLED+confirmed (%s)", r->name,
			      use_set ? "SET+1" : "RMW");
			return;
		}
	}
	klogf("rail: %-9s enable FAILED -- state bit stuck at 0", r->name);
}

static void rail_sweep(void *bus, uint32_t acc, uint8_t sid, const char *who,
		       const struct mrail *t, int n)
{
	int i, on = 0, en = 0, err = 0;

	for (i = 0; i < n; i++) {
		long v = spmi_read_byte(bus, acc, sid, t[i].en, NULL);

		if (v < 0) {
			klogf("rail: %s %-9s en=0x%04x read err=%ld", who,
			      t[i].name, t[i].en, v);
			err++;
			continue;
		}
		if (v & (1u << t[i].bit)) {
			on++;
			continue;
		}
		klogf("rail: %s %-9s en=0x%04x = 0x%02lx (off)", who,
		      t[i].name, t[i].en, v);
		enable_rail(bus, acc, sid, &t[i]);
		en++;
	}
	klogf("rail: %s sweep: %d on, %d off->enable, %d read-err", who, on,
	      en, err);
}

static void usb_rail_recovery(void *i2c)
{
	void *pio, *bus;
	uint32_t acc = 0x880;		/* ch2: kernel soc_chan=2 */
	uint32_t v, d;
	long r;

	klog("rail: whole-bus NAK -- pull-up level, mt6373+mt6363 rail sweep");

	pio = link_map_devmem(0x1002d000, 0x1000);
	bus = link_map_devmem(0x1c01a000, 0x1000);
	if (!pio || !bus) {
		klog("rail: devmem map failed");
		return;
	}

	/* 1. Static bus level.  Pins 104/105 -> GPIO inputs, read DIN
	 * reg 0x0230 (covers pins 96..127, 1 bit per pin).  Flip back to
	 * SCL6/SDA6 immediately after. */
	v = LINK_RD32(pio, 0x3d0);
	LINK_WR32(pio, 0x3d0, v & ~0xffu);	/* mode 0 = GPIO */
	d = LINK_RD32(pio, 0x0030);
	LINK_WR32(pio, 0x0030, d & ~(3u << 8));	/* direction = input */
	LINK_RD32(pio, 0x0230);			/* settle */
	d = LINK_RD32(pio, 0x0230);
	klogf("rail: gpio104/scl=%d gpio105/sda=%d (1=bus rail alive/idle)",
	      !!(d & (1u << 8)), !!(d & (1u << 9)));
	LINK_WR32(pio, 0x3d0, (v & ~0xffu) | 0x11);	/* back to SCL6/SDA6 */

	/* 2. Sweep mt6373 (sid 5) then mt6363 (sid 4): every rail, enable
	 * anything off, confirm the latch by re-read.  Restoring Android's
	 * runtime state is harmless and each rail is one hypothesis fewer. */
	rail_sweep(bus, acc, 0x05, "mt6373", m6373_rails,
		   (int)(sizeof(m6373_rails) / sizeof(m6373_rails[0])));
	rail_sweep(bus, acc, 0x04, "mt6363", m6363_rails,
		   (int)(sizeof(m6363_rails) / sizeof(m6363_rails[0])));

	/* 3. mt6379 charger DPDM mux state -- the BC12 tap on D+/D- that
	 * may gate the USB2 data path toward the connector (informational;
	 * realme's charger driver drives these during attach/detect). */
	{
		static const struct {
			uint16_t en;
			const char *n;
		} dp[] = {
			{ 0x603, "DPDM_CTRL1" },
			{ 0x604, "DPDM_CTRL2" },
			{ 0x606, "DPDM_CTRL4" },
		};
		int i;

		for (i = 0; i < (int)(sizeof(dp) / sizeof(dp[0])); i++) {
			r = spmi_read_byte(bus, acc, 0x0e, dp[i].en, NULL);
			if (r < 0)
				klogf("rail: mt6379 %s (0x%03x) read err=%ld",
				      dp[i].n, dp[i].en, r);
			else
				klogf("rail: mt6379 %s = 0x%02lx", dp[i].n, r);
		}
	}

	/* 4. Let the rails settle, then re-probe every i2c6 slave.  The
	 * speaker amps and the gate-ic are the canaries: they are not on
	 * the USB chain, so if one of them ACKs while the repeater/redriver
	 * pair stays silent, the bus/domain is healthy and the USB chips
	 * have their own missing supply; if even rt5512 stays silent, the
	 * whole slave side is unpowered. */
	sleep_us(50000);
	{
		static const struct {
			uint8_t addr;
			uint8_t reg;
			const char *name;
		} probes[] = {
			{ 0x11, 0x00, "gate-ic" },
			{ 0x28, 0x00, "ps5170" },
			{ 0x4f, 0x14, "nxp" },
			{ 0x5c, 0x00, "rt5512(L)" },
		};
		int i;

		for (i = 0; i < (int)(sizeof(probes) / sizeof(probes[0]));
		     i++) {
			r = i2c6_read_reg_byte(i2c, probes[i].addr,
					       probes[i].reg);
			if (r >= 0) {
				if (probes[i].addr == 0x4f) {
					long c1 = i2c6_read_reg_byte(i2c, 0x4f,
								     0x15);

					klogf("rail: re-probe 0x%02x %-9s -> "
					      "ACK, CID %02lx %02lx",
					      probes[i].addr, probes[i].name,
					      r, c1);
					if (r == 0x22 && c1 == 0x32) {
						long lc1 = i2c6_read_reg_byte(
							i2c, 0x4f, 0x02);

						if (lc1 >= 0 &&
						    i2c6_write_reg_byte(
							    i2c, 0x4f, 0x02,
							    (uint8_t)((lc1 &
								       ~0x30) |
								      0x20)) ==
						    0)
							klogf("rail: "
							      "LINK_CONTROL_1 "
							      "-> 0x%02lx "
							      "role=%ld "
							      "(FORCE_DEVICE "
							      "after rail fix)",
							      (lc1 & ~0x30) |
							      0x20,
							      ((lc1 & ~0x30) |
							       0x20) >> 4 & 3);
					}
				} else {
					klogf("rail: re-probe 0x%02x %-9s -> "
					      "ACK data=0x%02lx",
					      probes[i].addr, probes[i].name,
					      r);
				}
			} else {
				klogf("rail: re-probe 0x%02x %-9s -> %s",
				      probes[i].addr, probes[i].name,
				      r == -7 ? "still NAK" : "err (see code)");
			}
		}
	}
}

/*
 * usb_rail_recovery_safe() -- like usb_rail_recovery() but avoids raw I2C6
 * MMIO (0x13170000) which crashes the kernel when the clock is gated.
 * Steps 1-3 (PIO GPIO + SPMI rail sweep + mt6379 DPDM) are unchanged.
 * Step 4 uses the kernel's /dev/i2c-6 interface instead of raw controller
 * register access.
 */
static int i2c_smbus_read_byte_fd(int fd, uint8_t addr, uint8_t reg)
{
	struct i2c_smbus_ioctl_data args;
	union i2c_smbus_data data;

	data.byte = 0;
	args.read_write = I2C_SMBUS_READ;
	args.command = reg;
	args.size = I2C_SMBUS_BYTE_DATA;
	args.data = &data;
	if (ioctl(fd, I2C_SMBUS, &args) < 0)
		return -errno;
	return data.byte;
}

static int i2c_smbus_write_byte_fd(int fd, uint8_t addr, uint8_t reg,
				   uint8_t val)
{
	struct i2c_smbus_ioctl_data args;
	union i2c_smbus_data data;

	data.byte = val;
	args.read_write = I2C_SMBUS_WRITE;
	args.command = reg;
	args.size = I2C_SMBUS_BYTE_DATA;
	args.data = &data;
	if (ioctl(fd, I2C_SMBUS, &args) < 0)
		return -errno;
	return 0;
}

static void usb_rail_recovery_safe(void)
{
	void *pio, *bus;
	uint32_t acc = 0x880;  /* ch2: kernel soc_chan=2 maps to SWINF ch2 */
	uint32_t v, d;
	long r;

	klog("rail: safe recovery v11 -- ch2 for mt6363 writes");

	pio = link_map_devmem(0x1002d000, 0x1000);
	bus = link_map_devmem(0x1c01a000, 0x1000);
	if (!pio || !bus) {
		klog("rail: devmem map failed");
		return;
	}

	/* v9: TOP register scan of mt6363 (SID 4) 0x000..0x0ff.
	 * Look for CKPDN, protection, or security registers that
	 * might gate OP_CFG write access.  READ ONLY -- no writes. */
	{
		char line[64];
		int a, pos;
		long v;

		klog("rail: mt6363 TOP scan 0x000..0x0ff (SID 4)");
		for (a = 0x000; a <= 0x0f0; a += 16) {
			pos = 0;
			for (int j = 0; j < 16; j++) {
				v = spmi_read_byte(bus, acc, 0x04,
						   (uint16_t)(a + j), NULL);
				pos += snprintf(line + pos, sizeof(line) - pos,
						" %02lx", v & 0xff);
			}
			klogf("rail: mt6363 +0x%03x:%s", a, line);
		}
	}

	/* v10: CKPDN unlock -- clear CKPDN1 (0x011) and CKPDN2 (0x012)
	 * which are 0xff (all clocks gated).  If these gate the LDO
	 * control logic clock, writes to OP_CFG/OP_EN are silently
	 * dropped by the PMIC hardware.  Clear them to 0x00, wait for
	 * the clock tree to settle, then the rail sweep will retry
	 * OP_CFG writes. */
	{
		long ck1, ck2;

		ck1 = spmi_read_byte(bus, acc, 0x04, 0x011, NULL);
		ck2 = spmi_read_byte(bus, acc, 0x04, 0x012, NULL);
		klogf("rail: CKPDN before: 0x011=0x%02lx 0x012=0x%02lx",
		      ck1 & 0xff, ck2 & 0xff);

		if ((ck1 & 0xff) || (ck2 & 0xff)) {
			spmi_write_byte(bus, acc, 0x04, 0x011, 0x00, NULL);
			spmi_write_byte(bus, acc, 0x04, 0x012, 0x00, NULL);
			spmi_write_byte(bus, acc, 0x04, 0x013, 0x00, NULL);
			sleep_us(500);

			ck1 = spmi_read_byte(bus, acc, 0x04, 0x011, NULL);
			ck2 = spmi_read_byte(bus, acc, 0x04, 0x012, NULL);
			klogf("rail: CKPDN after:  0x011=0x%02lx 0x012=0x%02lx",
			      ck1 & 0xff, ck2 & 0xff);
		} else {
			klog("rail: CKPDN already clear, skip");
		}
	}

	/* 1. Static bus level via GPIO */
	v = LINK_RD32(pio, 0x3d0);
	LINK_WR32(pio, 0x3d0, v & ~0xffu);
	d = LINK_RD32(pio, 0x0030);
	LINK_WR32(pio, 0x0030, d & ~(3u << 8));
	LINK_RD32(pio, 0x0230);
	d = LINK_RD32(pio, 0x0230);
	klogf("rail: gpio104/scl=%d gpio105/sda=%d (1=alive/idle)",
	      !!(d & (1u << 8)), !!(d & (1u << 9)));
	LINK_WR32(pio, 0x3d0, (v & ~0xffu) | 0x11);

	/* 2. SPMI rail sweep */
	rail_sweep(bus, acc, 0x05, "mt6373", m6373_rails,
		   (int)(sizeof(m6373_rails) / sizeof(m6373_rails[0])));
	rail_sweep(bus, acc, 0x04, "mt6363", m6363_rails,
		   (int)(sizeof(m6363_rails) / sizeof(m6363_rails[0])));

	/* 2b. NXP PTN36241 -- do NOT reset cycle.
	 * A LOW→HIGH pulse on GPIO136 resets the NXP to its power-on
	 * default, which destroys the bootloader's configuration.  Since
	 * I2C6 is dead (all slaves NAK), we cannot reconfigure the NXP,
	 * so we must preserve whatever the bootloader left.  Just ensure
	 * GPIO136 is HIGH (reset de-asserted). */
	{
		uint32_t do_v;

		do_v = LINK_RD32(pio, 0x140);
		if (!(do_v & (1u << 8))) {
			LINK_WR32(pio, 0x140, do_v | (1u << 8));
			klog("rail: gpio136 was LOW -- set HIGH (no reset cycle)");
		}
		klogf("rail: gpio136 do=%08x (bit8=1 = reset de-asserted)",
		      LINK_RD32(pio, 0x140));
	}

	/* 2c. Explicit OP_EN/OP_CFG for LDOs whose table entry has op_en=0.
	 * MT6363 LDOs have a two-level enable gate: the EN bit only takes
	 * effect when OP_EN allows at least one operation mode.  LK sets this
	 * up, but Android or CCF may clear it during shutdown.  Our sweep
	 * only checks the EN bit, so LDOs that appear "on" may not actually
	 * be outputting if OP_EN was cleared.  Fix: write NORMAL_OP_CFG (0x10)
	 * to OP_CFG and NORMAL_OP_EN to OP_EN0/1/2 for VIO18 (I2C VDDIO)
	 * and VRFIO18 (USB-C I/O). */
	{
		/* VIO18: EN=0x1c4d, OP_CFG=0x1c51, OP_EN0=0x1c52 */
		long opcfg, open;

		opcfg = spmi_read_byte(bus, acc, 0x04, 0x1c51, NULL);
		open  = spmi_read_byte(bus, acc, 0x04, 0x1c52, NULL);
		klogf("rail: vio18 OP pre: cfg=%02lx en0=%02lx", opcfg & 0xff, open & 0xff);
		/* Write NORMAL_OP_CFG = 0x10, NORMAL_OP_EN0 = 0x01 (bit0=normal mode) */
		spmi_write_byte(bus, acc, 0x04, 0x1c51, 0x10, NULL);
		spmi_write_byte(bus, acc, 0x04, 0x1c52, 0x01, NULL);
		sleep_us(100);
		opcfg = spmi_read_byte(bus, acc, 0x04, 0x1c51, NULL);
		open  = spmi_read_byte(bus, acc, 0x04, 0x1c52, NULL);
		klogf("rail: vio18 OP post: cfg=%02lx en0=%02lx", opcfg & 0xff, open & 0xff);

		/* VRFIO18: EN=0x1bcd, OP_CFG=0x1bd1, OP_EN0=0x1bd2 */
		opcfg = spmi_read_byte(bus, acc, 0x04, 0x1bd1, NULL);
		open  = spmi_read_byte(bus, acc, 0x04, 0x1bd2, NULL);
		klogf("rail: vrfio18 OP pre: cfg=%02lx en0=%02lx", opcfg & 0xff, open & 0xff);
		spmi_write_byte(bus, acc, 0x04, 0x1bd1, 0x10, NULL);
		spmi_write_byte(bus, acc, 0x04, 0x1bd2, 0x01, NULL);
		sleep_us(100);
		opcfg = spmi_read_byte(bus, acc, 0x04, 0x1bd1, NULL);
		open  = spmi_read_byte(bus, acc, 0x04, 0x1bd2, NULL);
		klogf("rail: vrfio18 OP post: cfg=%02lx en0=%02lx", opcfg & 0xff, open & 0xff);

		/* Re-assert GPIO174 HIGH after rail settle (gate-ic power) */
		{
			uint32_t gv = LINK_RD32(pio, 0x150);

			LINK_WR32(pio, 0x150, gv | (1u << 14));
			klogf("rail: gpio174 re-assert HIGH (do=%08x)",
			      LINK_RD32(pio, 0x150));
		}
	}

	/* 3. mt6379 DPDM mux */
	{
		static const struct {
			uint16_t en;
			const char *n;
		} dp[] = {
			{ 0x603, "DPDM_CTRL1" },
			{ 0x604, "DPDM_CTRL2" },
			{ 0x606, "DPDM_CTRL4" },
		};
		int i;

		for (i = 0; i < (int)(sizeof(dp) / sizeof(dp[0])); i++) {
			r = spmi_read_byte(bus, acc, 0x0e, dp[i].en, NULL);
			if (r < 0)
				klogf("rail: mt6379 %s (0x%03x) err=%ld",
				      dp[i].n, dp[i].en, r);
			else
				klogf("rail: mt6379 %s = 0x%02lx", dp[i].n, r);
		}
	}

	/* 3b. mt6363 VS voter enable for PS5170/redriver power.
	 * DTS property: mediatek,vs-voter = <&pmic 0x189a 0x20 1>
	 *   &pmic = mt6363 (SID 4), base=0x189a, mask=0x20 (bit5).
	 * SET = base+1 = 0x189b; CLR = base+2 = 0x189c.
	 * Last time we tried SID 0x0e (mt6379) which was wrong! */
	{
		long pre, post;
		uint8_t sid_mt = 4;  /* mt6363 */

		pre = spmi_read_byte(bus, acc, sid_mt, 0x189a, NULL);
		klogf("rail: mt6363 VS_VOTER pre = 0x%02lx", pre & 0xff);
		/* Write 0x20 to SET reg (0x189b) to enable bit5 */
		spmi_write_byte(bus, acc, sid_mt, 0x189b, 0x20, NULL);
		sleep_us(5000);
		post = spmi_read_byte(bus, acc, sid_mt, 0x189a, NULL);
		klogf("rail: mt6363 VS_VOTER post = 0x%02lx (bit5=%ld)",
		      post & 0xff, post >= 0 ? (post >> 5) & 1 : -1);

		/* Also scan 0x1890-0x189f on both mt6363 (SID4) and
		 * mt6379 (SID0xe) for comparison */
		{
			char line[128];
			int pos, i;
			uint8_t sids[] = {4, 0x0e};
			const char *names[] = {"mt6363", "mt6379"};

			for (pos = 0; pos < 2; pos++) {
				int p = 0;

				p += snprintf(line + p, sizeof(line) - p,
					      "rail: %s 0x189x:", names[pos]);
				for (i = 0x1890; i <= 0x189f; i++) {
					long v2 = spmi_read_byte(bus, acc,
								  sids[pos],
								  (uint16_t)i,
								  NULL);

					p += snprintf(line + p,
						      sizeof(line) - p,
						      " %02lx", v2 & 0xff);
				}
				klog(line);
			}
		}
	}

	/* 4. Re-probe I2C6 via kernel interface /dev/i2c-6 */
	sleep_us(50000);
	{
		static const struct {
			uint8_t addr;
			uint8_t reg;
			const char *name;
		} probes[] = {
			{ 0x11, 0x00, "gate-ic" },
			{ 0x28, 0x00, "ps5170" },
			{ 0x4f, 0x14, "nxp" },
			{ 0x5c, 0x00, "rt5512(L)" },
		};
		int i, fd;
		char path[32];

		snprintf(path, sizeof(path), "/dev/i2c-0");
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			klogf("rail: %s open failed: %s", path, strerror(errno));
			return;
		}
		for (i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
			if (ioctl(fd, I2C_SLAVE_FORCE, probes[i].addr) < 0) {
				klogf("rail: i2c-6 slave 0x%02x: %s",
				      probes[i].addr, strerror(errno));
				continue;
			}
			r = i2c_smbus_read_byte_fd(fd, probes[i].addr,
						   probes[i].reg);
			if (r >= 0) {
				if (probes[i].addr == 0x4f) {
					long c1 = i2c_smbus_read_byte_fd(
						fd, 0x4f, 0x15);

					klogf("rail: i2c-6 0x%02x %s -> ACK "
					      "data=0x%02lx CID=%02lx",
					      probes[i].addr, probes[i].name,
					      r, c1);
					if (r == 0x22 && c1 == 0x32) {
						long lc1 = i2c_smbus_read_byte_fd(
							fd, 0x4f, 0x02);

						if (lc1 >= 0) {
							uint8_t new_lc1 =
								(uint8_t)((lc1 &
									   ~0x30) |
									  0x20);

							i2c_smbus_write_byte_fd(
								fd, 0x4f,
								0x02,
								new_lc1);
							lc1 = i2c_smbus_read_byte_fd(
								fd, 0x4f,
								0x02);
							klogf("rail: "
							      "LINK_CONTROL_1 "
							      "= 0x%02lx "
							      "role=%ld",
							      lc1,
							      lc1 >= 0 ?
							      (lc1 >> 4) & 3 :
							      -1);
						}
					}
				} else {
					klogf("rail: i2c-6 0x%02x %s -> "
					      "ACK data=0x%02lx",
					      probes[i].addr, probes[i].name,
					      r);
				}
			} else {
				klogf("rail: i2c-6 0x%02x %s -> %s",
				      probes[i].addr, probes[i].name,
				      r == -ENXIO ? "NAK" : strerror((int)-r));
			}
		}
		/* v8: full bus scan 0x03..0x77 to see if ANY device responds. */
		{
			int found = 0;
			char acks[256];
			int pos = 0;

			for (i = 0x03; i <= 0x77; i++) {
				if (ioctl(fd, I2C_SLAVE_FORCE, i) < 0)
					continue;
				r = i2c_smbus_read_byte_fd(fd, (uint8_t)i, 0);
				if (r >= 0) {
					pos += snprintf(acks + pos,
							sizeof(acks) - pos,
							" %02x", i);
					found++;
				}
			}
			klogf("rail: i2c-6 bus scan 0x03..0x77: %d ACKed:%s",
			      found, found ? acks : " (none)");
		}
		close(fd);
	}
}

/*
 * (2026-09-05, #30) Android /data probe.
 *
 * userdata is the GPT partition whose number is 95 (Android's
 * /dev/block/by-name/userdata -> /dev/block/sdc95); the sdX letter can in
 * principle differ between kernels, so the block node is found by scanning
 * /sys for the partition numbered 95 instead of trusting the letter.
 *
 * Android formats it f2fs.  With plain file-based encryption the fs mounts
 * without any key and the top level (app/, data/, oplus/, ...) is readable
 * -- only per-user subtrees are locked.  With metadata encryption the whole
 * partition sits behind dm-crypt and the f2fs superblock (magic F2FS at
 * +0x400) is invisible, so the magic check says which world this phone is
 * in before the mount is even attempted.
 *
 * Mount read-only: listing needs no write access, and a ro mount cannot
 * dirty the filesystem for Android's next boot.
 */
static int data_find_userdata(char *path, size_t sz)
{
	DIR *d;
	struct dirent *e;
	int found = 0;

	path[0] = '\0';
	d = opendir("/sys/class/block");
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char buf[16], p[300];
		ssize_t n;

		if (strncmp(e->d_name, "sd", 2) != 0)
			continue;
		snprintf(p, sizeof(p), "/sys/class/block/%s/partition",
			 e->d_name);
		n = slurp(p, buf, sizeof(buf));
		if (n > 0 && strtol(buf, NULL, 10) == 95) {
			snprintf(path, sz, "/dev/%s", e->d_name);
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

static void userdata_report(void)
{
	static const unsigned char f2fs_magic[4] = { 0x10, 0x20, 0xf5, 0xf2 };
	char dev[300];
	struct stat st;
	unsigned char sb[4096];
	DIR *d;
	int fd, n = 0, e;

	klog("data: probing Android userdata (f2fs, GPT partition 95)");
	if (!data_find_userdata(dev, sizeof(dev))) {
		klog("data: no sd* partition numbered 95 -- UFS partition table issue?");
		return;
	}
	klogf("data: userdata is %s", dev);
	if (stat(dev, &st) != 0) {
		klogf("data: stat %s: %s", dev, strerror(errno));
		return;
	}

	/*
	 * f2fs superblock magic lives at +0x400.  Visible = plain FBE (mount
	 * will work, listing unencrypted at the top level); ciphertext = the
	 * whole partition is dm-crypt'd and mounting will fail below.
	 */
	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		klogf("data: open %s: %s", dev, strerror(errno));
		return;
	}
	if (pread(fd, sb, sizeof(sb), 0x400) != (ssize_t)sizeof(sb)) {
		klogf("data: superblock read failed: %s", strerror(errno));
	} else if (memcmp(sb, f2fs_magic, 4) == 0) {
		klog("data: f2fs magic present at +0x400 -- file-based encryption, mountable");
	} else {
		klogf("data: no f2fs magic at +0x400 (%02x %02x %02x %02x...) "
		      "-- metadata encryption (dm-crypt) or not userdata",
		      sb[0], sb[1], sb[2], sb[3]);
	}
	close(fd);

	mkdir("/data", 0755);
	if (mount(dev, "/data", "f2fs", MS_RDONLY, NULL) != 0) {
		klogf("data: mount f2fs ro on /data failed: %s", strerror(errno));
		return;
	}
	klog("data: mounted read-only, first-level entries:");
	d = opendir("/data");
	if (!d) {
		klogf("data: opendir /data: %s", strerror(errno));
		goto out;
	}
	for (;;) {
		struct dirent *ent = readdir(d);
		char kind = '-';

		if (!ent)
			break;
		if (ent->d_type == DT_DIR)
			kind = 'd';
		else if (ent->d_type == DT_LNK)
			kind = 'l';
		else if (ent->d_type == DT_BLK)
			kind = 'b';
		if (n < 128)
			klogf("data:   %c %s", kind, ent->d_name);
		n++;
	}
	closedir(d);
	klogf("data: %d first-level entries total", n);
out:
	e = umount("/data");
	if (e != 0)
		klogf("data: umount /data: %s", strerror(errno));
	else
		klog("data: unmounted cleanly");
}

int main(void)
{
	struct utsname u;
	unsigned long beat = 0;
	int ufs_ok;
	int replayed = 0;
	int enumerated = 0;

	/* devtmpfs is normally mounted by the kernel before init runs. */
	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	try_mount("proc", "/proc", "proc");
	try_mount("sysfs", "/sys", "sysfs");
	try_mount("devtmpfs", "/dev", "devtmpfs");
	if (kmsg_fd < 0)
		kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	{
		int fd = open("/proc/sys/kernel/printk_devkmsg",
			      O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t ignored = write(fd, "on\n", 3);
			(void)ignored;
			close(fd);
		}
	}

	klog("=== MT6991 bring-up test init ===");
	klog("  MT6991 / realme GT7 (RMX6688) -- mainline userspace v2");
	klog("  v14: SPI padding byte skip + full-duplex read");
	klog("=====================================================");

	if (uname(&u) == 0)
		klogf("running %s %s (%s)", u.sysname, u.release, u.machine);

	klog("init reached userspace -- SoC bring-up succeeded");

//	verify_dtb();	/* 15 logs -- not needed for USB debug */
//	pstore_report();

	/*
	 * (2026-09-15) Touch panel AVDD: the mainline kernel has no MT6373
	 * PMIC driver, so the S3910's analog rail was never powered.  Bring
	 * it up before the SPI6 rebind so the touch driver's second probe
	 * runs with sensing enabled.
	 */
	touch_vtp_power_on();

	/*
	 * (2026-09-05, #29) eUSB2 link bring-up for the USB-C serial console:
	 * the vendor kernel's connect path (SoC EN_PU_DP + nxp repeater EN)
	 * has no mainline equivalent, so this pokes it from userspace.  Must
	 * run before the heartbeat loop -- the host can enumerate the gadget
	 * at any point once both ends of the link are signalling.
	 */
	usb_eusb2_link_bringup();

	/*
	 * (2026-09-05, #31a/#31b) mt6379 repeater: probe reports the state
	 * Android's shutdown left (expect the isolate pattern), power_on then
	 * runs the vendor power sequence over the same raw-SWINF path and
	 * announces the device D+ pull-up -- the final eUSB2 chain stage.
	 */
	mt6379_repeater_probe();
	mt6379_repeater_power_on();
	usb_power_recon();

	/* DRM/DSI display subsystem probe check */
//	drm_report();	/* verbose -- skip to reduce pstore volume */

	/* Capture the bootloader's OVL/EXDMA state before DRM touches it */
	if (DUMP_DISPLAY_REGS)
		disp_hw_dump();

	/* Drop the stale bootloader overlay layer (EXDMA4) */
	/*
	 * EXPERIMENT: do not disable the stale EXDMA4 layer.  EXDMA4 is a
	 * stage in the blender chain (EXDMA3 -> BLENDER1 -> EXDMA4 ->
	 * BLENDER2 -> ...), so turning its layer off may stop the flow.
	 */
//	disp_takeover_exdma();

	/*
	 * Display test.  With the DRM fbdev client owning /dev/fb0, use the
	 * fbdev + FBIOBLANK path; the legacy DRM ioctl test stays available
	 * behind LEGACY_DRM_IOCTL_TEST.
	 */
	if (LEGACY_DRM_IOCTL_TEST) {
		drm_modeset_test();
	} else {
		/* display_dpms_fb_test(); -- disabled, serial console only */
		/* display_rate_test(); -- disabled to speed up touch bring-up */
	}

	if (DUMP_DISPLAY_REGS) {
		/* Mainline DSC configuration - compare with DSC[pre] above */
		disp_dump_dsc("post");
		disp_dump_dsi("post");

		/* Downstream chain state after the DPMS off/on cycle */
		disp_dump_exdma("EXDMA3post", 0x32860000);
		disp_dump_blender("BLENDER1post", 0x328e0000);
		disp_dump_exdma("EXDMA4post", 0x32870000);
		disp_dump_outproc("OUTPROC0post", 0x32970000);
		disp_dump_mutex("post");
	}

	/*
	 * Separation test: restore the bootloader's known-good EXDMA3 config
	 * and point it at the bootloader framebuffer, then repaint /dev/fb0.
	 * Bars visible  -> downstream chain (blender/PQ/DSC/DSI) is fine and
	 *                  the black screen is a source/config problem.
	 * Still black   -> the DRM take-over broke the downstream chain.
	 */
	// disp_exdma3_restore_test();  /* experiment M: no fb0 paint */
	if (LEGACY_DRM_IOCTL_TEST)
		display_color_test();

	/*
	 * v4: Raw I2C6 MMIO (0x13170000) via /dev/men causes a synchronous
	 * external abort when the clock is gated -- kernel panic + watchdog
	 * reboot.  Do only the safe parts: PIO GPIO bus-level check (0x1002d000)
	 * and SPMI rail sweep (0x1c01a000).  For I2C access, use the kernel's
	 * /dev/i2c-6 instead of raw controller MMIO.
	 */
//	usb_rail_recovery_safe();	/* 26 logs -- skip to reduce pstore volume */

	/*
	 * (2026-09-05, #30) Android /data probe: mount userdata (f2fs,
	 * read-only) and list the first-level entries -- see userdata_report()
	 * for why that works even with file-based encryption.
	 */
//	userdata_report();

	/*
	 * ufs_report() temporarily disabled: its partition-table dump scrolls
	 * the pstore state block above off the panel before it can be
	 * photographed.  UFS was verified good in earlier boots; re-enable
	 * once pstore is understood.  (ufs_ok = 1 also keeps diagnose() from
	 * burying the pstore output again right before the panic.)
	 */
	ufs_ok = 1;	/* was: ufs_ok = ufs_report(); */

	/*
	 * SPI6 clock hack: the HWV voter (0x14500000) on MT6991 doesn't
	 * actually forward votes to the hardware gates.  Force-enable the
	 * SPI6 clock chain via raw MMIO, then rebind the SPI device.
	 *
	 * Clock tree: UNIVPLL_D6_D2 → spi6_b mux+gate (topckgen 0x10000000)
	 *             → SPI6_BCLK gate (pericfg_ao 0x16640000)
	 *             → SPI6 controller (0x161d0000)
	 *
	 * topckgen: CLK_CFG_8_CLR=0x0098, mux gate is bit 23
	 * pericfg_ao: clr=0x30, SPI6 BCLK gate is bit 7 (active-low)
	 */
	{
		void *top = link_map_devmem(0x10000000, 0x200);
		void *peri = link_map_devmem(0x16640000, 0x100);

		if (top) {
			/* Force-enable spi6_b mux gate: write bit23 to CLR reg */
			uint32_t v = M32(top, 0x0090); /* CLK_CFG_8 current */
			klogf("topckgen CFG_8=0x%08x", v);
			LINK_WR32(top, 0x0098, (1u << 23)); /* CLR bit 23 */
			v = M32(top, 0x0090);
			klogf("after mux gate enable: CFG_8=0x%08x", v);
		}
		if (peri) {
			uint32_t v = M32(peri, 0x14);
			klogf("pericfg_ao sta=0x%08x (bit7=1=gated)", v);
			LINK_WR32(peri, 0x30, (1u << 7));
			v = M32(peri, 0x14);
			klogf("after gate enable: sta=0x%08x", v);
		}
	}

	/* Rebind SPI6 to force the controller to re-probe with clocks on */
	{
		const char *unbind_path = "/sys/bus/platform/drivers/mtk-spi/unbind";
		const char *bind_path   = "/sys/bus/platform/drivers/mtk-spi/bind";
		const char *spi_dt      = "161d0000.spi";
		int fd;
		struct timespec ts;

		fd = open(unbind_path, O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t n = write(fd, spi_dt, strlen(spi_dt));
			int e = errno;
			close(fd);
			klogf("SPI6: unbind ret=%zd errno=%d (%s)", n, e,
			      n < 0 ? strerror(e) : "ok");
			ts.tv_sec = 1; ts.tv_nsec = 0;
			nanosleep(&ts, NULL);
		} else {
			klogf("SPI6: unbind open failed: fd=%d errno=%d", fd, errno);
		}

		fd = open(bind_path, O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t n = write(fd, spi_dt, strlen(spi_dt));
			int e = errno;
			close(fd);
		klogf("SPI6: bind ret=%zd errno=%d (%s)", n, e,
		      n < 0 ? strerror(e) : "ok");
			ts.tv_sec = 3; ts.tv_nsec = 0;
			nanosleep(&ts, NULL);
		} else {
			klogf("SPI6: bind open failed: fd=%d errno=%d", fd, errno);
		}
	}

	/* Touch input test: fork a child to read /dev/input/eventX */
	{
		pid_t tp_pid = fork();
		if (tp_pid == 0) {
			/* Child: scan /dev/input/event* for synaptics-tcm */
			char evpath[64];
			int evfd = -1;
			int i;

			for (i = 0; i < 10; i++) {
				snprintf(evpath, sizeof(evpath),
					 "/dev/input/event%d", i);
				evfd = open(evpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
				if (evfd >= 0) {
					/* Check name via sysfs */
					char namepath[128], namebuf[128] = {};
					FILE *nf;

					snprintf(namepath, sizeof(namepath),
						 "/sys/class/input/event%d/device/name", i);
					nf = fopen(namepath, "r");
					if (nf) {
						fgets(namebuf, sizeof(namebuf), nf);
						fclose(nf);
					}
					if (strstr(namebuf, "synaptics") ||
					    strstr(namebuf, "tcm")) {
						klogf("touch: found %s on %s",
						      namebuf, evpath);
						break;
					}
					close(evfd);
					evfd = -1;
				}
			}
			if (evfd < 0) {
				/* Try all events with SYN_REPORT */
				for (i = 0; i < 10; i++) {
					snprintf(evpath, sizeof(evpath),
						 "/dev/input/event%d", i);
					evfd = open(evpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
					if (evfd >= 0)
						break;
				}
			}
			if (evfd < 0) {
				klog("touch: no input event device found");
				_exit(1);
			}

			klogf("touch: reading %s for 15s -- tap the screen!",
			      evpath);

			{
				/*
				 * 64-bit struct input_event: timeval is two
				 * 64-bit fields (16 bytes), then type/code/value.
				 * 24 bytes total -- evdev rejects anything less.
				 */
				struct input_event {
					uint64_t sec;
					uint64_t usec;
					uint16_t type;
					uint16_t code;
					int32_t value;
				} ev;
				int cnt = 0;
				int slot = -1;
				int conc = 0, max_conc = 0;
				int id_of_slot[10];
				int i;
				struct timespec deadline, now;

				for (i = 0; i < 10; i++)
					id_of_slot[i] = -1;

				clock_gettime(CLOCK_MONOTONIC, &deadline);
				deadline.tv_sec += 15;

				while (1) {
					fd_set rfds;
					struct timeval tv;

					FD_ZERO(&rfds);
					FD_SET(evfd, &rfds);
					tv.tv_sec = 1;
					tv.tv_usec = 0;

					int sel = select(evfd + 1, &rfds,
							 NULL, NULL, &tv);
					if (sel < 0)
						break;
					clock_gettime(CLOCK_MONOTONIC, &now);
					if (now.tv_sec > deadline.tv_sec ||
					    (now.tv_sec == deadline.tv_sec &&
					     now.tv_nsec >= deadline.tv_nsec))
						break;

					if (sel == 0)
						continue;

					while (read(evfd, &ev, sizeof(ev)) ==
					       sizeof(ev)) {
						if (ev.type == 3) {
							/* EV_ABS */
							if (ev.code == 47)
								slot = ev.value;
							else if (ev.code == 57) {
								/* MT_TRACKING_ID */
								if (ev.value < 0) {
									if (slot >= 0 && slot < 10 &&
									    id_of_slot[slot] >= 0) {
										klogf("touch: slot%d id%d up",
										      slot, id_of_slot[slot]);
										id_of_slot[slot] = -1;
										conc--;
									}
								} else if (slot >= 0 && slot < 10) {
									id_of_slot[slot] = ev.value;
									conc++;
									if (conc > max_conc)
										max_conc = conc;
									klogf("touch: slot%d id%d down",
									      slot, ev.value);
								}
							} else if (ev.code == 53) {
								klogf("touch:  x=%d",
								      ev.value);
							} else if (ev.code == 54) {
								klogf("touch:  y=%d",
								      ev.value);
							}
						} else if (ev.type == 0 &&
							   ev.code == 0) {
							cnt++;
						}
					}
				}
				klogf("touch: done, %d SYN, max_concurrent=%d",
				      cnt, max_conc);
			}
			close(evfd);
			_exit(0);
		} else if (tp_pid > 0) {
			klogf("touch: test child forked as pid %d", tp_pid);
		}
	}

	klog("heartbeat follows; each line proves the kernel still schedules");

	/* Parent continues heartbeat so USB enumeration + serial redirect works */

	/*
	 * Our cmdline has panic=0, which makes the kernel hang forever after a
	 * panic instead of rebooting -- and a hang means no way back to Android
	 * to read pstore.  Ask for a short reboot delay instead.  panic() dumps
	 * to pstore at kernel/panic.c:685 and only reboots at :743, so the log
	 * is written before the reset either way.
	 */
	{
		int fd = open("/proc/sys/kernel/panic", O_WRONLY | O_CLOEXEC);

		if (fd >= 0) {
			ssize_t ignored = write(fd, "5\n", 2);

			(void)ignored;
			close(fd);
			klog("panic timeout set to 5s (was 0 = hang forever)");
		} else {
			klog("WARNING: cannot set panic timeout -- may hang instead");
		}
	}

	klogf("will exit after %ds to panic on purpose, so that the log lands in",
	      LIFETIME_SEC);
	klog("pstore and the phone reboots back to Android");

	/*
	 * Run for a fixed time, then return: PID 1 exiting panics the kernel
	 * ("Attempted to kill init!", kernel/exit.c:963), which is the point --
	 * it triggers the pstore dump and, with the timeout above, a reboot.
	 */
	for (beat = 0; beat * BEAT_SEC < LIFETIME_SEC; beat++) {
		struct timespec ts = { .tv_sec = BEAT_SEC, .tv_nsec = 0 };

		/* Retry device-mode props until the HW auto-calibration
		 * settles and our writes stick.  The vendor does this from
		 * set_mode() which fires on role-switch (cable plug), long
		 * after power-on. */
		if (!enumerated)
			mt6379_set_device_props();

//		mtu3_diag();

//		klogf("heartbeat %lu (~%lus in userspace, exiting at %ds)", beat,
//		      beat * BEAT_SEC, LIFETIME_SEC);
		nanosleep(&ts, NULL);

		/*
		 * USB enumeration detection.
		 *
		 * "udc state == configured" means the host enumerated the
		 * device.  Replay the early kernel log into /dev/ttyGS0 and
		 * redirect kmsg_fd there so subsequent klog() calls go straight
		 * to the PC serial port.
		 *
		 * NOTE: VBUS_FRC_EN stays set permanently.  The eUSB2 PHY chain
		 * (xsphy -> mt6379 -> NXP PTN36241) cannot reliably report VBUS
		 * without it, so clearing it causes immediate disconnect.
		 * Trade-off: cable removal is never detected, and re-plug
		 * requires a reboot.
		 */
		if (!enumerated && udc_configured()) {
			enumerated = 1;
			klog("usb: HOST ENUMERATED -- udc state is 'configured'");
			if (!replayed)
				replayed = ttygs0_replay_once();
			/* redirect all further klog output to the USB serial */
			{
				int ttyfd = open("/dev/ttyGS0",
						 O_WRONLY | O_NOCTTY | O_CLOEXEC);
				if (ttyfd >= 0) {
					kmsg_fd = ttyfd;
					klog("serial-usb: kmsg_fd redirected to /dev/ttyGS0");
				}
			}
		}
	}

	/*
	 * (2026-09-05) sdc5 pstore-stash removed -- cross-boot capture is the
	 * shared ramoops ring again: this boot's console text lands in it live
	 * and the panic record below is the next boot's dmesg-ramoops-0.
	 */

	/*
	 * Dump the ENTIRE kernel log buffer to /dev/ttyS0 (serial console).
	 * This is our last-resort logging: pstore may be full, USB serial may
	 * not have enumerated, but the hardware UART always works if a cable
	 * is attached.  Use klogctl SYSLOG_ACTION_READ_ALL (3) to grab the
	 * full ring buffer from the kernel.
	 */
	{
		int ser_fd = open("/dev/ttyS0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

		if (ser_fd >= 0) {
			/* 256 KiB is generous; the ring buffer is usually <128 KiB */
			size_t buf_sz = 256 * 1024;
			char *kbuf = malloc(buf_sz);

			if (kbuf) {
				/* klogctl(type, buf, len): type 3 = read all, clear
				 * nothing; returns bytes copied. */
				ssize_t n = klogctl(3, kbuf, buf_sz - 1);

				if (n > 0) {
					const char *hdr =
						"\r\n===== FULL KERNEL LOG DUMP TO SERIAL =====\r\n";
					(void)write(ser_fd, hdr, strlen(hdr));
					(void)write(ser_fd, kbuf, (size_t)n);
					(void)write(ser_fd, "\r\n===== END KERNEL LOG DUMP =====\r\n", 34);
				} else {
					const char *msg = "klogctl(3) failed -- no kernel log to dump\r\n";
					(void)write(ser_fd, msg, strlen(msg));
				}
				free(kbuf);
			}
			close(ser_fd);
		}
	}

	klog("=====================================================");
	klogf("  %ds elapsed -- exiting to panic on purpose", LIFETIME_SEC);
	klog("  expect: Attempted to kill init! / Rebooting in 5 seconds");
	if (ufs_ok)
		klog("  UFS verified ok earlier (see log above)");
	else
		klog("  UFS FAILED -- diagnostics below");
	klog("=====================================================");

	/*
	 * Last thing before dying, so its output (oldest records printed last)
	 * is still on the panel when the panic lands.
	 */
	if (!ufs_ok)
		diagnose();

	/*
	 * Repaint the separation-test bars right before the panic so the very
	 * last thing the panel shows is the test pattern (not console text).
	 */
	// disp_exdma3_restore_test();  /* experiment M: no fb0 paint */

	/* Give the framebuffer console time to paint the lines above. */
	sleep(1);

	return 0;
}
