// SPDX-License-Identifier: GPL-2.0
/*
 * A test of splitting PMD THPs and PTE-mapped THPs from a specified virtual
 * address range in a process via <debugfs>/split_huge_pages interface.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <malloc.h>
#include <stdbool.h>
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include "vm_util.h"

uint64_t pagesize;
unsigned int pageshift;
uint64_t pmd_pagesize;

#define SPLIT_DEBUGFS "/sys/kernel/debug/split_huge_pages"
#define INPUT_MAX 80

#define PID_FMT "%d,0x%lx,0x%lx"
#define PATH_FMT "%s,0x%lx,0x%lx"

#define PFN_MASK     ((1UL<<55)-1)
#define KPF_THP      (1UL<<22)

int is_backed_by_thp(char *vaddr, int pagemap_file, int kpageflags_file)
{
	uint64_t paddr;
	uint64_t page_flags;

	if (pagemap_file) {
		pread(pagemap_file, &paddr, sizeof(paddr),
			((long)vaddr >> pageshift) * sizeof(paddr));

		if (kpageflags_file) {
			pread(kpageflags_file, &page_flags, sizeof(page_flags),
				(paddr & PFN_MASK) * sizeof(page_flags));

			return !!(page_flags & KPF_THP);
		}
	}
	return 0;
}

static int write_file(const char *path, const char *buf, size_t buflen)
{
	int fd;
	ssize_t numwritten;

	fd = open(path, O_WRONLY);
	if (fd == -1)
		return 0;

	numwritten = write(fd, buf, buflen - 1);
	close(fd);
	if (numwritten < 1)
		return 0;

	return (unsigned int) numwritten;
}

static void write_debugfs(const char *fmt, ...)
{
	char input[INPUT_MAX];
	int ret;
	va_list argp;

	va_start(argp, fmt);
	ret = vsnprintf(input, INPUT_MAX, fmt, argp);
	va_end(argp);

	if (ret >= INPUT_MAX) {
		printf("%s: Debugfs input is too long\n", __func__);
		exit(EXIT_FAILURE);
	}

	if (!write_file(SPLIT_DEBUGFS, input, ret + 1)) {
		perror(SPLIT_DEBUGFS);
		exit(EXIT_FAILURE);
	}
}

static char *allocate_zero_filled_hugepage(size_t len)
{
	char *result;
	size_t i;

	result = memalign(pmd_pagesize, len);
	if (!result) {
		printf("Fail to allocate memory\n");
		exit(EXIT_FAILURE);
	}

	madvise(result, len, MADV_HUGEPAGE);

	for (i = 0; i < len; i++)
		result[i] = (char)0;

	return result;
}

static void verify_rss_anon_split_huge_page_all_zeroes(char *one_page, int nr_hpages, size_t len)
{
	uint64_t rss_anon_before, rss_anon_after;
	size_t i;

	if (!check_huge_anon(one_page, 4, pmd_pagesize)) {
		printf("No THP is allocated\n");
		exit(EXIT_FAILURE);
	}

	rss_anon_before = rss_anon();
	if (!rss_anon_before) {
		printf("No RssAnon is allocated before split\n");
		exit(EXIT_FAILURE);
	}

	/* split all THPs */
	write_debugfs(PID_FMT, getpid(), (uint64_t)one_page,
		      (uint64_t)one_page + len);

	for (i = 0; i < len; i++)
		if (one_page[i] != (char)0) {
			printf("%ld byte corrupted\n", i);
			exit(EXIT_FAILURE);
		}

	if (!check_huge_anon(one_page, 0, pmd_pagesize)) {
		printf("Still AnonHugePages not split\n");
		exit(EXIT_FAILURE);
	}

	rss_anon_after = rss_anon();
	if (rss_anon_after >= rss_anon_before) {
		printf("Incorrect RssAnon value. Before: %ld After: %ld\n",
		       rss_anon_before, rss_anon_after);
		exit(EXIT_FAILURE);
	}
}

void split_pmd_zero_pages(void)
{
	char *one_page;
	int nr_hpages = 4;
	size_t len = nr_hpages * pmd_pagesize;

	one_page = allocate_zero_filled_hugepage(len);
	verify_rss_anon_split_huge_page_all_zeroes(one_page, nr_hpages, len);
	printf("Split zero filled huge pages successful\n");
	free(one_page);
}

void split_pmd_zero_pages_uffd(void)
{
	char *one_page;
	int nr_hpages = 4;
	size_t len = nr_hpages * pmd_pagesize;
	long uffd; /* userfaultfd file descriptor */
	struct uffdio_api uffdio_api;
	struct uffdio_register uffdio_register;

	/* Create and enable userfaultfd object. */

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd == -1) {
		perror("userfaultfd");
		exit(1);
	}

	uffdio_api.api = UFFD_API;
	uffdio_api.features = 0;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
		perror("ioctl-UFFDIO_API");
		exit(1);
	}

	one_page = allocate_zero_filled_hugepage(len);

	uffdio_register.range.start = (unsigned long)one_page;
	uffdio_register.range.len = len;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_WP;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
		perror("ioctl-UFFDIO_REGISTER");
		exit(1);
	}

	verify_rss_anon_split_huge_page_all_zeroes(one_page, nr_hpages, len);
	printf("Split zero filled huge pages with uffd successful\n");
	free(one_page);
}

void split_pmd_thp(void)
{
	char *one_page;
	size_t len = 4 * pmd_pagesize;
	size_t i;

	one_page = memalign(pmd_pagesize, len);

	if (!one_page) {
		printf("Fail to allocate memory\n");
		exit(EXIT_FAILURE);
	}

	madvise(one_page, len, MADV_HUGEPAGE);

	for (i = 0; i < len; i++)
		one_page[i] = (char)i;

	if (!check_huge_anon(one_page, 1, pmd_pagesize)) {
		printf("No THP is allocated\n");
		exit(EXIT_FAILURE);
	}

	/* split all THPs */
	write_debugfs(PID_FMT, getpid(), (uint64_t)one_page,
		(uint64_t)one_page + len);

	for (i = 0; i < len; i++)
		if (one_page[i] != (char)i) {
			printf("%ld byte corrupted\n", i);
			exit(EXIT_FAILURE);
		}

	if (check_huge_anon(one_page, 0, pmd_pagesize)) {
		printf("Still AnonHugePages not split\n");
		exit(EXIT_FAILURE);
	}

	printf("Split huge pages successful\n");
	free(one_page);
}

void split_pte_mapped_thp(void)
{
	char *one_page, *pte_mapped, *pte_mapped2;
	size_t len = 4 * pmd_pagesize;
	uint64_t thp_size;
	size_t i;
	const char *pagemap_template = "/proc/%d/pagemap";
	const char *kpageflags_proc = "/proc/kpageflags";
	char pagemap_proc[255];
	int pagemap_fd;
	int kpageflags_fd;

	if (snprintf(pagemap_proc, 255, pagemap_template, getpid()) < 0) {
		perror("get pagemap proc error");
		exit(EXIT_FAILURE);
	}
	pagemap_fd = open(pagemap_proc, O_RDONLY);

	if (pagemap_fd == -1) {
		perror("read pagemap:");
		exit(EXIT_FAILURE);
	}

	kpageflags_fd = open(kpageflags_proc, O_RDONLY);

	if (kpageflags_fd == -1) {
		perror("read kpageflags:");
		exit(EXIT_FAILURE);
	}

	one_page = mmap((void *)(1UL << 30), len, PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	madvise(one_page, len, MADV_HUGEPAGE);

	for (i = 0; i < len; i++)
		one_page[i] = (char)i;

	if (!check_huge_anon(one_page, 1, pmd_pagesize)) {
		printf("No THP is allocated\n");
		exit(EXIT_FAILURE);
	}

	/* remap the first pagesize of first THP */
	pte_mapped = mremap(one_page, pagesize, pagesize, MREMAP_MAYMOVE);

	/* remap the Nth pagesize of Nth THP */
	for (i = 1; i < 4; i++) {
		pte_mapped2 = mremap(one_page + pmd_pagesize * i + pagesize * i,
				     pagesize, pagesize,
				     MREMAP_MAYMOVE|MREMAP_FIXED,
				     pte_mapped + pagesize * i);
		if (pte_mapped2 == (char *)-1) {
			perror("mremap failed");
			exit(EXIT_FAILURE);
		}
	}

	/* smap does not show THPs after mremap, use kpageflags instead */
	thp_size = 0;
	for (i = 0; i < pagesize * 4; i++)
		if (i % pagesize == 0 &&
		    is_backed_by_thp(&pte_mapped[i], pagemap_fd, kpageflags_fd))
			thp_size++;

	if (thp_size != 4) {
		printf("Some THPs are missing during mremap\n");
		exit(EXIT_FAILURE);
	}

	/* split all remapped THPs */
	write_debugfs(PID_FMT, getpid(), (uint64_t)pte_mapped,
		      (uint64_t)pte_mapped + pagesize * 4);

	/* smap does not show THPs after mremap, use kpageflags instead */
	thp_size = 0;
	for (i = 0; i < pagesize * 4; i++) {
		if (pte_mapped[i] != (char)i) {
			printf("%ld byte corrupted\n", i);
			exit(EXIT_FAILURE);
		}
		if (i % pagesize == 0 &&
		    is_backed_by_thp(&pte_mapped[i], pagemap_fd, kpageflags_fd))
			thp_size++;
	}

	if (thp_size) {
		printf("Still %ld THPs not split\n", thp_size);
		exit(EXIT_FAILURE);
	}

	printf("Split PTE-mapped huge pages successful\n");
	munmap(one_page, len);
	close(pagemap_fd);
	close(kpageflags_fd);
}

void split_file_backed_thp(void)
{
	int status;
	int fd;
	ssize_t num_written;
	char tmpfs_template[] = "/tmp/thp_split_XXXXXX";
	const char *tmpfs_loc = mkdtemp(tmpfs_template);
	char testfile[INPUT_MAX];
	uint64_t pgoff_start = 0, pgoff_end = 1024;

	printf("Please enable pr_debug in split_huge_pages_in_file() if you need more info.\n");

	status = mount("tmpfs", tmpfs_loc, "tmpfs", 0, "huge=always,size=4m");

	if (status) {
		printf("Unable to create a tmpfs for testing\n");
		exit(EXIT_FAILURE);
	}

	status = snprintf(testfile, INPUT_MAX, "%s/thp_file", tmpfs_loc);
	if (status >= INPUT_MAX) {
		printf("Fail to create file-backed THP split testing file\n");
		goto cleanup;
	}

	fd = open(testfile, O_CREAT|O_WRONLY);
	if (fd == -1) {
		perror("Cannot open testing file\n");
		goto cleanup;
	}

	/* write something to the file, so a file-backed THP can be allocated */
	num_written = write(fd, tmpfs_loc, strlen(tmpfs_loc) + 1);
	close(fd);

	if (num_written < 1) {
		printf("Fail to write data to testing file\n");
		goto cleanup;
	}

	/* split the file-backed THP */
	write_debugfs(PATH_FMT, testfile, pgoff_start, pgoff_end);

	status = unlink(testfile);
	if (status)
		perror("Cannot remove testing file\n");

cleanup:
	status = umount(tmpfs_loc);
	if (status) {
		printf("Unable to umount %s\n", tmpfs_loc);
		exit(EXIT_FAILURE);
	}
	status = rmdir(tmpfs_loc);
	if (status) {
		perror("cannot remove tmp dir");
		exit(EXIT_FAILURE);
	}

	printf("file-backed THP split test done, please check dmesg for more information\n");
}

int main(int argc, char **argv)
{
	if (geteuid() != 0) {
		printf("Please run the benchmark as root\n");
		exit(EXIT_FAILURE);
	}

	pagesize = getpagesize();
	pageshift = ffs(pagesize) - 1;
	pmd_pagesize = read_pmd_pagesize();

	split_pmd_zero_pages();
	split_pmd_zero_pages_uffd();
	split_pmd_thp();
	split_pte_mapped_thp();
	split_file_backed_thp();

	return 0;
}
