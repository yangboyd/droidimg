#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <linux/limits.h>

#define KERNEL_TEXT			(0xffffffc000080000UL)
#define KERN_VA(p)			(void *)((unsigned long)(p) - (unsigned long)kern_buf + KERNEL_TEXT)
#define LOCAL_VA(p)			(void *)((unsigned long)(p) - KERNEL_TEXT + (unsigned long)kern_buf)

#define IN_RANGE(p, b, l)	(((uint8_t *)(p) >= (uint8_t *)(b)) && ((uint8_t *)(p) < ((uint8_t *)(b) + (ssize_t)(l))))

uint8_t 	*kern_buf;
size_t		kern_size;
size_t		kern_mmap_size;

char		infile[PATH_MAX];
char		outfile[PATH_MAX];

struct rela_entry_t {
	uint64_t	offset;
	uint64_t	info;
	uint64_t	sym;
};

struct Elf64_Sym {
	uint32_t	st_name;
	uint8_t		st_info;
	uint8_t		st_other;
	uint16_t	st_shndx;
	uint64_t	st_value;
	uint64_t	st_size;
};

struct rela_entry_t		*rela_start;
struct rela_entry_t		*rela_end;

static inline int32_t
extract_signed_bitfield (uint32_t insn, unsigned width, unsigned offset)
{
	unsigned shift_l = sizeof (int32_t) * 8 - (offset + width);
	unsigned shift_r = sizeof (int32_t) * 8 - width;

	return ((int32_t) insn << shift_l) >> shift_r;
} __attribute__((always_inline))

static inline int
parse_insn_adrp(uint32_t insn, ssize_t *offset)
{
	uint32_t immlo = (insn >> 29) & 0x3;
	int32_t immhi = extract_signed_bitfield(insn, 19, 5) << 2;

	*offset = (immhi | immlo) * 4096;

	return 0;
} __attribute__((always_inline))

static inline int
parse_insn_add(uint32_t insn, uint32_t *inc)
{
	*inc = (insn >> 10) & 0xfff;

	return 0;
} __attribute__((always_inline))


static inline int alloc_kern_buf()
{
	struct stat st;
	size_t malloc_size;

	int fd;

	if (stat(infile, &st) == -1) {
		perror("stat failed");
		return -1;
	}

	kern_size = (size_t)st.st_size;
	kern_mmap_size = (kern_size + 0xfff) & (~0xfff);

	kern_buf = (uint8_t *)mmap(NULL, kern_mmap_size, 
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (kern_buf == (void *)-1) {
		perror("mmap failed");
		return -1;
	}
	printf("kern_buf @ %p, mmap_size = %lu\n", kern_buf, kern_mmap_size);

	fd = open(infile, O_RDONLY);
	if (fd == -1) {
		perror("open failed");
		return -1;
	}

	read(fd, kern_buf, kern_size);

	close(fd);
	return 0;
} __attribute__((always_inline))

/* Calculates critical locations
 * - rela_entry
 * - rela_end;
 */
static inline int parse_rela_sect()
{
	#define PATTERN_RELOCATE_KERNEL		(0xd1420277)

	int			found;
	uint32_t	*p_insn;
	uint32_t	insn;
	ssize_t		offset;

	p_insn = (uint32_t *)kern_buf;
	found = 0;

	while (IN_RANGE(p_insn, kern_buf, kern_size - sizeof(*p_insn) * 4)) {
		if (*p_insn == PATTERN_RELOCATE_KERNEL) {
			found = 1;
			break;
		}
		p_insn++;
	}
	printf("__relocate_kernel @ %p(%p)\n", p_insn, KERN_VA(p_insn));

	if (!found) {
		printf("Can't find __relocate_kernel!\n");
		return -1;
	}

	p_insn++; insn = *p_insn;
	printf("insn = %08x\n", insn);
	parse_insn_adrp(insn, &offset);
	rela_end = (struct rela_entry_t *)(((size_t)p_insn & (~0xfffUL)) + offset);
	printf("0x%x %p(%p)\n", (int32_t)offset, rela_end, KERN_VA(rela_end));

	p_insn++; insn = *p_insn;
	printf("insn = %08x\n", insn);
	parse_insn_add(insn, (uint32_t *)&offset);
	rela_end = (struct rela_entry_t *)((size_t)rela_end +(uint32_t)offset);
	printf("0x%x %p(%p)\n", (int32_t)offset, rela_end, KERN_VA(rela_end));

	p_insn++; insn = *p_insn;
	printf("insn = %08x\n", insn);
	parse_insn_adrp(insn, &offset);
	rela_start = (struct rela_entry_t *)(((size_t)p_insn & (~0xfffUL)) + offset);	
	printf("0x%x %p(%p)\n", (int32_t)offset, rela_start, KERN_VA(rela_start));

	p_insn++; insn = *p_insn;
	printf("insn = %08x\n", insn);
	parse_insn_add(insn, (uint32_t *)&offset);
	rela_start = (struct rela_entry_t *)((size_t)rela_start +(uint32_t)offset);
	printf("0x%x %p(%p)\n", (int32_t)offset, rela_start, KERN_VA(rela_start));

	if (!IN_RANGE(rela_start, kern_buf, kern_size)) {
		printf("rela_start out of range!\n");
		return -1;
	}

	if (!IN_RANGE(rela_end, kern_buf, kern_size)) {
		printf("rela_end out of range!\n");
		return -1;
	}

	if (rela_end <= rela_start) {
		printf("rela_start/end invalid!\n");
		return -1;
	}

	return 0;
} __attribute__((always_inline))

static inline int relocate_kernel()
{
	#define KERNEL_SLIDE		(0)
	#define R_AARCH64_RELATIVE	(0x403)
	#define R_AARCH64_ABS		(0x101)

	struct rela_entry_t *rela_entry = rela_start;
	int64_t		sym_offset;
	uint64_t	sym_info;
	size_t		sym_addr;

	int count = 0;

	while (rela_entry < rela_end)
	{
		sym_offset = rela_entry->offset;
		sym_info = rela_entry->info;
		sym_addr = rela_entry->sym;

		size_t *p = (size_t *)(sym_offset + KERNEL_SLIDE);

		if (sym_info == R_AARCH64_RELATIVE) {
			size_t new_addr = sym_addr + KERNEL_SLIDE;
			*(size_t *)LOCAL_VA(p) = new_addr;			
		}
		else if ((uint32_t)sym_info == R_AARCH64_ABS) {
			struct Elf64_Sym *elf64_sym;

			elf64_sym = (struct Elf64_Sym *)((size_t)rela_end + 24 * ((sym_info >> 32) & 0xffffffff));
			if (elf64_sym->st_shndx) {
				size_t real_stext = elf64_sym->st_value;
				if ((int64_t)elf64_sym->st_shndx != -15) {
					real_stext += KERNEL_SLIDE;
				}
				*(size_t *)LOCAL_VA(p) = real_stext + sym_addr;
			}			
		}

		rela_entry++;
		count++;
	}

	printf("%d entries processed\n", count);

	return 0;
} __attribute__((always_inline))

static inline int write_outfile()
{
	int fd;

	fd = open(outfile, O_CREAT | O_RDWR);
	if (fd == -1) {
		perror("outfile");
		return -1;
	}

	write(fd, kern_buf, kern_size);
	close(fd);

	return 0;
} __attribute__((always_inline))

int main(int argc, char **argv)
{
	if (argc != 3) {
		printf("Usage: fix_kaslr <infile> <outfile>\n");
		return -1;
	}

	strncpy(infile, argv[1], PATH_MAX);
	strncpy(outfile, argv[2], PATH_MAX);

	printf("Original kernel: %s, output file: %s\n", infile, outfile);

	if (alloc_kern_buf()) {
		return -1;
	}

	if (parse_rela_sect()) {
		return -1;
	}

	if (relocate_kernel()) {
		return -1;
	}

	if (write_outfile()) {
		return -1;
	}

	munmap(kern_buf, kern_mmap_size);

	return 0;
}
