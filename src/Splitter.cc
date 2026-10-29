
#include <omnetpp.h>
#include <cstring>

using namespace omnetpp;

class Splitter : public cSimpleModule
{
  private:
    int numONUs = 0;

  protected:
    void initialize() override;
    void handleMessage(cMessage *msg) override;
};

Define_Module(Splitter);

void Splitter::initialize()
{
    numONUs = par("numONUs").intValue();
}

void Splitter::handleMessage(cMessage *msg)
{
    cGate *arrivalGate = msg->getArrivalGate();

    if (arrivalGate == nullptr)
    {
        delete msg;
        throw cRuntimeError(
            "Splitter received a message without an arrival gate");
    }

    const char *arrivalGateName = arrivalGate->getName();

    // ========================================
    // NORMAL UPSTREAM: ONU -> Splitter -> OLT
    // ========================================
    // Preserve the original DATA/REPORT packet.
    if (std::strcmp(arrivalGateName, "in") == 0)
    {
        send(msg, "out");
        return;
    }

    
    
    
    
    
    //=========================================
    // GATE / CONTROL: OLT -> Splitter -> ONUs
    // ========================================
    // GATE messages remain broadcast to all ONUs.
    if (std::strcmp(arrivalGateName, "grantIn") == 0)
    {
        if (!msg->hasPar("onuId"))
        {
            delete msg;
            throw cRuntimeError("GATE message is missing onuId");
        }

        int destinationONU = static_cast<int>(
            msg->par("onuId").longValue()
        );

        if (destinationONU < 0 || destinationONU >= numONUs)
        {
        delete msg;
            throw cRuntimeError(
                "Invalid destination ONU ID in GATE: %d",
                destinationONU
            );
        }

        send(msg, "grantOut", destinationONU);
        return;
    }

    // ========================================
    // LOCAL SHARING: Contributor -> Splitter -> Host
    // ========================================
    // Send DATA/REPORT only to the intended Host ONU.
    if (std::strcmp(arrivalGateName, "lsIn") == 0)
    {
        if (!msg->hasPar("hostId"))
        {
            delete msg;
            throw cRuntimeError(
                "Local Sharing packet is missing hostId");
        }

        int hostId =
            static_cast<int>(msg->par("hostId").longValue());

        if (hostId < 0 || hostId >= numONUs)
        {
        delete msg;
            throw cRuntimeError(
                "Invalid Local Sharing Host ONU: %d",
                hostId);
        }

        send(msg, "lsOut", hostId);
        return;
    }

    // ========================================
    // UNEXPECTED INPUT
    // ========================================
    delete msg;
    throw cRuntimeError(
        "Splitter received a message on unexpected gate '%s'",
        arrivalGateName);
}
