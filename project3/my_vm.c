// my_vm.c
#include "my_vm.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h> // memcpy, memset
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>


#define VA_BASE 0x40000000u

// -----------------------------------------------------------------------------
// Global Declarations
// -----------------------------------------------------------------------------

struct tlb tlbGlobal[TLB_ENTRIES];
// Optional counters for TLB statistics
static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses = 0;
static uintptr_t next_virt_addr = VA_BASE; 
void *phys_mem = NULL;
uint8_t *phys_bitmap = NULL;
uint8_t *virt_bitmap = NULL;
pde_t *pgdir = NULL;

static bool vm_initialized = false;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER; // protect allocation bitmaps, next_virt_addr
static pthread_mutex_t tlb_lock = PTHREAD_MUTEX_INITIALIZER; // protect TLB ops

// -----------------------------------------------------------------------------
// Internal helper: lazy init
// -----------------------------------------------------------------------------
static void ensure_vm_init(void)
{
    if (vm_initialized)
        return;

    // basic initialization: allocate physical memory structures
    set_physical_mem();
    // next_virt_addr already has a default non-zero value above
    vm_initialized = true;
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void set_physical_mem(void)
{
    if (phys_mem)
        return; // already set

    phys_mem = malloc(MEMSIZE);
    if (!phys_mem)
    {
        fprintf(stderr, "set_physical_mem: malloc failed\n");
        exit(1);
    }

    memset(phys_mem, 0, MEMSIZE);

    uint32_t num_phys_pages = MEMSIZE / PGSIZE;
    phys_bitmap = calloc(num_phys_pages, 1);
    if (!phys_bitmap)
    {
        fprintf(stderr, "set_physical_mem: phys_bitmap calloc failed\n");
        exit(1);
    }

    uint32_t num_virt_pages = MAX_MEMSIZE / PGSIZE;
    virt_bitmap = calloc(num_virt_pages, 1);
    if (!virt_bitmap)
    {
        fprintf(stderr, "set_physical_mem: virt_bitmap calloc failed\n");
        exit(1);
    }

    // 1024 entries as in original (two-level page table)
    pgdir = calloc(1024, sizeof(pde_t));
    if (!pgdir)
    {
        fprintf(stderr, "set_physical_mem: pgdir calloc failed\n");
        exit(1);
    }

    // ensure initialized flags
    vm_initialized = true;
}

// -----------------------------------------------------------------------------
// TLB
// -----------------------------------------------------------------------------

int TLB_add(void *va, void *pa)
{
    if (va == NULL || pa == NULL)
        return -1;

    vaddr32_t vaddr = VA2U(va);
    vaddr32_t vpn = vaddr >> PFN_SHIFT;

    // Convert pa into a frame number relative to phys_mem
    if (!phys_mem)
        return -1;

    uintptr_t pa_off = (uintptr_t)pa - (uintptr_t)phys_mem;
    if ((intptr_t)pa_off < 0)
        return -1;
    vaddr32_t pfn = (vaddr32_t)(pa_off >> PFN_SHIFT);

    pthread_mutex_lock(&tlb_lock);

    // Try to place into an empty TLB entry
    for (int i = 0; i < TLB_ENTRIES; i++)
    {
        if (!tlbGlobal[i].valid)
        {
            tlbGlobal[i].vpn = vpn;
            tlbGlobal[i].pfn = pfn;
            tlbGlobal[i].valid = true;
            tlbGlobal[i].last_used = ++tlb_lookups;
            pthread_mutex_unlock(&tlb_lock);
            return 0;
        }
    }

    // LRU replacement
    int lru_index = 0;
    uint64_t min_time = tlbGlobal[0].last_used;
    for (int i = 1; i < TLB_ENTRIES; i++)
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

    pthread_mutex_unlock(&tlb_lock);
    return 0;
}

/*
 * TLB_check()
 * For simplicity (and safety) this TLB implementation only tracks vpn->pfn
 * and updates stats. To keep translate() behavior simple and correct we return
 * NULL so page-table walk happens. This avoids complexities where TLB would
 * need to return a pointer into a page table page stored inside phys_mem.
 *
 * Keeping the TLB as a stats structure is still useful and can be extended
 * later to short-circuit translate() safely.
 */
pte_t *TLB_check(void *va)
{
    if (!va)
        return NULL;

    vaddr32_t vpn = VA2U(va) >> PFN_SHIFT;

    pthread_mutex_lock(&tlb_lock);
    for (int i = 0; i < TLB_ENTRIES; i++)
    {
        if (tlbGlobal[i].valid && tlbGlobal[i].vpn == vpn)
        {
            tlbGlobal[i].last_used = ++tlb_lookups;
            // We don't return a PTE pointer here; let translate() walk the page table.
            pthread_mutex_unlock(&tlb_lock);
            return NULL;
        }
    }

    // TLB miss
    tlb_misses++;
    pthread_mutex_unlock(&tlb_lock);
    return NULL;
}

void print_TLB_missrate(void)
{
    double miss_rate = (tlb_lookups == 0) ? 0.0 : ((double)tlb_misses / tlb_lookups);
    fprintf(stderr, "TLB miss rate %lf \n", miss_rate);
}

// -----------------------------------------------------------------------------
// Page Table
// -----------------------------------------------------------------------------

pte_t *translate(pde_t *pgdir_param, void *va)
{
    if (!va || !pgdir_param)
        return NULL;

    ensure_vm_init();

    uintptr_t vaddr = (uintptr_t)va;
    uint32_t pd_index = PDX(vaddr);
    uint32_t pt_index = PTX(vaddr);

    // First try TLB (current TLB design is stats-only: returns NULL so we do full walk)
    pte_t *pte = TLB_check(va);
    if (pte)
        return pte;

    pde_t pde = pgdir_param[pd_index];
    if (pde == 0)
        return NULL;

    // pde stores frame index << PFN_SHIFT (consistent with map_page below)
    uint32_t pt_frame = (uint32_t)(pde >> PFN_SHIFT);
    if ((size_t)pt_frame >= (MEMSIZE / PGSIZE))
        return NULL; // invalid
    pte_t *pt = (pte_t *)((uint8_t *)phys_mem + (pt_frame * PGSIZE));
    pte = &pt[pt_index];

    if (*pte == 0)
        return NULL;

    // Add to TLB for statistics (pa = phys_mem + frame*PGSIZE)
    uint32_t data_frame = (uint32_t)(*pte >> PFN_SHIFT);
    void *pa = (void *)((uint8_t *)phys_mem + data_frame * PGSIZE);
    TLB_add(va, pa);

    return pte;
}

int map_page(pde_t *pgdir_param, void *va, void *pa)
{
    if (!pgdir_param || !va || !pa)
        return -1;

    ensure_vm_init();

    uintptr_t vaddr = (uintptr_t)va;
    uintptr_t paddr = (uintptr_t)pa;

    if ((vaddr & OFFMASK) != 0 || (paddr & OFFMASK) != 0)
        return -1;

    uint32_t pd_index = PDX(vaddr);
    uint32_t pt_index = PTX(vaddr);
    pde_t pde = pgdir_param[pd_index];
    pte_t *pt = NULL;

    pthread_mutex_lock(&vm_lock);

    if (pde == 0)
    {
        // Allocate a page for the page table in physical memory
        bool allocated_pt = false;
        for (int i = 0; i < MEMSIZE / PGSIZE; i++)
        {
            if (!phys_bitmap[i])
            {
                phys_bitmap[i] = 1;
                pt = (pte_t *)((uint8_t *)phys_mem + i * PGSIZE);
                memset(pt, 0, PGSIZE);
                pgdir_param[pd_index] = (pde_t)( ( (uintptr_t)i ) << PFN_SHIFT );
                allocated_pt = true;
                break;
            }
        }
        if (!allocated_pt)
        {
            pthread_mutex_unlock(&vm_lock);
            return -1;
        }
    }
    else
    {
        uint32_t pt_frame = (uint32_t)(pde >> PFN_SHIFT);
        pt = (pte_t *)((uint8_t *)phys_mem + pt_frame * PGSIZE);
    }

    // check existing mapping
    if (pt[pt_index] != 0)
    {
        pthread_mutex_unlock(&vm_lock);
        return -1;
    }

    // Compute the frame index for 'pa'
    uintptr_t pa_off = paddr - (uintptr_t)phys_mem;
    if ((intptr_t)pa_off < 0)
    {
        pthread_mutex_unlock(&vm_lock);
        return -1;
    }
    uint32_t frame_index = (uint32_t)(pa_off / PGSIZE);
    if (frame_index >= (MEMSIZE / PGSIZE))
    {
        pthread_mutex_unlock(&vm_lock);
        return -1;
    }

    // Store frame index << PFN_SHIFT in the PTE (consistent format)
    pt[pt_index] = (pte_t)( ((uintptr_t)frame_index) << PFN_SHIFT );

    pthread_mutex_unlock(&vm_lock);
    return 0;
}

// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------


void *get_next_avail(int num_pages)
{
    if (num_pages <= 0)
        return NULL;

    ensure_vm_init();

    pthread_mutex_lock(&vm_lock);

    uintptr_t base = next_virt_addr;

    // mark virtual pages as allocated
    uint32_t start_page = (uint32_t)(base / PGSIZE);
    for (int i = 0; i < num_pages; i++)
    {
        uint32_t idx = start_page + i;
        if (idx >= (MAX_MEMSIZE / PGSIZE)) {
            pthread_mutex_unlock(&vm_lock);
            return NULL;
        }
        virt_bitmap[idx] = 1;
    }

    next_virt_addr += (uintptr_t)num_pages * PGSIZE;

    pthread_mutex_unlock(&vm_lock);
    return (void *)base;
}


void *n_malloc(unsigned int num_bytes)
{
    ensure_vm_init();

    if (!num_bytes || !phys_mem || !pgdir || !virt_bitmap || !phys_bitmap)
        return NULL;

    int num_pages = (num_bytes + PGSIZE - 1) / PGSIZE;
    void *va_base = get_next_avail(num_pages);
    if (!va_base)
        return NULL;

    int *allocated_pages = calloc(num_pages, sizeof(int));
    if (!allocated_pages)
        return NULL;

    for (int i = 0; i < num_pages; i++)
        allocated_pages[i] = -1;

    // Allocate physical pages and map
    for (int i = 0; i < num_pages; i++)
    {
        int found = -1;
        pthread_mutex_lock(&vm_lock);
        for (int j = 0; j < MEMSIZE / PGSIZE; j++)
        {
            if (!phys_bitmap[j])
            {
                phys_bitmap[j] = 1;
                found = j;
                break;
            }
        }
        pthread_mutex_unlock(&vm_lock);

        if (found == -1)
        {
            // rollback allocated pages if no free page found
            for (int k = 0; k < i; k++)
            {
                int fr = allocated_pages[k];
                if (fr >= 0)
                {
                    pthread_mutex_lock(&vm_lock);
                    phys_bitmap[fr] = 0;
                    pthread_mutex_unlock(&vm_lock);

                    void *rollback_va = (void *)((uintptr_t)va_base + k * PGSIZE);
                    pte_t *pte = translate(pgdir, rollback_va);
                    if (pte)
                        *pte = 0;
                }
            }
            free(allocated_pages);
            return NULL;
        }

        allocated_pages[i] = found;

        void *va = (void *)((uintptr_t)va_base + i * PGSIZE);
        void *pa = (void *)((uint8_t *)phys_mem + found * PGSIZE);

        if (map_page(pgdir, va, pa) != 0)
        {
            // rollback current and previous allocations
            pthread_mutex_lock(&vm_lock);
            phys_bitmap[found] = 0;
            pthread_mutex_unlock(&vm_lock);

            for (int k = 0; k < i; k++)
            {
                int fr = allocated_pages[k];
                if (fr >= 0)
                {
                    pthread_mutex_lock(&vm_lock);
                    phys_bitmap[fr] = 0;
                    pthread_mutex_unlock(&vm_lock);

                    void *rollback_va = (void *)((uintptr_t)va_base + k * PGSIZE);
                    pte_t *pte = translate(pgdir, rollback_va);
                    if (pte)
                        *pte = 0;
                }
            }
            free(allocated_pages);
            return NULL;
        }
    }

    free(allocated_pages);
    return va_base;
}

void n_free(void *va, int size)
{
    if (!va || size <= 0)
        return;

    ensure_vm_init();

    int num_pages = (size + PGSIZE - 1) / PGSIZE;
    uintptr_t vaddr_base = (uintptr_t)va;

    for (int i = 0; i < num_pages; i++)
    {
        void *curr_va = (void *)(vaddr_base + i * PGSIZE);

        pte_t *pte = translate(pgdir, curr_va);   // translate() handles its own locking
        if (!pte || *pte == 0)
            continue;

        uint32_t frame = (uint32_t)(*pte >> PFN_SHIFT);

        // Free physical frame
        pthread_mutex_lock(&vm_lock);
        phys_bitmap[frame] = 0;
        pthread_mutex_unlock(&vm_lock);

        // Clear PTE
        *pte = 0;

        // Free virtual bitmap
        uint32_t vpn = VA2U(curr_va) >> PFN_SHIFT;
        pthread_mutex_lock(&vm_lock);
        virt_bitmap[vpn] = 0;
        pthread_mutex_unlock(&vm_lock);

        // Invalidate TLB
        pthread_mutex_lock(&tlb_lock);
        for (int j = 0; j < TLB_ENTRIES; j++)
        {
            if (tlbGlobal[j].valid && tlbGlobal[j].vpn == vpn)
            {
                tlbGlobal[j].valid = 0;
                break;
            }
        }
        pthread_mutex_unlock(&tlb_lock);
    }
}


// -----------------------------------------------------------------------------
// Data Movement
// -----------------------------------------------------------------------------

int put_data(void *va, void *val, int size)
{
    if (!va || !val || size <= 0)
        return -1;

    ensure_vm_init();

    uintptr_t src_offset = 0;
    uintptr_t dst_va = (uintptr_t)va;

    while (src_offset < (uintptr_t)size)
    {
        pte_t *pte = translate(pgdir, (void *)dst_va);
        if (!pte || *pte == 0)
            return -1;

        uintptr_t pfn = (uintptr_t)(*pte >> PFN_SHIFT);
        uintptr_t page_offset = dst_va & OFFMASK;
        uintptr_t pa_off_bytes = (pfn << PFN_SHIFT) + page_offset;

        int bytes_in_page = PGSIZE - page_offset;
        int bytes_to_copy = (size - src_offset < bytes_in_page) ? (size - src_offset) : bytes_in_page;

        memcpy((uint8_t *)phys_mem + pa_off_bytes, (uint8_t *)val + src_offset, bytes_to_copy);

        src_offset += bytes_to_copy;
        dst_va += bytes_to_copy;
    }

    return 0;
}

void get_data(void *va, void *val, int size)
{
    if (!va || !val || size <= 0)
        return;

    ensure_vm_init();

    uintptr_t dst_offset = 0;
    uintptr_t src_va = (uintptr_t)va;

    while (dst_offset < (uintptr_t)size)
    {
        pte_t *pte = translate(pgdir, (void *)src_va);
        if (!pte || *pte == 0)
        {
            return;
        }

        uintptr_t pfn = (uintptr_t)(*pte >> PFN_SHIFT);
        uintptr_t page_offset = src_va & OFFMASK;
        uintptr_t pa_off_bytes = (pfn << PFN_SHIFT) + page_offset;

        int bytes_in_page = PGSIZE - page_offset;
        int bytes_to_copy = (size - dst_offset < bytes_in_page) ? (size - dst_offset) : bytes_in_page;

        memcpy((uint8_t *)val + dst_offset, (uint8_t *)phys_mem + pa_off_bytes, bytes_to_copy);

        // Advance
        dst_offset += bytes_to_copy;
        src_va += bytes_to_copy;
    }
}

// -----------------------------------------------------------------------------
// Matrix Multiplication
// -----------------------------------------------------------------------------

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
