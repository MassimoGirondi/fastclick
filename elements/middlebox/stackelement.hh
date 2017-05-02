#ifndef MIDDLEBOX_STACKELEMENT_HH
#define MIDDLEBOX_STACKELEMENT_HH
#include <click/config.h>
#include <click/element.hh>
#include <click/flowelement.hh>
#include <click/router.hh>
#include <click/routervisitor.hh>

CLICK_DECLS

/*
=c

StackElement()

=s middlebox

base class for the stack of the middlebox

=d

This element provides a common abstract base for the elements of the stack of the middlebox.
It provides useful methods and the mechanism of function stack. This element is not meant to be used
directly in a click configuration. Instead, use elements that inherit from it.

To use the function stack, simply call one of the methods using this mechanism and the method
will be called automatically on upstream elements until an IPIn element is reached. For instance,
to remove bytes in a packet, elements can simply call removeBytes, giving it the right parameters
and the method will be called on upstream elements that will handle the request and act
consequently.

Elements that inherit from this class can override the processPacket method to define their own
behaviour.

*/

class StackElement : public VirtualFlowBufferElement
{
public:
    friend class PathMerger;
    friend class FlowBuffer;
    friend class FlowBufferContentIter;

    /**
     * @brief Construct a StackElement
     * StackElement must not be instanciated directly. Consider it as an abstract element.
     */
    StackElement() CLICK_COLD;

    /**
     * @brief Destruct a StackElement
     */
    ~StackElement() CLICK_COLD;

    // Click related methods
    const char *class_name() const        { return "StackElement"; }
    const char *port_count() const        { return PORTS_1_1; }
    const char *processing() const        { return "h/hh"; }
    virtual const size_t flow_data_size() const { return 0; };
    void* cast(const char*);
    int initialize(ErrorHandler*)   CLICK_COLD;

    // Custom methods

    /**
     * @brief Indicate whether the element is an exit point element of the stack
     * @return A boolean indicating whether the element is an exit point element of the stack
     */
    virtual bool isOutElement()          { return false; }

    /**
     * @brief Method used during the building of the function stack. It sets the element
     * on which we must call the corresponding method to propagate the call in the stack
     * @param element The next element (upstream) in the function stack
     * @param port The input port connected to this element
     */
    virtual void addStackElementInList(StackElement* element, int port);

    /**
     * @brief Indicate whether an element is a stack element (which inherits from StackElement)
     * @param element The element to check
     * @return A boolean indicating whether an element is a stack element
     */
    static bool isStackElement(Element* element);

protected:

    /**
     * @brief Set the value of the LAST_USEFUL annotation
     * @param packet The packet
     * @param value The new value for the annotation
     */
    void setAnnotationLastUseful(Packet* packet, bool value) const;

    /**
     * @brief Get the value of the LAST_USEFUL annotation
     * @param packet The packet
     * @return The value of the annotation
     */
    bool getAnnotationLastUseful(Packet* packet) const;

    /**
     * @brief Get the offset at which the current useful content starts.
     * It depends on the elements by which the packet went through. For instance, after TCPIn,
     * the offset points to the TCP payload. After HTTPIn, the offset points to the body
     * of the HTTP request/response.
     * @param packet The packet
     * @return The current offset of the content
     */
    uint16_t getContentOffset(Packet* packet) const;

    /**
     * @brief Get the current useful content of the packet
     * It depends on the elements by which the packet went through. For instance, after TCPIn,
     * it points to the TCP payload. After HTTPIn, it points to the body
     * of the HTTP request/response.
     * @param packet The packet
     * @return A pointer to the constant current useful content of the packet
     */
    const unsigned char* getPacketContentConst(Packet* packet) const;

    /**
     * @brief Get the current useful content of the packet
     * It depends on the elements by which the packet went through. For instance, after TCPIn,
     * it points to the TCP payload. After HTTPIn, it points to the body
     * of the HTTP request/response.
     * @param packet The packet
     * @return A pointer to the current useful content of the packet
     */
    unsigned char* getPacketContent(WritablePacket* packet) const;

    /**
     * @brief Search a given pattern in the given content. The content does not have
     * to be NULL-terminated.
     * @param content The content in which the pattern will be searched
     * @param pattern The pattern to search
     * @param length The length of the content
     * @return A pointer to the first byte of pattern in the content or NULL if it cannot be found
     */
    char* searchInContent(char *content, const char *pattern, uint32_t length);

    /**
     * @brief Set the offset at which the current useful content starts.
     * @param packet The packet
     * @param offset The new offset
     */
    void setContentOffset(Packet* packet, uint16_t offset) const;

    /**
     * @brief Set the INITIAL_ACK annotation of the packet. This annotation stores the initial
     * ACK number that the packet had before modification.
     * @param packet The packet
     * @param initialAck The initial ACK number of the packet
     */
    void setInitialAck(Packet *packet, uint32_t initialAck) const;

    /**
     * @brief Get the value of the INITIAL_ACK annotation of the packet.
     * This annotation stores the initial ACK number that the packet had before modification.
     * @param packet The packet
     * @return The initial ACK number of the packet
     */
    uint32_t getInitialAck(Packet *packet) const;

    /**
     * @brief Indicate whether the packet has useful content for the current protocol
     * @param The packet
     * @return A boolean indicating if the useful content of the packet is empty
     */
    bool isPacketContentEmpty(Packet* packet) const;

    /**
     * @brief Return the size of the useful content of the packet
     * @param The packet
     * @return The size of the useful content of the packet
     */
    uint16_t getPacketContentSize(Packet *packet) const;

    /**
     * @brief Used to create the function stack. It will run a StackVisitor
     * downstream that will register this element as the next element in the function stack
     * of the next stack element.
     */
    void buildFunctionStack();

    // Methods using the function stack mechanism

    /**
     * @brief Remove bytes in a packet
     * @param fcb A pointer to the FCB of the flow
     * @param packet The packet
     * @param position The position (relative to the current useful content)
     * @param length Number of bytes to remove
     */
    virtual void removeBytes(WritablePacket* packet, uint32_t position,
        uint32_t length);

    /**
     * @brief Insert bytes in a packet. This method creates room for the new bytes and moves
     * the content after the insertion point so that it is after the new bytes.
     * @param fcb A pointer to the FCB of the flow
     * @param packet The packet
     * @param position The position (relative to the current useful content)
     * @param length Number of bytes to insert
     * @return A pointer to the packet with the bytes inserted (can be different from the given
     * pointer)
     */
    virtual WritablePacket* insertBytes(WritablePacket* packet, uint32_t position,
        uint32_t length) CLICK_WARN_UNUSED_RESULT;

    /**
     * @brief Request more packets. Must be used by objects that buffer packets to ensure that
     * they will receive the next packets
     * @param fcb A pointer to the FCB of the flow
     * @param packet The packet
     * @param force A boolean indicating whether the request must be repeated if it as already
     * been done for this packet (default: false)
     */
    virtual void requestMorePackets(Packet *packet, bool force = false);

    /**
     * @brief Indicate that a given packet has exited the middlebox
     * @param fcb A pointer to the FCB of the flow
     * @param packet The packet
     */
    virtual void packetSent(Packet* packet);

    /**
     * @brief Close the connection
     * @param fcb A pointer to the FCB of the flow
     * @param packet The packet
     * @param grafecul A boolean indicating whether the connection must be closed gracefully or not
     * @param bothSides A boolean indicating if the connection must be closed for both sides or
     * just this one.
     */
    virtual void closeConnection(WritablePacket *packet, bool graceful,
        bool bothSides);

    /**
     * @brief Indicate whether a given packet is the last useful one for this side of the flow
     * @param fcb A pointer to the FCB of the flow
     * @param packet The packet
     * @return A boolean indicating whether a given packet is the last useful one for this side
     * of the flow
     */
    virtual bool isLastUsefulPacket(Packet *packet);

    /**
     * @brief Determine the flow ID for this path (0 or 1).
     * Each side of a TCP connection has a different flow direction (0 for one of them and 1
     * for the other).
     * This ID is defined in the Click configuration.
     * @return An unsigned int representing the ID (called direction) of the flow in the connection
     */
    virtual unsigned int determineFlowDirection();

private:
    /**
     * @brief Set a bit in the annotation byte used to store booleans. This annotation is used
     * to store up to 8 boolean values
     * @param packet The packet
     * @param bit The offset of the bit (in [0, 7])
     * @param value The new value of the bit
     */
    void setAnnotationBit(Packet* packet, int bit, bool value) const;

    /**
     * @brief Return the value of a bit in the annotation byte used to store booleans. This
     * annotation is used to store up to 8 boolean values
     * @param packet The packet
     * @param bit The offset of the bit (in [0, 7])
     * @return Value of the bit
     */
    bool getAnnotationBit(Packet* packet, int bit) const;

    StackElement *previousStackElement; // Previous stack element in the configuration path
                                        // and therefore next element in the function stack.

    // Constants
    // Up to 8 booleans can be stored in the corresponding annotation (see setAnnotationBit)
    const int OFFSET_ANNOTATION_LASTUSEFUL = 0; // Indicates if a packet is the last useful one

};


template<typename T>
class StackBufferElement : public StackElement {
public :
    StackBufferElement();

    ~StackBufferElement();


    virtual int initialize(ErrorHandler *errh) {
        if (_flow_data_offset == -1) {
            return errh->error("No SFCBAssigner() element sets the flow context for %s !",name().c_str());
        }
        return 0;
    }

    virtual const size_t flow_data_size()  const { return sizeof(T); }

//Todo : rename as fcb data
    inline T* fcb() {
        T* flowdata = static_cast<T*>((void*)&fcb_stack->data[_flow_data_offset]);
        return flowdata;
    }

    void push_batch(int port,PacketBatch* head) final {
        push_batch(port, fcb(), head);
    }

    virtual void push_batch(int port, T* flowdata, PacketBatch* head) = 0;

 };


/**
 * @brief This class defines a RouterVisitor that will be used to build the function stack
 * Each element starts a visitor downsteam so that when the visitor reaches the next StackElement,
 * the object that started the visitor will be registered as the next element in the function stack
 * of the visited element.
 */
class StackVisitor : public RouterVisitor
{
public:
    /**
     * @brief Construct a StackVisitor
     */
    StackVisitor(StackElement* startElement)
    {
        this->startElement = startElement;
    }

    /**
     * @brief Destruct a StackVisitor
     */
    ~StackVisitor()
    {

    }

    /**
     * @brief Visit the path of elements until we find a stack element. We will indicate to this
     * element that we are the next element in the function stack so that it will propagate
     * the calls to us. See the Click documentation for the description of the parameters
     */
    bool visit(Element *e, bool, int port, Element*, int, int)
    {
        // Check that the element is a stack element
        // If this is not the case, we skip it and continue the traversing
        if(!StackElement::isStackElement(e))
            return true;

        // We now know that we have a stack element so we can cast it
        StackElement *element = reinterpret_cast<StackElement*>(e);

        // Add the starting element in the list of the current element
        click_chatter("Adding element %s as predecessor of %s", startElement->class_name(),
            element->class_name());
        element->addStackElementInList(startElement, port);

        // Stop search when we encounter the IPOut Element
        if(strcmp(element->class_name(), "IPOut") == 0)
            return false;

        // Stop the traversing
        return false;
    }

private:
    StackElement* startElement; // Element that started the visit
};

template<typename T>
StackBufferElement<T>::StackBufferElement() : StackElement() {

}

template<typename T>
StackBufferElement<T>::~StackBufferElement() {

}

CLICK_ENDDECLS

#endif
