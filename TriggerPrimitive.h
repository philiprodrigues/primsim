#ifndef TRIGGERPRIMITIVE_H
#define TRIGGERPRIMITIVE_H

#include "czmq.h"

#include <iostream>
#include <cstring> // For memcpy
#include <vector>

//======================================================================
class TriggerPrimitive
{
public:
    TriggerPrimitive()
        : timestamp(0),
          apa(0),
          channel(0),
          time_over_threshold(0),
          charge(0),
          seqno(0)
    {

    }


    // Constructor including sequence number
    TriggerPrimitive(uint64_t _timestamp, uint16_t _apa, uint16_t _channel,
                     uint16_t _time_over_threshold, uint16_t _charge, uint32_t _seqno=0)
        : timestamp(_timestamp),
          apa(_apa),
          channel(_channel),
          time_over_threshold(_time_over_threshold),
          charge(_charge),
          seqno(_seqno)
    {

    }

    void send(zsock_t* socket) const
    {
        zframe_t *message = zframe_new(this, sizeof(TriggerPrimitive));
        int rc = zframe_set_group(message, "all");
        assert (rc == 0);
        // zframe_send destroys the message for us so we don't have to
        rc = zframe_send (&message, socket, 0);
        
        if(rc!=0){
            std::cout << "Failed sending. rc=" << rc << " timestamp=" << timestamp << std::endl;
        }
    }

    // TODO: Wrap the return value in some appropriate smart pointer
    static TriggerPrimitive* from_socket(zsock_t* socket)
    {
        zframe_t *message=zframe_recv(socket);
        if(!message){
            return nullptr;
        }
        assert( zframe_size(message)==sizeof(TriggerPrimitive) );

        void* mem=malloc(sizeof(TriggerPrimitive));
        TriggerPrimitive* p=new(mem) TriggerPrimitive;
        memcpy(mem, zframe_data(message), sizeof(TriggerPrimitive));
        zframe_destroy(&message);
        return p;
    }

    friend std::ostream& operator<<(std::ostream& os, const TriggerPrimitive& prim);

    uint64_t timestamp;
    uint16_t apa; // The APA number
    uint16_t channel;
    uint16_t time_over_threshold;
    uint16_t charge; // Integrated charge
    uint32_t seqno; // The sequence number of this primitive in the
                    // current time step, for the benefit of
                    // downstream escalators
};

//======================================================================
std::ostream& operator<<(std::ostream& os, const TriggerPrimitive& prim)
{
    os << "TriggerPrimitive: t=" << prim.timestamp
       << " apa=" << prim.apa
       << " chan=" << prim.channel
       << " t_over=" << prim.time_over_threshold
       << " Q=" << prim.charge
       << " seq=" << prim.seqno;
    return os;
}

//======================================================================
//
// Send the TriggerPrimitives in `prims` on `socket` as a single
// ZeroMQ message
void sendPrimitives(zsock_t* socket, std::vector<TriggerPrimitive>& prims)
{
    zframe_t *message = zframe_new(&prims[0], prims.size()*sizeof(TriggerPrimitive));
    int rc = zframe_set_group(message, "all");
    assert (rc == 0);
    // zframe_send destroys the message for us so we don't have to
    rc = zframe_send (&message, socket, 0);
        
    if(rc!=0){
        std::cout << "Failed sending. rc=" << rc << std::endl;
    }
}

//======================================================================
//
// Receive one ZeroMQ message of TriggerPrimitives from `socket`,
// storing the number of primitives in `N`. The returned pointer must
// be freed with free() (I think)
TriggerPrimitive* recvPrimitives(zsock_t* socket, int& N)
{
    zframe_t *message=zframe_recv(socket);
    if(!message){
        return nullptr;
    }
    unsigned int msgSize=zframe_size(message);
    N = zframe_size(message)/sizeof(TriggerPrimitive);
    assert(  N*sizeof(TriggerPrimitive) == msgSize );
    void* mem=malloc(N*sizeof(TriggerPrimitive));
    memcpy(mem, zframe_data(message), N*sizeof(TriggerPrimitive));
    zframe_destroy(&message);
    return (TriggerPrimitive*)mem;
}

#endif

// Local Variables:
// mode: c++
// c-basic-offset: 4
// c-file-style: "linux"
// End:
