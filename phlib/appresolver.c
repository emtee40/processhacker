/*
 * Process Hacker -
 *   Appmodel support functions
 *
 * Copyright (C) 2017-2018 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#define COBJMACROS
#define CINTERFACE
#include <ph.h>
#include <lsasup.h>

#include <minappmodel.h>
#include <appmodel.h>
#include <combaseapi.h>
#include <propsys.h>
#include <shobjidl.h>

#include <appresolverp.h>
#include <appresolver.h>

static PVOID PhpQueryAppResolverInterface(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PVOID resolverInterface = NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        if (WindowsVersion < WINDOWS_8)
            CoCreateInstance(&CLSID_StartMenuCacheAndAppResolver_I, NULL, CLSCTX_INPROC_SERVER, &IID_IApplicationResolver61_I, &resolverInterface);
        else
            CoCreateInstance(&CLSID_StartMenuCacheAndAppResolver_I, NULL, CLSCTX_INPROC_SERVER, &IID_IApplicationResolver62_I, &resolverInterface);

        PhEndInitOnce(&initOnce);
    }

    return resolverInterface;
}

static PVOID PhpQueryStartMenuCacheInterface(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PVOID startMenuInterface = NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        if (WindowsVersion < WINDOWS_8)
            CoCreateInstance(&CLSID_StartMenuCacheAndAppResolver_I, NULL, CLSCTX_INPROC_SERVER, &IID_IStartMenuAppItems61_I, &startMenuInterface);
        else
            CoCreateInstance(&CLSID_StartMenuCacheAndAppResolver_I, NULL, CLSCTX_INPROC_SERVER, &IID_IStartMenuAppItems62_I, &startMenuInterface);

        PhEndInitOnce(&initOnce);
    }

    return startMenuInterface;
}

static BOOLEAN PhpKernelAppCoreInitialized(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static BOOLEAN kernelAppCoreInitialized = FALSE;
    
    if (PhBeginInitOnce(&initOnce))
    {
        if (WindowsVersion >= WINDOWS_8)
        {
            PVOID kernelAppBaseAddress;

            if (kernelAppBaseAddress = LoadLibrary(L"kernel.appcore.dll"))
            {
                AppContainerLookupMoniker_I = PhGetProcedureAddress(kernelAppBaseAddress, "AppContainerLookupMoniker", 0);
                AppContainerFreeMemory_I = PhGetProcedureAddress(kernelAppBaseAddress, "AppContainerFreeMemory", 0);
                AppContainerRegisterSid_I = PhGetProcedureAddress(kernelAppBaseAddress, "AppContainerRegisterSid", 0);
                AppContainerUnregisterSid_I = PhGetProcedureAddress(kernelAppBaseAddress, "AppContainerUnregisterSid", 0);

                AppPolicyGetWindowingModel_I = PhGetProcedureAddress(kernelAppBaseAddress, "AppPolicyGetWindowingModel", 0);
            }

            if (
                AppContainerLookupMoniker_I && 
                AppContainerFreeMemory_I && 
                AppContainerRegisterSid_I && 
                AppContainerUnregisterSid_I
                )
            {
                kernelAppCoreInitialized = TRUE;
            }
        }

        PhEndInitOnce(&initOnce);
    }

    return kernelAppCoreInitialized;
}

BOOLEAN PhAppResolverGetAppIdForProcess(
    _In_ HANDLE ProcessId,
    _Out_ PPH_STRING *ApplicationUserModelId
    )
{
    PVOID resolverInterface;
    PWSTR appIdText = NULL;

    if (!(resolverInterface = PhpQueryAppResolverInterface()))
        return FALSE;

    if (WindowsVersion < WINDOWS_8)
    {
        IApplicationResolver_GetAppIDForProcess(
            (IApplicationResolver61*)resolverInterface, 
            HandleToUlong(ProcessId), 
            &appIdText, 
            NULL, 
            NULL, 
            NULL
            );
    }
    else
    {
        IApplicationResolver2_GetAppIDForProcess(
            (IApplicationResolver62*)resolverInterface, 
            HandleToUlong(ProcessId), 
            &appIdText, 
            NULL, 
            NULL, 
            NULL
            );
    }

    if (appIdText)
    {
        *ApplicationUserModelId = PhCreateString(appIdText);
        return TRUE;
    }

    return FALSE;
}

HRESULT PhAppResolverActivateAppId(
    _In_ PPH_STRING AppUserModelId,
    _In_opt_ PWSTR CommandLine,
    _Out_opt_ HANDLE *ProcessId
    )
{
    HRESULT status;
    ULONG processId = 0;
    IApplicationActivationManager* applicationActivationManager;

    status = CoCreateInstance(
        &CLSID_ApplicationActivationManager,
        NULL,
        CLSCTX_LOCAL_SERVER,
        &IID_IApplicationActivationManager,
        &applicationActivationManager
        );

    if (SUCCEEDED(status))
    {
        CoAllowSetForegroundWindow((IUnknown*)applicationActivationManager, NULL);

        status = IApplicationActivationManager_ActivateApplication(
            applicationActivationManager,
            PhGetString(AppUserModelId),
            CommandLine,
            AO_NONE,
            &processId
            );

        IApplicationActivationManager_Release(applicationActivationManager);
    }

    if (SUCCEEDED(status))
    {
        if (ProcessId) *ProcessId = UlongToHandle(processId);
    }

    return status;
}

PPH_STRING PhGetAppContainerName(
    _In_ PSID AppContainerSid
    )
{
    PPH_STRING appContainerName = NULL;
    PWSTR packageMonikerName;

    if (!PhpKernelAppCoreInitialized())
        return NULL;

    if (SUCCEEDED(AppContainerLookupMoniker_I(AppContainerSid, &packageMonikerName)))
    {
        appContainerName = PhCreateString(packageMonikerName);
        AppContainerFreeMemory_I(packageMonikerName);
    }

    return appContainerName;
}

PPH_STRING PhGetAppContainerPackageName(
    _In_ PSID Sid
    )
{   
    static PH_STRINGREF appcontainerMappings = PH_STRINGREF_INIT(L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings\\");
    HANDLE keyHandle;
    PPH_STRING sidString;
    PPH_STRING keyPath;
    PPH_STRING packageName = NULL;

    sidString = PhSidToStringSid(Sid);

    if (PhEqualString2(sidString, L"S-1-15-3-4096", FALSE)) // HACK
    {
        PhDereferenceObject(sidString);
        return PhCreateString(L"InternetExplorer");
    }

    keyPath = PhConcatStringRef2(&appcontainerMappings, &sidString->sr);

    if (NT_SUCCESS(PhOpenKey(
        &keyHandle,
        KEY_READ,
        PH_KEY_CURRENT_USER,
        &keyPath->sr,
        0
        )))
    {
        PhMoveReference(&packageName, PhQueryRegistryString(keyHandle, L"Moniker"));
        NtClose(keyHandle);
    }

    PhDereferenceObject(keyPath);
    PhDereferenceObject(sidString);

    return packageName;
}

BOOLEAN PhGetAppWindowingModel(
    _In_ HANDLE ProcessTokenHandle,
    _Out_ AppPolicyWindowingModel *ProcessWindowingModelPolicy
    )
{
    if (!PhpKernelAppCoreInitialized() && !AppPolicyGetWindowingModel_I)
        return FALSE;

    return SUCCEEDED(AppPolicyGetWindowingModel_I(ProcessTokenHandle, ProcessWindowingModelPolicy));
}

PPH_LIST PhGetPackageAssetsFromResourceFile(
    _In_ PWSTR FilePath
    )
{
    IMrtResourceManager* resourceManager = NULL;
    IResourceMap* resourceMap = NULL;
    PPH_LIST resourceList = NULL;
    ULONG resourceCount = 0;

    if (FAILED(CoCreateInstance(
        &CLSID_MrtResourceManager_I,
        NULL,
        CLSCTX_INPROC_SERVER,
        &IID_IMrtResourceManager_I,
        &resourceManager
        )))
    {
        return FALSE;
    }

    if (FAILED(IMrtResourceManager_InitializeForFile(resourceManager, FilePath)))
        goto CleanupExit;

    if (FAILED(IMrtResourceManager_GetMainResourceMap(resourceManager, &IID_IResourceMap_I, &resourceMap)))
        goto CleanupExit;

    if (FAILED(IResourceMap_GetNamedResourceCount(resourceMap, &resourceCount)))
        goto CleanupExit;

    resourceList = PhCreateList(10);

    for (ULONG i = 0; i < resourceCount; i++)
    {
        PWSTR resourceName;

        if (SUCCEEDED(IResourceMap_GetNamedResourceUri(resourceMap, i, &resourceName)))
        {
            PhAddItemList(resourceList, PhCreateString(resourceName));
        }
    }

CleanupExit:

    if (resourceMap)
        IResourceMap_Release(resourceMap);

    if (resourceManager)
        IMrtResourceManager_Release(resourceManager);

    return resourceList;
}

// TODO: FIXME
//HICON PhAppResolverGetPackageIcon(
//    _In_ HANDLE ProcessId,
//    _In_ PPH_STRING PackageFullName
//    )
//{
//    PVOID startMenuInterface;
//    PPH_STRING applicationUserModelId;
//    IPropertyStore* propStoreInterface;
//    HICON packageIcon = NULL;
//
//    if (!(startMenuInterface = PhpQueryStartMenuCacheInterface()))
//        return NULL;
//
//    if (!PhAppResolverGetAppIdForProcess(ProcessId, &applicationUserModelId))
//        return NULL;
//
//    if (WindowsVersion < WINDOWS_8)
//    {
//        IStartMenuAppItems_GetItem(
//            (IStartMenuAppItems61*)startMenuInterface,
//            SMAIF_DEFAULT,
//            applicationUserModelId->Buffer,
//            &IID_IPropertyStore,
//            &propStoreInterface
//            );
//    }
//    else
//    {
//        IStartMenuAppItems2_GetItem(
//            (IStartMenuAppItems62*)startMenuInterface,
//            SMAIF_DEFAULT,
//            applicationUserModelId->Buffer,
//            &IID_IPropertyStore,
//            &propStoreInterface
//            );
//    }
//
//    if (propStoreInterface)
//    {
//        IMrtResourceManager* mrtResourceManager;
//        IResourceMap* resourceMap;
//        PROPVARIANT propVar;
//        PWSTR filePath;
//
//        IPropertyStore_GetValue(propStoreInterface, &PKEY_Tile_Background, &propVar);
//        IPropertyStore_GetValue(propStoreInterface, &PKEY_Tile_SmallLogoPath, &propVar);
//
//        CoCreateInstance(
//            &CLSID_MrtResourceManager_I,
//            NULL,
//            CLSCTX_INPROC_SERVER,
//            &IID_IMrtResourceManager_I,
//            &mrtResourceManager
//            );
//
//        IMrtResourceManager_InitializeForPackage(mrtResourceManager, PackageFullName->Buffer);
//        IMrtResourceManager_GetMainResourceMap(mrtResourceManager, &IID_IResourceMap_I, &resourceMap);
//        IResourceMap_GetFilePath(resourceMap, propVar.pwszVal, &filePath);
//
//        //HBITMAP bitmap = PhLoadImageFromFile(filePath, 32, 32);
//        //packageIcon = PhBitmapToIcon(bitmap, 32, 32);
//
//        IResourceMap_Release(resourceMap);
//        IMrtResourceManager_Release(mrtResourceManager);
//        PropVariantClear(&propVar);
//
//        IPropertyStore_Release(propStoreInterface);
//    }
//
//    PhDereferenceObject(applicationUserModelId);
//
//    return packageIcon;
//}
