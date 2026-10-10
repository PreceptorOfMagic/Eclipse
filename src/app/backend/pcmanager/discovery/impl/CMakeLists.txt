if (OS_WINDOWS)
    # Windows 10 and later provide DNS-SD in dnsapi.dll. Using the OS browser
    # avoids libmicrodns' competing wildcard sockets on machines with Hyper-V,
    # WSL, Docker, or VPN adapters, which can silently hide the physical LAN.
    target_sources(moonlight-lib PRIVATE windows_dnssd.c)
    target_link_libraries(moonlight-lib PUBLIC dnsapi)
else ()
    # Keep Linux/webOS discovery on the same audited implementation regardless
    # of which optional Bonjour compatibility headers happen to be installed on
    # the build machine. The old dnssd.c predates the generic discovery worker
    # API, and selecting it opportunistically produced duplicate pcmanager
    # entry points plus missing discovery_worker symbols. Vendoring microdns is
    # both reproducible for a fresh checkout and the smallest future Linux seam.
    target_sources(moonlight-lib PRIVATE microdns.c)
    set(BUILD_SHARED_LIBS ON)
    include(BuildMicrodns)
    target_link_libraries(moonlight-lib PUBLIC microdns)
    unset(BUILD_SHARED_LIBS)
endif ()
