#ifndef CLICK_CHECKTCPHEADER_HH
#define CLICK_CHECKTCPHEADER_HH
#include <click/batchelement.hh>
#include <click/atomic.hh>
CLICK_DECLS

/*
=c

CheckTCPHeader([I<KEYWORDS>])

=s tcp

checks TCP header on TCP/IP packets

=d

Expects TCP/IP packets as input. Checks that the TCP header length and
checksum fields are valid. Pushes invalid packets out on output 1, unless
output 1 was unused; if so, drops invalid packets.

Prints a message to the console the first time it encounters an incorrect
packet (but see VERBOSE below).

Keyword arguments are:

=over 5

=item VERBOSE

Boolean. If it is true, then a message will be printed for every erroneous
packet, rather than just the first. False by default.

=item DETAILS

Boolean. If it is true, then CheckTCPHeader will maintain detailed counts of
how many packets were dropped for each possible reason, accessible through the
C<drop_details> handler. False by default.

=item CHECKSUM

Boolean. If it is true, the TCP checksum is validated. True by default.

=back

=h count read-only

Returns the number of correct packets CheckTCPHeader has seen.

=h drops read-only

Returns the number of incorrect packets CheckTCPHeader has seen.

=h drop_details read-only

Returns a text file showing how many erroneous packets CheckTCPHeader has
seen, subdivided by error. Only available if the DETAILS keyword argument was
true.

=a CheckIPHeader, CheckUDPHeader, CheckICMPHeader, MarkIPHeader */

class CheckTCPHeader : public SimpleElement<CheckTCPHeader> {
    public:
        CheckTCPHeader() CLICK_COLD;
        ~CheckTCPHeader() CLICK_COLD;

        const char *class_name() const { return "CheckTCPHeader"; }
        const char *port_count() const { return PORTS_1_1X2; }
        const char *processing() const { return PROCESSING_A_AH; }

        int configure(Vector<String> &, ErrorHandler *) CLICK_COLD;
        void add_handlers() CLICK_COLD;

        Packet *simple_action(Packet *p);

    private:
        bool _verbose : 1;
        bool _checksum : 1;
        atomic_uint64_t _count;
        atomic_uint64_t _drops;
        atomic_uint64_t *_reason_drops;

        enum Reason {
            NOT_TCP,
            BAD_LENGTH,
            BAD_CHECKSUM,
            NREASONS
        };
        static const char *reason_texts[NREASONS];

        enum { h_count, h_drops, h_drop_details };

        Packet *drop(Reason reason, Packet *p);
        static String read_handler(Element *e, void *thunk) CLICK_COLD;
};

CLICK_ENDDECLS
#endif
