
#include <omnetpp.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>

using namespace omnetpp;

class OLT : public cSimpleModule
{
  private:
    cMessage *scheduleMsg = nullptr;

    enum class SchedulerPhase
    {
        INITIAL_DISCOVERY,
        INITIAL_LOCAL_SHARING,
        STEADY_STATE
    };

    SchedulerPhase phase = SchedulerPhase::INITIAL_DISCOVERY;

    int numONUs = 0;
    bool initialScheduleDone = false;
    SimTime lowlimWindow;
    int maxONUFullWindow;
    SimTime horizonUP = SIMTIME_ZERO;
    SimTime horizonLS = SIMTIME_ZERO;

    SimTime maxCycleTime;
    SimTime guardTime;

    double dataRate = 0;
    double oltToRNDelay = 0;
    double waitRatio;

    long upMaxGrantBytes = 0;
    long reportSizeBytes = 0;
    long packetSizeBytes = 0;

    // DATA packet statistics
    long totalDataPacketsReceived = 0;
    SimTime totalPacketDelay = SIMTIME_ZERO;


    std::vector<long> queueBytes;
    std::vector<bool> isScheduled;
    std::vector<bool> RemainingIncycle;
    std::vector<bool> hostsUsedtoEmptyRemainingONU;
    std::vector<double> rnToONUDelays;

    std::vector<bool> initialReportReceived;
    int initialReportsRemaining = 0;

    int initialHostId = -1;
    std::vector<int> initialLSGroup;

    std::ofstream deltaTLSFile;
    void sendGrantMessage(
        int onuId,
        long grantBytes,
        int mode,
        int hostId,
        double lsDuration,
        SimTime delayAfterGrant,
        SimTime DelayExperiencedByPacket);

    void sendGrantForONU(int onuId, long grantBytes);

    void selectInitialLocalSharingGroup();
    
    bool scheduleDynamicLocalSharing();
    

    long alignedGrantBytes(long bytes) const;

  protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    virtual void finish() override;
};

Define_Module(OLT);


static SimTime secondsToSimTime(double seconds)
{
    constexpr double TICKS_PER_SECOND = 1e12;

    int64_t ticks = static_cast<int64_t>(
        std::llround(seconds * TICKS_PER_SECOND)
    );

    return SimTime(ticks, SIMTIME_PS);
}


long OLT::alignedGrantBytes(long bytes) const
{
    if (bytes <= 0)
        return 0;

    bytes = std::min(bytes, upMaxGrantBytes);
    bytes -= bytes % packetSizeBytes;

    return std::max(0L, bytes);
}


void OLT::initialize()
{
    scheduleMsg = new cMessage("InitialSchedule");

    cModule *network = getParentModule();

    numONUs = par("numONUs").intValue();
    lowlimWindow = par("lowlimWindow");
    maxONUFullWindow = par("maxONUFullWindow").intValue();

    if (numONUs < 6)
        throw cRuntimeError(
            "Initial Local Sharing requires at least 6 ONUs"
        );

    maxCycleTime = par("maxCycleTime");
    guardTime = par("guardTime");
    waitRatio = par("waitRatio");
    if (waitRatio <= 0.0 || waitRatio > 1.0)
        throw cRuntimeError("waitRatio must be between 0 and 1");

    reportSizeBytes = par("reportSizeBytes").intValue();

    dataRate = network->par("dataRate").doubleValue();

    oltToRNDelay =
        network->par("del_OLTtoRN").doubleValue();

    if (dataRate <= 0 ||
        reportSizeBytes <= 0 ||
        guardTime < SIMTIME_ZERO ||
        maxCycleTime <= SIMTIME_ZERO)
    {
        throw cRuntimeError("Invalid OLT scheduling parameters");
    }

    rnToONUDelays.resize(numONUs);

    cModule *firstONU = network->getSubmodule("onu", 0);

    long packetSizeBits =
        firstONU->par("packetSize").intValue();

    if (packetSizeBits <= 0 || packetSizeBits % 8 != 0)
        throw cRuntimeError(
            "packetSize must be a positive multiple of 8 bits"
        );

    packetSizeBytes = packetSizeBits / 8;

    for (int i = 0; i < numONUs; i++)
    {
        cModule *onuModule = network->getSubmodule("onu", i);

        rnToONUDelays[i] =
            onuModule->par("rnToONUDelay").doubleValue();

        EV_INFO << "ONU " << i
                << " per-ONU delay = "
                << rnToONUDelays[i] * 1e6
                << " us"
                << endl;
    }

    // Calculate a conservative normal grant size.
    double reportTxTime =
        reportSizeBytes * 8.0 / dataRate;

    
    upMaxGrantBytes = par("UpmaxGrantBytes").intValue();

    upMaxGrantBytes -= upMaxGrantBytes % packetSizeBytes;

    if (upMaxGrantBytes < packetSizeBytes)
        throw cRuntimeError("Calculated grant is too small");

    queueBytes.assign(numONUs, 0);
    isScheduled.assign(numONUs, false);
    RemainingIncycle.assign(numONUs, true);
    hostsUsedtoEmptyRemainingONU.assign(numONUs, false);
    initialReportReceived.assign(numONUs, false);
    initialReportsRemaining = numONUs;

    horizonUP = SIMTIME_ZERO;
    horizonLS = SIMTIME_ZERO;

    phase = SchedulerPhase::INITIAL_DISCOVERY;

    EV_INFO << "\n=== OLT CONFIGURATION ==="
            << "\nNumber of ONUs = " << numONUs
            << "\nData rate = " << dataRate
            << " bit/s"
            << "\nMaximum grant = " << upMaxGrantBytes
            << " bytes"
            << "\nREPORT size = " << reportSizeBytes
            << " bytes"
            << "\nGuard time = " << guardTime
            << endl;

    scheduleAt(SIMTIME_ZERO, scheduleMsg);

    deltaTLSFile.open("deltaTLS_vs_simTime.csv");

    if (!deltaTLSFile.is_open())
        throw cRuntimeError("Could not open deltaTLS_vs_simTime.csv");

    deltaTLSFile << "simTime_s,deltaTLS_s\n";
    deltaTLSFile << std::setprecision(15);
}


// Create and send a GATE. Timing and reservations are calculated
// by the caller so LS and UP paths can be scheduled independently.
void OLT::sendGrantMessage(
    int onuId,
    long grantBytes,
    int mode,
    int hostId,
    double lsDuration,
    SimTime delayAfterGrant,
    SimTime DelayExperiencedByPacket)
{
    if (onuId < 0 || onuId >= numONUs)
        throw cRuntimeError("Invalid ONU ID in GATE");

    if (grantBytes < 0 || delayAfterGrant < SIMTIME_ZERO)
        throw cRuntimeError("Invalid GATE grant or delay");

    cMessage *grant = new cMessage("Grant");

    grant->addPar("onuId");
    grant->par("onuId").setLongValue(onuId);

    grant->addPar("grantBytes");
    grant->par("grantBytes").setLongValue(grantBytes);

    grant->addPar("delay_after_rec_grant");
    grant->par("delay_after_rec_grant")
         .setDoubleValue(delayAfterGrant.dbl());

    grant->addPar("DelayExperiencedByPacket");
    grant->par("DelayExperiencedByPacket")
        .setDoubleValue(DelayExperiencedByPacket.dbl());

    grant->addPar("mode");
    grant->par("mode").setLongValue(mode);

    grant->addPar("hostId");
    grant->par("hostId").setLongValue(hostId);

    grant->addPar("lsDuration");
    grant->par("lsDuration").setDoubleValue(lsDuration);

    EV_INFO << "\nOLT sends GATE"
            << "\nONU = " << onuId
            << "\nMode = " << mode
            << "\nHost ID = " << hostId
            << "\nGrant = " << grantBytes << " bytes"
            << "\nDelay after GATE reception = "
            << delayAfterGrant.dbl() * 1e6 << " us"
            << "\nLS duration = " << lsDuration * 1e6 << " us"
            << endl;

    send(grant, "grantOut");
}


// Normal upstream scheduling. The reservation horizon represents
// the expected end of the previous UP reservation at the OLT.
void OLT::sendGrantForONU(int onuId, long grantBytes)
{
    grantBytes = alignedGrantBytes(grantBytes);

    SimTime upstreamPropagation =
        secondsToSimTime(
            oltToRNDelay + rnToONUDelays[onuId]
        );

    SimTime roundTrip =
        secondsToSimTime(
            2.0 * (oltToRNDelay + rnToONUDelays[onuId])
        );

    SimTime earliestArrival = simTime() + roundTrip;

    SimTime arrivalTime =
        std::max(horizonUP, earliestArrival);

    SimTime delayAfterGrant =
        arrivalTime - simTime() - roundTrip;

    sendGrantMessage(
        onuId,
        grantBytes,
        0,
        -1,
        0.0,
        delayAfterGrant,
        SimTime(0,SIMTIME_US)
    );

    SimTime dataTxTime =
        secondsToSimTime(grantBytes * 8.0 / dataRate);

    SimTime reportTxTime =
        secondsToSimTime(reportSizeBytes * 8.0 / dataRate);

    horizonUP =
        arrivalTime + dataTxTime + reportTxTime + guardTime;

    EV_INFO << "UP reservation: ONU " << onuId
            << ", first arrival = " << arrivalTime
            << ", reserved until = " << horizonUP
            << ", upstream propagation = " << upstreamPropagation
            << endl;
}


// Rank ONUs and select the six largest reported queues.
void OLT::selectInitialLocalSharingGroup()
{
    if (phase != SchedulerPhase::INITIAL_DISCOVERY)
        return;

    if (initialReportsRemaining != 0)
        throw cRuntimeError(
            "Initial discovery is not complete"
        );

    std::vector<int> rankedONUs(numONUs);

    for (int i = 0; i < numONUs; i++)
        rankedONUs[i] = i;

    std::stable_sort(
        rankedONUs.begin(),
        rankedONUs.end(),
        [this](int a, int b)
        {
            return queueBytes[a] > queueBytes[b];
        }
    );

    initialLSGroup.assign(
        rankedONUs.begin(),
        rankedONUs.begin() + 6
    );

    // The highest reported queue becomes the Host.
    initialHostId = initialLSGroup.front();

    

    EV_INFO << "\n=== INITIAL LOCAL SHARING GROUP ==="
            << "\nHost ONU = " << initialHostId
            << "\nReported queue = "
            << queueBytes[initialHostId] << " bytes"
            << endl;

    for (int rank = 0; rank < 6; rank++)
    {
        int onuId = initialLSGroup[rank];

        EV_INFO << "Rank " << rank + 1
                << ": ONU " << onuId
                << ", queue = " << queueBytes[onuId]
                << " bytes"
                << (rank == 0 ? " (HOST)" : " (CONTRIBUTOR)")
                << endl;
    }

    SimTime now = simTime();

    horizonLS = now + SimTime(350+125, SIMTIME_US);

    horizonUP = now
            +SimTime(350, SIMTIME_US)
            + SimTime(125, SIMTIME_US)
            + SimTime(20, SIMTIME_US)
            + SimTime(125, SIMTIME_US);

    EV_INFO << "\n=== INITIAL LS HORIZONS ==="
            << "\nHorizon_LS = " << horizonLS
            << "\nHorizon_UP = " << horizonUP
                << endl;

    // Enter the existing dynamic scheduling logic.
    phase = SchedulerPhase::STEADY_STATE;
    scheduleAt(now + SimTime(350, SIMTIME_US), scheduleMsg);
}

// Dynamic upstream-only Local Sharing scheduler.
// Returns true only when an LS group reservation was committed.
bool OLT::scheduleDynamicLocalSharing()
{
    const SimTime jitOffset = SimTime(125, SIMTIME_US);
    const SimTime now = simTime();
    const SimTime reportTxTime =
        secondsToSimTime(reportSizeBytes * 8.0 / dataRate);

    
    //The LS window is measured at the Splitter/RN.
    SimTime rightLimit = horizonUP - jitOffset;
    SimTime deltaTLS = rightLimit - horizonLS;

    //Write deltaTLS and simTime to file
    if (deltaTLSFile.is_open())
    {
        deltaTLSFile << now.dbl() << ","
                    << deltaTLS.dbl()*1e6 << "\n";

        deltaTLSFile.flush();
    }

    SimTime SinglePacketTime =secondsToSimTime(static_cast<double>(packetSizeBytes) * 8.0 / dataRate);

    // Stage A: if the window is too small, schedule ordinary upstream
    // grants from fresh, unconsumed REPORTs to advance Horizon_UP.
    // Each fallback ONU is consumed only once in this scheduling round.
    while (deltaTLS < guardTime + reportTxTime)//Try to increase the LS window by scheduling ordinary upstream grants for the largest unconsumed REPORTs until the LS window is large enough or there are no more unconsumed REPORTs only when deltaTLS is small because of horizonUP not being far enough.
    {
        int fallbackONU = -1;
        long largestQueue = -1;

        for (int i = 0; i < numONUs; ++i)
        {
            if (isScheduled[i])
                continue;

            if (queueBytes[i] > largestQueue)
            {
                largestQueue = queueBytes[i];
                fallbackONU = i;
            }
        }

        if (fallbackONU < 0)
        {
            // EV_INFO << "No fresh REPORT available for fallback upstream scheduling; "
            //         << "deferring LS until a new REPORT arrives."
            //         << endl;
            return false;
        }

        long fallbackGrant = alignedGrantBytes(queueBytes[fallbackONU]);
        isScheduled[fallbackONU] = true;

        EV_INFO << "LS window too small; fallback upstream grant for ONU "
                << fallbackONU << ", grant = " << fallbackGrant
                << " bytes" << endl;

        sendGrantForONU(fallbackONU, fallbackGrant);

        rightLimit = horizonUP - jitOffset;
        deltaTLS = rightLimit - horizonLS;
    }

    if (deltaTLS < guardTime + reportTxTime)
    {
        EV_INFO << "No usable LS window; waiting for a fresh REPORT."
                << endl;
        return false;
    }

    // Stage B: eligible Host candidates are fresh, unconsumed reports
    // with at least one complete DATA packet available for a grant.
    std::vector<int> candidates;
    candidates.reserve(numONUs);

    for (int i = 0; i < numONUs; ++i)
    {
        if (!isScheduled[i] && alignedGrantBytes(queueBytes[i]) >= 0)
            candidates.push_back(i);
    }

    std::stable_sort(candidates.begin(), candidates.end(),
        [this](int a, int b) {
            if (queueBytes[a] != queueBytes[b])
                return queueBytes[a] > queueBytes[b];
            return a < b;
        });

        // Calculate the total expected grant bytes for all candidates.
        long totalGrantBytes = 0;

        for (const int onuId : candidates)
        {
            totalGrantBytes += alignedGrantBytes(queueBytes[onuId]);
        }

        // Calculate the data burst transmission time for the total expected data.
        SimTime dataBurstTime =
            SimTime(static_cast<double>(totalGrantBytes) * 8.0 / dataRate,
                    SIMTIME_S);

        // Available time for Local Sharing at the Splitter.
        deltaTLS =
            horizonUP
            - SimTime(125, SIMTIME_US)
            - horizonLS;

        // Check whether sufficient time remains after reserving waitRatio
        // of the Local Sharing window for waiting.
        if ((1.0 - waitRatio) * deltaTLS >=
            dataBurstTime +
            candidates.size() * (reportTxTime + guardTime) && candidates.size()>=1)
        {
            horizonLS += (waitRatio * deltaTLS);

            SimTime nextDecision =
                horizonLS - SimTime(125, SIMTIME_US);

            // Avoid scheduling the event in the past.
            if (nextDecision < simTime())
            {
                nextDecision = simTime();
                horizonLS =
                    nextDecision + SimTime(125, SIMTIME_US);
            }

            EV_INFO << "Sufficient LS window available; waiting before scheduling."
                    << "\nTotal expected grant bytes = " << totalGrantBytes
                    << "\nData burst time = " << dataBurstTime
                    << "\nAvailable LS window (deltaTLS) = " << deltaTLS
                    << "\nWait ratio = " << waitRatio
                    << "\nUpdated Horizon_LS = " << horizonLS
                    << "\nNext scheduling time = " << nextDecision
                    << endl;

            if (!scheduleMsg->isScheduled())
                scheduleAt(nextDecision, scheduleMsg);

            return false;
        }

        if (candidates.size() < 2)
        {
            if (!candidates.empty())
            {
                const int onlyONU = candidates.front();
                const long grantBytes = alignedGrantBytes(queueBytes[onlyONU]);
        
                // Available time for Local Sharing at the Splitter.
                SimTime deltaTLS =
                    horizonUP
                    - SimTime(125, SIMTIME_US)
                    - horizonLS;
        
                
        
                // Case 1: Wait for another opportunity to form an LS group.
                if (deltaTLS > (guardTime + reportTxTime)*1.36)
                {
                    SimTime waitTime = deltaTLS / 4;
        
                    // Advance the LS horizon by one-quarter of the
                    // currently available LS window.
                    horizonLS += waitTime;
        
                    // Consume this scheduling decision without consuming
                    // the ONU's REPORT: it may participate in LS later.
                    SimTime nextDecision =
                        horizonLS - SimTime(125, SIMTIME_US);
        
                    // Do not schedule the next event in the past.
                    if (nextDecision < simTime())
                    {
                        nextDecision = simTime();
                        horizonLS =
                            nextDecision + SimTime(125, SIMTIME_US);
                    }
        
                    EV_INFO << "LS window is large enough to wait"
                            << "\nWait time = " << waitTime
                            << "\nUpdated Horizon_LS = " << horizonLS
                            << "\nNext scheduling time = " << nextDecision
                            << endl;
        
                    if (!scheduleMsg->isScheduled())
                        scheduleAt(nextDecision, scheduleMsg);
        
                    return false;
                }
        
                // Case 2: Insufficient LS opportunity; serve this ONU
                // through ordinary upstream scheduling.
                isScheduled[onlyONU] = true;
                EV_INFO <<"Single ONU with eligible Data";
                EV_INFO << "LS window too small; using normal upstream "
                        << "grant for ONU " << onlyONU
                        << ", grant = " << grantBytes << " bytes"
                        << endl;
        
                sendGrantForONU(onlyONU, grantBytes);
            }
            else
            {
                EV_INFO << "No ONU has eligible DATA; waiting for a fresh REPORT."
                        << endl;
            }
        
            return false;
        }

    // Upstream-only phase: choose the maximum eligible burst as Host.
    // If eligible bursts tie (e.g. both are capped at maxGrantBytes),
    // choose the smallest ONU ID deterministically. Contributor order
    // remains decreasing reported queue size as requested.
    int hostId = -1;
    long largestEligibleBurst = -1;
    for (int candidateId : candidates)
    {
        const long eligibleBurst = alignedGrantBytes(queueBytes[candidateId]);
        if (eligibleBurst > largestEligibleBurst ||
            (eligibleBurst == largestEligibleBurst &&
             (hostId < 0 || candidateId < hostId)))
        {
            largestEligibleBurst = eligibleBurst;
            hostId = candidateId;
        }
    }
    
    // Record Hosts that have already contributed during this cycle.
    if (!RemainingIncycle[hostId])
        hostsUsedtoEmptyRemainingONU[hostId] = true;
    else
        RemainingIncycle[hostId] = false;
    
    // Refresh an exhausted cycle before selecting its contributors.
    if (std::none_of(RemainingIncycle.begin(), RemainingIncycle.end(),
                     [](bool remaining) { return remaining; }))
    {
        for (int i = 0; i < numONUs; ++i)
        {
            RemainingIncycle[i] =
                !(RemainingIncycle[i] || hostsUsedtoEmptyRemainingONU[i]);
            hostsUsedtoEmptyRemainingONU[i] = false;
        }
        EV_INFO << "Contributor eligibility cycle refreshed." << endl;
    }

    //Delay Experienced by packet from splitter to the OLT
    SimTime DelayExperiencedByPacket = horizonUP - horizonLS;

    

    const SimTime oldHorizonLS = horizonLS;
    SimTime lsCursor = horizonLS;
    SimTime latestLSArrivalAtHost = horizonLS;
    long expectedForwardBytes = 0;
    std::vector<int> committedContributors;

    const SimTime oltToRNSim = secondsToSimTime(oltToRNDelay);
    const SimTime hostDelay = secondsToSimTime(rnToONUDelays[hostId]);

    EV_INFO << "\nDynamic LS Host selected: ONU " << hostId
            << ", reported queue = " << queueBytes[hostId]
            << " bytes" << endl;
    
    // //NEWADDITION
    // // Filter candidates that cannot fit a valid DATA + REPORT + guard slot.
    // const SimTime availableWindow = rightLimit - horizonLS;
    // std::vector<int> viableCandidates;
    
    // for (int candidateId : candidates)
    // {
    //     if (candidateId == hostId)
    //         continue;
    
    //     const long expectedGrantBytes =
    //         alignedGrantBytes(queueBytes[candidateId]);
    
    //     const SimTime availableDataTime =
    //         rightLimit - horizonLS - reportTxTime - guardTime;
    
    //     if (availableDataTime <= SIMTIME_ZERO)
    //         continue;
    
    //     const SimTime expectedDataTxTime =
    //         secondsToSimTime(expectedGrantBytes * 8.0 / dataRate);
    
    //     if (expectedDataTxTime <= availableDataTime)
    //         viableCandidates.push_back(candidateId);
    // }
    
    // candidates = std::move(viableCandidates);


    // if (candidates.empty())
    // {
    //     // Issue the Host's ordinary upstream grant here.
    //     // Update the relevant scheduling state/horizon.
    //     long grantBytes =
    //     alignedGrantBytes(queueBytes[hostId]);
    //     isScheduled[hostId] = true;
    //     sendGrantForONU(hostId, grantBytes);

    //     if (!scheduleMsg->isScheduled())
    //         scheduleAt(simTime(), scheduleMsg);

    //     return false;
    // }
    // //NEWADDITION END
    
    const long hostGrantBytes = alignedGrantBytes(queueBytes[hostId]);

    //We can't Restrict upperlimit for the policy which favours those with higher data
    // Restrict the No of contributors to int(hostBurst/guard)+1
    // Restrict the number of contributors based on the Host's DATA burst time.

    const SimTime hostDataBurst =
        secondsToSimTime(hostGrantBytes * 8.0 / dataRate);

    int maxContributors = 0;

    if (guardTime > SIMTIME_ZERO)
    {
        maxContributors =
            static_cast<int>(std::floor(
                hostDataBurst.dbl() / guardTime.dbl())) + 1;
    }
    else
    {
        // Avoid division by zero when guardTime is zero.
        maxContributors = static_cast<int>(candidates.size());
    }

    // Do not exceed the number of available non-Host candidates.
    const int availableContributors = static_cast<int>(
        std::count_if(
            candidates.begin(),
            candidates.end(),
            [hostId](int id) { return id != hostId; }));

    maxContributors = std::min(maxContributors, availableContributors);

    EV_INFO << "Host DATA burst = " << hostDataBurst
            << "\nMaximum permitted contributors = " << maxContributors
            << endl;

    
    
    // Stage C: process contributors in decreasing queue order and fit
    // each DATA + REPORT + guard slot into the remaining LS window.

    // Reverse candidates: lowest reported bytes first.
    //If we limit the no of contributors then its absolutely necessary to start from the lowest reported bytes First
    std::reverse(candidates.begin(), candidates.end());
    

    for (int candidateId : candidates)
    {
        const long expectedGrantBytes =
            alignedGrantBytes(queueBytes[candidateId]);
    
        EV_INFO<<"Reported Bytes: "<<expectedGrantBytes<<"\nONU id: "<<candidateId<<endl;
    }
    int contributorsScheduled = 0;
    
    deltaTLS =
                    horizonUP
                    - SimTime(125, SIMTIME_US)
                    - horizonLS;
    for (int contributorId : candidates)
    {
        if (contributorId == hostId)
            continue;
        if (!RemainingIncycle[contributorId])
            continue;

        if(deltaTLS < lowlimWindow || contributorsScheduled>=maxONUFullWindow)
        {// Enforce the contributor limit.
            if (contributorsScheduled >= maxContributors)
                break;
        }
        const SimTime contributorDelay =
            secondsToSimTime(rnToONUDelays[contributorId]);

        // GATE: OLT->RN->ONU; DATA: ONU->RN.
        const SimTime earliestSplitterArrival =
            now + oltToRNSim + contributorDelay + contributorDelay;
        const SimTime lsStartAtSplitter =
            std::max(lsCursor, earliestSplitterArrival);

        const SimTime remainingAtSplitter = rightLimit - lsStartAtSplitter;
        const SimTime usableDataTime =
            remainingAtSplitter - reportTxTime - guardTime;

        if (usableDataTime <= SIMTIME_ZERO)
        {
            // EV_INFO << "No time left for another LS contributor; stopping group fit."
            //         << endl;
            break;
        }

        long timeCapacityBytes = static_cast<long>(
            std::floor(dataRate * usableDataTime.dbl() / 8.0));
        timeCapacityBytes -= timeCapacityBytes % packetSizeBytes;

        const long grantBytes = std::min(
            alignedGrantBytes(queueBytes[contributorId]),
            alignedGrantBytes(timeCapacityBytes));

        if (grantBytes < 0)
            continue;

        SimTime delayAfterGrant =
            lsStartAtSplitter - now - oltToRNSim
            - contributorDelay - contributorDelay;
        if (delayAfterGrant < SIMTIME_ZERO)
            delayAfterGrant = SIMTIME_ZERO;

        const SimTime dataTxTime =
            secondsToSimTime(grantBytes * 8.0 / dataRate);
        const SimTime contributorSlotDuration =
            dataTxTime + reportTxTime;

        // Commit the contributor's REPORT to this LS reservation.
        isScheduled[contributorId] = true;
        RemainingIncycle[contributorId] = false;
        committedContributors.push_back(contributorId);

        sendGrantMessage(
            contributorId,
            grantBytes,
            1,
            hostId,
            contributorSlotDuration.dbl(),
            delayAfterGrant,
            DelayExperiencedByPacket
        );

        const SimTime reportArrivalAtHost =
            lsStartAtSplitter + contributorSlotDuration + hostDelay;
        latestLSArrivalAtHost =
            std::max(latestLSArrivalAtHost, reportArrivalAtHost);

        expectedForwardBytes += grantBytes + reportSizeBytes;
        lsCursor = lsStartAtSplitter + contributorSlotDuration + guardTime;

        EV_INFO << "Dynamic LS contributor ONU " << contributorId
                << "\nGrant = " << grantBytes << " bytes"
                << "\nLS start at Splitter = " << lsStartAtSplitter
                << "\nSlot duration (DATA + REPORT) = "
                << contributorSlotDuration
                << "\nLS reservation end = " << lsCursor
                << endl;
    }

    // Refresh the cycle immediately after the last eligible ONU contributes.
    if (std::none_of(RemainingIncycle.begin(), RemainingIncycle.end(),
                     [](bool remaining) { return remaining; }))
    {
        for (int i = 0; i < numONUs; ++i)
        {
            RemainingIncycle[i] =
                !(RemainingIncycle[i] || hostsUsedtoEmptyRemainingONU[i]);
            hostsUsedtoEmptyRemainingONU[i] = false;
        }
        EV_INFO << "Contributor eligibility cycle refreshed." << endl;
    }

    // A Host-only reservation is not Local Sharing. Leave its REPORT
    // available if no contributor could fit in this LS window.
    if (committedContributors.empty())
    {
        // EV_INFO << "No contributor fitted into the LS window; "
        //         << "no LS reservation committed."
        //         << endl;
        return false;
    }

    // Stage D: reserve the Host's aggregate upstream burst. The Host's
    // individual GATE allowance covers only its own DATA; contributor
    // DATA/REPORT serialization is reserved separately in time.
    const SimTime hostGateArrival =
        now + oltToRNSim + hostDelay;
    SimTime hostSendStart =
        std::max(hostGateArrival, latestLSArrivalAtHost);
    const SimTime hostUpstreamPropagation = oltToRNSim + hostDelay;

    if (hostSendStart + hostUpstreamPropagation < horizonUP)
        hostSendStart = horizonUP - hostUpstreamPropagation;
    if (hostSendStart < hostGateArrival)
        hostSendStart = hostGateArrival;

    SimTime hostDelayAfterGrant = hostSendStart - hostGateArrival;
    if (hostDelayAfterGrant < SIMTIME_ZERO)
        hostDelayAfterGrant = SIMTIME_ZERO;

    
    const SimTime hostOwnDataTime =
        secondsToSimTime(hostGrantBytes * 8.0 / dataRate);
    const SimTime forwardingTime =
        secondsToSimTime(expectedForwardBytes * 8.0 / dataRate);
    const SimTime hostFirstArrivalAtOLT =
        hostSendStart + hostUpstreamPropagation;

    horizonLS = lsCursor;
    horizonUP = hostFirstArrivalAtOLT
        + forwardingTime
        + hostOwnDataTime
        + reportTxTime
        + guardTime;

    // Consume the Host REPORT only when the aggregate reservation is committed.
    isScheduled[hostId] = true;

    const SimTime totalLSDuration = horizonLS - oldHorizonLS;
    sendGrantMessage(
        hostId,
        hostGrantBytes,
        2,
        -1,
        totalLSDuration.dbl(),
        hostDelayAfterGrant,
    SimTime(0,SIMTIME_US));

    EV_INFO << "\n=== DYNAMIC LS RESERVATION COMMITTED ==="
            << "\nHost ONU = " << hostId
            << "\nContributors = " << committedContributors.size()
            << "\nExpected forwarded bytes = " << expectedForwardBytes
            << "\nHost own grant = " << hostGrantBytes << " bytes"
            << "\nHorizon_LS: " << oldHorizonLS << " -> " << horizonLS
            << "\nHorizon_UP = " << horizonUP
            << "\nNext JIT time = " << horizonLS - jitOffset
            << endl;

    return true;
}


void OLT::handleMessage(cMessage *msg)
{
    // ----------------------------------------
    // Initial discovery event
    // ----------------------------------------
    
    if (msg == scheduleMsg)
    {
        // ========================================
        // PHASE 1: INITIAL DISCOVERY
        // ========================================
        if (!initialScheduleDone)
        {
            initialScheduleDone = true;
            phase = SchedulerPhase::INITIAL_DISCOVERY;

            EV_INFO << "\n=== INITIAL DISCOVERY ==="
                    << "\nSending zero-byte GATEs to all ONUs"
                    << endl;

            for (int i = 0; i < numONUs; i++)
            {
                sendGrantForONU(i, 0);
            }

            return;
        }

        

        // ========================================
        // PHASE 3: DYNAMIC LOCAL SHARING
        // ========================================
        if (phase == SchedulerPhase::STEADY_STATE)
        {
            EV_INFO << "\n=== DYNAMIC LOCAL SHARING ==="
                    << "\nScheduling time = " << simTime()
                    << "\nHorizon LS = " << horizonLS
                    << "\nHorizon UP = " << horizonUP
                    << endl;

            const bool lsCommitted = scheduleDynamicLocalSharing();
            if (!lsCommitted)
                return; // A future REPORT will trigger another decision.

            SimTime nextScheduleTime =
                horizonLS - SimTime(125, SIMTIME_US);

            if (nextScheduleTime <= simTime())
            {
                throw cRuntimeError(
                    "Next dynamic LS scheduling deadline is not "
                    "in the future"
                );
            }

            if (!scheduleMsg->isScheduled())
                scheduleAt(nextScheduleTime, scheduleMsg);

            return;
        }

        return;
    }


    
    // ----------------------------------------
    // DATA packet reception and statistics
    // ----------------------------------------
    if (msg->isName("DATA"))
    {
        cPacket *packet = check_and_cast<cPacket *>(msg);

        // Count DATA packets received by the OLT
        totalDataPacketsReceived++;

        // Calculate end-to-end delay from packet creation to OLT reception
        SimTime packetDelay =
            simTime() - packet->getCreationTime();

        totalPacketDelay += packetDelay;

        EV_INFO << "OLT received DATA packet"
                << "\nPacket creation time = "
                << packet->getCreationTime()
                << "\nPacket reception time = "
                << simTime()
                << "\nPacket delay = "
                << packetDelay
                << endl;

        delete packet;
        return;
    }

    // ----------------------------------------
    // REPORT reception
    // ----------------------------------------
    if (msg->isName("REPORT"))
    {
        if (!msg->hasPar("onuId") ||
            !msg->hasPar("queueBytes"))
        {
            delete msg;
            throw cRuntimeError(
                "REPORT missing onuId or queueBytes"
            );
        }

        int onuId =
            static_cast<int>(msg->par("onuId").longValue());

        long reportedBytes =
            msg->par("queueBytes").longValue();

        if (onuId < 0 || onuId >= numONUs || reportedBytes < 0)
        {
            delete msg;
            throw cRuntimeError("Invalid REPORT contents");
        }

        queueBytes[onuId] = reportedBytes;
        isScheduled[onuId] = false;

        EV_INFO << "\nOLT received REPORT"
                << "\nReported source ONU = " << onuId
                << "\nQueue bytes = " << reportedBytes
                << "\nPhase = " << static_cast<int>(phase)
                << endl;

        if (phase == SchedulerPhase::INITIAL_DISCOVERY)
        {
            if (!initialReportReceived[onuId])
            {
                initialReportReceived[onuId] = true;
                initialReportsRemaining--;

                EV_INFO << "Initial REPORT count remaining = "
                        << initialReportsRemaining
                        << endl;
            }

            delete msg;

            if (initialReportsRemaining == 0)
                selectInitialLocalSharingGroup();

            return;
        }

        if (phase == SchedulerPhase::INITIAL_LOCAL_SHARING)
        {
            // This phase is only for calculating and issuing the initial
            // Local Sharing schedule. It should not wait for the Host REPORT.
            delete msg;
            return;
        }

        // In steady state, a new REPORT re-enables this ONU. If a prior
        // LS opportunity was deferred and no JIT event is pending, retry now
        // (or at the next JIT deadline if it is still in the future).
        delete msg;
        if (phase == SchedulerPhase::STEADY_STATE && !scheduleMsg->isScheduled())
        {
            SimTime nextDecision = horizonLS - SimTime(125, SIMTIME_US);
            if (nextDecision < simTime())
            {    
                nextDecision = simTime();
                horizonLS = nextDecision + SimTime(125, SIMTIME_US);
            }
            scheduleAt(nextDecision, scheduleMsg);
        }
        return;
    }

    delete msg;
}


void OLT::finish()
{
    cancelAndDelete(scheduleMsg);

    if (deltaTLSFile.is_open())
        deltaTLSFile.close();
    // ----------------------------------------
    // Record final DATA packet statistics
    // ----------------------------------------
    double meanDelaySeconds = 0.0;

    if (totalDataPacketsReceived > 0)
    {
        meanDelaySeconds =
            totalPacketDelay.dbl() /
            totalDataPacketsReceived;
    }

    recordScalar(
        "Total DATA Packets Received",
        totalDataPacketsReceived
    );
    EV_WARN << "OLT::finish() called at " << simTime() << endl;
    recordScalar(
        "Mean DATA Packet Delay (s)",
        meanDelaySeconds
    );

    EV_INFO << "\n========== FINAL DATA STATISTICS =========="
            << "\nTotal DATA packets received = "
            << totalDataPacketsReceived
            << "\nMean DATA packet delay = "
            << meanDelaySeconds * 1e6
            << " us"
            << "\n============================================"
            << endl;

}
