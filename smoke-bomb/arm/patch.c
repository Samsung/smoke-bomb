#include <linux/capability.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/security.h>
#include <linux/kallsyms.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/version.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/highmem.h>

#include <asm/uaccess.h>
#include <asm/cacheflush.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>

#include "patch.h"
#include "cache.h"
#include "insn.h"
#include "../header.h"

#include <linux/pagemap.h>  /* read_mapping_page */
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/rmap.h>     /* anon_vma_prepare */
#include <linux/mmu_notifier.h> /* set_pte_at_notify */
#include <linux/swap.h>     /* try_to_free_swap */
#include <linux/ptrace.h>   /* user_enable_single_step */
#include <linux/kdebug.h>   /* notifier mechanism */
#include <linux/percpu-rwsem.h>
#include <linux/task_work.h>
#include <linux/shmem_fs.h>
#include <linux/uprobes.h>

/* unresolved function pointers */
static int (*anon_vma_prepare_fp)(struct vm_area_struct *) = NULL;
static int (*try_to_free_swap_fp)(struct page *) = NULL;
static void (*page_add_new_anon_rmap_fp)(struct page *, struct vm_area_struct *, unsigned long) = NULL;
static void (*lru_cache_add_active_or_unevictable_fp)(struct page *page, struct vm_area_struct *vma) = NULL;
static void (*flush_cache_page_fp)(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn) = NULL;
static pte_t (*ptep_clear_flush_fp)(struct vm_area_struct *vma, unsigned long address, pte_t *ptep) = NULL;
static void (*__sync_icache_dcache_fp)(pte_t pteval) = NULL;
static pte_t *(*__page_check_address_fp)(struct page *page, struct mm_struct *mm, unsigned long address, spinlock_t **ptlp, int sync) = NULL;
static void (*page_remove_rmap_fp)(struct page *page) = NULL;
static unsigned int (*munlock_vma_page_fp)(struct page *page) = NULL;

static void (*register_undef_hook_fp)(struct undef_hook *hook) = NULL;
static void (*unregister_undef_hook_fp)(struct undef_hook *hook) = NULL;

static inline pte_t *sb_page_check_address(struct page *page, struct mm_struct *mm,
                    unsigned long address,
                    spinlock_t **ptlp, int sync)
{
    pte_t *ptep;

    __cond_lock(*ptlp, ptep = __page_check_address_fp(page, mm, address,
                               ptlp, sync));
    return ptep;
}

static inline void sb_set_pte_at(struct mm_struct *mm, unsigned long addr,
                  pte_t *ptep, pte_t pteval)
{
    unsigned long ext = 0;

    if (addr < TASK_SIZE && pte_valid_user(pteval)) {
        if (!pte_special(pteval))
            __sync_icache_dcache_fp(pteval);
        ext |= PTE_EXT_NG;
    }

    set_pte_ext(ptep, pteval, ext);
}

static void copy_to_page(struct page *page, unsigned long vaddr, const void *src, int len)
{
    void *kaddr = kmap_atomic(page);
    memcpy(kaddr + (vaddr & ~PAGE_MASK), src, len);
    kunmap_atomic(kaddr);
}

static int __replace_page(struct vm_area_struct *vma, unsigned long addr,
                struct page *page, struct page *kpage)
{
    struct mm_struct *mm = vma->vm_mm;
    spinlock_t *ptl;
    pte_t *ptep;
    int err;
    /* For mmu_notifiers */
    const unsigned long mmun_start = addr;
    const unsigned long mmun_end   = addr + PAGE_SIZE;
    struct mem_cgroup *memcg;

    err = mem_cgroup_try_charge(kpage, vma->vm_mm, GFP_KERNEL, &memcg);
    if (err)
        return err;

    /* For try_to_free_swap() and munlock_vma_page() below */
    lock_page(page);

    mmu_notifier_invalidate_range_start(mm, mmun_start, mmun_end);
    err = -EAGAIN;
    ptep = sb_page_check_address(page, mm, addr, &ptl, 0);
    if (!ptep)
        goto unlock;

    get_page(kpage);
    page_add_new_anon_rmap_fp(kpage, vma, addr);
    mem_cgroup_commit_charge(kpage, memcg, false);
    lru_cache_add_active_or_unevictable_fp(kpage, vma);

    if (!PageAnon(page)) {
        dec_mm_counter(mm, MM_FILEPAGES);
        inc_mm_counter(mm, MM_ANONPAGES);
    }

    flush_cache_page_fp(vma, addr, pte_pfn(*ptep));
    ptep_clear_flush_fp(vma, addr, ptep);
    sb_set_pte_at(mm, addr, ptep, mk_pte(kpage, vma->vm_page_prot));

    page_remove_rmap_fp(page);
    if (!page_mapped(page))
        try_to_free_swap_fp(page);
    pte_unmap_unlock(ptep, ptl);

    if (vma->vm_flags & VM_LOCKED)
        munlock_vma_page_fp(page);
    put_page(page);

    err = 0;
 unlock:
    mem_cgroup_cancel_charge(kpage, memcg);
    mmu_notifier_invalidate_range_end(mm, mmun_start, mmun_end);
    unlock_page(page);
    return err;
}


static int write_opcode(struct mm_struct *mm, unsigned long vaddr, unsigned int opcode)
{
	struct page *old_page, *new_page;
    struct vm_area_struct *vma;
    int ret;

retry:
    /* Read the page with vaddr into memory */
    ret = get_user_pages(NULL, mm, vaddr, 1, 0, 1, &old_page, &vma);
    if (ret <= 0)
        return ret;

    ret = anon_vma_prepare_fp(vma);
    if (ret)
        goto put_old;

    ret = -ENOMEM;
    new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, vaddr);
    if (!new_page)
        goto put_old;

    __SetPageUptodate(new_page);
    copy_highpage(new_page, old_page);
    copy_to_page(new_page, vaddr, &opcode, sizeof(opcode));

    ret = __replace_page(vma, vaddr, old_page, new_page);
    page_cache_release(new_page);
put_old:
    put_page(old_page);

    if (unlikely(ret == -EAGAIN))
        goto retry;
    return ret;
}

int patch_user_memory(unsigned long sva, unsigned long eva)
{
	/* [ToDo] decode insn & replace LDRFLUSH */
	sb_insn code;
	int r;
	insn *ptr;

	/* loop */ 
	for (ptr = (insn *)sva; ptr <= (insn *)eva; ) {
		code = convert_insn_to_sb_insn(*ptr);
		if (code != 0) {
			r = write_opcode(current->mm, (unsigned long)ptr, code);
			if (r) {
				sb_pr_err("write_opcode error : %lx, %08x\n", (unsigned long)ptr, code);
				return -1;
			}
			else {
				sb_pr_info("write_opcode success : %lx, %08x\n", (unsigned long)ptr, code);
			}
		}

		ptr = (insn *)((char *)ptr + sizeof(insn));
	}

	return 0;
}

int fix_unresolve_function_ptrs(void)
{
	anon_vma_prepare_fp = (void *)kallsyms_lookup_name("anon_vma_prepare");
	try_to_free_swap_fp = (void *)kallsyms_lookup_name("try_to_free_swap");
	page_add_new_anon_rmap_fp = (void *)kallsyms_lookup_name("page_add_new_anon_rmap");
	lru_cache_add_active_or_unevictable_fp = (void *)kallsyms_lookup_name("lru_cache_add_active_or_unevictable");
	flush_cache_page_fp = (void *)kallsyms_lookup_name("flush_cache_page");
	ptep_clear_flush_fp = (void *)kallsyms_lookup_name("ptep_clear_flush");
	__sync_icache_dcache_fp = (void *)kallsyms_lookup_name("__sync_icache_dcache");
	__page_check_address_fp = (void *)kallsyms_lookup_name("__page_check_address");
	page_remove_rmap_fp = (void *)kallsyms_lookup_name("page_remove_rmap");
	munlock_vma_page_fp = (void *)kallsyms_lookup_name("munlock_vma_page");

	register_undef_hook_fp = (void *)kallsyms_lookup_name("register_undef_hook");
	unregister_undef_hook_fp = (void *)kallsyms_lookup_name("unregister_undef_hook");

	if (!anon_vma_prepare_fp || !try_to_free_swap_fp || !page_add_new_anon_rmap_fp || !lru_cache_add_active_or_unevictable_fp ||
		!flush_cache_page_fp || !ptep_clear_flush_fp || !__sync_icache_dcache_fp || !__page_check_address_fp || !page_remove_rmap_fp || 
		!munlock_vma_page_fp || !register_undef_hook_fp || !unregister_undef_hook_fp)
		return -1;

	return 0;
}

struct undef_hook smoke_bomb_ex_hook = {
    .instr_mask = 0xff300010,
    .instr_val  = 0x07200010,
    .cpsr_mask  = 0,
    .cpsr_val   = 0,
    .fn     = smoke_bomb_ex_handler,
};

void register_ex_handler(void)
{
	register_undef_hook_fp(&smoke_bomb_ex_hook);
}

void unregister_ex_handler(void)
{
	unregister_undef_hook_fp(&smoke_bomb_ex_hook);
}

