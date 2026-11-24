#ifndef XBOX_PROBE_PIPE_PROTOCOL_H
#define XBOX_PROBE_PIPE_PROTOCOL_H

#define PROBE_PIPE_PREFIX L"\\\\.\\pipe\\LOCAL\\XboxPortabilityProbe-"
#define PROBE_PIPE_SUFFIX_LENGTH 24
#define PROBE_PIPE_HELLO 0x6D0151A7u
#define PROBE_PIPE_REPLY 0x7F291BB5u
#define PROBE_PIPE_EXIT 0x51A8u

#define PROBE_LIFETIME_EXIT(challenge, token) (0x51000000u | (((challenge) ^ (token)) & 0x00FFFFFFu))

typedef struct ProbeLifetimeHello {
    DWORD pid;
    DWORD token;
} ProbeLifetimeHello;

#endif
