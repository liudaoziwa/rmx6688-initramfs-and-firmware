// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal PID 1 for booting the Ubuntu rootfs with the MT6991 mainline
 * kernel.
 *
 * Companion to init.c (the bring-up test init, which runs the display
 * modeset/rate tests, the touch input test and then panics on purpose).
 * This one does only what a normal boot needs:
 *
 *   1. mount /proc /sys /dev
 *   2. raise the MT6363 MD rails if the regulator driver did not
 *   3. enable the MT6681 audio codec and release the speaker amp resets
 *   4. start the real-time kernel log mirror on the ext4 log partition
 *   5. load the SCP connector modules, wait for SCP ready, start the modem
 *   6. locate the rootfs by its ext4 UUID, mount it and switch_root into
 *      the distro init
 *
 * USB-C and the touch rail are kernel-owned (mtu3/xsphy/repeater and
 * mt6991-pmic-early), so init never touches them.
 *
 * The touch firmware stays in the initramfs: the touch driver requests it
 * during probe, long before any rootfs is mounted.
 *
 * Built static -- there is no libc in the initramfs.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/*
 * Rootfs identity.  The image was created with
 *
 *     fallocate -l 4G ubuntu-rootfs.img
 *     mkfs.ext4 -F -L rootfs ubuntu-rootfs.img
 *
 * and dd'ed to its own GPT partition (partition 84, GPT name "rootfs"), so
 * the filesystem UUID travels with the data and survives any sdX letter
 * change.  The filesystem label and the GPT partition name ("rootfs") are
 * secondary fallbacks.
 */
#define ROOTFS_UUID	"4958add9-6624-490f-8374-533af2b7754d"
#define ROOTFS_LABEL	"rootfs"
#define ROOTFS_FSTYPE	"ext4"

static int kmsg_fd = -1;

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
 * sysfs attribute files report no meaningful size before they are read, so
 * read() is the only way to size them.
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

static void sleep_us(long us)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = us * 1000 };

	nanosleep(&ts, NULL);
}

/* ---- rootfs location ------------------------------------------------- */

/*
 * ext4 superblock at fs offset 1024: magic 0xEF53 at +0x38, the 16-byte
 * filesystem UUID at +0x68, the 16-byte volume label at +0x78.
 */
static void uuid_parse(const char *str, unsigned char out[16])
{
	int i = 0, nib = -1;
	unsigned char b = 0;

	memset(out, 0, 16);
	for (; *str && i < 16; str++) {
		int v;

		if (*str >= '0' && *str <= '9')
			v = *str - '0';
		else if (*str >= 'a' && *str <= 'f')
			v = *str - 'a' + 10;
		else if (*str >= 'A' && *str <= 'F')
			v = *str - 'A' + 10;
		else
			continue;
		if (nib < 0) {
			b = (unsigned char)(v << 4);
			nib = 0;
		} else {
			out[i++] = b | v;
			nib = -1;
		}
	}
}

static int ext4_probe(const char *dev, unsigned char uuid[16], char label[17])
{
	unsigned char sb[1024];
	ssize_t n;
	int fd;

	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	n = pread(fd, sb, sizeof(sb), 1024);
	close(fd);
	if (n != (ssize_t)sizeof(sb))
		return 0;
	if (sb[0x38] != 0x53 || sb[0x39] != 0xef)	/* s_magic, LE */
		return 0;
	memcpy(uuid, sb + 0x68, 16);
	memcpy(label, sb + 0x78, 16);
	label[16] = '\0';
	return 1;
}

/*
 * Scan the sd* partitions for the rootfs.  The GPT is read off the media
 * by the UFS driver, which probes around the time init starts, so retry
 * for a few seconds before giving up.
 */
static int find_rootfs(char *dev, size_t devsz)
{
	unsigned char want[16];
	char fallback[128] = "";
	int tries;

	uuid_parse(ROOTFS_UUID, want);

	for (tries = 0; tries < 20; tries++) {
		struct dirent *e;
		DIR *d = opendir("/sys/class/block");

		if (d) {
			while ((e = readdir(d))) {
				char path[300], buf[512], label[17];
				char pname[64] = "";
				unsigned char uuid[16];
				char *p;

				if (strncmp(e->d_name, "sd", 2) != 0)
					continue;
				/* partitions only: /sys/.../<sdX>/partition exists */
				snprintf(path, sizeof(path),
					 "/sys/class/block/%s/partition",
					 e->d_name);
				if (slurp(path, buf, sizeof(buf)) <= 0)
					continue;
				snprintf(path, sizeof(path), "/dev/%s", e->d_name);
				if (!ext4_probe(path, uuid, label))
					continue;

				snprintf(path, sizeof(path),
					 "/sys/class/block/%s/uevent", e->d_name);
				if (slurp(path, buf, sizeof(buf)) > 0 &&
				    (p = strstr(buf, "PARTNAME=")) != NULL) {
					size_t l;

					p += 9;
					l = strcspn(p, "\n");
					if (l >= sizeof(pname))
						l = sizeof(pname) - 1;
					memcpy(pname, p, l);
					pname[l] = '\0';
				}

				if (memcmp(uuid, want, 16) == 0) {
					snprintf(dev, devsz, "/dev/%.60s", e->d_name);
					closedir(d);
					klogf("rootfs: found %s by uuid (label '%s')",
					      dev, label);
					return 1;
				}
				if (!fallback[0] &&
				    (strcmp(label, ROOTFS_LABEL) == 0 ||
				     strcmp(pname, ROOTFS_LABEL) == 0)) {
					snprintf(fallback, sizeof(fallback),
						 "/dev/%.60s", e->d_name);
					klogf("rootfs: %s matches label/partname '%s'",
					      fallback, ROOTFS_LABEL);
				}
			}
			closedir(d);
		}
		if (tries == 0)
			klog("rootfs: waiting for the ext4 rootfs to appear");
		sleep_us(300000);
	}

	if (fallback[0]) {
		snprintf(dev, devsz, "%s", fallback);
		klogf("rootfs: falling back to %s", dev);
		return 1;
	}
	return 0;
}

/* ---- switch_root ----------------------------------------------------- */

static const char *rootfs_inits[] = {
	"/sbin/init",			/* systemd (Ubuntu) */
	"/usr/lib/systemd/systemd",
	"/bin/init",
};

/*
 * Canonical switch_root: mount the target, check for an init, move the
 * virtual filesystems over, then MS_MOVE + chroot + exec.  execv does not
 * change the PID, so the distro init takes over as PID 1.
 *
 * Returns only on failure; PID 1 then exits and the kernel panics.
 */
static int copy_file(const char *src, const char *dst, mode_t mode)
{
	char buf[8192];
	int in, out;
	ssize_t n, w;
	off_t off;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		for (off = 0; off < n; off += w) {
			w = write(out, buf + off, n - off);
			if (w <= 0) {
				close(in);
				close(out);
				return -1;
			}
		}
	}
	if (n < 0 || close(out) != 0) {
		close(in);
		return -1;
	}
	close(in);
	return 0;
}

static const char *rootfs_pick_init(void)
{
	static char path[128];
	unsigned int i;

	for (i = 0; i < sizeof(rootfs_inits) / sizeof(rootfs_inits[0]); i++) {
		snprintf(path, sizeof(path), "/newroot%s", rootfs_inits[i]);
		if (access(path, X_OK) == 0)
			return rootfs_inits[i];
	}
	return NULL;
}

/*
 * main() mounts the rootfs early (before the modem starts) so the vendor
 * partitions can be put in place underneath it; switch_root_to() then
 * re-checks instead of mounting again.
 */
static int mount_newroot(const char *dev)
{
	mkdir("/newroot", 0755);
	if (mount(dev, "/newroot", ROOTFS_FSTYPE, 0, NULL) != 0) {
		klogf("rootfs: mount %s failed: %s", dev, strerror(errno));
		return -1;
	}
	klogf("rootfs: %s mounted", dev);
	if (!rootfs_pick_init()) {
		klog("rootfs: no usable init inside -- wrong filesystem?");
		umount("/newroot");
		return -1;
	}
	return 0;
}

static void switch_root_to(const char *dev)
{
	const char *init;

	if (!rootfs_pick_init() && mount_newroot(dev) != 0)
		return;
	init = rootfs_pick_init();
	if (!init)
		return;

	/*
	 * The initramfs is discarded by the switch_root below, so keep the
	 * raw-SPMI rail tool and the rail test script reachable from the
	 * running system.
	 */
	mkdir("/newroot/usr/local/bin", 0755);
	{
		static const char *tools[] = { "spmirail", "railtest" };
		char src[128], dst[160];
		unsigned int t;

		for (t = 0; t < sizeof(tools) / sizeof(tools[0]); t++) {
			snprintf(src, sizeof(src), "/bin/%s", tools[t]);
			if (access(src, R_OK) != 0)
				continue;
			snprintf(dst, sizeof(dst),
				 "/newroot/usr/local/bin/%s", tools[t]);
			if (copy_file(src, dst, 0755) == 0)
				klogf("%s: %s -> %s", tools[t], src, dst);
			else
				klogf("%s: install failed: %s", tools[t],
				      strerror(errno));
		}
	}

	/* carry the live virtual filesystems into the new root */
	mkdir("/newroot/dev", 0755);
	mkdir("/newroot/proc", 0755);
	mkdir("/newroot/sys", 0755);
	if (mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL) != 0 ||
	    mount("/proc", "/newroot/proc", NULL, MS_MOVE, NULL) != 0 ||
	    mount("/sys", "/newroot/sys", NULL, MS_MOVE, NULL) != 0) {
		klogf("rootfs: moving /dev /proc /sys failed: %s",
		      strerror(errno));
		return;
	}

	if (chdir("/newroot") != 0 ||
	    mount("/newroot", "/", NULL, MS_MOVE, NULL) != 0 ||
	    chroot(".") != 0) {
		klogf("rootfs: switch_root failed: %s", strerror(errno));
		return;
	}

	klogf("rootfs: exec %s", init);
	execl(init, init, NULL);
	klogf("rootfs: exec %s failed: %s", init, strerror(errno));
}

/*
 * Minimal replacement for Android's ccci_mdinit daemon.
 *
 * The CCCI FSM boots in CCCI_FSM_GATED and only leaves it when a process
 * named "ccci_mdinit" issues CCCI_IOC_DO_START_MD on /dev/ccci_monitor
 * (ccci_fsm_ioctl() enforces the process name).  There is no such daemon in
 * the Ubuntu rootfs, so the modem would otherwise never start.
 *
 * The child keeps the monitor node open for the lifetime of the system: the
 * release path (dev_char_close) forces the modem to stop, and the node is
 * single-open.
 */
#define CCCI_IOC_DO_START_MD	0x430D		/* _IO('C', 13)  */
#define CCCI_IOC_DO_STOP_MD	0x430C		/* _IO('C', 12)  */
#define CCCI_IOC_GET_MD_STATE	0x80044301	/* _IOR('C', 1, unsigned int) */

#define MD_STATE_INVALID	0
#define MD_STATE_EXCEPTION	3

/*
 * After an exception the FSM parks in CCCI_FSM_EXCEPTION, and
 * fsm_routine_start() only accepts a START from CCCI_FSM_GATED -- so a bare
 * DO_START_MD would just land in fsm_routine_zombie().  Android's mdinit
 * recovers with STOP then START (fsm_routine_stop() explicitly allows
 * starting from the exception state, and ends in GATED).  Nobody did that
 * here, which is why the modem stayed dead after every EE.
 *
 * The STOP command is queued behind the still-running EE command, so the
 * exception dump is never cut short.
 */
#define EE_RESTART_POLLS	3	/* x2 s of EXCEPTION before restarting */

static void mdinit_restart(int fd, unsigned int *last)
{
	unsigned int mode = 0;
	int i;

	klog("mdinit: modem EXCEPTION, restarting (STOP + START)");
	if (ioctl(fd, CCCI_IOC_DO_STOP_MD, &mode) < 0) {
		klogf("mdinit: DO_STOP_MD failed: %s", strerror(errno));
		return;
	}
	for (i = 0; i < 30; i++) {
		unsigned int st = 0;

		sleep(1);
		if (ioctl(fd, CCCI_IOC_GET_MD_STATE, &st) < 0)
			continue;
		if (st != *last) {
			klogf("mdinit: md state=%u", st);
			*last = st;
		}
		if (st == MD_STATE_INVALID)
			break;
	}
	if (ioctl(fd, CCCI_IOC_DO_START_MD, 0) < 0)
		klogf("mdinit: DO_START_MD (restart) failed: %s",
		      strerror(errno));
	else
		klog("mdinit: MD restart issued");
	*last = ~0U;
}

static void mdinit_launcher(void)
{
	pid_t pid;
	int fd;
	unsigned int state = 0;
	unsigned int last = ~0U;
	int ex_cnt = 0;

	pid = fork();
	if (pid < 0) {
		klogf("mdinit: fork failed: %s", strerror(errno));
		return;
	}
	if (pid != 0)
		return;		/* parent keeps booting */

	setsid();
	prctl(PR_SET_NAME, "ccci_mdinit", 0, 0, 0);

	fd = open("/dev/ccci_monitor", O_RDWR | O_NOCTTY);
	if (fd < 0) {
		klogf("mdinit: open /dev/ccci_monitor failed: %s",
		      strerror(errno));
		_exit(0);
	}

	if (ioctl(fd, CCCI_IOC_DO_START_MD, 0) < 0)
		klogf("mdinit: DO_START_MD failed: %s", strerror(errno));
	else
		klog("mdinit: DO_START_MD issued");

	for (;;) {
		sleep(2);
		if (ioctl(fd, CCCI_IOC_GET_MD_STATE, &state) == 0 &&
		    state != last) {
			klogf("mdinit: md state=%u", state);
			last = state;
		}
		if (state == MD_STATE_EXCEPTION) {
			if (++ex_cnt >= EE_RESTART_POLLS) {
				ex_cnt = 0;
				mdinit_restart(fd, &last);
			}
		} else
			ex_cnt = 0;
	}
}

/*
 * Load the SCP driver as a module.  It cannot be built into vmlinux on this
 * port (linking the scp/rv objects in makes the ATF touch an unpowered SCP
 * and wedge the SoC at boot), so it ships in the initramfs and is loaded here
 * before the modem starts, so the SCP is up for the MD CCISM handshake.
 */
static void load_kernel_module_params(const char *path, const char *params)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		klogf("%s: not present (%s)", path, strerror(errno));
		return;
	}
	if (syscall(SYS_finit_module, fd, params, 0) != 0)
		klogf("%s: finit_module failed: %s", path, strerror(errno));
	else
		klogf("%s: loaded '%s'", path, params);
	close(fd);
}

static void load_kernel_module(const char *path)
{
	load_kernel_module_params(path, "");
}

static void load_scp_module(void)
{
	/*
	 * Vendor MT6991 clock tree first: scp-dvfs does devm_clk_get("clk_mux")
	 * on the scp-dvfs node, which is provided by vlp_cksys/cksys.
	 */
	load_kernel_module("/lib/modules/clk-mt6991-drv.ko");
	load_kernel_module("/lib/modules/scp.ko");
	/*
	 * AP-side CCISM glue: registers the ccci_md_scp platform driver and
	 * talks to the SCP, completing the MD<->SCP handshake.  Must load
	 * after scp.ko because it uses scp.ko's exported symbols.
	 */
	load_kernel_module("/lib/modules/ccci_fsm_scp.ko");
	/* connectivity<->SCP bridge (uses scp.ko's exported symbols) */
	load_kernel_module("/lib/modules/connscp.ko");
	/*
	 * SCP sensorhub liveness probe: registers the vendor sensorhub IPI
	 * channels and performs the ready handshake (uses scp.ko's exported
	 * symbols).
	 */
	load_kernel_module_params("/lib/modules/sensorhub_probe.ko",
				  "shm_mask=0x07");
	/*
	 * MT6991 GPS data-link driver (vendor v062).  Binds to the
	 * gps@40c00000 DT node and exposes /dev/stpgps; the built-in
	 * conninfra provides the EMI/power/register API it links against.
	 */
	load_kernel_module("/lib/modules/gps_drv_dl_v062.ko");
}

/*
 * Wait until scp.ko reports the SCP firmware fully booted (SCP_EVENT_READY,
 * which is only delivered after ULPOSC calibration).  Starting the modem
 * while the SCP is still resetting is what leaves the MD stuck in
 * BOOT_WAITING_FOR_HS2 (the MD CCISM peer is not there yet), so gate the
 * modem start on this.
 */
static void wait_scp_ready(void)
{
	const char *path = "/sys/class/misc/scp/scp_ap_ready";
	int i;

	for (i = 0; i < 200; i++) {
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		char b[8] = { 0 };

		if (fd >= 0) {
			int n = read(fd, b, sizeof(b) - 1);

			close(fd);
			if (n > 0 && b[0] == '1') {
				klog("scp: ready");
				return;
			}
		}
		usleep(100 * 1000);	/* 100 ms, up to 20 s */
	}
	klog("scp: not ready after 20s, starting modem anyway");
}

/* ---- vendor partitions: backing store for the CCCI FS (X:/Z:/T:) ----- */

/*
 * Mainline has no ccci_mdinit, so port_char serves the modem's file
 * requests straight from the phone's own partitions: the driver walks
 * /mnt/vendor/... and answers -9 when a partition or file is missing,
 * exactly like stock.  Mount them under /newroot before the modem starts;
 * the driver also tries the pre-switch alias /newroot/mnt/vendor/... for
 * early requests (in practice the first FS request arrives after
 * switch_root).
 */
#define VENDOR_ROOT "/newroot/mnt/vendor"

static int str_has_line(const char *buf, int len, const char *needle)
{
	int nl = strlen(needle);
	int i;

	for (i = 0; i + nl <= len; i++) {
		if (memcmp(buf + i, needle, nl) != 0)
			continue;
		if (i + nl == len || buf[i + nl] == '\n')
			return 1;
	}
	return 0;
}

/* GPT partition name (PARTNAME= in the block device uevent) -> /dev node */
static int part_by_name(const char *name, char *out, size_t outsz)
{
	char needle[40];
	DIR *d;
	struct dirent *e;
	int ret = -1;

	snprintf(needle, sizeof(needle), "PARTNAME=%s", name);
	d = opendir("/dev");
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char path[128], buf[512];
		int n;

		if (strncmp(e->d_name, "sd", 2) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/block/%s/uevent",
			 e->d_name);
		n = slurp(path, buf, sizeof(buf));
		if (n <= 0)
			continue;
		if (!str_has_line(buf, n, needle))
			continue;
		snprintf(out, outsz, "/dev/%s", e->d_name);
		ret = 0;
		break;
	}
	closedir(d);
	return ret;
}

static int mount_vendor_one(const char *const *names, const char *dir)
{
	char dev[64], full[160];
	int i;

	for (i = 0; names[i]; i++) {
		if (part_by_name(names[i], dev, sizeof(dev)) != 0)
			continue;
		snprintf(full, sizeof(full), "%s/%s", VENDOR_ROOT, dir);
		mkdir(full, 0755);
		if (mount(dev, full, "ext4", 0, NULL) != 0) {
			klogf("fs: mount %s on %s failed: %s", dev, full,
			      strerror(errno));
			return -1;
		}
		klogf("fs: %s (%s) -> %s", names[i], dev, full);
		return 0;
	}
	klogf("fs: partition '%s' not found", names[0]);
	return -1;
}

static void mount_vendor_partitions(void)
{
	static const char *const nvdata[] = { "nvdata", NULL };
	static const char *const protect_f[] = { "protect1", "protect_f", NULL };
	static const char *const protect_s[] = { "protect2", "protect_s", NULL };
	/* Android's /mnt/vendor/mdota: GPT name is slot-suffixed (mcf_ota_a) */
	static const char *const mdota[] = { "mcf_ota_a", "mcf_ota_b", "mdota",
					     NULL };
	int ok = 0;

	mkdir("/newroot/mnt", 0755);
	mkdir(VENDOR_ROOT, 0755);
	ok += mount_vendor_one(nvdata, "nvdata") == 0;
	ok += mount_vendor_one(protect_f, "protect_f") == 0;
	ok += mount_vendor_one(protect_s, "protect_s") == 0;
	ok += mount_vendor_one(mdota, "mdota") == 0;
	if (access("/newroot/mnt/vendor/nvdata/md", F_OK) != 0)
		klog("fs: nvdata partition has no /md tree");
	if (access("/newroot/mnt/vendor/protect_f/md", F_OK) != 0)
		klog("fs: protect_f partition has no /md tree");
	klogf("fs: vendor partitions mounted: %d/4 (CCCI FS X:/Z:/T:)", ok);
}

/* ---- main ------------------------------------------------------------ */

/* ---- real-time kernel log to an ext4 partition ----------------------- */

/*
 * A panic in the SCP/module bring-up can happen before pstore is flushed,
 * so the crash output is lost.  Mirror the kernel ring buffer straight to a
 * file on a writable ext4 partition: mount it and keep the log at the root
 * of the mount point.
 */
#define LOGMNT_DIR	"/logmnt"
#define LOGMNT_FILE	LOGMNT_DIR "/kernel.log"

static int mount_log_partition(void)
{
	static const char *const cands[] = {
		"/dev/sdc5", "/dev/block/sdc5", "/dev/sdc", NULL
	};
	char buf[512], label[17];
	unsigned char uuid[16];
	DIR *d;
	struct dirent *e;
	int i;

	if (mkdir(LOGMNT_DIR, 0755) != 0 && errno != EEXIST)
		return -1;

	for (i = 0; cands[i]; i++) {
		if (mount(cands[i], LOGMNT_DIR, "ext4", 0, NULL) == 0) {
			klogf("log: mounted %s on %s", cands[i], LOGMNT_DIR);
			return 0;
		}
	}
	klog("log: sdc5 mount failed, scanning ext4 partitions");

	d = opendir("/dev");
	if (d) {
		while ((e = readdir(d))) {
			char path[300];

			if (strncmp(e->d_name, "sd", 2) != 0)
				continue;
			snprintf(path, sizeof(path), "/sys/class/block/%s/partition",
				 e->d_name);
			if (slurp(path, buf, sizeof(buf)) <= 0)
				continue;
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			if (!ext4_probe(path, uuid, label))
				continue;
			if (strcmp(label, ROOTFS_LABEL) == 0)
				continue;
			if (mount(path, LOGMNT_DIR, "ext4", 0, NULL) == 0) {
				klogf("log: mounted %s (label '%s') on %s",
				      path, label, LOGMNT_DIR);
				closedir(d);
				return 0;
			}
		}
		closedir(d);
	}
	klog("log: no writable ext4 log partition found");
	return -1;
}

static void kmsg_logger(void)
{
	struct sched_param sp = { .sched_priority = 10 };
	char buf[4096];
	int rfd, wfd;

	prctl(PR_SET_NAME, "kmsg-logger", 0, 0, 0);
	sched_setscheduler(0, SCHED_FIFO, &sp);

	rfd = open("/dev/kmsg", O_RDONLY | O_CLOEXEC);
	if (rfd < 0)
		_exit(1);
	wfd = open(LOGMNT_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
		   0644);
	if (wfd < 0)
		_exit(1);

	for (;;) {
		ssize_t n = read(rfd, buf, sizeof(buf));

		if (n > 0) {
			size_t off = 0;

			while (off < (size_t)n) {
				ssize_t w = write(wfd, buf + off, (size_t)n - off);

				if (w <= 0)
					break;
				off += (size_t)w;
			}
			fsync(wfd);
		} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
			break;
		}
	}
	_exit(0);
}

/*
 * The AP dumps the MD's CCB (modem log ring), DBGSYS and exception info to
 * the /proc/ccci_dump ring buffers, but nothing reads them on this minimal
 * rootfs (Android has emdlogger).  Sample them repeatedly and keep the latest
 * copy on the ext4 log partition so the modem's own log survives a reboot.
 */
static void ccci_dump_logger(void)
{
	char *buf;
	int rfd, wfd;
	int i;

	prctl(PR_SET_NAME, "ccci-dump-logger", 0, 0, 0);

	buf = malloc(4 * 1024 * 1024);
	if (!buf)
		_exit(1);

	/*
	 * Open /proc/ccci_dump now: switch_root MS_MOVEs the /proc mount to
	 * the new root, so a later open("/proc/ccci_dump") would hit the now
	 * empty old /proc.  The proc read is incremental (read_idx vs
	 * write_pos), so polling this fd captures the CCB/modem log as the AP
	 * dumps it later.
	 */
	rfd = open("/proc/ccci_dump", O_RDONLY | O_CLOEXEC);
	wfd = open(LOGMNT_DIR "/ccci_dump.log",
		   O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (rfd < 0 || wfd < 0)
		_exit(1);

	for (i = 0; i < 200; i++) {
		ssize_t n = read(rfd, buf, 4 * 1024 * 1024);

		if (n > 0) {
			size_t off = 0;

			while (off < (size_t)n) {
				ssize_t w = write(wfd, buf + off,
						  (size_t)n - off);

				if (w <= 0)
					break;
				off += (size_t)w;
			}
			fsync(wfd);
		}
		sleep(5);
	}
	_exit(0);
}

static void start_kmsg_logger(void)
{
	pid_t pid;

	if (mount_log_partition() != 0)
		return;

	pid = fork();
	if (pid < 0) {
		klog("log: fork failed");
		return;
	}
	if (pid == 0)
		kmsg_logger();

	/* also capture the MD dump ring (the HS1->HS2 CCIF trace) */
	pid = fork();
	if (pid == 0)
		ccci_dump_logger();

	klog("log: real-time kmsg logging started");
}

/*
 * Preserve the previous boot's pstore records (console-ramoops above all)
 * on the log partition before the next reboot overwrites them, and log a
 * one-line summary per record so a missing panic log is visible in
 * kernel.log.
 */
static void snapshot_pstore(void)
{
	char src[256], dst[256], head[160];
	DIR *d;
	struct dirent *e;
	char *buf;
	int n = 0;

	/*
	 * /sys/fs/pstore is a kernel-created sysfs mount point
	 * (sysfs_create_mount_point in fs/pstore/inode.c).  mkdir()
	 * there always fails with EPERM, even though it exists.
	 */
	if (mkdir("/sys/fs/pstore", 0755) != 0 && errno != EEXIST &&
	    errno != EPERM) {
		klogf("pstore: mkdir failed (%d)", errno);
		return;
	}
	if (mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL) != 0) {
		klogf("pstore: mount failed (%d)", errno);
		return;
	}

	d = opendir("/sys/fs/pstore");
	if (!d) {
		klogf("pstore: opendir failed (%d)", errno);
		return;
	}
	mkdir(LOGMNT_DIR "/pstore", 0755);

	buf = malloc(65536);
	if (!buf) {
		closedir(d);
		return;
	}

	while ((e = readdir(d))) {
		size_t total = 0, copy = 0;
		ssize_t r;
		int rfd, wfd;

		if (e->d_name[0] == '.')
			continue;

		snprintf(src, sizeof(src), "/sys/fs/pstore/%s", e->d_name);
		rfd = open(src, O_RDONLY | O_CLOEXEC);
		if (rfd < 0)
			continue;

		head[0] = 0;
		snprintf(dst, sizeof(dst), LOGMNT_DIR "/pstore/%s", e->d_name);
		wfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		while ((r = read(rfd, buf, 65536)) > 0) {
			size_t i, j = 0;

			total += r;
			if (wfd >= 0 && copy < 8 * 1024 * 1024) {
				write(wfd, buf, r);
				copy += r;
			}
			if (head[0])
				continue;
			for (i = 0; i < (size_t)r && buf[i] != '\n'; i++)
				if (j < sizeof(head) - 1)
					head[j++] = (buf[i] >= 32 && buf[i] < 127) ?
						    buf[i] : ' ';
			head[j] = 0;
		}
		close(rfd);
		if (wfd >= 0) {
			fsync(wfd);
			close(wfd);
		}
		klogf("pstore: %s %zu bytes%s%s", e->d_name, total,
		      head[0] ? " first: " : "", head);
		n++;
	}
	free(buf);
	closedir(d);
	klogf("pstore: %d file(s) found", n);
}

int main(void)
{
	struct utsname u;
	char dev[128];

	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	try_mount("proc", "/proc", "proc");
	try_mount("sysfs", "/sys", "sysfs");
	try_mount("devtmpfs", "/dev", "devtmpfs");
	if (kmsg_fd < 0)
		kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	/*
	 * No firmware userspace helper exists in the initramfs.  Disable the
	 * sysfs fallback globally so request_firmware() fails fast instead of
	 * blocking the boot when an optional firmware file is missing.
	 */
	{
		int fd = open("/proc/sys/kernel/firmware_config/ignore_sysfs_fallback",
			      O_WRONLY | O_CLOEXEC);

		if (fd >= 0) {
			if (write(fd, "1", 1) >= 0)
				klog("firmware: sysfs fallback disabled");
			close(fd);
		}
	}

	klog("=== MT6991 rootfs boot init ===");
	if (uname(&u) == 0)
		klogf("running %s %s (%s)", u.sysname, u.release, u.machine);

	/*
	 * Let every init message through.  /dev/kmsg writes (what klog does)
	 * are throttled by the separate devkmsg ratelimiter (default burst 10
	 * / 5 s), NOT by printk_ratelimit; turn it fully on, and widen the
	 * regular printk ratelimit for good measure.
	 */
	{
		int fd = open("/proc/sys/kernel/printk_devkmsg",
			      O_WRONLY | O_CLOEXEC);

		if (fd >= 0) {
			ssize_t ignored = write(fd, "on", 2);

			(void)ignored;
			close(fd);
		}
		fd = open("/proc/sys/kernel/printk_ratelimit_burst",
			  O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t ignored = write(fd, "100000", 6);

			(void)ignored;
			close(fd);
		}
		fd = open("/proc/sys/kernel/printk_ratelimit",
			  O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t ignored = write(fd, "0", 1);

			(void)ignored;
			close(fd);
		}
	}

	/*
	 * MD power rails are kernel-owned: the md-rails child of the
	 * pmic-early node runs at subsys_initcall and repairs the MT6363
	 * vbuck4/5/6 rails if the bootloader left them down.
	 */

	/*
	 * USB-C is fully kernel-owned now (mtu3 -> xsphy -> MT6379 repeater
	 * -> connector); init does nothing for USB.
	 */

	/*
	 * Touch is kernel-owned too: mt6991-pmic-early raises touch-vtp at
	 * ~89 ms and syna-tcm probes on its own at ~7 s, so init never
	 * touches the rail and never rebinds the controller.
	 */

	/*
	 * Audio bring-up is kernel-owned too: mt6991-pmic-early runs the
	 * codec-en / amp-vaud18 / amp-pins / amp-reset sequences at ~0.3 s,
	 * well before the i2c6 driver probes the amps, so init neither
	 * pokes SPMI nor rebinds tfa98xx.
	 */

	/* real-time kernel log to the ext4 log partition (survives a panic) */
	start_kmsg_logger();

	/* keep the previous boot's panic records around */
	snapshot_pstore();

	/* bring up the SCP (module) before the modem needs the CCISM peer */
	load_scp_module();

	/* ...and let it finish booting before the modem's CCISM handshake */
	wait_scp_ready();

	/* mount and enter the distro rootfs */
	if (!find_rootfs(dev, sizeof(dev))) {
		klogf("rootfs: no ext4 rootfs found (uuid %s, label %s)",
		      ROOTFS_UUID, ROOTFS_LABEL);
		return 1;
	}
	if (mount_newroot(dev) != 0)
		return 1;

	/*
	 * Give the CCCI FS a real backing store before the modem boots:
	 * port_char serves X:/Z:/T: from these partitions (the embedded
	 * blobs remain the per-file fallback).
	 */
	mount_vendor_partitions();

	/* start the modem: CCCI FSM START, driven by a process named ccci_mdinit */
	mdinit_launcher();

	switch_root_to(dev);
	return 1;
}
