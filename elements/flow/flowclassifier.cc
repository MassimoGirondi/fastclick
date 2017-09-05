/*
 * flowclassifier.{cc,hh}
 */

#include <click/config.h>
#include <click/glue.hh>
#include <click/args.hh>
#include <click/routervisitor.hh>
#include <click/flowelement.hh>
#include "flowclassifier.hh"
#include "flowdispatcher.hh"
#include <click/idletask.hh>

CLICK_DECLS


class FlowBufferVisitor : public RouterVisitor {
public:
    size_t data_size;
    FlowClassifier* _classifier;
    static int shared_position[NR_SHARED_FLOW];
    Bitvector map;
    bool display_assignation;

    FlowBufferVisitor(FlowClassifier* classifier,size_t myoffset) : data_size(myoffset), _classifier(classifier), map(64,false) {
        map.set_range(0, myoffset, 1);
    }


    bool visit(Element *e, bool isoutput, int port,
                   Element *from_e, int from_port, int distance) {
        FlowElement* fe = dynamic_cast<FlowElement*>(e);
        if (fe != NULL) {
            fe->_classifier = _classifier;
        }

        VirtualFlowSpaceElement* fbe = dynamic_cast<VirtualFlowSpaceElement*>(e);
        if (fbe != NULL) { //The visited element is an element that need FCB space

            //Resize the map if needed
            if (fbe->flow_data_offset() + fbe->flow_data_size() >= map.size()) map.resize(map.size() * 2);

            if (fbe->flow_data_offset() != -1) { //If flow already have some classifier
                if (fbe->flow_data_offset() >= data_size) {
                    data_size = fbe->flow_data_offset() + fbe->flow_data_size();
                } else {
                    if (map.weight_range(fbe->flow_data_offset(),fbe->flow_data_size()) > 0 && !(fbe->flow_data_index() >= 0 && shared_position[fbe->flow_data_index()] == fbe->flow_data_offset())) {
                        //TODO !
                        /*click_chatter("ERROR : multiple assigner can assign flows for %s at overlapped positions, "
                                              "use the RESERVE parameter to provision space.",e->name().c_str());
                        click_chatter("Vector : %s, trying offset %d length %d",map.unparse().c_str(),fbe->flow_data_offset(), fbe->flow_data_size());
                        e->router()->please_stop_driver();*/
                    }
                    if (data_size < fbe->flow_data_offset() + fbe->flow_data_size()) {
                        data_size = fbe->flow_data_offset() + fbe->flow_data_size();
                    }
                }
            } else {
                if (fbe->flow_data_index() >= 0) {
                    if (shared_position[fbe->flow_data_index()] == -1) {
                        fbe->_flow_data_offset = data_size;
                        shared_position[fbe->flow_data_index()] = data_size;
                        data_size += fbe->flow_data_size();
                    } else {
                        fbe->_flow_data_offset = shared_position[fbe->flow_data_index()];
                    }
                } else {
                    fbe->_flow_data_offset = data_size;
                    data_size += fbe->flow_data_size();
#if DEBUG_CLASSIFIER
                    click_chatter("%s from %d to %d",e->name().c_str(),fbe->_flow_data_offset,data_size);
#endif
                }
                fbe->_classifier = _classifier;
            }
            map.set_range(fbe->flow_data_offset(),fbe->flow_data_size(),true);
            if (display_assignation)
                click_chatter("Adding %d bytes for %s at %d",fbe->flow_data_size(),e->name().c_str(),fbe->flow_data_offset());
        } else {
            /*Bitvector b;
            e->port_flow(false,port,&b);*/
            const char *f = e->router()->flow_code_override(e->eindex());
            if (!f)
                f = e->flow_code();
            if (strcmp(f,Element::COMPLETE_FLOW) != 0) {
#if DEBUG_CLASSIFIER > 0
                click_chatter("%p{element}: Unstacking flows from port %d",e,port);
#endif
                const_cast<Element::Port&>(from_e->port(true,from_port)).set_unstack(true);
                return false;
            }
        }
        return true;
    }
};

FlowClassifier::FlowClassifier(): _aggcache(false), _cache(),_cache_size(4096), _cache_ring_size(8),_pull_burst(0),_builder(true),_collision_is_life(false), cache_miss(0),cache_sharing(0),cache_hit(0),_clean_timer(5000), _timer(this) {
    in_batch_mode = BATCH_MODE_NEEDED;
#if DEBUG_CLASSIFIER
    _verbose = 3;
#else
    _verbose = 0;
#endif
}

FlowClassifier::~FlowClassifier() {

}

int
FlowClassifier::configure(Vector<String> &conf, ErrorHandler *errh)
{
    int reserve = 0;

    if (Args(conf, this, errh)
            .read_p("AGGCACHE",_aggcache)
            .read_p("RESERVE",reserve)
            .read("CACHESIZE", _cache_size)
            .read("CACHERINGSIZE", _cache_ring_size)
            .read("BUILDER",_builder)
            .read("AGGTRUST",_collision_is_life)
            .read("VERBOSE",_verbose)
#if HAVE_FLOW_RELEASE_SLOPPY_TIMEOUT
            .read("CLEAN_TIMER",_clean_timer)
#endif
            .complete() < 0)
        return -1;

    FlowBufferVisitor v(this, sizeof(FlowNodeData) + (_aggcache?sizeof(uint32_t):0) + reserve);
    v.display_assignation = _verbose > 1;
    router()->visit_ports(this,true,-1,&v);
#if DEBUG_CLASSIFIER
    click_chatter("%s : pool size %d",name().c_str(),v.data_size);
#endif
    _table.get_pool()->initialize(v.data_size, &_table);
    _cache_mask = _cache_size - 1;
    if ((_cache_size & _cache_mask) != 0)
        return errh->error("Chache size must be a power of 2 !");

    return 0;
}

#if HAVE_FLOW_RELEASE_SLOPPY_TIMEOUT
void FlowClassifier::run_timer(Timer*) {
#if DEBUG_CLASSIFIER_RELEASE
    click_chatter("Force run check-release");
#endif
    fcb_table = &_table;
    _table.check_release();
    fcb_table = 0;
    _timer.reschedule_after_msec(_clean_timer);
}
#endif


void release_subflow(FlowControlBlock* fcb, void* thunk) {
#if DEBUG_CLASSIFIER_RELEASE
    click_chatter("Release from tree fcb %p data %d, parent %p",fcb,fcb->data_64[0],fcb->parent);
#endif
    if (fcb->parent == NULL) return;

    //A -> B -> F

    //Parent of the fcb is release_ptr
    flow_assert(thunk);
    static_cast<FlowClassifier*>(thunk)->remove_cache_fcb(fcb);
    FlowNode* child = static_cast<FlowNode*>(fcb->parent); //Child is B
    flow_assert(child->getNum() == child->findGetNum());
    flow_assert(fcb->parent);
    FlowNodeData data = *fcb->node_data;
    child->release_child(FlowNodePtr(fcb), data); //B->release(F)
#if DEBUG_CLASSIFIER_RELEASE
    fcb->parent = 0;
#endif
    flow_assert(child->getNum() == child->findGetNum());
    data = child->node_data; //Data is B data inside A

    FlowNode* parent = child->parent(); //Parent is A
    //If parent is dynamic, we cannot be the child of a default
    //Release nodes up to the root
    int up = 0;
    while (parent && parent->level()->is_dynamic() && child->getNum() == 0) { //A && B is empty and A is dynamic (so B was a duplicate as it comes from a FCB)
#if DEBUG_CLASSIFIER_RELEASE
        click_chatter("[%d] Releasing parent %s's child %p, growing %d, num %d, child is type %s num %d",up,parent->name().c_str(), child, parent->growing(), parent->getNum(),child->name().c_str(),child->getNum());
#endif
        parent->check(true);
        assert(child->level()->is_dynamic());
        if (parent->growing() && !child->growing() && child == parent->default_ptr()->ptr) { //If child is default root, it is the default child table of a growing table and must not be deleted
            click_chatter("Non growing child of its growing original, not deleting");
            //parent->release_child(FlowNodePtr(child), data);
            flow_assert(parent->getNum() == parent->findGetNum());
            break;
        }
        if (child->growing()) {
            click_chatter("Releasing a growing child, we can remove it from the tree. Parent num is %d",parent->getNum());
            flow_assert(parent->getNum() == parent->findGetNum());
            FlowNode* subchild = child->default_ptr()->node;
            child->default_ptr()->ptr = 0; //Remove the default to prevent deletion
            if (child == parent->default_ptr()->ptr) { //Growing child of growing
                click_chatter("Default");
                child->set_parent(0);
                child->destroy();
                parent->default_ptr()->ptr = subchild;
            } else { //Growing child of normal, we cannot swap pointer because find could give a different bucket
                click_chatter("Child");
                parent->release_child(FlowNodePtr(child), data); //A->release(B)
                flow_assert(parent->getNum() == parent->findGetNum());
                parent->find(data)->set_node(subchild);
                subchild->node_data = data;
                parent->inc_num();
            }
            subchild->set_parent(parent);
            flow_assert(parent->getNum() == parent->findGetNum());
            parent->check(true);
            break;
        }

        flow_assert(parent->getNum() == parent->findGetNum());
        parent->release_child(FlowNodePtr(child), data); //A->release(B)
#if DEBUG_CLASSIFIER_CHECK
        if (parent->getNum() != parent->findGetNum()) {
            click_chatter("Parent has %d != %d counted childs",parent->getNum(),parent->findGetNum());
            assert(parent->getNum() == parent->findGetNum());
        }
#endif
        parent->check(true);
        //parent->print();
        //click_chatter("Releasing parent");
        child = parent; //Child is A
        data = child->node_data;
        parent = child->parent(); //Parent is 0
        up++;
    };
}



int FlowClassifier::initialize(ErrorHandler *errh) {

    //click_chatter("%s : %d %d",name().c_str(),ninputs(),noutputs());
    //click_chatter("Pull : %d Push : %d",input_is_pull(0),input_is_push(0));
    if (input_is_pull(0)) {
        assert(input(0).element());
    }
    auto passing = get_passing_threads();
    _table.get_pool()->compress(passing);

    FlowNode* table = FlowElementVisitor::get_downward_table(this, 0);

    if (!table)
       return errh->error("%s: FlowClassifier without any downward dispatcher?",name().c_str());

    _table.set_release_fnt(release_subflow,this);
    _table.set_root(table->optimize(passing.weight() <= 1));
    _table.get_root()->check();
    if (_verbose) {
        click_chatter("Table of %s after optimization :",name().c_str());
        _table.get_root()->print(-1,false);
    }
    FCBPool::initialized --;

    _table.get_root()->traverse_all_leaves([this](FlowNodePtr* ptr) {
        FlowControlBlock* nfcb = _table.get_pool()->allocate();
        FlowNode* p = ptr->parent();
        memcpy(nfcb->data, ptr->leaf->data, _table.get_pool()->data_size());
        nfcb->parent = ptr->leaf->parent;
        nfcb->flags = ptr->leaf->flags;
        nfcb->acquire(1);
        nfcb->release_fnt = 0;
        delete ptr->leaf;
        ptr->leaf = nfcb;
    }, true, true);

    if (_aggcache) {
        for (unsigned i = 0; i < _cache.weight(); i++) {
            _cache.get_value(i) = (FlowCache*)CLICK_LALLOC(sizeof(FlowCache) * _cache_size * _cache_ring_size);
            bzero(_cache.get_value(i), sizeof(FlowCache) * _cache_size * _cache_ring_size);
        }
    }


#if HAVE_FLOW_RELEASE_SLOPPY_TIMEOUT
    for (unsigned i = 0; i < click_max_cpu_ids(); i++) {
        IdleTask* idletask = new IdleTask(this);
        idletask->initialize(this, i, 100);
    }
    //todo : INIT timer if needed? The current solution seems ok
#endif

    return 0;
}

void FlowClassifier::cleanup(CleanupStage stage) {
    fcb_table = &_table;
    _table.get_root()->traverse_all_leaves([this](FlowNodePtr* ptr) {
        _table.get_pool()->release(ptr->leaf);
        ptr->leaf = 0;
    }, true, true);
    fcb_table = 0;
    /*click_chatter("%p{element} Hit : %d",this,cache_hit);
    click_chatter("%p{element}  Shared : %d",this,cache_sharing);
    click_chatter("%p{element} Miss : %d",this,cache_miss);*/
}

#define USE_CACHE_RING 1

bool FlowClassifier::run_idle_task(IdleTask*) {
#if HAVE_FLOW_RELEASE_SLOPPY_TIMEOUT
//#if !HAVE_DYNAMIC_FLOW_RELEASE_FNT
    fcb_table = &_table;
//#endif
#if DEBUG_CLASSIFIER_TIMEOUT > 0
    click_chatter("%p{element} Idle release",this);
#endif
    bool work_done = _table.check_release();
//#if !HAVE_DYNAMIC_FLOW_RELEASE_FNT
    fcb_table = 0;
//#endif
    //return work_done;
#endif
    return false; //No reschedule
}

inline void FlowClassifier::remove_cache_fcb(FlowControlBlock* fcb) {
    if (!_aggcache)
        return;
    uint32_t agg = *((uint32_t*)&fcb->node_data[1]);
    uint16_t hash = (agg ^ (agg >> 16)) & _cache_mask;
#if USE_CACHE_RING
        FlowCache* bucket = _cache.get() + (hash * _cache_ring_size);
        FlowCache* c = bucket;
        int ic = 0;
        do {
            if (c->agg == agg && c->fcb == fcb) {
                c->agg = 0;
                return;
            }
            ic++;
            c++;
        } while (ic < _cache_ring_size);
#else
        FlowCache* bucket = _cache.get() + hash;
        if (bucket->agg == agg) {
            bucket->agg = 0;
        }
#endif

}
inline FlowControlBlock* FlowClassifier::get_cache_fcb(Packet* p, uint32_t agg) {

    if (unlikely(agg == 0)) {
        return _table.match(p,false);
    }

#if DEBUG_CLASSIFIER > 1
    click_chatter("Aggregate %d",agg);
#endif
        FlowControlBlock* fcb = 0;
        uint16_t hash = (agg ^ (agg >> 16)) & _cache_mask;
#if USE_CACHE_RING
        FlowCache* bucket = _cache.get() + hash * _cache_ring_size;
#else
        FlowCache* bucket = _cache.get() + hash;
#endif
        FlowCache* c = bucket;
        int ic = 0;
        do {
            if (c->agg == 0) { //Empty slot
    #if DEBUG_CLASSIFIER > 1
                    click_chatter("Cache miss !");
    #endif
                cache_miss++;
                fcb = _table.match(p,_aggcache);
                if (c->agg == 0) {
                    c->fcb = fcb;
                    c->agg = agg;
                    *((uint32_t*)&fcb->node_data[1]) = agg;
                }
                return fcb;
            } else { //Non empty slot
                if (likely(c->agg == agg)) { //Good agg
                    if (likely(_collision_is_life || _table.reverse_match(c->fcb, p))) {
        #if DEBUG_CLASSIFIER > 1
                        click_chatter("Cache hit");
        #endif
                        cache_hit++;
                        fcb = c->fcb;
                        return fcb;
                        //OK
                    } else { //The fcb for that agg does not match !

                        cache_sharing++;
                        fcb = _table.match(p,_aggcache);
#if DEBUG_CLASSIFIER > 1
                        assert(fcb != c->fcb);
                        click_chatter("Cache %d shared for agg %d : fcb %p %p!",hash,agg,fcb,c->fcb);
                        //for (int i = 0; i < 10; i++)
                            //click_chatter("%x",*(((uint32_t*)p->data()) + i));

#endif
                        return fcb;
                        /*FlowControlBlock* firstfcb = fcb;
                        int sechash = (agg >> 12) & 0xfff;
                        fcb = _cache.get()[sechash + 4096];
                        if (fcb && !fcb->released()) {
                            if (_table.reverse_match(fcb, p)) {

                            } else {
                                click_chatter("Double collision for hash %d!",hash);
                                fcb = _table.match(p);
                                if (_cache.get()[hash]->lastseen < fcb->lastseen)
                                    _cache.get()[hash] = fcb;
                                else
                                    _cache.get()[sechash] = fcb;
                            }
                        } else {
                            fcb = _table.match(p);
                            _cache.get()[sechash] = fcb;
                        }*/
                    }
                } //Continue if bad agg
            }
            c++;
            ic++;
#if !USE_CACHE_RING
        } while (false);
        fcb = _table.match(p,_aggcache);
        c->agg = agg;
        c->fcb = fcb;
        return fcb;
#else

        } while (ic < _cache_ring_size); //Try to put in the ring of the bucket

        //Remove the oldest from the bucket
        c = bucket;
        FlowCache* oldest = c;
        c++;
        ic = 1;
        int o = 0;
        while (ic < _cache_ring_size) {
            if (c->fcb->lastseen < oldest->fcb->lastseen) {
                o = ic;
                oldest = c;
            }

            c++;
            ic++;
        }
        //click_chatter("Oldest is %d",o );
        c = bucket;
        if (o != 0) {
            oldest->agg = c->agg;
            oldest->fcb = c->fcb;
        }

        #if DEBUG_CLASSIFIER > 1
        click_chatter("Cache miss with full ring !");
        #endif
        cache_miss++;
        fcb = _table.match(p,_aggcache);
        c->agg = agg;
        c->fcb = fcb;
        *((uint32_t*)&fcb->node_data[1]) = agg;
        return fcb;
#endif
}



static inline void check_fcb_still_valid(FlowControlBlock* fcb, Timestamp now) {
#if HAVE_FLOW_RELEASE_SLOPPY_TIMEOUT
            if (unlikely(fcb->count() == 0 && fcb->hasTimeout())) {
# if DEBUG_CLASSIFIER_TIMEOUT > 0
                assert(fcb->flags & FLOW_TIMEOUT_INLIST);
# endif
                if (fcb->timeoutPassed(now)) {
# if DEBUG_CLASSIFIER_TIMEOUT > 1
                    click_chatter("Timeout of %p passed or released and is now seen again, reinitializing timer",fcb);
# endif
                    //Do not call initialize as everything is still set, just reinit timeout
                    fcb->flags = FLOW_TIMEOUT | FLOW_TIMEOUT_INLIST;
                } else {
# if DEBUG_CLASSIFIER_TIMEOUT > 1
                    click_chatter("Timeout recovered, keeping the flow %p",fcb);
# endif
                }
            } else {
# if DEBUG_CLASSIFIER_TIMEOUT > 1
                click_chatter("Fcb %p Still valid : Fcb count is %d and hasTimeout %d",fcb, fcb->count(),fcb->hasTimeout());
# endif
            }
#endif
}

/**
 * Push batch simple simply classify packets and push a batch when a packet
 * is different
 */
inline  void FlowClassifier::push_batch_simple(int port, PacketBatch* batch) {
    PacketBatch* awaiting_batch = NULL;
    Packet* p = batch;
    Packet* last = NULL;
    uint32_t lastagg = 0;
    Timestamp now = Timestamp::recent_steady();

    int count =0;
    FlowControlBlock* fcb = 0;
#if DEBUG_CLASSIFIER > 1
    click_chatter("Simple have %d packets.",batch->count());
#endif
    while (p != NULL) {
#if DEBUG_CLASSIFIER > 1
        click_chatter("Packet %p in %s",p,name().c_str());
#endif
        Packet* next = p->next();

        if (_aggcache) {
            uint32_t agg = AGGREGATE_ANNO(p);
            if (!(lastagg == agg && fcb && likely(_table.reverse_match(fcb,p))))
                fcb = get_cache_fcb(p,agg);
        }
        else
        {
            fcb = _table.match(p,_aggcache);
        }
        if (_verbose > 2) {
            click_chatter("Table of %s after getting fcb %p :",name().c_str(),fcb);
            _table.get_root()->print(-1,false);
        }
        if (unlikely(!fcb || fcb->is_early_drop())) {
            debug_flow("Early drop !");
            if (last) {
                last->set_next(next);
            }
            SFCB_STACK(p->kill(););
            p = next;
            continue;
        }
        check_fcb_still_valid(fcb, now);
        if (awaiting_batch == NULL) {
#if DEBUG_CLASSIFIER > 1
            click_chatter("New fcb %p",fcb);
#endif
            fcb_stack = fcb;
            awaiting_batch = PacketBatch::start_head(p);
        } else {
            if (fcb == fcb_stack) {
#if DEBUG_CLASSIFIER > 1
        click_chatter("Same fcb %p",fcb);
#endif
                //Do nothing as we follow the LL
            } else {
#if DEBUG_CLASSIFIER > 1
        click_chatter("Different fcb %p, last was %p",fcb,fcb_stack);
#endif
                fcb_stack->acquire(count);
                last->set_next(0);
                awaiting_batch->set_tail(last);
                awaiting_batch->set_count(count);
                fcb_stack->lastseen = now;
                output_push_batch(port,awaiting_batch);
                awaiting_batch = PacketBatch::start_head(p);
                fcb_stack = fcb;
                count = 0;
            }
        }

        count ++;

        last = p;
        p = next;
    }

    if (awaiting_batch) {
        fcb_stack->acquire(count);
        fcb_stack->lastseen = now;
        last->set_next(0);
        awaiting_batch->set_tail(last);
        awaiting_batch->set_count(count);
        output_push_batch(port,awaiting_batch);
        fcb_stack = 0;
    }
}



#define RING_SIZE 16

/**
 * Push_batch_builder use double connection to build a ring and process packets trying to reconcile flows more
 *
 */
inline void FlowClassifier::push_batch_builder(int port, PacketBatch* batch) {
    Packet* p = batch;
    int curbatch = -1;
    Packet* last = NULL;
    FlowControlBlock* fcb = 0;
    FlowControlBlock* lastfcb = 0;
    uint32_t lastagg = 0;
    int count = 0;

    FlowBatch batches[RING_SIZE];
    int head = 0;
    int tail = 0;

    Timestamp now = Timestamp::recent_steady();
    process:


    //click_chatter("Builder have %d packets.",batch->count());

    while (p != NULL) {
#if DEBUG_CLASSIFIER > 1
        click_chatter("Packet %p in %s",p,name().c_str());
#endif
        Packet* next = p->next();

        if (_aggcache) {
            uint32_t agg = AGGREGATE_ANNO(p);
            if (!(lastagg == agg && fcb && likely(_table.reverse_match(fcb,p))))
                fcb = get_cache_fcb(p,agg);
        }
        else
        {
            fcb = _table.match(p,_aggcache);
        }
        if (_verbose > 2) {
            click_chatter("Table of %s after getting fcb %p :",name().c_str(),fcb);
            _table.get_root()->print(-1,false);
        }
        if (unlikely(!fcb || fcb->is_early_drop())) {
            debug_flow("Early drop !");
            if (last) {
                last->set_next(next);
            }
            SFCB_STACK(p->kill(););
            p = next;
            continue;
        }
        check_fcb_still_valid(fcb, now);
        if (lastfcb == fcb) {
            //Just continue as they are still linked
        } else {

                //Break the last flow
                if (last) {
                    last->set_next(0);
                    batches[curbatch].batch->set_count(count);
                    batches[curbatch].batch->set_tail(last);
                }

                //Find a potential match
                for (int i = tail; i < head; i++) {
                    if (batches[i % RING_SIZE].fcb == fcb) { //Flow already in list, append
                        //click_chatter("Flow already in list");
                        curbatch = i % RING_SIZE;
                        count = batches[curbatch].batch->count();
                        last = batches[curbatch].batch->tail();
                        last->set_next(p);
                        goto attach;
                    }
                }
                //click_chatter("Unknown fcb %p, curbatch = %d",fcb,head);
                curbatch = head % RING_SIZE;
                head++;

                if (tail % RING_SIZE == head % RING_SIZE) {
                    auto &b = batches[tail % RING_SIZE];
                    if (_verbose > 1) {
                        click_chatter("WARNING (unoptimized) Ring full with batch of %d packets, processing now !", b.batch->count());
                    }
                    //Ring full, process batch NOW
                    fcb_stack = b.fcb;
                    fcb_stack->acquire(b.batch->count());
                    fcb_stack->lastseen = now;
                    //click_chatter("FPush %d of %d packets",tail % RING_SIZE,batches[tail % RING_SIZE].batch->count());
                    output_push_batch(port,b.batch);
                    tail++;
                }
                //click_chatter("batches[%d].batch = %p",curbatch,batch);
                //click_chatter("batches[%d].fcb = %p",curbatch,fcb);
                batches[curbatch].batch = PacketBatch::start_head(p);
                batches[curbatch].fcb = fcb;
                count = 0;
        }
        attach:
        count ++;
        last = p;
        p = next;
        lastfcb = fcb;
    }


    if (last) {
        last->set_next(0);
        batches[curbatch].batch->set_tail(last);
        batches[curbatch].batch->set_count(count);
    }

    //click_chatter("%d batches :",head-tail);
    for (int i = tail;i < head;i++) {
        //click_chatter("[%d] %d packets for fcb %p",i,batches[i%RING_SIZE].batch->count(),batches[i%RING_SIZE].fcb);
    }
    for (;tail < head;) {
        auto &b = batches[tail % RING_SIZE];
        fcb_stack = b.fcb;
        fcb_stack->acquire(b.batch->count());
        fcb_stack->lastseen = now;
        //click_chatter("EPush %d of %d packets",tail % RING_SIZE,batches[tail % RING_SIZE].batch->count());
        output_push_batch(port,b.batch);

        curbatch = -1;
        lastfcb = 0;
        count = 0;
        lastagg = 0;

        tail++;
        if (tail == head) break;

        if (input_is_pull(0) && ((batch = input_pull_batch(0,_pull_burst)) != 0)) { //If we can pull and it's not the last pull
            //click_chatter("Pull continue because received %d ! tail %d, head %d",batch->count(),tail,head);
            p = batch;
            goto process;
        }
    }
    fcb_stack = 0;
    //click_chatter("End");

}


void FlowClassifier::push_batch(int port, PacketBatch* batch) {
//#if !HAVE_DYNAMIC_FLOW_RELEASE_FNT
    fcb_table = &_table;
//#endif
    if (_builder)
        push_batch_builder(port,batch);
    else
        push_batch_simple(port,batch);
#if HAVE_FLOW_RELEASE_SLOPPY_TIMEOUT
    auto &head = _table.old_flows.get();
    if (head.count() > head._count_thresh) {
#if DEBUG_CLASSIFIER_TIMEOUT > 0
        click_chatter("%p{element} Forced release because %d is > than %d",this,head.count(), head._count_thresh);
#endif
        _table.check_release();
        if (unlikely(head.count() < (head._count_thresh / 8) && head._count_thresh > FlowTableHolder::fcb_list::DEFAULT_THRESH)) {
            head._count_thresh /= 2;
        } else
            head._count_thresh *= 2;
#if DEBUG_CLASSIFIER_TIMEOUT > 0
        click_chatter("%p{element} Forced release count %d thresh %d",this,head.count(), head._count_thresh);
#endif
    }
#endif
//#if !HAVE_DYNAMIC_FLOW_RELEASE_FNT
    fcb_table = 0;
//#endif

}

int FlowBufferVisitor::shared_position[NR_SHARED_FLOW] = {-1};

CLICK_ENDDECLS
ELEMENT_REQUIRES(flow)
EXPORT_ELEMENT(FlowClassifier)
