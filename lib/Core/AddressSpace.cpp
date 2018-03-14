//===-- AddressSpace.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "AddressSpace.h"

#include "ExecutionState.h"
#include "Memory.h"
#include "TimingSolver.h"

#include "klee/Expr/Expr.h"
#include "klee/Module/KValue.h"
#include "klee/Statistics/TimerStatIncrementer.h"

#include "CoreStats.h"


using namespace klee;

///

void AddressSpace::bindObject(const MemoryObject *mo, ObjectState *os) {
  assert(os->copyOnWriteOwner==0 && "object already has owner");
  os->copyOnWriteOwner = cowKey;
  objects = objects.replace(std::make_pair(mo, os));
  if (mo->segment != 0)
    segmentMap = segmentMap.replace(std::make_pair(mo->segment, mo));
}

void AddressSpace::unbindObject(const MemoryObject *mo) {
  if (mo->segment != 0)
    segmentMap = segmentMap.remove(mo->segment);
  objects = objects.remove(mo);
  // NOTE MemoryObjects are reference counted, *mo is deleted at this point
}

const ObjectState *AddressSpace::findObject(const MemoryObject *mo) const {
  const auto res = objects.lookup(mo);
  return res ? res->second.get() : nullptr;
}

ObjectState *AddressSpace::getWriteable(const MemoryObject *mo,
                                        const ObjectState *os) {
  assert(!os->readOnly);

  // If this address space owns they object, return it
  if (cowKey == os->copyOnWriteOwner)
    return const_cast<ObjectState*>(os);

  // Add a copy of this object state that can be updated
  ref<ObjectState> newObjectState(new ObjectState(*os));
  newObjectState->copyOnWriteOwner = cowKey;
  objects = objects.replace(std::make_pair(mo, newObjectState));
  return newObjectState.get();
}

/// 

bool AddressSpace::resolveConstantAddress(const KValue &pointer,
                                          ObjectPair &result) const {
  uint64_t segment = cast<ConstantExpr>(pointer.getSegment())->getZExtValue();

  if (segment != 0) {
    if (const SegmentMap::value_type *res = segmentMap.lookup(segment)) {
      // TODO bounds check?
      const auto& objpair = *objects.lookup(res->second);
      result.first = objpair.first;
      result.second = objpair.second.get();
      return true;
    }
  } else {
    uint64_t address = cast<ConstantExpr>(pointer.getValue())->getZExtValue();
    MemoryObject hack(address);
    if (const auto res = objects.lookup_previous(&hack)) {
      const auto &mo = res->first;
      // objects with symbolic size can only be accessed through segmented pointers
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size)) {
        unsigned size = CE->getZExtValue();
        // Check if the provided address is between start and end of the object
        // [mo->address, mo->address + mo->size) or the object is a 0-sized object.
        if ((size==0 && address==mo->address) ||
            (address - mo->address < size)) {
          result.first = res->first;
          result.second = res->second.get();
          return true;
        }
      }
    }
  }
  return false;
}

bool AddressSpace::resolveOne(ExecutionState &state,
                              TimingSolver *solver,
                              const KValue &pointer,
                              ObjectPair &result,
                              bool &success) const {
  if (pointer.isConstant()) {
    success = resolveConstantAddress(pointer, result);
    return true;
  } else {
    ref<ConstantExpr> segment = dyn_cast<ConstantExpr>(pointer.getSegment());
    if (segment.isNull()) {
      TimerStatIncrementer timer(stats::resolveTime);
      if (!solver->getValue(state.constraints, pointer.getSegment(), segment,
                            state.queryMetaData))
        return false;
    }

    if (!segment->isZero()) {
      return resolveConstantAddress(KValue(segment, pointer.getOffset()), result);
    }

    TimerStatIncrementer timer(stats::resolveTime);

    // try cheap search, will succeed for any inbounds pointer

    ref<ConstantExpr> cex;
    if (!solver->getValue(state.constraints, pointer.getOffset(), cex, state.queryMetaData))
      return false;
    uint64_t example = cex->getZExtValue();
    MemoryObject hack(example);
    const auto res = objects.lookup_previous(&hack);

    if (res) {
      const MemoryObject *mo = res->first;
      // objects with symbolic size can only be accessed through segmented pointers
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size)) {
        if (example - mo->address < CE->getZExtValue()) {
          result.first = res->first;
          result.second = res->second.get();
          success = true;
          return true;
        }
      }
    }

    // didn't work, now we have to search
       
    MemoryMap::iterator oi = objects.upper_bound(&hack);
    MemoryMap::iterator begin = objects.begin();
    MemoryMap::iterator end = objects.end();
      
    MemoryMap::iterator start = oi;
    while (oi!=begin) {
      --oi;
      const auto &mo = oi->first;

      bool mayBeTrue;
      if (!solver->mayBeTrue(state.constraints,
                             mo->getBoundsCheckPointer(pointer), mayBeTrue,
                             state.queryMetaData))
        return false;
      if (mayBeTrue) {
        result.first = oi->first;
        result.second = oi->second.get();
        success = true;
        return true;
      } else {
        bool mustBeTrue;
        if (!solver->mustBeTrue(state.constraints,
                                UgeExpr::create(pointer.getOffset(), mo->getBaseExpr()),
                                mustBeTrue, state.queryMetaData))
          return false;
        if (mustBeTrue)
          break;
      }
    }

    // search forwards
    for (oi=start; oi!=end; ++oi) {
      const auto &mo = oi->first;

      bool mustBeTrue;
      if (!solver->mustBeTrue(state.constraints,
                              UltExpr::create(pointer.getOffset(), mo->getBaseExpr()),
                              mustBeTrue, state.queryMetaData))
        return false;
      if (mustBeTrue) {
        break;
      } else {
        bool mayBeTrue;

        if (!solver->mayBeTrue(state.constraints,
                               mo->getBoundsCheckPointer(pointer), mayBeTrue,
                               state.queryMetaData))
          return false;
        if (mayBeTrue) {
          result.first = oi->first;
          result.second = oi->second.get();
          success = true;
          return true;
        }
      }
    }

    success = false;
    return true;
  }
}

int AddressSpace::checkPointerInObject(ExecutionState &state,
                                       TimingSolver *solver,
                                       const KValue& pointer,
                                       const ObjectPair &op, ResolutionList &rl,
                                       unsigned maxResolutions) const {
  // XXX I think there is some query wasteage here?
  // In the common case we can save one query if we ask
  // mustBeTrue before mayBeTrue for the first result. easy
  // to add I just want to have a nice symbolic test case first.
  const MemoryObject *mo = op.first;
  ref<Expr> inBounds = mo->getBoundsCheckPointer(pointer);
  bool mayBeTrue;
  if (!solver->mayBeTrue(state.constraints, inBounds, mayBeTrue,
                         state.queryMetaData)) {
    return 1;
  }

  if (mayBeTrue) {
    rl.push_back(op);

    // fast path check
    auto size = rl.size();
    if (size == 1) {
      bool mustBeTrue;
      if (!solver->mustBeTrue(state.constraints, inBounds, mustBeTrue,
                              state.queryMetaData))
        return 1;
      if (mustBeTrue)
        return 0;
    }
    else
      if (size == maxResolutions)
        return 1;
  }

  return 2;
}

bool AddressSpace::resolve(ExecutionState &state,
                           TimingSolver *solver,
                           const KValue &pointer,
                           ResolutionList &rl,
                           unsigned maxResolutions,
                           time::Span timeout) const {
  if (isa<ConstantExpr>(pointer.getSegment()))
    return resolveConstantSegment(state, solver, pointer, rl, maxResolutions, timeout);

  TimerStatIncrementer timer(stats::resolveTime);

  bool mayBeTrue;
  ref<Expr> zeroSegment = ConstantExpr::create(0, pointer.getWidth());
  if (!solver->mayBeTrue(state.constraints,
                         Expr::createIsZero(pointer.getSegment()),
                         mayBeTrue, state.queryMetaData))
    return true;
  if (mayBeTrue && resolveConstantSegment(state, solver,
                                          KValue(zeroSegment, pointer.getValue()),
                                          rl, maxResolutions, timeout))
    return true;
  // TODO inefficient
  for (const SegmentMap::value_type &res : segmentMap) {
    if (timeout && timeout < timer.delta())
      return true;
    ref<Expr> segmentExpr = ConstantExpr::create(res.first, pointer.getWidth());
    ref<Expr> expr = EqExpr::create(pointer.getSegment(), segmentExpr);
    if (!solver->mayBeTrue(state.constraints, expr, mayBeTrue,
                           state.queryMetaData))
      return true;
    if (mayBeTrue) {
      const auto &pair = *objects.lookup(res.second);
      rl.emplace_back(pair.first, pair.second.get());
    }
  }
  return false;
}

bool AddressSpace::resolveConstantSegment(ExecutionState &state,
                                          TimingSolver *solver,
                                          const KValue &pointer,
                                          ResolutionList &rl,
                                          unsigned maxResolutions,
                                          time::Span timeout) const {
  if (!cast<ConstantExpr>(pointer.getSegment())->isZero()) {
    ObjectPair res;
    if (resolveConstantAddress(pointer, res))
      rl.push_back(res);
    return false;
  }

  TimerStatIncrementer timer(stats::resolveTime);

  // XXX in general this isn't exactly what we want... for
  // a multiple resolution case (or for example, a \in {b,c,0})
  // we want to find the first object, find a cex assuming
  // not the first, find a cex assuming not the second...
  // etc.

  // XXX how do we smartly amortize the cost of checking to
  // see if we need to keep searching up/down, in bad cases?
  // maybe we don't care?

  // XXX we really just need a smart place to start (although
  // if its a known solution then the code below is guaranteed
  // to hit the fast path with exactly 2 queries). we could also
  // just get this by inspection of the expr.

  ref<ConstantExpr> cex;
  if (!solver->getValue(state.constraints, pointer.getOffset(), cex, state.queryMetaData))
    return true;
  uint64_t example = cex->getZExtValue();
  MemoryObject hack(example);

  MemoryMap::iterator oi = objects.upper_bound(&hack);
  MemoryMap::iterator begin = objects.begin();
  MemoryMap::iterator end = objects.end();

  MemoryMap::iterator start = oi;

  // XXX in the common case we can save one query if we ask
  // mustBeTrue before mayBeTrue for the first result. easy
  // to add I just want to have a nice symbolic test case first.

  // search backwards, start with one minus because this
  // is the object that p *should* be within, which means we
  // get write off the end with 4 queries (XXX can be better,
  // no?)
    while (oi!=begin) {
      --oi;
      const MemoryObject *mo = oi->first;
      if (timeout && timeout < timer.delta())
        return true;

      auto op = std::make_pair<>(mo, oi->second.get());

      int incomplete =
          checkPointerInObject(state, solver, pointer, op, rl, maxResolutions);
      if (incomplete != 2)
        return incomplete ? true : false;


      bool mustBeTrue;
      if (!solver->mustBeTrue(state.constraints,
                              UgeExpr::create(pointer.getOffset(),
                                                     mo->getBaseExpr()), mustBeTrue,
                              state.queryMetaData))
        return true;
      if (mustBeTrue)
         break;
    }

    // search forwards
    for (oi = start; oi != end; ++oi) {
      const MemoryObject *mo = oi->first;
      if (timeout && timeout < timer.delta())
        return true;

      bool mustBeTrue;
      if (!solver->mustBeTrue(state.constraints,
                              UltExpr::create(pointer.getOffset(),
                                              mo->getBaseExpr()),mustBeTrue,
                              state.queryMetaData))
        return true;
      if (mustBeTrue)
        break;
      auto op = std::make_pair<>(mo, oi->second.get());

      int incomplete =
          checkPointerInObject(state, solver, pointer, op, rl, maxResolutions);
      if (incomplete != 2)
        return incomplete ? true : false;
    }

  return false;
}

// These two are pretty big hack so we can sort of pass memory back
// and forth to externals. They work by abusing the concrete cache
// store inside of the object states, which allows them to
// transparently avoid screwing up symbolics (if the byte is symbolic
// then its concrete cache byte isn't being used) but is just a hack.

void AddressSpace::copyOutConcretes() {
  for (MemoryMap::iterator it = objects.begin(), ie = objects.end(); 
       it != ie; ++it) {
    const MemoryObject *mo = it->first;

    if (!mo->isUserSpecified) {
      const auto &os = it->second;
      auto address = reinterpret_cast<std::uint8_t*>(mo->address);

      if (!os->readOnly) {
        auto &concreteStore = os->offsetPlane->concreteStore;
        concreteStore.resize(os->offsetPlane->sizeBound,
                             os->offsetPlane->initialValue);
        memcpy(address, concreteStore.data(), concreteStore.size());
      }
    }
  }
}

bool AddressSpace::copyInConcretes() {
  for (auto &obj : objects) {
    const MemoryObject *mo = obj.first;

    if (!mo->isUserSpecified) {
      const auto &os = obj.second;

      if (!copyInConcrete(mo, os.get(), mo->address))
        return false;
    }
  }

  return true;
}

bool AddressSpace::copyInConcrete(const MemoryObject *mo, const ObjectState *os,
                                  uint64_t src_address) {
  auto address = reinterpret_cast<std::uint8_t*>(src_address);
  // TODO segment
  auto &concreteStoreR = os->offsetPlane->concreteStore;
  if (memcmp(address, concreteStoreR.data(), concreteStoreR.size())!=0) {
    if (os->readOnly) {
      return false;
    } else {
      ObjectState *wos = getWriteable(mo, os);
      auto &concreteStoreW = wos->offsetPlane->concreteStore;
      memcpy(concreteStoreW.data(), address, concreteStoreW.size());
    }
  }
  return true;
}

/***/

bool MemoryObjectLT::operator()(const MemoryObject *a, const MemoryObject *b) const {
  return a->address < b->address;
}

