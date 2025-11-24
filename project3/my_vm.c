
#include "my_vm.h"
#include <string.h> // optional for memcpy if you later implement put/get
// -----------------------------------------------------------------------------
// Global Declarations (optional)
// -----------------------------------------------------------------------------

struct tlb tlbGlobal[TLB_ENTRIES];
// Optional counters for TLB statistics
static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses = 0;
static uintptr_t next_virt_addr = 0;
void *phys_mem = NULL;
uint8_t *phys_bitmap = NULL;
uint8_t *virt_bitmap = NULL;
pde_t *pgdir = NULL;

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
/*
 * set_physical_mem()
 * ------------------
 * Allocates and initializes simulated physical memory and any required
 * data structures (e.g., bitmaps for tracking page use).
 *
 * Return value: None.
 * Errors should be handled internally (e.g., failed allocation).
 */
void set_physical_mem(void)
{
    phys_mem = malloc(MEMSIZE);
    if (!phys_mem)
    {
        exit(1);
    }

    memset(phys_mem, 0, MEMSIZE);

    uint32_t num_phys_pages = MEMSIZE / PGSIZE;
    phys_bitmap = calloc(num_phys_pages, 1);

    if (!phys_bitmap)
    {
        exit(1);
    }

    uint32_t num_virt_pages = MAX_MEMSIZE / PGSIZE;
    virt_bitmap = calloc(num_virt_pages, 1);

    if (!virt_bitmap)
    {
        exit(1);
    }

    pgdir = calloc(1024, sizeof(pde_t));

    if (!pgdir)
    {
        exit(1);
    }
}

// -----------------------------------------------------------------------------
// TLB
// -----------------------------------------------------------------------------

/*
 * TLB_add()
 * ---------
 * Adds a new virtual-to-physical translation to the TLB.
 * Ensure thread safety when updating shared TLB data.
 *
 * Return:
 *   0  -> Success (translation successfully added)
 *  -1  -> Failure (e.g., TLB full or invalid input)
 */
int TLB_add(void *va, void *pa)
{
    if (va == NULL || pa == NULL)
        return -1;

    vaddr32_t vaddr = VA2U(va);
    vaddr32_t vpn = vaddr >> PFN_SHIFT;
    vaddr32_t pfn = VA2U(pa) >> PFN_SHIFT;

    int i;
    for (i = 0; i < TLB_ENTRIES; i++)
    {
        if (!tlbGlobal[i].valid)
        {
            tlbGlobal[i].vpn = vpn;
            tlbGlobal[i].pfn = pfn;
            tlbGlobal[i].valid = true;
            tlbGlobal[i].last_used = ++tlb_lookups;
            return 0;
        }
    }

    // LRU replacement
    int lru_index = 0;
    uint64_t min_time = tlbGlobal[0].last_used;

    for (i = 1; i < TLB_ENTRIES; i++)
    {
        if (tlbGlobal[i].last_used < min_time)
        {
            min_time = tlbGlobal[i].last_used;
            lru_index = i;
        }
    }

    tlbGlobal[lru_index].vpn = vpn;
    tlbGlobal[lru_index].pfn = pfn;
    tlbGlobal[lru_index].valid = true;
    tlbGlobal[lru_index].last_used = ++tlb_lookups;

    return 0;
}

/*
 * TLB_check()
 * -----------
 * Looks up a virtual address in the TLB.
 *
 * Return:
 *   Pointer to the corresponding page table entry (PTE) if found.
 *   NULL if the translation is not found (TLB miss).
 */
pte_t *TLB_check(void *va)
{
    if (!va)
        return NULL;

    vaddr32_t vpn = VA2U(va) >> PFN_SHIFT;

    for (int i = 0; i < TLB_ENTRIES; i++)
    {
        if (tlbGlobal[i].valid && tlbGlobal[i].vpn == vpn)
        {
            tlbGlobal[i].last_used = ++tlb_lookups;

            paddr32_t pa = tlbGlobal[i].pfn << PFN_SHIFT;
            return (pte_t *)U2VA(pa);
        }
    }

    // TLB miss
    tlb_misses++;
    return NULL;
}

/*
 * print_TLB_missrate()
 * --------------------
 * Calculates and prints the TLB miss rate.
 *
 * Return value: None.
 */
void print_TLB_missrate(void)
{
    double miss_rate = (tlb_lookups == 0) ? 0.0 : ((double)tlb_misses / tlb_lookups);
    // TODO: Calculate miss rate as (tlb_misses / tlb_lookups).
    fprintf(stderr, "TLB miss rate %lf \n", miss_rate);
}

// -----------------------------------------------------------------------------
// Page Table
// -----------------------------------------------------------------------------

/*
 * translate()
 * -----------
 * Translates a virtual address to a physical address.
 * Perform a TLB lookup first; if not found, walk the page directory
 * and page tables using a two-level lookup.
 *
 * Return:
 *   Pointer to the PTE structure if translation succeeds.
 *   NULL if translation fails (e.g., page not mapped).
 */
pte_t *translate(pde_t *pgdir, void *va)
{
    uintptr_t vaddr = (uintptr_t)va;
    uint32_t pd_index = PDX(vaddr);
    uint32_t pt_index = PTX(vaddr);

    pte_t *pte = TLB_check(va);
    if (pte)
        return pte;

    pde_t pde = pgdir[pd_index];
    if (pde == 0)
        return NULL;

    pte_t *pt = (pte_t *)(phys_mem + (pde >> PFN_SHIFT) * PGSIZE);
    pte = &pt[pt_index];

    if (*pte == 0)
        return NULL;

    TLB_add(va, phys_mem + (*pte & ~OFFMASK));
    return pte;
}

/*
 * map_page()
 * -----------
 * Establishes a mapping between a virtual and a physical page.
 * Creates intermediate page tables if necessary.
 *
 * Return:
 *   0  -> Success (mapping created)
 *  -1  -> Failure (e.g., no space or invalid address)
 */
int map_page(pde_t *pgdir, void *va, void *pa)
{
    uintptr_t vaddr = (uintptr_t)va;
    uintptr_t paddr = (uintptr_t)pa;

    if ((vaddr & OFFMASK) != 0 || (paddr & OFFMASK) != 0)
        return -1;

    uint32_t pd_index = PDX(vaddr);
    uint32_t pt_index = PTX(vaddr);
    pde_t pde = pgdir[pd_index];
    pte_t *pt;

    if (pde == 0)
    {
        // Allocate a page for the page table in physical memory
        for (int i = 0; i < MEMSIZE / PGSIZE; i++)
        {
            if (!phys_bitmap[i])
            {
                phys_bitmap[i] = 1;
                pt = (pte_t *)(phys_mem + i * PGSIZE);
                memset(pt, 0, PGSIZE);
                pgdir[pd_index] = i << PFN_SHIFT;
                break;
            }
        }
        if (pgdir[pd_index] == 0)
            return -1;
    }
    else
    {
        pt = (pte_t *)(phys_mem + (pde >> PFN_SHIFT) * PGSIZE);
    }

    if (pt[pt_index] != 0)
        return -1;

    pt[pt_index] = paddr & ~OFFMASK;
    return 0;
}

// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------

/*
 * get_next_avail()
 * ----------------
 * Finds and returns the base virtual address of the next available
 * block of contiguous free pages.
 *
 * Return:
 *   Pointer to the base virtual address if available.
 *   NULL if there are no sufficient free pages.
 */
void *get_next_avail(int num_pages)
{
    if (num_pages <= 0)
        return NULL;

    uintptr_t base = next_virt_addr;
    next_virt_addr += num_pages * PGSIZE;

    for (int i = 0; i < num_pages; i++)
    {
        virt_bitmap[(base / PGSIZE) + i] = 1;
    }

    return (void *)base;
}

/*
 * n_malloc()
 * -----------
 * Allocates a given number of bytes in virtual memory.
 * Initializes physical memory and page directories if not already done.
 *
 * Return:
 *   Pointer to the starting virtual address of allocated memory (success).
 *   NULL if allocation fails.
 */
void *n_malloc(unsigned int num_bytes)
{
    if (!num_bytes || !phys_mem || !pgdir || !virt_bitmap || !phys_bitmap)
        return NULL;

    int num_pages = (num_bytes + PGSIZE - 1) / PGSIZE;
    void *va_base = get_next_avail(num_pages);
    if (!va_base)
        return NULL;

    int allocated_pages[num_pages];
    for (int i = 0; i < num_pages; i++)
        allocated_pages[i] = -1;

    for (int i = 0; i < num_pages; i++)
    {
        // find a free physical page
        for (int j = 0; j < MEMSIZE / PGSIZE; j++)
        {
            if (!phys_bitmap[j])
            {
                phys_bitmap[j] = 1;
                allocated_pages[i] = j;
                break;
            }
        }
        if (allocated_pages[i] == -1)
        {
            // rollback allocated pages if no free page found
            for (int k = 0; k < i; k++)
            {
                phys_bitmap[allocated_pages[k]] = 0;
                void *rollback_va = (void *)((uintptr_t)va_base + k * PGSIZE);
                pte_t *pte = translate(pgdir, rollback_va);
                if (pte)
                    *pte = 0;
            }
            return NULL;
        }

        void *va = (void *)((uintptr_t)va_base + i * PGSIZE);
        void *pa = phys_mem + allocated_pages[i] * PGSIZE;
        if (map_page(pgdir, va, pa) != 0)
        {
            phys_bitmap[allocated_pages[i]] = 0;
            for (int k = 0; k < i; k++)
            {
                void *rollback_va = (void *)((uintptr_t)va_base + k * PGSIZE);
                pte_t *pte = translate(pgdir, rollback_va);
                if (pte)
                    *pte = 0;
                phys_bitmap[allocated_pages[k]] = 0;
            }
            return NULL;
        }
    }

    return va_base;
}

/*
 * n_free()
 * ---------
 * Frees one or more pages of memory starting at the given virtual address.
 * Marks the corresponding virtual and physical pages as free.
 * Removes the translation from the TLB.
 *
 * Return value: None.
 */
void n_free(void *va, int size)
{
    if (!va || size <= 0)
        return;

    int num_pages = (size + PGSIZE - 1) / PGSIZE;
    uintptr_t vaddr_base = (uintptr_t)va;

    for (int i = 0; i < num_pages; i++)
    {
        void *curr_va = (void *)(vaddr_base + i * PGSIZE);
        pte_t *pte = translate(pgdir, curr_va);
        if (pte && *pte != 0)
        {
            uintptr_t pa = *pte & ~OFFMASK;
            uint32_t pfn = (pa - (uintptr_t)phys_mem) / PGSIZE;

            if (pfn < MEMSIZE / PGSIZE)
                phys_bitmap[pfn] = 0;
            *pte = 0;

            uint32_t vpn = VA2U(curr_va) >> PFN_SHIFT;
            if (vpn < MAX_MEMSIZE / PGSIZE)
                virt_bitmap[vpn] = 0;

            for (int j = 0; j < TLB_ENTRIES; j++)
            {
                if (tlbGlobal[j].valid && tlbGlobal[j].vpn == vpn)
                {
                    tlbGlobal[j].valid = false;
                    break;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Data Movement
// -----------------------------------------------------------------------------

/*
 * put_data()
 * ----------
 * Copies data from a user buffer into simulated physical memory using
 * the virtual address. Handle page boundaries properly.
 *
 * Return:
 *   0  -> Success (data written successfully)
 *  -1  -> Failure (e.g., translation failure)
 */
int put_data(void *va, void *val, int size)
{
    if (!va || !val || size <= 0)
        return -1;

    uintptr_t src_offset = 0;
    uintptr_t dst_va = (uintptr_t)va;

    while (src_offset < (uintptr_t)size)
    {
        pte_t *pte = translate(pgdir, (void *)dst_va);
        if (!pte || *pte == 0)
            return -1;

        uintptr_t pfn = *pte >> PFN_SHIFT;
        uintptr_t page_offset = dst_va & OFFMASK;
        uintptr_t pa = (pfn << PFN_SHIFT) + page_offset;

        int bytes_in_page = PGSIZE - page_offset;
        int bytes_to_copy = (size - src_offset < (uintptr_t)bytes_in_page) ? size - src_offset : bytes_in_page;

        memcpy((uint8_t *)phys_mem + pa, (uint8_t *)val + src_offset, bytes_to_copy);

        src_offset += bytes_to_copy;
        dst_va += bytes_to_copy;
    }

    return 0;
}

/*
 * get_data()
 * -----------
 * Copies data from simulated physical memory (accessed via virtual address)
 * into a user buffer.
 *
 * Return value: None.
 */
void get_data(void *va, void *val, int size)
{
    if (!va || !val || size <= 0)
        return;

    uintptr_t dst_offset = 0;
    uintptr_t src_va = (uintptr_t)va;

    while (dst_offset < (uintptr_t)size)
    {
        pte_t *pte = translate(pgdir, (void *)src_va);
        if (!pte || *pte == 0)
        {
            return;
        }

        uintptr_t pfn = *pte >> PFN_SHIFT;
        uintptr_t page_offset = src_va & OFFMASK;
        uintptr_t pa = (pfn << PFN_SHIFT) + page_offset;

        int bytes_in_page = PGSIZE - page_offset;
        int bytes_to_copy = (size - dst_offset < (uintptr_t)bytes_in_page) ? size - dst_offset : bytes_in_page;

        memcpy((uint8_t *)val + dst_offset, (uint8_t *)phys_mem + pa, bytes_to_copy);

        // Advance
        dst_offset += bytes_to_copy;
        src_va += bytes_to_copy;
    }
}

// -----------------------------------------------------------------------------
// Matrix Multiplication
// -----------------------------------------------------------------------------

/*
 * mat_mult()
 * ----------
 * Performs matrix multiplication of two matrices stored in virtual memory.
 * Each element is accessed and stored using get_data() and put_data().
 *
 * Return value: None.
 */
void mat_mult(void *mat1, void *mat2, int size, void *answer)
{
    int i, j, k;
    int a_val, b_val, c_val;

    for (i = 0; i < size; i++)
    {
        for (j = 0; j < size; j++)
        {
            c_val = 0;

            for (k = 0; k < size; k++)
            {
                void *addr1 = (void *)((uintptr_t)mat1 + (i * size + k) * sizeof(int));
                void *addr2 = (void *)((uintptr_t)mat2 + (k * size + j) * sizeof(int));

                a_val = 0; 
                b_val = 0;

                get_data(addr1, &a_val, sizeof(int));
                get_data(addr2, &b_val, sizeof(int));

                c_val += a_val * b_val;
            }

            void *addr_out = (void *)((uintptr_t)answer + (i * size + j) * sizeof(int));
            put_data(addr_out, &c_val, sizeof(int));
        }
    }
}
