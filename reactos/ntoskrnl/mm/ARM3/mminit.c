/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/mm/ARM3/mminit.c
 * PURPOSE:         ARM Memory Manager Initialization
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include "miarm.h"

extern KEVENT ZeroPageThreadEvent;

/* GLOBALS ********************************************************************/

//
// These are all registry-configurable, but by default, the memory manager will
// figure out the most appropriate values.
//
ULONG MmMaximumNonPagedPoolPercent;
SIZE_T MmSizeOfNonPagedPoolInBytes;
SIZE_T MmMaximumNonPagedPoolInBytes;

/* Some of the same values, in pages */
PFN_NUMBER MmMaximumNonPagedPoolInPages;

//
// These numbers describe the discrete equation components of the nonpaged
// pool sizing algorithm.
//
// They are described on http://support.microsoft.com/default.aspx/kb/126402/ja
// along with the algorithm that uses them, which is implemented later below.
//
SIZE_T MmMinimumNonPagedPoolSize = 256 * 1024;
ULONG MmMinAdditionNonPagedPoolPerMb = 32 * 1024;
SIZE_T MmDefaultMaximumNonPagedPool = 1024 * 1024;
ULONG MmMaxAdditionNonPagedPoolPerMb = 400 * 1024;

//
// The memory layout (and especially variable names) of the NT kernel mode
// components can be a bit hard to twig, especially when it comes to the non
// paged area.
//
// There are really two components to the non-paged pool:
//
// - The initial nonpaged pool, sized dynamically up to a maximum.
// - The expansion nonpaged pool, sized dynamically up to a maximum.
//
// The initial nonpaged pool is physically continuous for performance, and
// immediately follows the PFN database, typically sharing the same PDE. It is
// a very small resource (32MB on a 1GB system), and capped at 128MB.
//
// Right now we call this the "ARM³ Nonpaged Pool" and it begins somewhere after
// the PFN database (which starts at 0xB0000000).
//
// The expansion nonpaged pool, on the other hand, can grow much bigger (400MB
// for a 1GB system). On ARM³ however, it is currently capped at 128MB.
//
// The address where the initial nonpaged pool starts is aptly named
// MmNonPagedPoolStart, and it describes a range of MmSizeOfNonPagedPoolInBytes
// bytes.
//
// Expansion nonpaged pool starts at an address described by the variable called
// MmNonPagedPoolExpansionStart, and it goes on for MmMaximumNonPagedPoolInBytes
// minus MmSizeOfNonPagedPoolInBytes bytes, always reaching MmNonPagedPoolEnd
// (because of the way it's calculated) at 0xFFBE0000.
//
// Initial nonpaged pool is allocated and mapped early-on during boot, but what
// about the expansion nonpaged pool? It is instead composed of special pages
// which belong to what are called System PTEs. These PTEs are the matter of a
// later discussion, but they are also considered part of the "nonpaged" OS, due
// to the fact that they are never paged out -- once an address is described by
// a System PTE, it is always valid, until the System PTE is torn down.
//
// System PTEs are actually composed of two "spaces", the system space proper,
// and the nonpaged pool expansion space. The latter, as we've already seen,
// begins at MmNonPagedPoolExpansionStart. Based on the number of System PTEs
// that the system will support, the remaining address space below this address
// is used to hold the system space PTEs. This address, in turn, is held in the
// variable named MmNonPagedSystemStart, which itself is never allowed to go
// below 0xEB000000 (thus creating an upper bound on the number of System PTEs).
//
// This means that 330MB are reserved for total nonpaged system VA, on top of
// whatever the initial nonpaged pool allocation is.
//
// The following URLs, valid as of April 23rd, 2008, support this evidence:
//
// http://www.cs.miami.edu/~burt/journal/NT/memory.html
// http://www.ditii.com/2007/09/28/windows-memory-management-x86-virtual-address-space/
//
PVOID MmNonPagedSystemStart;
PVOID MmNonPagedPoolStart;
PVOID MmNonPagedPoolExpansionStart;
PVOID MmNonPagedPoolEnd = MI_NONPAGED_POOL_END;

//
// This is where paged pool starts by default
//
PVOID MmPagedPoolStart = MI_PAGED_POOL_START;
PVOID MmPagedPoolEnd;

//
// And this is its default size
//
SIZE_T MmSizeOfPagedPoolInBytes = MI_MIN_INIT_PAGED_POOLSIZE;
PFN_NUMBER MmSizeOfPagedPoolInPages = MI_MIN_INIT_PAGED_POOLSIZE / PAGE_SIZE;

//
// Session space starts at 0xBFFFFFFF and grows downwards
// By default, it includes an 8MB image area where we map win32k and video card
// drivers, followed by a 4MB area containing the session's working set. This is
// then followed by a 20MB mapped view area and finally by the session's paged
// pool, by default 16MB.
//
// On a normal system, this results in session space occupying the region from
// 0xBD000000 to 0xC0000000
//
// See miarm.h for the defines that determine the sizing of this region. On an
// NT system, some of these can be configured through the registry, but we don't
// support that yet.
//
PVOID MiSessionSpaceEnd;    // 0xC0000000
PVOID MiSessionImageEnd;    // 0xC0000000
PVOID MiSessionImageStart;  // 0xBF800000
PVOID MiSessionViewStart;   // 0xBE000000
PVOID MiSessionPoolEnd;     // 0xBE000000
PVOID MiSessionPoolStart;   // 0xBD000000
PVOID MmSessionBase;        // 0xBD000000
SIZE_T MmSessionSize;
SIZE_T MmSessionViewSize;
SIZE_T MmSessionPoolSize;
SIZE_T MmSessionImageSize;

/*
 * These are the PTE addresses of the boundaries carved out above
 */
PMMPTE MiSessionImagePteStart;
PMMPTE MiSessionImagePteEnd;
PMMPTE MiSessionBasePte;
PMMPTE MiSessionLastPte;

//
// The system view space, on the other hand, is where sections that are memory
// mapped into "system space" end up.
//
// By default, it is a 16MB region.
//
PVOID MiSystemViewStart;
SIZE_T MmSystemViewSize;

#if (_MI_PAGING_LEVELS == 2)
//
// A copy of the system page directory (the page directory associated with the
// System process) is kept (double-mapped) by the manager in order to lazily
// map paged pool PDEs into external processes when they fault on a paged pool
// address.
//
PFN_NUMBER MmSystemPageDirectory[PD_COUNT];
PMMPDE MmSystemPagePtes;
#endif

//
// The system cache starts right after hyperspace. The first few pages are for
// keeping track of the system working set list.
//
// This should be 0xC0C00000 -- the cache itself starts at 0xC1000000
//
PMMWSL MmSystemCacheWorkingSetList = MI_SYSTEM_CACHE_WS_START;

//
// Windows NT seems to choose between 7000, 11000 and 50000
// On systems with more than 32MB, this number is then doubled, and further
// aligned up to a PDE boundary (4MB).
//
ULONG_PTR MmNumberOfSystemPtes;

//
// This is how many pages the PFN database will take up
// In Windows, this includes the Quark Color Table, but not in ARM³
//
PFN_NUMBER MxPfnAllocation;

//
// Unlike the old ReactOS Memory Manager, ARM³ (and Windows) does not keep track
// of pages that are not actually valid physical memory, such as ACPI reserved
// regions, BIOS address ranges, or holes in physical memory address space which
// could indicate device-mapped I/O memory.
//
// In fact, the lack of a PFN entry for a page usually indicates that this is
// I/O space instead.
//
// A bitmap, called the PFN bitmap, keeps track of all page frames by assigning
// a bit to each. If the bit is set, then the page is valid physical RAM.
//
RTL_BITMAP MiPfnBitMap;

//
// This structure describes the different pieces of RAM-backed address space
//
PPHYSICAL_MEMORY_DESCRIPTOR MmPhysicalMemoryBlock;

//
// This is where we keep track of the most basic physical layout markers
//
PFN_NUMBER MmNumberOfPhysicalPages, MmHighestPhysicalPage, MmLowestPhysicalPage = -1;

//
// The total number of pages mapped by the boot loader, which include the kernel
// HAL, boot drivers, registry, NLS files and other loader data structures is
// kept track of here. This depends on "LoaderPagesSpanned" being correct when
// coming from the loader.
//
// This number is later aligned up to a PDE boundary.
//
SIZE_T MmBootImageSize;

//
// These three variables keep track of the core separation of address space that
// exists between kernel mode and user mode.
//
ULONG_PTR MmUserProbeAddress;
PVOID MmHighestUserAddress;
PVOID MmSystemRangeStart;

/* And these store the respective highest PTE/PDE address */
PMMPTE MiHighestUserPte;
PMMPDE MiHighestUserPde;
#if (_MI_PAGING_LEVELS >= 3)
/* We need the highest PPE and PXE addresses */
#endif

/* These variables define the system cache address space */
PVOID MmSystemCacheStart;
PVOID MmSystemCacheEnd;
MMSUPPORT MmSystemCacheWs;

//
// This is where hyperspace ends (followed by the system cache working set)
//
PVOID MmHyperSpaceEnd;

//
// Page coloring algorithm data
//
ULONG MmSecondaryColors;
ULONG MmSecondaryColorMask;

//
// Actual (registry-configurable) size of a GUI thread's stack
//
ULONG MmLargeStackSize = KERNEL_LARGE_STACK_SIZE;

//
// Before we have a PFN database, memory comes straight from our physical memory
// blocks, which is nice because it's guaranteed contiguous and also because once
// we take a page from here, the system doesn't see it anymore.
// However, once the fun is over, those pages must be re-integrated back into
// PFN society life, and that requires us keeping a copy of the original layout
// so that we can parse it later.
//
PMEMORY_ALLOCATION_DESCRIPTOR MxFreeDescriptor;
MEMORY_ALLOCATION_DESCRIPTOR MxOldFreeDescriptor;

/*
 * For each page's worth bytes of L2 cache in a given set/way line, the zero and
 * free lists are organized in what is called a "color".
 *
 * This array points to the two lists, so it can be thought of as a multi-dimensional
 * array of MmFreePagesByColor[2][MmSecondaryColors]. Since the number is dynamic,
 * we describe the array in pointer form instead.
 *
 * On a final note, the color tables themselves are right after the PFN database.
 */
C_ASSERT(FreePageList == 1);
PMMCOLOR_TABLES MmFreePagesByColor[FreePageList + 1];

/* An event used in Phase 0 before the rest of the system is ready to go */
KEVENT MiTempEvent;

/* All the events used for memory threshold notifications */
PKEVENT MiLowMemoryEvent;
PKEVENT MiHighMemoryEvent;
PKEVENT MiLowPagedPoolEvent;
PKEVENT MiHighPagedPoolEvent;
PKEVENT MiLowNonPagedPoolEvent;
PKEVENT MiHighNonPagedPoolEvent;

/* The actual thresholds themselves, in page numbers */
PFN_NUMBER MmLowMemoryThreshold;
PFN_NUMBER MmHighMemoryThreshold;
PFN_NUMBER MiLowPagedPoolThreshold;
PFN_NUMBER MiHighPagedPoolThreshold;
PFN_NUMBER MiLowNonPagedPoolThreshold;
PFN_NUMBER MiHighNonPagedPoolThreshold;

/*
 * This number determines how many free pages must exist, at minimum, until we
 * start trimming working sets and flushing modified pages to obtain more free
 * pages.
 *
 * This number changes if the system detects that this is a server product
 */
PFN_NUMBER MmMinimumFreePages = 26;

/*
 * This number indicates how many pages we consider to be a low limit of having
 * "plenty" of free memory.
 *
 * It is doubled on systems that have more than 63MB of memory
 */
PFN_NUMBER MmPlentyFreePages = 400;

/* These values store the type of system this is (small, med, large) and if server */
ULONG MmProductType;
MM_SYSTEMSIZE MmSystemSize;

/*
 * These values store the cache working set minimums and maximums, in pages
 *
 * The minimum value is boosted on systems with more than 24MB of RAM, and cut
 * down to only 32 pages on embedded (<24MB RAM) systems.
 *
 * An extra boost of 2MB is given on systems with more than 33MB of RAM.
 */
PFN_NUMBER MmSystemCacheWsMinimum = 288;
PFN_NUMBER MmSystemCacheWsMaximum = 350;

/* FIXME: Move to cache/working set code later */
BOOLEAN MmLargeSystemCache;

/*
 * This value determines in how many fragments/chunks the subsection prototype
 * PTEs should be allocated when mapping a section object. It is configurable in
 * the registry through the MapAllocationFragment parameter.
 *
 * The default is 64KB on systems with more than 1GB of RAM, 32KB on systems with
 * more than 256MB of RAM, and 16KB on systems with less than 256MB of RAM.
 *
 * The maximum it can be set to is 2MB, and the minimum is 4KB.
 */
SIZE_T MmAllocationFragment;

/*
 * These two values track how much virtual memory can be committed, and when
 * expansion should happen.
 */
 // FIXME: They should be moved elsewhere since it's not an "init" setting?
SIZE_T MmTotalCommitLimit;
SIZE_T MmTotalCommitLimitMaximum;

/* Internal setting used for debugging memory descriptors */
BOOLEAN MiDbgEnableMdDump =
#ifdef _ARM_
TRUE;
#else
FALSE;
#endif

SCHAR LocationByMemoryType[LoaderMaximum];
#define MiIsMemoryTypeInDatabase(t) (LocationByMemoryType[t] != -1)
#define MiIsMemoryTypeFree(t) (LocationByMemoryType[t] == FreePageList)

PFN_NUMBER MiNumberOfFreePages = 0;
PFN_NUMBER MiEarlyAllocCount = 0;
PFN_NUMBER MiEarlyAllocBase;
ULONG MiNumberDescriptors = 0;

/* PRIVATE FUNCTIONS **********************************************************/

VOID
NTAPI
MiScanMemoryDescriptors(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PLIST_ENTRY NextEntry;
    PMEMORY_ALLOCATION_DESCRIPTOR MdBlock;

    /* Setup memory locations */
    LocationByMemoryType[LoaderExceptionBlock] = ActiveAndValid;
    LocationByMemoryType[LoaderSystemBlock] = ActiveAndValid;
    LocationByMemoryType[LoaderFree] = FreePageList;
    LocationByMemoryType[LoaderBad] = BadPageList;
    LocationByMemoryType[LoaderLoadedProgram] = FreePageList;
    LocationByMemoryType[LoaderFirmwareTemporary] = FreePageList;
    LocationByMemoryType[LoaderFirmwarePermanent] = -1;
    LocationByMemoryType[LoaderOsloaderHeap] = ActiveAndValid;
    LocationByMemoryType[LoaderOsloaderStack] = FreePageList;
    LocationByMemoryType[LoaderSystemCode] = ActiveAndValid;
    LocationByMemoryType[LoaderHalCode] = ActiveAndValid;
    LocationByMemoryType[LoaderBootDriver] = ActiveAndValid;
    LocationByMemoryType[LoaderConsoleInDriver] = ActiveAndValid;
    LocationByMemoryType[LoaderConsoleOutDriver] = ActiveAndValid;
    LocationByMemoryType[LoaderStartupDpcStack] = ActiveAndValid;
    LocationByMemoryType[LoaderStartupKernelStack] = ActiveAndValid;
    LocationByMemoryType[LoaderStartupPanicStack] = ActiveAndValid;
    LocationByMemoryType[LoaderStartupPcrPage] = ActiveAndValid;
    LocationByMemoryType[LoaderStartupPdrPage] = ActiveAndValid;
    LocationByMemoryType[LoaderRegistryData] = ActiveAndValid;
    LocationByMemoryType[LoaderMemoryData] = ActiveAndValid;
    LocationByMemoryType[LoaderNlsData] = ActiveAndValid;
    LocationByMemoryType[LoaderSpecialMemory] = -1;
    LocationByMemoryType[LoaderBBTMemory] = -1;
    LocationByMemoryType[LoaderReserve] = ActiveAndValid;
    LocationByMemoryType[LoaderXIPRom] = ActiveAndValid;
    LocationByMemoryType[LoaderHALCachedMemory] = ActiveAndValid;
    LocationByMemoryType[LoaderLargePageFiller] = ActiveAndValid;
    LocationByMemoryType[LoaderErrorLogMemory] = ActiveAndValid;

    /* Loop the memory descriptors */
    for (NextEntry = LoaderBlock->MemoryDescriptorListHead.Flink;
         NextEntry != &LoaderBlock->MemoryDescriptorListHead;
         NextEntry = NextEntry->Flink)
    {
        /* Count descriptor */
        MiNumberDescriptors++;

        /* Get the descriptor */
        MdBlock = CONTAINING_RECORD(NextEntry,
                                    MEMORY_ALLOCATION_DESCRIPTOR,
                                    ListEntry);
        DPRINT("MD Type: %lx Base: %lx Count: %lx\n", 
               MdBlock->MemoryType, MdBlock->BasePage, MdBlock->PageCount);

        /* Skip memory that is not part of the database */
        if (!MiIsMemoryTypeInDatabase(MdBlock->MemoryType)) continue;

        /* Check if BURNMEM was used */
        if (MdBlock->MemoryType != LoaderBad)
        {
            /* Count this in the total of pages */
            MmNumberOfPhysicalPages += MdBlock->PageCount;
        }

        /* Update the lowest and highest page */
        MmLowestPhysicalPage = min(MmLowestPhysicalPage, MdBlock->BasePage);
        MmHighestPhysicalPage = max(MmHighestPhysicalPage, 
                                    MdBlock->BasePage + MdBlock->PageCount - 1);

        /* Check if this is free memory */
        if (MiIsMemoryTypeFree(MdBlock->MemoryType))
        {
            /* Count it too free pages */
            MiNumberOfFreePages += MdBlock->PageCount;

            /* Check if this is the largest memory descriptor */
            if (MdBlock->PageCount > MiEarlyAllocCount)
            {
                /* Use this one for early allocations */
                MxFreeDescriptor = MdBlock;
                MiEarlyAllocCount = MxFreeDescriptor->PageCount;
            }
        }
    }

    // Save original values of the free descriptor, since it'll be
    // altered by early allocations
    MxOldFreeDescriptor = *MxFreeDescriptor;
    MiEarlyAllocBase = MxFreeDescriptor->BasePage;
}

PFN_NUMBER
NTAPI
MiEarlyAllocPages(IN PFN_NUMBER PageCount)
{
    PFN_NUMBER Pfn;

    /* Make sure we have enough pages */
    if (PageCount > MiEarlyAllocCount)
    {
        /* Crash the system */
        KeBugCheckEx(INSTALL_MORE_MEMORY,
                     MmNumberOfPhysicalPages,
                     MiEarlyAllocCount,
                     MxFreeDescriptor->PageCount,
                     PageCount);
    }

    /* Use our lowest usable free pages */
    Pfn = MiEarlyAllocBase;
    MiEarlyAllocBase += PageCount;
    MiEarlyAllocCount -= PageCount;
    return Pfn;
}

VOID
NTAPI
INIT_FUNCTION
MiComputeColorInformation(VOID)
{
    ULONG L2Associativity;

    /* Check if no setting was provided already */
    if (!MmSecondaryColors)
    {
        /* Get L2 cache information */
        L2Associativity = KeGetPcr()->SecondLevelCacheAssociativity;

        /* The number of colors is the number of cache bytes by set/way */
        MmSecondaryColors = KeGetPcr()->SecondLevelCacheSize;
        if (L2Associativity) MmSecondaryColors /= L2Associativity;
    }

    /* Now convert cache bytes into pages */
    MmSecondaryColors >>= PAGE_SHIFT;
    if (!MmSecondaryColors)
    {
        /* If there was no cache data from the KPCR, use the default colors */
        MmSecondaryColors = MI_SECONDARY_COLORS;
    }
    else
    {
        /* Otherwise, make sure there aren't too many colors */
        if (MmSecondaryColors > MI_MAX_SECONDARY_COLORS)
        {
            /* Set the maximum */
            MmSecondaryColors = MI_MAX_SECONDARY_COLORS;
        }

        /* Make sure there aren't too little colors */
        if (MmSecondaryColors < MI_MIN_SECONDARY_COLORS)
        {
            /* Set the default */
            MmSecondaryColors = MI_SECONDARY_COLORS;
        }

        /* Finally make sure the colors are a power of two */
        if (MmSecondaryColors & (MmSecondaryColors - 1))
        {
            /* Set the default */
            MmSecondaryColors = MI_SECONDARY_COLORS;
        }
    }

    /* Compute the mask and store it */
    MmSecondaryColorMask = MmSecondaryColors - 1;
    KeGetCurrentPrcb()->SecondaryColorMask = MmSecondaryColorMask;
}

VOID
NTAPI
INIT_FUNCTION
MiInitializeColorTables(VOID)
{
    ULONG i;
    PMMPTE PointerPte, LastPte;
    MMPTE TempPte = ValidKernelPte;

    /* The color table starts after the ARM3 PFN database */
    MmFreePagesByColor[0] = (PMMCOLOR_TABLES)&MmPfnDatabase[MmHighestPhysicalPage + 1];

    /* Loop the PTEs. We have two color tables for each secondary color */
    PointerPte = MiAddressToPte(&MmFreePagesByColor[0][0]);
    LastPte = MiAddressToPte((ULONG_PTR)MmFreePagesByColor[0] +
                             (2 * MmSecondaryColors * sizeof(MMCOLOR_TABLES))
                             - 1);
    while (PointerPte <= LastPte)
    {
        /* Check for valid PTE */
        if (PointerPte->u.Hard.Valid == 0)
        {
            /* Get a page and map it */
            TempPte.u.Hard.PageFrameNumber = MiEarlyAllocPages(1);
            MI_WRITE_VALID_PTE(PointerPte, TempPte);

            /* Zero out the page */
            RtlZeroMemory(MiPteToAddress(PointerPte), PAGE_SIZE);
        }

        /* Next */
        PointerPte++;
    }

    /* Now set the address of the next list, right after this one */
    MmFreePagesByColor[1] = &MmFreePagesByColor[0][MmSecondaryColors];

    /* Now loop the lists to set them up */
    for (i = 0; i < MmSecondaryColors; i++)
    {
        /* Set both free and zero lists for each color */
        MmFreePagesByColor[ZeroedPageList][i].Flink = 0xFFFFFFFF;
        MmFreePagesByColor[ZeroedPageList][i].Blink = (PVOID)0xFFFFFFFF;
        MmFreePagesByColor[ZeroedPageList][i].Count = 0;
        MmFreePagesByColor[FreePageList][i].Flink = 0xFFFFFFFF;
        MmFreePagesByColor[FreePageList][i].Blink = (PVOID)0xFFFFFFFF;
        MmFreePagesByColor[FreePageList][i].Count = 0;
    }
}

VOID
NTAPI
INIT_FUNCTION
MiMapPfnDatabase(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG BasePage, PageCount, PageFrameIndex;
    PLIST_ENTRY ListEntry;
    PMEMORY_ALLOCATION_DESCRIPTOR MdBlock;
    PMMPTE PointerPte, LastPte;
    MMPTE TempPte = ValidKernelPte;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    /* Lock the PFN Database */
    OldIrql = KeAcquireQueuedSpinLock(LockQueuePfnLock);

    /* Loop the memory descriptors */
    for (ListEntry = LoaderBlock->MemoryDescriptorListHead.Flink;
         ListEntry != &LoaderBlock->MemoryDescriptorListHead;
         ListEntry = ListEntry->Flink)
    {
        /* Get the descriptor */
        MdBlock = CONTAINING_RECORD(ListEntry,
                                    MEMORY_ALLOCATION_DESCRIPTOR,
                                    ListEntry);

        /* Skip descriptors that are not part of the database */
        if (!MiIsMemoryTypeInDatabase(MdBlock->MemoryType)) continue;

        /* Use the descriptor's numbers */
        BasePage = MdBlock->BasePage;
        PageCount = MdBlock->PageCount;
        
        /* Get the PTEs for this range */
        PointerPte = MiAddressToPte(&MmPfnDatabase[BasePage]);
        LastPte = MiAddressToPte(((ULONG_PTR)&MmPfnDatabase[BasePage + PageCount]) - 1);
        DPRINT("MD Type: %lx Base: %lx Count: %lx\n", MdBlock->MemoryType, BasePage, PageCount);

        /* Loop them */
        while (PointerPte <= LastPte)
        {
            /* We'll only touch PTEs that aren't already valid */
            if (PointerPte->u.Hard.Valid == 0)
            {
                /* Use the next free page */
                TempPte.u.Hard.PageFrameNumber = MiEarlyAllocPages(1);

                /* Write out this PTE */
                MI_WRITE_VALID_PTE(PointerPte, TempPte);

                /* Zero this page */
                RtlZeroMemory(MiPteToAddress(PointerPte), PAGE_SIZE);
            }

            /* Next! */
            PointerPte++;
        }

        /* Skip the free descriptor, we'll handle it later */
        if (MdBlock == MxFreeDescriptor) continue;

        if (MdBlock->MemoryType == LoaderBad)
        {
            DPRINT1("You have damaged RAM modules. Stopping boot\n");
            ASSERT(FALSE);
        }

        /* Now check the descriptor type */
        if (MiIsMemoryTypeFree(MdBlock->MemoryType))
        {
            /* Get the last page of this descriptor. Note we loop backwards */
            PageFrameIndex = MdBlock->BasePage + PageCount - 1;
            Pfn1 = MiGetPfnEntry(PageFrameIndex);
            
            /*  */
            while (PageCount--)
            {
                /* Add it to the free list */
                Pfn1->u3.e1.CacheAttribute = MiNonCached;
                MiInsertPageInFreeList(PageFrameIndex);

                /* Go to the next page */
                Pfn1--;
                PageFrameIndex--;
            }
        }
        else if (MdBlock->MemoryType == LoaderXIPRom)
        {
            Pfn1 = MiGetPfnEntry(MdBlock->BasePage);
            while (PageCount--)
            {
                /* Make it a pseudo-I/O ROM mapping */
                Pfn1->PteAddress = 0;
                Pfn1->u1.Flink = 0;
                Pfn1->u2.ShareCount = 0;
                Pfn1->u3.e1.PageLocation = 0;
                Pfn1->u3.e1.CacheAttribute = MiNonCached;
                Pfn1->u3.e1.Rom = 1;
                Pfn1->u3.e1.PrototypePte = 1;
                Pfn1->u3.e2.ReferenceCount = 0;
                Pfn1->u4.InPageError = 0;
                Pfn1->u4.PteFrame = 0;
                
                /* Advance page structures */
                Pfn1++;
            }
        }
        else
        {
            Pfn1 = MiGetPfnEntry(MdBlock->BasePage);
            while (PageCount--)
            {
                /* Mark it as being in-use */
                Pfn1->u4.PteFrame = 0;
                Pfn1->PteAddress = 0;
                Pfn1->u2.ShareCount++;
                Pfn1->u3.e2.ReferenceCount = 1;
                Pfn1->u3.e1.PageLocation = ActiveAndValid;
                Pfn1->u3.e1.CacheAttribute = MiNonCached;
                
                /* Advance page structures */
                Pfn1++;
            }
        }
    }

    /* Now handle the remaininng pages from the free descriptor */
    PageFrameIndex = MiEarlyAllocBase + MiEarlyAllocCount - 1;
    Pfn1 = MiGetPfnEntry(PageFrameIndex);
    PageCount = MiEarlyAllocCount;

    while (PageCount--)
    {
        /* Add it to the free list */
        Pfn1->u3.e1.CacheAttribute = MiNonCached;
        MiInsertPageInFreeList(PageFrameIndex);

        /* Go to the next page */
        Pfn1--;
        PageFrameIndex--;
    }
    
    /* Release PFN database */
    KeReleaseQueuedSpinLock(LockQueuePfnLock, OldIrql);
}

static
VOID
MiSetupPfnForPageTable(
    PFN_NUMBER PageFrameIndex,
    PMMPTE PointerPte)
{
    PMMPFN Pfn;
    PMMPDE PointerPde;

    /* Get the pfn entry for this page */
    Pfn = MiGetPfnEntry(PageFrameIndex);

    /* Check if it's valid memory */
    if ((PageFrameIndex <= MmHighestPhysicalPage) &&
        (MmIsAddressValid(Pfn)) &&
        (MmIsAddressValid(Pfn + 1)))
    {
        /* Setup the PFN entry */
        Pfn->u1.WsIndex = 0;
        Pfn->u2.ShareCount++;
        Pfn->PteAddress = PointerPte;
        Pfn->OriginalPte = *PointerPte;
        Pfn->u3.e1.PageLocation = ActiveAndValid;
        Pfn->u3.e1.CacheAttribute = MiNonCached;
        Pfn->u3.e2.ReferenceCount = 1;
        Pfn->u4.PteFrame = PFN_FROM_PTE(MiAddressToPte(PointerPte));
    }

    /* Increase the shared count of the PFN entry for the PDE */
    PointerPde = MiAddressToPde(MiPteToAddress(PointerPte));
    Pfn = MiGetPfnEntry(PFN_FROM_PTE(PointerPde));
    ASSERT(Pfn != NULL);
    Pfn->u2.ShareCount++;
}

VOID
NTAPI
INIT_FUNCTION
MiBuildPfnDatabaseFromPages(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PVOID Address = NULL;
    PFN_NUMBER PageFrameIndex;
    PMMPDE PointerPde;
    PMMPTE PointerPte;
    ULONG k, l;
#if (_MI_PAGING_LEVELS >= 3)
    PMMPDE PointerPpe;
    ULONG j;
#endif
#if (_MI_PAGING_LEVELS == 4)
    PMMPDE PointerPxe;
    ULONG i;
#endif

#if (_MI_PAGING_LEVELS == 4)
    /* Loop all PXEs in the PML4 */
    PointerPxe = MiAddressToPxe(Address);
    for (i = 0; i < PXE_PER_PAGE; i++, PointerPxe++)
    {
        /* Skip invalid PXEs */
        if (!PointerPxe->u.Hard.Valid) continue;

        /* Handle the PFN */
        PageFrameIndex = PFN_FROM_PXE(PointerPxe);
        MiSetupPfnForPageTable(PageFrameIndex, PointerPxe);
        
        /* Get starting VA for this PXE */
        Address = MiPxeToAddress(PointerPxe);
#endif
#if (_MI_PAGING_LEVELS >= 3)
        /* Loop all PPEs in this PDP */
        PointerPpe = MiAddressToPpe(Address);
        for (j = 0; j < PPE_PER_PAGE; j++, PointerPpe++)
        {
            /* Skip invalid PPEs */
            if (!PointerPpe->u.Hard.Valid) continue;

            /* Handle the PFN */
            PageFrameIndex = PFN_FROM_PPE(PointerPpe);
            MiSetupPfnForPageTable(PageFrameIndex, PointerPpe);

            /* Get starting VA for this PPE */
            Address = MiPpeToAddress(PointerPpe);
#endif
            /* Loop all PDEs in this PD */
            PointerPde = MiAddressToPde(Address);
            for (k = 0; k < PDE_PER_PAGE; k++, PointerPde++)
            {
                /* Skip invalid PDEs */
                if (!PointerPde->u.Hard.Valid) continue;

                /* Handle the PFN */
                PageFrameIndex = PFN_FROM_PDE(PointerPde);
                MiSetupPfnForPageTable(PageFrameIndex, PointerPde);

                /* Get starting VA for this PDE */
                Address = MiPdeToAddress(PointerPde);

                /* Loop all PTEs in this PT */
                PointerPte = MiAddressToPte(Address);
                for (l = 0; l < PTE_PER_PAGE; l++, PointerPte++)
                {
                    /* Skip invalid PTEs */
                    if (!PointerPte->u.Hard.Valid) continue;
                   
                    /* Handle the PFN */
                    PageFrameIndex = PFN_FROM_PTE(PointerPte);
                    MiSetupPfnForPageTable(PageFrameIndex, PointerPte);
                }
            }
#if (_MI_PAGING_LEVELS >= 3)
        }
#endif
#if (_MI_PAGING_LEVELS == 4)
    }
#endif
}

VOID
NTAPI
INIT_FUNCTION
MiBuildPfnDatabaseZeroPage(VOID)
{
    PMMPFN Pfn1;
    PMMPDE PointerPde;

    /* Grab the lowest page and check if it has no real references */
    Pfn1 = MiGetPfnEntry(MmLowestPhysicalPage);
    if (!(MmLowestPhysicalPage) && !(Pfn1->u3.e2.ReferenceCount))
    {
        /* Make it a bogus page to catch errors */
        PointerPde = MiAddressToPde(0xFFFFFFFF);
        Pfn1->u4.PteFrame = PFN_FROM_PTE(PointerPde);
        Pfn1->PteAddress = (PMMPTE)PointerPde;
        Pfn1->u2.ShareCount++;
        Pfn1->u3.e2.ReferenceCount = 0xFFF0;
        Pfn1->u3.e1.PageLocation = ActiveAndValid;
        Pfn1->u3.e1.CacheAttribute = MiNonCached;
    }
}

VOID
NTAPI
INIT_FUNCTION
MiBuildPfnDatabaseSelf(VOID)
{
    PMMPTE PointerPte, LastPte;
    PMMPFN Pfn1;

    /* Loop the PFN database page */
    PointerPte = MiAddressToPte(MiGetPfnEntry(MmLowestPhysicalPage));
    LastPte = MiAddressToPte(MiGetPfnEntry(MmHighestPhysicalPage));
    while (PointerPte <= LastPte)
    {
        /* Make sure the page is valid */
        if (PointerPte->u.Hard.Valid == 1)
        {
            /* Get the PFN entry and just mark it referenced */
            Pfn1 = MiGetPfnEntry(PointerPte->u.Hard.PageFrameNumber);
            Pfn1->u2.ShareCount = 1;
            Pfn1->u3.e2.ReferenceCount = 1;
#if MI_TRACE_PFNS
            Pfn1->PfnUsage = MI_USAGE_PFN_DATABASE;
#endif
        }

        /* Next */
        PointerPte++;
    }
}

VOID
NTAPI
INIT_FUNCTION
MiInitializePfnDatabase(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Map the PFN database pages */
    MiMapPfnDatabase(LoaderBlock);
    
    /* Initialize the color tables */
    MiInitializeColorTables();

    /* Scan memory and start setting up PFN entries */
    MiBuildPfnDatabaseFromPages(LoaderBlock);

    /* Add the zero page */
    MiBuildPfnDatabaseZeroPage();

    /* Finally add the pages for the PFN database itself */ 
    MiBuildPfnDatabaseSelf();
}

VOID
NTAPI
INIT_FUNCTION
MiAdjustWorkingSetManagerParameters(IN BOOLEAN Client)
{
    /* This function needs to do more work, for now, we tune page minimums */

    /* Check for a system with around 64MB RAM or more */
    if (MmNumberOfPhysicalPages >= (63 * _1MB) / PAGE_SIZE)
    {
        /* Double the minimum amount of pages we consider for a "plenty free" scenario */
        MmPlentyFreePages *= 2;
    }
}

VOID
NTAPI
INIT_FUNCTION
MiNotifyMemoryEvents(VOID)
{
    /* Are we in a low-memory situation? */
    if (MmAvailablePages < MmLowMemoryThreshold)
    {
        /* Clear high, set low  */
        if (KeReadStateEvent(MiHighMemoryEvent)) KeClearEvent(MiHighMemoryEvent);
        if (!KeReadStateEvent(MiLowMemoryEvent)) KeSetEvent(MiLowMemoryEvent, 0, FALSE);
    }
    else if (MmAvailablePages < MmHighMemoryThreshold)
    {
        /* We are in between, clear both */
        if (KeReadStateEvent(MiHighMemoryEvent)) KeClearEvent(MiHighMemoryEvent);
        if (KeReadStateEvent(MiLowMemoryEvent)) KeClearEvent(MiLowMemoryEvent);
    }
    else
    {
        /* Clear low, set high  */
        if (KeReadStateEvent(MiLowMemoryEvent)) KeClearEvent(MiLowMemoryEvent);
        if (!KeReadStateEvent(MiHighMemoryEvent)) KeSetEvent(MiHighMemoryEvent, 0, FALSE);
    }
}

NTSTATUS
NTAPI
INIT_FUNCTION
MiCreateMemoryEvent(IN PUNICODE_STRING Name,
                    OUT PKEVENT *Event)
{
    PACL Dacl;
    HANDLE EventHandle;
    ULONG DaclLength;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SECURITY_DESCRIPTOR SecurityDescriptor;

    /* Create the SD */
    Status = RtlCreateSecurityDescriptor(&SecurityDescriptor,
                                         SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status)) return Status;

    /* One ACL with 3 ACEs, containing each one SID */
    DaclLength = sizeof(ACL) +
                 3 * sizeof(ACCESS_ALLOWED_ACE) +
                 RtlLengthSid(SeLocalSystemSid) +
                 RtlLengthSid(SeAliasAdminsSid) +
                 RtlLengthSid(SeWorldSid);

    /* Allocate space for the DACL */
    Dacl = ExAllocatePoolWithTag(PagedPool, DaclLength, 'lcaD');
    if (!Dacl) return STATUS_INSUFFICIENT_RESOURCES;

    /* Setup the ACL inside it */
    Status = RtlCreateAcl(Dacl, DaclLength, ACL_REVISION);
    if (!NT_SUCCESS(Status)) goto CleanUp;

    /* Add query rights for everyone */
    Status = RtlAddAccessAllowedAce(Dacl,
                                    ACL_REVISION,
                                    SYNCHRONIZE | EVENT_QUERY_STATE | READ_CONTROL,
                                    SeWorldSid);
    if (!NT_SUCCESS(Status)) goto CleanUp;

    /* Full rights for the admin */
    Status = RtlAddAccessAllowedAce(Dacl,
                                    ACL_REVISION,
                                    EVENT_ALL_ACCESS,
                                    SeAliasAdminsSid);
    if (!NT_SUCCESS(Status)) goto CleanUp;

    /* As well as full rights for the system */
    Status = RtlAddAccessAllowedAce(Dacl,
                                    ACL_REVISION,
                                    EVENT_ALL_ACCESS,
                                    SeLocalSystemSid);
    if (!NT_SUCCESS(Status)) goto CleanUp;

    /* Set this DACL inside the SD */
    Status = RtlSetDaclSecurityDescriptor(&SecurityDescriptor,
                                          TRUE,
                                          Dacl,
                                          FALSE);
    if (!NT_SUCCESS(Status)) goto CleanUp;

    /* Setup the event attributes, making sure it's a permanent one */
    InitializeObjectAttributes(&ObjectAttributes,
                               Name,
                               OBJ_KERNEL_HANDLE | OBJ_PERMANENT,
                               NULL,
                               &SecurityDescriptor);

    /* Create the event */
    Status = ZwCreateEvent(&EventHandle,
                           EVENT_ALL_ACCESS,
                           &ObjectAttributes,
                           NotificationEvent,
                           FALSE);
CleanUp:
    /* Free the DACL */
    ExFreePool(Dacl);

    /* Check if this is the success path */
    if (NT_SUCCESS(Status))
    {
        /* Add a reference to the object, then close the handle we had */
        Status = ObReferenceObjectByHandle(EventHandle,
                                           EVENT_MODIFY_STATE,
                                           ExEventObjectType,
                                           KernelMode,
                                           (PVOID*)Event,
                                           NULL);
        ZwClose (EventHandle);
    }

    /* Return status */
    return Status;
}

BOOLEAN
NTAPI
INIT_FUNCTION
MiInitializeMemoryEvents(VOID)
{
    UNICODE_STRING LowString = RTL_CONSTANT_STRING(L"\\KernelObjects\\LowMemoryCondition");
    UNICODE_STRING HighString = RTL_CONSTANT_STRING(L"\\KernelObjects\\HighMemoryCondition");
    UNICODE_STRING LowPagedPoolString = RTL_CONSTANT_STRING(L"\\KernelObjects\\LowPagedPoolCondition");
    UNICODE_STRING HighPagedPoolString = RTL_CONSTANT_STRING(L"\\KernelObjects\\HighPagedPoolCondition");
    UNICODE_STRING LowNonPagedPoolString = RTL_CONSTANT_STRING(L"\\KernelObjects\\LowNonPagedPoolCondition");
    UNICODE_STRING HighNonPagedPoolString = RTL_CONSTANT_STRING(L"\\KernelObjects\\HighNonPagedPoolCondition");
    NTSTATUS Status;

    /* Check if we have a registry setting */
    if (MmLowMemoryThreshold)
    {
        /* Convert it to pages */
        MmLowMemoryThreshold *= (_1MB / PAGE_SIZE);
    }
    else
    {
        /* The low memory threshold is hit when we don't consider that we have "plenty" of free pages anymore */
        MmLowMemoryThreshold = MmPlentyFreePages;

        /* More than one GB of memory? */
        if (MmNumberOfPhysicalPages > 0x40000)
        {
            /* Start at 32MB, and add another 16MB for each GB */
            MmLowMemoryThreshold = (32 * _1MB) / PAGE_SIZE;
            MmLowMemoryThreshold += ((MmNumberOfPhysicalPages - 0x40000) >> 7);
        }
        else if (MmNumberOfPhysicalPages > 0x8000)
        {
            /* For systems with > 128MB RAM, add another 4MB for each 128MB */
            MmLowMemoryThreshold += ((MmNumberOfPhysicalPages - 0x8000) >> 5);
        }

        /* Don't let the minimum threshold go past 64MB */
        MmLowMemoryThreshold = min(MmLowMemoryThreshold, (64 * _1MB) / PAGE_SIZE);
    }

    /* Check if we have a registry setting */
    if (MmHighMemoryThreshold)
    {
        /* Convert it into pages */
        MmHighMemoryThreshold *= (_1MB / PAGE_SIZE);
    }
    else
    {
        /* Otherwise, the default is three times the low memory threshold */
        MmHighMemoryThreshold = 3 * MmLowMemoryThreshold;
        ASSERT(MmHighMemoryThreshold > MmLowMemoryThreshold);
    }

    /* Make sure high threshold is actually higher than the low */
    MmHighMemoryThreshold = max(MmHighMemoryThreshold, MmLowMemoryThreshold);

    /* Create the memory events for all the thresholds */
    Status = MiCreateMemoryEvent(&LowString, &MiLowMemoryEvent);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = MiCreateMemoryEvent(&HighString, &MiHighMemoryEvent);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = MiCreateMemoryEvent(&LowPagedPoolString, &MiLowPagedPoolEvent);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = MiCreateMemoryEvent(&HighPagedPoolString, &MiHighPagedPoolEvent);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = MiCreateMemoryEvent(&LowNonPagedPoolString, &MiLowNonPagedPoolEvent);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = MiCreateMemoryEvent(&HighNonPagedPoolString, &MiHighNonPagedPoolEvent);
    if (!NT_SUCCESS(Status)) return FALSE;

    /* Now setup the pool events */
    MiInitializePoolEvents();

    /* Set the initial event state */
    MiNotifyMemoryEvents();
    return TRUE;
}

VOID
NTAPI
INIT_FUNCTION
MiAddHalIoMappings(VOID)
{
    PVOID BaseAddress;
    PMMPDE PointerPde;
    PMMPTE PointerPte;
    ULONG i, j, PdeCount;
    PFN_NUMBER PageFrameIndex;

    /* HAL Heap address -- should be on a PDE boundary */
    BaseAddress = (PVOID)0xFFC00000;
    //ASSERT(MiAddressToPteOffset(BaseAddress) == 0);

    /* Check how many PDEs the heap has */
    PointerPde = MiAddressToPde(BaseAddress);
    PdeCount = PDE_COUNT - MiGetPdeOffset(BaseAddress);
    for (i = 0; i < PdeCount; i++)
    {
        /* Does the HAL own this mapping? */
        if ((PointerPde->u.Hard.Valid == 1) &&
            (MI_IS_PAGE_LARGE(PointerPde) == FALSE))
        {
            /* Get the PTE for it and scan each page */
            PointerPte = MiAddressToPte(BaseAddress);
            for (j = 0 ; j < PTE_COUNT; j++)
            {
                /* Does the HAL own this page? */
                if (PointerPte->u.Hard.Valid == 1)
                {
                    /* Is the HAL using it for device or I/O mapped memory? */
                    PageFrameIndex = PFN_FROM_PTE(PointerPte);
                    if (!MiGetPfnEntry(PageFrameIndex))
                    {
                        /* FIXME: For PAT, we need to track I/O cache attributes for coherency */
                        DPRINT1("HAL I/O Mapping at %p is unsafe\n", BaseAddress);
                    }
                }

                /* Move to the next page */
                BaseAddress = (PVOID)((ULONG_PTR)BaseAddress + PAGE_SIZE);
                PointerPte++;
            }
        }
        else
        {
            /* Move to the next address */
            BaseAddress = (PVOID)((ULONG_PTR)BaseAddress + PDE_MAPPED_VA);
        }

        /* Move to the next PDE */
        PointerPde++;
    }
}

VOID
NTAPI
MmDumpArmPfnDatabase(IN BOOLEAN StatusOnly)
{
    ULONG i;
    PMMPFN Pfn1;
    PCHAR Consumer = "Unknown";
    KIRQL OldIrql;
    ULONG ActivePages = 0, FreePages = 0, OtherPages = 0;
#if MI_TRACE_PFNS
    ULONG UsageBucket[MI_USAGE_FREE_PAGE + 1] = {0};
    PCHAR MI_USAGE_TEXT[MI_USAGE_FREE_PAGE + 1] =
    {
        "Not set",
        "Paged Pool",
        "Nonpaged Pool",
        "Nonpaged Pool Ex",
        "Kernel Stack",
        "Kernel Stack Ex",
        "System PTE",
        "VAD",
        "PEB/TEB",
        "Section",
        "Page Table",
        "Page Directory",
        "Old Page Table",
        "Driver Page",
        "Contiguous Alloc",
        "MDL",
        "Demand Zero",
        "Zero Loop",
        "Cache",
        "PFN Database",
        "Boot Driver",
        "Initial Memory",
        "Free Page"
    };
#endif
    //
    // Loop the PFN database
    //
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    for (i = 0; i <= MmHighestPhysicalPage; i++)
    {
        Pfn1 = MiGetPfnEntry(i);
        if (!Pfn1) continue;
#if MI_TRACE_PFNS
        ASSERT(Pfn1->PfnUsage <= MI_USAGE_FREE_PAGE);
#endif
        //
        // Get the page location
        //
        switch (Pfn1->u3.e1.PageLocation)
        {
            case ActiveAndValid:

                Consumer = "Active and Valid";
                ActivePages++;
                break;

            case ZeroedPageList:

                Consumer = "Zero Page List";
                FreePages++;
                break;//continue;

            case FreePageList:

                Consumer = "Free Page List";
                FreePages++;
                break;//continue;

            default:

                Consumer = "Other (ASSERT!)";
                OtherPages++;
                break;
        }

#if MI_TRACE_PFNS
        /* Add into bucket */
        UsageBucket[Pfn1->PfnUsage]++;
#endif

        //
        // Pretty-print the page
        //
        if (!StatusOnly)
        DbgPrint("0x%08p:\t%20s\t(%04d.%04d)\t[%16s - %16s])\n",
                 i << PAGE_SHIFT,
                 Consumer,
                 Pfn1->u3.e2.ReferenceCount,
                 Pfn1->u2.ShareCount == LIST_HEAD ? 0xFFFF : Pfn1->u2.ShareCount,
#if MI_TRACE_PFNS
                 MI_USAGE_TEXT[Pfn1->PfnUsage],
                 Pfn1->ProcessName);
#else
                 "Page tracking",
                 "is disabled");
#endif
    }

    DbgPrint("Active:               %5d pages\t[%6d KB]\n", ActivePages,  (ActivePages    << PAGE_SHIFT) / 1024);
    DbgPrint("Free:                 %5d pages\t[%6d KB]\n", FreePages,    (FreePages      << PAGE_SHIFT) / 1024);
    DbgPrint("-----------------------------------------\n");
#if MI_TRACE_PFNS
    OtherPages = UsageBucket[MI_USAGE_BOOT_DRIVER];
    DbgPrint("Boot Images:          %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_DRIVER_PAGE];
    DbgPrint("System Drivers:       %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_PFN_DATABASE];
    DbgPrint("PFN Database:         %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_PAGE_TABLE] + UsageBucket[MI_USAGE_LEGACY_PAGE_DIRECTORY];
    DbgPrint("Page Tables:          %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_NONPAGED_POOL] + UsageBucket[MI_USAGE_NONPAGED_POOL_EXPANSION];
    DbgPrint("NonPaged Pool:        %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_PAGED_POOL];
    DbgPrint("Paged Pool:           %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_KERNEL_STACK] + UsageBucket[MI_USAGE_KERNEL_STACK_EXPANSION];
    DbgPrint("Kernel Stack:         %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_INIT_MEMORY];
    DbgPrint("Init Memory:          %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_SECTION];
    DbgPrint("Sections:             %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
    OtherPages = UsageBucket[MI_USAGE_CACHE];
    DbgPrint("Cache:                %5d pages\t[%6d KB]\n", OtherPages,   (OtherPages     << PAGE_SHIFT) / 1024);
#endif
    KeLowerIrql(OldIrql);
}

PPHYSICAL_MEMORY_DESCRIPTOR
NTAPI
INIT_FUNCTION
MmInitializeMemoryLimits(IN PLOADER_PARAMETER_BLOCK LoaderBlock,
                         IN PBOOLEAN IncludeType)
{
    PLIST_ENTRY NextEntry;
    ULONG Run = 0;
    PFN_NUMBER NextPage = -1, PageCount = 0;
    PPHYSICAL_MEMORY_DESCRIPTOR Buffer, NewBuffer;
    PMEMORY_ALLOCATION_DESCRIPTOR MdBlock;

    //
    // Allocate the maximum we'll ever need
    //
    Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                   sizeof(PHYSICAL_MEMORY_DESCRIPTOR) +
                                   sizeof(PHYSICAL_MEMORY_RUN) *
                                   (MiNumberDescriptors - 1),
                                   'lMmM');
    if (!Buffer) return NULL;

    //
    // For now that's how many runs we have
    //
    Buffer->NumberOfRuns = MiNumberDescriptors;

    //
    // Now loop through the descriptors again
    //
    NextEntry = LoaderBlock->MemoryDescriptorListHead.Flink;
    while (NextEntry != &LoaderBlock->MemoryDescriptorListHead)
    {
        //
        // Grab each one, and check if it's one we should include
        //
        MdBlock = CONTAINING_RECORD(NextEntry,
                                    MEMORY_ALLOCATION_DESCRIPTOR,
                                    ListEntry);
        if ((MdBlock->MemoryType < LoaderMaximum) &&
            (IncludeType[MdBlock->MemoryType]))
        {
            //
            // Add this to our running total
            //
            PageCount += MdBlock->PageCount;

            //
            // Check if the next page is described by the next descriptor
            //
            if (MdBlock->BasePage == NextPage)
            {
                //
                // Combine it into the same physical run
                //
                ASSERT(MdBlock->PageCount != 0);
                Buffer->Run[Run - 1].PageCount += MdBlock->PageCount;
                NextPage += MdBlock->PageCount;
            }
            else
            {
                //
                // Otherwise just duplicate the descriptor's contents
                //
                Buffer->Run[Run].BasePage = MdBlock->BasePage;
                Buffer->Run[Run].PageCount = MdBlock->PageCount;
                NextPage = Buffer->Run[Run].BasePage + Buffer->Run[Run].PageCount;

                //
                // And in this case, increase the number of runs
                //
                Run++;
            }
        }

        //
        // Try the next descriptor
        //
        NextEntry = MdBlock->ListEntry.Flink;
    }

    //
    // We should not have been able to go past our initial estimate
    //
    ASSERT(Run <= Buffer->NumberOfRuns);

    //
    // Our guess was probably exaggerated...
    //
    if (MiNumberDescriptors > Run)
    {
        //
        // Allocate a more accurately sized buffer
        //
        NewBuffer = ExAllocatePoolWithTag(NonPagedPool,
                                          sizeof(PHYSICAL_MEMORY_DESCRIPTOR) +
                                          sizeof(PHYSICAL_MEMORY_RUN) *
                                          (Run - 1),
                                          'lMmM');
        if (NewBuffer)
        {
            //
            // Copy the old buffer into the new, then free it
            //
            RtlCopyMemory(NewBuffer->Run,
                          Buffer->Run,
                          sizeof(PHYSICAL_MEMORY_RUN) * Run);
            ExFreePool(Buffer);

            //
            // Now use the new buffer
            //
            Buffer = NewBuffer;
        }
    }

    //
    // Write the final numbers, and return it
    //
    Buffer->NumberOfRuns = Run;
    Buffer->NumberOfPages = PageCount;
    return Buffer;
}

VOID
NTAPI
INIT_FUNCTION
MiBuildPagedPool(VOID)
{
    PMMPTE PointerPte;
    PMMPDE PointerPde;
    MMPTE TempPte = ValidKernelPte;
    MMPDE TempPde = ValidKernelPde;
    PFN_NUMBER PageFrameIndex;
    KIRQL OldIrql;
    ULONG Size, BitMapSize;
#if (_MI_PAGING_LEVELS == 2)
    //
    // Get the page frame number for the system page directory
    //
    PointerPte = MiAddressToPte(PDE_BASE);
    ASSERT(PD_COUNT == 1);
    MmSystemPageDirectory[0] = PFN_FROM_PTE(PointerPte);

    //
    // Allocate a system PTE which will hold a copy of the page directory
    //
    PointerPte = MiReserveSystemPtes(1, SystemPteSpace);
    ASSERT(PointerPte);
    MmSystemPagePtes = MiPteToAddress(PointerPte);

    //
    // Make this system PTE point to the system page directory.
    // It is now essentially double-mapped. This will be used later for lazy
    // evaluation of PDEs accross process switches, similarly to how the Global
    // page directory array in the old ReactOS Mm is used (but in a less hacky
    // way).
    //
    TempPte = ValidKernelPte;
    ASSERT(PD_COUNT == 1);
    TempPte.u.Hard.PageFrameNumber = MmSystemPageDirectory[0];
    MI_WRITE_VALID_PTE(PointerPte, TempPte);
#endif
    //
    // Let's get back to paged pool work: size it up.
    // By default, it should be twice as big as nonpaged pool.
    //
    MmSizeOfPagedPoolInBytes = 2 * MmMaximumNonPagedPoolInBytes;
    if (MmSizeOfPagedPoolInBytes > ((ULONG_PTR)MmNonPagedSystemStart -
                                    (ULONG_PTR)MmPagedPoolStart))
    {
        //
        // On the other hand, we have limited VA space, so make sure that the VA
        // for paged pool doesn't overflow into nonpaged pool VA. Otherwise, set
        // whatever maximum is possible.
        //
        MmSizeOfPagedPoolInBytes = (ULONG_PTR)MmNonPagedSystemStart -
                                   (ULONG_PTR)MmPagedPoolStart;
    }

    //
    // Get the size in pages and make sure paged pool is at least 32MB.
    //
    Size = MmSizeOfPagedPoolInBytes;
    if (Size < MI_MIN_INIT_PAGED_POOLSIZE) Size = MI_MIN_INIT_PAGED_POOLSIZE;
    Size = BYTES_TO_PAGES(Size);

    //
    // Now check how many PTEs will be required for these many pages.
    //
    Size = (Size + (1024 - 1)) / 1024;

    //
    // Recompute the page-aligned size of the paged pool, in bytes and pages.
    //
    MmSizeOfPagedPoolInBytes = Size * PAGE_SIZE * 1024;
    MmSizeOfPagedPoolInPages = MmSizeOfPagedPoolInBytes >> PAGE_SHIFT;

    //
    // Let's be really sure this doesn't overflow into nonpaged system VA
    //
    ASSERT((MmSizeOfPagedPoolInBytes + (ULONG_PTR)MmPagedPoolStart) <=
           (ULONG_PTR)MmNonPagedSystemStart);

    //
    // This is where paged pool ends
    //
    MmPagedPoolEnd = (PVOID)(((ULONG_PTR)MmPagedPoolStart +
                              MmSizeOfPagedPoolInBytes) - 1);

    //
    // So now get the PDE for paged pool and zero it out
    //
    PointerPde = MiAddressToPde(MmPagedPoolStart);

#if (_MI_PAGING_LEVELS >= 3)
    /* On these systems, there's no double-mapping, so instead, the PPE and PXEs
     * are setup to span the entire paged pool area, so there's no need for the
     * system PD */
     ASSERT(FALSE);
#endif

    RtlZeroMemory(PointerPde,
                  (1 + MiAddressToPde(MmPagedPoolEnd) - PointerPde) * sizeof(MMPDE));

    //
    // Next, get the first and last PTE
    //
    PointerPte = MiAddressToPte(MmPagedPoolStart);
    MmPagedPoolInfo.FirstPteForPagedPool = PointerPte;
    MmPagedPoolInfo.LastPteForPagedPool = MiAddressToPte(MmPagedPoolEnd);

    //
    // Lock the PFN database
    //
    OldIrql = KeAcquireQueuedSpinLock(LockQueuePfnLock);

    /* Allocate a page and map the first paged pool PDE */
    MI_SET_USAGE(MI_USAGE_PAGED_POOL);
    MI_SET_PROCESS2("Kernel");
    PageFrameIndex = MiRemoveZeroPage(0);
    TempPde.u.Hard.PageFrameNumber = PageFrameIndex;
    MI_WRITE_VALID_PDE(PointerPde, TempPde);
#if (_MI_PAGING_LEVELS >= 3)
    /* Use the PPE of MmPagedPoolStart that was setup above */
//    Bla = PFN_FROM_PTE(PpeAddress(MmPagedPool...));
    ASSERT(FALSE);
#else
    /* Do it this way */
//    Bla = MmSystemPageDirectory[(PointerPde - (PMMPTE)PDE_BASE) / PDE_COUNT]

    /* Initialize the PFN entry for it */
    MiInitializePfnForOtherProcess(PageFrameIndex,
                                   (PMMPTE)PointerPde,
                                   MmSystemPageDirectory[(PointerPde - (PMMPDE)PDE_BASE) / PDE_COUNT]);
#endif

    //
    // Release the PFN database lock
    //
    KeReleaseQueuedSpinLock(LockQueuePfnLock, OldIrql);

    //
    // We only have one PDE mapped for now... at fault time, additional PDEs
    // will be allocated to handle paged pool growth. This is where they'll have
    // to start.
    //
    MmPagedPoolInfo.NextPdeForPagedPoolExpansion = PointerPde + 1;

    //
    // We keep track of each page via a bit, so check how big the bitmap will
    // have to be (make sure to align our page count such that it fits nicely
    // into a 4-byte aligned bitmap.
    //
    // We'll also allocate the bitmap header itself part of the same buffer.
    //
    Size = Size * 1024;
    ASSERT(Size == MmSizeOfPagedPoolInPages);
    BitMapSize = Size;
    Size = sizeof(RTL_BITMAP) + (((Size + 31) / 32) * sizeof(ULONG));

    //
    // Allocate the allocation bitmap, which tells us which regions have not yet
    // been mapped into memory
    //
    MmPagedPoolInfo.PagedPoolAllocationMap = ExAllocatePoolWithTag(NonPagedPool,
                                                                   Size,
                                                                   '  mM');
    ASSERT(MmPagedPoolInfo.PagedPoolAllocationMap);

    //
    // Initialize it such that at first, only the first page's worth of PTEs is
    // marked as allocated (incidentially, the first PDE we allocated earlier).
    //
    RtlInitializeBitMap(MmPagedPoolInfo.PagedPoolAllocationMap,
                        (PULONG)(MmPagedPoolInfo.PagedPoolAllocationMap + 1),
                        BitMapSize);
    RtlSetAllBits(MmPagedPoolInfo.PagedPoolAllocationMap);
    RtlClearBits(MmPagedPoolInfo.PagedPoolAllocationMap, 0, 1024);

    //
    // We have a second bitmap, which keeps track of where allocations end.
    // Given the allocation bitmap and a base address, we can therefore figure
    // out which page is the last page of that allocation, and thus how big the
    // entire allocation is.
    //
    MmPagedPoolInfo.EndOfPagedPoolBitmap = ExAllocatePoolWithTag(NonPagedPool,
                                                                 Size,
                                                                 '  mM');
    ASSERT(MmPagedPoolInfo.EndOfPagedPoolBitmap);
    RtlInitializeBitMap(MmPagedPoolInfo.EndOfPagedPoolBitmap,
                        (PULONG)(MmPagedPoolInfo.EndOfPagedPoolBitmap + 1),
                        BitMapSize);

    //
    // Since no allocations have been made yet, there are no bits set as the end
    //
    RtlClearAllBits(MmPagedPoolInfo.EndOfPagedPoolBitmap);

    //
    // Initialize paged pool.
    //
    InitializePool(PagedPool, 0);

    /* Default low threshold of 30MB or one fifth of paged pool */
    MiLowPagedPoolThreshold = (30 * _1MB) >> PAGE_SHIFT;
    MiLowPagedPoolThreshold = min(MiLowPagedPoolThreshold, Size / 5);

    /* Default high threshold of 60MB or 25% */
    MiHighPagedPoolThreshold = (60 * _1MB) >> PAGE_SHIFT;
    MiHighPagedPoolThreshold = min(MiHighPagedPoolThreshold, (Size * 2) / 5);
    ASSERT(MiLowPagedPoolThreshold < MiHighPagedPoolThreshold);

    /* Setup the global session space */
    MiInitializeSystemSpaceMap(NULL);
}

VOID
NTAPI
INIT_FUNCTION
MiDbgDumpMemoryDescriptors(VOID)
{
    PLIST_ENTRY NextEntry;
    PMEMORY_ALLOCATION_DESCRIPTOR Md;
    ULONG TotalPages = 0;
    PCHAR
    MemType[] =
    {
        "ExceptionBlock    ",
        "SystemBlock       ",
        "Free              ",
        "Bad               ",
        "LoadedProgram     ",
        "FirmwareTemporary ",
        "FirmwarePermanent ",
        "OsloaderHeap      ",
        "OsloaderStack     ",
        "SystemCode        ",
        "HalCode           ",
        "BootDriver        ",
        "ConsoleInDriver   ",
        "ConsoleOutDriver  ",
        "StartupDpcStack   ",
        "StartupKernelStack",
        "StartupPanicStack ",
        "StartupPcrPage    ",
        "StartupPdrPage    ",
        "RegistryData      ",
        "MemoryData        ",
        "NlsData           ",
        "SpecialMemory     ",
        "BBTMemory         ",
        "LoaderReserve     ",
        "LoaderXIPRom      "
    };

    DPRINT1("Base\t\tLength\t\tType\n");
    for (NextEntry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
         NextEntry != &KeLoaderBlock->MemoryDescriptorListHead;
         NextEntry = NextEntry->Flink)
    {
        Md = CONTAINING_RECORD(NextEntry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        DPRINT1("%08lX\t%08lX\t%s\n", Md->BasePage, Md->PageCount, MemType[Md->MemoryType]);
        TotalPages += Md->PageCount;
    }

    DPRINT1("Total: %08lX (%d MB)\n", TotalPages, (TotalPages * PAGE_SIZE) / 1024 / 1024);
}

BOOLEAN
NTAPI
INIT_FUNCTION
MmArmInitSystem(IN ULONG Phase,
                IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG i;
    BOOLEAN IncludeType[LoaderMaximum];
    PVOID Bitmap;
    PPHYSICAL_MEMORY_RUN Run;

    /* Dump memory descriptors */
    if (MiDbgEnableMdDump) MiDbgDumpMemoryDescriptors();

    //
    // Instantiate memory that we don't consider RAM/usable
    // We use the same exclusions that Windows does, in order to try to be
    // compatible with WinLDR-style booting
    //
    for (i = 0; i < LoaderMaximum; i++) IncludeType[i] = TRUE;
    IncludeType[LoaderBad] = FALSE;
    IncludeType[LoaderFirmwarePermanent] = FALSE;
    IncludeType[LoaderSpecialMemory] = FALSE;
    IncludeType[LoaderBBTMemory] = FALSE;
    if (Phase == 0)
    {
        /* Initialize the phase 0 temporary event */
        KeInitializeEvent(&MiTempEvent, NotificationEvent, FALSE);

        /* Set all the events to use the temporary event for now */
        MiLowMemoryEvent = &MiTempEvent;
        MiHighMemoryEvent = &MiTempEvent;
        MiLowPagedPoolEvent = &MiTempEvent;
        MiHighPagedPoolEvent = &MiTempEvent;
        MiLowNonPagedPoolEvent = &MiTempEvent;
        MiHighNonPagedPoolEvent = &MiTempEvent;

        //
        // Define the basic user vs. kernel address space separation
        //
        MmSystemRangeStart = (PVOID)KSEG0_BASE;
        MmUserProbeAddress = (ULONG_PTR)MmSystemRangeStart - 0x10000;
        MmHighestUserAddress = (PVOID)(MmUserProbeAddress - 1);

        /* Highest PTE and PDE based on the addresses above */
        MiHighestUserPte = MiAddressToPte(MmHighestUserAddress);
        MiHighestUserPde = MiAddressToPde(MmHighestUserAddress);
#if (_MI_PAGING_LEVELS >= 3)
        /* We need the highest PPE and PXE addresses */
        ASSERT(FALSE);
#endif
        //
        // Get the size of the boot loader's image allocations and then round
        // that region up to a PDE size, so that any PDEs we might create for
        // whatever follows are separate from the PDEs that boot loader might've
        // already created (and later, we can blow all that away if we want to).
        //
        MmBootImageSize = KeLoaderBlock->Extension->LoaderPagesSpanned;
        MmBootImageSize *= PAGE_SIZE;
        MmBootImageSize = (MmBootImageSize + PDE_MAPPED_VA - 1) & ~(PDE_MAPPED_VA - 1);
        ASSERT((MmBootImageSize % PDE_MAPPED_VA) == 0);

        //
        // Set the size of session view, pool, and image
        //
        MmSessionSize = MI_SESSION_SIZE;
        MmSessionViewSize = MI_SESSION_VIEW_SIZE;
        MmSessionPoolSize = MI_SESSION_POOL_SIZE;
        MmSessionImageSize = MI_SESSION_IMAGE_SIZE;

        //
        // Set the size of system view
        //
        MmSystemViewSize = MI_SYSTEM_VIEW_SIZE;

        //
        // This is where it all ends
        //
        MiSessionImageEnd = (PVOID)PTE_BASE;

        //
        // This is where we will load Win32k.sys and the video driver
        //
        MiSessionImageStart = (PVOID)((ULONG_PTR)MiSessionImageEnd -
                                      MmSessionImageSize);

        //
        // So the view starts right below the session working set (itself below
        // the image area)
        //
        MiSessionViewStart = (PVOID)((ULONG_PTR)MiSessionImageEnd -
                                     MmSessionImageSize -
                                     MI_SESSION_WORKING_SET_SIZE -
                                     MmSessionViewSize);

        //
        // Session pool follows
        //
        MiSessionPoolEnd = MiSessionViewStart;
        MiSessionPoolStart = (PVOID)((ULONG_PTR)MiSessionPoolEnd -
                                     MmSessionPoolSize);

        //
        // And it all begins here
        //
        MmSessionBase = MiSessionPoolStart;

        //
        // Sanity check that our math is correct
        //
        ASSERT((ULONG_PTR)MmSessionBase + MmSessionSize == PTE_BASE);

        //
        // Session space ends wherever image session space ends
        //
        MiSessionSpaceEnd = MiSessionImageEnd;

        //
        // System view space ends at session space, so now that we know where
        // this is, we can compute the base address of system view space itself.
        //
        MiSystemViewStart = (PVOID)((ULONG_PTR)MmSessionBase -
                                    MmSystemViewSize);

        /* Compute the PTE addresses for all the addresses we carved out */
        MiSessionImagePteStart = MiAddressToPte(MiSessionImageStart);
        MiSessionImagePteEnd = MiAddressToPte(MiSessionImageEnd);
        MiSessionBasePte = MiAddressToPte(MmSessionBase);

        /* ReactOS Stuff */
        KeInitializeEvent(&ZeroPageThreadEvent, NotificationEvent, TRUE);

        /* Initialize the user mode image list */
        InitializeListHead(&MmLoadedUserImageList);

        /* Initialize the paged pool mutex */
        KeInitializeGuardedMutex(&MmPagedPoolMutex);

        /* Initialize the Loader Lock */
        KeInitializeMutant(&MmSystemLoadLock, FALSE);        

        /* Scan the boot loader memory descriptors */
        MiScanMemoryDescriptors(LoaderBlock);

        /* Compute color information (L2 cache-separated paging lists) */
        MiComputeColorInformation();

        // Calculate the number of bytes for the PFN database, double it for ARM3,
        // then add the color tables and convert to pages
        MxPfnAllocation = (MmHighestPhysicalPage + 1) * sizeof(MMPFN);
        //MxPfnAllocation <<= 1;
        MxPfnAllocation += (MmSecondaryColors * sizeof(MMCOLOR_TABLES) * 2);
        MxPfnAllocation >>= PAGE_SHIFT;

        // We have to add one to the count here, because in the process of
        // shifting down to the page size, we actually ended up getting the
        // lower aligned size (so say, 0x5FFFF bytes is now 0x5F pages).
        // Later on, we'll shift this number back into bytes, which would cause
        // us to end up with only 0x5F000 bytes -- when we actually want to have
        // 0x60000 bytes.
        //
        MxPfnAllocation++;

        //
        // Check if this is a machine with less than 19MB of RAM
        //
        if (MmNumberOfPhysicalPages < MI_MIN_PAGES_FOR_SYSPTE_TUNING)
        {
            //
            // Use the very minimum of system PTEs
            //
            MmNumberOfSystemPtes = 7000;
        }
        else
        {
            //
            // Use the default, but check if we have more than 32MB of RAM
            //
            MmNumberOfSystemPtes = 11000;
            if (MmNumberOfPhysicalPages > MI_MIN_PAGES_FOR_SYSPTE_BOOST) //
            {
                //
                // Double the amount of system PTEs
                //
                MmNumberOfSystemPtes <<= 1;
            }
        }

        DPRINT("System PTE count has been tuned to %d (%d bytes)\n",
               MmNumberOfSystemPtes, MmNumberOfSystemPtes * PAGE_SIZE);

        /* Initialize the working set lock */
        ExInitializePushLock((PULONG_PTR)&MmSystemCacheWs.WorkingSetMutex);

        /* Set commit limit */
        MmTotalCommitLimit = 2 * _1GB;
        MmTotalCommitLimitMaximum = MmTotalCommitLimit;

        /* Has the allocation fragment been setup? */
        if (!MmAllocationFragment)
        {
            /* Use the default value */
            MmAllocationFragment = MI_ALLOCATION_FRAGMENT;
            if (MmNumberOfPhysicalPages < ((256 * _1MB) / PAGE_SIZE)) //
            {
                /* On memory systems with less than 256MB, divide by 4 */
                MmAllocationFragment = MI_ALLOCATION_FRAGMENT / 4;
            }
            else if (MmNumberOfPhysicalPages < (_1GB / PAGE_SIZE)) //
            {
                /* On systems with less than 1GB, divide by 2 */
                MmAllocationFragment = MI_ALLOCATION_FRAGMENT / 2;
            }
        }
        else
        {
            /* Convert from 1KB fragments to pages */
            MmAllocationFragment *= _1KB;
            MmAllocationFragment = ROUND_TO_PAGES(MmAllocationFragment);

            /* Don't let it past the maximum */
            MmAllocationFragment = min(MmAllocationFragment,
                                       MI_MAX_ALLOCATION_FRAGMENT);

            /* Don't let it too small either */
            MmAllocationFragment = max(MmAllocationFragment,
                                       MI_MIN_ALLOCATION_FRAGMENT);
        }

        /* Check for kernel stack size that's too big */
        if (MmLargeStackSize > (KERNEL_LARGE_STACK_SIZE / _1KB))
        {
            /* Sanitize to default value */
            MmLargeStackSize = KERNEL_LARGE_STACK_SIZE;
        }
        else
        {
            /* Take the registry setting, and convert it into bytes */
            MmLargeStackSize *= _1KB;
            
            /* Now align it to a page boundary */
            MmLargeStackSize = PAGE_ROUND_UP(MmLargeStackSize);
            
            /* Sanity checks */
            ASSERT(MmLargeStackSize <= KERNEL_LARGE_STACK_SIZE);
            ASSERT((MmLargeStackSize & (PAGE_SIZE - 1)) == 0);
            
            /* Make sure it's not too low */
            if (MmLargeStackSize < KERNEL_STACK_SIZE) MmLargeStackSize = KERNEL_STACK_SIZE;
        }

        /* Initialize the platform-specific parts */       
        MiInitMachineDependent(LoaderBlock);

        /* Now go ahead and initialize the nonpaged pool */
        MiInitializeNonPagedPool();
        MiInitializeNonPagedPoolThresholds();

        /* Build the PFN Database */
        MiInitializePfnDatabase(LoaderBlock);
        MmInitializeBalancer(MmAvailablePages, 0);

        /* Initialize the nonpaged pool */
        InitializePool(NonPagedPool, 0);
        
        /* Create the system PTE space */
        MiInitializeSystemPtes(MiAddressToPte(MmNonPagedSystemStart),
                               MmNumberOfSystemPtes,
                               SystemPteSpace);
        
        /* Setup the mapping PTEs */
        MmFirstReservedMappingPte = MiAddressToPte(MI_MAPPING_RANGE_START);
        MmLastReservedMappingPte = MiAddressToPte(MI_MAPPING_RANGE_END);
        MmFirstReservedMappingPte->u.Hard.PageFrameNumber = MI_HYPERSPACE_PTES;

        /* Reserve system PTEs for zeroing PTEs and clear them */
        MiFirstReservedZeroingPte = MiReserveSystemPtes(MI_ZERO_PTES,
                                                        SystemPteSpace);
        RtlZeroMemory(MiFirstReservedZeroingPte, MI_ZERO_PTES * sizeof(MMPTE));
        
        /* Set the counter to maximum to boot with */
        MiFirstReservedZeroingPte->u.Hard.PageFrameNumber = MI_ZERO_PTES - 1;

        //
        // Build the physical memory block
        //
        MmPhysicalMemoryBlock = MmInitializeMemoryLimits(LoaderBlock,
                                                         IncludeType);

        //
        // Allocate enough buffer for the PFN bitmap
        // Align it up to a 32-bit boundary
        //
        Bitmap = ExAllocatePoolWithTag(NonPagedPool,
                                       (((MmHighestPhysicalPage + 1) + 31) / 32) * 4,
                                       '  mM');
        if (!Bitmap)
        {
            //
            // This is critical
            //
            KeBugCheckEx(INSTALL_MORE_MEMORY,
                         MmNumberOfPhysicalPages,
                         MmLowestPhysicalPage,
                         MmHighestPhysicalPage,
                         0x101);
        }

        //
        // Initialize it and clear all the bits to begin with
        //
        RtlInitializeBitMap(&MiPfnBitMap,
                            Bitmap,
                            MmHighestPhysicalPage + 1);
        RtlClearAllBits(&MiPfnBitMap);

        //
        // Loop physical memory runs
        //
        for (i = 0; i < MmPhysicalMemoryBlock->NumberOfRuns; i++)
        {
            //
            // Get the run
            //
            Run = &MmPhysicalMemoryBlock->Run[i];
            DPRINT("PHYSICAL RAM [0x%08p to 0x%08p]\n",
                   Run->BasePage << PAGE_SHIFT,
                   (Run->BasePage + Run->PageCount) << PAGE_SHIFT);

            //
            // Make sure it has pages inside it
            //
            if (Run->PageCount)
            {
                //
                // Set the bits in the PFN bitmap
                //
                RtlSetBits(&MiPfnBitMap, Run->BasePage, Run->PageCount);
            }
        }

        /* Look for large page cache entries that need caching */
        MiSyncCachedRanges();

        /* Loop for HAL Heap I/O device mappings that need coherency tracking */
        MiAddHalIoMappings();

        /* Set the initial resident page count */
        MmResidentAvailablePages = MmAvailablePages - 32;

        /* Initialize large page structures on PAE/x64, and MmProcessList on x86 */
        MiInitializeLargePageSupport();

        /* Check if the registry says any drivers should be loaded with large pages */
        MiInitializeDriverLargePageList();

        /* Relocate the boot drivers into system PTE space and fixup their PFNs */
        MiReloadBootLoadedDrivers(LoaderBlock);

        /* FIXME: Call out into Driver Verifier for initialization  */

        /* Check how many pages the system has */
        if (MmNumberOfPhysicalPages <= ((13 * _1MB) / PAGE_SIZE))
        {
            /* Set small system */
            MmSystemSize = MmSmallSystem;
        }
        else if (MmNumberOfPhysicalPages <= ((19 * _1MB) / PAGE_SIZE))
        {
            /* Set small system and add 100 pages for the cache */
            MmSystemSize = MmSmallSystem;
            MmSystemCacheWsMinimum += 100;
        }
        else
        {
            /* Set medium system and add 400 pages for the cache */
            MmSystemSize = MmMediumSystem;
            MmSystemCacheWsMinimum += 400;
        }

        /* Check for less than 24MB */
        if (MmNumberOfPhysicalPages < ((24 * _1MB) / PAGE_SIZE))
        {
            /* No more than 32 pages */
            MmSystemCacheWsMinimum = 32;
        }

        /* Check for more than 32MB */
        if (MmNumberOfPhysicalPages >= ((32 * _1MB) / PAGE_SIZE))
        {
            /* Check for product type being "Wi" for WinNT */
            if (MmProductType == '\0i\0W')
            {
                /* Then this is a large system */
                MmSystemSize = MmLargeSystem;
            }
            else
            {
                /* For servers, we need 64MB to consider this as being large */
                if (MmNumberOfPhysicalPages >= ((64 * _1MB) / PAGE_SIZE))
                {
                    /* Set it as large */
                    MmSystemSize = MmLargeSystem;
                }
            }
        }

        /* Check for more than 33 MB */
        if (MmNumberOfPhysicalPages > ((33 * _1MB) / PAGE_SIZE))
        {
            /* Add another 500 pages to the cache */
            MmSystemCacheWsMinimum += 500;
        }

        /* Now setup the shared user data fields */
        ASSERT(SharedUserData->NumberOfPhysicalPages == 0);
        SharedUserData->NumberOfPhysicalPages = MmNumberOfPhysicalPages;
        SharedUserData->LargePageMinimum = 0;

        /* Check for workstation (Wi for WinNT) */
        if (MmProductType == '\0i\0W')
        {
            /* Set Windows NT Workstation product type */
            SharedUserData->NtProductType = NtProductWinNt;
            MmProductType = 0;
        }
        else
        {
            /* Check for LanMan server */
            if (MmProductType == '\0a\0L')
            {
                /* This is a domain controller */
                SharedUserData->NtProductType = NtProductLanManNt;
            }
            else
            {
                /* Otherwise it must be a normal server */
                SharedUserData->NtProductType = NtProductServer;
            }

            /* Set the product type, and make the system more aggressive with low memory */
            MmProductType = 1;
            MmMinimumFreePages = 81;
        }

        /* Update working set tuning parameters */
        MiAdjustWorkingSetManagerParameters(!MmProductType);

        /* Finetune the page count by removing working set and NP expansion */
        MmResidentAvailablePages -= MiExpansionPoolPagesInitialCharge;
        MmResidentAvailablePages -= MmSystemCacheWsMinimum;
        MmResidentAvailableAtInit = MmResidentAvailablePages;
        if (MmResidentAvailablePages <= 0)
        {
            /* This should not happen */
            DPRINT1("System cache working set too big\n");
            return FALSE;
        }

        /* Initialize the system cache */
        //MiInitializeSystemCache(MmSystemCacheWsMinimum, MmAvailablePages);

        /* Update the commit limit */
        MmTotalCommitLimit = MmAvailablePages;
        if (MmTotalCommitLimit > 1024) MmTotalCommitLimit -= 1024;
        MmTotalCommitLimitMaximum = MmTotalCommitLimit;

        /* Size up paged pool and build the shadow system page directory */
        MiBuildPagedPool();

        /* Debugger physical memory support is now ready to be used */
        MmDebugPte = MiAddressToPte(MiDebugMapping);

        /* Initialize the loaded module list */
        MiInitializeLoadedModuleList(LoaderBlock);
    }

    //
    // Always return success for now
    //
    return TRUE;
}

/* EOF */
