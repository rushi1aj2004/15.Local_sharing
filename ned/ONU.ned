
simple ONU
{
    parameters:
        @display("i=device/router");
        int onuId;
        double packetInterval @unit(s) = default(10us);
        int packetSize @unit(b) = default(1000b);
        double dataRate @unit(bps) = default(10Gbps);
        // Pareto ON/OFF traffic load for this ONU
        double load = default(0.2);
        // Upstream REPORT configuration
        int reportSizeBytes = default(64);

        //Packet Delay Budget
        double PDB @unit(s) = default(10ms);
        // RN-to-ONU propagation delay, sampled once
        double rnToONUDelay @unit(s) = default(uniform(5us, 25us));

    gates:
        // Normal upstream DATA and REPORT transmission
        output out;

        // Normal downstream GATE/control path
        input grantIn;

        // Local Sharing wavelength
        output lsOut;
        input lsIn;
}
