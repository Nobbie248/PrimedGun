#!/usr/bin/env python3
# GalaxyFrame: SteamVR's compositor frame timing, a line a
# second, for whatever app has the scene (IVRCompositor::GetFrameTimings and
# GetCumulativeStats through ctypes).  Read-only: it connects as a background
# app and changes nothing.  The game's own log counts the frames it was late
# for (shown twice, missed refreshes); this shows what the compositor did with
# the frames it got (reused, mispresented, dropped, reprojected), and the
# compositor's GPU time.  tools/session-report.py reads both logs.
# Usage: env -u LD_LIBRARY_PATH python3 -I vrtiming.py SECONDS [out.txt]
# the wrapper runs it for the length of a session into
# ~/.local/share/GalaxyFrame/vrtiming.log (GQ_VRTIMING=0 turns that off).
# Origin: tools/vrtiming.py of Moonlight XR Native (GPLv3).
import ctypes, os, sys, time

class HmdMatrix34(ctypes.Structure):
    _fields_ = [('m', ctypes.c_float * 12)]
class HmdVector3(ctypes.Structure):
    _fields_ = [('v', ctypes.c_float * 3)]
class TrackedDevicePose(ctypes.Structure):
    _fields_ = [('mDeviceToAbsoluteTracking', HmdMatrix34), ('vVelocity', HmdVector3),
                ('vAngularVelocity', HmdVector3), ('eTrackingResult', ctypes.c_int),
                ('bPoseIsValid', ctypes.c_bool), ('bDeviceIsConnected', ctypes.c_bool)]
class FrameTiming(ctypes.Structure):
    _fields_ = [('m_nSize', ctypes.c_uint32), ('m_nFrameIndex', ctypes.c_uint32),
                ('m_nNumFramePresents', ctypes.c_uint32), ('m_nNumMisPresented', ctypes.c_uint32),
                ('m_nNumDroppedFrames', ctypes.c_uint32), ('m_nReprojectionFlags', ctypes.c_uint32),
                ('m_flSystemTimeInSeconds', ctypes.c_double)] + [(n, ctypes.c_float) for n in (
                'm_flPreSubmitGpuMs', 'm_flPostSubmitGpuMs', 'm_flTotalRenderGpuMs', 'm_flCompositorRenderGpuMs',
                'm_flCompositorRenderCpuMs', 'm_flCompositorIdleCpuMs', 'm_flClientFrameIntervalMs',
                'm_flPresentCallCpuMs', 'm_flWaitForPresentCpuMs', 'm_flSubmitFrameMs', 'm_flWaitGetPosesCalledMs',
                'm_flNewPosesReadyMs', 'm_flNewFrameReadyMs', 'm_flCompositorUpdateStartMs',
                'm_flCompositorUpdateEndMs', 'm_flCompositorRenderStartMs')] + [
                ('m_HmdPose', TrackedDevicePose), ('m_nNumVSyncsReadyForUse', ctypes.c_uint32),
                ('m_nNumVSyncsToFirstView', ctypes.c_uint32)]
class CumulativeStats(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in (
                'm_nPid', 'm_nNumFramePresents', 'm_nNumDroppedFrames', 'm_nNumReprojectedFrames',
                'm_nNumFramePresentsOnStartup', 'm_nNumDroppedFramesOnStartup', 'm_nNumReprojectedFramesOnStartup',
                'm_nNumLoading', 'm_nNumFramePresentsLoading', 'm_nNumDroppedFramesLoading',
                'm_nNumReprojectedFramesLoading', 'm_nNumTimedOut', 'm_nNumFramePresentsTimedOut',
                'm_nNumDroppedFramesTimedOut', 'm_nNumReprojectedFramesTimedOut', 'm_nNumFrameSubmits')] + [
                (n, ctypes.c_double) for n in ('m_flSumCompositorCPUTimeMS', 'm_flSumCompositorGPUTimeMS',
                'm_flSumTargetFrameTimes', 'm_flSumApplicationCPUTimeMS', 'm_flSumApplicationGPUTimeMS')] + [
                ('m_nNumFramesWithDepth', ctypes.c_uint32)]

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30
out = open(sys.argv[2], 'w') if len(sys.argv) > 2 else None
def say(s):
    print(s, flush=True)
    if out: out.write(s + '\n'); out.flush()

lib = ctypes.CDLL('/opt/steamvr/bin/linuxarm64/libopenvr_api.so')
err = ctypes.c_int(0)
lib.VR_InitInternal2.restype = ctypes.c_uint32
lib.VR_InitInternal2.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_char_p]
t_end = time.time() + seconds
# SteamVR may come up after the launcher: try again every 5 s until it does
while True:
    lib.VR_InitInternal2(ctypes.byref(err), 3, None)   # VRApplication_Background
    if not err.value:
        break
    if time.time() + 5 >= t_end:
        say('VR init failed %d (is SteamVR running?)' % err.value); sys.exit(1)
    time.sleep(5)
lib.VR_GetGenericInterface.restype = ctypes.c_void_p
lib.VR_GetGenericInterface.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
p = lib.VR_GetGenericInterface(b'FnTable:IVRCompositor_027', ctypes.byref(err))
if not p:
    say('no IVRCompositor_027 (%d)' % err.value); sys.exit(1)
table = (ctypes.c_void_p * 12).from_address(p)
GetFrameTimings = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.POINTER(FrameTiming), ctypes.c_uint32)(table[9])
GetCumulativeStats = ctypes.CFUNCTYPE(None, ctypes.POINTER(CumulativeStats), ctypes.c_uint32)(table[11])

def cumulative():
    c = CumulativeStats()
    GetCumulativeStats(ctypes.byref(c), ctypes.sizeof(c))
    return c

N = 128
buf = (FrameTiming * N)()
c0 = cumulative()
say('# scene app pid %d; per second: compositor frames, app frames reused (presents>1), mispresented, dropped, '
    'reprojected cpu/gpu, async on, motion, compositor gpu ms, app interval ms avg/max, poses->frame ready ms avg/max, '
    'vsyncs to photons' % c0.m_nPid)
say('# %s' % time.strftime('%Y-%m-%d %H:%M:%S'))
say('time      frames reused mispres dropped rp_cpu rp_gpu async motion  compGPU  interval(avg/max)  app(avg/max)  vsyncs')
seen = set()
win = []
t_next = time.time() + 1.0
pid = c0.m_nPid
totals = dict(frames=0, reused=0, mis=0, dropped=0, cpu=0, gpu=0)
while time.time() < t_end:
    time.sleep(0.25)
    buf[0].m_nSize = ctypes.sizeof(FrameTiming)
    n = GetFrameTimings(buf, N)
    for i in range(n):
        f = buf[i]
        if f.m_nFrameIndex in seen: continue
        seen.add(f.m_nFrameIndex)
        win.append((f.m_nFrameIndex, f.m_nNumFramePresents, f.m_nNumMisPresented, f.m_nNumDroppedFrames,
                    f.m_nReprojectionFlags, f.m_flCompositorRenderGpuMs, f.m_flClientFrameIntervalMs,
                    f.m_flNewFrameReadyMs - f.m_flNewPosesReadyMs, f.m_nNumVSyncsToFirstView))
    if time.time() >= t_next and win:
        t_next += 1.0
        now_pid = cumulative().m_nPid
        if now_pid != pid:
            pid = now_pid
            say('# %s scene app now pid %d' % (time.strftime('%H:%M:%S'), pid))
        k = len(win)
        reused = sum(1 for w in win if w[1] > 1)
        mis = sum(w[2] for w in win); dropped = sum(w[3] for w in win)
        cpu = sum(1 for w in win if w[4] & 1); gpu = sum(1 for w in win if w[4] & 2)
        asy = sum(1 for w in win if w[4] & 4); mot = sum(1 for w in win if w[4] & 8)
        comp = sum(w[5] for w in win) / k
        ivs = [w[6] for w in win]; app = [w[7] for w in win if w[7] > 0]
        vs = sum(w[8] for w in win) / k
        say('%s %6d %6d %7d %7d %6d %6d %5d %6d  %6.2f   %6.2f/%-7.2f    %5.2f/%-6.2f  %5.2f' % (
            time.strftime('%H:%M:%S'), k, reused, mis, dropped, cpu, gpu, asy, mot, comp,
            sum(ivs) / k, max(ivs), (sum(app) / len(app)) if app else 0, max(app) if app else 0, vs))
        totals['frames'] += k; totals['reused'] += reused; totals['mis'] += mis
        totals['dropped'] += dropped; totals['cpu'] += cpu; totals['gpu'] += gpu
        win = []
    if len(seen) > 100000: seen = set(list(seen)[-1000:])
c1 = cumulative()
say('# window totals: %s' % totals)
say('# SteamVR cumulative for pid %d over the run: presents %d, dropped %d, reprojected %d, app submits %d, '
    'frames with depth %d' % (
    c1.m_nPid, c1.m_nNumFramePresents - (c0.m_nNumFramePresents if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumDroppedFrames - (c0.m_nNumDroppedFrames if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumReprojectedFrames - (c0.m_nNumReprojectedFrames if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumFrameSubmits - (c0.m_nNumFrameSubmits if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumFramesWithDepth - (c0.m_nNumFramesWithDepth if c0.m_nPid == c1.m_nPid else 0)))
lib.VR_ShutdownInternal()
