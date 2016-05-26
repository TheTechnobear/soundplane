//
//  RemoteParams.cpp
//  soundplane
//
//  Created by thetechnobear on 26/5/16.
//
//

#include "RemoteControl.h"
#include "OscOutboundPacketStream.h"


#define OUTPUT_BUFFER_SIZE 1024

RemoteControl::RemoteControl(MLPropertySet* m) : MLPropertyListener(m), model_(m) {
}


void RemoteControl::Connect(int input, int output) {
    listenToOSC(input);
    socket_.Connect(IpEndpointName( "127.0.0.1", output ) );
}

void RemoteControl::ProcessBundle(const osc::ReceivedBundle &b, const IpEndpointName& remoteEndpoint)
{
}


void RemoteControl::ProcessMessage(const osc::ReceivedMessage &m, const IpEndpointName& remoteEndpoint)
{
    if(model_!=nullptr) {
        osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
        try
        {
            if( std::strcmp( m.AddressPattern(), "/soundplane/param") == 0 )
            {
                if(m.ArgumentCount() == 2) {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    const char* name = arg->AsString();
                    if(name == nullptr) return;
                    arg++;
                    
                    MLSymbol sym(name);
                    
                    if(arg->IsFloat()) {
                        model_->setProperty(name,arg->AsFloat());
                    } else if (arg->IsString()) {
                        model_->setProperty(name,arg->AsString());
//                    } else if (arg->IsInt32()) {
//                        model_->setProperty(name,arg->AsInt32());
//                    } else if (arg->IsBool()) {
//                        model_->setProperty(name,arg->AsBool());
                    }
                }
            }
            
        }
        catch( osc::Exception& e )
        {
            MLConsole() << "oscpack error while parsing message: "
            << m.AddressPattern() << ": " << e.what() << "\n";
        }
    }
}


void RemoteControl::doPropertyChangeAction(MLSymbol param, const MLProperty & newVal)
{
    char buffer[OUTPUT_BUFFER_SIZE];
    osc::OutboundPacketStream p( buffer, OUTPUT_BUFFER_SIZE );
    
    try {
        int propertyType = newVal.getType();
        switch(propertyType) {
            case MLProperty::kFloatProperty:
            {
                float v = newVal.getFloatValue();
                
                p << osc::BeginBundleImmediate;
                p << osc::BeginMessage( "/soundplane/param" );
                p << param.getString().c_str();
                p << v;
                p << osc::EndMessage;
                p << osc::EndBundle;
                socket_.Send( p.Data(), p.Size() );
                break;
            }
            case MLProperty::kStringProperty:
            {
                std::string v = newVal.getStringValue();
                p << osc::BeginBundleImmediate;
                p << osc::BeginMessage( "/soundplane/param" );
                p << param.getString().c_str();
                p << v.c_str();
                p << osc::EndMessage;
                p << osc::EndBundle;
                socket_.Send( p.Data(), p.Size() );
                break;
            }
        }
    }
    catch (std::runtime_error& e) {
        
    }
}


