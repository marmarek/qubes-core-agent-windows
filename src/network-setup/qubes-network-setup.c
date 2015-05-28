#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <xencontrol.h>

#include "log.h"

// from service.c
DWORD ServiceStartup(void);

// FIXME: new pvdrivers
#define XEN_ADAPTER_DESCRIPTION "Xen Net Device Driver"

DWORD SetNetworkParameters(IN DWORD ip, IN DWORD netmask, IN DWORD gateway, OUT DWORD *interfaceIndex)
{
    IP_ADAPTER_INFO *adapterInfo = NULL;
    IP_ADAPTER_INFO *adapterInfoCurrent;
    IP_ADDR_STRING *addrCurrent;
    MIB_IPINTERFACE_ROW ipInterfaceRow = { 0 };
    ULONG cbAdaptersInfo;
    DWORD nteContext, nteInstance;
    MIB_IPFORWARDTABLE *ipForwardTable = NULL;
    MIB_IPFORWARDROW ipForwardRow = { 0 }; // clear this early, to not override dwForwardIfIndex
    DWORD size = 0;
    DWORD status = 0;
    DWORD i;

    cbAdaptersInfo = 0;
    /* wait for adapters to initialize */
    while ((status = GetAdaptersInfo(NULL, &cbAdaptersInfo)) == ERROR_NO_DATA)
    {
        LogWarning("GetAdaptersInfo call failed with 0x%x, retrying\n", status);
        Sleep(200);
    }

    if (status != ERROR_BUFFER_OVERFLOW)
    {
        perror2(status, "GetAdaptersInfo");
        goto cleanup;
    }
    adapterInfo = (IP_ADAPTER_INFO *) malloc(cbAdaptersInfo);

    if ((status = GetAdaptersInfo(adapterInfo, &cbAdaptersInfo)) != ERROR_SUCCESS)
    {
        perror2(status, "GetAdaptersInfo 2");
        goto cleanup;
    }

    /* set IP address */
    adapterInfoCurrent = adapterInfo;
    while (adapterInfoCurrent)
    {
        if (adapterInfoCurrent->Type == MIB_IF_TYPE_ETHERNET)
        {
            LogInfo("Adapter %d: %S %S", adapterInfoCurrent->Index, adapterInfoCurrent->AdapterName, adapterInfoCurrent->Description);
            
            if (0 == strcmp(adapterInfoCurrent->Description, XEN_ADAPTER_DESCRIPTION))
            {
                LogDebug("setting interface info");
                addrCurrent = &adapterInfoCurrent->IpAddressList;
                while (addrCurrent)
                {
                    if (0 == strcmp("0.0.0.0", addrCurrent->IpAddress.String))
                    {
                        addrCurrent = addrCurrent->Next;
                        continue;
                    }

                    LogInfo("Deleting IP %S", addrCurrent->IpAddress.String);
                    status = DeleteIPAddress(addrCurrent->Context);
                    if (status != ERROR_SUCCESS)
                    {
                        perror2(status, "DeleteIPAddress");
                        goto cleanup;
                    }
                    addrCurrent = addrCurrent->Next;
                }

                LogInfo("Adding IP 0x%x (0x%x)", ip, netmask);
                status = AddIPAddress(ip, netmask, adapterInfoCurrent->Index, &nteContext, &nteInstance);
                if (status != ERROR_SUCCESS)
                {
                    perror2(status, "AddIPAddress");
                    goto cleanup;
                }

                ipForwardRow.dwForwardIfIndex = adapterInfoCurrent->Index;
                if (interfaceIndex)
                    *interfaceIndex = ipForwardRow.dwForwardIfIndex;
            }
        }
        adapterInfoCurrent = adapterInfoCurrent->Next;
    }

    /* set default gateway */
    status = GetIpForwardTable(NULL, &size, FALSE);
    if (status == ERROR_INSUFFICIENT_BUFFER)
    {
        // Allocate the memory for the table
        ipForwardTable = (MIB_IPFORWARDTABLE *) malloc(size);
        if (ipForwardTable == NULL)
        {
            status = ERROR_NOT_ENOUGH_MEMORY;
            LogError("Unable to allocate memory for the IPFORWARDTALE\n");
            goto cleanup;
        }
        // Now get the table.
        status = GetIpForwardTable(ipForwardTable, &size, FALSE);
    }

    if (status != ERROR_SUCCESS)
    {
        perror2(status, "GetIpForwardTable");
        goto cleanup;
    }

    // Search for the row in the table we want. The default gateway has a destination
    // of 0.0.0.0. Notice that we continue looking through the table, but copy only
    // one row. This is so that if there happen to be multiple default gateways, we can
    // be sure to delete them all.
    for (i = 0; i < ipForwardTable->dwNumEntries; i++)
    {
        if (ipForwardTable->table[i].dwForwardDest == 0)
        {
            // Delete the old default gateway entry.
            status = DeleteIpForwardEntry(&(ipForwardTable->table[i]));

            if (status != ERROR_SUCCESS)
            {
                perror2(status, "DeleteIpForwardEntry");
                goto cleanup;
            }
        }
    }

    /* dwForwardIfIndex filled earlier */
    ipInterfaceRow.Family = AF_INET;
    ipInterfaceRow.InterfaceIndex = ipForwardRow.dwForwardIfIndex;
    status = GetIpInterfaceEntry(&ipInterfaceRow);
    if (status != NO_ERROR)
    {
        perror2(status, "GetIpInterfaceEntry");
        goto cleanup;
    }

    ipForwardRow.dwForwardDest = 0; // default gateway (0.0.0.0)
    ipForwardRow.dwForwardMask = 0;
    ipForwardRow.dwForwardNextHop = gateway;
    ipForwardRow.dwForwardProto = MIB_IPPROTO_NETMGMT;
    ipForwardRow.dwForwardMetric1 = ipInterfaceRow.Metric;

    // Create a new route entry for the default gateway.
    status = CreateIpForwardEntry(&ipForwardRow);

    if (status != NO_ERROR)
        perror2(status, "CreateIpForwardEntry");

    status = ERROR_SUCCESS;

cleanup:
    if (ipForwardTable)
        free(ipForwardTable);
    if (adapterInfo)
        free(adapterInfo);

    return status;
}

DWORD SetupNetwork(void)
{
    HANDLE xif = NULL;
    int interfaceIndex;
    char qubesIp[64];
    char qubesNetmask[64];
    char qubesGateway[64];
    DWORD status = ERROR_UNIDENTIFIED_ERROR;
    char cmdline[255];

    status = XenifaceOpen(&xif);
    if (status != ERROR_SUCCESS)
    {
        perror("XenifaceOpen");
        goto cleanup;
    }

    status = StoreRead(xif, "qubes-ip", sizeof(qubesIp), qubesIp);
    if (status != ERROR_SUCCESS)
    {
        LogError("Failed to get qubes-ip\n");
        goto cleanup;
    }

    status = StoreRead(xif, "qubes-netmask", sizeof(qubesNetmask), qubesNetmask);
    if (status != ERROR_SUCCESS)
    {
        LogError("Failed to get qubes-netmask\n");
        goto cleanup;
    }

    status = StoreRead(xif, "qubes-gateway", sizeof(qubesGateway), qubesGateway);
    if (status != ERROR_SUCCESS)
    {
        LogError("Failed to get qubes-gateway\n");
        goto cleanup;
    }

    LogInfo("ip: %S, netmask: %S, gateway: %S", qubesIp, qubesNetmask, qubesGateway);

    if (SetNetworkParameters(
        inet_addr(qubesIp),
        inet_addr(qubesNetmask),
        inet_addr(qubesGateway),
        &interfaceIndex) != ERROR_SUCCESS)
    {
        /* error already reported */
        goto cleanup;
    }

    /* don't know how to programatically (and easily) set DNS address, so stay
     * with netsh... */
    _snprintf(cmdline, RTL_NUMBER_OF(cmdline), "netsh interface ipv4 set dnsservers \"%d\" static %s register=none validate=no",
        interfaceIndex, qubesGateway);

    if (system(cmdline) != 0)
    {
        LogError("Failed to set DNS address by calling: %S\n", cmdline);
        goto cleanup;
    }

    status = 0;

cleanup:
    if (qubesIp)
        free(qubesIp);
    if (qubesNetmask)
        free(qubesNetmask);
    if (qubesGateway)
        free(qubesGateway);

    if (xif)
        XenifaceClose(xif);

    return status;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    if (argc >= 2 && 0 == wcscmp(argv[1], L"-service"))
    {
        return ServiceStartup();
    }
    else
    {
        return SetupNetwork();
    }
}