/*
 * FlowIPNAT.{cc,hh}
 */

#include <click/config.h>
#include <click/glue.hh>
#include <click/args.hh>
#include <click/flow.hh>
#include <clicknet/ip.h>
#include <clicknet/tcp.h>
#include "flowipnat.hh"

CLICK_DECLS


#define DEBUG_LB 0
int _offset = 0;

inline void rewrite_ips(WritablePacket* q, IPPair pair) {
    assert(q->network_header());
    uint16_t *x = reinterpret_cast<uint16_t *>(&q->ip_header()->ip_src);
    uint32_t old_hw = (uint32_t) x[0] + x[1] + x[2] + x[3];
    old_hw += (old_hw >> 16);

    memcpy(x, &pair, 8);

    uint32_t new_hw = (uint32_t) x[0] + x[1] + x[2] + x[3];
    new_hw += (new_hw >> 16);
    click_ip *iph = q->ip_header();
    click_update_in_cksum(&iph->ip_sum, old_hw, new_hw);
    click_update_in_cksum(&q->tcp_header()->th_sum, old_hw, new_hw);

}


FlowIPNAT::FlowIPNAT() : _sip(){

};

FlowIPNAT::~FlowIPNAT() {

}

int
FlowIPNAT::configure(Vector<String> &conf, ErrorHandler *errh)
{
    if (Args(conf, this, errh)
               .read("SIP",_sip)
               .complete() < 0)
        return -1;

    return 0;
}


int FlowIPNAT::initialize(ErrorHandler *errh) {

    return 0;
}

void FlowIPNAT::push_batch(int port, IPPair* flowdata, PacketBatch* batch) {
    if (flowdata->src == 0) {
#if DEBUG_NAT
        click_chatter("New flow");
#endif
        auto ip = batch->ip_header();
        IPAddress osip = IPAddress(ip->ip_src);
        IPAddress odip = IPAddress(ip->ip_dst);
        auto th = batch->tcp_header();
        NATEntry entry = NATEntry(flowdata->dst, th->th_sport);
        _map.find_insert(entry, IPPair(odip,osip));
#if DEBUG_LB
        click_chatter("Adding entry %s %d [%d]",entry.dst.unparse().c_str(),entry.port);
#endif
        fcb_acquire();
    }

    EXECUTE_FOR_EACH_PACKET([flowdata](Packet*p) -> Packet*{
        WritablePacket* q=p->uniqueify();
        rewrite_ips(q, *flowdata);
        q->set_dst_ip_anno(flowdata->dst);
        return q;
    }, batch);

    checked_output_push_batch(*flowdata, batch);
}

int
FlowIPNATReverse::configure(Vector<String> &conf, ErrorHandler *errh)
{
    if (Args(conf, this, errh)
                .read_mp("NAT",_in)
                .complete() < 0)
        return -1;

    return 0;
}

void FlowIPNATReverse::push_batch(int port, IPPair* flowdata, PacketBatch* batch) {
    IPAddress mapped;
    if (flowdata->src == IPAddress(0)) {
        auto ip = batch->ip_header();
        auto th = batch->tcp_header();
        LBEntry entry = LBEntry(ip->ip_src, th->th_dport);
#if IPLOADBALANCER_MP
        LBHashtable::ptr ptr = _lb->_map.find(entry);
#else
        LBHashtable::iterator ptr = _lb->_map.find(entry);
#endif
        if (!ptr) {

#if DEBUG_LB
            click_chatter("Could not find %s %d",IPAddress(ip->ip_src).unparse().c_str(),th->th_dport);
#endif
            //assert(false);
            //checked_output_push_batch(0, batch);
            batch->kill();
            return;
        } else{
#if DEBUG_LB
            click_chatter("Found entry %s %d : %s -> %s",entry.chosen_server.unparse().c_str(),entry.port,ptr->src.unparse().c_str(),ptr->dst.unparse().c_str());
#endif
        }
#if IPLOADBALANCER_MP
        *flowdata = *ptr;
#else
        *flowdata = ptr.value();
#endif
        fcb_acquire();
    } else {
        mapped = *flowdata;
        if (mapped == 0)
            output_push_batch(0,batch);
    }

    EXECUTE_FOR_EACH_PACKET([flowdata](Packet*p) -> Packet*{
            WritablePacket* q=p->uniqueify();
            rewrite_ips(q, *flowdata);
            q->set_dst_ip_anno(flowdata->dst);
            return q;
        }, batch);

    checked_output_push_batch(0, batch);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(flow)
EXPORT_ELEMENT(FlowIPNATReverse)
ELEMENT_MT_SAFE(FlowIPNATReverse)
EXPORT_ELEMENT(FlowIPNAT)
ELEMENT_MT_SAFE(FlowIPNAT)
