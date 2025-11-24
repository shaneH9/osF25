#include "my_vm.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define VA_BASE 0x40000000u   // base of our simulated virtual region (non-zero)

// -----------------------------------------------------------------------------
// Global Declarations
// -----------------------------------------------------------------------------
struct tlb tlbGlobal[TLB_ENTRIES];
struct tlb tlb_store;

static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses  = 0;

void    *phys_mem     = NULL;  // simulated physical memory buffer
uint8_t *phys_bitmap  = NULL;  // 1 bit per physical page
uint8_t *virt_bitmap  = NULL;  // 1 bit per virtual page
pde_t   *pgdir        = NULL;  // page directory (root page table)

static bool vm_initialized = false;
static pthread_mutex_t vm_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tlb_lock = PTHREAD_MUTEX_INITIALIZER;

// bitmap helpers
#define BIT_SET(bitmap, i)   ( (bitmap)[(i) / 8] |=  (1u << ((i) % 8)) )
#define BIT_CLEAR(bitmap, i) ( (bitmap)[(i) / 8] &= ~(1u << ((i) % 8)) )
#define BIT_TEST(bitmap, i)  ( (bitmap)[(i) / 8] &   (1u << ((i) % 8)) )

// present bit
#define PTE_PRESENT 0x1u

// -----------------------------------------------------------------------------
// Helper: Initialize VM
// -----------------------------------------------------------------------------
static void ensure_vm_init(void)
{
    if (vm_initialized)
        return;

    pthread_mutex_lock(&vm_lock);
    if (!vm_initialized)
    {
        // allocate simulated physical memory
        phys_mem = calloc(1, MEMSIZE);
        if (!phys_mem)
        {
            fprintf(stderr, "OOM phys_mem\n");
            exit(1);
        }

        // physical bitmap: 1 bit per physical page
        uint32_t num_phys_pages = MEMSIZE / PGSIZE;
        phys_bitmap = calloc((num_phys_pages + 7) / 8, 1);
        if (!phys_bitmap)
        {
            fprintf(stderr, "OOM phys_bitmap\n");
            exit(1);
        }

        // virtual bitmap: 1 bit per virtual page in MAX_MEMSIZE
        uint32_t num_virt_pages = (uint32_t)(MAX_MEMSIZE / PGSIZE);
        virt_bitmap = calloc((num_virt_pages + 7) / 8, 1);
        if (!virt_bitmap)
        {
            fprintf(stderr, "OOM virt_bitmap\n");
            exit(1);
        }

        // page directory: 1024 entries (10 bits)
        pgdir = calloc(1024, sizeof(pde_t));
        if (!pgdir)
        {
            fprintf(stderr, "OOM pgdir\n");
            exit(1);
        }

        // init TLB
        for (int i = 0; i < TLB_ENTRIES; i++)
        {
            tlbGlobal[i].vpn       = 0;
            tlbGlobal[i].pfn       = 0;
            tlbGlobal[i].valid     = false;
            tlbGlobal[i].last_used = 0;
        }
        tlb_lookups = 0;
        tlb_misses  = 0;

        vm_initialized = true;
    }
    pthread_mutex_unlock(&vm_lock);
}

// Provided API symbol – we just delegate to our init.
void set_physical_mem(void)
{
    ensure_vm_init();
}

// -----------------------------------------------------------------------------
// TLB
// -----------------------------------------------------------------------------
int TLB_add(void *va, void *pa)
{
    if (!pa)
        return -1;

    vaddr32_t v   = VA2U(va);
    vaddr32_t vpn = v >> PFN_SHIFT;
    vaddr32_t pfn = ((uintptr_t)pa - (uintptr_t)phys_mem) >> PFN_SHIFT;

    pthread_mutex_lock(&tlb_lock);

    int empty = -1;
    for (int i = 0; i < TLB_ENTRIES; i++)
    {
        if (!tlbGlobal[i].valid)
        {
            empty = i;
            break;
        }
    }

    int idx;
    if (empty >= 0)
    {
        idx = empty;
    }
    else
    {
        // naive LRU: pick smallest last_used
        idx = 0;
        uint64_t min = tlbGlobal[0].last_used;
        for (int i = 1; i < TLB_ENTRIES; i++)
        {
            if (tlbGlobal[i].last_used < min)
            {
                min = tlbGlobal[i].last_used;
                idx = i;
            }
        }
    }

    tlbGlobal[idx].vpn       = vpn;
    tlbGlobal[idx].pfn       = pfn;
    tlbGlobal[idx].valid     = true;
    tlbGlobal[idx].last_used = ++tlb_lookups;

    pthread_mutex_unlock(&tlb_lock);
    return 0;
}

pte_t *TLB_check(void *va)
{
    // va can be 0x40000000, etc. – no null check here.
    vaddr32_t v   = VA2U(va);
    vaddr32_t vpn = v >> PFN_SHIFT;

    pthread_mutex_lock(&tlb_lock);
    for (int i = 0; i < TLB_ENTRIES; i++)
    {
        if (tlbGlobal[i].valid && tlbGlobal[i].vpn == vpn)
        {
            tlbGlobal[i].last_used = ++tlb_lookups;
            void *page_base = (uint8_t *)phys_mem + ((uintptr_t)tlbGlobal[i].pfn << PFN_SHIFT);
            pthread_mutex_unlock(&tlb_lock);
            // return page base (we abuse type as pte_t*)
            return (pte_t *)page_base;
        }
    }
    tlb_misses++;
    pthread_mutex_unlock(&tlb_lock);
    return NULL;
}

void print_TLB_missrate(void)
{
    double rate = (tlb_lookups == 0) ? 0.0 : (double)tlb_misses / (double)tlb_lookups;
    fprintf(stderr, "TLB miss rate=%lf\n", rate);
}

// -----------------------------------------------------------------------------
// Helper: allocate a free physical frame
// -----------------------------------------------------------------------------
static int alloc_phys_frame(void)
{
    uint32_t num_phys_pages = MEMSIZE / PGSIZE;

    for (uint32_t i = 0; i < num_phys_pages; i++)
    {
        if (!BIT_TEST(phys_bitmap, i))
        {
            BIT_SET(phys_bitmap, i);
            return (int)i;
        }
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Translate VA -> PA (returns page *base* as pte_t*)
// -----------------------------------------------------------------------------
pte_t *translate(pde_t *pgdir_root, void *va)
{
    if (!pgdir_root)
        return NULL;

    ensure_vm_init();

    // First check TLB
    pte_t *tlb_page_base = TLB_check(va);
    if (tlb_page_base)
        return tlb_page_base; // page base

    vaddr32_t vaddr = VA2U(va);

    uint32_t pd_index = PDX(vaddr);
    uint32_t pt_index = PTX(vaddr);

    pde_t pde = pgdir_root[pd_index];
    if (!(pde & PTE_PRESENT))
        return NULL;

    uint32_t pt_frame = pde >> PFN_SHIFT;
    pte_t *pt = (pte_t *)((uint8_t *)phys_mem + ((uintptr_t)pt_frame << PFN_SHIFT));

    pte_t pte = pt[pt_index];
    if (!(pte & PTE_PRESENT))
        return NULL;

    uint32_t data_frame = pte >> PFN_SHIFT;
    void *page_base = (uint8_t *)phys_mem + ((uintptr_t)data_frame << PFN_SHIFT);

    // Add to TLB
    TLB_add(va, page_base);

    // Return page base
    return (pte_t *)page_base;
}

// -----------------------------------------------------------------------------
// Map a single page: VA page -> PA page
// -----------------------------------------------------------------------------
int map_page(pde_t *pgdir_root, void *va, void *pa)
{
    if (!pgdir_root || !va || !pa)
        return -1;

    ensure_vm_init();

    vaddr32_t vaddr = VA2U(va);

    // IMPORTANT: for physical address alignment, use offset from phys_mem
    uintptr_t p_raw  = (uintptr_t)pa;
    uintptr_t p_off  = p_raw - (uintptr_t)phys_mem;  // simulated physical offset

    if ((vaddr & OFFMASK) || (p_off & OFFMASK))
        return -1; // both VA and simulated PA must be page-aligned

    uint32_t pd_index = PDX(vaddr);
    uint32_t pt_index = PTX(vaddr);

    pthread_mutex_lock(&vm_lock);

    pde_t pde = pgdir_root[pd_index];
    pte_t *pt = NULL;

    if (!(pde & PTE_PRESENT))
    {
        // allocate page table in physical memory
        int pt_frame = alloc_phys_frame();
        if (pt_frame < 0)
        {
            pthread_mutex_unlock(&vm_lock);
            return -1;
        }

        pt = (pte_t *)((uint8_t *)phys_mem + ((uintptr_t)pt_frame << PFN_SHIFT));
        memset(pt, 0, PGSIZE);

        pgdir_root[pd_index] = ((uintptr_t)pt_frame << PFN_SHIFT) | PTE_PRESENT;
    }
    else
    {
        uint32_t pt_frame = pgdir_root[pd_index] >> PFN_SHIFT;
        pt = (pte_t *)((uint8_t *)phys_mem + ((uintptr_t)pt_frame << PFN_SHIFT));
    }

    if (pt[pt_index] & PTE_PRESENT)
    {
        pthread_mutex_unlock(&vm_lock);
        return -1; // already mapped
    }

    // frame index is offset / PGSIZE
    uint32_t frame_index = (uint32_t)(p_off / PGSIZE);
    pt[pt_index] = ((uintptr_t)frame_index << PFN_SHIFT) | PTE_PRESENT;

    pthread_mutex_unlock(&vm_lock);
    return 0;
}

// -----------------------------------------------------------------------------
// Get Next Available Virtual Pages
// -----------------------------------------------------------------------------
void *get_next_avail(int num_pages)
{
    ensure_vm_init();
    pthread_mutex_lock(&vm_lock);

    int max_pages = (int)(MAX_MEMSIZE / PGSIZE);
    int start = -1;

    for (int i = 0; i <= max_pages - num_pages; i++)
    {
        bool free = true;
        for (int j = 0; j < num_pages; j++)
        {
            if (BIT_TEST(virt_bitmap, i + j))
            {
                free = false;
                break;
            }
        }
        if (free)
        {
            start = i;
            break;
        }
    }

    if (start < 0)
    {
        pthread_mutex_unlock(&vm_lock);
        return NULL;
    }

    for (int j = 0; j < num_pages; j++)
        BIT_SET(virt_bitmap, start + j);

    vaddr32_t va32 = VA_BASE + (vaddr32_t)start * PGSIZE;

    pthread_mutex_unlock(&vm_lock);
    return U2VA(va32);
}

// -----------------------------------------------------------------------------
// Allocation / Free
// -----------------------------------------------------------------------------
void *n_malloc(unsigned int num_bytes)
{
    ensure_vm_init();

    if (num_bytes == 0)
        return NULL;

    int num_pages = (num_bytes + PGSIZE - 1) / PGSIZE;

    void *va_base = get_next_avail(num_pages);
    if (!va_base)
        return NULL;

    int *allocated_frames = (int *)malloc(sizeof(int) * num_pages);
    if (!allocated_frames)
        return NULL;
    for (int i = 0; i < num_pages; i++)
        allocated_frames[i] = -1;

    for (int i = 0; i < num_pages; i++)
    {
        pthread_mutex_lock(&vm_lock);
        int frame = alloc_phys_frame();
        pthread_mutex_unlock(&vm_lock);

        if (frame < 0)
        {
            // rollback phys frames
            for (int k = 0; k < i; k++)
            {
                if (allocated_frames[k] >= 0)
                {
                    pthread_mutex_lock(&vm_lock);
                    BIT_CLEAR(phys_bitmap, allocated_frames[k]);
                    pthread_mutex_unlock(&vm_lock);
                }
            }
            free(allocated_frames);
            return NULL;
        }

        allocated_frames[i] = frame;

        vaddr32_t page_va32 = VA2U(va_base) + (vaddr32_t)i * PGSIZE;
        void *page_va = U2VA(page_va32);
        void *page_pa = (uint8_t *)phys_mem + ((uintptr_t)frame << PFN_SHIFT);

        if (map_page(pgdir, page_va, page_pa) != 0)
        {
            // rollback frames
            for (int k = 0; k <= i; k++)
            {
                if (allocated_frames[k] >= 0)
                {
                    pthread_mutex_lock(&vm_lock);
                    BIT_CLEAR(phys_bitmap, allocated_frames[k]);
                    pthread_mutex_unlock(&vm_lock);
                }
            }
            free(allocated_frames);
            return NULL;
        }
    }

    free(allocated_frames);
    return va_base;
}

void n_free(void *va, int size)
{
    if (!va || size <= 0)
        return;

    ensure_vm_init();

    int num_pages = (size + PGSIZE - 1) / PGSIZE;
    vaddr32_t base_va32 = VA2U(va);

    for (int i = 0; i < num_pages; i++)
    {
        vaddr32_t curr_va32 = base_va32 + (vaddr32_t)i * PGSIZE;

        uint32_t pd_index = PDX(curr_va32);
        uint32_t pt_index = PTX(curr_va32);

        pde_t pde = pgdir[pd_index];
        if (!(pde & PTE_PRESENT))
            continue;

        uint32_t pt_frame = pde >> PFN_SHIFT;
        pte_t *pt = (pte_t *)((uint8_t *)phys_mem + ((uintptr_t)pt_frame << PFN_SHIFT));

        pte_t pte = pt[pt_index];
        if (!(pte & PTE_PRESENT))
            continue;

        uint32_t frame = pte >> PFN_SHIFT;

        // free physical frame
        pthread_mutex_lock(&vm_lock);
        BIT_CLEAR(phys_bitmap, frame);
        pthread_mutex_unlock(&vm_lock);

        // free virtual page bit
        int virt_index = (int)((curr_va32 - VA_BASE) / PGSIZE);
        if (virt_index >= 0)
        {
            pthread_mutex_lock(&vm_lock);
            BIT_CLEAR(virt_bitmap, virt_index);
            pthread_mutex_unlock(&vm_lock);
        }

        // clear PTE
        pt[pt_index] = 0;

        // invalidate TLB
        pthread_mutex_lock(&tlb_lock);
        uint32_t vpn = curr_va32 >> PFN_SHIFT;
        for (int j = 0; j < TLB_ENTRIES; j++)
        {
            if (tlbGlobal[j].valid && tlbGlobal[j].vpn == vpn)
                tlbGlobal[j].valid = false;
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

    uintptr_t offset = 0;
    while (offset < (uintptr_t)size)
    {
        vaddr32_t curr_va32 = VA2U(va) + (vaddr32_t)offset;
        void *curr_va = U2VA(curr_va32);

        // translate returns page base
        void *page_base = (void *)translate(pgdir, curr_va);
        if (!page_base)
            return -1;

        uintptr_t page_off   = curr_va32 & OFFMASK;
        int bytes_in_page    = (int)(PGSIZE - page_off);
        int remaining        = (int)size - (int)offset;
        int bytes_to_copy    = (remaining < bytes_in_page) ? remaining : bytes_in_page;

        memcpy((uint8_t *)page_base + page_off,
               (uint8_t *)val + offset,
               bytes_to_copy);

        offset += (uintptr_t)bytes_to_copy;
    }
    return 0;
}

void get_data(void *va, void *val, int size)
{
    if (!va || !val || size <= 0)
        return;

    ensure_vm_init();

    uintptr_t offset = 0;
    while (offset < (uintptr_t)size)
    {
        vaddr32_t curr_va32 = VA2U(va) + (vaddr32_t)offset;
        void *curr_va = U2VA(curr_va32);

        void *page_base = (void *)translate(pgdir, curr_va);
        if (!page_base)
            return;

        uintptr_t page_off   = curr_va32 & OFFMASK;
        int bytes_in_page    = (int)(PGSIZE - page_off);
        int remaining        = (int)size - (int)offset;
        int bytes_to_copy    = (remaining < bytes_in_page) ? remaining : bytes_in_page;

        memcpy((uint8_t *)val + offset,
               (uint8_t *)page_base + page_off,
               bytes_to_copy);

        offset += (uintptr_t)bytes_to_copy;
    }
}

// -----------------------------------------------------------------------------
// Matrix Multiplication
// -----------------------------------------------------------------------------
void mat_mult(void *mat1, void *mat2, int size, void *answer)
{
    for (int i = 0; i < size; i++)
    {
        for (int j = 0; j < size; j++)
        {
            int c_val = 0;
            for (int k = 0; k < size; k++)
            {
                int a = 0, b = 0;

                vaddr32_t a_va32 =
                    VA2U(mat1) + (vaddr32_t)((i * size + k) * (int)sizeof(int));
                vaddr32_t b_va32 =
                    VA2U(mat2) + (vaddr32_t)((k * size + j) * (int)sizeof(int));

                get_data(U2VA(a_va32), &a, sizeof(int));
                get_data(U2VA(b_va32), &b, sizeof(int));

                c_val += a * b;
            }

            vaddr32_t c_va32 =
                VA2U(answer) + (vaddr32_t)((i * size + j) * (int)sizeof(int));
            put_data(U2VA(c_va32), &c_val, sizeof(int));
        }
    }
}
