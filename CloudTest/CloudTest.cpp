// CloudTest.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "stdafx.h"

#include "CloudTest.h"
#include <iostream>

#include "Utilities.h"
#include "CloudProviderRegistrar.h"
#include <filesystem>
#include <conio.h>
#include <system_error>

CF_CONNECTION_KEY s_transferCallbackConnectionKey;
HRESULT Init(std::wstring localRoot);
HRESULT CreatePlaceHolder(_In_ std::wstring localRoot, _In_ PCWSTR parentPath, _In_ std::wstring fileName, bool inSync, _Out_ USN& usn);
void DisconnectSyncRootTransferCallbacks();
void ConnectSyncRootTransferCallbacks(std::wstring localRoot);
HRESULT GetUSN(LPCWSTR path, _Out_ USN& usn);


int main()
{
    assert(CoInitialize(nullptr) == S_OK);

    std::cout << "Cloud sample test!\n";

    wchar_t tmpDir[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, tmpDir) != 0);

    wchar_t tmpDirLock[MAX_PATH];
    assert(GetTempFileNameW(tmpDir, L"cf-", 0, tmpDirLock) != 0);

    std::wstring workingDir = std::wstring(tmpDirLock) + L"-dir";
    assert(CreateDirectoryW(workingDir.c_str(), nullptr));

    Init(workingDir);

    CF_PLATFORM_INFO platformInfo;
    CfGetPlatformInfo(&platformInfo);

    std::cout << "platform version is " <<
        platformInfo.BuildNumber << "." <<
        platformInfo.RevisionNumber << "." <<
        platformInfo.IntegrationNumber << std::endl;

    USN usn;
    auto testFileName = L"test1.txt";
    auto testFile = workingDir + L"\\" + testFileName;
    auto hr = CreatePlaceHolder(workingDir, L"", testFileName, true, usn);
    assert(hr == S_OK);

    HANDLE protectedHandle;
    hr = CfOpenFileWithOplock(testFile.c_str(), CF_OPEN_FILE_FLAG_EXCLUSIVE, &protectedHandle);
    assert(hr == S_OK);

    {
        uint64_t fileSize = 9999999999999;
        CF_FILE_RANGE range = { 0 };
        range.Length.QuadPart = fileSize;

        CF_FS_METADATA meta;
        meta.FileSize.QuadPart = fileSize;
        meta.BasicInfo.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        meta.BasicInfo.CreationTime.QuadPart = 1;
        meta.BasicInfo.LastWriteTime.QuadPart = 1;
        meta.BasicInfo.LastAccessTime.QuadPart = 1;
        meta.BasicInfo.ChangeTime.QuadPart = 1;

        std::vector<uint8_t> identityStub{0, 1};

        CF_UPDATE_FLAGS updateFlags = CF_UPDATE_FLAG_MARK_IN_SYNC | CF_UPDATE_FLAG_DEHYDRATE;

        USN tempUSN(usn);
        hr = CfUpdatePlaceholder(protectedHandle, &meta, identityStub.data(), (DWORD)identityStub.size(), &range, 1, updateFlags, &tempUSN, nullptr);
        std::cout << "CfUpdatePlaceholder (with USN) for the file of size " << fileSize << " returned hresult = "
            << std::hex << hr << " (" << std::system_category().message(hr) << ")"
            <<", usn = " << tempUSN << std::endl;
  
        hr = CfUpdatePlaceholder(protectedHandle, &meta, identityStub.data(), (DWORD)identityStub.size(), &range, 1, updateFlags, nullptr, nullptr);
        std::cout << "CfUpdatePlaceholder for the file of size " << fileSize << " returned hresult = "
            << std::hex << hr << " (" << std::system_category().message(hr) << ")"
            << std::endl;
    }

    CfCloseHandle(protectedHandle);

    std::filesystem::file_time_type mt = std::filesystem::last_write_time(testFile);
    std::cout << "file mtime after updating placeholder is " << std::filesystem::file_time_type::clock::to_utc(mt) << std::endl;

    USN usn2;
    GetUSN(testFile.c_str(), usn2);
    std::cout << "file usn after updating placeholder is " << std::hex << usn2 << ", prev usn was " << usn << std::endl;

    DisconnectSyncRootTransferCallbacks();
    CoUninitialize();
    
    std::filesystem::remove_all(workingDir);
    std::filesystem::remove(tmpDirLock);

    system("pause");

    return 0;
}

HRESULT GetUSN(LPCWSTR path, _Out_ USN& usn) {
#define BUF_LEN 1024
    // ...

    CHAR Buffer[BUF_LEN];
    DWORD dwBytes;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    USN_RECORD_V2* fUsn = NULL;

    hFile = CreateFile(path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        printf("CreateFile failed (%d)\n", GetLastError());
        return -1;
    }

    memset(Buffer, 0, BUF_LEN);

    if (!DeviceIoControl(hFile,
        FSCTL_READ_FILE_USN_DATA,
        NULL,
        0,
        Buffer,
        BUF_LEN,
        &dwBytes,
        NULL))
    {
        printf("Read journal failed (%d)\n", GetLastError());
        return -2;
    }

    //printf("****************************************\n");

    fUsn = (USN_RECORD_V2*)Buffer;
    usn = fUsn->Usn;
    //printf("USN: %I64x\n", fUsn->Usn);
    //printf("File name: %.*S\n",
    //    fUsn->FileNameLength / 2,
    //    fUsn->FileName);
    //printf("Reason: %x\n", fUsn->Reason);

    CloseHandle(hFile);
    return S_OK;
}

HRESULT Mount(std::wstring localRoot) {

    // Stage 1: Setup
    //--------------------------------------------------------------------------------------------
    // The client folder (syncroot) must be indexed in order for states to properly display
    Utilities::AddFolderToSearchIndexer(localRoot.c_str());


    concurrency::create_task([localRoot] {
        // Register the provider with the shell so that the Sync Root shows up in File Explorer
        CloudProviderRegistrar::RegisterWithShell(localRoot, L"FOO");
        }).wait();

        return S_OK;
}

HRESULT IsSyncRoot(LPCWSTR path, _Out_ bool& isSyncRoot) {
    CF_SYNC_ROOT_BASIC_INFO info = { 0 };
    DWORD returnedLength;
    auto hr = CfGetSyncRootInfoByPath(path, CF_SYNC_ROOT_INFO_CLASS::CF_SYNC_ROOT_INFO_BASIC, &info, sizeof(info), &returnedLength);
    isSyncRoot = hr == S_OK && info.SyncRootFileId.QuadPart != 0;
    return S_OK;

    if (hr == 0x80070186) { // 0x80070186: This operation is only supported in a SyncRoot.
        return S_OK;
    }
    return hr;
}


HRESULT Init(std::wstring localRoot) {
    bool isAlreadySyncRoot;
    auto hr = IsSyncRoot(localRoot.c_str(), isAlreadySyncRoot);
    if (hr != S_OK) {
        return hr;
    }

    if (!isAlreadySyncRoot) {
        hr = Mount(localRoot);
        if (hr != S_OK) {
            return hr;
        }
    }

    // Hook up callback methods (in this class) for transferring files between client and server
    ConnectSyncRootTransferCallbacks(localRoot);


    return S_OK;
}

// This is a list of callbacks our fake provider support. This
// class has the callback methods, which are then delegated to
// helper classes
CF_CALLBACK_REGISTRATION s_MirrorCallbackTable[] =
{
    //{ CF_CALLBACK_TYPE_FETCH_DATA, CloudFolder::OnFetchData_C },
    //{ CF_CALLBACK_TYPE_CANCEL_FETCH_DATA, CloudFolder::OnCancelFetchData_C },

    //{ CF_CALLBACK_TYPE_NOTIFY_DELETE_COMPLETION, CloudFolder::OnDeleteCompletion_C },

    //{ CF_CALLBACK_TYPE_NOTIFY_RENAME_COMPLETION, CloudFolder::OnRenameCompletion_C },
    CF_CALLBACK_REGISTRATION_END
};

// Registers the callbacks in the table at the top of this file so that the methods above
// are called for our fake provider
void ConnectSyncRootTransferCallbacks(std::wstring localRoot)
{
    // Connect to the sync root using Cloud File API
    auto hr = CfConnectSyncRoot(
        localRoot.c_str(),
        s_MirrorCallbackTable,
        nullptr,
        CF_CONNECT_FLAGS::CF_CONNECT_FLAG_NONE,
        &s_transferCallbackConnectionKey);
    if (hr != S_OK)
    {
        // winrt::to_hresult() will eat the exception if it is a result of winrt::check_hresult,
        // otherwise the exception will get rethrown and this method will crash out as it should
        //LOG(LOG_LEVEL::Error, L"Could not connect to sync root, hr %08x", hr);
    }
}


// Unregisters the callbacks in the table at the top of this file so that 
// the client doesn't Hindenburg
void DisconnectSyncRootTransferCallbacks()
{
    //LOG(LOG_LEVEL::Info, L"Shutting down");
    auto hr = CfDisconnectSyncRoot(s_transferCallbackConnectionKey);

    if (hr != S_OK)
    {
        // winrt::to_hresult() will eat the exception if it is a result of winrt::check_hresult,
        // otherwise the exception will get rethrown and this method will crash out as it should
        //LOG(LOG_LEVEL::Error, L"Could not disconnect the sync root, hr %08x", hr);
    }
}

HRESULT CreatePlaceHolder(_In_ std::wstring localRoot, _In_ PCWSTR parentPath, _In_ std::wstring fileName, bool inSync, _Out_ USN& usn)
{
    std::wstring relativePath(parentPath);
    if (relativePath.size() > 0)
        if (relativePath.at(relativePath.size() - 1) != L'\\')
        {
            relativePath.append(L"\\");
        }
    relativePath.append(fileName);

    FileMetaData metadata = {};

    metadata.FileSize = 0;
    metadata.IsDirectory = false;

    fileName.copy(metadata.Name, fileName.length());

    CF_PLACEHOLDER_CREATE_INFO cloudEntry;
    auto fileIdentety = L"F";
    cloudEntry.FileIdentity = &fileIdentety;
    cloudEntry.FileIdentityLength = sizeof(fileIdentety);

    cloudEntry.RelativeFileName = relativePath.data();
    cloudEntry.Flags = inSync
        ? CF_PLACEHOLDER_CREATE_FLAGS::CF_PLACEHOLDER_CREATE_FLAG_MARK_IN_SYNC
        : CF_PLACEHOLDER_CREATE_FLAGS::CF_PLACEHOLDER_CREATE_FLAG_NONE;
    cloudEntry.FsMetadata.FileSize.QuadPart = metadata.FileSize;
    cloudEntry.FsMetadata.BasicInfo.FileAttributes = metadata.FileAttributes;
    cloudEntry.FsMetadata.BasicInfo.CreationTime = metadata.CreationTime;
    cloudEntry.FsMetadata.BasicInfo.LastWriteTime = metadata.LastWriteTime;
    cloudEntry.FsMetadata.BasicInfo.LastAccessTime = metadata.LastAccessTime;
    cloudEntry.FsMetadata.BasicInfo.ChangeTime = metadata.ChangeTime;

    if (metadata.IsDirectory)
    {
        cloudEntry.FsMetadata.BasicInfo.FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        cloudEntry.Flags |= CF_PLACEHOLDER_CREATE_FLAG_DISABLE_ON_DEMAND_POPULATION;
        cloudEntry.FsMetadata.FileSize.QuadPart = 0;
    }

    auto hr = CfCreatePlaceholders(localRoot.c_str(), &cloudEntry, 1, CF_CREATE_FLAGS::CF_CREATE_FLAG_NONE, NULL);
    if (hr != S_OK) {
        return hr;
    }

    usn = cloudEntry.CreateUsn;
    return cloudEntry.Result;
}
