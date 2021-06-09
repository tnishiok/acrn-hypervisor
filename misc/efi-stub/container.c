/*
 * Copyright (c) 2021, Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *    * Neither the name of Intel Corporation nor the names of its
 *      contributors may be used to endorse or promote products
 *      derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Library to support ACRN HV booting with Slim Bootloader container
 *
*/

#include <elf.h>
#include <efi.h>
#include <efilib.h>
#include "boot.h"
#include "stdlib.h"
#include "efilinux.h"
#include "multiboot.h"
#include "container.h"

#define LZH_BOOT_CMD	0u
#define LZH_BOOT_IMG	1u
#define LZH_MOD0_CMD	2u

typedef struct multiboot2_header_tag_relocatable RELOC_INFO;
typedef struct multiboot2_header_tag_address LADDR_INFO;

typedef struct {
  UINT32           Signature;
  UINT8            Version;
  UINT8            Svn;
  UINT16           DataOffset;
  UINT32           DataSize;
  UINT8            AuthType;
  UINT8            ImageType;
  UINT8            Flags;
  UINT8            Count;
} CONTAINER_HDR;

typedef struct {
  UINT32           Name;
  UINT32           Offset;
  UINT32           Size;
  UINT8            Attribute;
  UINT8            Alignment;
  UINT8            AuthType;
  UINT8            HashSize;
  UINT8            HashData[0];
} COMPONENT_ENTRY;

typedef struct {
  UINT32        Signature;
  UINT32        CompressedSize;
  UINT32        Size;
  UINT16        Version;
  UINT8         Svn;
  UINT8         Attribute;
  UINT8         Data[];
} LOADER_COMPRESSED_HEADER;

struct container {
	struct hv_loader ops;   /* loader operation table */

	UINT8 mb_version;       /* multiboot version of hv image. Can be either 1 or 2. */

	CHAR16 *options;        /* uefi boot option passed by efibootmgr -u */
	UINT32 options_size;    /* length of UEFI boot option */
	UINTN boot_cmdsize;     /* length of boot command to pass hypervisor */

	EFI_PHYSICAL_ADDRESS hv_hpa;    /* start of memory stored hv image */
	EFI_PHYSICAL_ADDRESS mod_hpa;	/* start of memory stored module files */
	EFI_PHYSICAL_ADDRESS hv_entry;	/* entry point offset of hv */
	RELOC_INFO *reloc;              /* relocation info */
	LADDR_INFO *laddr;              /* load address info */

	UINTN mod_count;        /* num of modules */
	UINTN total_modsize;    /* memory size allocated to load modules */
	UINTN total_modcmdsize; /* memory size to store module commands */

	UINTN lzh_count;        /* num of files in container */
	LOADER_COMPRESSED_HEADER *lzh_ptr[];	/* cache of each file header in container */
};

/**
 * @brief Load acrn.32.out ELF file
 *
 * @param[in]  elf_image ELF image
 * @param[out] hv_hpa    The physical memory address the relocated hypervisor is stored
 *
 * @return EFI_SUCCESS(0) on success, non-zero on error
 */
static EFI_STATUS load_acrn_elf(const UINT8 *elf_image, EFI_PHYSICAL_ADDRESS *hv_hpa,
	UINT32 hv_ram_start, UINT32 hv_ram_size, const RELOC_INFO *reloc)
{
	EFI_STATUS err = EFI_SUCCESS;
	int i;

	EFI_PHYSICAL_ADDRESS addr = 0u;

	Elf32_Ehdr *ehdr  = (Elf32_Ehdr *)(elf_image);
	Elf32_Phdr *pbase = (Elf32_Phdr *)(elf_image + ehdr->e_phoff);
	Elf32_Phdr *phdr  = (Elf32_Phdr *)(elf_image + ehdr->e_phoff);

	/* without relocation enabled, hypervisor binary need to reside in
	 * fixed memory address starting from CONFIG_HV_RAM_START, make a call
	 * to emalloc_fixed_addr for that case. With CONFIG_RELOC enabled,
	 * hypervisor is able to do relocation, the only requirement is that
	 * it need to reside in memory below 4GB, call emalloc_reserved_mem()
	 * instead.
	 *
	 * Don't relocate hypervisor binary under 256MB, which could be where
	 * guest Linux kernel boots from, and other usage, e.g. hvlog buffer
	*/
	if (reloc) {
		err = emalloc_reserved_aligned(hv_hpa, hv_ram_size, reloc->align,
			reloc->min_addr, reloc->max_addr);
	}
	else {
		err = emalloc_fixed_addr(hv_hpa, hv_ram_size, hv_ram_start);
	}

	if (err != EFI_SUCCESS) {
		Print(L"Failed to allocate memory for ACRN HV %r\n", err);
		goto out;
	}

	for (i = 0; i < ehdr->e_phnum; i++) {
		phdr = (Elf32_Phdr *)((UINT8 *)pbase + i * ehdr->e_phentsize);
		if ((phdr->p_type != PT_LOAD) || (phdr->p_memsz == 0) || (phdr->p_offset == 0)) {
			continue;
		}

		if (phdr->p_filesz > phdr->p_memsz) {
			err = EFI_LOAD_ERROR;
			goto out;
		}

		addr = (EFI_PHYSICAL_ADDRESS)(*hv_hpa + (phdr->p_paddr - hv_ram_start));
		memcpy((char *)addr, (const char *)(elf_image + phdr->p_offset), phdr->p_filesz);

		if (phdr->p_memsz > phdr->p_filesz) {
			addr = (EFI_PHYSICAL_ADDRESS)(*hv_hpa + (phdr->p_paddr - hv_ram_start + phdr->p_filesz));
			(void)memset((void *)addr, 0x0, (phdr->p_memsz - phdr->p_filesz));
		}
	}

out:
	return err;
}

static const struct multiboot_header *find_mb1header(const UINT8 *buffer, uint64_t len)
{
	const struct multiboot_header *header;

	for (header = (struct multiboot_header *)buffer;
		((char *)header <= (char *)buffer + len - 12);
		header = (struct multiboot_header *)((char *)header + MULTIBOOT_HEADER_ALIGN))
	{
		if (header->mh_magic == MULTIBOOT_HEADER_MAGIC &&
			!(header->mh_magic + header->mh_flags + header->mh_checksum))
			return header;
	}

	return NULL;
}

static int parse_mb1header(const struct multiboot_header *header, struct container *ctr)
{
	/* TODO: Currently we have nothing other than magic, flags and checksum in mb1 header, and
	 * flags contains only 1 flag: request memory map, and by default we pass these
	 * information to ACRN. So for now there is nothing we need to do here.
	 */
	return 0;
}

static const struct multiboot2_header *find_mb2header(const UINT8 *buffer, uint64_t len)
{
	const struct multiboot2_header *header;

	for (header = (const struct multiboot2_header *)buffer;
		((char *)header <= (char *)buffer + len - 12);
		header = (struct multiboot2_header *)((uint64_t)header + MULTIBOOT2_HEADER_ALIGN / 4))
	{
		if (header->magic == MULTIBOOT2_HEADER_MAGIC &&
			!(header->magic + header->architecture + header->header_length + header->checksum) &&
			header->architecture == MULTIBOOT2_ARCHITECTURE_I386)
			return header;
	}

	return NULL;
}

static int parse_mb2header(const struct multiboot2_header *header, struct container *ctr)
{
	struct multiboot2_header_tag *tag;

	for (tag = (struct multiboot2_header_tag *)(header + 1);
		tag->type != MULTIBOOT2_TAG_TYPE_END;
		tag = (struct multiboot2_header_tag *)((uint32_t *)tag + ALIGN_UP(tag->size, MULTIBOOT2_TAG_ALIGN) / 4))
	{
		switch (tag->type) {
			case MULTIBOOT2_HEADER_TAG_INFORMATION_REQUEST:
				/* Ignored. Currently we didn't support all categories of requested information,
				 * only the part that ACRN requests. So we don't parse the requests here. */
				break;

			case MULTIBOOT2_HEADER_TAG_ADDRESS:
				ctr->laddr = (LADDR_INFO *)tag;
				break;

			case MULTIBOOT2_HEADER_TAG_ENTRY_ADDRESS:
				ctr->hv_entry = ((struct multiboot2_header_tag_entry_address *)tag)->entry_addr;
				break;

			case MULTIBOOT2_HEADER_TAG_RELOCATABLE:
				ctr->reloc = (RELOC_INFO *)tag;
				break;

			default:
				Print(L"Unsupported multiboot2 tag type: %d\n", tag->type);
				return -1;
		}
	}

	if (ctr->laddr && ctr->hv_entry == 0x0)
		return -1;

	return 0;
}

/**
 * @brief Load hypervisor into memory from a container blob
 *
 * @param[in] hvld Loader handle
 *
 * @return EFI_SUCCESS(0) on success, non-zero on error
 */
static EFI_STATUS container_load_boot_image(HV_LOADER hvld)
{
	EFI_STATUS err = EFI_SUCCESS;
	struct container *ctr = (struct container *)hvld;

	LOADER_COMPRESSED_HEADER *lzh = NULL;
	const void *mb_hdr;

	/* hv_cmdline.txt: to be copied into memory by the fill_bootcmd_tag operation later */
	lzh = ctr->lzh_ptr[LZH_BOOT_CMD];
	ctr->boot_cmdsize = lzh->Size + StrnLen(ctr->options, ctr->options_size);

	/* Currently mb2 only */
	mb_hdr = find_mb2header((const UINT8 *)lzh->Data, MULTIBOOT2_SEARCH);
	if (mb_hdr) {
		if (parse_mb2header((struct multiboot2_header *)mb_hdr, ctr)) {
			Print(L"Illegal multiboot2 header, aborting\n");
			return EFI_INVALID_PARAMETER;
		}
		ctr->mb_version = 2;
	} else {
		/* try find mb1 header */
		mb_hdr = (struct multiboot_header *)find_mb1header((const UINT8 *)lzh->Data, MULTIBOOT_SEARCH);
		if (!mb_hdr) {
			Print(L"Image is not multiboot compatible\n");
			return EFI_INVALID_PARAMETER;
		}
		ctr->mb_version = 1;
		if (parse_mb1header((struct multiboot_header *)mb_hdr, ctr)) {
			Print(L"Illegal multiboot header, aborting\n");
			return EFI_INVALID_PARAMETER;
		}
	}

	/* acrn.32.out */
	/* TODO: Add support for loading flat binary (i.e., acrn.bin) */
	lzh = ctr->lzh_ptr[LZH_BOOT_IMG];
	err = load_acrn_elf((const UINT8 *)lzh->Data, &ctr->hv_hpa,
		ctr->laddr->load_addr,
		ctr->laddr->load_end_addr - ctr->laddr->load_addr,
		ctr->reloc);
	if (err != EFI_SUCCESS) {
		Print(L"Failed to load ACRN HV ELF Image%r\n", err);
		goto out;
	}

	/* Fix up entry address */
	if (ctr->reloc)
		ctr->hv_entry += ctr->hv_hpa - ctr->laddr->load_addr;
out:
	return err;
}

/**
 * @brief Load kernel modules and acpi tables into memory from a container blob
 *
 * @param[in] hvld Loader handle
 *
 * @return EFI_SUCCESS(0) on success, non-zero on error
 */
 static EFI_STATUS container_load_modules(HV_LOADER hvld)
{
	EFI_STATUS err = EFI_SUCCESS;
	struct container *ctr = (struct container *)hvld;

	UINTN i;

	UINT8 * p = NULL;
	LOADER_COMPRESSED_HEADER *lzh = NULL;

	/* scan module headers to calculate required memory size to store files */
	for (i = LZH_MOD0_CMD; i < ctr->lzh_count - 1; i++) {
		if ((i % 2) == 0) {	/* vm0_tag.txt, vm1_tag.txt, acpi_vm0.txt ... */
			ctr->total_modcmdsize += ctr->lzh_ptr[i]->Size;
		} else {	/* vm0_kernel, vm1_kernel, vm0_acpi.bin ... */
			ctr->total_modsize += ALIGN_UP(ctr->lzh_ptr[i]->Size, EFI_PAGE_SIZE);
		}
	}
	/* exclude hypervisor and SBL signature files. e.g.)
	 *    lzh_count = 9 (hv_cmdline, acrn.32.out, vm0_tag, vm0_kernel, vm1_tag, vm1_kernel, vm0_acpi_tag, vm0_acpi, sig)
	 *    mod_count = 3 (vm0_tag + vm0_kernel, vm1_tag + vm1_kernel, vm0_acpi_tag + vm0_acpi)
	 */
	ctr->mod_count = (ctr->lzh_count - 3) / 2;

	/* allocate single memory region to store all binary files to avoid mmap fragmentation */
	if (ctr->reloc) {
		err = emalloc_reserved_aligned(&(ctr->mod_hpa), ctr->total_modsize,
								EFI_PAGE_SIZE, ctr->reloc->min_addr, ctr->reloc->max_addr);
	} else {
		/* We put modules after hv */
		UINTN hv_ram_size = ctr->laddr->load_end_addr - ctr->laddr->load_addr;
		err = emalloc_fixed_addr(&(ctr->mod_hpa), hv_ram_size, ctr->hv_hpa + ALIGN_UP(hv_ram_size, EFI_PAGE_SIZE));
	}
	if (err != EFI_SUCCESS) {
		Print(L"Failed to allocate memory for modules %r\n", err);
		goto out;
	}

	p = (UINT8 *)ctr->mod_hpa;
	for (i = LZH_BOOT_IMG + 2; i < ctr->lzh_count - 1; i = i + 2) {
		lzh = ctr->lzh_ptr[i];
		memcpy((char *)p, (const char *)lzh->Data, lzh->Size);
		p += ALIGN_UP(lzh->Size, EFI_PAGE_SIZE);
	}
out:
	return err;
}

/**
 * @brief Get hypervisor boot command length
 *
 * @param[in] hvld Loader handle
 *
 * @return the length of hypervisor boot command
 */
static UINTN container_get_boot_cmdsize(HV_LOADER hvld)
{
	/* boot_cmd = hv_cmdline.txt in container + extra arg given by the 'efibootmgr -u' option */
	return ((struct container *)hvld)->boot_cmdsize;
}

/**
 * @brief Get the number of multiboot2 modules
 *
 * @param[in] hvld Loader handle
 *
 * @return the number of multiboot2 modules
 */
static UINTN container_get_mod_count(HV_LOADER hvld)
{
	return ((struct container *)hvld)->mod_count;
}

/**
 * @brief Get the total memory size allocated to load module files
 *
 * @param[in] hvld Loader handle
 *
 * @return the total size of memory allocated to store the module files
 */
static UINTN container_get_total_modsize(HV_LOADER hvld)
{
	return ((struct container *)hvld)->total_modsize;
}

/**
 * @brief Get the total lengths of the module commands
 *
 * @param[in] hvld Loader handle
 *
 * @return the total lengths of module command files
 */
static UINTN container_get_total_modcmdsize(HV_LOADER hvld)
{
	return ((struct container *)hvld)->total_modcmdsize;
}

/**
 * @brief Get the start address of the memory region stored ACRN hypervisor image
 *
 * @param[in] hvld Loader handle
 *
 * @return the address of hv image
 */
static EFI_PHYSICAL_ADDRESS container_get_hv_hpa(HV_LOADER hvld)
{
	return ((struct container *)hvld)->hv_hpa;
}

/**
 * @brief Get the start address of the memory region stored module files
 *
 * @param[in] hvld Loader handle
 *
 * @return the address of modules
 */
static EFI_PHYSICAL_ADDRESS container_get_mod_hpa(HV_LOADER hvld)
{
	return ((struct container *)hvld)->mod_hpa;
}

/**
 * @brief Get the supported multiboot version of ACRN hypervisor image
 *
 * @param[in] hvld Loader handle
 *
 * @return supported multiboot version. Can be either 1 or 2.
 */
static int container_get_multiboot_version(HV_LOADER hvld)
{
	return ((struct container *)hvld)->mb_version;
}

/**
 * @brief Get the entry point of ACRN hypervisor
 *
 * @param[in] hvld Loader handle
 *
 * @return the entry point of hypervisor
 */
static EFI_PHYSICAL_ADDRESS container_get_hv_entry(HV_LOADER hvld)
{
	return ((struct container *)hvld)->hv_entry;
}

/**
 * @brief Get the total memory size of hv image
 *
 * @param[in] hvld Loader handle
 *
 * @return the memory size of hv image
 */
static UINTN container_get_hv_ram_size(HV_LOADER hvld)
{
	struct container *ctr = (struct container *)hvld;
	if (ctr->laddr) {
		return ctr->laddr->load_end_addr - ctr->laddr->load_addr;
	}

	/* TODO: calculate size from ACRN ELF header */
	return 0;
}

/**
 * @brief Set hypervisor boot command line to multiboot2 tag
 *
 * @param[in]  hvld Loader handle
 * @param[out] tag  The buffer to be filled in. It's the caller's responsibility to allocate memory for the buffer
 *
 * @return None
 */
static void container_fill_bootcmd_tag(HV_LOADER hvld, struct multiboot2_tag_string *tag)
{
	struct container *ctr = (struct container *)hvld;
	LOADER_COMPRESSED_HEADER *lzh = ctr->lzh_ptr[LZH_BOOT_CMD];
	UINTN cmdline_size = container_get_boot_cmdsize(hvld);

	UINTN i;

	tag->type = MULTIBOOT2_TAG_TYPE_CMDLINE;
	tag->size = sizeof(struct multiboot2_tag_string) + cmdline_size;

	(void)memset((void *)tag->string, 0x0, cmdline_size);
	memcpy(tag->string, (const char *)lzh->Data, lzh->Size - 1);
	if (ctr->options) {
		tag->string[lzh->Size - 1] = ' ';
		for (i = lzh->Size; i < cmdline_size; i++) {
			/* append the options to the boot command line */
			tag->string[i] = ctr->options[i - lzh->Size];
		}
	}
}

/**
 * @brief Set n-th module info to multiboot2 tag
 *
 * @param[in]  hvld Loader handle
 * @param[out] tag  The buffer to be filled in. It's the caller's responsibility to allocate memory for the buffer
 *
 * @return None
 */
static void container_fill_module_tag(HV_LOADER hvld, struct multiboot2_tag_module *tag, UINTN index)
{
	struct container *ctr = (struct container *)hvld;

	LOADER_COMPRESSED_HEADER *cmd_lzh = NULL;
	LOADER_COMPRESSED_HEADER *mod_lzh = NULL;
	UINT8 * p = (UINT8 *)ctr->mod_hpa;

	UINTN i;

	for (i = LZH_MOD0_CMD; i < ctr->lzh_count - 1; i = i + 2) {
		mod_lzh = ctr->lzh_ptr[i + 1];
		if (i == (index * 2 + LZH_MOD0_CMD)) {
			cmd_lzh = ctr->lzh_ptr[i];
			tag->type = MULTIBOOT2_TAG_TYPE_MODULE;
			tag->size = sizeof(struct multiboot2_tag_module) + cmd_lzh->Size;
			tag->mod_start = (EFI_PHYSICAL_ADDRESS)p;
			tag->mod_end = tag->mod_start + mod_lzh->Size;
			memcpy(tag->cmdline, (char *)(uint64_t)cmd_lzh->Data, cmd_lzh->Size);
			break;
		}
		p += ALIGN_UP(mod_lzh->Size, EFI_PAGE_SIZE);
	}
}

/**
 * @brief Free up memory allocated by the container loader
 *
 * @param[in]  hvld Loader handle
 *
 * @return None
 */
static void container_deinit(HV_LOADER hvld)
{
	struct container *ctr = (struct container *)hvld;

	if (ctr->lzh_ptr) {
		free_pool(ctr->lzh_ptr);
		free_pool(ctr);
	}

	if (ctr->mod_hpa) {
		free_pages(ctr->mod_hpa, EFI_SIZE_TO_PAGES(ctr->total_modsize));
	}
}

/* hypervisor loader operation table */
static struct hv_loader container_ops = {
	.load_boot_image = container_load_boot_image,
	.load_modules = container_load_modules,
	.get_boot_cmdsize = container_get_boot_cmdsize,
	.get_total_modsize = container_get_total_modsize,
	.get_total_modcmdsize = container_get_total_modcmdsize,
	.get_mod_count = container_get_mod_count,
	.get_hv_hpa = container_get_hv_hpa,
	.get_mod_hpa = container_get_mod_hpa,
	.get_hv_entry = container_get_hv_entry,
	.get_multiboot_version = container_get_multiboot_version,
	.get_hv_ram_size = container_get_hv_ram_size,
	.fill_bootcmd_tag = container_fill_bootcmd_tag,
	.fill_module_tag = container_fill_module_tag,
	.deinit = container_deinit,
};

/**
 * @brief Initialize Container Library and returned the loader operation table
 *
 * @param[in]  info Firmware-allocated handle that identifies the EFI application image (i.e. acrn.efi)
 * @param[out] info Allocated loader operation table
 *
 * @return EFI_SUCCESS(0) on success, non-zero on error
 */
EFI_STATUS container_init(EFI_LOADED_IMAGE *info, HV_LOADER *hvld)
{
	EFI_STATUS err = EFI_SUCCESS;

	struct container *ctr = NULL;

	UINTN sec_addr = 0u;
	UINTN sec_size = 0u;
	char *section  = ".hv";

	UINTN i;
	CONTAINER_HDR   *hdr  = NULL;
	COMPONENT_ENTRY *comp = NULL;

	UINTN offset = 0u;

	err = allocate_pool(EfiLoaderData, sizeof(struct container), (void **)&ctr);
	if (EFI_ERROR(err)) {
		Print(L"Failed to allocate memory for Container Library %r\n", err);
		goto out;
	}

	(void)memset((void *)ctr, 0x0, sizeof(struct container));
	memcpy((char *)&ctr->ops, (const char *)&container_ops, sizeof(struct hv_loader));

	/* store the options */
	ctr->options = info->LoadOptions;
	ctr->options_size = info->LoadOptionsSize;

	/* read a container stitched at the .hv section */
	err = get_pe_section(info->ImageBase, section, strlen(section), &sec_addr, &sec_size);
	if (EFI_ERROR(err)) {
		Print(L"Unable to locate section of ACRNHV Container %r ", err);
		goto out;
	}

	hdr = (CONTAINER_HDR*)(info->ImageBase + sec_addr);
	ctr->lzh_count = hdr->Count;

	err = allocate_pool(EfiLoaderData, sizeof(LOADER_COMPRESSED_HEADER *) * hdr->Count, (void **)&ctr->lzh_ptr);
	if (EFI_ERROR(err)) {
		Print(L"Failed to allocate memory for Container Library %r\n", err);
		goto out;
	}

	/* cache each file's header point for later use */
	comp = (COMPONENT_ENTRY *)(hdr + 1);
	for (i = 0; i < hdr->Count; i++) {
		offset = hdr->DataOffset + comp->Offset;
		ctr->lzh_ptr[i] = (LOADER_COMPRESSED_HEADER *)((UINT8 *)(hdr) + offset);

		comp = (COMPONENT_ENTRY *)((UINT8 *)(comp + 1) + comp->HashSize);
	}

	*hvld = (struct hv_loader *)ctr;
out:
	if (EFI_ERROR(err)) {
		if (ctr) {
			container_deinit((HV_LOADER)ctr);
		}
	}
	return err;
}
