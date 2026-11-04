"""Extend an already running PDS wait without interrupting the PDS process.
Temporarily suspend only its Python timeout monitor; resume it after PDS exits.
"""
import argparse,ctypes,time
from ctypes import wintypes
p=argparse.ArgumentParser(description=__doc__);p.add_argument("--monitor-pid",type=int,required=True);p.add_argument("--pds-pid",type=int,required=True);p.add_argument("--seconds",type=int,default=1200);a=p.parse_args()
k=ctypes.WinDLL("kernel32",use_last_error=True);n=ctypes.WinDLL("ntdll")
k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];k.OpenProcess.restype=wintypes.HANDLE
k.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD];k.WaitForSingleObject.restype=wintypes.DWORD
k.CloseHandle.argtypes=[wintypes.HANDLE]
n.NtSuspendProcess.argtypes=[wintypes.HANDLE];n.NtSuspendProcess.restype=wintypes.LONG
n.NtResumeProcess.argtypes=[wintypes.HANDLE];n.NtResumeProcess.restype=wintypes.LONG
m=k.OpenProcess(0x0800,False,a.monitor_pid);d=k.OpenProcess(0x100000,False,a.pds_pid)
if not m or not d:raise ctypes.WinError(ctypes.get_last_error())
status=n.NtSuspendProcess(m)
if status:raise OSError("NtSuspendProcess failed",status)
print("PDS_TIMEOUT_MONITOR_SUSPENDED",a.monitor_pid,"PDS_CONTINUES",a.pds_pid,flush=True)
try:
    result=k.WaitForSingleObject(d,a.seconds*1000)
    print("PDS_FINISHED" if result==0 else "EXTENDED_WAIT_EXPIRED",a.pds_pid,flush=True)
finally:
    n.NtResumeProcess(m);k.CloseHandle(d);k.CloseHandle(m)
    print("PDS_TIMEOUT_MONITOR_RESUMED",a.monitor_pid,flush=True)
