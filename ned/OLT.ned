
simple OLT
{
    parameters:
        @display("i=device/server");
        int numONUs;

        // Downstream parameters
        int UpmaxGrantBytes = default(25000);
        int packetSize @unit(b) = default(1000b);
        double packetInterval @unit(s) = default(10us);
        double dataRate @unit(bps) = default(10Gbps);
        double load = default(0.2);
        double waitRatio = default (0.1);
        // Upstream scheduling parameters
        double maxCycleTime @unit(s) = default(400us);
        double guardTime @unit(s) = default(1us);
        int reportSizeBytes = default(64);
        double lowlimWindow @unit(s) = default(40us); 
        int maxONUFullWindow = default(100);
    gates:
        input in;
        output grantOut;
}
