/*
 * ld - a small static linker
 *
 * Copyright (C) 2010 Ali Gholami Rudi
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, as published by the
 * Free Software Foundation.
 */
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#define SRCADDR		0x4000000ul
#define DATADDR		0x6000000ul
#define BSSADDR		0x8000000ul
#define MAXSECS		(1 << 10)
#define MAXOBJS		(1 << 7)
#define MAXSYMS		(1 << 12)
#define PAGE_SIZE	(1 << 12)
#define GOT_PAD		16
#define MAXFILES	(1 << 10)
#define MAXPHDRS	4

#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define ALIGN(x, a)	(((x) + (long) (a) - 1) & ~((long) (a) - 1))

struct obj {
	char *mem;
	Elf32_Ehdr *ehdr;
	Elf32_Shdr *shdr;
	Elf32_Sym *syms;
	int nsyms;
	char *symstr;
	char *shstr;
};

struct secmap {
	Elf32_Shdr *o_shdr;
	struct obj *obj;
	unsigned long vaddr;
	unsigned long faddr;
};

struct bss_sym {
	Elf32_Sym *sym;
	int off;
};

struct got_sym {
	Elf32_Sym *sym;
	struct obj *obj;
};

struct outelf {
	Elf32_Ehdr ehdr;
	Elf32_Phdr phdr[MAXSECS];
	int nph;
	struct secmap secs[MAXSECS];
	int nsecs;
	struct obj objs[MAXOBJS];
	int nobjs;

	/* code section */
	unsigned long code_addr;

	/* bss section */
	struct bss_sym bss_syms[MAXSYMS];
	int nbss_syms;
	unsigned long bss_vaddr;
	int bss_len;

	/* got/plt section */
	struct got_sym got_syms[MAXSYMS];
	int ngot_syms;
	unsigned long got_vaddr;
	unsigned long got_faddr;
};

static Elf32_Sym *obj_find(struct obj *obj, char *name)
{
	int i;
	for (i = 0; i < obj->nsyms; i++) {
		Elf32_Sym *sym = &obj->syms[i];
		if (ELF32_ST_BIND(sym->st_info) == STB_LOCAL ||
				sym->st_shndx == SHN_UNDEF)
			continue;
		if (!strcmp(name, obj->symstr + sym->st_name))
			return sym;
	}
	return NULL;
}

static void obj_init(struct obj *obj, char *mem)
{
	int i;
	obj->mem = mem;
	obj->ehdr = (void *) mem;
	obj->shdr = (void *) (mem + obj->ehdr->e_shoff);
	obj->shstr = mem + obj->shdr[obj->ehdr->e_shstrndx].sh_offset;
	for (i = 0; i < obj->ehdr->e_shnum; i++) {
		if (obj->shdr[i].sh_type != SHT_SYMTAB)
			continue;
		obj->symstr = mem + obj->shdr[obj->shdr[i].sh_link].sh_offset;
		obj->syms = (void *) (mem + obj->shdr[i].sh_offset);
		obj->nsyms = obj->shdr[i].sh_size / sizeof(*obj->syms);
	}
}

static void outelf_init(struct outelf *oe)
{
	memset(oe, 0, sizeof(*oe));
	oe->ehdr.e_ident[0] = 0x7f;
	oe->ehdr.e_ident[1] = 'E';
	oe->ehdr.e_ident[2] = 'L';
	oe->ehdr.e_ident[3] = 'F';
	oe->ehdr.e_ident[4] = ELFCLASS32;
	oe->ehdr.e_ident[5] = ELFDATA2LSB;
	oe->ehdr.e_ident[6] = EV_CURRENT;
	oe->ehdr.e_type = ET_EXEC;
	oe->ehdr.e_machine = EM_386;
	oe->ehdr.e_version = EV_CURRENT;
	oe->ehdr.e_shstrndx = SHN_UNDEF;
	oe->ehdr.e_ehsize = sizeof(oe->ehdr);
	oe->ehdr.e_phentsize = sizeof(oe->phdr[0]);
	oe->ehdr.e_shentsize = sizeof(Elf32_Shdr);
}

static struct secmap *outelf_mapping(struct outelf *oe, Elf32_Shdr *shdr)
{
	int i;
	for (i = 0; i < oe->nsecs; i++)
		if (oe->secs[i].o_shdr == shdr)
			return &oe->secs[i];
	return NULL;
}

static int outelf_find(struct outelf *oe, char *name,
			struct obj **sym_obj, Elf32_Sym **sym_sym)
{
	int i;
	for (i = 0; i < oe->nobjs; i++) {
		struct obj *obj = &oe->objs[i];
		Elf32_Sym *sym;
		if ((sym = obj_find(obj, name))) {
			*sym_obj = obj;
			*sym_sym = sym;
			return 0;
		}
	}
	return 1;
}

static unsigned long bss_addr(struct outelf *oe, Elf32_Sym *sym)
{
	int i;
	for (i = 0; i < oe->nbss_syms; i++)
		if (oe->bss_syms[i].sym == sym)
			return oe->bss_vaddr + oe->bss_syms[i].off;
	return 0;
}

static void die(char *msg)
{
	write(1, msg, strlen(msg));
	exit(1);
}

static void warn_undef(char *name)
{
	char msg[128];
	strcpy(msg, name);
	strcpy(msg + strlen(msg), " undefined\n");
	die(msg);
}

static unsigned long symval(struct outelf *oe, struct obj *obj, Elf32_Sym *sym)
{
	struct secmap *sec;
	char *name = obj ? obj->symstr + sym->st_name : NULL;
	int s_idx, s_off;
	switch (ELF32_ST_TYPE(sym->st_info)) {
	case STT_SECTION:
		if ((sec = outelf_mapping(oe, &obj->shdr[sym->st_shndx])))
			return sec->vaddr;
		break;
	case STT_NOTYPE:
	case STT_OBJECT:
	case STT_FUNC:
		if (name && *name && sym->st_shndx == SHN_UNDEF)
			if (outelf_find(oe, name, &obj, &sym))
				warn_undef(name);
		if (sym->st_shndx == SHN_COMMON)
			return bss_addr(oe, sym);
		s_idx = sym->st_shndx;
		s_off = sym->st_value;
		sec = outelf_mapping(oe, &obj->shdr[s_idx]);
		if ((sec = outelf_mapping(oe, &obj->shdr[s_idx])))
			return sec->vaddr + s_off;
	}
	return 0;
}

static unsigned long outelf_addr(struct outelf *oe, char *name)
{
	struct obj *obj;
	Elf32_Sym *sym;
	if (outelf_find(oe, name, &obj, &sym))
		die("unknown symbol!\n");
	return symval(oe, obj, sym);
}

static void outelf_reloc_sec(struct outelf *oe, int o_idx, int s_idx)
{
	struct obj *obj = &oe->objs[o_idx];
	Elf32_Shdr *rel_shdr = &obj->shdr[s_idx];
	Elf32_Rel *rels = (void *) obj->mem + obj->shdr[s_idx].sh_offset;
	Elf32_Shdr *other_shdr = &obj->shdr[rel_shdr->sh_info];
	void *other = (void *) obj->mem + other_shdr->sh_offset;
	int nrels = rel_shdr->sh_size / sizeof(*rels);
	unsigned long addr;
	int i;
	for (i = 0; i < nrels; i++) {
		Elf32_Rela *rel = &rels[i];
		int sym_idx = ELF32_R_SYM(rel->r_info);
		Elf32_Sym *sym = &obj->syms[sym_idx];
		unsigned long val = symval(oe, obj, sym);
		unsigned long *dst = other + rel->r_offset;
		switch (ELF32_R_TYPE(rel->r_info)) {
		case R_386_NONE:
			break;
		case R_386_16:
			*(unsigned short *) dst += val;
			break;
		case R_386_32:
			*dst += val;
			break;
		case R_386_PC32:
		case R_386_PLT32:
			addr = outelf_mapping(oe, other_shdr)->vaddr +
				rel->r_offset;
			*dst += val - addr;
			break;
		default:
			die("unknown relocation type\n");
		}
	}
}

static void outelf_reloc(struct outelf *oe)
{
	int i, j;
	for (i = 0; i < oe->nobjs; i++) {
		struct obj *obj = &oe->objs[i];
		for (j = 0; j < obj->ehdr->e_shnum; j++)
			if (obj->shdr[j].sh_type == SHT_REL)
				outelf_reloc_sec(oe, i, j);
	}
}

static void alloc_bss(struct outelf *oe, Elf32_Sym *sym)
{
	int n = oe->nbss_syms++;
	int off = ALIGN(oe->bss_len, MAX(sym->st_value, 4));
	oe->bss_syms[n].sym = sym;
	oe->bss_syms[n].off = off + sym->st_size;
	oe->bss_len += off + sym->st_size;
}

static void outelf_bss(struct outelf *oe)
{
	int i, j;
	for (i = 0; i < oe->nobjs; i++) {
		struct obj *obj = &oe->objs[i];
		for (j = 0; j < obj->nsyms; j++)
			if (obj->syms[j].st_shndx == SHN_COMMON)
				alloc_bss(oe, &obj->syms[j]);
	}
}

static int outelf_putgot(struct outelf *oe, char *buf)
{
	unsigned long *got = (void *) buf;
	int len = 4 * oe->ngot_syms;
	int i;
	for (i = 0; i < oe->ngot_syms; i++)
		got[i] = symval(oe, oe->got_syms[i].obj,
				oe->got_syms[i].sym);
	memset(buf + len, 0, GOT_PAD);
	return len + GOT_PAD;
}

#define SEC_CODE(s)	((s)->sh_flags & SHF_EXECINSTR)
#define SEC_BSS(s)	((s)->sh_type == SHT_NOBITS)
#define SEC_DATA(s)	(!SEC_CODE(s) && !SEC_BSS(s))

static void outelf_write(struct outelf *oe, int fd)
{
	int i;
	char buf[1 << 14];
	int got_len;
	oe->ehdr.e_entry = outelf_addr(oe, "_start");
	got_len = outelf_putgot(oe, buf);

	oe->ehdr.e_phnum = oe->nph;
	oe->ehdr.e_phoff = sizeof(oe->ehdr);
	lseek(fd, 0, SEEK_SET);
	write(fd, &oe->ehdr, sizeof(oe->ehdr));
	write(fd, &oe->phdr, oe->nph * sizeof(oe->phdr[0]));
	for (i = 0; i < oe->nsecs; i++) {
		struct secmap *sec = &oe->secs[i];
		char *buf = sec->obj->mem + sec->o_shdr->sh_offset;
		int len = sec->o_shdr->sh_size;
		if (SEC_BSS(sec->o_shdr))
			continue;
		lseek(fd, sec->faddr, SEEK_SET);
		write(fd, buf, len);
	}
	lseek(fd, oe->got_faddr, SEEK_SET);
	write(fd, buf, got_len);
}

static void outelf_add(struct outelf *oe, char *mem)
{
	Elf32_Ehdr *ehdr = (void *) mem;
	Elf32_Shdr *shdr = (void *) (mem + ehdr->e_shoff);
	struct obj *obj;
	int i;
	if (ehdr->e_type != ET_REL)
		return;
	if (oe->nobjs >= MAXOBJS)
		die("ld: MAXOBJS reached!\n");
	obj = &oe->objs[oe->nobjs++];
	obj_init(obj, mem);
	for (i = 0; i < ehdr->e_shnum; i++) {
		struct secmap *sec;
		if (!(shdr[i].sh_flags & 0x7))
			continue;
		if (oe->nsecs >= MAXSECS)
			die("ld: MAXSECS reached\n");
		sec = &oe->secs[oe->nsecs++];
		sec->o_shdr = &shdr[i];
		sec->obj = obj;
	}
}

static void outelf_link(struct outelf *oe)
{
	int i;
	Elf32_Phdr *code_phdr = &oe->phdr[oe->nph++];
	Elf32_Phdr *bss_phdr = &oe->phdr[oe->nph++];
	Elf32_Phdr *data_phdr = &oe->phdr[oe->nph++];
	unsigned long faddr = sizeof(oe->ehdr) + MAXPHDRS * sizeof(oe->phdr[0]);
	unsigned long vaddr = SRCADDR + faddr % PAGE_SIZE;
	int len = 0;
	for (i = 0; i < oe->nsecs; i++) {
		struct secmap *sec = &oe->secs[i];
		int alignment = MAX(sec->o_shdr->sh_addralign, 4);
		if (!SEC_CODE(sec->o_shdr))
			continue;
		len = ALIGN(vaddr + len, alignment) - vaddr;
		sec->vaddr = vaddr + len;
		sec->faddr = faddr + len;
		len += sec->o_shdr->sh_size;
	}
	code_phdr->p_type = PT_LOAD;
	code_phdr->p_flags = PF_R | PF_W | PF_X;
	code_phdr->p_vaddr = vaddr;
	code_phdr->p_paddr = vaddr;
	code_phdr->p_offset = faddr;
	code_phdr->p_filesz = len;
	code_phdr->p_memsz = len;
	code_phdr->p_align = PAGE_SIZE;

	faddr += len;
	vaddr = BSSADDR + faddr % PAGE_SIZE;
	len = 0;
	outelf_bss(oe);
	oe->bss_vaddr = vaddr + len;
	len += oe->bss_len;
	for (i = 0; i < oe->nsecs; i++) {
		struct secmap *sec = &oe->secs[i];
		int alignment = MAX(sec->o_shdr->sh_addralign, 4);
		if (!SEC_BSS(sec->o_shdr))
			continue;
		len = ALIGN(vaddr + len, alignment) - vaddr;
		sec->vaddr = vaddr + len;
		sec->faddr = faddr;
		len += sec->o_shdr->sh_size;
	}
	bss_phdr->p_type = PT_LOAD;
	bss_phdr->p_flags = PF_R | PF_W;
	bss_phdr->p_vaddr = vaddr;
	bss_phdr->p_paddr = vaddr;
	bss_phdr->p_offset = faddr;
	bss_phdr->p_filesz = 0;
	bss_phdr->p_memsz = len;
	bss_phdr->p_align = PAGE_SIZE;

	faddr = ALIGN(faddr, 4);
	vaddr = DATADDR + faddr % PAGE_SIZE;
	len = 0;
	for (i = 0; i < oe->nsecs; i++) {
		struct secmap *sec = &oe->secs[i];
		if (!SEC_DATA(sec->o_shdr))
			continue;
		sec->vaddr = vaddr + len;
		sec->faddr = faddr + len;
		len += sec->o_shdr->sh_size;
	}
	len = ALIGN(len, 4);
	oe->got_faddr = faddr + len;
	oe->got_vaddr = vaddr + len;
	outelf_reloc(oe);
	len += oe->ngot_syms * 4 + GOT_PAD;

	data_phdr->p_type = PT_LOAD;
	data_phdr->p_flags = PF_R | PF_W | PF_X;
	data_phdr->p_align = PAGE_SIZE;
	data_phdr->p_vaddr = vaddr;
	data_phdr->p_paddr = vaddr;
	data_phdr->p_filesz = len;
	data_phdr->p_memsz = len;
	data_phdr->p_offset = faddr;
}

struct arhdr {
	char ar_name[16];
	char ar_date[12];
	char ar_uid[6];
	char ar_gid[6];
	char ar_mode[8];
	char ar_size[10];
	char ar_fmag[2];
};

static int get_be32(unsigned char *s)
{
	return s[3] | (s[2] << 8) | (s[1] << 16) | (s[0] << 32);
}

static int sym_undef(struct outelf *oe, char *name)
{
	int i, j;
	int undef = 0;
	for (i = 0; i < oe->nobjs; i++) {
		struct obj *obj = &oe->objs[i];
		for (j = 0; j < obj->nsyms; j++) {
			Elf32_Sym *sym = &obj->syms[j];
			if (ELF32_ST_BIND(sym->st_info) == STB_LOCAL)
				continue;
			if (strcmp(name, obj->symstr + sym->st_name))
				continue;
			if (sym->st_shndx != SHN_UNDEF)
				return 0;
			undef = 1;
		}
	}
	return undef;
}

static int outelf_ar_link(struct outelf *oe, char *ar, int base)
{
	char *ar_index;
	char *ar_name;
	int nsyms = get_be32((void *) ar);
	int added = 0;
	int i;
	ar_index = ar + 4;
	ar_name = ar_index + nsyms * 4;
	for (i = 0; i < nsyms; i++) {
		int off = get_be32((void *) ar_index + i * 4) +
				sizeof(struct arhdr);
		if (sym_undef(oe, ar_name)) {
			outelf_add(oe, ar - base + off);
			added++;
		}
		ar_name = strchr(ar_name, '\0') + 1;
	}
	return added;
}

static void link_archive(struct outelf *oe, char *ar)
{
	char *beg = ar;

	/* skip magic */
	ar += 8;
	for(;;) {
		struct arhdr *hdr = (void *) ar;
		int size;
		ar += sizeof(*hdr);
		hdr->ar_size[sizeof(hdr->ar_size) - 1] = '\0';
		size = atoi(hdr->ar_size);
		size = (size + 1) & ~1;
		if (!strncmp(hdr->ar_name, "/ ", 2)) {
			while (outelf_ar_link(oe, ar, ar - beg))
				;
			return;
		}
		if (!strncmp(hdr->ar_name, "// ", 3))
			outelf_add(oe, ar);
		ar += size;
	}
}

static long filesize(int fd)
{
	struct stat stat;
	fstat(fd, &stat);
	return stat.st_size;
}

static char *fileread(char *path)
{
	int fd = open(path, O_RDONLY);
	int size = filesize(fd);
	char *buf = malloc(size);
	read(fd, buf, size);
	close(fd);
	return buf;
}

static int is_ar(char *path)
{
	int len = strlen(path);
	return len > 2 && path[len - 2] == '.' && path[len - 1] == 'a';
}

int main(int argc, char **argv)
{
	char out[1 << 10] = "a.out";
	char *buf;
	struct outelf oe;
	char *mem[MAXFILES];
	int nmem = 0;
	int fd;
	int i = 0;
	if (argc < 2)
		die("no object given\n");
	outelf_init(&oe);

	while (++i < argc) {
		if (!strcmp("-o", argv[i])) {
			strcpy(out, argv[++i]);
			continue;
		}
		if (!strcmp("-g", argv[i]))
			continue;
		buf = fileread(argv[i]);
		mem[nmem++] = buf;
		if (!buf)
			die("cannot open object\n");
		if (is_ar(argv[i]))
			link_archive(&oe, buf);
		else
			outelf_add(&oe, buf);
	}
	outelf_link(&oe);
	fd = open(out, O_WRONLY | O_TRUNC | O_CREAT, 0700);
	outelf_write(&oe, fd);
	close(fd);
	for (i = 0; i < nmem; i++)
		free(mem[i]);
	return 0;
}
