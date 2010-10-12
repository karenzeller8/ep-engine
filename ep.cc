/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 NorthScale, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"
#include <vector>
#include <time.h>
#include <string.h>
#include <iostream>
#include <functional>

#include "ep.hh"
#include "flusher.hh"
#include "locks.hh"
#include "dispatcher.hh"
#include "sqlite-kvstore.hh"
#include "ep_engine.h"
#include "item_pager.hh"

extern "C" {
    static rel_time_t uninitialized_current_time(void) {
        abort();
        return 0;
    }

    static time_t default_abs_time(rel_time_t offset) {
        /* This is overridden at init time */
        return time(NULL) - ep_current_time() + offset;
    }

    rel_time_t (*ep_current_time)() = uninitialized_current_time;
    time_t (*ep_abs_time)(rel_time_t) = default_abs_time;

    time_t ep_real_time() {
        return ep_abs_time(ep_current_time());
    }
}

class BGFetchCallback : public DispatcherCallback {
public:
    BGFetchCallback(EventuallyPersistentStore *e,
                    const std::string &k, uint16_t vbid, uint64_t r,
                    const void *c) :
        ep(e), key(k), vbucket(vbid), rowid(r), cookie(c),
        init(gethrtime()), start(0) {
        assert(ep);
        assert(cookie);
    }

    bool callback(Dispatcher &d, TaskId t) {
        (void)d; (void)t;
        start = gethrtime();
        ep->completeBGFetch(key, vbucket, rowid, cookie, init, start);
        return false;
    }

    std::string description() {
        std::stringstream ss;
        ss << "Fetching item from disk:  " << key;
        return ss.str();
    }

private:
    EventuallyPersistentStore *ep;
    std::string                key;
    uint16_t                   vbucket;
    uint64_t                   rowid;
    const void                *cookie;

    hrtime_t init;
    hrtime_t start;
};

class VKeyStatBGFetchCallback : public DispatcherCallback {
public:
    VKeyStatBGFetchCallback(EventuallyPersistentStore *e,
                            const std::string &k, uint16_t vbid, uint64_t r,
                            const void *c, shared_ptr<Callback<GetValue> > cb) :
        ep(e), key(k), vbucket(vbid), rowid(r), cookie(c),
        lookup_cb(cb) {
        assert(ep);
        assert(cookie);
        assert(lookup_cb);
    }

    bool callback(Dispatcher &d, TaskId t) {
        (void)d; (void)t;
        RememberingCallback<GetValue> gcb;

        --ep->bgFetchQueue;
        ep->getUnderlying()->get(key, rowid, gcb);
        gcb.waitForValue();
        assert(gcb.fired);
        lookup_cb->callback(gcb.val);

        return false;
    }

    std::string description() {
        std::stringstream ss;
        ss << "Fetching item from disk for vkey stat:  " << key;
        return ss.str();
    }

private:
    EventuallyPersistentStore       *ep;
    std::string                      key;
    uint16_t                         vbucket;
    uint64_t                         rowid;
    const void                      *cookie;
    shared_ptr<Callback<GetValue> >  lookup_cb;
};

class SetVBStateCallback : public DispatcherCallback {
public:
    SetVBStateCallback(EventuallyPersistentStore *e, uint16_t vb,
                       const std::string &k)
        : ep(e), vbid(vb), key(k) { }

    bool callback(Dispatcher &d, TaskId t) {
        (void)d; (void)t;
        ep->completeSetVBState(vbid, key);
        return false;
    }

    std::string description() {
        std::stringstream ss;
        ss << "Setting vbucket " << vbid << " state to " << key;
        return ss.str();
    }
private:
    EventuallyPersistentStore *ep;
    uint16_t vbid;
    std::string key;
};

class NotifyVBStateChangeCallback : public DispatcherCallback {
public:
    NotifyVBStateChangeCallback(RCPtr<VBucket> vb, SERVER_HANDLE_V1 *a)
        : vbucket(vb), api(a) {
        assert(api);
    }

    bool callback(Dispatcher &d, TaskId t) {
        (void)d; (void)t;
        vbucket->fireAllOps(api);
        return false;
    }

    std::string description() {
        std::stringstream ss;
        ss << "Notifying state change of vbucket " << vbucket->getId();
        return ss.str();
    }

private:
    RCPtr<VBucket>   vbucket;
    SERVER_HANDLE_V1 *api;
};

class VBucketDeletionCallback : public DispatcherCallback {
public:
    VBucketDeletionCallback(EventuallyPersistentStore *e, uint16_t vbid)
        : ep(e), vbucket(vbid) {
        assert(ep);
    }

    bool callback(Dispatcher &d, TaskId t) {
        (void)d; (void)t;
        ep->completeVBucketDeletion(vbucket);
        return false;
    }

    std::string description() {
        std::stringstream ss;
        ss << "Removing vbucket " << vbucket << " from disk.";
        return ss.str();
    }
private:
    EventuallyPersistentStore *ep;
    uint16_t                   vbucket;

};

EventuallyPersistentStore::EventuallyPersistentStore(EventuallyPersistentEngine &theEngine,
                                                     StrategicSqlite3 *t,
                                                     bool startVb0) :
    engine(theEngine), stats(engine.getEpStats()), bgFetchDelay(0)
{
    doPersistence = getenv("EP_NO_PERSISTENCE") == NULL;
    dispatcher = new Dispatcher();
    nonIODispatcher = new Dispatcher();
    flusher = new Flusher(this, dispatcher);

    stats.memOverhead = sizeof(EventuallyPersistentStore);

    setTxnSize(DEFAULT_TXN_SIZE);

    underlying = t;

    if (startVb0) {
        RCPtr<VBucket> vb(new VBucket(0, active, stats));
        vbuckets.addBucket(vb);
    }

    startDispatcher();
    startFlusher();
    startNonIODispatcher();
    assert(underlying);
}

class VerifyStoredVisitor : public HashTableVisitor {
public:
    std::vector<std::string> dirty;
    void visit(StoredValue *v) {
        if (v->isDirty()) {
            dirty.push_back(v->getKey());
        }
    }
};

EventuallyPersistentStore::~EventuallyPersistentStore() {
    stopFlusher();
    dispatcher->stop();
    nonIODispatcher->stop();

    delete flusher;
    delete dispatcher;
    delete nonIODispatcher;
}

void EventuallyPersistentStore::startDispatcher() {
    dispatcher->start();
}

void EventuallyPersistentStore::startNonIODispatcher() {
    nonIODispatcher->start();
}

const Flusher* EventuallyPersistentStore::getFlusher() {
    return flusher;
}

void EventuallyPersistentStore::startFlusher() {
    flusher->start();
}

void EventuallyPersistentStore::stopFlusher() {
    bool rv = flusher->stop();
    if (rv) {
        flusher->wait();
    }
}

bool EventuallyPersistentStore::pauseFlusher() {
    flusher->pause();
    return true;
}

bool EventuallyPersistentStore::resumeFlusher() {
    flusher->resume();
    return true;
}

RCPtr<VBucket> EventuallyPersistentStore::getVBucket(uint16_t vbucket) {
    return vbuckets.getBucket(vbucket);
}

RCPtr<VBucket> EventuallyPersistentStore::getVBucket(uint16_t vbid,
                                                     vbucket_state_t wanted_state) {
    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    vbucket_state_t found_state(vb ? vb->getState() : dead);
    if (found_state == wanted_state) {
        return vb;
    } else {
        RCPtr<VBucket> rv;
        return rv;
    }
}

class Deleter {
public:
    Deleter(EventuallyPersistentStore *ep) : e(ep) {}
    void operator() (std::pair<uint16_t, std::string> vk) {
        RCPtr<VBucket> vb = e->getVBucket(vk.first);
        if (vb) {
            int bucket_num = vb->ht.bucket(vk.second);
            LockHolder lh(vb->ht.getMutex(bucket_num));

            StoredValue *v = vb->ht.unlocked_find(vk.second, bucket_num);
            if (v) {
                if (vb->ht.unlocked_softDelete(vk.second, bucket_num) == WAS_CLEAN) {
                    e->queueDirty(vk.second, vb->getId(), queue_op_del);
                }
            }
        }
    }
private:
    EventuallyPersistentStore *e;
};

void EventuallyPersistentStore::deleteMany(std::list<std::pair<uint16_t, std::string> > &keys) {
    // This can be made a lot more efficient, but I'd rather see it
    // show up in a profiling report first.
    std::for_each(keys.begin(), keys.end(), Deleter(this));
}

StoredValue *EventuallyPersistentStore::fetchValidValue(RCPtr<VBucket> vb,
                                                        const std::string &key,
                                                        int bucket_num,
                                                        bool wantDeleted) {
    StoredValue *v = vb->ht.unlocked_find(key, bucket_num, wantDeleted);
    if (v && v->isDeleted()) {
        // In the deleted case, we ignore expiration time.
        return v;
    } else if (v && v->isExpired(ep_real_time())) {
        ++stats.expired;
        if (vb->ht.unlocked_softDelete(key, bucket_num) == WAS_CLEAN) {
            queueDirty(key, vb->getId(), queue_op_del);
        }
        return NULL;
    }
    return v;
}

protocol_binary_response_status EventuallyPersistentStore::evictKey(const std::string &key,
                                                                    uint16_t vbucket,
                                                                    const char **msg) {
    RCPtr<VBucket> vb = getVBucket(vbucket);
    if (!(vb && vb->getState() == active)) {
        return PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET;
    }

    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = fetchValidValue(vb, key, bucket_num);

    protocol_binary_response_status rv(PROTOCOL_BINARY_RESPONSE_SUCCESS);

    if (v) {
        if (v->isResident()) {
            if (v->ejectValue(stats)) {
                ++stats.numValueEjects;
                ++stats.numNonResident;
                *msg = "Ejected.";
            } else {
                *msg = "Can't eject: Dirty or a small object.";
            }
        } else {
            *msg = "Already ejected.";
        }
    } else {
        *msg = "Not found.";
        rv = PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
    }

    return rv;
}

ENGINE_ERROR_CODE EventuallyPersistentStore::set(const Item &item,
                                                 const void *cookie,
                                                 bool force) {

    RCPtr<VBucket> vb = getVBucket(item.getVBucketId());
    if (!vb || vb->getState() == dead) {
        ++stats.numNotMyVBuckets;
        return ENGINE_NOT_MY_VBUCKET;
    } else if (vb->getState() == active) {
        // OK
    } else if(vb->getState() == replica && !force) {
        ++stats.numNotMyVBuckets;
        return ENGINE_NOT_MY_VBUCKET;
    } else if(vb->getState() == pending && !force) {
        if (vb->addPendingOp(cookie)) {
            return ENGINE_EWOULDBLOCK;
        }
    }

    bool cas_op = (item.getCas() != 0);

    mutation_type_t mtype = vb->ht.set(item, !force);

    switch(mtype) {
    case NOMEM:
        assert(!force);
        return ENGINE_ENOMEM;
        break;
    case INVALID_CAS:
    case IS_LOCKED:
        return ENGINE_KEY_EEXISTS;
        break;
    case WAS_DIRTY:
        // Do normal stuff, but don't enqueue dirty flags.
        break;
    case NOT_FOUND:
        if (cas_op) {
            return ENGINE_KEY_ENOENT;
        }
        // FALLTHROUGH
    case WAS_CLEAN:
        queueDirty(item.getKey(), item.getVBucketId(), queue_op_set);
        break;
    case INVALID_VBUCKET:
        return ENGINE_NOT_MY_VBUCKET;
        break;
    }

    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE EventuallyPersistentStore::add(const Item &item,
                                                 const void *cookie)
{
    RCPtr<VBucket> vb = getVBucket(item.getVBucketId());
    if (!vb || vb->getState() == dead || vb->getState() == replica) {
        ++stats.numNotMyVBuckets;
        return ENGINE_NOT_MY_VBUCKET;
    } else if (vb->getState() == active) {
        // OK
    } else if(vb->getState() == pending) {
        if (vb->addPendingOp(cookie)) {
            return ENGINE_EWOULDBLOCK;
        }
    }

    if (item.getCas() != 0) {
        // Adding with a cas value doesn't make sense..
        return ENGINE_NOT_STORED;
    }

    switch (vb->ht.add(item)) {
    case ADD_NOMEM:
        return ENGINE_ENOMEM;
    case ADD_EXISTS:
        return ENGINE_NOT_STORED;
    case ADD_SUCCESS:
    case ADD_UNDEL:
        queueDirty(item.getKey(), item.getVBucketId(), queue_op_set);
    }
    return ENGINE_SUCCESS;
}

void EventuallyPersistentStore::completeSetVBState(uint16_t vbid, const std::string &key) {
    if (!underlying->setVBState(vbid, key)) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                             "Rescheduling a task to set the state of vbucket %d in disc\n", vbid);
        dispatcher->schedule(shared_ptr<DispatcherCallback>(new SetVBStateCallback(this, vbid, key)),
                             NULL, Priority::VBucketPersistPriority, 5, false);
    }
}

void EventuallyPersistentStore::setVBucketState(uint16_t vbid,
                                                vbucket_state_t to) {
    // Lock to prevent a race condition between a failed update and add.
    LockHolder lh(vbsetMutex);
    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (vb) {
        vb->setState(to, engine.getServerApi());
        nonIODispatcher->schedule(shared_ptr<DispatcherCallback>
                                             (new NotifyVBStateChangeCallback(vb,
                                                                        engine.getServerApi())),
                                  NULL, Priority::NotifyVBStateChangePriority, 0, false);
        dispatcher->schedule(shared_ptr<DispatcherCallback>(new SetVBStateCallback(this, vb,
                                                                                   VBucket::toString(to))),
                             NULL, Priority::VBucketPersistPriority, 0, false);
    } else {
        RCPtr<VBucket> newvb(new VBucket(vbid, to, stats));
        vbuckets.addBucket(newvb);
    }
}

void EventuallyPersistentStore::completeVBucketDeletion(uint16_t vbid) {
    LockHolder lh(vbsetMutex);

    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (!vb || vb->getState() == dead || vbuckets.isBucketDeletion(vbid)) {
        lh.unlock();
        BlockTimer timer(&stats.diskVBDelHisto);
        if (underlying->delVBucket(vbid)) {
            vbuckets.setBucketDeletion(vbid, false);
            ++stats.vbucketDeletions;
        } else {
            ++stats.vbucketDeletionFail;
            getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                             "Rescheduling a task to delete vbucket %d from disk\n", vbid);
            dispatcher->schedule(shared_ptr<DispatcherCallback>(new VBucketDeletionCallback(this, vbid)),
                                 NULL, Priority::VBucketDeletionPriority, 10, false);
        }
    }
}

bool EventuallyPersistentStore::deleteVBucket(uint16_t vbid) {
    // Lock to prevent a race condition between a failed update and add (and delete).
    LockHolder lh(vbsetMutex);
    bool rv(false);

    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (vb && vb->getState() == dead) {
        lh.unlock();
        vbuckets.setBucketDeletion(vbid, true);
        HashTableStatVisitor statvis(vbuckets.removeBucket(vbid));
        stats.numNonResident.decr(statvis.numNonResident);
        stats.currentSize.decr(statvis.memSize);
        assert(stats.currentSize.get() < GIGANTOR);
        stats.totalCacheSize.decr(statvis.memSize);
        dispatcher->schedule(shared_ptr<DispatcherCallback>(new VBucketDeletionCallback(this, vbid)),
                             NULL, Priority::VBucketDeletionPriority, 0, false);
        rv = true;
    }
    return rv;
}

void EventuallyPersistentStore::completeBGFetch(const std::string &key,
                                                uint16_t vbucket,
                                                uint64_t rowid,
                                                const void *cookie,
                                                hrtime_t init, hrtime_t start) {
    --bgFetchQueue;
    ++stats.bg_fetched;
    std::stringstream ss;
    ss << "Completed a background fetch, now at " << bgFetchQueue.get()
       << std::endl;
    getLogger()->log(EXTENSION_LOG_DEBUG, NULL, ss.str().c_str());

    // Go find the data
    RememberingCallback<GetValue> gcb;

    underlying->get(key, rowid, gcb);
    gcb.waitForValue();
    assert(gcb.fired);

    // Lock to prevent a race condition between a fetch for restore and delete
    LockHolder lh(vbsetMutex);

    RCPtr<VBucket> vb = getVBucket(vbucket);
    if (vb && vb->getState() == active && gcb.val.getStatus() == ENGINE_SUCCESS) {
        int bucket_num = vb->ht.bucket(key);
        LockHolder vblh(vb->ht.getMutex(bucket_num));
        StoredValue *v = fetchValidValue(vb, key, bucket_num);

        if (v) {
            if (v->restoreValue(gcb.val.getValue()->getValue(), stats)) {
                --stats.numNonResident;
            }
        }
    }

    lh.unlock();

    hrtime_t stop = gethrtime();

    if (stop > start && start > init) {
        // skip the measurement if the counter wrapped...
        ++stats.bgNumOperations;
        hrtime_t w = (start - init) / 1000;
        stats.bgWaitHisto.add(w);
        stats.bgWait += w;
        stats.bgMinWait.setIfLess(w);
        stats.bgMaxWait.setIfBigger(w);

        hrtime_t l = (stop - start) / 1000;
        stats.bgLoadHisto.add(l);
        stats.bgLoad += l;
        stats.bgMinLoad.setIfLess(l);
        stats.bgMaxLoad.setIfBigger(l);
    }

    engine.getServerApi()->cookie->notify_io_complete(cookie, gcb.val.getStatus());
    delete gcb.val.getValue();
}

void EventuallyPersistentStore::bgFetch(const std::string &key,
                                        uint16_t vbucket,
                                        uint64_t rowid,
                                        const void *cookie) {
    shared_ptr<BGFetchCallback> dcb(new BGFetchCallback(this, key,
                                                        vbucket, rowid, cookie));
    ++bgFetchQueue;
    assert(bgFetchQueue > 0);
    std::stringstream ss;
    ss << "Queued a background fetch, now at " << bgFetchQueue.get()
       << std::endl;
    getLogger()->log(EXTENSION_LOG_DEBUG, NULL, ss.str().c_str());
    dispatcher->schedule(dcb, NULL, Priority::BgFetcherPriority, bgFetchDelay);
}

GetValue EventuallyPersistentStore::get(const std::string &key,
                                        uint16_t vbucket,
                                        const void *cookie,
                                        bool queueBG, bool honorStates) {
    RCPtr<VBucket> vb = getVBucket(vbucket);
    if (!vb) {
        ++stats.numNotMyVBuckets;
        return GetValue(NULL, ENGINE_NOT_MY_VBUCKET);
    } else if (honorStates && vb->getState() == dead) {
        ++stats.numNotMyVBuckets;
        return GetValue(NULL, ENGINE_NOT_MY_VBUCKET);
    } else if (vb->getState() == active) {
        // OK
    } else if(honorStates && vb->getState() == replica) {
        ++stats.numNotMyVBuckets;
        return GetValue(NULL, ENGINE_NOT_MY_VBUCKET);
    } else if(honorStates && vb->getState() == pending) {
        if (vb->addPendingOp(cookie)) {
            return GetValue(NULL, ENGINE_EWOULDBLOCK);
        }
    }

    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = fetchValidValue(vb, key, bucket_num);

    if (v) {
        // If the value is not resident, wait for it...
        if (!v->isResident()) {
            if (queueBG) {
                bgFetch(key, vbucket, v->getId(), cookie);
            }
            return GetValue(NULL, ENGINE_EWOULDBLOCK, v->getId());
        }

        // return an invalid cas value if the item is locked
        GetValue rv(new Item(v->getKey(), v->getFlags(), v->getExptime(),
                             v->getValue(),
                             v->isLocked(ep_current_time()) ? -1 : v->getCas(),
                             v->getId(), vbucket),
                    ENGINE_SUCCESS, v->getId());
        return rv;
    } else {
        GetValue rv;
        return rv;
    }
}

ENGINE_ERROR_CODE
EventuallyPersistentStore::getFromUnderlying(const std::string &key,
                                             uint16_t vbucket,
                                             const void *cookie,
                                             shared_ptr<Callback<GetValue> > cb) {
    RCPtr<VBucket> vb = getVBucket(vbucket);
    if (!vb || vb->getState() == dead) {
        ++stats.numNotMyVBuckets;
        return ENGINE_NOT_MY_VBUCKET;
    } else if (vb->getState() == active) {
        // OK
    } else if (vb->getState() == replica) {
        ++stats.numNotMyVBuckets;
        return ENGINE_NOT_MY_VBUCKET;
    } else if (vb->getState() == pending) {
        if (vb->addPendingOp(cookie)) {
            return ENGINE_EWOULDBLOCK;
        }
    }

    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = fetchValidValue(vb, key, bucket_num);

    if (v) {
        shared_ptr<VKeyStatBGFetchCallback> dcb(new VKeyStatBGFetchCallback(this, key,
                                                                            vbucket,
                                                                            v->getId(),
                                                                            cookie, cb));
        ++bgFetchQueue;
        assert(bgFetchQueue > 0);
        dispatcher->schedule(dcb, NULL, Priority::VKeyStatBgFetcherPriority, bgFetchDelay);
        return ENGINE_EWOULDBLOCK;
    } else {
        return ENGINE_KEY_ENOENT;
    }
}

bool EventuallyPersistentStore::getLocked(const std::string &key,
                                          uint16_t vbucket,
                                          Callback<GetValue> &cb,
                                          rel_time_t currentTime,
                                          uint32_t lockTimeout) {
    RCPtr<VBucket> vb = getVBucket(vbucket, active);
    if (!vb) {
        ++stats.numNotMyVBuckets;
        GetValue rv(NULL, ENGINE_NOT_MY_VBUCKET);
        cb.callback(rv);
        return false;
    }

    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = fetchValidValue(vb, key, bucket_num);

    if (v) {
        if (v->isLocked(currentTime)) {
            GetValue rv;
            cb.callback(rv);
            return false;
        }

        // acquire lock and increment cas value

        v->lock(currentTime + lockTimeout);

        Item *it = new Item(v->getKey(), v->getFlags(), v->getExptime(),
                            v->getValue(), v->getCas());

        it->setCas();
        v->setCas(it->getCas());

        GetValue rv(it);
        cb.callback(rv);

    } else {
        GetValue rv;
        cb.callback(rv);
    }
    return true;
}

bool EventuallyPersistentStore::getKeyStats(const std::string &key,
                                            uint16_t vbucket,
                                            struct key_stats &kstats)
{
    RCPtr<VBucket> vb = getVBucket(vbucket, active);
    if (!vb) {
        return false;
    }

    bool found = false;
    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = fetchValidValue(vb, key, bucket_num);

    found = (v != NULL);
    if (found) {
        kstats.dirty = v->isDirty();
        kstats.exptime = v->getExptime();
        kstats.flags = v->getFlags();
        kstats.cas = v->getCas();
        // TODO:  Know this somehow.
        kstats.dirtied = 0; // v->getDirtied();
        kstats.data_age = v->getDataAge();
        kstats.last_modification_time = ep_abs_time(v->getDataAge());
    }
    return found;
}

void EventuallyPersistentStore::setMinDataAge(int to) {
    stats.min_data_age.set(to);
}

void EventuallyPersistentStore::setQueueAgeCap(int to) {
    stats.queue_age_cap.set(to);
}

ENGINE_ERROR_CODE EventuallyPersistentStore::del(const std::string &key,
                                                 uint16_t vbucket,
                                                 const void *cookie) {
    RCPtr<VBucket> vb = getVBucket(vbucket);
    if (!vb || vb->getState() == dead) {
        ++stats.numNotMyVBuckets;
        return ENGINE_NOT_MY_VBUCKET;
    } else if (vb->getState() == active) {
        // OK
    } else if(vb->getState() == replica) {
        ++stats.numNotMyVBuckets;
        return ENGINE_NOT_MY_VBUCKET;
    } else if(vb->getState() == pending) {
        if (vb->addPendingOp(cookie)) {
            return ENGINE_EWOULDBLOCK;
        }
    }

    mutation_type_t delrv = vb->ht.softDelete(key);
    ENGINE_ERROR_CODE rv = delrv == NOT_FOUND ? ENGINE_KEY_ENOENT : ENGINE_SUCCESS;

    if (delrv == WAS_CLEAN) {
        queueDirty(key, vbucket, queue_op_del);
    }
    return rv;
}

void EventuallyPersistentStore::reset() {
    std::vector<int> buckets = vbuckets.getBuckets();
    std::vector<int>::iterator it;
    for (it = buckets.begin(); it != buckets.end(); ++it) {
        RCPtr<VBucket> vb = getVBucket(*it, active);
        if (vb) {
            HashTableStatVisitor statvis;
            vb->ht.visit(statvis);
            vb->ht.clear();
            stats.numNonResident.decr(statvis.numNonResident);
            stats.currentSize.decr(statvis.memSize);
            assert(stats.currentSize.get() < GIGANTOR);
            stats.totalCacheSize.decr(statvis.memSize);
        }
    }

    queueDirty("", 0, queue_op_flush);
}

std::queue<QueuedItem>* EventuallyPersistentStore::beginFlush() {
    std::queue<QueuedItem> *rv(NULL);
    if (towrite.empty() && writing.empty()) {
        stats.dirtyAge = 0;
    } else {
        assert(underlying);
        towrite.getAll(writing);
        stats.flusher_todo.set(writing.size());
        stats.queue_size.set(towrite.size());
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Flushing %d items with %d still in queue\n",
                         writing.size(), towrite.size());
        rv = &writing;
    }
    return rv;
}

void EventuallyPersistentStore::completeFlush(std::queue<QueuedItem> *rej,
                                              rel_time_t flush_start) {
    // Requeue the rejects.
    stats.queue_size.incr(rej->size());
    while (!rej->empty()) {
        writing.push(rej->front());
        rej->pop();
    }

    stats.queue_size.set(towrite.size() + writing.size());
    rel_time_t complete_time = ep_current_time();
    stats.flushDuration.set(complete_time - flush_start);
    stats.flushDurationHighWat.set(std::max(stats.flushDuration.get(),
                                            stats.flushDurationHighWat.get()));
    stats.cumulativeFlushTime.incr(complete_time - flush_start);
}

int EventuallyPersistentStore::flushSome(std::queue<QueuedItem> *q,
                                         std::queue<QueuedItem> *rejectQueue) {
    int tsz = getTxnSize();
    underlying->begin();
    int oldest = stats.min_data_age;
    for (int i = 0; i < tsz && !q->empty() && bgFetchQueue == 0; i++) {
        int n = flushOne(q, rejectQueue);
        if (n != 0 && n < oldest) {
            oldest = n;
        }
    }
    if (bgFetchQueue > 0) {
        ++stats.flusherPreempts;
    }
    rel_time_t cstart = ep_current_time();
    BlockTimer timer(&stats.diskCommitHisto);
    while (!underlying->commit()) {
        sleep(1);
        ++stats.commitFailed;
    }
    ++stats.flusherCommits;
    rel_time_t complete_time = ep_current_time();

    stats.commit_time.set(complete_time - cstart);
    stats.cumulativeCommitTime.incr(complete_time - cstart);
    return oldest;
}

// This class exists to create a closure around a few variables within
// EventuallyPersistentStore::flushOne so that an object can be
// requeued in case of failure to store in the underlying layer.

class PersistenceCallback : public Callback<mutation_result>,
                            public Callback<int> {
public:

    PersistenceCallback(const QueuedItem &qi, std::queue<QueuedItem> *q,
                        EventuallyPersistentStore *st,
                        rel_time_t qd, rel_time_t d, EPStats *s) :
        queuedItem(qi), rq(q), store(st), queued(qd), dirtied(d), stats(s) {
        assert(rq);
        assert(s);
    }

    // This callback is invoked for set only.
    void callback(mutation_result &value) {
        if (value.first == 1) {
            if (value.second > 0) {
                ++stats->newItems;
                setId(value.second);
            }
            RCPtr<VBucket> vb = store->getVBucket(queuedItem.getVBucketId());
            if (vb && vb->getState() != active) {
                int bucket_num = vb->ht.bucket(queuedItem.getKey());
                LockHolder lh(vb->ht.getMutex(bucket_num));
                StoredValue *v = store->fetchValidValue(vb, queuedItem.getKey(),
                                                        bucket_num, true);
                double current = static_cast<double>(StoredValue::getCurrentSize(*stats));
                double lower = static_cast<double>(stats->mem_low_wat);
                if (v && current > lower) {
                    v->ejectValue(*stats);
                }
            }
        } else {
            // If the return was 0 here, we're in a bad state because
            // we do not know the rowid of this object.
            if (value.first == 0) {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "Persisting vb%d, returned 0 updates for ``%s''\n",
                                 queuedItem.getVBucketId(), queuedItem.getKey().c_str());
            } else {
                redirty();
            }
        }
    }

    // This callback is invoked for deletions only.
    //
    // The boolean indicates whether the underlying storage
    // successfully deleted the item.
    void callback(int &value) {
        // > 1 would be bad.  We were only trying to delete one row.
        assert(value < 2);
        // -1 means fail
        // 1 means we deleted one row
        // 0 means we did not delete a row, but did not fail (did not exist)
        if (value >= 0) {
            if (value > 0) {
                ++stats->delItems;
            }
            // We have succesfully removed an item from the disk, we
            // may now remove it from the hash table.
            RCPtr<VBucket> vb = store->getVBucket(queuedItem.getVBucketId());
            if (vb) {
                int bucket_num = vb->ht.bucket(queuedItem.getKey());
                LockHolder lh(vb->ht.getMutex(bucket_num));
                StoredValue *v = store->fetchValidValue(vb, queuedItem.getKey(),
                                                        bucket_num, true);

                if (v && v->isDeleted()) {
                    bool deleted = vb->ht.unlocked_del(queuedItem.getKey(),
                                                       bucket_num);
                    assert(deleted);
                } else if (v) {
                    v->clearId();
                }
            }
        } else {
            redirty();
        }
    }

private:

    void setId(int64_t id) {
        bool did = store->invokeOnLockedStoredValue(queuedItem.getKey(),
                                                    queuedItem.getVBucketId(),
                                                    std::mem_fun(&StoredValue::setId),
                                                    id);
        if (!did) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Failed to set id on vb%d ``%s''\n",
                             queuedItem.getVBucketId(), queuedItem.getKey().c_str());
        }
    }

    void redirty() {
        stats->memOverhead.incr(queuedItem.size());
        assert(stats->memOverhead.get() < GIGANTOR);
        ++stats->flushFailed;
        store->invokeOnLockedStoredValue(queuedItem.getKey(),
                                         queuedItem.getVBucketId(),
                                         std::mem_fun(&StoredValue::reDirty),
                                         dirtied);
        rq->push(queuedItem);
    }

    const QueuedItem queuedItem;
    std::queue<QueuedItem> *rq;
    EventuallyPersistentStore *store;
    rel_time_t queued;
    rel_time_t dirtied;
    EPStats *stats;
    DISALLOW_COPY_AND_ASSIGN(PersistenceCallback);
};

int EventuallyPersistentStore::flushOneDeleteAll() {
    underlying->reset();
    return 1;
}

// While I actually know whether a delete or set was intended, I'm
// still a bit better off running the older code that figures it out
// based on what's in memory.
int EventuallyPersistentStore::flushOneDelOrSet(QueuedItem &qi,
                                           std::queue<QueuedItem> *rejectQueue) {

    RCPtr<VBucket> vb = getVBucket(qi.getVBucketId());
    if (!vb) {
        return 0;
    }

    int bucket_num = vb->ht.bucket(qi.getKey());
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = fetchValidValue(vb, qi.getKey(), bucket_num, true);

    int64_t rowid = v != NULL ? v->getId() : -1;
    bool found = v != NULL;
    bool deleted = found && v->isDeleted();
    bool isDirty = found && v->isDirty();
    Item *val = NULL;
    rel_time_t queued(qi.getDirtied()), dirtied(0);

    int ret = 0;

    if (isDirty && v->isExpired(ep_real_time() + engine.getItemExpiryWindow())) {
        ++stats.flushExpired;
        v->markClean(&dirtied);
        isDirty = false;
    }

    if (isDirty) {
        v->markClean(&dirtied);
        // Calculate stats if this had a positive time.
        rel_time_t now = ep_current_time();
        int dataAge = now - dirtied;
        int dirtyAge = now - queued;
        bool eligible = true;

        if (v->isPendingId()) {
            eligible = false;
        } else if (dirtyAge > stats.queue_age_cap.get()) {
            ++stats.tooOld;
        } else if (dataAge < stats.min_data_age.get()) {
            eligible = false;
            // Skip this one.  It's too young.
            ret = stats.min_data_age.get() - dataAge;
            ++stats.tooYoung;
        }

        if (eligible) {
            assert(dirtyAge < (86400 * 30));
            stats.dirtyAge.set(dirtyAge);
            stats.dataAge.set(dataAge);
            stats.dirtyAgeHighWat.set(std::max(stats.dirtyAge.get(),
                                               stats.dirtyAgeHighWat.get()));
            stats.dataAgeHighWat.set(std::max(stats.dataAge.get(),
                                              stats.dataAgeHighWat.get()));
            // Copy it for the duration.
            if (!deleted) {
                assert(rowid == v->getId());
                val = new Item(qi.getKey(), v->getFlags(), v->getExptime(),
                               v->getValue(), v->getCas(), rowid,
                               qi.getVBucketId());
            }

            if (rowid == -1) {
                v->setPendingId();
            }
        } else {
            isDirty = false;
            v->reDirty(dirtied);
            rejectQueue->push(qi);
            stats.memOverhead.incr(qi.size());
            assert(stats.memOverhead.get() < GIGANTOR);
        }
    }

    lh.unlock();

    if (isDirty && !deleted) {
        // If vbucket deletion is currently being flushed, don't flush a set operation,
        // but requeue a set operation to a flusher to avoid duplicate items on disk
        if (vbuckets.isBucketDeletion(qi.getVBucketId())) {
            towrite.push(qi);
            stats.memOverhead.incr(qi.size());
            assert(stats.memOverhead.get() < GIGANTOR);
            ++stats.totalEnqueued;
            stats.queue_size = towrite.size();
        } else {
            BlockTimer timer(rowid == -1 ? &stats.diskInsertHisto : &stats.diskUpdateHisto);
            PersistenceCallback cb(qi, rejectQueue, this, queued, dirtied, &stats);
            underlying->set(*val, cb);
        }
    } else if (deleted) {
        BlockTimer timer(&stats.diskDelHisto);
        PersistenceCallback cb(qi, rejectQueue, this, queued, dirtied, &stats);
        if (rowid > 0) {
            underlying->del(qi.getKey(), rowid, cb);
        } else {
            // bypass deletion if missing items, but still call the
            // deletion callback for clean cleanup.
            int affected(0);
            cb.callback(affected);
        }
    }

    if (val != NULL) {
        delete val;
    }

    return ret;
}

int EventuallyPersistentStore::flushOne(std::queue<QueuedItem> *q,
                                        std::queue<QueuedItem> *rejectQueue) {

    QueuedItem qi = q->front();
    q->pop();
    stats.memOverhead.decr(qi.size());
    assert(stats.memOverhead.get() < GIGANTOR);
    stats.flusher_todo--;

    int rv = 0;
    switch (qi.getOperation()) {
    case queue_op_flush:
        rv = flushOneDeleteAll();
        break;
    case queue_op_set:
        // FALLTHROUGH
    case queue_op_del:
        rv = flushOneDelOrSet(qi, rejectQueue);
        break;
    }

    return rv;

}

void EventuallyPersistentStore::queueDirty(const std::string &key, uint16_t vbid,
                                           enum queue_operation op) {
    if (doPersistence) {
        // Assume locked.
        QueuedItem qi(key, vbid, op);
        towrite.push(qi);
        stats.memOverhead.incr(qi.size());
        assert(stats.memOverhead.get() < GIGANTOR);
        ++stats.totalEnqueued;
        stats.queue_size = towrite.size();
    }
}

void LoadStorageKVPairCallback::initVBucket(uint16_t vbid,
                                            vbucket_state_t state) {
    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (!vb) {
        vb.reset(new VBucket(vbid, state, stats));
        vbuckets.addBucket(vb);
    }
}

void LoadStorageKVPairCallback::callback(GetValue &val) {
    Item *i = val.getValue();
    if (i != NULL) {
        RCPtr<VBucket> vb = vbuckets.getBucket(i->getVBucketId());
        if (!vb) {
            vb.reset(new VBucket(i->getVBucketId(), pending, stats));
            vbuckets.addBucket(vb);
        }
        bool retain(shouldBeResident());
        bool succeeded(false);

        switch (vb->ht.add(*i, false, retain)) {
        case ADD_SUCCESS:
        case ADD_UNDEL:
            // Yay
            succeeded = true;
            break;
        case ADD_EXISTS:
            // Boo
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Warmup dataload error: Duplicate key: %s.\n",
                             i->getKey().c_str());
            ++stats.warmDups;
            succeeded = true;
            break;
        case ADD_NOMEM:
            if (hasPurged) {
                if (++stats.warmOOM == 1) {
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "Warmup dataload failure: max_size too low.\n");
                }
            } else {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "Emergency startup purge to free space for load.\n");
                purge();
                // Try that item again.
                switch(vb->ht.add(*i, false, retain)) {
                case ADD_SUCCESS:
                case ADD_UNDEL:
                    succeeded = true;
                    break;
                case ADD_EXISTS:
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "Warmup dataload error: Duplicate key: %s.\n",
                                     i->getKey().c_str());
                    ++stats.warmDups;
                    succeeded = true;
                    break;
                case ADD_NOMEM:
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "Cannot store an item after emergency purge.\n");
                    ++stats.warmOOM;
                    break;
                default:
                    abort();
                }
            }
            break;
        default:
            abort();
        }

        if (succeeded && !retain) {
            ++stats.numValueEjects;
            ++stats.numNonResident;
        }

        delete i;
    }
    ++stats.warmedUp;
}

void LoadStorageKVPairCallback::purge() {

    class EmergencyPurgeVisitor : public HashTableVisitor {
    public:
        EmergencyPurgeVisitor(EPStats &s) : stats(s) {}

        void visit(StoredValue *v) {
            if (v->ejectValue(stats)) {
                ++stats.numValueEjects;
                ++stats.numNonResident;
            }
        }
    private:
        EPStats &stats;
    };

    std::vector<int> vbucketIds(vbuckets.getBuckets());
    std::vector<int>::iterator it;
    EmergencyPurgeVisitor epv(stats);
    for (it = vbucketIds.begin(); it != vbucketIds.end(); ++it) {
        int vbid = *it;
        RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
        if (vb) {
            vb->ht.visit(epv);
        }
    }
    hasPurged = true;
}
