use std::{
    alloc::{GlobalAlloc, Layout, System},
    cell::Cell,
};

pub struct CountingAllocator;

#[derive(Clone, Copy, Default)]
struct ThreadAllocationState {
    allocations: u64,
    allocated_bytes: u64,
    live_bytes: usize,
    peak_live_bytes: usize,
}

thread_local! {
    static THREAD_STATE: Cell<ThreadAllocationState> = const {
        Cell::new(ThreadAllocationState {
            allocations: 0,
            allocated_bytes: 0,
            live_bytes: 0,
            peak_live_bytes: 0,
        })
    };
}

#[derive(Clone, Copy, Debug, Default)]
pub struct AllocationSnapshot {
    pub allocations: u64,
    pub allocated_bytes: u64,
    pub live_bytes: usize,
    pub peak_live_bytes: usize,
}

unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() {
            record_allocation(layout.size());
        }
        pointer
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc_zeroed(layout) };
        if !pointer.is_null() {
            record_allocation(layout.size());
        }
        pointer
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        record_deallocation(layout.size());
        unsafe { System.dealloc(pointer, layout) };
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        let resized = unsafe { System.realloc(pointer, layout, new_size) };
        if !resized.is_null() {
            record_reallocation(layout.size(), new_size);
        }
        resized
    }
}

fn record_allocation(size: usize) {
    let _ = THREAD_STATE.try_with(|state| {
        let mut current = state.get();
        current.allocations += 1;
        current.allocated_bytes += size as u64;
        current.live_bytes += size;
        current.peak_live_bytes = current.peak_live_bytes.max(current.live_bytes);
        state.set(current);
    });
}

fn record_deallocation(size: usize) {
    let _ = THREAD_STATE.try_with(|state| {
        let mut current = state.get();
        current.live_bytes = current.live_bytes.saturating_sub(size);
        state.set(current);
    });
}

fn record_reallocation(old_size: usize, new_size: usize) {
    let _ = THREAD_STATE.try_with(|state| {
        let mut current = state.get();
        current.allocations += 1;
        current.allocated_bytes += new_size as u64;
        current.live_bytes = current
            .live_bytes
            .saturating_sub(old_size)
            .saturating_add(new_size);
        current.peak_live_bytes = current.peak_live_bytes.max(current.live_bytes);
        state.set(current);
    });
}

pub fn snapshot() -> AllocationSnapshot {
    THREAD_STATE.with(|state| {
        let current = state.get();
        AllocationSnapshot {
            allocations: current.allocations,
            allocated_bytes: current.allocated_bytes,
            live_bytes: current.live_bytes,
            peak_live_bytes: current.peak_live_bytes,
        }
    })
}

pub fn begin_measurement() -> AllocationSnapshot {
    let current = snapshot();
    THREAD_STATE.with(|state| {
        let mut value = state.get();
        value.peak_live_bytes = value.live_bytes;
        state.set(value);
    });
    current
}

pub fn delta(before: AllocationSnapshot) -> AllocationSnapshot {
    let after = snapshot();
    AllocationSnapshot {
        allocations: after.allocations.saturating_sub(before.allocations),
        allocated_bytes: after.allocated_bytes.saturating_sub(before.allocated_bytes),
        live_bytes: after.live_bytes.saturating_sub(before.live_bytes),
        peak_live_bytes: after.peak_live_bytes.saturating_sub(before.live_bytes),
    }
}
