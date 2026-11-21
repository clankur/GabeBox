#ifndef PROBE_CHILD_PROCESS_H
#define PROBE_CHILD_PROCESS_H

#define PROBE_CHILD_MODULE L"child-process.dll"
#define PROBE_CHILD_EXPORT "RunPackagedChild"
#define PROBE_EXPECTED_CHILD_EXIT 0x51A7u

typedef struct ChildProcessResult {
    int created;
    unsigned long error;
    unsigned long waitResult;
    unsigned long exitCode;
} ChildProcessResult;

typedef void (__stdcall *RunPackagedChildFn)(const unsigned short *, ChildProcessResult *);

#endif
