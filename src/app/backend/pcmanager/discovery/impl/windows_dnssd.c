/* Windows 10+ DNS-SD backend.
 *
 * The previous Windows build used libmicrodns. It creates one wildcard socket
 * per eligible adapter; on Windows, Hyper-V/WSL/VPN adapters can then compete
 * with the physical NIC for multicast packets. The symptom was particularly
 * misleading: local Apollo instances appeared, while every other LAN host was
 * silently absent. dnsapi.dll already owns adapter selection and returns each
 * service with its SRV and address records, so use the platform facility here.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#include <stdlib.h>
#include <wchar.h>

#include "impl.h"
#include "logging.h"

#define DISCOVERY_SERVICE_NAME L"_nvstream._tcp.local"
#define DISCOVERY_DEFAULT_PORT 47989
#define DISCOVERY_CANCEL_WAIT_MS 2000

typedef struct windows_discovery_context_t {
    /* DnsServiceBrowse() is asynchronous and may invoke its callback directly from
     * DnsServiceBrowseCancel().  Keep every object the API can still reference in this
     * heap allocation.  The worker and the outstanding browse each own one reference;
     * callbacks take a transient reference while running. */
    volatile LONG refs;
    volatile LONG operation_ref_held;
    SRWLOCK lock;
    HANDLE completed;
    discovery_task_t *task;
    bool terminal;
    DWORD terminal_status;
    DNS_SERVICE_BROWSE_REQUEST request;
    DNS_SERVICE_CANCEL cancel;
} windows_discovery_context_t;

static void context_retain(windows_discovery_context_t *context) {
    InterlockedIncrement(&context->refs);
}

static void context_release(windows_discovery_context_t *context) {
    if (InterlockedDecrement(&context->refs) == 0) {
        if (context->completed != NULL) {
            CloseHandle(context->completed);
        }
        free(context);
    }
}

static void context_release_operation(windows_discovery_context_t *context) {
    if (InterlockedExchange(&context->operation_ref_held, 0) != 0) {
        context_release(context);
    }
}

static bool discovery_is_stopped(discovery_task_t *task) {
    SDL_LockMutex(task->lock);
    const bool stopped = task->stop;
    SDL_UnlockMutex(task->lock);
    return stopped;
}

static size_t dns_name_length(const wchar_t *name) {
    size_t length = name != NULL ? wcslen(name) : 0;
    while (length > 0 && name[length - 1] == L'.') {
        --length;
    }
    return length;
}

static bool dns_names_equal(const wchar_t *a, const wchar_t *b) {
    const size_t a_length = dns_name_length(a);
    const size_t b_length = dns_name_length(b);
    return a_length == b_length && a_length > 0 &&
           _wcsnicmp(a, b, a_length) == 0;
}

static bool dns_name_is_service_instance(const wchar_t *name) {
    const size_t name_length = dns_name_length(name);
    const size_t service_length = wcslen(DISCOVERY_SERVICE_NAME);
    return name_length > service_length &&
           name[name_length - service_length - 1] == L'.' &&
           _wcsnicmp(name + name_length - service_length,
                     DISCOVERY_SERVICE_NAME, service_length) == 0;
}

static void report_service_address(discovery_task_t *task,
                                   const DNS_RECORD *records,
                                   const DNS_RECORD *service) {
    if (service->Data.SRV.pNameTarget == NULL) {
        return;
    }
    for (const DNS_RECORD *record = records; record != NULL; record = record->pNext) {
        if (record->wType != DNS_TYPE_A ||
            !dns_names_equal(record->pName, service->Data.SRV.pNameTarget)) {
            continue;
        }
        sockaddr_t *address = sockaddr_new();
        if (address == NULL) {
            return;
        }
        if (sockaddr_set_ip(address, AF_INET, &record->Data.A.IpAddress) == 0) {
            const uint16_t port = service->Data.SRV.wPort != 0
                                  ? service->Data.SRV.wPort
                                  : DISCOVERY_DEFAULT_PORT;
            sockaddr_set_port(address, port);
            discovery_discovered(task->discovery, address);
        }
        sockaddr_free(address);
    }
}

static void WINAPI discovery_callback(DWORD status, void *user_data,
                                      DNS_RECORD *records) {
    windows_discovery_context_t *context = user_data;
    context_retain(context);

    bool first_terminal = false;
    AcquireSRWLockExclusive(&context->lock);
    if (status != ERROR_SUCCESS && !context->terminal) {
        /* ERROR_CANCELLED is the documented completion after a successful cancel.
         * Other failures are terminal too: remembering all of them prevents shutdown
         * from waiting forever after the browse has already ended. */
        context->terminal = true;
        context->terminal_status = status;
        first_terminal = true;
    }

    discovery_task_t *task = context->task;
    if (status == ERROR_SUCCESS && task != NULL && !discovery_is_stopped(task)) {
        for (const DNS_RECORD *record = records; record != NULL; record = record->pNext) {
            if (record->wType == DNS_TYPE_SRV &&
                dns_name_is_service_instance(record->pName)) {
                report_service_address(task, records, record);
            }
        }
    }
    ReleaseSRWLockExclusive(&context->lock);

    if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) {
        commons_log_warn("Discovery", "Windows DNS-SD callback failed: %lu",
                         (unsigned long) status);
    }

    if (records != NULL) {
        DnsRecordListFree(records, DnsFreeRecordList);
    }

    if (first_terminal) {
        /* Publish completion only after this callback has finished using the task and
         * returned its record list.  The browse's lifetime reference is released once,
         * whether completion raced the start/cancel paths or not. */
        SetEvent(context->completed);
        context_release_operation(context);
    }
    context_release(context);
}

int discovery_worker(discovery_task_t *task) {
    windows_discovery_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        commons_log_error("Discovery", "Could not allocate Windows DNS-SD context");
        return -1;
    }
    context->refs = 2; /* worker + outstanding browse */
    context->operation_ref_held = 1;
    InitializeSRWLock(&context->lock);
    context->completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    context->task = task;
    if (context->completed == NULL) {
        commons_log_error("Discovery", "Could not create DNS-SD completion event: %lu",
                          (unsigned long) GetLastError());
        /* No browse was submitted, so both provisional references are ours. */
        context_release_operation(context);
        context_release(context);
        return -1;
    }

    context->request.Version = DNS_QUERY_REQUEST_VERSION1;
    context->request.InterfaceIndex = 0; /* The OS evaluates every active interface. */
    context->request.QueryName = DISCOVERY_SERVICE_NAME;
    context->request.pBrowseCallback = discovery_callback;
    context->request.pQueryContext = context;

    const DNS_STATUS start_status = DnsServiceBrowse(&context->request, &context->cancel);
    if (start_status != DNS_REQUEST_PENDING) {
        commons_log_error("Discovery", "Windows DNS-SD browse failed to start: %ld",
                          (long) start_status);
        AcquireSRWLockExclusive(&context->lock);
        context->task = NULL;
        ReleaseSRWLockExclusive(&context->lock);
        context_release_operation(context);
        context_release(context);
        return -1;
    }

    commons_log_info("Discovery", "Start Windows DNS-SD discovery");
    while (!discovery_is_stopped(task)) {
        SDL_Delay(50);
    }

    AcquireSRWLockExclusive(&context->lock);
    const bool already_terminal = context->terminal;
    ReleaseSRWLockExclusive(&context->lock);

    DNS_STATUS cancel_status = ERROR_SUCCESS;
    if (!already_terminal) {
        cancel_status = DnsServiceBrowseCancel(&context->cancel);
        if (cancel_status != ERROR_SUCCESS) {
            commons_log_error("Discovery", "Windows DNS-SD cancellation failed: %ld",
                              (long) cancel_status);
        }
    }

    /* A successful cancel promises one final ERROR_CANCELLED callback.  A failed
     * cancel does not promise anything, so an infinite wait here can wedge app exit.
     * The callback context and cancel handle are heap-owned: on timeout we detach the
     * task and drop only the worker reference.  If dnsapi calls back later it remains
     * safe and frees itself on terminal completion; if dnsapi never completes, one
     * small context is deliberately retained instead of manufacturing a UAF. */
    const DWORD wait_status = WaitForSingleObject(context->completed,
                                                   DISCOVERY_CANCEL_WAIT_MS);
    DWORD terminal_status = ERROR_SUCCESS;
    AcquireSRWLockExclusive(&context->lock);
    context->task = NULL;
    if (context->terminal) {
        terminal_status = context->terminal_status;
    }
    ReleaseSRWLockExclusive(&context->lock);

    if (wait_status == WAIT_TIMEOUT) {
        commons_log_error("Discovery",
                          "Windows DNS-SD completion timed out after %u ms; "
                          "callback state retained safely",
                          (unsigned int) DISCOVERY_CANCEL_WAIT_MS);
    } else if (wait_status == WAIT_FAILED) {
        commons_log_error("Discovery", "Waiting for Windows DNS-SD completion failed: %lu",
                          (unsigned long) GetLastError());
    }

    const bool completed = wait_status == WAIT_OBJECT_0;
    const bool cancelled = completed && terminal_status == ERROR_CANCELLED;
    context_release(context); /* worker reference */
    commons_log_info("Discovery", "Windows DNS-SD discovery stopped");
    return cancel_status == ERROR_SUCCESS && cancelled ? 0 : -1;
}

void discovery_worker_stop(discovery_task_t *task) {
    SDL_LockMutex(task->lock);
    task->stop = true;
    SDL_UnlockMutex(task->lock);
}
