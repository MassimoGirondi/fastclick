#include <click/config.h>
#include <click/router.hh>
#include <click/args.hh>
#include <click/error.hh>
#include <clicknet/tcp.h>
#include "tcpin.hh"

CLICK_DECLS

TCPIn::TCPIn() : outElement(NULL), returnElement(NULL),
    poolModificationNodes(MODIFICATIONNODES_POOL_SIZE),
    poolModificationLists(MODIFICATIONLISTS_POOL_SIZE)
{
}

int TCPIn::configure(Vector<String> &conf, ErrorHandler *errh)
{
    String returnName = "";
    String outName = "";
    int flowDirectionParam = -1;

    if(Args(conf, this, errh)
    .read_p("FLOWDIRECTION", flowDirectionParam)
    .read_p("OUTNAME", outName)
    .read_p("RETURNNAME", returnName)
    .complete() < 0)
        return -1;

    if(flowDirectionParam == -1 || outName == "" || returnName == "")
    {
        click_chatter("Missing parameter(s): TCPIn requires three parameters (FLOWDIRECTION, OUTNAME and RETURNNAME)");
        return -1;
    }

    Element* returnElement = this->router()->find(returnName, errh);
    Element* outElement = this->router()->find(outName, errh);

    if(returnElement == NULL)
    {
        click_chatter("Error: Could not find TCPIn element "
            "called \"%s\".", returnName.c_str());
        return -1;
    }
    else if(outElement == NULL)
    {
        click_chatter("Error: Could not find TCPOut element "
            "called \"%s\".", outName.c_str());
        return -1;
    }
    else if(strcmp("TCPIn", returnElement->class_name()) != 0)
    {
        click_chatter("Error: Element \"%s\" is not a TCPIn element "
            "but a %s element.", returnName.c_str(), returnElement->class_name());
        return -1;
    }
    else if(strcmp("TCPOut", outElement->class_name()) != 0)
    {
        click_chatter("Error: Element \"%s\" is not a TCPOut element "
            "but a %s element.", outName.c_str(), outElement->class_name());
        return -1;
    }
    else if(flowDirectionParam != 0 && flowDirectionParam != 1)
    {
        click_chatter("Error: FLOWDIRECTION %u is not valid.", flowDirectionParam);
        return -1;
    }
    else
    {
        this->returnElement = (TCPIn*)returnElement;
        this->outElement = (TCPOut*)outElement;
        this->outElement->setInElement(this);
        setFlowDirection((unsigned int)flowDirectionParam);
        this->outElement->setFlowDirection(getFlowDirection());
    }

    return 0;
}

void TCPIn::push_batch(int port, fcb_tcpin* fcb, PacketBatch* flow)
{
    auto fnt = [this,fcb](Packet* p){
        // Ensure that the pointers in the FCB are set
        if (fcb->poolModificationNodes == NULL)
            fcb->poolModificationNodes = &poolModificationNodes;

        if (fcb->poolModificationLists == NULL)
            fcb->poolModificationLists = &poolModificationLists;

        WritablePacket *packet = p->uniqueify();

        const click_tcp *tcph = packet->tcp_header();

        // Compute the offset of the TCP payload
        unsigned tcph_len = tcph->th_off << 2;
        uint16_t offset = (uint16_t) (packet->transport_header() + tcph_len - packet->data());
        setContentOffset(packet, offset);

        // Update the ack number according to the bytestreammaintainer of the other direction
        tcp_seq_t ackNumber = getAckNumber(packet);
        tcp_seq_t newAckNumber = fcb->maintainers[getOppositeFlowDirection()].mapAck(ackNumber);

        if (ackNumber != newAckNumber) {
            click_chatter("Ack number %u becomes %u in flow %u", ackNumber, newAckNumber, flowDirection);
            setAckNumber(packet, newAckNumber);
            setPacketModified(packet);
        } else {
            click_chatter("Ack number %u stays the same in flow %u", ackNumber, flowDirection);
        }
        return packet;
    };
    EXECUTE_FOR_EACH_PACKET(fnt,flow);
    output(0).push_batch(flow);

}

TCPOut* TCPIn::getOutElement()
{
    return outElement;
}

TCPIn* TCPIn::getReturnElement()
{
    return returnElement;
}

void TCPIn::closeConnection(fcb_tcpin* fcb, uint32_t saddr, uint32_t daddr, uint16_t sport,
                             uint16_t dport, tcp_seq_t seq, tcp_seq_t ack, bool graceful)
{
    closeConnection(fcb, saddr, daddr, sport, dport, seq, ack, graceful, true);
}

void TCPIn::closeConnection(fcb_tcpin* fcb, uint32_t saddr, uint32_t daddr, uint16_t sport,
                             uint16_t dport, tcp_seq_t seq, tcp_seq_t ack, bool graceful, bool initiator)
{
    // TODO: ungraceful part (RST)

    // Initiator indicates which side of the connection closes it (us or the other side)

    Packet *packet = forgePacket(saddr, daddr, sport, dport, seq, ack, TH_FIN);
    push(0, packet);
    fcb->closingState = TCPClosingState::FIN_WAIT;

    if(initiator)
        returnElement->closeConnection(fcb, daddr, saddr, dport, sport, ack, seq, graceful, false);
}

ModificationList* TCPIn::getModificationList(fcb_tcpin* fcb, WritablePacket* packet)
{
    HashTable<tcp_seq_t, ModificationList*> modificationLists = fcb->modificationLists;

    ModificationList* list = fcb->modificationLists.get(getSequenceNumber(packet));

    // If no list was assigned to this packet, create a new one
    if(list == NULL)
    {
        ModificationList* listPtr = fcb->poolModificationLists->getMemory();
        // Call the constructor manually to have a clean object
        list = new(listPtr) ModificationList(&poolModificationNodes);
        fcb->modificationLists.set(getSequenceNumber(packet), list);
    }

    return list;
}

bool TCPIn::hasModificationList(fcb_tcpin* fcb, Packet* packet)
{
    HashTable<tcp_seq_t, ModificationList*> modificationLists = fcb->modificationLists;

    ModificationList* list = fcb->modificationLists.get(getSequenceNumber(packet));

    return (list != NULL);
}

void TCPIn::removeBytes(WritablePacket* packet, uint32_t position, uint32_t length)
{
    click_chatter("Removing %u bytes", length);
    ModificationList* list = getModificationList(fcb(), packet);

    tcp_seq_t seqNumber = getSequenceNumber(packet);

    list->addModification(seqNumber + position, -((int)length));

    unsigned char *source = packet->data();
    uint32_t bytesAfter = packet->length() - position;

    memmove(&source[position], &source[position + length], bytesAfter);
    packet->take(length);

    // Continue in the stack function
    StackElement::removeBytes(packet, position, length);
}

void TCPIn::insertBytes(WritablePacket* packet, uint32_t position, uint32_t length)
{
    click_chatter("Adding %u bytes", length);

    tcp_seq_t seqNumber = getSequenceNumber(packet);

    getModificationList(fcb(), packet)->addModification(seqNumber + position, (int)length);

    unsigned char *source = packet->data();
    uint32_t bytesAfter = packet->length() + position;

    packet = packet->put(length);
    assert(packet != NULL);

    memmove(&source[position + length], &source[position], bytesAfter);

    // Continue in the stack function
    StackElement::insertBytes(packet, position, length);
}

void TCPIn::requestMoreBytes()
{
    //TODO

    // Continue in the stack function
    StackElement::requestMoreBytes();
}

void TCPIn::setPacketModified(WritablePacket* packet)
{
    // Annotate the packet to indicate it has been modified
    // While going through "out elements", the checksum will be recomputed
    setAnnotationModification(packet, true);

    // Continue in the stack function
    StackElement::setPacketModified(packet);
}

unsigned int TCPIn::determineFlowDirection()
{
    return getFlowDirection();
}

TCPIn::~TCPIn()
{

}

CLICK_ENDDECLS
EXPORT_ELEMENT(TCPIn)
//ELEMENT_MT_SAFE(TCPIn)
