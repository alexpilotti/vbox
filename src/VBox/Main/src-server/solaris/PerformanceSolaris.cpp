/* $Id$ */

/** @file
 *
 * VBox Solaris-specific Performance Classes implementation.
 */

/*
 * Copyright (C) 2008 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#undef _FILE_OFFSET_BITS
#include <procfs.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <kstat.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/statvfs.h>

#include <iprt/ctype.h>
#include <iprt/err.h>
#include <iprt/string.h>
#include <iprt/alloc.h>
#include <iprt/param.h>
#include <iprt/path.h>
#include <VBox/log.h>
#include "Performance.h"

#include <dlfcn.h>

#include <libzfs.h>
#include <libnvpair.h>

#include <map>

namespace pm {

    typedef libzfs_handle_t *(*PFNZFSINIT)(void);
    typedef zfs_handle_t *(*PFNZFSOPEN)(libzfs_handle_t *, const char *, int);
    typedef void (*PFNZFSCLOSE)(zfs_handle_t *);
    typedef uint64_t (*PFNZFSPROPGETINT)(zfs_handle_t *, zfs_prop_t);
    typedef zpool_handle_t *(*PFNZPOOLOPEN)(libzfs_handle_t *, const char *);
    typedef void (*PFNZPOOLCLOSE)(zpool_handle_t *);
    typedef nvlist_t *(*PFNZPOOLGETCONFIG)(zpool_handle_t *, nvlist_t **);
    typedef char *(*PFNZPOOLVDEVNAME)(libzfs_handle_t *, zpool_handle_t *, nvlist_t *, boolean_t);

    typedef std::map<RTCString,RTCString> FsMap;

class CollectorSolaris : public CollectorHAL
{
public:
    CollectorSolaris();
    virtual ~CollectorSolaris();
    virtual int getHostMemoryUsage(ULONG *total, ULONG *used, ULONG *available);
    virtual int getHostFilesystemUsage(const char *name, ULONG *total, ULONG *used, ULONG *available);
    virtual int getHostDiskSize(const char *name, uint64_t *size);
    virtual int getProcessMemoryUsage(RTPROCESS process, ULONG *used);

    virtual int getRawHostCpuLoad(uint64_t *user, uint64_t *kernel, uint64_t *idle);
    virtual int getRawHostNetworkLoad(const char *name, uint64_t *rx, uint64_t *tx);
    virtual int getRawHostDiskLoad(const char *name, uint64_t *disk_ms, uint64_t *total_ms);
    virtual int getRawProcessCpuLoad(RTPROCESS process, uint64_t *user, uint64_t *kernel, uint64_t *total);

    virtual int getDiskListByFs(const char *name, DiskList& list);
private:
    static uint32_t getInstance(const char *pszIfaceName, char *pszDevName);
    uint64_t getZfsTotal(uint64_t cbTotal, const char *szFsType, const char *szFsName);
    void updateFilesystemMap(void);
    RTCString physToInstName(const char *pcszPhysName);
    RTCString pathToInstName(const char *pcszDevPathName);

    kstat_ctl_t *mKC;
    kstat_t     *mSysPages;
    kstat_t     *mZFSCache;

    void             *mZfsSo;
    libzfs_handle_t  *mZfsLib;
    PFNZFSINIT        mZfsInit;
    PFNZFSOPEN        mZfsOpen;
    PFNZFSCLOSE       mZfsClose;
    PFNZFSPROPGETINT  mZfsPropGetInt;
    PFNZPOOLOPEN      mZpoolOpen;
    PFNZPOOLCLOSE     mZpoolClose;
    PFNZPOOLGETCONFIG mZpoolGetConfig;
    PFNZPOOLVDEVNAME  mZpoolVdevName;

    FsMap             mFsMap;
};

CollectorHAL *createHAL()
{
    return new CollectorSolaris();
}

// Collector HAL for Solaris


CollectorSolaris::CollectorSolaris()
    : mKC(0),
      mSysPages(0),
      mZFSCache(0),
      mZfsLib(0)
{
    if ((mKC = kstat_open()) == 0)
    {
        Log(("kstat_open() -> %d\n", errno));
        return;
    }

    if ((mSysPages = kstat_lookup(mKC, (char *)"unix", 0, (char *)"system_pages")) == 0)
    {
        Log(("kstat_lookup(system_pages) -> %d\n", errno));
        return;
    }

    if ((mZFSCache = kstat_lookup(mKC, (char *)"zfs", 0, (char *)"arcstats")) == 0)
    {
        Log(("kstat_lookup(system_pages) -> %d\n", errno));
    }

    /* Try to load libzfs dynamically, it may be missing. */
    mZfsSo = dlopen("libzfs.so", RTLD_LAZY);
    if (mZfsSo)
    {
        mZfsInit        =        (PFNZFSINIT)dlsym(mZfsSo, "libzfs_init");
        mZfsOpen        =        (PFNZFSOPEN)dlsym(mZfsSo, "zfs_open");
        mZfsClose       =       (PFNZFSCLOSE)dlsym(mZfsSo, "zfs_close");
        mZfsPropGetInt  =  (PFNZFSPROPGETINT)dlsym(mZfsSo, "zfs_prop_get_int");
        mZpoolOpen      =      (PFNZPOOLOPEN)dlsym(mZfsSo, "zpool_open");
        mZpoolClose     =     (PFNZPOOLCLOSE)dlsym(mZfsSo, "zpool_close");
        mZpoolGetConfig = (PFNZPOOLGETCONFIG)dlsym(mZfsSo, "zpool_get_config");
        mZpoolVdevName  =  (PFNZPOOLVDEVNAME)dlsym(mZfsSo, "zpool_vdev_name");

        if (mZfsInit && mZfsOpen && mZfsClose && mZfsPropGetInt
            && mZpoolOpen && mZpoolClose && mZpoolGetConfig && mZpoolVdevName)
            mZfsLib = mZfsInit();
        else
            LogRel(("Incompatible libzfs? libzfs_init=%p zfs_open=%p zfs_close=%p zfs_prop_get_int=%p\n",
                    mZfsInit, mZfsOpen, mZfsClose, mZfsPropGetInt));
    }

    updateFilesystemMap();
}

CollectorSolaris::~CollectorSolaris()
{
    if (mKC)
        kstat_close(mKC);
    if (mZfsSo)
        dlclose(mZfsSo);
}

int CollectorSolaris::getRawHostCpuLoad(uint64_t *user, uint64_t *kernel, uint64_t *idle)
{
    int rc = VINF_SUCCESS;
    kstat_t *ksp;
    uint64_t tmpUser, tmpKernel, tmpIdle;
    int cpus;
    cpu_stat_t cpu_stats;

    if (mKC == 0)
        return VERR_INTERNAL_ERROR;

    tmpUser = tmpKernel = tmpIdle = cpus = 0;
    for (ksp = mKC->kc_chain; ksp != NULL; ksp = ksp->ks_next) {
        if (strcmp(ksp->ks_module, "cpu_stat") == 0) {
            if (kstat_read(mKC, ksp, &cpu_stats) == -1)
            {
                Log(("kstat_read() -> %d\n", errno));
                return VERR_INTERNAL_ERROR;
            }
            ++cpus;
            tmpUser   += cpu_stats.cpu_sysinfo.cpu[CPU_USER];
            tmpKernel += cpu_stats.cpu_sysinfo.cpu[CPU_KERNEL];
            tmpIdle   += cpu_stats.cpu_sysinfo.cpu[CPU_IDLE];
        }
    }

    if (cpus == 0)
    {
        Log(("no cpu stats found!\n"));
        return VERR_INTERNAL_ERROR;
    }

    if (user)   *user   = tmpUser;
    if (kernel) *kernel = tmpKernel;
    if (idle)   *idle   = tmpIdle;

    return rc;
}

int CollectorSolaris::getRawProcessCpuLoad(RTPROCESS process, uint64_t *user, uint64_t *kernel, uint64_t *total)
{
    int rc = VINF_SUCCESS;
    char *pszName;
    prusage_t prusage;

    RTStrAPrintf(&pszName, "/proc/%d/usage", process);
    Log(("Opening %s...\n", pszName));
    int h = open(pszName, O_RDONLY);
    RTStrFree(pszName);

    if (h != -1)
    {
        if (read(h, &prusage, sizeof(prusage)) == sizeof(prusage))
        {
            //Assert((pid_t)process == pstatus.pr_pid);
            //Log(("user=%u kernel=%u total=%u\n", prusage.pr_utime.tv_sec, prusage.pr_stime.tv_sec, prusage.pr_tstamp.tv_sec));
            *user = (uint64_t)prusage.pr_utime.tv_sec * 1000000000 + prusage.pr_utime.tv_nsec;
            *kernel = (uint64_t)prusage.pr_stime.tv_sec * 1000000000 + prusage.pr_stime.tv_nsec;
            *total = (uint64_t)prusage.pr_tstamp.tv_sec * 1000000000 + prusage.pr_tstamp.tv_nsec;
            //Log(("user=%llu kernel=%llu total=%llu\n", *user, *kernel, *total));
        }
        else
        {
            Log(("read() -> %d\n", errno));
            rc = VERR_FILE_IO_ERROR;
        }
        close(h);
    }
    else
    {
        Log(("open() -> %d\n", errno));
        rc = VERR_ACCESS_DENIED;
    }

    return rc;
}

int CollectorSolaris::getHostMemoryUsage(ULONG *total, ULONG *used, ULONG *available)
{
    int rc = VINF_SUCCESS;

    kstat_named_t *kn;

    if (mKC == 0 || mSysPages == 0)
        return VERR_INTERNAL_ERROR;

    if (kstat_read(mKC, mSysPages, 0) == -1)
    {
        Log(("kstat_read(sys_pages) -> %d\n", errno));
        return VERR_INTERNAL_ERROR;
    }
    if ((kn = (kstat_named_t *)kstat_data_lookup(mSysPages, (char *)"freemem")) == 0)
    {
        Log(("kstat_data_lookup(freemem) -> %d\n", errno));
        return VERR_INTERNAL_ERROR;
    }
    *available = kn->value.ul * (PAGE_SIZE/1024);

    if (kstat_read(mKC, mZFSCache, 0) != -1)
    {
        if (mZFSCache)
        {
            if ((kn = (kstat_named_t *)kstat_data_lookup(mZFSCache, (char *)"size")))
            {
                ulong_t ulSize = kn->value.ul;

                if ((kn = (kstat_named_t *)kstat_data_lookup(mZFSCache, (char *)"c_min")))
                {
                    /*
                     * Account for ZFS minimum arc cache size limit.
                     * "c_min" is the target minimum size of the ZFS cache, and not the hard limit. It's possible
                     * for "size" to shrink below "c_min" (e.g: during boot & high memory consumption).
                     */
                    ulong_t ulMin = kn->value.ul;
                    *available += ulSize > ulMin ? (ulSize - ulMin) / 1024 : 0;
                }
                else
                    Log(("kstat_data_lookup(c_min) ->%d\n", errno));
            }
            else
                Log(("kstat_data_lookup(size) -> %d\n", errno));
        }
        else
            Log(("mZFSCache missing.\n"));
    }

    if ((kn = (kstat_named_t *)kstat_data_lookup(mSysPages, (char *)"physmem")) == 0)
    {
        Log(("kstat_data_lookup(physmem) -> %d\n", errno));
        return VERR_INTERNAL_ERROR;
    }
    *total = kn->value.ul * (PAGE_SIZE/1024);
    *used = *total - *available;

    return rc;
}

int CollectorSolaris::getProcessMemoryUsage(RTPROCESS process, ULONG *used)
{
    int rc = VINF_SUCCESS;
    char *pszName = NULL;
    psinfo_t psinfo;

    RTStrAPrintf(&pszName, "/proc/%d/psinfo", process);
    Log(("Opening %s...\n", pszName));
    int h = open(pszName, O_RDONLY);
    RTStrFree(pszName);

    if (h != -1)
    {
        if (read(h, &psinfo, sizeof(psinfo)) == sizeof(psinfo))
        {
            Assert((pid_t)process == psinfo.pr_pid);
            *used = psinfo.pr_rssize;
        }
        else
        {
            Log(("read() -> %d\n", errno));
            rc = VERR_FILE_IO_ERROR;
        }
        close(h);
    }
    else
    {
        Log(("open() -> %d\n", errno));
        rc = VERR_ACCESS_DENIED;
    }

    return rc;
}

uint32_t CollectorSolaris::getInstance(const char *pszIfaceName, char *pszDevName)
{
    /*
     * Get the instance number from the interface name, then clip it off.
     */
    int cbInstance = 0;
    int cbIface = strlen(pszIfaceName);
    const char *pszEnd = pszIfaceName + cbIface - 1;
    for (int i = 0; i < cbIface - 1; i++)
    {
        if (!RT_C_IS_DIGIT(*pszEnd))
            break;
        cbInstance++;
        pszEnd--;
    }

    uint32_t uInstance = RTStrToUInt32(pszEnd + 1);
    strncpy(pszDevName, pszIfaceName, cbIface - cbInstance);
    pszDevName[cbIface - cbInstance] = '\0';
    return uInstance;
}

int CollectorSolaris::getRawHostNetworkLoad(const char *name, uint64_t *rx, uint64_t *tx)
{
    AssertReturn(strlen(name) < KSTAT_STRLEN, VERR_INVALID_PARAMETER);
    LogFlowThisFunc(("m=%s i=%d n=%s\n", "link", -1, name));
    kstat_t *ksAdapter = kstat_lookup(mKC, "link", -1, (char *)name);
    if (ksAdapter == 0)
    {
        char szModule[KSTAT_STRLEN];
        uint32_t uInstance = getInstance(name, szModule);
        LogFlowThisFunc(("m=%s i=%u n=%s\n", szModule, uInstance, "phys"));
        ksAdapter = kstat_lookup(mKC, szModule, uInstance, "phys");
        if (ksAdapter == 0)
        {
            LogFlowThisFunc(("m=%s i=%u n=%s\n", szModule, uInstance, name));
            ksAdapter = kstat_lookup(mKC, szModule, uInstance, (char *)name);
            if (ksAdapter == 0)
            {
                LogRel(("Failed to get network statistics for %s\n", name));
                return VERR_INTERNAL_ERROR;
            }
        }
    }
    if (kstat_read(mKC, ksAdapter, 0) == -1)
    {
        LogRel(("kstat_read(adapter) -> %d\n", errno));
        return VERR_INTERNAL_ERROR;
    }
    kstat_named_t *kn;
    if ((kn = (kstat_named_t *)kstat_data_lookup(ksAdapter, (char *)"rbytes")) == 0)
    {
        LogRel(("kstat_data_lookup(rbytes) -> %d, name=%s\n", errno, name));
        return VERR_INTERNAL_ERROR;
    }
    *rx = kn->value.ul;
    if ((kn = (kstat_named_t *)kstat_data_lookup(ksAdapter, (char *)"obytes")) == 0)
    {
        LogRel(("kstat_data_lookup(obytes) -> %d\n", errno));
        return VERR_INTERNAL_ERROR;
    }
    *tx = kn->value.ul;
    return VINF_SUCCESS;
}

int CollectorSolaris::getRawHostDiskLoad(const char *name, uint64_t *disk_ms, uint64_t *total_ms)
{
    int rc = VINF_SUCCESS;
    AssertReturn(strlen(name) < KSTAT_STRLEN, VERR_INVALID_PARAMETER);
    LogFlowThisFunc(("n=%s\n", name));
    kstat_t *ksDisk = kstat_lookup(mKC, NULL, -1, (char *)name);
    if (ksDisk != 0)
    {
        if (kstat_read(mKC, ksDisk, 0) == -1)
        {
            LogRel(("kstat_read(%s) -> %d\n", name, errno));
            rc = VERR_INTERNAL_ERROR;
        }
        else
        {
            kstat_io_t *ksIo = KSTAT_IO_PTR(ksDisk);
            /*
             * We do not care for wrap possibility here, although we may
             * reconsider in about 300 years (9223372036854775807 ns).
             */
            *disk_ms = ksIo->rtime / 1000000;
            *total_ms = ksDisk->ks_snaptime / 1000000;
        }
    }
    else
    {
        LogRel(("kstat_lookup(%s) -> %d\n", name, errno));
        rc = VERR_INTERNAL_ERROR;
    }

    return rc;
}

uint64_t CollectorSolaris::getZfsTotal(uint64_t cbTotal, const char *szFsType, const char *szFsName)
{
    if (strcmp(szFsType, "zfs"))
        return cbTotal;
    FsMap::iterator it = mFsMap.find(szFsName);
    if (it == mFsMap.end())
        return cbTotal;

    char *pszDataset = strdup(it->second.c_str());
    char *pszEnd = pszDataset + strlen(pszDataset);
    uint64_t uAvail = 0;
    while (pszEnd)
    {
        zfs_handle_t *hDataset;

        *pszEnd = 0;
        hDataset = mZfsOpen(mZfsLib, pszDataset, ZFS_TYPE_DATASET);
        if (!hDataset)
            break;

        if (uAvail == 0)
        {
            uAvail = mZfsPropGetInt(hDataset, ZFS_PROP_REFQUOTA);
            if (uAvail == 0)
                uAvail = UINT64_MAX;
        }

        uint64_t uQuota = mZfsPropGetInt(hDataset, ZFS_PROP_QUOTA);
        if (uQuota && uAvail > uQuota)
            uAvail = uQuota;

        pszEnd = strrchr(pszDataset, '/');
        if (!pszEnd)
        {
            uint64_t uPoolSize = mZfsPropGetInt(hDataset, ZFS_PROP_USED) +
                                 mZfsPropGetInt(hDataset, ZFS_PROP_AVAILABLE);
            if (uAvail > uPoolSize)
                uAvail = uPoolSize;
        }
        mZfsClose(hDataset);
    }
    free(pszDataset);

    return uAvail ? uAvail : cbTotal;
}

int CollectorSolaris::getHostFilesystemUsage(const char *path, ULONG *total, ULONG *used, ULONG *available)
{
    struct statvfs64 stats;
    const unsigned _MB = 1024 * 1024;

    if (statvfs64(path, &stats) == -1)
    {
        LogRel(("Failed to collect %s filesystem usage: errno=%d.\n", path, errno));
        return VERR_ACCESS_DENIED;
    }
    uint64_t cbBlock = stats.f_frsize ? stats.f_frsize : stats.f_bsize;
    *total = (ULONG)(getZfsTotal(cbBlock * stats.f_blocks, stats.f_basetype, path) / _MB);
    LogRel(("f_blocks=%llu.\n", stats.f_blocks));
    *used  = (ULONG)(cbBlock * (stats.f_blocks - stats.f_bfree) / _MB);
    *available = (ULONG)(cbBlock * stats.f_bavail / _MB);

    return VINF_SUCCESS;
}

int CollectorSolaris::getHostDiskSize(const char *name, uint64_t *size)
{
    int rc = VINF_SUCCESS;
    AssertReturn(strlen(name) + 5 < KSTAT_STRLEN, VERR_INVALID_PARAMETER);
    LogFlowThisFunc(("n=%s\n", name));
    char szName[KSTAT_STRLEN];
    strcpy(szName, name);
    strcat(szName, ",err");
    kstat_t *ksDisk = kstat_lookup(mKC, "sderr", -1, szName);
    if (ksDisk != 0)
    {
        if (kstat_read(mKC, ksDisk, 0) == -1)
        {
            LogRel(("kstat_read(%s) -> %d\n", name, errno));
            rc = VERR_INTERNAL_ERROR;
        }
        else
        {
            kstat_named_t *kn;
            if ((kn = (kstat_named_t *)kstat_data_lookup(ksDisk, (char *)"Size")) == 0)
            {
                LogRel(("kstat_data_lookup(rbytes) -> %d, name=%s\n", errno, name));
                return VERR_INTERNAL_ERROR;
            }
            *size = kn->value.ull;
        }
    }

    return rc;
}

RTCString CollectorSolaris::physToInstName(const char *pcszPhysName)
{
    FILE *fp = fopen("/etc/path_to_inst", "r");
    if (!fp)
        return RTCString();

    RTCString strInstName;
    size_t cbName = strlen(pcszPhysName);
    char szBuf[RTPATH_MAX];
    while (fgets(szBuf, sizeof(szBuf), fp))
    {
        if (szBuf[0] == '"' && strncmp(szBuf + 1, pcszPhysName, cbName) == 0)
        {
            char *pszDriver, *pszInstance;
            pszDriver = strrchr(szBuf, '"');
            if (pszDriver)
            {
                *pszDriver = '\0';
                pszDriver = strrchr(szBuf, '"');
                if (pszDriver)
                {
                    *pszDriver++ = '\0';
                    pszInstance = strrchr(szBuf, ' ');
                    if (pszInstance)
                    {
                        *pszInstance = '\0';
                        pszInstance = strrchr(szBuf, ' ');
                        if (pszInstance)
                        {
                            *pszInstance++ = '\0';
                            strInstName = pszDriver;
                            strInstName += pszInstance;
                            break;
                        }
                    }
                }
            }
        }
    }
    fclose(fp);

    return strInstName;
}

RTCString CollectorSolaris::pathToInstName(const char *pcszDevPathName)
{
    char szLink[RTPATH_MAX];
    if (readlink(pcszDevPathName, szLink, sizeof(szLink)) != -1)
    {
        char *pszStart, *pszEnd;
        pszStart = strstr(szLink, "/devices/");
        pszEnd = strrchr(szLink, ':');
        if (pszStart && pszEnd)
        {
            pszStart += 8; // Skip "/devices"
            *pszEnd = '\0'; // Trim partition
            return physToInstName(pszStart);
        }
    }

    return RTCString(pcszDevPathName);
}

int CollectorSolaris::getDiskListByFs(const char *name, DiskList& list)
{
    FsMap::iterator it = mFsMap.find(name);
    if (it == mFsMap.end())
        return VERR_INVALID_PARAMETER;

    RTCString strName = it->second.substr(0, it->second.find("/"));
    if (mZpoolOpen && mZpoolClose && mZpoolGetConfig && !strName.isEmpty())
    {
        zpool_handle_t *zh = mZpoolOpen(mZfsLib, strName.c_str());
        if (zh)
        {
            unsigned int cChildren = 0;
            nvlist_t **nvChildren  = NULL;
            nvlist_t *nvRoot       = NULL;
            nvlist_t *nvConfig     = mZpoolGetConfig(zh, NULL);
            if (   !nvlist_lookup_nvlist(nvConfig, ZPOOL_CONFIG_VDEV_TREE, &nvRoot)
                && !nvlist_lookup_nvlist_array(nvRoot, ZPOOL_CONFIG_CHILDREN, &nvChildren, &cChildren))
            {
                for (unsigned int i = 0; i < cChildren; ++i)
                {
                    uint64_t fHole = 0;
                    uint64_t fLog  = 0;

                    nvlist_lookup_uint64(nvChildren[i], ZPOOL_CONFIG_IS_HOLE, &fHole);
                    nvlist_lookup_uint64(nvChildren[i], ZPOOL_CONFIG_IS_LOG,  &fLog);

                    if (!fHole && !fLog)
                    {
                        char *pszChildName = mZpoolVdevName(mZfsLib, zh, nvChildren[i], _B_FALSE);
                        Assert(pszChildName);
                        RTCString strDevPath("/dev/dsk/");
                        strDevPath += pszChildName;
                        char szLink[RTPATH_MAX];
                        if (readlink(strDevPath.c_str(), szLink, sizeof(szLink)) != -1)
                        {
                            char *pszStart, *pszEnd;
                            pszStart = strstr(szLink, "/devices/");
                            pszEnd = strrchr(szLink, ':');
                            if (pszStart && pszEnd)
                            {
                                pszStart += 8; // Skip "/devices"
                                *pszEnd = '\0'; // Trim partition
                                list.push_back(physToInstName(pszStart));
                            }
                        }
                        free(pszChildName);
                    }
                }
            }
            mZpoolClose(zh);
        }
    }
    else
        list.push_back(pathToInstName(it->second.c_str()));
    return VINF_SUCCESS;
}

void CollectorSolaris::updateFilesystemMap(void)
{
    FILE *fp = fopen("/etc/mnttab", "r");
    if (fp)
    {
        struct mnttab Entry;
        int rc = 0;
        resetmnttab(fp);
        while ((rc = getmntent(fp, &Entry)) == 0)
            mFsMap[Entry.mnt_mountp] = Entry.mnt_special;
        fclose(fp);
        if (rc != -1)
            LogRel(("Error while reading mnttab: %d\n", rc));
    }
}

}
