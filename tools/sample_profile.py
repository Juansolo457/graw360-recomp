#!/usr/bin/env python3
"""A small sampling profiler for the recomp on Windows: which functions a thread is executing.

Attaches to a running grawyeti.exe (or another program), picks its busiest threads (or the given
thread ids), pauses each about every millisecond to read its instruction pointer and a few
return addresses from the stack, and names them afterwards with llvm-symbolizer (from .tools) and
each module's PDB. The recompiled game functions are named sub_XXXXXXXX after their guest address.

    python tools/sample_profile.py --process grawyeti.exe --start 60 --seconds 20 --out prof.txt

--start waits until the process has run that long; --threads N picks the N busiest threads over
the 3 s before sampling. Read-only: threads are only suspended for the instant of each sample.
"""

import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import os
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
for f in ('SuspendThread', 'ResumeThread'):
    getattr(k32, f).argtypes = [wt.HANDLE]
    getattr(k32, f).restype = wt.DWORD
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.c_void_p] * 4
k32.GetProcessTimes.argtypes = [wt.HANDLE] + [ctypes.c_void_p] * 4
k32.CloseHandle.argtypes = [wt.HANDLE]

PROCESS_ALL = 0x1F0FFF
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0010 | 0x0040  # suspend/resume, get context, query
TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
CONTEXT_AMD64 = 0x100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD),
                ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', ctypes.c_long),
                ('tpDeltaPri', ctypes.c_long), ('dwFlags', wt.DWORD)]


class M128A(ctypes.Structure):
    _fields_ = [('Low', ctypes.c_uint64), ('High', ctypes.c_int64)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [('P1Home', ctypes.c_uint64), ('P2Home', ctypes.c_uint64),
                ('P3Home', ctypes.c_uint64), ('P4Home', ctypes.c_uint64),
                ('P5Home', ctypes.c_uint64), ('P6Home', ctypes.c_uint64),
                ('ContextFlags', wt.DWORD), ('MxCsr', wt.DWORD),
                ('SegCs', wt.WORD), ('SegDs', wt.WORD), ('SegEs', wt.WORD), ('SegFs', wt.WORD),
                ('SegGs', wt.WORD), ('SegSs', wt.WORD), ('EFlags', wt.DWORD),
                ('Dr0', ctypes.c_uint64), ('Dr1', ctypes.c_uint64), ('Dr2', ctypes.c_uint64),
                ('Dr3', ctypes.c_uint64), ('Dr6', ctypes.c_uint64), ('Dr7', ctypes.c_uint64),
                ('Rax', ctypes.c_uint64), ('Rcx', ctypes.c_uint64), ('Rdx', ctypes.c_uint64),
                ('Rbx', ctypes.c_uint64), ('Rsp', ctypes.c_uint64), ('Rbp', ctypes.c_uint64),
                ('Rsi', ctypes.c_uint64), ('Rdi', ctypes.c_uint64), ('R8', ctypes.c_uint64),
                ('R9', ctypes.c_uint64), ('R10', ctypes.c_uint64), ('R11', ctypes.c_uint64),
                ('R12', ctypes.c_uint64), ('R13', ctypes.c_uint64), ('R14', ctypes.c_uint64),
                ('R15', ctypes.c_uint64), ('Rip', ctypes.c_uint64),
                ('FltSave', ctypes.c_byte * 512), ('VectorRegister', M128A * 26),
                ('VectorControl', ctypes.c_uint64), ('DebugControl', ctypes.c_uint64),
                ('LastBranchToRip', ctypes.c_uint64), ('LastBranchFromRip', ctypes.c_uint64),
                ('LastExceptionToRip', ctypes.c_uint64), ('LastExceptionFromRip', ctypes.c_uint64)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [('lpBaseOfDll', ctypes.c_void_p), ('SizeOfImage', wt.DWORD), ('EntryPoint', ctypes.c_void_p)]


def modules_of(proc):
    arr = (wt.HMODULE * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
    psapi.EnumProcessModulesEx(proc, arr, ctypes.sizeof(arr), ctypes.byref(needed), 0x3)
    psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
    psapi.GetModuleInformation.argtypes = [wt.HANDLE, wt.HMODULE, ctypes.c_void_p, wt.DWORD]
    out = []
    for i in range(needed.value // ctypes.sizeof(wt.HMODULE)):
        path = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(proc, arr[i], path, 1024)
        info = MODULEINFO()
        psapi.GetModuleInformation(proc, arr[i], ctypes.byref(info), ctypes.sizeof(info))
        out.append((info.lpBaseOfDll or 0, info.SizeOfImage, path.value))
    return out


def symbolize(addresses, modules, symbolizer):
    """Names for addresses: module!function, using llvm-symbolizer on each module's file."""
    import subprocess
    names = {}
    by_module = collections.defaultdict(list)
    for a in addresses:
        for base, size, path in modules:
            if base <= a < base + size:
                by_module[(base, path)].append(a)
                break
        else:
            names[a] = f'?{a:x}'
    for (base, path), addrs in by_module.items():
        short = os.path.basename(path)
        try:
            inp = ''.join(f'0x{a - base:x}\n' for a in addrs)
            r = subprocess.run([symbolizer, f'--obj={path}', '--relative-address', '--no-inlines',
                                '--functions=short', '--output-style=GNU'],
                               input=inp, capture_output=True, text=True, timeout=600)
            outs = [l for l in r.stdout.splitlines()]
            funcs = outs[0::2]
            for a, f in zip(addrs, funcs):
                names[a] = f'{short}!{f}' if f and f != '??' else f'{short}+{a - base:x}'
        except Exception:
            for a in addrs:
                names[a] = f'{short}+{a - base:x}'
    return names


def find_process(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W(dwSize=ctypes.sizeof(PROCESSENTRY32W))
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.lower() == name.lower():
            pid = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32(dwSize=ctypes.sizeof(THREADENTRY32))
    ok = k32.Thread32First(snap, ctypes.byref(e))
    out = []
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def thread_cpu(handle):
    c, x, k, u = (wt.FILETIME() for _ in range(4))
    if not k32.GetThreadTimes(handle, ctypes.byref(c), ctypes.byref(x), ctypes.byref(k), ctypes.byref(u)):
        return 0
    t = lambda f: (f.dwHighDateTime << 32) | f.dwLowDateTime
    return t(k) + t(u)


def process_age(handle):
    c, x, k, u = (wt.FILETIME() for _ in range(4))
    k32.GetProcessTimes(handle, ctypes.byref(c), ctypes.byref(x), ctypes.byref(k), ctypes.byref(u))
    created = (c.dwHighDateTime << 32) | c.dwLowDateTime
    now = wt.FILETIME()
    k32.GetSystemTimeAsFileTime(ctypes.byref(now))
    return (((now.dwHighDateTime << 32) | now.dwLowDateTime) - created) / 1e7


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--process', default='grawyeti.exe')
    ap.add_argument('--start', type=float, default=0, help='seconds of process age to wait for')
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--threads', type=int, default=3)
    ap.add_argument('--tids', default='', help='comma-separated thread ids instead of --threads')
    ap.add_argument('--depth', type=int, default=6, help='stack words scanned for return addresses')
    ap.add_argument('--out', default='')
    args = ap.parse_args()

    pid = None
    while pid is None:
        pid = find_process(args.process)
        if pid is None:
            time.sleep(0.2)
    proc = k32.OpenProcess(PROCESS_ALL, False, pid)
    while process_age(proc) < args.start - 3:
        time.sleep(0.2)

    if args.tids:
        tids = [int(t) for t in args.tids.split(',')]
    else:  # the busiest threads over 3 s
        handles = {t: k32.OpenThread(THREAD_ACCESS, False, t) for t in threads_of(pid)}
        before = {t: thread_cpu(h) for t, h in handles.items()}
        time.sleep(3)
        busy = sorted(handles, key=lambda t: thread_cpu(handles[t]) - before[t], reverse=True)
        tids = busy[:args.threads]
        for h in handles.values():
            k32.CloseHandle(h)
    threads = {t: k32.OpenThread(THREAD_ACCESS, False, t) for t in tids}

    modules = modules_of(proc)

    ctx = CONTEXT()
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER
    leaf = {t: collections.Counter() for t in tids}
    callers = {t: collections.Counter() for t in tids}
    samples = collections.Counter()
    end = time.perf_counter() + args.seconds
    buf = (ctypes.c_uint64 * args.depth)()
    read = ctypes.c_size_t()
    while time.perf_counter() < end:
        for t, h in threads.items():
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            ok = k32.GetThreadContext(h, ctypes.byref(ctx))
            rip, rsp = ctx.Rip, ctx.Rsp
            k32.ReadProcessMemory(proc, ctypes.c_void_p(rsp), buf, ctypes.sizeof(buf), ctypes.byref(read))
            k32.ResumeThread(h)
            if not ok:
                continue
            samples[t] += 1
            leaf[t][rip] += 1
            for i in range(args.depth):
                callers[t][buf[i]] += 1
        time.sleep(0.0008)

    every = set()
    for t in tids:
        every |= set(leaf[t]) | set(callers[t])
    symbolizer = os.path.join(os.path.dirname(__file__), '..', '.tools', 'llvm', 'bin', 'llvm-symbolizer.exe')
    resolved = symbolize(every, modules, symbolizer)

    def name(addr):
        n = resolved.get(addr)
        return None if n is None or n.startswith('?') else n

    lines = []
    for t in tids:
        n = samples[t]
        lines.append(f'== thread {t}: {n} samples over {args.seconds:.0f} s')
        by_func = collections.Counter()
        for addr, c in leaf[t].items():
            by_func[name(addr) or f'?{addr:x}'] += c
        for f, c in by_func.most_common(25):
            lines.append(f'  {100 * c / max(1, n):5.1f}%  {f}')
        by_caller = collections.Counter()
        for addr, c in callers[t].items():
            f = name(addr)
            if f and '+' not in f:  # a stack word naming a function: a likely caller
                by_caller[f] += c
        lines.append('  on the stack (likely callers, counted per sample word):')
        for f, c in by_caller.most_common(15):
            lines.append(f'  {100 * c / max(1, n):5.1f}%  {f}')
    text = '\n'.join(lines)
    print(text)
    if args.out:
        open(args.out, 'w', encoding='utf-8').write(text + '\n')


if __name__ == '__main__':
    main()
