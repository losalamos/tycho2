/*
Copyright (c) 2016, Los Alamos National Security, LLC
All rights reserved.

Copyright 2016. Los Alamos National Security, LLC. This software was produced 
under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National 
Laboratory (LANL), which is operated by Los Alamos National Security, LLC for 
the U.S. Department of Energy. The U.S. Government has rights to use, 
reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR LOS 
ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR 
ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is modified 
to produce derivative works, such modified software should be clearly marked, 
so as not to confuse it with the version available from LANL.

Additionally, redistribution and use in source and binary forms, with or 
without modification, are permitted provided that the following conditions 
are met:
1.      Redistributions of source code must retain the above copyright notice, 
        this list of conditions and the following disclaimer.
2.      Redistributions in binary form must reproduce the above copyright 
        notice, this list of conditions and the following disclaimer in the 
        documentation and/or other materials provided with the distribution.
3.      Neither the name of Los Alamos National Security, LLC, Los Alamos 
        National Laboratory, LANL, the U.S. Government, nor the names of its 
        contributors may be used to endorse or promote products derived from 
        this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND 
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT 
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A 
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL 
SECURITY, LLC OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; 
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR 
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED 
OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "GraphTraverser.hh"
#include "Mat.hh"
#include "Global.hh"
#include "TychoMesh.hh"
#include "Comm.hh"
#include "Timer.hh"
#include <vector>
#include <set>
#include <queue>
#include <utility>
#include <omp.h>
#include <limits.h>
#include <string.h>

using namespace std;



const UINT MAX_PACKETS = 2000;
const bool ONE_SIDED = true;


/*
    winChunkLock
*/
void winChunkLock(const MPI_Win &mpiWin, int rank, UINT offset)
{
    int mpiError;
    UINT lockValue = 1;
    while (lockValue > 0) {
        UINT zero = 0;
        UINT one = 1;

        // Compares zero to value at window's offset
        // If equal, sets value to 1
        // Returns original value at window's offset into lockValue
        mpiError = MPI_Compare_and_swap(&one, &zero, &lockValue,
                                        MPI_UINT64_T, rank, offset, mpiWin);
        Insist(mpiError == MPI_SUCCESS, "");
        mpiError = MPI_Win_flush(rank, mpiWin);
        Insist(mpiError == MPI_SUCCESS, "");
    }
}


/*
    winChunkUnlock
*/
void winChunkUnlock(const MPI_Win &mpiWin, int rank, UINT offset)
{
    int mpiError;
    UINT zero = 0;

    // Atomically set window's offset to zero
    mpiError = MPI_Accumulate(&zero, 1, MPI_UINT64_T, rank, offset, 1, 
                              MPI_UINT64_T, MPI_REPLACE, mpiWin);
    Insist(mpiError == MPI_SUCCESS, "");
    mpiError = MPI_Win_flush(rank, mpiWin);
    Insist(mpiError == MPI_SUCCESS, "");
}


/*
    Tuple class
*/
namespace {
class Tuple
{
private:
    UINT c_cell;
    UINT c_angle;
    UINT c_priority;
    
public:
    Tuple(UINT cell, UINT angle, UINT priority)
        : c_cell(cell), c_angle(angle), c_priority(priority) {}
    
    UINT getCell() const { return c_cell; }
    UINT getAngle() const { return c_angle; }
    
    // Comparison operator to determine relative priorities
    // Needed for priority_queue
    bool operator<(const Tuple &rhs) const
    {
        return c_priority < rhs.c_priority;
    }
};}


/*
    splitPacket
    
    Packet is (global side, angle, data)
*/
static
void splitPacket(char *packet, UINT &globalSide, UINT &angle, char **data)
{
    memcpy(&globalSide, packet, sizeof(UINT));
    packet += sizeof(UINT);
    memcpy(&angle, packet, sizeof(UINT));
    packet += sizeof(UINT);
    *data = packet;
}


/*
    createPacket
    
    Packet is (global side, angle, data)
*/
static
void createPacket(vector<char> &packet, UINT globalSide, UINT angle, 
                  UINT dataSize, const char *data)
{
    packet.resize(2 * sizeof(UINT) + dataSize);
    char *p = packet.data();
    
    memcpy(p, &globalSide, sizeof(UINT));
    p += sizeof(UINT);
    memcpy(p, &angle, sizeof(UINT));
    p += sizeof(UINT);
    memcpy(p, data, dataSize);
}


/*
    isIncoming
    
    Determines whether data is incoming to the cell 
    depending on sweep direction.
*/
static
bool isIncoming(UINT angle, UINT cell, UINT face, Direction direction)
{
    if (direction == Direction_Forward)
        return g_tychoMesh->isIncoming(angle, cell, face);
    else if (direction == Direction_Backward)
        return g_tychoMesh->isOutgoing(angle, cell, face);
    
    // Should never get here
    Assert(false);
    return false;
}


/*
    angleGroupIndex
    
    Gets angle groups for angle index.
    e.g. 20 angles numbered 0...19 with 3 threads.
    Split into 3 angle chunks of size 7,7,6:  0...6  7...13  14...19
    If angle in 0...6,   return 0
    If angle in 7...13,  return 1
    If angle in 14...19, return 2
*/
UINT angleGroupIndex(UINT angle)
{
    UINT numAngles = g_nAngles;
    UINT chunkSize = numAngles / g_nThreads;
    UINT numChunksBigger = numAngles % g_nThreads;
    UINT lowIndex = 0;
    
    
    // Find angleGroup
    for (UINT angleGroup = 0; angleGroup < g_nThreads; angleGroup++) {
        
        UINT nextLowIndex = lowIndex + chunkSize;
        if (angleGroup < numChunksBigger)
            nextLowIndex++;
        
        if (angle < nextLowIndex) 
            return angleGroup;
        
        lowIndex = nextLowIndex;
    }
    
    
    // Should never get here
    Assert(false);
    return 0;
}


/*
    sendData

    Implements one-sided MPI for sending data.
*/
static 
void sendData(const vector<vector<char>> &sendBuffers,
              const vector<UINT> &adjRankIndexToRank,
              const vector<UINT> &offRankOffsets,
              const UINT packetSizeInBytes,
              const UINT maxPackets,
              const MPI_Win &mpiWin)
{
    UINT numAdjRanks = adjRankIndexToRank.size();
    

    // Send data to each adjacent rank
    for (UINT index = 0; index < numAdjRanks; index++) {
        
        // Make sure there is data to send
        if (sendBuffers[index].size() == 0)
            continue;
        

        // Useful values
        int adjRank = adjRankIndexToRank[index];
        UINT offRankOffset = offRankOffsets[index];
        const vector<char> &sendBuffer = sendBuffers[index];
        int mpiError;


        // Lock window chunk
        winChunkLock(mpiWin, adjRank, offRankOffset);
    

        // Get number of packets still not read by adjRank
        UINT numPacketsWritten = 0;
        mpiError = MPI_Get(&numPacketsWritten, 1, MPI_UINT64_T, adjRank, 
                           offRankOffset+8, 1, MPI_UINT64_T, mpiWin);
        Insist(mpiError == MPI_SUCCESS, "");
        mpiError = MPI_Win_flush_local(adjRank, mpiWin);
        Insist(mpiError == MPI_SUCCESS, "");
        

        // Check to see if there is room to write data
        UINT numPacketsToSend = sendBuffer.size() / packetSizeInBytes;
        if (numPacketsWritten + numPacketsToSend < maxPackets) {
            
            // Write the packets to adjRank
            UINT offset = offRankOffset + 16 + 
                          numPacketsWritten * packetSizeInBytes;
            mpiError = MPI_Put(sendBuffer.data(), sendBuffer.size(), 
                               MPI_BYTE, adjRank, offset, 
                               sendBuffer.size(), MPI_BYTE, mpiWin);
            Insist(mpiError == MPI_SUCCESS, "");
            
            // Update number of packets written
            numPacketsWritten += numPacketsToSend;
            mpiError = MPI_Put(&numPacketsWritten, 1, MPI_UINT64_T, adjRank, 
                               offRankOffset+8, 1, MPI_UINT64_T, mpiWin);
            Insist(mpiError == MPI_SUCCESS, "");
        }
        // SHOULD BE REMOVED AT SOME POINT
        else {
            Insist(false, "Could not send data.");
        }


        // Unlock window chunk
        // This will cause a window flush so one isn't needed above
        winChunkUnlock(mpiWin, adjRank, offRankOffset);
    }
}


/*
    recvData

    Implements one-sided MPI for receiving data.
*/
static
void recvData(UINT numAdjRanks,
              const vector<UINT> &onRankOffsets,
              const UINT packetSizeInBytes,
              TraverseData &traverseData, 
              set<pair<UINT,UINT>> &sideRecv,
              const MPI_Win &mpiWin)
{
    int myRank = Comm::rank();
    int mpiError;


    // Recv data from each adjacent rank
    for (UINT index = 0; index < numAdjRanks; index++) {
    
        // Useful values
        UINT onRankOffset = onRankOffsets[index];
        
        
        // Lock window chunk
        winChunkLock(mpiWin, myRank, onRankOffset);
        
        
        // Get number of packets written
        UINT numPacketsWritten = 0;
        mpiError = MPI_Get(&numPacketsWritten, 1, MPI_UINT64_T, myRank,
                           onRankOffset+8, 1, MPI_UINT64_T, mpiWin);
        Insist(mpiError == MPI_SUCCESS, "");
        mpiError = MPI_Win_flush_local(myRank, mpiWin);
        Insist(mpiError == MPI_SUCCESS, "");
        
        
        // Read packets;
        if (numPacketsWritten > 0) {
            UINT dataSizeInBytes = numPacketsWritten * packetSizeInBytes;
            UINT offset = onRankOffset + 16;
            vector<char> dataPackets(dataSizeInBytes);
            
            mpiError = MPI_Get(dataPackets.data(), dataSizeInBytes, 
                               MPI_BYTE, myRank, offset, dataSizeInBytes, 
                               MPI_BYTE, mpiWin);
            Insist(mpiError == MPI_SUCCESS, "");
            mpiError = MPI_Win_flush_local(myRank, mpiWin);
            Insist(mpiError == MPI_SUCCESS, "");
            
            
            // Unpack packets
            for (UINT i = 0; i < numPacketsWritten; i++) {
                char *packet = &dataPackets[i * packetSizeInBytes];
                UINT globalSide;
                UINT angle;
                char *packetData;
                splitPacket(packet, globalSide, angle, &packetData);
                
                UINT localSide = g_tychoMesh->getGLSide(globalSide);
                traverseData.setSideData(localSide, angle, packetData);
                sideRecv.insert(make_pair(localSide,angle));
            }


            // Reset numPacketsWritten
            numPacketsWritten = 0;
            mpiError = MPI_Put(&numPacketsWritten, 1, MPI_UINT64_T, myRank,
                               onRankOffset+8, 1, MPI_UINT64_T, mpiWin);
            Insist(mpiError == MPI_SUCCESS, "");
        }
        

        // Unlock window chunk
        // Does a window flush so one is not needed above
        winChunkUnlock(mpiWin, myRank, onRankOffset);
    }
}



/*
    sendAndRecvData()
    
    The algorithm is
    - Irecv data size for all adjacent ranks
    - ISend data size and then data (if any)
    - Wait on recv of data size, then blocking recv for data if there is any.
    
    Data is sent in two steps to each adjacent rank.
    First is the number of bytes of data that will be sent.
    Second is the raw data in bytes.
    The tag for the first send is 0.
    The tag for the second send is 1.
    
    The raw data is made of data packets containing:
    globalSide, angle, and data to send.
    The data can have different meanings depending on the TraverseData 
    subclass.
    
    When done traversing local graph, you want to stop all communication.
    This is done by setting killComm to true.
    To mark killing communication, sendSize is set to UINT64_MAX.
    In this event, commDark[rank] is set to true on the receiving rank 
    so we no longer look for communication from this rank.
*/
static
void sendAndRecvData(const vector<vector<char>> &sendBuffers, 
                     const vector<UINT> &adjRankIndexToRank, 
                     TraverseData &traverseData, 
                     const UINT dataSizeInBytes, 
                     set<pair<UINT,UINT>> &sideRecv,
                     vector<bool> &commDark, const bool killComm)
{
    // Check input
    Assert(adjRankIndexToRank.size() == sendBuffers.size());
    Assert(adjRankIndexToRank.size() == commDark.size());
    
    
    // Variables
    UINT numAdjRanks = adjRankIndexToRank.size();
    int mpiError;
    
    vector<UINT> recvSizes(numAdjRanks);
    vector<UINT> sendSizes(numAdjRanks);
    vector<MPI_Request> mpiRecvRequests(numAdjRanks);
    vector<MPI_Request> mpiSendRequests;
    UINT numRecv = numAdjRanks;
    
    
    // Setup recv of data size
    for (UINT index = 0; index < numAdjRanks; index++) {
        
        // No recv if adjRank is no longer communicating
        if (commDark[index]) {
            mpiRecvRequests[index] = MPI_REQUEST_NULL;
            numRecv--;
            continue;
        }
        
        // Irecv data size
        int numDataToRecv = 1;
        int adjRank = adjRankIndexToRank[index];
        int tag0 = 0;
        
        mpiError = MPI_Irecv(&recvSizes[index], numDataToRecv, MPI_UINT64_T, 
                             adjRank, tag0, MPI_COMM_WORLD, 
                             &mpiRecvRequests[index]);
        Insist(mpiError == MPI_SUCCESS, "");
    }
    
    
    // Send data size and data
    for (UINT index = 0; index < numAdjRanks; index++) {
        
        // Don't send if adjRank is no longer communicating
        if (commDark[index])
            continue;
        
        const vector<char> &sendBuffer = sendBuffers[index];
        int numDataToSend = 1;
        int adjRank = adjRankIndexToRank[index];
        int tag0 = 0;
        int tag1 = 1;
        
        
        // Send data size
        MPI_Request request;
        sendSizes[index] = sendBuffer.size();
        if (killComm)
            sendSizes[index] = UINT64_MAX;
        
        mpiError = MPI_Isend(&sendSizes[index], numDataToSend, MPI_UINT64_T, 
                             adjRank, tag0, MPI_COMM_WORLD, &request);
        Insist(mpiError == MPI_SUCCESS, "");
        mpiSendRequests.push_back(request);
        
        
        // Send data
        if (sendSizes[index] > 0 && sendSizes[index] != UINT64_MAX) {
            MPI_Request request;
            Assert(sendBuffer.size() < INT_MAX);
            
            mpiError = MPI_Isend(const_cast<char*>(sendBuffer.data()), 
                                 sendBuffer.size(), 
                                 MPI_BYTE, adjRank, tag1, 
                                 MPI_COMM_WORLD, &request);
            Insist(mpiError == MPI_SUCCESS, "");
            mpiSendRequests.push_back(request);
        }
    }
    
    
    // Recv data size and data
    for (UINT numWaits = 0; numWaits < numRecv; numWaits++) {
        
        // Wait for a data size to arrive
        int index;
        mpiError = MPI_Waitany(mpiRecvRequests.size(), mpiRecvRequests.data(), 
                               &index, MPI_STATUS_IGNORE);
        Insist(mpiError == MPI_SUCCESS, "");
        

        // Recv data
        if (recvSizes[index] > 0 && recvSizes[index] != UINT64_MAX) {
            
            int adjRank = adjRankIndexToRank[index];
            int tag1 = 1;
            vector<char> dataPackets(recvSizes[index]);
            
            mpiError = MPI_Recv(dataPackets.data(), recvSizes[index], 
                                MPI_BYTE, adjRank, tag1, MPI_COMM_WORLD, 
                                MPI_STATUS_IGNORE);
            Insist(mpiError == MPI_SUCCESS, "");
            
            UINT packetSize = 2 * sizeof(UINT) + dataSizeInBytes;
            UINT numPackets = recvSizes[index] / packetSize;
            Assert(recvSizes[index] % packetSize == 0);
            
            for (UINT i = 0; i < numPackets; i++) {
                char *packet = &dataPackets[i * packetSize];
                UINT globalSide;
                UINT angle;
                char *packetData;
                splitPacket(packet, globalSide, angle, &packetData);
                
                UINT localSide = g_tychoMesh->getGLSide(globalSide);
                traverseData.setSideData(localSide, angle, packetData);
                sideRecv.insert(make_pair(localSide,angle));
            }
        }
        
        
        // Stop communication with this rank
        if (recvSizes[index] == UINT64_MAX) {
            commDark[index] = true;
        }
    }
    
    
    // Make sure all sends are done
    if (mpiSendRequests.size() > 0) {
        mpiError = MPI_Waitall(mpiSendRequests.size(), mpiSendRequests.data(), 
                               MPI_STATUSES_IGNORE);
        Insist(mpiError == MPI_SUCCESS, "");
    }

}


/*
    GraphTraverser
    
    If doComm is true, graph traversal is global.
    If doComm is false, each mesh partition is traversed locally with no
    consideration for boundaries between partitions.
*/
GraphTraverser::GraphTraverser(Direction direction, bool doComm, 
                               UINT dataSizeInBytes)
    : c_direction(direction), c_doComm(doComm), 
      c_dataSizeInBytes(dataSizeInBytes)
{
    int mpiError;
    
    
    // Get adjacent ranks
    for (UINT cell = 0; cell < g_nCells; cell++) {
    for (UINT face = 0; face < g_nFacePerCell; face++) {
        
        UINT adjRank = g_tychoMesh->getAdjRank(cell, face);
        UINT adjCell = g_tychoMesh->getAdjCell(cell, face);
        
        if (adjCell == TychoMesh::BOUNDARY_FACE && 
            adjRank != TychoMesh::BAD_RANK &&
            c_adjRankToRankIndex.count(adjRank) == 0)
        {
            UINT rankIndex = c_adjRankIndexToRank.size();
            c_adjRankToRankIndex.insert(make_pair(adjRank, rankIndex));
            c_adjRankIndexToRank.push_back(adjRank);
        }
    }}
    
    
    // Calc num dependencies for each (cell, angle) pair
    c_initNumDependencies.resize(g_nAngles, g_nCells);
    for (UINT cell = 0; cell < g_nCells; cell++) {
    for (UINT angle = 0; angle < g_nAngles; angle++) {
        
        c_initNumDependencies(angle, cell) = 0;
        for (UINT face = 0; face < g_nFacePerCell; face++) {
            
            bool incoming = isIncoming(angle, cell, face, c_direction);
            UINT adjRank = g_tychoMesh->getAdjRank(cell, face);
            UINT adjCell = g_tychoMesh->getAdjCell(cell, face);
            
            if (c_doComm && incoming && adjRank != TychoMesh::BAD_RANK) {
                c_initNumDependencies(angle, cell)++;
            }
            else if (!c_doComm && incoming && adjCell != TychoMesh::BOUNDARY_FACE) {
                c_initNumDependencies(angle, cell)++;
            }
        }
    }}

    
    // Allocate MPI_Win
    UINT numAdjRanks = c_adjRankIndexToRank.size();
    UINT packetSize = 2 * sizeof(UINT) + c_dataSizeInBytes;
    UINT windowSizeInBytes = 
        (16 + MAX_PACKETS * packetSize) * numAdjRanks;
    MPI_Win_allocate(windowSizeInBytes, 1, MPI_INFO_NULL, 
                     MPI_COMM_WORLD, &c_mpiWinMemory, &c_mpiWin);


    // Setup onRankOffsets
    c_onRankOffsets.resize(numAdjRanks);
    for (UINT i = 0; i < numAdjRanks; i++) {
        c_onRankOffsets[i] = i * (16 + MAX_PACKETS * packetSize);
    }


    // Setup offRankOffsets
    c_offRankOffsets.resize(numAdjRanks);
    vector<MPI_Request> mpiSendRequests(numAdjRanks);
    vector<MPI_Request> mpiRecvRequests(numAdjRanks);
    for (UINT i = 0; i < numAdjRanks; i++) {
        UINT adjRank = c_adjRankIndexToRank[i];
        int tag = 0;
        
        // Send index
        mpiError = MPI_Isend(&c_onRankOffsets[i], 1, MPI_UINT64_T, adjRank, 
                             tag, MPI_COMM_WORLD, &mpiSendRequests[i]);
        Insist(mpiError == MPI_SUCCESS, "");

        // Recv index
        mpiError = MPI_Irecv(&c_offRankOffsets[i], 1, MPI_UINT64_T, adjRank, 
                             tag, MPI_COMM_WORLD, &mpiRecvRequests[i]);
        Insist(mpiError == MPI_SUCCESS, "");
    }
    

    // Wait for messages to send/recv
    if (numAdjRanks > 0) {
        mpiError = MPI_Waitall(mpiSendRequests.size(), mpiSendRequests.data(), 
                               MPI_STATUSES_IGNORE);
        Insist(mpiError == MPI_SUCCESS, "");
        
        mpiError = MPI_Waitall(mpiRecvRequests.size(), mpiRecvRequests.data(), 
                               MPI_STATUSES_IGNORE);
        Insist(mpiError == MPI_SUCCESS, "");
    }


    // Lock the window to start RMA operations and set memory to zero
    MPI_Win_lock_all(0, c_mpiWin);
    memset(c_mpiWinMemory, 0, windowSizeInBytes);
    MPI_Win_sync(c_mpiWin);
    MPI_Barrier(MPI_COMM_WORLD);
    

    // Print out use of one-sided MPI
    if (Comm::rank() == 0) {
        if (ONE_SIDED)
            printf("Using one-sided MPI.\n");
        else
            printf("NOT using one-sided MPI.\n");
    }
}


/*
    ~GraphTraverser
*/
GraphTraverser::~GraphTraverser()
{
    MPI_Win_unlock_all(c_mpiWin);
    MPI_Win_free(&c_mpiWin);
}


/*
    traverse
    
    Traverses g_tychoMesh.
*/
void GraphTraverser::traverse(const UINT maxComputePerStep,
                              TraverseData &traverseData)
{
    vector<priority_queue<Tuple>> canCompute(g_nThreads);
    Mat2<UINT> numDependencies(g_nAngles, g_nCells);
    UINT numCellAnglePairsToCalculate = g_nAngles * g_nCells;
    set<pair<UINT,UINT>> sideRecv;
    Mat2<vector<char>> sendBuffers;
    vector<vector<char>> sendBuffers1;
    vector<bool> commDark;
    Timer totalTimer;
    Timer setupTimer;
    Timer commTimer;
    Timer sendTimer;
    Timer recvTimer;
    

    // Start total timer
    totalTimer.start();
    setupTimer.start();
    
    
    // Calc num dependencies for each (cell, angle) pair
    for (UINT cell = 0; cell < g_nCells; cell++) {
    for (UINT angle = 0; angle < g_nAngles; angle++) {
        numDependencies(angle, cell) = c_initNumDependencies(angle, cell);
    }}
    
    
    // Set size of sendBuffers and commDark
    UINT numAdjRanks = c_adjRankIndexToRank.size();
    sendBuffers.resize(g_nThreads, numAdjRanks);
    sendBuffers1.resize(numAdjRanks);
    commDark.resize(numAdjRanks, false);
    
    
    // Initialize canCompute queue
    for (UINT cell = 0; cell < g_nCells; cell++) {
    for (UINT angle = 0; angle < g_nAngles; angle++) {
        if (numDependencies(angle, cell) == 0) {
            UINT priority = traverseData.getPriority(cell, angle);
            UINT angleGroup = angleGroupIndex(angle);
            canCompute[angleGroup].push(Tuple(cell, angle, priority));
        }
    }}


    // End setup timer
    setupTimer.stop();
    
    
    // Traverse the graph
    while (numCellAnglePairsToCalculate > 0) {
        
        // Do local traversal
        #pragma omp parallel
        {
            UINT stepsTaken = 0;
            UINT angleGroup = omp_get_thread_num();
            while (canCompute[angleGroup].size() > 0 && 
                   stepsTaken < maxComputePerStep)
            {
                // Get cell/angle pair to compute
                Tuple cellAnglePair = canCompute[angleGroup].top();
                canCompute[angleGroup].pop();
                UINT cell = cellAnglePair.getCell();
                UINT angle = cellAnglePair.getAngle();
                stepsTaken++;
                
                #pragma omp atomic
                numCellAnglePairsToCalculate--;
                
                
                // Get boundary type and adjacent cell/side data for each face
                BoundaryType bdryType[g_nFacePerCell];
                UINT adjCellsSides[g_nFacePerCell];
                bool isOutgoingWrtDirection[g_nFacePerCell];
                for (UINT face = 0; face < g_nFacePerCell; face++) {
                    
                    UINT adjCell = g_tychoMesh->getAdjCell(cell, face);
                    UINT adjRank = g_tychoMesh->getAdjRank(cell, face);
                    adjCellsSides[face] = adjCell;
                    
                    if (g_tychoMesh->isOutgoing(angle, cell, face)) {
                        
                        if (adjCell == TychoMesh::BOUNDARY_FACE && 
                            adjRank != TychoMesh::BAD_RANK)
                        {
                            bdryType[face] = BoundaryType_OutIntBdry;
                            adjCellsSides[face] = 
                                g_tychoMesh->getSide(cell, face);
                        }
                        
                        else if (adjCell == TychoMesh::BOUNDARY_FACE && 
                                 adjRank == TychoMesh::BAD_RANK)
                        {
                            bdryType[face] = BoundaryType_OutExtBdry;
                        }
                        
                        else {
                            bdryType[face] = BoundaryType_OutInt;
                        }
                        
                        if (c_direction == Direction_Forward) {
                            isOutgoingWrtDirection[face] = true;
                        }
                        else {
                            isOutgoingWrtDirection[face] = false;
                        }
                    }
                    else {
                        
                        if (adjCell == TychoMesh::BOUNDARY_FACE && 
                            adjRank != TychoMesh::BAD_RANK)
                        {
                            bdryType[face] = BoundaryType_InIntBdry;
                            adjCellsSides[face] = 
                                g_tychoMesh->getSide(cell, face);
                        }
                        
                        else if (adjCell == TychoMesh::BOUNDARY_FACE && 
                                 adjRank == TychoMesh::BAD_RANK)
                        {
                            bdryType[face] = BoundaryType_InExtBdry;
                        }
                        
                        else {
                            bdryType[face] = BoundaryType_InInt;
                        }
                        
                        if (c_direction == Direction_Forward) {
                            isOutgoingWrtDirection[face] = false;
                        }
                        else {
                            isOutgoingWrtDirection[face] = true;
                        }
                    }
                }
                
                
                // Update data for this cell-angle pair
                traverseData.update(cell, angle, adjCellsSides, bdryType);
                
                
                // Update dependency for children
                for (UINT face = 0; face < g_nFacePerCell; face++) {
                    
                    if (isOutgoingWrtDirection[face]) {

                        UINT adjCell = g_tychoMesh->getAdjCell(cell, face);
                        UINT adjRank = g_tychoMesh->getAdjRank(cell, face);
                        
                        if (adjCell != TychoMesh::BOUNDARY_FACE) {
                            numDependencies(angle, adjCell)--;
                            if (numDependencies(angle, adjCell) == 0) {
                                UINT priority = 
                                    traverseData.getPriority(adjCell, angle);
                                Tuple tuple(adjCell, angle, priority);
                                canCompute[angleGroup].push(tuple);
                            }
                        }
                        
                        else if (c_doComm && adjRank != TychoMesh::BAD_RANK) {
                            UINT rankIndex = c_adjRankToRankIndex.at(adjRank);
                            UINT side = g_tychoMesh->getSide(cell, face);
                            UINT globalSide = g_tychoMesh->getLGSide(side);
                            
                            vector<char> packet;
                            createPacket(packet, globalSide, angle, 
                                         c_dataSizeInBytes, 
                                         traverseData.getData(cell, face, angle));
                            
                            sendBuffers(angleGroup, rankIndex).insert(
                                sendBuffers(angleGroup, rankIndex).end(), 
                                packet.begin(), packet.end());
                        }
                    }
                }
            }
        }
        
        
        // Put together sendBuffers from different angleGroups
        for (UINT angleGroup = 0; angleGroup < g_nThreads; angleGroup++) {
        for (UINT rankIndex = 0; rankIndex < numAdjRanks; rankIndex++) {
            sendBuffers1[rankIndex].insert(
                sendBuffers1[rankIndex].end(), 
                sendBuffers(angleGroup, rankIndex).begin(), 
                sendBuffers(angleGroup, rankIndex).end());
        }}
               
        
        // Do communication
        commTimer.start();
        if (c_doComm) {
            
            // Send/Recv
            sideRecv.clear();
            
            if (!ONE_SIDED) {
                const bool killComm = false;
                sendAndRecvData(sendBuffers1, c_adjRankIndexToRank, traverseData, 
                                c_dataSizeInBytes, sideRecv, commDark, killComm);
            }
            else {
                UINT packetSizeInBytes = 2 * sizeof(UINT) + c_dataSizeInBytes;

                sendTimer.start();
                sendData(sendBuffers1, c_adjRankIndexToRank, c_offRankOffsets, 
                         packetSizeInBytes, MAX_PACKETS, c_mpiWin);
                sendTimer.stop();

                recvTimer.start();
                recvData(c_adjRankIndexToRank.size(), c_onRankOffsets, 
                         packetSizeInBytes, traverseData, sideRecv, c_mpiWin);
                recvTimer.stop();
            }

            
            // Clear send buffers for next iteration
            for (UINT angleGroup = 0; angleGroup < g_nThreads; angleGroup++) {
            for (UINT rankIndex = 0; rankIndex < numAdjRanks; rankIndex++) {
                sendBuffers(angleGroup, rankIndex).clear();
            }}
            
            for (UINT rankIndex = 0; rankIndex < numAdjRanks; rankIndex++) {
                sendBuffers1[rankIndex].clear();
            }
            
            
            // Update dependency for parents using received side data
            for (auto sideAngle : sideRecv) {
                UINT side = sideAngle.first;
                UINT angle = sideAngle.second;
                UINT cell = g_tychoMesh->getSideCell(side);
                numDependencies(angle, cell)--;
                if (numDependencies(angle, cell) == 0) {
                    UINT priority = traverseData.getPriority(cell, angle);
                    Tuple tuple(cell, angle, priority);
                    canCompute[angleGroupIndex(angle)].push(tuple);
                }
            }
        }
        commTimer.stop();
    }
    
    
    // Send kill comm signal to adjacent ranks
    if (!ONE_SIDED) {
        commTimer.start();
        if (c_doComm) {
            const bool killComm = true;
            sendAndRecvData(sendBuffers1, c_adjRankIndexToRank, traverseData, 
                            c_dataSizeInBytes, sideRecv, commDark, killComm);
        }
        commTimer.stop();
    }

    
    // Print times
    totalTimer.stop();

    double totalTime = totalTimer.wall_clock();
    Comm::gmax(totalTime);

    double setupTime = setupTimer.wall_clock();
    Comm::gmax(setupTime);

    double commTime = commTimer.sum_wall_clock();
    Comm::gmax(commTime);
    
    double sendTime = sendTimer.sum_wall_clock();
    Comm::gmax(sendTime);
    
    double recvTime = recvTimer.sum_wall_clock();
    Comm::gmax(recvTime);
    
    if (Comm::rank() == 0) {
        printf("      Traverse Timer (comm):    %fs\n", commTime);
        printf("      Traverse Timer (send):    %fs\n", sendTime);
        printf("      Traverse Timer (recv):    %fs\n", recvTime);
        printf("      Traverse Timer (setup):   %fs\n", setupTime);
        printf("      Traverse Timer (total):   %fs\n", totalTime);
    }
}


