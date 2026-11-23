# one-off edit: includes sync32.inc in peload32.cpp and lists its stand-ins in kKnown
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")
if '#include "sync32.inc"' not in s:
    s = s.replace("static uint64_t B_GetModuleHandleA2(uint64_t name);",
                  '#include "sync32.inc"\n\nstatic uint64_t B_GetModuleHandleA2(uint64_t name);', 1)
rows = [
    ("InitializeCriticalSection", 1), ("InitializeCriticalSectionAndSpinCount", 2), ("InitializeCriticalSectionEx", 3),
    ("EnterCriticalSection", 1), ("LeaveCriticalSection", 1), ("TryEnterCriticalSection", 1), ("DeleteCriticalSection", 1),
    ("InitializeSRWLock", 1), ("AcquireSRWLockExclusive", 1), ("AcquireSRWLockShared", 1), ("ReleaseSRWLockExclusive", 1),
    ("ReleaseSRWLockShared", 1), ("TryAcquireSRWLockExclusive", 1), ("TryAcquireSRWLockShared", 1),
    ("InitializeConditionVariable", 1), ("SleepConditionVariableCS", 3), ("SleepConditionVariableSRW", 4),
    ("WakeConditionVariable", 1), ("WakeAllConditionVariable", 1), ("InitializeSListHead", 1),
    ("InterlockedPushEntrySList", 2), ("InterlockedPopEntrySList", 1), ("InterlockedFlushSList", 1),
    ("QueryDepthSList", 1), ("FlsAlloc", 1),
]
add = "".join('    {"kernel32.dll", "%s", %d, false, (void*)B_%s},\n' % (n, a, n) for n, a in rows)
add += '    {"kernel32.dll", "GetStartupInfoW", 1, false, (void*)B_GetStartupInfo},\n'
add += '    {"kernel32.dll", "GetStartupInfoA", 1, false, (void*)B_GetStartupInfo},\n'
anchor = '    {"kernel32.dll", "GetProcAddress", 2, false, (void*)B_GetProcAddress},\n'
if '"InitializeSRWLock"' not in s:
    s = s.replace(anchor, anchor + add, 1)
p.write_text(s, encoding="utf-8")
print("ok")
