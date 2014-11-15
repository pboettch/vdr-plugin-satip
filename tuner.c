/*
 * tuner.c: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include "common.h"
#include "config.h"
#include "discover.h"
#include "poller.h"
#include "tuner.h"

cSatipTuner::cSatipTuner(cSatipDeviceIf &deviceP, unsigned int packetLenP)
: cThread(cString::sprintf("SAT>IP %d tuner", deviceP.GetId())),
  sleepM(),
  deviceM(&deviceP),
  deviceIdM(deviceP.GetId()),
  rtspM(new cSatipRtsp(*this)),
  rtpM(new cSatipRtp(deviceP, packetLenP)),
  rtcpM(new cSatipRtcp(*this, 1500)),
  streamAddrM(""),
  streamParamM(""),
  currentServerM(NULL),
  nextServerM(NULL),
  mutexM(),
  keepAliveM(),
  statusUpdateM(),
  pidUpdateCacheM(),
  sessionM(""),
  tunerStatusM(tsIdle),
  timeoutM(eMinKeepAliveIntervalMs),
  hasLockM(false),
  signalStrengthM(-1),
  signalQualityM(-1),
  streamIdM(-1),
  addPidsM(),
  delPidsM(),
  pidsM()
{
  debug("cSatipTuner::%s(%d) [device %d]", __FUNCTION__, packetLenP, deviceIdM);

  // Open sockets
  int i = 100;
  while (i-- > 0) {
        if (rtpM->Open(0) && rtcpM->Open(rtpM->Port() + 1))
           break;
        rtpM->Close();
        rtcpM->Close();
        }
  if ((rtpM->Port() <= 0) || (rtcpM->Port() <= 0)) {
     error("Cannot open required RTP/RTCP ports [device %d]", deviceIdM);
     }
  // Must be done after socket initialization!
  cSatipPoller::GetInstance()->Register(*rtpM);
  cSatipPoller::GetInstance()->Register(*rtcpM);

  // Start thread
  Start();
}

cSatipTuner::~cSatipTuner()
{
  debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);

  tunerStatusM = tsIdle;

  // Stop thread
  sleepM.Signal();
  if (Running())
     Cancel(3);
  Close();

  // Close the listening sockets
  cSatipPoller::GetInstance()->Unregister(*rtcpM);
  cSatipPoller::GetInstance()->Unregister(*rtpM);
  rtcpM->Close();
  rtpM->Close();

  // Free allocated memory
  DELETENULL(rtpM);
  DELETENULL(rtcpM);
  DELETENULL(rtspM);
}

void cSatipTuner::Action(void)
{
  debug("cSatipTuner::%s(): entering [device %d]", __FUNCTION__, deviceIdM);
  cTimeMs reconnection(eConnectTimeoutMs);
  // Do the thread loop
  while (Running()) {
        switch (tunerStatusM) {
          case tsIdle:
               //debug("cSatipTuner::%s(): tsIdle [device %d]", __FUNCTION__, deviceIdM);
               break;
          case tsRelease:
               //debug("cSatipTuner::%s(): tsRelease [device %d]", __FUNCTION__, deviceIdM);
               Disconnect();
               tunerStatusM = tsIdle;
               break;
          case tsSet:
               //debug("cSatipTuner::%s(): tsSet [device %d]", __FUNCTION__, deviceIdM);
               reconnection.Set(eConnectTimeoutMs);
               Disconnect();
               if (Connect()) {
                  tunerStatusM = tsTuned;
                  UpdatePids(true);
                  }
               else
                  tunerStatusM = tsIdle;
               break;
          case tsTuned:
               //debug("cSatipTuner::%s(): tsTuned [device %d]", __FUNCTION__, deviceIdM);
               reconnection.Set(eConnectTimeoutMs);
               // Read reception statistics via DESCRIBE and RTCP
               if (hasLockM || ReadReceptionStatus()) {
                  // Quirk for devices without valid reception data
                  if (currentServerM && currentServerM->Quirk(cSatipServer::eSatipQuirkForceLock)) {
                     hasLockM = true;
                     signalStrengthM = eDefaultSignalStrength;
                     signalQualityM = eDefaultSignalQuality;
                     }
                  if (hasLockM)
                     tunerStatusM = tsLocked;
                  }
               break;
          case tsLocked:
               //debug("cSatipTuner::%s(): tsLocked [device %d]", __FUNCTION__, deviceIdM);
               tunerStatusM = tsLocked;
               if (!UpdatePids()) {
                  debug("cSatipTuner::%s(): pid update failed - re-tuning [device %d]", __FUNCTION__, deviceIdM);
                  tunerStatusM = tsSet;
                  break;
                  }
               if (!KeepAlive()) {
                  debug("cSatipTuner::%s(): keep-alive failed - re-tuning [device %d]", __FUNCTION__, deviceIdM);
                  tunerStatusM = tsSet;
                  break;
                  }
               if (reconnection.TimedOut()) {
                  debug("cSatipTuner::%s(): connection timeout - re-tuning [device %d]", __FUNCTION__, deviceIdM);
                  tunerStatusM = tsSet;
                  break;
                  }
               break;
          default:
               error("Unknown tuner status %d", tunerStatusM);
               break;
          }
        sleepM.Wait(100); // to avoid busy loop and reduce cpu load
        }
  debug("cSatipTuner::%s(): exiting [device %d]", __FUNCTION__, deviceIdM);
}

bool cSatipTuner::Open(void)
{
  cMutexLock MutexLock(&mutexM);
  debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  tunerStatusM = tsSet;
  return true;
}

bool cSatipTuner::Close(void)
{
  cMutexLock MutexLock(&mutexM);
  debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  tunerStatusM = tsRelease;
  return true;
}

bool cSatipTuner::Connect(void)
{
  cMutexLock MutexLock(&mutexM);
  debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);

  if (!isempty(*streamAddrM)) {
     cString connectionUri = cString::sprintf("rtsp://%s", *streamAddrM);
     cString uri = cString::sprintf("%s/?%s", *connectionUri, *streamParamM);
     // Just retune
     if ((tunerStatusM >= tsTuned) && (streamIdM >= 0) && rtpM && rtcpM) {
        debug("cSatipTuner::%s(): retune [device %d]", __FUNCTION__, deviceIdM);
        KeepAlive(true);
        // Flush any old content
        rtpM->Flush();
        return rtspM->Setup(*uri, rtpM->Port(), rtcpM->Port());
        }
     keepAliveM.Set(timeoutM);
     if (rtspM->Options(*connectionUri)) {
        if (nextServerM && nextServerM->Quirk(cSatipServer::eSatipQuirkSessionId))
           rtspM->SetSession(SkipZeroes(*sessionM));
        if (rtspM->Setup(*uri, rtpM->Port(), rtcpM->Port())) {
           if (nextServerM) {
              cSatipDiscover::GetInstance()->UseServer(nextServerM, true);
              currentServerM = nextServerM;
              nextServerM = NULL;
              }
           }
           return true;
        }
     }

  return false;
}

bool cSatipTuner::Disconnect(void)
{
  cMutexLock MutexLock(&mutexM);
  debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);

  if ((tunerStatusM != tsIdle) && !isempty(*streamAddrM) && (streamIdM >= 0)) {
     cString uri = cString::sprintf("rtsp://%s/stream=%d", *streamAddrM, streamIdM);
     rtspM->Teardown(*uri);
     }
  tunerStatusM = tsIdle;

  // Reset signal parameters
  hasLockM = false;
  signalStrengthM = -1;
  signalQualityM = -1;

  if (currentServerM)
     cSatipDiscover::GetInstance()->UseServer(currentServerM, false);
  statusUpdateM.Set(0);
  timeoutM = eMinKeepAliveIntervalMs;
  addPidsM.Clear();
  delPidsM.Clear();

  // return always true
  return true;
}

void cSatipTuner::ParseReceptionParameters(u_char *bufferP, int lengthP)
{
  //debug("cSatipTuner::%s(%s, %d) [device %d]", __FUNCTION__, bufferP, lengthP, deviceIdM);
  // DVB-S2:
  // ver=<major>.<minor>;src=<srcID>;tuner=<feID>,<level>,<lock>,<quality>,<frequency>,<polarisation>,<system>,<type>,<pilots>,<roll_off>,<symbol_rate>,<fec_inner>;pids=<pid0>,...,<pidn>
  // DVB-T2:
  // ver=1.1;tuner=<feID>,<level>,<lock>,<quality>,<freq>,<bw>,<msys>,<tmode>,<mtype>,<gi>,<fec>,<plp>,<t2id>,<sm>;pids=<pid0>,...,<pidn>
  if (lengthP > 0) {
     char *s = strdup((char *)bufferP);
     char *c = strstr(s, ";tuner=");
     if (c)  {
        int value;

        // level:
        // Numerical value between 0 and 255
        // An incoming L-band satellite signal of
        // -25dBm corresponds to 224
        // -65dBm corresponds to 32
        // No signal corresponds to 0
        c = strstr(c, ",");
        value = atoi(++c);
        // Scale value to 0-100
        signalStrengthM = (value >= 0) ? (value * 100 / 255) : -1;

        // lock:
        // lock Set to one of the following values:
        // "0" the frontend is not locked
        // "1" the frontend is locked
        c = strstr(c, ",");
        hasLockM = !!atoi(++c);

        // quality:
        // Numerical value between 0 and 15
        // Lowest value corresponds to highest error rate
        // The value 15 shall correspond to
        // -a BER lower than 2x10-4 after Viterbi for DVB-S
        // -a PER lower than 10-7 for DVB-S2
        c = strstr(c, ",");
        value = atoi(++c);
        // Scale value to 0-100
        signalQualityM = (hasLockM && (value >= 0)) ? (value * 100 / 15) : 0;
        }
     free(s);
     }
}

void cSatipTuner::SetStreamId(int streamIdP)
{
  cMutexLock MutexLock(&mutexM);
  debug("cSatipTuner::%s(%d) [device %d]", __FUNCTION__, streamIdP, deviceIdM);
  streamIdM = streamIdP;
}

void cSatipTuner::SetSessionTimeout(const char *sessionP, int timeoutP)
{
  cMutexLock MutexLock(&mutexM);
  debug("cSatipTuner::%s(%s, %d) [device %d]", __FUNCTION__, sessionP, timeoutP, deviceIdM);
  sessionM = sessionP;
  timeoutM = (timeoutP > eMinKeepAliveIntervalMs) ? timeoutP : eMinKeepAliveIntervalMs;
}

int cSatipTuner::GetId(void)
{
  //debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  return deviceIdM;
}

bool cSatipTuner::SetSource(cSatipServer *serverP, const char *parameterP, const int indexP)
{
  debug("cSatipTuner::%s(%s, %d) [device %d]", __FUNCTION__, parameterP, indexP, deviceIdM);
  cMutexLock MutexLock(&mutexM);
  if (serverP) {
     nextServerM = cSatipDiscover::GetInstance()->GetServer(serverP);
     if (rtspM && nextServerM && !isempty(nextServerM->Address()) && !isempty(parameterP)) {
        // Update stream address and parameter
        streamAddrM = rtspM->RtspUnescapeString(nextServerM->Address());
        streamParamM = rtspM->RtspUnescapeString(parameterP);
        tunerStatusM = tsSet;
        }
     }
  else
     tunerStatusM = tsRelease;
  sleepM.Signal();
  return true;
}

bool cSatipTuner::SetPid(int pidP, int typeP, bool onP)
{
  //debug("cSatipTuner::%s(%d, %d, %d) [device %d]", __FUNCTION__, pidP, typeP, onP, deviceIdM);
  cMutexLock MutexLock(&mutexM);
  if (onP) {
     pidsM.AddPid(pidP);
     addPidsM.AddPid(pidP);
     delPidsM.RemovePid(pidP);
     }
  else {
     pidsM.RemovePid(pidP);
     delPidsM.AddPid(pidP);
     addPidsM.RemovePid(pidP);
     }
  pidUpdateCacheM.Set(ePidUpdateIntervalMs);
  sleepM.Signal();
  return true;
}

bool cSatipTuner::UpdatePids(bool forceP)
{
  //debug("cSatipTuner::%s(%d) tunerStatus=%s [device %d]", __FUNCTION__, forceP, TunerStatusString(tunerStatusM), deviceIdM);
  if (((forceP && pidsM.Size()) || (pidUpdateCacheM.TimedOut() && (addPidsM.Size() || delPidsM.Size()))) &&
      (tunerStatusM >= tsTuned) && !isempty(*streamAddrM) && (streamIdM > 0)) {
     cString uri = cString::sprintf("rtsp://%s/stream=%d", *streamAddrM, streamIdM);
     bool usedummy = !!(currentServerM && currentServerM->Quirk(cSatipServer::eSatipQuirkPlayPids));
     if (forceP || usedummy) {
        if (pidsM.Size()) {
           uri = cString::sprintf("%s?pids=", *uri);
           for (int i = 0; i < pidsM.Size(); ++i)
               uri = cString::sprintf("%s%d%s", *uri, pidsM[i], (i == (pidsM.Size() - 1)) ? "" : ",");
           }
        if (usedummy && (pidsM.Size() == 1) && (pidsM[0] < 0x20))
           uri = cString::sprintf("%s,%d", *uri, eDummyPid);
        }
     else {
        if (addPidsM.Size()) {
           uri = cString::sprintf("%s?addpids=", *uri);
           for (int i = 0; i < addPidsM.Size(); ++i)
               uri = cString::sprintf("%s%d%s", *uri, addPidsM[i], (i == (addPidsM.Size() - 1)) ? "" : ",");
           }
        if (delPidsM.Size()) {
           uri = cString::sprintf("%s%sdelpids=", *uri, addPidsM.Size() ? "&" : "?");
           for (int i = 0; i < delPidsM.Size(); ++i)
               uri = cString::sprintf("%s%d%s", *uri, delPidsM[i], (i == (delPidsM.Size() - 1)) ? "" : ",");
           }
        }
     if (!rtspM->Play(*uri))
        return false;
     addPidsM.Clear();
     delPidsM.Clear();
     }

  return true;
}

bool cSatipTuner::KeepAlive(bool forceP)
{
  //debug("cSatipTuner::%s(%d) tunerStatus=%s [device %d]", __FUNCTION__, forceP, TunerStatusString(tunerStatusM), deviceIdM);
  cMutexLock MutexLock(&mutexM);
  if (keepAliveM.TimedOut()) {
     keepAliveM.Set(timeoutM);
     forceP = true;
     }
  if (forceP && !isempty(*streamAddrM) && (streamIdM > 0)) {
     cString uri = cString::sprintf("rtsp://%s/stream=%d", *streamAddrM, streamIdM);
     if (!rtspM->Options(*uri))
        return false;
     }

  return true;
}

bool cSatipTuner::ReadReceptionStatus(bool forceP)
{
  //debug("cSatipTuner::%s(%d) tunerStatus=%s [device %d]", __FUNCTION__, forceP, TunerStatusString(tunerStatusM), deviceIdM);
  cMutexLock MutexLock(&mutexM);
  if (statusUpdateM.TimedOut()) {
     statusUpdateM.Set(eStatusUpdateTimeoutMs);
     forceP = true;
     }
  if (forceP && !isempty(*streamAddrM) && (streamIdM > 0)) {
     cString uri = cString::sprintf("rtsp://%s/stream=%d", *streamAddrM, streamIdM);
     if (rtspM->Describe(*uri))
        return true;
     }

  return false;
}

const char *cSatipTuner::TunerStatusString(eTunerStatus statusP)
{
  switch (statusP) {
    case tsIdle:
         return "tsIdle";
    case tsRelease:
         return "tsRelease";
    case tsSet:
         return "tsSet";
    case tsLocked:
         return "tsLocked";
    case tsTuned:
         return "tsTuned";
    default:
         break;
    }
  return "---";
}

int cSatipTuner::SignalStrength(void)
{
  //debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  return signalStrengthM;
}

int cSatipTuner::SignalQuality(void)
{
  //debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  return signalQualityM;
}

bool cSatipTuner::HasLock(void)
{
  //debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  return (tunerStatusM >= tsTuned) && hasLockM;
}

cString cSatipTuner::GetSignalStatus(void)
{
  //debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  return cString::sprintf("lock=%d strength=%d quality=%d", HasLock(), SignalStrength(), SignalQuality());
}

cString cSatipTuner::GetInformation(void)
{
  //debug("cSatipTuner::%s() [device %d]", __FUNCTION__, deviceIdM);
  return (tunerStatusM >= tsTuned) ? cString::sprintf("rtsp://%s/?%s [stream=%d]", *streamAddrM, *streamParamM, streamIdM) : "connection failed";
}
