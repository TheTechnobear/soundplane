//
//  RemoteControl
//  soundplane
//
//  Created by thetechnobear on 26/5/16.
//
//

#ifndef RemoteControl_h
#define RemoteControl_h

#include "MLOSCListener.h"
#include "MLProperty.h"
#include "UdpSocket.h"


class RemoteControl :
    public MLOSCListener,
    public MLPropertyListener
{
public:
    RemoteControl(MLPropertySet* m);
    
    //Connect
    void Connect(int input, int output);
    
    //MLOSCListener
    void ProcessMessage(const osc::ReceivedMessage &m, const IpEndpointName& remoteEndpoint);
    void ProcessBundle(const osc::ReceivedBundle &b, const IpEndpointName& remoteEndpoint);
    
    //MLPropertyListener
    virtual void doPropertyChangeAction(MLSymbol param, const MLProperty & newVal);

private:
    MLPropertySet* model_;
    UdpSocket socket_; // for output
};

#endif /* RemoteControl_h */
