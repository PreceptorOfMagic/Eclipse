target_sources(moonlight-lib PRIVATE session_video.c coop_tunables.c coop_splice.c coop_poc.c coop_ipc.c coop_secondary.c coop_coord.c coop_conceal.c coop_seq.c coop_session.c coop_stream_mode.c coop_trace.c)

if (WIN32)
    # coop_coord.c's process handling (the /proc orphan reaper, spawn/reap/status) is
    # #ifndef _WIN32 -- the Windows bodies live here, using a JOB OBJECT with
    # KILL_ON_JOB_CLOSE instead of PR_SET_PDEATHSIG, so the kernel kills the secondary
    # however the primary dies. Taken unchanged from the Qt client's co-op port.
    target_sources(moonlight-lib PRIVATE coop_proc_win.c)
endif ()
