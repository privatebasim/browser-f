/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Tracer_h
#define js_Tracer_h

#include "mozilla/DebugOnly.h"

#include "gc/Heap.h"
#include "js/GCAPI.h"
#include "js/SliceBudget.h"
#include "js/TracingAPI.h"

namespace js {
class BaseShape;
class GCMarker;
class LazyScript;
class NativeObject;
class ObjectGroup;
namespace gc {
struct ArenaHeader;
}
namespace jit {
class JitCode;
}

static const size_t NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY = 4096;
static const size_t INCREMENTAL_MARK_STACK_BASE_CAPACITY = 32768;

/*
 * When the native stack is low, the GC does not call JS_TraceChildren to mark
 * the reachable "children" of the thing. Rather the thing is put aside and
 * JS_TraceChildren is called later with more space on the C stack.
 *
 * To implement such delayed marking of the children with minimal overhead for
 * the normal case of sufficient native stack, the code adds a field per arena.
 * The field markingDelay->link links all arenas with delayed things into a
 * stack list with the pointer to stack top in GCMarker::unmarkedArenaStackTop.
 * GCMarker::delayMarkingChildren adds arenas to the stack as necessary while
 * markDelayedChildren pops the arenas from the stack until it empties.
 */
class MarkStack
{
    friend class GCMarker;

    uintptr_t* stack_;
    uintptr_t* tos_;
    uintptr_t* end_;

    // The capacity we start with and reset() to.
    size_t baseCapacity_;
    size_t maxCapacity_;

  public:
    explicit MarkStack(size_t maxCapacity)
      : stack_(nullptr),
        tos_(nullptr),
        end_(nullptr),
        baseCapacity_(0),
        maxCapacity_(maxCapacity)
    {}

    ~MarkStack() {
        js_free(stack_);
    }

    size_t capacity() { return end_ - stack_; }

    ptrdiff_t position() const { return tos_ - stack_; }

    void setStack(uintptr_t* stack, size_t tosIndex, size_t capacity) {
        stack_ = stack;
        tos_ = stack + tosIndex;
        end_ = stack + capacity;
    }

    bool init(JSGCMode gcMode);

    void setBaseCapacity(JSGCMode mode);
    size_t maxCapacity() const { return maxCapacity_; }
    void setMaxCapacity(size_t maxCapacity);

    bool push(uintptr_t item) {
        if (tos_ == end_) {
            if (!enlarge(1))
                return false;
        }
        MOZ_ASSERT(tos_ < end_);
        *tos_++ = item;
        return true;
    }

    bool push(uintptr_t item1, uintptr_t item2, uintptr_t item3) {
        uintptr_t* nextTos = tos_ + 3;
        if (nextTos > end_) {
            if (!enlarge(3))
                return false;
            nextTos = tos_ + 3;
        }
        MOZ_ASSERT(nextTos <= end_);
        tos_[0] = item1;
        tos_[1] = item2;
        tos_[2] = item3;
        tos_ = nextTos;
        return true;
    }

    bool isEmpty() const {
        return tos_ == stack_;
    }

    uintptr_t pop() {
        MOZ_ASSERT(!isEmpty());
        return *--tos_;
    }

    void reset();

    /* Grow the stack, ensuring there is space for at least count elements. */
    bool enlarge(unsigned count);

    void setGCMode(JSGCMode gcMode);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

#ifdef DEBUG
namespace gc {

template <typename T>
extern bool
ZoneIsGCMarking(T* thing);

template <typename T>
extern bool
ZoneIsAtomsZoneForString(JSRuntime* rt, T* thing);

} /* namespace gc */
#endif

#define JS_COMPARTMENT_ASSERT(rt, thing) \
    MOZ_ASSERT(gc::ZoneIsGCMarking((thing)) || gc::ZoneIsAtomsZoneForString((rt), (thing)))

class GCMarker : public JSTracer
{
  public:
    explicit GCMarker(JSRuntime* rt);
    bool init(JSGCMode gcMode);

    void setMaxCapacity(size_t maxCap) { stack.setMaxCapacity(maxCap); }
    size_t maxCapacity() const { return stack.maxCapacity(); }

    void start();
    void stop();
    void reset();

    // Mark the given GC thing and traverse its children at some point.
    void traverse(JSObject* thing) { markAndPush(ObjectTag, thing); }
    void traverse(ObjectGroup* thing) { markAndPush(GroupTag, thing); }
    void traverse(jit::JitCode* thing) { markAndPush(JitCodeTag, thing); }
    // The following traverse methods traverse immediately, go out-of-line to do so.
    void traverse(JSScript* thing) { markAndTraverse(thing); }
    void traverse(LazyScript* thing) { markAndTraverse(thing); }
    // The other types are marked immediately and inline via a ScanFoo shared
    // between PushMarkStack and the processMarkStackTop. Since ScanFoo is
    // inline in Marking.cpp, we cannot inline it here, yet.

    /*
     * Care must be taken changing the mark color from gray to black. The cycle
     * collector depends on the invariant that there are no black to gray edges
     * in the GC heap. This invariant lets the CC not trace through black
     * objects. If this invariant is violated, the cycle collector may free
     * objects that are still reachable.
     */
    void setMarkColorGray() {
        MOZ_ASSERT(isDrained());
        MOZ_ASSERT(color == gc::BLACK);
        color = gc::GRAY;
    }
    void setMarkColorBlack() {
        MOZ_ASSERT(isDrained());
        MOZ_ASSERT(color == gc::GRAY);
        color = gc::BLACK;
    }
    uint32_t markColor() const { return color; }

    void delayMarkingArena(gc::ArenaHeader* aheader);
    void delayMarkingChildren(const void* thing);
    void markDelayedChildren(gc::ArenaHeader* aheader);
    bool markDelayedChildren(SliceBudget& budget);
    bool hasDelayedChildren() const {
        return !!unmarkedArenaStackTop;
    }

    bool isDrained() {
        return isMarkStackEmpty() && !unmarkedArenaStackTop;
    }

    bool drainMarkStack(SliceBudget& budget);

    void setGCMode(JSGCMode mode) { stack.setGCMode(mode); }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

#ifdef DEBUG
    bool shouldCheckCompartments() { return strictCompartmentChecking; }
#endif

    /* This is public exclusively for ScanRope. */
    MarkStack stack;

  private:
#ifdef DEBUG
    void checkZone(void* p);
#else
    void checkZone(void* p) {}
#endif

    /*
     * We use a common mark stack to mark GC things of different types and use
     * the explicit tags to distinguish them when it cannot be deduced from
     * the context of push or pop operation.
     */
    enum StackTag {
        ValueArrayTag,
        ObjectTag,
        GroupTag,
        SavedValueArrayTag,
        JitCodeTag,
        LastTag = JitCodeTag
    };

    static const uintptr_t StackTagMask = 7;
    static_assert(StackTagMask >= uintptr_t(LastTag), "The tag mask must subsume the tags.");
    static_assert(StackTagMask <= gc::CellMask, "The tag mask must be embeddable in a Cell*.");

    // Push an object onto the stack for later tracing and assert that it has
    // already been marked.
    void repush(JSObject* obj) {
        MOZ_ASSERT(gc::TenuredCell::fromPointer(obj)->isMarked(markColor()));
        pushTaggedPtr(ObjectTag, obj);
    }

    template <typename T>
    void markAndPush(StackTag tag, T* thing) {
        if (mark(thing))
            pushTaggedPtr(tag, thing);
    }

    template <typename T>
    void markAndTraverse(T* thing) {
        if (mark(thing))
            markChildren(thing);
    }

    template <typename T>
    void markChildren(T* thing);

    // Mark the given GC thing, but do not trace its children. Return true
    // if the thing became marked.
    template <typename T>
    bool mark(T* thing) {
        JS_COMPARTMENT_ASSERT(runtime(), thing);
        MOZ_ASSERT(!IsInsideNursery(gc::TenuredCell::fromPointer(thing)));
        return gc::TenuredCell::fromPointer(thing)->markIfUnmarked(markColor());
    }

    void pushTaggedPtr(StackTag tag, void* ptr) {
        checkZone(ptr);
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        MOZ_ASSERT(!(addr & StackTagMask));
        if (!stack.push(addr | uintptr_t(tag)))
            delayMarkingChildren(ptr);
    }

    void pushValueArray(JSObject* obj, void* start, void* end) {
        checkZone(obj);

        MOZ_ASSERT(start <= end);
        uintptr_t tagged = reinterpret_cast<uintptr_t>(obj) | GCMarker::ValueArrayTag;
        uintptr_t startAddr = reinterpret_cast<uintptr_t>(start);
        uintptr_t endAddr = reinterpret_cast<uintptr_t>(end);

        /*
         * Push in the reverse order so obj will be on top. If we cannot push
         * the array, we trigger delay marking for the whole object.
         */
        if (!stack.push(endAddr, startAddr, tagged))
            delayMarkingChildren(obj);
    }

    bool isMarkStackEmpty() {
        return stack.isEmpty();
    }

    bool restoreValueArray(NativeObject* obj, void** vpp, void** endp);
    void saveValueRanges();
    inline void processMarkStackTop(SliceBudget& budget);
    void processMarkStackOther(uintptr_t tag, uintptr_t addr);

    void markAndScanString(JSObject* source, JSString* str);
    void markAndScanSymbol(JSObject* source, JS::Symbol* sym);

    void appendGrayRoot(void* thing, JSGCTraceKind kind);

    /* The color is only applied to objects and functions. */
    uint32_t color;

    /* Pointer to the top of the stack of arenas we are delaying marking on. */
    js::gc::ArenaHeader* unmarkedArenaStackTop;

    /* Count of arenas that are currently in the stack. */
    mozilla::DebugOnly<size_t> markLaterArenas;

    /* Assert that start and stop are called with correct ordering. */
    mozilla::DebugOnly<bool> started;

    /*
     * If this is true, all marked objects must belong to a compartment being
     * GCed. This is used to look for compartment bugs.
     */
    mozilla::DebugOnly<bool> strictCompartmentChecking;
};

// Append traced things to a buffer on the zone for use later in the GC.
// See the comment in GCRuntime.h above grayBufferState for details.
class BufferGrayRootsTracer : public JS::CallbackTracer
{
    // Set to false if we OOM while buffering gray roots.
    bool bufferingGrayRootsFailed;

    void appendGrayRoot(void* thing, JSGCTraceKind kind);

  public:
    explicit BufferGrayRootsTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt, grayTraceCallback), bufferingGrayRootsFailed(false)
    {}

    static void grayTraceCallback(JS::CallbackTracer* trc, void** thingp, JSGCTraceKind kind) {
        static_cast<BufferGrayRootsTracer*>(trc)->appendGrayRoot(*thingp, kind);
    }

    bool failed() const { return bufferingGrayRootsFailed; }
};

void
SetMarkStackLimit(JSRuntime* rt, size_t limit);

// Return true if this trace is happening on behalf of gray buffering during
// the marking phase of incremental GC.
inline bool
IsBufferingGrayRoots(JSTracer* trc)
{
    return trc->isCallbackTracer() &&
           trc->asCallbackTracer()->hasCallback(BufferGrayRootsTracer::grayTraceCallback);
}

} /* namespace js */

#endif /* js_Tracer_h */