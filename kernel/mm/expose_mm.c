#include <linux/slab.h>
#include <asm/pgtable.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/syscalls.h>
#include <asm/page.h>
#include <asm/memory.h>
#include <asm/uaccess.h>

struct walk_info {
	unsigned long user_fake_pgd_base;
	unsigned long user_fake_pmd_base;
	unsigned long user_fake_pte_base;
	unsigned long last_written_pgd_val;
	unsigned long last_written_pmd_val;
	unsigned long last_written_pte_val;
};


int my_pgd_entry(pgd_t *pgd, unsigned long addr, unsigned long next, 
	      struct mm_walk *walk)
{
	unsigned long pgd_index = pgd_index(addr);
	//unsigned long pgd_index = pgd - walk->mm->pgd;
	struct walk_info *my_walk_info = (struct walk_info *)walk->private;
	unsigned long current_pgd_base = my_walk_info->user_fake_pgd_base;
	printk("Before put_user addr = %lu\n", addr);
	if (put_user(my_walk_info->last_written_pmd_val, 
		  (pgd_t*)current_pgd_base + pgd_index)) {
		return -EFAULT;
	}
	printk("After put_user addr = %lu\n", addr);
	my_walk_info->last_written_pmd_val += PAGE_SIZE;
	return 0;
}

int my_pmd_entry(pmd_t *pmd, unsigned long addr, unsigned long next, 
	      struct mm_walk *walk)
{	
	unsigned long pmd_index = pmd_index(addr);
	struct walk_info *my_walk_info = (struct walk_info *)walk->private;
	
	unsigned long current_pte_base = my_walk_info->last_written_pte_val;
	struct vm_area_struct *user_vma = 
		find_vma(current->mm, current_pte_base);
	if (split_vma(current->mm, user_vma, current_pte_base + PAGE_SIZE, 0))
		return -EFAULT;

	if (user_vma == NULL)
		return -EINVAL;
	
	if (unlikely(user_vma->vm_start != current_pte_base)) {
		printk("vma_start mismatch\n");
		return -EFAULT;
	}
	if (unlikely(user_vma->vm_end != current_pte_base + PAGE_SIZE)) {
		printk("vma_end mismatch\n");
		return -EFAULT;
	}
	/* TODO: Check how to use PROT_READ flag */
	/* TODO: Think about behavior later*/

	if (pmd == NULL)
		return 0;

	unsigned long pfn = page_to_pfn(pmd_page(*pmd));
	unsigned long pfn_tmp = pmd_pfn(*pmd);
	if (pfn_tmp == pfn)
		printk("we have the same wrong pfn!\n");
	//unsigned long pfn = pmd_val(*pmd)&PHYS_MASK >> PAGE_SHIFT;
	if (pmd_bad(*pmd) || !pfn_valid(pfn)) 
		return -EINVAL;
	
	printk("Before remap_pfn_range %lu\n", addr);
	int err = 0;
	err = remap_pfn_range(user_vma, current_pte_base, 
		pfn, PAGE_SIZE, user_vma->vm_page_prot);
	if (err) {
		printk("remap_pfn_range errno %d\n", err);
		return -EINVAL;
	}
	//if (remap_pfn_range(user_vma, current_pte_base, 
	//	pfn, PAGE_SIZE, user_vma->vm_page_prot)) 
	//	return -EINVAL;
	printk("After remap_pfn_range %lu\n", addr);

	unsigned long current_pmd_base = 
		my_walk_info->last_written_pmd_val - PAGE_SIZE;
	//TODO: figure out why put_user might fail!! is it failing because of
	//semaphore?
	printk("Before put_user in pmd %lu\n", addr);
	if (put_user(my_walk_info->last_written_pte_val, 
		 (pmd_t*)current_pmd_base + pmd_index))
		return -EFAULT;
	printk("After put_user in pmd %lu\n", addr);
	my_walk_info->last_written_pte_val += PAGE_SIZE;
	
	return 0;
}
SYSCALL_DEFINE2(get_pagetable_layout, struct pagetable_layout_info __user *, 
		pgtbl_info, int, size) 
{
	struct pagetable_layout_info layout_info;
	if (size < sizeof(struct pagetable_layout_info))
		return -EINVAL;
	layout_info.pgdir_shift = PGDIR_SHIFT;
	layout_info.pmd_shift = PMD_SHIFT;
	layout_info.page_shift = PAGE_SHIFT;
	if (copy_to_user(pgtbl_info, &layout_info, size))
		return -EFAULT;
	
	return 0;

}

SYSCALL_DEFINE6(expose_page_table, pid_t, pid, unsigned long, fake_pgd,
		unsigned long, fake_pmds, unsigned long, page_table_addr, 
		unsigned long, begin_vaddr, unsigned long, end_vaddr) 
{
	struct mm_walk walk;
	struct walk_info my_walk_info;
	struct task_struct *target_tsk;
	struct vm_area_struct *user_vma;

	//TODO: RCU lock
	target_tsk = pid == -1 ? current : find_task_by_vpid(pid);
	if (target_tsk == NULL)
		return -EINVAL;
	down_write(&current->mm->mmap_sem);
	if (pid != -1)
		down_write(&target_tsk->mm->mmap_sem);
	//prepare member functions of struct mm_walk *walk;
	my_walk_info.user_fake_pmd_base = fake_pmds;
	my_walk_info.user_fake_pgd_base = fake_pgd;
	my_walk_info.user_fake_pte_base = page_table_addr;
	my_walk_info.last_written_pgd_val = fake_pgd;
	my_walk_info.last_written_pmd_val = fake_pmds;
	my_walk_info.last_written_pte_val = page_table_addr;

	walk.mm = target_tsk->mm;
	walk.private = &my_walk_info;
	walk.pgd_entry = my_pgd_entry;
	walk.pmd_entry = my_pmd_entry;
	walk.pte_entry = NULL;
	walk.pud_entry = NULL;
	walk.pte_hole = NULL;
	walk.hugetlb_entry = NULL;
	//walk->pgd_entry = my_pgd_entry;
	//walk->pmd_entry = my_pmd_entry;

	walk_page_range(begin_vaddr, end_vaddr, &walk);
	if (pid != -1)
		up_write(&target_tsk->mm->mmap_sem);
	up_write(&current->mm->mmap_sem);
	return 0;
}
